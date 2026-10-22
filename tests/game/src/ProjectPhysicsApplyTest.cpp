// ProjectPhysicsApplyTest -- the manifest's PHYSICS.* keys, from the file to a running world (wave 11,
// WORLD_STATE_DEBUG): the parser and writer in Aver.Formats, PhysicsProjectApply.hpp in Runtime, and the physics
// module's settings and limits.
//
//   * the name list OcProject.cpp mirrors (Formats may not depend on Physics) equals the physics table,
//   * a manifest with the keys parses; a bad value is dropped, a repeated key is replaced, an unknown PHYSICS.* key
//     passes through a re-save untouched, an owned key is replaced once and never duplicated,
//   * configureProjectPhysics writes the settings and limits and rebuilds an EMPTY world; a world that holds a body
//     keeps its limits and says they apply later; the next project that states nothing puts the defaults back.
//
// Hand-run, exit code = failure count, same shape as the physics tests.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/game/PhysicsProjectApply.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_state_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static float setting(int key) { float v = -1; aver_phys_setting_get(key, &v); return v; }
static float active(int key) { float v = -1; return aver_phys_config_active(key, &v) ? v : -1.0f; }

static int g_lines = 0;
static const char* g_needle = "";
static void countingSink(void*, LogLevel, std::string_view msg) {
    if (msg.find(g_needle) != std::string_view::npos) ++g_lines;
}

static fmt::ProjectDesc parse(const std::string& text) {
    fmt::ProjectDesc d;
    std::string err;
    if (!fmt::parseOcproject(text, d, &err)) AVER_ERROR("parse failed: {}", err);
    return d;
}

static int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

static void resetAll() {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    game::physicsAppliedKeys() = game::PhysicsAppliedKeys{};
    aver_phys_shape_cache_enable(0);
}

static void testMirror() {
    AVER_INFO("=== the name mirror equals the physics table ===");
    check(fmt::physicsSettingCount() == aver_phys_setting_count(), "same number of settings");
    bool same = true;
    for (int i = 0; i < aver_phys_setting_count(); ++i) {
        const char* a = fmt::physicsSettingName(i);
        const char* b = aver_phys_setting_name(i);
        if (!a || !b || std::strcmp(a, b) != 0) { same = false; AVER_ERROR("   index {}: '{}' vs '{}'", i, a ? a : "null", b ? b : "null"); }
    }
    check(same, "every name matches, in order");
    check(fmt::physicsSettingName(-1) == nullptr && fmt::physicsSettingName(fmt::physicsSettingCount()) == nullptr, "out of range is null");
}

static const char* kManifest =
    "OCPROJECT 1\n"
    "NAME PhysTest\n"
    "PHYSICS.VELOCITYSTEPS 14\n"
    "PHYSICS.SLEEPTIME 1\n"
    "PHYSICS.MAXBODIES 200000\n"
    "PHYSICS.THREADS 6\n"
    "PHYSICS.FUTURETHING 7\n";

static void testParseAndWrite() {
    AVER_INFO("=== parse and write ===");
    fmt::ProjectDesc d = parse(kManifest);
    check(d.physicsSettings.size() == 2, "two solver settings parsed");
    check(d.physicsSettings.size() == 2 && d.physicsSettings[0].first == "velocitySteps" && d.physicsSettings[0].second == 14.0f &&
          d.physicsSettings[1].first == "sleepTime" && d.physicsSettings[1].second == 1.0f, "in file order, by their table names");
    check(d.physMaxBodies == 200000 && d.physThreads == 6, "the limits parse");
    check(d.hasPhysicsSettings(), "and the section reports itself present");
    check(parse("OCPROJECT 1\nNAME A\n").physThreads == -2 && !parse("OCPROJECT 1\nNAME A\n").hasPhysicsSettings(), "an unstated section is absent");
    check(parse("OCPROJECT 1\nNAME A\nPHYSICS.THREADS -1\n").physThreads == -1, "THREADS -1 (automatic) is a stated value");

    check(parse("OCPROJECT 1\nNAME A\nPHYSICS.SLEEPTIME abc\n").physicsSettings.empty(), "a value that is not a number is dropped");
    {
        const fmt::ProjectDesc r = parse("OCPROJECT 1\nNAME A\nphysics.sleeptime 2\nPHYSICS.SLEEPTIME 3\n");
        check(r.physicsSettings.size() == 1 && r.physicsSettings[0].second == 3.0f, "keys are case-insensitive and the last one wins");
    }

    const std::string w = fmt::writeOcproject(d, "");
    check(w.find("PHYSICS.VELOCITYSTEPS 14") != std::string::npos && w.find("PHYSICS.SLEEPTIME 1") != std::string::npos &&
          w.find("PHYSICS.THREADS 6") != std::string::npos && w.find("PHYSICS.MAXBODIES 200000") != std::string::npos, "the keys are written");
    check(w.find("PHYSICS.VELOCITYSTEPS") < w.find("PHYSICS.SLEEPTIME"), "in the order the project stated them");
    const fmt::ProjectDesc back = parse(w);
    check(back.physicsSettings == d.physicsSettings && back.physThreads == 6 && back.physMaxBodies == 200000, "and read back equal");

    // A re-save over the existing text: the owned lines are replaced once, the unknown one is kept.
    const std::string existing = std::string(kManifest) + "PHYSICS.VELOCITYSTEPS 99\n";
    d.physicsSettings[0].second = 12.0f;
    const std::string rw = fmt::writeOcproject(d, existing);
    check(count(rw, "PHYSICS.VELOCITYSTEPS") == 1 && rw.find("PHYSICS.VELOCITYSTEPS 12") != std::string::npos, "an owned key is replaced once, never duplicated");
    check(rw.find("PHYSICS.FUTURETHING 7") != std::string::npos, "an unknown PHYSICS.* key survives the re-save");
    check(count(rw, "PHYSICS.THREADS") == 1 && count(rw, "PHYSICS.MAXBODIES") == 1, "and so do the limits");

    fmt::ProjectDesc none = parse("OCPROJECT 1\nNAME A\n");
    none.physicsSettings.emplace_back("noSuchSetting", 5.0f);
    check(fmt::writeOcproject(none, "").find("NOSUCHSETTING") == std::string::npos, "a name the mirror does not know is not written");
}

static void testConfigureRebuilds() {
    AVER_INFO("=== configureProjectPhysics: settings, limits, rebuild ===");
    resetAll();
    aver_phys_init();
    check(aver_phys_max_bodies() == 65536, "the world starts with the default limits");

    fmt::ProjectDesc d = parse(kManifest);
    const bool rebuilt = game::configureProjectPhysics(d, "Test");
    check(rebuilt, "an empty world is rebuilt for the project's limits");
    check(aver_phys_max_bodies() == 200000 && active(AVER_PHYS_CF_MAX_BODIES) == 200000.0f, "max bodies follows the manifest");
    check(active(AVER_PHYS_CF_THREADS) == 6.0f, "so does THREADS");
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 14.0f && setting(AVER_PHYS_WS_SLEEP_TIME) == 1.0f, "and the solver settings");
    check(aver_phys_config_differs() == 0, "the world and the pending config agree");

    // A second project that states a bigger limit while the world holds a body.
    const int32_t body = aver_phys_add_dynamic_box(0, 0, 100, 10, 10, 10, 1.0f);
    fmt::ProjectDesc d2 = parse("OCPROJECT 1\nNAME B\nPHYSICS.MAXBODIES 300000\nPHYSICS.VELOCITYSTEPS 6\n");
    g_needle = "apply when the world is next created";
    g_lines = 0;
    setLogSink(&countingSink, nullptr);
    const bool again = game::configureProjectPhysics(d2, "Test");
    setLogSink(nullptr, nullptr);
    check(!again, "a world with a body is not rebuilt");
    check(aver_phys_max_bodies() == 200000 && body != 0, "it keeps its limits and its body");
    check(g_lines == 1, "and the log says the limits apply when it is next created (" + std::to_string(g_lines) + " line)");
    check(aver_phys_config_differs() == 1, "the pending config differs, so the page's badge shows");
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 6.0f, "the live settings applied anyway");
    check(setting(AVER_PHYS_WS_SLEEP_TIME) == 0.5f, "the previous project's sleep time went back to the default");

    // A third project that states nothing: the defaults come back.
    aver_phys_remove_body(body);
    const fmt::ProjectDesc d3 = parse("OCPROJECT 1\nNAME C\n");
    check(game::configureProjectPhysics(d3, "Test"), "an empty world is rebuilt back to the defaults");
    check(aver_phys_max_bodies() == 65536 && active(AVER_PHYS_CF_THREADS) == -1.0f, "limits are the defaults again");
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 10.0f, "and so is the solver");

    // Applying with no world stores for the next one.
    aver_phys_shutdown();
    check(!game::configureProjectPhysics(d, "Test"), "no world: nothing to rebuild");
    float v = 0;
    aver_phys_config_get(AVER_PHYS_CF_MAX_BODIES, &v);
    check(v == 200000.0f, "the limit is pending for the world that follows");
    aver_phys_init();
    check(aver_phys_max_bodies() == 200000, "and that world uses it");

    // gravity and the fixed step still apply after the level.
    fmt::ProjectDesc g = parse("OCPROJECT 1\nNAME D\nPHYSICS.GRAVITY 0 0 -490\nPHYSICS.FIXEDSTEP 0.02\nPHYSICS.VELOCITYSTEPS 5\n");
    game::applyProjectPhysicsKeys(g);   // GameTick.hpp's applyProjectPhysics forwards here
    check(std::fabs(aver_phys_fixed_step() - 0.02f) < 1e-7f, "applyProjectPhysicsKeys still sets the fixed step");
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 5.0f, "and the solver settings, for a host that never configured");

    // a value outside the range is clamped, with a warning, not refused
    fmt::ProjectDesc lo = parse("OCPROJECT 1\nNAME E\nPHYSICS.VELOCITYSTEPS 1\n");
    game::configureProjectPhysics(lo, "Test");
    check(setting(AVER_PHYS_WS_VELOCITY_STEPS) == 2.0f, "VELOCITYSTEPS 1 is clamped to 2");

    resetAll();
}

// PHYSICS.SHAPECACHE reaches the physics module's switch before any level, and the next project that does not state it puts it back.
static void testShapeCacheKey() {
    AVER_INFO("=== PHYSICS.SHAPECACHE ===");
    resetAll();
    check(aver_phys_shape_cache_enabled() == 0, "off by default");
    const fmt::ProjectDesc on = parse("OCPROJECT 1\nNAME S\nPHYSICS.SHAPECACHE 1\n");
    check(on.physShapeCache == 1, "the key parses");
    game::configureProjectPhysics(on, "Test");
    check(aver_phys_shape_cache_enabled() == 1, "configureProjectPhysics turns the cache on");
    game::configureProjectPhysics(parse("OCPROJECT 1\nNAME S\n"), "Test");
    check(aver_phys_shape_cache_enabled() == 0, "a later project that does not state it puts it back to off");
    game::configureProjectPhysics(parse("OCPROJECT 1\nNAME S\nPHYSICS.SHAPECACHE 0\n"), "Test");
    check(aver_phys_shape_cache_enabled() == 0, "0 is off");
    resetAll();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("project PHYSICS.* keys: mirror, parse, write, configure, rebuild");

    testMirror();
    testParseAndWrite();
    testConfigureRebuilds();
    testShapeCacheKey();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
