// PhysicsDeterminismTest -- does the same world, stepped the same way, hash the same at every fixed step? Through the ABI only
// (physics_state_abi.h: aver_phys_state_hash, aver_phys_hash_log, aver_phys_snapshot_diff, aver_phys_step_exact).
//
//   * replay twice: the same scene built twice and stepped 600 steps has an equal hash at every step,
//   * step_exact(1) x N is the same simulation as aver_phys_step x N,
//   * thread-count independence: 0 (single-threaded), 1 and 4 workers hash equal at every step (PHYSICS.THREADS),
//   * restore and replay: a snapshot, 60 steps, a restore and 60 steps again differ nowhere (snapshot_diff finds the first byte),
//   * the hash log: a run writes one "step hash" line per fixed step, and two runs write the same file.
//
//   * the golden hash: the same scene at fixed checkpoints of a 600-step run. Printed on every run; when the environment
//     variable AVER_PHYSICS_GOLDEN_OUT names a file it is written there ("precision N", then "<step> <hash>" lines, nothing that
//     names the platform), which is what the CI job "physics-determinism" collects from Windows and macOS and diffs: the
//     cross-platform claim is made when those two files are equal and the checkpoints below are filled in, and not before. In a
//     build with AVER_PHYSICS_CROSS_PLATFORM the checkpoints are compared against kGolden; without it the comparison is skipped,
//     loudly, because a build that does not try for bit-identical results has nothing to promise. An unrecorded kGolden entry
//     (0) passes and says so: the first CI run prints what to put there.
//
// Nothing in this file claims more than the build it runs on.
//
// Hand-run, exit code = failure count, same shape as PhysicsStateTest.cpp.
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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// ---- the scene -----------------------------------------------------------------------------------------------------------

static void freshWorld(int threads) {
    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_config_set(AVER_PHYS_CF_THREADS, static_cast<float>(threads));
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 5000.0f, 5000.0f, 50.0f);
}

static int32_t buildCar(float x, float y, float z) {
    const float hz = 2.0f;
    const float sag = 981.0f / ((2.0f * kPi * hz) * (2.0f * kPi * hz));
    const float attachZ = 33.0f + 30.0f - sag;
    const int32_t v = aver_phys_vehicle_create(220.0f, 90.0f, 35.0f, 25.0f, 0.0f, 0.0f, 40.0f, 1300.0f, x, y, z, 0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        aver_phys_vehicle_add_wheel(v, front ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, 33.0f, 22.0f, 5.0f, 30.0f, hz,
                                    0.5f, front ? 35.0f : 0.0f, 1500.0f, 2500.0f, front ? 0 : 1);
    }
    const float aids[AVER_PHYS_VD_COUNT] = {1, 1, 0.25f, 8, 1, 0.35f, 3, 5, 0, 6, 8, 8, 1, 50, 0.3f, 30, 0.45f, 12};
    aver_phys_vehicle_set_assists(v, aids, AVER_PHYS_VD_COUNT);
    aver_phys_vehicle_finish(v, 60.0f);
    return v;
}

// Boxes, a car under throttle with the aids on, a walking character, a pinned cloth, a weight on a joint and a limp ragdoll: every
// kind of state a snapshot holds, on one floor. The same call builds the same world.
static void buildScene() {
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 2; ++k)
                aver_phys_add_dynamic_box(static_cast<float>(i) * 45.0f, static_cast<float>(j) * 45.0f,
                                          12.0f + static_cast<float>(k) * 30.0f + static_cast<float>((i + j) % 2) * 2.0f, 10.0f, 10.0f, 10.0f, 5.0f);
    const int32_t car = buildCar(-1200.0f, 0.0f, 5.0f);
    aver_phys_vehicle_set_input(car, 0.7f, 0.15f, 0.0f, 0.0f);
    const int32_t ch = aver_phys_character_create(30.0f, 170.0f, 800.0f, 0.0f, 100.0f);
    aver_phys_character_set_velocity(ch, 150.0f, 40.0f, 0.0f);

    const int32_t n = 6;
    std::vector<float> verts;
    std::vector<int32_t> idx;
    for (int32_t iy = 0; iy < n; ++iy)
        for (int32_t ix = 0; ix < n; ++ix) {
            verts.push_back(static_cast<float>(ix) * 14.0f - 35.0f);
            verts.push_back(static_cast<float>(iy) * 14.0f - 35.0f);
            verts.push_back(300.0f);
        }
    for (int32_t iy = 0; iy + 1 < n; ++iy)
        for (int32_t ix = 0; ix + 1 < n; ++ix) {
            const int32_t a = iy * n + ix, b = a + 1, c = a + n + 1, d = a + n;
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    std::vector<float> invMass(static_cast<size_t>(n * n), 1.0f);
    for (int32_t ix = 0; ix < n; ++ix) invMass[static_cast<size_t>(ix)] = 0.0f;
    aver_phys_softbody_create(verts.data(), n * n, idx.data(), static_cast<int32_t>(idx.size()), invMass.data(), 0.0f, 900.0f, 0.0f, 0.0f, 0.0f);

    const int32_t weight = aver_phys_add_dynamic_box(0.0f, -800.0f, 400.0f, 10.0f, 10.0f, 10.0f, 10.0f);
    const float pivot[3] = {0.0f, -800.0f, 500.0f};
    const int32_t joint = aver_phys_joint_point(weight, AVER_PHYS_WORLD_BODY, pivot);
    aver_phys_joint_set_break(joint, 5000.0f, 0.0f);

    std::vector<int32_t> parents;
    std::vector<float> pos, rot, caps, lim;
    for (int i = 0; i < 3; ++i) {
        parents.push_back(i - 1);
        pos.insert(pos.end(), {1500.0f + 50.0f * static_cast<float>(i), 600.0f, 300.0f});
        rot.insert(rot.end(), {0.0f, 0.0f, 0.0f, 1.0f});
        caps.insert(caps.end(), {0.0f, 0.0f, 0.0f, 50.0f, 0.0f, 0.0f, 8.0f});
        lim.insert(lim.end(), {80.0f, 80.0f, 30.0f});
    }
    const int32_t rd = aver_phys_ragdoll_create(3, parents.data(), pos.data(), rot.data(), caps.data(), lim.data(), 30.0f, 0);
    aver_phys_ragdoll_set_mode(rd, AVER_PHYS_RAGDOLL_LIMP);
}

static int firstDiff(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    if (a.size() != b.size()) return 0;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return static_cast<int>(i);
    return -1;
}

static std::vector<uint64_t> runExact(int threads, int steps) {
    freshWorld(threads);
    buildScene();
    std::vector<uint64_t> h;
    h.reserve(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) { aver_phys_step_exact(1); h.push_back(H()); }
    return h;
}

// ---- 1 and 2 -------------------------------------------------------------------------------------------------------------

static void testReplayTwice() {
    AVER_INFO("=== the same scene, built twice, hashes the same at every one of 600 steps ===");
    const std::vector<uint64_t> a = runExact(-1, 600);
    const std::vector<uint64_t> b = runExact(-1, 600);
    const int d = firstDiff(a, b);
    check(d < 0, d < 0 ? "600 per-step hashes equal" : "the runs first differ at step " + std::to_string(d + 1));
    check(!a.empty() && a.front() != a.back(), "and the state does move (first hash != last hash)");
}

static void testStepExactIsStep() {
    AVER_INFO("=== step_exact(1) x N is aver_phys_step x N ===");
    freshWorld(-1);
    buildScene();
    std::vector<uint64_t> viaStep;
    for (int i = 0; i < 200; ++i) { aver_phys_step(kDt); viaStep.push_back(H()); }
    const std::vector<uint64_t> viaExact = runExact(-1, 200);
    const int d = firstDiff(viaStep, viaExact);
    check(d < 0, d < 0 ? "200 hashes equal" : "they first differ at step " + std::to_string(d + 1));
}

static void testThreadCounts() {
    AVER_INFO("=== the worker thread count does not change the simulation ===");
    const int steps = 300;
    const std::vector<uint64_t> single = runExact(0, steps);
    double workers[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(workers, AVER_PHYS_ST_COUNT);
    check(workers[AVER_PHYS_ST_WORKER_THREADS] == 0.0, "THREADS 0 builds the single-threaded job system");
    const std::vector<uint64_t> one = runExact(1, steps);
    aver_phys_stats(workers, AVER_PHYS_ST_COUNT);
    check(workers[AVER_PHYS_ST_WORKER_THREADS] == 1.0, "THREADS 1 reports one worker");
    const std::vector<uint64_t> four = runExact(4, steps);
    aver_phys_stats(workers, AVER_PHYS_ST_COUNT);
    check(workers[AVER_PHYS_ST_WORKER_THREADS] == 4.0, "THREADS 4 reports four");
    int d = firstDiff(single, one);
    check(d < 0, d < 0 ? "0 and 1 workers hash equal at all 300 steps" : "0 and 1 first differ at step " + std::to_string(d + 1));
    d = firstDiff(single, four);
    check(d < 0, d < 0 ? "0 and 4 workers hash equal at all 300 steps" : "0 and 4 first differ at step " + std::to_string(d + 1));
    aver_phys_config_reset(-1);
}

// ---- 3. restore and replay, found by snapshot_diff ---------------------------------------------------------------

static void testRestoreReplayHasNoDiff() {
    AVER_INFO("=== snapshot, step, restore, step: the two ends are byte-equal ===");
    freshWorld(-1);
    buildScene();
    for (int i = 0; i < 90; ++i) aver_phys_step_exact(1);
    const int32_t s0 = aver_phys_snapshot_capture(0, 0);
    aver_phys_step_exact(60);
    const int32_t a = aver_phys_snapshot_capture(0, 0);
    check(aver_phys_snapshot_restore(s0, 0) == 1, "restore the start");
    aver_phys_step_exact(60);
    const int32_t b = aver_phys_snapshot_capture(0, 0);
    const int off = aver_phys_snapshot_diff(a, b);
    check(off == -1, off == -1 ? "no differing byte after the replay" : "the replay first differs at payload byte " + std::to_string(off));
    check(aver_phys_snapshot_hash(a) == aver_phys_snapshot_hash(b), "and the hashes agree");
    check(aver_phys_snapshot_diff(s0, a) >= 0, "while the start and the end do differ");
}

// ---- 4. the hash log ----------------------------------------------------------------------------------------------------

static std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void testHashLog() {
    AVER_INFO("=== the hash log: one line per fixed step, identical across runs ===");
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path p1 = dir / "aver_phys_hashlog_run1.txt";
    const std::filesystem::path p2 = dir / "aver_phys_hashlog_run2.txt";
    auto run = [&](const std::filesystem::path& p) {
        freshWorld(-1);
        buildScene();
        const std::string path = p.string();
        check(aver_phys_hash_log(path.c_str()) == 1, "the log opens: " + path);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        check(aver_phys_hash_log(nullptr) == 1, "and closes");
    };
    run(p1);
    run(p2);
    const std::string a = slurp(p1), b = slurp(p2);
    int lines = 0;
    for (const char c : a) lines += c == '\n' ? 1 : 0;
    check(lines == 120, "120 fixed steps wrote 120 lines, got " + std::to_string(lines));
    check(!a.empty() && a == b, "the two runs wrote byte-identical files");
    unsigned long long step = 0, hash = 0;
    check(std::sscanf(a.c_str(), "%llu %llx", &step, &hash) == 2 && step == 1 && hash != 0, "a line is the step number then the hash");
    check(aver_phys_hash_log("") == 0, "an empty path is refused");
    aver_phys_hash_log(nullptr);
    std::error_code ec;
    std::filesystem::remove(p1, ec);
    std::filesystem::remove(p2, ec);
}

// ---- 5. the golden hash --------------------------------------------------------------------------------------------------

// The recorded checkpoints (steps 100, 200 ... 600) per precision, from a build with AVER_PHYSICS_CROSS_PLATFORM on. 0 = not
// recorded yet. They are filled in from the "physics-determinism" CI job's output once Windows and macOS agree; until then the
// test passes and prints the line to copy.
static const int kCheckpoints = 6;
static const uint64_t kGoldenSingle[kCheckpoints] = {0, 0, 0, 0, 0, 0};
static const uint64_t kGoldenDouble[kCheckpoints] = {0, 0, 0, 0, 0, 0};

static void testGolden() {
    AVER_INFO("=== the golden hash: a fixed 600-step scene at six checkpoints ===");
    const bool cross = (aver_phys_build_flags() & AVER_PHYS_BUILD_CROSS_PLATFORM) != 0;
    const bool dbl = aver_phys_precision() == AVER_PHYS_PRECISION_DOUBLE;
    // Single-threaded: the golden must not depend on the runner's core count (testThreadCounts proves the result does not, but
    // the golden is the one place worth removing the question from).
    freshWorld(0);
    buildScene();
    uint64_t got[kCheckpoints] = {};
    for (int i = 1; i <= 600; ++i) {
        aver_phys_step_exact(1);
        if (i % 100 == 0) got[i / 100 - 1] = H();
    }
    std::string text = std::string("precision ") + (dbl ? "2" : "1") + "\n";
    for (int k = 0; k < kCheckpoints; ++k) {
        char line[64];
        std::snprintf(line, sizeof line, "%d %016llx\n", (k + 1) * 100, static_cast<unsigned long long>(got[k]));
        text += line;
    }
    AVER_INFO("   golden checkpoints ({} precision, cross-platform build {}):\n{}", dbl ? "double" : "single", cross ? "ON" : "off", text);
    check(got[kCheckpoints - 1] != 0 && got[0] != got[kCheckpoints - 1], "the scene moves: the first and last checkpoints differ");

    if (const char* out = std::getenv("AVER_PHYSICS_GOLDEN_OUT"); out && out[0]) {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << text;
        check(f.good(), std::string("the checkpoints were written to ") + out);
    }

    if (!cross) {
        AVER_INFO("   SKIP  golden comparison: this binary is not built with AVER_PHYSICS_CROSS_PLATFORM, so it promises nothing across machines");
        return;
    }
    const uint64_t* want = dbl ? kGoldenDouble : kGoldenSingle;
    bool anyRecorded = false, allEqual = true;
    for (int k = 0; k < kCheckpoints; ++k) {
        if (want[k] == 0) continue;
        anyRecorded = true;
        allEqual = allEqual && want[k] == got[k];
    }
    if (!anyRecorded) {
        AVER_INFO("   NO GOLDEN RECORDED for this precision: copy the checkpoints above into kGolden{} once the CI job shows Windows and macOS agree", dbl ? "Double" : "Single");
        return;
    }
    check(allEqual, "every recorded checkpoint equals this build's");
}

int main() {
    AVER_INFO("=== PhysicsDeterminismTest ===");
    testReplayTwice();
    testStepExactIsStep();
    testThreadCounts();
    testRestoreReplayHasNoDiff();
    testHashLog();
    testGolden();
    aver_phys_shutdown();
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
