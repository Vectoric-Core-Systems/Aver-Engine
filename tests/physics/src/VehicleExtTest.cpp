// VehicleExtTest -- the v2 vehicle ABI in physics_vehicle_ext_abi.h.
//
// THE STANDARD IS OBSERVED MOTION again (see VehicleTest): a gearbox that was stored and never read, a
// differential mode that never reaches Jolt, an ABS that releases nothing all return 1. So each check drives a
// car on a floor and asks what happened, against a relation worked out from the description rather than read off
// the implementation. The margins are UNVERIFIED starting values -- this file was written without being run --
// and a failing relation is a prompt to look at the physics, not to loosen the number first.
//
//   1. v1 identity: a car that sets every v2 block to its default drives 300 steps to the same pose as one built
//      by v1 alone,
//   2. the torque curve and the gearbox change how far and how fast it goes, and the manual box holds a gear,
//   3. the differential modes order themselves on an ice-split lane, and an axle's torque share steers the power,
//   4. anti-roll stiffness, the tyre curves and the grip scale change a steady turn,
//   5. the driver aids: ABS, traction control, steering rate and speed sensitivity, brake-to-reverse, hold brake,
//   6. aero, telemetry, events, compound chassis, lifetime, and bit-identical replay.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;

// ---- The car: VehicleTest's saloon -----------------------------------------------------------------------

static const f32 kHalfLength = 220.0f, kHalfWidth = 90.0f, kHalfHeight = 35.0f, kClearance = 25.0f;
static const f32 kComZ = 40.0f, kMass = 1300.0f;
static const f32 kRadius = 33.0f, kTyreWidth = 22.0f, kSuspMin = 5.0f, kSuspMax = 30.0f;
static const f32 kAxleX = 180.0f, kTrackY = 77.0f;
static const f32 kSpringHz = 2.0f;
static const f32 kSagCm = 981.0f / ((2.0f * kPi * kSpringHz) * (2.0f * kPi * kSpringHz));
static const f32 kAttachZ = kRadius + kSuspMax - kSagCm;
static const f32 kMaxSteerDeg = 35.0f;

using Hook = std::function<void(int32_t)>;

// 0 front-left, 1 front-right, 2 rear-left, 3 rear-right; "left" is the -Y side. `driven` is a bitmask of wheels.
static int32_t buildCar(f32 x, f32 y, f32 z, unsigned drivenMask = 0xC, const Hook& pre = {}, const Hook& post = {}) {
    const int32_t v = aver_phys_vehicle_create(kHalfLength, kHalfWidth, kHalfHeight, kClearance,
                                               0.0f, 0.0f, kComZ, kMass, x, y, z, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        aver_phys_vehicle_add_wheel(v, front ? kAxleX : -kAxleX, (i % 2 == 0) ? -kTrackY : kTrackY, kAttachZ,
                                    kRadius, kTyreWidth, kSuspMin, kSuspMax, kSpringHz, 0.5f,
                                    front ? kMaxSteerDeg : 0.0f, 1500.0f, 2500.0f, (drivenMask >> i) & 1);
    }
    if (pre) pre(v);
    aver_phys_vehicle_finish(v, 60.0f);
    if (post) post(v);
    return v;
}

static int32_t beginWorld() {
    aver_phys_init();
    return aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
}

struct Ev { int32_t k[4]; float f[8]; };

static void collect(std::vector<Ev>& out) {
    const int n = aver_phys_vehicle_event_count();
    for (int i = 0; i < n; ++i) {
        Ev e{};
        aver_phys_vehicle_event_get(i, e.k, e.f);
        out.push_back(e);
    }
}

// Steps `n` times, calling `each(step)` BEFORE each step and gathering the events after it.
static void run(int n, std::vector<Ev>* ev = nullptr, const std::function<void(int)>& each = {}) {
    for (int i = 0; i < n; ++i) {
        if (each) each(i);
        aver_phys_step(kDt);
        if (ev) collect(*ev);
    }
}

struct Pose { float p[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };
static Pose poseOf(int32_t v) { Pose r; aver_phys_vehicle_pose(v, r.p, r.q); return r; }
static Vec3 rotated(const float q[4], const Vec3& v) { return Quat(q[0], q[1], q[2], q[3]).rotate(v); }

struct State { float f[AVER_PHYS_VS_COUNT]; int32_t i[AVER_PHYS_VSI_COUNT]; };
static State stateOf(int32_t v) {
    State s{};
    aver_phys_vehicle_state(v, s.f, AVER_PHYS_VS_COUNT, s.i, AVER_PHYS_VSI_COUNT);
    return s;
}
struct WState { float f[AVER_PHYS_VWS_COUNT]; int32_t i[AVER_PHYS_VWSI_COUNT]; };
static WState wheelOf(int32_t v, int w) {
    WState s{};
    aver_phys_vehicle_wheel_state(v, w, s.f, AVER_PHYS_VWS_COUNT, s.i, AVER_PHYS_VWSI_COUNT);
    return s;
}

static void assistsOn(int32_t v, bool abs, bool tcs) {
    float d[AVER_PHYS_VD_COUNT];
    for (float& f : d) f = -1.0f;
    // A full block: the defaults of the header with the aids switched on.
    const float def[AVER_PHYS_VD_COUNT] = {1, abs ? 1.0f : 0.0f, 0.25f, 8, tcs ? 1.0f : 0.0f, 0.35f, 3, 5, 0, 6, 8, 8, 1, 50, 0.3f, 30, 0.45f, 12};
    std::memcpy(d, def, sizeof d);
    aver_phys_vehicle_set_assists(v, d, AVER_PHYS_VD_COUNT);
}

static void shutdown() { aver_phys_shutdown(); }

// ---------------------------------------------------------------------------------------------------

static Pose scriptedDrive(bool v2Defaults) {
    beginWorld();
    const Hook pre = [](int32_t v) {
        const float eng[5] = {500.0f, 1000.0f, 6000.0f, 0.0f, 0.2f};
        aver_phys_vehicle_set_engine_ex(v, eng, 5);
        aver_phys_vehicle_set_gearbox(v, 0, nullptr, 0, nullptr, 0, nullptr, 0);
        const float axle[6] = {3.42f, 1.0f, 1.4f, 0.5f, 0.0f, 1000.0f};
        aver_phys_vehicle_set_axle(v, 0, axle, 6);
        aver_phys_vehicle_set_axle(v, 1, axle, 6);
        aver_phys_vehicle_set_center_diff(v, 1, 1.4f);
        const float wh[5] = {0.9f, 0.2f, 4.0f, 2.0f, 1.0f};
        for (int w = 0; w < 4; ++w) aver_phys_vehicle_set_wheel_ex(v, w, wh, 5);
        aver_phys_vehicle_set_aero(v, 0.0f, 0.0f);
        aver_phys_vehicle_set_collision_period(v, 1);
        aver_phys_vehicle_tune(v, AVER_PHYS_VU_GRIP_SCALE, 1.0f);
        const float vd[2] = {0.0f, 0.0f};
        aver_phys_vehicle_set_assists(v, vd, 2);
    };
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f, 0xC, v2Defaults ? pre : Hook{});
    run(300, nullptr, [&](int k) { aver_phys_vehicle_set_input(v, 1.0f, k > 150 ? 0.3f : 0.0f, 0.0f, 0.0f); });
    const Pose p = poseOf(v);
    shutdown();
    return p;
}

static void testV1Identity() {
    AVER_INFO("=== a car that sets every v2 block to its default is field-for-field the v1 car ===");
    const Pose a = scriptedDrive(false);
    const Pose b = scriptedDrive(true);
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i) worst = std::max(worst, std::fabs(a.p[i] - b.p[i]));
    for (int i = 0; i < 4; ++i) worst = std::max(worst, std::fabs(a.q[i] - b.q[i]));
    check(a.p[0] > 500.0f, "the v1 car drove: x = " + f2s(a.p[0]));
    check(worst <= 1e-4f, "the two poses agree to " + f2s(worst) + " (v1 and v2 defaults)");
    check(aver_phys_vehicle_abi_version() == 2, "the ABI reports version 2");
}

// Distance driven along +X in `steps` of full throttle from a standing start.
static float throttleDistance(const Hook& pre, int steps) {
    beginWorld();
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f, 0xC, pre);
    run(90);
    const float x0 = poseOf(v).p[0];
    run(steps, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const float d = poseOf(v).p[0] - x0;
    shutdown();
    return d;
}

static float speedAtFirstUpshift(const Hook& pre) {
    beginWorld();
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f, 0xC, pre);
    run(90);
    float speed = -1.0f;
    for (int k = 0; k < 600 && speed < 0.0f; ++k) {
        aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
        aver_phys_step(kDt);
        for (int i = 0, n = aver_phys_vehicle_event_count(); i < n; ++i) {
            Ev e{};
            aver_phys_vehicle_event_get(i, e.k, e.f);
            if (e.k[0] == AVER_PHYS_VEV_SHIFT && e.k[2] == 1 && e.k[3] == 2) speed = aver_phys_vehicle_forward_speed(v);
        }
    }
    shutdown();
    return speed;
}

static int stepsToFirstUpshift(const Hook& pre) {
    beginWorld();
    const int32_t v = buildCar(0.0f, 0.0f, 5.0f, 0xC, pre);
    run(90);
    int at = -1;
    for (int k = 0; k < 900 && at < 0; ++k) {
        aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
        aver_phys_step(kDt);
        for (int i = 0, n = aver_phys_vehicle_event_count(); i < n; ++i) {
            Ev e{};
            aver_phys_vehicle_event_get(i, e.k, e.f);
            if (e.k[0] == AVER_PHYS_VEV_SHIFT && e.k[2] == 1 && e.k[3] == 2) at = k;
        }
    }
    shutdown();
    return at;
}

static void testTorqueCurve() {
    AVER_INFO("=== the torque curve changes how hard it pulls ===");
    const float flatX[2] = {0.0f, 1.0f}, lowY[2] = {0.3f, 0.3f}, highY[2] = {1.0f, 1.0f};
    const float base = throttleDistance({}, 180);
    const float weak = throttleDistance([&](int32_t v) { aver_phys_vehicle_set_torque_curve(v, flatX, lowY, 2); }, 180);
    check(weak < base * 0.85f, "a flat 30 percent curve covers less in 3 s: " + f2s(weak) + " against " + f2s(base));
    const int tBase = stepsToFirstUpshift({});
    const int tWeak = stepsToFirstUpshift([&](int32_t v) { aver_phys_vehicle_set_torque_curve(v, flatX, lowY, 2); });
    const int tStrong = stepsToFirstUpshift([&](int32_t v) { aver_phys_vehicle_set_torque_curve(v, flatX, highY, 2); });
    check(tBase > 0 && (tWeak < 0 || tWeak > tBase + 30), "a flat 30 percent curve reaches the first shift much later: " + std::to_string(tWeak) + " against " + std::to_string(tBase) + " steps");
    // Grip-limited to the first shift, so full torque only ties the default curve; a step either way is rounding (macOS: 256 vs 255).
    check(tStrong > 0 && tStrong <= tBase + 2, "a flat full-torque curve reaches the first shift no later: " + std::to_string(tStrong) + " against " + std::to_string(tBase) + " steps");
    const float xBad[3] = {0.0f, 0.5f, 0.4f};
    const float yBad[3] = {1, 1, 1};
    beginWorld();
    const int32_t v = aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1);
    check(aver_phys_vehicle_set_torque_curve(v, xBad, yBad, 3) == 0, "a descending curve is refused");
    check(aver_phys_vehicle_set_torque_curve(v, flatX, highY, 1) == 0, "one point is refused");
    shutdown();
}

static void testGearbox() {
    AVER_INFO("=== custom ratios, reverse magnitudes and the manual box ===");
    const float tall[2] = {5.0f, 2.0f};
    const float deflt = speedAtFirstUpshift({});
    const float geared = speedAtFirstUpshift([&](int32_t v) { aver_phys_vehicle_set_gearbox(v, 0, tall, 2, nullptr, 0, nullptr, 0); });
    check(deflt > 0 && geared > 0 && geared < deflt * 0.8f,
          "a 5.0 first gear upshifts at a lower speed: " + f2s(geared) + " against " + f2s(deflt) + " cm/s");

    // Reverse ratios are POSITIVE magnitudes: a car with one of 2.0 reverses.
    beginWorld();
    const float rev[1] = {2.0f};
    const int32_t v = buildCar(0, 0, 5, 0xC, [&](int32_t id) { aver_phys_vehicle_set_gearbox(id, 0, nullptr, 0, rev, 1, nullptr, 0); });
    run(90);
    run(240, nullptr, [&](int) { aver_phys_vehicle_set_input(v, -1.0f, 0.0f, 0.0f, 0.0f); });
    check(aver_phys_vehicle_forward_speed(v) < -100.0f, "it reverses with a positive reverse magnitude: " + f2s(aver_phys_vehicle_forward_speed(v)));
    shutdown();

    // Manual: holds first gear through the rev limit, shifts when told, refuses gears that do not exist.
    beginWorld();
    const float gears[3] = {2.66f, 1.78f, 1.3f};
    const int32_t m = buildCar(0, 0, 5, 0xC, [&](int32_t id) { aver_phys_vehicle_set_gearbox(id, 1, gears, 3, nullptr, 0, nullptr, 0); });
    run(90);
    std::vector<Ev> ev;
    run(240, &ev, [&](int) { aver_phys_vehicle_set_input(m, 1.0f, 0.0f, 0.0f, 0.0f); });
    State s = stateOf(m);
    int shifts = 0;
    for (const Ev& e : ev) shifts += e.k[0] == AVER_PHYS_VEV_SHIFT;
    check(s.i[AVER_PHYS_VSI_MANUAL] == 1, "the box reports manual");
    check(s.i[AVER_PHYS_VSI_GEAR] == 1 && shifts == 0, "it holds first gear through 4 s of full throttle");
    check(s.f[AVER_PHYS_VS_RPM] > 5000.0f, "against the rev limiter: " + f2s(s.f[AVER_PHYS_VS_RPM]) + " rpm");
    check(aver_phys_vehicle_shift(m, 1) == 1, "shift up is accepted");
    run(120, &ev, [&](int) { aver_phys_vehicle_set_input(m, 1.0f, 0.0f, 0.0f, 0.0f); });
    check(stateOf(m).i[AVER_PHYS_VSI_GEAR] == 2, "it is in second after the shift");
    check(aver_phys_vehicle_set_gear(m, 4) == 0, "a gear that does not exist is refused");
    check(aver_phys_vehicle_set_gear(m, 3) == 1 && aver_phys_vehicle_set_gear(m, -1) == 1 && aver_phys_vehicle_set_gear(m, -2) == 0,
          "third and Jolt's one reverse gear are accepted, a second reverse is not");
    shutdown();

    // An automatic box refuses the manual calls.
    beginWorld();
    const int32_t a = buildCar(0, 0, 5);
    check(aver_phys_vehicle_shift(a, 1) == 0 && aver_phys_vehicle_set_gear(a, 2) == 0, "shift and set_gear are refused on an automatic box");
    check(aver_phys_vehicle_set_gearbox_mode(a, 1) == 1 && aver_phys_vehicle_shift(a, 1) == 1, "switching to manual enables them");
    shutdown();
}

// A floor of two lanes split down y = 0, left (-Y) lane at `leftFriction`. Returns nothing; bodies are not needed after.
static void iceSplitWorld(float leftFriction) {
    aver_phys_init();
    const int32_t l = aver_phys_add_static_box(0, -10000, -50, 20000, 10000, 50);
    const int32_t r = aver_phys_add_static_box(0, 10000, -50, 20000, 10000, 50);
    aver_phys_body_set_friction(l, leftFriction);
    aver_phys_body_set_friction(r, 0.6f);
}

static float iceSplitDistance(int mode, float ratio) {
    iceSplitWorld(0.03f);
    const int32_t v = buildCar(0, 0, 5, 0xC, [&](int32_t id) {
        const float axle[6] = {3.42f, (float)mode, ratio, 0.5f, 0.0f, 1000.0f};
        aver_phys_vehicle_set_axle(id, 1, axle, 6);
    });
    run(90);
    const float x0 = poseOf(v).p[0];
    run(240, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const float d = poseOf(v).p[0] - x0;
    shutdown();
    return d;
}

static float shareDistance(float frontShare, float rearShare) {
    aver_phys_init();
    const int32_t grip = aver_phys_add_static_box(5000, 0, -50, 5000, 20000, 50);    // x > 0
    const int32_t ice = aver_phys_add_static_box(-5000, 0, -50, 5000, 20000, 50);    // x < 0
    aver_phys_body_set_friction(grip, 0.6f);
    aver_phys_body_set_friction(ice, 0.0f);
    const int32_t v = buildCar(0, 0, 5, 0xF, [&](int32_t id) {
        const float f[6] = {3.42f, 1.0f, 1.4f, 0.5f, frontShare, 1000.0f};
        const float r[6] = {3.42f, 1.0f, 1.4f, 0.5f, rearShare, 1000.0f};
        aver_phys_vehicle_set_axle(id, 0, f, 6);
        aver_phys_vehicle_set_axle(id, 1, r, 6);
        aver_phys_vehicle_set_center_diff(id, 0, 1.4f);
    });
    run(60);
    const float x0 = poseOf(v).p[0];
    run(90, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const float d = poseOf(v).p[0] - x0;
    shutdown();
    return d;
}

static void testDifferentials() {
    AVER_INFO("=== differentials: open < limited slip < locked on an ice-split lane; the torque share steers the power ===");
    const float open = iceSplitDistance(0, 1.4f);
    const float lsd = iceSplitDistance(1, 1.2f);
    const float locked = iceSplitDistance(2, 1.4f);
    check(open < locked * 0.9f, "open " + f2s(open) + " covers less than locked " + f2s(locked));
    check(open <= lsd + 20.0f && lsd <= locked + 20.0f, "limited slip " + f2s(lsd) + " sits between them");

    const float front = shareDistance(1.0f, 0.0f);
    const float rear = shareDistance(0.0f, 1.0f);
    check(front > rear * 1.5f + 20.0f, "all torque to the axle on grip moves it: " + f2s(front) + " against " + f2s(rear) + " with it on the frictionless half");

    beginWorld();
    const int32_t v = buildCar(0, 0, 5);
    check(aver_phys_vehicle_axle_count(v) == 2, "a four-wheeler has two axles");
    check(aver_phys_vehicle_wheel_axle(v, 0) == 0 && aver_phys_vehicle_wheel_axle(v, 3) == 1, "the front axle is axle 0");
    const float bad[6] = {3.42f, 5.0f, 1.4f, 0.5f, 0.0f, 1000.0f};
    check(aver_phys_vehicle_set_axle(aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1), 0, bad, 6) == 0, "an axle call with no wheels is refused");
    shutdown();
}

// Drives to ~1200 cm/s, then holds a steady turn; returns the mean |roll| (sin of the up vector's y) and the peak |lateral g|.
struct TurnResult { float roll = 0.0f; float peakLatG = 0.0f; };
static TurnResult steadyTurn(const Hook& pre, const Hook& post = {}, float steer = 0.2f) {
    beginWorld();
    const int32_t v = buildCar(0, 0, 5, 0xC, pre, post);
    run(90);
    run(260, nullptr, [&](int) { aver_phys_vehicle_set_input(v, std::max(0.0f, std::min(1.0f, (1200.0f - aver_phys_vehicle_forward_speed(v)) / 400.0f)), 0.0f, 0.0f, 0.0f); });
    TurnResult r;
    float sum = 0.0f;
    int n = 0;
    run(120, nullptr, [&](int k) {
        aver_phys_vehicle_set_input(v, std::max(0.0f, std::min(1.0f, (1200.0f - aver_phys_vehicle_forward_speed(v)) / 400.0f)), steer, 0.0f, 0.0f);
        if (k > 40) {
            const Pose p = poseOf(v);
            sum += std::fabs(rotated(p.q, Vec3(0, 0, 1)).y);
            ++n;
            r.peakLatG = std::max(r.peakLatG, std::fabs(stateOf(v).f[AVER_PHYS_VS_LAT_ACCEL_G]));
        }
    });
    r.roll = n ? sum / n : 0.0f;
    shutdown();
    return r;
}

static void testChassisTuning() {
    AVER_INFO("=== anti-roll, tyre curves and the grip scale change a steady turn ===");
    auto bar = [](float k) { return [k](int32_t id) { const float a[6] = {3.42f, 1.0f, 1.4f, 0.5f, 0.0f, k}; aver_phys_vehicle_set_axle(id, 0, a, 6); aver_phys_vehicle_set_axle(id, 1, a, 6); }; };
    const TurnResult soft = steadyTurn(bar(0.0f));
    const TurnResult stiff = steadyTurn(bar(8000.0f));
    check(stiff.roll < soft.roll, "an 8000 N/m bar rolls less than none: " + f2s(stiff.roll) + " against " + f2s(soft.roll));

    // A hard turn, so the tyres saturate: the peak lateral g follows the grip.
    const TurnResult full = steadyTurn({}, {}, 0.7f);
    const TurnResult half = steadyTurn({}, [](int32_t id) { aver_phys_vehicle_tune(id, AVER_PHYS_VU_GRIP_SCALE, 0.5f); }, 0.7f);
    check(full.peakLatG > 0.3f, "the full-grip car pulls lateral g: " + f2s(full.peakLatG));
    check(half.peakLatG < full.peakLatG * 0.75f, "half the grip pulls clearly less: " + f2s(half.peakLatG) + " against " + f2s(full.peakLatG));

    const float lx[3] = {0, 0.06f, 0.2f}, lm[3] = {0, 0.6f, 0.5f};
    const float ax[3] = {0, 3, 20}, am[3] = {0, 0.6f, 0.5f};
    const TurnResult flat = steadyTurn([&](int32_t id) { aver_phys_vehicle_set_wheel_curves(id, -1, lx, lm, 3, ax, am, 3); }, {}, 0.7f);
    check(flat.peakLatG < full.peakLatG * 0.8f, "a flatter, lower friction curve pulls less: " + f2s(flat.peakLatG) + " against " + f2s(full.peakLatG));
}

// ---- The aids ---------------------------------------------------------------------------------------

// Speed reached, then a full brake: the most negative driven-wheel slip ratio seen.
static float brakeMinSlip(bool abs) {
    beginWorld();
    const int32_t v = buildCar(0, 0, 5, 0xC, {}, [&](int32_t id) { assistsOn(id, abs, false); });
    run(90);
    run(300, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    float minSlip = 1.0f;
    run(120, nullptr, [&](int) {
        aver_phys_vehicle_set_input(v, 0.0f, 0.0f, 1.0f, 0.0f);
        if (aver_phys_vehicle_forward_speed(v) > 500.0f)
            for (int w = 0; w < 4; ++w) minSlip = std::min(minSlip, wheelOf(v, w).f[AVER_PHYS_VWS_SLIP_LONG]);
    });
    shutdown();
    return minSlip;
}

static float startPeakSlip(bool tcs) {
    aver_phys_init();
    const int32_t ice = aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
    aver_phys_body_set_friction(ice, 0.05f);
    const int32_t v = buildCar(0, 0, 5, 0xC, {}, [&](int32_t id) { assistsOn(id, false, tcs); });
    run(90);
    float peak = 0.0f;
    run(150, nullptr, [&](int) {
        aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
        for (int w = 2; w < 4; ++w) peak = std::max(peak, wheelOf(v, w).f[AVER_PHYS_VWS_SLIP_LONG]);
    });
    shutdown();
    return peak;
}

static void testAids() {
    AVER_INFO("=== driver aids: ABS, traction control, steering rate and speed, brake-to-reverse, hold ===");
    const float locked = brakeMinSlip(false);
    const float withAbs = brakeMinSlip(true);
    check(locked < -0.8f, "without ABS a full brake locks a wheel: slip " + f2s(locked));
    check(withAbs > locked + 0.1f, "ABS keeps the worst wheel from locking as hard: " + f2s(withAbs) + " against " + f2s(locked));

    const float spin = startPeakSlip(false);
    const float held = startPeakSlip(true);
    check(spin > 0.5f, "a full-throttle start on ice spins the driven wheels: " + f2s(spin));
    check(held < spin, "traction control lowers the peak wheelspin: " + f2s(held) + " against " + f2s(spin));

    // Steering rate: the applied value moves at STEER_IN (3 locks a second) towards the target.
    beginWorld();
    const int32_t v = buildCar(0, 0, 5, 0xC, {}, [](int32_t id) { assistsOn(id, false, false); });
    run(30);
    run(10, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 0.0f, 1.0f, 0.0f, 0.0f); });
    const float steer10 = stateOf(v).f[AVER_PHYS_VS_STEER_APPLIED];
    check(std::fabs(steer10 - 10.0f * 3.0f / 60.0f) < 0.03f, "after 10 steps the applied steer is " + f2s(steer10) + " (expected 0.5)");
    run(60, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 0.0f, 1.0f, 0.0f, 0.0f); });
    check(std::fabs(stateOf(v).f[AVER_PHYS_VS_STEER_APPLIED] - 1.0f) < 0.01f, "and it settles at full lock when slow");

    // Speed sensitivity: a curve of (0,1) (1500,0.6) (3500,0.35); at 3000 cm/s the target is 0.4125.
    aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_0, 0.0f);
    aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_1, 1.0f);
    aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_2, 1500.0f);
    aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_3, 0.6f);
    aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_4, 3500.0f);
    aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_5, 0.35f);
    int32_t body = aver_phys_vehicle_body(v);
    run(80, nullptr, [&](int) {
        aver_phys_body_set_velocity(body, 3000.0f, 0.0f, 0.0f);
        aver_phys_vehicle_set_input(v, 0.0f, 1.0f, 0.0f, 0.0f);
    });
    const float fast = stateOf(v).f[AVER_PHYS_VS_STEER_APPLIED];
    check(std::fabs(fast - 0.4125f) < 0.05f, "at 3000 cm/s full steer asks for " + f2s(fast) + " of a lock (expected 0.41)");
    shutdown();

    // Brake-to-reverse: reverse demand while rolling forward brakes first, then reverses.
    beginWorld();
    const int32_t c = buildCar(0, 0, 5, 0xC, {}, [](int32_t id) { assistsOn(id, false, false); });
    run(90);
    run(240, nullptr, [&](int) { aver_phys_vehicle_set_input(c, 1.0f, 0.0f, 0.0f, 0.0f); });
    bool braked = false;
    run(30, nullptr, [&](int) {
        aver_phys_vehicle_set_input(c, -1.0f, 0.0f, 0.0f, 0.0f);
        if (stateOf(c).f[AVER_PHYS_VS_BRAKE_APPLIED] > 0.3f && stateOf(c).f[AVER_PHYS_VS_THROTTLE_APPLIED] <= 0.05f) braked = true;
    });
    check(braked, "reverse demand at speed is applied as a brake, not as reverse torque");
    run(480, nullptr, [&](int) { aver_phys_vehicle_set_input(c, -1.0f, 0.0f, 0.0f, 0.0f); });
    check(aver_phys_vehicle_forward_speed(c) < -100.0f, "and it then reverses: " + f2s(aver_phys_vehicle_forward_speed(c)));
    // Hold brake: released pedals at standstill apply the hold.
    run(240, nullptr, [&](int) { aver_phys_vehicle_set_input(c, 0.0f, 0.0f, 1.0f, 0.0f); });
    run(120, nullptr, [&](int) { aver_phys_vehicle_set_input(c, 0.0f, 0.0f, 0.0f, 0.0f); });
    const State s = stateOf(c);
    check(std::fabs(s.f[AVER_PHYS_VS_FORWARD_SPEED_CMS]) < 30.0f && std::fabs(s.f[AVER_PHYS_VS_BRAKE_APPLIED] - 0.3f) < 0.05f,
          "at rest with no pedal the hold brake is on: " + f2s(s.f[AVER_PHYS_VS_BRAKE_APPLIED]));
    shutdown();
}

// ---- Aero, telemetry, events -------------------------------------------------------------------------

static float topDistance(float drag) {
    beginWorld();
    const int32_t v = buildCar(0, 0, 5, 0xC, {}, [&](int32_t id) { aver_phys_vehicle_set_aero(id, drag, 0.0f); });
    run(90);
    const float x0 = poseOf(v).p[0];
    run(480, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const float d = poseOf(v).p[0] - x0;
    shutdown();
    return d;
}

static float loadAtSpeed(float downforce) {
    beginWorld();
    const int32_t v = buildCar(0, 0, 5, 0xC, {}, [&](int32_t id) { aver_phys_vehicle_set_aero(id, 0.0f, downforce); });
    run(90);
    const int32_t body = aver_phys_vehicle_body(v);
    run(90, nullptr, [&](int) { aver_phys_body_set_velocity(body, 3000.0f, 0.0f, 0.0f); });
    float sum = 0.0f;
    for (int w = 0; w < 4; ++w) sum += wheelOf(v, w).f[AVER_PHYS_VWS_LOAD_N];
    shutdown();
    return sum;
}

static void testAeroAndTelemetry() {
    AVER_INFO("=== aero, telemetry ===");
    const float free = topDistance(0.0f);
    // Drag is C*v^2 newtons on a 1300 kg car with an engine that is still pulling at 25 m/s: C = 2 (five times a real
    // car's) cost 4.5% of the distance, the force being right (it matches dv = C*v^2/m integrated), so the 10% margin
    // needs C = 12 (measured 17% shorter).
    const float dragged = topDistance(12.0f);
    check(dragged < free * 0.9f, "drag shortens 8 s of full throttle: " + f2s(dragged) + " against " + f2s(free));
    const float plain = loadAtSpeed(0.0f);
    const float pressed = loadAtSpeed(3.0f);
    check(pressed > plain * 1.1f, "downforce raises the summed wheel load at 30 m/s: " + f2s(pressed) + " against " + f2s(plain));

    aver_phys_init();
    const int32_t floor = aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
    aver_phys_set_entity(floor, 4242);
    const int32_t v = buildCar(0, 0, 5);
    run(120);
    float load = 0.0f;
    for (int w = 0; w < 4; ++w) load += wheelOf(v, w).f[AVER_PHYS_VWS_LOAD_N];
    const float weight = kMass * 9.81f;
    check(std::fabs(load - weight) < weight * 0.15f, "the four wheel loads sum to the weight: " + f2s(load) + " of " + f2s(weight) + " N");
    const WState w0 = wheelOf(v, 0);
    check(w0.f[AVER_PHYS_VWS_COMPRESSION_CM] > 0.0f, "the suspension is compressed: " + f2s(w0.f[AVER_PHYS_VWS_COMPRESSION_CM]) + " cm");
    check(w0.i[AVER_PHYS_VWSI_CONTACT] == 1 && w0.i[AVER_PHYS_VWSI_CONTACT_ENTITY] == 4242, "the contact entity is what the floor was stamped with");
    check(w0.i[AVER_PHYS_VWSI_CONTACT_BODY] == floor, "the contact body is the floor");
    check(std::fabs(w0.f[AVER_PHYS_VWS_CONTACT_NORMAL_Z] - 1.0f) < 0.05f, "the contact normal points up");
    run(120, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    const State s = stateOf(v);
    float vel[3];
    aver_phys_vehicle_velocity(v, vel);
    const float speed = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    check(s.f[AVER_PHYS_VS_RPM] >= 1000.0f - 1.0f && s.f[AVER_PHYS_VS_RPM] <= 6000.0f + 1.0f, "rpm stays in the engine's range: " + f2s(s.f[AVER_PHYS_VS_RPM]));
    check(std::fabs(s.f[AVER_PHYS_VS_SPEED_CMS] - speed) < speed * 0.02f + 2.0f, "speed matches the velocity vector: " + f2s(s.f[AVER_PHYS_VS_SPEED_CMS]) + " and " + f2s(speed));
    check(s.i[AVER_PHYS_VSI_WHEEL_COUNT] == 4 && s.i[AVER_PHYS_VSI_AXLE_COUNT] == 2 && s.i[AVER_PHYS_VSI_KIND] == AVER_PHYS_VEH_KIND_WHEELED, "the counts and the kind are reported");
    shutdown();
}

static void testEvents() {
    AVER_INFO("=== events: shift, skid, land, impact ===");
    // Shift and skid on a hard brake.
    beginWorld();
    const int32_t v = buildCar(0, 0, 5);
    run(90);
    std::vector<Ev> ev;
    run(300, &ev, [&](int) { aver_phys_vehicle_set_input(v, 1.0f, 0.0f, 0.0f, 0.0f); });
    bool shift12 = false;
    for (const Ev& e : ev) shift12 |= e.k[0] == AVER_PHYS_VEV_SHIFT && e.k[2] == 1 && e.k[3] == 2 && e.k[1] == v;
    check(shift12, "accelerating through the gears gives a SHIFT 1 -> 2 for this vehicle");
    ev.clear();
    run(120, &ev, [&](int) { aver_phys_vehicle_set_input(v, 0.0f, 0.0f, 1.0f, 0.0f); });
    bool skid = false;
    for (const Ev& e : ev) skid |= e.k[0] == AVER_PHYS_VEV_SKID_BEGIN;
    check(skid, "a full brake locks the wheels and gives SKID_BEGIN");
    shutdown();

    // Landing: dropped from 2 m.
    beginWorld();
    const int32_t d = buildCar(0, 0, 200);
    ev.clear();
    run(180, &ev);
    float landSpeed = 0.0f;
    for (const Ev& e : ev)
        if (e.k[0] == AVER_PHYS_VEV_LAND && landSpeed == 0.0f) landSpeed = e.f[6];
    const float expect = std::sqrt(2.0f * 980.0f * 190.0f);
    check(landSpeed > 0.0f && std::fabs(landSpeed - expect) < expect * 0.2f, "LAND reports the downward speed: " + f2s(landSpeed) + " against about " + f2s(expect));
    (void)d;
    shutdown();

    // Impact: rolled into a wall at 10 m/s.
    aver_phys_init();
    aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
    aver_phys_add_static_box(2500.0f, 0.0f, 200.0f, 50.0f, 1000.0f, 200.0f);
    const int32_t c = buildCar(0, 0, 5);
    run(90);
    const int32_t body = aver_phys_vehicle_body(c);
    aver_phys_body_set_velocity(body, 1000.0f, 0.0f, 0.0f);
    ev.clear();
    float lastApproach = 0.0f;
    run(150, &ev, [&](int) {
        bool seen = false;
        for (const Ev& e : ev) seen |= e.k[0] == AVER_PHYS_VEV_IMPACT;
        if (!seen) lastApproach = aver_phys_vehicle_forward_speed(c);
    });
    int impacts = 0;
    float closing = 0.0f;
    for (const Ev& e : ev)
        if (e.k[0] == AVER_PHYS_VEV_IMPACT) { ++impacts; closing = std::max(closing, e.f[6]); }
    check(impacts >= 1, "hitting a wall gives an IMPACT");
    check(closing > lastApproach * 0.7f && closing < lastApproach * 1.3f + 50.0f,
          "its closing speed is about the approach speed: " + f2s(closing) + " against " + f2s(lastApproach));
    shutdown();
}

static void testChassisBoxes() {
    AVER_INFO("=== a compound chassis keeps the centre of mass it was asked for ===");
    beginWorld();
    // A cab and a bed of different heights.
    const float boxes[12] = {120, 0, 60, 100, 90, 60, -110, 0, 40, 110, 90, 40};
    const int32_t v = buildCar(0, 0, 5, 0xC, [&](int32_t id) { aver_phys_vehicle_set_chassis_boxes(id, boxes, 2); });
    run(150);
    const Pose p = poseOf(v);
    float com[3];
    aver_phys_body_position(aver_phys_vehicle_body(v), com);
    check(std::fabs(com[2] - p.p[2] - kComZ) < 4.0f, "the centre of mass is " + f2s(com[2] - p.p[2]) + " above the origin (asked for 40)");
    int touching = 0;
    for (int w = 0; w < 4; ++w) touching += aver_phys_vehicle_wheel_contact(v, w);
    check(touching == 4, "it rests on its four wheels");
    check(aver_phys_vehicle_set_chassis_boxes(v, boxes, 2) == 0, "the boxes are refused after finish");
    shutdown();
}

static void testLifetime() {
    AVER_INFO("=== v2 calls on dead handles, before and after finish; flip; collision period ===");
    beginWorld();
    float f[AVER_PHYS_VS_COUNT];
    check(aver_phys_vehicle_state(999, f, AVER_PHYS_VS_COUNT, nullptr, 0) == 0, "state of a dead handle is 0");
    check(aver_phys_vehicle_tune(999, 0, 1.0f) == 0 && aver_phys_vehicle_flip(999, 10.0f) == 0, "tune and flip refuse a dead handle");
    const int32_t b = aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1);
    check(aver_phys_vehicle_state(b, f, AVER_PHYS_VS_COUNT, nullptr, 0) == 0, "state before finish is 0");
    check(aver_phys_vehicle_set_gear(b, 1) == 0 && aver_phys_vehicle_flip(b, 10.0f) == 0, "live calls are refused before finish");
    const float tooLong[AVER_PHYS_VE_COUNT + 1] = {500, 1000, 6000, 0, 0.2f, 1};
    check(aver_phys_vehicle_set_engine_ex(b, tooLong, AVER_PHYS_VE_COUNT + 1) == 0, "a parameter block longer than defined is refused");
    const float shortBlock[2] = {600.0f, 800.0f};
    check(aver_phys_vehicle_set_engine_ex(b, shortBlock, 2) == 1, "a shorter block is accepted (the rest keep their values)");
    aver_phys_vehicle_destroy(b);

    const int32_t v = buildCar(0, 0, 5);
    run(90);
    check(aver_phys_vehicle_set_engine_ex(v, shortBlock, 2) == 0, "builder calls are refused after finish");
    check(aver_phys_vehicle_set_collision_period(v, 2) == 1 && aver_phys_vehicle_set_collision_period(v, 9) == 0, "the collision period takes 1 to 8");
    run(60, nullptr, [&](int) { aver_phys_vehicle_set_input(v, 0.5f, 0.0f, 0.0f, 0.0f); });
    check(aver_phys_vehicle_wheel_contact(v, 0) == 1 || aver_phys_vehicle_wheel_contact(v, 1) == 1, "at period 2 the wheels still find the ground");

    // On its side, then flipped: upright, still, a little higher.
    const Quat roll = Quat::fromAxisAngle(Vec3(1, 0, 0), kPi * 0.5f);
    aver_phys_vehicle_set_pose(v, 0, 0, 80, roll.x, roll.y, roll.z, roll.w);
    run(5);
    check(aver_phys_vehicle_flip(v, 60.0f) == 1, "flip is accepted");
    const Pose p = poseOf(v);
    float vel[3];
    aver_phys_vehicle_velocity(v, vel);
    check(rotated(p.q, Vec3(0, 0, 1)).z > 0.95f, "the car is upright after a flip");
    check(std::fabs(vel[0]) + std::fabs(vel[1]) + std::fabs(vel[2]) < 1.0f, "and still");
    shutdown();
}

static std::vector<float> replay() {
    beginWorld();
    const int32_t v = buildCar(0, 0, 5, 0xC, {}, [](int32_t id) { assistsOn(id, true, true); });
    std::vector<float> out;
    run(90);
    run(400, nullptr, [&](int k) {
        aver_phys_vehicle_set_input(v, k < 200 ? 1.0f : -0.5f, k % 90 < 45 ? 0.4f : -0.4f, k > 300 ? 0.5f : 0.0f, 0.0f);
    });
    const Pose p = poseOf(v);
    for (float x : p.p) out.push_back(x);
    for (float x : p.q) out.push_back(x);
    const State s = stateOf(v);
    for (float x : s.f) out.push_back(x);
    shutdown();
    return out;
}

static void testReplay() {
    AVER_INFO("=== the same input twice is the same, bit for bit ===");
    const std::vector<float> a = replay();
    const std::vector<float> b = replay();
    check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "poses and telemetry are bit-identical");
}

int main() {
    AVER_INFO("=== VehicleExtTest ===");
    testV1Identity();
    testTorqueCurve();
    testGearbox();
    testDifferentials();
    testChassisTuning();
    testAids();
    testAeroAndTelemetry();
    testEvents();
    testChassisBoxes();
    testLifetime();
    testReplay();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
