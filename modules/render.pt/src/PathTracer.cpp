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
constexpr u32 kSrvCount = 5;

// The E(cos(theta), roughness) table's dimensions. Small on purpose: E is a smooth, monotone-ish
// function of both arguments with no features to resolve, so 32x32 bilinear is well under the
// furnace's 5e-3 tolerance while the whole table is 4 KB.
constexpr u32 kEnergyLutDim = 32;
// Samples per cell when integrating it. Hammersley, not random: the table must be identical on every
// run, or two runs of a progressive tracer would disagree and PtFurnaceTest's determinism pair would
// be comparing a moving target.
constexpr u32 kEnergyLutSamples = 4096;
// u0 the accumulator.
constexpr u32 kUavCount = 1;
// The shader's [numthreads(8,8,1)].
constexpr u32 kGroup = 8;

// RayQuery is SM 6.5, and DXR 1.1 is what makes it available from a compute shader with no state
// object and no shader table.
constexpr u32 kShaderModel = 65;
constexpr u32 kRayTracingTier = 11;

// RESOURCE BINDING TIER IS NOT CHECKED FOR THE INTEGRATOR ITSELF, AND NOW IS FOR ITS TEXTURES.
// This note used to end "until then"; that moment arrived, so here is what actually happened.
//
// The engine as a whole is explicitly not bindless -- RHIResources.hpp:3, a Resource Binding Tier 1
// commitment that protects the MINIMUM tier (D3D12 FL 11_0: Kepler, GCN 1.0, Haswell). This tracer
// is gated far above that, to DXR 1.1 hardware, every generation of which reports Binding Tier 3.
// So the tracer COULD use descriptor indexing without raising the engine's floor, because it is
// already gated to hardware that has it.
//
// THE UNTEXTURED INTEGRATOR STILL DOES NOT NEED IT, and still does not ask. It binds a fixed five
// SRVs and one UAV (kSrvCount/kUavCount above) -- TLAS, vertices, indices, instances and the energy
// table -- every one of them an explicitly bound slot, which Tier 1 satisfies comfortably. Requiring tier 3 of THAT would refuse the path tracer on hardware where it
// runs correctly, buying nothing. So the check lives on the textured twin only, which is why there
// are two pipelines here rather than one with a branch: the capability difference is a ROOT
// SIGNATURE difference, and a root signature is not something a dispatch can opt out of.
//
// The predicate is DeviceCaps::rtBindlessTextures (rayTracingTier >= 11 && resourceBindingTier >= 3),
// the same one VoxiRenderer's ray path uses -- not a bare resourceBindingTier read, so the two
// features cannot drift on what "can this device index a texture from a ray hit" means. It stays
// clampable for testing through CapsOverride (`tier1` in --force-caps), which is how the fallback
// path gets exercised on hardware that would otherwise never take it.
//
// The honest statement is now: this tracer requires DXR 1.1 and SM 6.5; it samples material textures
// additionally on hardware reporting binding tier 3, and falls back to flat per-surface albedo --
// exactly what it did before this existed -- everywhere else, including all of Vulkan, whose
// createBindlessTextureTable returns 0 by construction.

// The bindless base-colour table's fixed size. FIXED because this backend serialises root signature
// 1.0, so the HLSL array length and PipelineLayout::bindlessTextureCount must be one compile-time
// number and must agree exactly -- declaring more in the shader reads past the root signature's
// range. 4096 is Voxi's figure for the same job; a scene's DISTINCT materials, not its draws, is
// what fills it, and nothing in this tree approaches four thousand of those.
constexpr u32 kBindlessTexCapacity = 4096;

// Builds E(cos(theta), roughness): the fraction of incident light a SINGLE-SCATTER GGX lobe with
// F = 1 actually returns. Row-major, roughness outer, cos(theta) inner.
//
// THIS IS THE SHADER'S ptScatterSpecular WITH F = 1, DELIBERATELY DUPLICATED. Every line below has a
// twin in pt_pathtrace.hlsl -- the same GGX half-vector sample, the same k = a/2 Smith pairing, the
// same below-horizon rejection -- because the compensation is only exact if it divides by the energy
// THIS estimator loses rather than the energy some published fit's estimator loses. If either copy
// changes, the other must change with it; the furnace's white-conductor check is what notices if
// they drift, since compensating for the wrong lobe cannot read exactly L.
std::vector<f32> buildEnergyLut() {
    std::vector<f32> lut(static_cast<usize>(kEnergyLutDim) * kEnergyLutDim, 1.0f);
    for (u32 ri = 0; ri < kEnergyLutDim; ++ri) {
        // ENDPOINT-INCLUSIVE: cell 0 is exactly 0 and cell D-1 is exactly 1, NOT texel centres.
        //
        // MEASURED, not a matter of taste. With centres the top row sat at roughness 0.984, so a
        // query at exactly 1.0 -- which is where a fully rough material lands, and where the furnace
        // asks -- read an E integrated for a smoother surface. E falls with roughness, so the table
        // overstated it, the compensation was correspondingly too small, and the white-conductor
        // check came back 5.4% short (0.9465 against a required 1.000) while both interior points
        // passed. The corners of this table are real query points and have to be sampled exactly.
        const f32 rough = static_cast<f32>(ri) / static_cast<f32>(kEnergyLutDim - 1);
        const f32 a  = std::max(rough * rough, 1e-3f);
        const f32 k  = a * 0.5f;
        for (u32 mi = 0; mi < kEnergyLutDim; ++mi) {
            // Endpoint-inclusive as above; clamped off zero because a view exactly in the surface
            // plane has no reflection to integrate and would divide by zero below.
            const f32 ndv = std::max(static_cast<f32>(mi) / static_cast<f32>(kEnergyLutDim - 1),
                                     1e-3f);
            // The view vector in a tangent frame where the normal is +Z. Only its elevation matters:
            // GGX is isotropic here, so azimuth is a free choice and 0 is as good as any.
            const f32 vx = std::sqrt(std::max(0.0f, 1.0f - ndv * ndv));
            f64 sum = 0.0;
            for (u32 s = 0; s < kEnergyLutSamples; ++s) {
                // Hammersley: van der Corput radical inverse in base 2 for the second dimension.
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

                // l = reflect(-v, h) = 2(v.h)h - v
                // Only the elevation of the reflected direction and (v.h) enter the weight; the
                // azimuth cancels, so lx/ly are never formed.
                const f32 vdh = vx * hx + ndv * hz;
                const f32 lz  = 2.0f * vdh * hz - ndv;
                if (!(lz > 0.0f) || !(vdh > 0.0f) || !(hz > 0.0f)) continue;   // below the horizon
                (void)hy;

                const f32 g1v = ndv / (ndv * (1.0f - k) + k);
                const f32 g1l = lz  / (lz  * (1.0f - k) + k);
                // F = 1, so the weight is exactly G * (v.h) / (n.v * n.h) -- ptScatterSpecular's
                // expression with the Fresnel factor left out.
                sum += static_cast<f64>(g1v * g1l * vdh / std::max(ndv * hz, 1e-6f));
            }
            const f32 e = static_cast<f32>(sum / static_cast<f64>(kEnergyLutSamples));
            // CLAMPED AWAY FROM ZERO because the shader divides by this. E genuinely approaches 0 at
            // grazing incidence on a rough surface, and 1/E there would be an infinity multiplying
            // a term that should be small.
            lut[static_cast<usize>(ri) * kEnergyLutDim + mi] = std::clamp(e, 1e-2f, 1.0f);
        }
    }
    return lut;
}

// The integrator's pipeline layout. `bindlessTextures` non-zero appends the base-colour table in its
// own register space AND the sampler that reads it; zero reproduces, field for field, the layout the
// untextured integrator has always declared -- which is what keeps PtFurnaceTest's replay comparing
// the same compiled arithmetic against itself.
//
// THE SAMPLER IS PART OF THE TEXTURED HALF, not unconditional. The untextured pass declares no
// sampler at all today, and adding one to it would change its root signature for a resource it never
// reads.
rhi::PipelineLayout ptLayout(u32 bindlessTextures) {
    rhi::PipelineLayout l{};
    l.srvCount = kSrvCount;
    l.uavCount = kUavCount;
    // b4 stays a ROOT CBV (zero dwords): the block is 112 bytes, which is more than root constants
    // should carry, and setConstantBuffer suballocates it from the frame's upload ring.
    l.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;
    if (bindlessTextures) {
        // WRAP, because UV tiling is a material's own business and a clamped sampler would smear the
        // edge texel across every repeat. LINEAR rather than the material path's ANISOTROPIC: this
        // shader samples with SampleLevel at an explicit mip, and anisotropy is a function of the
        // screen-space derivatives a compute path-tracing kernel does not have -- asking for it would
        // cost the sampler slot's extra state to change nothing.
        l.samplers[0].filter  = rhi::Filter::Linear;
        l.samplers[0].address = rhi::AddressMode::Wrap;
        l.samplerCount = 1;
        l.bindlessTextureCount = bindlessTextures;
    }
    return l;
}

// MIRRORS cbuffer PtFrame in rhi::shaderFile("pt_pathtrace.hlsl").c_str(), field for field. A shifted field here reads a camera
// basis as a sample count -- silently, and only in the rendered image.
struct FrameCB {
    f32 origin[4];
    f32 forward[4];
    f32 right[4];
    f32 up[4];
    u32 image[4];
    u32 sample[4];
    f32 trace[4];
};

// Seven float4/uint4 rows, matching PtShaders.hpp's cbuffer exactly. This assert used to say
// "nothing checks this across the C++/HLSL boundary" -- which was true, and was the whole problem.
// It cannot catch a field REORDERED within the same size, but it does catch the common case: a row
// added on one side and not the other. pbr::MaterialConstants (MaterialGpu.hpp:38) has guarded
// itself this way all along; this struct and pcg::VolumeCB were simply the two that never did.
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
    // The engine's own declarations, which is where PI, skyColor and the averFurnace* contract come
    // from. Taking the environment from the shipped prelude rather than a private constant is what
    // makes the furnace a measurement of the engine and not of a test rig.
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry   = "CSPathTrace";
    sd.stage   = rhi::ShaderStage::Compute;
    sd.minShaderModel = kShaderModel;
    // The energy table's dimension travels as a define rather than a literal in the HLSL, so the
    // shader's index arithmetic and buildEnergyLut()'s layout cannot be edited apart.
    const std::string baseDefs = "AVER_PT_ENERGY_DIM=" + std::to_string(kEnergyLutDim);
    sd.defines = baseDefs.c_str();
    cs_ = res_->createShader(sd);
    if (!cs_) {
        // HLSL is compiled at RUNTIME by DXC, so this is the only place a shader error can surface.
        AVER_ERROR("[PT] the integrator would not compile");
        shutdown();
        return false;
    }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout = ptLayout(0);   // 0 == the layout this pass has always declared; see ptLayout
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[PT] integrator pipeline unavailable"); shutdown(); return false; }

    // BUILT BEFORE ANY TARGET, because createTarget binds it into every target's descriptor table
    // and a table slot left empty is a fault on the dispatch, not a warning.
    {
        const std::vector<f32> lut = buildEnergyLut();
        rhi::BufferDesc ed;
        ed.bytes = lut.size() * sizeof(f32);
        // UPLOAD, not Default, and written once here rather than copied on a command list: this is
        // 4 KB of read-only constants that never change after init, so a staging copy would buy
        // nothing and would drag a command context into a function that needs none. Same shape as
        // instanceBuf_ below.
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

// Builds the textured twin -- table, shader, pipeline -- on first demand. ONE ATTEMPT PER SESSION:
// texTried_ latches whether it succeeded or not, so a device that cannot do this is asked once and
// then left alone, rather than re-entering DXC on every material a scene streams in.
bool PathTracer::ensureTexturing() {
    if (texTried_) return texPipeline_ != 0;
    texTried_ = true;
    if (!res_ || !dev_ || !pipeline_) return false;

    // The capability the long note at the top of this file promised to check HERE, next to the
    // feature that needs it -- so a refusal names a real reason rather than being a blanket tier
    // requirement on a tracer that mostly does not need one.
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
    // AVER_PT_TEX_CAPACITY must equal the layout's bindlessTextureCount exactly -- both come from
    // kBindlessTexCapacity here so they cannot be edited apart.
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
        // REFUSED, NOT WRAPPED. Reusing slot 0 would silently paint one material with another's
        // texture; an unbound index falls back to the flat albedo, which is merely flat.
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
        if (verts_)       res_->destroyBuffer(verts_);
        if (indices_)     res_->destroyBuffer(indices_);
        if (instanceBuf_) res_->destroyBuffer(instanceBuf_);
        // BLAS HANDLES ARE NOT RELEASED HERE, and the history of this comment is the reason to say so
        // loudly. It once freed nothing (a per-toggle leak once a live editor could re-init this
        // object), then freed everything in blasCache_. Neither is right now: prepare() ADOPTS
        // structures other features built, via IResourceFactory::blasForMesh, so this map no longer
        // holds only things this object made. Freeing one another feature is still tracing is a
        // dangling handle inside a live TLAS build -- a device fault, not a quiet image bug.
        //
        // A BLAS belongs to its MESH. destroyMesh calls destroyBlasForMesh, which frees every
        // structure for that mesh, so nothing outlives the geometry it describes and VoxiRenderer has
        // always relied on exactly this. What is given up is releasing early when this view is
        // toggled off while its meshes stay loaded -- memory the scene is very likely still using.
        // THE TLASES STILL CANNOT BE RELEASED: the RHI has no destroyTlas at all (see
        // IResourceFactory -- BLAS has one, TLAS does not), so what this object allocated outlives
        // it, for the life of the DEVICE. What changed is the COUNT: addScene reuses a handle PER
        // SCENE SLOT and grows it by doubling, so a session leaks O(log n) per slot rather than one
        // per re-arm. Dropping the vectors here forgets the handles rather than freeing them, which
        // is all that can be done; removing the last of it needs a real destroyTlas added to the
        // RHI, a wider change than this module.
        tlasPool_.clear();
        tlasPoolCap_.clear();
        if (pipeline_)    res_->destroyPipeline(pipeline_);
        if (cs_)          res_->destroyShader(cs_);
        if (texPipeline_) res_->destroyPipeline(texPipeline_);
        if (texCs_)       res_->destroyShader(texCs_);
        // THE TABLE IS NOT DESTROYED, and the omission is the RHI's shape rather than a leak this
        // function is ignoring: destroyBindlessTextureTable exists, but the descriptors it hands back
        // are only safe to recycle once no in-flight frame can still reference them, and this class
        // has no fence to prove that. Voxi's table has the same lifetime for the same reason. What
        // matters here is that the INDEX MAP is cleared alongside the handle below, so a re-init
        // cannot hand out an index into a table it no longer owns.
    }
    texTable_ = 0;
    texIndex_.clear();
    texCount_ = 0;
    texTried_ = texWarned_ = false;
    texPipeline_ = 0;
    texCs_ = 0;
    blasCache_.clear();
    // CLEARED WITH THE BUFFERS IT DESCRIBES. Leaving it populated would let the next prepare() after
    // a re-init match its mesh set against a table whose verts_/indices_ were just destroyed, and
    // reuse two freed handles -- the exact hazard the geoMeshes_/verts_ pair has to be kept in step
    // to avoid, which is why resetScene() clears NEITHER and this clears BOTH.
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
        // REUSED PER SLOT, NOT RECREATED, AND NOT SHARED BETWEEN SCENES -- see tlasPool_'s
        // declaration for both halves of that. Reuse is what stops a re-arm leaking a handle; the
        // per-slot part is what stops five simultaneous scenes all tracing the same geometry.
        //
        // Grows to the next power of two so a snapshot that gains a single instance does not
        // allocate again; shrinks never, because a smaller TLAS buys nothing and every allocation
        // here is permanent until the device goes.
        const usize slot = scenes_.size();
        if (tlasPool_.size() <= slot) { tlasPool_.resize(slot + 1, 0); tlasPoolCap_.resize(slot + 1, 0); }

        const u32 need = count ? count : 1;
        if (!tlasPool_[slot] || tlasPoolCap_[slot] < need) {
            u32 cap = tlasPoolCap_[slot] ? tlasPoolCap_[slot] : 1;
            while (cap < need) cap <<= 1;
            const rhi::TlasHandle grown = res_->createTlas(cap);
            // A failed grow keeps the old handle rather than dropping to zero: too small is a
            // degraded scene, but zero is no scene at all, and prepare() would then build nothing
            // while reporting success.
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

    // Where each surface's geometry lands in the flat table. Per SURFACE and not per mesh: two
    // surfaces may share a mesh (the same quad with two albedos is exactly that case), and giving
    // them separate rows costs a few vertices and removes a deduplication that could go wrong.
    instances_.resize(surfaces_.size());
    surfaceRow_.assign(surfaces_.size(), 0);
    meshRows_.clear();
    totalVerts_ = totalIndices_ = 0;
    // Mesh handle -> row. Local to this call: the rows are rebuilt from scratch on every prepare(),
    // so this is a deduplication within one snapshot, never a cache across snapshots.
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
        // The SHARED rows: every surface on this mesh reads the same geometry. What stays per
        // surface is objectToWorld, albedo, ior and the base-colour index, right here.
        inst.firstVertex = row.firstVertex;
        inst.firstIndex  = row.firstIndex;
    }
    if (totalVerts_ == 0 || totalIndices_ == 0) return false;

    // THE SAME MESHES IN THE SAME ORDER means the table this call just laid out is byte-for-byte the
    // one verts_/indices_ already hold -- every firstVertex/firstIndex above was recomputed from the
    // same walk -- so both the allocation and buildScenes' copy pass can be skipped entirely. See
    // geoMeshes_ for why comparing handles is sufficient to know the geometry itself is unchanged.
    std::vector<rhi::MeshHandle> meshOrder;
    meshOrder.reserve(meshRows_.size());
    for (const MeshRow& row : meshRows_) meshOrder.push_back(row.mesh);
    // BOTH the mesh set AND the totals it implies. The set alone is what the argument above rests
    // on -- same meshes in the same order means the same offsets -- but the totals are what the SRV
    // is actually declared with (setSrvBuffer, in createTarget), so they are checked independently
    // rather than assumed to follow. A mismatch here would mean declaring more vertices than the
    // buffer holds, which is a GPU out-of-bounds read rather than a wrong picture; falling back to a
    // rebuild costs 4ms and cannot do that.
    geometryReused_ = verts_ && indices_ && meshOrder == geoMeshes_ &&
                      totalVerts_ == geoVerts_ && totalIndices_ == geoIndices_;

    if (!geometryReused_) {
        // A DIFFERENT mesh set: the offsets have moved, so whatever the old buffers hold is wrong.
        // Released here rather than in resetScene(), which now deliberately keeps them -- this is
        // the one place that knows whether they are still correct.
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

    // Upload rather than Default: it is written once from the CPU and never by the GPU, so there is
    // no copy to schedule and no state to walk.
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

    // One bottom-level structure per DISTINCT MESH. A BLAS describes geometry in object space and
    // nothing else -- the instance transform lives in the TLAS -- so two surfaces on one mesh have
    // always been able to share one, and building it twice was pure waste. See MeshRow for the
    // measurement that made that waste the dominant cost of the whole feature.
    const auto tBuf = std::chrono::steady_clock::now();
    blas_.assign(meshRows_.size(), 0);
    blasFresh_.assign(meshRows_.size(), 0);
    u32 reused = 0;
    for (usize i = 0; i < meshRows_.size(); ++i) {
        const rhi::MeshHandle mesh = meshRows_[i].mesh;
        if (const auto hit = blasCache_.find(mesh); hit != blasCache_.end()) {
            // Reached only for a mesh whose meshGeometry() succeeded above, which is the liveness
            // test the whole cache rests on -- see blasCache_ for why that is sufficient.
            blas_[i] = hit->second;
            ++reused;
            continue;
        }
        // ANOTHER FEATURE MAY ALREADY HAVE ONE, and before this asked, the answer being yes cost a
        // whole second structure. VoxiRenderer keeps its own MeshHandle -> BlasHandle map and builds
        // over the same static meshes this snapshot names, so in the standing configuration -- where
        // Voxi paints the scene and this view is the reference -- every mesh here was already built.
        // MEASURED on Sponza: 220 meshes, 154.3 ms of createBlas at load, all of it duplicating
        // structures over byte-identical geometry, plus double the resident BLAS memory.
        //
        // NOT MARKED FRESH, deliberately: blasForMesh only returns a structure that has been BUILT,
        // so there is nothing left to do for it. Marking it fresh would have this feature record a
        // second build over another feature's scratch buffer in the same frame, which is a race
        // rather than a redundancy.
        //
        // AND NOT OWNED: a shared structure belongs to the mesh, and destroyMesh frees it through
        // destroyBlasForMesh. See shutdown(), which no longer frees these.
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
    // Logged with its TIMING because this function was, measured, the entire cost of the feature:
    // 2.2 SECONDS per re-arm before geometry was deduplicated by mesh and structures were cached,
    // and a re-arm fires whenever the visible set changes. Anyone who makes this slow again should
    // find out from the log rather than from the frame rate.
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
    // ONLY THE NEW ONES. A built BLAS stays built until it is destroyed, and re-tracing the same
    // geometry on every re-arm is GPU work whose result is bit-identical to what is already there.
    for (usize i = 0; i < blas_.size(); ++i) if (blasFresh_[i]) ctx.buildBlas(blas_[i]);

    // BOTH transitions are explicit. D3D12 would promote a Common buffer to CopyDest by itself, but
    // the RHI tracks buffer state to catch exactly this class of mistake and does not model
    // promotion, so an implicit promotion followed by an explicit walk-back is a barrier claiming a
    // state the tracker never saw it enter.
    // SKIPPED ENTIRELY when prepare() kept the previous table: the buffers already hold this exact
    // geometry at these exact offsets, so the copy would write identical bytes over themselves. The
    // BARRIERS are skipped with it, deliberately -- transitioning a buffer nothing is about to write
    // would be a barrier claiming a state the RHI's tracker never saw it enter, which is the same
    // mistake the explicit-transition comment below exists to prevent.
    if (!geometryReused_) {
        // BOTH transitions are explicit. D3D12 would promote a Common buffer to CopyDest by itself,
        // but the RHI tracks buffer state to catch exactly this class of mistake and does not model
        // promotion, so an implicit promotion followed by an explicit walk-back is a barrier
        // claiming a state the tracker never saw it enter.
        ctx.bufferBarrier(verts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
        ctx.bufferBarrier(indices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
        // Per DISTINCT MESH, matching the table prepare() laid out. Copying per surface duplicated
        // the same blob once per instance -- 31.2M vertices where 2M would do.
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
            // The id a hit reads back. It indexes the SHARED instance table, so a scene holding a
            // subset of the surfaces still resolves each hit to the right geometry and albedo --
            // which CommittedInstanceIndex, a position inside this one structure, could not do.
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
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    out.set = res_->createBindingSet(bd);

    if (!out.accum || !out.set) {
        AVER_ERROR("[PT] could not allocate an accumulation target");
        destroyTarget(out);
        return false;
    }

    // ONE BINDING SET PER TARGET, and never one shared set rebound between dispatches: a descriptor
    // table is read when the command EXECUTES, so rewriting a shared set between two recorded
    // dispatches would give both of them the last scene written.
    res_->setSrvTlas(out.set, 0, scenes_[scene].tlas);
    res_->setSrvBuffer(out.set, 1, verts_, sizeof(rhi::MeshVertex), totalVerts_, 0);
    res_->setSrvBuffer(out.set, 2, indices_, sizeof(u32), totalIndices_, 0);
    res_->setSrvBuffer(out.set, 3, instanceBuf_, sizeof(Instance),
                       static_cast<u32>(instances_.size()), 0);
    res_->setSrvBuffer(out.set, 4, energyLut_, sizeof(f32), kEnergyLutDim * kEnergyLutDim, 0);
    res_->setUavBuffer(out.set, 0, out.accum, kPtAccumStride,
                       width * height * kPtAccumElementsPerPixel, 0);

    out.width = width;
    out.height = height;
    out.scene = scene;
    return true;
}

void PathTracer::resetScene() {
    if (res_) {
        // ONLY the per-surface buffer. verts_/indices_ are NOT released here any more: they belong
        // to the geometry table keyed by geoMeshes_, which outlives a snapshot for the same reason
        // blasCache_ does (see both of their comments). prepare() is the one place that can tell
        // whether they are still correct, and it releases them itself when the mesh set has moved;
        // shutdown() frees whatever is left. instanceBuf_ genuinely IS per snapshot -- it is one
        // record per SURFACE, and the surface set is exactly what changes.
        if (instanceBuf_) res_->destroyBuffer(instanceBuf_);
        // BLAS handles are NOT released here either, for the same reason.
        // THE TLAS POOL IS KEPT, DELIBERATELY, and that is the point of the change: clearing
        // scenes_ below drops the Scenes that referenced them, but tlasPool_ itself survives, so the
        // next round of addScene() calls reuses slot 0, slot 1, ... in the same order instead of
        // allocating structures that can never be freed. See tlasPool_'s own declaration for what
        // that used to cost, and for why the pool is per SLOT rather than a single shared handle.
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
    // gPtSample.w was the one unused slot in the block, so this needs no new row and the
    // sizeof(FrameCB) assert below is untouched. 0 means "never roulette", which is what every
    // caller that does not set it gets.
    cb.sample[3] = d.rouletteDepth;
    cb.trace[0] = d.rayBias;
    cb.trace[1] = d.tMax;
    // R5/F6: gPtTrace.z, decoded in pt_pathtrace.hlsl's miss branch. 1.0 keeps the pre-fix
    // unmatched-reference-sky behaviour; the default (d.legacyEnvironment false) writes 0.0, which
    // is also what an all-zero FrameCB (any caller from before this field existed) already read as,
    // so PtFurnaceTest -- which never sets it -- is unaffected by construction.
    cb.trace[2] = d.legacyEnvironment ? 1.0f : 0.0f;

    // THE ONLY TIMED SPAN THIS MODULE HAS EVER HAD, and its absence was not cosmetic. Until this
    // line the sole ScopedGpuStat anywhere in render.pt was on PtSceneView's present BLIT, so
    // --gpu-timing could report what it cost to SHOW the image and nothing at all about tracing it.
    // Every "the path tracer is Nx faster than raster" figure in this repository was therefore a
    // whole-frame CPU number from --frame-time (which SandboxApp's own comment at :4854 describes as
    // "WHOLE frames from the CPU"), set beside a GPU pass span from the renderer it was said to beat.
    // That is a category error, and it survived precisely because nothing here was measurable enough
    // to contradict it: a dispatch with no marker is a dispatch nobody can price.
    rhi::ScopedGpuStat gpuStat(ctx, "PT accumulate");
    ctx.pushMarker("Aver.PathTracer");
    ctx.bufferBarrier(t.accum, rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);
    // THE TEXTURED TWIN ONLY ONCE SOMETHING IS ACTUALLY RESIDENT. texturing() is false for every
    // caller that never asked for a texture -- PtFurnaceTest above all -- so those dispatches run the
    // original pipeline, the original root signature and the original compiled arithmetic, which is
    // what its bit-identical replay check is entitled to assume.
    const bool textured = texturing();
    ctx.setPipeline(textured ? texPipeline_ : pipeline_);
    ctx.setBindingSet(t.set);
    // AFTER setPipeline and setBindingSet: the table is a root parameter of the pipeline just bound,
    // and binding it against the previous pipeline's root signature is a silent mismatch.
    if (textured) ctx.setBindlessTable(texTable_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb, sizeof(cb));
    ctx.dispatch((t.width + kGroup - 1) / kGroup, (t.height + kGroup - 1) / kGroup, 1);
    // Back to Common before the frame ends: a buffer's state does not survive the command list.
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
