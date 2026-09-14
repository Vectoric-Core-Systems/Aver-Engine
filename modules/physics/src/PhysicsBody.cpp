// PhysicsBody.cpp -- the body-dynamics half of the ABI: motion type, spin, forces, impulses, the
// material properties, mass and sleeping.
//
// ITS OWN TRANSLATION UNIT for the reason Buoyancy.cpp already is one: PhysicsWorld.cpp is the world's
// lifetime and the step loop, and this is a flat sheet of per-body setters that share nothing with it
// but the handle tables. Splitting them keeps the file that owns the world small enough to read.
//
// EVERY FUNCTION HERE IS THE SAME FOUR LINES -- resolve the handle, refuse 0 if it is dead, convert
// the argument out of engine units, call Jolt -- so what is worth commenting is only the places where
// that shape is NOT enough: the axial-vector sign, the two mass paths, and the lock that the
// properties without a BodyInterface setter need.
#include "aver/physics/physics_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/PhysicsSystem.h>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// THE MOTION PROPERTIES, OR NULL -- and null is the ordinary answer, not an error.
//
// A static body genuinely has none: Jolt models infinite mass by the absence of the struct rather than
// by a big number in it, so friction on a floor and damping on a wall are not "not implemented", they
// are not meaningful. Every setter below that needs this returns 0 for such a body, which the header
// documents as the dead-handle answer too -- a caller that cares which it was has the motion type.
//
// WRITE-LOCKED, because MotionProperties is body state and the physics job threads read it. This is
// the same JPH::BodyLockWrite pattern aver_phys_set_entity already uses in PhysicsWorld.cpp; the
// BodyInterface setters used elsewhere in this file take that lock internally, and these paths have no
// BodyInterface equivalent to take it for them.
//
// GetMotionPropertiesUnchecked, NOT GetMotionProperties. The plain accessor is
// `{ JPH_ASSERT(!IsStatic()); return mMotionProperties; }` (Body.h:325-326) -- ASKING a static body
// for its motion properties is the assertion failure, not a way to find out that it has none. The
// null test below then "worked" only because asserts compile out of a Release build and the pointer
// really is null there, so every Release run agreed with this function's own comment while a Debug
// run died on the first call. Jolt ships the Unchecked pair for exactly this question.
template <typename Fn>
int32_t withMotionProperties(int32_t handle, Fn&& fn) {
    const JPH::BodyID* id = findBody(handle);
    if (!id) return 0;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return 0;
    JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    if (!mp) return 0;
    fn(*mp, lock.GetBody());
    return 1;
}

// A body's id, or nothing. The one-liner every setter below starts with.
#define AVER_PHYS_BODY_OR_ZERO(h)                    \
    const JPH::BodyID* id = findBody(h);             \
    if (!id) return 0

} // namespace

extern "C" {

// ---- motion type -----------------------------------------------------------------------------------

int32_t aver_phys_body_set_motion_type(int32_t body, int32_t motionType) {
    AVER_PHYS_BODY_OR_ZERO(body);
    JPH::EMotionType t;
    switch (motionType) {
        case AVER_PHYS_MOTION_STATIC:    t = JPH::EMotionType::Static;    break;
        case AVER_PHYS_MOTION_KINEMATIC: t = JPH::EMotionType::Kinematic; break;
        case AVER_PHYS_MOTION_DYNAMIC:   t = JPH::EMotionType::Dynamic;   break;
        default:
            AVER_WARN("[Physics] motion type {} is not one of static(0)/kinematic(1)/dynamic(2)",
                      motionType);
            return 0;
    }
    // ACTIVATE FOR ANYTHING THAT MOVES. A body switched to dynamic or kinematic while asleep would
    // otherwise stay exactly where it was until something else touched it, which reads as the call
    // having done nothing at all.
    bi().SetMotionType(*id, t,
                       t == JPH::EMotionType::Static ? JPH::EActivation::DontActivate
                                                     : JPH::EActivation::Activate);
    return 1;
}

int32_t aver_phys_body_motion_type(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return -1;   // not 0: 0 is STATIC, a real answer
    switch (bi().GetMotionType(*id)) {
        case JPH::EMotionType::Static:    return AVER_PHYS_MOTION_STATIC;
        case JPH::EMotionType::Kinematic: return AVER_PHYS_MOTION_KINEMATIC;
        case JPH::EMotionType::Dynamic:   return AVER_PHYS_MOTION_DYNAMIC;
    }
    return -1;
}

// ---- transform and velocity ------------------------------------------------------------------------

int32_t aver_phys_body_set_rotation(int32_t body, float x, float y, float z, float w) {
    AVER_PHYS_BODY_OR_ZERO(body);
    // NORMALISED FIRST. Jolt asserts on a non-unit quaternion in a debug build and misbehaves quietly
    // in a release one, and a caller accumulating small rotations frame over frame drifts off unit
    // without ever doing anything obviously wrong.
    const Quat q = Quat(x, y, z, w).normalized();
    bi().SetRotation(*id, toJolt(q), JPH::EActivation::Activate);
    return 1;
}

int32_t aver_phys_body_angular_velocity(int32_t body, float* outXyz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    if (!outXyz) return 0;
    writeVec(outXyz, fromJoltAngular(bi().GetAngularVelocity(*id)));
    return 1;
}

int32_t aver_phys_body_set_angular_velocity(int32_t body, float wx, float wy, float wz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    // toJoltAngular, NOT toJoltDir: see Convert.hpp. An angular velocity is axial and flips sign.
    bi().SetAngularVelocity(*id, toJoltAngular(Vec3(wx, wy, wz)));
    return 1;
}

int32_t aver_phys_body_add_velocity(int32_t body, float vx, float vy, float vz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().AddLinearVelocity(*id, toJoltDir(Vec3(vx, vy, vz)));
    return 1;
}

// ---- forces and impulses ---------------------------------------------------------------------------

int32_t aver_phys_body_add_force(int32_t body, float fx, float fy, float fz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().AddForce(*id, toJoltDir(Vec3(fx, fy, fz)), JPH::EActivation::Activate);
    return 1;
}

int32_t aver_phys_body_add_force_at(int32_t body, float fx, float fy, float fz,
                                    float px, float py, float pz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    // The force converts as a direction and the point as a POSITION -- two different converters, and
    // the only place in this file where one call needs both.
    bi().AddForce(*id, toJoltDir(Vec3(fx, fy, fz)), toJolt(Vec3(px, py, pz)),
                  JPH::EActivation::Activate);
    return 1;
}

int32_t aver_phys_body_add_torque(int32_t body, float tx, float ty, float tz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().AddTorque(*id, toJoltTorque(Vec3(tx, ty, tz)), JPH::EActivation::Activate);
    return 1;
}

int32_t aver_phys_body_add_impulse(int32_t body, float ix, float iy, float iz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().AddImpulse(*id, toJoltDir(Vec3(ix, iy, iz)));
    return 1;
}

int32_t aver_phys_body_add_impulse_at(int32_t body, float ix, float iy, float iz,
                                      float px, float py, float pz) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().AddImpulse(*id, toJoltDir(Vec3(ix, iy, iz)), toJolt(Vec3(px, py, pz)));
    return 1;
}

int32_t aver_phys_body_add_angular_impulse(int32_t body, float ax, float ay, float az) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().AddAngularImpulse(*id, toJoltTorque(Vec3(ax, ay, az)));
    return 1;
}

// ---- material --------------------------------------------------------------------------------------
// Friction and restitution live on the BODY, not on MotionProperties, so a STATIC body has them --
// which is exactly right, since a frictionless floor is a thing a game wants and a floor is static.

int32_t aver_phys_body_set_friction(int32_t body, float friction) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().SetFriction(*id, friction);
    return 1;
}

int32_t aver_phys_body_friction(int32_t body, float* outFriction) {
    AVER_PHYS_BODY_OR_ZERO(body);
    if (!outFriction) return 0;
    *outFriction = bi().GetFriction(*id);
    return 1;
}

int32_t aver_phys_body_set_restitution(int32_t body, float restitution) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().SetRestitution(*id, restitution);
    return 1;
}

int32_t aver_phys_body_restitution(int32_t body, float* outRestitution) {
    AVER_PHYS_BODY_OR_ZERO(body);
    if (!outRestitution) return 0;
    *outRestitution = bi().GetRestitution(*id);
    return 1;
}

int32_t aver_phys_body_set_gravity_factor(int32_t body, float factor) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().SetGravityFactor(*id, factor);
    return 1;
}

int32_t aver_phys_body_gravity_factor(int32_t body, float* outFactor) {
    AVER_PHYS_BODY_OR_ZERO(body);
    if (!outFactor) return 0;
    *outFactor = bi().GetGravityFactor(*id);
    return 1;
}

// Damping has no BodyInterface setter, so it goes through the lock. Non-negative on the way in: Jolt
// integrates `v *= max(0, 1 - damping*dt)`, and a negative damping is an energy source that grows a
// body's speed without bound -- clamped rather than refused, because a caller sweeping a slider
// through zero should not have half its range silently do nothing.
int32_t aver_phys_body_set_damping(int32_t body, float linear, float angular) {
    const float l = linear  < 0.0f ? 0.0f : linear;
    const float a = angular < 0.0f ? 0.0f : angular;
    return withMotionProperties(body, [&](JPH::MotionProperties& mp, JPH::Body&) {
        mp.SetLinearDamping(l);
        mp.SetAngularDamping(a);
    });
}

int32_t aver_phys_body_damping(int32_t body, float* outLinear, float* outAngular) {
    if (!outLinear || !outAngular) return 0;
    return withMotionProperties(body, [&](const JPH::MotionProperties& mp, JPH::Body&) {
        *outLinear  = mp.GetLinearDamping();
        *outAngular = mp.GetAngularDamping();
    });
}

// ---- mass ------------------------------------------------------------------------------------------

// SCALES THE EXISTING INERTIA RATHER THAN REPLACING IT. MotionProperties stores INVERSE mass and an
// inverse inertia diagonal, both derived from the shape at creation; setting mass alone would leave a
// body that weighs 10kg and still resists rotation like the 1kg one it was built as. Rescaling by the
// ratio keeps the shape's own distribution and only changes how much of it there is, which is what
// "this crate is heavier" means.
int32_t aver_phys_body_set_mass(int32_t body, float massKg) {
    if (massKg <= 0.0f) {
        AVER_WARN("[Physics] mass must be positive; {} kg refused", massKg);
        return 0;
    }
    return withMotionProperties(body, [&](JPH::MotionProperties& mp, JPH::Body&) {
        const float oldMass = mp.GetInverseMass() > 0.0f ? 1.0f / mp.GetInverseMass() : 0.0f;
        mp.SetInverseMass(1.0f / massKg);
        if (oldMass > 0.0f) {
            const float ratio = oldMass / massKg;   // inverse inertia scales the OTHER way
            mp.SetInverseInertia(mp.GetInverseInertiaDiagonal() * ratio,
                                 mp.GetInertiaRotation());
        }
    });
}

int32_t aver_phys_body_mass(int32_t body, float* outMassKg) {
    if (!outMassKg) return 0;
    return withMotionProperties(body, [&](const JPH::MotionProperties& mp, JPH::Body&) {
        const float inv = mp.GetInverseMass();
        *outMassKg = inv > 0.0f ? 1.0f / inv : 0.0f;
    });
}

// ---- sleeping --------------------------------------------------------------------------------------

int32_t aver_phys_body_activate(int32_t body) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().ActivateBody(*id);
    return 1;
}

int32_t aver_phys_body_deactivate(int32_t body) {
    AVER_PHYS_BODY_OR_ZERO(body);
    bi().DeactivateBody(*id);
    return 1;
}

int32_t aver_phys_body_is_active(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    return bi().IsActive(*id) ? 1 : 0;
}

} // extern "C"
