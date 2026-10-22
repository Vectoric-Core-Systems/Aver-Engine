// PhysicsStateTest -- snapshots, restore, exact stepping and the state hash (physics_state_abi.h), through the ABI only.
//
//   * round trip: a scene of 50 boxes, a car, a character, a hanging weight on a joint, a cloth and a ragdoll is captured,
//     run 120 steps recording the state hash every step, restored, run 120 again: EVERY per-step hash is equal,
//   * validation first: a captured body removed, a joint or a vehicle added or removed, or a body added under STRICT, refuses the
//     restore with TOPOLOGY and the world's hash does not move; a body added without STRICT is left alone,
//   * the engine section: a car braking with ABS and traction control replays identically from a snapshot, a character riding a
//     moving deck keeps riding, the accumulator comes back, events are cleared,
//   * the blob: reuse overwrites in place, export / import into a fresh world of the same build restores to the same hash, a flipped
//     precision byte is BUILD_MISMATCH, a truncated blob BAD_HEADER, a flipped payload byte CORRUPT, statics stay out,
//   * step_exact runs exactly n steps and leaves the accumulator alone; a broken joint un-breaks; cloth vertices come back.
//
// Hand-run, exit code = failure count, same shape as PhysicsConfigTest.cpp. Tolerances marked UNVERIFIED are tuned at the first
// headed run (design section 6); every equality of state hashes is exact by design.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_ragdoll_abi.h"
#include "aver/physics/physics_state_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

static const float kDt = 1.0f / 60.0f;
static uint64_t H() { return aver_phys_state_hash(0); }
static void stepN(int n) { while (n > 0) { const int k = n > 600 ? 600 : n; aver_phys_step_exact(k); n -= k; } }
static std::string hex(uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b; }

// ---- scenes ------------------------------------------------------------------------------------------------------------

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 5000.0f, 5000.0f, 50.0f);   // the floor, top at z = 0
}

static void driverAids(int32_t v, bool abs, bool tcs) {
    const float d[AVER_PHYS_VD_COUNT] = {1, abs ? 1.0f : 0.0f, 0.25f, 8, tcs ? 1.0f : 0.0f, 0.35f, 3, 5, 0, 6, 8, 8, 1, 50, 0.3f, 30, 0.45f, 12};
    aver_phys_vehicle_set_assists(v, d, AVER_PHYS_VD_COUNT);
}

// A 440 x 180 cm saloon with four wheels, as VehicleTest builds it; the aids on, so the engine section has something to hold.
static int32_t buildCar(float x, float y, float z) {
    const float kSpringHz = 2.0f;
    const float kSag = 981.0f / ((2.0f * kPi * kSpringHz) * (2.0f * kPi * kSpringHz));
    const float attachZ = 33.0f + 30.0f - kSag;
    const int32_t v = aver_phys_vehicle_create(220.0f, 90.0f, 35.0f, 25.0f, 0.0f, 0.0f, 40.0f, 1300.0f, x, y, z, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        aver_phys_vehicle_add_wheel(v, front ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, 33.0f, 22.0f, 5.0f, 30.0f,
                                    kSpringHz, 0.5f, front ? 35.0f : 0.0f, 1500.0f, 2500.0f, front ? 0 : 1);
    }
    driverAids(v, true, true);
    aver_phys_vehicle_finish(v, 60.0f);
    return v;
}

struct Chain {
    int n = 0;
    std::vector<int32_t> parents;
    std::vector<float> pos, rot, caps, lim;
    void addBone(int parent, float x, float y, float z, float len, float radius) {
        parents.push_back(parent);
        pos.insert(pos.end(), {x, y, z});
        rot.insert(rot.end(), {0.0f, 0.0f, 0.0f, 1.0f});
        caps.insert(caps.end(), {0.0f, 0.0f, 0.0f, len, 0.0f, 0.0f, radius});
        lim.insert(lim.end(), {80.0f, 80.0f, 30.0f});
        ++n;
    }
};

struct Grid {
    std::vector<float> verts;
    std::vector<int32_t> indices;
    int32_t n = 0;
    int32_t count() const { return n * n; }
    int32_t at(int32_t ix, int32_t iy) const { return iy * n + ix; }
};

static Grid makeGrid(int32_t n, float spacingCm, float z) {
    Grid g;
    g.n = n;
    const float half = (n - 1) * spacingCm * 0.5f;
    for (int32_t iy = 0; iy < n; ++iy)
        for (int32_t ix = 0; ix < n; ++ix) {
            g.verts.push_back(ix * spacingCm - half);
            g.verts.push_back(iy * spacingCm - half);
            g.verts.push_back(z);
        }
    for (int32_t iy = 0; iy + 1 < n; ++iy)
        for (int32_t ix = 0; ix + 1 < n; ++ix) {
            const int32_t a = g.at(ix, iy), b = g.at(ix + 1, iy), c = g.at(ix + 1, iy + 1), d = g.at(ix, iy + 1);
            g.indices.insert(g.indices.end(), {a, b, c});
            g.indices.insert(g.indices.end(), {a, c, d});
        }
    return g;
}

struct Scene {
    std::vector<int32_t> boxes;
    int32_t car = 0, carBody = 0, ch = 0, joint = 0, weight = 0, cloth = 0, ragdoll = 0;
    int32_t clothVerts = 0;
};

// Fifty boxes in two layers, a car under throttle, a character walking, a weight hung on a (strong) joint, a pinned cloth and a
// limp three-bone ragdoll. `constraints` false leaves out the joint and the ragdoll (the statics test wants a lighter world).
// Handles of every kind restart with the world, so building the same scene twice gives the same handles and the same hash.
static Scene buildScene(bool constraints) {
    freshWorld();
    Scene s;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            for (int k = 0; k < 2; ++k)
                s.boxes.push_back(aver_phys_add_dynamic_box(static_cast<float>(i) * 45.0f, static_cast<float>(j) * 45.0f,
                                                            12.0f + static_cast<float>(k) * 30.0f + static_cast<float>((i + j) % 2) * 2.0f,
                                                            10.0f, 10.0f, 10.0f, 5.0f));
    s.car = buildCar(-1500.0f, 0.0f, 5.0f);
    s.carBody = aver_phys_vehicle_body(s.car);
    aver_phys_vehicle_set_input(s.car, 0.6f, 0.1f, 0.0f, 0.0f);
    s.ch = aver_phys_character_create(30.0f, 170.0f, 800.0f, 0.0f, 100.0f);
    aver_phys_character_set_velocity(s.ch, 150.0f, 40.0f, 0.0f);

    const Grid g = makeGrid(7, 14.0f, 300.0f);
    std::vector<float> invMass(static_cast<size_t>(g.count()), 1.0f);
    for (int32_t ix = 0; ix < g.n; ++ix) invMass[static_cast<size_t>(g.at(ix, 0))] = 0.0f;   // the top edge is pinned
    s.cloth = aver_phys_softbody_create(g.verts.data(), g.count(), g.indices.data(), static_cast<int32_t>(g.indices.size()),
                                        invMass.data(), 0.0f, 900.0f, 0.0f, 0.0f, 0.0f);
    s.clothVerts = g.count();

    if (constraints) {
        s.weight = aver_phys_add_dynamic_box(0.0f, -800.0f, 400.0f, 10.0f, 10.0f, 10.0f, 10.0f);
        const float pivot[3] = {0.0f, -800.0f, 500.0f};
        s.joint = aver_phys_joint_point(s.weight, AVER_PHYS_WORLD_BODY, pivot);
        aver_phys_joint_set_break(s.joint, 5000.0f, 0.0f);   // far above the 98 N it carries: it stays whole
        Chain c;
        for (int i = 0; i < 3; ++i) c.addBone(i - 1, 1500.0f + 50.0f * static_cast<float>(i), 600.0f, 300.0f, 50.0f, 8.0f);
        s.ragdoll = aver_phys_ragdoll_create(c.n, c.parents.data(), c.pos.data(), c.rot.data(), c.caps.data(), c.lim.data(), 30.0f, 0);
        aver_phys_ragdoll_set_mode(s.ragdoll, AVER_PHYS_RAGDOLL_LIMP);
    }
    return s;
}

// ---- helpers over the state ABI ----------------------------------------------------------------------------------------

static void expectRestoreFails(int32_t snap, uint32_t options, int32_t want, const std::string& what) {
    const uint64_t before = H();
    const int32_t ok = aver_phys_snapshot_restore(snap, options);
    const int32_t result = aver_phys_snapshot_last_result();
    check(ok == 0 && result == want && H() == before,
          what + " (returned " + std::to_string(ok) + ", result " + std::to_string(result) + ", hash " +
              (H() == before ? "unchanged" : "CHANGED") + ")");
}

static std::vector<uint64_t> runHashes(int steps) {
    std::vector<uint64_t> h;
    h.reserve(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) { aver_phys_step_exact(1); h.push_back(H()); }
    return h;
}

// Index of the first differing entry, or -1.
static int firstDiff(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    if (a.size() != b.size()) return 0;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return static_cast<int>(i);
    return -1;
}

// ---- 1. the round trip --------------------------------------------------------------------------------------------------

static void testRoundTrip() {
    AVER_INFO("=== capture, run, restore, run again: every per-step hash equal ===");
    Scene s = buildScene(true);
    stepN(120);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    check(snap != 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_OK, "a capture of the whole scene succeeds");
    const uint64_t hAtCapture = H();
    check(aver_phys_snapshot_hash(snap) == hAtCapture, "the snapshot's hash equals the state hash taken at the same instant");

    float p0[3] = {}, p1[3] = {};
    aver_phys_body_position(s.carBody, p0);
    const std::vector<uint64_t> first = runHashes(120);
    aver_phys_body_position(s.carBody, p1);
    check(std::fabs(p1[0] - p0[0]) > 50.0f, "the car drove on in the 120 steps (so the replay has something to disagree about)");
    check(H() != hAtCapture, "the state moved");

    check(aver_phys_snapshot_restore(snap, 0) == 1, "restore succeeds");
    check(H() == hAtCapture, "straight after the restore the hash is the captured one");
    check(aver_phys_step_total() == 120, "and the step count is the capture's, " + std::to_string(aver_phys_step_total()));
    const std::vector<uint64_t> second = runHashes(120);
    const int d = firstDiff(first, second);
    check(d < 0, d < 0 ? "all 120 per-step hashes equal after the restore" : "the hashes first differ at step " + std::to_string(d));
    aver_phys_body_position(s.carBody, p0);
    check(std::fabs(p0[0] - p1[0]) < 0.001f && std::fabs(p0[1] - p1[1]) < 0.001f, "and the car is in the same place");

    // Restoring twice in a row from the same snapshot is the same restore.
    check(aver_phys_snapshot_restore(snap, 0) == 1 && H() == hAtCapture, "a second restore from the same snapshot lands on the same state");
    aver_phys_snapshot_release(snap);
}

// ---- 2. validation first ------------------------------------------------------------------------------------------------

static void testValidation() {
    AVER_INFO("=== validation comes first: nothing is applied to a world that no longer matches ===");
    {
        Scene s = buildScene(true);
        stepN(60);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        aver_phys_remove_body(s.boxes[3]);
        expectRestoreFails(snap, 0, AVER_PHYS_RESTORE_TOPOLOGY, "a captured body removed: TOPOLOGY, world untouched");
    }
    {
        Scene s = buildScene(true);
        stepN(60);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        stepN(30);
        const int32_t extra = aver_phys_add_dynamic_box(900.0f, 900.0f, 100.0f, 10, 10, 10, 5.0f);
        float before[3] = {}, after[3] = {};
        aver_phys_body_position(extra, before);
        check(aver_phys_snapshot_restore(snap, 0) == 1, "a body ADDED since the capture does not stop the restore");
        aver_phys_body_position(extra, after);
        check(before[0] == after[0] && before[1] == after[1] && before[2] == after[2], "and the added body keeps its state");
        check(aver_phys_step_total() == 60, "the rest of the world went back to step 60");
        expectRestoreFails(snap, AVER_PHYS_RESTORE_STRICT, AVER_PHYS_RESTORE_TOPOLOGY, "under STRICT the added body refuses the restore");
        aver_phys_remove_body(extra);
        check(aver_phys_snapshot_restore(snap, AVER_PHYS_RESTORE_STRICT) == 1, "with the body gone STRICT passes");
    }
}

// ---- 3. constraint epoch ------------------------------------------------------------------------------------------------

static void testConstraintTopology() {
    AVER_INFO("=== any constraint added or removed since the capture refuses the restore ===");
    {
        Scene s = buildScene(true);
        stepN(30);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        const float pt[3] = {0.0f, 0.0f, 20.0f};
        const int32_t j = aver_phys_joint_point(s.boxes[0], s.boxes[2], pt);
        check(j != 0, "a joint was created after the capture");
        expectRestoreFails(snap, 0, AVER_PHYS_RESTORE_TOPOLOGY, "a joint added since: TOPOLOGY");
        aver_phys_joint_remove(j);
        expectRestoreFails(snap, 0, AVER_PHYS_RESTORE_TOPOLOGY, "removing it again does not undo the epoch: still TOPOLOGY (Jolt restores constraints by index)");
        const int32_t fresh = aver_phys_snapshot_capture(0, 0);
        check(fresh != 0 && aver_phys_snapshot_restore(fresh, 0) == 1, "a snapshot taken now restores fine");
    }
    {
        Scene s = buildScene(true);
        stepN(30);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        check(aver_phys_vehicle_destroy(s.car) == 1, "the car was destroyed after the capture");
        expectRestoreFails(snap, 0, AVER_PHYS_RESTORE_TOPOLOGY, "a vehicle removed since: TOPOLOGY");
    }
    {
        Scene s = buildScene(true);
        stepN(30);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        check(aver_phys_ragdoll_destroy(s.ragdoll) == 1, "the ragdoll was destroyed after the capture");
        expectRestoreFails(snap, 0, AVER_PHYS_RESTORE_TOPOLOGY, "a ragdoll removed since: TOPOLOGY");
    }
    {
        Scene s = buildScene(true);
        stepN(30);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        aver_phys_character_destroy(s.ch);
        expectRestoreFails(snap, 0, AVER_PHYS_RESTORE_TOPOLOGY, "a character removed since: TOPOLOGY");
    }
}

// ---- 4. vehicle driver-aid state ---------------------------------------------------------------------------------------

static void testVehicleAidState() {
    AVER_INFO("=== a car mid-ABS-brake replays identically from a snapshot ===");
    freshWorld();
    const int32_t car = buildCar(0.0f, 0.0f, 5.0f);
    const int32_t body = aver_phys_vehicle_body(car);
    stepN(60);
    aver_phys_body_set_velocity(body, 2500.0f, 0.0f, 0.0f);
    aver_phys_vehicle_set_input(car, 0.0f, 0.0f, 1.0f, 0.0f);   // full brake at 25 m/s
    stepN(10);

    float vsCaptured[AVER_PHYS_VS_COUNT] = {};
    int32_t vsiCaptured[AVER_PHYS_VSI_COUNT] = {};
    aver_phys_vehicle_state(car, vsCaptured, AVER_PHYS_VS_COUNT, vsiCaptured, AVER_PHYS_VSI_COUNT);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    auto record = [&](std::vector<uint64_t>& hashes, std::vector<int32_t>& abs) {
        for (int i = 0; i < 40; ++i) {
            aver_phys_step_exact(1);
            hashes.push_back(H());
            float f[AVER_PHYS_VS_COUNT];
            int32_t iv[AVER_PHYS_VSI_COUNT];
            aver_phys_vehicle_state(car, f, AVER_PHYS_VS_COUNT, iv, AVER_PHYS_VSI_COUNT);
            abs.push_back(iv[AVER_PHYS_VSI_ABS_ACTIVE]);
        }
    };
    std::vector<uint64_t> h1, h2;
    std::vector<int32_t> a1, a2;
    record(h1, a1);
    int engaged = 0;
    for (const int32_t a : a1) engaged += a != 0 ? 1 : 0;
    AVER_INFO("   note  ABS was active in {} of 40 braking steps (UNVERIFIED threshold; the equality below holds either way)", engaged);

    check(aver_phys_snapshot_restore(snap, 0) == 1, "restore the car mid-brake");
    {
        // The telemetry the car had at the capture comes back with it: the next step's ABS and traction control read the last step's slip from it.
        float vs[AVER_PHYS_VS_COUNT] = {};
        int32_t vsi[AVER_PHYS_VSI_COUNT] = {};
        aver_phys_vehicle_state(car, vs, AVER_PHYS_VS_COUNT, vsi, AVER_PHYS_VSI_COUNT);
        check(std::memcmp(vs, vsCaptured, sizeof vs) == 0 && std::memcmp(vsi, vsiCaptured, sizeof vsi) == 0,
              "straight after the restore the vehicle's telemetry is the captured telemetry");
    }
    record(h2, a2);
    const int d = firstDiff(h1, h2);
    check(d < 0, d < 0 ? "the state hash is equal at every one of the 40 steps" : "the hashes first differ at step " + std::to_string(d));
    check(a1 == a2, "and the ABS flag follows the same steps through the vehicle state");
}

// ---- 5. a character riding a deck --------------------------------------------------------------------------------------

static void testCharacterRidesAfterRestore() {
    AVER_INFO("=== a character on a moving deck keeps riding after a restore ===");
    freshWorld();
    const int32_t deck = aver_phys_add_dynamic_box(0.0f, 0.0f, 190.0f, 300.0f, 300.0f, 10.0f, 1000.0f);
    aver_phys_body_set_motion_type(deck, AVER_PHYS_MOTION_KINEMATIC);
    const int32_t rider = aver_phys_character_create(30.0f, 180.0f, 0.0f, 0.0f, 291.0f);
    float deck0[3] = {};
    aver_phys_body_position(deck, deck0);
    const int64_t t0 = aver_phys_step_total();

    // The inputs of one tick, from the tick number alone: what a rollback layer re-issues for each resimulated step.
    auto tick = [&]() {
        const float x = deck0[0] + 400.0f * kDt * static_cast<float>(aver_phys_step_total() - t0);
        aver_phys_body_move_kinematic(deck, x, deck0[1], deck0[2], 0.0f, 0.0f, 0.0f, 1.0f, kDt);
        float v[3] = {};
        aver_phys_character_velocity(rider, v);
        aver_phys_character_set_velocity(rider, 0.0f, 0.0f, v[2]);
        aver_phys_step_exact(1);
    };
    for (int i = 0; i < 90; ++i) tick();
    check(aver_phys_character_grounded(rider) == 1, "the rider stands on the deck");

    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    std::vector<uint64_t> h1, h2;
    float r1[3] = {}, r2[3] = {}, d1[3] = {};
    for (int i = 0; i < 90; ++i) { tick(); h1.push_back(H()); }
    aver_phys_character_position(rider, r1);
    aver_phys_body_position(deck, d1);
    check(std::fabs((r1[0] - d1[0])) < 40.0f, "after 90 more ticks the rider is still over the deck's centre, dx = " + std::to_string(r1[0] - d1[0]));

    check(aver_phys_snapshot_restore(snap, 0) == 1, "restore");
    for (int i = 0; i < 90; ++i) { tick(); h2.push_back(H()); }
    aver_phys_character_position(rider, r2);
    check(firstDiff(h1, h2) < 0, "the resimulated 90 ticks are hash-equal");
    check(r1[0] == r2[0] && r1[1] == r2[1] && r1[2] == r2[2], "and the rider ends exactly where it did");
}

// ---- 6. events ---------------------------------------------------------------------------------------------------------

static void testEventsCleared() {
    AVER_INFO("=== a restore clears the event queues ===");
    freshWorld();
    aver_phys_add_dynamic_box(0.0f, 0.0f, 40.0f, 10.0f, 10.0f, 10.0f, 5.0f);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    bool seen = false;
    for (int i = 0; i < 120 && !seen; ++i) { aver_phys_step(kDt); seen = aver_phys_contact_count() > 0; }
    check(seen, "the box landed and reported a contact");
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restore");
    check(aver_phys_contact_count() == 0 && aver_phys_overlap_count() == 0, "the contact and overlap queues are empty");
}

// ---- 7. reuse ----------------------------------------------------------------------------------------------------------

static void testReuse() {
    AVER_INFO("=== reuse overwrites in place ===");
    buildScene(true);
    stepN(30);
    const int32_t a = aver_phys_snapshot_capture(0, 0);
    const int32_t bytes = aver_phys_snapshot_bytes(a);
    check(a != 0 && bytes > 64, "a snapshot has bytes: " + std::to_string(bytes));
    stepN(20);
    const int32_t again = aver_phys_snapshot_capture(a, 0);
    check(again == a, "capturing into a live handle returns the same handle");
    check(aver_phys_snapshot_count() == 1, "and does not add a second snapshot");
    check(aver_phys_snapshot_hash(a) == H(), "it now holds the newer state");
    // The size follows the world, not the handle: Jolt saves only awake bodies' motion, and the boxes fell asleep between the two
    // captures (20101 bytes at step 30, 11003 at step 50). So the reused snapshot must be as big as a fresh capture of the same state.
    const int32_t fresh = aver_phys_snapshot_capture(0, 0);
    check(fresh != 0 && fresh != a && aver_phys_snapshot_bytes(a) == aver_phys_snapshot_bytes(fresh),
          "of the size a fresh capture of that state has, " + std::to_string(aver_phys_snapshot_bytes(a)));
    aver_phys_snapshot_release(fresh);
    const int32_t other = aver_phys_snapshot_capture(0, 0);
    check(other != 0 && other != a && aver_phys_snapshot_count() == 2, "a capture with reuse 0 is a new snapshot");
    check(aver_phys_snapshot_release(a) == 1 && aver_phys_snapshot_release(a) == 0, "release works once");
    check(aver_phys_snapshot_bytes(a) == 0 && aver_phys_snapshot_hash(a) == 0, "a released handle reads as dead");
    const int32_t revived = aver_phys_snapshot_capture(a, 0);
    check(revived != 0 && revived != a, "capturing into a dead handle allocates a new one");
    const int32_t live = aver_phys_snapshot_count();
    check(live >= 2 && aver_phys_snapshot_release_all() == live, "release_all returns how many it dropped (a level unload calls it)");
    check(aver_phys_snapshot_count() == 0 && aver_phys_snapshot_bytes(other) == 0 && aver_phys_snapshot_release_all() == 0,
          "and nothing is left: every handle reads as dead, a second call drops none");
}

// ---- 8. transport ------------------------------------------------------------------------------------------------------

static void testTransport() {
    AVER_INFO("=== export, import into a fresh world of the same build, restore ===");
    buildScene(true);
    stepN(100);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    const uint64_t captured = aver_phys_snapshot_hash(snap);
    const int32_t size = aver_phys_snapshot_read(snap, nullptr, 0);
    check(size == aver_phys_snapshot_bytes(snap) && size > 64, "the size query matches the byte count");
    std::vector<uint8_t> blob(static_cast<size_t>(size));
    check(aver_phys_snapshot_read(snap, blob.data(), size) == size, "the blob is copied out");
    check(aver_phys_snapshot_read(snap, blob.data(), size - 1) == 0, "a buffer one byte short is refused, never cut");
    int32_t info[AVER_PHYS_SI_COUNT] = {};
    check(aver_phys_snapshot_info(snap, info, AVER_PHYS_SI_COUNT) == AVER_PHYS_SI_COUNT, "info fills every slot");
    check(info[AVER_PHYS_SI_FLAGS] == static_cast<int32_t>(AVER_PHYS_SNAP_ALL) && info[AVER_PHYS_SI_BYTES] == size &&
              info[AVER_PHYS_SI_PRECISION] == aver_phys_precision() && info[AVER_PHYS_SI_STEPS_LOW] == 100 &&
              info[AVER_PHYS_SI_CHARACTERS] == 1 && info[AVER_PHYS_SI_VEHICLES] == 1 && info[AVER_PHYS_SI_JOINTS] == 1 &&
              info[AVER_PHYS_SI_RAGDOLLS] == 1 && info[AVER_PHYS_SI_BODIES] >= 50,
          "info reports the parts, the size, the precision, step 100, one character, vehicle, joint and ragdoll, and the bodies");

    // The same world built again, stepped a different amount, then handed the blob.
    buildScene(true);
    stepN(37);
    check(aver_phys_snapshot_bytes(snap) == 0, "the old world's handle died with it");
    const int32_t imported = aver_phys_snapshot_import(blob.data(), size);
    check(imported != 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_OK, "the blob imports");
    check(aver_phys_snapshot_hash(imported) == captured, "with the hash it had");
    check(aver_phys_snapshot_restore(imported, 0) == 1, "and restores");
    check(H() == captured, "the world is in the state that was captured: the same hash");
    check(aver_phys_step_total() == 100, "at the same step count");

    // Refusals: nothing becomes a handle.
    std::vector<uint8_t> bad = blob;
    bad[8] ^= 0x04;   // the Real size
    check(aver_phys_snapshot_import(bad.data(), size) == 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_BUILD_MISMATCH,
          "a flipped precision byte is BUILD_MISMATCH");
    check(aver_phys_snapshot_import(blob.data(), size - 1) == 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_BAD_HEADER,
          "a truncated blob is BAD_HEADER");
    bad = blob;
    bad[0] = 'X';
    check(aver_phys_snapshot_import(bad.data(), size) == 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_BAD_HEADER,
          "a wrong magic is BAD_HEADER");
    bad = blob;
    bad[static_cast<size_t>(size) / 2] ^= 0xFF;
    const uint64_t before = H();
    check(aver_phys_snapshot_import(bad.data(), size) == 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_CORRUPT,
          "a flipped payload byte is CORRUPT");
    check(H() == before, "and nothing was applied");
    check(aver_phys_snapshot_import(nullptr, 10) == 0, "a null blob is refused");
}

// ---- 9. statics ---------------------------------------------------------------------------------------------------------

static void testStaticsStayOut() {
    AVER_INFO("=== statics are excluded by default ===");
    freshWorld();
    for (int i = 0; i < 20; ++i) aver_phys_add_dynamic_box(static_cast<float>(i) * 60.0f, 0.0f, 12.0f, 10, 10, 10, 5.0f);
    stepN(30);
    const int32_t plain = aver_phys_snapshot_capture(0, 0);
    const int32_t bytes0 = aver_phys_snapshot_bytes(plain);

    int added = 0;
    for (int i = 0; i < 150; ++i)
        for (int j = 0; j < 134; ++j)
            if (aver_phys_add_static_box(static_cast<float>(i) * 30.0f, 3000.0f + static_cast<float>(j) * 30.0f, 100.0f, 5, 5, 5)) ++added;
    check(added == 20100, "20,100 static boxes were added: " + std::to_string(added));
    const int32_t with = aver_phys_snapshot_capture(0, 0);
    const int32_t bytes1 = aver_phys_snapshot_bytes(with);
    check(std::abs(bytes1 - bytes0) <= bytes0 / 20, "the snapshot is within 5% of the same scene with no statics: " +
                                                      std::to_string(bytes0) + " -> " + std::to_string(bytes1));
    const int32_t all = aver_phys_snapshot_capture(0, AVER_PHYS_SNAP_ALL | AVER_PHYS_SNAP_STATICS);
    check(aver_phys_snapshot_bytes(all) > bytes1 + 20000 * 20, "asking for statics adds them: " + std::to_string(aver_phys_snapshot_bytes(all)));
    check(aver_phys_snapshot_restore(all, 0) == 1 && aver_phys_snapshot_restore(with, 0) == 1, "both restore");
}

// ---- 10. exact stepping -------------------------------------------------------------------------------------------------

static void testStepExact() {
    AVER_INFO("=== step_exact runs exactly n steps and leaves the accumulator alone ===");
    freshWorld();
    aver_phys_add_dynamic_box(0.0f, 0.0f, 100.0f, 10, 10, 10, 5.0f);
    aver_phys_accumulator_set(0.005f);
    const int64_t t0 = aver_phys_step_total();
    check(aver_phys_step_exact(7) == 7, "step_exact(7) reports 7");
    check(aver_phys_step_total() - t0 == 7, "and 7 fixed steps ran");
    check(aver_phys_accumulator_get() == 0.005f, "the accumulator is unchanged");
    check(aver_phys_step_exact(20) == 20 && aver_phys_step_total() - t0 == 27, "a count above one call's catch-up cap still runs in full");
    check(aver_phys_step_exact(0) == 0 && aver_phys_step_exact(601) == 0 && aver_phys_step_exact(-3) == 0, "0, 601 and negative counts run nothing");
    check(aver_phys_step_total() - t0 == 27, "and change nothing");
    check(aver_phys_accumulator_set(1000.0f) == 1 && aver_phys_accumulator_get() <= aver_phys_fixed_step() * 8.0f + 1e-6f, "the accumulator is clamped to eight steps");
    check(aver_phys_accumulator_set(-1.0f) == 1 && aver_phys_accumulator_get() == 0.0f, "and to zero below");
    check(aver_phys_accumulator_set(NAN) == 0, "NaN is refused");
}

// ---- 11. a broken joint un-breaks ---------------------------------------------------------------------------------------

static void testBrokenJointRestores() {
    AVER_INFO("=== a broken joint comes back whole with the snapshot ===");
    freshWorld();
    const int32_t weight = aver_phys_add_dynamic_box(0.0f, 0.0f, 400.0f, 10, 10, 10, 10.0f);
    const float pivot[3] = {0.0f, 0.0f, 500.0f};
    const int32_t j = aver_phys_joint_point(weight, AVER_PHYS_WORLD_BODY, pivot);
    aver_phys_joint_set_break(j, 50.0f, 0.0f);   // below the 98 N it carries
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    stepN(120);
    check(aver_phys_joint_enabled(j) == 0 && aver_phys_joint_break_count() == 1, "the joint broke");
    float p[3] = {};
    aver_phys_body_position(weight, p);
    check(p[2] < 300.0f, "and the weight fell, z = " + std::to_string(p[2]));
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restore");
    check(aver_phys_joint_enabled(j) == 1, "the joint is whole again");
    check(aver_phys_joint_break_count() == 0, "the undrained break event is gone");
    aver_phys_body_position(weight, p);
    check(std::fabs(p[2] - 400.0f) < 1.0f, "the weight hangs where it did, z = " + std::to_string(p[2]));
    stepN(120);
    check(aver_phys_joint_enabled(j) == 0 && aver_phys_joint_break_count() == 1, "and it breaks again, once");
}

// ---- 12. cloth ---------------------------------------------------------------------------------------------------------

static void testClothRestores() {
    AVER_INFO("=== soft-body vertices come back exactly ===");
    freshWorld();
    const Grid g = makeGrid(7, 14.0f, 300.0f);
    std::vector<float> invMass(static_cast<size_t>(g.count()), 1.0f);
    for (int32_t ix = 0; ix < g.n; ++ix) invMass[static_cast<size_t>(g.at(ix, 0))] = 0.0f;
    const int32_t cloth = aver_phys_softbody_create(g.verts.data(), g.count(), g.indices.data(), static_cast<int32_t>(g.indices.size()),
                                                    invMass.data(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    stepN(20);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    std::vector<float> v0(static_cast<size_t>(g.count()) * 3), v1(v0.size()), v2(v0.size());
    aver_phys_softbody_vertices(cloth, v0.data(), g.count());
    stepN(60);
    aver_phys_softbody_vertices(cloth, v1.data(), g.count());
    check(std::memcmp(v0.data(), v1.data(), v0.size() * sizeof(float)) != 0, "the cloth moved in 60 steps");
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restore");
    aver_phys_softbody_vertices(cloth, v2.data(), g.count());
    check(std::memcmp(v0.data(), v2.data(), v0.size() * sizeof(float)) == 0, "every vertex is bit-identical to the capture");
}

// ---- 13. parts, flags, info ---------------------------------------------------------------------------------------------

static void testPartsAndFlags() {
    AVER_INFO("=== parts: the accumulator rides with ENGINE only; unknown flags are refused ===");
    freshWorld();
    aver_phys_add_dynamic_box(0.0f, 0.0f, 100.0f, 10, 10, 10, 5.0f);
    aver_phys_accumulator_set(0.004f);
    const int32_t withEngine = aver_phys_snapshot_capture(0, AVER_PHYS_SNAP_ALL);
    const int32_t bodiesOnly = aver_phys_snapshot_capture(0, AVER_PHYS_SNAP_BODIES);
    aver_phys_accumulator_set(0.009f);
    check(aver_phys_snapshot_restore(bodiesOnly, 0) == 1 && aver_phys_accumulator_get() == 0.009f, "a bodies-only restore leaves the accumulator");
    check(aver_phys_snapshot_restore(withEngine, 0) == 1 && aver_phys_accumulator_get() == 0.004f, "a full restore puts it back");
    check(aver_phys_snapshot_capture(0, 0x100u) == 0, "a flag outside the known set gives no snapshot");
    check(aver_phys_snapshot_restore(withEngine, 0x2u) == 0, "an unknown restore option is refused");
    check(aver_phys_state_hash(0x100u) == 0, "and so is an unknown part in a state hash");
    check(aver_phys_state_hash(AVER_PHYS_SNAP_BODIES) != aver_phys_state_hash(AVER_PHYS_SNAP_ALL), "different parts hash differently");
    int32_t info[AVER_PHYS_SI_COUNT] = {};
    check(aver_phys_snapshot_info(bodiesOnly, info, AVER_PHYS_SI_COUNT) == AVER_PHYS_SI_COUNT &&
              info[AVER_PHYS_SI_FLAGS] == static_cast<int32_t>(AVER_PHYS_SNAP_BODIES) && info[AVER_PHYS_SI_BODIES] == 1,
          "info of a bodies-only snapshot: the flag and one body");
    check(aver_phys_snapshot_info(999, info, AVER_PHYS_SI_COUNT) == 0, "info of a dead handle is 0");
    check(aver_phys_state_abi_version() == AVER_PHYS_STATE_ABI_VERSION, "the ABI version reads back");
}

// ---- 14. lifetime ------------------------------------------------------------------------------------------------------

static void testWorldLifetime() {
    AVER_INFO("=== a snapshot dies with its world ===");
    freshWorld();
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    check(snap != 0 && aver_phys_snapshot_count() == 1, "one snapshot");
    aver_phys_shutdown();
    check(aver_phys_snapshot_capture(0, 0) == 0, "no capture without a world");
    aver_phys_init();
    check(aver_phys_snapshot_bytes(snap) == 0 && aver_phys_snapshot_count() == 0, "the new world holds none of the old world's snapshots");
    check(aver_phys_snapshot_restore(snap, 0) == 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_BAD_HANDLE,
          "restoring one answers BAD_HANDLE");
    check(aver_phys_state_hash(0) != 0, "an empty world still hashes");
    aver_phys_shutdown();
    check(aver_phys_state_hash(0) == 0 && aver_phys_step_total() == 0 && aver_phys_step_exact(1) == 0, "with no world: hash 0, no steps");
}

// ---- 15. hash and diff -------------------------------------------------------------------------------------------------

static void testHashAndDiff() {
    AVER_INFO("=== hash and diff ===");
    freshWorld();
    for (int i = 0; i < 5; ++i) aver_phys_add_dynamic_box(static_cast<float>(i) * 40.0f, 0.0f, 50.0f, 10, 10, 10, 5.0f);
    stepN(10);
    const int32_t a = aver_phys_snapshot_capture(0, 0);
    const int32_t b = aver_phys_snapshot_capture(0, 0);
    check(aver_phys_snapshot_hash(a) == aver_phys_snapshot_hash(b) && aver_phys_snapshot_diff(a, b) == -1, "two captures of the same instant are equal");
    stepN(10);
    const int32_t c = aver_phys_snapshot_capture(0, 0);
    check(aver_phys_snapshot_hash(a) != aver_phys_snapshot_hash(c), "a later state hashes differently");
    check(aver_phys_snapshot_diff(a, c) >= 0, "and diff names an offset: " + std::to_string(aver_phys_snapshot_diff(a, c)));
    check(aver_phys_snapshot_hash(c) == H(), "the hash of a capture equals the state hash");
    check(aver_phys_snapshot_diff(a, 999) == -2, "diff of a dead handle is -2");
    check(hex(aver_phys_snapshot_hash(c)).size() == 16, "hashes print as 16 hex digits");
}

int main() {
    AVER_INFO("=== PhysicsStateTest ===");
    testRoundTrip();
    testValidation();
    testConstraintTopology();
    testVehicleAidState();
    testCharacterRidesAfterRestore();
    testEventsCleared();
    testReuse();
    testTransport();
    testStaticsStayOut();
    testStepExact();
    testBrokenJointRestores();
    testClothRestores();
    testPartsAndFlags();
    testWorldLifetime();
    testHashAndDiff();
    aver_phys_shutdown();
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
