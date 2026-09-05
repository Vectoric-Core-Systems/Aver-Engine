// PhysicsJoints.cpp -- the joints ABI: Jolt's twelve constraint classes, plumbed through the same
// handle convention every other file in this module uses.
//
// ITS OWN HANDLE TABLE AND COUNTER, deliberately separate from World::bodies/nextHandle -- see
// physics_joints_abi.h's own comment for why sharing them would be a mistake. aver_phys_joint_* never
// looks in the body/character tables and vice versa.
#include "aver/physics/physics_joints_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/PulleyConstraint.h>
#include <Jolt/Physics/Constraints/PathConstraint.h>
#include <Jolt/Physics/Constraints/PathConstraintPathHermite.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// ---- The joint table -------------------------------------------------------------------------------

// A joint plus the two body handles it was made from -- Jolt's Constraint knows its two JPH::Body*,
// but not the aver_phys handle behind either one, and a joint to the world has no body on that side
// at all. Recording the handles ourselves at creation time is what lets aver_phys_joint_bodies answer
// without trying to reverse-engineer them from Jolt.
struct Joint {
    JPH::Ref<JPH::Constraint> constraint;
    int32_t bodyA = 0;
    int32_t bodyB = 0;
};

std::unordered_map<int32_t, Joint> g_joints;
int32_t g_nextJointHandle = 1;

// A joint's Constraint points into the PhysicsSystem and at the bodies inside it, so the joint table
// must be emptied BEFORE the world is destroyed. aver_phys_shutdown calls detail::destroyAllJoints()
// (defined at the foot of this file) to do exactly that; the guard here is the belt to that braces,
// covering the one case the hook cannot -- a caller reaching for a joint handle when no world was ever
// created at all.
Joint* findJoint(int32_t h) {
    if (!g_world) return nullptr;
    auto it = g_joints.find(h);
    return it == g_joints.end() ? nullptr : &it->second;
}

// ---- Resolving bodies for constraint creation --------------------------------------------------

struct ResolvedBody {
    JPH::BodyID id;
    bool isWorld = false;
    bool ok = false;
};

// Handle 0 means the world -- JPH::Body::sFixedToWorld, a static dummy body that was never added to
// any BodyManager and so has no BodyID and needs no lock. Anything else must resolve through the
// shared body table the way every other ABI file's handles do.
ResolvedBody resolveBody(int32_t handle) {
    if (handle == AVER_PHYS_WORLD_BODY) return ResolvedBody{JPH::BodyID(), true, true};
    if (const JPH::BodyID* id = findBody(handle)) return ResolvedBody{*id, false, true};
    return ResolvedBody{};
}

// Resolves both bodies, locks whichever are real, hands their JPH::Body& to `build` (which returns
// the new constraint, or null to refuse), registers the result with the physics system, and returns
// its handle.
//
// ONE JPH::BodyLockMultiWrite, NOT TWO SEPARATE JPH::BodyLockWrite CALLS: locking bodyA then bodyB here
// while another thread creates a joint on the same pair and locks bodyB then bodyA is a textbook
// lock-order inversion. BodyLockMultiWrite exists in Jolt for exactly this -- it locks an arbitrary
// set of bodies through one combined mutex mask, in one consistent order, regardless of the order
// they're listed in here.
template <typename Build>
int32_t createJoint(int32_t bodyA, int32_t bodyB, Build&& build) {
    if (!g_world) return 0;
    if (bodyA == AVER_PHYS_WORLD_BODY && bodyB == AVER_PHYS_WORLD_BODY) {
        AVER_WARN("[Physics] a joint cannot attach the world to itself");
        return 0;
    }

    const ResolvedBody a = resolveBody(bodyA);
    const ResolvedBody b = resolveBody(bodyB);
    if (!a.ok || !b.ok) return 0;

    JPH::BodyID realIds[2];
    int numReal = 0, idxA = -1, idxB = -1;
    if (!a.isWorld) { idxA = numReal; realIds[numReal++] = a.id; }
    if (!b.isWorld) { idxB = numReal; realIds[numReal++] = b.id; }

    JPH::BodyLockMultiWrite lock(g_world->system.GetBodyLockInterface(), realIds, numReal);
    JPH::Body* jphA = a.isWorld ? &JPH::Body::sFixedToWorld : lock.GetBody(idxA);
    JPH::Body* jphB = b.isWorld ? &JPH::Body::sFixedToWorld : lock.GetBody(idxB);
    if (!jphA || !jphB) return 0;   // a body died between the handle lookup and the lock

    JPH::Ref<JPH::Constraint> constraint = build(*jphA, *jphB);
    if (!constraint) return 0;

    g_world->system.AddConstraint(constraint);
    const int32_t h = g_nextJointHandle++;
    g_joints.emplace(h, Joint{constraint, bodyA, bodyB});
    return h;
}

// ---- Axes ---------------------------------------------------------------------------------------

// Normalises an incoming direction into `outUnit`. Returns false for a zero-length axis, which a
// caller should treat as a refusal (AVER_WARN, return 0) rather than forwarding degenerate data to
// Jolt -- a zero-length hinge/slider/twist axis is a constraint that behaves unpredictably rather
// than one that fails loudly.
bool normalizedAxis(const float axis[3], Vec3& outUnit) {
    outUnit = Vec3(axis[0], axis[1], axis[2]).getSafeNormal();
    return outUnit.sizeSquared() > 0.0f;
}

Vec3 point3(const float p[3]) { return Vec3(p[0], p[1], p[2]); }

// A SixDOFConstraint translation limit of +-1e30 cm (physics_joints_abi.h's own "frees it" sentinel)
// converted through cmToM lands at +-1e28 m -- nowhere near a real motion range, but also nowhere near
// the exact -FLT_MAX/FLT_MAX that SixDOFConstraint::UpdateFixedFreeAxis tests for to put an axis on
// its fast "fully free" path instead of the general ranged one. Recognising the sentinel here keeps a
// truly-unlimited axis on that fast path rather than leaving it merely "unlimited in practice".
constexpr float kSixDofFreeSentinelCm = 1.0e29f;

float sixDofTranslationLimit(float valueCm) {
    if (valueCm <= -kSixDofFreeSentinelCm) return -FLT_MAX;
    if (valueCm >= kSixDofFreeSentinelCm) return FLT_MAX;
    return cmToM(valueCm);
}

} // namespace

extern "C" {

// ---- Lifetime and common state ---------------------------------------------------------------------

int32_t aver_phys_joint_remove(int32_t joint) {
    Joint* j = findJoint(joint);
    if (!j) return 0;
    g_world->system.RemoveConstraint(j->constraint);   // findJoint proved g_world is alive
    g_joints.erase(joint);
    return 1;
}

int32_t aver_phys_joint_count(void) {
    return g_world ? static_cast<int32_t>(g_joints.size()) : 0;
}

int32_t aver_phys_joint_bodies(int32_t joint, int32_t* outBodyA, int32_t* outBodyB) {
    Joint* j = findJoint(joint);
    if (!j) return 0;
    if (outBodyA) *outBodyA = j->bodyA;
    if (outBodyB) *outBodyB = j->bodyB;
    return 1;
}

int32_t aver_phys_joint_set_enabled(int32_t joint, int32_t enabled) {
    Joint* j = findJoint(joint);
    if (!j) return 0;
    j->constraint->SetEnabled(enabled != 0);
    return 1;
}

int32_t aver_phys_joint_enabled(int32_t joint) {
    Joint* j = findJoint(joint);
    if (!j) return 0;   // ambiguous with "disabled", same as aver_phys_body_is_active accepts
    return j->constraint->GetEnabled() ? 1 : 0;
}

// ---- Constraint types --------------------------------------------------------------------------------

int32_t aver_phys_joint_fixed(int32_t bodyA, int32_t bodyB,
                              const float pointCm[3],
                              const float axisX[3], const float axisY[3]) {
    Vec3 ux, uy;
    if (!normalizedAxis(axisX, ux) || !normalizedAxis(axisY, uy)) {
        AVER_WARN("[Physics] fixed joint axis is zero-length");
        return 0;
    }
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::FixedConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = toJolt(point);
        settings.mAxisX1 = settings.mAxisX2 = toJoltUnit(ux);
        settings.mAxisY1 = settings.mAxisY2 = toJoltUnit(uy);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_point(int32_t bodyA, int32_t bodyB, const float pointCm[3]) {
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::PointConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = toJolt(point);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_distance(int32_t bodyA, int32_t bodyB,
                                 const float pointACm[3], const float pointBCm[3],
                                 float minDistanceCm, float maxDistanceCm) {
    const Vec3 pointA = point3(pointACm), pointB = point3(pointBCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::DistanceConstraintSettings settings;
        settings.mPoint1 = toJolt(pointA);
        settings.mPoint2 = toJolt(pointB);
        // A negative distance is Jolt's OWN sentinel for "use the current separation"
        // (DistanceConstraint's constructor checks `< 0.0f`), and cmToM keeps a negative value
        // negative, so the sentinel survives unit conversion without special-casing it here.
        settings.mMinDistance = cmToM(minDistanceCm);
        settings.mMaxDistance = cmToM(maxDistanceCm);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_hinge(int32_t bodyA, int32_t bodyB,
                              const float pointCm[3],
                              const float hingeAxis[3], const float normalAxis[3],
                              float minAngleRad, float maxAngleRad) {
    Vec3 hAxis, nAxis;
    if (!normalizedAxis(hingeAxis, hAxis) || !normalizedAxis(normalAxis, nAxis)) {
        AVER_WARN("[Physics] hinge axis is zero-length");
        return 0;
    }
    // A HINGE THAT CANNOT TURN IS NOT A HINGE, and Jolt says so itself: HingeConstraint.cpp:83
    // asserts mLimitsMin != mLimitsMax unless a limit spring is configured, with the message "Better
    // use a fixed constraint in this case". The clamp below forces min <= 0 <= max, so equality can
    // only be 0 == 0 -- which is exactly what a caller writes when they mean "locked". Refused here,
    // and named, rather than left to fail as an assert in a Debug build and a degenerate constraint
    // in a Release one.
    if (minAngleRad >= 0.0f && maxAngleRad <= 0.0f) {
        AVER_WARN("[Physics] hinge locked at a single angle ({} to {} rad) is not a constraint Jolt "
                  "will build; use aver_phys_joint_fixed for no rotation at all",
                  minAngleRad, maxAngleRad);
        return 0;
    }
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::HingeConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = toJolt(point);
        settings.mHingeAxis1 = settings.mHingeAxis2 = toJoltUnit(hAxis);
        settings.mNormalAxis1 = settings.mNormalAxis2 = toJoltUnit(nAxis);
        // HingeConstraint's constructor calls SetLimits, which asserts inLimitsMin e [-pi, 0] and
        // inLimitsMax e [0, pi]. This header's own "-PI/+PI, or anything wider, means unlimited"
        // would otherwise trip that assert on a debug build for any caller who takes it literally.
        settings.mLimitsMin = std::clamp(minAngleRad, -JPH::JPH_PI, 0.0f);
        settings.mLimitsMax = std::clamp(maxAngleRad, 0.0f, JPH::JPH_PI);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_slider(int32_t bodyA, int32_t bodyB,
                               const float pointCm[3],
                               const float sliderAxis[3], const float normalAxis[3],
                               float minCm, float maxCm) {
    Vec3 sAxis, nAxis;
    if (!normalizedAxis(sliderAxis, sAxis) || !normalizedAxis(normalAxis, nAxis)) {
        AVER_WARN("[Physics] slider axis is zero-length");
        return 0;
    }
    // The same refusal the hinge makes, for the same assert: SliderConstraint.cpp:159 rejects
    // min == max without a limit spring. After the clamp below that can only be 0 == 0, which means
    // a slider with nowhere to slide.
    if (minCm >= 0.0f && maxCm <= 0.0f) {
        AVER_WARN("[Physics] slider locked at a single position ({} to {} cm) is not a constraint Jolt "
                  "will build; use aver_phys_joint_fixed to hold two bodies together",
                  minCm, maxCm);
        return 0;
    }
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::SliderConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = toJolt(point);
        settings.mSliderAxis1 = settings.mSliderAxis2 = toJoltUnit(sAxis);
        settings.mNormalAxis1 = settings.mNormalAxis2 = toJoltUnit(nAxis);
        // SliderConstraint's constructor asserts min <= 0 <= max (0 is where mPoint1 and mPoint2
        // coincide); clamp rather than refuse so a one-sided range (a drawer that only opens, never
        // over-closes) still gets a joint instead of nothing.
        settings.mLimitsMin = std::min(cmToM(minCm), 0.0f);
        settings.mLimitsMax = std::max(cmToM(maxCm), 0.0f);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_cone(int32_t bodyA, int32_t bodyB,
                             const float pointCm[3], const float twistAxis[3],
                             float halfConeAngleRad) {
    Vec3 tAxis;
    if (!normalizedAxis(twistAxis, tAxis)) {
        AVER_WARN("[Physics] cone twist axis is zero-length");
        return 0;
    }
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::ConeConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = toJolt(point);
        settings.mTwistAxis1 = settings.mTwistAxis2 = toJoltUnit(tAxis);
        // ConeConstraint's constructor calls SetHalfConeAngle, which asserts the angle is in [0, pi].
        settings.mHalfConeAngle = std::clamp(halfConeAngleRad, 0.0f, JPH::JPH_PI);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_swing_twist(int32_t bodyA, int32_t bodyB,
                                    const float pointCm[3],
                                    const float twistAxis[3], const float planeAxis[3],
                                    float normalHalfConeRad, float planeHalfConeRad,
                                    float twistMinRad, float twistMaxRad) {
    Vec3 tAxis, pAxis;
    if (!normalizedAxis(twistAxis, tAxis) || !normalizedAxis(planeAxis, pAxis)) {
        AVER_WARN("[Physics] swing-twist axis is zero-length");
        return 0;
    }
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::SwingTwistConstraintSettings settings;
        settings.mPosition1 = settings.mPosition2 = toJolt(point);
        settings.mTwistAxis1 = settings.mTwistAxis2 = toJoltUnit(tAxis);
        settings.mPlaneAxis1 = settings.mPlaneAxis2 = toJoltUnit(pAxis);
        settings.mNormalHalfConeAngle = normalHalfConeRad;
        settings.mPlaneHalfConeAngle = planeHalfConeRad;
        settings.mTwistMinAngle = twistMinRad;
        settings.mTwistMaxAngle = twistMaxRad;
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_six_dof(int32_t bodyA, int32_t bodyB,
                                const float pointCm[3],
                                const float axisX[3], const float axisY[3],
                                const float limitMin[6], const float limitMax[6]) {
    Vec3 ux, uy;
    if (!normalizedAxis(axisX, ux) || !normalizedAxis(axisY, uy)) {
        AVER_WARN("[Physics] six-DOF joint axis is zero-length");
        return 0;
    }
    if (!limitMin || !limitMax) return 0;
    const Vec3 point = point3(pointCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::SixDOFConstraintSettings settings;
        settings.mPosition1 = settings.mPosition2 = toJolt(point);
        settings.mAxisX1 = settings.mAxisX2 = toJoltUnit(ux);
        settings.mAxisY1 = settings.mAxisY2 = toJoltUnit(uy);
        // SixDOFConstraint's own Z axis is cross(AxisX, AxisY) computed AFTER conversion into Jolt's
        // space. Because that conversion flips handedness (Convert.hpp's basis change has determinant
        // -1), that computed Z does not correspond to converting an engine-space "AxisX cross AxisY"
        // the ordinary way. This ABI only ever hands us AxisX and AxisY, never a third axis, so there
        // is no engine-space Z it defines for us to correct against -- TRANSLATION_Z/ROTATION_Z below
        // are Jolt's own Z axis for this frame, taken as-is.
        for (int i = AVER_PHYS_DOF_TRANSLATION_X; i <= AVER_PHYS_DOF_TRANSLATION_Z; ++i) {
            settings.mLimitMin[i] = sixDofTranslationLimit(limitMin[i]);
            settings.mLimitMax[i] = sixDofTranslationLimit(limitMax[i]);
        }
        for (int i = AVER_PHYS_DOF_ROTATION_X; i <= AVER_PHYS_DOF_ROTATION_Z; ++i) {
            // Radians need no scaling, and SixDOFConstraint clamps to [-pi, pi] and zeroes an
            // inverted pair itself (UpdateRotationLimits), so this needs no pre-clamp the way hinge
            // and cone do.
            settings.mLimitMin[i] = limitMin[i];
            settings.mLimitMax[i] = limitMax[i];
        }
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_gear(int32_t bodyA, int32_t bodyB,
                             const float hingeAxisA[3], const float hingeAxisB[3],
                             float ratio) {
    Vec3 axisA, axisB;
    if (!normalizedAxis(hingeAxisA, axisA) || !normalizedAxis(hingeAxisB, axisB)) {
        AVER_WARN("[Physics] gear hinge axis is zero-length");
        return 0;
    }
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::GearConstraintSettings settings;
        settings.mHingeAxis1 = toJoltUnit(axisA);
        settings.mHingeAxis2 = toJoltUnit(axisB);
        // Jolt's own GearConstraintSettings::SetRatio(teeth1, teeth2) computes exactly teeth2/teeth1
        // too, so this header's ratio convention passes straight through unscaled -- it is a pure
        // ratio, dimensionless, so cm never enters into it.
        settings.mRatio = ratio;
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_rack_and_pinion(int32_t bodyA, int32_t bodyB,
                                        const float hingeAxisA[3],
                                        const float sliderAxisB[3],
                                        float ratioRadPerCm) {
    Vec3 hAxis, sAxis;
    if (!normalizedAxis(hingeAxisA, hAxis) || !normalizedAxis(sliderAxisB, sAxis)) {
        AVER_WARN("[Physics] rack-and-pinion axis is zero-length");
        return 0;
    }
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::RackAndPinionConstraintSettings settings;
        settings.mHingeAxis = toJoltUnit(hAxis);
        settings.mSliderAxis = toJoltUnit(sAxis);
        // PinionRotation(t) = mRatio * RackTranslation(t) inside Jolt, with RackTranslation in
        // METRES. This header's ratio is radians per CENTIMETRE of rack travel, i.e. kCmPerMetre
        // times smaller per unit length than what Jolt wants, so it needs multiplying rather than
        // the dividing every position/length conversion in Convert.hpp does.
        settings.mRatio = ratioRadPerCm * kCmPerMetre;
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_pulley(int32_t bodyA, int32_t bodyB,
                               const float bodyPointACm[3], const float fixedPointACm[3],
                               const float bodyPointBCm[3], const float fixedPointBCm[3],
                               float ratio, float minLengthCm, float maxLengthCm) {
    const Vec3 bodyPointA = point3(bodyPointACm), fixedPointA = point3(fixedPointACm);
    const Vec3 bodyPointB = point3(bodyPointBCm), fixedPointB = point3(fixedPointBCm);
    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::PulleyConstraintSettings settings;
        settings.mBodyPoint1 = toJolt(bodyPointA);
        settings.mFixedPoint1 = toJolt(fixedPointA);
        settings.mBodyPoint2 = toJolt(bodyPointB);
        settings.mFixedPoint2 = toJolt(fixedPointB);
        settings.mRatio = ratio;
        // Same negative-means-"use the current length" sentinel as DISTANCE, and for the same
        // reason cmToM preserves it: PulleyConstraint's constructor checks `< 0.0f` on each of
        // mMinLength/mMaxLength independently, after unit conversion has already happened.
        settings.mMinLength = cmToM(minLengthCm);
        settings.mMaxLength = cmToM(maxLengthCm);
        return settings.Create(a, b);
    });
}

int32_t aver_phys_joint_path(int32_t bodyA, int32_t bodyB,
                             const float* pointsCm, int32_t pointCount,
                             int32_t closed, float maxSlideCm) {
    if (!pointsCm || pointCount < 2) {
        AVER_WARN("[Physics] a path needs at least two points, got {}", pointCount);
        return 0;
    }

    // Converted once, up front, in Jolt space -- everything below is differences between these
    // positions (chords for the Hermite tangents, arc length for maxSlideCm), and doing that
    // directly in Jolt's units and axes avoids converting each intermediate direction separately.
    std::vector<JPH::Vec3> pts(static_cast<size_t>(pointCount));
    for (int32_t i = 0; i < pointCount; ++i) pts[i] = toJolt(point3(pointsCm + i * 3));

    const bool loop = closed != 0;

    // maxSlideCm has no Jolt-native equivalent: PathConstraint's own position-limit logic only ever
    // clamps at fraction 0 or GetPathMaxFraction() (the ends of the path as built), and that logic
    // does not even run for a looping path. A finite request on a non-looping path is honoured
    // instead by cutting the polyline itself short, interpolating one final point at the requested
    // arc length -- there is nothing to honour it with on a loop, which has no ends to begin with.
    if (!loop && maxSlideCm >= 0.0f) {
        const float maxSlideM = cmToM(maxSlideCm);
        std::vector<JPH::Vec3> trimmed;
        trimmed.reserve(pts.size());
        trimmed.push_back(pts[0]);
        float travelled = 0.0f;
        bool trimmedShort = false;
        for (size_t i = 1; i < pts.size(); ++i) {
            const float segment = (pts[i] - pts[i - 1]).Length();
            if (segment > 0.0f && travelled + segment >= maxSlideM) {
                const float t = (maxSlideM - travelled) / segment;
                trimmed.push_back(pts[i - 1] + (pts[i] - pts[i - 1]) * t);
                trimmedShort = true;
                break;
            }
            travelled += segment;
            trimmed.push_back(pts[i]);
        }
        if (trimmedShort) pts = std::move(trimmed);
        // else maxSlideCm reaches at or past the whole path -- every point stays, nothing to cut.
    }
    if (pts.size() < 2) {
        AVER_WARN("[Physics] maxSlideCm leaves fewer than two points on the path");
        return 0;
    }

    const int32_t n = static_cast<int32_t>(pts.size());
    JPH::Ref<JPH::PathConstraintPathHermite> path = new JPH::PathConstraintPathHermite();
    path->SetIsLooping(loop);

    for (int32_t i = 0; i < n; ++i) {
        // A Catmull-Rom-style tangent -- half the chord to each neighbour -- makes the Hermite spline
        // pass exactly through every supplied point instead of a smoothed approximation of them.
        // Jolt does not require this to be normalised: GetPointOnPath normalises the INTERPOLATED
        // tangent after evaluating the curve, so only its direction and relative scale between
        // neighbouring points matter, not its absolute length.
        JPH::Vec3 tangent;
        if (loop) tangent = 0.5f * (pts[(i + 1) % n] - pts[(i - 1 + n) % n]);
        else if (i == 0) tangent = pts[1] - pts[0];
        else if (i == n - 1) tangent = pts[n - 1] - pts[n - 2];
        else tangent = 0.5f * (pts[i + 1] - pts[i - 1]);

        // The stored normal only has to be non-parallel to the tangent: GetPointOnPath rebuilds a
        // proper orthonormal frame from it via two cross products every time it evaluates the path.
        // Jolt's up axis (+Y) is a stable reference except where the path runs vertically, so fall
        // back to its +X axis there.
        const JPH::Vec3 tangentDir = tangent.NormalizedOr(JPH::Vec3::sAxisX());
        JPH::Vec3 reference = JPH::Vec3::sAxisY();
        if (std::abs(tangentDir.Dot(reference)) > 0.99f) reference = JPH::Vec3::sAxisX();
        const JPH::Vec3 normal = (reference - tangentDir * reference.Dot(tangentDir)).Normalized();

        path->AddPoint(pts[i], tangent, normal);
    }

    return createJoint(bodyA, bodyB, [&](JPH::Body& a, JPH::Body& b) -> JPH::Ref<JPH::Constraint> {
        JPH::PathConstraintSettings settings;
        settings.mPath = path.GetPtr();

        // PathConstraintSettings has NO EConstraintSpace -- unique among every constraint above.
        // mPathPosition/mPathRotation are ALWAYS body-1-local. To honour physics_joints_abi.h's
        // "every point is world-space at the moment the joint is created" for this one anyway, set
        // them to body 1's CURRENT inverse transform, so that path space and world space coincide
        // right now. If body 1 later moves, the path (and the body riding it) moves rigidly with it,
        // same as every other two-body constraint here -- it just isn't given the choice up front.
        const JPH::Mat44 invBody1 = a.GetInverseCenterOfMassTransform();
        settings.mPathRotation = invBody1.GetQuaternion();
        settings.mPathPosition = invBody1.GetTranslation() + a.GetShape()->GetCenterOfMass();

        // mPathFraction is where body 2 starts on the path; left at its 0 default it would silently
        // snap body 2 to the path's start on the very first solve unless it already happens to be
        // there. Path space is world space right now (see above), so body 2's actual world position
        // is a valid query into the path just built.
        settings.mPathFraction = path->GetClosestPoint(b.GetCenterOfMassPosition(), 0.0f);

        return settings.Create(a, b);
    });
}

// ---- Motors and limits -------------------------------------------------------------------------------

int32_t aver_phys_joint_set_motor(int32_t joint, int32_t axis, int32_t state, float target) {
    Joint* j = findJoint(joint);
    if (!j) return 0;

    JPH::EMotorState motorState;
    switch (state) {
        case AVER_PHYS_MOTOR_OFF:      motorState = JPH::EMotorState::Off;      break;
        case AVER_PHYS_MOTOR_VELOCITY: motorState = JPH::EMotorState::Velocity; break;
        case AVER_PHYS_MOTOR_POSITION: motorState = JPH::EMotorState::Position; break;
        default:
            AVER_WARN("[Physics] motor state {} is not off(0)/velocity(1)/position(2)", state);
            return 0;
    }

    switch (j->constraint->GetSubType()) {
        case JPH::EConstraintSubType::Hinge: {
            auto* hinge = static_cast<JPH::HingeConstraint*>(j->constraint.GetPtr());
            hinge->SetMotorState(motorState);
            if (motorState == JPH::EMotorState::Velocity) hinge->SetTargetAngularVelocity(target);
            else if (motorState == JPH::EMotorState::Position) hinge->SetTargetAngle(target);
            return 1;
        }
        case JPH::EConstraintSubType::Slider: {
            auto* slider = static_cast<JPH::SliderConstraint*>(j->constraint.GetPtr());
            slider->SetMotorState(motorState);
            if (motorState == JPH::EMotorState::Velocity) slider->SetTargetVelocity(cmToM(target));
            else if (motorState == JPH::EMotorState::Position) slider->SetTargetPosition(cmToM(target));
            return 1;
        }
        case JPH::EConstraintSubType::SixDOF: {
            if (axis < 0 || axis >= AVER_PHYS_DOF_COUNT) {
                AVER_WARN("[Physics] six-DOF axis {} is out of range 0-5", axis);
                return 0;
            }
            auto* sixDof = static_cast<JPH::SixDOFConstraint*>(j->constraint.GetPtr());
            sixDof->SetMotorState(static_cast<JPH::SixDOFConstraint::EAxis>(axis), motorState);
            if (axis <= AVER_PHYS_DOF_TRANSLATION_Z) {
                // A single combined constraint-space vector drives all three translation axes at
                // once, so only the ONE requested component is touched here -- read-modify-write, or
                // a motor set on a different axis earlier in this joint's life would be clobbered
                // back to zero.
                if (motorState == JPH::EMotorState::Velocity) {
                    JPH::Vec3 v = sixDof->GetTargetVelocityCS();
                    v.SetComponent(axis, cmToM(target));
                    sixDof->SetTargetVelocityCS(v);
                } else if (motorState == JPH::EMotorState::Position) {
                    JPH::Vec3 p = sixDof->GetTargetPositionCS();
                    p.SetComponent(axis, cmToM(target));
                    sixDof->SetTargetPositionCS(p);
                }
            } else {
                const int rot = axis - AVER_PHYS_DOF_ROTATION_X;
                if (motorState == JPH::EMotorState::Velocity) {
                    JPH::Vec3 av = sixDof->GetTargetAngularVelocityCS();
                    av.SetComponent(rot, target);
                    sixDof->SetTargetAngularVelocityCS(av);
                } else if (motorState == JPH::EMotorState::Position) {
                    // SixDOFConstraint has no per-axis rotation target, only a full target
                    // ORIENTATION (SetTargetOrientationCS). A rotation of `target` radians about
                    // just this one axis is the closest match to "drive this single axis to an
                    // angle" that fits this call's one-scalar-per-axis shape -- it REPLACES whatever
                    // orientation target the joint had, it does not compose with a target set on a
                    // different rotation axis a moment ago.
                    static const JPH::Vec3 kAxis[3] = {JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY(),
                                                        JPH::Vec3::sAxisZ()};
                    sixDof->SetTargetOrientationCS(JPH::Quat::sRotation(kAxis[rot], target));
                }
            }
            return 1;
        }
        default:
            AVER_WARN("[Physics] joint {} has no motor (only hinge, slider and six-DOF do)", joint);
            return 0;
    }
}

int32_t aver_phys_joint_set_motor_strength(int32_t joint, int32_t axis, float maxForceOrTorque) {
    Joint* j = findJoint(joint);
    if (!j) return 0;

    switch (j->constraint->GetSubType()) {
        case JPH::EConstraintSubType::Hinge:
            // Torque is kg*cm^2/s^2 -> kg*m^2/s^2, the same cm^2->m^2 factor as toJoltTorque.
            static_cast<JPH::HingeConstraint*>(j->constraint.GetPtr())
                ->GetMotorSettings().SetTorqueLimit(maxForceOrTorque / (kCmPerMetre * kCmPerMetre));
            return 1;
        case JPH::EConstraintSubType::Slider:
            // Force is kg*cm/s^2 -> kg*m/s^2 = N, one factor of kCmPerMetre.
            static_cast<JPH::SliderConstraint*>(j->constraint.GetPtr())
                ->GetMotorSettings().SetForceLimit(maxForceOrTorque / kCmPerMetre);
            return 1;
        case JPH::EConstraintSubType::SixDOF: {
            if (axis < 0 || axis >= AVER_PHYS_DOF_COUNT) {
                AVER_WARN("[Physics] six-DOF axis {} is out of range 0-5", axis);
                return 0;
            }
            auto* sixDof = static_cast<JPH::SixDOFConstraint*>(j->constraint.GetPtr());
            JPH::MotorSettings& motor =
                sixDof->GetMotorSettings(static_cast<JPH::SixDOFConstraint::EAxis>(axis));
            if (axis <= AVER_PHYS_DOF_TRANSLATION_Z) motor.SetForceLimit(maxForceOrTorque / kCmPerMetre);
            else motor.SetTorqueLimit(maxForceOrTorque / (kCmPerMetre * kCmPerMetre));
            return 1;
        }
        default:
            AVER_WARN("[Physics] joint {} has no motor (only hinge, slider and six-DOF do)", joint);
            return 0;
    }
}

int32_t aver_phys_joint_set_limits(int32_t joint, int32_t axis, float minimum, float maximum) {
    Joint* j = findJoint(joint);
    if (!j) return 0;

    switch (j->constraint->GetSubType()) {
        case JPH::EConstraintSubType::Hinge:
            static_cast<JPH::HingeConstraint*>(j->constraint.GetPtr())
                ->SetLimits(std::clamp(minimum, -JPH::JPH_PI, 0.0f), std::clamp(maximum, 0.0f, JPH::JPH_PI));
            return 1;
        case JPH::EConstraintSubType::Slider:
            static_cast<JPH::SliderConstraint*>(j->constraint.GetPtr())
                ->SetLimits(std::min(cmToM(minimum), 0.0f), std::max(cmToM(maximum), 0.0f));
            return 1;
        case JPH::EConstraintSubType::Distance: {
            // Unlike the SETTINGS these joints were built from, the RUNTIME setters (SetDistance
            // here, SetLength below) have no negative-means-auto sentinel and simply assert
            // inMin <= inMax -- so that meaning, only ever a creation-time nicety, does not carry
            // over into a later aver_phys_joint_set_limits call, and the pair is sorted instead of
            // trusted.
            const float lo = cmToM(minimum);
            static_cast<JPH::DistanceConstraint*>(j->constraint.GetPtr())
                ->SetDistance(lo, std::max(cmToM(maximum), lo));
            return 1;
        }
        case JPH::EConstraintSubType::Pulley: {
            // SetLength additionally asserts inMinLength >= 0 (a rope length cannot go negative,
            // unlike DISTANCE's separation which is unsigned by construction but not asserted so).
            const float lo = std::max(cmToM(minimum), 0.0f);
            static_cast<JPH::PulleyConstraint*>(j->constraint.GetPtr())
                ->SetLength(lo, std::max(cmToM(maximum), lo));
            return 1;
        }
        case JPH::EConstraintSubType::SixDOF: {
            if (axis < 0 || axis >= AVER_PHYS_DOF_COUNT) {
                AVER_WARN("[Physics] six-DOF axis {} is out of range 0-5", axis);
                return 0;
            }
            auto* sixDof = static_cast<JPH::SixDOFConstraint*>(j->constraint.GetPtr());
            // SixDOFConstraint exposes per-axis GETTERS but only bulk (all three at once) SETTERS,
            // so changing one axis without disturbing the other two means reading the current triple
            // first.
            if (axis <= AVER_PHYS_DOF_TRANSLATION_Z) {
                JPH::Vec3 lo = sixDof->GetTranslationLimitsMin();
                JPH::Vec3 hi = sixDof->GetTranslationLimitsMax();
                lo.SetComponent(axis, sixDofTranslationLimit(minimum));
                hi.SetComponent(axis, sixDofTranslationLimit(maximum));
                sixDof->SetTranslationLimits(lo, hi);
            } else {
                const int rot = axis - AVER_PHYS_DOF_ROTATION_X;
                JPH::Vec3 lo = sixDof->GetRotationLimitsMin();
                JPH::Vec3 hi = sixDof->GetRotationLimitsMax();
                lo.SetComponent(rot, minimum);
                hi.SetComponent(rot, maximum);
                sixDof->SetRotationLimits(lo, hi);
            }
            return 1;
        }
        default:
            // Cone (one half-angle, not a min/max pair) and swing-twist (four independent angles,
            // not one pair either) do not fit this call's (minimum, maximum) shape, so they report
            // "no limits to change" here rather than silently discarding half of what was asked.
            AVER_WARN("[Physics] joint {} has no min/max limits to change", joint);
            return 0;
    }
}

int32_t aver_phys_joint_value(int32_t joint, float* outValue) {
    Joint* j = findJoint(joint);
    if (!j || !outValue) return 0;

    switch (j->constraint->GetSubType()) {
        case JPH::EConstraintSubType::Hinge:
            *outValue = static_cast<JPH::HingeConstraint*>(j->constraint.GetPtr())->GetCurrentAngle();
            return 1;
        case JPH::EConstraintSubType::Slider:
            *outValue = mToCm(
                static_cast<JPH::SliderConstraint*>(j->constraint.GetPtr())->GetCurrentPosition());
            return 1;
        default:
            return 0;
    }
}

} // extern "C"

// Called by aver_phys_shutdown before it destroys the world -- see PhysicsInternal.hpp for why this is
// a hook rather than something noticed later. RemoveConstraint is deliberately NOT called per joint:
// the PhysicsSystem is about to be destroyed and takes its own ConstraintManager with it, so the only
// thing that has to happen here is that this file stops holding references into it.
namespace aver::physics::detail {
void destroyAllJoints() { g_joints.clear(); }
}
