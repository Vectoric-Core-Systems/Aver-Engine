#include "aver/world/ChunkPayload.hpp"

#if AVER_MODULE_SCENE

#  include "aver/core/Log.hpp"
#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"

namespace aver::world {
namespace {

// Appends `e` and then its children, depth first, so a parent is always at a lower index than any of
// its descendants. restore() relies on exactly that and does no fix-up pass.
void appendSubtree(const scene::World& w, scene::Entity e, i32 parentIndex,
                   const ChunkCoord& coord, i32 chunkSizeCm, std::vector<PayloadEntity>& out) {
    if (!w.valid(e)) return;
    const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
    if (!loc) return;

    PayloadEntity pe;
    pe.name = w.name(e);
    pe.objectId = w.objectId(e);
    pe.parent = parentIndex;
    pe.local = loc->xf;

    // A ROOT's transform is world-space, so it is rebased onto the chunk here. A child's is already
    // parent-relative and is carried through untouched -- rebasing it would move the child.
    if (parentIndex < 0) {
        const SplitPos sp = splitCm(loc->xf.position, chunkSizeCm);
        // The caller decided ownership; this only re-expresses the position against that decision.
        // A root whose split lands in another chunk means partition and capture disagree, and the
        // offset below would then be outside [0, chunkSize) -- which the round-trip test checks.
        pe.local.position =
            Vec3{sp.local.x + static_cast<f32>(chunkOriginCmAxis(sp.chunk.x - coord.x, chunkSizeCm)),
                 sp.local.y + static_cast<f32>(chunkOriginCmAxis(sp.chunk.y - coord.y, chunkSizeCm)),
                 sp.local.z + static_cast<f32>(chunkOriginCmAxis(sp.chunk.z - coord.z, chunkSizeCm))};
    }

    if (const auto* tg = w.component<scene::CTags>(e, scene::kComponentTags)) pe.tags = tg->bits;

    if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
        pe.hasMesh = true;
        pe.mesh = mr->mesh;
        pe.meshFlags = mr->flags;
        // THE NAME, NEVER THE TOKEN. See the header.
        if (mr->material) pe.material = aver_scene_material_name(mr->material);
        for (int a = 0; a < 3; ++a) { pe.aabbMin[a] = mr->aabbMin[a]; pe.aabbMax[a] = mr->aabbMax[a]; }
    }

    const i32 selfIndex = static_cast<i32>(out.size());
    out.push_back(std::move(pe));

    if (const auto* h = w.component<scene::CHierarchy>(e, scene::kComponentHierarchy)) {
        for (scene::Entity c = h->firstChild; c != scene::kInvalidEntity;) {
            const auto* ch = w.component<scene::CHierarchy>(c, scene::kComponentHierarchy);
            const scene::Entity next = ch ? ch->nextSibling : scene::kInvalidEntity;
            appendSubtree(w, c, selfIndex, coord, chunkSizeCm, out);
            c = next;
        }
    }
}

} // namespace

ChunkPayload capture(const scene::World& w, const std::vector<scene::Entity>& roots,
                     const ChunkCoord& coord, i32 chunkSizeCm) {
    ChunkPayload p;
    p.coord = coord;
    p.chunkSizeCm = chunkSizeCm;
    p.entities.reserve(roots.size());
    for (const scene::Entity e : roots) appendSubtree(w, e, -1, coord, chunkSizeCm, p.entities);
    return p;
}

std::vector<scene::Entity> restore(const ChunkPayload& p, scene::World& w, const RestoreOptions& opt) {
    std::vector<scene::Entity> made(p.entities.size(), scene::kInvalidEntity);
    bool warnedOrphan = false;   // said once per restore, not once per orphaned child

    for (usize i = 0; i < p.entities.size(); ++i) {
        const PayloadEntity& pe = p.entities[i];

        Transform xf = pe.local;
        scene::Entity parent = scene::kInvalidEntity;
        if (pe.parent >= 0 && static_cast<usize>(pe.parent) < i) {
            parent = made[static_cast<usize>(pe.parent)];
            // A PARENT THAT NEVER GOT CREATED TAKES ITS SUBTREE WITH IT.
            //
            // made[] starts as kInvalidEntity and the `continue` below leaves it that way for any
            // entity World::create refused -- which it does, returning kInvalidEntity, once the
            // index space is exhausted (World.cpp:310). Falling through here with parent still
            // kInvalidEntity does NOT merely lose the hierarchy: w.create treats kInvalidEntity as
            // "no parent" (World.cpp:339), and this branch has already skipped the else below that
            // puts the chunk origin back on. So the child would be created as a ROOT holding a
            // PARENT-RELATIVE position read as a world one -- landing near the world origin,
            // hundreds of metres from where it belongs, silently.
            //
            // Skipping is the honest outcome: the payload described this entity only in terms of a
            // parent that does not exist, so there is no correct place to put it. It also cascades
            // properly -- made[i] stays invalid, so this entity's own children skip in turn.
            if (parent == scene::kInvalidEntity) {
                if (!warnedOrphan) {
                    warnedOrphan = true;
                    AVER_WARN("[Chunk] restoring {},{},{}: an entity could not be created, so its "
                              "children are skipped rather than reparented to the world",
                              p.coord.x, p.coord.y, p.coord.z);
                }
                continue;
            }
        } else {
            // A root: put the chunk's origin back on. This is the exact inverse of capture, and it
            // is exact -- the origin is integer centimetres and the offset is under one chunk, so
            // the only rounding is the final narrowing to the f32 the runtime uses.
            xf.position = Vec3{
                pe.local.position.x + static_cast<f32>(chunkOriginCmAxis(p.coord.x, p.chunkSizeCm)),
                pe.local.position.y + static_cast<f32>(chunkOriginCmAxis(p.coord.y, p.chunkSizeCm)),
                pe.local.position.z + static_cast<f32>(chunkOriginCmAxis(p.coord.z, p.chunkSizeCm))};
        }

        const scene::Entity e = w.create(pe.name, parent, xf);
        if (e == scene::kInvalidEntity) continue;
        made[i] = e;
        // Carried rather than rehashed: a level may have authored an id that is not fnv1a64(name),
        // and recomputing it would silently renumber every such entity on the first reload.
        w.setObjectId(e, pe.objectId);

        if (pe.tags) {
            if (auto* tg = static_cast<scene::CTags*>(w.addComponent(e, scene::kComponentTags)))
                tg->bits = pe.tags;
        }

        if (pe.hasMesh) {
            auto* mr = static_cast<scene::CMeshRenderer*>(
                w.addComponent(e, scene::kComponentMeshRenderer));
            if (mr) {
                // addComponent hands back ZERO-FILLED bytes, so nothing here may rely on a default
                // member initialiser having run (ComponentPool.cpp:63). Every field is written.
                mr->mesh = pe.mesh;
                mr->material = pe.material.empty() ? 0 : aver_scene_material(0, pe.material.c_str());
                mr->flags = pe.meshFlags;
                for (int a = 0; a < 3; ++a) { mr->aabbMin[a] = pe.aabbMin[a]; mr->aabbMax[a] = pe.aabbMax[a]; }
                if (mr->material && opt.bindMaterial) opt.bindMaterial(mr->material, pe.material);
            }
        }

        if (pe.hasBody && opt.createBody) {
            // World position, which is what the physics ABI takes -- it knows nothing of chunks.
            const Vec3 half{pe.bodyHalfExtentCm[0], pe.bodyHalfExtentCm[1], pe.bodyHalfExtentCm[2]};
            opt.createBody(e, xf.position, half);
        }
    }

    return made;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
