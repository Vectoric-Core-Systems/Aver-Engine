// See LandscapeRenderer.hpp -- in particular the residency note, which is why this class is a cache
// with a fallback rather than a loop over the selection.
#include "aver/landscape/LandscapeRenderer.hpp"

#include "aver/core/Log.hpp"

namespace aver::landscape {

void LandscapeRenderer::setSurface(const f32 baseColor[4], f32 metallic, f32 roughness) {
    for (int i = 0; i < 4; ++i) baseColor_[i] = baseColor[i];
    metallic_ = metallic;
    roughness_ = roughness;
}

u32 LandscapeRenderer::residentAncestor(const LandscapeTree& tree, u32 node) const {
    // Walked by scanning for a parent rather than stored, because a node knows its children and not
    // its parent, and a parent pointer would have to be kept correct through every rebuild for the
    // sake of a path that only runs when the cache is full.
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

void LandscapeRenderer::draw(rhi::IDevice& device, const fmt::OcLandData& data,
                             const LandscapeTree& tree, const SelectResult& selection,
                             const f32 world[16], f32 uvTilingCm) {
    stats_ = LandscapeRenderStats{};
    drawnThisFrame_.clear();

    // THE ROOT IS ALWAYS RESIDENT, and it is claimed before anything else can take the last slot.
    //
    // This is what makes "no holes" an actual guarantee rather than a hope. The ancestor fallback below
    // can only substitute a node that is already resident, and a first frame at the cache cap has only
    // LEAVES resident -- a leaf is nobody's ancestor, so every other selected node had nowhere to fall
    // back to and was skipped. The test caught exactly that: a cache of 1 submitted one draw and
    // dropped fifteen nodes on the floor.
    //
    // Reserving the root costs one mesh, ~242 KiB, and buys a worst case that is "the whole section is
    // drawn at its coarsest" instead of "most of the ground is missing".
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
                    // Reinterpreted rather than copied. The static_asserts in the header are what make
                    // this legal, and they are checked at compile time in this translation unit
                    // precisely so it cannot silently become a lie.
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
                // Not resident and not creatable. A SKIPPED node is a hole in the ground, so fall back
                // to the nearest ancestor that is resident: coarser than asked for, but continuous.
                const u32 anc = residentAncestor(tree, nodeIndex);
                if (anc == kInvalidNode) { ++stats_.skipped; continue; }
                toDraw = anc;
                ++stats_.substituted;
            }
        }

        // Deduplicated, because several descendants can defer to the same ancestor and drawing it twice
        // costs a full submitted draw -- about 2 KiB of the frame's 1 MiB constant ring -- for nothing.
        if (!drawnThisFrame_.insert(toDraw).second) continue;

        const auto mesh = meshes_.find(toDraw);
        if (mesh == meshes_.end()) { ++stats_.skipped; continue; }

        // ONE binding for the whole landscape, and it is sticky, so it is set before the loop by the
        // caller rather than per node. This function only submits geometry.
        device.drawMesh(mesh->second, world, baseColor_, metallic_, roughness_);
        ++stats_.submitted;
    }

    stats_.residentNodes = static_cast<u32>(meshes_.size());

    // Said once, loudly, when the cache fills. Silent degradation to coarser terrain is exactly the
    // kind of thing that gets reported as "the ground looks blurry" months later.
    if (stats_.substituted > 0 && !warnedFull_) {
        warnedFull_ = true;
        AVER_WARN("[Landscape] the mesh cache is full at {} nodes; {} node(s) fell back to a coarser "
                  "resident ancestor. There is no destroyMesh, so residency cannot be reclaimed -- "
                  "raise maxResidentNodes or use smaller sections.",
                  maxResident_, stats_.substituted);
    }
}

} // namespace aver::landscape
