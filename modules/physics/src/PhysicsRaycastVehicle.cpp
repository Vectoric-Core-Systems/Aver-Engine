// PhysicsRaycastVehicle.cpp -- the raycast vehicle: a dynamic chassis plus wheels that are rays, not bodies. The ABI is
// physics_raycast_vehicle_abi.h; the model and its limits are in docs/physics/RAYCAST_VEHICLE.md.
// Written from the behaviour of a raycast-vehicle model (spring and damper at the contact, friction impulses limited by friction_slip, side force at a
// roll-reduced height); no code from Bullet or any other engine. All the arithmetic is in Jolt's SI space (metres, right-handed): the parameters are converted when
// a step reads them, so the ABI keeps the engine's centimetres. Wheels are walked in handle order, so a replay is exact.
#include "aver/physics/physics_raycast_vehicle_abi.h"
#include "Convert.hpp"
#include "PhysicsLayerFilter.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsQueryFilters.hpp"
#include "PhysicsRaycastVehicle.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace aver::physics::detail {

namespace {
constexpr int   kMaxWheels = 64;
constexpr float kCmToM = 0.01f;
constexpr float kTau = 6.28318530718f;
constexpr float kMinGroundDot = 0.1f;   // a contact whose normal is within ~84 degrees of sideways stops scaling the spring
}  // namespace

struct RayWheel {
    float    p[AVER_PHYS_RAYW_COUNT] = {};
    uint32_t mask = 0xFFFFFFFFu;
    float    engine = 0.0f, brake = 0.0f, steer = 0.0f;   // inputs: kg*cm/s^2, kg*cm/s^2, rad
    // Results of the last step, SI: metres, newtons, m/s.
    bool       contact = false;
    JPH::Vec3  point = JPH::Vec3::sZero(), normal = JPH::Vec3(0, 1, 0);
    float      suspLen = 0.0f, suspForce = 0.0f, skid = 1.0f, spin = 0.0f, rotation = 0.0f;
    float      fwdSpeed = 0.0f, sideSpeed = 0.0f, steerApplied = 0.0f;
    int32_t    groundBody = 0;
};

struct RayVeh {
    int32_t              chassis = 0;
    std::vector<RayWheel> wheels;
};

struct RayVehicleState {
    std::map<int32_t, RayVeh>            vehicles;   // ordered: the step walks it in handle order
    std::unordered_map<int32_t, int32_t> ofBody;     // chassis body handle -> vehicle handle
    int32_t                              next = 1;
};

namespace {

void setDefaults(RayWheel& w) {
    float* p = w.p;
    for (float& f : w.p) f = 0.0f;
    p[AVER_PHYS_RAYW_DIR_Z] = -1.0f;
    p[AVER_PHYS_RAYW_AXLE_Y] = 1.0f;
    p[AVER_PHYS_RAYW_REST_LENGTH] = 30.0f;
    p[AVER_PHYS_RAYW_MAX_TRAVEL] = 30.0f;
    p[AVER_PHYS_RAYW_STIFFNESS] = 20.0f;
    p[AVER_PHYS_RAYW_DAMP_COMPRESSION] = 2.0f;
    p[AVER_PHYS_RAYW_DAMP_RELAXATION] = 3.0f;
    p[AVER_PHYS_RAYW_RADIUS] = 35.0f;
    p[AVER_PHYS_RAYW_FRICTION_SLIP] = 1.0f;
    p[AVER_PHYS_RAYW_ROLL_INFLUENCE] = 1.0f;
    p[AVER_PHYS_RAYW_TRACTION] = 1.0f;
    w.suspLen = p[AVER_PHYS_RAYW_REST_LENGTH] * kCmToM;
}

// A key's legal values; false = refuse. Vector components and the connection point are free (a zero DIR or AXLE is caught as a whole).
bool validParam(int32_t key, float v) {
    if (!std::isfinite(v)) return false;
    switch (key) {
        case AVER_PHYS_RAYW_RADIUS: return v > 1e-3f;
        case AVER_PHYS_RAYW_REST_LENGTH: case AVER_PHYS_RAYW_MAX_TRAVEL: case AVER_PHYS_RAYW_STIFFNESS:
        case AVER_PHYS_RAYW_DAMP_COMPRESSION: case AVER_PHYS_RAYW_DAMP_RELAXATION: case AVER_PHYS_RAYW_MAX_FORCE:
        case AVER_PHYS_RAYW_FRICTION_SLIP: case AVER_PHYS_RAYW_ROLL_INFLUENCE: return v >= 0.0f;
        default: return true;
    }
}

float len3(const float* p) { return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); }

JPH::Vec3 gravityUp() {
    const JPH::Vec3 g = g_world->system.GetGravity();
    return g.LengthSq() > 1e-12f ? (-g).Normalized() : JPH::Vec3(0, 1, 0);
}

// ---- reading bodies ----------------------------------------------------------------------------------

struct Dyn {
    bool      valid = false, dynamic = false, active = false;
    float     invM = 0.0f;
    JPH::Mat44 invI = JPH::Mat44::sZero();
    JPH::Vec3 com = JPH::Vec3::sZero(), origin = JPH::Vec3::sZero(), lin = JPH::Vec3::sZero(), ang = JPH::Vec3::sZero();
    JPH::Quat rot = JPH::Quat::sIdentity();
};

void fillDyn(const JPH::Body& b, Dyn& d) {
    d.valid = true;
    d.com = JPH::Vec3(b.GetCenterOfMassPosition());
    d.origin = JPH::Vec3(b.GetPosition());
    d.rot = b.GetRotation();
    d.lin = b.GetLinearVelocity();
    d.ang = b.GetAngularVelocity();
    if (b.IsDynamic() && !b.IsSensor() && b.GetMotionPropertiesUnchecked()) {
        d.dynamic = true;
        d.active = b.IsActive();
        d.invM = b.GetMotionPropertiesUnchecked()->GetInverseMass();
        d.invI = b.GetInverseInertia();
    }
}

Dyn readDyn(const JPH::BodyID& id) {
    Dyn d;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    if (lock.Succeeded()) fillDyn(lock.GetBody(), d);
    return d;
}

JPH::Vec3 pointVel(const Dyn& d, JPH::Vec3Arg p) { return d.lin + d.ang.Cross(p - d.com); }

// Inverse effective mass of the body at point p along the unit direction dir (0 for anything that does not move).
float invEff(const Dyn& d, JPH::Vec3Arg p, JPH::Vec3Arg dir) {
    if (!d.dynamic || d.invM <= 0.0f) return 0.0f;
    const JPH::Vec3 r = p - d.com;
    const JPH::Vec3 a = d.invI.Multiply3x3(r.Cross(dir));
    return d.invM + a.Cross(r).Dot(dir);
}

// Skips the chassis and sensors, and keeps bodies whose layer bits meet the wheel's mask.
class WheelRayFilter final : public JPH::BodyFilter {
public:
    WheelRayFilter(const JPH::BodyID& chassis, uint32_t mask) : chassis_(chassis), mask_(mask) {}
    bool ShouldCollide(const JPH::BodyID& other) const override { return other != chassis_; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return !body.IsSensor() && (mask_ & layerBitsOfBody(body)) != 0u; }
private:
    JPH::BodyID chassis_;
    uint32_t mask_;
};

// ---- the step ----------------------------------------------------------------------------------------

struct Probe {
    bool        hit = false;
    JPH::Vec3   dirW = JPH::Vec3::sZero(), axleW = JPH::Vec3::sZero(), point = JPH::Vec3::sZero(), normal = JPH::Vec3(0, 1, 0);
    JPH::BodyID groundId;
    Dyn         ground;
    float       suspLen = 0.0f, force = 0.0f;
};

void clearContact(RayWheel& w, float dt) {
    w.contact = false;
    w.suspForce = 0.0f;
    w.skid = 1.0f;
    w.groundBody = 0;
    w.fwdSpeed = w.sideSpeed = 0.0f;
    w.suspLen = w.p[AVER_PHYS_RAYW_REST_LENGTH] * kCmToM;
    w.spin *= std::pow(0.99f, dt * 60.0f);   // a wheel in the air coasts down
    w.rotation = std::fmod(w.rotation + w.spin * dt, kTau);
}

// The wheel's axle in the world, steered about the suspension's up.
JPH::Vec3 steeredAxle(const RayWheel& w, JPH::QuatArg chassisRot, JPH::Vec3Arg upS, float& steerOut) {
    const float steer = w.p[AVER_PHYS_RAYW_STEERING] != 0.0f ? w.steer : 0.0f;
    steerOut = steer;
    JPH::Vec3 axle = chassisRot * toJoltUnit(Vec3(w.p[AVER_PHYS_RAYW_AXLE_X], w.p[AVER_PHYS_RAYW_AXLE_Y], w.p[AVER_PHYS_RAYW_AXLE_Z]));
    axle = axle.Normalized();
    if (steer != 0.0f) axle = JPH::Quat::sRotation(upS, -steer) * axle;   // right-handed Y-up: a rightward turn is negative about up
    return axle;
}

void stepVehicle(RayVeh& v, float dt) {
    const JPH::BodyID* idp = findBody(v.chassis);
    if (!idp) return;
    const JPH::BodyID chassisId = *idp;
    Dyn c = readDyn(chassisId);
    if (!c.valid || !c.dynamic || c.invM <= 0.0f) { for (RayWheel& w : v.wheels) clearContact(w, dt); return; }
    if (!c.active) {
        bool wake = false;
        for (const RayWheel& w : v.wheels) wake = wake || (w.p[AVER_PHYS_RAYW_TRACTION] != 0.0f && w.engine != 0.0f);
        if (!wake) return;
        bi().ActivateBody(chassisId);
        c = readDyn(chassisId);
    }
    const float mass = 1.0f / c.invM;
    const JPH::Vec3 up = gravityUp();
    const size_t n = v.wheels.size();
    std::vector<Probe> probes(n);

    // 1. Suspension: one ray per wheel, spring and damper along the contact normal.
    const JPH::NarrowPhaseQuery& npq = g_world->system.GetNarrowPhaseQuery();
    for (size_t i = 0; i < n; ++i) {
        RayWheel& w = v.wheels[i];
        Probe& pr = probes[i];
        const float rest = w.p[AVER_PHYS_RAYW_REST_LENGTH] * kCmToM;
        const float radius = w.p[AVER_PHYS_RAYW_RADIUS] * kCmToM;
        const JPH::Vec3 dirL = toJoltUnit(Vec3(w.p[AVER_PHYS_RAYW_DIR_X], w.p[AVER_PHYS_RAYW_DIR_Y], w.p[AVER_PHYS_RAYW_DIR_Z]));
        if (dirL.LengthSq() < 1e-12f) { clearContact(w, dt); continue; }
        pr.dirW = (c.rot * dirL).Normalized();
        const JPH::Vec3 connW = c.origin + c.rot * toJolt(Vec3(w.p[AVER_PHYS_RAYW_CONNECT_X], w.p[AVER_PHYS_RAYW_CONNECT_Y], w.p[AVER_PHYS_RAYW_CONNECT_Z]));
        const float rayLen = rest + radius;
        w.point = connW + pr.dirW * rayLen;   // what a wheel in the air reports
        w.normal = -pr.dirW;
        const JPH::RRayCast ray{connW, pr.dirW * rayLen};
        JPH::RayCastResult hit;
        const WheelRayFilter filter(chassisId, w.mask);
        if (!npq.CastRay(ray, hit, {}, {}, filter)) { clearContact(w, dt); continue; }

        pr.point = connW + pr.dirW * (hit.mFraction * rayLen);
        {
            JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), hit.mBodyID);
            if (!lock.Succeeded()) { clearContact(w, dt); continue; }
            pr.normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, pr.point).Normalized();
            fillDyn(lock.GetBody(), pr.ground);
        }
        pr.groundId = hit.mBodyID;
        pr.hit = true;

        const float travel = w.p[AVER_PHYS_RAYW_MAX_TRAVEL] * kCmToM;
        const float minLen = std::max(rest - travel, 0.0f);
        pr.suspLen = std::clamp(hit.mFraction * rayLen - radius, minLen, rest);

        const float d = pr.normal.Dot(-pr.dirW);
        const float inv = 1.0f / std::max(d, kMinGroundDot);
        const float relVel = d >= kMinGroundDot ? pr.normal.Dot(pointVel(c, pr.point) - pointVel(pr.ground, pr.point)) * inv : 0.0f;
        float accel = w.p[AVER_PHYS_RAYW_STIFFNESS] * (rest - pr.suspLen) * inv;   // mass-normalised
        accel -= (relVel < 0.0f ? w.p[AVER_PHYS_RAYW_DAMP_COMPRESSION] : w.p[AVER_PHYS_RAYW_DAMP_RELAXATION]) * relVel;
        float force = accel * mass;
        const float maxForce = w.p[AVER_PHYS_RAYW_MAX_FORCE] * kCmToM;
        if (maxForce > 0.0f) force = std::min(force, maxForce);
        pr.force = std::max(force, 0.0f);

        w.contact = true;
        w.point = pr.point;
        w.normal = pr.normal;
        w.suspLen = pr.suspLen;
        w.suspForce = pr.force;
        w.groundBody = handleForBodyId(hit.mBodyID);
    }
    for (size_t i = 0; i < n; ++i) {
        const Probe& pr = probes[i];
        if (!pr.hit || pr.force <= 0.0f) continue;
        const JPH::Vec3 j = pr.normal * (pr.force * dt);
        bi().AddImpulse(chassisId, j, pr.point);
        if (pr.ground.dynamic && pr.ground.invM > 0.0f) bi().AddImpulse(pr.groundId, -j, pr.point);
    }

    // 2. Friction, from the velocities after the springs pushed. All wheels are solved against the same velocities and share the correction
    // (1/contacts for the side, 1/braking wheels for the brake) so that four wheels do not each cancel the whole slip.
    c = readDyn(chassisId);
    int contacts = 0, braking = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!probes[i].hit) continue;
        ++contacts;
        const RayWheel& w = v.wheels[i];
        if (w.brake > 0.0f && !(w.p[AVER_PHYS_RAYW_TRACTION] != 0.0f && w.engine != 0.0f)) ++braking;
    }
    struct Apply { JPH::Vec3 sideJ, fwdJ, sidePt, pt; JPH::BodyID ground; bool groundDyn = false; };
    std::vector<Apply> applies(n);
    for (size_t i = 0; i < n; ++i) {
        RayWheel& w = v.wheels[i];
        Probe& pr = probes[i];
        if (!pr.hit) continue;
        const JPH::Vec3 upS = -pr.dirW;
        float steerUsed = 0.0f;
        const JPH::Vec3 axle = steeredAxle(w, c.rot, upS, steerUsed);
        w.steerApplied = steerUsed;
        JPH::Vec3 fwd = pr.normal.Cross(axle);
        const float fl = fwd.Length();
        w.skid = 1.0f;
        if (fl < 1e-4f) { w.fwdSpeed = w.sideSpeed = 0.0f; continue; }
        fwd /= fl;
        const JPH::Vec3 side = fwd.Cross(pr.normal);
        const JPH::Vec3 vrel = pointVel(c, pr.point) - pointVel(pr.ground, pr.point);
        const float sideVel = vrel.Dot(side), fwdVel = vrel.Dot(fwd);
        w.fwdSpeed = fwdVel;
        w.sideSpeed = sideVel;

        const float invS = invEff(c, pr.point, side) + invEff(pr.ground, pr.point, side);
        float sideImp = invS > 0.0f ? -sideVel / invS / static_cast<float>(contacts) : 0.0f;
        float fwdImp = 0.0f;
        const bool driven = w.p[AVER_PHYS_RAYW_TRACTION] != 0.0f && w.engine != 0.0f;
        if (driven) {
            fwdImp = w.engine * kCmToM * dt;
        } else if (w.brake > 0.0f) {
            const float invF = invEff(c, pr.point, fwd) + invEff(pr.ground, pr.point, fwd);
            const float want = invF > 0.0f ? -fwdVel / invF / static_cast<float>(std::max(braking, 1)) : 0.0f;
            const float cap = w.brake * kCmToM * dt;
            fwdImp = std::clamp(want, -cap, cap);
        }
        const float maxImp = pr.force * dt * w.p[AVER_PHYS_RAYW_FRICTION_SLIP];
        const float mag = std::sqrt(fwdImp * fwdImp + sideImp * sideImp);
        if (mag > maxImp) {
            const float f = mag > 0.0f ? maxImp / mag : 1.0f;
            w.skid = f;
            fwdImp *= f;
            sideImp *= f;
        }
        w.spin = fwdVel / (w.p[AVER_PHYS_RAYW_RADIUS] * kCmToM);

        Apply& a = applies[i];
        a.sideJ = side * sideImp;
        a.fwdJ = fwd * fwdImp;
        a.pt = pr.point;
        const JPH::Vec3 rel = pr.point - c.com;
        a.sidePt = c.com + rel - up * (up.Dot(rel) * (1.0f - w.p[AVER_PHYS_RAYW_ROLL_INFLUENCE]));
        a.ground = pr.groundId;
        a.groundDyn = pr.ground.dynamic && pr.ground.invM > 0.0f;
    }
    for (size_t i = 0; i < n; ++i) {
        RayWheel& w = v.wheels[i];
        if (!probes[i].hit) continue;
        const Apply& a = applies[i];
        if (a.fwdJ.LengthSq() > 0.0f) {
            bi().AddImpulse(chassisId, a.fwdJ, a.pt);
            if (a.groundDyn) bi().AddImpulse(a.ground, -a.fwdJ, a.pt);
        }
        if (a.sideJ.LengthSq() > 0.0f) {
            bi().AddImpulse(chassisId, a.sideJ, a.sidePt);
            if (a.groundDyn) bi().AddImpulse(a.ground, -a.sideJ, a.pt);
        }
        w.rotation = std::fmod(w.rotation + w.spin * dt, kTau);
    }
}

RayVehicleState* state() { return g_world ? g_world->rayVehicles.get() : nullptr; }

}  // namespace

void rayVehicleStepBegin(float dt) {
    RayVehicleState* st = state();
    if (!st || st->vehicles.empty()) return;
    for (auto& [h, v] : st->vehicles) stepVehicle(v, dt);
}

void rayVehicleOnBodyRemoved(int32_t handle) {
    RayVehicleState* st = state();
    if (!st) return;
    const auto it = st->ofBody.find(handle);
    if (it == st->ofBody.end()) return;
    st->vehicles.erase(it->second);
    st->ofBody.erase(it);
}

}  // namespace aver::physics::detail

// ---- the ABI ----------------------------------------------------------------------------------------

namespace {

int32_t refuse(AbiError e) { setAbiError(e); return 0; }

RayVeh* vehOf(int32_t h) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return nullptr; }
    RayVehicleState* st = g_world->rayVehicles.get();
    if (!st) { setAbiError(AbiError::BadHandle); return nullptr; }
    const auto it = st->vehicles.find(h);
    if (it == st->vehicles.end()) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &it->second;
}

RayWheel* wheelOf(int32_t veh, int32_t index) {
    RayVeh* v = vehOf(veh);
    if (!v) return nullptr;
    if (index < 0 || index >= static_cast<int32_t>(v->wheels.size())) { setAbiError(AbiError::OutOfRange); return nullptr; }
    return &v->wheels[static_cast<size_t>(index)];
}

// Applies f to the wheel, or to every wheel for index -1.
template <class F>
int32_t forWheels(int32_t veh, int32_t index, F f) {
    RayVeh* v = vehOf(veh);
    if (!v) return 0;
    if (index == -1) { for (RayWheel& w : v->wheels) f(w); return 1; }
    if (index < 0 || index >= static_cast<int32_t>(v->wheels.size())) return refuse(AbiError::OutOfRange);
    f(v->wheels[static_cast<size_t>(index)]);
    return 1;
}

}  // namespace

extern "C" {

int32_t aver_phys_rayveh_abi_version(void) { return AVER_PHYS_RAYVEH_ABI_VERSION; }

int32_t aver_phys_rayveh_create(int32_t chassisBody) {
    if (!g_world) return refuse(AbiError::NotInitialised);
    const JPH::BodyID* id = findBody(chassisBody);
    if (!id) return refuse(AbiError::BadHandle);
    if (g_world->vehicleOfBody.count(chassisBody)) return refuse(AbiError::InvalidArgument);   // already a Jolt vehicle's chassis
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic() || lock.GetBody().IsSensor()) return refuse(AbiError::InvalidArgument);
    }
    if (!g_world->rayVehicles) g_world->rayVehicles = std::make_shared<RayVehicleState>();
    RayVehicleState& st = *g_world->rayVehicles;
    if (st.ofBody.count(chassisBody)) return refuse(AbiError::InvalidArgument);
    const int32_t h = st.next++;
    st.vehicles[h].chassis = chassisBody;
    st.ofBody[chassisBody] = h;
    return h;
}

int32_t aver_phys_rayveh_destroy(int32_t veh) {
    RayVeh* v = vehOf(veh);
    if (!v) return 0;
    RayVehicleState& st = *g_world->rayVehicles;
    st.ofBody.erase(v->chassis);
    st.vehicles.erase(veh);
    return 1;
}

int32_t aver_phys_rayveh_exists(int32_t veh) {
    RayVehicleState* st = state();
    return st && st->vehicles.count(veh) ? 1 : 0;
}

int32_t aver_phys_rayveh_count(void) {
    RayVehicleState* st = state();
    return st ? static_cast<int32_t>(st->vehicles.size()) : 0;
}

int32_t aver_phys_rayveh_chassis(int32_t veh) {
    RayVehicleState* st = state();
    if (!st) return 0;
    const auto it = st->vehicles.find(veh);
    return it == st->vehicles.end() ? 0 : it->second.chassis;
}

int32_t aver_phys_rayveh_add_wheel(int32_t veh, const float* p, int32_t n) {
    RayVeh* v = vehOf(veh);
    if (!v) return -1;
    if (n < 0 || n > AVER_PHYS_RAYW_COUNT || (n > 0 && !p)) { setAbiError(n < 0 || n > AVER_PHYS_RAYW_COUNT ? AbiError::InvalidArgument : AbiError::NullPointer); return -1; }
    if (static_cast<int>(v->wheels.size()) >= kMaxWheels) { setAbiError(AbiError::OutOfRange); return -1; }
    RayWheel w;
    setDefaults(w);
    for (int32_t k = 0; k < n; ++k) {
        if (!validParam(k, p[k])) { setAbiError(AbiError::InvalidArgument); return -1; }
        w.p[k] = p[k];
    }
    if (len3(&w.p[AVER_PHYS_RAYW_DIR_X]) < 1e-6f || len3(&w.p[AVER_PHYS_RAYW_AXLE_X]) < 1e-6f) { setAbiError(AbiError::InvalidArgument); return -1; }
    w.p[AVER_PHYS_RAYW_TRACTION] = w.p[AVER_PHYS_RAYW_TRACTION] != 0.0f ? 1.0f : 0.0f;
    w.p[AVER_PHYS_RAYW_STEERING] = w.p[AVER_PHYS_RAYW_STEERING] != 0.0f ? 1.0f : 0.0f;
    w.suspLen = w.p[AVER_PHYS_RAYW_REST_LENGTH] * kCmToM;
    v->wheels.push_back(w);
    return static_cast<int32_t>(v->wheels.size()) - 1;
}

int32_t aver_phys_rayveh_wheel_count(int32_t veh) {
    RayVeh* v = vehOf(veh);
    return v ? static_cast<int32_t>(v->wheels.size()) : -1;
}

int32_t aver_phys_rayveh_set_wheel_param(int32_t veh, int32_t wheel, int32_t key, float value) {
    RayWheel* w = wheelOf(veh, wheel);
    if (!w) return 0;
    if (key < 0 || key >= AVER_PHYS_RAYW_COUNT) return refuse(AbiError::OutOfRange);
    if (!validParam(key, value)) return refuse(AbiError::InvalidArgument);
    if (key == AVER_PHYS_RAYW_TRACTION || key == AVER_PHYS_RAYW_STEERING) value = value != 0.0f ? 1.0f : 0.0f;
    w->p[key] = value;
    if (!w->contact && key == AVER_PHYS_RAYW_REST_LENGTH) w->suspLen = value * kCmToM;
    return 1;
}

int32_t aver_phys_rayveh_wheel_param(int32_t veh, int32_t wheel, int32_t key, float* outValue) {
    RayWheel* w = wheelOf(veh, wheel);
    if (!w) return 0;
    if (!outValue) return refuse(AbiError::NullPointer);
    if (key < 0 || key >= AVER_PHYS_RAYW_COUNT) return refuse(AbiError::OutOfRange);
    *outValue = w->p[key];
    return 1;
}

int32_t aver_phys_rayveh_set_wheel_mask(int32_t veh, int32_t wheel, uint32_t mask) {
    RayWheel* w = wheelOf(veh, wheel);
    if (!w) return 0;
    w->mask = mask;
    return 1;
}

int32_t aver_phys_rayveh_wheel_mask(int32_t veh, int32_t wheel, uint32_t* outMask) {
    RayWheel* w = wheelOf(veh, wheel);
    if (!w) return 0;
    if (!outMask) return refuse(AbiError::NullPointer);
    *outMask = w->mask;
    return 1;
}

int32_t aver_phys_rayveh_set_engine_force(int32_t veh, int32_t wheel, float engineForce) {
    if (!std::isfinite(engineForce)) return refuse(AbiError::InvalidArgument);
    return forWheels(veh, wheel, [&](RayWheel& w) { w.engine = engineForce; });
}

int32_t aver_phys_rayveh_set_brake(int32_t veh, int32_t wheel, float brake) {
    if (!std::isfinite(brake) || brake < 0.0f) return refuse(AbiError::InvalidArgument);
    return forWheels(veh, wheel, [&](RayWheel& w) { w.brake = brake; });
}

int32_t aver_phys_rayveh_set_steering(int32_t veh, int32_t wheel, float steeringRad) {
    if (!std::isfinite(steeringRad)) return refuse(AbiError::InvalidArgument);
    return forWheels(veh, wheel, [&](RayWheel& w) { w.steer = steeringRad; });
}

int32_t aver_phys_rayveh_wheel_state(int32_t veh, int32_t wheel, float* outState, int32_t n) {
    RayWheel* w = wheelOf(veh, wheel);
    if (!w) return 0;
    if (!outState || n < 0) return refuse(n < 0 ? AbiError::InvalidArgument : AbiError::NullPointer);
    float s[AVER_PHYS_RAYS_COUNT] = {};
    const Vec3 pt = fromJolt(w->point), nrm = fromJoltUnit(w->normal);
    s[AVER_PHYS_RAYS_IN_CONTACT] = w->contact ? 1.0f : 0.0f;
    s[AVER_PHYS_RAYS_CONTACT_POINT] = pt.x; s[AVER_PHYS_RAYS_CONTACT_POINT + 1] = pt.y; s[AVER_PHYS_RAYS_CONTACT_POINT + 2] = pt.z;
    s[AVER_PHYS_RAYS_CONTACT_NORMAL] = nrm.x; s[AVER_PHYS_RAYS_CONTACT_NORMAL + 1] = nrm.y; s[AVER_PHYS_RAYS_CONTACT_NORMAL + 2] = nrm.z;
    s[AVER_PHYS_RAYS_SUSP_LENGTH] = w->suspLen * kCmPerMetre;
    s[AVER_PHYS_RAYS_SUSP_FORCE] = w->suspForce * kCmPerMetre;
    s[AVER_PHYS_RAYS_SKID_INFO] = w->skid;
    s[AVER_PHYS_RAYS_SPIN] = w->spin;
    s[AVER_PHYS_RAYS_ROTATION] = w->rotation;
    s[AVER_PHYS_RAYS_RPM] = w->spin * 60.0f / kTau;
    s[AVER_PHYS_RAYS_FORWARD_SPEED] = w->fwdSpeed * kCmPerMetre;
    s[AVER_PHYS_RAYS_SIDE_SPEED] = w->sideSpeed * kCmPerMetre;
    s[AVER_PHYS_RAYS_CONTACT_BODY] = static_cast<float>(w->groundBody);
    s[AVER_PHYS_RAYS_STEER_APPLIED] = w->steerApplied;
    for (int32_t k = 0; k < n && k < AVER_PHYS_RAYS_COUNT; ++k) outState[k] = s[k];
    return 1;
}

int32_t aver_phys_rayveh_wheel_pose(int32_t veh, int32_t wheel, float* outPositionCm, float* outRotation) {
    RayVeh* v = vehOf(veh);
    if (!v) return 0;
    if (wheel < 0 || wheel >= static_cast<int32_t>(v->wheels.size())) return refuse(AbiError::OutOfRange);
    const RayWheel& w = v->wheels[static_cast<size_t>(wheel)];
    const JPH::BodyID* id = findBody(v->chassis);
    if (!id) return refuse(AbiError::BadHandle);
    const Dyn c = readDyn(*id);
    if (!c.valid) return refuse(AbiError::BadHandle);

    const JPH::Vec3 dirW = (c.rot * toJoltUnit(Vec3(w.p[AVER_PHYS_RAYW_DIR_X], w.p[AVER_PHYS_RAYW_DIR_Y], w.p[AVER_PHYS_RAYW_DIR_Z]))).Normalized();
    const JPH::Vec3 connW = c.origin + c.rot * toJolt(Vec3(w.p[AVER_PHYS_RAYW_CONNECT_X], w.p[AVER_PHYS_RAYW_CONNECT_Y], w.p[AVER_PHYS_RAYW_CONNECT_Z]));
    const JPH::Vec3 centre = connW + dirW * w.suspLen;
    if (outPositionCm) writeVec(outPositionCm, fromJolt(centre));
    if (outRotation) {
        const JPH::Vec3 upS = -dirW;
        float steer = 0.0f;
        JPH::Vec3 axle = steeredAxle(w, c.rot, upS, steer);
        axle = (axle - upS * upS.Dot(axle)).Normalized();   // the wheel's axle is square to its suspension
        const JPH::Vec3 fwd = upS.Cross(axle);
        const JPH::Mat44 basis(JPH::Vec4(axle, 0.0f), JPH::Vec4(upS, 0.0f), JPH::Vec4(-fwd, 0.0f), JPH::Vec4(0, 0, 0, 1));
        const JPH::Quat q = JPH::Quat::sRotation(axle, -w.rotation) * basis.GetQuaternion();
        const Quat e = fromJolt(q.Normalized());
        outRotation[0] = e.x; outRotation[1] = e.y; outRotation[2] = e.z; outRotation[3] = e.w;
    }
    return 1;
}

}  // extern "C"
