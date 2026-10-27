// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// PlacementTreeTest: placed actors keep the hierarchy their level file gives them, across the two passes
// that create a level -- ordinary placements first, placed actors later. A bare scene::World; no device.

#include "aver/game/PlacementTree.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) AVER_INFO("   ok    {}", what);
    else { ++g_failures; AVER_ERROR("   FAIL  {}", what); }
}

int main() {
    AVER_INFO("PlacementTree test");
    scene::World& world = scene::World::instance();

    // 0 mesh root > 1 actor > {2 mesh, 3 actor}; 4 actor root.
    fmt::OcWorldData w;
    w.placements.resize(5);
    w.placements[1].className = "Door";   w.placements[1].parent = 0;
    w.placements[2].parent = 1;
    w.placements[3].className = "Handle"; w.placements[3].parent = 1;
    w.placements[4].className = "Spawner";

    // What world::instantiate makes: the ordinary placements only, the child of an actor as a root.
    const scene::Entity m0 = world.create("mesh0");
    const scene::Entity m2 = world.create("mesh2");
    world::LevelInstance inst;
    inst.entities = {m0, m2};
    inst.placementIndex = {0, 2};

    game::PlacementTree tree;
    tree.build(w, inst);
    check(tree.parentEntity(1, world) == m0, "an actor under an ordinary placement finds its entity");
    check(tree.parentEntity(3, world) == scene::kInvalidEntity, "an actor under an actor not yet spawned finds none");
    check(tree.parentEntity(4, world) == scene::kInvalidEntity, "a root actor has no parent");

    // The host's pass, in file order: each actor spawns, joins its parent, and is recorded.
    const scene::Entity door = world.create("door");
    world.setParent(door, tree.parentEntity(1, world), false);
    tree.setEntity(1, door);
    check(tree.parentEntity(3, world) == door, "an actor under an actor spawned earlier finds it");
    const scene::Entity handle = world.create("handle");
    world.setParent(handle, tree.parentEntity(3, world), false);
    tree.setEntity(3, handle);

    check(world.parent(m2) == scene::kInvalidEntity, "before the attach, the mesh under the door is a root");
    check(tree.attachUnderClassPlacements(world) == 1, "one ordinary placement moves under its actor");
    check(world.parent(m2) == door, "the mesh is now the door's child");
    check(world.parent(handle) == door && world.parent(door) == m0, "the actors keep their parents");
    check(tree.attachUnderClassPlacements(world) == 0, "a second attach moves nothing");

    world.destroy(door);
    world.flush();
    check(tree.parentEntity(3, world) == scene::kInvalidEntity, "a parent that is gone is no parent");

    for (const scene::Entity e : {m0}) world.destroy(e);
    world.flush();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
