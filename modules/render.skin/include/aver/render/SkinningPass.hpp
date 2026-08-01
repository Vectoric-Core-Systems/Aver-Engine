#pragma once
// GPU linear-blend skinning: one compute dispatch turns a mesh's rest vertices into posed ones.
//
// The parity partner of anim::skinVertices, whose comment states the contract both sides implement.
// This module links the GENERIC RHI and never a backend, so the pass is expressible against any
// device -- the same property that made Aver.Render.UI testable.
//
// RENDERING.md 7.5 calls this "a separate skinning compute pass feeding the same output buffer" as
// the cage deform. The output is written in the engine's own rhi::MeshVertex layout, which is what
// lets it BE a mesh's vertex buffer rather than something a draw call has to be taught about.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Math.hpp"

#include <vector>

namespace aver::render {

// Bones one dispatch can address, and the ceiling on what an instance's own ring is sized to.
inline constexpr u32 kSkinMaxBones = 256;
// Vertices per thread group. The shader's own [numthreads] must agree.
inline constexpr u32 kSkinGroupSize = 64;

// The element stride of `rest` and `out`.
//
// IT IS rhi::MeshVertex, EXACTLY -- position, normal, uv -- and that is the design rather than a
// coincidence. A posed buffer in the engine's own vertex layout can be handed to the input
// assembler, to the mesh shader's root SRV, and to a BLAS build with no change to any of them; a
// bespoke position-only layout would have required teaching all three about a second stream. The
// shader carries the uv through untouched, which also makes it the instrument the self-test uses to
// prove the two sides agree about this number.
inline constexpr u32 kSkinVertexStride = 32;
static_assert(kSkinVertexStride == sizeof(rhi::MeshVertex),
              "the skinned output IS a MeshVertex stream; a divergence here is a silent stride bug");

// One skinned INSTANCE's GPU residency: the two static streams in, the posed stream out, and the
// per-instance bone ring that feeds it.
//
// PER INSTANCE, NOT PER MESH, AND NOT PER PASS. The bone buffers and binding sets live here for a
// reason that cost a real bug: they were originally a ring on the pass itself, advanced once per
// dispatch. Two instances in one frame then shared a slot, so the second one's setSrvBuffer
// overwrote the descriptors the first one's ALREADY-RECORDED dispatch would read at execution time
// -- the first instance's output was never written and it wore last frame's pose forever. Nothing
// caught it because the self-test dispatched exactly one mesh per frame. Owning the ring here makes
// that collision impossible to express.
struct SkinnedMeshGpu {
    rhi::BufferHandle rest = 0;   // StructuredBuffer<SkinVertex>: rest position, normal, uv
    rhi::BufferHandle bind = 0;   // StructuredBuffer<SkinBind>: four bone indices, four weights
    rhi::BufferHandle out  = 0;   // RWStructuredBuffer<SkinVertex>, UAV-capable
    u32 vertexCount = 0;
    u32 boneCapacity = 0;         // what this instance's ring was sized for
    // False when the caller supplied `out` -- as a mesh whose vertex buffer IS the skin target
    // does. destroyMesh must then not free a buffer somebody else owns.
    bool ownsOut = true;

    // What `out` is in RIGHT NOW. Tracked here because a barrier must claim the state the hardware
    // actually holds, and because a buffer's state does not survive the frame -- see skinTransition.
    rhi::ResourceState outState = rhi::ResourceState::Common;

    // Frames in flight. Two, matching the backend's swapchain depth: a bone buffer written while
    // the GPU still reads last frame's copy is a use-after-write, and writeBuffer is by contract
    // immediate and unsynchronised.
    static constexpr u32 kRing = 2;
    rhi::BufferHandle     bones[kRing] = {};
    rhi::BindingSetHandle sets[kRing] = {};
    u32                   frame = 0;

    bool valid() const { return rest && bind && out && vertexCount > 0 && bones[0] && sets[0]; }
};

// Transitions a skinned instance's output buffer and keeps SkinnedMeshGpu::outState in step.
//
// A FRAME MUST END WITH THE BUFFER BACK IN Common. D3D12 decays every buffer to the common state
// when a command list finishes, so a barrier next frame claiming VertexBuffer would be claiming a
// state the hardware no longer holds -- which is a validation error, not a stale-data bug, and so
// is silent until someone runs with the debug layer.
void skinTransition(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, rhi::ResourceState to);

// Records skinning dispatches. Owns the shader and the pipeline; every per-instance resource lives
// in the SkinnedMeshGpu it belongs to.
class SkinningPass {
public:
    SkinningPass() = default;
    ~SkinningPass();
    SkinningPass(const SkinningPass&) = delete;
    SkinningPass& operator=(const SkinningPass&) = delete;

    // Compiles the shader and creates the pipeline. False leaves the pass inert, not broken: every
    // other method then does nothing, so a device without compute is a missing effect rather than
    // a crash.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return pipeline_ != 0; }

    // Uploads a decoded mesh's rest and bind streams and allocates this instance's output and bone
    // ring. `boneCount` sizes the ring to the rig that will actually drive it rather than to
    // kSkinMaxBones, so a five-bone prop does not carry a 256-bone upload buffer.
    //
    // `existingOut` lets the caller supply the output buffer -- which is how a mesh whose VERTEX
    // BUFFER is the skin target gets skinned in place. Zero means allocate one and own it.
    bool createMesh(const fmt::OcMeshData& mesh, u32 boneCount, SkinnedMeshGpu& out,
                    rhi::BufferHandle existingOut = 0);
    void destroyMesh(SkinnedMeshGpu& m);

    // Records one dispatch: `skin` is poseToSkinning's output, in the engine's row-vector
    // convention, and is uploaded as-is. Bones past this instance's capacity are dropped, and a
    // vertex referencing one of them falls back to its rest position -- the same rule the CPU
    // applies to an out-of-range index, so the two still agree about a rig this cannot hold.
    //
    // On return the output buffer is left in ResourceState::VertexBuffer, which is the state a
    // raster pass wants of it. The CALLER is what must return it to Common before the frame ends.
    void dispatch(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, const Mat4* skin, u32 boneCount);

private:
    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::ShaderHandle      cs_ = 0;
    rhi::PipelineHandle    pipeline_ = 0;

    std::vector<f32> upload_;   // bone matrices flattened for writeBuffer, kept to avoid a per-call
                                // allocation in what is meant to be per-frame work
};

} // namespace aver::render
