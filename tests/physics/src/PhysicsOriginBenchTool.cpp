// PhysicsOriginBenchTool -- not a test. P-0 of LARGE_WORLD (docs/plans/wave11/LARGE_WORLD.md 2.3.7): what does a floating-origin shift cost the
// physics world, and does it loosen the broad phase? Run by hand on a real machine (the integrator measures; nothing here is asserted).
//
//   PhysicsOriginBenchTool [--scene small|large|stacks|all] [--repeat R] [--rays N] [--threads N] [--stack-steps N]
//
// SCENES (the three of 2.3.7):
//   small   10,000 statics + 1,000 dynamics        decision rule: at most 16 ms ships the design as it stands
//   large   100,000 statics + 2,000 dynamics       decision rule: over 50 ms, apply the levers of 2.3.7 in order and measure again
//   stacks  500 stacks of 5 boxes, asleep          the shift must leave every stack asleep
//
// For each scene it reports, in the order of the design's list:
//   * origin_shift ms in total (the whole world) and BY CLASS (the same content built as statics only, awake dynamics only and sleeping
//     stacks only, each in its own world, so a class's cost is not blurred into the others'): the first shift, then min / median / max over
//     R alternating shifts (+16.384 km on X, then back), with the ABI's own wall time beside the tool's,
//   * optimize_broadphase ms before any shift and again after the shifts (aver_phys_optimize_broadphase, timed),
//   * a raycast batch (N downward rays over the content): ms on the freshly optimised tree, ms right after the first shift (the loose-tree
//     penalty), ms after the shifts, and ms after optimising again, with the hit counts, which must all agree,
//   * for sleeping scenes, how many bodies were asleep before the shift and after it, after a further second of stepping.
//
// The shift is applied through physics_origin_abi.h, so this tool needs P-1 in the library. Not a *Test, so ctest does not pick it up.
// Exit code 0 unless it could not run. Positions are chosen as whole numbers of quarter metres, so every shift is exact and the content never
// leaves the 8 km that matters for the tool's own sanity check.
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_origin_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

static constexpr double kStepCm = 1638400.0;   // one region: the floating origin's step
static constexpr float  kDt = 1.0f / 60.0f;
static constexpr float  kCell = 400.0f;        // spacing of the static grid, cm

struct Config {
    const char* label = "";
    int statics = 0, dynamics = 0, stacks = 0;
    bool sleeping = false;   // allow sleeping (stacks); off keeps the dynamics awake so the awake path is what is timed
};

struct Result {
    std::string label;
    int bodies = 0, statics = 0, activeBefore = 0;
    double buildMs = 0, optimizeMs = 0, optimizeAfterMs = 0;
    double firstShiftMs = 0, minMs = 0, medianMs = 0, maxMs = 0, abiMedianMs = 0;
    double rayTightMs = 0, rayLooseMs = 0, rayAfterMs = 0, rayReoptMs = 0;
    int hitsTight = 0, hitsLoose = 0, hitsAfter = 0, hitsReopt = 0;
    int asleepBefore = 0, asleepAfter = 0;
    bool ok = false;
};

static uint32_t g_seed = 12345u;
static float rnd() { g_seed = g_seed * 1664525u + 1013904223u; return static_cast<float>((g_seed >> 8) & 0xFFFFu) / 65536.0f; }

static int activeDynamic() {
    double st[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(st, AVER_PHYS_ST_COUNT);
    return static_cast<int>(st[AVER_PHYS_ST_ACTIVE_DYNAMIC]);
}
static int dynamicCount() {
    double st[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(st, AVER_PHYS_ST_COUNT);
    return static_cast<int>(st[AVER_PHYS_ST_DYNAMIC]);
}

// Quarter-metre positions only (a multiple of 25 cm): the cm to m to cm round trip is exact in f32.
static float snap25(float v) { return std::round(v / 25.0f) * 25.0f; }

static int side(int n) { int s = 1; while (s * s < n) ++s; return s; }

struct Rays { std::vector<float> x, y; };

// N downward rays over the content, in the content's own frame; `shiftX` is how far the origin has moved since the build.
static void castBatch(const Rays& r, double shiftX, double& ms, int& hits) {
    hits = 0;
    const Clock::time_point t0 = Clock::now();
    for (size_t i = 0; i < r.x.size(); ++i) {
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 1};
        int32_t ent = 0;
        const float ox = static_cast<float>(static_cast<double>(r.x[i]) - shiftX);
        if (aver_phys_raycast(ox, r.y[i], 500.0f, 0.0f, 0.0f, -1.0f, 1000.0f, p, n, &ent)) ++hits;
    }
    ms = msSince(t0);
}

static Result runConfig(const Config& c, int repeat, int rayCount, int stackSteps, int threads) {
    Result r;
    r.label = c.label;

    aver_phys_shutdown();
    aver_phys_setting_reset(-1);
    aver_phys_config_reset(-1);
    aver_phys_config_set(AVER_PHYS_CF_MAX_BODIES, 262144.0f);
    aver_phys_config_set(AVER_PHYS_CF_MAX_BODY_PAIRS, 524288.0f);
    aver_phys_config_set(AVER_PHYS_CF_MAX_CONTACTS, 131072.0f);
    aver_phys_config_set(AVER_PHYS_CF_TEMP_MB, 128.0f);
    aver_phys_config_set(AVER_PHYS_CF_THREADS, static_cast<float>(threads));
    aver_phys_setting_set(AVER_PHYS_WS_ALLOW_SLEEPING, c.sleeping ? 1.0f : 0.0f);
    if (!aver_phys_init()) { AVER_WARN("{}: the world did not start", c.label); return r; }

    // ---- build ----
    const Clock::time_point tb = Clock::now();
    const int S = side(std::max(std::max(c.statics, c.dynamics), 1));
    const float extent = static_cast<float>(S) * kCell;
    const int S2 = side(std::max(c.stacks, 1));
    const float stackY0 = (c.statics > 0 ? extent + 1000.0f : 0.0f);
    const float floorHalfX = std::max(extent, static_cast<float>(S2) * 300.0f) * 0.5f + 2000.0f;
    const float floorHalfY = (stackY0 + static_cast<float>(S2) * 300.0f + extent) * 0.5f + 2000.0f;
    aver_phys_add_static_box(floorHalfX - 2000.0f, floorHalfY - 2000.0f, -50.0f, floorHalfX, floorHalfY, 50.0f);   // the floor, top at z = 0

    Rays rays;
    for (int k = 0; k < c.statics; ++k) {
        const float x = static_cast<float>(k / S) * kCell, y = static_cast<float>(k % S) * kCell;
        aver_phys_add_static_box(x, y, 25.0f, 100.0f, 100.0f, 25.0f);
    }
    for (int k = 0; k < c.dynamics; ++k) {
        const int cell = static_cast<int>((static_cast<long long>(k) * S * S) / std::max(c.dynamics, 1));
        const float x = static_cast<float>(cell / S) * kCell + 200.0f, y = static_cast<float>(cell % S) * kCell + 200.0f;
        aver_phys_add_dynamic_box(x, y, 60.0f, 20.0f, 20.0f, 20.0f, 10.0f);
    }
    for (int s = 0; s < c.stacks; ++s) {
        const float x = static_cast<float>(s / S2) * 300.0f, y = stackY0 + static_cast<float>(s % S2) * 300.0f;
        for (int b = 0; b < 5; ++b) aver_phys_add_dynamic_box(x, y, 25.5f + 51.0f * static_cast<float>(b), 25.0f, 25.0f, 25.0f, 20.0f);
    }
    r.buildMs = msSince(tb);
    r.bodies = aver_phys_body_count();
    {
        double st[AVER_PHYS_ST_COUNT] = {};
        aver_phys_stats(st, AVER_PHYS_ST_COUNT);
        r.statics = static_cast<int>(st[AVER_PHYS_ST_STATIC]);
    }

    const int rayExtentX = std::max(1, static_cast<int>(std::max(extent, static_cast<float>(S2) * 300.0f)));
    const int rayExtentY = std::max(1, static_cast<int>(std::max(extent, stackY0 + static_cast<float>(S2) * 300.0f)));
    for (int i = 0; i < rayCount; ++i) {
        rays.x.push_back(snap25(rnd() * static_cast<float>(rayExtentX)));
        rays.y.push_back(snap25(rnd() * static_cast<float>(rayExtentY)));
    }

    // ---- settle: dynamics land, stacks go to sleep (when allowed) ----
    const int settle = c.stacks > 0 ? stackSteps : 40;
    for (int i = 0; i < settle; ++i) aver_phys_step(kDt);
    const int dyn = dynamicCount();
    r.activeBefore = activeDynamic();
    r.asleepBefore = dyn - r.activeBefore;

    // ---- the broad phase, tight ----
    const Clock::time_point to = Clock::now();
    aver_phys_optimize_broadphase();
    r.optimizeMs = msSince(to);
    castBatch(rays, 0.0, r.rayTightMs, r.hitsTight);

    // ---- the shifts: +1 region on X, then back, R times in all ----
    std::vector<double> wall, abi;
    double total = 0.0;
    for (int i = 0; i < repeat; ++i) {
        const double dx = (i % 2 == 0) ? kStepCm : -kStepCm;
        const Clock::time_point t0 = Clock::now();
        const int32_t ok = aver_phys_origin_shift(dx, 0.0, 0.0);
        const double ms = msSince(t0);
        if (!ok) { AVER_WARN("{}: shift {} was refused", c.label, i); return r; }
        total += dx;
        float abiMs = 0.0f;
        aver_phys_origin_stats(&abiMs, nullptr, nullptr, nullptr, nullptr, nullptr);
        wall.push_back(ms);
        abi.push_back(static_cast<double>(abiMs));
        if (i == 0) {
            r.firstShiftMs = ms;
            castBatch(rays, total, r.rayLooseMs, r.hitsLoose);   // right after the first shift: the loose-tree penalty
        }
    }
    castBatch(rays, total, r.rayAfterMs, r.hitsAfter);
    const Clock::time_point t1 = Clock::now();
    aver_phys_optimize_broadphase();
    r.optimizeAfterMs = msSince(t1);
    castBatch(rays, total, r.rayReoptMs, r.hitsReopt);

    // A further second of stepping: a sleeper that the shift woke would show here.
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    r.asleepAfter = dynamicCount() - activeDynamic();

    std::sort(wall.begin(), wall.end());
    std::sort(abi.begin(), abi.end());
    r.minMs = wall.front();
    r.maxMs = wall.back();
    r.medianMs = wall[wall.size() / 2];
    r.abiMedianMs = abi[abi.size() / 2];
    r.ok = true;
    return r;
}

static void printHeader() {
    std::printf("\n%-34s %8s %8s | %9s %9s %9s %9s %9s | %8s %8s\n", "class / scene", "bodies", "statics", "1st ms", "min ms", "median ms", "max ms", "abi med", "opt ms", "opt-after");
}
static void printRow(const Result& r) {
    if (!r.ok) { std::printf("%-34s  (did not run)\n", r.label.c_str()); return; }
    std::printf("%-34s %8d %8d | %9.3f %9.3f %9.3f %9.3f %9.3f | %8.2f %8.2f\n", r.label.c_str(), r.bodies, r.statics, r.firstShiftMs, r.minMs,
                r.medianMs, r.maxMs, r.abiMedianMs, r.optimizeMs, r.optimizeAfterMs);
}
static void printRays(const Result& r) {
    if (!r.ok) return;
    std::printf("  %-32s rays: tight %7.2f ms | after 1st shift %7.2f ms (x%.2f) | after all %7.2f ms | re-optimised %7.2f ms | hits %d / %d / %d / %d%s\n",
                r.label.c_str(), r.rayTightMs, r.rayLooseMs, r.rayTightMs > 0.0 ? r.rayLooseMs / r.rayTightMs : 0.0, r.rayAfterMs, r.rayReoptMs,
                r.hitsTight, r.hitsLoose, r.hitsAfter, r.hitsReopt,
                (r.hitsTight == r.hitsLoose && r.hitsLoose == r.hitsAfter && r.hitsAfter == r.hitsReopt) ? "" : "   <-- HIT COUNTS DISAGREE");
}
static void printSleep(const Result& r) {
    if (!r.ok || r.asleepBefore + r.activeBefore == 0) return;
    std::printf("  %-32s asleep before the shift %d of %d, after the shifts and one more second %d%s\n", r.label.c_str(), r.asleepBefore,
                r.asleepBefore + r.activeBefore, r.asleepAfter, r.asleepAfter < r.asleepBefore ? "   <-- THE SHIFT WOKE BODIES" : "");
}

int main(int argc, char** argv) {
    std::string scene = "all";
    int repeat = 5, rays = 2000, threads = -1, stackSteps = 420;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int& out) { if (i + 1 < argc) out = std::atoi(argv[++i]); };
        if (a == "--scene" && i + 1 < argc) scene = argv[++i];
        else if (a == "--repeat") next(repeat);
        else if (a == "--rays") next(rays);
        else if (a == "--threads") next(threads);
        else if (a == "--stack-steps") next(stackSteps);
        else { std::printf("usage: PhysicsOriginBenchTool [--scene small|large|stacks|all] [--repeat R] [--rays N] [--threads N] [--stack-steps N]\n"); return 2; }
    }
    repeat = std::max(repeat, 2);
    rays = std::max(rays, 1);

    AVER_INFO("=== PhysicsOriginBenchTool: scene {}, {} shifts, {} rays, threads {} ===", scene, repeat, rays, threads);
    const bool doSmall = scene == "small" || scene == "all", doLarge = scene == "large" || scene == "all", doStacks = scene == "stacks" || scene == "all";
    double smallMedian = -1.0, largeMedian = -1.0;

    auto runScene = [&](const char* name, int statics, int dynamics, double* median) {
        std::printf("\n##### scene %s: %d statics + %d dynamics #####", name, statics, dynamics);
        printHeader();
        const std::string nm = name;
        Result rAll = runConfig(Config{"all", statics, dynamics, 0, false}, repeat, rays, stackSteps, threads);
        rAll.label = nm + " / all";
        Result rStat = runConfig(Config{"statics", statics, 0, 0, false}, repeat, rays, stackSteps, threads);
        rStat.label = nm + " / statics only";
        Result rDyn = runConfig(Config{"dynamics", 0, dynamics, 0, false}, repeat, rays, stackSteps, threads);
        rDyn.label = nm + " / dynamics only (awake)";
        printRow(rAll); printRow(rStat); printRow(rDyn);
        std::printf("\n");
        printRays(rAll); printRays(rStat); printRays(rDyn);
        if (median) *median = rAll.ok ? rAll.medianMs : -1.0;
    };

    if (doSmall) runScene("small", 10000, 1000, &smallMedian);
    if (doLarge) runScene("large", 100000, 2000, &largeMedian);
    if (doStacks) {
        std::printf("\n##### scene stacks: 500 stacks of 5 boxes, asleep #####");
        printHeader();
        Result r = runConfig(Config{"stacks", 0, 0, 500, true}, repeat, rays, stackSteps, threads);
        r.label = "stacks / 500 asleep";
        printRow(r);
        std::printf("\n");
        printRays(r);
        printSleep(r);
    }
    aver_phys_shutdown();

    std::printf("\n##### decision rule (LARGE_WORLD 2.3.7) #####\n");
    if (smallMedian >= 0.0)
        std::printf("small scene, median shift %.3f ms: %s\n", smallMedian, smallMedian <= 16.0 ? "within 16 ms, ships as designed" : "OVER 16 ms: tell the owner the numbers before any lever");
    if (largeMedian >= 0.0)
        std::printf("large scene, median shift %.3f ms: %s\n", largeMedian,
                    largeMedian <= 50.0 ? "within 50 ms, ships as designed"
                                        : "OVER 50 ms: levers in order (1 PHYSICS.STATICRADIUS, 2 late statics, 3 re-create streamed statics); tell the owner the numbers first");
    std::printf("Record the table in LARGE_WORLD.md section 9 (P-0).\n");
    return 0;
}
