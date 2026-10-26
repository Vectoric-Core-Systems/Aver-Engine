#pragma once
// A placement record's position from a live entity, under the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.6.2).
//
// A record is ABSOLUTE f64 text; the entity's CLocal is origin-relative f32. A root's record is therefore `origin + local`; a child's record is
// parent-relative and has no origin in it. A streamed root additionally keeps its authored f64 on every axis the author did not move (the
// untouched-record rule: narrowing to f32 and widening back is not an identity), which LevelStreaming answers from the baseline it took when
// the entity loaded and rebased at every shift. At a zero origin a root that is not streamed gets `local` widened, exactly what it always got.
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelStreaming.hpp"
#include "aver/world/PlacementWriteBack.hpp"

#if AVER_MODULE_SCENE

namespace aver::editor {

// Sets p.x, p.y, p.z for entity `e` whose CLocal position is `local`. `isRoot` is "no parent entity" (the record is not parent-relative).
// `useBaseline` false when `local` is not the entity's CLocal (a reparented entity written at its world pose), so no baseline applies.
inline void writeRecordPosition(fmt::OcWorldPlacement& p, const Vec3& local, world::OriginCm origin, const game::LevelStreaming& ls,
                                scene::Entity e, bool isRoot, bool useBaseline = true) {
    if (isRoot) {
        world::Vec3d cm;
        if (useBaseline && ls.active() && ls.recordPositionOf(e, local, cm)) { p.x = cm.x; p.y = cm.y; p.z = cm.z; return; }
        if (origin.x != 0 || origin.y != 0) {
            p.x = world::quantiseCm(static_cast<f64>(origin.x) + static_cast<f64>(local.x));
            p.y = world::quantiseCm(static_cast<f64>(origin.y) + static_cast<f64>(local.y));
            p.z = local.z;
            return;
        }
    }
    p.x = local.x; p.y = local.y; p.z = local.z;
}

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
