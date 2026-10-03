#pragma once
// A brute-force path tracer: cosine-weighted hemisphere sampling, N bounces, progressive
// accumulation into a linear float buffer. Every surface is Lambertian EXCEPT one flagged dielectric
// (PtSurface::ior > 0), which gets a smooth Fresnel-weighted reflect/refract BSDF instead -- see
// ptScatterDielectric in PtShaders.hpp and PtFurnaceTest.cpp's dielectric energy-conservation checks.
//
// It is a COMPUTE pass over an RWStructuredBuffer<float4> rather than a texture, for two reasons
// that are both about being checkable. Format has no RGBA32F, and a structured buffer needs no
// typed-UAV-load support -- so the accumulator is full-precision linear radiance on every device
// this RHI runs on. And a buffer can be copied to a Readback heap and read as FLOATS: the furnace
// oracle this ships with is an ABSOLUTE claim ("albedo 1 in an environment of radiance L reads
// exactly L"), and a missing 1/PI is a GLOBAL SCALE, so a tonemapped 8-bit pixel cannot see one and
// no ratio or relational test can either.
//
// It links the GENERIC RHI and never a backend, and it owns its own geometry: a caller hands it
// meshes and world matrices and gets back scenes it can trace, without a World, a draw list or a
// renderer being involved. That is what lets the self-test beside it construct exactly the geometry
// its arithmetic needs.
#include "aver/rhi/RHI.hpp"

#include <unordered_map>
#include <vector>

namespace aver::pt {

// Elements of the accumulator per pixel: radiance, then path statistics. See kPathTracerHLSL.
constexpr u32 kPtAccumElementsPerPixel = 2;
// Bytes per accumulator element (one float4).
constexpr u32 kPtAccumStride = 16;

// A deliberate arithmetic fault in the estimator, shipped so the furnace can be shown FAILING.
// See the PT_DEFECT_* block in the shader for what each one is and what it must read.
enum class PtDefect : u32 { None = 0, TimesPi = 1, NoCosine = 2, DielectricNoPdfCancel = 3 };

// One surface the tracer can hit: a mesh, where it is, and what it reflects.
// "This slot holds no texture." NOT ZERO -- zero is a real, reachable bindless index. Same value
// and reasoning as pbr::kUnboundTexture, restated to avoid that dependency.
inline constexpr u32 kUnboundTexture = 0xFFFFFFFFu;

struct PtSurface {
    rhi::MeshHandle mesh = 0;
    // ENGINE convention: row-major / row-vector, cm, +Z up. Handed to TlasInstance untouched.
    f32 world[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    // THE FACTOR, NOT THE FINISHED COLOUR, when baseColorTex is bound: texel multiplies this.
    // If binding a texture, must pass baseColorFactor here, NOT the texture's mean.
    f32 albedo[3] = {1, 1, 1};
    // Index into the tracer's bindless base-colour table, or kUnboundTexture.
    // MUST come from PathTracer::residentTexture() to avoid re-arming the accumulator every frame.
    u32 baseColorTex = kUnboundTexture;

    // NEGATIVE MEANS "NO SPECULAR LOBE AT ALL" -- the pure Lambertian default. Kept as default
    // for backward compatibility with existing furnace measurements. Roughness is a [0,1] quantity;
    // -1 is the absence of the question.
    f32 roughness = -1.0f;
    // Only read when roughness >= 0. 0 = dielectric (F0 0.04), 1 = conductor (F0 = albedo).
    f32 metallic = 0.0f;

    // glTF packing: occlusion in R, roughness in G, metallic in B. When bound, `roughness` and
    // `metallic` are FACTORS multiplying the sampled channels.
    u32 metalRoughTex = kUnboundTexture;
    // Tangent-space normal map. Tangent frame derived per triangle from UV gradient.
    u32 normalTex = kUnboundTexture;
    f32 normalScale = 1.0f;
    // 0.0 (default) = opaque Lambertian. > 0 = smooth dielectric with that index of refraction.
    // NO SEPARATE "kind" FIELD: no real dielectric has IOR of exactly 0, so the field's absence
    // signals "not a dielectric" without needing a flag. See PathTracer::Instance.
    f32 ior = 0.0f;

    // Linear radiance this surface emits (a lamp bulb's emissiveFactor); {0,0,0} = no glow.
    f32 emissive[3] = {0, 0, 0};
};

// The pinhole camera primary rays are generated from. Carried in the pass's own constants rather
// than read from the engine's gViewProj, so a test scene is not also a statement about the editor.
struct PtCamera {
    f32 origin[3]  = {0, 0, 0};
    f32 forward[3] = {0, 0, -1};
    f32 right[3]   = {1, 0, 0};
    f32 up[3]      = {0, 1, 0};
    f32 tanHalfFov = 0.1f;   // tangent of the half VERTICAL field of view
    f32 aspect     = 1.0f;   // width / height
};

// What one accumulation dispatch does.
struct PtDispatch {
    u32      maxBounces = 4;
    PtDefect defect     = PtDefect::None;
    u32      firstSample = 0;    // the sample index this dispatch starts at; the seed reads it
    u32      samples     = 16;   // samples per pixel in this dispatch
    bool     reset       = false;// overwrite rather than add; the first dispatch must set it
    f32      rayBias     = 0.05f;// cm, along the normal and as TMin
    f32      tMax        = 1.0e7f;
    // RUSSIAN ROULETTE: bounce index at which paths are terminated in proportion to remaining
    // energy. 0 is OFF and is the DEFAULT. See shader for technique; off by default because
    // the furnace test re-arms every frame and cannot reach sample 1 if it re-arms.
    u32      rouletteDepth = 0;

    // Every INDIRECT miss reads averSkyRadianceCheap(dir) * gAmbient.r, the SH-sourced sky
    // ReSTIR uses for its own indirect miss, instead of raw uncalibrated skyColor(). Only CAMERA
    // rays are unaffected. legacyEnvironment = true restores the old, unmatched behaviour.
    bool     legacyEnvironment = false;
};

// Where one camera's accumulation lands: the buffer, and the descriptors naming its scene.
struct PtTarget {
    rhi::BufferHandle     accum = 0;
    rhi::BindingSetHandle set   = 0;
    u32 width = 0, height = 0;
    u32 scene = 0;
    bool valid() const { return accum != 0 && set != 0; }
    u32 pixels() const { return width * height; }
    u64 bytes() const {
        return static_cast<u64>(width) * height * kPtAccumElementsPerPixel * kPtAccumStride;
    }
};

// The pass: one pipeline, one flat geometry table, and any number of scenes over it.
class PathTracer {
public:
    ~PathTracer();

    // Compiles the integrator and allocates nothing else. False means this device cannot run it --
    // which is not a failure and must be reported as "unavailable", not as a wrong answer.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool available() const { return pipeline_ != 0; }

    // Makes `h` resident in the tracer's own bindless base-colour table and returns its index, or
    // kUnboundTexture if this device has no bindless support, the table could not be created, or it
    // is full. Idempotent: the same handle always returns the same index for the life of the tracer.
    // APPEND-ONLY AND NEVER FREED: the index travels into PtSceneView::drawsKey(); if an index could
    // be reused, a scene whose visible set merely changed would hash differently and re-arm every frame.
    u32 residentTexture(rhi::TextureHandle h);

    // The tracer samples textures only once something has actually been made resident. Until then it
    // dispatches the ORIGINAL, texture-free pipeline -- which keeps PtFurnaceTest running the identical
    // compiled arithmetic and its bit-identical replay check meaningful.
    bool texturing() const { return texPipeline_ != 0 && texCount_ != 0; }

    // Registers a surface and returns the id a hit reads back as CommittedInstanceID().
    // Surfaces are declared BEFORE any scene, because the flat geometry table is built over all of
    // them at once and a scene only names which ids it contains.
    u32 addSurface(const PtSurface& s);

    // Declares a scene over a subset of the registered surfaces. Returns its index.
    // SEPARATE ACCELERATION STRUCTURES rather than one scene with everything far apart, because
    // a single-bounce configuration must be single-bounce because nothing else is reachable.
    u32 addScene(const u32* surfaceIds, u32 count);

    // Allocates the flat geometry table and the acceleration structures, once every surface and
    // scene has been declared. NEEDS NO COMMAND LIST, which lets targets be created and their
    // descriptors written before the frame that records the builds.
    bool prepare();

    // Records the bottom- and top-level builds and the flat geometry copies. Once, in a frame with
    // an open command list, after prepare().
    bool buildScenes(rhi::IRenderContext& ctx);
    bool scenesBuilt() const { return built_; }

    bool createTarget(u32 scene, u32 width, u32 height, PtTarget& out);
    void destroyTarget(PtTarget& t);

    // Clears every SURFACE, SCENE and the flat geometry table -- everything shutdown() clears, MINUS
    // the compiled integrator (cs_/pipeline_), which survives. addSurface()/addScene()/prepare()/
    // buildScenes() can all be called again afterwards, exactly as if this were a freshly-init()ed
    // PathTracer, but with no DXC recompile.
    //
    // WHY THIS EXISTS: prepare()/buildScenes() are a ONE-SHOT contract -- right for PtFurnaceTest
    // but wrong for a caller streaming a scene from a running level (PtSceneView).
    //
    // ANY PtTarget FROM BEFORE THIS CALL IS NOW INVALID and must be destroyTarget()ed: its binding
    // set was written against the SCENE INDEX this call just discarded, and a scene rebuilt after
    // this returns fresh TlasHandle values that may reuse the same scene index with a DIFFERENT
    // underlying acceleration structure.
    //
    // WHAT IT KEEPS: the cached BLAS handles (blasCache_) and the flat GEOMETRY TABLE
    // (verts_/indices_, recorded by geoMeshes_) deliberately survive this call. Both make a re-arm
    // cheap. prepare() decides whether the table is still correct for the new snapshot.
    //
    // A KNOWN, ACCEPTED COST: the RHI has no destroyTlas, so every TLAS this call discards is NOT
    // released; it leaks for the life of the device. This is fine for the intended caller -- a
    // reference view that re-arms on a genuine static scene change -- and would NOT be fine for a
    // caller that rebuilds every frame.
    void resetScene();

    // One accumulation step. The target's buffer is left in Common, because a buffer's state does
    // not survive the command list.
    void accumulate(rhi::IRenderContext& ctx, const PtTarget& t, const PtCamera& cam,
                    const PtDispatch& d);

    // Copies a target's accumulator into a Readback buffer at `dstOffset`. The caller is
    // responsible for waitIdle() before reading it: readBuffer synchronises nothing by contract.
    void copyForReadback(rhi::IRenderContext& ctx, const PtTarget& t, rhi::BufferHandle readback,
                         u64 dstOffset);

private:
    // MIRRORS the HLSL PtInstance. 64 + 4 + 4 + 12 + 4; a structured buffer packs tightly with
    // natural alignment, so this is 88 bytes on both sides -- and the stride handed to
    // setSrvBuffer must agree with it or every instance after the first reads its neighbour.
    // THE LAST FOUR BYTES USED TO BE A SPARE `u32 pad`. Reinterpreted as a float IOR instead to
    // avoid adding a new field. See PtSurface::ior for why 0.0 doubling as "not a dielectric"
    // needs no separate kind flag.
    struct Instance {
        f32 objectToWorld[16];
        u32 firstIndex = 0;
        u32 firstVertex = 0;
        f32 albedo[3] = {1, 1, 1};
        f32 ior = 0.0f;
        // APPENDED, never inserted: any earlier position shifts albedo/ior and every existing
        // instance silently reads the wrong fields. Hand-mirrored into HLSL with nothing but a
        // static_assert on its size watching.
        u32 baseColorTex = kUnboundTexture;
        f32 roughness = -1.0f;   // negative = pure Lambertian; see PtSurface::roughness
        f32 metallic = 0.0f;
        u32 metalRoughTex = kUnboundTexture;
        u32 normalTex = kUnboundTexture;
        f32 normalScale = 1.0f;
        // PtSurface::emissive, appended like normalScale; read by both shader variants.
        f32 emissive[3] = {0, 0, 0};
    };
    static_assert(sizeof(Instance) == 124, "PtInstance is the HLSL PtInstance ABI");

    struct Scene {
        rhi::TlasHandle  tlas = 0;
        std::vector<u32> surfaces;
    };

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    // THE SINGLE-SCATTER DIRECTIONAL ALBEDO TABLE, E(cos(theta), roughness), built once on the CPU
    // at init and never touched again. It is what the multiple-scattering compensation divides by:
    // a microfacet lobe with Smith shadowing returns only E of the light it receives, the rest being
    // inter-facet scattering the single-scatter model has no term for. Integrated with the shader's
    // own estimator rather than taken from a published analytic fit.
    rhi::BufferHandle energyLut_ = 0;

    rhi::ShaderHandle   cs_ = 0;
    rhi::PipelineHandle pipeline_ = 0;

    // THE TEXTURED TWIN, a second PSO rather than a runtime branch inside the first. The bindless
    // range is part of the ROOT SIGNATURE, not a uniform, so it cannot be toggled per dispatch.
    // Built lazily, on the first successful residentTexture(). Returns whether texturing is usable;
    // one attempt per session, latched by texTried_.
    bool ensureTexturing();

    rhi::ShaderHandle   texCs_ = 0;
    rhi::PipelineHandle texPipeline_ = 0;
    rhi::BindlessTableHandle texTable_ = 0;
    // Handle -> slot, append-only. See residentTexture() for why it is never cleared.
    std::unordered_map<rhi::TextureHandle, u32> texIndex_;
    u32  texCount_ = 0;
    bool texTried_ = false;   // one attempt per session; a failure is remembered, not retried
    bool texWarned_ = false;

    std::vector<PtSurface> surfaces_;
    std::vector<Instance>  instances_;
    std::vector<Scene>     scenes_;
    // ONE TLAS, REUSED ACROSS RE-ARMS, GROWN ONLY WHEN A SNAPSHOT NEEDS MORE ROOM THAN EVERY
    // PREVIOUS ONE. Growing rather than fixing at a cap keeps this module free of PtSceneView's
    // kMaxInstances. Rounding up to a power of two stops a snapshot that grows by one instance
    // from allocating again. A POOL, INDEXED BY SCENE ORDINAL -- NOT ONE SHARED HANDLE.
    std::vector<rhi::TlasHandle> tlasPool_;
    std::vector<u32>             tlasPoolCap_;
    // GEOMETRY IS PER MESH, NOT PER SURFACE: one createBlas and vertex/index copy per distinct
    // mesh, not per surface. This matters for performance: a snapshot drawn from a handful of
    // distinct meshes can deduplicate within the snapshot and survive resetScene().
    struct MeshRow {
        rhi::MeshHandle mesh = 0;
        u32 firstVertex = 0, firstIndex = 0, vertexCount = 0, indexCount = 0;
        bool copiesVertices = true;   // false: another row already copied this shared vertex buffer
    };
    std::vector<MeshRow> meshRows_;          // one per DISTINCT mesh in the snapshot
    std::vector<u32>     surfaceRow_;        // surface -> index into meshRows_/blas_
    std::vector<rhi::BlasHandle> blas_;      // one per DISTINCT mesh, parallel to meshRows_
    std::vector<u8>              blasFresh_; // parallel: 1 for one built this snapshot

    // BLAS BY MESH, SURVIVING resetScene(): caches per-mesh BLAS handles across re-arms.
    // A BLAS describes one mesh and stays valid until that mesh is destroyed. The RHI already tears
    // one down at exactly that moment -- destroyMesh calls destroyBlasForMesh -- so the liveness
    // test is free and the cache cannot outlive what it describes.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blasCache_;

    // THE GEOMETRY TABLE BY MESH SET, SURVIVING resetScene(): caches the flat geometry table
    // across re-arms when the distinct mesh set has not changed. What actually changes between
    // snapshots is which INSTANCES are visible. When mesh order is identical, verts_/indices_
    // already hold exactly the right bytes, so both the allocation and the copy are pure waste.
    // Comparing handles is sufficient because a mesh handle is never recycled.
    std::vector<rhi::MeshHandle> geoMeshes_;
    // What the LIVE buffers were actually sized for. Checked alongside the mesh set on reuse.
    u32 geoVerts_ = 0, geoIndices_ = 0;
    // Set by prepare() when it reused the table above, read by buildScenes() to skip the copy pass
    // AND its two barriers.
    bool geometryReused_ = false;
    u64 tlasBlasGeneration_ = 0;   // res_->blasGeneration() the TLASes were built against
    bool buildTlases(rhi::IRenderContext& ctx);

    rhi::BufferHandle verts_ = 0, indices_ = 0, instanceBuf_ = 0;
    u32  totalVerts_ = 0, totalIndices_ = 0;
    bool prepared_ = false;
    bool built_ = false;
};

} // namespace aver::pt
