// RaycastVehicleTest -- Godot's VehicleBody3D as a raycast vehicle (physics_raycast_vehicle_abi.h, G2), checked by driving a car on a floor.
//
//   1. refusals and read-back: handles, the wheel block, params, the mask, the broadcast setters, removing the chassis,
//   2. the suspension: a wheel in the air reports the rest length; under load it settles at rest - g / (N x stiffness), whatever the mass,
//      carrying the weight; the wheel pose follows,
//   3. drive: engine force accelerates (only through TRACTION wheels), a free car coasts, the brake stops it without reversing,
//   4. steering turns toward +Y for positive angles and -Y for negative ones (only through STEERING wheels), rpm and spin follow the ground,
//   5. friction_slip limits a skid: thrust, and a sideways slide, against a grippy twin,
//   6. roll_influence lowers the height side friction acts at (less roll),
//   7. a dynamic ground is pushed back (momentum), the wheel mask, sleeping and waking,
//   8. determinism: two worlds agree bit for bit, and a snapshot restore replays a driven run exactly.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_raycast_vehicle_abi.h"
#include "aver/physics/physics_state_abi.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static const float kDt = 1.0f / 60.0f;
static const int32_t kBadHandle = -1, kNullPointer = -2, kOutOfRange = -4, kInvalidArgument = -6;

static void stepN(int n) { for (int i = 0; i < n; ++i) aver_phys_step(kDt); }
static float posOf(int32_t b, int axis) { float p[3] = {0, 0, 0}; aver_phys_body_position(b, p); return p[axis]; }
static float velOf(int32_t b, int axis) { float v[3] = {0, 0, 0}; aver_phys_body_velocity(b, v); return v[axis]; }
static int32_t floorBox() { return aver_phys_add_static_box(0, 0, -50.0f, 6000.0f, 6000.0f, 50.0f); }   // top at z = 0

// The body's forward (+X) and up (+Z) rotated into the world.
static void axes(int32_t b, float* fwd, float* up) {
    float q[4] = {0, 0, 0, 1};
    aver_phys_body_rotation(b, q);
    auto rot = [&](float vx, float vy, float vz, float* out) {
        const float cx = q[1] * vz - q[2] * vy, cy = q[2] * vx - q[0] * vz, cz = q[0] * vy - q[1] * vx;
        const float tx = cx + q[3] * vx, ty = cy + q[3] * vy, tz = cz + q[3] * vz;
        out[0] = vx + 2.0f * (q[1] * tz - q[2] * ty);
        out[1] = vy + 2.0f * (q[2] * tx - q[0] * tz);
        out[2] = vz + 2.0f * (q[0] * ty - q[1] * tx);
    };
    if (fwd) rot(1, 0, 0, fwd);
    if (up) rot(0, 0, 1, up);
}

static float rs(int32_t veh, int w, int idx) {
    float s[AVER_PHYS_RAYS_COUNT] = {};
    aver_phys_rayveh_wheel_state(veh, w, s, AVER_PHYS_RAYS_COUNT);
    return s[idx];
}

// ---- a car --------------------------------------------------------------------------------------------------------------------------------
// 1000 kg, 300 x 160 x 40 cm, origin at its centre. Wheels 0,1 front (x +110) steer; 2,3 rear (x -110) drive. Rest 30 cm, travel 30, radius 35.
// Sag under load = g / (4 x 20) = 12.25 cm, so a wheel settles at 17.75 cm and the centre at 62.75 cm.
struct Car { int32_t body = 0, veh = 0; };

static const float kStiff = 20.0f;
static const float kSagCm = 980.0f / (4.0f * kStiff);
static const float kRestCm = 30.0f, kRadiusCm = 35.0f;
static const float kSettledZ = (kRestCm - kSagCm) + kRadiusCm + 10.0f;   // wheel connection sits 10 cm below the origin

static void wheelBlock(float* p, float x, float y, bool steer, bool drive, float slip, float roll) {
    for (int i = 0; i < AVER_PHYS_RAYW_COUNT; ++i) p[i] = 0.0f;
    p[AVER_PHYS_RAYW_CONNECT_X] = x; p[AVER_PHYS_RAYW_CONNECT_Y] = y; p[AVER_PHYS_RAYW_CONNECT_Z] = -10.0f;
    p[AVER_PHYS_RAYW_DIR_Z] = -1.0f; p[AVER_PHYS_RAYW_AXLE_Y] = 1.0f;
    p[AVER_PHYS_RAYW_REST_LENGTH] = kRestCm; p[AVER_PHYS_RAYW_MAX_TRAVEL] = 30.0f;
    p[AVER_PHYS_RAYW_STIFFNESS] = kStiff; p[AVER_PHYS_RAYW_DAMP_COMPRESSION] = 2.5f; p[AVER_PHYS_RAYW_DAMP_RELAXATION] = 3.0f;
    p[AVER_PHYS_RAYW_MAX_FORCE] = 0.0f; p[AVER_PHYS_RAYW_RADIUS] = kRadiusCm;
    p[AVER_PHYS_RAYW_FRICTION_SLIP] = slip; p[AVER_PHYS_RAYW_ROLL_INFLUENCE] = roll;
    p[AVER_PHYS_RAYW_TRACTION] = drive ? 1.0f : 0.0f; p[AVER_PHYS_RAYW_STEERING] = steer ? 1.0f : 0.0f;
}

static Car makeCar(float x, float y, float z, float mass = 1000.0f, float slip = 1.0f, float roll = 0.1f) {
    Car c;
    c.body = aver_phys_add_dynamic_box(x, y, z, 150.0f, 80.0f, 20.0f, mass);
    c.veh = aver_phys_rayveh_create(c.body);
    float p[AVER_PHYS_RAYW_COUNT];
    wheelBlock(p, 110, -70, true, false, slip, roll);  aver_phys_rayveh_add_wheel(c.veh, p, AVER_PHYS_RAYW_COUNT);
    wheelBlock(p, 110, 70, true, false, slip, roll);   aver_phys_rayveh_add_wheel(c.veh, p, AVER_PHYS_RAYW_COUNT);
    wheelBlock(p, -110, -70, false, true, slip, roll); aver_phys_rayveh_add_wheel(c.veh, p, AVER_PHYS_RAYW_COUNT);
    wheelBlock(p, -110, 70, false, true, slip, roll);  aver_phys_rayveh_add_wheel(c.veh, p, AVER_PHYS_RAYW_COUNT);
    return c;
}

static Car settledCar(float x = 0, float y = 0, float mass = 1000.0f, float slip = 1.0f, float roll = 0.1f) {
    Car c = makeCar(x, y, kSettledZ + 6.0f, mass, slip, roll);
    stepN(240);
    return c;
}

static int contacts(const Car& c) { int n = 0; for (int w = 0; w < 4; ++w) n += static_cast<int>(rs(c.veh, w, AVER_PHYS_RAYS_IN_CONTACT)); return n; }

// ---- 1. refusals and read-back ---------------------------------------------------------------------------------------------------------------

static void testRefusals() {
    AVER_INFO("--- refusals and read-back");
    check(aver_phys_rayveh_abi_version() == AVER_PHYS_RAYVEH_ABI_VERSION && AVER_PHYS_RAYVEH_ABI_VERSION == 1, "ABI version 1");
    check(aver_phys_rayveh_create(1) == 0, "no world: create refused");
    check(aver_phys_last_error() == static_cast<int32_t>(AbiError::NotInitialised), "no world: NotInitialised");
    aver_phys_init();
    const int32_t floor = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 100.0f, 50, 50, 50, 10.0f);
    check(aver_phys_rayveh_create(floor) == 0 && aver_phys_last_error() == kInvalidArgument, "a static body is not a chassis");
    check(aver_phys_rayveh_create(9999) == 0 && aver_phys_last_error() == kBadHandle, "a dead body handle is refused");
    const int32_t v = aver_phys_rayveh_create(box);
    check(v > 0 && aver_phys_rayveh_exists(v) == 1 && aver_phys_rayveh_count() == 1 && aver_phys_rayveh_chassis(v) == box, "create: handle, exists, count, chassis");
    check(aver_phys_rayveh_create(box) == 0, "a second vehicle on one chassis is refused");
    check(aver_phys_rayveh_wheel_count(v) == 0 && aver_phys_rayveh_wheel_count(777) == -1, "wheel count 0; dead vehicle -1");

    float p[AVER_PHYS_RAYW_COUNT];
    wheelBlock(p, 10, 20, true, true, 1.5f, 0.5f);
    const int32_t w0 = aver_phys_rayveh_add_wheel(v, p, AVER_PHYS_RAYW_COUNT);
    check(w0 == 0 && aver_phys_rayveh_wheel_count(v) == 1, "first wheel is index 0");
    float out = 0;
    bool all = true;
    for (int k = 0; k < AVER_PHYS_RAYW_COUNT; ++k) { out = -999; all = all && aver_phys_rayveh_wheel_param(v, w0, k, &out) == 1 && out == p[k]; }
    check(all, "every block value reads back");
    // a short block: the rest are Aver's defaults
    const float shortBlock[3] = {5, 6, 7};
    const int32_t w1 = aver_phys_rayveh_add_wheel(v, shortBlock, 3);
    float dz = 0, ay = 0, rad = 0;
    aver_phys_rayveh_wheel_param(v, w1, AVER_PHYS_RAYW_DIR_Z, &dz);
    aver_phys_rayveh_wheel_param(v, w1, AVER_PHYS_RAYW_AXLE_Y, &ay);
    aver_phys_rayveh_wheel_param(v, w1, AVER_PHYS_RAYW_RADIUS, &rad);
    check(w1 == 1 && dz == -1.0f && ay == 1.0f && rad > 0.0f, "a short block keeps the defaults (down, +Y axle, a radius)");
    check(aver_phys_rayveh_add_wheel(v, p, AVER_PHYS_RAYW_COUNT + 1) == -1 && aver_phys_last_error() == kInvalidArgument, "a block longer than COUNT is refused");
    check(aver_phys_rayveh_add_wheel(v, nullptr, 3) == -1 && aver_phys_last_error() == kNullPointer, "a null block is refused");
    float bad[AVER_PHYS_RAYW_COUNT];
    wheelBlock(bad, 0, 0, false, true, 1, 1); bad[AVER_PHYS_RAYW_RADIUS] = 0.0f;
    check(aver_phys_rayveh_add_wheel(v, bad, AVER_PHYS_RAYW_COUNT) == -1, "radius 0 is refused");
    wheelBlock(bad, 0, 0, false, true, 1, 1); bad[AVER_PHYS_RAYW_DIR_Z] = 0.0f;
    check(aver_phys_rayveh_add_wheel(v, bad, AVER_PHYS_RAYW_COUNT) == -1, "a zero suspension direction is refused");
    wheelBlock(bad, 0, 0, false, true, 1, 1); bad[AVER_PHYS_RAYW_AXLE_Y] = 0.0f;
    check(aver_phys_rayveh_add_wheel(v, bad, AVER_PHYS_RAYW_COUNT) == -1, "a zero axle is refused");
    wheelBlock(bad, 0, 0, false, true, 1, 1); bad[AVER_PHYS_RAYW_STIFFNESS] = -1.0f;
    check(aver_phys_rayveh_add_wheel(v, bad, AVER_PHYS_RAYW_COUNT) == -1, "a negative stiffness is refused");
    wheelBlock(bad, 0, 0, false, true, 1, 1); bad[AVER_PHYS_RAYW_FRICTION_SLIP] = NAN;
    check(aver_phys_rayveh_add_wheel(v, bad, AVER_PHYS_RAYW_COUNT) == -1, "a NaN is refused");
    check(aver_phys_rayveh_wheel_count(v) == 2, "refused blocks added nothing");

    check(aver_phys_rayveh_set_wheel_param(v, 0, AVER_PHYS_RAYW_FRICTION_SLIP, 0.4f) == 1, "set_wheel_param");
    aver_phys_rayveh_wheel_param(v, 0, AVER_PHYS_RAYW_FRICTION_SLIP, &out);
    check(near(out, 0.4f, 1e-6f), "set_wheel_param reads back");
    check(aver_phys_rayveh_set_wheel_param(v, 0, AVER_PHYS_RAYW_RADIUS, -2.0f) == 0, "set_wheel_param: a bad value is refused");
    check(aver_phys_rayveh_set_wheel_param(v, 0, AVER_PHYS_RAYW_COUNT, 1.0f) == 0 && aver_phys_last_error() == kOutOfRange, "set_wheel_param: a bad key is out of range");
    check(aver_phys_rayveh_set_wheel_param(v, 5, AVER_PHYS_RAYW_RADIUS, 30.0f) == 0 && aver_phys_last_error() == kOutOfRange, "set_wheel_param: a bad wheel is out of range");
    check(aver_phys_rayveh_wheel_param(v, 0, AVER_PHYS_RAYW_RADIUS, nullptr) == 0 && aver_phys_last_error() == kNullPointer, "wheel_param: a null out is refused");
    aver_phys_rayveh_set_wheel_param(v, 0, AVER_PHYS_RAYW_TRACTION, 7.0f);
    aver_phys_rayveh_wheel_param(v, 0, AVER_PHYS_RAYW_TRACTION, &out);
    check(out == 1.0f, "a flag is stored as 0 or 1");

    uint32_t mask = 0;
    check(aver_phys_rayveh_wheel_mask(v, 0, &mask) == 1 && mask == 0xFFFFFFFFu, "the mask defaults to all layers");
    check(aver_phys_rayveh_set_wheel_mask(v, 0, 0x6u) == 1 && aver_phys_rayveh_wheel_mask(v, 0, &mask) == 1 && mask == 0x6u, "mask set and read back");

    check(aver_phys_rayveh_set_engine_force(v, -1, 1000.0f) == 1 && aver_phys_rayveh_set_brake(v, -1, 50.0f) == 1 && aver_phys_rayveh_set_steering(v, -1, 0.2f) == 1, "drive setters accept -1 = every wheel");
    check(aver_phys_rayveh_set_engine_force(v, 9, 1.0f) == 0 && aver_phys_last_error() == kOutOfRange, "drive setter: a bad wheel is out of range");
    check(aver_phys_rayveh_set_brake(v, 0, -1.0f) == 0 && aver_phys_last_error() == kInvalidArgument, "a negative brake is refused");
    check(aver_phys_rayveh_set_steering(v, 0, NAN) == 0, "a NaN steering is refused");
    float st[AVER_PHYS_RAYS_COUNT];
    check(aver_phys_rayveh_wheel_state(v, 0, st, AVER_PHYS_RAYS_COUNT) == 1 && st[AVER_PHYS_RAYS_IN_CONTACT] == 0.0f, "state reads before any step: not in contact");
    check(aver_phys_rayveh_wheel_state(v, 0, nullptr, 3) == 0 && aver_phys_last_error() == kNullPointer, "state: a null out is refused");
    check(aver_phys_rayveh_wheel_pose(v, 0, nullptr, nullptr) == 1, "pose with two null outs is fine");

    check(aver_phys_rayveh_destroy(v) == 1 && aver_phys_rayveh_exists(v) == 0 && aver_phys_rayveh_count() == 0, "destroy");
    check(aver_phys_rayveh_destroy(v) == 0 && aver_phys_last_error() == kBadHandle, "destroy twice is a bad handle");
    check(aver_phys_body_count() >= 2 && aver_phys_body_position(box, st) == 1, "destroying the vehicle keeps the chassis body");
    const int32_t v2 = aver_phys_rayveh_create(box);
    check(v2 > 0 && aver_phys_rayveh_create(box) == 0, "the chassis can take a new vehicle after destroy");
    aver_phys_remove_body(box);
    check(aver_phys_rayveh_exists(v2) == 0 && aver_phys_rayveh_count() == 0, "removing the chassis body destroys the vehicle");
    aver_phys_shutdown();
}

// ---- 2. the suspension ------------------------------------------------------------------------------------------------------------------------

static void testSuspension() {
    AVER_INFO("--- the suspension");
    aver_phys_init();
    floorBox();
    Car air = makeCar(0, 0, 300.0f);
    stepN(2);
    check(contacts(air) == 0, "in the air: no wheel touches");
    check(near(rs(air.veh, 0, AVER_PHYS_RAYS_SUSP_LENGTH), kRestCm, 1e-3f), "in the air the suspension reads its rest length: " + f2s(rs(air.veh, 0, AVER_PHYS_RAYS_SUSP_LENGTH)));
    check(rs(air.veh, 0, AVER_PHYS_RAYS_SUSP_FORCE) == 0.0f && rs(air.veh, 0, AVER_PHYS_RAYS_SKID_INFO) == 1.0f && rs(air.veh, 0, AVER_PHYS_RAYS_CONTACT_BODY) == 0.0f, "in the air: no force, no skid, no ground");
    aver_phys_remove_body(air.body);

    Car car = settledCar();
    check(contacts(car) == 4, "settled: all four wheels touch");
    float worst = 0, sumForce = 0;
    for (int w = 0; w < 4; ++w) {
        worst = std::fmax(worst, std::fabs(rs(car.veh, w, AVER_PHYS_RAYS_SUSP_LENGTH) - (kRestCm - kSagCm)));
        sumForce += rs(car.veh, w, AVER_PHYS_RAYS_SUSP_FORCE);
    }
    check(worst < 1.5f, "settled: every wheel at rest - g/(N k) = " + f2s(kRestCm - kSagCm) + " cm (worst error " + f2s(worst) + ")");
    check(near(sumForce, 1000.0f * 980.0f, 0.03f * 1000.0f * 980.0f), "settled: the springs carry the weight, " + f2s(sumForce) + " of " + f2s(1000.0f * 980.0f));
    check(near(posOf(car.body, 2), kSettledZ, 2.0f), "settled: the chassis rides at " + f2s(posOf(car.body, 2)) + " (expected " + f2s(kSettledZ) + ")");
    check(std::fabs(velOf(car.body, 2)) < 3.0f && std::fabs(velOf(car.body, 0)) < 3.0f, "settled: at rest");
    check(rs(car.veh, 0, AVER_PHYS_RAYS_CONTACT_NORMAL + 2) > 0.99f && rs(car.veh, 0, AVER_PHYS_RAYS_CONTACT_BODY) > 0.0f, "contact normal is up and the ground body is reported");
    check(near(rs(car.veh, 2, AVER_PHYS_RAYS_CONTACT_POINT + 2), 0.0f, 0.5f), "the contact point is on the floor");

    // The pose of a wheel at rest: its centre one radius above the floor, orientation level.
    float pos[3] = {}, q[4] = {};
    check(aver_phys_rayveh_wheel_pose(car.veh, 0, pos, q) == 1, "wheel pose");
    check(near(pos[2], kRadiusCm, 1.5f) && near(pos[0], 110.0f, 3.0f) && near(pos[1], -70.0f, 3.0f), "front-left wheel centre at (110, -70, radius): " + f2s(pos[0]) + " " + f2s(pos[1]) + " " + f2s(pos[2]));
    check(std::fabs(q[3]) > 0.995f, "a level wheel's rotation is near the identity: w = " + f2s(q[3]));

    // The same spring on a four-times heavier car: the spring is mass-normalised, so the sag does not change.
    aver_phys_body_set_mass(car.body, 4000.0f);
    aver_phys_body_activate(car.body);   // a sleeping chassis is not stepped
    stepN(300);
    float sag4 = rs(car.veh, 0, AVER_PHYS_RAYS_SUSP_LENGTH), sum4 = 0;
    for (int w = 0; w < 4; ++w) sum4 += rs(car.veh, w, AVER_PHYS_RAYS_SUSP_FORCE);
    check(near(sag4, kRestCm - kSagCm, 1.5f), "4x the mass: the same suspension length " + f2s(sag4));
    check(near(sum4, 4000.0f * 980.0f, 0.04f * 4000.0f * 980.0f), "4x the mass: the force carries the heavier weight " + f2s(sum4));
    aver_phys_shutdown();

    // max_force clamps the push: a weak clamp lets the car sink to its travel limit and ride on the bottom.
    aver_phys_init();
    floorBox();
    Car weak = makeCar(0, 0, kSettledZ + 6.0f);
    for (int w = 0; w < 4; ++w) aver_phys_rayveh_set_wheel_param(weak.veh, w, AVER_PHYS_RAYW_MAX_FORCE, 100.0f * 100.0f);   // 100 N, far under the 2450 N a wheel carries
    stepN(240);
    float f = rs(weak.veh, 0, AVER_PHYS_RAYS_SUSP_FORCE);
    check(f <= 100.0f * 100.0f * 1.001f && f > 0.0f, "max_force: a wheel pushes with at most its limit: " + f2s(f));
    check(rs(weak.veh, 0, AVER_PHYS_RAYS_SUSP_LENGTH) < 1.0f || posOf(weak.body, 2) < kSettledZ - 20.0f, "max_force: the car sinks onto its travel limit");
    aver_phys_shutdown();
}

// ---- 3. drive ----------------------------------------------------------------------------------------------------------------------------------

static void testDrive() {
    AVER_INFO("--- engine and brake");
    aver_phys_init();
    floorBox();
    Car front = settledCar(0, -2000.0f);
    Car rear = settledCar(0, 0.0f);
    Car coast = settledCar(0, 2000.0f);

    // Engine force on the front wheels (no TRACTION) does nothing; on the rear wheels it drives the car along +X.
    aver_phys_rayveh_set_engine_force(front.veh, 0, 200000.0f); aver_phys_rayveh_set_engine_force(front.veh, 1, 200000.0f);
    aver_phys_rayveh_set_engine_force(rear.veh, 2, 200000.0f); aver_phys_rayveh_set_engine_force(rear.veh, 3, 200000.0f);   // 2000 N each
    aver_phys_body_set_velocity(coast.body, 800.0f, 0, 0);
    stepN(120);
    const float vRear = velOf(rear.body, 0), vFront = velOf(front.body, 0);
    check(vRear > 600.0f && vRear < 850.0f, "rear drive: 4000 N on 1000 kg for 2 s reaches ~800 cm/s: " + f2s(vRear));
    check(std::fabs(vFront) < 15.0f, "engine force on wheels without TRACTION does nothing: " + f2s(vFront));
    check(std::fabs(velOf(rear.body, 1)) < 25.0f, "driving straight stays straight: vy " + f2s(velOf(rear.body, 1)));
    check(rs(rear.veh, 2, AVER_PHYS_RAYS_SPIN) > 10.0f && rs(rear.veh, 2, AVER_PHYS_RAYS_RPM) > 100.0f, "the driven wheel spins forward: rpm " + f2s(rs(rear.veh, 2, AVER_PHYS_RAYS_RPM)));
    const float expectSpin = vRear / kRadiusCm;
    check(near(rs(rear.veh, 2, AVER_PHYS_RAYS_SPIN), expectSpin, 0.12f * expectSpin), "spin follows the ground: " + f2s(rs(rear.veh, 2, AVER_PHYS_RAYS_SPIN)) + " rad/s for " + f2s(expectSpin));
    check(rs(rear.veh, 2, AVER_PHYS_RAYS_ROTATION) != 0.0f, "the wheel rotation accumulates");
    // Jolt's default linear damping (0.05/s) alone takes 800 to ~724 in 2 s; the wheels add no rolling resistance.
    check(velOf(coast.body, 0) > 700.0f, "no engine, no brake: the car coasts on its wheels: " + f2s(velOf(coast.body, 0)));

    // The brake: all wheels, 3000 N each = up to 12 m/s^2.
    aver_phys_rayveh_set_engine_force(rear.veh, -1, 0.0f);
    aver_phys_rayveh_set_brake(rear.veh, -1, 300000.0f);
    float minV = 1e9f;
    for (int i = 0; i < 120; ++i) { aver_phys_step(kDt); minV = std::fmin(minV, velOf(rear.body, 0)); }
    check(std::fabs(velOf(rear.body, 0)) < 12.0f, "the brake stops the car: " + f2s(velOf(rear.body, 0)));
    check(minV > -30.0f, "the brake does not drive it backwards (only the nose-dive rebound): lowest speed " + f2s(minV));
    // A light brake stops it later than a hard one.
    aver_phys_body_set_velocity(rear.body, 800.0f, 0, 0);
    aver_phys_rayveh_set_brake(rear.veh, -1, 50000.0f);   // 500 N each = 2 m/s^2
    stepN(60);
    // 2 m/s^2 for 1 s takes 200 cm/s off, damping ~40 more.
    check(velOf(rear.body, 0) > 500.0f && velOf(rear.body, 0) < 640.0f, "a light brake slows it by about its force: " + f2s(velOf(rear.body, 0)));
    // The brake is frame-rate independent: it is a force.
    aver_phys_shutdown();

    // An engine force wakes a sleeping chassis.
    aver_phys_init();
    floorBox();
    Car sleeper = settledCar();
    stepN(300);
    const bool slept = aver_phys_body_is_active(sleeper.body) == 0;
    check(slept, "a car at rest with no input falls asleep");
    aver_phys_rayveh_set_engine_force(sleeper.veh, 2, 200000.0f); aver_phys_rayveh_set_engine_force(sleeper.veh, 3, 200000.0f);
    stepN(30);
    check(aver_phys_body_is_active(sleeper.body) == 1 && velOf(sleeper.body, 0) > 50.0f, "a non-zero engine force wakes it and it moves: " + f2s(velOf(sleeper.body, 0)));
    aver_phys_shutdown();
}

// ---- 4. steering -------------------------------------------------------------------------------------------------------------------------------

static void testSteering() {
    AVER_INFO("--- steering");
    aver_phys_init();
    floorBox();
    Car right = settledCar(0, -3000.0f);
    Car left = settledCar(0, 3000.0f);
    Car locked = settledCar(0, 0.0f);
    for (const Car* c : {&right, &left, &locked}) aver_phys_body_set_velocity(c->body, 500.0f, 0, 0);
    for (const Car* c : {&right, &left, &locked}) { aver_phys_rayveh_set_engine_force(c->veh, 2, 40000.0f); aver_phys_rayveh_set_engine_force(c->veh, 3, 40000.0f); }
    aver_phys_rayveh_set_steering(right.veh, -1, 0.3f);
    aver_phys_rayveh_set_steering(left.veh, -1, -0.3f);
    // The rear wheels have no STEERING flag, so steering them as well changes nothing. `locked` steers only the rear: it must go straight.
    aver_phys_rayveh_set_steering(locked.veh, 2, 0.3f); aver_phys_rayveh_set_steering(locked.veh, 3, 0.3f);
    const float y0r = posOf(right.body, 1), y0l = posOf(left.body, 1), y0k = posOf(locked.body, 1);
    stepN(120);
    float fr[3], fl[3], fk[3];
    axes(right.body, fr, nullptr); axes(left.body, fl, nullptr); axes(locked.body, fk, nullptr);
    const float dyr = posOf(right.body, 1) - y0r, dyl = posOf(left.body, 1) - y0l, dyk = posOf(locked.body, 1) - y0k;
    check(dyr > 150.0f, "positive steering turns toward +Y: moved " + f2s(dyr) + " cm sideways");
    check(dyl < -150.0f, "negative steering turns toward -Y: moved " + f2s(dyl) + " cm sideways");
    check(fr[1] > 0.3f && fl[1] < -0.3f, "the heading turned with it: forward.y " + f2s(fr[1]) + " and " + f2s(fl[1]));
    check(std::fabs(dyr + dyl) < 0.25f * std::fabs(dyr), "the two turns are mirror images: " + f2s(dyr) + " and " + f2s(dyl));
    check(std::fabs(dyk) < 20.0f && std::fabs(fk[1]) < 0.05f, "steering wheels that have no STEERING flag does nothing: moved " + f2s(dyk));
    check(near(rs(right.veh, 0, AVER_PHYS_RAYS_STEER_APPLIED), 0.3f, 1e-5f) && rs(right.veh, 2, AVER_PHYS_RAYS_STEER_APPLIED) == 0.0f, "STEER_APPLIED: the front 0.3, the rear 0");
    float up[3];
    axes(right.body, nullptr, up);
    check(up[2] > 0.85f, "the car stayed on its wheels: up.z " + f2s(up[2]));

    // The wheel pose carries the steering: a car standing still, its front wheel steered 0.3 rad, is turned about +Z by 0.3 (z = sin(0.15)).
    Car still = settledCar(0, 5000.0f);
    aver_phys_rayveh_set_steering(still.veh, -1, 0.3f);
    float q[4], qRear[4];
    aver_phys_rayveh_wheel_pose(still.veh, 0, nullptr, q);
    aver_phys_rayveh_wheel_pose(still.veh, 2, nullptr, qRear);
    check(near(q[2], 0.149f, 0.02f) && near(q[3], 0.989f, 0.01f), "the steered wheel's pose is turned about +Z by the steering angle: z " + f2s(q[2]) + ", w " + f2s(q[3]));
    check(std::fabs(qRear[2]) < 0.02f, "the rear wheel's pose is not turned: z " + f2s(qRear[2]));
    aver_phys_shutdown();
}

// ---- 5. friction_slip ---------------------------------------------------------------------------------------------------------------------------

static void testSkid() {
    AVER_INFO("--- friction_slip");
    aver_phys_init();
    floorBox();
    Car grip = settledCar(0, -2000.0f, 1000.0f, 1.0f);
    Car slick = settledCar(0, 2000.0f, 1000.0f, 0.3f);
    for (const Car* c : {&grip, &slick}) { aver_phys_rayveh_set_engine_force(c->veh, 2, 200000.0f); aver_phys_rayveh_set_engine_force(c->veh, 3, 200000.0f); }
    stepN(90);
    const float vg = velOf(grip.body, 0), vs = velOf(slick.body, 0);
    check(vg > 450.0f, "thrust below the grip limit accelerates fully: " + f2s(vg));
    check(vs > 100.0f && vs < 0.6f * vg, "thrust above the grip limit is capped by friction_slip: " + f2s(vs) + " against " + f2s(vg));
    check(rs(slick.veh, 2, AVER_PHYS_RAYS_SKID_INFO) < 0.9f, "the slipping wheel reports skid info below 1: " + f2s(rs(slick.veh, 2, AVER_PHYS_RAYS_SKID_INFO)));
    check(rs(grip.veh, 2, AVER_PHYS_RAYS_SKID_INFO) > 0.99f, "the gripping wheel reports 1: " + f2s(rs(grip.veh, 2, AVER_PHYS_RAYS_SKID_INFO)));
    aver_phys_shutdown();

    // A sideways slide: the grippy car stops sliding at once, the slick one slides on and slows at about slip x g.
    aver_phys_init();
    floorBox();
    Car g2 = settledCar(0, -2000.0f, 1000.0f, 1.0f);
    Car s2 = settledCar(0, 2000.0f, 1000.0f, 0.3f);
    aver_phys_body_set_velocity(g2.body, 0, 300.0f, 0);
    aver_phys_body_set_velocity(s2.body, 0, 300.0f, 0);
    stepN(30);
    const float vyG = velOf(g2.body, 1), vyS = velOf(s2.body, 1);
    check(std::fabs(vyG) < 40.0f, "high friction kills a sideways slide in half a second: " + f2s(vyG));
    check(vyS > 120.0f && vyS < 230.0f, "low friction lets it slide on, slowing at about 0.3 g (300 - 147 = 153): " + f2s(vyS));
    check(rs(s2.veh, 0, AVER_PHYS_RAYS_SKID_INFO) < 0.95f && std::fabs(rs(s2.veh, 0, AVER_PHYS_RAYS_SIDE_SPEED)) > 100.0f, "the sliding wheel reports the skid and its side speed: " + f2s(rs(s2.veh, 0, AVER_PHYS_RAYS_SIDE_SPEED)));
    // grip is read at the contact: a live change of friction_slip takes effect on the next step
    for (int w = 0; w < 4; ++w) aver_phys_rayveh_set_wheel_param(s2.veh, w, AVER_PHYS_RAYW_FRICTION_SLIP, 1.5f);
    stepN(30);
    check(std::fabs(velOf(s2.body, 1)) < 40.0f, "raising friction_slip live stops the slide: " + f2s(velOf(s2.body, 1)));
    aver_phys_shutdown();
}

// ---- 6. roll_influence --------------------------------------------------------------------------------------------------------------------------

static float maxTilt(float roll) {
    aver_phys_init();
    floorBox();
    Car c = settledCar(0, 0, 1000.0f, 1.0f, roll);
    aver_phys_body_set_velocity(c.body, 0, 350.0f, 0);   // a sideways slide that the tyres must stop
    float worst = 1.0f;
    for (int i = 0; i < 60; ++i) {
        aver_phys_step(kDt);
        float up[3];
        axes(c.body, nullptr, up);
        worst = std::fmin(worst, up[2]);
    }
    aver_phys_shutdown();
    return worst;   // smaller = more roll
}

static void testRoll() {
    AVER_INFO("--- roll_influence");
    const float up0 = maxTilt(0.0f), up1 = maxTilt(1.0f);
    const float tilt0 = std::acos(std::fmin(up0, 1.0f)), tilt1 = std::acos(std::fmin(up1, 1.0f));
    check(tilt1 > tilt0 + 0.02f, "side friction at the contact rolls the car more than side friction at its centre: " + f2s(tilt1) + " rad against " + f2s(tilt0));
    check(tilt0 < 0.15f, "roll_influence 0 keeps the body nearly level: " + f2s(tilt0) + " rad");
}

// ---- 7. a dynamic ground, the mask --------------------------------------------------------------------------------------------------------------

static void testGroundAndMask() {
    AVER_INFO("--- dynamic ground and the wheel mask");
    aver_phys_init();
    floorBox();
    // A slab that slides freely under the car.
    const int32_t slab = aver_phys_add_dynamic_box(0, 0, 10.0f, 600.0f, 300.0f, 10.0f, 3000.0f);
    aver_phys_body_set_friction(slab, 0.0f);
    aver_phys_body_set_damping(slab, 0.0f, 0.0f);
    Car car = makeCar(0, 0, kSettledZ + 20.0f + 6.0f);
    stepN(240);
    check(contacts(car) == 4, "the car rides on the slab");
    check(rs(car.veh, 0, AVER_PHYS_RAYS_CONTACT_BODY) == static_cast<float>(slab), "the wheel reports the slab as its ground");
    aver_phys_rayveh_set_engine_force(car.veh, 2, 100000.0f); aver_phys_rayveh_set_engine_force(car.veh, 3, 100000.0f);
    stepN(60);
    const float vc = velOf(car.body, 0), vs = velOf(slab, 0);
    check(vc > 60.0f && vs < -10.0f, "driving pushes the slab back: car " + f2s(vc) + ", slab " + f2s(vs));
    const float p = 1000.0f * vc + 3000.0f * vs;
    check(std::fabs(p) < 0.3f * 1000.0f * vc, "momentum is shared by the masses: " + f2s(p) + " of " + f2s(1000.0f * vc));
    aver_phys_shutdown();

    // The wheel mask: the floor is user layer 0, whose Godot layer bit is 1.
    aver_phys_init();
    floorBox();
    Car c = makeCar(0, 0, kSettledZ + 6.0f);
    for (int w = 0; w < 4; ++w) aver_phys_rayveh_set_wheel_mask(c.veh, w, 0x2u);
    stepN(5);
    check(contacts(c) == 0, "a mask that misses the floor's layer: no wheel touches");
    for (int w = 0; w < 4; ++w) aver_phys_rayveh_set_wheel_mask(c.veh, w, 0x1u);
    aver_phys_body_set_position(c.body, 0, 0, kSettledZ + 6.0f);
    aver_phys_body_set_velocity(c.body, 0, 0, 0);
    stepN(240);
    check(contacts(c) == 4 && near(posOf(c.body, 2), kSettledZ, 3.0f), "a mask that meets it: the car rides again at " + f2s(posOf(c.body, 2)));
    aver_phys_shutdown();
}

// ---- 8. determinism and snapshots ---------------------------------------------------------------------------------------------------------------

static void driven(const Car& c, int step) {
    aver_phys_rayveh_set_engine_force(c.veh, 2, step % 90 < 60 ? 150000.0f : 0.0f);
    aver_phys_rayveh_set_engine_force(c.veh, 3, step % 90 < 60 ? 150000.0f : 0.0f);
    aver_phys_rayveh_set_steering(c.veh, -1, 0.2f * std::sin(0.05f * static_cast<float>(step)));
}

static std::vector<float> run(int steps) {
    aver_phys_init();
    floorBox();
    Car c = makeCar(0, 0, kSettledZ + 20.0f);
    for (int i = 0; i < steps; ++i) { driven(c, i); aver_phys_step(kDt); }
    std::vector<float> out(7);
    aver_phys_body_position(c.body, out.data());
    aver_phys_body_rotation(c.body, out.data() + 3);
    aver_phys_shutdown();
    return out;
}

static void testDeterminism() {
    AVER_INFO("--- determinism");
    const std::vector<float> a = run(300), b = run(300);
    check(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "two worlds, the same inputs: the same bits (x " + f2s(a[0]) + ", y " + f2s(a[1]) + ")");
    check(a[0] > 100.0f, "the driven run went somewhere: x " + f2s(a[0]));

    aver_phys_init();
    floorBox();
    Car c = makeCar(0, 0, kSettledZ + 20.0f);
    for (int i = 0; i < 120; ++i) { driven(c, i); aver_phys_step(kDt); }
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    float p1[7], p2[7];
    for (int i = 120; i < 240; ++i) { driven(c, i); aver_phys_step(kDt); }
    aver_phys_body_position(c.body, p1); aver_phys_body_rotation(c.body, p1 + 3);
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restored");
    for (int i = 120; i < 240; ++i) { driven(c, i); aver_phys_step(kDt); }
    aver_phys_body_position(c.body, p2); aver_phys_body_rotation(c.body, p2 + 3);
    check(std::memcmp(p1, p2, sizeof p1) == 0, "a restore replays the driven run bit for bit: x " + f2s(p1[0]) + " and " + f2s(p2[0]));
    check(aver_phys_rayveh_exists(c.veh) == 1 && contacts(c) > 0, "the vehicle survives the restore and reads contacts again");
    aver_phys_snapshot_release_all();
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("Raycast vehicle (G2)");

    testRefusals();
    testSuspension();
    testDrive();
    testSteering();
    testSkid();
    testRoll();
    testGroundAndMask();
    testDeterminism();

    AVER_INFO("==================================================");
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
