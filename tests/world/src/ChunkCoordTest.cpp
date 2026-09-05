// Aver.World: the region -> chunk -> local-float coordinate hierarchy. Exit code = failure count.
//
// The header carries static_asserts for the identities about the CONSTANTS (10 bits is 1024, 6+4
// spends it exactly, 2047 is the boundary). Those fail the build. This file covers the BEHAVIOUR,
// and it leans hard on two things the tree has already been burned by:
//
//   NEGATIVE COORDINATES. C++ integer division truncates toward zero, so a naive world->chunk map
//   makes chunk 0 twice as wide as every other chunk and puts -1 cm in the same chunk as +1 cm.
//   PcgVolume.cpp:50-52 truncates where :78-80 floors and PcgShaders.hpp:64-66 admits they disagree
//   "the moment a volume is centred on the origin". Every case below at or below zero is there
//   because of that, not for symmetry.
//
//   EXACT BOUNDARIES. Off-by-one errors hide at gc = -513/-512 and +511/+512, where the region rolls
//   over, and at world positions that are exact multiples of the chunk size.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/world/ChunkCoord.hpp"

#include <cmath>
#include <string>

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

// A reference floor division, written the slow obvious way, to check the fast one against.
static i32 refFloorDiv(i32 a, i32 b) {
    return static_cast<i32>(std::floor(static_cast<f64>(a) / static_cast<f64>(b)));
}
static i32 refFloorMod(i32 a, i32 b) { return a - refFloorDiv(a, b) * b; }

int main() {
    const i32 S = kDefaultChunkSizeCm;   // 1600

    // ---- floorDiv / floorMod against an independent reference ------------------------------------
    {
        int bad = 0;
        for (i32 a = -5000; a <= 5000; ++a) {
            for (const i32 b : {1, 2, 7, 16, 1024, 1600}) {
                if (floorDiv(a, b) != refFloorDiv(a, b)) ++bad;
                if (floorMod(a, b) != refFloorMod(a, b)) ++bad;
                // The defining identity, which is the thing that actually matters.
                if (floorDiv(a, b) * b + floorMod(a, b) != a) ++bad;
                if (floorMod(a, b) < 0 || floorMod(a, b) >= b) ++bad;
            }
        }
        check(bad == 0, "floorDiv/floorMod agree with a floating-point reference over -5000..5000 x 6 divisors");
    }

    // ---- region <-> local round trip, exhaustive over four regions -------------------------------
    // Four whole regions, so the sweep crosses three boundaries and spends real time on both sides
    // of zero rather than sampling near it.
    {
        int bad = 0, badRange = 0;
        for (i32 gc = -2 * kChunksPerRegionAxis; gc < 2 * kChunksPerRegionAxis; ++gc) {
            const i32 r = regionOfAxis(gc);
            const i16 l = localOfAxis(gc);
            if (l < kChunkLocalMin || l > kChunkLocalMax) ++badRange;
            if (globalChunkOfAxis(r, l) != gc) ++bad;
        }
        check(bad == 0, "global chunk -> (region, local) -> global chunk is the identity over 4 regions");
        check(badRange == 0, "the local index never leaves [-512, +511]");
    }

    // ---- the boundary cases, named individually --------------------------------------------------
    check(regionOfAxis(0) == 0 && localOfAxis(0) == 0, "chunk 0 is region 0, local 0");
    check(regionOfAxis(511) == 0 && localOfAxis(511) == 511, "chunk 511 is the last of region 0");
    check(regionOfAxis(512) == 1 && localOfAxis(512) == -512, "chunk 512 is the first of region 1");
    check(regionOfAxis(-512) == 0 && localOfAxis(-512) == -512, "chunk -512 is the FIRST of region 0");
    check(regionOfAxis(-513) == -1 && localOfAxis(-513) == 511, "chunk -513 is the last of region -1");
    check(regionOfAxis(-1) == 0 && localOfAxis(-1) == -1, "chunk -1 is region 0 -- the origin is not a seam");
    check(regionOfAxis(1024 + 511) == 1 && regionOfAxis(1024 + 512) == 2,
          "region 1 ends where region 2 begins");

    // Region 0 really is CENTRED on the origin, which is the reason for the -512..+511 range at all:
    // an existing flat level sitting at (0,0,0) must be in the middle of its region, not on a corner
    // where four region files meet.
    check(regionOfAxis(kChunkLocalMin) == 0 && regionOfAxis(kChunkLocalMax) == 0,
          "the whole local range maps to region 0");
    check(regionOfAxis(kChunkLocalMin - 1) == -1 && regionOfAxis(kChunkLocalMax + 1) == 1,
          "and one step past either end leaves it");

    // ---- packing, exhaustive on one axis and spot-checked in three -------------------------------
    {
        int bad = 0;
        for (i32 v = kChunkLocalMin; v <= kChunkLocalMax; ++v) {
            const ChunkLocal l{static_cast<i16>(v), static_cast<i16>(-v > kChunkLocalMax ? kChunkLocalMax : -v),
                               static_cast<i16>(kChunkLocalMin + ((v + 512) % 1024))};
            const u32 p = packLocal(l);
            if (p >= (1u << 30)) ++bad;                       // must fit in 30 bits
            if (unpackLocal(p) != l) ++bad;                   // must round-trip
            if (localOfGroupSlot(groupIndexOf(l), slotIndexOf(l)) != l) ++bad;  // group/slot too
            if (groupIndexOf(l) >= kGroupsPerRegion) ++bad;
            if (slotIndexOf(l) >= kChunksPerGroup) ++bad;
        }
        check(bad == 0, "pack/unpack and group/slot round-trip over all 1024 values on the x axis");
    }
    {
        // The corners, where a sign-extension mistake in the i16 <-> u32 conversion would show.
        int bad = 0;
        for (const i16 a : {static_cast<i16>(kChunkLocalMin), static_cast<i16>(-1),
                            static_cast<i16>(0), static_cast<i16>(kChunkLocalMax)}) {
            for (const i16 b : {static_cast<i16>(kChunkLocalMin), static_cast<i16>(-1),
                                static_cast<i16>(0), static_cast<i16>(kChunkLocalMax)}) {
                for (const i16 c : {static_cast<i16>(kChunkLocalMin), static_cast<i16>(-1),
                                    static_cast<i16>(0), static_cast<i16>(kChunkLocalMax)}) {
                    const ChunkLocal l{a, b, c};
                    if (unpackLocal(packLocal(l)) != l) ++bad;
                    if (localOfGroupSlot(groupIndexOf(l), slotIndexOf(l)) != l) ++bad;
                }
            }
        }
        check(bad == 0, "all 64 sign corners of the 3-axis pack round-trip");
    }
    // The packed index is DISTINCT per chunk -- the property the region directory is keyed on.
    check(packLocal(ChunkLocal{0, 0, 0}) != packLocal(ChunkLocal{-512, 0, 0}), "pack distinguishes x");
    check(packLocal(ChunkLocal{0, 0, 0}) != packLocal(ChunkLocal{0, -512, 0}), "pack distinguishes y");
    check(packLocal(ChunkLocal{0, 0, 0}) != packLocal(ChunkLocal{0, 0, -512}), "pack distinguishes z");
    check(groupIndexOf(ChunkLocal{kChunkLocalMin, kChunkLocalMin, kChunkLocalMin}) == 0,
          "the first chunk of a region is in group 0");
    check(groupIndexOf(ChunkLocal{kChunkLocalMax, kChunkLocalMax, kChunkLocalMax}) == kGroupsPerRegion - 1,
          "the last chunk of a region is in the last group");
    check(slotIndexOf(ChunkLocal{kChunkLocalMin, kChunkLocalMin, kChunkLocalMin}) == 0,
          "...and in slot 0 of it");
    check(slotIndexOf(ChunkLocal{kChunkLocalMax, kChunkLocalMax, kChunkLocalMax}) == kChunksPerGroup - 1,
          "...and the last chunk in the last slot");
    // 16 consecutive chunks share a group and use every slot: the property that makes one group's
    // dense 4096-entry table worth reading in one go.
    {
        int distinctGroups = 0;
        u32 seenSlots = 0, prevGroup = 0xFFFFFFFFu;
        for (i32 v = 0; v < kChunksPerGroupAxis; ++v) {
            const ChunkLocal l{static_cast<i16>(v), 0, 0};
            if (groupIndexOf(l) != prevGroup) { ++distinctGroups; prevGroup = groupIndexOf(l); }
            seenSlots |= 1u << slotIndexOf(l);
        }
        check(distinctGroups == 1, "16 consecutive chunks on one axis share a single group");
        check(seenSlots == 0xFFFFu, "...and cover all 16 of its slots on that axis");
    }

    // ---- splitting a world position --------------------------------------------------------------
    {
        int bad = 0, badRange = 0;
        // Every 100 cm across four regions' worth of world, plus the exact boundaries between them.
        for (i64 cm = -2 * 1024LL * 1600; cm < 2 * 1024LL * 1600; cm += 100) {
            const SplitPos p = splitCm(static_cast<f64>(cm), 0.0, 0.0, S);
            if (p.local.x < 0.0f || p.local.x >= static_cast<f32>(S)) ++badRange;
            if (chunkOriginCmAxis(p.chunk.x, S) + static_cast<i64>(p.local.x) != cm) ++bad;
        }
        check(badRange == 0, "the local offset is always in [0, chunkSize) -- never negative");
        check(bad == 0, "chunk origin + local offset reconstructs the exact centimetre, across 4 regions");
    }
    // The exact boundaries, which is where a floor/truncate mistake actually bites.
    {
        const SplitPos a = splitCm(0.0, 0.0, 0.0, S);
        const SplitPos b = splitCm(-1.0, 0.0, 0.0, S);
        const SplitPos c = splitCm(static_cast<f64>(S), 0.0, 0.0, S);
        const SplitPos d = splitCm(-static_cast<f64>(S), 0.0, 0.0, S);
        check(a.chunk.x == 0 && a.local.x == 0.0f, "0 cm is chunk 0 at offset 0");
        check(b.chunk.x == -1 && b.local.x == static_cast<f32>(S - 1),
              "-1 cm is chunk -1 at offset 1599, NOT chunk 0 -- this is the truncation bug");
        check(c.chunk.x == 1 && c.local.x == 0.0f, "exactly one chunk out is chunk 1 at offset 0");
        check(d.chunk.x == -1 && d.local.x == 0.0f, "exactly one chunk back is chunk -1 at offset 0");
    }

    // ---- the point of the whole exercise ---------------------------------------------------------
    //
    // A position 100 km out: absolute f32 quantises it to a 1 cm grid, but the split keeps it exact,
    // because the big part became an integer and the float part never exceeds one chunk.
    {
        const f64 farCm = 10'000'000.0 + 0.5;   // 100 km + 5 mm
        const SplitPos p = splitCm(farCm, 0.0, 0.0, S);
        const f32 asAbsolute = static_cast<f32>(farCm);
        check(f32GridAtCm(asAbsolute) >= 1.0f,
              "absolute f32 at 100 km has a grid of a centimetre or worse");
        check(f32GridAtCm(p.local.x) < 0.001f,
              "...while the local offset resolves to better than 10 microns");
        check(static_cast<f64>(chunkOriginCmAxis(p.chunk.x, S)) + static_cast<f64>(p.local.x) == farCm,
              "and the split reconstructs the 5 mm the absolute f32 cannot hold");
        // The absolute rejoin really does lose it, and by a MEASURED amount -- stated as a positive
        // claim so the limitation is recorded rather than implied by a disjunction that cannot fail.
        const f64 lost = std::fabs(static_cast<f64>(toWorldCm(p, S).x) - farCm);
        check(lost >= 0.25,
              "rejoining to an absolute f32 loses at least 2.5 mm out there -- which is why slice 10 exists");
        AVER_INFO("   note  absolute f32 at 100 km loses {:.3f} cm of a position the split keeps exactly", lost);
    }

    // Region-relative reconstruction: the number slice 10's floating origin will actually use.
    {
        int bad = 0;
        f32 worst = 0.0f;
        // Walk 100 km out in 16 m steps and check the region-relative coordinate stays small.
        for (i64 chunk = 0; chunk <= 6250; ++chunk) {
            const f64 cm = static_cast<f64>(chunk) * S + 800.0;   // mid-chunk
            const SplitPos p = splitCm(cm, 0.0, 0.0, S);
            const RegionCoord r = regionOf(p.chunk);
            const Vec3 rel = toRegionRelativeCm(p, r, S);
            if (std::fabs(rel.x) > 512.0f * S) ++bad;
            worst = std::fmax(worst, f32GridAtCm(rel.x));
        }
        check(bad == 0, "a region-relative coordinate never exceeds half a region, 100 km out");
        check(worst <= 0.1f,
              "...so f32 holds it to better than a millimetre anywhere -- the whole point of the hierarchy");
        AVER_INFO("   note  worst f32 grid on a region-relative coordinate over 100 km: {:.4f} mm",
                  worst * 10.0f);
    }

    // ---- the precision budget --------------------------------------------------------------------
    check(chunkSizeValid(kDefaultChunkSizeCm), "16 m is a valid chunk size");
    check(chunkSizeValid(1024) && chunkSizeValid(2047), "so are 10.24 m and the 2047 cm boundary");
    check(!chunkSizeValid(2048), "2048 cm is NOT -- 512 * 2048 lands exactly on the 2^20 cliff");
    check(!chunkSizeValid(0) && !chunkSizeValid(-1600), "zero and negative sizes are rejected");
    check(withinSubMillimetre(Vec3{0, 0, 0}), "the origin is inside the budget");
    check(withinSubMillimetre(Vec3{512.0f * kDefaultChunkSizeCm, 0, 0}),
          "so is the far corner of region 0");
    check(!withinSubMillimetre(Vec3{static_cast<f32>(kSubMillimetreCm), 0, 0}),
          "2^20 cm is NOT -- the guard fires exactly at the cliff");
    check(!withinSubMillimetre(Vec3{0, 0, -2.0f * kSubMillimetreCm}),
          "and it fires on any axis, in either direction");

    // The grid values the plan quotes, checked rather than trusted.
    check(std::fabs(f32GridAtCm(819200.0f) - 0.0625f) < 1e-9f,
          "f32's grid at 8.192 km is 0.0625 cm = 0.625 mm, as docs/CHUNKS.md 4.3 claims");
    check(std::fabs(f32GridAtCm(1600.0f) - 0.0001220703125f) < 1e-12f,
          "and inside one 16 m chunk it is ~1.2 microns");

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
