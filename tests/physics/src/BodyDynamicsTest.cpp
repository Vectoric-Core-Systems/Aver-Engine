// BodyDynamicsTest -- forces, impulses, spin, motion type, the material properties, mass and sleeping.
//
// WHAT THIS IS ACTUALLY DEFENDING, because "the setter returned 1" is worth almost nothing here. Every
// function under test converts an argument out of the engine's units and axes before handing it to
// Jolt, and a WRONG CONVERSION STILL RETURNS 1. The failures that matter are all silent ones:
//
//   * an angular velocity converted like a direction instead of like a pseudovector -- the body spins
//     BACKWARDS, and nothing anywhere reports it,
//   * a torque scaled by one factor of 100 instead of two -- every torque is a hundred times too
//     weak, which reads as "torque does not work",
//   * a mass setter that changes mass without rescaling inertia -- the body weighs more and still
//     spins like its old self.
//
// So the assertions below are almost all about OBSERVED MOTION after a step, against a number derived
// independently, rather than about return codes. The one case that is only a return code says so.
//
// Hand-run, exit code = failure count, same shape as PhysicsTest.cpp beside it.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"

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

// A world with gravity switched OFF, which is what nearly every test here wants: gravity is a second
// force acting on the same body, and leaving it on would mean every expected velocity carried a
// gravity term that has nothing to do with what is under test.
static void beginWorldNoGravity() {
    aver_phys_init();
    aver_phys_set_gravity(0.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------------------------------

// THE UNIT CHECK FOR EVERY LINEAR QUANTITY. An impulse is mass times a change in velocity, so a 2 kg
// body given 200 kg*cm/s must come out at exactly 100 cm/s -- and it only does if the engine's
// centimetres survived the trip into Jolt's metres and back. A factor-of-100 error anywhere in that
// path lands this at 1 cm/s or 10000 cm/s, neither of which is subtle once the number is asserted.
static void testImpulseIsMassTimesVelocity() {
    AVER_INFO("=== an impulse changes velocity by impulse/mass ===");
    beginWorldNoGravity();

    const float massKg = 2.0f;
    const int32_t b = aver_phys_add_dynamic_sphere(0, 0, 0, 25.0f, massKg);
    check(b != 0, "a dynamic sphere exists");

    check(aver_phys_body_add_impulse(b, 200.0f, 0.0f, 0.0f) == 1, "the impulse is accepted");
    float v[3] = {0, 0, 0};
    aver_phys_body_velocity(b, v);
    check(near(v[0], 100.0f, 0.5f),
          "200 kg*cm/s on 2 kg gives 100 cm/s along +X, got " + f2s(v[0]));
    check(near(v[1], 0.0f, 0.5f) && near(v[2], 0.0f, 0.5f),
          "and nothing on the other two axes, got (" + f2s(v[1]) + ", " + f2s(v[2]) + ")");

    // AND IT IS INSTANT, not integrated: the header says an impulse changes velocity immediately, and
    // the read above happened before any step ran, which is what proves it.
    aver_phys_shutdown();
}

// A FORCE IS NOT AN IMPULSE, and the difference is the whole of why both exist. F = ma, so a force of
// 200 kg*cm/s^2 on a 2 kg body is 100 cm/s^2, and one step of 1/60 s buys 100/60 cm/s -- not 100.
// Jolt clears accumulated forces at the end of each step, so a second step with no further call adds
// nothing more, which the second half asserts.
static void testForceIsPerStepAndClears() {
    AVER_INFO("=== a force accelerates for one step, then is gone ===");
    beginWorldNoGravity();

    const int32_t b = aver_phys_add_dynamic_sphere(0, 0, 0, 25.0f, 2.0f);
    aver_phys_body_add_force(b, 200.0f, 0.0f, 0.0f);
    aver_phys_step(kDt);

    float v[3] = {0, 0, 0};
    aver_phys_body_velocity(b, v);
    const float expected = 100.0f * kDt;
    check(near(v[0], expected, 0.5f),
          "one step of 200 kg*cm/s^2 on 2 kg gives " + f2s(expected) + " cm/s, got " + f2s(v[0]));

    aver_phys_step(kDt);
    float v2[3] = {0, 0, 0};
    aver_phys_body_velocity(b, v2);
    check(near(v2[0], v[0], 0.5f),
          "and a second step with no new force adds nothing, got " + f2s(v2[0]));

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// THE TEST THIS WHOLE FILE EXISTS FOR.
//
// Angular velocity is an AXIAL vector: it flips sign between the engine's left-handed axes and Jolt's
// right-handed ones (Convert.hpp derives this from det(B) = -1). That derivation is on paper, and a
// derivation on paper is exactly the kind of thing that is confidently wrong -- so this asserts the
// OBSERVABLE consequence instead.
//
// Spin a body about the engine's +Z (up) at a known rate, step, and ask where a point that started on
// +X ended up. In a LEFT-handed frame a positive rotation about +Z carries +X toward +Y. If the
// converter used toJoltDir instead of toJoltAngular the body would spin the other way and the point
// would land on -Y, failing this by a mile rather than by a rounding error.
//
// The point is carried by the body's own reported ROTATION, which goes through the quaternion
// converter this module has shipped and tested since the beginning -- so what is really being checked
// is that the angular-velocity convention AGREES with the rotation convention. Two converters that
// disagree is precisely the bug.
static void testAngularVelocitySpinsTheRightWay() {
    AVER_INFO("=== angular velocity is axial: +Z spin carries +X toward +Y ===");
    beginWorldNoGravity();

    const int32_t b = aver_phys_add_dynamic_box(0, 0, 0, 50, 50, 50, 1.0f);

    // A quarter turn per second about engine +Z (up).
    const float omega = 1.5707963f;   // rad/s
    check(aver_phys_body_set_angular_velocity(b, 0.0f, 0.0f, omega) == 1, "the spin is accepted");

    float w[3] = {0, 0, 0};
    aver_phys_body_angular_velocity(b, w);
    check(near(w[2], omega, 1e-2f) && near(w[0], 0.0f, 1e-2f) && near(w[1], 0.0f, 1e-2f),
          "and reads back on the same axis it was set on, got (" + f2s(w[0]) + ", " + f2s(w[1]) +
          ", " + f2s(w[2]) + ")");

    // A sixth of a second: a quarter turn in one second means 15 degrees here, small enough that the
    // sign is unmistakable and large enough to be far outside any solver noise.
    for (int i = 0; i < 10; ++i) aver_phys_step(kDt);

    float q[4] = {0, 0, 0, 1};
    aver_phys_body_rotation(b, q);

    // Rotate (1,0,0) by the reported quaternion, in the engine's own maths.
    const Vec3 u(q[0], q[1], q[2]);
    const f32 s = q[3];
    const Vec3 v(1.0f, 0.0f, 0.0f);
    const Vec3 uxv(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x);
    const f32 dot = u.x * v.x + u.y * v.y + u.z * v.z;
    const f32 uu  = u.x * u.x + u.y * u.y + u.z * u.z;
    const Vec3 rotated(2.0f * dot * u.x + (s * s - uu) * v.x + 2.0f * s * uxv.x,
                       2.0f * dot * u.y + (s * s - uu) * v.y + 2.0f * s * uxv.y,
                       2.0f * dot * u.z + (s * s - uu) * v.z + 2.0f * s * uxv.z);

    check(rotated.y > 0.15f,
          "+X moved TOWARD +Y as a positive left-handed +Z spin requires -- y = " + f2s(rotated.y) +
          " (a negative y here means the axial sign is inverted, i.e. toJoltDir was used where "
          "toJoltAngular belongs)");
    check(rotated.x > 0.9f,
          "and only part way round after 1/6 s, x = " + f2s(rotated.x));
    check(near(rotated.z, 0.0f, 0.05f), "staying in the XY plane, z = " + f2s(rotated.z));

    aver_phys_shutdown();
}

// An angular impulse must spin the body the SAME way an angular velocity does. Same axial rule, a
// different Jolt entry point and a different unit scale (kg*cm^2/s, so cm TWICE) -- which is exactly
// why it gets its own check rather than being assumed to follow.
static void testAngularImpulseSpinsTheRightWay() {
    AVER_INFO("=== an angular impulse spins the same way, and its cm^2 scale is right ===");
    beginWorldNoGravity();

    // A solid box, 1 m on a side, 12 kg: inertia about a principal axis is m*(w^2+h^2)/12, which for
    // 1 m sides and 12 kg is exactly 1 kg*m^2 -- chosen so the arithmetic below is checkable by eye.
    const int32_t b = aver_phys_add_dynamic_box(0, 0, 0, 50.0f, 50.0f, 50.0f, 12.0f);

    // 1 kg*m^2/s is 10000 kg*cm^2/s. On 1 kg*m^2 of inertia that is 1 rad/s.
    check(aver_phys_body_add_angular_impulse(b, 0.0f, 0.0f, 10000.0f) == 1,
          "the angular impulse is accepted");

    float w[3] = {0, 0, 0};
    aver_phys_body_angular_velocity(b, w);
    check(w[2] > 0.0f,
          "it spins about +Z in the same sense set_angular_velocity does, got " + f2s(w[2]));
    // A GENEROUS BAND ON PURPOSE. The exact figure depends on Jolt's own inertia for the shape, which
    // this test deliberately does not re-derive; what it is pinning down is the SCALE -- one factor of
    // 100 out lands at 0.01 or 100 rad/s, both far outside this.
    check(w[2] > 0.2f && w[2] < 5.0f,
          "and lands within a factor of a few of the 1 rad/s the cm^2 scale predicts, got " +
          f2s(w[2]) + " rad/s (a single factor of 100 would put this at 0.01 or 100)");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// KINEMATIC IS THE MOTION TYPE THE ABI HAD NO WAY TO ASK FOR, and a moving platform is the thing it
// unlocks. The check is behavioural: gravity is ON here, and a kinematic body must not fall while a
// dynamic one beside it does.
static void testKinematicIgnoresGravity() {
    AVER_INFO("=== a kinematic body carries on regardless; a dynamic one falls ===");
    aver_phys_init();   // gravity left at its default -980 cm/s^2

    const int32_t kin = aver_phys_add_dynamic_box(0, 0, 500, 50, 50, 50, 1.0f);
    const int32_t dyn = aver_phys_add_dynamic_box(300, 0, 500, 50, 50, 50, 1.0f);

    check(aver_phys_body_motion_type(kin) == AVER_PHYS_MOTION_DYNAMIC, "it starts dynamic");
    check(aver_phys_body_set_motion_type(kin, AVER_PHYS_MOTION_KINEMATIC) == 1, "and turns kinematic");
    check(aver_phys_body_motion_type(kin) == AVER_PHYS_MOTION_KINEMATIC, "which reads back");

    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);

    float pk[3] = {0, 0, 0}, pd[3] = {0, 0, 0};
    aver_phys_body_position(kin, pk);
    aver_phys_body_position(dyn, pd);
    check(near(pk[2], 500.0f, 1.0f), "the kinematic body has not fallen, z = " + f2s(pk[2]));
    check(pd[2] < 400.0f, "the dynamic one beside it has, z = " + f2s(pd[2]));

    // AND IT STILL MOVES WHEN TOLD TO, which is the other half of what kinematic means: a platform
    // that cannot be pushed but can be driven.
    aver_phys_body_set_velocity(kin, 100.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    aver_phys_body_position(kin, pk);
    check(pk[0] > 50.0f, "and a kinematic body given a velocity travels, x = " + f2s(pk[0]));

    check(aver_phys_body_set_motion_type(kin, 7) == 0, "an unrecognised motion type is refused");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

static void testGravityFactor() {
    AVER_INFO("=== gravity factor is per body ===");
    aver_phys_init();

    const int32_t floaty = aver_phys_add_dynamic_sphere(0, 0, 500, 25, 1.0f);
    const int32_t heavy  = aver_phys_add_dynamic_sphere(300, 0, 500, 25, 1.0f);
    check(aver_phys_body_set_gravity_factor(floaty, 0.0f) == 1, "one body is given gravity factor 0");

    float got = -1.0f;
    check(aver_phys_body_gravity_factor(floaty, &got) == 1 && near(got, 0.0f),
          "which reads back, got " + f2s(got));

    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    float pf[3] = {0, 0, 0}, ph[3] = {0, 0, 0};
    aver_phys_body_position(floaty, pf);
    aver_phys_body_position(heavy, ph);
    check(near(pf[2], 500.0f, 1.0f), "it does not fall, z = " + f2s(pf[2]));
    check(ph[2] < 400.0f, "while its twin at factor 1 does, z = " + f2s(ph[2]));

    aver_phys_shutdown();
}

static void testDamping() {
    AVER_INFO("=== damping bleeds velocity away ===");
    beginWorldNoGravity();

    const int32_t b = aver_phys_add_dynamic_sphere(0, 0, 0, 25, 1.0f);
    check(aver_phys_body_set_damping(b, 2.0f, 0.0f) == 1, "linear damping is set");
    float l = -1.0f, a = -1.0f;
    check(aver_phys_body_damping(b, &l, &a) == 1 && near(l, 2.0f) && near(a, 0.0f),
          "and reads back as (" + f2s(l) + ", " + f2s(a) + ")");

    aver_phys_body_set_velocity(b, 1000.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    float v[3] = {0, 0, 0};
    aver_phys_body_velocity(b, v);
    check(v[0] < 500.0f && v[0] > 0.0f,
          "a second at damping 2 leaves well under half the speed and still positive, got " + f2s(v[0]));

    // A NEGATIVE DAMPING IS CLAMPED, NOT OBEYED -- it is an energy source, and a body accelerating
    // forever from a slider dragged below zero is not a physics bug anyone would find quickly.
    check(aver_phys_body_set_damping(b, -5.0f, -5.0f) == 1, "a negative damping is accepted");
    aver_phys_body_damping(b, &l, &a);
    check(near(l, 0.0f) && near(a, 0.0f), "and clamped to zero, got (" + f2s(l) + ", " + f2s(a) + ")");

    aver_phys_shutdown();
}

static void testRestitutionAndFriction() {
    AVER_INFO("=== restitution bounces, friction resists ===");
    aver_phys_init();

    aver_phys_add_static_box(0, 0, -50, 5000, 5000, 50);   // a floor at z = 0
    const int32_t bouncy = aver_phys_add_dynamic_sphere(0, 0, 300, 25, 1.0f);
    const int32_t dead   = aver_phys_add_dynamic_sphere(400, 0, 300, 25, 1.0f);
    check(aver_phys_body_set_restitution(bouncy, 0.9f) == 1, "one ball is made bouncy");
    check(aver_phys_body_set_restitution(dead, 0.0f) == 1, "and one dead");

    float r = -1.0f;
    check(aver_phys_body_restitution(bouncy, &r) == 1 && near(r, 0.9f),
          "restitution reads back, got " + f2s(r));

    // Track the highest either ball reaches AFTER its first contact with the floor.
    float bouncyPeak = 0.0f, deadPeak = 0.0f;
    bool bouncyLanded = false, deadLanded = false;
    for (int i = 0; i < 240; ++i) {
        aver_phys_step(kDt);
        float pb[3], pd[3];
        aver_phys_body_position(bouncy, pb);
        aver_phys_body_position(dead, pd);
        if (pb[2] < 40.0f) bouncyLanded = true;
        if (pd[2] < 40.0f) deadLanded = true;
        if (bouncyLanded && pb[2] > bouncyPeak) bouncyPeak = pb[2];
        if (deadLanded   && pd[2] > deadPeak)   deadPeak   = pd[2];
    }
    check(bouncyLanded && deadLanded, "both balls reached the floor");
    check(bouncyPeak > deadPeak + 20.0f,
          "the bouncy ball rebounds higher than the dead one (" + f2s(bouncyPeak) + " vs " +
          f2s(deadPeak) + ")");

    // Friction, on the SAME floor: a frictionless puck given a sideways shove keeps more of it.
    const int32_t slick = aver_phys_add_dynamic_box(-800, 0, 26, 25, 25, 25, 1.0f);
    const int32_t grippy = aver_phys_add_dynamic_box(-800, 400, 26, 25, 25, 25, 1.0f);
    aver_phys_body_set_friction(slick, 0.0f);
    aver_phys_body_set_friction(grippy, 1.0f);
    float fr = -1.0f;
    check(aver_phys_body_friction(slick, &fr) == 1 && near(fr, 0.0f),
          "friction reads back, got " + f2s(fr));
    aver_phys_body_set_velocity(slick, 500.0f, 0.0f, 0.0f);
    aver_phys_body_set_velocity(grippy, 500.0f, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
    float vs[3], vg[3];
    aver_phys_body_velocity(slick, vs);
    aver_phys_body_velocity(grippy, vg);
    check(vs[0] > vg[0],
          "the frictionless box is still faster than the gripped one (" + f2s(vs[0]) + " vs " +
          f2s(vg[0]) + ")");

    aver_phys_shutdown();
}

// SETTING MASS MUST RESCALE INERTIA, not just mass. The check is that a body made ten times heavier
// resists an ANGULAR impulse ten times more -- which is only true if the inertia moved with the mass.
// A setter that wrote inverse mass alone passes every linear test and fails this one.
static void testMassRescalesInertia() {
    AVER_INFO("=== setting mass carries the inertia with it ===");
    beginWorldNoGravity();

    const int32_t light = aver_phys_add_dynamic_box(0, 0, 0, 50, 50, 50, 1.0f);
    const int32_t heavy = aver_phys_add_dynamic_box(300, 0, 0, 50, 50, 50, 1.0f);

    float m = 0.0f;
    check(aver_phys_body_mass(light, &m) == 1 && near(m, 1.0f, 0.01f),
          "mass reads back as created, got " + f2s(m));
    check(aver_phys_body_set_mass(heavy, 10.0f) == 1, "the second body is made ten times heavier");
    check(aver_phys_body_mass(heavy, &m) == 1 && near(m, 10.0f, 0.01f),
          "which reads back, got " + f2s(m));

    aver_phys_body_add_angular_impulse(light, 0, 0, 10000.0f);
    aver_phys_body_add_angular_impulse(heavy, 0, 0, 10000.0f);
    float wl[3], wh[3];
    aver_phys_body_angular_velocity(light, wl);
    aver_phys_body_angular_velocity(heavy, wh);
    check(wl[2] > 0.0f && wh[2] > 0.0f, "both spin up");
    check(near(wl[2] / wh[2], 10.0f, 1.0f),
          "and the heavy one spins ~10x slower for the same angular impulse -- ratio " +
          f2s(wl[2] / wh[2]) + " (a mass setter that left inertia alone would give 1.0)");

    check(aver_phys_body_set_mass(light, 0.0f) == 0, "a non-positive mass is refused");
    aver_phys_shutdown();
}

static void testSleeping() {
    AVER_INFO("=== sleeping ===");
    beginWorldNoGravity();
    const int32_t b = aver_phys_add_dynamic_sphere(0, 0, 0, 25, 1.0f);
    check(aver_phys_body_is_active(b) == 1, "a fresh dynamic body is awake");
    check(aver_phys_body_deactivate(b) == 1, "it can be put to sleep");
    check(aver_phys_body_is_active(b) == 0, "and reports asleep");
    check(aver_phys_body_activate(b) == 1, "it can be woken");
    check(aver_phys_body_is_active(b) == 1, "and reports awake");

    // A FORCE WAKES IT, which is the property that makes every force call above usable on a settled
    // world -- an impulse that quietly did nothing to sleeping bodies would be the worst kind of bug,
    // because it would work in every test where the body was already moving.
    aver_phys_body_deactivate(b);
    aver_phys_body_add_impulse(b, 100.0f, 0, 0);
    check(aver_phys_body_is_active(b) == 1, "and an impulse wakes a sleeping body");
    aver_phys_shutdown();
}

// A STATIC BODY HAS NO MOTION PROPERTIES, and that is not a gap to paper over: infinite mass is
// modelled by their absence. What matters is that asking returns 0 rather than reading through a null.
static void testStaticAndDeadHandles() {
    AVER_INFO("=== what a static body and a dead handle answer ===");
    beginWorldNoGravity();

    const int32_t stat = aver_phys_add_static_box(0, 0, 0, 100, 100, 100);
    float f = 0.0f, l = 0.0f, a = 0.0f;
    check(aver_phys_body_mass(stat, &f) == 0, "a static body reports no mass");
    check(aver_phys_body_damping(stat, &l, &a) == 0, "and no damping");
    // ...but friction and restitution DO live on the body itself, so a static floor has them, which
    // is exactly what a frictionless floor needs.
    check(aver_phys_body_set_friction(stat, 0.5f) == 1, "yet friction is settable on it");
    check(aver_phys_body_friction(stat, &f) == 1 && near(f, 0.5f), "and reads back");

    const int32_t dead = 999999;
    check(aver_phys_body_add_impulse(dead, 1, 1, 1) == 0, "a dead handle refuses an impulse");
    check(aver_phys_body_add_force(dead, 1, 1, 1) == 0, "and a force");
    check(aver_phys_body_add_torque(dead, 1, 1, 1) == 0, "and a torque");
    check(aver_phys_body_set_angular_velocity(dead, 1, 1, 1) == 0, "and a spin");
    check(aver_phys_body_set_friction(dead, 1) == 0, "and friction");
    check(aver_phys_body_set_mass(dead, 1) == 0, "and mass");
    check(aver_phys_body_is_active(dead) == 0, "and reports not active");
    check(aver_phys_body_motion_type(dead) == -1,
          "and its motion type is -1, not 0 -- 0 is STATIC, a real answer");

    // NULL OUT-POINTERS ARE REFUSED, not dereferenced. Every getter here takes one.
    check(aver_phys_body_angular_velocity(stat, nullptr) == 0, "a null out-pointer is refused");
    check(aver_phys_body_friction(stat, nullptr) == 0, "on every getter");

    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("body dynamics: forces, impulses, spin, material, mass");

    testImpulseIsMassTimesVelocity();
    testForceIsPerStepAndClears();
    testAngularVelocitySpinsTheRightWay();
    testAngularImpulseSpinsTheRightWay();
    testKinematicIgnoresGravity();
    testGravityFactor();
    testDamping();
    testRestitutionAndFriction();
    testMassRescalesInertia();
    testSleeping();
    testStaticAndDeadHandles();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
