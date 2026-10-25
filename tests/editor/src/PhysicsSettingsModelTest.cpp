// PhysicsSettingsModelTest -- the model behind Project Settings > Physics (sandbox/src/PhysicsSettingsModel.hpp):
// rows from the physics ABI's table, validation, "changed" detection, canonical omission of defaults, the limits'
// mapping onto the project fields, and the restart-badge logic. No ImGui, no window; the real physics module.
#include "PhysicsSettingsModel.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"

#include <cstdio>
#include <limits>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static float live(int key) { float v = -1; aver_phys_setting_get(key, &v); return v; }

static const PhysicsSettingRow* row(const std::vector<PhysicsSettingRow>& rows, const char* name) {
    for (const PhysicsSettingRow& r : rows) if (r.name == name) return &r;
    return nullptr;
}

static void testRows() {
    AVER_INFO("=== rows come from the ABI table ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    const std::vector<PhysicsSettingRow> rows = physicsSettingRows();
    check(static_cast<int>(rows.size()) == aver_phys_setting_count(), "one row per setting");
    bool match = true;
    for (const PhysicsSettingRow& r : rows) {
        float lo = 0, hi = 0, def = 0;
        aver_phys_setting_range(r.key, &lo, &hi, &def);
        if (r.name != aver_phys_setting_name(r.key) || r.unit != aver_phys_setting_unit(r.key) || r.min != lo || r.max != hi || r.def != def) match = false;
    }
    check(match, "names, units, ranges and defaults are the ABI's");
    const PhysicsSettingRow* v = row(rows, "velocitySteps");
    check(v && v->projectKey == "PHYSICS.VELOCITYSTEPS" && v->isCount && !v->isBool && v->def == 10.0f, "velocitySteps is a count with its project key");
    const PhysicsSettingRow* s = row(rows, "allowSleeping");
    check(s && s->isBool && s->projectKey == "PHYSICS.ALLOWSLEEPING", "allowSleeping is a flag");
    const PhysicsSettingRow* d = row(rows, "speculativeDistance");
    check(d && !d->isCount && d->unit == "cm", "a length is a float in cm");

    const std::vector<PhysicsLimitRow> limits = physicsLimitRows();
    check(limits.size() == AVER_PHYS_CF_COUNT, "five limits");
    check(limits[AVER_PHYS_CF_MAX_BODIES].projectKey == "PHYSICS.MAXBODIES" && limits[AVER_PHYS_CF_TEMP_MB].projectKey == "PHYSICS.TEMPALLOCMB" &&
          limits[AVER_PHYS_CF_THREADS].projectKey == "PHYSICS.THREADS", "limit rows use the manifest's own key names");
    check(limits[AVER_PHYS_CF_THREADS].min == -1.0f && limits[AVER_PHYS_CF_THREADS].def == -1.0f, "threads: -1 is automatic and the default");

    check(physicsSettingTooltip(*v).find("cm") == std::string::npos && physicsSettingTooltip(*v).find("count") != std::string::npos &&
          physicsSettingTooltip(*v).find("default: 10") != std::string::npos, "a tooltip carries the unit and the default");
    check(physicsSettingTooltip(*d).find("unit: cm") != std::string::npos && physicsSettingTooltip(*d).find("range: 0.1 .. 20") != std::string::npos, "and the range");
    check(physicsSettingTooltip(*row(rows, "deterministic")).find("BREAKS") != std::string::npos, "the determinism row warns");
}

static void testValidationAndOmission() {
    AVER_INFO("=== validation and canonical omission ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    const std::vector<PhysicsSettingRow> rows = physicsSettingRows();
    const PhysicsSettingRow& v = *row(rows, "velocitySteps");
    std::string why;
    check(!physicsSettingValid(v, std::numeric_limits<float>::quiet_NaN(), &why) && !why.empty(), "NaN is invalid, with a reason");
    check(!physicsSettingValid(v, 1000.0f, &why) && why.find("2 .. 64") != std::string::npos, "out of range is invalid, and the reason names the range");
    check(physicsSettingValid(v, 14.0f), "14 is valid");
    check(physicsSettingClamp(v, 1.0f) == 2.0f && physicsSettingClamp(v, 99.0f) == 64.0f && physicsSettingClamp(v, 7.4f) == 7.0f, "clamp and round");

    fmt::ProjectDesc p;
    check(!physicsSettingStated(p, v) && physicsSettingValue(p, v) == 10.0f && !physicsSettingChanged(p, v), "a project that states nothing has the default");
    check(setPhysicsSettingValue(p, v, 14.0f, false) && p.physicsSettings.size() == 1, "setting a value adds it");
    check(physicsSettingStated(p, v) && physicsSettingValue(p, v) == 14.0f && physicsSettingChanged(p, v), "and it reads back as changed");
    check(!setPhysicsSettingValue(p, v, 14.0f, false), "setting the same value changes nothing");
    check(setPhysicsSettingValue(p, v, 20.0f, false) && p.physicsSettings.size() == 1 && physicsSettingValue(p, v) == 20.0f, "a new value replaces it in place");
    check(setPhysicsSettingValue(p, v, 10.0f, false) && p.physicsSettings.empty(), "a value at the default REMOVES the entry (canonical form)");
    check(!setPhysicsSettingValue(p, v, 10.0f, false), "and setting the default again changes nothing");
    check(!setPhysicsSettingValue(p, v, std::numeric_limits<float>::quiet_NaN(), false) && p.physicsSettings.empty(), "NaN changes nothing");
    check(setPhysicsSettingValue(p, v, 1.0f, false) && physicsSettingValue(p, v) == 2.0f, "an out-of-range value is clamped, not refused");

    const PhysicsSettingRow& b = *row(rows, "baumgarte");
    setPhysicsSettingValue(p, b, 0.2000000001f, false);
    check(!physicsSettingStated(p, b), "a float within rounding of the default is the default");
    setPhysicsSettingValue(p, b, 0.35f, false);
    check(physicsChangedCount(p, rows) == 2, "the changed count");
    check(resetPhysicsSetting(p, v, false) && !physicsSettingStated(p, v) && physicsChangedCount(p, rows) == 1, "reset removes one");
    check(!resetPhysicsSetting(p, v, false), "resetting an unstated setting changes nothing");

    // the order the project stated them is the order they are kept (and written)
    fmt::ProjectDesc q;
    setPhysicsSettingValue(q, *row(rows, "sleepTime"), 2.0f, false);
    setPhysicsSettingValue(q, v, 12.0f, false);
    check(q.physicsSettings.size() == 2 && q.physicsSettings[0].first == "sleepTime" && q.physicsSettings[1].first == "velocitySteps", "stated order is kept");
}

static void testApplyLive() {
    AVER_INFO("=== setting applies to the physics module ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    const std::vector<PhysicsSettingRow> rows = physicsSettingRows();
    const PhysicsSettingRow& v = *row(rows, "velocitySteps");
    fmt::ProjectDesc p;
    setPhysicsSettingValue(p, v, 14.0f);
    check(live(v.key) == 14.0f, "applyLive writes the live setting");
    setPhysicsSettingValue(p, v, 6.0f, false);
    check(live(v.key) == 14.0f, "applyLive = false leaves it alone");
    resetPhysicsSetting(p, v);
    check(live(v.key) == 10.0f, "reset restores the live default too");
    aver_phys_setting_reset(-1);
}

static void testLimits() {
    AVER_INFO("=== limits map onto the project's fields ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    const std::vector<PhysicsLimitRow> limits = physicsLimitRows();
    fmt::ProjectDesc p;
    const PhysicsLimitRow& mb = limits[AVER_PHYS_CF_MAX_BODIES];
    check(!physicsLimitStated(p, mb) && physicsLimitValue(p, mb) == 65536.0f, "an unstated limit reads as its default");
    check(setPhysicsLimit(p, mb, 200000.0f) && p.physMaxBodies == 200000 && physicsLimitChanged(p, mb), "setting a limit writes the project field");
    float pend = 0;
    aver_phys_config_get(AVER_PHYS_CF_MAX_BODIES, &pend);
    check(pend == 200000.0f, "and the pending config");
    check(setPhysicsLimit(p, mb, 65536.0f) && p.physMaxBodies == -1 && !physicsLimitStated(p, mb), "the default is stored as unstated");
    aver_phys_config_get(AVER_PHYS_CF_MAX_BODIES, &pend);
    check(pend == 65536.0f, "and the pending config is the default again");

    const PhysicsLimitRow& th = limits[AVER_PHYS_CF_THREADS];
    check(!physicsLimitStated(p, th) && p.physThreads == -2, "threads is unstated at -2");
    check(setPhysicsLimit(p, th, 0.0f) && p.physThreads == 0 && physicsLimitStated(p, th) && physicsLimitChanged(p, th), "THREADS 0 (single-threaded) is a stated value");
    check(setPhysicsLimit(p, th, -1.0f) && p.physThreads == -2, "-1 is the default, so it is unstated");
    check(setPhysicsLimit(p, th, 1000.0f) && p.physThreads == 64, "a limit clamps into its range");
    check(!setPhysicsLimit(p, th, std::numeric_limits<float>::quiet_NaN()), "NaN is refused");
    check(resetPhysicsLimit(p, th) && p.physThreads == -2, "reset");

    setPhysicsLimit(p, limits[AVER_PHYS_CF_TEMP_MB], 32.0f, false);
    setPhysicsLimit(p, limits[AVER_PHYS_CF_MAX_CONTACTS], 4096.0f, false);
    setPhysicsLimit(p, limits[AVER_PHYS_CF_MAX_BODY_PAIRS], 2048.0f, false);
    check(p.physTempAllocatorMb == 32 && p.physMaxContacts == 4096 && p.physMaxBodyPairs == 2048, "each limit has its own field");
    aver_phys_config_reset(-1);
}

static void testRestartBadge() {
    AVER_INFO("=== the restart badge and the rebuild button ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    check(!physicsRestartBadge(), "no world, no badge");
    check(physicsRebuildEnabled(), "and a rebuild is allowed (it is a plain init)");
    aver_phys_init();
    check(!physicsRestartBadge(), "a fresh world matches the pending config");

    const std::vector<PhysicsLimitRow> limits = physicsLimitRows();
    fmt::ProjectDesc p;
    setPhysicsLimit(p, limits[AVER_PHYS_CF_MAX_BODIES], 4096.0f);
    check(physicsRestartBadge(), "changing a limit lights the badge");
    const int32_t b = aver_phys_add_dynamic_box(0, 0, 100, 10, 10, 10, 1.0f);
    check(!physicsRebuildEnabled(), "a body disables the Rebuild button");
    aver_phys_remove_body(b);
    check(physicsRebuildEnabled(), "an empty world enables it");
    check(aver_phys_rebuild() == 1 && !physicsRestartBadge() && aver_phys_max_bodies() == 4096, "rebuilding applies the limit and clears the badge");
    resetPhysicsLimit(p, limits[AVER_PHYS_CF_MAX_BODIES]);
    check(physicsRestartBadge(), "putting the default back differs from the world again");
    aver_phys_shutdown();
    aver_phys_config_reset(-1);
}

static void testRoundTrip() {
    AVER_INFO("=== the model's project survives the file ===");
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    const std::vector<PhysicsSettingRow> rows = physicsSettingRows();
    const std::vector<PhysicsLimitRow> limits = physicsLimitRows();
    fmt::ProjectDesc p;
    p.name = "ModelRoundTrip";
    setPhysicsSettingValue(p, *row(rows, "sleepTime"), 2.5f, false);
    setPhysicsSettingValue(p, *row(rows, "velocitySteps"), 10.0f, false);   // the default: not stated
    setPhysicsSettingValue(p, *row(rows, "deterministic"), 0.0f, false);
    setPhysicsLimit(p, limits[AVER_PHYS_CF_THREADS], 0.0f, false);
    const std::string text = fmt::writeOcproject(p, "");
    check(text.find("PHYSICS.VELOCITYSTEPS") == std::string::npos, "a default is not written");
    check(text.find("PHYSICS.SLEEPTIME 2.5") != std::string::npos && text.find("PHYSICS.DETERMINISTIC 0") != std::string::npos &&
          text.find("PHYSICS.THREADS 0") != std::string::npos, "what changed is");
    fmt::ProjectDesc q;
    std::string err;
    check(fmt::parseOcproject(text, q, &err), "it parses back");
    check(q.physicsSettings == p.physicsSettings && q.physThreads == 0, "to the same settings");
    check(physicsSettingValue(q, *row(rows, "sleepTime")) == 2.5f && physicsSettingValue(q, *row(rows, "velocitySteps")) == 10.0f, "and the model reads the same values");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("physics settings model: rows, validation, omission, limits, badge");

    testRows();
    testValidationAndOmission();
    testApplyLive();
    testLimits();
    testRestartBadge();
    testRoundTrip();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
