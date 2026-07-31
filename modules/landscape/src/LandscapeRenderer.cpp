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

// Draws one section's selected nodes, creating meshes lazily and substituting resident ancestors.
void LandscapeRenderer::draw(rhi::IDevice& device, const fmt::OcLandData& data,
                             const LandscapeTree& tree, const SelectResult& selection,
                             const f32 world[16], f32 uvTilingCm) {
    stats_ = LandscapeRenderStats{};
    drawnThisFrame_.clear();

    // The root is claimed first so there is always one ancestor every other node can fall back to.
    const u32 rootIndex = tree.root();
    if (rootIndex != kInvalidNode && meshes_.find(rootIndex) == meshes_.end()) {
        ChunkMesh rootMesh;
        if (buildChunkMesh(data, tree, rootIndex, rootMesh, uvTilingCm)) {
            const rhi::MeshHandle h = device.createMesh(
                reinterpret_cast<const rhi::MeshVertex*>(rootMesh.vertices.data()),
                static_cast<u32>(rootMesh.vertices.size()),
                rootMesh.indices.data(), static_cast<u32>(rootMesh.indices.size()));
            if (h) { meshes_.emplace(rootIndex, h); ++stats_.created; }
        }
    }

    for (u32 nodeIndex : selection.nodes) {
        u32 toDraw = nodeIndex;

        auto it = meshes_.find(nodeIndex);
        if (it == meshes_.end()) {
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
                        meshes_.emplace(nodeIndex, h);
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

        device.drawMesh(mesh->second, world, baseColor_, metallic_, roughness_);
        ++stats_.submitted;
    }

    stats_.residentNodes = static_cast<u32>(meshes_.size());

    // Said once per session, not once per frame.
    if (stats_.substituted > 0 && !warnedFull_) {
        warnedFull_ = true;
        AVER_WARN("[Landscape] the mesh cache is full at {} nodes; {} node(s) fell back to a coarser "
                  "resident ancestor. There is no destroyMesh, so residency cannot be reclaimed -- "
                  "raise maxResidentNodes or use smaller sections.",
                  maxResident_, stats_.substituted);
    }
}

} // namespace aver::landscape
