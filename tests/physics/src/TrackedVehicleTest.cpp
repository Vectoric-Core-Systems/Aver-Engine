// TrackedVehicleTest -- tracked vehicles through the v2 vehicle ABI (set_kind, set_track).
//
// OBSERVED MOTION, as in VehicleTest: a tank built with the wrong track side or the wrong steering sign returns
// a good handle and then turns away from where it was told. So each check drops a tank on a floor and asks where
// it went: it holds a straight line on throttle, a hard turn while moving curves the way the steer asked, with
// no throttle a pivot turns it on the spot, the track speeds in the telemetry are the rim speed of the sprocket
// wheel, and the builder refuses a tank that is not one. The tank is small and light (12 t): Jolt advises against a
// real tank's mass beside a one-metre-thick floor. Its engine is 6000 N*m because a pivot has to beat the lateral
// grip of ten track wheels: measured, 800 N*m left the tank standing still and 4000 turned it 22 degrees a second.
// All numbers are UNVERIFIED starting values.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;

static const f32 kRadius = 40.0f, kSuspMin = 5.0f, kSuspMax = 35.0f, kHz = 2.0f;
static const f32 kSagCm = 981.0f / ((2.0f * kPi * kHz) * (2.0f * kPi * kHz));
static const f32 kAttachZ = kRadius + kSuspMax - kSagCm;

// Wheels 0..4 are the left track (-Y) from the rear forward, 5..9 the right. `skip` leaves a track out.
static int32_t buildTank(f32 x, f32 y, f32 z, bool left = true, bool right = true, int32_t drivenLeft = 4, int32_t drivenRight = 9,
                         const std::function<void(int32_t)>& pre = {}) {
    const int32_t v = aver_phys_vehicle_create(300.0f, 150.0f, 50.0f, 30.0f, 0.0f, 0.0f, 60.0f, 12000.0f, x, y, z, 0, 0, 0, 1);
    aver_phys_vehicle_set_kind(v, AVER_PHYS_VEH_KIND_TRACKED);
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < 5; ++i)
            aver_phys_vehicle_add_wheel(v, -200.0f + 100.0f * i, side == 0 ? -120.0f : 120.0f, kAttachZ, kRadius, 40.0f,
                                        kSuspMin, kSuspMax, kHz, 0.6f, 0.0f, 0.0f, 0.0f, 0);
    const int32_t l[5] = {0, 1, 2, 3, 4}, r[5] = {5, 6, 7, 8, 9};
    if (left) aver_phys_vehicle_set_track(v, 0, l, 5, drivenLeft, nullptr, 0);
    if (right) aver_phys_vehicle_set_track(v, 1, r, 5, drivenRight, nullptr, 0);
    const float eng[5] = {6000.0f, 800.0f, 4000.0f, 0.0f, 0.2f};
    aver_phys_vehicle_set_engine_ex(v, eng, 5);
    if (pre) pre(v);
    aver_phys_vehicle_finish(v, 0.0f);
    return v;
}

static void beginWorld() {
    aver_phys_init();
    aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
}

static void run(int n, const std::function<void(int)>& each = {}) {
    for (int i = 0; i < n; ++i) {
        if (each) each(i);
        aver_phys_step(kDt);
    }
}

struct Pose { float p[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };
static Pose poseOf(int32_t v) { Pose r; aver_phys_vehicle_pose(v, r.p, r.q); return r; }
static Vec3 rotated(const float q[4], const Vec3& v) { return Quat(q[0], q[1], q[2], q[3]).rotate(v); }
static f32 yawDelta(const Pose& a, const Pose& b) {
    const Vec3 fa = rotated(a.q, Vec3(1, 0, 0)), fb = rotated(b.q, Vec3(1, 0, 0));
    return std::atan2(fa.x * fb.y - fa.y * fb.x, fa.x * fb.x + fa.y * fb.y);
}

static void testStraightLine() {
    AVER_INFO("=== a tank on throttle holds a straight line ===");
    beginWorld();
    const int32_t v = buildTank(0, 0, 5);
    check(v != 0, "the tank builds");
    run(120);
    int touching = 0;
    for (int w = 0; w < 10; ++w) touching += aver_phys_vehicle_wheel_contact(v, w);
    check(touching >= 8, "it settles on its tracks: " + std::to_string(touching) + " of 10 wheels touch");
    const Pose a = poseOf(v);
    run(300, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const Pose b = poseOf(v);
    check(b.p[0] - a.p[0] > 200.0f, "it drives forward: " + f2s(b.p[0] - a.p[0]) + " cm in 5 s");
    check(std::fabs(degrees(yawDelta(a, b))) < 5.0f, "and holds its heading to within 5 degrees: " + f2s(degrees(yawDelta(a, b))));
    float st[AVER_PHYS_VS_COUNT];
    aver_phys_vehicle_state(v, st, AVER_PHYS_VS_COUNT, nullptr, 0);
    check(st[AVER_PHYS_VS_FORWARD_SPEED_CMS] > 50.0f, "the telemetry sees it moving: " + f2s(st[AVER_PHYS_VS_FORWARD_SPEED_CMS]));
    aver_phys_shutdown();
}

// Steps `n` times and returns the heading change summed step by step: a hard turn passes 180 degrees in a
// second or two, and the difference of the first and last pose wraps there and reads as a turn the other way.
static f32 turned(int32_t v, int n, const std::function<void(int)>& each) {
    Pose last = poseOf(v);
    f32 sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        each(i);
        aver_phys_step(kDt);
        const Pose now = poseOf(v);
        sum += yawDelta(last, now);
        last = now;
    }
    return sum;
}

static void testSteering() {
    AVER_INFO("=== steering while moving curves toward the asked side; a pivot turns it on the spot ===");
    beginWorld();
    const int32_t v = buildTank(0, 0, 5);
    run(120);
    run(180, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const f32 right = turned(v, 60, [&](int) { aver_phys_vehicle_set_input(v, 0.8f, 1.0f, 0.0f, 0.0f); });
    check(right > radians(20.0f), "steer +1 while moving turns toward +Y (right): " + f2s(degrees(right)) + " degrees");
    run(180, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const f32 left = turned(v, 60, [&](int) { aver_phys_vehicle_set_input(v, 0.8f, -1.0f, 0.0f, 0.0f); });
    check(left < -radians(20.0f), "steer -1 turns toward -Y (left): " + f2s(degrees(left)) + " degrees");
    aver_phys_shutdown();

    beginWorld();
    const int32_t p = buildTank(0, 0, 5);
    run(120);
    float maxSpeed = 0.0f;
    const f32 yaw = turned(p, 90, [&](int) {
        aver_phys_vehicle_set_input(p, 0.0f, 1.0f, 0.0f, 0.0f);
        maxSpeed = std::fmax(maxSpeed, std::fabs(aver_phys_vehicle_forward_speed(p)));
    });
    const f32 rate = degrees(yaw) / 1.5f;
    check(rate > 30.0f, "a pivot turns it in place: " + f2s(rate) + " degrees a second");
    check(maxSpeed < 60.0f, "and it stays where it is: forward speed peaked at " + f2s(maxSpeed) + " cm/s");
    aver_phys_shutdown();
}

static void testTelemetry() {
    AVER_INFO("=== track speeds are the sprocket's rim speed ===");
    beginWorld();
    const int32_t v = buildTank(0, 0, 5);
    run(120);
    run(150, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.3f, 0.0f, 0.0f); });
    float st[AVER_PHYS_VS_COUNT];
    aver_phys_vehicle_state(v, st, AVER_PHYS_VS_COUNT, nullptr, 0);
    float wl[AVER_PHYS_VWS_COUNT], wr[AVER_PHYS_VWS_COUNT];
    aver_phys_vehicle_wheel_state(v, 4, wl, AVER_PHYS_VWS_COUNT, nullptr, 0);
    aver_phys_vehicle_wheel_state(v, 9, wr, AVER_PHYS_VWS_COUNT, nullptr, 0);
    const float left = wl[AVER_PHYS_VWS_ANG_VEL_RAD_S] * kRadius, right = wr[AVER_PHYS_VWS_ANG_VEL_RAD_S] * kRadius;
    check(std::fabs(st[AVER_PHYS_VS_TRACK_LEFT_CMS] - left) < std::fabs(left) * 0.02f + 1.0f, "the left track speed " + f2s(st[AVER_PHYS_VS_TRACK_LEFT_CMS]) + " is the sprocket rim speed " + f2s(left));
    check(std::fabs(st[AVER_PHYS_VS_TRACK_RIGHT_CMS] - right) < std::fabs(right) * 0.02f + 1.0f, "the right track speed " + f2s(st[AVER_PHYS_VS_TRACK_RIGHT_CMS]) + " is the sprocket rim speed " + f2s(right));
    check(st[AVER_PHYS_VS_TRACK_RIGHT_CMS] < st[AVER_PHYS_VS_TRACK_LEFT_CMS], "turning right, the right track is the slower one");
    aver_phys_shutdown();
}

static void testRefusals() {
    AVER_INFO("=== a tank that is not one is refused ===");
    beginWorld();
    check(buildTank(0, 0, 5, true, false) != 0, "the handle exists");
    aver_phys_shutdown();

    beginWorld();
    const int32_t a = buildTank(0, 0, 5, true, false);
    check(aver_phys_vehicle_finish(a, 0.0f) == 0, "finish is refused with only one track");
    const int32_t b = buildTank(0, 0, 5, true, true, 7, 9);
    (void)b;
    aver_phys_shutdown();

    beginWorld();
    const int32_t c = aver_phys_vehicle_create(300, 150, 50, 30, 0, 0, 60, 12000, 0, 0, 5, 0, 0, 0, 1);
    aver_phys_vehicle_set_kind(c, AVER_PHYS_VEH_KIND_TRACKED);
    for (int i = 0; i < 4; ++i)
        aver_phys_vehicle_add_wheel(c, -100.0f + 66.0f * i, (i % 2) ? 120.0f : -120.0f, kAttachZ, kRadius, 40, kSuspMin, kSuspMax, kHz, 0.6f, 0, 0, 0, 0);
    const int32_t one[2] = {0, 2}, two[2] = {1, 3};
    check(aver_phys_vehicle_set_track(c, 0, one, 2, 1, nullptr, 0) == 0, "a driven wheel that is not in its track's list is refused");
    check(aver_phys_vehicle_set_track(c, 0, one, 2, 0, nullptr, 0) == 1 && aver_phys_vehicle_set_track(c, 1, two, 2, 1, nullptr, 0) == 1, "valid tracks are accepted");
    const float axle[6] = {3.42f, 1.0f, 1.4f, 0.5f, 0.0f, 1000.0f};
    check(aver_phys_vehicle_set_axle(c, 0, axle, 6) == 0, "the axle calls are refused for a tracked vehicle");
    check(aver_phys_vehicle_finish(c, 0.0f) == 1, "and the tank finishes");
    check(aver_phys_vehicle_set_kind(c, AVER_PHYS_VEH_KIND_WHEELED) == 0, "the kind cannot change afterwards");
    aver_phys_shutdown();

    beginWorld();
    const int32_t w = aver_phys_vehicle_create(300, 150, 50, 30, 0, 0, 60, 12000, 0, 0, 5, 0, 0, 0, 1);
    aver_phys_vehicle_add_wheel(w, 0, -120, kAttachZ, kRadius, 40, kSuspMin, kSuspMax, kHz, 0.6f, 0, 0, 0, 1);
    check(aver_phys_vehicle_set_kind(w, AVER_PHYS_VEH_KIND_TRACKED) == 0, "the kind is refused once a wheel exists");
    aver_phys_shutdown();
}

int main() {
    AVER_INFO("=== TrackedVehicleTest ===");
    testStraightLine();
    testSteering();
    testTelemetry();
    testRefusals();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
