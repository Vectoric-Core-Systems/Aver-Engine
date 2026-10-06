// The path-tracing compute pass: scenes in, linear radiance out.
#include "aver/pt/PathTracer.hpp"

#include <chrono>
#include <unordered_map>
#include "aver/core/Log.hpp"
#include "PtShaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "aver/rhi/ShaderFiles.hpp"   // this pass's HLSL is a deployed file

namespace aver::pt {

namespace {

// t0 acceleration structure, t1 vertices, t2 indices, t3 instances, t4 the energy-compensation table.
constexpr u32 kSrvCount = 6;   // t5: the scene light list

// E(cos(theta), roughness) table: 32x32 bilinear under 5e-3 furnace tolerance, 4 KB total.
constexpr u32 kEnergyLutDim = 32;
// Samples per cell: Hammersley (deterministic, not random).
constexpr u32 kEnergyLutSamples = 4096;
// u0 the accumulator.
constexpr u32 kUavCount = 1;
// The shader's [numthreads(8,8,1)].
constexpr u32 kGroup = 8;

// RayQuery is SM 6.5, and DXR 1.1 is what makes it available from a compute shader with no state
// object and no shader table.
constexpr u32 kShaderModel = 65;
constexpr u32 kRayTracingTier = 11;

// The untextured integrator binds five fixed SRVs and one UAV (kSrvCount/kUavCount) with Tier 1.
// The textured twin (bindless base-colours) requires Tier 3 and lives in its own pipeline.
// rtBindlessTextures gate = rayTracingTier >= 11 && resourceBindingTier >= 3.

// Bindless base-colour table's fixed size (matches root signature 1.0 compile-time limit).
constexpr u32 kBindlessTexCapacity = 4096;

// Builds E(cos(theta), roughness): fraction of incident light a single-scatter GGX lobe with F=1
// returns. Duplicates ptScatterSpecular in pt_pathtrace.hlsl exactly; compensation is only correct
// if dividing by the energy THIS estimator loses, not a published fit.
std::vector<f32> buildEnergyLut() {
    std::vector<f32> lut(static_cast<usize>(kEnergyLutDim) * kEnergyLutDim, 1.0f);
    for (u32 ri = 0; ri < kEnergyLutDim; ++ri) {
        // Endpoints inclusive (cell 0 = 0.0, cell D-1 = 1.0), not texel centres.
        const f32 rough = static_cast<f32>(ri) / static_cast<f32>(kEnergyLutDim - 1);
        const f32 a  = std::max(rough * rough, 1e-3f);
        const f32 k  = a * 0.5f;
        for (u32 mi = 0; mi < kEnergyLutDim; ++mi) {
            // Endpoint-inclusive; clamped off zero to avoid dividing at surface-plane view.
            const f32 ndv = std::max(static_cast<f32>(mi) / static_cast<f32>(kEnergyLutDim - 1),
                                     1e-3f);
            // View vector in tangent frame; only elevation matters (GGX isotropic).
            const f32 vx = std::sqrt(std::max(0.0f, 1.0f - ndv * ndv));
            f64 sum = 0.0;
            for (u32 s = 0; s < kEnergyLutSamples; ++s) {
                // Hammersley: van der Corput radical inverse in base 2.
                const f32 u1 = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kEnergyLutSamples);
                u32 bits = s;
                bits = (bits << 16) | (bits >> 16);
                bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
                bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
                bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
                bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
                const f32 u2 = static_cast<f32>(static_cast<f64>(bits) * 2.3283064365386963e-10);

                const f32 phi = 6.2831853071795864f * u1;
                const f32 ct  = std::sqrt(std::max(0.0f, (1.0f - u2) / (1.0f + (a * a - 1.0f) * u2)));
                const f32 st  = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
                const f32 hx = st * std::cos(phi), hy = st * std::sin(phi), hz = ct;

                // l = reflect(-v, h) = 2(v.h)h - v; only elevation and (v.h) used.
                const f32 vdh = vx * hx + ndv * hz;
                const f32 lz  = 2.0f * vdh * hz - ndv;
                if (!(lz > 0.0f) || !(vdh > 0.0f) || !(hz > 0.0f)) continue;   // below the horizon
                (void)hy;

                const f32 g1v = ndv / (ndv * (1.0f - k) + k);
                const f32 g1l = lz  / (lz  * (1.0f - k) + k);
                // Weight = G * (v.h) / (n.v * n.h) with F=1.
                sum += static_cast<f64>(g1v * g1l * vdh / std::max(ndv * hz, 1e-6f));
            }
            const f32 e = static_cast<f32>(sum / static_cast<f64>(kEnergyLutSamples));
            // Clamped away from zero; shader divides by this, which is infinity at grazing rough surfaces.
            lut[static_cast<usize>(ri) * kEnergyLutDim + mi] = std::clamp(e, 1e-2f, 1.0f);
        }
    }
    return lut;
}

// Integrator pipeline layout. bindlessTextures=0 is the untextured layout for bit-identical replay;
// bindlessTextures>0 appends base-colour table and sampler in their own register space.
rhi::PipelineLayout ptLayout(u32 bindlessTextures) {
    rhi::PipelineLayout l{};
    l.srvCount = kSrvCount;
    l.uavCount = kUavCount;
    // b4 is a ROOT CBV (zero dwords); the 112-byte block suballocates from frame upload ring.
    l.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;
    if (bindlessTextures) {
        // Wrap (UV tiling is material's business); Linear (compute kernel has no screen derivatives).
        l.samplers[0].filter  = rhi::Filter::Linear;
        l.samplers[0].address = rhi::AddressMode::Wrap;
        l.samplerCount = 1;
        l.bindlessTextureCount = bindlessTextures;
    }
    return l;
}

// Mirrors cbuffer PtFrame in rhi::shaderFile("pt_pathtrace.hlsl"), field for field.
struct FrameCB {
    f32 origin[4];
    f32 forward[4];
    f32 right[4];
    f32 up[4];
    u32 image[4];
    u32 sample[4];
    f32 trace[4];
};

// Seven float4/uint4 rows, matching PtShaders.hpp. Catches field additions on one side only.
static_assert(sizeof(FrameCB) == 112, "cbuffer PtFrame in PtShaders.hpp mirrors this byte for byte");
static_assert(sizeof(FrameCB) % 16 == 0, "must be a legal constant-buffer size");

} // namespace

PathTracer::~PathTracer() { shutdown(); }

bool PathTracer::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;

    const rhi::DeviceCaps caps = dev.caps();
    if (caps.rayTracingTier < kRayTracingTier || caps.shaderModel < kShaderModel ||
        !caps.dxcAvailable || !caps.computeShaders) {
        AVER_INFO("[PT] unavailable on this device: RT tier {}, SM {}, DXC {} (needs tier {}, SM {})",
                  caps.rayTracingTier, caps.shaderModel, caps.dxcAvailable, kRayTracingTier,
                  kShaderModel);
        shutdown();
        return false;
    }

    rhi::ShaderDesc sd;
    sd.source  = rhi::shaderFile("pt_pathtrace.hlsl").c_str();
    // Engine's own declarations (PI, skyColor, furnace contract).
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry   = "CSPathTrace";
    sd.stage   = rhi::ShaderStage::Compute;
    sd.minShaderModel = kShaderModel;
    // Energy table dimension as define to keep it synchronized with buildEnergyLut().
    const std::string baseDefs = "AVER_PT_ENERGY_DIM=" + std::to_string(kEnergyLutDim);
    sd.defines = baseDefs.c_str();
    cs_ = res_->createShader(sd);
    if (!cs_) {
        // HLSL compiled at runtime by DXC.
        AVER_ERROR("[PT] the integrator would not compile");
        shutdown();
        return false;
    }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout = ptLayout(0);
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[PT] integrator pipeline unavailable"); shutdown(); return false; }

    // Built before any target; createTarget binds it into every target's descriptor table.
    for (u32 i = 0; i < kLightRing; ++i) {
        rhi::BufferDesc ld;
        ld.bytes = sizeof(PtLight) * kPtMaxLights;
        ld.kind = rhi::BufferKind::Upload;
        ld.debugName = "pt scene lights";
        lightBuf_[i] = res_->createBuffer(ld);
        const std::vector<PtLight> zero(kPtMaxLights);
        if (!lightBuf_[i] || !res_->writeBuffer(lightBuf_[i], zero.data(), ld.bytes)) {
            AVER_ERROR("[PT] the light list buffer could not be created");
            shutdown();
            return false;
        }
    }

    {
        const std::vector<f32> lut = buildEnergyLut();
        rhi::BufferDesc ed;
        ed.bytes = lut.size() * sizeof(f32);
        // Upload, not Default; 4 KB of read-only constants written once.
        ed.kind = rhi::BufferKind::Upload;
        ed.debugName = "pt energy compensation";
        energyLut_ = res_->createBuffer(ed);
        if (!energyLut_ || !res_->writeBuffer(energyLut_, lut.data(), ed.bytes)) {
            AVER_ERROR("[PT] the energy-compensation table could not be uploaded");
            shutdown();
            return false;
        }
    }

    AVER_INFO("[PT] path tracer ready (RayQuery, SM {}, {}x{} threads per group)",
              kShaderModel, kGroup, kGroup);
    return true;
}

// Textured twin built on first demand; texTried_ ensures one attempt per session.
bool PathTracer::ensureTexturing() {
    if (texTried_) return texPipeline_ != 0;
    texTried_ = true;
    if (!res_ || !dev_ || !pipeline_) return false;

    if (!dev_->caps().rtBindlessTextures) {
        AVER_INFO("[PT] no bindless texture support on this device; surfaces shade from their flat "
                  "albedo, exactly as before textures existed (said once)");
        return false;
    }

    texTable_ = res_->createBindlessTextureTable(kBindlessTexCapacity);
    if (!texTable_) {
        AVER_WARN("[PT] the bindless texture table could not be created; surfaces keep their flat "
                  "albedo (said once)");
        return false;
    }

    rhi::ShaderDesc sd;
    sd.source  = rhi::shaderFile("pt_pathtrace.hlsl").c_str();
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry   = "CSPathTrace";
    sd.stage   = rhi::ShaderStage::Compute;
    sd.minShaderModel = kShaderModel;
    // AVER_PT_TEX_CAPACITY and bindlessTextureCount must equal kBindlessTexCapacity exactly.
    const std::string defs = "AVER_PT_BINDLESS=1;AVER_PT_TEX_CAPACITY=" +
                             std::to_string(kBindlessTexCapacity) +
                             ";AVER_PT_ENERGY_DIM=" + std::to_string(kEnergyLutDim);
    sd.defines = defs.c_str();
    texCs_ = res_->createShader(sd);
    if (!texCs_) {
        AVER_WARN("[PT] the textured integrator would not compile; flat albedo stands in");
        return false;
    }

    rhi::ComputePipelineDesc pd;
    pd.cs = texCs_;
    pd.layout = ptLayout(kBindlessTexCapacity);
    texPipeline_ = res_->createComputePipeline(pd);
    if (!texPipeline_) {
        AVER_WARN("[PT] the textured integrator pipeline could not be created; flat albedo stands in");
        return false;
    }
    AVER_INFO("[PT] textured integrator ready ({} texture slots)", kBindlessTexCapacity);
    return true;
}

u32 PathTracer::residentTexture(rhi::TextureHandle h) {
    if (!h) return kUnboundTexture;
    if (!ensureTexturing()) return kUnboundTexture;
    if (const auto it = texIndex_.find(h); it != texIndex_.end()) return it->second;
    if (texCount_ >= kBindlessTexCapacity) {
        // Refused, not wrapped. Unbound index falls back to flat albedo instead of silently
        // painting one material with another's texture.
        if (!texWarned_) {
            texWarned_ = true;
            AVER_WARN("[PT] the {} bindless texture slots are full; further materials shade from "
                      "their flat albedo (said once)", kBindlessTexCapacity);
        }
        return kUnboundTexture;
    }
    const u32 index = texCount_;
    if (!res_->setBindlessTexture(texTable_, index, h)) return kUnboundTexture;
    ++texCount_;
    texIndex_.emplace(h, index);
    return index;
}

void PathTracer::shutdown() {
    if (res_) {
        if (energyLut_)   res_->destroyBuffer(energyLut_);
        for (rhi::BufferHandle& b : lightBuf_) { if (b) res_->destroyBuffer(b); b = 0; }
        for (auto& kv : lightTex_) if (kv.second.tex) res_->destroyTexture(kv.second.tex);
        if (verts_)       res_->destroyBuffer(verts_);
        if (indices_)     res_->destroyBuffer(indices_);
        if (instanceBuf_) res_->destroyBuffer(instanceBuf_);
        // BLAS handles: not owned here. A BLAS belongs to its mesh; destroyMesh frees it.
        // TLASes: no destroyTlas exists; handles leak to device lifetime.
        tlasPool_.clear();
        tlasPoolCap_.clear();
        if (pipeline_)    res_->destroyPipeline(pipeline_);
        if (cs_)          res_->destroyShader(cs_);
        if (texPipeline_) res_->destroyPipeline(texPipeline_);
        if (texCs_)       res_->destroyShader(texCs_);
        // Texture table not destroyed (descriptors unsafe to recycle until frames complete).
        // Index map is cleared alongside, so re-init cannot use stale handles.
    }
    lightTex_.clear();
    lights_.clear();
    lightsHash_ = 0;
    lightsDirty_ = false;
    lightSlot_ = 0;
    texTable_ = 0;
    texIndex_.clear();
    texCount_ = 0;
    texTried_ = texWarned_ = false;
    texPipeline_ = 0;
    texCs_ = 0;
    blasCache_.clear();
    // Cleared with the buffers; kept populated would let next prepare() reuse freed handles.
    geoMeshes_.clear();
    geoVerts_ = geoIndices_ = 0;
    geometryReused_ = false;
    verts_ = indices_ = instanceBuf_ = 0;
    energyLut_ = 0;
    pipeline_ = 0;
    cs_ = 0;
    surfaces_.clear();
    instances_.clear();
    scenes_.clear();
    blas_.clear();
    totalVerts_ = totalIndices_ = 0;
    prepared_ = false;
    built_ = false;
    dev_ = nullptr;
    res_ = nullptr;
}

u32 PathTracer::addSurface(const PtSurface& s) {
    surfaces_.push_back(s);
    return static_cast<u32>(surfaces_.size() - 1);
}

u32 PathTracer::addScene(const u32* surfaceIds, u32 count) {
    Scene s;
    s.surfaces.assign(surfaceIds, surfaceIds + count);
    if (res_) {
        // TLAS reused per slot, not shared between scenes. Grows by doubling.
        const usize slot = scenes_.size();
        if (tlasPool_.size() <= slot) { tlasPool_.resize(slot + 1, 0); tlasPoolCap_.resize(slot + 1, 0); }

        const u32 need = count ? count : 1;
        if (!tlasPool_[slot] || tlasPoolCap_[slot] < need) {
            u32 cap = tlasPoolCap_[slot] ? tlasPoolCap_[slot] : 1;
            while (cap < need) cap <<= 1;
            const rhi::TlasHandle grown = res_->createTlas(cap);
            // Failed grow keeps old handle; zero would mean no scene at all.
            if (grown) { tlasPool_[slot] = grown; tlasPoolCap_[slot] = cap; }
        }
        s.tlas = tlasPool_[slot];
    }
    scenes_.push_back(std::move(s));
    return static_cast<u32>(scenes_.size() - 1);
}

bool PathTracer::prepare() {
    if (!res_ || !dev_ || surfaces_.empty()) return false;
    if (prepared_) return true;
    const auto tPrep0 = std::chrono::steady_clock::now();

    // Per surface, not per mesh: deduplicates when two surfaces share a mesh.
    instances_.resize(surfaces_.size());
    surfaceRow_.assign(surfaces_.size(), 0);
    meshRows_.clear();
    totalVerts_ = totalIndices_ = 0;
    // Mesh handle -> row in this snapshot.
    std::unordered_map<rhi::MeshHandle, u32> rowOf;
    rowOf.reserve(surfaces_.size());
    for (usize i = 0; i < surfaces_.size(); ++i) {
        const rhi::MeshHandle mesh = surfaces_[i].mesh;
        auto found = rowOf.find(mesh);
        if (found == rowOf.end()) {
            rhi::BufferHandle vb = 0, ib = 0;
            u32 vc = 0, ic = 0;
            if (!dev_->meshGeometry(mesh, &vb, &ib, &vc, &ic)) {
                AVER_ERROR("[PT] surface {} has no readable geometry; the backend cannot express it", i);
                return false;
            }
            MeshRow row;
            row.mesh        = mesh;
            row.firstVertex = totalVerts_;
            row.firstIndex  = totalIndices_;
            row.vertexCount = vc;
            row.indexCount  = ic;
            totalVerts_   += vc;
            totalIndices_ += ic;
            found = rowOf.emplace(mesh, static_cast<u32>(meshRows_.size())).first;
            meshRows_.push_back(row);
        }
        const MeshRow& row = meshRows_[found->second];
        surfaceRow_[i] = found->second;
        Instance& inst = instances_[i];
        std::memcpy(inst.objectToWorld, surfaces_[i].world, sizeof(inst.objectToWorld));
        std::memcpy(inst.albedo, surfaces_[i].albedo, sizeof(inst.albedo));
        inst.ior = surfaces_[i].ior;
        inst.baseColorTex  = surfaces_[i].baseColorTex;
        inst.roughness     = surfaces_[i].roughness;
        inst.metallic      = surfaces_[i].metallic;
        inst.metalRoughTex = surfaces_[i].metalRoughTex;
        inst.normalTex     = surfaces_[i].normalTex;
        inst.normalScale   = surfaces_[i].normalScale;
        std::memcpy(inst.emissive, surfaces_[i].emissive, sizeof(inst.emissive));
        // Shared rows: geometry is the same, per-surface data is transforms/albedo/ior/base-colour index.
        inst.firstVertex = row.firstVertex;
        inst.firstIndex  = row.firstIndex;
    }
    if (totalVerts_ == 0 || totalIndices_ == 0) return false;

    // Same meshes in same order means table is byte-for-byte identical; skip allocation and copy.
    std::vector<rhi::MeshHandle> meshOrder;
    meshOrder.reserve(meshRows_.size());
    for (const MeshRow& row : meshRows_) meshOrder.push_back(row.mesh);
    // Check both mesh set and totals; mismatch would mean GPU out-of-bounds read.
    geometryReused_ = verts_ && indices_ && meshOrder == geoMeshes_ &&
                      totalVerts_ == geoVerts_ && totalIndices_ == geoIndices_;

    if (!geometryReused_) {
        // Different mesh set: offsets have moved, old buffers are wrong.
        if (verts_)   res_->destroyBuffer(verts_);
        if (indices_) res_->destroyBuffer(indices_);
        verts_ = indices_ = 0;

        rhi::BufferDesc vd;
        vd.bytes = static_cast<u64>(totalVerts_) * sizeof(rhi::MeshVertex);
        vd.kind  = rhi::BufferKind::Default;
        vd.debugName = "pt vertices";
        verts_ = res_->createBuffer(vd);

        rhi::BufferDesc id;
        id.bytes = static_cast<u64>(totalIndices_) * sizeof(u32);
        id.kind  = rhi::BufferKind::Default;
        id.debugName = "pt indices";
        indices_ = res_->createBuffer(id);

        geoMeshes_  = std::move(meshOrder);
        geoVerts_   = totalVerts_;
        geoIndices_ = totalIndices_;
    }

    // Upload: written once from CPU, never by GPU.
    rhi::BufferDesc nd;
    nd.bytes = sizeof(Instance) * instances_.size();
    nd.kind  = rhi::BufferKind::Upload;
    nd.debugName = "pt instances";
    instanceBuf_ = res_->createBuffer(nd);

    if (!verts_ || !indices_ || !instanceBuf_) {
        AVER_ERROR("[PT] could not allocate the flat geometry table");
        return false;
    }
    res_->writeBuffer(instanceBuf_, instances_.data(), nd.bytes);

    // One BLAS per distinct mesh; two surfaces on one mesh share one BLAS.
    const auto tBuf = std::chrono::steady_clock::now();
    blas_.assign(meshRows_.size(), 0);
    blasFresh_.assign(meshRows_.size(), 0);
    u32 reused = 0;
    for (usize i = 0; i < meshRows_.size(); ++i) {
        const rhi::MeshHandle mesh = meshRows_[i].mesh;
        if (const auto hit = blasCache_.find(mesh); hit != blasCache_.end()) {
            blas_[i] = hit->second;
            ++reused;
            continue;
        }
        // Try reusing a shared BLAS from another feature that may have already built one.
        // (VoxiRenderer keeps its own map over the same static meshes.)
        if (const rhi::BlasHandle shared = res_->blasForMesh(mesh)) {
            blas_[i] = shared;
            ++reused;
            blasCache_.emplace(mesh, shared);
            continue;
        }
        blas_[i] = res_->createBlas(mesh);
        if (!blas_[i]) { AVER_ERROR("[PT] no bottom-level structure for mesh {}", mesh); return false; }
        blasFresh_[i] = 1;
        blasCache_.emplace(mesh, blas_[i]);
    }
    const auto tBlas = std::chrono::steady_clock::now();
    AVER_INFO("[PT] flat table: {} surface(s) over {} distinct mesh(es), {} vertices, {} indices"
              " | geometry {}, buffers {:.1f}ms, createBlas {:.1f}ms ({} built, {} reused)",
              static_cast<u32>(surfaces_.size()), static_cast<u32>(meshRows_.size()),
              totalVerts_, totalIndices_,
              geometryReused_ ? "REUSED" : "rebuilt",
              std::chrono::duration<f64, std::milli>(tBuf - tPrep0).count(),
              std::chrono::duration<f64, std::milli>(tBlas - tBuf).count(),
              static_cast<u32>(meshRows_.size()) - reused, reused);
    for (const Scene& s : scenes_) if (!s.tlas) {
        AVER_ERROR("[PT] a scene has no top-level structure");
        return false;
    }

    prepared_ = true;
    return true;
}

bool PathTracer::buildScenes(rhi::IRenderContext& ctx) {
    if (!prepared_ || built_) return built_;

    ctx.pushMarker("Aver.PathTracer build");
    // Build only the new BLASes; built structures stay valid until destroyed.
    for (usize i = 0; i < blas_.size(); ++i) if (blasFresh_[i]) ctx.buildBlas(blas_[i]);

    if (!geometryReused_) {
        // Explicit transitions: RHI tracker doesn't model implicit promotion.
        ctx.bufferBarrier(verts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
        ctx.bufferBarrier(indices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
        // Per distinct mesh, matching the table prepare() laid out.
        for (const MeshRow& row : meshRows_) {
            rhi::BufferHandle vb = 0, ib = 0;
            u32 vc = 0, ic = 0;
            if (!dev_->meshGeometry(row.mesh, &vb, &ib, &vc, &ic)) { ctx.popMarker(); return false; }
            ctx.copyBuffer(verts_, vb, static_cast<u64>(vc) * sizeof(rhi::MeshVertex),
                           static_cast<u64>(row.firstVertex) * sizeof(rhi::MeshVertex), 0);
            ctx.copyBuffer(indices_, ib, static_cast<u64>(ic) * sizeof(u32),
                           static_cast<u64>(row.firstIndex) * sizeof(u32), 0);
        }
        ctx.bufferBarrier(verts_,   rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
        ctx.bufferBarrier(indices_, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
    }

    for (const Scene& s : scenes_) {
        std::vector<rhi::TlasInstance> inst;
        inst.reserve(s.surfaces.size());
        for (u32 id : s.surfaces) {
            if (id >= surfaces_.size()) continue;
            const u32 row = surfaceRow_[id];
            if (row >= blas_.size() || !blas_[row]) continue;
            rhi::TlasInstance i;
            std::memcpy(i.world, surfaces_[id].world, sizeof(i.world));
            i.mask = 0xFF;
            i.blas = blas_[row];
            // instanceId indexes the shared instance table for geometry and albedo lookup.
            i.instanceId = id & rhi::kMaxTlasInstanceId;
            inst.push_back(i);
        }
        if (inst.empty()) { ctx.popMarker(); return false; }
        ctx.buildTlas(s.tlas, inst.data(), static_cast<u32>(inst.size()));
    }
    ctx.popMarker();

    built_ = true;
    AVER_INFO("[PT] {} surfaces, {} scenes, {} vertices and {} indices in the flat table",
              surfaces_.size(), scenes_.size(), totalVerts_, totalIndices_);
    return true;
}

void PathTracer::setLights(const PtLight* lights, u32 count) {
    count = std::min(count, kPtMaxLights);
    if (!lights) count = 0;
    lights_.assign(lights, lights + count);
    u64 h = 1469598103934665603ull;
    const u8* bytes = reinterpret_cast<const u8*>(lights_.data());
    for (usize i = 0; i < lights_.size() * sizeof(PtLight); ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    h ^= lights_.size();
    h *= 1099511628211ull;
    if (h != lightsHash_) {
        lightsHash_ = h;
        lightsDirty_ = true;
    }
}

u32 PathTracer::lightTexture(u64 id, u32 width, u32 height, bool ies, const void* pixels) {
    if (!res_ || !pixels || width == 0 || height == 0) return kUnboundTexture;
    if (const auto it = lightTex_.find(id); it != lightTex_.end()) return it->second.index;
    LightTex lt;
    rhi::TextureDesc d;
    d.width = width;
    d.height = height;
    d.format = ies ? rhi::Format::R16F : rhi::Format::RGBA8UnormSrgb;
    d.bind = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    const void* levels[1] = {pixels};
    d.initialData = levels;
    d.initialDataCount = 1;
    d.initialRowPitch = width * (ies ? 2u : 4u);
    d.debugName = ies ? "pt light IES profile" : "pt light cookie";
    lt.tex = res_->createTexture(d);
    if (lt.tex) lt.index = residentTexture(lt.tex);
    lightTex_.emplace(id, lt);
    return lt.index;
}

bool PathTracer::createTarget(u32 scene, u32 width, u32 height, PtTarget& out) {
    out = {};
    if (!res_ || !prepared_ || scene >= scenes_.size() || width == 0 || height == 0) return false;

    rhi::BufferDesc ad;
    ad.bytes = static_cast<u64>(width) * height * kPtAccumElementsPerPixel * kPtAccumStride;
    ad.kind  = rhi::BufferKind::Default;
    ad.allowUnorderedAccess = true;
    ad.debugName = "pt accumulator";
    out.accum = res_->createBuffer(ad);

    rhi::BindingSetDesc bd;
    bd.srvCount = kSrvCount;
    bd.uavCount = kUavCount;
    bd.srvKinds[0] = rhi::SlotKind::AccelerationStructure;
    bd.srvKinds[1] = bd.srvKinds[2] = bd.srvKinds[3] = rhi::SlotKind::StructuredBuffer;
    bd.srvKinds[4] = rhi::SlotKind::StructuredBuffer;
    bd.srvKinds[5] = rhi::SlotKind::StructuredBuffer;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    out.set = res_->createBindingSet(bd);

    if (!out.accum || !out.set) {
        AVER_ERROR("[PT] could not allocate an accumulation target");
        destroyTarget(out);
        return false;
    }

    // One binding set per target, never shared and rebound between dispatches.
    res_->setSrvTlas(out.set, 0, scenes_[scene].tlas);
    res_->setSrvBuffer(out.set, 1, verts_, sizeof(rhi::MeshVertex), totalVerts_, 0);
    res_->setSrvBuffer(out.set, 2, indices_, sizeof(u32), totalIndices_, 0);
    res_->setSrvBuffer(out.set, 3, instanceBuf_, sizeof(Instance),
                       static_cast<u32>(instances_.size()), 0);
    res_->setSrvBuffer(out.set, 4, energyLut_, sizeof(f32), kEnergyLutDim * kEnergyLutDim, 0);
    res_->setSrvBuffer(out.set, 5, lightBuf_[lightSlot_], sizeof(PtLight), kPtMaxLights, 0);
    res_->setUavBuffer(out.set, 0, out.accum, kPtAccumStride,
                       width * height * kPtAccumElementsPerPixel, 0);

    out.width = width;
    out.height = height;
    out.scene = scene;
    return true;
}

void PathTracer::resetScene() {
    if (res_) {
        // instanceBuf_ is per snapshot (one record per surface); verts_/indices_ belong to
        // geoMeshes_ table which outlives a snapshot. prepare() releases them if mesh set changes.
        if (instanceBuf_) res_->destroyBuffer(instanceBuf_);
        // BLAS handles not released here (owned by meshes). TLAS pool kept for reuse by next addScene().
    }
    instanceBuf_ = 0;
    surfaces_.clear();
    instances_.clear();
    scenes_.clear();
    blas_.clear();
    meshRows_.clear();
    surfaceRow_.clear();
    totalVerts_ = totalIndices_ = 0;
    prepared_ = false;
    built_ = false;
}

void PathTracer::destroyTarget(PtTarget& t) {
    if (res_) {
        if (t.accum) res_->destroyBuffer(t.accum);
        if (t.set)   res_->destroyBindingSet(t.set);
    }
    t = {};
}

void PathTracer::accumulate(rhi::IRenderContext& ctx, const PtTarget& t, const PtCamera& cam,
                            const PtDispatch& d) {
    if (!pipeline_ || !built_ || !t.valid()) return;

    FrameCB cb{};
    for (u32 i = 0; i < 3; ++i) {
        cb.origin[i]  = cam.origin[i];
        cb.forward[i] = cam.forward[i];
        cb.right[i]   = cam.right[i];
        cb.up[i]      = cam.up[i];
    }
    cb.origin[3]  = cam.tanHalfFov;
    cb.forward[3] = cam.aspect;
    cb.image[0] = t.width;
    cb.image[1] = t.height;
    cb.image[2] = d.maxBounces;
    cb.image[3] = static_cast<u32>(d.defect);
    cb.sample[0] = d.firstSample;
    cb.sample[1] = d.samples;
    cb.sample[2] = d.reset ? 1u : 0u;
    // sample[3]: roulette depth. 0 = never roulette.
    cb.sample[3] = d.rouletteDepth;
    cb.trace[0] = d.rayBias;
    cb.trace[1] = d.tMax;
    // trace[2]: sky environment. 1.0 = legacy pre-fix behaviour; 0.0 = default.
    cb.trace[2] = d.legacyEnvironment ? 1.0f : 0.0f;
    // trace[3]: scene light count; the list itself is t5.
    cb.trace[3] = static_cast<f32>(lights_.size());
    if (lightsDirty_) {
        lightSlot_ = (lightSlot_ + 1) % kLightRing;
        if (!lights_.empty())
            res_->writeBuffer(lightBuf_[lightSlot_], lights_.data(), sizeof(PtLight) * lights_.size(), 0);
        lightsDirty_ = false;
    }
    res_->setSrvBuffer(t.set, 5, lightBuf_[lightSlot_], sizeof(PtLight), kPtMaxLights, 0);

    rhi::ScopedGpuStat gpuStat(ctx, "PT accumulate");
    ctx.pushMarker("Aver.PathTracer");
    ctx.bufferBarrier(t.accum, rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);
    // Use textured pipeline only when texture is resident; untextured pipeline runs original compiled arithmetic.
    const bool textured = texturing();
    ctx.setPipeline(textured ? texPipeline_ : pipeline_);
    ctx.setBindingSet(t.set);
    // After setPipeline and setBindingSet: table is a root parameter of the bound pipeline.
    if (textured) ctx.setBindlessTable(texTable_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb, sizeof(cb));
    ctx.dispatch((t.width + kGroup - 1) / kGroup, (t.height + kGroup - 1) / kGroup, 1);
    // Buffer state doesn't survive the command list.
    ctx.bufferBarrier(t.accum, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::Common);
    ctx.popMarker();
}

void PathTracer::copyForReadback(rhi::IRenderContext& ctx, const PtTarget& t,
                                 rhi::BufferHandle readback, u64 dstOffset) {
    if (!t.valid() || !readback) return;
    ctx.bufferBarrier(t.accum, rhi::ResourceState::Common, rhi::ResourceState::CopySource);
    ctx.copyBuffer(readback, t.accum, t.bytes(), dstOffset, 0);
    ctx.bufferBarrier(t.accum, rhi::ResourceState::CopySource, rhi::ResourceState::Common);
}

} // namespace aver::pt
