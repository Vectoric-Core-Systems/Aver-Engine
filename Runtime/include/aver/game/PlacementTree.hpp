#pragma once
// PlacementTree: the hierarchy a level file gives its placements, kept across the two passes that create
// them -- world::instantiate's ordinary placements first, the host's class placements (placed actors)
// later, once scripting is up. Without it a placed actor spawned flat at its parent-relative position,
// and an ordinary placement under a placed actor loaded as a root. Header-only: both hosts use it, and a
// test drives it with a bare scene::World.
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/LevelInstance.hpp"

#include <vector>

namespace aver::game {

struct PlacementTree {
    std::vector<i32>           parent;    // by placement index: its parent's index, -1 for a root
    std::vector<u8>            isClass;   // by placement index: a class placement, spawned by the host
    std::vector<scene::Entity> entity;    // by placement index: the entity it produced, kInvalidEntity if none yet

    // The ordinary placements' entities from `inst`; class placements fill in through setEntity as they spawn.
    void build(const fmt::OcWorldData& w, const world::LevelInstance& inst) {
        const usize n = w.placements.size();
        parent.assign(n, -1);
        isClass.assign(n, 0);
        entity.assign(n, scene::kInvalidEntity);
        for (usize i = 0; i < n; ++i) {
            const i32 p = w.placements[i].parent;
            parent[i] = (p >= 0 && static_cast<usize>(p) < n) ? p : -1;
            isClass[i] = w.placements[i].className.empty() ? 0 : 1;
        }
        for (usize k = 0; k < inst.entities.size() && k < inst.placementIndex.size(); ++k)
            if (inst.placementIndex[k] < n) entity[inst.placementIndex[k]] = inst.entities[k];
    }

    void clear() { parent.clear(); isClass.clear(); entity.clear(); }

    void setEntity(usize index, scene::Entity e) { if (index < entity.size()) entity[index] = e; }

    // The live entity placement `index`'s parent produced, or kInvalidEntity (a root, or a parent that is gone).
    scene::Entity parentEntity(usize index, const scene::World& world) const {
        if (index >= parent.size() || parent[index] < 0) return scene::kInvalidEntity;
        const scene::Entity p = entity[static_cast<usize>(parent[index])];
        return (p != scene::kInvalidEntity && world.valid(p) && !world.destroyPending(p)) ? p : scene::kInvalidEntity;
    }

    // Moves every ordinary placement whose parent is a class placement under that actor. Its transform was
    // authored parent-relative, so it is kept as the local transform. Returns how many moved.
    u32 attachUnderClassPlacements(scene::World& world) const {
        u32 moved = 0;
        for (usize i = 0; i < parent.size(); ++i) {
            if (isClass[i] || parent[i] < 0 || !isClass[static_cast<usize>(parent[i])]) continue;
            const scene::Entity child = entity[i];
            const scene::Entity par = parentEntity(i, world);
            if (child == scene::kInvalidEntity || par == scene::kInvalidEntity || !world.valid(child)) continue;
            if (world.parent(child) == par) continue;
            if (world.setParent(child, par, false)) ++moved;
        }
        return moved;
    }
};

}  // namespace aver::game
