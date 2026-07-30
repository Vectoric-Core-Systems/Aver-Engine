#pragma once
// The GPU half: node meshes, and the per-frame draw loop.
//
// A SEPARATE TARGET from Aver.Landscape, for the reason Aver.Render.Voxi.Renderer is one -- it needs
// the RHI, and giving the CPU half that dependency would end the property that makes the CPU half
// testable with no device.
//
// NOT AN IRenderFeature, and that is a finding rather than a preference. A render feature's hooks run
// before and after the scene, but there is no hook that ADDS geometry to the lit pass: the colour and
// depth targets are backend-private, so a feature cannot draw into them. The only route to lit terrain
// is `drawMesh` from the app's own update loop, alongside the draws it already makes -- which is also
// what gets the terrain shadowed, voxelised and cone-traced for free, because Voxi replays whatever
// the frame submitted.
//
// EACH NODE IS AN ORDINARY MeshHandle. That is the whole trick: no new pipeline, no new shader, no new
// binding layout. A node inherits Voxi's lit PSO, its four shadow cascades, its cone-traced GI and its
// RayQuery sun shadows because it is indistinguishable from any other mesh in the frame.
//
// RESIDENCY IS FOREVER, and this is the constraint that shapes the class. There is no `destroyMesh` in
// the RHI -- `PreviewMeshCache.cpp:75` records the same discovery -- so a mesh cannot be freed until
// the device goes. A cache here can therefore only ever GROW, which means:
//
//   * it must be capped, or a camera wandering a large world exhausts video memory with no way back;
//   * and when the cap is reached, a selected node that is not resident must NOT be skipped, because
//     a skipped node is a hole in the ground. It draws its nearest RESIDENT ANCESTOR instead -- coarser
//     than asked for, but continuous, and deduplicated so the ancestor is drawn once however many of
//     its descendants defer to it.
//
// A fully resident 1025-sample section is 341 nodes at ~242 KiB, about 80 MiB. The default cap is set
// above that, so a single section is never degraded; the cap exists for the many-section case.
#include "aver/landscape/ChunkMesh.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/rhi/RHI.hpp"

#include <unordered_map>
#include <unordered_set>

namespace aver::landscape {

// The one place the two vertex layouts may legally be compared. Aver.Landscape cannot include the RHI,
// so its own static_assert can only pin its size; this is what proves the two actually agree, and it
// is why `buildChunkMesh` output can be uploaded with a reinterpret rather than a per-vertex copy.
static_assert(sizeof(LandVertex) == sizeof(rhi::MeshVertex),
              "LandVertex must stay uploadable as rhi::MeshVertex without conversion");
static_assert(offsetof(LandVertex, px) == offsetof(rhi::MeshVertex, px), "position offset drifted");
static_assert(offsetof(LandVertex, nx) == offsetof(rhi::MeshVertex, nx), "normal offset drifted");
static_assert(offsetof(LandVertex, u)  == offsetof(rhi::MeshVertex, u),  "uv offset drifted");

struct LandscapeRenderStats {
    u32 submitted = 0;      // drawMesh calls made
    u32 created = 0;        // meshes uploaded this frame
    u32 substituted = 0;    // nodes that fell back to a resident ancestor
    u32 residentNodes = 0;
    u32 skipped = 0;        // selected, not resident, and no resident ancestor either
};

class LandscapeRenderer {
public:
    // `maxResidentNodes` caps the cache. 512 is above a whole section's 341, so one section never
    // degrades; it is the multi-section case the cap is for.
    explicit LandscapeRenderer(u32 maxResidentNodes = 512) : maxResident_(maxResidentNodes) {}

    // How the surface is shaded. ONE material for the entire landscape, bound once before the node
    // loop because setDrawBinding is sticky -- 341 identical binding changes would be 341 wasted
    // constant-buffer allocations out of a 1 MiB ring.
    void setSurface(const f32 baseColor[4], f32 metallic, f32 roughness);

    // Draw one section's selected nodes. `world` is applied to every node; nodes are authored in world
    // centimetres already, so identity is the normal case and this exists for instancing a section.
    //
    // Meshes are created LAZILY here rather than up front: a section is 341 nodes and most of them are
    // never seen, so uploading all of it to look at one hillside would be 80 MiB of stalls for nothing.
    void draw(rhi::IDevice& device, const fmt::OcLandData& data, const LandscapeTree& tree,
              const SelectResult& selection, const f32 world[16], f32 uvTilingCm = 1000.0f);

    const LandscapeRenderStats& stats() const { return stats_; }

    // Resident meshes survive the device, so this only forgets them -- it cannot free them. Called when
    // the device goes, so a new device does not inherit handles that belonged to the old one.
    void forgetAll() { meshes_.clear(); }

private:
    // The nearest ancestor of `node` that already has a mesh, or kInvalidNode. Walks up rather than
    // rebuilding: an ancestor is always coarser and therefore always cheaper than what was asked for.
    u32 residentAncestor(const LandscapeTree& tree, u32 node) const;

    std::unordered_map<u32, rhi::MeshHandle> meshes_;
    std::unordered_set<u32> drawnThisFrame_;
    f32 baseColor_[4] = {0.42f, 0.45f, 0.36f, 1.0f};   // a muted green-grey; terrain, not a test colour
    f32 metallic_ = 0.0f;
    f32 roughness_ = 0.85f;                            // ground is rough; a shiny landscape reads as ice
    u32 maxResident_ = 512;
    // Latched so the cache-full warning is said once rather than every frame for the rest of
    // the session. A per-frame warning is a warning people filter out.
    bool warnedFull_ = false;
    LandscapeRenderStats stats_;
};

} // namespace aver::landscape
