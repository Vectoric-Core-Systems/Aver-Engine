// Buoyancy.cpp -- the whole of the physics is one call to Jolt's own ApplyBuoyancyImpulse; the rest
// of this file is bookkeeping. See Buoyancy.hpp for the reasoning behind the shape of this class.
#include "Buoyancy.hpp"

#include <Jolt/Physics/Body/BodyType.h>

namespace aver::phys::water {

namespace {

// SOFT BODIES ARE NOT BUOYANT, and asking Jolt to make one so is not a soft failure -- it is
// `JPH_ASSERT(IsRigidBody())` at the top of Body::ApplyBuoyancyImpulse, which is a hard stop in a
// debug build. Jolt says why in the line right beside it: buoyancy is "Only implemented for rigid
// bodies currently". Nothing in this engine created a soft body inside a water plane until fluid
// volumes arrived, and a fluid volume is the worst possible case -- it sits BELOW its own surface
// by construction, so it is fully submerged from its first step and hits the assert immediately.
//
// Skipped rather than clamped, because there is no sensible impulse to apply: a soft body's shape
// is its particles, GetSubmergedVolume is a rigid-shape query, and a volume of water floating on
// itself is not a thing the plane is describing in the first place.
bool buoyant(const JPH::BodyInterface& bi, const JPH::BodyID& id) {
    return bi.GetBodyType(id) == JPH::EBodyType::RigidBody;
}

} // namespace

void WaterVolumeTable::set(int32_t bodyHandle, const WaterVolume& volume) {
    volumes_[bodyHandle] = volume;
}

void WaterVolumeTable::clear(int32_t bodyHandle) {
    volumes_.erase(bodyHandle);
}

void WaterVolumeTable::clearAll() {
    volumes_.clear();
}

size_t WaterVolumeTable::count() const {
    return volumes_.size();
}

void WaterVolumeTable::setPlane(const WaterVolume& plane) {
    plane_ = plane;
    hasPlane_ = true;
}

void WaterVolumeTable::clearPlane() {
    hasPlane_ = false;
}

bool WaterVolumeTable::hasPlane() const {
    return hasPlane_;
}

const WaterVolume& WaterVolumeTable::plane() const {
    return plane_;
}

int32_t WaterVolumeTable::lastAppliedCount() const {
    return lastApplied_;
}

int32_t WaterVolumeTable::evaluate(
        JPH::PhysicsSystem& system,
        const std::function<const JPH::BodyID*(int32_t)>& findBody,
        const std::function<void(const std::function<void(int32_t, const JPH::BodyID&)>&)>& forEachBody,
        JPH::Vec3Arg gravity, float dt) const {
    JPH::BodyInterface& bi = system.GetBodyInterface();
    int32_t applied = 0;

    // Per-body overrides first, each against its own registered plane. The return value is
    // intentionally ignored beyond counting it: false just means "no impulse this substep" (the body
    // is above the surface, or static, or asleep with nothing to wake it) -- not an error, per the
    // Jolt source this module was scouted against.
    for (const auto& [handle, v] : volumes_) {
        const JPH::BodyID* id = findBody(handle);
        if (!id) continue;   // a stale or since-removed handle drops out silently, by design
        // Checked on the override path too, not just the plane below: registering a water volume
        // against a soft body's handle is a thing an ABI caller can do, and it would assert in
        // exactly the same place.
        if (!buoyant(bi, *id)) continue;
        if (bi.ApplyBuoyancyImpulse(*id, v.surfacePosition, v.surfaceNormal, v.buoyancy,
                                     v.linearDrag, v.angularDrag, v.fluidVelocity, gravity, dt))
            ++applied;
    }

    // Then the global plane, for every body forEachBody reports EXCEPT one already handled above --
    // "a per-body volume overrides the plane for that body" is enforced right here, by checking the
    // ENGINE HANDLE against volumes_ rather than the BodyID, which is exactly why forEachBody hands
    // back the handle: a plain map lookup, where checking by BodyID would need a second, reverse
    // table this file has no other reason to keep. Buoyancy <= 0 is the documented way to disable the
    // plane without forgetting its height/normal/drag (see aver_phys_set_water_plane), checked once
    // here instead of by every caller of setPlane().
    if (hasPlane_ && plane_.buoyancy > 0.0f && forEachBody) {
        forEachBody([&](int32_t handle, const JPH::BodyID& id) {
            if (volumes_.count(handle) != 0) return;   // this body already has its own water
            if (!buoyant(bi, id)) return;
            if (bi.ApplyBuoyancyImpulse(id, plane_.surfacePosition, plane_.surfaceNormal,
                                         plane_.buoyancy, plane_.linearDrag, plane_.angularDrag,
                                         plane_.fluidVelocity, gravity, dt))
                ++applied;
        });
    }

    lastApplied_ = applied;
    return applied;
}

WaterVolumeTable& waterVolumes() {
    static WaterVolumeTable table;
    return table;
}

} // namespace aver::phys::water
