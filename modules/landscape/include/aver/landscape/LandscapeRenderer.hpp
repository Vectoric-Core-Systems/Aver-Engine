#pragma once
// The GPU half of the landscape: one ordinary MeshHandle per node, and the per-frame draw loop.
// The mesh cache only ever grows -- the RHI has no destroyMesh -- so it is capped, and a node that
// cannot be made resident draws its nearest resident ancestor instead of leaving a hole.
#include "aver/landscape/ChunkMesh.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/rhi/RHI.hpp"

#include <unordered_map>
#include <unordered_set>

namespace aver::landscape {

// The one place the two vertex layouts may be compared; they must agree for the upload reinterpret.
static_assert(sizeof(LandVertex) == sizeof(rhi::MeshVertex),
              "LandVertex must stay uploadable as rhi::MeshVertex without conversion");
static_assert(offsetof(LandVertex, px) == offsetof(rhi::MeshVertex, px), "position offset drifted");
static_assert(offsetof(LandVertex, nx) == offsetof(rhi::MeshVertex, nx), "normal offset drifted");
static_assert(offsetof(LandVertex, u)  == offsetof(rhi::MeshVertex, u),  "uv offset drifted");

// What one frame's landscape draw did.
struct LandscapeRenderStats {
    u32 submitted = 0;      // drawMesh calls made
    u32 created = 0;        // meshes uploaded this frame
    u32 substituted = 0;    // nodes that fell back to a resident ancestor
    u32 residentNodes = 0;
    u32 skipped = 0;        // selected, not resident, and no resident ancestor either
    u32 evicted = 0;        // least-recently-used residents freed to make room this frame
};

// Caches node meshes and submits the selected nodes each frame.
class LandscapeRenderer {
public:
    // Builds a renderer whose mesh cache holds at most `maxResidentNodes`.
    explicit LandscapeRenderer(u32 maxResidentNodes = 512) : maxResident_(maxResidentNodes) {}

    // Sets the one material the whole landscape is shaded with.
    void setSurface(const f32 baseColor[4], f32 metallic, f32 roughness);

    // Draws one section's selected nodes, creating their meshes lazily. `world` is applied to every node.
    void draw(rhi::IDevice& device, const fmt::OcLandData& data, const LandscapeTree& tree,
              const SelectResult& selection, const f32 world[16], f32 uvTilingCm = 1000.0f);

    const LandscapeRenderStats& stats() const { return stats_; }

    // Releases every resident mesh.
    //
    // IT USED TO LEAK, and said so: "Forgets the resident meshes. Call when the device goes; it
    // cannot free them." That was true -- there was no IDevice::destroyMesh -- and it meant one
    // vertex buffer and one index buffer per cached node stranded on the GPU for the life of the
    // process, every time a section was closed. The device is a parameter now because freeing is
    // the whole point; a caller that genuinely only wants the handles dropped is a caller whose
    // device has already gone, and there is no such caller in the tree.
    void forgetAll(rhi::IDevice& device) {
        for (const auto& kv : meshes_) device.destroyMesh(kv.second.handle);
        meshes_.clear();
        warnedFull_ = false;
    }

private:
    // The nearest ancestor of `node` that already has a mesh, or kInvalidNode.
    u32 residentAncestor(const LandscapeTree& tree, u32 node) const;

    // One cached node mesh and the frame it was last wanted on, which is what makes eviction
    // possible: without it the cache can only refuse to grow, never choose what to give up.
    struct Resident {
        rhi::MeshHandle handle = 0;
        u64 lastUsed = 0;
    };
    // Evicts the least-recently-used resident to make room, and returns true if it managed to.
    // Refuses to evict anything wanted THIS frame, and refuses to evict the root -- which is the
    // ancestor every other node falls back to, so freeing it turns a substitution into a hole.
    bool evictOne(rhi::IDevice& device, u32 rootIndex);

    std::unordered_map<u32, Resident> meshes_;
    std::unordered_set<u32> drawnThisFrame_;
    u64 frame_ = 0;
    f32 baseColor_[4] = {0.42f, 0.45f, 0.36f, 1.0f};
    f32 metallic_ = 0.0f;
    f32 roughness_ = 0.85f;
    u32 maxResident_ = 512;
    bool warnedFull_ = false;
    LandscapeRenderStats stats_;
};

} // namespace aver::landscape
