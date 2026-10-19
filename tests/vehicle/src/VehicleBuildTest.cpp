// VehicleBuildTest -- buildVehicle through the real physics world: every preset and the shipped specs build,
// settle on their wheels and drive, a preset-shaped spec drives exactly like the car the old traffic builder made, and
// a spec the physics ABI cannot take leaves nothing behind.
//
// OBSERVED MOTION, as in tests/physics: a vehicle that builds is not a vehicle that drives. The numbers are UNVERIFIED
// starting values -- written without being run.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehicleBuild.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace aver;
using namespace aver::vehicle;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;

static void beginWorld() {
    aver_phys_init();
    aver_phys_add_static_box(0.0f, 0.0f, -50.0f, 20000.0f, 20000.0f, 50.0f);
}
static void run(int n, i32 v = 0, f32 throttle = 0.0f, f32 steer = 0.0f) {
    for (int i = 0; i < n; ++i) {
        if (v) aver_phys_vehicle_set_input(v, throttle, steer, 0.0f, 0.0f);
        aver_phys_step(kDt);
    }
}
static int contacts(i32 v) {
    int n = 0;
    for (int i = 0, c = aver_phys_vehicle_wheel_count(v); i < c; ++i) n += aver_phys_vehicle_wheel_contact(v, i);
    return n;
}
struct Pose { float p[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };
static Pose poseOf(i32 v) { Pose r; aver_phys_vehicle_pose(v, r.p, r.q); return r; }

static Transform at(f32 x, f32 y, f32 z) {
    Transform t;
    t.position = Vec3(x, y, z);
    return t;
}

// ---- the old traffic builder, copied verbatim as the golden reference ------------------------------------

struct OldPreset { f32 mass, rMin, rMax, engine, steer, hz; bool front; };
static i32 oldBuild(const OldPreset& pr, const Vec3& size, const Transform& xf) {
    const f32 length = size.x, width = size.y, height = size.z;
    const f32 radius = std::clamp(0.17f * height, pr.rMin, pr.rMax);
    const f32 wheelWidth = 0.7f * radius;
    const i32 v = aver_phys_vehicle_create(0.5f * length, 0.5f * width, 0.5f * 0.55f * height, 0.6f * radius, 0.0f, 0.0f, 0.35f * height, pr.mass,
                                           xf.position.x, xf.position.y, xf.position.z, xf.rotation.x, xf.rotation.y, xf.rotation.z, xf.rotation.w);
    const f32 axleX = std::max(0.5f * length - 1.2f * radius, 0.1f * length);
    const f32 trackY = std::max(0.5f * width - 0.6f * wheelWidth, 0.25f * width);
    const f32 omega = 2.0f * kPi * pr.hz;
    const f32 attachZ = radius + 30.0f - 981.0f / (omega * omega);
    const f32 brake = pr.mass * 9.0f * (radius * 0.01f) * 0.25f;
    struct Wheel { f32 x, y; bool front; };
    const Wheel wheels[4] = {{axleX, -trackY, true}, {axleX, trackY, true}, {-axleX, -trackY, false}, {-axleX, trackY, false}};
    for (const Wheel& wh : wheels)
        aver_phys_vehicle_add_wheel(v, wh.x, wh.y, attachZ, radius, wheelWidth, 5.0f, 30.0f, pr.hz, 0.5f, wh.front ? pr.steer : 0.0f, brake,
                                    wh.front ? 0.0f : 1.5f * brake, wh.front == pr.front ? 1 : 0);
    aver_phys_vehicle_set_engine(v, pr.engine, 1000.0f, 6000.0f);
    aver_phys_vehicle_finish(v, 60.0f);
    return v;
}

static const char* const kTruck6x4 =
    "OCVEHICLE 1\n"
    "NAME Truck6x4\n"
    "BODY half 520 130 70 clearance 45 com 0 0 90 mass 8000 pitchroll 60\n"
    "ENGINE torque 1500 minrpm 800 maxrpm 3000\n"
    "CENTER lsd 1.4\n"
    "AXLE 0 final 3.42 diff lsd lsd 1.4 share 0 antiroll 1500\n"
    "AXLE 1 final 4.1 diff lsd lsd 1.4 share 0 antiroll 1500\n"
    "AXLE 2 final 4.1 diff lsd lsd 1.4 share 0 antiroll 1500\n"
    "WHEELCLASS big radius 50 width 30 susp 8 40 hz 1.6 damping 0.6 brake 6000 handbrake 9000\n"
    "WHEEL FL class big hub 380 -115 50 steer 30\n"
    "WHEEL FR class big hub 380 115 50 steer 30\n"
    "WHEEL ML class big hub -150 -115 50 driven 1\n"
    "WHEEL MR class big hub -150 115 50 driven 1\n"
    "WHEEL RL class big hub -330 -115 50 driven 1 handbrake 1\n"
    "WHEEL RR class big hub -330 115 50 driven 1 handbrake 1\n";

static void testPresetsDrive() {
    AVER_INFO("=== every preset builds, settles on its wheels and drives ===");
    const char* const names[] = {"car", "van", "truck", "bus", "sports"};
    const Vec3 sizes[] = {Vec3(440, 180, 140), Vec3(520, 200, 260), Vec3(900, 250, 320), Vec3(1200, 260, 330), Vec3(430, 190, 120)};
    for (int i = 0; i < 5; ++i) {
        beginWorld();
        VehicleSpec s;
        specFromBounds(names[i], sizes[i], s);
        BuiltVehicle b;
        std::string why;
        const bool ok = buildVehicle(s, at(0, 0, 10), b, why);
        check(ok, std::string(names[i]) + ": builds (" + why + ")");
        if (!ok) { aver_phys_shutdown(); continue; }
        check(b.wheelCount == 4 && b.kind == Kind::Wheeled && b.vehicle != 0 && b.body != 0, std::string(names[i]) + ": four wheels, wheeled, handles set");
        check(b.wheelbase > 0.0f && b.maxSteerRad > 0.0f, std::string(names[i]) + ": wheelbase " + f2s(b.wheelbase) + " and steer " + f2s(b.maxSteerRad));
        run(150);
        check(contacts(b.vehicle) == 4, std::string(names[i]) + ": settles on four wheels");
        const Pose p0 = poseOf(b.vehicle);
        run(240, b.vehicle, 1.0f);
        const Pose p1 = poseOf(b.vehicle);
        check(p1.p[0] - p0.p[0] > 150.0f, std::string(names[i]) + ": drives along +X, " + f2s(p1.p[0] - p0.p[0]) + " cm in 4 s");
        aver_phys_shutdown();
    }
}

static void testShippedSpecs() {
    AVER_INFO("=== the sedan and the tank ===");
    {
        beginWorld();
        BuiltVehicle b;
        std::string why;
        const VehicleSpec* sedan = builtinSpec("sedan");
        check(sedan && buildVehicle(*sedan, at(0, 0, 10), b, why), "the sedan builds: " + why);
        if (sedan && b.vehicle) {
            run(150);
            check(contacts(b.vehicle) == 4, "the sedan settles");
            run(240, b.vehicle, 1.0f);
            float st[AVER_PHYS_VS_COUNT];
            aver_phys_vehicle_state(b.vehicle, st, AVER_PHYS_VS_COUNT, nullptr, 0);
            check(st[AVER_PHYS_VS_FORWARD_SPEED_CMS] > 300.0f, "and drives: " + f2s(st[AVER_PHYS_VS_FORWARD_SPEED_CMS]) + " cm/s after 4 s");
        }
        aver_phys_shutdown();
    }
    {
        beginWorld();
        BuiltVehicle b;
        std::string why;
        const VehicleSpec* tank = builtinSpec("tank");
        check(tank && buildVehicle(*tank, at(0, 0, 10), b, why), "the tank builds: " + why);
        if (tank && b.vehicle) {
            check(b.kind == Kind::Tracked && b.wheelCount == 10, "tracked, ten wheels");
            run(180);
            check(contacts(b.vehicle) >= 8, "the tank settles on its tracks: " + std::to_string(contacts(b.vehicle)) + " of 10");
            const Pose p0 = poseOf(b.vehicle);
            run(300, b.vehicle, 1.0f);
            check(poseOf(b.vehicle).p[0] - p0.p[0] > 100.0f, "and drives: " + f2s(poseOf(b.vehicle).p[0] - p0.p[0]) + " cm in 5 s");
        }
        aver_phys_shutdown();
    }
}

static void testMatchesOldBuilder() {
    AVER_INFO("=== a preset-shaped spec drives exactly like the old traffic car ===");
    const OldPreset car = {1300.0f, 28.0f, 38.0f, 500.0f, 35.0f, 2.0f, false};
    const Vec3 size(440, 180, 140);
    Pose oldPose, newPose;
    {
        beginWorld();
        const i32 v = oldBuild(car, size, at(30, -20, 10));
        run(60);
        for (int k = 0; k < 300; ++k) {
            aver_phys_vehicle_set_input(v, 0.8f, k > 100 ? 0.25f : 0.0f, k > 250 ? 0.4f : 0.0f, 0.0f);
            aver_phys_step(kDt);
        }
        oldPose = poseOf(v);
        aver_phys_shutdown();
    }
    {
        beginWorld();
        VehicleSpec s;
        specFromBounds("car", size, s);
        BuiltVehicle b;
        std::string why;
        check(buildVehicle(s, at(30, -20, 10), b, why), "the spec builds: " + why);
        run(60);
        for (int k = 0; k < 300; ++k) {
            aver_phys_vehicle_set_input(b.vehicle, 0.8f, k > 100 ? 0.25f : 0.0f, k > 250 ? 0.4f : 0.0f, 0.0f);
            aver_phys_step(kDt);
        }
        newPose = poseOf(b.vehicle);
        aver_phys_shutdown();
    }
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i) worst = std::max(worst, std::fabs(oldPose.p[i] - newPose.p[i]));
    for (int i = 0; i < 4; ++i) worst = std::max(worst, std::fabs(oldPose.q[i] - newPose.q[i]));
    check(oldPose.p[0] > 100.0f, "the reference car drove: x = " + f2s(oldPose.p[0]));
    check(worst <= 1e-4f, "the two poses agree to " + f2s(worst));
}

static void testTruck() {
    AVER_INFO("=== a 6x4 truck ===");
    VehicleSpec s;
    std::string err;
    check(parseOcVehicle(kTruck6x4, s, err), "the truck text parses: " + err);
    beginWorld();
    BuiltVehicle b;
    std::string why;
    check(buildVehicle(s, at(0, 0, 10), b, why), "it builds: " + why);
    if (b.vehicle) {
        check(b.wheelCount == 6 && aver_phys_vehicle_axle_count(b.vehicle) == 3, "six wheels, three axles");
        check(aver_phys_vehicle_wheel_axle(b.vehicle, 0) == 0 && aver_phys_vehicle_wheel_axle(b.vehicle, 5) == 2, "front axle 0, rear axle 2");
        run(200);
        check(contacts(b.vehicle) == 6, "it settles on six wheels: " + std::to_string(contacts(b.vehicle)));
        const Pose p0 = poseOf(b.vehicle);
        run(300, b.vehicle, 1.0f);
        check(poseOf(b.vehicle).p[0] - p0.p[0] > 100.0f, "and pulls away with both rear axles driven: " + f2s(poseOf(b.vehicle).p[0] - p0.p[0]));
    }
    aver_phys_shutdown();
}

static void testRefusal() {
    AVER_INFO("=== a spec the world cannot take leaves nothing behind ===");
    beginWorld();
    const i32 before = aver_phys_body_count();
    VehicleSpec s;
    std::string err;
    parseOcVehicle(kTruck6x4, s, err);
    s.wheels.clear();
    BuiltVehicle b;
    std::string why;
    check(!buildVehicle(s, at(0, 0, 10), b, why) && !why.empty(), "an invalid spec is refused with a reason: " + why);
    check(aver_phys_body_count() == before, "and no body was made");
    aver_phys_shutdown();
    check(!buildVehicle(*builtinSpec("sedan"), at(0, 0, 10), b, why), "with no physics world there is nothing to build: " + why);
}

int main() {
    AVER_INFO("=== VehicleBuildTest ===");
    testPresetsDrive();
    testShippedSpecs();
    testMatchesOldBuilder();
    testTruck();
    testRefusal();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
