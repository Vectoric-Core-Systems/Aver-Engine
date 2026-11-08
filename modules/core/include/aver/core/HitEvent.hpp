#pragma once
// HitEvent: the neutral "something hit this entity" record. Whatever deals damage (a weapon, an explosion,
// a script, a game converted by Fabric mapping its own damage event) fills one in; the physical-animation
// layer (ragdoll hit reactions) and the destruction layer (fracture) both consume the same type, so a hit
// is described once. Plain data, no engine dependency, nothing keyed on a game.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

namespace aver {

struct HitEvent {
    u32  entity = 0;          // the entity that was hit (scene::Entity)
    u64  boneHash = 0;        // fnv1a64 of the bone name that was hit; 0 = the entity's root part
    Vec3 impulse{0, 0, 0};    // world space, kg*cm/s
    Vec3 point{0, 0, 0};      // world space, cm
    f32  damage = 0.0f;       // gameplay damage; the consumers that need a magnitude fall back to |impulse|
    f32  radius = 0.0f;       // > 0: a radial event (explosion) of this reach in cm around `point`; 0 = a point hit
};

} // namespace aver
