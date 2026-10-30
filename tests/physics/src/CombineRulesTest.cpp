// CombineRulesTest -- the material combine rule: Jolt's (default) or the material rule's (physics_surface_abi.h, G2).
//
// Every number is read from a REAL contact (aver_phys_contact_detail: the friction and restitution the contact applies), so the
// hook, the terrain-sampler path and the leaf-material path are the ones measured, not a copy of the formula. A dropped ball also
// shows the rule in motion, and a snapshot restore shows the mode does not disturb replay.
//
//   1. defaults: the mode is Jolt's, flags are stored but ignored, bad arguments are refused,
//   2. Material friction: min, and max when either side is ROUGH,
//   3. Material restitution: clamped sum, and an ABSORBENT side subtracts,
//   4. back to Jolt: the same bodies give Jolt's numbers again,
//   5. leaf surfaces and the terrain sampler use the same function,
//   6. a ball bounces higher under the material rule's sum than under Jolt's max,
//   7. a snapshot taken in Material mode replays to the same hash.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_shapes_abi.h"
#include "aver/physics/physics_state_abi.h"
#include "aver/physics/physics_surface_abi.h"
#include "aver/physics/physics_water_abi.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static const float kDt = 1.0f / 60.0f;
static const int32_t kBadHandle = -1, kNotInitialised = -3, kInvalidArgument = -6;

static void stepN(int n) {
    for (int i = 0; i < n; ++i) aver_phys_step(kDt);
}

struct Applied { bool found = false; float friction = -1.0f, restitution = -1.0f; };

// Steps until a contact between `a` and `b` is reported, and returns the friction and restitution it applies.
static Applied appliedBetween(int32_t a, int32_t b, int maxSteps = 120) {
    Applied r;
    for (int s = 0; s < maxSteps && !r.found; ++s) {
        aver_phys_step(kDt);
        const int32_t n = aver_phys_contact_count();
        for (int32_t i = 0; i < n && !r.found; ++i) {
            int32_t x = 0, y = 0;
            float pt[3], nrm[3];
            aver_phys_contact_get(i, &x, &y, pt, nrm);
            if (!((x == a && y == b) || (x == b && y == a))) continue;
            float f[AVER_PHYS_CONF_COUNT];
            int32_t iv[AVER_PHYS_CONI_COUNT];
            if (aver_phys_contact_detail(i, f, AVER_PHYS_CONF_COUNT, iv, AVER_PHYS_CONI_COUNT) != 1) continue;
            r.found = true;
            r.friction = f[AVER_PHYS_CONF_FRICTION];
            r.restitution = f[AVER_PHYS_CONF_RESTITUTION];
        }
    }
    return r;
}

// A fresh world with a static floor (top at z = 0) and a dynamic box about to land on it, with the given numbers and flags.
struct Pair { int32_t floor = 0, box = 0; };
static Pair makePair(float floorF, float floorR, int32_t floorFlags, float boxF, float boxR, int32_t boxFlags) {
    Pair p;
    p.floor = aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
    p.box = aver_phys_add_dynamic_box(0, 0, 60.0f, 20.0f, 20.0f, 20.0f, 10.0f);
    aver_phys_body_set_friction(p.floor, floorF);
    aver_phys_body_set_restitution(p.floor, floorR);
    aver_phys_body_set_friction(p.box, boxF);
    aver_phys_body_set_restitution(p.box, boxR);
    if (floorFlags) aver_phys_body_set_combine_flags(p.floor, floorFlags);
    if (boxFlags) aver_phys_body_set_combine_flags(p.box, boxFlags);
    return p;
}

// One world, one contact: the numbers a pair applies under `mode`.
static Applied measure(int32_t mode, float floorF, float floorR, int32_t floorFlags, float boxF, float boxR, int32_t boxFlags) {
    aver_phys_init();
    aver_phys_set_combine_mode(mode);
    const Pair p = makePair(floorF, floorR, floorFlags, boxF, boxR, boxFlags);
    const Applied a = appliedBetween(p.floor, p.box);
    aver_phys_shutdown();
    return a;
}

static const int32_t ROUGH = AVER_PHYS_BODY_COMBINE_ROUGH, ABSORBENT = AVER_PHYS_BODY_COMBINE_ABSORBENT;
static const int32_t JOLT = AVER_PHYS_COMBINE_JOLT, MATERIAL = AVER_PHYS_COMBINE_MATERIAL;

static void testDefaultsAndArguments() {
    AVER_INFO("=== defaults, flags stored, bad arguments ===");
    check(aver_phys_surface_abi_version() == AVER_PHYS_SURFACE_ABI_VERSION && AVER_PHYS_SURFACE_ABI_VERSION >= 2, "the surface ABI version is 2 or later");
    check(aver_phys_combine_mode() == -1 && aver_phys_last_error() == kNotInitialised, "no world: the mode reads -1 (NotInitialised)");
    check(aver_phys_set_combine_mode(MATERIAL) == 0 && aver_phys_last_error() == kNotInitialised, "and cannot be set");

    aver_phys_init();
    check(aver_phys_combine_mode() == JOLT, "a new world uses Jolt's rules");
    check(aver_phys_set_combine_mode(7) == 0 && aver_phys_last_error() == kInvalidArgument && aver_phys_combine_mode() == JOLT, "an unknown mode is refused");
    const Pair p = makePair(0.3f, 0.2f, 0, 0.6f, 0.5f, 0);
    check(aver_phys_body_combine_flags(p.box) == 0, "a body starts with no flags");
    check(aver_phys_body_set_combine_flags(p.box, ROUGH | ABSORBENT) == 1 && aver_phys_body_combine_flags(p.box) == (ROUGH | ABSORBENT), "flags set and read back");
    check(aver_phys_body_set_combine_flags(p.box, 4) == 0 && aver_phys_last_error() == kInvalidArgument && aver_phys_body_combine_flags(p.box) == (ROUGH | ABSORBENT),
          "an unknown bit is refused and changes nothing");
    check(aver_phys_body_set_combine_flags(p.box, 0) == 1 && aver_phys_body_combine_flags(p.box) == 0, "0 clears them");
    check(aver_phys_body_set_combine_flags(9999, ROUGH) == 0 && aver_phys_last_error() == kBadHandle && aver_phys_body_combine_flags(9999) == -1, "a dead handle is refused");
    check(aver_phys_set_combine_mode(MATERIAL) == 1 && aver_phys_combine_mode() == MATERIAL, "Material mode sets");
    aver_phys_shutdown();
    aver_phys_init();
    check(aver_phys_combine_mode() == JOLT, "the mode belongs to the world: a new world starts on Jolt's rules");
    aver_phys_shutdown();

    // Jolt's numbers, flags set or not.
    const Applied j = measure(JOLT, 0.3f, 0.2f, 0, 0.6f, 0.5f, 0);
    check(j.found && near(j.friction, std::sqrt(0.3f * 0.6f), 1e-4f), "Jolt mode friction is sqrt(0.3 * 0.6) = " + f2s(std::sqrt(0.18f)) + ", got " + f2s(j.friction));
    check(near(j.restitution, 0.5f, 1e-4f), "Jolt mode restitution is the larger, 0.5, got " + f2s(j.restitution));
    const Applied jf = measure(JOLT, 0.3f, 0.2f, ROUGH | ABSORBENT, 0.6f, 0.5f, ROUGH | ABSORBENT);
    check(jf.found && near(jf.friction, j.friction, 1e-6f) && near(jf.restitution, j.restitution, 1e-6f), "ROUGH and ABSORBENT change nothing in Jolt mode");
}

static void testMaterialFriction() {
    AVER_INFO("=== Material friction: min, or max when either side is rough ===");
    const Applied m = measure(MATERIAL, 0.3f, 0.0f, 0, 0.6f, 0.0f, 0);
    check(m.found && near(m.friction, 0.3f, 1e-4f), "friction 0.3 against 0.6 is the smaller, 0.3, got " + f2s(m.friction));
    const Applied m2 = measure(MATERIAL, 0.8f, 0.0f, 0, 0.1f, 0.0f, 0);
    check(m2.found && near(m2.friction, 0.1f, 1e-4f), "and 0.8 against 0.1 is 0.1 (either side), got " + f2s(m2.friction));
    const Applied rf = measure(MATERIAL, 0.3f, 0.0f, ROUGH, 0.6f, 0.0f, 0);
    check(rf.found && near(rf.friction, 0.6f, 1e-4f), "a rough floor makes it the larger, 0.6, got " + f2s(rf.friction));
    const Applied rb = measure(MATERIAL, 0.3f, 0.0f, 0, 0.6f, 0.0f, ROUGH);
    check(rb.found && near(rb.friction, 0.6f, 1e-4f), "a rough box does too, got " + f2s(rb.friction));
    const Applied rb2 = measure(MATERIAL, 0.9f, 0.0f, 0, 0.2f, 0.0f, ROUGH);
    check(rb2.found && near(rb2.friction, 0.9f, 1e-4f), "the larger whichever side it is on (0.9 floor, rough 0.2 box), got " + f2s(rb2.friction));
    const Applied both = measure(MATERIAL, 0.3f, 0.0f, ROUGH, 0.6f, 0.0f, ROUGH);
    check(both.found && near(both.friction, 0.6f, 1e-4f), "both rough is still the larger, got " + f2s(both.friction));
    const Applied abs = measure(MATERIAL, 0.3f, 0.0f, ABSORBENT, 0.6f, 0.0f, 0);
    check(abs.found && near(abs.friction, 0.3f, 1e-4f), "ABSORBENT does not touch friction, got " + f2s(abs.friction));
}

static void testMaterialRestitution() {
    AVER_INFO("=== Material restitution: clamped sum, and absorbent subtracts ===");
    const Applied s = measure(MATERIAL, 0.5f, 0.3f, 0, 0.5f, 0.4f, 0);
    check(s.found && near(s.restitution, 0.7f, 1e-4f), "0.3 with 0.4 is 0.7, got " + f2s(s.restitution));
    const Applied c = measure(MATERIAL, 0.5f, 0.8f, 0, 0.5f, 0.7f, 0);
    check(c.found && near(c.restitution, 1.0f, 1e-4f), "0.8 with 0.7 clamps to 1, got " + f2s(c.restitution));
    const Applied z = measure(MATERIAL, 0.5f, 0.0f, 0, 0.5f, 0.0f, 0);
    check(z.found && near(z.restitution, 0.0f, 1e-6f), "0 with 0 stays 0, got " + f2s(z.restitution));
    const Applied af = measure(MATERIAL, 0.5f, 0.2f, ABSORBENT, 0.5f, 0.5f, 0);
    check(af.found && near(af.restitution, 0.3f, 1e-4f), "an absorbent floor (0.2) subtracts from the box's 0.5: 0.3, got " + f2s(af.restitution));
    const Applied ab = measure(MATERIAL, 0.5f, 0.5f, 0, 0.5f, 0.2f, ABSORBENT);
    check(ab.found && near(ab.restitution, 0.3f, 1e-4f), "an absorbent box (0.2) against a floor of 0.5: 0.3, got " + f2s(ab.restitution));
    const Applied under = measure(MATERIAL, 0.5f, 0.5f, ABSORBENT, 0.5f, 0.2f, 0);
    check(under.found && near(under.restitution, 0.0f, 1e-6f), "an absorbent 0.5 against 0.2 would be -0.3: clamped to 0, got " + f2s(under.restitution));
    const Applied both = measure(MATERIAL, 0.5f, 0.3f, ABSORBENT, 0.5f, 0.3f, ABSORBENT);
    check(both.found && near(both.restitution, 0.0f, 1e-6f), "two absorbent sides give 0, got " + f2s(both.restitution));
    const Applied fr = measure(MATERIAL, 0.5f, 0.3f, ABSORBENT, 0.7f, 0.5f, ABSORBENT);
    check(fr.found && near(fr.friction, 0.5f, 1e-4f), "absorbent leaves the friction rule alone (min 0.5), got " + f2s(fr.friction));
}

static void testBackToJolt() {
    AVER_INFO("=== switching the mode on a live world ===");
    aver_phys_init();
    aver_phys_set_combine_mode(MATERIAL);
    aver_phys_set_combine_mode(JOLT);
    const Pair p = makePair(0.3f, 0.2f, ROUGH, 0.6f, 0.5f, ABSORBENT);
    const Applied a = appliedBetween(p.floor, p.box);
    check(a.found && near(a.friction, std::sqrt(0.18f), 1e-4f) && near(a.restitution, 0.5f, 1e-4f), "Material then Jolt again gives Jolt's numbers (" + f2s(a.friction) + ", " + f2s(a.restitution) + ")");
    aver_phys_shutdown();

    // A flag on a static body takes effect against a dynamic one, and a removed body's are forgotten.
    aver_phys_init();
    aver_phys_set_combine_mode(MATERIAL);
    const int32_t floor = aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
    aver_phys_body_set_combine_flags(floor, ROUGH);
    aver_phys_body_set_friction(floor, 0.8f);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 60.0f, 20.0f, 20.0f, 20.0f, 10.0f);
    aver_phys_body_set_friction(box, 0.1f);
    const Applied r = appliedBetween(floor, box);
    check(r.found && near(r.friction, 0.8f, 1e-4f), "a rough static floor gives the larger of 0.8 and 0.1, got " + f2s(r.friction));
    aver_phys_remove_body(box);
    check(aver_phys_body_combine_flags(box) == -1, "a removed body's flags read as a dead handle");
    aver_phys_shutdown();
}

// ---- leaf surfaces and the terrain sampler ---------------------------------------------------------------------------

static int32_t shapeBody(int32_t shape, float x, float y, float z, int32_t motion, float mass) {
    const float o[3] = {x, y, z};
    return aver_phys_body_from_shape(shape, o, nullptr, motion, mass, nullptr, 0);
}

static Applied leafPair(int32_t mode, int32_t floorFlags) {
    aver_phys_init();
    aver_phys_set_combine_mode(mode);
    const int32_t ice = aver_phys_surface_register("CombineIce", 0.1f, 0.05f, 0);
    const int32_t rub = aver_phys_surface_register("CombineRubber", 0.9f, 0.2f, 0);
    const int32_t floor = shapeBody(aver_phys_shape_box(5000, 5000, 100, ice, nullptr, 0), 0, 0, -100.0f, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t box = shapeBody(aver_phys_shape_box(20, 20, 20, rub, nullptr, 0), 0, 0, 60.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    if (floorFlags) aver_phys_body_set_combine_flags(floor, floorFlags);
    const Applied a = appliedBetween(floor, box);
    aver_phys_shutdown();
    return a;
}

static void testLeafSurfaces() {
    AVER_INFO("=== leaf materials meet in the same rule ===");
    const Applied j = leafPair(JOLT, 0);
    check(j.found && near(j.friction, 0.3f, 1e-3f) && near(j.restitution, 0.2f, 1e-4f), "Jolt: Ice 0.1 on Rubber 0.9 is sqrt(0.09) = 0.3, restitution max 0.2; got " + f2s(j.friction) + ", " + f2s(j.restitution));
    const Applied g = leafPair(MATERIAL, 0);
    check(g.found && near(g.friction, 0.1f, 1e-4f) && near(g.restitution, 0.25f, 1e-4f), "Material: the same leaves give min 0.1 and sum 0.25; got " + f2s(g.friction) + ", " + f2s(g.restitution));
    const Applied r = leafPair(MATERIAL, ROUGH | ABSORBENT);
    check(r.found && near(r.friction, 0.9f, 1e-4f) && near(r.restitution, 0.15f, 1e-4f), "and with the floor body rough and absorbent: 0.9 and 0.2 - 0.05 = 0.15; got " + f2s(r.friction) + ", " + f2s(r.restitution));
}

struct Sampler { float friction = 0.3f, restitution = 0.4f; };
static int32_t fixedSampler(float, float, float* fr, float* re, void* user) {
    *fr = static_cast<Sampler*>(user)->friction;
    *re = static_cast<Sampler*>(user)->restitution;
    return 1;
}

static Applied terrainPair(int32_t mode, int32_t terrainFlags, int32_t boxFlags) {
    aver_phys_init();
    aver_phys_set_combine_mode(mode);
    static Sampler s;
    const Pair p = makePair(0.2f, 0.0f, terrainFlags, 0.6f, 0.5f, boxFlags);
    aver_phys_body_set_surface_sampled(p.floor, 1);
    aver_phys_set_surface_sampler(&fixedSampler, &s);
    const Applied a = appliedBetween(p.floor, p.box);
    aver_phys_shutdown();
    return a;
}

static void testSurfaceSampler() {
    AVER_INFO("=== the terrain surface sampler uses the same rule ===");
    const Applied j = terrainPair(JOLT, 0, 0);
    check(j.found && near(j.friction, std::sqrt(0.3f * 0.6f), 1e-4f) && near(j.restitution, 0.5f, 1e-4f), "Jolt: sampler 0.3 / 0.4 against a box of 0.6 / 0.5: sqrt(0.18) and 0.5; got " + f2s(j.friction) + ", " + f2s(j.restitution));
    const Applied g = terrainPair(MATERIAL, 0, 0);
    check(g.found && near(g.friction, 0.3f, 1e-4f) && near(g.restitution, 0.9f, 1e-4f), "Material: min 0.3 and sum 0.9; got " + f2s(g.friction) + ", " + f2s(g.restitution));
    const Applied rt = terrainPair(MATERIAL, ROUGH, 0);
    check(rt.found && near(rt.friction, 0.6f, 1e-4f), "a rough terrain body takes the larger, 0.6; got " + f2s(rt.friction));
    const Applied at = terrainPair(MATERIAL, ABSORBENT, 0);
    check(at.found && near(at.restitution, 0.1f, 1e-4f), "an absorbent terrain body subtracts its 0.4 from the box's 0.5: 0.1; got " + f2s(at.restitution));
    const Applied ab = terrainPair(MATERIAL, 0, ABSORBENT);
    check(ab.found && near(ab.restitution, 0.0f, 1e-6f), "an absorbent box (0.5) against the terrain's 0.4 clamps to 0; got " + f2s(ab.restitution));
}

// ---- in motion ---------------------------------------------------------------------------------------------------------

// How high a ball bounces (cm above its resting height) after being dropped from 180 cm above it.
static float bounceApex(int32_t mode, float floorR, float ballR) {
    aver_phys_init();
    aver_phys_set_combine_mode(mode);
    const int32_t floor = aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
    aver_phys_body_set_restitution(floor, floorR);
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 200.0f, 20.0f, 5.0f);
    aver_phys_body_set_restitution(ball, ballR);
    bool rebounded = false;
    float apex = 0.0f;
    for (int i = 0; i < 240; ++i) {
        aver_phys_step(kDt);
        float v[3] = {0, 0, 0}, p[3] = {0, 0, 0};
        aver_phys_body_velocity(ball, v);
        aver_phys_body_position(ball, p);
        if (v[2] > 50.0f) rebounded = true;
        if (rebounded) { apex = std::fmax(apex, p[2] - 20.0f); if (v[2] < 0.0f) break; }
    }
    aver_phys_shutdown();
    return apex;
}

static void testBounce() {
    AVER_INFO("=== a ball bounces higher under the sum than under the max ===");
    const float jolt = bounceApex(JOLT, 0.45f, 0.45f), material = bounceApex(MATERIAL, 0.45f, 0.45f);
    check(jolt > 5.0f && material > 2.0f * jolt, "restitution 0.45 on 0.45 rebounds " + f2s(jolt) + " cm under Jolt's max (0.45) and " + f2s(material) + " cm under the sum (0.9)");
    const float dead = bounceApex(MATERIAL, 0.5f, 0.5f), soak = bounceApex(MATERIAL, 0.5f, 0.0f);
    check(dead > soak, "0.5 on 0.5 (clamped sum 1) rebounds " + f2s(dead) + " cm, 0.5 on 0 only " + f2s(soak));
}

static void testSnapshotReplay() {
    AVER_INFO("=== a snapshot taken in Material mode replays to the same hash ===");
    auto run = [](int32_t mode, uint64_t* hashAfterReplay, int32_t* modeAfter) {
        aver_phys_init();
        aver_phys_set_combine_mode(mode);
        const int32_t floor = aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
        aver_phys_body_set_restitution(floor, 0.4f);
        aver_phys_body_set_combine_flags(floor, ROUGH);
        const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 150.0f, 20.0f, 5.0f);
        aver_phys_body_set_restitution(ball, 0.4f);
        aver_phys_body_set_friction(ball, 0.7f);
        aver_phys_body_set_velocity(ball, 120.0f, 0, 0);
        stepN(20);
        const int32_t snap = aver_phys_snapshot_capture(0, 0);
        stepN(100);
        const uint64_t first = aver_phys_state_hash(0);
        const int32_t restored = aver_phys_snapshot_restore(snap, 0);
        stepN(100);
        const uint64_t second = aver_phys_state_hash(0);
        *modeAfter = aver_phys_combine_mode();
        *hashAfterReplay = second;
        aver_phys_snapshot_release_all();
        aver_phys_shutdown();
        return restored != 0 && first == second;
    };
    uint64_t hg = 0, hj = 0;
    int32_t mg = -1, mj = -1;
    check(run(MATERIAL, &hg, &mg), "Material mode: capture, run, restore, run again - the state hash is the same");
    check(mg == MATERIAL, "and the mode is untouched by the restore");
    check(run(JOLT, &hj, &mj), "Jolt mode replays too");
    check(hg != hj, "the two rules are different simulations (hashes differ)");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("material combine rules: Jolt's and the material rule's");

    testDefaultsAndArguments();
    testMaterialFriction();
    testMaterialRestitution();
    testBackToJolt();
    testLeafSurfaces();
    testSurfaceSampler();
    testBounce();
    testSnapshotReplay();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
