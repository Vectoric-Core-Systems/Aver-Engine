#pragma once
// A brute-force path tracer: cosine-weighted hemisphere sampling, N bounces, progressive
// accumulation into a linear float buffer.
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
enum class PtDefect : u32 { None = 0, TimesPi = 1, NoCosine = 2 };

// One surface the tracer can hit: a mesh, where it is, and what it reflects.
struct PtSurface {
    rhi::MeshHandle mesh = 0;
    // ENGINE convention: row-major / row-vector, cm, +Z up. Handed to TlasInstance untouched.
    f32 world[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    f32 albedo[3] = {1, 1, 1};
};

// The pinhole camera primary rays are generated from. Carried in the pass's own constants rather
// than read from the engine's gViewProj, so a scene under test is not also a statement about
// whatever the editor's camera happens to be doing.
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

    // Registers a surface and returns the id a hit reads back as CommittedInstanceID().
    // Surfaces are declared BEFORE any scene, because the flat geometry table is built over all of
    // them at once and a scene only names which ids it contains.
    u32 addSurface(const PtSurface& s);

    // Declares a scene over a subset of the registered surfaces. Returns its index.
    //
    // SEPARATE ACCELERATION STRUCTURES rather than one scene with everything far apart, because
    // "far apart" is a probability argument and this module exists to avoid those: a single-bounce
    // configuration must be single-bounce because nothing else is reachable, not because the odds
    // of reaching it are small.
    u32 addScene(const u32* surfaceIds, u32 count);

    // Allocates the flat geometry table and the acceleration structures, once every surface and
    // scene has been declared. NEEDS NO COMMAND LIST, which is what lets targets be created --
    // and their descriptors written -- before the frame that records the builds.
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
    // WHY THIS EXISTS: prepare()/buildScenes() are a ONE-SHOT contract -- exactly right for
    // PtFurnaceTest, which declares its geometry once and never again, and wrong for a caller that
    // streams a scene from a running level (PtSceneView). resetScene() is the seam that turns
    // "declare a scene once" into "re-arm on a new snapshot".
    //
    // ANY PtTarget FROM BEFORE THIS CALL IS NOW INVALID and must be destroyTarget()ed: its binding
    // set was written against the SCENE INDEX this call just discarded (createTarget() calls
    // setSrvTlas against scenes_[scene].tlas at creation time), and a scene rebuilt after this returns
    // fresh TlasHandle values that may reuse the same small integer scene index with a DIFFERENT
    // underlying acceleration structure.
    //
    // WHAT IT KEEPS: the cached BLAS handles (blasCache_) and the flat GEOMETRY TABLE
    // (verts_/indices_, recorded by geoMeshes_) both deliberately survive this call. Between them
    // they are what makes a re-arm cheap -- see their own comments for the measurements. The next
    // prepare() decides whether the table is still correct for the new snapshot and rebuilds it
    // there if not, because this call cannot know: it has not been told the new surfaces yet.
    //
    // A KNOWN, ACCEPTED COST: the RHI has no destroyTlas (see IResourceFactory -- BLAS has one,
    // TLAS does not), so every TLAS this call discards is NOT released; it leaks for the life of the
    // device. BLAS handles ARE released here, since destroyBlas exists. This is fine for the intended
    // caller -- a reference view that re-arms on a genuine STATIC scene change, which for a level that
    // has finished streaming is rare to never -- and would NOT be fine for a caller that rebuilds every
    // frame; nothing in this module enforces that distinction, so a future caller doing the latter
    // would need a real destroyTlas added to the RHI first.
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
    struct Instance {
        f32 objectToWorld[16];
        u32 firstIndex = 0;
        u32 firstVertex = 0;
        f32 albedo[3] = {1, 1, 1};
        u32 pad = 0;
    };
    static_assert(sizeof(Instance) == 88, "PtInstance is the HLSL PtInstance ABI");

    struct Scene {
        rhi::TlasHandle  tlas = 0;
        std::vector<u32> surfaces;
    };

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    rhi::ShaderHandle   cs_ = 0;
    rhi::PipelineHandle pipeline_ = 0;

    std::vector<PtSurface> surfaces_;
    std::vector<Instance>  instances_;
    std::vector<Scene>     scenes_;
    // ONE TLAS, REUSED ACROSS RE-ARMS, GROWN ONLY WHEN A SNAPSHOT NEEDS MORE ROOM THAN EVERY
    // PREVIOUS ONE. Voxi's own pattern (VoxiRenderer.cpp: createTlas(kMaxDraws) once at init, then
    // buildTlas into it every frame), arrived at here for a sharper reason than tidiness.
    //
    // addScene used to call createTlas(count) on EVERY re-arm, sized to that snapshot exactly. The
    // RHI has no destroyTlas -- BLAS has one, TLAS does not -- so each of those leaked for the life
    // of the DEVICE. resetScene()'s own comment called that known and accepted, on the stated
    // grounds that this is "a reference view that re-arms on a genuine STATIC scene change, which
    // for a level that has finished streaming is rare to never".
    //
    // THAT PREMISE IS FALSE AND WAS MEASURED FALSE: the view re-arms three times in the first three
    // frames of a completely static scene with a fixed camera, and drawsKey() re-arms it again on
    // any dynamic-draw change. "Rare to never" was describing an intent, not the behaviour.
    //
    // Growing rather than fixing at a cap keeps this module free of PtSceneView's kMaxInstances --
    // a library should not inherit its caller's limit -- and rounding up to a power of two stops a
    // snapshot that grows by one instance from allocating again. A grow still leaks the old handle,
    // because it still cannot be destroyed; what changes is that this now happens O(log n) times in
    // a session instead of once per re-arm.
    rhi::TlasHandle        tlas_ = 0;
    u32                    tlasCapacity_ = 0;
    // GEOMETRY IS PER MESH, NOT PER SURFACE, and this is the difference between a re-arm costing
    // 20ms and costing two and a half SECONDS.
    //
    // It used to be per surface -- one createBlas and one full vertex/index copy each -- on the
    // stated grounds that a duplicate build "is a few microseconds and removes a cache whose
    // invalidation rule would otherwise have to be right". Measured on a real level that scatters
    // its foliage procedurally, one snapshot was 2174 surfaces drawn from a handful of distinct
    // meshes: 2174 createBlas calls, each allocating its own acceleration-structure AND scratch
    // buffer, and 31.2 MILLION vertices copied into the flat table for perhaps a fiftieth of that
    // much distinct geometry. prepare() measured 2189ms, 2251ms, 2291ms on consecutive re-arms,
    // and a re-arm fires whenever the visible set changes -- about every five frames while flying.
    //
    // There is no invalidation rule to get right, which is what the original reasoning missed: the
    // map lives for exactly one prepare() call and is thrown away with it. Nothing outlives the
    // snapshot, so nothing can go stale.
    struct MeshRow {
        rhi::MeshHandle mesh = 0;
        u32 firstVertex = 0, firstIndex = 0, vertexCount = 0, indexCount = 0;
    };
    std::vector<MeshRow> meshRows_;          // one per DISTINCT mesh in the snapshot
    std::vector<u32>     surfaceRow_;        // surface -> index into meshRows_/blas_
    std::vector<rhi::BlasHandle> blas_;      // one per DISTINCT mesh, parallel to meshRows_
    std::vector<u8>              blasFresh_; // parallel: 1 for one built this snapshot

    // BLAS BY MESH, SURVIVING resetScene(), and this is the second half of the same measurement.
    // Deduplicating within a snapshot took a re-arm from ~2200ms to ~110ms, and the split showed
    // the remainder was still almost all createBlas: 94 calls, 103-164ms, for the same 94 meshes
    // every time. What churns between snapshots is which INSTANCES are visible; the set of distinct
    // meshes barely moves, so rebuilding their structures per re-arm is the same waste one level up.
    //
    // THE INVALIDATION RULE, which is the thing worth being exact about: a BLAS describes one mesh
    // and stays valid until that mesh is destroyed. The RHI already tears one down at exactly that
    // moment -- destroyMesh calls destroyBlasForMesh -- so the ONLY way a cached handle can dangle
    // is a mesh that died, and IDevice::meshGeometry answers false for exactly those. prepare()
    // already calls it for every mesh it touches, so the liveness test costs nothing extra and the
    // cache cannot outlive what it describes. Mesh handles are never recycled either (destroyMesh
    // clears the slot and keeps it), so a handle can never come to mean a different mesh.
    std::unordered_map<rhi::MeshHandle, rhi::BlasHandle> blasCache_;

    // THE GEOMETRY TABLE BY MESH SET, SURVIVING resetScene(), and this is the third instalment of
    // the same measurement the BLAS cache above records. With structures cached, what remained of a
    // re-arm was the flat table itself: ~158 MB of vertex and index buffer reallocated, and then
    // every distinct mesh's geometry copied into it on the GPU -- for a mesh set that, measured
    // across a streaming ElectricDreams capture, went 93 -> 94 -> 94 while the SURFACE count went
    // 2154 -> 2168 -> 2174. The third re-arm rebuilt the entire table to express six new instances
    // of meshes it already had.
    //
    // What actually changes between snapshots is which INSTANCES are visible. The distinct meshes,
    // their row order and therefore every firstVertex/firstIndex are usually identical -- and when
    // they are, verts_/indices_ already hold exactly the right bytes, so both the allocation and the
    // copy are pure waste. This records the mesh order the LIVE buffers were built for; prepare()
    // compares the freshly computed order against it and reuses on a match.
    //
    // WHY COMPARING HANDLES IS SUFFICIENT, and it is the same argument blasCache_ makes: a mesh
    // handle is never recycled, and meshGeometry() -- which prepare() already calls for every mesh
    // before it gets here -- answers false for a dead one. So an unchanged handle means unchanged
    // geometry, and the ORDER is compared too, because the row offsets depend on it.
    std::vector<rhi::MeshHandle> geoMeshes_;
    // What the LIVE buffers were actually sized for. Checked alongside the mesh set on reuse, and
    // kept as its own fact rather than re-derived: these two are what the SRV is declared with.
    u32 geoVerts_ = 0, geoIndices_ = 0;
    // Set by prepare() when it reused the table above, read by buildScenes() to skip the copy pass
    // AND its two barriers -- transitioning a buffer nothing is about to write would be a barrier
    // claiming a state the RHI's own tracker never saw it enter (see buildScenes' own comment).
    bool geometryReused_ = false;

    rhi::BufferHandle verts_ = 0, indices_ = 0, instanceBuf_ = 0;
    u32  totalVerts_ = 0, totalIndices_ = 0;
    bool prepared_ = false;
    bool built_ = false;
};

} // namespace aver::pt
