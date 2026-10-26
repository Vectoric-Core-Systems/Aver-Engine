// WaterSurface: the flat grid, the compute dispatch that waves it, and the lifetime of the mesh.
// See WaterSurface.hpp for why this is a mesh and not a pipeline of its own.
#include "aver/fluids/WaterSurface.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace aver::fluids {
namespace {

// b3, as in the skinning pass: b0 is the engine frame block, b1/b2 the raster path, b4 a feature's
// frame block. A root CBV, because the wave set is more than root constants should carry.
constexpr u32 kDisplaceConstantSlot = 3;
constexpr u32 kGroupSize = 64;   // [numthreads] in water_displace.hlsl

// uv = world cm * this, i.e. one texture repeat per metre before the material's own tiling.
constexpr f32 kUvPerCm = 0.01f;

// MIRRORS cbuffer WaterDisplace in water_displace.hlsl field for field.
struct WaterDisplaceCB {
    f32 waveDirSteep[kMaxGerstnerWaves][4];
    f32 waveLenAmp[kMaxGerstnerWaves][4];
    f32 originTime[4];   // origin x, y, seconds, wave count
    f32 grid[4];         // cell x, y, uv per cm, skirt scale
    u32 dims[4];         // verts x, y, skirt vertex count, skirt rings
};
static_assert(sizeof(WaterDisplaceCB) == 176, "cbuffer WaterDisplace mirrors this byte for byte");

u32 cellsFor(f64 extentCm, f64 cellCm, u32 minCells) {
    const f64 n = std::round(extentCm / cellCm);
    const f64 clamped = std::min<f64>(std::max<f64>(n, static_cast<f64>(minCells)), kWaterMaxCellsPerAxis);
    return static_cast<u32>(clamped);
}

}  // namespace

WaterGridLayout waterGridLayout(const WaterSurfaceDesc& d) {
    WaterGridLayout l;
    const f64 cell = std::max<f64>(d.cellCm, 0.01);
    if (d.bounded) {
        const f64 ex = std::max<f64>(std::fabs(static_cast<f64>(d.boundsMaxCm[0]) - d.boundsMinCm[0]), 1.0);
        const f64 ey = std::max<f64>(std::fabs(static_cast<f64>(d.boundsMaxCm[1]) - d.boundsMinCm[1]), 1.0);
        const u32 cx = cellsFor(ex, cell, 1), cy = cellsFor(ey, cell, 1);
        l.vertsX = cx + 1;
        l.vertsY = cy + 1;
        l.cellX = static_cast<f32>(ex / cx);
        l.cellY = static_cast<f32>(ey / cy);
        return l;
    }
    const f64 extent = 2.0 * std::max<f64>(d.radiusCm, 1.0);
    u32 cells = cellsFor(extent, cell, 2);
    if (cells & 1u) ++cells;   // even, so a vertex sits on the origin
    // A coarsened grid keeps its extent; an exact one keeps the requested cell.
    const f64 actual = (extent / cell > kWaterMaxCellsPerAxis) ? extent / cells : cell;
    l.vertsX = l.vertsY = cells + 1;
    l.cellX = l.cellY = static_cast<f32>(actual);
    // Out to ~10 km, past where any view distance sees an edge.
    l.skirtScale = static_cast<f32>(std::max(1000000.0 / (0.5 * actual * cells), 2.0));
    l.skirtRings = static_cast<u32>(std::ceil(std::log2(static_cast<f64>(l.skirtScale))));
    return l;
}

void buildWaterGrid(const WaterGridLayout& l, std::vector<rhi::MeshVertex>& verts, std::vector<u32>& indices) {
    verts.clear();
    indices.clear();
    if (l.vertsX < 2 || l.vertsY < 2) return;

    verts.resize(static_cast<usize>(l.vertsX) * l.vertsY);
    for (u32 iy = 0; iy < l.vertsY; ++iy) {
        for (u32 ix = 0; ix < l.vertsX; ++ix) {
            rhi::MeshVertex& v = verts[static_cast<usize>(iy) * l.vertsX + ix];
            v.px = (static_cast<f32>(ix) - 0.5f * static_cast<f32>(l.vertsX - 1)) * l.cellX;
            v.py = (static_cast<f32>(iy) - 0.5f * static_cast<f32>(l.vertsY - 1)) * l.cellY;
            v.pz = 0.0f;
            v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
            v.u = v.px * kUvPerCm;
            v.v = v.py * kUvPerCm;
        }
    }

    // Each horizon ring walks the grid boundary (bottom, right, top, left; water_displace.hlsl walks it
    // the same way) and pushes each boundary vertex out by its ring's scale.
    const u32 m = l.vertsX - 1, grid = l.vertsX * l.vertsY;
    const u32 ring = l.skirtRings ? waterSkirtVertexCount(l) / l.skirtRings : 0u;
    const auto boundary = [&](u32 k) {
        const u32 side = k / m, t = k % m;
        const u32 ix = side == 0 ? t : side == 1 ? m : side == 2 ? m - t : 0;
        const u32 iy = side == 0 ? 0 : side == 1 ? t : side == 2 ? m : m - t;
        return iy * l.vertsX + ix;
    };
    for (u32 r = 1; r <= (ring ? l.skirtRings : 0u); ++r) {
        const f32 scale = std::pow(l.skirtScale, static_cast<f32>(r) / static_cast<f32>(l.skirtRings));
        for (u32 k = 0; k < ring; ++k) {
            rhi::MeshVertex v = verts[boundary(k)];
            v.px *= scale;
            v.py *= scale;
            v.u = v.px * kUvPerCm;
            v.v = v.py * kUvPerCm;
            verts.push_back(v);
        }
    }

    indices.reserve(static_cast<usize>(waterGridIndexCount(l)));
    for (u32 iy = 0; iy + 1 < l.vertsY; ++iy) {
        for (u32 ix = 0; ix + 1 < l.vertsX; ++ix) {
            const u32 a = iy * l.vertsX + ix;   // (p, q)
            const u32 b = a + 1;                // (p + 1, q)
            const u32 c = a + l.vertsX;         // (p, q + 1)
            const u32 d = c + 1;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    // Same winding as the grid's cells (clockwise seen from +Z). Ring 0 is the grid's own boundary.
    const auto at = [&](u32 r, u32 k) { return r == 0 ? boundary(k) : grid + (r - 1) * ring + k; };
    for (u32 r = 1; r <= (ring ? l.skirtRings : 0u); ++r) {
        for (u32 k = 0; k < ring; ++k) {
            const u32 k1 = (k + 1) % ring;
            const u32 a = at(r - 1, k), b = at(r - 1, k1), c = at(r, k), d = at(r, k1);
            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            indices.push_back(b); indices.push_back(d); indices.push_back(c);
        }
    }
}

void waterGridOrigin(const WaterSurfaceDesc& d, const WaterGridLayout& l, f32 camX, f32 camY, f32 out[2]) {
    if (d.bounded) {
        out[0] = 0.5f * (d.boundsMinCm[0] + d.boundsMaxCm[0]);
        out[1] = 0.5f * (d.boundsMinCm[1] + d.boundsMaxCm[1]);
        return;
    }
    out[0] = snapWorldToGridCm(camX, l.cellX);
    out[1] = snapWorldToGridCm(camY, l.cellY);
}

WaterSurface::~WaterSurface() { shutdown(); }

bool WaterSurface::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;

    const std::string defines = "AVER_MAX_WAVES=" + std::to_string(kMaxGerstnerWaves);
    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("water_displace.hlsl").c_str();
    sd.defines = defines.c_str();
    sd.entry = "CSWaterDisplace";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    cs_ = res_->createShader(sd);
    if (!cs_) { AVER_ERROR("[WaterSurface] the displacement shader would not compile"); shutdown(); return false; }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout.uavCount = 1;
    pd.layout.slotKindsDeclared = true;
    pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.constantDwords[kDisplaceConstantSlot] = 0;   // root CBV
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[WaterSurface] the displacement pipeline would not build"); shutdown(); return false; }
    return true;
}

void WaterSurface::shutdown() {
    clear();
    if (res_) {
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_) res_->destroyShader(cs_);
    }
    pipeline_ = 0;
    cs_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
}

void WaterSurface::clear() {
    if (res_ && set_) res_->destroyBindingSet(set_);
    set_ = 0;
    // drawMesh_ owns vertices_ (createSkinTargetMesh), and shares source_'s index buffer, so it goes
    // first: destroyMesh refuses a source that is still shared.
    if (dev_ && drawMesh_) dev_->destroyMesh(drawMesh_);
    if (dev_ && source_) dev_->destroyMesh(source_);
    drawMesh_ = 0;
    source_ = 0;
    vertices_ = 0;
    state_ = rhi::ResourceState::Common;
}

bool WaterSurface::configure(const WaterSurfaceDesc& d) {
    if (!dev_ || !res_) return false;
    clear();
    desc_ = d;
    layout_ = waterGridLayout(d);
    waterGridOrigin(desc_, layout_, 0.0f, 0.0f, originXY_);

    std::vector<rhi::MeshVertex> verts;
    std::vector<u32> indices;
    buildWaterGrid(layout_, verts, indices);
    if (verts.empty()) return false;

    source_ = dev_->createMesh(verts.data(), static_cast<u32>(verts.size()), indices.data(),
                               static_cast<u32>(indices.size()));
    if (!source_) {
        AVER_WARN("[WaterSurface] this device would not upload the {}x{} grid", layout_.vertsX, layout_.vertsY);
        return false;
    }
    drawMesh_ = dev_->createSkinTargetMesh(source_, &vertices_);
    if (!drawMesh_ || !vertices_) {
        AVER_WARN("[WaterSurface] this device cannot make a compute-written mesh; the surface is not drawn");
        clear();
        return false;
    }

    rhi::BindingSetDesc bd;
    bd.uavCount = 1;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    set_ = res_->createBindingSet(bd);
    if (!set_) { clear(); return false; }
    res_->setUavBuffer(set_, 0, vertices_, static_cast<u32>(sizeof(rhi::MeshVertex)),
                       static_cast<u32>(verts.size()), 0);

    AVER_INFO("[WaterSurface] {}x{} vertices, {:.0f} cm cells{}", layout_.vertsX, layout_.vertsY, layout_.cellX,
              d.bounded ? " (bounded)" : " (camera-following)");
    return true;
}

void WaterSurface::setWaves(const GerstnerWave* waves, size_t count) {
    const bool overflowed = count > kMaxGerstnerWaves;
    const size_t n = overflowed ? kMaxGerstnerWaves : count;
    for (size_t i = 0; i < n; ++i) waves_[i] = waves[i];
    // Unused slots are inert, not GerstnerWave{}'s defaults, so a loop bound edited later cannot pick
    // up a hardcoded wave.
    for (size_t i = n; i < kMaxGerstnerWaves; ++i) {
        waves_[i] = GerstnerWave{};
        waves_[i].amplitudeCm = 0.0f;
    }
    waveCount_ = n;
    if (overflowed && !loggedWaveOverflow_) {
        AVER_WARN("[WaterSurface] {} waves requested, kMaxGerstnerWaves is {}; the rest are dropped",
                  count, kMaxGerstnerWaves);
        loggedWaveOverflow_ = true;
    }
}

void WaterSurface::tick(f32 dtSeconds) {
    if (!(dtSeconds >= 0.0f) || dtSeconds > 1.0f) {   // NaN fails the first test and lands at 0
        if (!loggedBadDt_) {
            AVER_WARN("[WaterSurface] tick() dt {} is out of [0, 1]; clamping", dtSeconds);
            loggedBadDt_ = true;
        }
        dtSeconds = dtSeconds > 1.0f ? 1.0f : 0.0f;
    }
    elapsedSeconds_ += dtSeconds;
}

void WaterSurface::place(const f32 camPos[3]) {
    waterGridOrigin(desc_, layout_, camPos[0], camPos[1], originXY_);
}

void WaterSurface::drawOriginCm(f32 out[3]) const {
    out[0] = originXY_[0];
    out[1] = originXY_[1];
    out[2] = desc_.levelCm;
}

void WaterSurface::prePass(rhi::IRenderContext& ctx) {
    if (!pipeline_ || !drawMesh_ || !vertices_ || !set_) return;

    WaterDisplaceCB cb{};
    const rhi::ShiftOrigin floatingOrigin = dev_ ? dev_->worldOrigin() : rhi::ShiftOrigin{};   // zero until the first rebase
    for (size_t i = 0; i < kMaxGerstnerWaves; ++i) {
        cb.waveDirSteep[i][0] = waves_[i].dirX;
        cb.waveDirSteep[i][1] = waves_[i].dirZ;   // GerstnerWave's second axis is engine Y
        cb.waveDirSteep[i][2] = waves_[i].steepness;
        cb.waveLenAmp[i][0] = waves_[i].wavelengthCm;
        cb.waveLenAmp[i][1] = waves_[i].amplitudeCm;
        // The floating origin's share of the wave phase (water_displace.hlsl adds it): the grid is origin-relative.
        cb.waveLenAmp[i][2] = rhi::gerstnerPhaseOffset(waves_[i].dirX, waves_[i].dirZ, waves_[i].wavelengthCm, floatingOrigin);
    }
    cb.originTime[0] = originXY_[0];
    cb.originTime[1] = originXY_[1];
    cb.originTime[2] = elapsedSeconds_;
    cb.originTime[3] = static_cast<f32>(waveCount_);
    cb.grid[0] = layout_.cellX;
    cb.grid[1] = layout_.cellY;
    cb.grid[2] = kUvPerCm;
    cb.grid[3] = layout_.skirtScale;
    cb.dims[0] = layout_.vertsX;
    cb.dims[1] = layout_.vertsY;
    cb.dims[2] = waterSkirtVertexCount(layout_);
    cb.dims[3] = layout_.skirtRings;

    rhi::ScopedGpuStat stat(ctx, "Aver.WaterSurface");
    if (state_ != rhi::ResourceState::UnorderedAccess) {
        ctx.bufferBarrier(vertices_, state_, rhi::ResourceState::UnorderedAccess);
        state_ = rhi::ResourceState::UnorderedAccess;
    }
    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(set_);
    ctx.setConstantBuffer(kDisplaceConstantSlot, &cb, sizeof(cb));
    ctx.dispatch((waterGridVertexCount(layout_) + kGroupSize - 1) / kGroupSize, 1, 1);
    ctx.bufferBarrier(vertices_, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::GeometryRead);
    state_ = rhi::ResourceState::GeometryRead;
}

void WaterSurface::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    (void)width; (void)height;
    // A buffer left in GeometryRead at submit makes next frame's barrier claim a state the hardware
    // dropped (SkinnedScene::overlayPass), so every frame ends in Common.
    if (!vertices_ || state_ == rhi::ResourceState::Common) return;
    ctx.bufferBarrier(vertices_, state_, rhi::ResourceState::Common);
    state_ = rhi::ResourceState::Common;
}

}  // namespace aver::fluids
