// PhysicsVehicleExt.cpp -- the v2 vehicle ABI (physics_vehicle_ext_abi.h): the builder calls that describe an
// engine, gearbox, differentials and tyres, the live calls (aids, aero, tuning, gears), the telemetry, and the
// two hooks the fixed step runs around system.Update.
//
// THE HOOKS ARE THE DESIGN. Driver aids run in the fixed step, not in the frame, so they are independent of
// the frame rate: vehicleStepBegin turns the pedals the caller set into what Jolt sees (rate-limited
// steering, brake-to-reverse, hold at standstill, ABS, traction control) from values Jolt produced on the
// previous step, applies aero and the manual box's clutch timing; vehicleStepEnd reads the result back
// (slip, load, contact, events). Both walk World::vehicles in handle order, read no clock and call no rand,
// and both do nothing numerically different from v1 for a vehicle that never asked for anything.
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/physics/physics_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr float kEps = 1e-6f;
constexpr float kGravityMs2 = 9.81f;
constexpr usize kMaxEventsPerCall = 4096;
// A chassis contact below this closing speed (cm/s) is not worth keeping for the merge.
constexpr float kImpactNoteCmS = 50.0f;
// ABS lets the brake back on at this fraction of the release rate: at the same rate a wheel that has just unlocked
// is braked into the lock again before the next slip reading (measured: worst slip -1.0 at equal rates, -0.64 at a quarter).
constexpr float kAbsReapply = 0.25f;
// Below this speed a locked wheel is harmless and the slip ratio is noise, so ABS stands down and the car can stop.
constexpr float kAbsMinSpeedCmS = 200.0f;
constexpr float kImpactEventCmS = 100.0f;
constexpr float kLandAirborneSeconds = 0.2f;
constexpr float kSkidMinSpeedCmS = 150.0f;
constexpr float kFlippedSeconds = 3.0f;
// The slip ratio's denominator never drops below this (m/s): a wheel spinning at a standstill reads as a big
// number, not as infinity, and a locked wheel at a crawl does not read as a lock.
constexpr float kSlipSpeedFloor = 1.0f;
constexpr float kSlipClampHigh = 8.0f;

bool finite(float v) { return std::isfinite(v); }

float approach(float cur, float target, float maxDelta) {
    if (cur < target) return std::min(target, cur + maxDelta);
    return std::max(target, cur - maxDelta);
}

// Copies the first n floats of a block over a base block, refusing a longer one or a non-finite number.
bool mergeBlock(const float* p, int32_t n, int32_t defined, float* base) {
    if (n < 0 || n > defined || (n > 0 && !p)) { aver::setAbiError(aver::AbiError::InvalidArgument); return false; }
    for (int32_t i = 0; i < n; ++i)
        if (!finite(p[i])) { aver::setAbiError(aver::AbiError::InvalidArgument); return false; }
    for (int32_t i = 0; i < n; ++i) base[i] = p[i];
    return true;
}

bool fail(aver::AbiError code) { aver::setAbiError(code); return false; }

VehicleEntry* findBuilding(int32_t v) {
    VehicleEntry* e = findVehicle(v);
    return (e && e->constraint == nullptr) ? e : nullptr;
}

JPH::WheeledVehicleController* wheeledOf(VehicleEntry& e) {
    return static_cast<JPH::WheeledVehicleController*>(e.constraint->GetController());
}
JPH::TrackedVehicleController* trackedOf(VehicleEntry& e) {
    return static_cast<JPH::TrackedVehicleController*>(e.constraint->GetController());
}

bool curveAscending(const float* x, int32_t n) {
    for (int32_t i = 0; i < n; ++i) {
        if (!finite(x[i])) return false;
        if (i > 0 && !(x[i] > x[i - 1])) return false;
    }
    return true;
}

// The steering-speed curve: up to four (speed cm/s, scale) points from the tune table, ended by a (0, 0) pair.
float steerCurveAt(const VehicleEntry& e, float speedCmS) {
    float xs[4], ys[4];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        const float sp = e.tune[AVER_PHYS_VU_STEER_CURVE_0 + i * 2], sc = e.tune[AVER_PHYS_VU_STEER_CURVE_0 + i * 2 + 1];
        if (sp == 0.0f && sc == 0.0f) break;
        xs[n] = sp; ys[n] = sc; ++n;
    }
    if (n == 0) return 1.0f;
    if (speedCmS <= xs[0]) return 1.0f;
    for (int i = 1; i < n; ++i)
        if (speedCmS <= xs[i]) {
            const float span = xs[i] - xs[i - 1];
            const float t = span > kEps ? (speedCmS - xs[i - 1]) / span : 1.0f;
            return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
        }
    return ys[n - 1];
}

// A wheel's extension record without growing the vector (the live path never mutates the builder's data).
float brakeBiasOf(const VehicleEntry& e, usize i) { return i < e.wheelExt.size() ? e.wheelExt[i].brakeBias : 1.0f; }

// Writes every wheel's service-brake torque: its builder value, times its bias, the BRAKE_SCALE tune and ABS.
void applyBrakeTorques(VehicleEntry& e) {
    if (e.kind == AVER_PHYS_VEH_KIND_TRACKED) {
        const float scale = e.tune[AVER_PHYS_VU_BRAKE_SCALE];
        auto& tracks = trackedOf(e)->GetTracks();
        for (int i = 0; i < 2; ++i) tracks[i].mMaxBrakeTorque = e.baseTrackBrake[i] * scale;
        return;
    }
    for (usize i = 0; i < e.wheelSettings.size(); ++i) {
        auto* ws = static_cast<JPH::WheelSettingsWV*>(e.wheelSettings[i].GetPtr());
        const float abs = i < e.live.size() ? e.live[i].absFactor : 1.0f;
        ws->mMaxBrakeTorque = e.wheels[i].maxBrakeNm * brakeBiasOf(e, i) * e.tune[AVER_PHYS_VU_BRAKE_SCALE] * abs;
    }
}

// The ENGINE_TORQUE_SCALE tune. At zero the engine is dead and gets no idle either: Jolt's clutch drags the wheels
// up to the idle speed whatever the torque, so a car with its torque scaled to nothing still crept away at 2.7 m/s.
void applyEngineScale(VehicleEntry& e) {
    JPH::VehicleEngine& engine = vehicleEngine(e);
    const float scale = e.tune[AVER_PHYS_VU_ENGINE_TORQUE_SCALE];
    engine.mMaxTorque = e.engineMaxTorque * scale;
    engine.mMinRPM = scale > 0.0f ? e.engineMinRpm : 0.0f;
}

void applySuspensionScale(VehicleEntry& e) {
    const float scale = std::max(0.1f, e.tune[AVER_PHYS_VU_SUSPENSION_STIFFNESS_SCALE]);
    for (usize i = 0; i < e.wheelSettings.size() && i < e.baseSpringHz.size(); ++i)
        e.wheelSettings[i]->mSuspensionSpring.mFrequency = e.baseSpringHz[i] * scale;
}

void pushEvent(int32_t kind, int32_t vehicle, int32_t a, int32_t b, const Vec3& point, const Vec3& normal,
               float value, float value2) {
    if (g_world->vehicleEvents.size() >= kMaxEventsPerCall) return;
    VehicleEventRec r;
    r.kind = kind; r.vehicle = vehicle; r.a = a; r.b = b;
    r.f[0] = point.x; r.f[1] = point.y; r.f[2] = point.z;
    r.f[3] = normal.x; r.f[4] = normal.y; r.f[5] = normal.z;
    r.f[6] = value; r.f[7] = value2;
    g_world->vehicleEvents.push_back(r);
}

// Starts a manual gear change: the clutch is open for the switch time (none out of neutral) and then closes
// over the release time, the same two timers Jolt's automatic box runs.
void beginManualShift(VehicleEntry& e, int32_t gear) {
    if (gear == e.gear) return;
    e.shiftLeft = e.gear != 0 ? e.gearbox[AVER_PHYS_VG_SWITCH_TIME] : 0.0f;
    e.clutchRelease = e.gearbox[AVER_PHYS_VG_CLUTCH_RELEASE_TIME];
    e.gear = gear;
}

// Jolt-space body helpers, all through the body interface (no lock held across steps).
struct BodyView {
    JPH::Vec3 vel, angVel, fwd, right, up;
};
BodyView viewOf(const JPH::BodyID& id) {
    BodyView b;
    const JPH::Quat rot = bi().GetRotation(id);
    b.vel = bi().GetLinearVelocity(id);
    b.angVel = bi().GetAngularVelocity(id);
    b.fwd = rot * toJoltUnit(Vec3(1.0f, 0.0f, 0.0f));
    b.right = rot * toJoltUnit(Vec3(0.0f, 1.0f, 0.0f));
    b.up = rot * toJoltUnit(Vec3(0.0f, 0.0f, 1.0f));
    return b;
}

// ---- one vehicle, before the solver ---------------------------------------------------------------------

void stepBeginOne(VehicleEntry& e, int32_t handle, const JPH::BodyID& id, float dt) {
    const bool active = bi().IsActive(id);
    const BodyView bv = viewOf(id);
    const float speedCm = mToCm(bv.vel.Length());
    const float fwdCm = mToCm(bv.vel.Dot(bv.fwd));
    JPH::VehicleTransmission& trans = vehicleTransmission(e);

    // The manual box: a timed clutch around every gear change. In automatic Jolt runs its own.
    if (trans.mMode == JPH::ETransmissionMode::Manual) {
        float clutch = 1.0f;
        if (e.shiftLeft > 0.0f) {
            e.shiftLeft = std::max(0.0f, e.shiftLeft - dt);
            clutch = 0.0f;
        } else if (e.clutchRelease > 0.0f) {
            e.clutchRelease = std::max(0.0f, e.clutchRelease - dt);
            const float total = std::max(kEps, e.gearbox[AVER_PHYS_VG_CLUTCH_RELEASE_TIME]);
            clutch = 1.0f - e.clutchRelease / total;
        }
        trans.Set(e.gear, clutch);
    }

    const float* a = e.assist;
    if (a[AVER_PHYS_VD_ENABLED] > 0.5f) {
        const bool tracked = e.kind == AVER_PHYS_VEH_KIND_TRACKED;
        const bool manual = trans.mMode == JPH::ETransmissionMode::Manual;
        float throttle = e.reqT, brake = e.reqB;

        // 1. Pedal intent: a pedal that fights the direction of travel is a brake, not a reverse.
        if (manual) {
            throttle = std::max(0.0f, e.reqT);
        } else if (a[AVER_PHYS_VD_REVERSE_ON_BRAKE] > 0.5f) {
            const float rs = a[AVER_PHYS_VD_REVERSE_SPEED];
            if (e.reqT < 0.0f && fwdCm > rs)       { brake = std::max(brake, -e.reqT); throttle = 0.0f; }
            else if (e.reqT > 0.0f && fwdCm < -rs) { brake = std::max(brake, e.reqT);  throttle = 0.0f; }
        }
        if (e.reqT == 0.0f && e.reqB == 0.0f && e.reqH == 0.0f && std::fabs(fwdCm) < a[AVER_PHYS_VD_HOLD_SPEED])
            brake = std::max(brake, a[AVER_PHYS_VD_HOLD_BRAKE]);

        // 2. Steering: the target shrinks with speed, the applied value moves towards it at a rate.
        float target = e.reqS * steerCurveAt(e, speedCm) * e.tune[AVER_PHYS_VU_STEER_SCALE];
        target = std::clamp(target, -1.0f, 1.0f);
        const float steerRate = (std::fabs(target) >= std::fabs(e.appS) ? a[AVER_PHYS_VD_STEER_IN] : a[AVER_PHYS_VD_STEER_OUT]);
        e.appS = approach(e.appS, target, std::max(0.0f, steerRate) * dt);
        const float thrRate = (std::fabs(throttle) >= std::fabs(e.appT) ? a[AVER_PHYS_VD_THROTTLE_IN] : a[AVER_PHYS_VD_THROTTLE_OUT]);

        // 3. Traction control (wheeled): throttle cut while a driven wheel spins faster than the ground.
        bool tcsOn = false;
        if (!tracked && a[AVER_PHYS_VD_TCS] > 0.5f && throttle > 0.05f) {
            for (usize i = 0; i < e.wheels.size() && i < e.live.size(); ++i)
                if (e.wheels[i].driven && e.live[i].i[AVER_PHYS_VWSI_CONTACT] &&
                    e.live[i].f[AVER_PHYS_VWS_SLIP_LONG] > a[AVER_PHYS_VD_TCS_SLIP]) { tcsOn = true; break; }
        }
        e.tcsFactor = tcsOn ? std::max(0.2f, e.tcsFactor - 4.0f * dt) : std::min(1.0f, e.tcsFactor + 2.0f * dt);
        const bool tcsNow = e.tcsFactor < 0.99f;
        if (tcsNow != e.tcsActive) {
            pushEvent(AVER_PHYS_VEV_ASSIST, handle, 1, tcsNow ? 1 : 0, Vec3(), Vec3(), -1.0f, 0.0f);
            e.tcsActive = tcsNow;
        }
        e.appT = approach(e.appT, throttle * e.tcsFactor, std::max(0.0f, thrRate) * dt);

        // 4. Brake, and ABS per wheel (wheeled): a locking wheel's brake torque falls and recovers at a rate.
        e.appB = approach(e.appB, brake, brake > e.appB ? std::max(0.0f, a[AVER_PHYS_VD_BRAKE_IN]) * dt
                                                       : std::max(0.0f, a[AVER_PHYS_VD_THROTTLE_OUT]) * dt);
        e.appH = e.reqH;
        bool absAny = false;
        int absWheel = -1;
        if (!tracked) {
            const bool absOn = a[AVER_PHYS_VD_ABS] > 0.5f && e.appB > 0.1f && speedCm > kAbsMinSpeedCmS;
            for (usize i = 0; i < e.live.size(); ++i) {
                VehicleWheelLive& L = e.live[i];
                const float rate = std::max(0.0f, a[AVER_PHYS_VD_ABS_RELEASE]) * dt;
                if (absOn && L.i[AVER_PHYS_VWSI_CONTACT] && -L.f[AVER_PHYS_VWS_SLIP_LONG] > a[AVER_PHYS_VD_ABS_SLIP])
                    L.absFactor = std::max(0.0f, L.absFactor - rate);   // to ZERO: Jolt keeps a locked wheel locked while any brake torque is on
                else
                    L.absFactor = std::min(1.0f, L.absFactor + (absOn ? rate * kAbsReapply : 1.0f));
                if (L.absFactor < 0.95f) { absAny = true; if (absWheel < 0) absWheel = static_cast<int>(i); }
            }
        }
        if (absAny != e.absActive) {
            pushEvent(AVER_PHYS_VEV_ASSIST, handle, 0, absAny ? 1 : 0, Vec3(), Vec3(), static_cast<float>(absWheel), 0.0f);
            e.absActive = absAny;
        }
        applyBrakeTorques(e);

        // 6. The result goes to the controller. (5, aero, below.)
        vehicleApplyInput(e, e.appT, e.appS, e.appB, e.appH);
    }

    // 5. Aero: a force at the centre of mass in this step, drag against the velocity and downforce along the chassis' up.
    const float drag = e.drag * e.tune[AVER_PHYS_VU_DRAG_SCALE], down = e.downforce * e.tune[AVER_PHYS_VU_DOWNFORCE_SCALE];
    if (active && (drag > 0.0f || down > 0.0f)) {
        const float speed = bv.vel.Length();
        const JPH::Vec3 f = bv.vel * (-drag * speed) - bv.up * (down * speed * speed);
        bi().AddForce(id, f, JPH::EActivation::DontActivate);
    }
}

// ---- one vehicle, after the solver ----------------------------------------------------------------------

void stepEndOne(VehicleEntry& e, int32_t handle, const JPH::BodyID& id, float dt) {
    const JPH::Wheels& wheels = e.constraint->GetWheels();
    JPH::Body* body = e.constraint->GetVehicleBody();
    const BodyView bv = viewOf(id);
    const float speedCm = mToCm(bv.vel.Length());
    const float* a = e.assist;
    const float invDt = dt > kEps ? 1.0f / dt : 0.0f;
    if (e.live.size() != wheels.size()) e.live.assign(wheels.size(), VehicleWheelLive{});

    int onGround = 0;
    for (usize wi = 0; wi < wheels.size(); ++wi) {
        const JPH::Wheel* w = wheels[wi];
        VehicleWheelLive& L = e.live[wi];
        const JPH::WheelSettings* ws = w->GetSettings();
        const bool contact = w->HasContact();
        float* f = L.f;
        int32_t* iv = L.i;
        std::memset(f, 0, sizeof(L.f));
        std::memset(iv, 0, sizeof(L.i));

        f[AVER_PHYS_VWS_SUSP_LENGTH_CM] = mToCm(w->GetSuspensionLength());
        f[AVER_PHYS_VWS_COMPRESSION_CM] = mToCm(ws->mSuspensionMaxLength - w->GetSuspensionLength());
        f[AVER_PHYS_VWS_ANG_VEL_RAD_S] = w->GetAngularVelocity();
        f[AVER_PHYS_VWS_STEER_DEG] = -JPH::RadiansToDegrees(w->GetSteerAngle());   // Jolt's steer angle is positive to the left
        f[AVER_PHYS_VWS_SPIN_RAD] = w->GetRotationAngle();
        f[AVER_PHYS_VWS_BRAKE_TORQUE_NM] = e.kind == AVER_PHYS_VEH_KIND_TRACKED ? 0.0f
            : e.appB * static_cast<const JPH::WheelSettingsWV*>(ws)->mMaxBrakeTorque +
              e.appH * static_cast<const JPH::WheelSettingsWV*>(ws)->mMaxHandBrakeTorque;

        float slipLong = 0.0f, slipLat = 0.0f, slide = 0.0f;
        if (contact) {
            ++onGround;
            if (!L.hadContact && L.airborne >= kLandAirborneSeconds)
                pushEvent(AVER_PHYS_VEV_LAND, handle, static_cast<int32_t>(wi), 0, fromJolt(w->GetContactPosition()),
                          fromJoltUnit(w->GetContactNormal()), mToCm(std::max(0.0f, -e.lastVel[1])), L.airborne);
            L.airborne = 0.0f;

            // Slip from the contact: the chassis' velocity at the contact point against the ground's, in the plane
            // of the surface. Signed, unlike Jolt's own (which is an absolute value): positive = the tyre is
            // turning faster than the road moves under it, negative = it is dragging.
            JPH::Vec3 rel = body->GetPointVelocity(w->GetContactPosition()) - w->GetContactPointVelocity();
            const JPH::Vec3 n = w->GetContactNormal();
            rel -= n * n.Dot(rel);
            const float vLong = rel.Dot(w->GetContactLongitudinal());
            const float vAll = rel.Length();
            const float rim = w->GetAngularVelocity() * ws->mRadius;
            slipLong = std::clamp((rim - vLong) / std::max(std::fabs(vLong), kSlipSpeedFloor), -1.0f, kSlipClampHigh);
            slipLat = vAll < 1.0e-3f ? 0.0f : JPH::RadiansToDegrees(std::acos(std::clamp(std::fabs(vLong) / vAll, 0.0f, 1.0f)));
            slide = mToCm(vAll);

            f[AVER_PHYS_VWS_SLIP_LONG] = slipLong;
            f[AVER_PHYS_VWS_SLIP_LAT_DEG] = slipLat;
            f[AVER_PHYS_VWS_LOAD_N] = w->GetSuspensionLambda() * invDt;
            f[AVER_PHYS_VWS_FORCE_LONG_N] = w->GetLongitudinalLambda() * invDt;
            f[AVER_PHYS_VWS_FORCE_LAT_N] = w->GetLateralLambda() * invDt;
            const Vec3 cp = fromJolt(w->GetContactPosition()), cn = fromJoltUnit(n);
            f[AVER_PHYS_VWS_CONTACT_POINT_X] = cp.x; f[AVER_PHYS_VWS_CONTACT_POINT_Y] = cp.y; f[AVER_PHYS_VWS_CONTACT_POINT_Z] = cp.z;
            f[AVER_PHYS_VWS_CONTACT_NORMAL_X] = cn.x; f[AVER_PHYS_VWS_CONTACT_NORMAL_Y] = cn.y; f[AVER_PHYS_VWS_CONTACT_NORMAL_Z] = cn.z;
            iv[AVER_PHYS_VWSI_CONTACT] = 1;
            if (const auto it = g_world->byId.find(w->GetContactBodyID()); it != g_world->byId.end())
                iv[AVER_PHYS_VWSI_CONTACT_BODY] = it->second;
            JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), w->GetContactBodyID());
            if (lock.Succeeded()) {
                f[AVER_PHYS_VWS_SURFACE_FRICTION] = lock.GetBody().GetFriction();
                iv[AVER_PHYS_VWSI_CONTACT_ENTITY] = static_cast<int32_t>(lock.GetBody().GetUserData());
            }
        } else {
            L.airborne += dt;
        }

        // Skidding: grounded, moving, and slipping past either threshold. Begin and end are events.
        const bool skid = contact && speedCm > kSkidMinSpeedCmS &&
                          (std::fabs(slipLong) > a[AVER_PHYS_VD_SKID_SLIP] || slipLat > a[AVER_PHYS_VD_SKID_ANGLE_DEG]);
        const float metric = std::fabs(slipLong) > a[AVER_PHYS_VD_SKID_SLIP] ? std::fabs(slipLong) : slipLat;
        const Vec3 cpt(f[AVER_PHYS_VWS_CONTACT_POINT_X], f[AVER_PHYS_VWS_CONTACT_POINT_Y], f[AVER_PHYS_VWS_CONTACT_POINT_Z]);
        const Vec3 cnm(f[AVER_PHYS_VWS_CONTACT_NORMAL_X], f[AVER_PHYS_VWS_CONTACT_NORMAL_Y], f[AVER_PHYS_VWS_CONTACT_NORMAL_Z]);
        if (skid && !L.skidding) {
            L.skidPeak = metric;
            pushEvent(AVER_PHYS_VEV_SKID_BEGIN, handle, static_cast<int32_t>(wi), 0, cpt, cnm, metric, slide);
        } else if (skid) {
            L.skidPeak = std::max(L.skidPeak, metric);
        } else if (L.skidding) {
            pushEvent(AVER_PHYS_VEV_SKID_END, handle, static_cast<int32_t>(wi), 0, cpt, cnm, L.skidPeak, slide);
            L.skidPeak = 0.0f;
        }
        L.skidding = skid;
        iv[AVER_PHYS_VWSI_SKIDDING] = skid ? 1 : 0;
        L.hadContact = contact;
    }

    // The vehicle as a whole.
    JPH::VehicleEngine& engine = vehicleEngine(e);
    JPH::VehicleTransmission& trans = vehicleTransmission(e);
    const float rpm = engine.GetCurrentRPM();
    const int32_t gear = trans.GetCurrentGear();
    if (gear != e.lastGear) {
        pushEvent(AVER_PHYS_VEV_SHIFT, handle, e.lastGear, gear, Vec3(), Vec3(), e.lastRpm, 0.0f);
        e.lastGear = gear;
    }
    const bool manual = trans.mMode == JPH::ETransmissionMode::Manual;
    const bool tracked = e.kind == AVER_PHYS_VEH_KIND_TRACKED;

    const JPH::Vec3 dv = bv.vel - JPH::Vec3(e.lastVel[0], e.lastVel[1], e.lastVel[2]);
    e.longAccelG = dv.Dot(bv.fwd) * invDt / kGravityMs2;
    e.latAccelG = dv.Dot(bv.right) * invDt / kGravityMs2;
    e.lastVel[0] = bv.vel.GetX(); e.lastVel[1] = bv.vel.GetY(); e.lastVel[2] = bv.vel.GetZ();
    e.lastRpm = rpm;
    e.airborneAll = onGround == 0 ? e.airborneAll + dt : 0.0f;

    // Tipped over: the chassis' up against the world's, for long enough.
    const float upZ = bv.up.GetY();
    const float flipLimit = (e.maxPitchRollDeg > 0.0f && e.maxPitchRollDeg < 180.0f)
        ? std::cos(JPH::DegreesToRadians(e.maxPitchRollDeg)) + 0.05f : 0.3f;
    if (upZ < flipLimit) e.sideSeconds += dt; else { e.sideSeconds = 0.0f; e.flippedSent = false; }
    if (e.sideSeconds >= kFlippedSeconds && !e.flippedSent) {
        e.flippedSent = true;
        pushEvent(AVER_PHYS_VEV_FLIPPED, handle, 0, 0, Vec3(), Vec3(), upZ, e.sideSeconds);
    }

    float* vs = e.vs;
    vs[AVER_PHYS_VS_RPM] = rpm;
    vs[AVER_PHYS_VS_ENGINE_TORQUE_NM] = engine.GetTorque(std::fabs(e.appT)) * (manual ? 1.0f : trans.GetClutchFriction());
    vs[AVER_PHYS_VS_SPEED_CMS] = speedCm;
    vs[AVER_PHYS_VS_FORWARD_SPEED_CMS] = mToCm(bv.vel.Dot(bv.fwd));
    vs[AVER_PHYS_VS_LATERAL_SPEED_CMS] = mToCm(bv.vel.Dot(bv.right));
    vs[AVER_PHYS_VS_THROTTLE_APPLIED] = e.appT;
    vs[AVER_PHYS_VS_BRAKE_APPLIED] = e.appB;
    vs[AVER_PHYS_VS_STEER_APPLIED] = e.appS;
    vs[AVER_PHYS_VS_HANDBRAKE_APPLIED] = e.appH;
    vs[AVER_PHYS_VS_CLUTCH] = trans.GetClutchFriction();
    vs[AVER_PHYS_VS_WHEEL_RPM_AT_CLUTCH] = tracked ? vehicleTrackedWheelRpm(e) : wheeledOf(e)->GetWheelSpeedAtClutch();
    vs[AVER_PHYS_VS_YAW_RATE_DEG_S] = JPH::RadiansToDegrees(fromJoltAngular(bv.angVel).z);
    vs[AVER_PHYS_VS_LONG_ACCEL_G] = e.longAccelG;
    vs[AVER_PHYS_VS_LAT_ACCEL_G] = e.latAccelG;
    vs[AVER_PHYS_VS_UP_Z] = upZ;
    float tl = 0.0f, tr = 0.0f;
    if (tracked) vehicleTrackSpeeds(e, tl, tr);
    vs[AVER_PHYS_VS_TRACK_LEFT_CMS] = tl;
    vs[AVER_PHYS_VS_TRACK_RIGHT_CMS] = tr;
    vs[AVER_PHYS_VS_AIRBORNE_SECONDS] = e.airborneAll;

    int32_t* vsi = e.vsi;
    vsi[AVER_PHYS_VSI_GEAR] = gear;
    vsi[AVER_PHYS_VSI_GEAR_COUNT] = static_cast<int32_t>(trans.mGearRatios.size());
    vsi[AVER_PHYS_VSI_MANUAL] = manual ? 1 : 0;
    vsi[AVER_PHYS_VSI_SHIFTING] = manual ? ((e.shiftLeft > 0.0f || e.clutchRelease > 0.0f) ? 1 : 0) : (trans.IsSwitchingGear() ? 1 : 0);
    vsi[AVER_PHYS_VSI_WHEELS_ON_GROUND] = onGround;
    vsi[AVER_PHYS_VSI_ABS_ACTIVE] = e.absActive ? 1 : 0;
    vsi[AVER_PHYS_VSI_TCS_ACTIVE] = e.tcsActive ? 1 : 0;
    vsi[AVER_PHYS_VSI_KIND] = e.kind;
    vsi[AVER_PHYS_VSI_AXLE_COUNT] = static_cast<int32_t>(vehicleAxles(e.wheels).size());
    vsi[AVER_PHYS_VSI_WHEEL_COUNT] = static_cast<int32_t>(wheels.size());
    vsi[AVER_PHYS_VSI_REV_LIMITED] = rpm >= engine.mMaxRPM * 0.995f ? 1 : 0;
}

} // namespace

namespace aver::physics::detail {

JPH::VehicleEngine& vehicleEngine(VehicleEntry& e) {
    return e.kind == AVER_PHYS_VEH_KIND_TRACKED ? trackedOf(e)->GetEngine() : wheeledOf(e)->GetEngine();
}

JPH::VehicleTransmission& vehicleTransmission(VehicleEntry& e) {
    return e.kind == AVER_PHYS_VEH_KIND_TRACKED ? trackedOf(e)->GetTransmission() : wheeledOf(e)->GetTransmission();
}

void vehicleApplyInput(VehicleEntry& e, float throttle, float steer, float brake, float handbrake) {
    if (e.kind == AVER_PHYS_VEH_KIND_TRACKED) vehicleTrackedInput(e, throttle, steer, brake, handbrake);
    else wheeledOf(e)->SetDriverInput(throttle, steer, brake, handbrake);
    // The controller lets the body sleep while `forward` is zero and nothing wakes it from the vehicle's
    // side, so a throttle that arrives while it sleeps has to (and a tank's pivot, which is steer alone).
    if (throttle != 0.0f || (e.kind == AVER_PHYS_VEH_KIND_TRACKED && steer != 0.0f)) {
        if (const auto it = g_world->bodies.find(e.body); it != g_world->bodies.end())
            if (!bi().IsActive(it->second)) bi().ActivateBody(it->second);
    }
}

void vehicleOnFinished(VehicleEntry& e) {
    e.live.assign(e.wheels.size(), VehicleWheelLive{});
    JPH::VehicleEngine& engine = vehicleEngine(e);
    JPH::VehicleTransmission& trans = vehicleTransmission(e);
    e.lastRpm = engine.GetCurrentRPM();
    e.lastGear = trans.GetCurrentGear();
    e.gear = 0;
    e.shiftLeft = e.clutchRelease = 0.0f;
    if (trans.mMode == JPH::ETransmissionMode::Manual) {
        e.gear = 1;
        e.lastGear = 1;
        trans.Set(1, 1.0f);
    }
    e.vsi[AVER_PHYS_VSI_KIND] = e.kind;
    e.vsi[AVER_PHYS_VSI_GEAR_COUNT] = static_cast<int32_t>(trans.mGearRatios.size());
    e.vsi[AVER_PHYS_VSI_WHEEL_COUNT] = static_cast<int32_t>(e.wheels.size());
    e.vsi[AVER_PHYS_VSI_AXLE_COUNT] = static_cast<int32_t>(vehicleAxles(e.wheels).size());
    e.vsi[AVER_PHYS_VSI_MANUAL] = trans.mMode == JPH::ETransmissionMode::Manual ? 1 : 0;
    e.vsi[AVER_PHYS_VSI_GEAR] = e.gear;
    if (const auto it = g_world->bodies.find(e.body); it != g_world->bodies.end()) {
        const JPH::Vec3 v = bi().GetLinearVelocity(it->second);
        e.lastVel[0] = v.GetX(); e.lastVel[1] = v.GetY(); e.lastVel[2] = v.GetZ();
    }
    // Tunes set while building: only those that are not already what the build wrote.
    if (e.tune[AVER_PHYS_VU_ENGINE_TORQUE_SCALE] != 1.0f) applyEngineScale(e);
    if (e.tune[AVER_PHYS_VU_SUSPENSION_STIFFNESS_SCALE] != 1.0f) applySuspensionScale(e);
    if (e.tune[AVER_PHYS_VU_BRAKE_SCALE] != 1.0f) applyBrakeTorques(e);
}

void vehicleResetDrivetrain(VehicleEntry& e) {
    for (JPH::Wheel* wheel : e.constraint->GetWheels()) wheel->SetAngularVelocity(0.0f);
    if (e.kind == AVER_PHYS_VEH_KIND_TRACKED)
        for (JPH::VehicleTrack& t : trackedOf(e)->GetTracks()) t.mAngularVelocity = 0.0f;
    JPH::VehicleEngine& engine = vehicleEngine(e);
    engine.SetCurrentRPM(engine.mMinRPM);
    JPH::VehicleTransmission& trans = vehicleTransmission(e);
    const bool manual = trans.mMode == JPH::ETransmissionMode::Manual;
    e.gear = manual ? 1 : 0;
    e.shiftLeft = e.clutchRelease = 0.0f;
    trans.Set(e.gear, 1.0f);
    e.lastGear = e.gear;
    e.lastRpm = engine.mMinRPM;
    e.tcsFactor = 1.0f;
    e.sideSeconds = 0.0f;
    e.flippedSent = false;
    e.airborneAll = 0.0f;
    e.lastVel[0] = e.lastVel[1] = e.lastVel[2] = 0.0f;
    for (VehicleWheelLive& L : e.live) { L.absFactor = 1.0f; L.airborne = 0.0f; L.skidding = false; L.skidPeak = 0.0f; }
    applyBrakeTorques(e);
}

void vehicleBeginCall() {
    if (!g_world) return;
    g_world->vehicleEvents.clear();
    g_world->vehicleImpacts.clear();
}

void vehicleStepBegin(float dt) {
    if (!g_world || g_world->vehicles.empty()) return;
    for (auto& [handle, e] : g_world->vehicles) {
        if (e.constraint == nullptr) continue;
        const auto it = g_world->bodies.find(e.body);
        if (it == g_world->bodies.end()) continue;
        stepBeginOne(e, handle, it->second, dt);
    }
}

void vehicleStepEnd(float dt) {
    if (!g_world || g_world->vehicles.empty()) return;
    for (auto& [handle, e] : g_world->vehicles) {
        if (e.constraint == nullptr) continue;
        const auto it = g_world->bodies.find(e.body);
        if (it == g_world->bodies.end()) continue;
        stepEndOne(e, handle, it->second, dt);
    }

    // Chassis hits noted by the contact listener (worker threads, arbitrary order): merge to one per (vehicle,
    // other body) at the hardest closing speed, in a fixed order, and keep those that matter.
    auto& raw = g_world->vehicleImpacts;
    if (raw.empty()) return;
    std::sort(raw.begin(), raw.end(), [](const VehicleImpactRaw& l, const VehicleImpactRaw& r) {
        if (l.vehicle != r.vehicle) return l.vehicle < r.vehicle;
        if (l.other != r.other) return l.other < r.other;
        return l.closingCmS > r.closingCmS;
    });
    for (usize i = 0; i < raw.size(); ++i) {
        if (i > 0 && raw[i].vehicle == raw[i - 1].vehicle && raw[i].other == raw[i - 1].other) continue;
        if (raw[i].closingCmS <= kImpactEventCmS) continue;
        pushEvent(AVER_PHYS_VEV_IMPACT, raw[i].vehicle, raw[i].other, 0, raw[i].point, raw[i].normal,
                  raw[i].closingCmS, raw[i].otherMassKg);
    }
    raw.clear();
}

void vehicleOnContact(int32_t ha, int32_t hb, const JPH::Body& a, const JPH::Body& b,
                      const Vec3& pointCm, const Vec3& normal) {
    if (g_world->vehicleOfBody.empty()) return;
    const auto va = g_world->vehicleOfBody.find(ha);
    const auto vb = g_world->vehicleOfBody.find(hb);
    if (va == g_world->vehicleOfBody.end() && vb == g_world->vehicleOfBody.end()) return;
    const JPH::Vec3 p = toJolt(pointCm);
    for (int side = 0; side < 2; ++side) {
        const auto owner = side == 0 ? va : vb;
        if (owner == g_world->vehicleOfBody.end()) continue;
        const JPH::Body& chassis = side == 0 ? a : b;
        const JPH::Body& other = side == 0 ? b : a;
        const Vec3 toOther = side == 0 ? normal : -normal;          // chassis -> other
        const Vec3 rel = fromJoltDir(chassis.GetPointVelocity(p) - other.GetPointVelocity(p));
        const float closing = rel.x * toOther.x + rel.y * toOther.y + rel.z * toOther.z;
        if (closing <= kImpactNoteCmS) continue;
        VehicleImpactRaw r;
        r.vehicle = owner->second;
        r.other = side == 0 ? hb : ha;
        r.closingCmS = closing;
        r.otherMassKg = other.IsDynamic() ? 1.0f / std::max(kEps, other.GetMotionProperties()->GetInverseMass()) : 0.0f;
        r.point = pointCm;
        r.normal = -toOther;                                        // out of the other body, towards the chassis
        g_world->vehicleImpacts.push_back(r);
    }
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_vehicle_abi_version(void) { return AVER_PHYS_VEHICLE_ABI_VERSION; }

// ---- Building ----------------------------------------------------------------------------------------

int32_t aver_phys_vehicle_set_engine_ex(int32_t v, const float* p, int32_t n) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    float b[AVER_PHYS_VE_COUNT] = {e->engineMaxTorque, e->engineMinRpm, e->engineMaxRpm, e->engineInertia, e->engineDamping};
    if (!mergeBlock(p, n, AVER_PHYS_VE_COUNT, b)) return 0;
    if (b[0] <= 0.0f || b[1] < 0.0f || b[2] <= b[1] || b[3] < 0.0f || b[4] < 0.0f) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }
    e->engineMaxTorque = b[0]; e->engineMinRpm = b[1]; e->engineMaxRpm = b[2];
    e->engineInertia = b[3]; e->engineDamping = b[4];
    return 1;
}

int32_t aver_phys_vehicle_set_torque_curve(int32_t v, const float* rpmFraction, const float* torqueFraction, int32_t n) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    if (!rpmFraction || !torqueFraction || n < 2 || n > 32 || !curveAscending(rpmFraction, n) ||
        rpmFraction[0] < 0.0f || rpmFraction[n - 1] > 1.0f) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }
    for (int32_t i = 0; i < n; ++i)
        if (!finite(torqueFraction[i]) || torqueFraction[i] < 0.0f) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    e->curveX.assign(rpmFraction, rpmFraction + n);
    e->curveY.assign(torqueFraction, torqueFraction + n);
    return 1;
}

int32_t aver_phys_vehicle_set_gearbox(int32_t v, int32_t manual, const float* fwd, int32_t nFwd,
                                      const float* rev, int32_t nRev, const float* p, int32_t n) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    if (nFwd < 0 || nFwd > 16 || nRev < 0 || nRev > 4 || (nFwd > 0 && !fwd) || (nRev > 0 && !rev)) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }
    for (int32_t i = 0; i < nFwd; ++i)
        if (!finite(fwd[i]) || fwd[i] <= 0.0f) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    for (int32_t i = 0; i < nRev; ++i)
        if (!finite(rev[i]) || rev[i] <= 0.0f) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    float b[AVER_PHYS_VG_COUNT];
    std::memcpy(b, e->gearbox, sizeof b);
    if (!mergeBlock(p, n, AVER_PHYS_VG_COUNT, b)) return 0;
    if (b[0] < 0.0f || b[1] < 0.0f || b[2] < 0.0f || b[5] < 0.0f ||
        !(b[4] > 0.0f && b[4] < b[3] && b[3] < 1.0f)) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }
    std::memcpy(e->gearbox, b, sizeof b);
    e->manual = manual != 0;
    e->gearFwd.assign(fwd, fwd + nFwd);
    e->gearRev.assign(rev, rev + nRev);
    return 1;
}

int32_t aver_phys_vehicle_axle_count(int32_t v) {
    VehicleEntry* e = findVehicle(v);
    return e ? static_cast<int32_t>(vehicleAxles(e->wheels).size()) : 0;
}

int32_t aver_phys_vehicle_wheel_axle(int32_t v, int32_t wheel) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return -1;
    if (wheel < 0 || static_cast<usize>(wheel) >= e->wheels.size()) { aver::setAbiError(aver::AbiError::OutOfRange); return -1; }
    const std::vector<VehicleAxlePair> axles = vehicleAxles(e->wheels);
    for (usize i = 0; i < axles.size(); ++i)
        if (axles[i].left == wheel || axles[i].right == wheel) return static_cast<int32_t>(i);
    return -1;
}

int32_t aver_phys_vehicle_set_axle(int32_t v, int32_t axle, const float* p, int32_t n) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    if (e->kind == AVER_PHYS_VEH_KIND_TRACKED) { AVER_WARN("[Physics] vehicle {} is tracked: it has no axles", v); return fail(aver::AbiError::InvalidArgument); }
    const usize count = vehicleAxles(e->wheels).size();
    if (axle < 0 || static_cast<usize>(axle) >= count) return fail(aver::AbiError::OutOfRange);
    VehicleAxleDesc d = static_cast<usize>(axle) < e->axleDesc.size() ? e->axleDesc[static_cast<usize>(axle)] : VehicleAxleDesc{};
    float b[AVER_PHYS_VA_COUNT] = {d.finalDrive, static_cast<float>(d.diffMode), d.lsdRatio, d.split, d.share, d.antiRoll};
    if (!mergeBlock(p, n, AVER_PHYS_VA_COUNT, b)) return 0;
    const int32_t mode = static_cast<int32_t>(b[1]);
    if (b[0] <= 0.0f || mode < 0 || mode > 2 || b[2] <= 1.0f || b[3] < 0.0f || b[3] > 1.0f || b[4] < 0.0f || b[5] < 0.0f)
        return fail(aver::AbiError::InvalidArgument);
    d.finalDrive = b[0]; d.diffMode = mode; d.lsdRatio = b[2]; d.split = b[3]; d.share = b[4]; d.antiRoll = b[5];
    if (e->axleDesc.size() <= static_cast<usize>(axle)) e->axleDesc.resize(static_cast<usize>(axle) + 1);
    e->axleDesc[static_cast<usize>(axle)] = d;
    return 1;
}

int32_t aver_phys_vehicle_set_center_diff(int32_t v, int32_t mode, float slipRatio) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    if (e->kind == AVER_PHYS_VEH_KIND_TRACKED) return fail(aver::AbiError::InvalidArgument);
    if (mode < 0 || mode > 2 || (mode == 1 && !(finite(slipRatio) && slipRatio > 1.0f))) return fail(aver::AbiError::InvalidArgument);
    e->centerSet = true;
    e->centerLsd = mode == 0 ? 3.4e38f : (mode == 2 ? 1.01f : slipRatio);
    return 1;
}

int32_t aver_phys_vehicle_set_wheel_ex(int32_t v, int32_t wheel, const float* p, int32_t n) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    if (wheel < 0 || static_cast<usize>(wheel) >= e->wheels.size()) return fail(aver::AbiError::OutOfRange);
    if (e->wheelExt.size() <= static_cast<usize>(wheel)) e->wheelExt.resize(static_cast<usize>(wheel) + 1);
    VehicleWheelExt& x = e->wheelExt[static_cast<usize>(wheel)];
    float b[AVER_PHYS_VW_COUNT] = {x.inertia, x.angDamping, x.tvLong, x.tvLat, x.brakeBias};
    if (!mergeBlock(p, n, AVER_PHYS_VW_COUNT, b)) return 0;
    if (b[0] <= 0.0f || b[1] < 0.0f || b[2] <= 0.0f || b[3] <= 0.0f || b[4] < 0.0f) return fail(aver::AbiError::InvalidArgument);
    x.inertia = b[0]; x.angDamping = b[1]; x.tvLong = b[2]; x.tvLat = b[3]; x.brakeBias = b[4];
    return 1;
}

int32_t aver_phys_vehicle_set_wheel_curves(int32_t v, int32_t wheel,
                                           const float* longSlip, const float* longMu, int32_t nLong,
                                           const float* latDeg, const float* latMu, int32_t nLat) {
    VehicleEntry* e = findBuilding(v);
    if (!e) return 0;
    if (wheel < -1 || (wheel >= 0 && static_cast<usize>(wheel) >= e->wheels.size())) return fail(aver::AbiError::OutOfRange);
    if (!longSlip || !longMu || !latDeg || !latMu || nLong < 2 || nLong > 16 || nLat < 2 || nLat > 16 ||
        !curveAscending(longSlip, nLong) || !curveAscending(latDeg, nLat) || longSlip[0] < 0.0f || latDeg[0] < 0.0f)
        return fail(aver::AbiError::InvalidArgument);
    for (int32_t i = 0; i < nLong; ++i) if (!finite(longMu[i]) || longMu[i] < 0.0f) return fail(aver::AbiError::InvalidArgument);
    for (int32_t i = 0; i < nLat; ++i)  if (!finite(latMu[i]) || latMu[i] < 0.0f)  return fail(aver::AbiError::InvalidArgument);
    if (wheel < 0) {
        e->allLongSlip.assign(longSlip, longSlip + nLong); e->allLongMu.assign(longMu, longMu + nLong);
        e->allLatDeg.assign(latDeg, latDeg + nLat);        e->allLatMu.assign(latMu, latMu + nLat);
        for (VehicleWheelExt& x : e->wheelExt) { x.longSlip.clear(); x.longMu.clear(); x.latDeg.clear(); x.latMu.clear(); }
    } else {
        if (e->wheelExt.size() <= static_cast<usize>(wheel)) e->wheelExt.resize(static_cast<usize>(wheel) + 1);
        VehicleWheelExt& x = e->wheelExt[static_cast<usize>(wheel)];
        x.longSlip.assign(longSlip, longSlip + nLong); x.longMu.assign(longMu, longMu + nLong);
        x.latDeg.assign(latDeg, latDeg + nLat);        x.latMu.assign(latMu, latMu + nLat);
    }
    return 1;
}

// ---- Live: before or after _finish ---------------------------------------------------------------------

int32_t aver_phys_vehicle_set_assists(int32_t v, const float* p, int32_t n) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    float b[AVER_PHYS_VD_COUNT];
    std::memcpy(b, e->assist, sizeof b);
    if (!mergeBlock(p, n, AVER_PHYS_VD_COUNT, b)) return 0;
    for (int i = 0; i < AVER_PHYS_VD_COUNT; ++i)
        if (b[i] < 0.0f) return fail(aver::AbiError::InvalidArgument);
    const bool wasOn = e->assist[AVER_PHYS_VD_ENABLED] > 0.5f;
    std::memcpy(e->assist, b, sizeof b);
    const bool isOn = e->assist[AVER_PHYS_VD_ENABLED] > 0.5f;
    if (e->constraint != nullptr && wasOn != isOn) {
        // Off is v1's pass-through: the pedals as set go straight to Jolt and every aid lets go.
        if (!isOn) {
            e->tcsFactor = 1.0f; e->tcsActive = false; e->absActive = false;
            for (VehicleWheelLive& L : e->live) L.absFactor = 1.0f;
            applyBrakeTorques(*e);
            e->appT = e->reqT; e->appS = e->reqS; e->appB = e->reqB; e->appH = e->reqH;
            vehicleApplyInput(*e, e->appT, e->appS, e->appB, e->appH);
        }
    }
    return 1;
}

int32_t aver_phys_vehicle_set_aero(int32_t v, float dragNs2Pm2, float downforceNs2Pm2) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    if (!finite(dragNs2Pm2) || !finite(downforceNs2Pm2) || dragNs2Pm2 < 0.0f || downforceNs2Pm2 < 0.0f) return fail(aver::AbiError::InvalidArgument);
    e->drag = dragNs2Pm2;
    e->downforce = downforceNs2Pm2;
    return 1;
}

int32_t aver_phys_vehicle_set_collision_period(int32_t v, int32_t everyNSteps) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    if (everyNSteps < 1 || everyNSteps > 8) return fail(aver::AbiError::InvalidArgument);
    e->collisionPeriod = everyNSteps;
    if (e->constraint != nullptr) {
        e->constraint->SetNumStepsBetweenCollisionTestActive(static_cast<JPH::uint>(everyNSteps));
        e->constraint->SetNumStepsBetweenCollisionTestInactive(static_cast<JPH::uint>(everyNSteps));
    }
    return 1;
}

int32_t aver_phys_vehicle_tune(int32_t v, int32_t key, float value) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    if (key < 0 || key >= AVER_PHYS_VU_COUNT || !finite(value)) return fail(aver::AbiError::InvalidArgument);
    if (key <= AVER_PHYS_VU_DOWNFORCE_SCALE && (value < 0.0f || value > 10.0f)) return fail(aver::AbiError::InvalidArgument);
    if (key == AVER_PHYS_VU_SUSPENSION_STIFFNESS_SCALE && value < 0.1f) return fail(aver::AbiError::InvalidArgument);
    if (key >= AVER_PHYS_VU_STEER_CURVE_0 && value < 0.0f) return fail(aver::AbiError::InvalidArgument);
    e->tune[key] = value;
    if (e->constraint == nullptr) return 1;
    switch (key) {
    case AVER_PHYS_VU_ENGINE_TORQUE_SCALE: applyEngineScale(*e); break;
    case AVER_PHYS_VU_BRAKE_SCALE:         applyBrakeTorques(*e); break;
    case AVER_PHYS_VU_SUSPENSION_STIFFNESS_SCALE: applySuspensionScale(*e); break;
    default: break;   // grip is read by the combine function, steer / drag / downforce / curve by the step hook
    }
    return 1;
}

// ---- Live: after _finish -----------------------------------------------------------------------------

int32_t aver_phys_vehicle_set_gearbox_mode(int32_t v, int32_t manual) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    JPH::VehicleTransmission& trans = vehicleTransmission(*e);
    const bool wantManual = manual != 0;
    if (wantManual == (trans.mMode == JPH::ETransmissionMode::Manual)) return 1;
    e->manual = wantManual;
    if (wantManual) {
        trans.mMode = JPH::ETransmissionMode::Manual;
        e->gear = trans.GetCurrentGear() != 0 ? trans.GetCurrentGear() : 1;
        e->shiftLeft = e->clutchRelease = 0.0f;
        trans.Set(e->gear, 1.0f);
    } else {
        trans.mMode = JPH::ETransmissionMode::Auto;
    }
    return 1;
}

int32_t aver_phys_vehicle_set_gear(int32_t v, int32_t gear) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    JPH::VehicleTransmission& trans = vehicleTransmission(*e);
    if (trans.mMode != JPH::ETransmissionMode::Manual) return fail(aver::AbiError::InvalidArgument);
    if (gear < -static_cast<int32_t>(trans.mReverseGearRatios.size()) || gear > static_cast<int32_t>(trans.mGearRatios.size()))
        return fail(aver::AbiError::OutOfRange);
    beginManualShift(*e, gear);
    return 1;
}

int32_t aver_phys_vehicle_shift(int32_t v, int32_t delta) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    JPH::VehicleTransmission& trans = vehicleTransmission(*e);
    if (trans.mMode != JPH::ETransmissionMode::Manual) return fail(aver::AbiError::InvalidArgument);
    const int32_t lo = -static_cast<int32_t>(trans.mReverseGearRatios.size());
    const int32_t hi = static_cast<int32_t>(trans.mGearRatios.size());
    beginManualShift(*e, std::clamp(e->gear + delta, lo, hi));
    return 1;
}

int32_t aver_phys_vehicle_flip(int32_t v, float liftCm) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    if (!finite(liftCm)) return fail(aver::AbiError::InvalidArgument);
    float pos[3], q[4];
    if (!aver_phys_vehicle_pose(v, pos, q)) return 0;
    const Vec3 f = Quat(q[0], q[1], q[2], q[3]).rotate(Vec3(1.0f, 0.0f, 0.0f));
    const float yaw = std::atan2(f.y, f.x);
    const Quat upright = Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), yaw);
    return aver_phys_vehicle_set_pose(v, pos[0], pos[1], pos[2] + liftCm, upright.x, upright.y, upright.z, upright.w);
}

int32_t aver_phys_vehicle_state(int32_t v, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    const int32_t cf = std::clamp(nF, 0, static_cast<int32_t>(AVER_PHYS_VS_COUNT));
    const int32_t ci = std::clamp(nI, 0, static_cast<int32_t>(AVER_PHYS_VSI_COUNT));
    if (outF) for (int32_t i = 0; i < cf; ++i) outF[i] = e->vs[i];
    if (outI) for (int32_t i = 0; i < ci; ++i) outI[i] = e->vsi[i];
    return outF ? cf : 0;
}

int32_t aver_phys_vehicle_wheel_state(int32_t v, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    if (wheel < 0 || static_cast<usize>(wheel) >= e->live.size()) { fail(aver::AbiError::OutOfRange); return 0; }
    const VehicleWheelLive& L = e->live[static_cast<usize>(wheel)];
    const int32_t cf = std::clamp(nF, 0, static_cast<int32_t>(AVER_PHYS_VWS_COUNT));
    const int32_t ci = std::clamp(nI, 0, static_cast<int32_t>(AVER_PHYS_VWSI_COUNT));
    if (outF) for (int32_t i = 0; i < cf; ++i) outF[i] = L.f[i];
    if (outI) for (int32_t i = 0; i < ci; ++i) outI[i] = L.i[i];
    return outF ? cf : 0;
}

int32_t aver_phys_vehicle_event_count(void) {
    return g_world ? static_cast<int32_t>(g_world->vehicleEvents.size()) : 0;
}

int32_t aver_phys_vehicle_event_get(int32_t index, int32_t* outI4, float* outF8) {
    if (!g_world || index < 0 || static_cast<usize>(index) >= g_world->vehicleEvents.size()) return 0;
    const VehicleEventRec& r = g_world->vehicleEvents[static_cast<usize>(index)];
    if (outI4) { outI4[0] = r.kind; outI4[1] = r.vehicle; outI4[2] = r.a; outI4[3] = r.b; }
    if (outF8) for (int i = 0; i < 8; ++i) outF8[i] = r.f[i];
    return 1;
}

} // extern "C"
