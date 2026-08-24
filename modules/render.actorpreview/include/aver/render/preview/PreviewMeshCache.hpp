#pragma once
// The preview's own mesh registry: loads on demand the few meshes one actor names, and caches them.
// Independent of the level editor's table, so the tab works without a project sweep.
#include "aver/rhi/RHI.hpp"

#include <functional>
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

    // Uploads geometry the CALLER generates, cached under `key`, and returns the same handle on
    // every later ask for that key.
    //
    // THE POINT IS WHAT THIS CACHE DOES NOT HAVE TO KNOW. capsule() above generates its shape here,
    // which is fine for a capsule -- every preview wants one and it needs nothing but two floats.
    // A fluid volume's shell is generated too, but generating it means calling into Aver.Fluids, and
    // a mesh cache that linked the fluids module to draw a preview would be a dependency this tab
    // pays for on behalf of one component kind. The caller already links what it needs; this only
    // has to own the handle and free it in clear().
    //
    // A CALLBACK, NOT TWO VECTORS, so that naming geometry is cheap and building it is not. A
    // caller with a cached key never runs `build` at all, which is what keeps this off the cost of
    // a draw list rebuilt every frame. The first shape of this took the vectors directly and left
    // callers to probe with empty ones first -- and an empty ask is indistinguishable from "I
    // generated nothing", which this caches as a permanent miss, so the probe poisoned the very key
    // it was asking about and the real geometry that followed was never uploaded. Passing the
    // generator in removes the ambiguity rather than documenting around it.
    //
    // `key` must not collide with a content path. Follow capsule()'s own convention and start it
    // with '$', which canonicalMeshPath never produces.
    using MeshBuilder = std::function<void(std::vector<rhi::MeshVertex>&, std::vector<u32>&)>;
    rhi::MeshHandle generated(rhi::IDevice& device, const std::string& key,
                              const MeshBuilder& build, f32* outRadius = nullptr);

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
