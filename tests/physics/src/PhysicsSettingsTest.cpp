// PhysicsSettingsTest -- the live solver settings of physics_world_abi.h, through the plain-C ABI only.
//
//   * the table is self-consistent (a name, a range holding its default, find() finds it back, any case),
//   * IDENTITY: a world that sets every setting to its own default steps bit-for-bit like one that never
//     touched the table. (A golden hash from the pre-change binary would be stronger; it is not kept here.)
//   * a setting is stored before aver_phys_init, survives aver_phys_shutdown, and reset(-1) restores defaults,
//   * out-of-range values clamp, NaN and unknown keys are refused, counts and flags round to whole numbers,
//   * the settings do something: collisionSteps stops a fast sphere tunnelling a thin wall (thresholds below
//     are tuned to this scene), allowSleeping / sleepTime decide when a resting box sleeps.
//
// Hand-run, exit code = failure count, same shape as JointBreakTest.cpp.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cctype>
#include <cmath>
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

static const float kDt = 1.0f / 60.0f;
static void step(int n) { for (int i = 0; i < n; ++i) aver_phys_step(kDt); }

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", static_cast<double>(v)); return b; }

static float setting(int key) { float v = -1.0f; aver_phys_setting_get(key, &v); return v; }

// ---- the table -------------------------------------------------------------------------------------

static void testTable() {
    AVER_INFO("=== the settings table ===");
    check(aver_phys_world_abi_version() == AVER_PHYS_WORLD_ABI_VERSION, "the world ABI version is reported");
    const int n = aver_phys_setting_count();
    check(n == AVER_PHYS_WS_COUNT && n == 27, "27 settings");
    bool allNamed = true, rangesHoldDefaults = true, findsBack = true, upperFindsBack = true, unitsPresent = true;
    for (int k = 0; k < n; ++k) {
        const char* name = aver_phys_setting_name(k);
        if (!name || !*name) { allNamed = false; continue; }
        float lo = 0, hi = 0, def = 0;
        if (!aver_phys_setting_range(k, &lo, &hi, &def) || def < lo || def > hi || !(lo < hi)) rangesHoldDefaults = false;
        if (aver_phys_setting_find(name) != k) findsBack = false;
        std::string up = name;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (aver_phys_setting_find(up.c_str()) != k) upperFindsBack = false;
        const char* unit = aver_phys_setting_unit(k);
        if (!unit || !*unit) unitsPresent = false;
    }
    check(allNamed, "every key has a name");
    check(rangesHoldDefaults, "every range holds its default");
    check(findsBack, "find(name(k)) == k");
    check(upperFindsBack, "find is case-insensitive");
    check(unitsPresent, "every key has a unit string");
    check(aver_phys_setting_find("noSuchSetting") == -1, "an unknown name finds nothing");
    check(aver_phys_setting_find(nullptr) == -1, "a null name finds nothing");
    check(aver_phys_setting_name(-1) == nullptr && aver_phys_setting_name(n) == nullptr, "out-of-range names are NULL");
    check(std::strcmp(aver_phys_setting_name(AVER_PHYS_WS_VELOCITY_STEPS), "velocitySteps") == 0, "key 0 is velocitySteps");
    check(std::strcmp(aver_phys_setting_name(AVER_PHYS_WS_LAMBDA_PRESERVE_DIST), "lambdaPreserveDist") == 0, "the last key is lambdaPreserveDist");
    // The AVER_PHYS_WS_* constants (which the C# PhysicsSetting enum mirrors, checked by AbiEnumTest) against the
    // names the table gives them: the one link neither of those two sees.
    static const struct { int key; const char* name; } kByMacro[] = {
        {AVER_PHYS_WS_VELOCITY_STEPS, "velocitySteps"}, {AVER_PHYS_WS_POSITION_STEPS, "positionSteps"},
        {AVER_PHYS_WS_COLLISION_STEPS, "collisionSteps"}, {AVER_PHYS_WS_BAUMGARTE, "baumgarte"},
        {AVER_PHYS_WS_SPECULATIVE_DISTANCE, "speculativeDistance"}, {AVER_PHYS_WS_PENETRATION_SLOP, "penetrationSlop"},
        {AVER_PHYS_WS_MAX_PENETRATION, "maxPenetration"}, {AVER_PHYS_WS_MANIFOLD_TOLERANCE, "manifoldTolerance"},
        {AVER_PHYS_WS_LINEAR_CAST_THRESHOLD, "linearCastThreshold"}, {AVER_PHYS_WS_LINEAR_CAST_MAX_PENETRATION, "linearCastMaxPenetration"},
        {AVER_PHYS_WS_MIN_RESTITUTION_SPEED, "minRestitutionSpeed"}, {AVER_PHYS_WS_SLEEP_TIME, "sleepTime"},
        {AVER_PHYS_WS_SLEEP_SPEED, "sleepSpeed"}, {AVER_PHYS_WS_ALLOW_SLEEPING, "allowSleeping"},
        {AVER_PHYS_WS_DETERMINISTIC, "deterministic"}, {AVER_PHYS_WS_WARM_START, "warmStart"},
        {AVER_PHYS_WS_BODY_PAIR_CACHE, "bodyPairCache"}, {AVER_PHYS_WS_MANIFOLD_REDUCTION, "manifoldReduction"},
        {AVER_PHYS_WS_LARGE_ISLAND_SPLITTER, "largeIslandSplitter"}, {AVER_PHYS_WS_CHECK_ACTIVE_EDGES, "checkActiveEdges"},
        {AVER_PHYS_WS_MAX_PAIRS_IN_FLIGHT, "maxPairsInFlight"}, {AVER_PHYS_WS_STEP_LISTENER_BATCH, "stepListenerBatch"},
        {AVER_PHYS_WS_STEP_LISTENER_BATCHES_PER_JOB, "stepListenerBatchesPerJob"}, {AVER_PHYS_WS_PAIR_CACHE_DELTA_POS, "pairCacheDeltaPos"},
        {AVER_PHYS_WS_PAIR_CACHE_DELTA_ROT, "pairCacheDeltaRot"}, {AVER_PHYS_WS_CONTACT_NORMAL_DELTA, "contactNormalDelta"},
        {AVER_PHYS_WS_LAMBDA_PRESERVE_DIST, "lambdaPreserveDist"},
    };
    bool macrosMatch = sizeof(kByMacro) / sizeof(kByMacro[0]) == static_cast<size_t>(AVER_PHYS_WS_COUNT);
    for (const auto& m : kByMacro) {
        const char* nm = aver_phys_setting_name(m.key);
        if (!nm || std::strcmp(nm, m.name) != 0) macrosMatch = false;
    }
    check(macrosMatch, "every AVER_PHYS_WS_* constant names the setting it should");
    float defaults[AVER_PHYS_WS_COUNT];
    check(aver_phys_setting_get_all(defaults, 100) == AVER_PHYS_WS_COUNT, "get_all writes min(n, COUNT)");
    check(aver_phys_setting_get_all(defaults, 3) == 3, "get_all with n = 3 writes 3");
    check(aver_phys_precision() == AVER_PHYS_PRECISION_SINGLE || aver_phys_precision() == AVER_PHYS_PRECISION_DOUBLE, "a precision is reported");
    char buf[512] = {};
    const int need = aver_phys_build_string(reinterpret_cast<uint8_t*>(buf), sizeof buf);
    check(need > 1 && need <= static_cast<int>(sizeof buf) && std::strstr(buf, "aver-phys-state") != nullptr, "the build string carries the state layout version");
    check(aver_phys_build_string(nullptr, 0) == need, "build_string(NULL, 0) answers the length");
}

// ---- identity --------------------------------------------------------------------------------------

// A pile of boxes and a sphere on a floor, 300 steps; every position, bit for bit.
static std::vector<float> runScene(bool touchEveryDefault) {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    if (touchEveryDefault) {
        // Set each setting to the value it already has, then reset one and re-set it: the table must keep
        // Jolt's own literals for anything still at its default.
        float v[AVER_PHYS_WS_COUNT];
        aver_phys_setting_get_all(v, AVER_PHYS_WS_COUNT);
        for (int k = 0; k < AVER_PHYS_WS_COUNT; ++k) aver_phys_setting_set(k, v[k]);
        aver_phys_setting_reset(AVER_PHYS_WS_SPECULATIVE_DISTANCE);
        aver_phys_setting_reset(-1);
    }
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50, 2000, 2000, 50);
    std::vector<int32_t> bodies;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 3; ++k)
                bodies.push_back(aver_phys_add_dynamic_box(static_cast<float>(i) * 21.5f, static_cast<float>(j) * 21.0f,
                                                           10.0f + static_cast<float>(k) * 23.0f, 10, 10, 10, 20.0f));
    bodies.push_back(aver_phys_add_dynamic_sphere(30, 30, 150, 12, 5.0f));
    step(300);
    std::vector<float> out;
    for (const int32_t b : bodies) {
        float p[3] = {}, q[4] = {};
        aver_phys_body_position(b, p);
        aver_phys_body_rotation(b, q);
        out.insert(out.end(), p, p + 3);
        out.insert(out.end(), q, q + 4);
    }
    aver_phys_shutdown();
    return out;
}

static void testIdentity() {
    AVER_INFO("=== identity: setting every setting to its default changes nothing ===");
    const std::vector<float> a = runScene(false);
    const std::vector<float> b = runScene(true);
    const std::vector<float> c = runScene(false);
    check(!a.empty() && a.size() == b.size(), "both runs have the same bodies");
    check(a.size() == c.size() && std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0, "the scene is deterministic run to run");
    check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "touching every default is bit-identical to never touching the table");
}

// ---- storage, clamping, refusals ---------------------------------------------------------------------

static void testStorageAndClamping() {
    AVER_INFO("=== storage, clamping and refusals ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);

    // Set with no world: stored, then applied by the world that follows, and it survives shutdown.
    check(aver_phys_setting_set(AVER_PHYS_WS_VELOCITY_STEPS, 14.0f) == 1, "set works before aver_phys_init");
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 14.0f, "and reads back");
    aver_phys_init();
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 14.0f, "still 14 once the world exists");
    aver_phys_shutdown();
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 14.0f, "a setting survives aver_phys_shutdown");

    check(aver_phys_setting_set(AVER_PHYS_WS_VELOCITY_STEPS, 1.0f) == 1 && setting(AVER_PHYS_WS_VELOCITY_STEPS) == 2.0f,
          "velocitySteps below 2 clamps to 2 (friction needs 2)");
    check(aver_phys_setting_set(AVER_PHYS_WS_VELOCITY_STEPS, 1000.0f) == 1 && setting(AVER_PHYS_WS_VELOCITY_STEPS) == 64.0f,
          "and above the range clamps to the top");
    check(aver_phys_setting_set(AVER_PHYS_WS_POSITION_STEPS, 3.4f) == 1 && setting(AVER_PHYS_WS_POSITION_STEPS) == 3.0f,
          "a count rounds to a whole number");
    check(aver_phys_setting_set(AVER_PHYS_WS_ALLOW_SLEEPING, 0.3f) == 1 && setting(AVER_PHYS_WS_ALLOW_SLEEPING) == 0.0f,
          "a flag rounds to 0 or 1");
    check(aver_phys_setting_set(AVER_PHYS_WS_BAUMGARTE, 0.35f) == 1 && std::fabs(setting(AVER_PHYS_WS_BAUMGARTE) - 0.35f) < 1e-6f,
          "a fraction keeps its value");

    const float before = setting(AVER_PHYS_WS_SLEEP_TIME);
    check(aver_phys_setting_set(AVER_PHYS_WS_SLEEP_TIME, std::numeric_limits<float>::quiet_NaN()) == 0, "NaN is refused");
    check(setting(AVER_PHYS_WS_SLEEP_TIME) == before, "and changes nothing");
    check(aver_phys_setting_set(-1, 1.0f) == 0 && aver_phys_setting_set(AVER_PHYS_WS_COUNT, 1.0f) == 0, "an unknown key is refused");
    float v = 0;
    check(aver_phys_setting_get(AVER_PHYS_WS_COUNT, &v) == 0 && aver_phys_setting_get(0, nullptr) == 0, "get refuses an unknown key and a null out");
    check(aver_phys_last_error() == static_cast<int32_t>(AbiError::NullPointer), "and says null pointer");
    float lo = 0, hi = 0, def = 0;
    check(aver_phys_setting_range(0, &lo, nullptr, nullptr) == 1 && lo == 2.0f, "range accepts NULL outs");

    check(aver_phys_setting_set(AVER_PHYS_WS_DETERMINISTIC, 0.0f) == 1 && setting(AVER_PHYS_WS_DETERMINISTIC) == 0.0f,
          "deterministic = 0 is accepted (and logs)");

    check(aver_phys_setting_reset(AVER_PHYS_WS_VELOCITY_STEPS) == 1 && setting(AVER_PHYS_WS_VELOCITY_STEPS) == 10.0f, "reset(key) restores one default");
    check(aver_phys_setting_reset(-1) == 1, "reset(-1) succeeds");
    bool allDefault = true;
    for (int k = 0; k < AVER_PHYS_WS_COUNT; ++k) {
        aver_phys_setting_range(k, &lo, &hi, &def);
        if (setting(k) != def) allDefault = false;
    }
    check(allDefault, "and every setting is back at its default");
    check(aver_phys_setting_reset(AVER_PHYS_WS_COUNT) == 0, "reset of an unknown key is refused");
}

// ---- the settings do something ----------------------------------------------------------------------

// A 5 cm ball shot at 30 m/s at a 1 cm wall. With one collision step per fixed step it moves 50 cm between
// tests and jumps the wall (the start is chosen so no sample lands within the ball's reach of it); with eight it
// moves 6.25 cm, smaller than the 7.5 cm band in front of the wall where a contact pushes it back, so one sample
// always lands there.
static float shootAtWall(float collisionSteps) {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_setting_set(AVER_PHYS_WS_COLLISION_STEPS, collisionSteps);
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);
    aver_phys_add_static_box(0, 0, 0, 0.5f, 100, 100);
    const int32_t ball = aver_phys_add_dynamic_sphere(-310, 0, 0, 5, 1.0f);
    aver_phys_body_set_velocity(ball, 3000, 0, 0);
    step(20);
    float p[3] = {};
    aver_phys_body_position(ball, p);
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    return p[0];
}

static void testCollisionSteps() {
    AVER_INFO("=== collisionSteps: a fast ball against a thin wall ===");
    const float one = shootAtWall(1.0f);
    const float eight = shootAtWall(8.0f);
    check(one > 50.0f, "one collision step per fixed step tunnels through (x=" + f2s(one) + ")");
    check(eight < 10.0f, "eight hold the wall (x=" + f2s(eight) + ")");
}

// A box dropped onto a floor; whether it is asleep after `seconds`.
static bool boxAsleepAfter(float seconds, bool allowSleeping, float sleepTime) {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_setting_set(AVER_PHYS_WS_ALLOW_SLEEPING, allowSleeping ? 1.0f : 0.0f);
    aver_phys_setting_set(AVER_PHYS_WS_SLEEP_TIME, sleepTime);
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50, 500, 500, 50);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 10.5f, 10, 10, 10, 10.0f);
    step(static_cast<int>(seconds / kDt));
    const bool asleep = aver_phys_body_is_active(box) == 0;
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    return asleep;
}

static void testSleeping() {
    AVER_INFO("=== sleeping: allowSleeping and sleepTime ===");
    check(boxAsleepAfter(4.0f, true, 0.5f), "a resting box sleeps with the defaults");
    check(!boxAsleepAfter(10.0f, false, 0.5f), "allowSleeping 0 keeps it active for 10 s");
    check(!boxAsleepAfter(2.0f, true, 5.0f), "a 5 s sleepTime has not put it to sleep after 2 s");
    check(boxAsleepAfter(10.0f, true, 5.0f), "but has after 10 s");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("physics world settings: table, identity, storage, clamping, effect");

    testTable();
    testIdentity();
    testStorageAndClamping();
    testCollisionSteps();
    testSleeping();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
