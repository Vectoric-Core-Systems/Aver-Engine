// WorldFlightTest -- the integration test of the floating origin (docs/plans/wave11/LARGE_WORLD.md 5.2, phase 2): a camera flies +X 100 km at
// 200 m/s in 1/60 s steps through the REAL participants (game::OriginParticipants: scene roots, PlayMobility, host members, streaming,
// physics, the renderer's seam, the script event) with a real scene::World, a real Jolt world and a real PCG chunk world generating at 100 km.
//
// It asserts, at each of the six rebases (9.216, 25.6, 41.984, 58.368, 74.752 and 91.136 km): the camera and a ring of tracked entities
// keep their absolute position exactly; PlayMobility keeps 2,000 statics static; the PCG chunk world loads and evicts nothing because of the
// shift (same chunk set, same entity count, same body count); a sleeping crate pile is still asleep and unmoved; a pile in flight keeps its
// velocities; a hinged door anchored to the world, a pulley to the world and a character on a moving deck all hold for two seconds; a car's
// speed is continuous. The per-participant milliseconds of every shift are logged (recorded, not asserted).
//
// WHY A KIT PER SHIFT. A shift is exact only for things within a kilometre of the camera (Sterbenz, LARGE_WORLD 2.2.1 I5): whatever the camera
// leaves behind drifts by half an ULP per shift, by design. So each shift gets its own set of physics content built 40 m ahead of where the
// camera will trigger it, ~20 s before, and torn down after its checks.
//
// Headless: no device, no window. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS

#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/GameStreaming.hpp"
#include "aver/game/OriginParticipants.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_character_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_origin_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/ChunkWorld.hpp"
#include "aver/world/WorldOrigin.hpp"

#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_origin_abi.h"
#include "aver/framework/framework_timers_abi.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kSpeedCmS = 20000.0f;                    // 200 m/s
constexpr f64 kStepCm = 1638400.0;                     // one region
constexpr f64 kTriggerCm = 921600.0;                   // |local| beyond this shifts
constexpr f64 kFlightEndCm = 10000000.0;               // 100 km
constexpr int kShiftsExpected = 6;
constexpr int kSettleFrames = 150;                     // two and a half seconds of checks after each shift, then the kit is torn down

// A device with nothing behind it that records what the renderer participant tells it.
struct RecDev final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    void shiftWorld(const rhi::ShiftWorldInfo& info) override { ++calls; last = info; org = info.newOrigin; }
    rhi::ShiftOrigin worldOrigin() const override { return org; }
    u32 calls = 0;
    rhi::ShiftWorldInfo last{};
    rhi::ShiftOrigin org{};
};

struct P3 { f32 v[3] = {0, 0, 0}; };
P3 bodyPos(i32 b) { P3 p; aver_phys_body_position(b, p.v); return p; }
P3 bodyVel(i32 b) { P3 p; aver_phys_body_velocity(b, p.v); return p; }

f32 localX(f64 absX, const world::OriginCm& o) { return static_cast<f32>(absX - static_cast<f64>(o.x)); }

// ---- the physics kit of one shift --------------------------------------------------------------------------------------

struct Kit {
    int index = 0;
    f64 anchorAbsX = 0.0;           // the kit's centre, absolute cm
    f64 triggerAbsX = 0.0;          // the camera's absolute X at which this shift triggers
    bool built = false, armed = false, tossed = false, shifted = false, done = false;
    int framesSinceShift = 0;

    i32 floorBody = 0, deck = 0, ch = 0, car = 0, door = 0, hinge = 0, hang = 0, pulley = 0;
    std::vector<i32> pile, toss;
    f64 hingeAbsX = 0.0, hingeY = 0.0, hangAbsX = 0.0, hangY = 0.0, hangZ = 0.0;
    f64 doorAbsX = 0.0;
};

struct Frozen {                     // the kit's state in ABSOLUTE terms, so before and after a shift compare directly
    std::vector<f64> pileX; std::vector<f32> pileY, pileZ;
    std::vector<bool> pileActive;
    std::vector<P3> tossVel; std::vector<f64> tossX; std::vector<bool> tossActive;
    f64 doorX = 0; f32 doorY = 0, doorZ = 0;
    f64 hangX = 0; f32 hangY = 0, hangZ = 0;
    f64 chX = 0; f32 chY = 0, chZ = 0;
    f32 carSpeed = 0; f64 carX = 0;
    int chGrounded = 0;
};

i32 buildCar(f32 x, f32 y, f32 z) {
    const f32 hz = 2.0f;
    const f32 w = 2.0f * 3.14159265f * hz;
    const f32 sag = 981.0f / (w * w);
    const f32 attachZ = 33.0f + 30.0f - sag;
    const i32 v = aver_phys_vehicle_create(220.0f, 90.0f, 35.0f, 25.0f, 0.0f, 0.0f, 40.0f, 1300.0f, x, y, z, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        aver_phys_vehicle_add_wheel(v, front ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, 33.0f, 22.0f, 5.0f, 30.0f,
                                    hz, 0.5f, front ? 35.0f : 0.0f, 1500.0f, 2500.0f, front ? 0 : 1);
    }
    aver_phys_vehicle_finish(v, 60.0f);
    return v;
}

int wheelsDown(i32 v) {
    int n = 0;
    for (int i = 0; i < aver_phys_vehicle_wheel_count(v); ++i) if (aver_phys_vehicle_wheel_contact(v, i)) ++n;
    return n;
}

void buildKit(Kit& k, const world::OriginCm& o) {
    const f32 lx = localX(k.anchorAbsX, o);
    k.floorBody = aver_phys_add_static_box(lx, 0.0f, -50.0f, 12000.0f, 12000.0f, 50.0f);
    for (int i = 0; i < 20; ++i)
        k.pile.push_back(aver_phys_add_dynamic_box(lx - 9000.0f + static_cast<f32>(i % 5) * 60.0f, -9000.0f + static_cast<f32>(i / 5) * 60.0f,
                                                   40.0f + static_cast<f32>(i % 3) * 10.0f, 25, 25, 25, 20.0f));
    // A deck, 30 cm above the floor, that moves once armed; a character stands on it.
    k.deck = aver_phys_add_static_box(lx + 3000.0f, 6000.0f, 10.0f, 1500.0f, 3000.0f, 20.0f);
    aver_phys_body_set_motion_type(k.deck, AVER_PHYS_MOTION_KINEMATIC);
    k.ch = aver_phys_character_create(30.0f, 180.0f, lx + 3000.0f, 4500.0f, 125.0f);
    k.car = buildCar(lx - 9000.0f, -3000.0f, 60.0f);
    // A door on a vertical hinge to the world.
    const f32 dx = lx + 8000.0f;
    k.door = aver_phys_add_dynamic_box(dx, -8000.0f, 200.0f, 50, 5, 100, 20.0f);
    const f32 hp[3] = {dx - 50.0f, -8000.0f, 200.0f}, axis[3] = {0, 0, 1}, normal[3] = {1, 0, 0};
    k.hinge = aver_phys_joint_hinge(k.door, AVER_PHYS_WORLD_BODY, hp, axis, normal, -1.5f, 1.5f);
    k.hingeAbsX = static_cast<f64>(hp[0]) + static_cast<f64>(o.x);
    k.hingeY = hp[1];
    // A box hung from a pulley whose far end is bolted to the world.
    const f32 cx = lx + 8000.0f;
    k.hang = aver_phys_add_dynamic_box(cx, 0.0f, 300.0f, 15, 15, 15, 10.0f);
    const f32 bp[3] = {cx, 0, 300}, fp[3] = {cx, 0, 600}, bpW[3] = {cx + 200.0f, 0, 300}, fpW[3] = {cx + 200.0f, 0, 600};
    k.pulley = aver_phys_joint_pulley(k.hang, AVER_PHYS_WORLD_BODY, bp, fp, bpW, fpW, 1.0f, -1.0f, -1.0f);
    k.built = true;
}

void armKit(Kit& k, const world::OriginCm& o) {
    const f32 lx = localX(k.anchorAbsX, o);
    aver_phys_body_move_kinematic(k.deck, lx + 3000.0f, 6300.0f, 10.0f, 0, 0, 0, 1, 1.0f);   // 300 cm/s along +Y
    aver_phys_body_set_angular_velocity(k.door, 0.0f, 0.0f, 1.0f);
    k.armed = true;
}

void tossKit(Kit& k, const world::OriginCm& o) {
    const f32 lx = localX(k.anchorAbsX, o);
    for (int i = 0; i < 6; ++i) {
        const i32 b = aver_phys_add_dynamic_box(lx - 3000.0f + static_cast<f32>(i) * 80.0f, 9000.0f, 60.0f, 25, 25, 25, 20.0f);
        aver_phys_body_set_velocity(b, 0.0f, -600.0f, 150.0f);
        k.toss.push_back(b);
    }
    k.tossed = true;
}

Frozen freeze(const Kit& k, const world::OriginCm& o) {
    Frozen f;
    const f64 ox = static_cast<f64>(o.x);
    for (i32 b : k.pile) { const P3 p = bodyPos(b); f.pileX.push_back(p.v[0] + ox); f.pileY.push_back(p.v[1]); f.pileZ.push_back(p.v[2]); f.pileActive.push_back(aver_phys_body_is_active(b) != 0); }
    for (i32 b : k.toss) { f.tossVel.push_back(bodyVel(b)); f.tossX.push_back(bodyPos(b).v[0] + ox); f.tossActive.push_back(aver_phys_body_is_active(b) != 0); }
    const P3 d = bodyPos(k.door); f.doorX = d.v[0] + ox; f.doorY = d.v[1]; f.doorZ = d.v[2];
    const P3 h = bodyPos(k.hang); f.hangX = h.v[0] + ox; f.hangY = h.v[1]; f.hangZ = h.v[2];
    P3 c; aver_phys_character_position(k.ch, c.v); f.chX = c.v[0] + ox; f.chY = c.v[1]; f.chZ = c.v[2];
    f.chGrounded = aver_phys_character_grounded(k.ch);
    f.carSpeed = aver_phys_vehicle_forward_speed(k.car);
    P3 cp; f32 q[4];
    aver_phys_vehicle_pose(k.car, cp.v, q);
    f.carX = cp.v[0] + ox;
    return f;
}

void teardownKit(Kit& k) {
    aver_phys_joint_remove(k.hinge);
    aver_phys_joint_remove(k.pulley);
    aver_phys_vehicle_destroy(k.car);
    aver_phys_character_destroy(k.ch);
    for (i32 b : k.pile) aver_phys_remove_body(b);
    for (i32 b : k.toss) aver_phys_remove_body(b);
    for (i32 b : {k.hang, k.door, k.deck, k.floorBody}) aver_phys_remove_body(b);
    k.pile.clear(); k.toss.clear();
    k.done = true;
}

// ---- the streaming state --------------------------------------------------------------------------------------------

struct StreamSnap {
    std::vector<world::ChunkCoord> chunks;
    u32 entities = 0;
    usize registered = 0;
    i32 bodies = 0;
};

StreamSnap snapStream(const game::GameStreaming& s) {
    StreamSnap sn;
    if (const world::ChunkWorld* f = s.primaryField()) {
        sn.chunks = f->streamer().residentChunks();
        std::sort(sn.chunks.begin(), sn.chunks.end(), [](const world::ChunkCoord& a, const world::ChunkCoord& b) {
            return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
        });
        sn.registered = f->bodies().size();
        sn.entities = static_cast<u32>(f->streamedEntities().size());
    }
    sn.bodies = aver_phys_body_count();
    return sn;
}

bool sameChunks(const StreamSnap& a, const StreamSnap& b) {
    if (a.chunks.size() != b.chunks.size()) return false;
    for (usize i = 0; i < a.chunks.size(); ++i) if (!(a.chunks[i] == b.chunks[i])) return false;
    return true;
}

#if AVER_MODULE_FRAMEWORK
struct Heard {
    int count = 0;
    i64 dx = 0, dy = 0, gen = 0, ox = 0, oy = 0;
};
Heard g_heard;
int32_t AVER_FW_CALL onOriginEvent(int32_t, int64_t, void*) {
    ++g_heard.count;
    g_heard.dx = aver_fw_event_current_arg_int(0);
    g_heard.dy = aver_fw_event_current_arg_int(1);
    g_heard.gen = aver_fw_event_current_arg_int(2);
    g_heard.ox = aver_fw_event_current_arg_int(3);
    g_heard.oy = aver_fw_event_current_arg_int(4);
    return 0;
}
#endif

} // namespace

int main() {
    AVER_INFO("WorldFlightTest");
    scene::World& w = scene::World::instance();

    // ---- the world: scene content, physics, a PCG chunk world, a project on disk for its region files ----
    {
        std::vector<scene::Entity> all;
        for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
        for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
        w.flush();
    }
    aver_phys_shutdown();
    aver_phys_init();

    const fs::path root = fs::temp_directory_path() / "aver_world_flight_test";
    fs::create_directories(root / "Content" / "Meshes");
    { std::ofstream f(root / "Content" / "Meshes" / "cube.ocmesh", std::ios::binary); f << "x"; }   // the palette check only wants the file to exist

    fmt::ProjectDesc project;
    project.name = "WorldFlight";
    project.dir = root.string();
    project.streamLoadBudget = 8;
    std::vector<fmt::OcPcgVolume> volumes(1);
    volumes[0].name = "Floor";
    volumes[0].seed = 7;
    volumes[0].coverageFloor = 0.45;
    volumes[0].samplesPerAxis = 4;
    volumes[0].radiusChunks = 5;
    std::vector<fmt::OcScatterSpecies> species(1);
    species[0].meshPath = "Meshes/cube.ocmesh";

    RecDev dev;
    game::PlayMobility pm;
    game::GameStreaming streaming;
    game::OriginParticipants op;

    // 2,000 statics near the start: they are left behind by every shift and must never turn Movable.
    std::vector<scene::Entity> scenery;
    for (u32 i = 0; i < 2000; ++i) {
        Transform xf;
        xf.position = Vec3{static_cast<f32>(i % 50) * 80.0f - 2000.0f, static_cast<f32>(i / 50) * 80.0f - 1600.0f, 0.0f};
        scenery.push_back(w.create("scenery", scene::kInvalidEntity, xf));
    }
    // 24 tracked entities that travel with the camera (what a convoy, a squad or a held item does): absolute offsets from the camera.
    std::vector<scene::Entity> tracked;
    std::vector<Vec3> trackedOffset;
    for (u32 i = 0; i < 24; ++i) {
        const f32 a = static_cast<f32>(i) * 0.2617994f;
        tracked.push_back(w.create("tracked", scene::kInvalidEntity, Transform{}));
        trackedOffset.push_back(Vec3{std::cos(a) * 600.0f, std::sin(a) * 600.0f, 30.0f + static_cast<f32>(i)});
    }
    w.flush();

    {   // CONTROL: the same shift without the PlayMobility participant turns every static Movable, so the count below can fail.
        game::PlayMobility pc;
        pc.begin(w);
        for (int pass = 0; pass < 2; ++pass) {   // the first look at an entity takes its baseline
            pc.beginFrame(scene::kInvalidEntity);
            for (const scene::Entity e : scenery) pc.movable(w, e, w.worldMatrix(e));
        }
        game::OriginParticipants ctl;
        Vec3 c{1000000.0f, 0.0f, 100.0f};
        ctl.bindings().positions = {&c};
        ctl.configure(1, -1, true);
        ctl.install();
        const bool did = ctl.update(c);
        w.flush();
        pc.beginFrame(scene::kInvalidEntity);
        int movable = 0;
        for (const scene::Entity e : scenery) if (pc.movable(w, e, w.worldMatrix(e))) ++movable;
        check(did && movable == static_cast<int>(scenery.size()), "(control) without PlayMobility::noteOriginShift all " + std::to_string(movable) + " statics turn Movable at a shift");
        ctl.service().shiftBy(world::OriginCm{-static_cast<i64>(kStepCm), 0});   // back to where the scene was
        w.flush();
        pc.end();
        ctl.resetForLevel();
    }
    pm.begin(w);   // after the control: its shift and shift back bumped every revision
    for (int pass = 0; pass < 2; ++pass) {   // the first look at an entity takes its baseline
        pm.beginFrame(scene::kInvalidEntity);
        for (const scene::Entity e : scenery) pm.movable(w, e, w.worldMatrix(e));
    }

    Vec3 cam{0.0f, 0.0f, 100.0f};
    game::OriginBindings& b = op.bindings();
    b.device = &dev;
    b.mobility = &pm;
    b.streaming = &streaming;
    b.positions = {&cam};
    op.configure(1, -1, true);
    op.install();
    streaming.enable(project, volumes, species, nullptr);
    check(streaming.enabled(), "the PCG chunk world opens (a temp project, one field, one species)");

#if AVER_MODULE_FRAMEWORK
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(&onOriginEvent, nullptr);
    aver_fw_event_subscribe("world.origin_changed", 0, 0, 0, 1);
#endif

    std::vector<Kit> kits(kShiftsExpected);
    for (int i = 0; i < kShiftsExpected; ++i) {
        kits[i].index = i;
        kits[i].triggerAbsX = kTriggerCm + static_cast<f64>(i) * kStepCm;
        kits[i].anchorAbsX = kits[i].triggerAbsX + 4000.0;
    }
    Frozen frozenBefore, frozenAfter;
    StreamSnap streamBefore;
    bool settled = false;
    int watchStatics = 0;
    int shifts = 0, kitInFlight = -1;
    std::vector<f64> shiftAtCamAbs;
    u32 frames = 0;
    f64 worstScenery = 0.0;
    f64 worstTrackedAtShift = 0.0, worstCamAtShift = 0.0, worstPhysAtShift = 0.0;
    f32 worstDoor = 0.0f, worstHang = 0.0f;
    int sceneryMovable = 0, chunkLoadsAtShift = 0, chunkEvictsAtShift = 0, streamMismatch = 0, bodyMismatch = 0, registryMismatch = 0;
    int pileAsleepBefore = 0, pileAsleepAfter = 0, pileAsleepLater = 0, tossMoving = 0, tossVelMismatch = 0, groundedAfter = 0, groundedLater = 0,
        carSpeedMismatch = 0, carWheelsLater = 0, doorHeld = 0, hangHeld = 0;
    std::vector<f64> shiftMs;
    std::vector<std::string> breakdown;

    const u32 physGenerationAtStart = aver_phys_origin_generation();
    const auto t0 = std::chrono::steady_clock::now();
    f64 camAbs = static_cast<f64>(cam.x) + static_cast<f64>(op.origin().x);

    while (camAbs < kFlightEndCm) {
        ++frames;

        // ---- S0: the one place a shift happens ----
        const world::OriginCm oBefore = op.origin();
        const f64 camAbsBefore = static_cast<f64>(cam.x) + static_cast<f64>(oBefore.x);
        std::vector<f64> trackedAbsBefore;
        for (const scene::Entity e : tracked) trackedAbsBefore.push_back(static_cast<f64>(w.localTransform(e).position.x) + static_cast<f64>(oBefore.x));
        const bool hasKit = kitInFlight >= 0 && kits[kitInFlight].built && !kits[kitInFlight].done;
        if (hasKit) frozenBefore = freeze(kits[kitInFlight], oBefore);
        const u32 eventsBefore = static_cast<u32>(shifts);
        (void)eventsBefore;

        const bool shifted = op.update(cam);
        if (shifted) {
            const world::OriginCm oAfter = op.origin();
            ++shifts;
            shiftAtCamAbs.push_back(camAbsBefore);
            const f64 camAbsAfter = static_cast<f64>(cam.x) + static_cast<f64>(oAfter.x);
            worstCamAtShift = std::max(worstCamAtShift, std::fabs(camAbsAfter - camAbsBefore));
            for (usize i = 0; i < tracked.size(); ++i)
                worstTrackedAtShift = std::max(worstTrackedAtShift,
                                               std::fabs(static_cast<f64>(w.localTransform(tracked[i]).position.x) + static_cast<f64>(oAfter.x) - trackedAbsBefore[i]));
            const world::WorldOrigin::Stats& st = op.service().stats();
            shiftMs.push_back(st.lastMs);
            std::string line;
            for (usize i = 0; i < op.participantCount() && i < world::WorldOrigin::kMaxTimedParticipants; ++i) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%s %.3f", op.participantName(i), st.lastParticipantMs[i]);
                line += (i ? ", " : "") + std::string(buf);
            }
            breakdown.push_back(line);
            AVER_INFO("[Flight] shift {} at camera {:.1f} m: {:.3f} ms ({} ms by participant)", shifts, camAbsBefore / 100.0, st.lastMs, line);

            // The chunk world: nothing loads or evicts because of the shift.
            if (settled) {
                const world::StreamStats ss = streaming.tick(cam, kDt);
                const StreamSnap after = snapStream(streaming);
                chunkLoadsAtShift += static_cast<int>(ss.loadedThisUpdate);
                chunkEvictsAtShift += static_cast<int>(ss.evictedThisUpdate);
                if (!sameChunks(streamBefore, after) || streamBefore.entities != after.entities) ++streamMismatch;
                if (streamBefore.bodies != after.bodies) ++bodyMismatch;
                if (streamBefore.registered != after.registered) ++registryMismatch;
                settled = false;
            }

            if (hasKit) {
                Kit& k = kits[kitInFlight];
                frozenAfter = freeze(k, oAfter);
                k.shifted = true;
                k.framesSinceShift = 0;
                // Sleeping pile: asleep before, asleep after, and not moved.
                bool allAsleepBefore = true, allAsleepAfter = true, same = true;
                for (usize i = 0; i < k.pile.size(); ++i) {
                    if (frozenBefore.pileActive[i]) allAsleepBefore = false;
                    if (frozenAfter.pileActive[i]) allAsleepAfter = false;
                    worstPhysAtShift = std::max(worstPhysAtShift, std::fabs(frozenAfter.pileX[i] - frozenBefore.pileX[i]));
                    if (std::fabs(frozenAfter.pileX[i] - frozenBefore.pileX[i]) > 0.2 || frozenAfter.pileY[i] != frozenBefore.pileY[i] ||
                        frozenAfter.pileZ[i] != frozenBefore.pileZ[i])
                        same = false;
                }
                pileAsleepBefore += allAsleepBefore ? 1 : 0;
                pileAsleepAfter += (allAsleepAfter && same) ? 1 : 0;
                // The pile in flight: some of it moving, and every velocity unchanged by the shift.
                bool anyMoving = false, velSame = true;
                for (usize i = 0; i < k.toss.size(); ++i) {
                    if (frozenBefore.tossActive[i]) anyMoving = true;
                    for (int c = 0; c < 3; ++c) if (frozenBefore.tossVel[i].v[c] != frozenAfter.tossVel[i].v[c]) velSame = false;
                    worstPhysAtShift = std::max(worstPhysAtShift, std::fabs(frozenAfter.tossX[i] - frozenBefore.tossX[i]));
                }
                tossMoving += anyMoving ? 1 : 0;
                tossVelMismatch += velSame ? 0 : 1;
                // Door, hang, character and car land at the same absolute place.
                worstPhysAtShift = std::max({worstPhysAtShift, std::fabs(frozenAfter.doorX - frozenBefore.doorX), std::fabs(frozenAfter.hangX - frozenBefore.hangX),
                                             std::fabs(frozenAfter.chX - frozenBefore.chX), std::fabs(frozenAfter.carX - frozenBefore.carX)});
                groundedAfter += frozenAfter.chGrounded == 1 ? 1 : 0;
                carSpeedMismatch += frozenAfter.carSpeed == frozenBefore.carSpeed ? 0 : 1;
                k.hangAbsX = frozenAfter.hangX; k.hangY = frozenAfter.hangY; k.hangZ = frozenAfter.hangZ;
                if (frozenBefore.carSpeed < 100.0f) AVER_WARN("[Flight] kit {}: the car was only doing {} cm/s at the shift", k.index, frozenBefore.carSpeed);
            }
        }

        // ---- the camera flies; the tracked entities follow it in absolute terms ----
        cam.x += kSpeedCmS * kDt;
        const world::OriginCm o = op.origin();
        camAbs = static_cast<f64>(cam.x) + static_cast<f64>(o.x);
        for (usize i = 0; i < tracked.size(); ++i) {
            const f64 ax = camAbs + static_cast<f64>(trackedOffset[i].x);
            w.setLocalPosition(tracked[i], Vec3{static_cast<f32>(ax - static_cast<f64>(o.x)), trackedOffset[i].y, trackedOffset[i].z});
        }

        // ---- the kit of the next shift: built ~4 km before the camera reaches its trigger, armed 5 s and tossed 0.5 s before ----
        if (kitInFlight < 0 || kits[kitInFlight].done) {
            for (int i = 0; i < kShiftsExpected; ++i) {
                if (!kits[i].built && camAbs >= kits[i].triggerAbsX - 400000.0) { kitInFlight = i; buildKit(kits[i], o); break; }
            }
        }
        if (kitInFlight >= 0) {
            Kit& k = kits[kitInFlight];
            if (k.built && !k.done) {
                if (!k.armed && camAbs >= k.triggerAbsX - 100000.0) armKit(k, o);
                if (k.armed) aver_phys_vehicle_set_input(k.car, 1.0f, 0.0f, 0.0f, 0.0f);
                if (!k.tossed && camAbs >= k.triggerAbsX - 6000.0) tossKit(k, o);
            }
        }

        aver_phys_step(kDt);

        // ---- after the shift: two and a half seconds of watching the kit, then it goes ----
        if (kitInFlight >= 0 && kits[kitInFlight].shifted && !kits[kitInFlight].done) {
            Kit& k = kits[kitInFlight];
            ++k.framesSinceShift;
            const f64 ox = static_cast<f64>(o.x);
            const P3 d = bodyPos(k.door);
            const f32 hx = static_cast<f32>(k.hingeAbsX - ox);
            const f32 r = std::sqrt((d.v[0] - hx) * (d.v[0] - hx) + (d.v[1] - static_cast<f32>(k.hingeY)) * (d.v[1] - static_cast<f32>(k.hingeY)));
            worstDoor = std::max({worstDoor, std::fabs(r - 50.0f), std::fabs(d.v[2] - 200.0f)});
            const P3 h = bodyPos(k.hang);
            const f32 ex = static_cast<f32>(k.hangAbsX - ox);
            const f32 ey = static_cast<f32>(k.hangY), ez = static_cast<f32>(k.hangZ);
            worstHang = std::max(worstHang, std::sqrt((h.v[0] - ex) * (h.v[0] - ex) + (h.v[1] - ey) * (h.v[1] - ey) + (h.v[2] - ez) * (h.v[2] - ez)));
            if (k.framesSinceShift == 120) {
                bool asleep = true;
                for (i32 pb : k.pile) if (aver_phys_body_is_active(pb)) asleep = false;
                pileAsleepLater += asleep ? 1 : 0;
                groundedLater += aver_phys_character_grounded(k.ch) == 1 ? 1 : 0;
                carWheelsLater += wheelsDown(k.car) == 4 ? 1 : 0;
            }
            if (k.framesSinceShift >= kSettleFrames) {
                doorHeld += worstDoor < 1.5f ? 1 : 0;
                hangHeld += worstHang < 2.5f ? 1 : 0;
                worstDoor = 0.0f; worstHang = 0.0f;
                teardownKit(k);
                kitInFlight = -1;
            }
        }

        // ---- streaming, like the game: after physics ----
        streaming.tick(cam, kDt);

        w.flush();
        pm.beginFrame(scene::kInvalidEntity);
        if (shifted) watchStatics = 4;
        if (watchStatics > 0) {   // the shift frame and the three after it
            --watchStatics;
            int movable = 0;
            for (const scene::Entity e : scenery) if (pm.movable(w, e, w.worldMatrix(e))) ++movable;
            sceneryMovable += movable;
        }

#if AVER_MODULE_FRAMEWORK
        aver_fw_events_flush();
#endif

        // The camera is past the limit: the next frame's S0 will shift. Let the chunk world finish what it owes first, so the
        // comparison across the shift measures the shift and not the backlog (the camera does not move here).
        if (!settled && static_cast<f64>(cam.x) > kTriggerCm) {
            for (int i = 0; i < 400; ++i) {
                const world::StreamStats ss = streaming.tick(cam, kDt);
                if (ss.pendingLoads == 0 && ss.loadedThisUpdate == 0 && ss.evictedThisUpdate == 0) break;
            }
            streamBefore = snapStream(streaming);
            settled = true;
        }
        if (frames % 6000 == 0)
            AVER_INFO("[Flight] frame {}: camera at {:.1f} km, origin ({}, {}), {} shifts, {} chunks, {} entities", frames, camAbs / 100000.0, op.origin().x, op.origin().y,
                      shifts, streaming.stats().residentChunks, streaming.stats().residentEntities);
    }

    const f64 wallS = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
    AVER_INFO("[Flight] {} frames ({:.1f} s of flight) in {:.1f} s of wall time", frames, static_cast<f64>(frames) / 60.0, wallS);

    // ---- the verdict ----
    check(shifts == kShiftsExpected, "six shifts over a 100 km flight (" + std::to_string(shifts) + ")");
    if (static_cast<int>(shiftAtCamAbs.size()) == kShiftsExpected) {
        bool ok = true;
        for (int i = 0; i < kShiftsExpected; ++i) {
            const f64 want = kTriggerCm + static_cast<f64>(i) * kStepCm;
            // One frame of lag: the shift runs at the top of the frame AFTER the camera crossed the limit (333 cm a frame).
            if (!(shiftAtCamAbs[i] > want && shiftAtCamAbs[i] <= want + 340.0)) { ok = false; AVER_ERROR("   shift {} at {:.2f} cm, wanted just past {:.2f}", i + 1, shiftAtCamAbs[i], want); }
        }
        check(ok, "at 9.216, 25.6, 41.984, 58.368, 74.752 and 91.136 km (within the one frame of lag)");
    }
    check(op.origin() == world::OriginCm{6 * static_cast<i64>(kStepCm), 0} && op.generation() == 6, "the origin ends six regions along +X, generation 6");
    check(dev.calls == 6 && dev.last.generation == 6 && dev.org.x == op.origin().x, "the renderer was told six times and holds the final origin");
    check(worstCamAtShift == 0.0, "the camera keeps its absolute position through every shift, exactly");
    check(worstTrackedAtShift == 0.0, "so do 24 tracked entities near it (exact within the margin: worst " + std::to_string(worstTrackedAtShift) + " cm)");
    check(sceneryMovable == 0, "PlayMobility: 2,000 statics stayed Static through all six shifts and flushes (" + std::to_string(sceneryMovable) + " turned Movable)");
    {
        f64 worst = 0.0;
        for (usize i = 0; i < scenery.size(); ++i) {
            const Vec3 p = w.localTransform(scenery[i]).position;
            const f64 absX = static_cast<f64>(p.x) + static_cast<f64>(op.origin().x);
            const f64 wantX = static_cast<f64>(static_cast<f32>(i % 50) * 80.0f - 2000.0f);
            worst = std::max(worst, std::fabs(absX - wantX));
        }
        worstScenery = worst;
        check(worstScenery < 6.0, "scenery left 100 km behind drifted by half an ULP per shift at most (" + std::to_string(worstScenery) + " cm after six shifts)");
    }
    check(chunkLoadsAtShift == 0 && chunkEvictsAtShift == 0, "the PCG chunk world loaded " + std::to_string(chunkLoadsAtShift) + " and evicted " + std::to_string(chunkEvictsAtShift) + " chunks because of the shifts");
    check(streamMismatch == 0, "the resident chunk set and entity count were identical immediately before and after every shift (" + std::to_string(streamMismatch) + " differed)");
    check(bodyMismatch == 0 && registryMismatch == 0, "physics body count and the BodyRegistry count did not change across any shift");
    check(streaming.stats().residentChunks > 0 && streaming.stats().residentEntities > 0, "and the world was streaming at the end (" + std::to_string(streaming.stats().residentChunks) + " chunks, " +
                                                                                                 std::to_string(streaming.stats().residentEntities) + " entities) 100 km out");
    check(pileAsleepBefore == 6 && pileAsleepAfter == 6, "the sleeping crate pile was asleep at every shift and still asleep and unmoved right after (" + std::to_string(pileAsleepBefore) + ", " + std::to_string(pileAsleepAfter) + " of 6)");
    check(pileAsleepLater == 6, "and still asleep two seconds later");
    check(tossMoving == 6 && tossVelMismatch == 0, "the pile in flight was moving at every shift and kept its velocities exactly (" + std::to_string(tossMoving) + " moving, " + std::to_string(tossVelMismatch) + " changed)");
    check(worstPhysAtShift < 0.2, "every body, the character and the car landed at the same absolute place (worst " + std::to_string(worstPhysAtShift) + " cm)");
    check(doorHeld == 6, "the hinged door stayed on its hinge for 2.5 s after each shift (" + std::to_string(doorHeld) + " of 6)");
    check(hangHeld == 6, "the box on the world pulley stayed where it hung (" + std::to_string(hangHeld) + " of 6)");
    check(groundedAfter == 6 && groundedLater == 6, "the character on the moving deck was grounded right after and two seconds after each shift");
    check(carSpeedMismatch == 0 && carWheelsLater == 6, "the car's speed was identical across every shift and it kept four wheels down (" + std::to_string(carWheelsLater) + " of 6)");
    check(aver_phys_origin_generation() == physGenerationAtStart + 6, "physics counted six shifts");
    double tot[3] = {0, 0, 0};
    aver_phys_origin_total(tot);
    check(tot[0] == 6.0 * kStepCm && tot[1] == 0.0, "and its total is six regions");

#if AVER_MODULE_FRAMEWORK
    check(g_heard.count == 6 && g_heard.dx == static_cast<i64>(kStepCm) && g_heard.dy == 0 && g_heard.gen == 6 && g_heard.ox == 6 * static_cast<i64>(kStepCm),
          "the script saw world.origin_changed six times; the last carried the shift, the generation and the origin (" + std::to_string(g_heard.count) + ")");
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(nullptr, nullptr);
#endif

    // Recorded, not asserted.
    f64 worstMs = 0.0, sumMs = 0.0;
    for (f64 ms : shiftMs) { worstMs = std::max(worstMs, ms); sumMs += ms; }
    AVER_INFO("[Flight] shift cost: mean {:.3f} ms, worst {:.3f} ms over {} shifts (Debug build, ~1,000 statics, one kit)", shiftMs.empty() ? 0.0 : sumMs / static_cast<f64>(shiftMs.size()), worstMs, shiftMs.size());
    for (usize i = 0; i < breakdown.size(); ++i) AVER_INFO("[Flight]   shift {}: {}", i + 1, breakdown[i]);

    pm.end();
    streaming.disable();
    aver_phys_shutdown();
    op.resetForLevel();
    {
        std::vector<scene::Entity> all;
        for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
        for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
        w.flush();
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}

#else

int main() {
    AVER_INFO("WorldFlightTest: skipped (needs the scene and physics modules)");
    return 0;
}

#endif
