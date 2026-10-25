// PhysicsConfig.cpp -- physics_world_abi.h: the solver settings table, the pending/active world config,
// aver_phys_rebuild, the job-system choice and the build facts. The table below is the ONE source the ABI, the
// PHYSICS.* project keys (OcProject.cpp mirrors the names), the phys.* console variables, the C# enum and the
// editor page are all derived from.
#include "PhysicsConfig.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"

#include <Jolt/ConfigurationString.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// Defaults are Jolt's own (Jolt/Physics/PhysicsSettings.h), lengths and speeds in the engine's centimetres.
// Ranges keep a setting out of the places Jolt documents as broken (velocity steps below 2 stop friction).
constexpr SettingRow kSettings[] = {
    {"velocitySteps",              "count",    2.0f,    64.0f,     10.0f},
    {"positionSteps",              "count",    1.0f,    32.0f,     2.0f},
    {"collisionSteps",             "count",    1.0f,    8.0f,      1.0f},
    {"baumgarte",                  "fraction", 0.0f,    1.0f,      0.2f},
    {"speculativeDistance",        "cm",       0.1f,    20.0f,     2.0f},
    {"penetrationSlop",            "cm",       0.01f,   10.0f,     2.0f},
    {"maxPenetration",             "cm",       1.0f,    200.0f,    20.0f},
    {"manifoldTolerance",          "cm",       0.001f,  2.0f,      0.1f},
    {"linearCastThreshold",        "fraction", 0.05f,   1.0f,      0.75f},
    {"linearCastMaxPenetration",   "fraction", 0.0f,    1.0f,      0.25f},
    {"minRestitutionSpeed",        "cm/s",     0.0f,    1000.0f,   100.0f},
    {"sleepTime",                  "s",        0.05f,   60.0f,     0.5f},
    {"sleepSpeed",                 "cm/s",     0.1f,    100.0f,    3.0f},
    {"allowSleeping",              "bool",     0.0f,    1.0f,      1.0f},
    {"deterministic",              "bool",     0.0f,    1.0f,      1.0f},
    {"warmStart",                  "bool",     0.0f,    1.0f,      1.0f},
    {"bodyPairCache",              "bool",     0.0f,    1.0f,      1.0f},
    {"manifoldReduction",          "bool",     0.0f,    1.0f,      1.0f},
    {"largeIslandSplitter",        "bool",     0.0f,    1.0f,      1.0f},
    {"checkActiveEdges",           "bool",     0.0f,    1.0f,      1.0f},
    {"maxPairsInFlight",           "count",    256.0f,  262144.0f, 16384.0f},
    {"stepListenerBatch",          "count",    1.0f,    64.0f,     8.0f},
    {"stepListenerBatchesPerJob",  "count",    1.0f,    1000000.0f, 1.0f},
    {"pairCacheDeltaPos",          "cm",       0.001f,  10.0f,     0.1f},
    {"pairCacheDeltaRot",          "degrees",  0.01f,   30.0f,     2.0f},
    {"contactNormalDelta",         "degrees",  0.1f,    45.0f,     5.0f},
    {"lambdaPreserveDist",         "cm",       0.01f,   50.0f,     1.0f},
};
static_assert(sizeof(kSettings) / sizeof(kSettings[0]) == AVER_PHYS_WS_COUNT, "one table row per AVER_PHYS_WS_* key");

struct ConfigRow { const char* name; float min, max, def; };
constexpr ConfigRow kConfig[] = {
    {"maxBodies",       1024.0f, 8388607.0f, 65536.0f},
    {"maxBodyPairs",    1024.0f, 4194304.0f, 65536.0f},
    {"maxContacts",     1024.0f, 1048576.0f, 16384.0f},
    {"tempAllocatorMb", 1.0f,    1024.0f,    10.0f},
    {"threads",         -1.0f,   64.0f,      -1.0f},
};
static_assert(sizeof(kConfig) / sizeof(kConfig[0]) == AVER_PHYS_CF_COUNT, "one table row per AVER_PHYS_CF_* key");

// Process-wide on purpose: a setting made before aver_phys_init is stored, and one made on a live world
// outlives aver_phys_shutdown (unlike the layer matrix, which is per world).
float g_settings[AVER_PHYS_WS_COUNT];
float g_pending[AVER_PHYS_CF_COUNT];
bool  g_tablesInit = false;
bool  g_rebuildRefusedLogged = false;

void ensureTables() {
    if (g_tablesInit) return;
    g_tablesInit = true;
    for (int i = 0; i < AVER_PHYS_WS_COUNT; ++i) g_settings[i] = kSettings[i].def;
    for (int i = 0; i < AVER_PHYS_CF_COUNT; ++i) g_pending[i] = kConfig[i].def;
}

bool keyOk(int32_t key) { return key >= 0 && key < AVER_PHYS_WS_COUNT; }
bool cfgOk(int32_t key) { return key >= 0 && key < AVER_PHYS_CF_COUNT; }

bool isWhole(const SettingRow& r) { return std::strcmp(r.unit, "count") == 0 || std::strcmp(r.unit, "bool") == 0; }

bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

float clampInto(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr float kDegToRad = 0.017453292519943295f;

} // namespace

namespace aver::physics::detail {

JPH::PhysicsSettings makePhysicsSettings() {
    ensureTables();
    JPH::PhysicsSettings ps;   // Jolt's own defaults
    // A setting at its default keeps Jolt's literal bit for bit: 0.1f / 100.0f is not guaranteed to equal
    // 1.0e-3f, and "an untouched project is identical" is the contract (PhysicsSettingsTest's identity case).
    auto set = [](int k) { return g_settings[k] != kSettings[k].def; };
    auto m = [](int k) { return g_settings[k] / kCmPerMetre; };
    if (set(AVER_PHYS_WS_VELOCITY_STEPS))      ps.mNumVelocitySteps = static_cast<JPH::uint>(std::lround(g_settings[AVER_PHYS_WS_VELOCITY_STEPS]));
    if (set(AVER_PHYS_WS_POSITION_STEPS))      ps.mNumPositionSteps = static_cast<JPH::uint>(std::lround(g_settings[AVER_PHYS_WS_POSITION_STEPS]));
    if (set(AVER_PHYS_WS_BAUMGARTE))           ps.mBaumgarte = g_settings[AVER_PHYS_WS_BAUMGARTE];
    if (set(AVER_PHYS_WS_SPECULATIVE_DISTANCE)) ps.mSpeculativeContactDistance = m(AVER_PHYS_WS_SPECULATIVE_DISTANCE);
    if (set(AVER_PHYS_WS_PENETRATION_SLOP))    ps.mPenetrationSlop = m(AVER_PHYS_WS_PENETRATION_SLOP);
    if (set(AVER_PHYS_WS_MAX_PENETRATION))     ps.mMaxPenetrationDistance = m(AVER_PHYS_WS_MAX_PENETRATION);
    if (set(AVER_PHYS_WS_MANIFOLD_TOLERANCE))  ps.mManifoldTolerance = m(AVER_PHYS_WS_MANIFOLD_TOLERANCE);
    if (set(AVER_PHYS_WS_LINEAR_CAST_THRESHOLD)) ps.mLinearCastThreshold = g_settings[AVER_PHYS_WS_LINEAR_CAST_THRESHOLD];
    if (set(AVER_PHYS_WS_LINEAR_CAST_MAX_PENETRATION)) ps.mLinearCastMaxPenetration = g_settings[AVER_PHYS_WS_LINEAR_CAST_MAX_PENETRATION];
    if (set(AVER_PHYS_WS_MIN_RESTITUTION_SPEED)) ps.mMinVelocityForRestitution = m(AVER_PHYS_WS_MIN_RESTITUTION_SPEED);
    if (set(AVER_PHYS_WS_SLEEP_TIME))          ps.mTimeBeforeSleep = g_settings[AVER_PHYS_WS_SLEEP_TIME];
    if (set(AVER_PHYS_WS_SLEEP_SPEED))         ps.mPointVelocitySleepThreshold = m(AVER_PHYS_WS_SLEEP_SPEED);
    if (set(AVER_PHYS_WS_ALLOW_SLEEPING))      ps.mAllowSleeping = g_settings[AVER_PHYS_WS_ALLOW_SLEEPING] != 0.0f;
    if (set(AVER_PHYS_WS_DETERMINISTIC))       ps.mDeterministicSimulation = g_settings[AVER_PHYS_WS_DETERMINISTIC] != 0.0f;
    if (set(AVER_PHYS_WS_WARM_START))          ps.mConstraintWarmStart = g_settings[AVER_PHYS_WS_WARM_START] != 0.0f;
    if (set(AVER_PHYS_WS_BODY_PAIR_CACHE))     ps.mUseBodyPairContactCache = g_settings[AVER_PHYS_WS_BODY_PAIR_CACHE] != 0.0f;
    if (set(AVER_PHYS_WS_MANIFOLD_REDUCTION))  ps.mUseManifoldReduction = g_settings[AVER_PHYS_WS_MANIFOLD_REDUCTION] != 0.0f;
    if (set(AVER_PHYS_WS_LARGE_ISLAND_SPLITTER)) ps.mUseLargeIslandSplitter = g_settings[AVER_PHYS_WS_LARGE_ISLAND_SPLITTER] != 0.0f;
    if (set(AVER_PHYS_WS_CHECK_ACTIVE_EDGES))  ps.mCheckActiveEdges = g_settings[AVER_PHYS_WS_CHECK_ACTIVE_EDGES] != 0.0f;
    if (set(AVER_PHYS_WS_MAX_PAIRS_IN_FLIGHT)) ps.mMaxInFlightBodyPairs = static_cast<int>(std::lround(g_settings[AVER_PHYS_WS_MAX_PAIRS_IN_FLIGHT]));
    if (set(AVER_PHYS_WS_STEP_LISTENER_BATCH)) ps.mStepListenersBatchSize = static_cast<int>(std::lround(g_settings[AVER_PHYS_WS_STEP_LISTENER_BATCH]));
    if (set(AVER_PHYS_WS_STEP_LISTENER_BATCHES_PER_JOB)) ps.mStepListenerBatchesPerJob = static_cast<int>(std::lround(g_settings[AVER_PHYS_WS_STEP_LISTENER_BATCHES_PER_JOB]));
    if (set(AVER_PHYS_WS_PAIR_CACHE_DELTA_POS)) { const float d = m(AVER_PHYS_WS_PAIR_CACHE_DELTA_POS); ps.mBodyPairCacheMaxDeltaPositionSq = d * d; }
    if (set(AVER_PHYS_WS_PAIR_CACHE_DELTA_ROT)) ps.mBodyPairCacheCosMaxDeltaRotationDiv2 = std::cos(0.5f * g_settings[AVER_PHYS_WS_PAIR_CACHE_DELTA_ROT] * kDegToRad);
    if (set(AVER_PHYS_WS_CONTACT_NORMAL_DELTA)) ps.mContactNormalCosMaxDeltaRotation = std::cos(g_settings[AVER_PHYS_WS_CONTACT_NORMAL_DELTA] * kDegToRad);
    if (set(AVER_PHYS_WS_LAMBDA_PRESERVE_DIST)) { const float d = m(AVER_PHYS_WS_LAMBDA_PRESERVE_DIST); ps.mContactPointPreserveLambdaMaxDistSq = d * d; }
    return ps;
}

void applySettingsToWorld() {
    if (g_world) g_world->system.SetPhysicsSettings(makePhysicsSettings());
}

int collisionStepsSetting() {
    ensureTables();
    return static_cast<int>(std::lround(g_settings[AVER_PHYS_WS_COLLISION_STEPS]));
}

WorldConfig pendingWorldConfig() {
    ensureTables();
    WorldConfig c;
    c.maxBodies = static_cast<JPH::uint>(std::lround(g_pending[AVER_PHYS_CF_MAX_BODIES]));
    c.maxBodyPairs = static_cast<JPH::uint>(std::lround(g_pending[AVER_PHYS_CF_MAX_BODY_PAIRS]));
    c.maxContacts = static_cast<JPH::uint>(std::lround(g_pending[AVER_PHYS_CF_MAX_CONTACTS]));
    c.tempBytes = static_cast<size_t>(std::lround(g_pending[AVER_PHYS_CF_TEMP_MB])) * 1024u * 1024u;
    c.threads = static_cast<int>(std::lround(g_pending[AVER_PHYS_CF_THREADS]));
    return c;
}

void noteActiveConfig(const WorldConfig& cfg, int workers) {
    if (!g_world) return;
    g_world->activeConfig[AVER_PHYS_CF_MAX_BODIES] = static_cast<float>(cfg.maxBodies);
    g_world->activeConfig[AVER_PHYS_CF_MAX_BODY_PAIRS] = static_cast<float>(cfg.maxBodyPairs);
    g_world->activeConfig[AVER_PHYS_CF_MAX_CONTACTS] = static_cast<float>(cfg.maxContacts);
    g_world->activeConfig[AVER_PHYS_CF_TEMP_MB] = static_cast<float>(cfg.tempBytes / (1024u * 1024u));
    g_world->activeConfig[AVER_PHYS_CF_THREADS] = static_cast<float>(cfg.threads);   // as asked: -1 stays -1
    g_world->stats.workers = workers;
}

std::unique_ptr<JPH::JobSystem> makeJobSystem(int threads, int& workersOut) {
    diagOnWorldInit();   // the diagnostics preset's profiler must exist before the workers start; nothing in any other build
    if (threads == 0) {
        workersOut = 0;
        return std::make_unique<JPH::JobSystemSingleThreaded>(static_cast<JPH::uint>(JPH::cMaxPhysicsJobs));
    }
    // -1: one worker per core, less the main thread and one spare; never fewer than one.
    int workers = threads < 0 ? static_cast<int>(std::thread::hardware_concurrency()) - 2 : threads;
    if (workers < 1) workers = 1;
    workersOut = workers;
    return std::make_unique<JPH::JobSystemThreadPool>(static_cast<JPH::uint>(JPH::cMaxPhysicsJobs),
                                                      static_cast<JPH::uint>(JPH::cMaxPhysicsBarriers), workers);
}

const char* worldNotEmptyReason() {
    if (!g_world) return nullptr;
    if (!g_world->bodies.empty()) return "bodies exist";
    if (!g_world->characters.empty()) return "characters exist";
    if (!g_world->vehicles.empty()) return "vehicles exist";
    // Joints, ragdolls and vehicle wheels are all constraints in the system.
    if (!g_world->system.GetConstraints().empty()) return "joints or ragdolls exist";
    return nullptr;
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_world_abi_version(void) { return AVER_PHYS_WORLD_ABI_VERSION; }

// ---- build facts -------------------------------------------------------------------------------------

int32_t aver_phys_precision(void) {
#ifdef JPH_DOUBLE_PRECISION
    return AVER_PHYS_PRECISION_DOUBLE;
#else
    return AVER_PHYS_PRECISION_SINGLE;
#endif
}

uint32_t aver_phys_build_flags(void) {
    uint32_t f = 0;
#ifdef JPH_DEBUG_RENDERER
    f |= AVER_PHYS_BUILD_DEBUG_RENDERER;
#endif
#ifdef JPH_EXTERNAL_PROFILE
    f |= AVER_PHYS_BUILD_PROFILER;
#endif
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
    f |= AVER_PHYS_BUILD_CROSS_PLATFORM;
#endif
#ifdef JPH_ENABLE_DETERMINISM_LOG
    f |= AVER_PHYS_BUILD_DETERMINISM_LOG;
#endif
#if defined(JPH_TRACK_BROADPHASE_STATS) || defined(JPH_TRACK_NARROWPHASE_STATS) || defined(JPH_TRACK_SIMULATION_STATS)
    f |= AVER_PHYS_BUILD_STATS_TRACKING;
#endif
#ifdef JPH_ENABLE_ASSERTS
    f |= AVER_PHYS_BUILD_ASSERTS;
#endif
    return f;
}

int32_t aver_phys_build_string(uint8_t* outUtf8, int32_t cap) {
    const std::string s = std::string(JPH::GetConfigurationString()) + " aver-phys-state " + std::to_string(kStateLayoutVersion);
    const int32_t need = static_cast<int32_t>(s.size()) + 1;
    if (outUtf8 && cap > 0) {
        const int32_t n = need <= cap ? need : cap;
        std::memcpy(outUtf8, s.c_str(), static_cast<size_t>(n));
        outUtf8[n - 1] = 0;
    }
    return need;
}

// ---- live settings -----------------------------------------------------------------------------------

int32_t aver_phys_setting_count(void) { return AVER_PHYS_WS_COUNT; }

const char* aver_phys_setting_name(int32_t key) { return keyOk(key) ? kSettings[key].name : nullptr; }
const char* aver_phys_setting_unit(int32_t key) { return keyOk(key) ? kSettings[key].unit : nullptr; }

int32_t aver_phys_setting_find(const char* name) {
    if (!name) return -1;
    for (int32_t i = 0; i < AVER_PHYS_WS_COUNT; ++i)
        if (equalsNoCase(name, kSettings[i].name)) return i;
    return -1;
}

int32_t aver_phys_setting_range(int32_t key, float* outMin, float* outMax, float* outDefault) {
    if (!keyOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (outMin) *outMin = kSettings[key].min;
    if (outMax) *outMax = kSettings[key].max;
    if (outDefault) *outDefault = kSettings[key].def;
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_setting_set(int32_t key, float value) {
    if (!keyOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (std::isnan(value)) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    ensureTables();
    const SettingRow& r = kSettings[key];
    float v = clampInto(value, r.min, r.max);
    if (isWhole(r)) v = std::round(v);
    if (key == AVER_PHYS_WS_DETERMINISTIC && v == 0.0f && g_settings[key] != 0.0f)
        AVER_WARN("[Physics] deterministic simulation off: faster, but state snapshots and replays will no longer match");
    g_settings[key] = v;
    applySettingsToWorld();
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_setting_get(int32_t key, float* outValue) {
    if (!keyOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (!outValue) { aver::setAbiError(aver::AbiError::NullPointer); return 0; }
    ensureTables();
    *outValue = g_settings[key];
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_setting_reset(int32_t key) {
    ensureTables();
    if (key == -1) {
        for (int i = 0; i < AVER_PHYS_WS_COUNT; ++i) g_settings[i] = kSettings[i].def;
    } else if (keyOk(key)) {
        g_settings[key] = kSettings[key].def;
    } else {
        aver::setAbiError(aver::AbiError::OutOfRange);
        return 0;
    }
    applySettingsToWorld();
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_setting_get_all(float* outValues, int32_t n) {
    if (!outValues || n <= 0) return 0;
    ensureTables();
    const int32_t w = n < AVER_PHYS_WS_COUNT ? n : AVER_PHYS_WS_COUNT;
    for (int32_t i = 0; i < w; ++i) outValues[i] = g_settings[i];
    return w;
}

// ---- init-time config --------------------------------------------------------------------------------

const char* aver_phys_config_name(int32_t key) { return cfgOk(key) ? kConfig[key].name : nullptr; }

int32_t aver_phys_config_range(int32_t key, float* outMin, float* outMax, float* outDefault) {
    if (!cfgOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (outMin) *outMin = kConfig[key].min;
    if (outMax) *outMax = kConfig[key].max;
    if (outDefault) *outDefault = kConfig[key].def;
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_config_set(int32_t key, float value) {
    if (!cfgOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (std::isnan(value)) { aver::setAbiError(aver::AbiError::InvalidArgument); return 0; }
    ensureTables();
    g_pending[key] = std::round(clampInto(value, kConfig[key].min, kConfig[key].max));
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_config_get(int32_t key, float* outValue) {
    if (!cfgOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (!outValue) { aver::setAbiError(aver::AbiError::NullPointer); return 0; }
    ensureTables();
    *outValue = g_pending[key];
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_config_active(int32_t key, float* outValue) {
    if (!cfgOk(key)) { aver::setAbiError(aver::AbiError::OutOfRange); return 0; }
    if (!outValue) { aver::setAbiError(aver::AbiError::NullPointer); return 0; }
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return 0; }
    *outValue = g_world->activeConfig[key];
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_config_reset(int32_t key) {
    ensureTables();
    if (key == -1) {
        for (int i = 0; i < AVER_PHYS_CF_COUNT; ++i) g_pending[i] = kConfig[i].def;
    } else if (cfgOk(key)) {
        g_pending[key] = kConfig[key].def;
    } else {
        aver::setAbiError(aver::AbiError::OutOfRange);
        return 0;
    }
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_config_differs(void) {
    if (!g_world) return 0;
    ensureTables();
    for (int i = 0; i < AVER_PHYS_CF_COUNT; ++i)
        if (g_pending[i] != g_world->activeConfig[i]) return 1;
    return 0;
}

int32_t aver_phys_world_empty(void) { return worldNotEmptyReason() == nullptr ? 1 : 0; }

int32_t aver_phys_rebuild(void) {
    if (g_world) {
        if (const char* why = worldNotEmptyReason()) {
            if (!g_rebuildRefusedLogged) {
                g_rebuildRefusedLogged = true;
                AVER_WARN("[Physics] world rebuild refused ({}): the new limits apply when the world is next created", why);
            }
            aver::setAbiError(aver::AbiError::InvalidArgument);
            return 0;
        }
        // The two world values a caller set on the live world carry across; everything else is empty anyway.
        const float fixedStep = g_world->fixedStep;
        const JPH::Vec3 gravity = g_world->system.GetGravity();
        aver_phys_shutdown();
        if (!aver_phys_init()) return 0;
        g_world->fixedStep = fixedStep;
        g_world->system.SetGravity(gravity);
        g_rebuildRefusedLogged = false;
        AVER_INFO("[Physics] world rebuilt (max bodies {}, {} worker threads)",
                  static_cast<int>(g_world->activeConfig[AVER_PHYS_CF_MAX_BODIES]), g_world->stats.workers);
        return 1;
    }
    return aver_phys_init();
}

} // extern "C"
