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
    std::vector<rhi::BlasHandle> blas_;      // one per surface; two surfaces may share a mesh

    rhi::BufferHandle verts_ = 0, indices_ = 0, instanceBuf_ = 0;
    u32  totalVerts_ = 0, totalIndices_ = 0;
    bool prepared_ = false;
    bool built_ = false;
};

} // namespace aver::pt
