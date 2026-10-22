// PhysicsVehicle.cpp -- the vehicle ABI: Jolt's wheeled VehicleConstraint behind the handle convention
// every other file in this module uses.
//
// A VEHICLE HANDLE IS DRAWN FROM THE SAME COUNTER AS BODIES AND CHARACTERS (World::nextHandle) and lives
// in its own table (World::vehicles), unlike joints, which keep a counter of their own. The reason is the
// one the chassis forces: it is an ordinary body made by addBody, so a raycast, a contact event,
// aver_phys_set_entity and a character riding it all work on it unchanged -- and a vehicle handle that
// can never equal a body handle makes "which family is this" a question nobody has to ask.
//
// WHAT IS NOT OBVIOUS BELOW, and earns the comments:
//
//   * AXES. Jolt's vehicle code assumes a car faces +Z with +Y up, which is wrong here twice over: the
//     engine's forward is +X and its up is +Z, and the engine-to-Jolt map (Convert.hpp) sends them to
//     Jolt's -Z and +Y. Every direction handed to Jolt below -- the vehicle's own forward and up, and
//     each wheel's forward, up, steering axis and suspension direction -- is an ENGINE axis run through
//     toJoltUnit. Left at Jolt's defaults the wheels would roll sideways.
//   * ORIGIN AND CENTRE OF MASS. Jolt places wheels relative to the body ORIGIN and simulates about the
//     CENTRE OF MASS. The chassis is built so the origin is the bottom centre of the car and the centre of
//     mass is wherever the caller put it, which is what lets the pose calls report the place a mesh's own
//     origin should be.
//   * STEERING SIGN. WheeledVehicleController steers by SetSteerAngle(-right * maxSteer), and a wheel's
//     forward is its mWheelForward turned about the steering axis by that angle. With forward = Jolt -Z
//     and the steering axis = Jolt +Y, a POSITIVE Jolt angle turns the forward toward Jolt -X (engine
//     -Y, the car's left), so the controller's minus sign makes +1 on `right` turn it toward engine +Y.
//     Derived on paper, and proved by VehicleTest rather than trusted.
//   * TEARDOWN ORDER, because the constraint is also a step listener holding a raw pointer to the chassis:
//     step listener out, constraint out, THEN the body. destroyAllVehicles and releaseVehicleOfBody at the
//     foot of this file are the only two places that do it.
//   * v2 (physics_vehicle_ext_abi.h): everything a vehicle describes beyond v1 is stored in VehicleEntry and
//     consumed HERE, by _finish. The per-step half (driver aids, aero, telemetry, events) is
//     PhysicsVehicleExt.cpp; the tracked controller is PhysicsVehicleTracked.cpp.
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/physics/physics_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsSurface.hpp"   // the grip reads the leaf material under the tyre

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// More wheels than this is a typo or a different kind of machine, and a refusal is kinder than a
// vehicle that quietly costs forty shape casts a step.
constexpr usize kMaxWheels = 16;

// Two wheels are one axle when their attachment points agree in x to within this, cm.
constexpr float kAxleToleranceCm = 5.0f;

// Jolt asserts |max steer| <= 90 degrees; a wheel turned all the way round steers nothing.
constexpr float kMaxSteerDegCeiling = 89.0f;

// JOLT'S OWN DEFAULT SURFACE FRICTION (BodyCreationSettings::mFriction) -- what every collider this
// engine builds has, because nothing sets it. A tyre is measured against it, not against 1: see the
// combine function in aver_phys_vehicle_finish.
constexpr float kDefaultSurfaceFriction = 0.2f;

// Jolt's engine inertia and clutch strength are tuned for a 500 N*m engine; they scale with the torque so
// a bigger engine slips its clutch by the same fraction of a rev range instead of by more of it.
constexpr float kReferenceEngineTorque = 500.0f;

// A limited-slip ratio of 1.01 is as near a hard lock as Jolt can be asked for (its ratio must exceed 1).
constexpr float kLockedSlipRatio = 1.01f;

constexpr int kMaxChassisBoxes = 8;

bool allFinite(std::initializer_list<float> values) {
    for (float f : values)
        if (!std::isfinite(f)) return false;
    return true;
}

bool allFinite(const float* p, int32_t n) {
    for (int32_t i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

// A driver input, clamped; a non-finite one is 0 rather than a NaN inside the solver.
float clampInput(float v, float lo, float hi) {
    if (!std::isfinite(v)) return 0.0f;
    return std::clamp(v, lo, hi);
}

// Takes a finished vehicle out of the simulation: step listener first, then the constraint. Leaves the
// chassis body alone. The order matters because the listener holds a raw pointer into the constraint.
void detachConstraint(VehicleEntry& e) {
    if (e.constraint == nullptr) return;
    g_world->system.RemoveStepListener(e.constraint.GetPtr());
    g_world->system.RemoveConstraint(e.constraint.GetPtr());
    noteConstraintTopology();   // a state restore refuses to cross this (physics_state_abi.h)
    e.constraint = nullptr;
}

// The wheel's extension record, grown on demand so a vehicle that never asks for one allocates none.
VehicleWheelExt& wheelExtOf(VehicleEntry& e, usize i) {
    if (e.wheelExt.size() <= i) e.wheelExt.resize(i + 1);
    return e.wheelExt[i];
}

const VehicleAxleDesc& axleDescOf(const VehicleEntry& e, usize axle) {
    static const VehicleAxleDesc kDefault;
    return axle < e.axleDesc.size() ? e.axleDesc[axle] : kDefault;
}

// Jolt's limited-slip ratio for a differential mode: open is "never", locked is as close to a lock as it goes.
float slipRatioFor(int32_t mode, float lsd) {
    if (mode == 0) return FLT_MAX;
    if (mode == 2) return kLockedSlipRatio;
    return lsd;
}

void setCurve(JPH::LinearCurve& curve, const std::vector<float>& xs, const std::vector<float>& ys) {
    curve.Clear();
    curve.Reserve(static_cast<JPH::uint>(xs.size()));
    for (usize i = 0; i < xs.size(); ++i) curve.AddPoint(xs[i], ys[i]);
}

// The wheeled build: wheel settings, differentials, anti-roll bars and the controller settings.
void buildWheeled(VehicleEntry& e, JPH::VehicleConstraintSettings& settings, const std::vector<VehicleAxlePair>& axles,
                  const JPH::Vec3& up, const JPH::Vec3& fwd, const JPH::Vec3& down) {
    for (usize wi = 0; wi < e.wheels.size(); ++wi) {
        const VehicleWheelDesc& d = e.wheels[wi];
        const VehicleWheelExt& x = wheelExtOf(e, wi);
        JPH::Ref<JPH::WheelSettingsWV> w = new JPH::WheelSettingsWV;
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
        w->mMaxSteerAngle = JPH::DegreesToRadians(d.maxSteerDeg);
        w->mMaxBrakeTorque = d.maxBrakeNm * x.brakeBias;
        w->mMaxHandBrakeTorque = d.maxHandBrakeNm;
        w->mInertia = x.inertia;
        w->mAngularDamping = x.angDamping;
        if (!x.longSlip.empty()) {
            setCurve(w->mLongitudinalFriction, x.longSlip, x.longMu);
            setCurve(w->mLateralFriction, x.latDeg, x.latMu);
        } else if (!e.allLongSlip.empty()) {
            setCurve(w->mLongitudinalFriction, e.allLongSlip, e.allLongMu);
            setCurve(w->mLateralFriction, e.allLatDeg, e.allLatMu);
        }
        e.wheelSettings.push_back(w.GetPtr());
        e.baseSpringHz.push_back(d.suspHz);
        settings.mWheels.push_back(w.GetPtr());
    }

    // THE DRIVELINE. Engine and gearbox from the builder's numbers; a differential on every axle that has
    // a driven wheel, a side that is not driven left out of it; an anti-roll bar on every complete axle.
    JPH::Ref<JPH::WheeledVehicleControllerSettings> ctl = new JPH::WheeledVehicleControllerSettings;
    vehicleFillPowertrain(e, ctl->mEngine, ctl->mTransmission);

    std::vector<usize> drivenAxle;   // axle index of each differential, in order
    for (usize ai = 0; ai < axles.size(); ++ai) {
        const VehicleAxlePair& a = axles[ai];
        const VehicleAxleDesc& ad = axleDescOf(e, ai);
        const bool leftDriven  = a.left  >= 0 && e.wheels[static_cast<usize>(a.left)].driven;
        const bool rightDriven = a.right >= 0 && e.wheels[static_cast<usize>(a.right)].driven;
        if (leftDriven || rightDriven) {
            JPH::VehicleDifferentialSettings d;
            d.mLeftWheel  = leftDriven  ? a.left  : -1;
            d.mRightWheel = rightDriven ? a.right : -1;
            d.mDifferentialRatio = ad.finalDrive;
            d.mLeftRightSplit = ad.split;
            d.mLimitedSlipRatio = slipRatioFor(ad.diffMode, ad.lsdRatio);
            ctl->mDifferentials.push_back(d);
            drivenAxle.push_back(ai);
        }
        if (a.left >= 0 && a.right >= 0 && ad.antiRoll > 0.0f) {
            JPH::VehicleAntiRollBar bar;
            bar.mLeftWheel = a.left;
            bar.mRightWheel = a.right;
            bar.mStiffness = ad.antiRoll;
            settings.mAntiRollBars.push_back(bar);
        }
    }

    // THE ENGINE'S TORQUE SPLIT between the driven axles: equal unless any axle asked for a share, in which
    // case the shares are normalised and an axle that asked for none gets none.
    float shareSum = 0.0f;
    for (usize ai : drivenAxle) shareSum += std::max(0.0f, axleDescOf(e, ai).share);
    for (usize k = 0; k < ctl->mDifferentials.size(); ++k)
        ctl->mDifferentials[k].mEngineTorqueRatio =
            shareSum > 0.0f ? std::max(0.0f, axleDescOf(e, drivenAxle[k]).share) / shareSum
                            : 1.0f / static_cast<float>(ctl->mDifferentials.size());
    if (e.centerSet) ctl->mDifferentialLimitedSlipRatio = e.centerLsd;
    settings.mController = ctl.GetPtr();
}

} // namespace

namespace aver::physics::detail {

// The vehicle behind a handle, or nullptr. Records why it failed the way findBody does, so
// aver_phys_last_error answers for these calls too.
VehicleEntry* findVehicle(int32_t h) {
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return nullptr; }
    auto it = g_world->vehicles.find(h);
    if (it == g_world->vehicles.end()) { aver::setAbiError(aver::AbiError::BadHandle); return nullptr; }
    aver::setAbiError(aver::AbiError::Ok);
    return &it->second;
}

// The vehicle behind a handle, but only once _finish has built its constraint.
VehicleEntry* findFinished(int32_t h) {
    VehicleEntry* e = findVehicle(h);
    return (e && e->constraint != nullptr) ? e : nullptr;
}

// Groups the wheels into axles by position, which is what a differential and an anti-roll bar are
// defined over. Positions rather than a flag because the ABI describes a wheel by where it is, and a car
// built wheel by wheel in any order still has a front pair and a rear pair. FRONT (LARGEST X) FIRST, so the
// axle number the v2 ABI speaks of does not depend on the order the wheels were added in.
std::vector<VehicleAxlePair> vehicleAxles(const std::vector<VehicleWheelDesc>& wheels) {
    std::vector<VehicleAxlePair> axles;
    std::vector<bool> taken(wheels.size(), false);
    for (usize i = 0; i < wheels.size(); ++i) {
        if (taken[i]) continue;
        taken[i] = true;
        VehicleAxlePair a;
        (wheels[i].attachCm.y > 0.0f ? a.right : a.left) = static_cast<int>(i);
        for (usize j = i + 1; j < wheels.size(); ++j) {
            if (taken[j]) continue;
            const float yi = wheels[i].attachCm.y, yj = wheels[j].attachCm.y;
            const bool oppositeSides = (yi < 0.0f && yj > 0.0f) || (yi > 0.0f && yj < 0.0f);
            const bool sameX = std::fabs(wheels[j].attachCm.x - wheels[i].attachCm.x) <= kAxleToleranceCm;
            if (!oppositeSides || !sameX) continue;
            (yj > 0.0f ? a.right : a.left) = static_cast<int>(j);
            taken[j] = true;
            break;
        }
        axles.push_back(a);
    }
    auto xOf = [&](const VehicleAxlePair& a) {
        return wheels[static_cast<usize>(a.left >= 0 ? a.left : a.right)].attachCm.x;
    };
    std::stable_sort(axles.begin(), axles.end(),
                     [&](const VehicleAxlePair& l, const VehicleAxlePair& r) { return xOf(l) > xOf(r); });
    return axles;
}

// The engine and gearbox a vehicle describes. Anything never set is what v1 built: Jolt's curve and ratios,
// inertia and clutch strength scaled by torque / 500, shift points at 20% and 60% of the rev range.
void vehicleFillPowertrain(const VehicleEntry& e, JPH::VehicleEngineSettings& engine,
                           JPH::VehicleTransmissionSettings& trans) {
    const float range = e.engineMaxRpm - e.engineMinRpm;
    const float torqueScale = e.engineMaxTorque / kReferenceEngineTorque;
    engine.mMaxTorque = e.engineMaxTorque;
    engine.mMinRPM = e.engineMinRpm;
    engine.mMaxRPM = e.engineMaxRpm;
    engine.mInertia = e.engineInertia > 0.0f ? e.engineInertia : engine.mInertia * torqueScale;
    engine.mAngularDamping = e.engineDamping;
    if (!e.curveX.empty()) setCurve(engine.mNormalizedTorque, e.curveX, e.curveY);

    trans.mMode = e.manual ? JPH::ETransmissionMode::Manual : JPH::ETransmissionMode::Auto;
    if (!e.gearFwd.empty()) {
        trans.mGearRatios.clear();
        for (float r : e.gearFwd) trans.mGearRatios.push_back(r);
    }
    if (!e.gearRev.empty()) {
        trans.mReverseGearRatios.clear();
        for (float r : e.gearRev) trans.mReverseGearRatios.push_back(-r);
    }
    trans.mSwitchTime = e.gearbox[AVER_PHYS_VG_SWITCH_TIME];
    trans.mClutchReleaseTime = e.gearbox[AVER_PHYS_VG_CLUTCH_RELEASE_TIME];
    trans.mSwitchLatency = e.gearbox[AVER_PHYS_VG_SWITCH_LATENCY];
    trans.mClutchStrength = e.gearbox[AVER_PHYS_VG_CLUTCH_STRENGTH] > 0.0f
        ? e.gearbox[AVER_PHYS_VG_CLUTCH_STRENGTH] : trans.mClutchStrength * torqueScale;
    trans.mShiftDownRPM = e.engineMinRpm + e.gearbox[AVER_PHYS_VG_SHIFT_DOWN_FRACTION] * range;
    trans.mShiftUpRPM = e.engineMinRpm + e.gearbox[AVER_PHYS_VG_SHIFT_UP_FRACTION] * range;
}

} // namespace aver::physics::detail

extern "C" {

// ---- Building ---------------------------------------------------------------------------------------

int32_t aver_phys_vehicle_create(float hx, float hy, float hz, float groundClearance,
                                 float comX, float comY, float comZ, float massKg,
                                 float x, float y, float z,
                                 float qx, float qy, float qz, float qw) {
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return 0; }
    if (!allFinite({hx, hy, hz, groundClearance, comX, comY, comZ, massKg, x, y, z, qx, qy, qz, qw}) ||
        hx <= 0.0f || hy <= 0.0f || hz <= 0.0f || massKg <= 0.0f || groundClearance < 0.0f) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }

    // THE CHASSIS: a box lifted off the origin by its ground clearance, with the centre of mass moved
    // to where the caller asked. Three shapes, inside out -- the box; the box translated up so the body
    // origin stays at the bottom centre of the car; and an offset of the centre of mass from the box
    // centre to the requested point. Half extents are a SIZE, so the axis permutation applies and the
    // sign flip does not, exactly as in aver_phys_add_dynamic_box.
    const JPH::Vec3 half(cmToM(hy), cmToM(hz), cmToM(hx));
    const JPH::Vec3 boxCentre = toJolt(Vec3(0.0f, 0.0f, hz + groundClearance));
    const JPH::Vec3 com = toJolt(Vec3(comX, comY, comZ));
    const JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(half);
    const JPH::RefConst<JPH::Shape> lifted =
        new JPH::RotatedTranslatedShape(boxCentre, JPH::Quat::sIdentity(), box.GetPtr());
    const JPH::RefConst<JPH::Shape> shape = new JPH::OffsetCenterOfMassShape(lifted.GetPtr(), com - boxCentre);

    // addBody takes the ORIGIN position, despite its name, and always creates at identity rotation, so the
    // pose is applied once the body exists.
    const Vec3 origin(x, y, z);
    const int32_t body = addBody(shape.GetPtr(), origin, true, massKg);
    if (body == 0) return 0;
    const JPH::BodyID id = g_world->bodies.at(body);
    const Quat q = Quat(qx, qy, qz, qw).normalized();
    bi().SetPositionAndRotation(id, toJolt(origin), toJolt(q), JPH::EActivation::Activate);

    const int32_t v = g_world->nextHandle++;
    VehicleEntry entry;
    entry.body = body;
    entry.halfCm[0] = hx; entry.halfCm[1] = hy; entry.halfCm[2] = hz;
    entry.clearanceCm = groundClearance;
    entry.comCm[0] = comX; entry.comCm[1] = comY; entry.comCm[2] = comZ;
    entry.massKg = massKg;
    g_world->vehicles.emplace(v, std::move(entry));
    g_world->vehicleOfBody.emplace(body, v);
    return v;
}

int32_t aver_phys_vehicle_add_wheel(int32_t v, float px, float py, float pz,
                                    float radius, float width,
                                    float suspensionMin, float suspensionMax,
                                    float suspensionHz, float suspensionDamping,
                                    float maxSteerDeg, float maxBrakeTorque,
                                    float maxHandBrakeTorque, int32_t driven) {
    VehicleEntry* e = findVehicle(v);
    if (!e || e->constraint != nullptr) return -1;
    if (e->wheels.size() >= kMaxWheels) { aver::setAbiError(aver::AbiError::OutOfRange); return -1; }
    if (!allFinite({px, py, pz, radius, width, suspensionMin, suspensionMax, suspensionHz,
                    suspensionDamping, maxSteerDeg, maxBrakeTorque, maxHandBrakeTorque}) ||
        radius <= 0.0f || width <= 0.0f || suspensionMin < 0.0f || suspensionMax <= suspensionMin ||
        suspensionHz <= 0.0f || suspensionDamping < 0.0f || maxBrakeTorque < 0.0f ||
        maxHandBrakeTorque < 0.0f) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return -1;
    }

    VehicleWheelDesc w;
    w.attachCm = Vec3(px, py, pz);
    w.radiusCm = radius;
    w.widthCm = width;
    w.suspMinCm = suspensionMin;
    w.suspMaxCm = suspensionMax;
    w.suspHz = suspensionHz;
    w.suspDamping = suspensionDamping;
    w.maxSteerDeg = std::clamp(maxSteerDeg, 0.0f, kMaxSteerDegCeiling);
    w.maxBrakeNm = maxBrakeTorque;
    w.maxHandBrakeNm = maxHandBrakeTorque;
    w.driven = driven != 0;
    e->wheels.push_back(w);
    return static_cast<int32_t>(e->wheels.size()) - 1;
}

int32_t aver_phys_vehicle_set_engine(int32_t v, float maxTorque, float minRpm, float maxRpm) {
    VehicleEntry* e = findVehicle(v);
    if (!e || e->constraint != nullptr) return 0;
    if (!allFinite({maxTorque, minRpm, maxRpm}) || maxTorque <= 0.0f || minRpm < 0.0f || maxRpm <= minRpm) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }
    e->engineMaxTorque = maxTorque;
    e->engineMinRpm = minRpm;
    e->engineMaxRpm = maxRpm;
    return 1;
}

// Replaces create()'s box with up to eight, mass and centre of mass unchanged. The body is REBUILT under the
// same handle (the shape swap that keeps a body's mass properties is a trap of its own), so this is refused
// after _finish, when a constraint holds the old one.
int32_t aver_phys_vehicle_set_chassis_boxes(int32_t v, const float* boxes6, int32_t count) {
    VehicleEntry* e = findVehicle(v);
    if (!e || e->constraint != nullptr) return 0;
    if (!boxes6 || count < 1 || count > kMaxChassisBoxes) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    if (!allFinite(boxes6, count * 6)) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    for (int32_t i = 0; i < count; ++i)
        if (boxes6[i * 6 + 3] <= 0.0f || boxes6[i * 6 + 4] <= 0.0f || boxes6[i * 6 + 5] <= 0.0f) {
            aver::setAbiError(aver::AbiError::InvalidArgument);
            return 0;
        }

    // The sub-shape is built here and the compound handed the SHAPE (PhysicsShapes.cpp's buildBoxCompound
    // says why: the settings overload keeps a pointer to a loop-local).
    JPH::StaticCompoundShapeSettings compound;
    for (int32_t i = 0; i < count; ++i) {
        const float* b = boxes6 + i * 6;
        JPH::BoxShapeSettings box(JPH::Vec3(cmToM(b[4]), cmToM(b[5]), cmToM(b[3])));
        box.SetEmbedded();
        JPH::ShapeSettings::ShapeResult sub = box.Create();
        if (sub.HasError()) { AVER_WARN("[Physics] vehicle {} chassis box {}: {}", v, i, sub.GetError().c_str()); return 0; }
        compound.AddShape(toJolt(Vec3(b[0], b[1], b[2])), JPH::Quat::sIdentity(), sub.Get());
    }
    auto res = compound.Create();
    if (res.HasError()) { AVER_WARN("[Physics] vehicle {} chassis: {}", v, res.GetError().c_str()); return 0; }
    const JPH::RefConst<JPH::Shape> inner = res.Get();
    const JPH::Vec3 com = toJolt(Vec3(e->comCm[0], e->comCm[1], e->comCm[2]));
    const JPH::RefConst<JPH::Shape> shape = new JPH::OffsetCenterOfMassShape(inner.GetPtr(), com - inner->GetCenterOfMass());

    const auto old = g_world->bodies.find(e->body);
    if (old == g_world->bodies.end()) { aver::setAbiError(aver::AbiError::BadHandle); return 0; }
    const JPH::BodyID oldId = old->second;
    JPH::BodyCreationSettings s(shape.GetPtr(), bi().GetPosition(oldId), bi().GetRotation(oldId),
                                JPH::EMotionType::Dynamic, Layers::encode(0, true));
    s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    s.mMassPropertiesOverride.mMass = e->massKg;
    s.mUserData = bi().GetUserData(oldId);
    JPH::Body* nb = bi().CreateBody(s);
    if (!nb) { AVER_WARN("[Physics] vehicle {}: no body left for the new chassis", v); return 0; }
    bi().AddBody(nb->GetID(), JPH::EActivation::Activate);

    bi().RemoveBody(oldId);
    bi().DestroyBody(oldId);
    g_world->byId.erase(oldId);
    old->second = nb->GetID();
    g_world->byId[nb->GetID()] = e->body;

    e->boxes.assign(boxes6, boxes6 + count * 6);
    return 1;
}

int32_t aver_phys_vehicle_finish(int32_t v, float maxPitchRollDeg) {
    VehicleEntry* e = findVehicle(v);
    if (!e || e->constraint != nullptr) return 0;
    if (e->wheels.empty()) {
        AVER_WARN("[Physics] vehicle {} has no wheels", v);
        return 0;
    }
    const bool tracked = e->kind == AVER_PHYS_VEH_KIND_TRACKED;
    if (!tracked) {
        const bool anyDriven = std::any_of(e->wheels.begin(), e->wheels.end(),
                                           [](const VehicleWheelDesc& w) { return w.driven; });
        if (!anyDriven) {
            AVER_WARN("[Physics] vehicle {} has no driven wheel, so its engine would be connected to nothing", v);
            return 0;
        }
    }
    const JPH::BodyID* id = findBody(e->body);
    if (!id) return 0;

    // THE VEHICLE'S OWN AXES, once, as engine directions through toJoltUnit -- see the file comment.
    const JPH::Vec3 fwd  = toJoltUnit(Vec3(1.0f, 0.0f, 0.0f));
    const JPH::Vec3 up   = toJoltUnit(Vec3(0.0f, 0.0f, 1.0f));
    const JPH::Vec3 down = toJoltUnit(Vec3(0.0f, 0.0f, -1.0f));

    JPH::VehicleConstraintSettings settings;
    settings.mUp = up;
    settings.mForward = fwd;
    settings.mMaxPitchRollAngle = (maxPitchRollDeg > 0.0f && maxPitchRollDeg < 180.0f)
        ? JPH::DegreesToRadians(maxPitchRollDeg) : JPH::JPH_PI;   // pi is Jolt's "off"

    e->wheelSettings.clear();
    e->baseSpringHz.clear();
    e->maxPitchRollDeg = maxPitchRollDeg;
    if (tracked) {
        if (!vehicleBuildTracked(*e, settings, v)) return 0;
    } else {
        buildWheeled(*e, settings, vehicleAxles(e->wheels), up, fwd, down);
    }

    // THE CONSTRAINT keeps a pointer to the chassis, so it is built under a write lock for the body and
    // that lock is let go before the world is told about it (AddConstraint takes its own).
    JPH::Ref<JPH::VehicleConstraint> vc;
    {
        JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return 0;
        vc = new JPH::VehicleConstraint(lock.GetBody(), settings);
    }

    // A CYLINDER CAST on the default moving layer: the road, kerbs and other cars, never the chassis
    // itself and never a sensor. Wider than a ray, so a tyre meets a kerb the way a tyre does.
    vc->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(Layers::MOVING));

    // THE SURFACE'S GRIP, measured against the default rather than against 1 -- see the header. Jolt's
    // own combine is sqrt(tyre * surface), which on a stock collider (friction 0.2) leaves a tyre with 0.49
    // of the grip it was built for and makes every road in the engine a wet one. Here a surface at or
    // above the default leaves the tyre alone, and one below it takes the proportion away. GRIP_SCALE
    // (_tune) multiplies both curves here, so it needs no curve rebuild; the pointer is into a node of
    // World::vehicles, which outlives the constraint (detachConstraint runs before the entry is erased).
    const float* gripScale = &e->tune[AVER_PHYS_VU_GRIP_SCALE];
    vc->SetCombineFriction([gripScale](JPH::uint, float& longitudinal, float& lateral, const JPH::Body& surface,
                                       const JPH::SubShapeID& subShape) {
        const float grip = std::min(1.0f, surfaceFrictionOf(surface, subShape, surface.GetFriction()) / kDefaultSurfaceFriction) * *gripScale;
        longitudinal *= grip;
        lateral *= grip;
    });

    if (e->collisionPeriod > 1) {
        vc->SetNumStepsBetweenCollisionTestActive(static_cast<JPH::uint>(e->collisionPeriod));
        vc->SetNumStepsBetweenCollisionTestInactive(static_cast<JPH::uint>(e->collisionPeriod));
    }

    g_world->system.AddConstraint(vc.GetPtr());
    g_world->system.AddStepListener(vc.GetPtr());
    noteConstraintTopology();   // the vehicle constraint was added just above: a state restore refuses to cross this (physics_state_abi.h)
    e->constraint = vc;
    vehicleOnFinished(*e);
    return 1;
}

int32_t aver_phys_vehicle_destroy(int32_t v) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    const int32_t body = e->body;
    releaseVehicleOfBody(body);   // `e` is gone from here
    aver_phys_remove_body(body);
    return 1;
}

int32_t aver_phys_vehicle_body(int32_t v) {
    VehicleEntry* e = findVehicle(v);
    return e ? e->body : 0;
}

// ---- Driving ---------------------------------------------------------------------------------------

int32_t aver_phys_vehicle_set_input(int32_t v, float forward, float right, float brake, float handbrake) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    const float f = clampInput(forward, -1.0f, 1.0f);
    const float r = clampInput(right, -1.0f, 1.0f);
    const float b = clampInput(brake, 0.0f, 1.0f);
    const float h = clampInput(handbrake, 0.0f, 1.0f);
    e->reqT = f; e->reqS = r; e->reqB = b; e->reqH = h;
    if (e->assist[AVER_PHYS_VD_ENABLED] > 0.5f) {
        // The aids turn these into what Jolt sees in the next fixed step; a sleeping chassis is woken now.
        if (f != 0.0f || (e->kind == AVER_PHYS_VEH_KIND_TRACKED && r != 0.0f))
            if (const JPH::BodyID* id = findBody(e->body))
                if (!bi().IsActive(*id)) bi().ActivateBody(*id);
        return 1;
    }
    // Aids off: exactly v1. Jolt applies these on every fixed step until they are set again.
    e->appT = f; e->appS = r; e->appB = b; e->appH = h;
    vehicleApplyInput(*e, f, r, b, h);
    return 1;
}

// ---- Reading it back ----------------------------------------------------------------------------------

int32_t aver_phys_vehicle_pose(int32_t v, float* outXyz, float* outQuat) {
    VehicleEntry* e = findVehicle(v);
    if (!e || !outXyz || !outQuat) return 0;
    const JPH::BodyID* id = findBody(e->body);
    if (!id) return 0;
    // GetPosition is the body ORIGIN; the centre of mass is what aver_phys_body_position reports.
    writeVec(outXyz, fromJolt(bi().GetPosition(*id)));
    const Quat q = fromJolt(bi().GetRotation(*id));
    outQuat[0] = q.x; outQuat[1] = q.y; outQuat[2] = q.z; outQuat[3] = q.w;
    return 1;
}

int32_t aver_phys_vehicle_velocity(int32_t v, float* outXyz) {
    VehicleEntry* e = findVehicle(v);
    if (!e || !outXyz) return 0;
    const JPH::BodyID* id = findBody(e->body);
    if (!id) return 0;
    writeVec(outXyz, fromJoltDir(bi().GetLinearVelocity(*id)));
    return 1;
}

float aver_phys_vehicle_forward_speed(int32_t v) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0.0f;
    const JPH::BodyID* id = findBody(e->body);
    if (!id) return 0.0f;
    const JPH::Vec3 forward = bi().GetRotation(*id) * toJoltUnit(Vec3(1.0f, 0.0f, 0.0f));
    return mToCm(bi().GetLinearVelocity(*id).Dot(forward));
}

int32_t aver_phys_vehicle_wheel_count(int32_t v) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    return static_cast<int32_t>(e->constraint != nullptr ? e->constraint->GetWheels().size() : e->wheels.size());
}

int32_t aver_phys_vehicle_wheel_pose(int32_t v, int32_t i, float* outXyz, float* outQuat) {
    VehicleEntry* e = findFinished(v);
    if (!e || !outXyz || !outQuat) return 0;
    if (i < 0 || static_cast<usize>(i) >= e->constraint->GetWheels().size()) {
        aver::setAbiError(aver::AbiError::OutOfRange);
        return 0;
    }
    // Jolt's own wheel transform, with the model axes the ABI promises: axle on engine Y, up on engine Z.
    // It is a proper rotation in body-origin space -- the wheel's steering turn, then its spin -- so the
    // rotation converts with the same quaternion map as every body's, and an unsteered wheel at rest comes
    // out as the identity.
    const JPH::Mat44 m = e->constraint->GetWheelLocalTransform(static_cast<JPH::uint>(i),
                                                               toJoltUnit(Vec3(0.0f, 1.0f, 0.0f)),
                                                               toJoltUnit(Vec3(0.0f, 0.0f, 1.0f)));
    writeVec(outXyz, fromJolt(m.GetTranslation()));
    const Quat q = fromJolt(m.GetQuaternion()).normalized();
    outQuat[0] = q.x; outQuat[1] = q.y; outQuat[2] = q.z; outQuat[3] = q.w;
    return 1;
}

int32_t aver_phys_vehicle_wheel_contact(int32_t v, int32_t i) {
    VehicleEntry* e = findFinished(v);
    if (!e) return 0;
    if (i < 0 || static_cast<usize>(i) >= e->constraint->GetWheels().size()) {
        aver::setAbiError(aver::AbiError::OutOfRange);
        return 0;
    }
    return e->constraint->GetWheel(static_cast<JPH::uint>(i))->HasContact() ? 1 : 0;
}

// ---- Teleport ---------------------------------------------------------------------------------------

int32_t aver_phys_vehicle_set_pose(int32_t v, float x, float y, float z,
                                   float qx, float qy, float qz, float qw) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    if (!allFinite({x, y, z, qx, qy, qz, qw})) {
        aver::setAbiError(aver::AbiError::InvalidArgument);
        return 0;
    }
    const JPH::BodyID* id = findBody(e->body);
    if (!id) return 0;
    const Quat q = Quat(qx, qy, qz, qw).normalized();
    bi().SetPositionAndRotation(*id, toJolt(Vec3(x, y, z)), toJolt(q), JPH::EActivation::Activate);
    bi().SetLinearAndAngularVelocity(*id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    // THE BODY IS NOT THE WHOLE VEHICLE. Wheels that kept spinning would drive the car off from its new
    // place at the speed it was going at the old one, and an engine still at its old revs would shift as
    // if it had not been moved.
    //
    // AND THE GEARBOX GOES BACK TO NEUTRAL. Jolt never lets the engine below its idle revs, so an engine
    // left clutched into second or third would pull the stationary wheels up to that gear's idle speed --
    // several metres a second -- until the automatic box had stepped down through every gear on its
    // shift timers. In neutral nothing is connected (the clutch strength is zero at ratio 0), and the box
    // engages first again on the next throttle, as it does for any standing start.
    if (e->constraint != nullptr) vehicleResetDrivetrain(*e);
    return 1;
}

} // extern "C"

// ---- Teardown ---------------------------------------------------------------------------------------

namespace aver::physics::detail {

void releaseVehicleOfBody(int32_t body) {
    if (!g_world) return;
    const auto owner = g_world->vehicleOfBody.find(body);
    if (owner == g_world->vehicleOfBody.end()) return;
    const int32_t v = owner->second;
    g_world->vehicleOfBody.erase(owner);
    const auto it = g_world->vehicles.find(v);
    if (it == g_world->vehicles.end()) return;
    detachConstraint(it->second);
    g_world->vehicles.erase(it);
}

// The constraints come out of the system here; the chassis bodies are ordinary entries in the body table
// and are destroyed with it by aver_phys_shutdown, after this has run.
void destroyAllVehicles() {
    if (!g_world) return;
    for (auto& [handle, entry] : g_world->vehicles) detachConstraint(entry);
    g_world->vehicles.clear();
    g_world->vehicleOfBody.clear();
    g_world->vehicleEvents.clear();
    g_world->vehicleImpacts.clear();
}

} // namespace aver::physics::detail
