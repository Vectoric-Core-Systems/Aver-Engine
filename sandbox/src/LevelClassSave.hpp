#pragma once
// Writing a level's CLASS placements back out, with any edit the user made to them.
//
// WHY THIS IS ITS OWN HEADER RATHER THAN A LOOP INSIDE saveLevel. It is the only part of the level
// save that had a bug you could not see: a class placement's line was written from the copy read off
// disk, so moving a placed graph class in the viewport and saving discarded the move silently. A loop
// buried in a 20,000-line executable cannot be tested, and SandboxApp is add_executable-only. This is
// header-only and depends on Core + Formats and nothing else -- the same trade EditorTransform.hpp
// makes for the same reason -- so a test can include it directly and drive it with a fake lookup.
//
// THE TWO THINGS IT EXISTS TO GET RIGHT, both of which are silent when wrong:
//
//   1. Placements and live instances are NOT index-parallel. spawnClassPlacements skips a class it
//      cannot resolve without recording anything, so the Nth entity is not the Nth placement whenever
//      any class failed -- which is the normal state of a level opened before its project's scripts
//      are compiled. Pairing by position would write one placement's transform onto another's line.
//
//   2. `snap` makes z an OFFSET ABOVE THE GROUND, not an absolute height. The live entity's z is the
//      resolved world height with the terrain already added, so writing it back would bake the ground
//      in and the placement would climb by that much on every save/load cycle.
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
// eulerDegFromQuat, so this file and every other level path agree on what "yaw 90" means.
#include "aver/world/LevelTransform.hpp"

#include <cstddef>
#include <vector>

namespace aver::editor {

// One live instance and the placement it was spawned from. `entity` is aver_fw_spawn's handle, which
// IS a scene entity (framework_abi.h:74: "a scene entity; 0 invalid").
struct LevelClassInstance {
    std::size_t placement = 0;
    int32_t     entity    = 0;
};

// Rebuilds `authored` into `out`, replacing each placement's transform with its live entity's when
// one exists. `transformOf(entity, xf)` returns false for an entity that is gone or has no transform,
// in which case that placement is written exactly as it was read.
//
// Appends to `out` rather than replacing it: the caller has already put every ordinary mesh placement
// there, and these join the same list.
template <typename TransformOf>
void appendClassPlacements(const std::vector<fmt::OcWorldPlacement>& authored,
                           const std::vector<LevelClassInstance>& live,
                           TransformOf&& transformOf,
                           std::vector<fmt::OcWorldPlacement>& out) {
    std::vector<const LevelClassInstance*> byPlacement(authored.size(), nullptr);
    for (const LevelClassInstance& li : live)
        if (li.placement < byPlacement.size()) byPlacement[li.placement] = &li;

    for (std::size_t i = 0; i < authored.size(); ++i) {
        fmt::OcWorldPlacement p = authored[i];
        Transform xf;
        if (byPlacement[i] && transformOf(byPlacement[i]->entity, xf)) {
            p.x = xf.position.x;
            p.y = xf.position.y;
            // See (2) above: a snapped placement keeps its authored offset and moves only in x/y.
            if (!p.snapToGround) p.z = xf.position.z;
            const Vec3 euler = world::eulerDegFromQuat(xf.rotation);
            p.roll = euler.x; p.pitch = euler.y; p.yaw = euler.z;
            p.sx = xf.scale.x; p.sy = xf.scale.y; p.sz = xf.scale.z;
        }
        out.push_back(std::move(p));
    }
}

} // namespace aver::editor
