// PhysicsProfileTest -- the profiler bridge (physics_world_abi.h, aver_phys_profile_*), through the ABI only.
//
//   * off by default: stepping with the profiler off publishes nothing,
//   * enabled, a loaded scene publishes a tree whose rows include Jolt's own "Update" under the engine's "PhysicsStep", and
//     the engine's fixed-step stages (Characters, Buoyancy, SoftBodyBegin, VehicleStepBegin, VehicleStepEnd, JointBreakChecks),
//   * every row's inclusive time is at least its exclusive time, parents come before their children with depth = parent + 1,
//   * rows merge by name across threads: no two rows share (parent, name),
//   * the engine's PhysicsStep runs once per fixed step,
//   * reset clears the table, a disabled profiler leaves the table frozen, summary names the largest scopes.
//
// SKIPS (exit 0, loudly) when the build has no profiler hook (cmake -DAVER_PHYSICS_PROFILER=OFF, or a configuration without
// Jolt's JPH_EXTERNAL_PROFILE). The overhead with the hook idle is a headed measurement (design 2.5, 6.14), not a ctest.
//
// Hand-run, exit code = failure count, same shape as PhysicsConfigTest.cpp.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
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

struct Row {
    std::string name;
    double incl = 0, excl = 0, maxMs = 0, calls = 0;
    int parent = -1, depth = 0;
};

static std::vector<Row> rows() {
    std::vector<Row> out;
    const int n = aver_phys_profile_count();
    for (int i = 0; i < n; ++i) {
        uint8_t name[96] = {};
        double v[4] = {};
        int pd[2] = {};
        if (!aver_phys_profile_get(i, name, static_cast<int32_t>(sizeof name), v, pd)) break;
        Row r;
        r.name = reinterpret_cast<const char*>(name);
        r.incl = v[0]; r.excl = v[1]; r.maxMs = v[2]; r.calls = v[3];
        r.parent = pd[0]; r.depth = pd[1];
        out.push_back(r);
    }
    return out;
}

// MSVC's __FUNCTION__ names Jolt's scope "JPH::PhysicsSystem::Update"; Clang and GCC give a longer pretty name. Match the member.
static bool isJoltUpdate(const std::string& n) { return n.find("PhysicsSystem::Update") != std::string::npos; }

static bool has(const std::vector<Row>& r, const char* name) {
    for (const Row& x : r) if (x.name == name) return true;
    return false;
}

static void scene() {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50, 3000, 3000, 50);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            aver_phys_add_dynamic_box(static_cast<float>(i) * 25.0f, static_cast<float>(j) * 25.0f, 30.0f, 10, 10, 10, 10.0f);
    aver_phys_character_create(30.0f, 170.0f, 400.0f, 0.0f, 100.0f);   // so the Characters scope runs
}

static void testOffByDefault() {
    AVER_INFO("=== off by default ===");
    scene();
    check(aver_phys_profile_is_enabled() == 0, "the profiler starts off");
    aver_phys_profile_reset();
    step(120);
    check(aver_phys_profile_count() == 0, "stepping with the profiler off publishes nothing");
}

static void testTree() {
    AVER_INFO("=== the table of a loaded scene ===");
    scene();
    check(aver_phys_profile_enable(1) == 1 && aver_phys_profile_is_enabled() == 1, "enable succeeds");
    step(150);
    const std::vector<Row> r = rows();
    check(!r.empty(), "a table is published after enough fixed steps");

    check(has(r, "PhysicsStep"), "the engine's PhysicsStep scope is a row");
    bool hasUpdate = false;
    for (const Row& x : r) hasUpdate = hasUpdate || isJoltUpdate(x.name);
    check(hasUpdate, "Jolt's own Update scope is a row");
    for (const char* n : {"Characters", "Buoyancy", "SoftBodyBegin", "VehicleStepBegin", "VehicleStepEnd", "JointBreakChecks"})
        check(has(r, n), std::string("the engine stage '") + n + "' is a row");

    // Jolt's Update nests under the engine's PhysicsStep.
    bool nested = false;
    for (const Row& x : r)
        if (isJoltUpdate(x.name) && x.parent >= 0 && r[static_cast<size_t>(x.parent)].name == "PhysicsStep") nested = true;
    check(nested, "Update is a child of PhysicsStep");

    bool inclOk = true, parentOk = true, depthOk = true, rootsOk = true;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i].incl + 1e-9 < r[i].excl) inclOk = false;
        if (r[i].parent >= static_cast<int>(i)) parentOk = false;          // parents come first, so the tree is acyclic
        if (r[i].parent < 0) { if (r[i].depth != 0) rootsOk = false; }
        else if (r[i].depth != r[static_cast<size_t>(r[i].parent)].depth + 1) depthOk = false;
    }
    check(inclOk, "every row's inclusive time is at least its exclusive time");
    check(parentOk, "parent rows precede their children");
    check(depthOk && rootsOk, "depth is parent depth + 1, roots are depth 0");

    std::set<std::pair<int, std::string>> seen;
    bool unique = true;
    for (const Row& x : r) unique = seen.insert({x.parent, x.name}).second && unique;
    check(unique, "no two rows share (parent, name): scope names merge across threads");

    for (const Row& x : r)
        if (x.name == "PhysicsStep") check(x.calls > 0.95 && x.calls < 1.05, "PhysicsStep runs once per fixed step");
    check(!r.empty() && r[0].incl > 0.0, "the first row has time in it");

    uint8_t summary[256] = {};
    const int need = aver_phys_profile_summary(summary, static_cast<int32_t>(sizeof summary));
    check(need > 1 && std::strlen(reinterpret_cast<const char*>(summary)) + 1 == static_cast<size_t>(need), "summary names the largest scopes");
    check(aver_phys_profile_summary(nullptr, 0) == need, "summary with no buffer answers the length needed");

    double v[4]; int pd[2]; uint8_t nm[8];
    check(aver_phys_profile_get(-1, nm, 8, v, pd) == 0 && aver_phys_profile_get(aver_phys_profile_count(), nm, 8, v, pd) == 0,
          "a bad row index is refused");
    check(aver_phys_profile_get(0, nm, 4, nullptr, nullptr) == 1 && std::strlen(reinterpret_cast<const char*>(nm)) <= 3,
          "a short name buffer is truncated and terminated");
}

static void testFrozenAndReset() {
    AVER_INFO("=== disabled freezes the table, reset clears it ===");
    scene();
    aver_phys_profile_enable(1);
    step(150);
    const std::vector<Row> before = rows();
    check(!before.empty(), "table present");

    aver_phys_profile_enable(0);
    check(aver_phys_profile_is_enabled() == 0, "disable takes");
    step(150);
    const std::vector<Row> after = rows();
    bool same = before.size() == after.size();
    for (size_t i = 0; same && i < before.size(); ++i) same = before[i].name == after[i].name && before[i].incl == after[i].incl;
    check(same, "a disabled profiler leaves the published table untouched");

    aver_phys_profile_reset();
    check(aver_phys_profile_count() == 0, "reset clears the table");
    aver_phys_profile_enable(1);
    step(30);
    check(aver_phys_profile_count() > 0, "a re-enabled profiler fills the table again (first table after a few steps)");
    aver_phys_profile_enable(0);
}

int main() {
    if (!(aver_phys_build_flags() & AVER_PHYS_BUILD_PROFILER)) {
        AVER_WARN("=== SKIPPED: this build has no profiler hook (AVER_PHYS_BUILD_PROFILER is clear) ===");
        check(aver_phys_profile_enable(1) == 0, "enable is refused without the hook");
        return g_failures;
    }
    testOffByDefault();
    testTree();
    testFrozenAndReset();
    aver_phys_shutdown();
    AVER_INFO("=== PhysicsProfileTest: {} checks, {} failures ===", g_checks, g_failures);
    return g_failures;
}
