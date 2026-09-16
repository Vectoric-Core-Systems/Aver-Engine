// Placing player controller 0's possessed pawn, shared by the editor's Play Start and the
// runtime's spawn -- each host supplies its own position/yaw and logs its own line; this is only
// the pawn lookup and the World writes.
#pragma once
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
#include "aver/scene/World.hpp"
#include "aver/world/LevelTransform.hpp"
#include "aver/framework/framework_abi.h"

namespace aver::game {

// pn = aver_fw_controlled_pawn(aver_fw_player_controller(0)); false when there is no controller 0,
// no possessed pawn, or the pawn id is not a valid World entity. Otherwise sets the pawn's local
// position and yaw and returns true. No log -- each host logs its own line after this succeeds.
inline bool placePossessedPawn(const Vec3& position, f32 yawDeg) {
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (!pn) return false;
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& w = scene::World::instance();
    if (!w.valid(pe)) return false;
    w.setLocalPosition(pe, position);
    w.setLocalRotation(pe, world::quatFromEulerDeg(Vec3{0.0f, 0.0f, yawDeg}));
    return true;
}

} // namespace aver::game
#endif
