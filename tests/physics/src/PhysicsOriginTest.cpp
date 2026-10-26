// PhysicsOriginTest -- the floating origin's physics half (physics_origin_abi.h; LARGE_WORLD 2.3, 5.3), through the ABI only.
//
//   P-1 (this file as first written):
//   * identity: a world that never shifts reports generation 0, zero totals and no stats, and aver_phys_origin_prepare mutates nothing
//     (the state hash after 120 steps is the same with and without a prepare every step),
//   * exactness: 1,000 bodies within 1 km of 9,216 m shift by exactly -delta (bit for bit in cm, positions on quarter metres), a far body
//     within 0.2 cm; velocities, body count and handles are untouched,
//   * a pile of 20 boxes that is ASLEEP stays asleep with equal positions (and does not wake over the next two seconds); an awake pile
//     keeps its velocities through the shift and settles within 0.5 cm of an unshifted control translated by delta,
//   * a character on a moving deck stays grounded, keeps its ground carry (the inherited velocity) and lands at pos - delta; its inner body
//     is not shifted twice (a ray finds it only at the new place),
//   * a car driving through the shift keeps its speed and its four wheels' contact,
//   * a pending contact's point is shifted; sensors still fire after the shift; a cloth stays finite and moves with its place,
//   * refusals: no world, Z component, not a multiple of 100 cm, NaN, zero, and from inside a step (a water sampler calls it),
//   * more than 4,096 statics are counted as moved (and the following step runs),
//   * generation and total accumulate, stats describe the last shift, a new world starts again at zero,
//   * raycasts and sphere overlaps hit the statics at their shifted coordinates and miss the old ones.
//
//   P-2 and P-3 (added after the first draft):
//   * items 6 and 7: a hinge door, a slider and a path joint to the world hold across the shift (the anchor body), swing and slide limits
//     unchanged; two pulleys (one between bodies, one to the world) are rebuilt with the same handle, keep their rope length, and a snapshot
//     from before the rebuild is refused by the constraint epoch,
//   * item 9: hooks run once per shift, in registration order, after the bodies moved, and cannot re-enter the shift (through the test seam
//     aver_phys_origin_test_hook, because the registry itself is module-internal and not exported),
//   * item 10: a snapshot restored after a shift lands in the current frame (boxes, a character, a hinged door whose anchor must not move twice;
//     statics only when the blob held them), a blob taken after a shift carries the generation and restores across the next one,
//   * item 15: a mesh shape's cache blob under one key is byte for byte the same after a shift.
//
// Hand-run, exit code = failure count, same shape as PhysicsStateTest.cpp.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_character_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_origin_abi.h"
#include "aver/physics/physics_state_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_water_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

static const float  kDt = 1.0f / 60.0f;
static const double kStep = 1638400.0;      // one region: the origin's step, cm
static const float  kX0 = 921600.0f;        // 9,216 m: the camera when the origin rebases

static void steps(int n) { for (int i = 0; i < n; ++i) aver_phys_step(kDt); }
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_init();
}

// A floor whose top is z = 0, centred on x, 1 km each way.
static int32_t floorAt(float x, float y) { return aver_phys_add_static_box(x, y, -50.0f, 50000.0f, 50000.0f, 50.0f); }

struct P3 { float v[3] = {0, 0, 0}; };
static P3 posOf(int32_t h) { P3 p; aver_phys_body_position(h, p.v); return p; }
static P3 velOf(int32_t h) { P3 p; aver_phys_body_velocity(h, p.v); return p; }

// ---- 1. identity -------------------------------------------------------------------------------------------------------

static uint64_t runPile(bool prepareEachStep) {
    freshWorld();
    floorAt(0, 0);
    for (int i = 0; i < 20; ++i)
        aver_phys_add_dynamic_box(static_cast<float>(i % 5) * 60.0f, static_cast<float>(i / 5) * 60.0f, 40.0f + static_cast<float>(i % 3) * 10.0f, 25, 25, 25, 20.0f);
    for (int i = 0; i < 120; ++i) {
        if (prepareEachStep) aver_phys_origin_prepare(kStep, 0.0, 0.0);
        aver_phys_step(kDt);
    }
    return aver_phys_state_hash(0);
}

static void testIdentity() {
    AVER_INFO("=== identity: a world that never shifts ===");
    const uint64_t a = runPile(false);
    check(aver_phys_origin_generation() == 0, "generation 0 in a world that never shifted");
    double tot[3] = {1, 1, 1};
    aver_phys_origin_total(tot);
    check(tot[0] == 0.0 && tot[1] == 0.0 && tot[2] == 0.0, "total is zero");
    float ms = 1.0f; int32_t b = 1, c = 1, an = 1, pu = 1, st = 1;
    check(aver_phys_origin_stats(&ms, &b, &c, &an, &pu, &st) == 0 && ms == 0.0f && b == 0 && c == 0 && an == 0 && pu == 0 && st == 0,
          "stats are zero and say no shift has run");
    const uint64_t withPrepare = runPile(true);
    check(a == withPrepare, "aver_phys_origin_prepare mutates nothing (equal state hash after 120 steps)");
    check(aver_phys_origin_generation() == 0, "and does not count as a shift");
    check(aver_phys_origin_abi_version() == AVER_PHYS_ORIGIN_ABI_VERSION, "ABI version");
}

// ---- 2. exactness ------------------------------------------------------------------------------------------------------

static void testExactness() {
    AVER_INFO("=== exactness ===");
    freshWorld();
    std::vector<int32_t> hs;
    std::vector<P3> before, velBefore;
    for (int i = 0; i < 1000; ++i) {
        // 25 m apart, within +-50 m on X: quarter-metre positions, 821,600 cm and up (>= delta / 2, so the subtraction is exact).
        const float x = kX0 + static_cast<float>((i % 40) - 20) * 2500.0f;
        const float y = static_cast<float>((i / 40) - 12) * 2500.0f;
        const int32_t h = aver_phys_add_dynamic_box(x, y, 1000.0f + static_cast<float>(i % 7) * 25.0f, 20, 20, 20, 5.0f);
        aver_phys_body_set_velocity(h, static_cast<float>(i % 11) * 3.0f - 15.0f, 7.0f, -2.0f);
        hs.push_back(h);
    }
    const int32_t far = aver_phys_add_dynamic_box(101234.5f, -3000.0f, 500.0f, 20, 20, 20, 5.0f);
    const int32_t count = aver_phys_body_count();
    for (int32_t h : hs) { before.push_back(posOf(h)); velBefore.push_back(velOf(h)); }
    const P3 farBefore = posOf(far);

    check(aver_phys_origin_prepare(kStep, 0.0, 0.0) == count, "prepare counts every body (" + std::to_string(count) + ")");
    check(aver_phys_origin_generation() == 0, "prepare did not shift");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");

    int exact = 0, velSame = 0;
    for (size_t i = 0; i < hs.size(); ++i) {
        const P3 a = posOf(hs[i]), v = velOf(hs[i]);
        if (a.v[0] == before[i].v[0] - static_cast<float>(kStep) && a.v[1] == before[i].v[1] && a.v[2] == before[i].v[2]) ++exact;
        if (v.v[0] == velBefore[i].v[0] && v.v[1] == velBefore[i].v[1] && v.v[2] == velBefore[i].v[2]) ++velSame;
    }
    check(exact == 1000, "1000 of 1000 bodies landed on exactly pos - delta, bit for bit (" + std::to_string(exact) + ")");
    check(velSame == 1000, "and kept their velocities exactly");
    const P3 fa = posOf(far);
    check(near(fa.v[0], farBefore.v[0] - static_cast<float>(kStep), 0.2f) && fa.v[1] == farBefore.v[1] && fa.v[2] == farBefore.v[2],
          "a body 1 km from the old origin lands within 0.2 cm (" + std::to_string(fa.v[0]) + ")");
    check(aver_phys_body_count() == count, "the body count is unchanged");

    float ms = 0; int32_t nb = 0, nc = 0, na = 0, np = 0, ns = 0;
    check(aver_phys_origin_stats(&ms, &nb, &nc, &na, &np, &ns) == 1 && nb == count && nc == 0 && ns == 0 && ms >= 0.0f,
          "stats: " + std::to_string(nb) + " bodies, no characters, no statics, " + std::to_string(ms) + " ms");
    check(aver_phys_origin_generation() == 1, "generation is 1");

    // Y shifts too, and a shift back restores the first positions (every position is a quarter metre, so both directions are exact).
    check(aver_phys_origin_shift(0.0, kStep, 0.0) == 1, "a Y shift is accepted");
    const P3 yb = posOf(hs[0]);
    check(yb.v[1] == before[0].v[1] - static_cast<float>(kStep), "Y moved by -delta");
    check(aver_phys_origin_shift(-kStep, -kStep, 0.0) == 1, "the opposite shift is accepted");
    const P3 back = posOf(hs[0]);
    check(back.v[0] == before[0].v[0] && back.v[1] == before[0].v[1] && back.v[2] == before[0].v[2], "shifting back restores the original position bit for bit");
    aver_phys_shutdown();
}

// ---- 3. sleeping and awake piles ---------------------------------------------------------------------------------------

static std::vector<int32_t> buildPile(float cx) {
    floorAt(cx, 0);
    std::vector<int32_t> hs;
    for (int i = 0; i < 20; ++i)
        hs.push_back(aver_phys_add_dynamic_box(cx + static_cast<float>(i % 5) * 60.0f, static_cast<float>(i / 5) * 60.0f, 40.0f + static_cast<float>(i % 3) * 10.0f, 25, 25, 25, 20.0f));
    return hs;
}

static void testSleepingPile() {
    AVER_INFO("=== a sleeping pile stays asleep ===");
    freshWorld();
    const std::vector<int32_t> hs = buildPile(kX0);
    bool asleep = false;
    for (int i = 0; i < 900 && !asleep; ++i) {
        aver_phys_step(kDt);
        asleep = true;
        for (int32_t h : hs) if (aver_phys_body_is_active(h)) { asleep = false; break; }
    }
    check(asleep, "the pile fell asleep by itself");
    std::vector<P3> before;
    for (int32_t h : hs) before.push_back(posOf(h));

    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    int stillAsleep = 0, placed = 0;
    for (size_t i = 0; i < hs.size(); ++i) {
        if (!aver_phys_body_is_active(hs[i])) ++stillAsleep;
        const P3 a = posOf(hs[i]);
        if (near(a.v[0], before[i].v[0] - static_cast<float>(kStep), 0.2f) && a.v[1] == before[i].v[1] && a.v[2] == before[i].v[2]) ++placed;
    }
    check(stillAsleep == 20, "all 20 are still asleep right after the shift (" + std::to_string(stillAsleep) + ")");
    check(placed == 20, "all 20 are at pos - delta (y and z equal exactly)");

    std::vector<P3> after;
    for (int32_t h : hs) after.push_back(posOf(h));
    steps(120);
    int asleepLater = 0, still = 0;
    for (size_t i = 0; i < hs.size(); ++i) {
        if (!aver_phys_body_is_active(hs[i])) ++asleepLater;
        const P3 a = posOf(hs[i]);
        if (near(a.v[0], after[i].v[0], 0.05f) && near(a.v[1], after[i].v[1], 0.05f) && near(a.v[2], after[i].v[2], 0.05f)) ++still;
    }
    check(asleepLater == 20, "and still asleep two seconds later (" + std::to_string(asleepLater) + ")");
    check(still == 20, "without moving");
    aver_phys_shutdown();
}

struct PileRun { std::vector<P3> final; float maxSpeed = 0.0f; bool velocitiesKept = true; int movingBefore = 0; };

static PileRun runAwakePile(bool shift) {
    PileRun r;
    freshWorld();
    const std::vector<int32_t> hs = buildPile(kX0);
    steps(8);
    if (shift) {
        std::vector<P3> vb;
        for (int32_t h : hs) vb.push_back(velOf(h));
        for (const P3& v : vb) if (std::fabs(v.v[2]) > 20.0f) ++r.movingBefore;
        aver_phys_origin_shift(kStep, 0.0, 0.0);
        for (size_t i = 0; i < hs.size(); ++i) {
            const P3 v = velOf(hs[i]);
            if (v.v[0] != vb[i].v[0] || v.v[1] != vb[i].v[1] || v.v[2] != vb[i].v[2]) r.velocitiesKept = false;
        }
    }
    for (int i = 0; i < 200; ++i) {
        aver_phys_step(kDt);
        for (int32_t h : hs) {
            const P3 v = velOf(h);
            r.maxSpeed = std::fmax(r.maxSpeed, std::sqrt(v.v[0] * v.v[0] + v.v[1] * v.v[1] + v.v[2] * v.v[2]));
        }
    }
    for (int32_t h : hs) r.final.push_back(posOf(h));
    aver_phys_shutdown();
    return r;
}

static void testAwakePile() {
    AVER_INFO("=== an awake pile continues through the shift ===");
    const PileRun control = runAwakePile(false);
    const PileRun shifted = runAwakePile(true);
    check(shifted.movingBefore >= 10, "most boxes were still falling when the shift came (" + std::to_string(shifted.movingBefore) + ")");
    check(shifted.velocitiesKept, "every velocity was exactly the same right after the shift");
    int close = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < control.final.size() && i < shifted.final.size(); ++i) {
        const float dx = std::fabs(shifted.final[i].v[0] + static_cast<float>(kStep) - control.final[i].v[0]);
        const float dy = std::fabs(shifted.final[i].v[1] - control.final[i].v[1]);
        const float dz = std::fabs(shifted.final[i].v[2] - control.final[i].v[2]);
        worst = std::fmax(worst, std::fmax(dx, std::fmax(dy, dz)));
        if (dx <= 0.5f && dy <= 0.5f && dz <= 0.5f) ++close;
    }
    check(close == 20, "all 20 settled within 0.5 cm of the unshifted control translated by delta (worst " + std::to_string(worst) + " cm)");
    check(shifted.maxSpeed <= control.maxSpeed * 1.5f + 10.0f, "no velocity spike after the shift (max " + std::to_string(shifted.maxSpeed) + " vs control " + std::to_string(control.maxSpeed) + " cm/s)");
}

// ---- 4. a character on a moving deck -----------------------------------------------------------------------------------

static void testCharacter() {
    AVER_INFO("=== a character on a moving deck ===");
    freshWorld();
    const int32_t deck = aver_phys_add_static_box(kX0, 0.0f, -20.0f, 2000.0f, 20000.0f, 20.0f);   // top at z = 0
    check(aver_phys_body_set_motion_type(deck, AVER_PHYS_MOTION_KINEMATIC) == 1, "the deck is kinematic");
    // One call derives 300 cm/s along +Y and Jolt keeps integrating it: no per-frame driver needed.
    aver_phys_body_move_kinematic(deck, kX0, 300.0f, -20.0f, 0, 0, 0, 1, 1.0f);
    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, kX0, 0.0f, 95.0f);
    steps(120);
    check(aver_phys_character_grounded(ch) == 1, "the character stands on the deck");
    P3 carryBefore;
    aver_phys_character_inherited_velocity(ch, carryBefore.v);
    check(near(carryBefore.v[1], 300.0f, 20.0f), "it carries the deck's velocity (" + std::to_string(carryBefore.v[1]) + " cm/s)");
    P3 pb;
    aver_phys_character_position(ch, pb.v);

    check(aver_phys_origin_prepare(kStep, 0.0, 0.0) == aver_phys_body_count() + 1, "prepare counts the deck and the character");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    P3 pa, carryAfter;
    aver_phys_character_position(ch, pa.v);
    aver_phys_character_inherited_velocity(ch, carryAfter.v);
    check(near(pa.v[0], pb.v[0] - static_cast<float>(kStep), 0.2f) && pa.v[1] == pb.v[1] && pa.v[2] == pb.v[2], "the character is at pos - delta (once, not twice)");
    check(aver_phys_character_grounded(ch) == 1, "still grounded right after the shift (contacts refreshed)");
    check(carryAfter.v[0] == carryBefore.v[0] && carryAfter.v[1] == carryBefore.v[1] && carryAfter.v[2] == carryBefore.v[2], "the ground carry survived exactly");
    float stats[1] = {0};
    int32_t nb = 0, nc = 0;
    aver_phys_origin_stats(stats, &nb, &nc, nullptr, nullptr, nullptr);
    check(nc == 1, "stats count one character");

    steps(60);
    P3 pl;
    aver_phys_character_position(ch, pl.v);
    check(aver_phys_character_grounded(ch) == 1, "still grounded a second later");
    check(near(pl.v[0], pa.v[0], 1.0f) && near(pl.v[2], pa.v[2], 2.0f), "and it did not fall or slide (z " + std::to_string(pl.v[2]) + ")");

    // The character's inner body is a body of its own in Jolt, moved by SetPosition with the character: a ray finds it at the new place only.
    float pt[3] = {0, 0, 0}, nm[3] = {0, 0, 1};
    int32_t ent = 0;
    const int32_t hitNew = aver_phys_raycast(pl.v[0], pl.v[1], 400.0f, 0, 0, -1, 1000.0f, pt, nm, &ent);
    check(hitNew != 0 && pt[2] > 100.0f, "a ray at the new place hits the character's top (z " + std::to_string(pt[2]) + ")");
    const int32_t hitOld = aver_phys_raycast(pl.v[0] + static_cast<float>(kStep), pl.v[1], 400.0f, 0, 0, -1, 1000.0f, pt, nm, &ent);
    check(hitOld == 0, "a ray at the old place hits nothing");
    aver_phys_shutdown();
}

// ---- 5. a car ----------------------------------------------------------------------------------------------------------

static int32_t buildCar(float x, float y, float z) {
    const float kSpringHz = 2.0f;
    const float w = 2.0f * 3.14159265f * kSpringHz;
    const float kSag = 981.0f / (w * w);
    const float attachZ = 33.0f + 30.0f - kSag;
    const int32_t v = aver_phys_vehicle_create(220.0f, 90.0f, 35.0f, 25.0f, 0.0f, 0.0f, 40.0f, 1300.0f, x, y, z, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        aver_phys_vehicle_add_wheel(v, front ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, 33.0f, 22.0f, 5.0f, 30.0f,
                                    kSpringHz, 0.5f, front ? 35.0f : 0.0f, 1500.0f, 2500.0f, front ? 0 : 1);
    }
    aver_phys_vehicle_finish(v, 60.0f);
    return v;
}

static int wheelsDown(int32_t v) {
    int n = 0;
    for (int i = 0; i < aver_phys_vehicle_wheel_count(v); ++i) if (aver_phys_vehicle_wheel_contact(v, i)) ++n;
    return n;
}

static void testVehicle() {
    AVER_INFO("=== a car drives through the shift ===");
    freshWorld();
    floorAt(kX0, 0);
    const int32_t car = buildCar(kX0 - 3000.0f, 0.0f, 60.0f);
    for (int i = 0; i < 240; ++i) { aver_phys_vehicle_set_input(car, 1.0f, 0.0f, 0.0f, 0.0f); aver_phys_step(kDt); }
    const float speedBefore = aver_phys_vehicle_forward_speed(car);
    P3 pb;
    float qb[4] = {0, 0, 0, 1}, qa[4] = {0, 0, 0, 1};
    aver_phys_vehicle_pose(car, pb.v, qb);
    check(speedBefore > 100.0f && wheelsDown(car) == 4, "it is moving (" + std::to_string(speedBefore) + " cm/s) on four wheels");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    P3 pa;
    aver_phys_vehicle_pose(car, pa.v, qa);
    check(near(pa.v[0], pb.v[0] - static_cast<float>(kStep), 0.3f) && pa.v[1] == pb.v[1] && pa.v[2] == pb.v[2], "the chassis is at pos - delta");
    check(aver_phys_vehicle_forward_speed(car) == speedBefore, "its speed is exactly the same right after the shift");
    for (int i = 0; i < 60; ++i) { aver_phys_vehicle_set_input(car, 1.0f, 0.0f, 0.0f, 0.0f); aver_phys_step(kDt); }
    check(wheelsDown(car) == 4, "four wheels in contact a second later");
    check(aver_phys_vehicle_forward_speed(car) >= speedBefore * 0.9f, "and it kept going (" + std::to_string(aver_phys_vehicle_forward_speed(car)) + " cm/s)");
    aver_phys_shutdown();
}

// ---- 6. events, sensors, cloth, queries --------------------------------------------------------------------------------

static void testPendingContact() {
    AVER_INFO("=== a pending contact point is shifted ===");
    freshWorld();
    floorAt(kX0, 0);
    const int32_t box = aver_phys_add_dynamic_box(kX0 + 125.0f, 75.0f, 27.0f, 25, 25, 25, 10.0f);
    int found = -1;
    for (int i = 0; i < 30 && found < 0; ++i) {
        aver_phys_step(kDt);
        for (int c = 0; c < aver_phys_contact_count(); ++c) {
            int32_t a = 0, b = 0;
            float p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
            aver_phys_contact_get(c, &a, &b, p, n);
            if (a == box || b == box) { found = c; break; }
        }
    }
    check(found >= 0, "the box touched the floor");
    if (found >= 0) {
        int32_t a = 0, b = 0;
        float pb[3] = {0, 0, 0}, nb[3] = {0, 0, 0}, pa[3] = {0, 0, 0}, na[3] = {0, 0, 0};
        aver_phys_contact_get(found, &a, &b, pb, nb);
        check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
        check(aver_phys_contact_count() > found, "the contact is still queued");
        aver_phys_contact_get(found, &a, &b, pa, na);
        check(near(pa[0], pb[0] - static_cast<float>(kStep), 0.2f) && near(pa[1], pb[1], 0.001f) && near(pa[2], pb[2], 0.001f),
              "its point moved by -delta (" + std::to_string(pa[0]) + ")");
        check(near(na[0], nb[0], 0.0f) && near(na[1], nb[1], 0.0f) && near(na[2], nb[2], 0.0f), "and its normal did not");
    }
    aver_phys_shutdown();
}

static void testSensor() {
    AVER_INFO("=== a sensor still fires ===");
    freshWorld();
    const int32_t sensor = aver_phys_add_sensor_box(kX0, 0.0f, 100.0f, 100, 100, 50);
    const int32_t ball = aver_phys_add_dynamic_sphere(kX0, 0.0f, 400.0f, 20.0f, 5.0f);
    steps(5);
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    bool entered = false;
    for (int i = 0; i < 90 && !entered; ++i) {
        aver_phys_step(kDt);
        for (int o = 0; o < aver_phys_overlap_count(); ++o) {
            int32_t s = 0, b = 0, e = 0;
            aver_phys_overlap_get(o, &s, &b, &e);
            if (s == sensor && b == ball && e == 1) entered = true;
        }
    }
    check(entered, "the ball falling into the sensor after the shift is reported as an enter");
    aver_phys_shutdown();
}

static void testCloth() {
    AVER_INFO("=== a cloth stays finite and goes with its place ===");
    freshWorld();
    floorAt(kX0, 0);
    const int n = 6;
    std::vector<float> verts;
    std::vector<int32_t> idx;
    for (int iy = 0; iy < n; ++iy)
        for (int ix = 0; ix < n; ++ix) { verts.push_back(kX0 + static_cast<float>(ix) * 10.0f - 25.0f); verts.push_back(static_cast<float>(iy) * 10.0f - 25.0f); verts.push_back(120.0f); }
    for (int iy = 0; iy + 1 < n; ++iy)
        for (int ix = 0; ix + 1 < n; ++ix) {
            const int32_t a = iy * n + ix, b = a + 1, c = a + n + 1, d = a + n;
            idx.insert(idx.end(), {a, b, c});
            idx.insert(idx.end(), {a, c, d});
        }
    const int32_t cloth = aver_phys_softbody_create(verts.data(), n * n, idx.data(), static_cast<int32_t>(idx.size()), nullptr, 0, 0, 0, 0.0f, 0.0f);
    check(cloth != 0, "a cloth exists above the floor");
    steps(20);
    std::vector<float> before(static_cast<size_t>(n * n) * 3), after(before.size());
    aver_phys_softbody_vertices(cloth, before.data(), n * n);
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    aver_phys_softbody_vertices(cloth, after.data(), n * n);
    bool moved = true;
    for (size_t i = 0; i < before.size() / 3; ++i)
        if (!near(after[i * 3], before[i * 3] - static_cast<float>(kStep), 0.5f) || !near(after[i * 3 + 1], before[i * 3 + 1], 0.01f) || !near(after[i * 3 + 2], before[i * 3 + 2], 0.01f)) moved = false;
    check(moved, "every vertex is at its old place minus delta");
    steps(120);
    aver_phys_softbody_vertices(cloth, after.data(), n * n);
    bool finite = true;
    float lowest = 1e9f;
    for (size_t i = 0; i < after.size(); ++i) if (!std::isfinite(after[i])) finite = false;
    for (size_t i = 0; i < after.size() / 3; ++i) lowest = std::fmin(lowest, after[i * 3 + 2]);
    check(finite, "all vertices are finite after 2 s");
    check(lowest > -5.0f && lowest < 60.0f, "it landed on the floor (lowest z " + std::to_string(lowest) + ")");
    aver_phys_shutdown();
}

static void testQueries() {
    AVER_INFO("=== queries hit the shifted coordinates ===");
    freshWorld();
    const int32_t box = aver_phys_add_static_box(kX0, 0.0f, 50.0f, 100, 100, 50);   // top at z = 100
    float pt[3] = {0, 0, 0}, nm[3] = {0, 0, 1};
    int32_t ent = 0;
    check(aver_phys_raycast(kX0, 0, 500, 0, 0, -1, 1000, pt, nm, &ent) == box && near(pt[2], 100.0f, 0.01f), "before: a ray from above hits the box top");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    const float nx = kX0 - static_cast<float>(kStep);
    check(aver_phys_raycast(nx, 0, 500, 0, 0, -1, 1000, pt, nm, &ent) == box && near(pt[2], 100.0f, 0.01f) && near(pt[0], nx, 0.1f),
          "after: a ray at the shifted coordinates hits it at the same height");
    check(aver_phys_raycast(kX0, 0, 500, 0, 0, -1, 1000, pt, nm, &ent) == 0, "and a ray at the old coordinates hits nothing");
    int32_t out[4] = {0, 0, 0, 0};
    check(aver_phys_overlap_sphere(nx, 0, 60, 30, out, 4) == 1 && out[0] == box, "a sphere overlap finds it at the new place");
    check(aver_phys_overlap_sphere(kX0, 0, 60, 30, out, 4) == 0, "and not at the old one");
    aver_phys_shutdown();
}

// ---- 7. refusals -------------------------------------------------------------------------------------------------------

static int32_t g_inStepPrepare = 99, g_inStepShift = 99, g_inStepError = 99, g_inStepCalls = 0;
static int32_t waterProbe(float, float, float, AverPhysWaterSample*, void*) {
    ++g_inStepCalls;
    g_inStepPrepare = aver_phys_origin_prepare(kStep, 0.0, 0.0);
    g_inStepShift = aver_phys_origin_shift(kStep, 0.0, 0.0);
    g_inStepError = aver_phys_last_error();
    return -1;   // no opinion
}

static void testRefusals() {
    AVER_INFO("=== refusals ===");
    aver_phys_shutdown();
    check(aver_phys_origin_prepare(kStep, 0.0, 0.0) == -1 && aver_phys_origin_shift(kStep, 0.0, 0.0) == 0, "no world: refused");
    check(aver_phys_origin_generation() == 0, "and generation 0");
    aver_phys_init();
    aver_phys_add_dynamic_box(0, 0, 500, 20, 20, 20, 5.0f);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    check(aver_phys_origin_prepare(kStep, 0.0, 100.0) == -1 && aver_phys_origin_shift(kStep, 0.0, 100.0) == 0, "a Z component is refused");
    check(aver_phys_origin_prepare(kStep + 50.0, 0.0, 0.0) == -1 && aver_phys_origin_shift(50.0, 0.0, 0.0) == 0, "a delta that is not a multiple of 100 cm is refused");
    check(aver_phys_origin_shift(nan, 0.0, 0.0) == 0 && aver_phys_origin_shift(0.0, inf, 0.0) == 0 && aver_phys_origin_prepare(0.0, nan, 0.0) == -1, "NaN and infinity are refused");
    check(aver_phys_origin_shift(0.0, 0.0, 0.0) == 0 && aver_phys_origin_prepare(0.0, 0.0, 0.0) == -1, "a zero delta is refused");
    check(aver_phys_origin_shift(1.0e12, 0.0, 0.0) == 0, "an absurd delta is refused");
    check(aver_phys_origin_generation() == 0, "nothing was shifted by any of them");
    P3 p = posOf(1);
    check(p.v[0] == 0.0f && p.v[2] == 500.0f, "and the body did not move");

    aver_phys_set_water_sampler(&waterProbe, nullptr);
    steps(3);
    aver_phys_clear_water_sampler();
    check(g_inStepCalls > 0, "the probe ran inside a step (" + std::to_string(g_inStepCalls) + " calls)");
    check(g_inStepPrepare == -1 && g_inStepShift == 0, "prepare and shift are refused from inside a step");
    check(g_inStepError == -5, "and the reason is Unsupported (" + std::to_string(g_inStepError) + ")");
    check(aver_phys_origin_generation() == 0, "nothing shifted inside the step");
    check(aver_phys_origin_prepare(kStep, 0.0, 0.0) > 0 && aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "between steps it works again");
    aver_phys_shutdown();
}

// ---- 8. counters and a new world ---------------------------------------------------------------------------------------

static void testCounters() {
    AVER_INFO("=== generation, total and stats ===");
    freshWorld();
    floorAt(0, 0);
    for (int i = 0; i < 10; ++i) aver_phys_add_dynamic_box(static_cast<float>(i) * 100.0f, 0, 500, 20, 20, 20, 5.0f);
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift 1");
    check(aver_phys_origin_shift(0.0, -2.0 * kStep, 0.0) == 1, "shift 2");
    check(aver_phys_origin_shift(3.0 * kStep, 0.0, 0.0) == 1, "shift 3, three regions at once");
    check(aver_phys_origin_generation() == 3, "generation is 3");
    double t[3] = {0, 0, 0};
    aver_phys_origin_total(t);
    check(t[0] == 4.0 * kStep && t[1] == -2.0 * kStep && t[2] == 0.0, "total is (4, -2, 0) regions");
    float ms = 0; int32_t nb = 0, nc = 0, na = 0, np = 0, ns = 0;
    check(aver_phys_origin_stats(&ms, &nb, &nc, &na, &np, &ns) == 1 && nb == 11 && ns == 1 && nc == 0, "stats describe the last shift: 11 bodies, 1 static");
    check(aver_phys_origin_stats(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == 1, "every out is optional");
    freshWorld();
    check(aver_phys_origin_generation() == 0, "a new world starts at generation 0");
    aver_phys_origin_total(t);
    check(t[0] == 0.0 && t[1] == 0.0, "with a zero total");
    check(aver_phys_origin_stats(&ms, &nb, &nc, &na, &np, &ns) == 0 && nb == 0, "and no stats");
    aver_phys_shutdown();
}

static void testManyStatics() {
    AVER_INFO("=== many statics ===");
    freshWorld();
    floorAt(kX0, 0);
    for (int i = 0; i < 4097; ++i) aver_phys_add_static_box(kX0 + static_cast<float>(i % 70) * 300.0f, static_cast<float>(i / 70) * 300.0f, 25.0f, 50, 50, 25);
    aver_phys_add_dynamic_box(kX0, -500.0f, 200.0f, 20, 20, 20, 5.0f);
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    int32_t nb = 0, ns = 0;
    aver_phys_origin_stats(nullptr, &nb, nullptr, nullptr, nullptr, &ns);
    check(ns == 4098 && nb == 4099, "4098 statics (4097 + the floor) counted among " + std::to_string(nb) + " bodies (" + std::to_string(ns) + ")");
    // Over 4,096 statics schedules OptimizeBroadPhase for the top of the next step: the step must run and queries must still answer.
    check(aver_phys_step(kDt) == 1, "the next step runs");
    float pt[3] = {0, 0, 0}, nm[3] = {0, 0, 1};
    int32_t ent = 0;
    check(aver_phys_raycast(kX0 - static_cast<float>(kStep), 0, 500, 0, 0, -1, 1000, pt, nm, &ent) != 0, "and a ray still hits a static at the shifted place");
    aver_phys_shutdown();
}

// ---- 9. joints to the world (P-2): the anchor body -----------------------------------------------------------------------

static float xyDist(const P3& a, float bx, float by) { return std::sqrt((a.v[0] - bx) * (a.v[0] - bx) + (a.v[1] - by) * (a.v[1] - by)); }

static void testWorldJoints() {
    AVER_INFO("=== hinge, slider and path joints to the world hold across a shift ===");
    freshWorld();

    // A door on a vertical hinge at its left edge. Nothing else holds it up: if the world side stayed put the shift would drag it 16 km back.
    const float hingeX = kX0 - 50.0f;
    const int32_t door = aver_phys_add_dynamic_box(kX0, 0.0f, 200.0f, 50, 5, 100, 20.0f);
    const float hp[3] = {hingeX, 0.0f, 200.0f}, hAxis[3] = {0, 0, 1}, hNormal[3] = {1, 0, 0};
    const int32_t hinge = aver_phys_joint_hinge(door, AVER_PHYS_WORLD_BODY, hp, hAxis, hNormal, -1.5f, 1.5f);
    check(hinge != 0, "a hinge to the world exists");
    aver_phys_body_set_angular_velocity(door, 0.0f, 0.0f, 1.0f);

    // A box on a horizontal slider (axis X, +-200 cm), pushed along it.
    const int32_t sbox = aver_phys_add_dynamic_box(kX0, 400.0f, 600.0f, 20, 20, 20, 5.0f);
    const float sp[3] = {kX0, 400.0f, 600.0f}, sAxis[3] = {1, 0, 0}, sNormal[3] = {0, 0, 1};
    const int32_t slider = aver_phys_joint_slider(sbox, AVER_PHYS_WORLD_BODY, sp, sAxis, sNormal, -200.0f, 200.0f);
    check(slider != 0, "a slider to the world exists");
    aver_phys_body_set_velocity(sbox, 100.0f, 0.0f, 0.0f);

    // A car on a rail: a polyline along +X. The path belongs to body A and body B rides it, so the world is A and the car is B.
    const int32_t cart = aver_phys_add_dynamic_box(kX0, 800.0f, 500.0f, 20, 20, 20, 5.0f);
    const float rail[6] = {kX0, 800.0f, 500.0f, kX0 + 1000.0f, 800.0f, 500.0f};
    const int32_t path = aver_phys_joint_path(AVER_PHYS_WORLD_BODY, cart, rail, 2, 0, -1.0f);
    check(path != 0, "a path joint to the world exists");
    aver_phys_body_set_velocity(cart, 200.0f, 0.0f, 0.0f);

    steps(20);
    const P3 doorB = posOf(door), sboxB = posOf(sbox), cartB = posOf(cart);
    float hingeAngleB = 0.0f, sliderB = 0.0f;
    aver_phys_joint_value(hinge, &hingeAngleB);
    aver_phys_joint_value(slider, &sliderB);
    check(xyDist(doorB, hingeX, 0.0f) < 50.5f && xyDist(doorB, hingeX, 0.0f) > 49.5f, "before: the door's centre is 50 cm from its hinge");
    check(sboxB.v[0] > kX0 + 5.0f, "before: the slider box moved along its axis (" + std::to_string(sboxB.v[0] - kX0) + " cm)");

    const int32_t count = aver_phys_body_count();
    check(aver_phys_origin_prepare(kStep, 0.0, 0.0) == count + 3, "prepare counts the bodies and the three world anchors");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    int32_t anchors = 0, pulleys = 0;
    aver_phys_origin_stats(nullptr, nullptr, nullptr, &anchors, &pulleys, nullptr);
    check(anchors == 3 && pulleys == 0, "stats: three anchors moved, no pulley (" + std::to_string(anchors) + ", " + std::to_string(pulleys) + ")");

    float hingeAngleA = 0.0f, sliderA = 0.0f;
    aver_phys_joint_value(hinge, &hingeAngleA);
    aver_phys_joint_value(slider, &sliderA);
    check(near(hingeAngleA, hingeAngleB, 1.0e-4f), "the hinge angle is unchanged by the shift (" + std::to_string(hingeAngleA) + " vs " + std::to_string(hingeAngleB) + ")");
    check(near(sliderA, sliderB, 0.01f), "the slider offset is unchanged by the shift (" + std::to_string(sliderA) + " vs " + std::to_string(sliderB) + " cm)");

    const float newHingeX = hingeX - static_cast<float>(kStep);
    float worstDoor = 0.0f, worstSlideYZ = 0.0f, worstRailYZ = 0.0f, worstDoorZ = 0.0f;
    for (int i = 0; i < 150; ++i) {
        aver_phys_step(kDt);
        const P3 d = posOf(door), s = posOf(sbox), c = posOf(cart);
        worstDoor = std::fmax(worstDoor, std::fabs(xyDist(d, newHingeX, 0.0f) - 50.0f));
        worstDoorZ = std::fmax(worstDoorZ, std::fabs(d.v[2] - 200.0f));
        worstSlideYZ = std::fmax(worstSlideYZ, std::fmax(std::fabs(s.v[1] - 400.0f), std::fabs(s.v[2] - 600.0f)));
        worstRailYZ = std::fmax(worstRailYZ, std::fmax(std::fabs(c.v[1] - 800.0f), std::fabs(c.v[2] - 500.0f)));
    }
    check(worstDoor < 1.0f && worstDoorZ < 1.0f, "the door stayed on its hinge for 2.5 s (radius off by " + std::to_string(worstDoor) + " cm, z by " + std::to_string(worstDoorZ) + ")");
    float hingeNow = 0.0f;
    aver_phys_joint_value(hinge, &hingeNow);
    check(std::fabs(hingeNow) <= 1.5f + 0.02f, "and its swing limit still holds (angle " + std::to_string(hingeNow) + " rad against 1.5)");
    // Gravity sags a 5 kg box ~2 cm over the 200 cm run (Jolt's slider under load; measured the same with no shift and at origin 0).
    check(worstSlideYZ < 3.0f, "the slider box stayed on its axis (off by " + std::to_string(worstSlideYZ) + " cm)");
    float sliderNow = 0.0f;
    aver_phys_joint_value(slider, &sliderNow);
    // The offset is body B (the world) relative to body A (the box), so travel along +X reads negative.
    check(-sliderNow > 150.0f && -sliderNow < 201.0f, "and stopped at its limit, 200 cm (" + std::to_string(-sliderNow) + ")");
    const P3 cartNow = posOf(cart);
    check(worstRailYZ < 1.0f && cartNow.v[0] > kX0 - static_cast<float>(kStep) + 100.0f && cartNow.v[0] < kX0 - static_cast<float>(kStep) + 1001.0f,
          "the cart stayed on the shifted rail and rolled along it (x " + std::to_string(cartNow.v[0] - (kX0 - static_cast<float>(kStep))) + " cm, off by " + std::to_string(worstRailYZ) + ")");

    // Removing a joint takes its anchor with it (the body count is back to the real bodies) and a second shift still works.
    check(aver_phys_joint_remove(hinge) == 1 && aver_phys_joint_remove(slider) == 1 && aver_phys_joint_remove(path) == 1, "all three joints are removed");
    check(aver_phys_origin_prepare(kStep, 0.0, 0.0) == aver_phys_body_count(), "and no anchor is left to move");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "a second shift is accepted");
    aver_phys_shutdown();
}

// ---- 10. pulleys (P-2): rebuilt around their fixed points ----------------------------------------------------------------

static float len3(const P3& a, float x, float y, float z) { return std::sqrt((a.v[0] - x) * (a.v[0] - x) + (a.v[1] - y) * (a.v[1] - y) + (a.v[2] - z) * (a.v[2] - z)); }

static void testPulley() {
    AVER_INFO("=== a pulley is rebuilt around its fixed points ===");
    freshWorld();
    const float ax = kX0 - 100.0f, bx = kX0 + 100.0f;
    const int32_t a = aver_phys_add_dynamic_box(ax, 0.0f, 300.0f, 15, 15, 15, 12.0f);
    const int32_t b = aver_phys_add_dynamic_box(bx, 0.0f, 300.0f, 15, 15, 15, 10.0f);
    const float bpA[3] = {ax, 0, 300}, fpA[3] = {ax, 0, 600}, bpB[3] = {bx, 0, 300}, fpB[3] = {bx, 0, 600};
    const int32_t pulley = aver_phys_joint_pulley(a, b, bpA, fpA, bpB, fpB, 1.0f, -1.0f, -1.0f);
    check(pulley != 0, "a pulley between two bodies exists");

    // A second pulley whose B end is the world: a box hung on a rope whose far end is bolted to a world point.
    const float cx = kX0 - 600.0f;
    const int32_t c = aver_phys_add_dynamic_box(cx, 0.0f, 300.0f, 15, 15, 15, 10.0f);
    const float bpC[3] = {cx, 0, 300}, fpC[3] = {cx, 0, 600}, bpW[3] = {cx + 200.0f, 0, 300}, fpW[3] = {cx + 200.0f, 0, 600};
    const int32_t pulleyW = aver_phys_joint_pulley(c, AVER_PHYS_WORLD_BODY, bpC, fpC, bpW, fpW, 1.0f, -1.0f, -1.0f);
    check(pulleyW != 0, "and a pulley to the world");

    const int32_t joints = aver_phys_joint_count();
    int32_t bodyA = -1, bodyB = -1;
    aver_phys_joint_bodies(pulley, &bodyA, &bodyB);
    steps(20);
    const P3 aB = posOf(a), bB = posOf(b);
    const float length0 = len3(aB, ax, 0, 600) + len3(bB, bx, 0, 600);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    check(snap != 0, "a snapshot is taken before the shift");

    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    int32_t anchors = 0, pulleys = 0;
    aver_phys_origin_stats(nullptr, nullptr, nullptr, &anchors, &pulleys, nullptr);
    check(pulleys == 2 && anchors == 1, "stats: two pulleys rebuilt, one anchor moved (" + std::to_string(pulleys) + ", " + std::to_string(anchors) + ")");
    check(aver_phys_joint_count() == joints, "the joint count is unchanged");
    int32_t bodyA2 = -2, bodyB2 = -2;
    check(aver_phys_joint_bodies(pulley, &bodyA2, &bodyB2) == 1 && bodyA2 == bodyA && bodyB2 == bodyB, "the handle still names the same two bodies");
    check(aver_phys_snapshot_restore(snap, 0) == 0 && aver_phys_snapshot_last_result() == AVER_PHYS_RESTORE_TOPOLOGY,
          "a snapshot from before the rebuild is refused: Jolt restores constraints by index (result " + std::to_string(aver_phys_snapshot_last_result()) + ")");

    const float kd = static_cast<float>(kStep);
    float worstRope = 0.0f, worstHang = 0.0f;
    const P3 cB = posOf(c);
    for (int i = 0; i < 100; ++i) {
        aver_phys_step(kDt);
        const P3 pa = posOf(a), pb = posOf(b), pc = posOf(c);
        worstRope = std::fmax(worstRope, std::fabs(len3(pa, ax - kd, 0, 600) + len3(pb, bx - kd, 0, 600) - length0));
        worstHang = std::fmax(worstHang, len3(pc, cB.v[0], cB.v[1], cB.v[2]));
    }
    const P3 aA = posOf(a), bA = posOf(b);
    check(aA.v[2] < aB.v[2] - 20.0f && bA.v[2] > bB.v[2] + 20.0f, "the heavier side went down and the lighter one up (" + std::to_string(aA.v[2] - aB.v[2]) + " and " + std::to_string(bA.v[2] - bB.v[2]) + " cm)");
    check(worstRope < 2.0f, "the rope length is conserved to 2 cm after the rebuild (worst " + std::to_string(worstRope) + ")");
    check(worstHang < 2.0f, "a box hung from a pulley whose far end is bolted to the world stayed where it was hung (moved at most " + std::to_string(worstHang) + " cm)");
    aver_phys_shutdown();
}

// ---- 11. hooks run once, in order, after the bodies moved (P-1/P-2 seam) --------------------------------------------------

// A test seam in PhysicsOrigin.cpp, deliberately in no ABI header (the module-internal registry is not exported).
extern "C" AVER_PHYS_API int32_t aver_phys_origin_test_hook(const char* name, void (*fn)(double, double, double, void*), void* user);

struct HookLog {
    std::vector<std::string> order;
    int32_t body = 0;
    float xAtHook = 0.0f;
    double dx = 0.0;
    int32_t nestedShift = 99;
};
static void hookA(double dx, double, double, void* user) {
    HookLog& l = *static_cast<HookLog*>(user);
    l.order.push_back("a");
    l.dx = dx;
    l.xAtHook = posOf(l.body).v[0];
    l.nestedShift = aver_phys_origin_shift(kStep, 0.0, 0.0);   // a hook calling back into the shift is refused
}
static void hookB(double, double, double, void* user) { static_cast<HookLog*>(user)->order.push_back("b"); }

static void testHooks() {
    AVER_INFO("=== hooks ===");
    freshWorld();
    HookLog log;
    log.body = aver_phys_add_dynamic_box(kX0, 0.0f, 500.0f, 20, 20, 20, 5.0f);
    aver_phys_origin_test_hook("test.a", &hookA, &log);
    aver_phys_origin_test_hook("test.b", &hookB, &log);
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    check(log.order.size() == 2 && log.order[0] == "a" && log.order[1] == "b", "each hook ran once, in registration order");
    check(log.dx == kStep, "and was handed the origin's move (" + std::to_string(log.dx) + ")");
    check(log.xAtHook == kX0 - static_cast<float>(kStep), "after the bodies had moved (hook saw x " + std::to_string(log.xAtHook) + ")");
    check(log.nestedShift == 0, "a hook that calls the shift is refused");
    check(aver_phys_origin_generation() == 1, "and that did not count as a shift");

    // Registering under an existing name replaces it in place (order kept); a null function removes it.
    log.order.clear();
    aver_phys_origin_test_hook("test.a", &hookB, &log);
    aver_phys_origin_shift(kStep, 0.0, 0.0);
    check(log.order.size() == 2 && log.order[0] == "b" && log.order[1] == "b", "a second registration under one name replaced the first in place");
    log.order.clear();
    aver_phys_origin_test_hook("test.a", nullptr, nullptr);
    aver_phys_origin_shift(kStep, 0.0, 0.0);
    check(log.order.size() == 1, "and a null function removes a hook");
    aver_phys_origin_test_hook("test.b", nullptr, nullptr);
    aver_phys_shutdown();
}

// ---- 12. snapshots across a shift (P-3) -------------------------------------------------------------------------------------

static uint32_t headerWord52(int32_t snap) {
    std::vector<uint8_t> b(static_cast<size_t>(aver_phys_snapshot_bytes(snap)));
    aver_phys_snapshot_read(snap, b.data(), static_cast<int32_t>(b.size()));
    uint32_t v = 0xFFFFFFFFu;
    if (b.size() >= 56) std::memcpy(&v, b.data() + 52, sizeof v);
    return v;
}

struct SnapScene {
    std::vector<int32_t> boxes;
    int32_t ch = 0, door = 0, joint = 0, floor = 0;
};

static SnapScene buildSnapScene() {
    SnapScene s;
    s.floor = floorAt(kX0, 0);
    for (int i = 0; i < 5; ++i) s.boxes.push_back(aver_phys_add_dynamic_box(kX0 + static_cast<float>(i) * 80.0f, 200.0f, 60.0f + static_cast<float>(i) * 40.0f, 20, 20, 20, 5.0f));
    s.ch = aver_phys_character_create(30.0f, 180.0f, kX0 - 400.0f, 0.0f, 95.0f);
    s.door = aver_phys_add_dynamic_box(kX0, -400.0f, 400.0f, 50, 5, 100, 20.0f);
    const float hp[3] = {kX0 - 50.0f, -400.0f, 400.0f}, hAxis[3] = {0, 0, 1}, hNormal[3] = {1, 0, 0};
    s.joint = aver_phys_joint_hinge(s.door, AVER_PHYS_WORLD_BODY, hp, hAxis, hNormal, -1.5f, 1.5f);
    aver_phys_body_set_angular_velocity(s.door, 0.0f, 0.0f, 1.0f);
    return s;
}

static void testSnapshot() {
    AVER_INFO("=== a snapshot restored across a shift lands in the current frame ===");
    freshWorld();
    const SnapScene sc = buildSnapScene();
    steps(40);

    // A world that never shifted writes the blob it always did: header word 52 stays zero.
    const int32_t first = aver_phys_snapshot_capture(0, 0);
    check(first != 0 && headerWord52(first) == 0, "a blob of an unshifted world has a zero origin word");
    const int32_t second = aver_phys_snapshot_capture(0, 0);
    check(aver_phys_snapshot_hash(first) == aver_phys_snapshot_hash(second), "and two captures of the same moment hash alike");
    aver_phys_snapshot_release(second);

    std::vector<P3> boxesAtCapture;
    for (int32_t h : sc.boxes) boxesAtCapture.push_back(posOf(h));
    P3 chAtCapture;
    aver_phys_character_position(sc.ch, chAtCapture.v);
    const P3 doorAtCapture = posOf(sc.door);

    steps(40);   // everything moves on
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    check(aver_phys_snapshot_restore(first, 0) == 1, "the snapshot taken before the shift restores (result " + std::to_string(aver_phys_snapshot_last_result()) + ")");

    const float kd = static_cast<float>(kStep);
    int landed = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < sc.boxes.size(); ++i) {
        const P3 p = posOf(sc.boxes[i]);
        const float e = std::fmax(std::fabs(p.v[0] - (boxesAtCapture[i].v[0] - kd)), std::fmax(std::fabs(p.v[1] - boxesAtCapture[i].v[1]), std::fabs(p.v[2] - boxesAtCapture[i].v[2])));
        worst = std::fmax(worst, e);
        if (e <= 0.3f) ++landed;
    }
    check(landed == 5, "the five boxes are at their captured places minus delta, not at the blob's frame (worst " + std::to_string(worst) + " cm)");
    P3 chNow;
    aver_phys_character_position(sc.ch, chNow.v);
    check(near(chNow.v[0], chAtCapture.v[0] - kd, 0.3f) && near(chNow.v[1], chAtCapture.v[1], 0.01f) && near(chNow.v[2], chAtCapture.v[2], 0.01f),
          "the character is at its captured place minus delta, once");
    check(aver_phys_character_grounded(sc.ch) == 1, "and still grounded");
    const P3 doorNow = posOf(sc.door);
    check(near(doorNow.v[0], doorAtCapture.v[0] - kd, 0.3f), "the hinged door too");

    // Statics: the floor was not in the blob (no SNAP_STATICS), so the restore must not touch it; the shift already put it in the new frame.
    float pt[3] = {0, 0, 0}, nm[3] = {0, 0, 1};
    int32_t ent = 0;
    check(aver_phys_raycast(kX0 - kd + 400.0f, 600.0f, 500.0f, 0, 0, -1, 1000.0f, pt, nm, &ent) == sc.floor && near(pt[2], 0.0f, 0.01f),
          "the floor, left out of the blob, is at its shifted place");
    check(aver_phys_raycast(kX0 + 400.0f, 600.0f, 500.0f, 0, 0, -1, 1000.0f, pt, nm, &ent) == 0, "and not at the old one");

    // The door's world anchor is in no blob and must not have been shifted a second time: the hinge still holds.
    float worstDoor = 0.0f;
    for (int i = 0; i < 90; ++i) {
        aver_phys_step(kDt);
        const P3 d = posOf(sc.door);
        worstDoor = std::fmax(worstDoor, std::fabs(xyDist(d, kX0 - 50.0f - kd, -400.0f) - 50.0f));
    }
    check(worstDoor < 1.0f, "the world hinge was not shifted twice: the door stayed 50 cm from it for 1.5 s (off by " + std::to_string(worstDoor) + ")");
    check(aver_phys_origin_generation() == 1, "restoring is not a shift");
    aver_phys_snapshot_release(first);

    // A blob taken AFTER a shift carries the generation, and restores across the next one.
    const int32_t later = aver_phys_snapshot_capture(0, 0);
    check(later != 0 && headerWord52(later) == 1, "a blob taken after one shift has origin word 1");
    std::vector<P3> boxesLater;
    for (int32_t h : sc.boxes) boxesLater.push_back(posOf(h));
    steps(30);
    check(aver_phys_origin_shift(0.0, kStep, 0.0) == 1, "a Y shift is accepted");
    check(aver_phys_snapshot_restore(later, 0) == 1, "the later blob restores across it");
    int landedY = 0;
    for (size_t i = 0; i < sc.boxes.size(); ++i) {
        const P3 p = posOf(sc.boxes[i]);
        if (near(p.v[0], boxesLater[i].v[0], 0.3f) && near(p.v[1], boxesLater[i].v[1] - kd, 0.4f) && near(p.v[2], boxesLater[i].v[2], 0.3f)) ++landedY;
    }
    check(landedY == 5, "all five are at their captured places minus the Y delta");
    aver_phys_snapshot_release(later);
    aver_phys_shutdown();

    // With statics in the blob they are moved by the difference too.
    freshWorld();
    const SnapScene sd = buildSnapScene();
    steps(10);
    const int32_t withStatics = aver_phys_snapshot_capture(0, AVER_PHYS_SNAP_ALL | AVER_PHYS_SNAP_STATICS);
    check(withStatics != 0, "a snapshot with statics");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    check(aver_phys_snapshot_restore(withStatics, 0) == 1, "and restores");
    check(aver_phys_raycast(kX0 - kd + 400.0f, 600.0f, 500.0f, 0, 0, -1, 1000.0f, pt, nm, &ent) == sd.floor && near(pt[2], 0.0f, 0.01f),
          "the floor is at its shifted place after the restore, not back in the blob's frame");
    check(aver_phys_raycast(kX0 + 400.0f, 600.0f, 500.0f, 0, 0, -1, 1000.0f, pt, nm, &ent) == 0, "and nothing is left at the old one");
    aver_phys_shutdown();
}

// ---- 13. the shape cache key has no position in it ----------------------------------------------------------------------

static void testShapeCacheKey() {
    AVER_INFO("=== shape cache: a blob and its key survive a shift ===");
    freshWorld();
    // A small bumpy mesh: 9 x 9 vertices, 50 cm apart.
    std::vector<float> v;
    std::vector<int32_t> idx;
    for (int iy = 0; iy < 9; ++iy)
        for (int ix = 0; ix < 9; ++ix) { v.push_back(static_cast<float>(ix) * 50.0f - 200.0f); v.push_back(static_cast<float>(iy) * 50.0f - 200.0f); v.push_back(10.0f * std::sin(static_cast<float>(ix + iy))); }
    for (int iy = 0; iy < 8; ++iy)
        for (int ix = 0; ix < 8; ++ix) { const int32_t a = iy * 9 + ix, b = a + 1, c = a + 10, d = a + 9; idx.insert(idx.end(), {a, b, c, a, c, d}); }
    const int32_t shape = aver_phys_create_mesh_shape(v.data(), static_cast<int32_t>(v.size() / 3), idx.data(), static_cast<int32_t>(idx.size()));
    check(shape != 0, "a mesh shape");
    const uint64_t key = 0xA11CE5EEDull;
    auto blob = [&]() {
        const int32_t need = aver_phys_shape_save(shape, key, nullptr, 0);
        std::vector<uint8_t> b(need > 0 ? static_cast<size_t>(need) : 0u);
        if (need > 0) aver_phys_shape_save(shape, key, b.data(), need);
        return b;
    };
    const int32_t body = aver_phys_add_mesh_shape_body(shape, kX0, 0.0f, 0.0f, 0, 0, 0, 1, 1, 1, 1);
    check(body != 0, "placed far from the origin");
    const std::vector<uint8_t> before = blob();
    check(!before.empty(), "it saves");
    check(aver_phys_origin_shift(kStep, 0.0, 0.0) == 1, "shift accepted");
    const std::vector<uint8_t> after = blob();
    check(before == after, "the blob under the same key is byte for byte the same after the shift: no position is in it");
    const int32_t again = aver_phys_shape_restore(after.data(), static_cast<int32_t>(after.size()), key);
    check(again != 0, "and it restores under the same key");
    float pt[3] = {0, 0, 0}, nm[3] = {0, 0, 1};
    int32_t ent = 0;
    check(aver_phys_raycast(kX0 - static_cast<float>(kStep), 0.0f, 400.0f, 0, 0, -1, 1000.0f, pt, nm, &ent) == body, "the placed shape is hit at its shifted place");
    aver_phys_shutdown();
}

int main() {
    AVER_INFO("=== PhysicsOriginTest ===");
    testIdentity();
    testExactness();
    testSleepingPile();
    testAwakePile();
    testCharacter();
    testVehicle();
    testPendingContact();
    testSensor();
    testCloth();
    testQueries();
    testRefusals();
    testCounters();
    testManyStatics();
    testWorldJoints();
    testPulley();
    testHooks();
    testSnapshot();
    testShapeCacheKey();
    aver_phys_shutdown();
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
