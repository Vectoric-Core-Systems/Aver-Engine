#pragma once
// GPU linear-blend skinning: one compute dispatch turns a mesh's rest vertices into posed ones.
//
// The parity partner of anim::skinVertices, whose comment states the contract both sides implement.
// This module links the GENERIC RHI and never a backend, so the pass is expressible against any
// device -- the same property that made Aver.Render.UI testable.
//
// RENDERING.md 7.5 calls this "a separate skinning compute pass feeding the same output buffer" as
// the cage deform. The output buffer is therefore created UAV-capable and transitioned to
// VertexBuffer after the dispatch, so a raster pass can consume it as the position stream once the
// mesh draw path can accept a substituted stream.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Math.hpp"

#include <vector>

namespace aver::render {

// Bones one dispatch can address. The bone buffer is sized for this once, not per mesh.
inline constexpr u32 kSkinMaxBones = 256;
// Vertices per thread group. The shader's own [numthreads] must agree.
inline constexpr u32 kSkinGroupSize = 64;

// One skinned mesh's GPU residency: the two static streams in, the posed stream out.
struct SkinnedMeshGpu {
    rhi::BufferHandle rest = 0;   // StructuredBuffer<SkinVertex>: rest position then rest normal
    rhi::BufferHandle bind = 0;   // StructuredBuffer<SkinBind>: four bone indices, four weights
    rhi::BufferHandle out  = 0;   // RWStructuredBuffer<SkinVertex>, UAV-capable
    u32 vertexCount = 0;

    // What `out` is in RIGHT NOW. Tracked here because a barrier must claim the state the hardware
    // actually holds, and because a buffer's state does not survive the frame -- see skinTransition.
    rhi::ResourceState outState = rhi::ResourceState::Common;

    bool valid() const { return rest && bind && out && vertexCount > 0; }
};

// The element stride of `rest` and `out`: rest position then rest normal, tightly packed. Declared
// so a raster pass binding `out` as a vertex stream states the same number the shader was compiled
// against rather than a second, independently-derived one.
inline constexpr u32 kSkinVertexStride = 24;

// Transitions a skinned mesh's output buffer and keeps SkinnedMeshGpu::outState in step.
//
// A FRAME MUST END WITH THE BUFFER BACK IN Common. D3D12 decays every buffer to the common state
// when a command list finishes, so a barrier next frame claiming VertexBuffer would be claiming a
// state the hardware no longer holds -- which is a validation error, not a stale-data bug, and so
// is silent until someone runs with the debug layer.
void skinTransition(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, rhi::ResourceState to);

// Records skinning dispatches. Owns the pipeline, the binding set, and the bone-matrix ring.
class SkinningPass {
public:
    SkinningPass() = default;
    ~SkinningPass();
    SkinningPass(const SkinningPass&) = delete;
    SkinningPass& operator=(const SkinningPass&) = delete;

    // Compiles the shader and allocates the bone ring. False leaves the pass inert, not broken:
    // every other method then does nothing, so a device without compute is a missing effect rather
    // than a crash.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return pipeline_ != 0; }

    // Uploads a decoded mesh's rest and bind streams and allocates its output. False when the mesh
    // carries no skin, or when a buffer could not be made.
    bool createMesh(const fmt::OcMeshData& mesh, SkinnedMeshGpu& out);
    void destroyMesh(SkinnedMeshGpu& m);

    // Records one dispatch: `skin` is poseToSkinning's output, in the engine's row-vector
    // convention, and is uploaded as-is. Bones past kSkinMaxBones are dropped, and a vertex
    // referencing one of them falls back to its rest position -- the same rule the CPU applies to
    // an out-of-range index.
    //
    // On return the output buffer is left in ResourceState::VertexBuffer, which is the state a
    // raster pass wants of it. The CALLER is what must return it to Common before the frame ends.
    void dispatch(rhi::IRenderContext& ctx, SkinnedMeshGpu& m, const Mat4* skin, u32 boneCount);

private:
    bool createBoneRing();

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::ShaderHandle      cs_ = 0;
    rhi::PipelineHandle    pipeline_ = 0;

    // One binding set per frame in flight, for the same reason the bone buffers are a ring: a set
    // written while the GPU still reads last frame's descriptors is a use-after-write.
    static constexpr u32 kRing = 2;
    rhi::BufferHandle     bones_[kRing] = {};
    rhi::BindingSetHandle sets_[kRing] = {};
    u32                   frame_ = 0;

    std::vector<f32> upload_;   // bone matrices flattened for writeBuffer, kept to avoid a per-call
                                // allocation in what is meant to be per-frame work
};

} // namespace aver::render
