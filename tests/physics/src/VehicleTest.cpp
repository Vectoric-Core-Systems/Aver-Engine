// VehicleTest -- the wheeled-vehicle ABI in physics_vehicle_abi.h.
//
// THE STANDARD IS OBSERVED MOTION, not return codes. A vehicle built with Jolt's own axis defaults
// returns a perfectly good handle and then rolls sideways; one with the steering sign the wrong way
// round steers perfectly well, into the kerb. Nothing in the ABI can say so, so nearly every check
// below drops a car on a floor, steps the world and asks where it went -- against a number worked out
// from the car's own description rather than read off the implementation:
//
//   * it settles on four wheels, with the chassis box clear of the floor and every tyre's lowest point
//     ON the floor -- which ties the wheel poses to the pose of the origin they are relative to,
//   * the throttle drives it along +X, and a rear wheel turns the way that moves it forward,
//   * +1 on `right` turns it toward +Y and -1 toward -Y, and the front wheel's own pose shows the same
//     sign -- the steering sign is derived on paper in PhysicsVehicle.cpp and PROVED here,
//   * the brake and the handbrake stop it, reverse reverses it, a teleport leaves nothing moving,
//   * a slippery floor is slippery: the surface's friction reaches the tyres.
//
// Everything else is the lifetime contract: refusals while building, dead handles, the chassis being an
// ordinary body, and that neither removing the chassis nor shutting the world down with a live vehicle
// leaves a constraint pointing at a body that is gone.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_vehicle_abi.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;

// ---- The car ------------------------------------------------------------------------------------------

// A 440 x 180 cm saloon. The chassis box is 70 cm tall, sits 25 cm off the ground and weighs 1300 kg with
// its centre of mass 40 cm above the origin; four 33 cm tyres, 22 cm wide, on 5 to 30 cm of suspension at
// 2 Hz and a damping ratio of 0.5; the front pair steers 35 degrees and the rear pair is driven.
static const f32 kHalfLength = 220.0f, kHalfWidth = 90.0f, kHalfHeight = 35.0f, kClearance = 25.0f;
static const f32 kComZ = 40.0f, kMass = 1300.0f;
static const f32 kRadius = 33.0f, kTyreWidth = 22.0f, kSuspMin = 5.0f, kSuspMax = 30.0f;
static const f32 kAxleX = 180.0f, kTrackY = 77.0f;
static const f32 kSpringHz = 2.0f;
// THE ATTACHMENT POINT IS LOWERED BY THE SPRING'S STATIC SAG. Jolt rests the spring at its MAX length, so
// the weight compresses it by about g / (2 pi f)^2 and a wheel hung from radius + max would leave the
// origin that far under the floor; testSettlesOnItsWheels asserts the origin comes to rest on it instead.
static const f32 kSagCm = 981.0f / ((2.0f * kPi * kSpringHz) * (2.0f * kPi * kSpringHz));
static const f32 kAttachZ = kRadius + kSuspMax - kSagCm;
static const f32 kMaxSteerDeg = 35.0f;

// Wheel indices follow the order the wheels are added: 0 front-left, 1 front-right, 2 rear-left, 3
// rear-right, "left" being the -Y side.
static f32 wheelX(int i) { return i < 2 ? kAxleX : -kAxleX; }
static f32 wheelY(int i) { return (i % 2 == 0) ? -kTrackY : kTrackY; }

static int32_t buildCar(f32 x, f32 y, f32 z) {
    const int32_t v = aver_phys_vehicle_create(kHalfLength, kHalfWidth, kHalfHeight, kClearance,
                                               0.0f, 0.0f, kComZ, kMass, x, y, z, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        aver_phys_vehicle_add_wheel(v, wheelX(i), wheelY(i), kAttachZ, kRadius, kTyreWidth, kSuspMin,
                                    kSuspMax, kSpringHz, 0.5f, front ? kMaxSteerDeg : 0.0f, 1500.0f, 2500.0f,
                                    front ? 0 : 1);
    }
    aver_phys_vehicle_finish(v, 60.0f);
    return v;
}

// A floor whose top is z = 0, 400 m square. Returns its body handle.
static int32_t beginWorld() {
    aver_phys_init();
    return aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
}

static void run(int steps) {
    for (int i = 0; i < steps; ++i) aver_phys_step(kDt);
}

struct Pose { float p[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };

static Pose poseOf(int32_t v) {
    Pose r;
    aver_phys_vehicle_pose(v, r.p, r.q);
    return r;
}

static Vec3 rotated(const float q[4], const Vec3& v) { return Quat(q[0], q[1], q[2], q[3]).rotate(v); }

// How far the car turned between two poses, radians: positive toward +Y, which is the side the car's
// RIGHT is on. Measured on the forward direction, so pitch and roll do not leak into it.
static f32 yawDelta(const Pose& a, const Pose& b) {
    const Vec3 fa = rotated(a.q, Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 fb = rotated(b.q, Vec3(1.0f, 0.0f, 0.0f));
    return std::atan2(fa.x * fb.y - fa.y * fb.x, fa.x * fb.x + fa.y * fb.y);
}

static int contacts(int32_t v) {
    int n = 0;
    for (int i = 0; i < 4; ++i) n += aver_phys_vehicle_wheel_contact(v, i);
    return n;
}

// A car on the floor, settled, ready to be driven.
static int32_t settledCar() {
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f);
    run(90);
    return v;
}

// ---------------------------------------------------------------------------------------------------

static void testSettlesOnItsWheels() {
    AVER_INFO("=== a car dropped onto a floor settles on its four wheels ===");
    beginWorld();
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f);
    check(v != 0, "the vehicle builds");
    check(aver_phys_vehicle_wheel_count(v) == 4, "it has four wheels");
    run(120);

    const Pose p = poseOf(v);
    check(contacts(v) == 4, "all four wheels touch the floor, " + std::to_string(contacts(v)) + " do");
    // RIDE HEIGHT. The origin is where a mesh's own origin is, the bottom of its tyres, so it must come to
    // rest ON the floor -- which it does only because the attachment points were lowered by the spring's
    // sag (kSagCm). Hung from radius + max instead, it settles that far under, and the tolerance below
    // is far smaller than the 7 cm or so it would sink to. It is not zero: Jolt sizes the spring from an
    // effective mass of about 280 kg against the 325 kg each wheel carries (worked by hand from
    // VehicleConstraint.cpp, not measured), so the real sag is about 15 percent more than kSagCm and the
    // origin settles about a centimetre under the floor.
    check(near(p.p[2], 0.0f, 2.5f), "the origin rests on the floor, to within 2.5 cm: z = " + f2s(p.p[2]));
    // And the chassis box, 25 cm above it, clears the floor by a wide margin.
    check(p.p[2] > -20.0f, "with the chassis box clear of the floor, origin z = " + f2s(p.p[2]));
    float vel[3] = {9, 9, 9};
    aver_phys_vehicle_velocity(v, vel);
    check(std::fabs(vel[0]) < 5.0f && std::fabs(vel[1]) < 5.0f && std::fabs(vel[2]) < 5.0f,
          "and it is at rest, v = (" + f2s(vel[0]) + ", " + f2s(vel[1]) + ", " + f2s(vel[2]) + ")");

    // THE CHASSIS IS AN ORDINARY BODY, whose position is its centre of mass -- 40 cm above the origin,
    // which is what the vehicle was told. The pose is the ORIGIN.
    const int32_t body = aver_phys_vehicle_body(v);
    float com[3] = {0, 0, 0};
    check(body != 0 && aver_phys_body_position(body, com) == 1, "the chassis is a registered body");
    check(near(com[2], p.p[2] + kComZ, 3.0f) && near(com[0], p.p[0], 3.0f) && near(com[1], p.p[1], 3.0f),
          "its centre of mass is 40 cm above the origin the pose reports, z = " + f2s(com[2]) +
          " against origin " + f2s(p.p[2]));

    // Every wheel hangs under its own attachment point, with its lowest point ON the floor, and its axle
    // along engine Y -- a wheel posed with the wrong model axes would have a different axle.
    for (int i = 0; i < 4; ++i) {
        float wp[3] = {0, 0, 0}, wq[4] = {0, 0, 0, 1};
        const bool ok = aver_phys_vehicle_wheel_pose(v, i, wp, wq) == 1;
        const std::string n = "wheel " + std::to_string(i);
        check(ok && near(wp[0], wheelX(i), 0.5f) && near(wp[1], wheelY(i), 0.5f),
              n + " hangs under its attachment point, (" + f2s(wp[0]) + ", " + f2s(wp[1]) + ")");
        check(wp[2] < kAttachZ - kSuspMin + 0.5f && wp[2] > kAttachZ - kSuspMax - 0.5f,
              n + " sits inside its suspension travel, z = " + f2s(wp[2]));
        check(near(p.p[2] + wp[2] - kRadius, 0.0f, 3.0f),
              n + "'s lowest point is on the floor, " + f2s(p.p[2] + wp[2] - kRadius));
        const Vec3 axle = rotated(wq, Vec3(0.0f, 1.0f, 0.0f));
        check(near(axle.y, 1.0f, 0.02f), n + "'s axle is engine Y, " + f2s(axle.y));
    }

    // And the ordinary machinery works on it: a ray from above hits the roof and reads the entity stamp.
    aver_phys_set_entity(body, 77);
    float hit[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
    int32_t entity = 0;
    const int32_t hitBody = aver_phys_raycast(0.0f, 0.0f, 500.0f, 0.0f, 0.0f, -1.0f, 1000.0f, hit, normal, &entity);
    check(hitBody == body && entity == 77, "a ray from above hits the chassis and reads its entity stamp");
    check(near(hit[2], p.p[2] + kClearance + 2.0f * kHalfHeight, 3.0f),
          "and lands on the chassis box's roof, z = " + f2s(hit[2]));

    aver_phys_shutdown();
}

static void testDrivesForward() {
    AVER_INFO("=== the throttle drives it along +X ===");
    beginWorld();
    const int32_t v = settledCar();
    const Pose start = poseOf(v);
    check(aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f) == 1, "the input is accepted");
    run(180);

    const Pose p = poseOf(v);
    const f32 speed = aver_phys_vehicle_forward_speed(v);
    check(speed > 500.0f, "three seconds of throttle reach more than 5 m/s, " + f2s(speed) + " cm/s");
    check(p.p[0] - start.p[0] > 300.0f, "it travelled along +X, " + f2s(p.p[0] - start.p[0]) + " cm");
    check(std::fabs(p.p[1] - start.p[1]) < 150.0f, "and held a straight line, drifted " + f2s(p.p[1] - start.p[1]));
    check(rotated(p.q, Vec3(0.0f, 0.0f, 1.0f)).z > 0.9f, "it stayed on its wheels");
    check(contacts(v) >= 3, "with the tyres on the floor, " + std::to_string(contacts(v)));

    // The speed reading and the velocity reading are the same motion.
    float vel[3] = {0, 0, 0};
    aver_phys_vehicle_velocity(v, vel);
    const Vec3 facing = rotated(p.q, Vec3(1.0f, 0.0f, 0.0f));
    check(near(vel[0] * facing.x + vel[1] * facing.y + vel[2] * facing.z, speed, 1.0f),
          "forward_speed is the velocity along the chassis' +X");

    // A driven rear wheel turns the way that carries the car forward: a quarter turn of the wheel mesh
    // about its +Y axle, top toward +X. Taken as the rotation between two steps in the wheel's own frame.
    float wp[3] = {0, 0, 0}, q0[4] = {0, 0, 0, 1}, q1[4] = {0, 0, 0, 1};
    aver_phys_vehicle_wheel_pose(v, 2, wp, q0);
    aver_phys_step(kDt);
    aver_phys_vehicle_wheel_pose(v, 2, wp, q1);
    const Quat a(q0[0], q0[1], q0[2], q0[3]), b(q1[0], q1[1], q1[2], q1[3]);
    Quat spin = Quat(-a.x, -a.y, -a.z, a.w) * b;
    if (spin.w < 0.0f) spin = Quat(-spin.x, -spin.y, -spin.z, -spin.w);
    check(spin.y > 0.05f && std::fabs(spin.x) < 0.05f && std::fabs(spin.z) < 0.05f,
          "a rear wheel spins about +Y, the way that rolls it forward, (" + f2s(spin.x) + ", " +
          f2s(spin.y) + ", " + f2s(spin.z) + ")");

    aver_phys_shutdown();
}

static void testReverse() {
    AVER_INFO("=== negative throttle reverses ===");
    beginWorld();
    const int32_t v = settledCar();
    const Pose start = poseOf(v);
    aver_phys_vehicle_set_input(v, -1.0f, 0.0f, 0.0f, 0.0f);
    run(120);
    const f32 speed = aver_phys_vehicle_forward_speed(v);
    const Pose p = poseOf(v);
    check(speed < -100.0f, "two seconds of reverse throttle, forward speed " + f2s(speed) + " cm/s");
    check(p.p[0] - start.p[0] < -100.0f, "it backed along -X, " + f2s(p.p[0] - start.p[0]) + " cm");
    aver_phys_shutdown();
}

struct Turn {
    f32 yaw = 0.0f, dy = 0.0f;
    f32 frontAxleX = 0.0f, rearAxleX = 0.0f, fullLockAxleX = 0.0f;
};

// Drives up to speed, steers by `right` for a second, and reads how the car and its wheels answered.
static Turn turn(f32 right) {
    beginWorld();
    const int32_t v = settledCar();
    aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
    run(90);
    const Pose before = poseOf(v);
    aver_phys_vehicle_set_input(v, 0.5f, right, 0.0f, 0.0f);
    run(60);
    const Pose after = poseOf(v);

    Turn t;
    t.yaw = yawDelta(before, after);
    t.dy = after.p[1] - before.p[1];
    float wp[3] = {0, 0, 0}, wq[4] = {0, 0, 0, 1};
    aver_phys_vehicle_wheel_pose(v, 0, wp, wq);
    t.frontAxleX = rotated(wq, Vec3(0.0f, 1.0f, 0.0f)).x;
    aver_phys_vehicle_wheel_pose(v, 2, wp, wq);
    t.rearAxleX = rotated(wq, Vec3(0.0f, 1.0f, 0.0f)).x;
    // Past full lock: the input is clamped, so the wheel turns the full 35 degrees and no further.
    aver_phys_vehicle_set_input(v, 0.5f, right * 3.0f, 0.0f, 0.0f);
    aver_phys_step(kDt);
    aver_phys_vehicle_wheel_pose(v, 0, wp, wq);
    t.fullLockAxleX = rotated(wq, Vec3(0.0f, 1.0f, 0.0f)).x;
    aver_phys_shutdown();
    return t;
}

// THE STEERING SIGN, PROVED. +1 on `right` must turn the car toward +Y, whatever the derivation in
// PhysicsVehicle.cpp says: the car's yaw, where it ended up, and the front wheel's own pose all agree.
// A front wheel's forward turned toward +Y puts its axle at (-sin, cos, 0), so the axle's x is negative
// for a right turn.
static void testSteeringSign() {
    AVER_INFO("=== +right turns toward +Y and -right toward -Y ===");
    const Turn r = turn(0.6f);
    check(r.yaw > 0.3f, "right turns the car toward +Y, yaw " + f2s(r.yaw) + " rad");
    check(r.dy > 60.0f, "and it ended up on the +Y side, " + f2s(r.dy) + " cm");
    check(r.frontAxleX < -0.25f, "the front wheel's axle swings to -X, " + f2s(r.frontAxleX));
    check(std::fabs(r.rearAxleX) < 0.02f, "the rear wheel does not steer, " + f2s(r.rearAxleX));
    check(near(r.fullLockAxleX, -std::sin(kMaxSteerDeg * kDegToRad), 0.03f),
          "and an input past 1 is clamped to the wheel's own 35 degrees, " + f2s(r.fullLockAxleX));

    const Turn l = turn(-0.6f);
    check(l.yaw < -0.3f, "left turns the car toward -Y, yaw " + f2s(l.yaw) + " rad");
    check(l.dy < -60.0f, "and it ended up on the -Y side, " + f2s(l.dy) + " cm");
    check(l.frontAxleX > 0.25f, "the front wheel's axle swings to +X, " + f2s(l.frontAxleX));
    check(near(l.fullLockAxleX, std::sin(kMaxSteerDeg * kDegToRad), 0.03f),
          "the same full lock the other way, " + f2s(l.fullLockAxleX));
}

// Drives up to speed, then holds the brake and handbrake as given, and returns the speed five seconds
// later. `before` is the speed it was braked from.
static f32 stoppingSpeed(f32 brake, f32 handbrake, f32& before) {
    beginWorld();
    const int32_t v = settledCar();
    aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
    run(150);
    before = aver_phys_vehicle_forward_speed(v);
    aver_phys_vehicle_set_input(v, 0.0f, 0.0f, brake, handbrake);
    run(300);
    const f32 after = aver_phys_vehicle_forward_speed(v);
    aver_phys_shutdown();
    return after;
}

static void testBrakes() {
    AVER_INFO("=== the brake and the handbrake stop it ===");
    f32 before = 0.0f;
    f32 after = stoppingSpeed(1.0f, 0.0f, before);
    check(before > 500.0f && std::fabs(after) < 30.0f,
          "the brake stops it from " + f2s(before) + " cm/s, to " + f2s(after));
    after = stoppingSpeed(0.0f, 1.0f, before);
    check(before > 500.0f && std::fabs(after) < 30.0f,
          "so does the handbrake, from " + f2s(before) + " cm/s to " + f2s(after));
    after = stoppingSpeed(0.0f, 0.0f, before);
    check(after > before * 0.25f, "while coasting does not, " + f2s(before) + " cm/s to " + f2s(after));
}

// Accelerates for three seconds on a floor of the given friction and returns the speed reached.
static f32 speedOnFloorOfFriction(f32 friction) {
    const int32_t floor = beginWorld();
    aver_phys_body_set_friction(floor, friction);
    const int32_t v = settledCar();
    aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
    run(180);
    const f32 speed = aver_phys_vehicle_forward_speed(v);
    aver_phys_shutdown();
    return speed;
}

// The surface's friction reaches the tyres. A stock floor has Jolt's default 0.2 and must give the tyre
// its full grip -- the whole reason the combine is not Jolt's geometric mean -- and a floor at a tenth of
// that gives a tenth of it.
static void testSurfaceFriction() {
    AVER_INFO("=== a slippery floor is slippery ===");
    const f32 normal = speedOnFloorOfFriction(0.2f);
    const f32 ice = speedOnFloorOfFriction(0.02f);
    check(normal > 500.0f, "the default floor lets it reach " + f2s(normal) + " cm/s");
    check(ice < normal * 0.4f, "a floor at a tenth of that friction does not: " + f2s(ice) + " cm/s");
}

static void testTeleport() {
    AVER_INFO("=== set_pose puts it somewhere else and leaves nothing moving ===");
    beginWorld();
    const int32_t v = settledCar();
    aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
    // Five seconds of throttle, long enough for the automatic box to have left first gear: a teleport
    // that left the gearbox where it was would have the idling engine drag the stopped wheels along in
    // whatever gear it was in, and a car caught in first gear would hide that.
    run(300);
    check(aver_phys_vehicle_forward_speed(v) > 300.0f, "it is moving before the teleport");
    aver_phys_vehicle_set_input(v, 0.0f, 0.0f, 0.0f, 0.0f);

    const Quat yaw90 = Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), kPi * 0.5f);
    check(aver_phys_vehicle_set_pose(v, 4000.0f, 800.0f, 5.0f, yaw90.x, yaw90.y, yaw90.z, yaw90.w) == 1,
          "set_pose is accepted");
    Pose p = poseOf(v);
    check(near(p.p[0], 4000.0f, 0.5f) && near(p.p[1], 800.0f, 0.5f) && near(p.p[2], 5.0f, 0.5f),
          "the origin is exactly where it was put, (" + f2s(p.p[0]) + ", " + f2s(p.p[1]) + ", " + f2s(p.p[2]) + ")");
    const Vec3 facing = rotated(p.q, Vec3(1.0f, 0.0f, 0.0f));
    check(near(facing.x, 0.0f, 0.02f) && near(facing.y, 1.0f, 0.02f), "it now faces +Y");
    float vel[3] = {9, 9, 9};
    aver_phys_vehicle_velocity(v, vel);
    check(std::fabs(vel[0]) < 1.0f && std::fabs(vel[1]) < 1.0f && std::fabs(vel[2]) < 1.0f,
          "and it is stopped dead");

    // The wheels were stopped too: ones still turning at the old speed would drive it off from here. A
    // half second in, with no brake and no throttle, nothing has had time to settle a drive-off away.
    run(30);
    check(std::fabs(aver_phys_vehicle_forward_speed(v)) < 100.0f,
          "no throttle and no brake: it does not creep off in its old gear, " +
          f2s(aver_phys_vehicle_forward_speed(v)) + " cm/s");
    run(90);
    p = poseOf(v);
    check(contacts(v) == 4, "it settles on its four wheels there");
    check(std::fabs(aver_phys_vehicle_forward_speed(v)) < 20.0f, "without driving off, " +
          f2s(aver_phys_vehicle_forward_speed(v)) + " cm/s");
    check(near(p.p[0], 4000.0f, 60.0f) && near(p.p[1], 800.0f, 60.0f), "it is still where it was put");
    aver_phys_shutdown();
}

static void testBuilderRefusals() {
    AVER_INFO("=== a vehicle that is not one is refused while it is built ===");
    beginWorld();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    check(aver_phys_vehicle_create(0.0f, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1) == 0,
          "a chassis with no length is refused");
    check(aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 0.0f, 0, 0, 5, 0, 0, 0, 1) == 0,
          "a chassis with no mass is refused");
    check(aver_phys_vehicle_create(220, 90, 35, -1.0f, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1) == 0,
          "a negative ground clearance is refused");
    check(aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, nan, 0, 5, 0, 0, 0, 1) == 0,
          "a NaN position is refused");

    const int32_t v = aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 0);
    check(v != 0, "an all-zero rotation is read as no rotation, not refused");
    const Pose unbuilt = poseOf(v);
    check(near(unbuilt.p[2], 5.0f, 0.01f) && near(unbuilt.q[3], 1.0f, 0.001f),
          "an unfinished vehicle already has a pose");
    check(aver_phys_vehicle_finish(v, 60.0f) == 0, "finishing with no wheels is refused");
    check(aver_phys_vehicle_set_input(v, 1, 0, 0, 0) == 0, "driving an unfinished vehicle is refused");
    float wp[3] = {0, 0, 0}, wq[4] = {0, 0, 0, 1};
    check(aver_phys_vehicle_wheel_pose(v, 0, wp, wq) == 0, "and so is reading a wheel that is not there");

    check(aver_phys_vehicle_add_wheel(v, 180, -77, 63, 0.0f, 22, 5, 30, 2, 0.5f, 35, 1500, 2500, 0) == -1,
          "a wheel with no radius is refused");
    check(aver_phys_vehicle_add_wheel(v, 180, -77, 63, 33, 22, 30, 30, 2, 0.5f, 35, 1500, 2500, 0) == -1,
          "a suspension with no travel is refused");
    check(aver_phys_vehicle_add_wheel(v, 180, -77, 63, 33, 22, 5, 30, 0.0f, 0.5f, 35, 1500, 2500, 0) == -1,
          "a spring with no frequency is refused");
    check(aver_phys_vehicle_wheel_count(v) == 0, "none of those was added");

    check(aver_phys_vehicle_add_wheel(v, 180, -77, 63, 33, 22, 5, 30, 2, 0.5f, 35, 1500, 2500, 0) == 0,
          "the first wheel is index 0");
    check(aver_phys_vehicle_add_wheel(v, 180, 77, 63, 33, 22, 5, 30, 2, 0.5f, 35, 1500, 2500, 0) == 1,
          "the second is index 1");
    check(aver_phys_vehicle_finish(v, 60.0f) == 0, "with no driven wheel the engine would drive nothing: refused");
    check(aver_phys_vehicle_set_engine(v, 0.0f, 1000, 6000) == 0, "an engine with no torque is refused");
    check(aver_phys_vehicle_set_engine(v, 500, 3000, 3000) == 0, "and one whose rev limit is its idle");
    check(aver_phys_vehicle_set_engine(v, 800, 800, 5000) == 1, "a sane engine is accepted");

    check(aver_phys_vehicle_add_wheel(v, -180, -77, 63, 33, 22, 5, 30, 2, 0.5f, 0, 1500, 2500, 1) == 2,
          "a driven wheel, alone on its axle, is index 2");
    check(aver_phys_vehicle_finish(v, 0.0f) == 1, "a three-wheeler with a driven wheel finishes");
    check(aver_phys_vehicle_finish(v, 60.0f) == 0, "a second finish is refused");
    check(aver_phys_vehicle_add_wheel(v, -180, 77, 63, 33, 22, 5, 30, 2, 0.5f, 0, 1500, 2500, 1) == -1,
          "no wheel is added once it is finished");
    check(aver_phys_vehicle_set_engine(v, 500, 1000, 6000) == 0, "nor the engine changed");
    check(aver_phys_vehicle_wheel_count(v) == 3, "it keeps its three wheels");
    check(aver_phys_vehicle_set_input(v, 1, 0, 0, 0) == 1, "and can now be driven");
    run(30);
    check(aver_phys_ready() == 1, "and stepped");
    aver_phys_shutdown();
}

static void testDeadHandles() {
    AVER_INFO("=== dead handles and null pointers ===");
    beginWorld();
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f);
    const int32_t body = aver_phys_vehicle_body(v);
    float a[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};

    check(aver_phys_vehicle_pose(v, nullptr, q) == 0 && aver_phys_vehicle_pose(v, a, nullptr) == 0,
          "pose with a null out-pointer is refused");
    check(aver_phys_vehicle_velocity(v, nullptr) == 0, "so is velocity");
    check(aver_phys_vehicle_wheel_pose(v, 0, nullptr, q) == 0, "so is a wheel pose");
    check(aver_phys_vehicle_wheel_pose(v, 4, a, q) == 0 && aver_phys_vehicle_wheel_pose(v, -1, a, q) == 0,
          "and a wheel index past either end");
    check(aver_phys_vehicle_wheel_contact(v, 4) == 0 && aver_phys_vehicle_wheel_contact(v, -1) == 0,
          "likewise a contact query");

    check(aver_phys_vehicle_destroy(v) == 1, "destroy succeeds");
    check(aver_phys_body_position(body, a) == 0, "and the chassis went with it");
    check(aver_phys_vehicle_destroy(v) == 0, "destroying twice is refused");

    for (const int32_t dead : {v, 0, 99999}) {
        const std::string n = "handle " + std::to_string(dead);
        check(aver_phys_vehicle_pose(dead, a, q) == 0 && aver_phys_vehicle_velocity(dead, a) == 0,
              n + ": pose and velocity are refused");
        check(aver_phys_vehicle_forward_speed(dead) == 0.0f && aver_phys_vehicle_wheel_count(dead) == 0 &&
              aver_phys_vehicle_body(dead) == 0, n + ": speed, wheel count and body are 0");
        check(aver_phys_vehicle_wheel_pose(dead, 0, a, q) == 0 && aver_phys_vehicle_wheel_contact(dead, 0) == 0,
              n + ": wheel queries are refused");
        check(aver_phys_vehicle_set_input(dead, 1, 0, 0, 0) == 0 &&
              aver_phys_vehicle_set_pose(dead, 0, 0, 0, 0, 0, 0, 1) == 0, n + ": driving and teleporting are refused");
        check(aver_phys_vehicle_finish(dead, 60.0f) == 0 && aver_phys_vehicle_set_engine(dead, 500, 1000, 6000) == 0 &&
              aver_phys_vehicle_add_wheel(dead, 0, 0, 0, 33, 22, 5, 30, 2, 0.5f, 0, 0, 0, 1) == -1,
              n + ": building on it is refused");
    }
    aver_phys_shutdown();
}

static void testRemovingTheChassisDestroysTheVehicle() {
    AVER_INFO("=== removing a chassis destroys its vehicle first ===");
    beginWorld();
    const int32_t first = buildCar(0.0f, 0.0f, 5.0f);
    const int32_t second = buildCar(2000.0f, 0.0f, 5.0f);
    run(60);

    const int32_t body = aver_phys_vehicle_body(first);
    check(aver_phys_remove_body(body) == 1, "the chassis is removed as any body is");
    check(aver_phys_vehicle_body(first) == 0 && aver_phys_vehicle_set_input(first, 1, 0, 0, 0) == 0,
          "and the vehicle behind it is gone");
    run(60);   // a constraint still pointing at the destroyed body would die here
    check(contacts(second) == 4 && poseOf(second).p[2] > -20.0f, "the other vehicle never noticed");
    aver_phys_shutdown();
}

static void testShutdownWithLiveVehicle() {
    AVER_INFO("=== shutting down with a live vehicle ===");
    beginWorld();
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f);
    buildCar(1500.0f, 0.0f, 5.0f);
    aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
    run(30);
    aver_phys_shutdown();
    check(aver_phys_ready() == 0, "the world is gone");
    check(aver_phys_vehicle_body(v) == 0 && aver_phys_vehicle_forward_speed(v) == 0.0f,
          "and the vehicle handle with it");

    beginWorld();
    const int32_t w = buildCar(0.0f, 0.0f, 5.0f);
    run(90);
    check(w != 0 && contacts(w) == 4, "a fresh world builds a car that settles on its wheels");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("vehicles: wheels, driving, steering sign, braking, lifetime");

    testSettlesOnItsWheels();
    testDrivesForward();
    testReverse();
    testSteeringSign();
    testBrakes();
    testSurfaceFriction();
    testTeleport();
    testBuilderRefusals();
    testDeadHandles();
    testRemovingTheChassisDestroysTheVehicle();
    testShutdownWithLiveVehicle();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
