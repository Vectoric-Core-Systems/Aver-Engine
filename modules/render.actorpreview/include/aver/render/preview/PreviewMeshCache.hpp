#pragma once
// The preview's own mesh registry: loads on demand the few meshes one actor names, and caches them.
// Independent of the level editor's table, so the tab works without a project sweep.
#include "aver/rhi/RHI.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::render::preview {

// Resolves designer-file mesh paths to mesh handles, keyed by canonical path.
class PreviewMeshCache {
public:
    // Sets the content root every mesh path is relative to. Clears the cache when it changes.
    void setContentRoot(rhi::IDevice& device, std::string root);
    const std::string& contentRoot() const { return root_; }

    // Resolves a path to a mesh handle, loading it on the first ask. Returns 0 when the file is
    // absent or will not load, and caches that failure too.
    rhi::MeshHandle resolve(rhi::IDevice& device, std::string_view meshPath, f32* outRadius = nullptr);

    // The furthest vertex from the origin, in the mesh's own units. 0 for an unknown path.
    f32 radiusOf(std::string_view meshPath) const;

    // Builds or returns a character capsule standing on Z=0, in centimetres, cached per size.
    rhi::MeshHandle capsule(rhi::IDevice& device, f32 heightCm, f32 radiusCm, f32* outRadius = nullptr);

    // Every path that failed to resolve, canonical form, in first-asked order.
    const std::vector<std::string>& missing() const { return missing_; }

    // Forgets every cached mesh, radius and miss.
    void clear(rhi::IDevice& device);
    usize loaded() const { return loaded_; }

private:
    std::string root_;
    std::unordered_map<std::string, rhi::MeshHandle> meshes_;   // keyed by canonical path
    std::unordered_map<std::string, f32> radii_;
    std::vector<std::string> missing_;
    usize loaded_ = 0;
};

} // namespace aver::render::preview
