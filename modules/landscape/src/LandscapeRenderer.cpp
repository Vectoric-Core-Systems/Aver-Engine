// Landscape draw loop: keeps a capped mesh cache and submits the selected nodes. See LandscapeRenderer.hpp.
#include "aver/landscape/LandscapeRenderer.hpp"

#include "aver/core/Log.hpp"

namespace aver::landscape {

// Sets the one material the whole landscape is shaded with.
void LandscapeRenderer::setSurface(const f32 baseColor[4], f32 metallic, f32 roughness) {
    for (int i = 0; i < 4; ++i) baseColor_[i] = baseColor[i];
    metallic_ = metallic;
    roughness_ = roughness;
}

// The nearest ancestor of `node` that already has a mesh, or kInvalidNode.
u32 LandscapeRenderer::residentAncestor(const LandscapeTree& tree, u32 node) const {
    u32 current = node;
    for (u32 guard = 0; guard < tree.levelCount() + 1; ++guard) {
        u32 parent = kInvalidNode;
        for (u32 i = 0; i < tree.nodes().size(); ++i) {
            const LandscapeNode& n = tree.nodes()[i];
            for (int c = 0; c < 4; ++c)
                if (n.child[c] == current) { parent = i; break; }
            if (parent != kInvalidNode) break;
        }
        if (parent == kInvalidNode) return kInvalidNode;      // reached the root
        if (meshes_.find(parent) != meshes_.end()) return parent;
        current = parent;
    }
    return kInvalidNode;
}

// Frees the least-recently-used resident. See the declaration for what it will not touch.
bool LandscapeRenderer::evictOne(rhi::IDevice& device, u32 rootIndex) {
    auto victim = meshes_.end();
    for (auto it = meshes_.begin(); it != meshes_.end(); ++it) {
        if (it->first == rootIndex) continue;          // the universal fallback ancestor
        if (it->second.lastUsed == frame_) continue;   // wanted on THIS frame; freeing it undoes the work
        if (victim == meshes_.end() || it->second.lastUsed < victim->second.lastUsed) victim = it;
    }
    if (victim == meshes_.end()) return false;         // everything resident is in use right now
    device.destroyMesh(victim->second.handle);
    meshes_.erase(victim);
    ++stats_.evicted;
    return true;
}

// Draws one section's selected nodes, creating meshes lazily and substituting resident ancestors.
void LandscapeRenderer::draw(rhi::IDevice& device, const fmt::OcLandData& data,
                             const LandscapeTree& tree, const SelectResult& selection,
                             const f32 world[16], f32 uvTilingCm) {
    stats_ = LandscapeRenderStats{};
    drawnThisFrame_.clear();
    ++frame_;

    // The root is claimed first so there is always one ancestor every other node can fall back to.
    const u32 rootIndex = tree.root();
    if (rootIndex != kInvalidNode && meshes_.find(rootIndex) == meshes_.end()) {
        ChunkMesh rootMesh;
        if (buildChunkMesh(data, tree, rootIndex, rootMesh, uvTilingCm)) {
            const rhi::MeshHandle h = device.createMesh(
                reinterpret_cast<const rhi::MeshVertex*>(rootMesh.vertices.data()),
                static_cast<u32>(rootMesh.vertices.size()),
                rootMesh.indices.data(), static_cast<u32>(rootMesh.indices.size()));
            if (h) { meshes_.emplace(rootIndex, Resident{h, frame_}); ++stats_.created; }
        }
    }

    for (u32 nodeIndex : selection.nodes) {
        u32 toDraw = nodeIndex;

        auto it = meshes_.find(nodeIndex);
        if (it == meshes_.end()) {
            // FULL NOW MEANS "MAKE ROOM", NOT "GIVE UP". This used to be a bare
            // `meshes_.size() < maxResident_` and nothing else: once the cache filled, every further
            // node drew a coarser ancestor forever, because there was no IDevice::destroyMesh and
            // residency could not be reclaimed. evictOne declines when everything resident is wanted
            // this frame, and the ancestor substitution below is still the answer then -- it is the
            // fallback now rather than the steady state.
            if (meshes_.size() >= maxResident_) evictOne(device, rootIndex);
            if (meshes_.size() < maxResident_) {
                ChunkMesh mesh;
                if (buildChunkMesh(data, tree, nodeIndex, mesh, uvTilingCm)) {
                    // Reinterpreted, not copied; the header's static_asserts are what make this legal.
                    const rhi::MeshHandle h = device.createMesh(
                        reinterpret_cast<const rhi::MeshVertex*>(mesh.vertices.data()),
                        static_cast<u32>(mesh.vertices.size()),
                        mesh.indices.data(),
                        static_cast<u32>(mesh.indices.size()));
                    if (h) {
                        meshes_.emplace(nodeIndex, Resident{h, frame_});
                        ++stats_.created;
                        it = meshes_.find(nodeIndex);
                    }
                }
            }
            if (it == meshes_.end()) {
                // Not resident and not creatable: draw the nearest resident ancestor instead.
                const u32 anc = residentAncestor(tree, nodeIndex);
                if (anc == kInvalidNode) { ++stats_.skipped; continue; }
                toDraw = anc;
                ++stats_.substituted;
            }
        }

        // Deduplicated: several descendants can defer to the same ancestor.
        if (!drawnThisFrame_.insert(toDraw).second) continue;

        const auto mesh = meshes_.find(toDraw);
        if (mesh == meshes_.end()) { ++stats_.skipped; continue; }

        // Touched on USE, including when it was reached as somebody else's ancestor -- an ancestor
        // standing in for four children is the most wanted node in the cache, and evicting it
        // because it is not itself selected is exactly backwards.
        mesh->second.lastUsed = frame_;
        device.drawMesh(mesh->second.handle, world, baseColor_, metallic_, roughness_);
        ++stats_.submitted;
    }

    stats_.residentNodes = static_cast<u32>(meshes_.size());

    // Said once per session, not once per frame.
    //
    // REWORDED because the cause changed. Substitution used to mean "the cache is full and can
    // never be reclaimed"; it now means the working set genuinely does not fit, because everything
    // resident was wanted on the same frame and there was nothing eviction could safely take.
    if (stats_.substituted > 0 && !warnedFull_) {
        warnedFull_ = true;
        AVER_WARN("[Landscape] {} node(s) fell back to a coarser resident ancestor: the {}-node mesh "
                  "cache is full of nodes all wanted on the same frame, so eviction had nothing to "
                  "take. Raise maxResidentNodes or use smaller sections.",
                  stats_.substituted, maxResident_);
    }
}

} // namespace aver::landscape
