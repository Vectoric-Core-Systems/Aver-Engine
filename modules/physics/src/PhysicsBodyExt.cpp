// PhysicsBodyExt.cpp -- per-body extras (physics_body_ext_abi.h): locked axes, continuous collision,
// velocity caps, sleep permission, and turning an existing body into a sensor.
//
// Same shape as PhysicsBody.cpp beside it: resolve the handle, lock the body, change one thing. The
// non-obvious parts are the axis mapping for locked axes, keeping the mass through a mass-properties
// rebuild, and the broad-phase layer a sensor has to live on.
#include "aver/physics/physics_body_ext_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsRigidExtras.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <unordered_map>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/PhysicsSystem.h>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr uint32_t kAllLockBits = AVER_PHYS_LOCK_ALL_TRANSLATION | AVER_PHYS_LOCK_ALL_ROTATION;

// Mass of a body whose translation is fully locked: Jolt stores that as inverse mass 0, so the number is
// otherwise gone and unlocking could not restore it. Handles are never reused within a world; the table
// is emptied with the world (clearBodyExtState, called from destroyAllJoints).
std::unordered_map<int32_t, float> g_lockedMass;

int32_t refuse(AbiError e) {
    setAbiError(e);
    return 0;
}

// Engine axis -> the Jolt world axis it lies along, taken from Convert.hpp's own basis change rather
// than written out here. A rotation about an axis uses the same axis index (the sign an axial vector
// picks up does not move a lock from one axis to another).
struct AxisMap {
    int jolt[3];
    AxisMap() {
        for (int e = 0; e < 3; ++e) {
            const JPH::Vec3 j = toJoltUnit(Vec3(e == 0 ? 1.0f : 0.0f, e == 1 ? 1.0f : 0.0f, e == 2 ? 1.0f : 0.0f));
            jolt[e] = 0;
            for (int a = 0; a < 3; ++a)
                if (std::abs(j[a]) > 0.5f) jolt[e] = a;
        }
    }
};
const AxisMap& axisMap() {
    static const AxisMap m;
    return m;
}

// Locked (engine bits, set = frozen) to Jolt's allowed degrees of freedom (set = free), and back.
JPH::EAllowedDOFs toJoltDofs(uint32_t locked) {
    uint32_t allowed = kAllLockBits;
    for (int e = 0; e < 3; ++e) {
        const int j = axisMap().jolt[e];
        if (locked & (AVER_PHYS_LOCK_TRANSLATION_X << e)) allowed &= ~(1u << j);
        if (locked & (AVER_PHYS_LOCK_ROTATION_X << e))    allowed &= ~(8u << j);
    }
    return static_cast<JPH::EAllowedDOFs>(allowed);
}

uint32_t fromJoltDofs(JPH::EAllowedDOFs dofs) {
    const uint32_t allowed = static_cast<uint32_t>(dofs);
    uint32_t locked = 0;
    for (int e = 0; e < 3; ++e) {
        const int j = axisMap().jolt[e];
        if (!(allowed & (1u << j))) locked |= AVER_PHYS_LOCK_TRANSLATION_X << e;
        if (!(allowed & (8u << j))) locked |= AVER_PHYS_LOCK_ROTATION_X << e;
    }
    return locked;
}

// Runs `fn(motionProperties, body)` under a write lock, or fails: 0 for a dead handle, and Unsupported
// for a static body (Jolt gives it no motion properties at all).
template <typename Fn>
int32_t withMotion(int32_t handle, Fn&& fn) {
    const JPH::BodyID* id = findBody(handle);
    if (!id) return 0;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return refuse(AbiError::BadHandle);
    JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (!mp) return refuse(AbiError::Unsupported);
    return fn(*mp, lock.GetBody());
}

template <typename Fn>
int32_t readMotion(int32_t handle, Fn&& fn) {
    const JPH::BodyID* id = findBody(handle);
    if (!id) return 0;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return refuse(AbiError::BadHandle);
    const JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (!mp) return refuse(AbiError::Unsupported);
    return fn(*mp);
}

} // namespace

extern "C" {

// ---- locked axes -----------------------------------------------------------------------------------

int32_t aver_phys_body_set_locked_axes(int32_t body, uint32_t mask) {
    if (mask & ~kAllLockBits) {
        AVER_WARN("[Physics] locked-axes mask {:#x} has bits outside the six AVER_PHYS_LOCK_* flags", mask);
        return refuse(AbiError::InvalidArgument);
    }
    if (mask == kAllLockBits) {
        AVER_WARN("[Physics] body {}: locking all six axes is a static body; use aver_phys_body_set_motion_type", body);
        return refuse(AbiError::InvalidArgument);
    }
    return withMotion(body, [&](JPH::MotionProperties& mp, JPH::Body& b) -> int32_t {
        if (!b.IsDynamic()) return refuse(AbiError::Unsupported);
        JPH::MassProperties props = b.GetShape()->GetMassProperties();
        if (!(props.mMass > 0.0f)) return refuse(AbiError::Unsupported);   // a mesh or height field

        // The body's mass now: from the inverse mass, or from the table when translation is frozen.
        float massKg = props.mMass;
        if (mp.GetInverseMassUnchecked() > 0.0f) massKg = 1.0f / mp.GetInverseMassUnchecked();
        else if (const auto it = g_lockedMass.find(body); it != g_lockedMass.end()) massKg = it->second;
        props.ScaleToMass(massKg);

        mp.SetMassProperties(toJoltDofs(mask), props);
        // Inertia set by hand outlives the rebuild; a rotation that is locked still has no inertia to resist with.
        if (JPH::Vec3 fixedInverse; rigidExtrasInertiaOverride(body, fixedInverse)) {
            const uint32_t allowed = static_cast<uint32_t>(toJoltDofs(mask));
            for (int j = 0; j < 3; ++j)
                if (!(allowed & (8u << j))) fixedInverse.SetComponent(j, 0.0f);
            mp.SetInverseInertia(fixedInverse, JPH::Quat::sIdentity());
        }
        // Whatever speed the body had along a newly locked axis would otherwise sit in the velocity
        // until the next integration masks it.
        mp.SetLinearVelocity(mp.LockTranslation(mp.GetLinearVelocity()));
        mp.SetAngularVelocity(mp.LockAngular(mp.GetAngularVelocity()));

        if ((mask & AVER_PHYS_LOCK_ALL_TRANSLATION) == AVER_PHYS_LOCK_ALL_TRANSLATION) g_lockedMass[body] = massKg;
        else g_lockedMass.erase(body);
        return 1;
    });
}

int32_t aver_phys_body_locked_axes(int32_t body, uint32_t* outMask) {
    if (!outMask) return refuse(AbiError::NullPointer);
    return readMotion(body, [&](const JPH::MotionProperties& mp) -> int32_t {
        *outMask = fromJoltDofs(mp.GetAllowedDOFs());
        return 1;
    });
}

// ---- continuous collision --------------------------------------------------------------------------

int32_t aver_phys_body_set_ccd(int32_t body, int32_t on) {
    if (!readMotion(body, [](const JPH::MotionProperties&) -> int32_t { return 1; })) return 0;
    bi().SetMotionQuality(*findBody(body), on ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
    return 1;
}

int32_t aver_phys_body_ccd(int32_t body, int32_t* outOn) {
    if (!outOn) return refuse(AbiError::NullPointer);
    return readMotion(body, [&](const JPH::MotionProperties& mp) -> int32_t {
        *outOn = mp.GetMotionQuality() == JPH::EMotionQuality::LinearCast ? 1 : 0;
        return 1;
    });
}

// ---- velocity caps ---------------------------------------------------------------------------------

int32_t aver_phys_body_set_max_linear_velocity(int32_t body, float cmPerSecond) {
    if (!(cmPerSecond >= 0.0f) || !std::isfinite(cmPerSecond)) return refuse(AbiError::InvalidArgument);
    return withMotion(body, [&](JPH::MotionProperties& mp, JPH::Body&) -> int32_t {
        mp.SetMaxLinearVelocity(cmToM(cmPerSecond));
        mp.SetLinearVelocityClamped(mp.GetLinearVelocity());   // a body already faster is brought down now
        return 1;
    });
}

int32_t aver_phys_body_max_linear_velocity(int32_t body, float* outCmPerSecond) {
    if (!outCmPerSecond) return refuse(AbiError::NullPointer);
    return readMotion(body, [&](const JPH::MotionProperties& mp) -> int32_t {
        *outCmPerSecond = mToCm(mp.GetMaxLinearVelocity());
        return 1;
    });
}

int32_t aver_phys_body_set_max_angular_velocity(int32_t body, float degPerSecond) {
    if (!(degPerSecond >= 0.0f) || !std::isfinite(degPerSecond)) return refuse(AbiError::InvalidArgument);
    return withMotion(body, [&](JPH::MotionProperties& mp, JPH::Body&) -> int32_t {
        mp.SetMaxAngularVelocity(JPH::DegreesToRadians(degPerSecond));
        mp.SetAngularVelocityClamped(mp.GetAngularVelocity());
        return 1;
    });
}

int32_t aver_phys_body_max_angular_velocity(int32_t body, float* outDegPerSecond) {
    if (!outDegPerSecond) return refuse(AbiError::NullPointer);
    return readMotion(body, [&](const JPH::MotionProperties& mp) -> int32_t {
        *outDegPerSecond = JPH::RadiansToDegrees(mp.GetMaxAngularVelocity());
        return 1;
    });
}

// ---- sleeping --------------------------------------------------------------------------------------

int32_t aver_phys_body_set_allow_sleeping(int32_t body, int32_t on) {
    if (!withMotion(body, [&](JPH::MotionProperties&, JPH::Body& b) -> int32_t {
            b.SetAllowSleeping(on != 0);
            return 1;
        }))
        return 0;
    // A body that may never sleep but is asleep now would sit frozen until something touched it.
    if (!on) bi().ActivateBody(*findBody(body));
    return 1;
}

int32_t aver_phys_body_allow_sleeping(int32_t body, int32_t* outOn) {
    if (!outOn) return refuse(AbiError::NullPointer);
    return readMotion(body, [&](const JPH::MotionProperties& mp) -> int32_t {
        *outOn = mp.GetAllowSleeping() ? 1 : 0;
        return 1;
    });
}

// ---- sensor ----------------------------------------------------------------------------------------

int32_t aver_phys_body_set_sensor(int32_t body, int32_t on) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    const bool sensor = on != 0;
    bi().SetIsSensor(*id, sensor);
    // A sensor lives in the moving broad-phase half even when static (addBody does the same), so it is
    // told about the world; a solid body goes where its motion type says.
    const bool moving = sensor || bi().GetMotionType(*id) != JPH::EMotionType::Static;
    bi().SetObjectLayer(*id, Layers::encode(Layers::userOf(bi().GetObjectLayer(*id)), moving));
    // The contact listener reads this table from worker threads under the same mutex.
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    if (sensor) g_world->sensors[body] = true;
    else g_world->sensors.erase(body);
    return 1;
}

int32_t aver_phys_body_sensor(int32_t body, int32_t* outOn) {
    if (!outOn) return refuse(AbiError::NullPointer);
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    *outOn = bi().IsSensor(*id) ? 1 : 0;
    return 1;
}

} // extern "C"

namespace aver::physics::detail {
// Called from destroyAllJoints, which aver_phys_shutdown already runs before the world goes.
void clearBodyExtState() { g_lockedMass.clear(); }
}
