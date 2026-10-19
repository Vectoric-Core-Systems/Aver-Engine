// Thick breaks on the system side: a piece's back skin and wall quads appended to its draw arrays (or handed to an
// interior-material cap entity), their per-frame positions, the cap level of detail, and baking a settled piece into a
// plain static mesh. The geometry itself is Aver.SoftBody's Caps.hpp; this file only places it. See docs/SOFTBODY.md.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {

// A cap entity draws a piece's walls with their own material. It is a child with an identity local transform, so it
// packs against the same origin as its piece. Created invisible; the draw side reveals it with its first resident.
void SoftBodySystem::ensureCapEntity(scene::World& world, Entry& en, Piece& p) {
    if (p.capEntity != scene::kInvalidEntity && world.valid(p.capEntity)) return;
    if (p.entity == scene::kInvalidEntity || !world.valid(p.entity)) return;
    const auto* src = world.component<scene::CMeshRenderer>(en.entity, scene::kComponentMeshRenderer);
    if (!src) return;
    const scene::Entity cap = world.create("SoftCap", p.entity, Transform{});
    if (cap == scene::kInvalidEntity) return;
    auto* mr = static_cast<scene::CMeshRenderer*>(world.addComponent(cap, scene::kComponentMeshRenderer));
    *mr = *src;
    mr->material = en.interiorToken;
    mr->flags &= ~scene::kMeshRendererVisible;
    mr->dirty = 1;
    auto* sb = static_cast<scene::CSoftBody*>(world.addComponent(cap, scene::kComponentSoftBody));
    sb->flags = scene::kSoftBodyPiece;
    p.capEntity = cap;
    needFlush_ = true;
}

// Called by fillPieceDraw once the front arrays exist: builds the caps from the visible triangles and appends them in
// the layout [front | back skin | walls]. With an interior material the walls go to the cap entity's own arrays.
void SoftBodySystem::buildPieceCaps(scene::World* world, Entry& en, Piece& p) {
    p.caps = CapGeometry{};
    p.capsOn = false;
    p.capWorld.clear(); p.capIndices.clear(); p.capSource.clear(); p.capUv.clear();
    if (!opt_.capsEnabled || !capsEnabled(en.asset) || p.empty || p.capsHidden || p.frontCount == 0) return;
    const f32 thickness = capThicknessCm(en.asset) * en.uniformScale;
    if (!(thickness > 0.0f)) return;

    CapSpec spec;
    spec.thicknessCm = thickness;
    spec.backSkin = backSkinFor(en.asset.caps.backSkin, p.debris, en.openSheet);
    spec.uvCmPerTile = std::max(en.asset.caps.uvCmPerTile, 1e-3f);
    p.caps = buildCaps(p.drawIndices.data(), p.frontIndexCount, p.drawWeld.data(), p.drawWorld.data(), p.drawUv.data(), p.frontCount, spec);
    if (p.caps.overLimit && !en.capsLogged) {
        en.capsLogged = true;
        AVER_WARN("[SoftBody] entity {}: a piece is over the cap vertex limit ({}); it draws without caps", en.entity, spec.vertexLimit);
    }
    if (p.caps.empty()) return;
    p.capsOn = true;

    u32 base = p.frontCount;
    for (const CapVertex& v : p.caps.backVerts) {
        p.sourceVertex.push_back(kGeneratedVertex);
        p.drawUv.push_back(v.u); p.drawUv.push_back(v.v);
    }
    for (const u32 i : p.caps.backIndices) p.drawIndices.push_back(base + i);
    base += static_cast<u32>(p.caps.backVerts.size());

    const bool interior = en.interiorToken != 0 && world && !p.caps.wallVerts.empty();
    if (interior) {
        for (const CapVertex& v : p.caps.wallVerts) {
            p.capSource.push_back(kGeneratedVertex);
            p.capUv.push_back(v.u); p.capUv.push_back(v.v);
        }
        p.capIndices = p.caps.wallIndices;
        p.capWorld.assign(p.caps.wallVerts.size(), Vec3{0, 0, 0});
        ensureCapEntity(*world, en, p);
    } else {
        for (const CapVertex& v : p.caps.wallVerts) {
            p.sourceVertex.push_back(kGeneratedVertex);
            p.drawUv.push_back(v.u); p.drawUv.push_back(v.v);
        }
        for (const u32 i : p.caps.wallIndices) p.drawIndices.push_back(base + i);
        base += static_cast<u32>(p.caps.wallVerts.size());
    }
    p.drawWorld.resize(base, Vec3{0, 0, 0});
}

// The cap vertex positions from the front's current positions: smooth normals first, then each cap vertex offset along
// -normal by its depth. Cheap (one pass over the draw vertices), and only run for pieces whose positions changed.
void SoftBodySystem::refreshCaps(Entry& en, Piece& p) {
    if (!p.capsOn || p.frontCount == 0) return;
    const f32 thickness = capThicknessCm(en.asset) * en.uniformScale;
    p.normals.resize(p.frontCount);
    smoothNormals(p.drawWorld.data(), p.frontCount, p.drawIndices.data(), p.frontIndexCount, p.drawWeld.data(), p.normals.data());
    Vec3* out = p.drawWorld.data() + p.frontCount;
    if (!p.caps.backVerts.empty()) {
        capPositions(p.caps.backVerts, p.drawWorld.data(), p.normals.data(), thickness, out);
        out += p.caps.backVerts.size();
    }
    if (!p.caps.wallVerts.empty()) {
        if (!p.capWorld.empty()) capPositions(p.caps.wallVerts, p.drawWorld.data(), p.normals.data(), thickness, p.capWorld.data());
        else                     capPositions(p.caps.wallVerts, p.drawWorld.data(), p.normals.data(), thickness, out);
    }
}

// Beyond caps.lodCm from the camera a piece draws without caps; a 10% band keeps it from flickering at the edge.
void SoftBodySystem::capLod(scene::World&) {
    if (!opt_.capsEnabled) return;
    for (auto& [e, en] : entries_) {
        if (en.failed || !capsEnabled(en.asset) || en.asset.caps.lodCm <= 0.0f || !(capThicknessCm(en.asset) > 0.0f)) continue;
        const f32 lod = en.asset.caps.lodCm;
        for (Piece& p : en.pieces) {
            if (!p.alive || p.empty) continue;
            const f32 d = dist(p.centroid, viewPos_);
            const bool hide = p.capsHidden ? d > lod * 0.9f : d > lod;
            if (hide == p.capsHidden) continue;
            p.capsHidden = hide;
            fillPieceDraw(en, p);   // a new epoch: the draw side rebuilds the mesh with or without the caps
        }
    }
}

// A piece that has been asleep long enough stops being a body: no Jolt body, no skin target, no per-frame pack. It is
// drawn as one plain static mesh around its centroid, and its despawn shrink becomes an entity scale. One way until
// Repair; a rigid body that lands on it passes through, and damage and fields skip it.
void SoftBodySystem::bakePiece(scene::World& world, Entry& en, Piece& p) {
    if (p.baked || !p.body) return;
    aver_phys_remove_body(p.body);
    byBody_.erase(p.body);
    p.body = 0;
    p.baked = true;
    p.fieldHeld = false;
    if (en.bakedBeams.size() < en.sim.cage.beams.size()) en.bakedBeams.resize(en.sim.cage.beams.size(), 0);
    for (const u32 bi : p.beams) en.bakedBeams[bi] = 1;
    if (auto* sb = world.component<scene::CSoftBody>(p.entity, scene::kComponentSoftBody)) sb->body = 0;
    p.origin = p.centroid;
    if (p.entity != scene::kInvalidEntity) world.setLocalPosition(p.entity, p.centroid);
    ++p.epoch;
    p.needsPack = true;
    needFlush_ = true;
}

// A debris or cap entity is hidden while it is culled (sub-pixel) and a cap entity while its caps are off. The draw side
// sets the visible bit again once the view says revealOnReady and a resident has packed.
void SoftBodySystem::publishVisibility(scene::World& world, Entry&, Piece& p, bool& culled) {
    culled = false;
    if (p.debris) {
        const f32 radius = 0.5f * dist(p.boundsMin, p.boundsMax);
        const f32 d = dist(p.centroid, viewPos_);
        culled = opt_.cullPixelRatio > 0.0f && d > 1.0f && radius < d * opt_.cullPixelRatio;
        if (culled && p.entity != scene::kInvalidEntity)
            if (auto* mr = world.component<scene::CMeshRenderer>(p.entity, scene::kComponentMeshRenderer))
                if (mr->flags & scene::kMeshRendererVisible) { mr->flags &= ~scene::kMeshRendererVisible; mr->dirty = 1; }
    }
    if (p.capEntity != scene::kInvalidEntity && (culled || p.capsHidden || p.capIndices.empty()))
        if (auto* cmr = world.component<scene::CMeshRenderer>(p.capEntity, scene::kComponentMeshRenderer))
            if (cmr->flags & scene::kMeshRendererVisible) { cmr->flags &= ~scene::kMeshRendererVisible; cmr->dirty = 1; }
}

}  // namespace aver::softbody
