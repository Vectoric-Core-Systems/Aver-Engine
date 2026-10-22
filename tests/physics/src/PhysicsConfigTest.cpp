// PhysicsConfigTest -- the world's init-time limits, stats and rebuild (physics_world_abi.h), through the ABI only.
//
//   * a contact limit too small for a pile sets the update-error stat and logs exactly ONE line per world,
//   * config_set on a live world flips config_differs; rebuild is REFUSED with a body present (nothing changes)
//     and accepted when empty, after which config_active and aver_phys_max_bodies follow the pending value,
//   * PHYSICS.THREADS 0 / 1 / 4 give 0 / 1 / 4 worker threads and every one of them steps correctly,
//   * a body limit of 1024 refuses the 1025th body,
//   * mesh shape handles survive a rebuild, settings survive it too,
//   * the stats are zero with no world and move with stepping.
//
// NOT HERE, and said so: the design's "per-step allocation count is unchanged" case needs a counting operator
// new, and the allocations that matter happen inside Aver.Physics.dll and Jolt's own allocator, which a hook in
// this executable cannot see. The integrator measures that headed (docs/plans/wave11/WORLD_STATE_DEBUG.md, 6.14).
//
// Hand-run, exit code = failure count, same shape as JointBreakTest.cpp.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
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

static double stat(int k) {
    double v[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(v, AVER_PHYS_ST_COUNT);
    return v[k];
}

static float pending(int k) { float v = -1; aver_phys_config_get(k, &v); return v; }
static float active(int k) { float v = -1; return aver_phys_config_active(k, &v) ? v : -1.0f; }

// Counts the lines containing `needle` that the physics DLL logs between begin and end. Aver.Physics carries its own copy of the
// log, so this executable's setLogSink never sees its lines; the DLL writes warnings to stderr, so stderr is what gets captured.
#ifdef _WIN32
#include <io.h>
#define AVER_DUP _dup
#define AVER_DUP2 _dup2
#define AVER_CLOSE _close
#define AVER_FILENO _fileno
#else
#include <unistd.h>
#define AVER_DUP dup
#define AVER_DUP2 dup2
#define AVER_CLOSE close
#define AVER_FILENO fileno
#endif

static std::FILE* g_capture = nullptr;
static int g_savedErr = -1;

static void captureBegin() {
    std::fflush(stderr);
    g_capture = std::tmpfile();
    g_savedErr = AVER_DUP(2);
    if (g_capture && g_savedErr >= 0) AVER_DUP2(AVER_FILENO(g_capture), 2);
}

static int captureEnd(const char* needle) {
    std::fflush(stderr);
    if (g_savedErr >= 0) { AVER_DUP2(g_savedErr, 2); AVER_CLOSE(g_savedErr); g_savedErr = -1; }
    int lines = 0;
    if (g_capture) {
        std::rewind(g_capture);
        std::string text;
        char buf[512];
        while (std::fgets(buf, sizeof buf, g_capture)) text += buf;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++lines;
        std::fclose(g_capture);
        g_capture = nullptr;
    }
    return lines;
}

static void resetAll() {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
}

static void pile(int nx, int ny, int nz) {
    aver_phys_add_static_box(0, 0, -50, 3000, 3000, 50);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
            for (int k = 0; k < nz; ++k)
                aver_phys_add_dynamic_box(static_cast<float>(i) * 21.0f, static_cast<float>(j) * 21.0f,
                                          10.0f + static_cast<float>(k) * 21.0f, 10, 10, 10, 10.0f);
}

static void testStatsWithoutAWorld() {
    AVER_INFO("=== stats with no world, and a small n ===");
    resetAll();
    double v[AVER_PHYS_ST_COUNT];
    for (double& d : v) d = 123.0;
    check(aver_phys_stats(v, AVER_PHYS_ST_COUNT) == AVER_PHYS_ST_COUNT, "stats reports the count it wrote");
    bool zeros = true;
    for (double d : v) if (d != 0.0) zeros = false;
    check(zeros, "every stat is zero with no world");
    check(aver_phys_stats(v, 4) == 4, "a short buffer writes min(n, COUNT)");
    check(aver_phys_stats(nullptr, 4) == 0 && aver_phys_stats(v, 0) == 0, "a null or empty buffer writes nothing");
    check(aver_phys_stats_reset_window() == 0, "reset_window needs a world");
    float active = 0.0f;
    check(aver_phys_config_active(AVER_PHYS_CF_MAX_BODIES, &active) == 0, "config_active needs a world");
    check(aver_phys_config_differs() == 0, "config_differs is 0 with no world");
    check(aver_phys_world_empty() == 1, "no world counts as empty");
}

static void testStatsMove() {
    AVER_INFO("=== stats move with the simulation ===");
    resetAll();
    aver_phys_init();
    pile(3, 3, 2);
    step(30);
    check(stat(AVER_PHYS_ST_BODIES) == 19.0, "bodies counts the floor and the 18 boxes");
    check(stat(AVER_PHYS_ST_STATIC) == 1.0 && stat(AVER_PHYS_ST_DYNAMIC) == 18.0, "static and dynamic split");
    check(stat(AVER_PHYS_ST_MAX_BODIES) == 65536.0, "the default body limit");
    check(stat(AVER_PHYS_ST_STEPS_TOTAL) == 30.0, "steps total counts fixed steps");
    check(stat(AVER_PHYS_ST_STEPS_LAST_CALL) == 1.0, "steps last call");
    check(stat(AVER_PHYS_ST_STEP_MS_LAST) > 0.0 && stat(AVER_PHYS_ST_STEP_MS_AVG) > 0.0, "step time is measured");
    check(stat(AVER_PHYS_ST_STEP_MS_MAX) >= stat(AVER_PHYS_ST_STEP_MS_LAST), "the window maximum is at least the last");
    check(stat(AVER_PHYS_ST_CALL_MS_LAST) >= stat(AVER_PHYS_ST_STEP_MS_LAST) * 0.5, "the call time covers the update");
    check(stat(AVER_PHYS_ST_TEMP_SIZE_BYTES) == 10.0 * 1024.0 * 1024.0, "the default temp allocator is 10 MB");
    check(stat(AVER_PHYS_ST_TEMP_PEAK_BYTES) > 0.0, "the temp allocator's peak is tracked");
    check(stat(AVER_PHYS_ST_UPDATE_ERRORS) == 0.0, "a default world drops nothing");
    check(aver_phys_stats_reset_window() == 1 && stat(AVER_PHYS_ST_STEP_MS_MAX) == 0.0, "reset_window clears the maximum");
    aver_phys_step(kDt * 3.5f);   // three whole steps and half of a fourth, clear of float rounding
    check(stat(AVER_PHYS_ST_STEPS_LAST_CALL) == 3.0 && stat(AVER_PHYS_ST_STEPS_TOTAL) == 33.0, "a long call reports its steps");
    aver_phys_shutdown();
}

static void testUpdateErrors() {
    AVER_INFO("=== a contact limit too small: the error is kept and logged once ===");
    resetAll();
    aver_phys_config_set(AVER_PHYS_CF_MAX_CONTACTS, 1024.0f);
    captureBegin();
    aver_phys_init();
    check(stat(AVER_PHYS_ST_UPDATE_ERRORS) == 0.0, "nothing dropped before the first step");
    pile(12, 12, 5);
    step(150);
    const double errors = stat(AVER_PHYS_ST_UPDATE_ERRORS);
    const int loggedLines = captureEnd("dropped contacts");
    check(errors != 0.0, "a 720-box pile overflows 1024 contacts and sets the error bits");
    check((static_cast<int>(errors) & 0x7) == static_cast<int>(errors), "only the three Jolt bits are used");
    check(stat(AVER_PHYS_ST_UPDATE_ERROR_STEPS) > 1.0, "and it kept counting steps");
    check(loggedLines == 1, "exactly one log line for the whole world (" + std::to_string(loggedLines) + ")");
    check(aver_phys_stats_reset_window() == 1 && stat(AVER_PHYS_ST_UPDATE_ERRORS) == 0.0 && stat(AVER_PHYS_ST_UPDATE_ERROR_STEPS) == 0.0,
          "reset_window clears the error fields");
    aver_phys_shutdown();
    aver_phys_config_reset(-1);
}

static void testConfigRebuild() {
    AVER_INFO("=== config_set, config_differs, rebuild ===");
    resetAll();
    float lo = 0, hi = 0, def = 0;
    check(aver_phys_config_range(AVER_PHYS_CF_MAX_BODIES, &lo, &hi, &def) == 1 && def == 65536.0f && lo == 1024.0f, "max bodies range");
    check(pending(AVER_PHYS_CF_MAX_BODIES) == 65536.0f && pending(AVER_PHYS_CF_THREADS) == -1.0f && pending(AVER_PHYS_CF_TEMP_MB) == 10.0f,
          "the pending defaults are the old literals");
    check(aver_phys_config_set(AVER_PHYS_CF_MAX_BODIES, 10.0f) == 1 && pending(AVER_PHYS_CF_MAX_BODIES) == 1024.0f, "a limit clamps into its range");
    check(aver_phys_config_set(AVER_PHYS_CF_MAX_BODIES, std::nanf("")) == 0, "NaN is refused");
    check(aver_phys_config_set(99, 1.0f) == 0, "an unknown key is refused");
    aver_phys_config_reset(-1);

    aver_phys_init();
    check(active(AVER_PHYS_CF_MAX_BODIES) == 65536.0f && aver_phys_max_bodies() == 65536, "the world starts with the defaults");
    check(aver_phys_config_differs() == 0, "and pending == active");
    aver_phys_config_set(AVER_PHYS_CF_MAX_BODIES, 2048.0f);
    check(aver_phys_config_differs() == 1, "config_set on a live world sets config_differs");
    check(active(AVER_PHYS_CF_MAX_BODIES) == 65536.0f && aver_phys_max_bodies() == 65536, "without touching the live world");

    const int32_t b = aver_phys_add_dynamic_box(0, 0, 100, 10, 10, 10, 1.0f);
    check(aver_phys_world_empty() == 0, "a body makes the world non-empty");
    check(aver_phys_rebuild() == 0, "rebuild is refused with a body present");
    check(aver_phys_body_count() == 1 && aver_phys_max_bodies() == 65536 && aver_phys_config_differs() == 1, "and nothing changed");
    aver_phys_remove_body(b);
    check(aver_phys_world_empty() == 1, "empty again");
    aver_phys_set_gravity(0, 0, -123.0f);
    aver_phys_set_fixed_step(0.01f);
    check(aver_phys_rebuild() == 1, "rebuild is accepted on an empty world");
    check(active(AVER_PHYS_CF_MAX_BODIES) == 2048.0f && aver_phys_max_bodies() == 2048, "config_active and max_bodies follow the pending value");
    check(aver_phys_config_differs() == 0, "and the badge clears");
    check(std::fabs(aver_phys_fixed_step() - 0.01f) < 1e-7f, "the fixed step carries across a rebuild");
    aver_phys_shutdown();
    check(aver_phys_rebuild() == 1 && aver_phys_ready() == 1, "rebuild with no world is a plain init");
    check(aver_phys_max_bodies() == 2048, "using the pending limit");
    aver_phys_shutdown();
    aver_phys_config_reset(-1);
}

static void testThreads() {
    AVER_INFO("=== PHYSICS.THREADS: single-threaded, one worker, four ===");
    const int counts[] = {0, 1, 4};
    for (const int t : counts) {
        resetAll();
        aver_phys_config_set(AVER_PHYS_CF_THREADS, static_cast<float>(t));
        aver_phys_init();
        check(stat(AVER_PHYS_ST_WORKER_THREADS) == static_cast<double>(t), "THREADS " + std::to_string(t) + " reports " + std::to_string(t) + " worker(s)");
        aver_phys_add_static_box(0, 0, -50, 500, 500, 50);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 60, 10, 10, 10, 10.0f);
        step(120);
        float p[3] = {};
        aver_phys_body_position(box, p);
        check(std::fabs(p[2] - 10.0f) < 1.5f, "and a box falls and rests on the floor (z=" + std::to_string(p[2]) + ")");
        aver_phys_shutdown();
    }
    resetAll();
    aver_phys_init();
    check(stat(AVER_PHYS_ST_WORKER_THREADS) >= 1.0, "automatic threads never means none");
    aver_phys_shutdown();
}

static void testBodyLimit() {
    AVER_INFO("=== MAX_BODIES 1024 refuses the 1025th body ===");
    resetAll();
    aver_phys_config_set(AVER_PHYS_CF_MAX_BODIES, 1024.0f);
    aver_phys_init();
    int added = 0;
    for (int i = 0; i < 1030; ++i)
        if (aver_phys_add_static_box(static_cast<float>(i) * 30.0f, 0, 0, 10, 10, 10)) ++added;
    check(added == 1024, "1024 bodies fit (" + std::to_string(added) + ")");
    check(aver_phys_add_static_box(0, 500, 0, 10, 10, 10) == 0, "and the next is refused");
    aver_phys_shutdown();
    aver_phys_config_reset(-1);
}

static void testShapesAndSettingsSurviveRebuild() {
    AVER_INFO("=== shape handles and settings survive a rebuild ===");
    resetAll();
    aver_phys_init();
    const float verts[] = {-100, -100, 0, 100, -100, 0, 100, 100, 0, -100, 100, 0};
    const int32_t idx[] = {0, 1, 2, 0, 2, 3};
    const int32_t shape = aver_phys_create_mesh_shape(verts, 4, idx, 6);
    check(shape != 0, "a mesh shape is built");
    aver_phys_setting_set(AVER_PHYS_WS_VELOCITY_STEPS, 12.0f);
    aver_phys_config_set(AVER_PHYS_CF_MAX_CONTACTS, 4096.0f);
    check(aver_phys_rebuild() == 1, "rebuild");
    const int32_t body = aver_phys_add_mesh_shape_body(shape, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1);
    check(body != 0, "the old shape handle still places a body in the new world");
    float v = 0;
    check(aver_phys_setting_get(AVER_PHYS_WS_VELOCITY_STEPS, &v) == 1 && v == 12.0f, "the solver setting carried over");
    aver_phys_remove_body(body);
    aver_phys_release_mesh_shape(shape);
    aver_phys_shutdown();
    resetAll();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("physics world config: stats, update errors, rebuild, threads, limits");

    testStatsWithoutAWorld();
    testStatsMove();
    testUpdateErrors();
    testConfigRebuild();
    testThreads();
    testBodyLimit();
    testShapesAndSettingsSurviveRebuild();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
