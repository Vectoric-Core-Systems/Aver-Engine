// PhysicsGodotFilter.hpp -- Godot-style collision filtering (G2): a 32-bit layer and mask per body, per-pair exceptions and joint
// exclusion. SRC-LOCAL like PhysicsInternal.hpp. The design is in docs/physics/GODOT_FILTERING.md.
#pragma once
#include "PhysicsInternal.hpp"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>

namespace aver::physics::detail {

// What a mask query compares its mask against: the body's Godot layer when it has a filter, else the bit of its 16-layer user layer
// (so a 32-bit mask can address unfiltered bodies too). Safe on workers; reads only.
uint32_t godotLayerBitsOf(const JPH::Body& body);

// Joint exclusion (PhysicsJoints.cpp): on = the two bodies stop colliding with each other, counted per joint so two joints on one
// pair need both removed. A body that cannot take a filter (ragdoll part, soft body) refuses. False also for a dead body.
bool godotJointExclusion(int32_t handleA, int32_t handleB, bool on);

// Called by aver_phys_remove_body before the body goes: drops its exceptions and frees its filter slot.
void godotOnBodyRemoved(int32_t handle, const JPH::BodyID& id);

// Skips one body and keeps only bodies whose layer bits meet the query's mask. Reusable by any query that takes a 32-bit Godot mask.
class GodotMaskBodyFilter final : public JPH::BodyFilter {
public:
    GodotMaskBodyFilter(uint32_t mask, const JPH::BodyID& ignore) : mask_(mask), ignore_(ignore) {}
    bool ShouldCollide(const JPH::BodyID& other) const override { return other != ignore_; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return (mask_ & godotLayerBitsOf(body)) != 0u; }
private:
    uint32_t mask_;
    JPH::BodyID ignore_;
};

} // namespace aver::physics::detail
