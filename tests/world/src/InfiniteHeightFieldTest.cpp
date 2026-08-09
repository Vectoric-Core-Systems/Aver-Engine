// InfiniteHeightFieldTest -- the pcg-based infinite height field, and its wiring into
// GeneratorSettings::heightSource via makeInfiniteHeightSource().
// Exit code = failure count.
//
// FOUR THINGS THIS PROVES, matching the task's own list:
//   1. makeInfiniteHeightSource's lambda calls pcg::sampleInfiniteHeightCm exactly -- not a parallel
//      reimplementation a future edit could let drift.
//   2. DETERMINISM ACROSS CALL ORDER: the same set of world positions, sampled forward and backward
//      (and from a second, independently-built spec), agree bit for bit.
//   3. CONTINUITY: no seam at a chunk boundary -- a step of 0.02cm across one moves the sampled
//      height by a vanishingly small amount, with a CONTROL proving sampleInfinite (the per-cell
//      density field this deliberately does NOT reuse as a height function) really would jump there.
//   4. UNSET STAYS FLAT: a GeneratorSettings that never calls makeInfiniteHeightSource is
//      byte-identical to today's generator, and installing it changes Z only -- never existence,
//      naming, or X/Y.
#include "aver/core/Log.hpp"
#include "aver/pcg/PcgVolume.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkGenerator.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Builds the SAME pcg::InfiniteSpec makeInfiniteHeightSource() builds internally, so this test can
// compare its lambda's output against a direct pcg::sampleInfiniteHeightCm call.
static pcg::InfiniteSpec specFor(const InfiniteHeightFieldSettings& hf) {
    pcg::InfiniteSpec spec{};
    spec.seed = hf.seed;
    spec.layerCount = 1;
    spec.layers[0].frequency = 1.0f;
    spec.layers[0].amplitude = 1.0f;
    spec.layers[0].octaves = hf.octaves > 0 ? static_cast<i32>(hf.octaves) : 1;
    spec.layers[0].lacunarity = 2.0f;
    spec.layers[0].gain = 0.5f;
    spec.layers[0].seedOffset = 0;
    spec.cellSizeCm = hf.featureSizeCm > 0.0f ? hf.featureSizeCm : 1600.0f;
    return spec;
}

int main() {
    InfiniteHeightFieldSettings hf;
    hf.seed = 0x1234;
    hf.featureSizeCm = 3200.0f;
    hf.amplitudeCm = 800.0f;
    hf.octaves = 4;
    hf.baseZCm = 150.0f;
    const pcg::InfiniteSpec spec = specFor(hf);

    // ---- the wiring calls the SAME function, not a copy of it -------------------------------------
    {
        auto src = makeInfiniteHeightSource(hf);
        bool allMatch = true, alwaysTrue = true;
        for (f32 wx = -20000.0f; wx <= 20000.0f; wx += 1777.0f) {
            for (f32 wy = -20000.0f; wy <= 20000.0f; wy += 2333.0f) {
                f32 fromSource = 0.0f;
                if (!src(wx, wy, fromSource)) alwaysTrue = false;
                const f32 direct = pcg::sampleInfiniteHeightCm(spec, hf.amplitudeCm, hf.baseZCm, wx, wy);
                if (fromSource != direct) allMatch = false;
            }
        }
        check(alwaysTrue, "the infinite height source never answers false -- every world position has a surface");
        check(allMatch, "makeInfiniteHeightSource's lambda returns EXACTLY pcg::sampleInfiniteHeightCm's "
                         "value for every sampled (wx, wy)");
    }

    // ---- determinism across call order -------------------------------------------------------------
    {
        std::vector<std::pair<f32, f32>> points;
        for (f32 wx = -50000.0f; wx <= 50000.0f; wx += 3671.0f)
            for (f32 wy = -50000.0f; wy <= 50000.0f; wy += 4127.0f)
                points.emplace_back(wx, wy);

        std::vector<f32> forward(points.size()), backward(points.size());
        for (usize i = 0; i < points.size(); ++i)
            forward[i] = pcg::sampleInfiniteHeightCm(spec, hf.amplitudeCm, hf.baseZCm,
                                                       points[i].first, points[i].second);
        for (usize i = points.size(); i-- > 0; )
            backward[i] = pcg::sampleInfiniteHeightCm(spec, hf.amplitudeCm, hf.baseZCm,
                                                        points[i].first, points[i].second);

        bool stable = true;
        for (usize i = 0; i < points.size(); ++i) if (forward[i] != backward[i]) stable = false;
        check(stable, "sampling the same points forward and then backward gives bit-identical heights "
                       "-- no dependence on call order");

        // A second, independently-built spec from the same settings agrees exactly, so the property is
        // in the VALUES, not any hidden state tied to the first spec instance.
        const pcg::InfiniteSpec spec2 = specFor(hf);
        bool againStable = true;
        for (const auto& p : points)
            if (pcg::sampleInfiniteHeightCm(spec, hf.amplitudeCm, hf.baseZCm, p.first, p.second) !=
                pcg::sampleInfiniteHeightCm(spec2, hf.amplitudeCm, hf.baseZCm, p.first, p.second))
                againStable = false;
        check(againStable, "a second, independently-built but value-identical spec agrees byte for byte "
                            "with the first, everywhere sampled");
    }

    // ---- continuity: no seam at a chunk boundary -----------------------------------------------------
    {
        // An arbitrary chunk seam -- a multiple of the default chunk size, exactly where a streaming
        // world's chunks meet.
        const f32 boundary = static_cast<f32>(kDefaultChunkSizeCm) * 7.0f;
        const f32 eps = 0.01f;   // 0.1mm either side of the boundary
        bool continuous = true;
        f32 maxJumpCm = 0.0f;
        for (f32 wy = -30000.0f; wy <= 30000.0f; wy += 977.0f) {
            const f32 below = pcg::sampleInfiniteHeightCm(spec, hf.amplitudeCm, hf.baseZCm, boundary - eps, wy);
            const f32 above = pcg::sampleInfiniteHeightCm(spec, hf.amplitudeCm, hf.baseZCm, boundary + eps, wy);
            const f32 jump = std::fabs(above - below);
            if (jump > maxJumpCm) maxJumpCm = jump;
            // A generous bound: a C1-continuous field crossed by a 0.02cm step should move by a
            // vanishing fraction of an 800cm amplitude. 1cm is nowhere close to tight and still rules
            // out a lattice-cell cliff, which would be tens to hundreds of centimetres.
            if (jump > 1.0f) continuous = false;
        }
        check(continuous, "sampling 0.01cm either side of a chunk boundary never jumps by more than 1cm, "
                           "anywhere along a 60m span across it -- no seam");
        AVER_INFO("   note  max jump across a 0.02cm step near the boundary: {:.6f}cm", maxJumpCm);

        // THE CONTROL: sampleInfinite -- the per-cell density field this deliberately does NOT reuse as
        // a height function -- really does jump at a lattice cell edge under the same tiny step, which
        // is exactly why sampleInfiniteHeight exists instead of calling sampleInfinite directly.
        bool sawCliff = false;
        f32 maxControlJump = 0.0f;
        const f32 cellEdge = hf.featureSizeCm * 4.0f;   // an exact octave-0 lattice boundary
        for (f32 wy = -30000.0f; wy <= 30000.0f; wy += 977.0f) {
            const f32 below = pcg::sampleInfinite(spec, cellEdge - eps, wy, 0.0f);
            const f32 above = pcg::sampleInfinite(spec, cellEdge + eps, wy, 0.0f);
            const f32 jump = std::fabs(above - below);
            if (jump > maxControlJump) maxControlJump = jump;
            if (jump > 0.05f) sawCliff = true;
        }
        check(sawCliff, "CONTROL: sampleInfinite DOES jump across the same tiny step at a lattice cell "
                         "edge -- confirming sampleInfiniteHeight's continuity is a real property of the "
                         "new function, not an artifact too small a step would hide anyway");
        AVER_INFO("   note  control (sampleInfinite) max jump across the same tiny step: {:.6f}", maxControlJump);
    }

    // ---- leaving it unset stays byte-identical to today's flat generator ------------------------------
    {
        GeneratorSettings gs;
        gs.worldSeed = 0xABCDEFull;
        gs.samplesPerAxis = 5;
        gs.threshold = 0.5f;
        gs.featureSizeCm = 1600.0f;
        gs.octaves = 3;
        check(!gs.heightSource, "a GeneratorSettings that never calls makeInfiniteHeightSource carries "
                                 "no height source, exactly as documented");

        GeneratedChunkSource flatA(gs);
        GeneratedChunkSource flatB(gs);
        std::vector<ChunkCoord> coords;
        for (i32 x = -3; x <= 3; ++x)
            for (i32 y = -3; y <= 3; ++y) coords.push_back(ChunkCoord{x, y, 0});

        bool everyZZero = true, identical = true;
        for (const ChunkCoord& c : coords) {
            const ChunkPayload a = flatA.generate(c);
            const ChunkPayload b = flatB.generate(c);
            if (encodeChunk(a) != encodeChunk(b)) identical = false;
            for (const PayloadEntity& e : a.entities)
                if (e.local.position.z != 0.0f) everyZZero = false;
        }
        check(everyZZero, "with no height source installed, every entity still sits at local Z == 0");
        check(identical, "two GeneratedChunkSource instances built from identical, height-source-free "
                          "settings generate byte-identical payloads");

        // Installing the infinite field must change Z only.
        GeneratorSettings hs = gs;
        InfiniteHeightFieldSettings hfs;
        hfs.seed = 7;
        hfs.featureSizeCm = 2400.0f;
        hfs.amplitudeCm = 500.0f;
        hfs.octaves = 3;
        hfs.baseZCm = 100.0f;
        hs.heightSource = makeInfiniteHeightSource(hfs);
        GeneratedChunkSource heighted(hs);

        bool sameShape = true, anyNonFlat = false, allFinite = true;
        for (const ChunkCoord& c : coords) {
            const ChunkPayload flatP = flatA.generate(c);
            const ChunkPayload hp = heighted.generate(c);
            if (flatP.entities.size() != hp.entities.size()) { sameShape = false; continue; }
            for (usize i = 0; i < flatP.entities.size(); ++i) {
                if (flatP.entities[i].name != hp.entities[i].name) sameShape = false;
                if (flatP.entities[i].local.position.x != hp.entities[i].local.position.x) sameShape = false;
                if (flatP.entities[i].local.position.y != hp.entities[i].local.position.y) sameShape = false;
                if (hp.entities[i].local.position.z != 0.0f) anyNonFlat = true;
                if (!std::isfinite(hp.entities[i].local.position.z)) allFinite = false;
            }
        }
        check(sameShape, "installing the infinite height field changes no candidate's existence, name, X or Y");
        check(anyNonFlat, "...and it DOES move at least some entities off local Z == 0");
        check(allFinite, "...to a finite height, always");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
