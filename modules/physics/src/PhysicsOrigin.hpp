// PhysicsOrigin.hpp -- the floating origin's physics half: the shift driver's seams and the engine-side hook registry
// (LARGE_WORLD 2.3; the ABI is physics_origin_abi.h, the driver PhysicsOrigin.cpp).
//
// SRC-LOCAL like PhysicsInternal.hpp: it names JPH:: types.
#pragma once
#include "PhysicsInternal.hpp"

#include <cstdint>
#include <vector>

namespace aver::physics::detail {

// A delta in the engine's absolute centimetres. Physics has no dependency on Aver.World, so the hook argument is this local type
// and not world::Vec3d.
struct Vec3d { double x = 0.0, y = 0.0, z = 0.0; };

// What the last shift did (aver_phys_origin_stats). The joint seam below fills anchors and pulleysRebuilt.
struct OriginStats {
    float   ms = 0.0f;
    int32_t bodies = 0, characters = 0, anchors = 0, pulleysRebuilt = 0, staticsMoved = 0;
};

// ---- Engine-side state that holds a world position ---------------------------------------------------------------------------
//
// Any file in this module that keeps an absolute position across steps (a force or water volume, a soft-body attachment with a
// world target, hair's previous root pose) registers one of these at init or static-init time. The shift calls every hook once,
// AFTER the bodies and characters have moved, in registration order, with `deltaCm` = the origin's move (new minus old): the
// state the hook owns changes by -deltaCm. Process-wide, so it survives aver_phys_shutdown and the next aver_phys_init; a second
// registration under the same `name` replaces the first. A hook must not call back into the shift. The pending contact,
// vehicle-impact and vehicle-event points are shifted by the driver itself and need no hook.
using OriginHookFn = void (*)(const Vec3d& deltaCm);
void physicsOriginHook(const char* name, OriginHookFn fn);

// ---- The joint seam (PhysicsJoints.cpp) -------------------------------------------------------------------------------------
//
// A joint with a world endpoint keeps its anchor in absolute coordinates, and a pulley keeps two absolute fixed points. A world joint
// is built against an unadded static ANCHOR body that the shift moves like any body, and a pulley is rebuilt from its own settings.
//
//   originJointsBegin  -- before ANY body moves: captures the settings of every pulley.
//   originJointsCount  -- how many anchor bodies a shift would move (aver_phys_origin_prepare's count).
//   originJointsShift  -- after the bodies and characters moved: anchors by -dj, pulleys rebuilt with their fixed points reduced by
//                         dj (same handle, noteConstraintTopology()); fills stats.anchors and stats.pulleysRebuilt.
void    originJointsBegin();
int32_t originJointsCount();
void    originJointsShift(const JPH::Vec3& dj, OriginStats& stats);

// ---- Snapshots (PhysicsState.cpp) ---------------------------------------------------------------------------------------------
//
// A restored snapshot comes back in the frame it was taken in. After the restore this moves exactly what the blob restored (the bodies
// of its manifest and the characters it held, never an anchor and never a static the blob did not hold) by -deltaCm, the live
// origin total minus the blob's. The registered hooks do NOT run: the state they own is not in a blob and is already in the live frame.
// Z is ignored (it is never shifted). A delta of zero, or one that is not a whole number of metres, does nothing.
void originReshiftRestored(const std::vector<uint32_t>& bodyIds, const std::vector<int32_t>& characterHandles, const Vec3d& deltaCm);

// The last shift's numbers for the live world; zeros before the first shift of this world.
const OriginStats& lastOriginStats();

} // namespace aver::physics::detail
