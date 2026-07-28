#pragma once
// The preview's OWN mesh registry.
//
// The editor already has one -- a private member of the app, filled by a sweep of the content root
// when a project opens. The preview does not use it, and that is the point rather than duplication
// for its own sake: an asset editor that reached into the level editor's state would be an asset
// editor that cannot exist without one, cannot be tested without one, and quietly makes "the tab is
// independent of the level" false. The forty lines below are what that independence costs.
//
// ON DEMAND, not a sweep. A preview needs the three to six meshes one actor names; the sweep exists
// because a LEVEL may reference anything in the project. Loading a whole content root to show one
// car is the wrong shape, and on a large project it is a visible stall on opening a tab.
//
// It is also where the two halves of the mesh-path disagreement are reconciled: every lookup goes
// through fmt::canonicalMeshPath, so a designer file that writes "Content/Meshes/X.ocmesh" and an
// engine that registers "Meshes/X.ocmesh" name the same asset.
#include "aver/rhi/RHI.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::render::preview {

class PreviewMeshCache {
public:
    // The project's content root, which every mesh path is relative to. Changing it CLEARS the
    // cache: the same relative path in a different project is a different file, and keeping the old
    // handle would draw the previous project's geometry in this one's preview.
    void setContentRoot(rhi::IDevice& device, std::string root);
    const std::string& contentRoot() const { return root_; }

    // Resolve a path as written in a designer file to a mesh handle, loading it if this is the first
    // ask. Returns 0 when the file is absent or will not load -- and the preview draws nothing for a
    // zero handle rather than substituting a shape, because a stand-in reads as the actor genuinely
    // containing it.
    //
    // A FAILURE IS CACHED TOO. Without that, an actor naming a missing mesh re-reads the disk for
    // every model, every frame, and the editor's frame time quietly becomes a function of how wrong
    // the file is.
    rhi::MeshHandle resolve(rhi::IDevice& device, std::string_view meshPath, f32* outRadius = nullptr);

    // The furthest vertex from the origin, in the mesh's own units. Needed because framing cannot be
    // done from placement ORIGINS alone: a class-level mesh has no placement at all -- it sits at the
    // origin with no transform -- so every such actor would frame identically and a unit sphere would
    // arrive on screen as one pixel. It did.
    f32 radiusOf(std::string_view meshPath) const;

    // Every path that failed to resolve, canonical form, in first-asked order. The panel shows these:
    // an actor whose meshes are all missing renders an empty view, and an empty view with no
    // explanation is indistinguishable from a broken preview.
    const std::vector<std::string>& missing() const { return missing_; }

    void clear(rhi::IDevice& device);
    usize loaded() const { return loaded_; }

private:
    std::string root_;
    // Keyed by CANONICAL path, so the two spellings collapse to one entry and one upload.
    std::unordered_map<std::string, rhi::MeshHandle> meshes_;
    std::unordered_map<std::string, f32> radii_;
    std::vector<std::string> missing_;
    usize loaded_ = 0;
};

} // namespace aver::render::preview
