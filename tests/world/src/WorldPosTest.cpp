// Aver.World: WorldPos and the floating-origin arithmetic (LARGE_WORLD 2.1, 2.2.1-2.2.2). Exit code = failure count.
//
// The properties are checked against an independent i64 / f64 reference, never against the code's own
// formulas, and the flight is walked one centimetre at a time so the trigger distances are measured,
// not assumed.
#include "aver/core/CharConv.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/PlacementWriteBack.hpp"
#include "aver/world/WorldOrigin.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool samePos(const WorldPos& a, const WorldPos& b) {
    return a.chunk == b.chunk && a.local.x == b.local.x && a.local.y == b.local.y && a.local.z == b.local.z;
}

static std::mt19937_64 g_rng(0xA7E2'0011ull);
static f64 uni(f64 lo, f64 hi) { return std::uniform_real_distribution<f64>(lo, hi)(g_rng); }

static int g_warns = 0;
static void countWarns(void*, LogLevel level, std::string_view) { if (level == LogLevel::Warn) ++g_warns; }

int main() {
    const i64 STEP = kOriginStepCm;

    // ---- constants ------------------------------------------------------------------------------------
    check(STEP == 1'638'400 && kRegionHalfCm == 819'200 && kSubMmCliffCm == 1'048'576, "origin constants");
    check(kRebaseMarginMaxCm == 229'376 && kRebaseMarginDefaultCm == 102'400, "margin constants");

    // ---- (1) worldPosOfAbs(absOf(p)) == p, regions -3..+3 and at +-10^8 cm -----------------------------
    {
        const f32 locals[] = {0.0f, 0.25f, 1.0f, 799.5f, 1599.75f, 1599.9375f, 0.0625f, 1024.0f};
        int bad = 0, n = 0;
        for (i32 rx = -3; rx <= 3; ++rx) {
            for (const i32 lx : {-512, -1, 0, 1, 511}) {
                const i32 gx = globalChunkOfAxis(rx, lx);
                for (const f32 a : locals) {
                    for (const f32 b : {0.0f, 1599.75f, 333.5f}) {
                        WorldPos p;
                        p.chunk = ChunkCoord{gx, -gx / 2, lx};
                        p.local = Vec3{a, b, locals[(n + 3) % 8]};
                        const Vec3d d = absOf(p);
                        ++n;
                        if (!samePos(worldPosOfAbs(d.x, d.y, d.z), p)) ++bad;
                    }
                }
            }
        }
        check(bad == 0, "worldPosOfAbs(absOf(p)) == p over regions -3..3 (" + std::to_string(n) + " cases)");
        for (const f64 v : {1e8, -1e8, 99999999.5, -99999999.5}) {
            const WorldPos p = worldPosOfAbs(v, -v, v / 2);
            const Vec3d d = absOf(p);
            check(d.x == v && d.y == -v && d.z == v / 2, "absOf round-trips " + std::to_string(v));
            check(p.local.x >= 0.0f && p.local.x < 1600.0f && p.local.y >= 0.0f && p.local.y < 1600.0f, "local on [0,1600) at 1e8");
        }
        // negatives floor: -1 cm is chunk -1, local 1599; exact multiples have local 0
        const WorldPos m1 = worldPosOfAbs(-1.0, 0.0, 1600.0);
        check(m1.chunk.x == -1 && m1.local.x == 1599.0f && m1.chunk.z == 1 && m1.local.z == 0.0f, "floor, not truncate");
        // the canonical-local repairs: just below a chunk boundary must not produce local == 1600
        const WorldPos edge = worldPosOfAbs(1599.9999999999, -1e-20, 0.0);
        check(edge.local.x < 1600.0f && edge.local.y >= 0.0f && edge.local.y < 1600.0f, "local never reaches the chunk size");
        const WorldPos viaSplit = worldPosOfAbs(12345.678, -98765.4321, 321.5);
        const SplitPos ref = splitCm(12345.678, -98765.4321, 321.5, kWorldChunkCm);
        check(samePos(viaSplit, ref), "agrees with splitCm away from boundaries");
        check(regionOf(worldPosOfAbs(0, 0, 0)) == RegionCoord{0, 0, 0}, "region of the origin");
        check(regionOf(worldPosOfAbs(819199.0, -819201.0, 0)) == RegionCoord{0, -1, 0}, "region seam at +-819,200");
    }

    // ---- (2) localOf against an i64 reference ----------------------------------------------------------
    {
        int badInt = 0, badSum = 0, n = 0;
        f64 worst = 0;
        for (int i = 0; i < 20000; ++i) {
            OriginCm o{static_cast<i64>(std::llround(uni(-8, 8))) * STEP, static_cast<i64>(std::llround(uni(-8, 8))) * STEP};
            // an absolute position within 1,048,575 cm of O on X and Y
            const f64 ax = static_cast<f64>(o.x) + uni(-1'048'575.0, 1'048'575.0);
            const f64 ay = static_cast<f64>(o.y) + uni(-1'048'575.0, 1'048'575.0);
            const WorldPos p = worldPosOfAbs(ax, ay, uni(-5e4, 5e4));
            const Vec3 l = localOf(p, o);
            const Vec3d a = absOf(p);
            const f64 rx = a.x - static_cast<f64>(o.x), ry = a.y - static_cast<f64>(o.y);
            const f64 ex = std::fabs(static_cast<f64>(l.x) - rx), ey = std::fabs(static_cast<f64>(l.y) - ry);
            worst = std::max(worst, std::max(ex, ey));
            if (ex > 0.0625 + 1e-9 || ey > 0.0625 + 1e-9) ++badSum;
            // the integer part exact: with local zeroed, localOf is exactly the i64 difference
            WorldPos z = p; z.local = Vec3{0, 0, 0};
            const Vec3 lz = localOf(z, o);
            if (static_cast<i64>(lz.x) != chunkOriginCmAxis(p.chunk.x, kWorldChunkCm) - o.x ||
                static_cast<i64>(lz.y) != chunkOriginCmAxis(p.chunk.y, kWorldChunkCm) - o.y) ++badInt;
            ++n;
        }
        check(badInt == 0, "localOf integer part exact (i64 subtraction first)");
        check(badSum == 0, "localOf within 0.625 mm of the f64 reference (worst " + std::to_string(worst) + " cm)");
        // the worked example: local (1,000,000,0,0) at O = 3 steps is the exact position 5,915,200
        const OriginCm o3{3 * STEP, 0};
        const WorldPos w = worldPosOfLocal(Vec3{1'000'000.0f, 0, 0}, o3);
        check(absOf(w).x == 5'915'200.0, "worldPosOfLocal adds in f64 (5,915,200)");
        check(localOf(w, o3).x == 1'000'000.0f, "and localOf inverts it");
    }

    // ---- distanceCm / offsetBy ---------------------------------------------------------------------------
    {
        const WorldPos a = worldPosOfAbs(1e8 + 0.5, 0, 0), b = worldPosOfAbs(1e8 - 3.5, 3.0, 0);
        check(std::fabs(distanceCm(a, b) - 5.0) < 1e-6, "distance at 1e8 cm keeps sub-micron precision");
        const WorldPos c = offsetBy(a, Vec3d{-4.0, 3.0, 0.0});
        check(samePos(c, b), "offsetBy lands on the expected WorldPos");
        const WorldPos far = offsetBy(worldPosOfAbs(0, 0, 0), Vec3d{1e9, -1e9, 12.5});
        check(absOf(far).x == 1e9 && absOf(far).y == -1e9 && absOf(far).z == 12.5, "offsetBy across 625,000 chunks is exact");
        check(far.local.x >= 0.0f && far.local.x < 1600.0f, "offsetBy keeps local canonical");
    }

    // ---- (3) the decide table ------------------------------------------------------------------------------
    {
        WorldOrigin w;
        auto d = [&](f32 x, f32 y, f32 z = 0) { return w.decide(Vec3{x, y, z}); };
        check(d(0, 0).x == 0 && d(0, 0).y == 0, "decide: origin");
        check(d(921'600, -921'600).x == 0 && d(921'600, -921'600).y == 0, "decide: 921,600 does not shift (limit is exclusive)");
        check(d(921'601, 0).x == STEP && d(921'601, 0).y == 0, "decide: 921,601 shifts one step");
        check(d(-921'601, 0).x == -STEP, "decide: negative symmetric");
        check(d(0, 921'601).y == STEP && d(0, -921'601).y == -STEP, "decide: Y axis");
        check(d(5'000'000, 0).x == 3 * STEP, "decide: a 5,000,000 teleport is three steps");
        check(d(-5'000'000, 0).x == -3 * STEP, "decide: -5,000,000 is -3 steps");
        check(d(2'000'000, -100).x == STEP && d(2'000'000, -100).y == 0, "decide: axes independent");
        check(d(100, 100, 5e6f).x == 0 && d(100, 100, 5e6f).y == 0, "decide: Z never shifts");
        check(d(2'457'600, 0).x == 2 * STEP, "decide: 2,457,600 (1.5 steps) rounds to the nearest region centre");
        const f32 inf = std::numeric_limits<f32>::infinity();
        check(d(inf, std::nanf("")).x == 0, "decide: non-finite camera never shifts");
        // margin changes the trigger
        WorldOrigin::Settings s; s.marginCm = 0;
        w.configure(s);
        check(d(819'201, 0).x == STEP && d(819'200, 0).x == 0, "decide: margin 0 triggers at the region half-width");
        s.enabled = false; w.configure(s);
        check(d(9e6f, 0).x == 0, "decide: disabled never shifts");
    }

    // ---- (4) dead band --------------------------------------------------------------------------------------
    {
        WorldOrigin w;
        // forward trigger at 921,601
        check(w.update(Vec3{921'601, 0, 0}), "dead band: forward trigger shifts");
        check(w.origin().x == STEP && w.origin().y == 0, "dead band: origin is one step");
        const f32 cam = 921'601.0f - static_cast<f32>(STEP);
        check(cam == -716'799.0f, "dead band: camera sits at -716,799 after the shift");
        check(!w.update(Vec3{cam, 0, 0}), "dead band: -716,799 does not flip back");
        check(!w.update(Vec3{-921'600.0f, 0, 0}), "dead band: -921,600 does not flip back");
        check(w.update(Vec3{-921'601.0f, 0, 0}), "dead band: -921,601 shifts back once");
        check(w.origin().x == 0, "dead band: back at zero");

        // a camera jittering +-1 km about a seam shifts at most once
        WorldOrigin j;
        const f64 seam = 921'600.0;
        int shifts = 0;
        for (int i = 0; i < 4000; ++i) {
            const f64 abs = seam + 100'000.0 * std::sin(i * 0.37) + ((i & 1) ? 100.0 : -100.0);
            if (j.update(Vec3{static_cast<f32>(abs - static_cast<f64>(j.origin().x)), 0, 0})) ++shifts;
        }
        check(shifts <= 1, "dead band: jitter about a seam rebases at most once (" + std::to_string(shifts) + ")");
    }

    // ---- (5) the 100 km flight: six shifts at the stated distances ----------------------------------------------
    {
        WorldOrigin w;
        std::vector<f64> at;
        for (i64 abs = 0; abs <= 10'000'000; ++abs) {                     // 1 cm per step
            const Vec3 cam{static_cast<f32>(abs - w.origin().x), 0.0f, 0.0f};
            if (w.update(cam)) at.push_back(static_cast<f64>(abs));
        }
        const f64 expect[6] = {921'600, 2'560'000, 4'198'400, 5'836'800, 7'475'200, 9'113'600};   // 9.216, 25.6, 41.984, 58.368, 74.752, 91.136 km
        check(at.size() == 6, "flight: six shifts over 100 km (got " + std::to_string(at.size()) + ")");
        for (usize i = 0; i < at.size() && i < 6; ++i)
            check(std::fabs(at[i] - expect[i]) <= 1.0 + 1e-9, "flight: shift " + std::to_string(i) + " at " + std::to_string(at[i]));
        check(w.origin().x == 6 * STEP && w.generation() == 6, "flight: origin is six steps, generation six");
    }

    // ---- (6) Sterbenz properties of the step ---------------------------------------------------------------------
    {
        int badExact = 0, badSmall = 0, badM = 0, badMSmall = 0;
        for (int i = 0; i < 1'000'000; ++i) {
            // exact: p in [819,200, 3,276,800] cm, either sign
            const f32 p = static_cast<f32>(uni(819'200.0, 3'276'800.0));
            const f32 sp = (i & 1) ? p : -p;
            const f32 dl = (i & 1) ? static_cast<f32>(STEP) : -static_cast<f32>(STEP);
            if (static_cast<f64>(sp - dl) != static_cast<f64>(sp) - static_cast<f64>(dl)) ++badExact;
            // metres: p in [8,192, 32,768], delta 16,384
            const f32 pm = static_cast<f32>(uni(8'192.0, 32'768.0));
            if (static_cast<f64>(pm - 16'384.0f) != static_cast<f64>(pm) - 16'384.0) ++badM;
            // smaller |p|: at most half an ULP of the result, 0.625 mm
            const f32 q = static_cast<f32>(uni(0.0, 819'200.0));
            const f64 errC = std::fabs(static_cast<f64>(q - static_cast<f32>(STEP)) - (static_cast<f64>(q) - static_cast<f64>(STEP)));
            if (errC > 0.0625 + 1e-9) ++badSmall;
            const f32 qm = static_cast<f32>(uni(0.0, 8'192.0));
            const f64 errM = std::fabs(static_cast<f64>(qm - 16'384.0f) - (static_cast<f64>(qm) - 16'384.0));
            if (errM > 0.000625 + 1e-9) ++badMSmall;
        }
        check(badExact == 0, "Sterbenz: p - 1,638,400 exact for |p| >= 819,200 (cm)");
        check(badM == 0, "Sterbenz: p - 16,384 exact for |p| >= 8,192 (m)");
        check(badSmall == 0, "smaller |p|: error <= 0.625 mm (cm)");
        check(badMSmall == 0, "smaller |p|: error <= 0.625 mm (m)");
    }

    // ---- (7) absolute preserved across (l - d, O + d) ---------------------------------------------------------------
    {
        int bad = 0;
        for (int i = 0; i < 20000; ++i) {
            const i64 k = static_cast<i64>(std::llround(uni(-6, 6)));
            const OriginCm o{k * STEP, -k * STEP};
            const bool pos = (i & 1) != 0;
            const f32 lx = static_cast<f32>(uni(819'200.0, 1'100'000.0)) * (pos ? 1.0f : -1.0f);
            const f32 ly = static_cast<f32>(uni(819'200.0, 1'100'000.0)) * (pos ? -1.0f : 1.0f);
            const Vec3 l{lx, ly, static_cast<f32>(uni(-1e4, 1e4))};
            const f32 dx = pos ? static_cast<f32>(STEP) : -static_cast<f32>(STEP);
            const f32 dy = -dx;
            const OriginCm o2{o.x + static_cast<i64>(dx), o.y + static_cast<i64>(dy)};
            const Vec3 l2{l.x - dx, l.y - dy, l.z};
            const Vec3d a = absOf(worldPosOfLocal(l, o)), b = absOf(worldPosOfLocal(l2, o2));
            if (a.x != b.x || a.y != b.y || a.z != b.z) ++bad;
        }
        check(bad == 0, "a root's absolute position is preserved across (l - d, O + d)");
    }

    // ---- (8) PlacementWriteBack -----------------------------------------------------------------------------------------
    {
        const Vec3d rec{123456789.123, -987654321.987, 42.123456789};
        const OriginCm o{75 * STEP, -603 * STEP};
        const Vec3d recNear{static_cast<f64>(o.x) + 1234567.891, static_cast<f64>(o.y) - 765432.109, 17.5};
        PlacementBaseline b = makeBaseline(recNear, o);
        const Vec3 cur = b.localAtLoad;
        check(untouched(b, cur), "untouched entity is untouched");
        const Vec3d w0 = writeBackCm(b, cur, o);
        check(w0.x == recNear.x && w0.y == recNear.y && w0.z == recNear.z, "an unchanged entity writes back the original f64 bit for bit");
        (void)rec;

        // after a shift: both entity and baseline subtract the same deltaF in f32
        const Vec3 dF{static_cast<f32>(STEP), static_cast<f32>(-STEP), 0.0f};
        Vec3 shifted{cur.x - dF.x, cur.y - dF.y, cur.z - dF.z};
        rebaseBaseline(b, dF);
        const OriginCm o2{o.x + STEP, o.y - STEP};
        check(untouched(b, shifted), "untouched survives a shift");
        const Vec3d w1 = writeBackCm(b, shifted, o2);
        check(w1.x == recNear.x && w1.y == recNear.y && w1.z == recNear.z, "and still writes the original f64 after a shift");

        // a moved axis is quantised to 1e-4 cm; the others stay original
        Vec3 moved = shifted; moved.x += 3.1234567f;
        const Vec3d w2 = writeBackCm(b, moved, o2);
        check(w2.y == recNear.y && w2.z == recNear.z, "an unmoved axis keeps its original f64");
        const f64 raw = static_cast<f64>(o2.x) + static_cast<f64>(moved.x);
        check(w2.x != recNear.x && std::fabs(w2.x - raw) <= 0.5e-4 + 1e-6, "a moved axis is f64(O) + local within the 1e-4 quantum");
        check(quantiseCm(w2.x) == w2.x, "quantisation is idempotent");
        // shortest-round-trip through a text writer
        char buf[64];
        const auto r = std::to_chars(buf, buf + sizeof buf, w2.x);
        f64 back = 0;
        aver::fromChars(buf, r.ptr, back);
        check(back == w2.x, "the quantised value round-trips through shortest text");
        check(static_cast<usize>(r.ptr - buf) <= 18, "and is short");
    }

    // ---- (9) margin clamp and the I3 warning, once ---------------------------------------------------------------------------
    {
        setLogSink(&countWarns, nullptr);
        g_warns = 0;
        WorldOrigin w;
        WorldOrigin::Settings s; s.marginCm = 1'000'000;
        w.configure(s);
        check(w.settings().marginCm == kRebaseMarginMaxCm, "margin clamps to 229,376");
        s.marginCm = -5; w.configure(s);
        check(w.settings().marginCm == 0, "negative margin clamps to 0");
        check(g_warns == 1, "the margin clamp warns once (" + std::to_string(g_warns) + ")");
        s.marginCm = kRebaseMarginDefaultCm; w.configure(s);
        g_warns = 0;
        check(w.checkStreamRadius(64'000), "a 64,000 cm streaming radius keeps sub-mm (102,400 + 64,000 <= 229,376)");
        check(g_warns == 0, "and does not warn");
        check(!w.checkStreamRadius(200'000), "a 200,000 cm radius breaks I3");
        check(!w.checkStreamRadius(300'000), "and still does");
        check(g_warns == 1, "the I3 violation warns once (" + std::to_string(g_warns) + ")");
        setLogSink(nullptr, nullptr);
    }

    AVER_INFO("WorldPosTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
