// WaterRenderer implementation: the static grid, the pipeline, and the per-frame draw. See
// WaterRenderer.hpp's class-body comment for why the grid is built ONCE and never re-uploaded, the
// one place this file's geometry strategy diverges from ParticleRenderer.cpp's ring-buffer
// precedent -- everything else here (pipeline shape, draw sequence, marker) follows that precedent
// as closely as the two features' actual differences allow.
#include "aver/fluids/WaterRenderer.hpp"
#include "WaterShaders.hpp"

#include "aver/core/Log.hpp"

#include <cstring>
#include <string>
#include <vector>
#include "aver/rhi/ShaderFiles.hpp"   // the ocean HLSL is a deployed file

namespace aver::fluids {
namespace {

// ---------------------------------------------------------------------------------------------
// THE GRID'S NUMBERS, chosen and written down here rather than left as unexplained magic:
//
//   129x129 VERTICES, 128x128 CELLS. Odd vertex counts (129, not 128 or 130) are what gives the
//   grid a CENTRE vertex -- the grid recentres on the camera every frame (see transparentPass), and
//   a centre vertex is what keeps that recentring symmetric: the camera sits over the middle of the
//   middle cell rather than straddling a seam between two cells on every axis at once.
//
//   200cm CELL SIZE. This is also snapWorldToGridCm's cell size for the grid's origin (see
//   transparentPass) -- the two MUST agree, because a grid built with one cell size and recentred on
//   a lattice with a DIFFERENT cell size would still shimmer: every recentre would land the mesh's
//   fixed local topology at a fractional offset from where its own vertices actually sit. At a
//   typical camera speed (a fast-moving free camera, tens of metres per second) 200cm keeps the
//   grid's own seam-hiding recentre well below one recentre per rendered frame at 60Hz, while still
//   being fine enough that a single Gerstner wave (the shortest authored default, wavelengthCm=800
//   -- GerstnerWave.hpp's own struct default) is sampled by four-plus vertices across one crest,
//   enough for the vertex shader's own crest to read as a crest and not a facet.
//
//   25600cm (128 * 200cm) TOTAL EXTENT. Large enough that the grid's own far edge sits well past
//   the horizon-ish view distances this engine's fog is tuned for (SkyAtmosphere's own authored fog
//   fields, RHI.hpp) at typical camera heights, so the grid's edge is not a visible seam under
//   ordinary play -- and small enough that 129*129 = 16641 vertices and 128*128*2 = 32768 triangles
//   is a trivial one-time upload, not a per-frame cost at all (see this class's own header comment
//   on why it is uploaded exactly once).
constexpr u32 kGridVertsPerSide = 129;
constexpr u32 kGridCellsPerSide = kGridVertsPerSide - 1;
constexpr f32 kCellSizeCm = 200.0f;
constexpr f32 kGridExtentCm = static_cast<f32>(kGridCellsPerSide) * kCellSizeCm;

// A plain XZ grid vertex: local (x, z) in this file's own generic sense (see WaterShaders.hpp's
// axis note -- this is engine X and engine Y once the vertex shader adds gGridOriginCount.xy).
// POSITION0, RG32Float, matching rhi::shaderFile("water.hlsl").c_str()'s VSWaterIn::localXZ.
struct WaterVertex { f32 x, z; };
static_assert(sizeof(WaterVertex) == 8, "the vertex layout in buildPipeline names this stride");

// MIRRORS rhi::shaderFile("water.hlsl").c_str()'s cbuffer WaterFrame in WaterShaders.hpp FIELD FOR FIELD -- see that cbuffer's
// own comment for what each field means and why gGridOriginCount carries the actual wave count
// rather than always looping the compile-time maximum.
struct WaterFrameCB {
    f32 waveDirSteep[kMaxGerstnerWaves][4];
    f32 waveLenAmp[kMaxGerstnerWaves][4];
    f32 gridOriginCount[4];
    f32 waterState[4];
    f32 shallowColor[4];
    f32 deepColor[4];
};
// Six float4 rows per wave slot pair plus four more rows, at kMaxGerstnerWaves=4: (4+4)*16 + 4*16 =
// 192 bytes. This assert exists for the same reason PathTracer.cpp's FrameCB one does: it cannot
// catch a field reordered within the same total size, but it catches the common mistake of a row
// added on one side of the C++/HLSL boundary and not the other.
static_assert(sizeof(WaterFrameCB) == 192, "cbuffer WaterFrame in WaterShaders.hpp mirrors this byte for byte");
static_assert(sizeof(WaterFrameCB) % 16 == 0, "must be a legal constant-buffer size");

} // namespace

bool WaterRenderer::init(rhi::IDevice& dev) {
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) {
        AVER_WARN("[Water] init declined: backend exposes no resource factory");
        return false;
    }

    // AVER_MAX_WAVES is computed from kMaxGerstnerWaves here, once, rather than hardcoded as a
    // second "4" inside WaterShaders.hpp -- the same precedent materialShaderDefines (PbrShaders.cpp)
    // sets for computing a register/size define in C++ instead of duplicating a number that has to
    // agree across two languages by hand.
    const std::string defines = "AVER_MAX_WAVES=" + std::to_string(kMaxGerstnerWaves);

    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("water.hlsl").c_str();
    sd.prelude = rhi::sharedShaderPrelude();
    sd.defines = defines.c_str();
    sd.entry = "VSWater";
    sd.stage = rhi::ShaderStage::Vertex;
    vs_ = res_->createShader(sd);
    sd.entry = "PSWater";
    sd.stage = rhi::ShaderStage::Pixel;
    ps_ = res_->createShader(sd);
    if (!vs_ || !ps_) {
        AVER_ERROR("[Water] the water shaders failed to compile");
        return false;
    }

    // ---- the static grid, built and uploaded exactly once (see this class's header comment) ----
    std::vector<WaterVertex> verts;
    verts.reserve(static_cast<usize>(kGridVertsPerSide) * kGridVertsPerSide);
    const f32 half = kGridExtentCm * 0.5f;
    for (u32 row = 0; row < kGridVertsPerSide; ++row) {
        for (u32 col = 0; col < kGridVertsPerSide; ++col) {
            WaterVertex v;
            v.x = static_cast<f32>(col) * kCellSizeCm - half;
            v.z = static_cast<f32>(row) * kCellSizeCm - half;
            verts.push_back(v);
        }
    }

    std::vector<u32> indices;
    indices.reserve(static_cast<usize>(kGridCellsPerSide) * kGridCellsPerSide * 6);
    for (u32 row = 0; row < kGridCellsPerSide; ++row) {
        for (u32 col = 0; col < kGridCellsPerSide; ++col) {
            const u32 a = row * kGridVertsPerSide + col;
            const u32 b = a + 1;
            const u32 c = a + kGridVertsPerSide;
            const u32 d = c + 1;
            // Two triangles per cell, both wound the same way the rest of this engine's opaque
            // geometry is (see SoftBodyTest.cpp's makeGrid for the identical a,b,c / a,c,d pattern
            // this mirrors) -- cull mode is None on this pipeline regardless (see buildPipeline's own
            // comment), so winding cannot hide a bug here the way it would on a culled pipeline, but
            // matching the established convention still matters for any tool (RenderDoc, a future
            // wireframe view) that assumes a consistent winding across the engine's own geometry.
            indices.push_back(a); indices.push_back(b); indices.push_back(d);
            indices.push_back(a); indices.push_back(d); indices.push_back(c);
        }
    }
    gridIndexCount_ = static_cast<u32>(indices.size());

    rhi::BufferDesc vbd;
    vbd.bytes = static_cast<u64>(verts.size()) * sizeof(WaterVertex);
    // BufferKind::Upload, not Default: IResourceFactory::writeBuffer's own contract (RHIResources.hpp)
    // is that it only writes a BufferKind::Upload buffer, and BufferDesc has no initial-data member
    // at all (unlike TextureDesc's initialData/initialDataCount) -- there is no Default-plus-init-data
    // path anywhere in this RHI to verify against, only this one. ParticleRenderer's own vertex/index
    // buffers use the identical BufferKind::Upload for the identical reason (see its ensureCapacity).
    // The grid never changes after this call, so "CPU-writable" is a one-time cost, not a per-frame
    // one -- exactly the property this class's header comment argues makes a static upload correct
    // here even though ParticleRenderer treats Upload as something to re-write every frame.
    vbd.kind = rhi::BufferKind::Upload;
    vbd.debugName = "Aver.Fluids grid vertices";
    gridVb_ = res_->createBuffer(vbd);

    rhi::BufferDesc ibd;
    ibd.bytes = static_cast<u64>(indices.size()) * sizeof(u32);
    ibd.kind = rhi::BufferKind::Upload;
    ibd.debugName = "Aver.Fluids grid indices";
    gridIb_ = res_->createBuffer(ibd);

    if (!gridVb_ || !gridIb_) {
        AVER_ERROR("[Water] grid buffer allocation failed");
        return false;
    }
    if (!res_->writeBuffer(gridVb_, verts.data(), vbd.bytes) ||
        !res_->writeBuffer(gridIb_, indices.data(), ibd.bytes)) {
        AVER_ERROR("[Water] grid buffer upload failed");
        return false;
    }

    // Best-effort initial build (ParticleRenderer::init's identical two-step pattern): the real
    // scene target's sample count and formats arrive through onRenderTargetsChanged, called by the
    // device before the first real frame, so this just means the very first registration is never
    // pipeline-less.
    buildPipeline(dev.sampleCount(), dev.backbufferFormat(), dev.depthFormat());

    AVER_INFO("[Water] ready: {}x{} grid, {}cm cells, {}cm extent",
              kGridVertsPerSide, kGridVertsPerSide, kCellSizeCm, kGridExtentCm);
    return true;
}

void WaterRenderer::shutdown() {
    if (res_) {
        res_->destroyBuffer(gridVb_);
        res_->destroyBuffer(gridIb_);
        res_->destroyPipeline(pso_);
        res_->destroyShader(vs_);
        res_->destroyShader(ps_);
    }
    gridVb_ = gridIb_ = 0;
    pso_ = 0;
    vs_ = ps_ = 0;
    gridIndexCount_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

void WaterRenderer::tick(f32 dtSeconds) {
    // A NaN/negative/spiked dt guard, the same general caution aver_phys_step's own maxCatchUp clamp
    // takes around a dt spike (PhysicsWorld.cpp) -- a stalled frame (a breakpoint, a device-lost
    // recovery) handing this a multi-second dt would jump the wave clock far enough that the sum's
    // own trig arguments lose precision, and a negative or NaN dt (a caller's own bug, or a first
    // frame whose previous timestamp was never initialised) must not corrupt elapsedSeconds_ for
    // every frame after it. Clamped to [0, 1] rather than rejected outright: a feature's tick() has
    // no way to signal failure back to whatever drives it once per frame, so the least-surprising
    // behaviour for an out-of-range dt is "the clock advances by at most one second this call," not
    // "the clock silently stops advancing at all."
    if (!(dtSeconds >= 0.0f) || dtSeconds > 1.0f) {
        if (!loggedBadDt_) {
            AVER_WARN("[Water] tick() dt {} is out of the expected [0, 1] range; clamping", dtSeconds);
            loggedBadDt_ = true;
        }
        dtSeconds = dtSeconds > 1.0f ? 1.0f : 0.0f;   // NaN also fails ">= 0.0f" above and lands here as 0
    }
    elapsedSeconds_ += dtSeconds;
}

void WaterRenderer::setWaves(const GerstnerWave* waves, size_t count) {
    const bool overflowed = count > kMaxGerstnerWaves;
    const size_t copyCount = overflowed ? kMaxGerstnerWaves : count;
    for (size_t i = 0; i < copyCount; ++i) waves_[i] = waves[i];
    // Unused slots are zeroed to an EXPLICITLY inert wave (amplitude 0), not left holding
    // GerstnerWave{}'s own non-zero defaults (amplitudeCm=25 among them) -- a stale non-zero
    // amplitude in a slot past waveCount_ would never actually be READ (both gerstnerHeightCm-style
    // C++ callers and rhi::shaderFile("water.hlsl").c_str()'s averGerstnerDisplace loop only up to the real count), but leaving
    // it there anyway invites exactly the bug this comment is here to prevent: a future edit to
    // EITHER loop bound that starts reading past waveCount_ would silently pick up a hardcoded wave
    // instead of nothing.
    for (size_t i = copyCount; i < kMaxGerstnerWaves; ++i) {
        waves_[i] = GerstnerWave{};
        waves_[i].amplitudeCm = 0.0f;
    }
    waveCount_ = copyCount;

    if (overflowed && !loggedWaveOverflow_) {
        AVER_WARN("[Water] setWaves: {} waves requested, kMaxGerstnerWaves is {}; the rest are dropped",
                  count, kMaxGerstnerWaves);
        loggedWaveOverflow_ = true;
    }
}

void WaterRenderer::setColors(const f32 shallowRGB[3], const f32 deepRGB[3]) {
    std::memcpy(shallowColor_, shallowRGB, sizeof(shallowColor_));
    std::memcpy(deepColor_, deepRGB, sizeof(deepColor_));
}

void WaterRenderer::setWaterBoundsCm(f32 minXCm, f32 minYCm, f32 maxXCm, f32 maxYCm) {
    boundsEnabled_ = true;
    boundsMinXCm_ = minXCm;
    boundsMinYCm_ = minYCm;
    boundsMaxXCm_ = maxXCm;
    boundsMaxYCm_ = maxYCm;
}

void WaterRenderer::clearWaterBounds() {
    boundsEnabled_ = false;
}

bool WaterRenderer::buildPipeline(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    if (!res_ || !vs_ || !ps_) return false;

    res_->destroyPipeline(pso_);
    pso_ = 0;

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs_;
    gd.ps = ps_;

    gd.vertexLayout.stride = sizeof(WaterVertex);
    gd.vertexLayout.attribCount = 1;
    gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};

    // DEPTH-TESTED, DEPTH-WRITE OFF. Write MUST stay off: this is the render-feature scout's central
    // risk item for this whole slice, and the reasoning is the same IRenderFeature::transparentPass's
    // own doc comment gives and ParticleRenderer::buildPipelines already relies on -- a blended
    // surface that also WROTE depth would occlude anything drawn behind it in a LATER transparent
    // draw this same frame (there is exactly one transparent pass per frame today, so that is not
    // live yet, but getting this wrong here is also what would make a future second transparent
    // feature -- say, foam -- draw incorrectly against water without any change on that feature's own
    // side), and would corrupt any depth-based effect that runs AFTER this pass reads the scene depth
    // buffer as it was left.
    gd.depth.test = true;
    gd.depth.write = false;

    // PREMULTIPLIED alpha -- REVERSED from this pipeline's own prior choice, and the reason is worth
    // stating precisely because this file used to argue the opposite conclusion at this exact line.
    //
    // The OLD PSWater output straight alpha: colorLinear = lerp(deepColor, shallowColor, fresnel) plus
    // a Blinn-Phong highlight, never divided by or otherwise scaled against alpha, so AlphaBlend's
    // `src.rgb*src.a + dst.rgb*(1-src.a)` was the correct composite for THAT rgb. The defect was never
    // in the blend math; it was that the rgb being blended had no real reflection in it to protect --
    // a flat shallowColor constant loses nothing important by being scaled down at low alpha, because
    // it was never anything but decoration.
    //
    // PSWater now outputs a genuine specular term (skyColor(R) mirror reflection plus GGX sun
    // glitter, WaterShaders.hpp's `specular`) that must land at FULL strength however transparent the
    // surface is authored to be: a pane of water set to alpha=0.2 still reflects the sky at 100%
    // strength in real life, exactly the glass defect this whole shading contract (see this module's
    // brief) exists to fix. Straight AlphaBlend would multiply that reflection by alpha too, which is
    // the ORIGINAL "flat cartoon" complaint reproducing itself one level deeper the moment a real
    // reflection existed to attenuate. Premultiplied alpha (rgb = specular + diffuse*alpha, a = alpha
    // -- PSWater's own final line) sends `specular` through the blend state's ONE(rgb) + INV_SRC_ALPHA
    // (dst) unattenuated, and only the transmitted body colour (`diffuse`, already multiplied into rgb
    // by the shader) shrinks with coverage. This is the identical trade ParticleRenderer already made
    // for the identical reason (packPremultiplied, ParticleRenderer.cpp) -- the two pipelines converge
    // here not because one was copying the other's convention without cause, but because both now
    // shade something whose bright term must survive transparency.
    gd.blend = rhi::BlendMode::PremultipliedAlpha;

    // None, not Back: the camera can be above OR below the surface (see Underwater.hpp's whole
    // reason to exist), and underwater must still see the plane from below -- a culled pipeline would
    // make the water vanish the instant the camera crosses it, rather than reading as a surface with
    // two visible sides.
    gd.cull = rhi::CullMode::None;

    gd.renderTargetCount = 1;
    gd.renderTargets[0] = color;
    gd.depthFormat = depth;
    // MSAA IS BAKED INTO THE PIPELINE AT CREATION, the same DECIDED-1 rule ParticleRenderer's own
    // buildPipelines comment cites: the scene target's sample count, not a hardcoded 1, rebuilt here
    // whenever onRenderTargetsChanged reports it changed.
    gd.sampleCount = sampleCount;

    // A root CBV (zero dwords) at b4 -- PathTracer's exact pattern (PathTracer.cpp's own comment on
    // its FrameCB: "the block is more than root constants should carry, and setConstantBuffer
    // suballocates it from the frame's upload ring"). Slot 0 is left untouched: PipelineLayout's own
    // comment reserves it for the engine's per-frame block, bound automatically on setPipeline.
    gd.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;

    pso_ = res_->createGraphicsPipeline(gd);

    bakedSampleCount_ = sampleCount;
    bakedColorFmt_ = color;
    bakedDepthFmt_ = depth;

    if (pso_) {
        AVER_INFO("[Water] pipeline (re)built: {}x MSAA", sampleCount);
    } else {
        AVER_ERROR("[Water] pipeline build failed at {}x MSAA", sampleCount);
    }
    return pso_ != 0;
}

void WaterRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                           u32 width, u32 height) {
    (void)width; (void)height;
    if (sampleCount == bakedSampleCount_ && color == bakedColorFmt_ && depth == bakedDepthFmt_) return;
    buildPipeline(sampleCount, color, depth);
}

void WaterRenderer::transparentPass(rhi::IRenderContext& ctx) {
    if (!pso_ || !dev_) return;

    f32 viewProj[16], camPos[3];
    if (!dev_->camera(viewProj, nullptr, camPos)) return;   // no inverse projection needed here

    // AXIS NOTE: GerstnerWave.hpp's snapWorldToGridCm and gerstnerDisplaceCm are written generically
    // over "x"/"z" as the two horizontal axes, borrowing GPU Gems' own x/z-horizontal convention --
    // that header has no idea which world axis is vertical. This engine is +Z-UP (centimetres, +X
    // forward, +Y right, +Z up -- physics_abi.h's own top-of-file convention), so the grid's plane is
    // XY, not XZ: camPos[0]/camPos[1] below are engine X/Y, and both are what gets snapped to and fed
    // into GerstnerWave.hpp's "x"/"z" parameters. This is purely a NAMING correction -- see
    // WaterShaders.hpp's identical note at its own axis-sensitive line -- not a different formula.
    //
    // UNBOUNDED keeps the exact behaviour this grid always had: the origin snaps to the camera so
    // the puck rides along underneath it, and the scale is 1 so the shader's localXY is the plain
    // local-plus-origin sum it has always been.
    //
    // BOUNDED swaps what the origin and scale MEAN rather than adding a second code path in the
    // shader: the origin becomes the centre of the bounds instead of the camera, and the scale maps
    // the grid's fixed 25600cm extent onto however wide the bounds actually are. This is deliberately
    // NOT done by rasterising the full grid and discarding fragments outside the bounds in the pixel
    // shader -- clipping would still pay for 25600cm of geometry to draw a pool a few metres across,
    // and the pool itself would sit at the grid's native 200cm cell size, three-or-so vertices from
    // edge to edge, which reads as a faceted tarp rather than a water surface. Scaling instead spends
    // every one of the grid's 129x129 vertices on the water that actually exists, which is both the
    // cheaper draw and the better-looking one. The trade is that a bounded surface's wave
    // tessellation now depends on its own size -- a small pool samples the same wave shapes at a
    // finer resolution than the ocean does -- and that is the right trade to make here, not an
    // oversight.
    f32 gridOriginX, gridOriginY, gridScaleX, gridScaleY;
    if (boundsEnabled_) {
        gridOriginX = (boundsMinXCm_ + boundsMaxXCm_) * 0.5f;
        gridOriginY = (boundsMinYCm_ + boundsMaxYCm_) * 0.5f;
        gridScaleX = (boundsMaxXCm_ - boundsMinXCm_) / kGridExtentCm;
        gridScaleY = (boundsMaxYCm_ - boundsMinYCm_) / kGridExtentCm;
    } else {
        gridOriginX = snapWorldToGridCm(camPos[0], kCellSizeCm);
        gridOriginY = snapWorldToGridCm(camPos[1], kCellSizeCm);
        gridScaleX = 1.0f;
        gridScaleY = 1.0f;
    }

    WaterFrameCB cb{};
    for (size_t i = 0; i < kMaxGerstnerWaves; ++i) {
        cb.waveDirSteep[i][0] = waves_[i].dirX;
        cb.waveDirSteep[i][1] = waves_[i].dirZ;
        cb.waveDirSteep[i][2] = waves_[i].steepness;
        cb.waveDirSteep[i][3] = 0.0f;
        cb.waveLenAmp[i][0] = waves_[i].wavelengthCm;
        cb.waveLenAmp[i][1] = waves_[i].amplitudeCm;
        cb.waveLenAmp[i][2] = 0.0f;
        cb.waveLenAmp[i][3] = 0.0f;
    }
    cb.gridOriginCount[0] = gridOriginX;
    cb.gridOriginCount[1] = gridOriginY;
    cb.gridOriginCount[2] = static_cast<f32>(waveCount_);
    cb.gridOriginCount[3] = 0.0f;
    cb.waterState[0] = waterLevelCm_;
    cb.waterState[1] = elapsedSeconds_;
    // z/w used to be genuinely unused; they now carry gridScaleX/Y (see this function's own comment
    // above) rather than growing WaterFrameCB with a fifth row -- there was already room, and every
    // other field here is filled by this same function, so there is nowhere else a stale scale could
    // leak in from.
    cb.waterState[2] = gridScaleX;
    cb.waterState[3] = gridScaleY;
    cb.shallowColor[0] = shallowColor_[0]; cb.shallowColor[1] = shallowColor_[1];
    cb.shallowColor[2] = shallowColor_[2]; cb.shallowColor[3] = 0.0f;
    cb.deepColor[0] = deepColor_[0]; cb.deepColor[1] = deepColor_[1];
    cb.deepColor[2] = deepColor_[2]; cb.deepColor[3] = 0.0f;

    // Viewport, scissor and the scene colour+depth targets are ALL already set -- the
    // transparentPass contract (RHIResources.hpp) -- so nothing here touches any of them; only the
    // pipeline, the constant buffer and the (already-uploaded, never-changing) grid are ours.
    ctx.pushMarker("Aver.Fluids");
    ctx.setPipeline(pso_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb, sizeof(cb));
    ctx.setVertexBuffer(gridVb_, sizeof(WaterVertex));
    ctx.setIndexBuffer(gridIb_, rhi::Format::R32Uint);
    ctx.drawIndexed(gridIndexCount_, 0, 0);
    ctx.popMarker();
}

} // namespace aver::fluids
