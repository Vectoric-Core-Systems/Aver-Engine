// PhysicsLayerFilter.hpp -- 32-bit layer/mask collision filtering: a layer and mask per body, per-pair exceptions and joint
// exclusion. SRC-LOCAL like PhysicsInternal.hpp. The design is in docs/physics/LAYER_FILTERING.md.
#pragma once
#include "PhysicsInternal.hpp"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>

namespace aver::physics::detail {

// What a mask query compares its mask against: the body's 32-bit layer when it has a filter, else the bit of its 16-layer user layer
// (so a 32-bit mask can address unfiltered bodies too). Safe on workers; reads only.
uint32_t layerBitsOfBody(const JPH::Body& body);

// Joint exclusion (PhysicsJoints.cpp): on = the two bodies stop colliding with each other, counted per joint so two joints on one
// pair need both removed. A body that cannot take a filter (ragdoll part, soft body) refuses. False also for a dead body.
bool layerJointExclusion(int32_t handleA, int32_t handleB, bool on);

// Called by aver_phys_remove_body before the body goes: drops its exceptions and frees its filter slot.
void layerFilterOnBodyRemoved(int32_t handle, const JPH::BodyID& id);

// Skips one body and keeps only bodies whose layer bits meet the query's mask. Reusable by any query that takes a 32-bit mask.
class LayerMaskBodyFilter final : public JPH::BodyFilter {
public:
    LayerMaskBodyFilter(uint32_t mask, const JPH::BodyID& ignore) : mask_(mask), ignore_(ignore) {}
    bool ShouldCollide(const JPH::BodyID& other) const override { return other != ignore_; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return (mask_ & layerBitsOfBody(body)) != 0u; }
private:
    uint32_t mask_;
    JPH::BodyID ignore_;
};

} // namespace aver::physics::detail
