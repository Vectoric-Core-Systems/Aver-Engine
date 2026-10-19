// PhysicsVehicleTracked.cpp -- tracked vehicles (tanks): Jolt's TrackedVehicleController behind the same builder
// as the wheeled car. Chosen with aver_phys_vehicle_set_kind before the first wheel; the wheels are the road
// wheels (same add_wheel arguments, steer angle, handbrake torque and driven flag ignored), grouped into a left
// and a right track by aver_phys_vehicle_set_track, one wheel of each being the driven sprocket.
//
// Jolt's controller takes (forward, leftRatio, rightRatio, brake) and asserts both ratios are non-zero.
// The pedals of the ABI (throttle, steer, brake, handbrake) become that tuple in vehicleTrackedInput:
//
//   * moving: the track on the inside of the turn runs at 1 - |steer| * STEER_GAIN of the outside one, never under
//     the 0.05 floor: Jolt keeps the two tracks in their ratio by exchanging speed between them, and a ratio of
//     opposite sign makes that exchange stop the whole vehicle (a moving tank given -0.6 stood still),
//   * standing (or nearly): a PIVOT -- forward demand |steer| * PIVOT_POWER, left ratio +sign, right ratio -sign,
//   * any ratio under 0.05 in magnitude is lifted to 0.05 with its sign.
//
// Engine and gearbox are the wheeled ones (vehicleFillPowertrain); the track's FINAL_DRIVE replaces the differential.
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>

#include <algorithm>
#include <cmath>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr float kMinRatio = 0.05f;
constexpr float kMovingThrottle = 0.05f;
constexpr float kPivotSteer = 0.05f;

JPH::TrackedVehicleController* controllerOf(const VehicleEntry& e) {
    return static_cast<JPH::TrackedVehicleController*>(e.constraint->GetController());
}

float liftRatio(float r) {
    if (std::fabs(r) >= kMinRatio) return r;
    return r < 0.0f ? -kMinRatio : kMinRatio;
}

bool fail(aver::AbiError code) { aver::setAbiError(code); return false; }

} // namespace

namespace aver::physics::detail {

bool vehicleBuildTracked(VehicleEntry& e, JPH::VehicleConstraintSettings& settings, int32_t handle) {
    const usize nWheels = e.wheels.size();
    // Both sides need wheels, the sprocket must be one of its side's, and every wheel belongs to exactly one
    // track (a wheel in no track has no track index in Jolt and would be read through -1).
    std::vector<int> owner(nWheels, -1);
    for (int side = 0; side < 2; ++side) {
        const VehicleTrackDesc& t = e.track[side];
        if (t.wheels.empty()) {
            AVER_WARN("[Physics] vehicle {}: a tracked vehicle needs wheels on both tracks ({} has none)", handle, side == 0 ? "left" : "right");
            return false;
        }
        bool drivenIn = false;
        for (int32_t w : t.wheels) {
            if (w < 0 || static_cast<usize>(w) >= nWheels || owner[static_cast<usize>(w)] != -1) {
                AVER_WARN("[Physics] vehicle {}: track wheel {} is out of range or on two tracks", handle, w);
                return false;
            }
            owner[static_cast<usize>(w)] = side;
            drivenIn |= (w == t.driven);
        }
        if (!drivenIn) {
            AVER_WARN("[Physics] vehicle {}: the driven wheel of the {} track is not one of its wheels", handle, side == 0 ? "left" : "right");
            return false;
        }
    }
    for (usize i = 0; i < nWheels; ++i)
        if (owner[i] == -1) {
            AVER_WARN("[Physics] vehicle {}: wheel {} is on neither track", handle, i);
            return false;
        }

    const JPH::Vec3 fwd  = toJoltUnit(Vec3(1.0f, 0.0f, 0.0f));
    const JPH::Vec3 up   = toJoltUnit(Vec3(0.0f, 0.0f, 1.0f));
    const JPH::Vec3 down = toJoltUnit(Vec3(0.0f, 0.0f, -1.0f));

    for (usize wi = 0; wi < nWheels; ++wi) {
        const VehicleWheelDesc& d = e.wheels[wi];
        JPH::Ref<JPH::WheelSettingsTV> w = new JPH::WheelSettingsTV;
        w->mPosition = toJolt(d.attachCm);
        w->mSuspensionDirection = down;
        w->mSteeringAxis = up;
        w->mWheelUp = up;
        w->mWheelForward = fwd;
        w->mSuspensionMinLength = cmToM(d.suspMinCm);
        w->mSuspensionMaxLength = cmToM(d.suspMaxCm);
        w->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, d.suspHz, d.suspDamping);
        w->mRadius = cmToM(d.radiusCm);
        w->mWidth = cmToM(d.widthCm);
        if (wi < e.wheelExt.size()) {
            w->mLongitudinalFriction = e.wheelExt[wi].tvLong;
            w->mLateralFriction = e.wheelExt[wi].tvLat;
        }
        e.wheelSettings.push_back(w.GetPtr());
        e.baseSpringHz.push_back(d.suspHz);
        settings.mWheels.push_back(w.GetPtr());
    }

    JPH::Ref<JPH::TrackedVehicleControllerSettings> ctl = new JPH::TrackedVehicleControllerSettings;
    vehicleFillPowertrain(e, ctl->mEngine, ctl->mTransmission);
    for (int side = 0; side < 2; ++side) {
        const VehicleTrackDesc& t = e.track[side];
        JPH::VehicleTrackSettings& s = ctl->mTracks[side];
        s.mDrivenWheel = static_cast<JPH::uint>(t.driven);
        s.mWheels.clear();
        for (int32_t w : t.wheels) s.mWheels.push_back(static_cast<JPH::uint>(w));
        s.mInertia = t.inertia;
        s.mAngularDamping = t.damping;
        s.mMaxBrakeTorque = t.brakeNm;
        s.mDifferentialRatio = t.finalDrive;
        e.baseTrackBrake[side] = t.brakeNm;
    }
    settings.mController = ctl.GetPtr();
    return true;
}

void vehicleTrackedInput(VehicleEntry& e, float throttle, float steer, float brake, float handbrake) {
    const float aS = std::fabs(steer);
    float forward = throttle, left = 1.0f, right = 1.0f;
    if (std::fabs(throttle) > kMovingThrottle) {
        // The inside track is the one on the side we turn towards.
        if (steer > 0.0f) right = std::max(kMinRatio, 1.0f - aS * e.track[1].steerGain);
        else if (steer < 0.0f) left = std::max(kMinRatio, 1.0f - aS * e.track[0].steerGain);
    } else if (aS > kPivotSteer) {
        forward = aS * e.track[0].pivot;
        left = steer > 0.0f ? 1.0f : -1.0f;
        right = -left;
    }
    controllerOf(e)->SetDriverInput(forward, liftRatio(left), liftRatio(right), std::max(brake, handbrake));
}

void vehicleTrackSpeeds(const VehicleEntry& e, float& leftCmS, float& rightCmS) {
    leftCmS = rightCmS = 0.0f;
    if (e.constraint == nullptr || e.kind != AVER_PHYS_VEH_KIND_TRACKED) return;
    const JPH::TrackedVehicleController* c = controllerOf(e);
    const JPH::Wheels& wheels = e.constraint->GetWheels();
    float* out[2] = {&leftCmS, &rightCmS};
    for (int side = 0; side < 2; ++side) {
        const JPH::VehicleTrack& t = c->GetTracks()[side];
        if (t.mDrivenWheel >= wheels.size()) continue;
        *out[side] = mToCm(t.mAngularVelocity * wheels[t.mDrivenWheel]->GetSettings()->mRadius);
    }
}

void vehicleTrackedBrakeScale(VehicleEntry& e, float scale) {
    auto& tracks = controllerOf(e)->GetTracks();
    for (int i = 0; i < 2; ++i) tracks[i].mMaxBrakeTorque = e.baseTrackBrake[i] * scale;
}

float vehicleTrackedWheelRpm(const VehicleEntry& e) {
    if (e.constraint == nullptr) return 0.0f;
    const JPH::TrackedVehicleController* c = controllerOf(e);
    float sum = 0.0f;
    for (int side = 0; side < 2; ++side)
        sum += std::fabs(c->GetTracks()[side].mAngularVelocity) * c->GetTracks()[side].mDifferentialRatio;
    return 0.5f * sum * std::fabs(c->GetTransmission().GetCurrentRatio()) * JPH::VehicleEngine::cAngularVelocityToRPM;
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_vehicle_set_kind(int32_t v, int32_t kind) {
    VehicleEntry* e = findVehicle(v);
    if (!e || e->constraint != nullptr) return 0;
    if (kind != AVER_PHYS_VEH_KIND_WHEELED && kind != AVER_PHYS_VEH_KIND_TRACKED) return fail(aver::AbiError::InvalidArgument);
    if (!e->wheels.empty()) return fail(aver::AbiError::InvalidArgument);   // before the first add_wheel
    e->kind = kind;
    return 1;
}

int32_t aver_phys_vehicle_set_track(int32_t v, int32_t side, const int32_t* wheels, int32_t n,
                                    int32_t drivenWheel, const float* p, int32_t np) {
    VehicleEntry* e = findVehicle(v);
    if (!e || e->constraint != nullptr) return 0;
    if (e->kind != AVER_PHYS_VEH_KIND_TRACKED) return fail(aver::AbiError::InvalidArgument);
    if (side < 0 || side > 1 || !wheels || n < 1 || n > 16 || np < 0 || np > AVER_PHYS_VT_COUNT || (np > 0 && !p))
        return fail(aver::AbiError::InvalidArgument);
    bool drivenIn = false;
    for (int32_t i = 0; i < n; ++i) {
        if (wheels[i] < 0 || static_cast<usize>(wheels[i]) >= e->wheels.size()) return fail(aver::AbiError::OutOfRange);
        drivenIn |= (wheels[i] == drivenWheel);
    }
    if (!drivenIn) return fail(aver::AbiError::InvalidArgument);
    VehicleTrackDesc t = e->track[side];
    float b[AVER_PHYS_VT_COUNT] = {t.inertia, t.damping, t.brakeNm, t.finalDrive, t.steerGain, t.pivot};
    for (int32_t i = 0; i < np; ++i) {
        if (!std::isfinite(p[i])) return fail(aver::AbiError::InvalidArgument);
        b[i] = p[i];
    }
    if (b[0] <= 0.0f || b[1] < 0.0f || b[2] < 0.0f || b[3] <= 0.0f || b[4] < 0.0f || b[5] < 0.0f)
        return fail(aver::AbiError::InvalidArgument);
    t.wheels.assign(wheels, wheels + n);
    t.driven = drivenWheel;
    t.inertia = b[0]; t.damping = b[1]; t.brakeNm = b[2]; t.finalDrive = b[3]; t.steerGain = b[4]; t.pivot = b[5];
    e->track[side] = t;
    return 1;
}

} // extern "C"
