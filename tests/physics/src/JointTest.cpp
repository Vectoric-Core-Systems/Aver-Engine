// JointTest -- fixed, point, distance, hinge, slider joints; motors; enable/disable; lifetime; and the
// refusals a bad call must get.
//
// WRITTEN AGAINST physics_joints_abi.h AND physics_abi.h ALONE, deliberately, before PhysicsJoints.cpp
// existed to read. A test derived from the implementation only proves the implementation agrees with
// itself; this one proves the implementation agrees with the CONTRACT the header promises callers.
//
// THE STANDARD IS OBSERVED MOTION, not return codes. A joint constructor that silently drops the axis
// conversion, or gets cm/m wrong, or wires the wrong body into Jolt's solver, still returns a nonzero
// handle every time -- so nearly every check here steps the world and asks where things ended up,
// against a number worked out independently of the code under test:
//
//   * a hinge to the world is a pendulum -- its distance from the pivot cannot change, and if the axis
//     conversion sent the wrong vector into Jolt the body will fly off in some other plane entirely
//     rather than merely swinging on the wrong side,
//   * a distance joint with min == max == 200 cm settles at 200.00, not 2.00 or 20000 -- a scale error
//     of exactly 100 in either direction,
//   * a slider is only free along one axis -- a force with components off that axis must produce no
//     motion off it, not "less" motion,
//   * a motor's target sign must show up as a REVERSAL of which way aver_phys_joint_value is moving,
//     which is checkable without knowing which absolute sign convention the implementation chose for
//     the angle (the header never promises one, so this file never assumes one).
//
// Hand-run, exit code = failure count, same shape as BodyDynamicsTest.cpp beside it.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_joints_abi.h"

#include <cmath>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

static const f32 kDt = 1.0f / 60.0f;

static float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The angle between a body's current orientation and identity, in radians -- used to show "it rotated
// a meaningful amount" without caring about which axis or which direction. w = cos(theta/2).
static float quatAngleFromIdentity(const float q[4]) {
    float w = q[3];
    if (w > 1.0f) w = 1.0f;
    if (w < -1.0f) w = -1.0f;
    return 2.0f * std::acos(std::fabs(w));
}

static void beginWorldNoGravity() {
    aver_phys_init();
    aver_phys_set_gravity(0.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------------------------------

// A HINGE TO THE WORLD IS A PENDULUM, and a pendulum has exactly one invariant that does not depend on
// timing, damping or which way Jolt happens to measure its own angle: the distance from the pivot to
// the body never changes, because rotation about a fixed axis through a fixed point preserves the
// length of every vector from that point. If the axis conversion sent the wrong basis vector into
// Jolt, the body does not swing wrong -- it leaves the pivot's sphere entirely, which this catches by
// a wide margin rather than a rounding one.
static void testHingeSwingsAtConstantRadius() {
    AVER_INFO("=== a hinge to the world swings at a fixed radius from the pivot ===");
    aver_phys_init();   // gravity left at its default -980 cm/s^2 -- the whole point is to have something to swing

    const float pivot[3] = {0.0f, 0.0f, 0.0f};
    const int32_t b = aver_phys_add_dynamic_box(0.0f, 200.0f, 0.0f, 20.0f, 20.0f, 20.0f, 1.0f);
    check(b != 0, "the bob exists");

    const float hingeAxis[3]  = {1.0f, 0.0f, 0.0f};   // engine +X: perpendicular to the Y-Z swing plane
    const float normalAxis[3] = {0.0f, 1.0f, 0.0f};   // the arm's own starting direction, i.e. angle 0 at creation
    const int32_t j = aver_phys_joint_hinge(b, AVER_PHYS_WORLD_BODY, pivot, hingeAxis, normalAxis,
                                            -100.0f, 100.0f);   // far wider than +-PI: unlimited
    check(j != 0, "the hinge is created");

    float v0 = 999.0f;
    check(aver_phys_joint_value(j, &v0) == 1 && near(v0, 0.0f, 0.1f),
          "the angle starts at ~0, got " + f2s(v0));

    float minZ = 0.0f, maxRadiusError = 0.0f;
    float maxAbsAngle = 0.0f;
    for (int i = 0; i < 150; ++i) {   // 2.5 s: comfortably past the bottom of the swing at least once
        aver_phys_step(kDt);
        float p[3] = {0, 0, 0};
        aver_phys_body_position(b, p);
        if (p[2] < minZ) minZ = p[2];
        const float r = dist3(p, pivot);
        const float err = std::fabs(r - 200.0f);
        if (err > maxRadiusError) maxRadiusError = err;
        float v = 0.0f;
        aver_phys_joint_value(j, &v);
        if (std::fabs(v) > maxAbsAngle) maxAbsAngle = std::fabs(v);
    }

    check(maxRadiusError < 4.0f,
          "distance from the pivot never drifted more than 4 cm from 200, worst was " +
          f2s(maxRadiusError));
    check(minZ < -80.0f,
          "gravity swung it well below the pivot, lowest z reached " + f2s(minZ) +
          " (a wrong axis conversion sends this sideways instead, not merely late)");
    check(maxAbsAngle > 0.3f,
          "aver_phys_joint_value tracked the swing, peak |angle| " + f2s(maxAbsAngle) + " rad");

    aver_phys_shutdown();
}

// THE OTHER HALF OF THE SAME JOINT, AND IT IS A REFUSAL RATHER THAN A BEHAVIOUR.
//
// This test used to assert that min == max == 0 produced a hinge that simply did not turn, and it
// passed -- in Release. Jolt does not build that constraint: HingeConstraint.cpp:83 asserts
// mLimitsMin != mLimitsMax with the message "Better use a fixed constraint in this case", and asserts
// are compiled out of Release, so the suite was measuring a constraint Jolt had already objected to.
// A Debug run died here on the first step.
//
// So the subject changed: a degenerate range is now refused at the ABI, named in the warning, and
// pointed at aver_phys_joint_fixed, which is the joint that actually means "no relative motion". The
// slider carries the identical guard for the identical assert at SliderConstraint.cpp:159.
static void testDegenerateLimitsAreRefused() {
    AVER_INFO("=== a hinge or slider locked at a single value is refused, not silently degenerate ===");
    aver_phys_init();

    const float pivot[3] = {0.0f, 0.0f, 0.0f};
    const int32_t b = aver_phys_add_dynamic_box(0.0f, 200.0f, 0.0f, 20.0f, 20.0f, 20.0f, 1.0f);
    const float hingeAxis[3]  = {1.0f, 0.0f, 0.0f};
    const float normalAxis[3] = {0.0f, 1.0f, 0.0f};

    check(aver_phys_joint_hinge(b, AVER_PHYS_WORLD_BODY, pivot, hingeAxis, normalAxis,
                                0.0f, 0.0f) == 0,
          "a hinge with min == max == 0 is refused");
    check(aver_phys_joint_slider(b, AVER_PHYS_WORLD_BODY, pivot, hingeAxis, normalAxis,
                                 0.0f, 0.0f) == 0,
          "and so is a slider with nowhere to slide");
    check(aver_phys_joint_count() == 0, "neither left a joint behind");

    // AND THE NEIGHBOURING CASES STILL WORK, because a guard that refuses too much is as wrong as one
    // that refuses too little. A one-sided range is a door that opens one way, not a degenerate joint.
    const int32_t oneSided = aver_phys_joint_hinge(b, AVER_PHYS_WORLD_BODY, pivot, hingeAxis,
                                                  normalAxis, 0.0f, 1.5f);
    check(oneSided != 0, "a hinge that may open one way but not the other is still created");
    check(aver_phys_joint_count() == 1, "and it is the only joint in the world");

    // What the caller should reach for instead, and it holds the body up under gravity.
    aver_phys_joint_remove(oneSided);
    const float axisX[3] = {1.0f, 0.0f, 0.0f};
    const float axisY[3] = {0.0f, 1.0f, 0.0f};
    const int32_t fixed = aver_phys_joint_fixed(b, AVER_PHYS_WORLD_BODY, pivot, axisX, axisY);
    check(fixed != 0, "aver_phys_joint_fixed, which the warning names, does build");

    // HELD, AND THE TEST IS THAT IT SETTLES rather than that it does not move at all. A fixed
    // constraint is solved, not welded: a 1 kg box on a 200 cm arm sags a few centimetres before the
    // solver catches it, and picking a tolerance tight enough to call that a failure would only be
    // measuring the solver's stiffness. What distinguishes "held" from "slipping" is whether the sag
    // CONVERGES, so the position is sampled twice, three seconds apart.
    for (int i = 0; i < 90; ++i) aver_phys_step(kDt);
    float early[3] = {0, 0, 0};
    aver_phys_body_position(b, early);
    for (int i = 0; i < 300; ++i) aver_phys_step(kDt);
    float late[3] = {0, 0, 0};
    aver_phys_body_position(b, late);

    check(dist3(early, late) < 1.0f,
          "the body has stopped moving between 1.5 s and 6.5 s, drifting " +
          f2s(dist3(early, late)) + " cm in five seconds -- a joint that were slipping would keep going");
    check(near(late[1], 200.0f, 10.0f) && near(late[2], 0.0f, 10.0f),
          "and it is still where it started, not on the floor, at (*, " + f2s(late[1]) + ", " +
          f2s(late[2]) + ")");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// A POINT JOINT PINS ONE POINT AND NOTHING ELSE. Put the pivot exactly at the body's own centre, spin
// the body up, and a correct point joint holds the centre still (the pivot cannot move) while letting
// the body rotate freely about it -- a hinge would refuse this rotation on two of its three axes; a
// point joint must allow all three.
static void testPointJointHoldsCentreAndFreesRotation() {
    AVER_INFO("=== a point joint keeps the pin fixed and rotation free ===");
    beginWorldNoGravity();

    const float pin[3] = {0.0f, 0.0f, 300.0f};
    const int32_t b = aver_phys_add_dynamic_sphere(pin[0], pin[1], pin[2], 25.0f, 1.0f);
    const int32_t j = aver_phys_joint_point(b, AVER_PHYS_WORLD_BODY, pin);
    check(j != 0, "the point joint is created");

    check(aver_phys_body_set_angular_velocity(b, 2.0f, 1.0f, 0.0f) == 1, "the body is set spinning");

    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);   // 0.5 s

    float p[3] = {0, 0, 0};
    aver_phys_body_position(b, p);
    check(dist3(p, pin) < 3.0f,
          "the centre stayed within 3 cm of the pin, drifted " + f2s(dist3(p, pin)));

    float q[4] = {0, 0, 0, 1};
    aver_phys_body_rotation(b, q);
    const float turned = quatAngleFromIdentity(q);
    check(turned > 0.4f,
          "and it actually rotated -- " + f2s(turned) + " rad from where it started");

    // A JOINT TYPE WITH NO SINGLE SCALAR. A point joint has three free rotational and zero free
    // translational axes; there is no one number that describes its state, which is exactly why
    // aver_phys_joint_value is one function with a documented "no scalar" answer rather than a family.
    float v = 0.0f;
    check(aver_phys_joint_value(j, &v) == 0, "joint_value on a point joint returns 0, not a body angle");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// THE CENTIMETRE CHECK. min == max == 200 must hold the two points at 200.00 cm, not 2 (a stray /100)
// or 20000 (a stray *100). The anchor sits directly above the body so gravity pulls straight along the
// strut rather than swinging it, which keeps this test purely about the DISTANCE, not the geometry.
static void testDistanceJointHoldsExactSeparation() {
    AVER_INFO("=== a distance joint with min == max holds an exact separation, in centimetres ===");
    aver_phys_init();   // gravity default -- something has to pull the strut taut

    const float anchor[3] = {0.0f, 0.0f, 500.0f};
    const float bodyStart[3] = {0.0f, 0.0f, 300.0f};   // 200 cm below the anchor already
    const int32_t b = aver_phys_add_dynamic_sphere(bodyStart[0], bodyStart[1], bodyStart[2], 25.0f, 1.0f);
    const int32_t j = aver_phys_joint_distance(b, AVER_PHYS_WORLD_BODY, bodyStart, anchor, 200.0f, 200.0f);
    check(j != 0, "the strut is created");

    float maxErr = 0.0f;
    for (int i = 0; i < 120; ++i) {   // 2 s -- long enough for gravity to load the strut fully
        aver_phys_step(kDt);
        float p[3] = {0, 0, 0};
        aver_phys_body_position(b, p);
        const float err = std::fabs(dist3(p, anchor) - 200.0f);
        if (err > maxErr) maxErr = err;
    }
    check(maxErr < 3.0f,
          "separation never left 200 +- 3 cm despite gravity, worst error " + f2s(maxErr) +
          " (a cm/m mixup would land this near 198 or -19800, not a few cm)");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// A SLIDER FREES EXACTLY ONE TRANSLATION AXIS. Push the body with a force that has a component along
// the slider axis and components off it; a correct slider lets the aligned component move it and
// refuses the other two outright, not merely "resists" them.
static void testSliderMovesAlongAxisOnly() {
    AVER_INFO("=== a slider moves along its axis and not across it ===");
    beginWorldNoGravity();

    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const int32_t b = aver_phys_add_dynamic_box(0.0f, 0.0f, 0.0f, 20.0f, 20.0f, 20.0f, 1.0f);
    const float sliderAxis[3] = {0.0f, 0.0f, 1.0f};   // engine +Z
    const float normalAxis[3] = {1.0f, 0.0f, 0.0f};   // perpendicular, per the header's requirement
    const int32_t j = aver_phys_joint_slider(b, AVER_PHYS_WORLD_BODY, origin, sliderAxis, normalAxis,
                                             -1000.0f, 1000.0f);
    check(j != 0, "the slider is created");

    // A force with a component on EVERY axis, reapplied every step because Jolt clears it at the end
    // of each one (BodyDynamicsTest.cpp already proves that half of the ABI).
    for (int i = 0; i < 60; ++i) {   // 1 s
        aver_phys_body_add_force(b, 100.0f, 100.0f, 100.0f);
        aver_phys_step(kDt);
    }

    float p[3] = {0, 0, 0};
    aver_phys_body_position(b, p);
    check(std::fabs(p[0]) < 3.0f && std::fabs(p[1]) < 3.0f,
          "the locked axes barely moved, got (" + f2s(p[0]) + ", " + f2s(p[1]) + ")");
    check(p[2] > 20.0f,
          "while the slider axis travelled a real distance, z = " + f2s(p[2]) +
          " (a stuck slider or a swapped axis would leave this near the locked axes' numbers instead)");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// A FIXED JOINT IS A WELD: two separate bodies must fall as if they were one rigid piece, which in a
// UNIFORM gravity field means translating together with NO relative rotation and NO change in their
// separation -- a uniform field imparts the same acceleration everywhere, so a truly rigid assembly
// experiences no net torque about its own centre of mass and does not tumble.
static void testFixedJointWeldsTwoBodies() {
    AVER_INFO("=== a fixed joint keeps two bodies' relative pose as they fall together ===");
    aver_phys_init();   // gravity default

    const float posA[3] = {0.0f, 0.0f, 300.0f};
    const float posB[3] = {0.0f, 80.0f, 300.0f};
    const float mid[3]  = {0.0f, 40.0f, 300.0f};
    const float axisX[3] = {1.0f, 0.0f, 0.0f};
    const float axisY[3] = {0.0f, 1.0f, 0.0f};

    const int32_t a = aver_phys_add_dynamic_box(posA[0], posA[1], posA[2], 20.0f, 20.0f, 20.0f, 1.0f);
    const int32_t b = aver_phys_add_dynamic_box(posB[0], posB[1], posB[2], 20.0f, 20.0f, 20.0f, 1.0f);
    const int32_t j = aver_phys_joint_fixed(a, b, mid, axisX, axisY);
    check(j != 0, "the weld is created");

    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);   // 0.5 s

    float pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
    aver_phys_body_position(a, pa);
    aver_phys_body_position(b, pb);
    check(pa[2] < 250.0f && pb[2] < 250.0f, "both bodies fell");

    const float diff[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    check(near(diff[0], 0.0f, 4.0f) && near(diff[1], 80.0f, 4.0f) && near(diff[2], 0.0f, 4.0f),
          "and stayed 80 cm apart on the same axis they started on, offset now (" +
          f2s(diff[0]) + ", " + f2s(diff[1]) + ", " + f2s(diff[2]) + ")");

    // A FIXED JOINT ALSO HAS NO SINGLE SCALAR -- it has nothing left to move.
    float v = 0.0f;
    check(aver_phys_joint_value(j, &v) == 0, "joint_value on a fixed joint returns 0 too");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// DISABLING MUST ACTUALLY STOP CONSTRAINING, not just report a flag. A heavy anchor body barely moves
// on its own, so any real separation growth while the joint is off can only be the bodies coming
// apart; re-enabling and killing the drifting body's velocity isolates whether the constraint pulls it
// back rather than merely stopping further drift.
static void testDisableAndReEnable() {
    AVER_INFO("=== disabling a joint lets bodies separate; re-enabling re-constrains them ===");
    beginWorldNoGravity();

    const float posA[3] = {0.0f, 0.0f, 0.0f};
    const float posB[3] = {0.0f, 100.0f, 0.0f};
    const int32_t a = aver_phys_add_dynamic_sphere(posA[0], posA[1], posA[2], 25.0f, 1000.0f);   // effectively an anchor
    const int32_t b = aver_phys_add_dynamic_sphere(posB[0], posB[1], posB[2], 25.0f, 1.0f);
    const int32_t j = aver_phys_joint_distance(a, b, posA, posB, 100.0f, 100.0f);
    check(j != 0, "the strut is created");
    check(aver_phys_joint_enabled(j) == 1, "and starts enabled");

    aver_phys_body_set_velocity(b, 0.0f, 300.0f, 0.0f);   // pulling outward
    for (int i = 0; i < 15; ++i) aver_phys_step(kDt);
    float p[3] = {0, 0, 0};
    aver_phys_body_position(b, p);
    check(near(dist3(p, posA), 100.0f, 5.0f),
          "while enabled the strut held it near 100 cm, got " + f2s(dist3(p, posA)));

    check(aver_phys_joint_set_enabled(j, 0) == 1, "the joint is disabled");
    check(aver_phys_joint_enabled(j) == 0, "and reports disabled");
    aver_phys_body_set_velocity(b, 0.0f, 300.0f, 0.0f);   // give it somewhere to go now that nothing stops it
    for (int i = 0; i < 15; ++i) aver_phys_step(kDt);
    aver_phys_body_position(b, p);
    check(dist3(p, posA) > 160.0f,
          "with it off the bodies pulled apart well past 100 cm, now " + f2s(dist3(p, posA)));

    check(aver_phys_joint_set_enabled(j, 1) == 1, "the joint is re-enabled");
    check(aver_phys_joint_enabled(j) == 1, "and reports enabled again");
    aver_phys_body_set_velocity(b, 0.0f, 0.0f, 0.0f);   // isolate the constraint's own pull from any leftover drift
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    aver_phys_body_position(b, p);
    check(near(dist3(p, posA), 100.0f, 15.0f),
          "and it was pulled back toward 100 cm, ending at " + f2s(dist3(p, posA)));

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

static void testRemoveDropsCountAndHandle() {
    AVER_INFO("=== removing a joint drops the count and kills the handle ===");
    beginWorldNoGravity();

    const int32_t before = aver_phys_joint_count();
    const float pin[3] = {0.0f, 0.0f, 0.0f};
    const int32_t j = aver_phys_joint_point(AVER_PHYS_WORLD_BODY,
                                            aver_phys_add_dynamic_sphere(0, 0, 0, 25.0f, 1.0f), pin);
    check(j != 0, "a joint exists");
    check(aver_phys_joint_count() == before + 1, "the count went up by one");

    check(aver_phys_joint_remove(j) == 1, "it is removed");
    check(aver_phys_joint_count() == before, "and the count dropped back");

    int32_t oa = -1, ob = -1;
    check(aver_phys_joint_bodies(j, &oa, &ob) == 0, "the dead handle no longer resolves for its bodies");
    check(aver_phys_joint_set_enabled(j, 1) == 0, "nor can it be enabled");
    check(aver_phys_joint_enabled(j) == 0, "nor queried for its enabled state");
    float v = 0.0f;
    check(aver_phys_joint_value(j, &v) == 0, "nor read for a value");
    check(aver_phys_joint_remove(j) == 0, "and removing it again is refused rather than double-freeing");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// EVERY REFUSAL THE HEADER PROMISES: a dead body, both ends tied to the world, and a degenerate axis
// that cannot define a rotation frame.
static void testRefusals() {
    AVER_INFO("=== joints refuse dead handles, world-to-world, and degenerate axes ===");
    beginWorldNoGravity();

    const int32_t live = aver_phys_add_dynamic_sphere(0, 0, 0, 25.0f, 1.0f);
    const int32_t dead = 999999;
    const float pt[3]        = {0.0f, 0.0f, 0.0f};
    const float axis[3]      = {1.0f, 0.0f, 0.0f};
    const float normal[3]    = {0.0f, 1.0f, 0.0f};
    const float zeroAxis[3]  = {0.0f, 0.0f, 0.0f};

    check(aver_phys_joint_point(dead, AVER_PHYS_WORLD_BODY, pt) == 0, "a joint to a dead body handle is refused");
    check(aver_phys_joint_hinge(dead, live, pt, axis, normal, -1.0f, 1.0f) == 0,
          "a dead handle is refused even when the other end is fine");
    check(aver_phys_joint_point(AVER_PHYS_WORLD_BODY, AVER_PHYS_WORLD_BODY, pt) == 0,
          "both bodies being the world is refused");
    check(aver_phys_joint_hinge(live, AVER_PHYS_WORLD_BODY, pt, zeroAxis, normal, -1.0f, 1.0f) == 0,
          "a zero-length hinge axis is refused");
    check(aver_phys_joint_slider(live, AVER_PHYS_WORLD_BODY, pt, zeroAxis, normal, -100.0f, 100.0f) == 0,
          "a zero-length slider axis is refused too");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// A MOTOR WITH NOTHING FIGHTING IT MUST ACTUALLY TURN THE BODY, and reverse when its target does. The
// pivot sits at the body's own centre so gravity (left off, for cleanliness) or any offset torque never
// enters the picture -- only the motor is spinning it.
static void testHingeVelocityMotorTurnsAndReverses() {
    AVER_INFO("=== a hinge velocity motor turns the body and reverses with its target ===");
    beginWorldNoGravity();

    const float pivot[3] = {0.0f, 0.0f, 0.0f};
    const int32_t b = aver_phys_add_dynamic_box(0, 0, 0, 20, 20, 20, 1.0f);
    const float hingeAxis[3]  = {0.0f, 0.0f, 1.0f};
    const float normalAxis[3] = {1.0f, 0.0f, 0.0f};
    const int32_t j = aver_phys_joint_hinge(b, AVER_PHYS_WORLD_BODY, pivot, hingeAxis, normalAxis,
                                            -100.0f, 100.0f);
    check(j != 0, "the motorised hinge is created");

    check(aver_phys_joint_set_motor(j, 0, AVER_PHYS_MOTOR_VELOCITY, 2.0f) == 1,
          "the motor is set to spin at a target velocity");

    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);   // 0.5 s
    float angle1 = 0.0f;
    check(aver_phys_joint_value(j, &angle1) == 1, "the angle reads back");
    check(std::fabs(angle1) > 0.2f,
          "the motor actually turned the body, angle now " + f2s(angle1));

    check(aver_phys_joint_set_motor(j, 0, AVER_PHYS_MOTOR_VELOCITY, -2.0f) == 1,
          "the target is reversed");
    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);   // 0.5 s more
    float angle2 = 0.0f;
    aver_phys_joint_value(j, &angle2);
    check(angle2 < angle1 - 0.1f,
          "and the angle moved back the other way -- " + f2s(angle1) + " then " + f2s(angle2));

    aver_phys_shutdown();
}

// A joint type with no motor must say so rather than pretend. A point joint is one of the header's
// named examples of a type the motor calls do not apply to.
static void testMotorRefusedOnNonMotorisedJoint() {
    AVER_INFO("=== a motor call on a joint type with no motor is refused ===");
    beginWorldNoGravity();

    const float pt[3] = {0.0f, 0.0f, 0.0f};
    const int32_t b = aver_phys_add_dynamic_sphere(0, 0, 0, 25.0f, 1.0f);
    const int32_t j = aver_phys_joint_point(b, AVER_PHYS_WORLD_BODY, pt);
    check(j != 0, "the point joint is created");
    check(aver_phys_joint_set_motor(j, 0, AVER_PHYS_MOTOR_VELOCITY, 1.0f) == 0,
          "set_motor on a point joint is refused");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("joints: fixed, point, distance, hinge, slider, motors, lifetime, refusals");

    testHingeSwingsAtConstantRadius();
    testDegenerateLimitsAreRefused();
    testPointJointHoldsCentreAndFreesRotation();
    testDistanceJointHoldsExactSeparation();
    testSliderMovesAlongAxisOnly();
    testFixedJointWeldsTwoBodies();
    testDisableAndReEnable();
    testRemoveDropsCountAndHandle();
    testRefusals();
    testHingeVelocityMotorTurnsAndReverses();
    testMotorRefusedOnNonMotorisedJoint();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
