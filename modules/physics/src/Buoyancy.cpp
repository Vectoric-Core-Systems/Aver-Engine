// Buoyancy.cpp -- the whole of the physics is one call to Jolt's own ApplyBuoyancyImpulse; the rest
// of this file is bookkeeping. See Buoyancy.hpp for the reasoning behind the shape of this class.
#include "Buoyancy.hpp"

namespace aver::phys::water {

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
