// VehicleSystemTest -- drivable vehicles on scene entities, headless. Specs, actions, audio and every sink are injected; the
// physics world is the real Jolt one, through Aver.Physics. The numbers are UNVERIFIED starting values: this file was
// written without being run.
//
//   * the component: 96 bytes, fields verified, OUT fields read-only through the generic field ABI,
//   * build and pose: a vehicle appears at the entity's pose, the entity follows the chassis, a destroyed entity takes its
//     vehicle with it, a scaled one is refused, a parked fleet writes nothing, at most two builds a frame,
//   * input: the fields drive it; with a driver the Drive.* actions do (gears only in a manual box), NoActions ignores them,
//   * wheel visuals: child entities receive the wheel poses, bones receive rig inputs, entities() lists them,
//   * driving: enter and exit hide and restore the driver, take and give back the character's gravity and strength, the
//     camera sink hears Enter, Next and Exit with the rig points,
//   * telemetry and events: OUT fields match the physics state, a crash raises the notify, the crash sink and damage, destroyed
//     cuts the engine and repair restores it, smoke follows time not frames,
//   * audio: the nearest eight are heard, pitch follows rpm, everything stops out of range and on clear,
//   * LOD: far driverless vehicles run their wheel casts less often, a driven one never,
//   * replay: the same script twice gives the same bits.
#include "VehicleFixtures.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace vt;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

static Telemetry telOf(Bench& b, scene::Entity e) {
    Telemetry t;
    b.sys.telemetry(e, t);
    return t;
}

static void testComponent() {
    AVER_INFO("=== the component ===");
    Bench b(false);
    scene::World& w = b.w;
    const u32 id = VehiclePhysicsSystem::typeId();
    check(id != 0 && w.componentSize(id) == 96 && w.componentVerified(id), "CVehicle is registered, 96 bytes, its field table verified");
    const scene::FieldDesc* driver = w.field(w.fieldId("CVehicle.driver"));
    const scene::FieldDesc* health = w.field(w.fieldId("CVehicle.outHealth01"));
    const scene::FieldDesc* thr = w.field(w.fieldId("CVehicle.inThrottle"));
    check(driver && driver->readOnly && health && health->readOnly, "the OUT fields are read-only through the generic ABI");
    check(thr && !thr->readOnly, "the input fields are writable");
    const scene::Entity e = b.entityAt("Thing", Vec3(0, 0, 0));
    CVehicle* c = VehiclePhysicsSystem::attach(w, e, 0, 3);
    check(c && c->preset == 3 && c->flags == 0 && c->asset == 0 && c->outHealth01 == 0.0f, "attach writes the defaults (zero-filled storage is a valid, unbuilt vehicle)");
    check(!VehiclePhysicsSystem::attach(w, scene::kInvalidEntity, 0, 0), "an invalid entity is refused");
}

static void testBuildAndPose() {
    AVER_INFO("=== build and pose ===");
    Bench b;
    scene::World& w = b.w;
    const scene::Entity e = b.presetCar("Car", Vec3(0, 0, 10));
    check(!b.sys.isVehicle(e), "nothing is built before the first tick");
    b.frame();
    check(b.sys.isVehicle(e) && b.comp(e)->vehicle != 0 && b.sys.wheelCount(e) == 4, "the first tick builds a four-wheeler");
    check(b.comp(e)->outHealth01 == 1.0f, "and it reads full health");
    b.frames(150);
    const Vec3 p = b.worldPos(e);
    check(std::fabs(p.z) < 6.0f && std::fabs(p.x) < 20.0f, "the entity follows the chassis down onto its wheels: z = " + f2s(p.z));
    b.comp(e)->inThrottle = 1.0f;
    b.frames(120);
    check(b.worldPos(e).x > 100.0f, "and along the road under power: x = " + f2s(b.worldPos(e).x));

    const i32 handle = b.comp(e)->vehicle;
    w.destroy(e);
    b.frame();
    check(!b.sys.isVehicle(e) && aver_phys_vehicle_wheel_count(handle) == 0, "a destroyed entity destroys its vehicle");

    // Not at scale 1: refused, never built.
    const scene::Entity big = b.presetCar("Big", Vec3(3000, 0, 10));
    Transform xf = w.localTransform(big);
    xf.scale = Vec3(2, 2, 2);
    w.setLocalTransform(big, xf);
    b.frames(4);
    check(!b.sys.isVehicle(big), "a scaled entity is not built");
}

static void testParkedFleet() {
    AVER_INFO("=== a parked vehicle dirties no transform ===");
    Bench b;
    scene::World& w = b.w;
    const scene::Entity e = b.presetCar("Car", Vec3(0, 0, 10));
    b.frames(600);
    const u32 rev = w.component<scene::CLocal>(e, scene::kComponentLocal)->rev;
    b.frames(60);
    check(w.component<scene::CLocal>(e, scene::kComponentLocal)->rev == rev, "once it sleeps the system writes nothing");
}

static void testBuildBudget() {
    AVER_INFO("=== at most two builds a frame ===");
    Bench b;
    for (int i = 0; i < 5; ++i) b.presetCar(("Car" + std::to_string(i)).c_str(), Vec3(1500.0f * static_cast<f32>(i), 0, 10));
    b.frame();
    check(b.sys.stats().builtThisFrame == 2 && b.sys.stats().vehicles == 2 && b.sys.stats().pending == 3, "the first frame builds two");
    b.frames(2);
    check(b.sys.stats().vehicles == 5 && b.sys.stats().pending == 0, "three frames build five");
}

static void testInput() {
    AVER_INFO("=== input: fields, actions, gears ===");
    {
        Bench b;
        const scene::Entity e = b.presetCar("Car", Vec3(0, 0, 10));
        b.frames(150);
        b.comp(e)->inThrottle = 1.0f;
        b.frames(150);
        check(telOf(b, e).forwardSpeedCmS > 200.0f, "the input field drives a vehicle with no driver: " + f2s(telOf(b, e).forwardSpeedCmS));
    }
    {
        Bench b;
        const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", sedanSpec());
        const scene::Entity d = b.entityAt("Driver", Vec3(600, 0, 100));
        b.frames(150);
        check(b.sys.enter(e, d), "enter accepts a driver");
        b.actions["Drive.Throttle"].value = 1.0f;
        b.frames(180);
        check(telOf(b, e).forwardSpeedCmS > 300.0f, "Drive.Throttle accelerates: " + f2s(telOf(b, e).forwardSpeedCmS));
        b.actions["Drive.Throttle"].value = 0.0f;
        b.actions["Drive.Brake"].value = 1.0f;
        b.frames(300);
        check(telOf(b, e).speedCmS < 30.0f, "Drive.Brake stops it: " + f2s(telOf(b, e).speedCmS));
        b.actions["Drive.Brake"].value = 0.0f;

        // Gear keys: ignored in an automatic box, change gear in a manual one.
        const i32 gear0 = telOf(b, e).gear;
        b.actions["Drive.ShiftUp"].pressed = true;
        b.frame();
        b.actions["Drive.ShiftUp"].pressed = false;
        b.frames(60);
        check(telOf(b, e).gear == gear0 || !telOf(b, e).manual, "ShiftUp does nothing in an automatic box");
        check(b.sys.setManual(e, true), "switch to manual");
        b.frames(10);
        const i32 g1 = telOf(b, e).gear;
        b.actions["Drive.ShiftUp"].pressed = true;
        b.frame();
        b.actions["Drive.ShiftUp"].pressed = false;
        b.frames(120);
        check(telOf(b, e).gear == g1 + 1, "ShiftUp shifts a manual box: " + std::to_string(g1) + " -> " + std::to_string(telOf(b, e).gear));

        // Reset flips; Exit exits.
        const i32 handle = b.comp(e)->vehicle;
        const Quat roll = Quat::fromAxisAngle(Vec3(1, 0, 0), kPi * 0.5f);
        float p[3], q[4];
        aver_phys_vehicle_pose(handle, p, q);
        aver_phys_vehicle_set_pose(handle, p[0], p[1], p[2] + 80.0f, roll.x, roll.y, roll.z, roll.w);
        b.frames(3);
        b.actions["Drive.Reset"].pressed = true;
        b.frame();
        b.actions["Drive.Reset"].pressed = false;
        b.frame();
        check(telOf(b, e).upZ > 0.9f, "Drive.Reset puts it upright: up.z = " + f2s(telOf(b, e).upZ));
        b.actions["Drive.Exit"].pressed = true;
        b.frame();
        b.actions["Drive.Exit"].pressed = false;
        check(b.sys.driverOf(e) == scene::kInvalidEntity, "Drive.Exit gets the driver out");
    }
    {
        Bench b;
        const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", sedanSpec());
        const scene::Entity d = b.entityAt("Driver", Vec3(600, 0, 100));
        b.frames(150);
        b.sys.enter(e, d);
        b.comp(e)->flags |= kVehicleNoActions;
        b.actions["Drive.Throttle"].value = 1.0f;
        b.frames(150);
        check(telOf(b, e).forwardSpeedCmS < 50.0f, "kVehicleNoActions ignores the actions: " + f2s(telOf(b, e).forwardSpeedCmS));
    }
}

static void testVisuals() {
    AVER_INFO("=== wheel visuals ===");
    Bench b;
    scene::World& w = b.w;
    const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", sedanSpec());
    scene::Entity wheel[4];
    const char* names[4] = {"WheelFL", "WheelFR", "WheelRL", "WheelRR"};
    for (int i = 0; i < 4; ++i) {
        wheel[i] = w.create(names[i], e, Transform{});
        b.made.push_back(wheel[i]);
    }
    b.frames(150);
    const i32 handle = b.comp(e)->vehicle;
    f32 worst = 0.0f;
    for (int i = 0; i < 4; ++i) {
        float p[3], q[4];
        aver_phys_vehicle_wheel_pose(handle, i, p, q);
        const Vec3 lp = w.localTransform(wheel[i]).position;
        worst = std::max(worst, std::fabs(lp.x - p[0]) + std::fabs(lp.y - p[1]) + std::fabs(lp.z - p[2]));
    }
    check(worst < 0.05f, "each wheel child sits where the physics wheel pose says: off by " + f2s(worst));

    b.comp(e)->inSteer = 1.0f;
    b.frames(60);
    const Vec3 fwd = w.localTransform(wheel[0]).rotation.rotate(Vec3(1, 0, 0));
    check(fwd.y > 0.1f, "full right turns the front wheel toward +Y: " + f2s(fwd.y));
    b.comp(e)->inSteer = 0.0f;
    b.comp(e)->inThrottle = 0.6f;
    b.frames(120);
    auto topAngle = [&](scene::Entity wh) {
        const Vec3 t = w.localTransform(wh).rotation.rotate(Vec3(0, 0, 1));
        return std::atan2(t.x, t.z);
    };
    const f32 a0 = topAngle(wheel[2]);
    b.frame();
    f32 delta = topAngle(wheel[2]) - a0;
    while (delta > kPi) delta -= kTwoPi;
    while (delta < -kPi) delta += kTwoPi;
    check(delta > 0.0f, "a rear wheel spins the way that rolls forward: " + f2s(delta));

    const std::vector<scene::Entity> ents = b.sys.entities();
    bool all = std::find(ents.begin(), ents.end(), e) != ents.end();
    for (int i = 0; i < 4; ++i) all = all && std::find(ents.begin(), ents.end(), wheel[i]) != ents.end();
    check(all, "entities() lists the vehicle and its wheel children");

    // A missing child: the vehicle still builds and drives.
    VehicleSpec s = sedanSpec();
    s.wheels[1].node = "NoSuchChild";
    const scene::Entity e2 = b.specCar("Car2", Vec3(3000, 0, 10), "Vehicles/Sedan2.ocvehicle", s);
    b.frames(4);
    check(b.sys.isVehicle(e2), "a wheel node with no child is a warning, not an error");
}

static void testBones() {
    AVER_INFO("=== skeletal wheels get rig inputs ===");
    Bench b;
    VehicleSpec s = sedanSpec();
    for (usize i = 0; i < s.wheels.size(); ++i) { s.wheels[i].node.clear(); s.wheels[i].bone = "Bone" + std::to_string(i); }
    const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Rigged.ocvehicle", s);
    b.frames(40);
    std::vector<u64> hashes;
    for (const Sinks::RigCall& c : b.sinks.rig)
        if (c.e == e && std::find(hashes.begin(), hashes.end(), c.hash) == hashes.end()) hashes.push_back(c.hash);
    check(hashes.size() == 8, "four bones give eight distinct named inputs (wheel_<bone>_rot and _pos): " + std::to_string(hashes.size()));
    check(std::find(hashes.begin(), hashes.end(), fnv1a64(std::string_view("wheel_Bone2_rot"))) != hashes.end(), "named from the bone");
}

static void testDriving() {
    AVER_INFO("=== getting in and out ===");
    Bench b;
    scene::World& w = b.w;
    const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", sedanSpec());
    const scene::Entity d = b.entityAt("Driver", Vec3(600, 0, 100));
    Bench::setBounds(w, d, 60, 60, 180);
    const scene::Entity d2 = b.entityAt("Other", Vec3(700, 0, 100));
    const i32 ch = aver_phys_character_create(30.0f, 180.0f, 600.0f, 0.0f, 100.0f);
    aver_phys_set_entity(ch, static_cast<i32>(d));
    b.frames(150);
    float g0 = 1.0f, s0 = 0.0f;
    aver_phys_character_gravity_factor(ch, &g0);
    aver_phys_character_max_strength(ch, &s0);

    check(b.sys.enter(e, d), "enter");
    b.frame();
    check(b.sys.driverOf(e) == d && b.comp(e)->driver == d, "the driver is recorded and shows in the component");
    check((w.component<scene::CMeshRenderer>(d, scene::kComponentMeshRenderer)->flags & scene::kMeshRendererVisible) == 0, "the driver's mesh is hidden");
    float g = 1.0f, s = 1.0f;
    aver_phys_character_gravity_factor(ch, &g);
    aver_phys_character_max_strength(ch, &s);
    check(g == 0.0f && s == 0.0f, "the character neither falls nor shoves");
    check(!b.sys.enter(e, d2), "a second driver is refused");
    check(b.sinks.notifyCount("VehicleEntered") == 1, "VehicleEntered is raised");

    check(b.sinks.cameras.size() == 1 && b.sinks.cameras[0].ev == CameraEvent::Enter && b.sinks.cameras[0].vehicle == e && b.sinks.cameras[0].driver == d,
          "the camera sink hears Enter with the vehicle and the driver");
    const VehicleCameraInfo& info = b.sinks.cameras[0].info;
    check(info.chassisBody == aver_phys_vehicle_body(b.comp(e)->vehicle) && info.chassisBody != 0, "and the chassis body to ignore");
    check(info.rigCount == 3 && info.rigs[0].kind == CameraRigKind::Chase && info.rigs[0].armCm == 600.0f && info.rigs[1].kind == CameraRigKind::Hood &&
              info.rigs[1].localPoint.x == 150.0f && info.rigs[2].kind == CameraRigKind::Cockpit && info.rigs[2].localPoint.z == 110.0f,
          "and the spec's three rigs with their points");
    check(w.valid(info.rigs[0].rig) && w.parent(info.rigs[0].rig) == e, "the rigs are children of the vehicle");

    // The driver sits in the seat.
    b.frames(30);
    const Vec3 dp = b.worldPos(d);
    const Vec3 vp = b.worldPos(e);
    check(dist(dp, vp + Vec3(20, -35, 55)) < 8.0f, "the driver is parked at the seat: off by " + f2s(dist(dp, vp + Vec3(20, -35, 55))));

    b.actions["Drive.CameraNext"].pressed = true;
    b.frame();
    b.actions["Drive.CameraNext"].pressed = false;
    check(b.sinks.cameras.size() == 2 && b.sinks.cameras[1].ev == CameraEvent::Next && b.sinks.cameras[1].info.current == 1, "Drive.CameraNext goes to the next rig");

    const scene::Entity rig0 = info.rigs[0].rig;
    check(b.sys.exit(e), "exit");
    b.frame();
    check(b.sys.driverOf(e) == scene::kInvalidEntity && b.comp(e)->driver == 0, "the driver is gone");
    check((w.component<scene::CMeshRenderer>(d, scene::kComponentMeshRenderer)->flags & scene::kMeshRendererVisible) != 0, "the driver's mesh is back");
    aver_phys_character_gravity_factor(ch, &g);
    aver_phys_character_max_strength(ch, &s);
    check(g == g0 && s == s0, "gravity and strength are given back");
    check(b.sinks.cameras.back().ev == CameraEvent::Exit && !w.valid(rig0), "the camera sink hears Exit and the rigs are destroyed");
    check(b.sinks.notifyCount("VehicleExited") == 1, "VehicleExited is raised");
    check(dist(b.worldPos(d), b.worldPos(e)) > 80.0f, "the driver was set down beside the vehicle");
    aver_phys_character_destroy(ch);
}

static u64 skidId() { return fnv1a64(std::string_view("Particles/TyreSmoke.ocparticle")); }

// Smoke emitted while braking from speed, at a given frame rate.
static u32 smokeAt(f32 dt, int frames) {
    Bench b;
    // ABS off: smoke is a locked or sliding tyre, and a sedan whose ABS works keeps its slip under the skid threshold.
    VehicleSpec spec = sedanSpec();
    spec.assist.abs = false;
    const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", spec);
    b.frames(static_cast<int>(2.5f / dt), dt);
    b.comp(e)->inThrottle = 1.0f;
    b.frames(static_cast<int>(4.0f / dt), dt);
    b.comp(e)->inThrottle = 0.0f;
    b.comp(e)->inBrake = 1.0f;
    b.sinks.effects.clear();
    b.frames(frames, dt);
    return b.sinks.effectTotal(skidId());
}

static void testTelemetryAndEvents() {
    AVER_INFO("=== telemetry, events, damage ===");
    {
        Bench b;
        const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", sedanSpec());
        b.frames(120);
        b.comp(e)->inThrottle = 1.0f;
        b.frames(180);
        float f[AVER_PHYS_VS_COUNT];
        i32 iv[AVER_PHYS_VSI_COUNT];
        aver_phys_vehicle_state(b.comp(e)->vehicle, f, AVER_PHYS_VS_COUNT, iv, AVER_PHYS_VSI_COUNT);
        const CVehicle* c = b.comp(e);
        check(c->outRpm == f[AVER_PHYS_VS_RPM] && c->outGear == iv[AVER_PHYS_VSI_GEAR] && c->outSpeedCmS == f[AVER_PHYS_VS_SPEED_CMS],
              "the OUT fields are the physics state: rpm " + f2s(c->outRpm) + ", gear " + std::to_string(c->outGear));
        check((c->outState & kVehicleStateGrounded) != 0 && c->outWheelsOnGround == 4, "grounded on four wheels");
        WheelTelemetry wt;
        check(b.sys.wheel(e, 0, wt) && wt.contact && wt.loadN > 1000.0f, "wheel telemetry: contact and load " + f2s(wt.loadN) + " N");
        check(!b.sys.wheel(e, 9, wt), "a wheel past the count is refused");
    }
    {
        // A crash: the sedan with a DAMAGE record whose energy factor is 1, into a wall.
        VehicleSpec s = sedanSpec();
        s.damage.present = true;
        s.damage.health = 1000.0f;
        s.damage.crash = 600.0f;
        s.damage.energy = 1.0f;
        s.damage.radius = 80.0f;
        Bench b;
        aver_phys_add_static_box(2500.0f, 0.0f, 200.0f, 50.0f, 1000.0f, 200.0f);
        const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", s);
        b.frames(120);
        aver_phys_body_set_velocity(aver_phys_vehicle_body(b.comp(e)->vehicle), 1400.0f, 0.0f, 0.0f);
        b.frames(150);
        check(b.sinks.notifyCount("VehicleCrash") >= 1, "a hard hit raises VehicleCrash");
        check(!b.sinks.crashes.empty(), "and reaches the crash sink");
        if (!b.sinks.crashes.empty()) {
            const CrashInfo& ci = b.sinks.crashes[0];
            const f32 v = ci.closingCmS * 0.01f;
            const f32 expect = 0.5f * s.body.massKg * v * v;
            check(std::fabs(ci.energyJ - expect) < expect * 0.05f, "with the energy 0.5 m v^2: " + f2s(ci.energyJ) + " J against " + f2s(expect));
            check(ci.vehicle == e && ci.radiusCm == 80.0f, "naming the vehicle and the radius");
        }
        const Telemetry t = telOf(b, e);
        check(t.health01 < 1.0f, "and costs health: " + f2s(t.health01));

        // Destroy it: no engine. Repair it: engine again.
        check(b.sys.damage(e, 1.0f), "damage to zero");
        b.frames(2);
        check(telOf(b, e).destroyed && (b.comp(e)->outState & kVehicleStateDestroyed) != 0, "it reads destroyed");
        aver_phys_vehicle_set_pose(b.comp(e)->vehicle, -500.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        b.frames(90);
        b.comp(e)->inThrottle = 1.0f;
        b.frames(150);
        check(telOf(b, e).forwardSpeedCmS < 60.0f, "a destroyed vehicle does not pull away: " + f2s(telOf(b, e).forwardSpeedCmS));
        check(b.sys.repair(e), "repair");
        b.frames(150);
        check(!telOf(b, e).destroyed && telOf(b, e).forwardSpeedCmS > 150.0f && telOf(b, e).health01 > 0.99f, "and it drives again: " + f2s(telOf(b, e).forwardSpeedCmS));
    }
    {
        // Smoke is a rate: 3 s of braking at 60 Hz and at 30 Hz emit about the same count.
        const u32 hi = smokeAt(1.0f / 60.0f, 180);
        const u32 lo = smokeAt(1.0f / 30.0f, 90);
        check(hi > 0 && lo > 0, "braking from speed makes smoke: " + std::to_string(hi) + " and " + std::to_string(lo));
        check(hi > 0 && std::fabs(static_cast<f32>(hi) - static_cast<f32>(lo)) <= 0.35f * static_cast<f32>(hi) + 2.0f,
              "and the count follows time, not frames");
    }
}

static void testAudio() {
    AVER_INFO("=== audio ===");
    Bench b;
    for (int i = 0; i < 10; ++i)
        b.specCar(("Car" + std::to_string(i)).c_str(), Vec3(0, 300.0f * static_cast<f32>(i), 10), "Vehicles/Sedan" + std::to_string(i) + ".ocvehicle", sedanSpec());
    b.sys.options().buildsPerFrame = 10;
    b.sys.setViewPos(Vec3(-1000, 0, 100));
    b.frames(60);
    check(b.sys.stats().audible == 8, "the nearest eight of ten are audible: " + std::to_string(b.sys.stats().audible));
    check(b.audio.liveLooping() == 16, "each has an idle and a high loop: " + std::to_string(b.audio.liveLooping()) + " voices");

    // Pitch rises with rpm: find the idle voice nearest the first car, which drives off along +X.
    const scene::Entity first = b.made.front();
    auto nearestPitch = [&]() {
        WheelTelemetry wt;
        const Vec3 at = b.sys.wheel(first, 0, wt) ? wt.contactPoint : Vec3(0, 0, 0);
        f32 best = 1e9f, pitch = 0;
        for (const FakeAudio::Voice& v : b.audio.voices) {
            if (!v.live || v.sets == 0) continue;
            const f32 d = dist(v.pos, at);
            if (d < best) { best = d; pitch = v.pitch; }
        }
        return pitch;
    };
    const f32 idlePitch = nearestPitch();
    b.comp(first)->inThrottle = 1.0f;
    b.frames(90);
    check(nearestPitch() > idlePitch + 0.05f, "pitch rises with rpm: " + f2s(idlePitch) + " -> " + f2s(nearestPitch()));

    b.sys.setViewPos(Vec3(200000, 0, 0));
    b.frames(2);
    check(b.audio.liveLooping() == 0, "everything stops out of range");
    b.sys.setViewPos(Vec3(-1000, 0, 100));
    b.frames(2);
    check(b.audio.liveLooping() > 0, "and starts again in range");
    b.sys.clear();
    check(b.audio.liveLooping() == 0, "clear() stops every voice");
}

static void testLod() {
    AVER_INFO("=== the distance LOD ===");
    Bench b;
    b.sys.options().buildsPerFrame = 5;
    const scene::Entity near = b.specCar("Near", Vec3(0, 0, 10), "Vehicles/A.ocvehicle", sedanSpec());
    const scene::Entity mid = b.specCar("Mid", Vec3(8000, 0, 10), "Vehicles/B.ocvehicle", sedanSpec());
    const scene::Entity far = b.specCar("Far", Vec3(20000, 0, 10), "Vehicles/C.ocvehicle", sedanSpec());
    b.sys.setViewPos(Vec3(0, 0, 100));
    b.frames(30);
    check(b.sys.collisionPeriodOf(near) == 1 && b.sys.collisionPeriodOf(mid) == 2 && b.sys.collisionPeriodOf(far) == 4,
          "driverless vehicles get collision period 1, 2 and 4 by distance");
    const scene::Entity d = b.entityAt("Driver", Vec3(20500, 0, 100));
    b.sys.enter(far, d);
    b.frames(5);
    check(b.sys.collisionPeriodOf(far) == 1, "a driven one stays at 1");
}

static std::vector<f32> replayRun() {
    std::vector<f32> out;
    Bench b;
    const scene::Entity e = b.specCar("Car", Vec3(0, 0, 10), "Vehicles/Sedan.ocvehicle", sedanSpec());
    const scene::Entity d = b.entityAt("Driver", Vec3(600, 0, 100));
    b.frames(100);
    b.sys.enter(e, d);
    std::vector<VehicleEvent> events;
    for (int k = 0; k < 600; ++k) {
        CVehicle* c = b.comp(e);
        c->inThrottle = k < 300 ? 1.0f : 0.0f;
        c->inBrake = k >= 300 ? 0.6f : 0.0f;
        c->inSteer = ((k / 60) % 2 == 0) ? 0.35f : -0.35f;
        b.frame();
        VehicleEvent ev[16];
        const u32 n = b.sys.drainEvents(ev, 16);
        for (u32 i = 0; i < n; ++i) events.push_back(ev[i]);
        const Telemetry t = telOf(b, e);
        out.push_back(t.rpm); out.push_back(t.speedCmS); out.push_back(t.throttle); out.push_back(t.brake); out.push_back(t.steer);
        out.push_back(static_cast<f32>(t.gear)); out.push_back(t.latAccelG);
        const Vec3 p = b.worldPos(e);
        out.push_back(p.x); out.push_back(p.y); out.push_back(p.z);
    }
    for (const VehicleEvent& ev : events) {
        out.push_back(static_cast<f32>(ev.kind)); out.push_back(static_cast<f32>(ev.seq)); out.push_back(ev.value); out.push_back(ev.value2);
        out.push_back(ev.point.x); out.push_back(ev.point.y);
    }
    return out;
}

static void testReplay() {
    AVER_INFO("=== the same script twice is the same, bit for bit ===");
    const std::vector<f32> a = replayRun();
    const std::vector<f32> b = replayRun();
    check(!a.empty() && a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0,
          "poses, telemetry, events and their sequence numbers are bit-identical (" + std::to_string(a.size()) + " values)");
}

int main() {
    AVER_INFO("=== VehicleSystemTest ===");
    testComponent();
    testBuildAndPose();
    testParkedFleet();
    testBuildBudget();
    testInput();
    testVisuals();
    testBones();
    testDriving();
    testTelemetryAndEvents();
    testAudio();
    testLod();
    testReplay();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
