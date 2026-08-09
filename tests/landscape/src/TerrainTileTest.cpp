// The infinite-fill half of Aver.Landscape: terrainHeightAt's determinism, tile coordinates, seamless
// synthesis across a tile boundary, and widenRimSkirts. Exit code = failure count. No device, no GPU.
#include "aver/core/Log.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/TerrainTile.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::landscape;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("=== terrainHeightAt ===");
    {
        TerrainNoiseParams p;   // defaults -- matching TerrainGenTool.cpp's own, and what
                                 // Content/Maps/Default.ocland was generated with (verified by
                                 // regenerating it with TerrainGenTool and comparing byte for byte).
        const f32 h1 = terrainHeightAt(1234.5f, -678.9f, p);
        const f32 h2 = terrainHeightAt(1234.5f, -678.9f, p);
        check(h1 == h2, "deterministic: the same (x, y, params) gives the exact same height twice");

        TerrainNoiseParams q = p;
        q.seed = p.seed + 1;
        check(terrainHeightAt(1234.5f, -678.9f, q) != h1,
              "a different seed changes the height (not accidentally ignored)");

        // Ridged fBm's own construction bounds it to [-amplitude, amplitude]: fbm's inner sum is a
        // weighted average of (1-|n|) terms each in [0,1], remapped *2-1 into [-1,1], then scaled by
        // amplitude. A value outside that range would mean the port drifted from the formula.
        bool inRange = true;
        for (f32 x = -50000.0f; x <= 50000.0f; x += 3333.0f)
            for (f32 y = -50000.0f; y <= 50000.0f; y += 4111.0f)
                if (std::fabs(terrainHeightAt(x, y, p)) > p.amplitudeCm + 0.01f) inRange = false;
        check(inRange, "stays within [-amplitudeCm, amplitudeCm] over a wide sample grid");

        check(terrainHeightAt(0.0f, 0.0f, TerrainNoiseParams{0, 0.0f, 900.0f, 5}) == 0.0f,
              "a zero feature size is refused gracefully (returns 0, not a divide-by-zero NaN)");
    }

    AVER_INFO("=== tile coordinates ===");
    {
        const f32 tileSize = 30000.0f;
        const f32 cx = 100.0f, cy = -50.0f;   // an arbitrary, non-origin tile-(0,0) centre

        check(tileAt(cx, cy, cx, cy, tileSize) == TileCoord{0, 0},
              "the centre point itself is tile (0, 0)");
        check(tileAt(cx + tileSize, cy, cx, cy, tileSize) == TileCoord{1, 0},
              "one tile east is (1, 0)");
        check(tileAt(cx - tileSize, cy + 2 * tileSize, cx, cy, tileSize) == TileCoord{-1, 2},
              "negative and multi-tile offsets resolve correctly");

        f32 corner[2];
        tileCornerCm(TileCoord{0, 0}, cx, cy, tileSize, corner);
        check(std::fabs(corner[0] - (cx - tileSize * 0.5f)) < 1e-3f &&
              std::fabs(corner[1] - (cy - tileSize * 0.5f)) < 1e-3f,
              "tile (0,0)'s corner sits half a tile below/left of the declared centre");

        // Round-trip: every point inside a tile's footprint must resolve back to that tile.
        bool roundTrip = true;
        for (i32 tx = -2; tx <= 2; ++tx)
            for (i32 ty = -2; ty <= 2; ++ty) {
                f32 c[2];
                tileCornerCm(TileCoord{tx, ty}, cx, cy, tileSize, c);
                const f32 midX = c[0] + tileSize * 0.4f, midY = c[1] + tileSize * 0.6f;
                if (tileAt(midX, midY, cx, cy, tileSize) != TileCoord{tx, ty}) roundTrip = false;
            }
        check(roundTrip, "a point well inside tile (tx,ty)'s footprint maps back to (tx,ty)");
    }

    AVER_INFO("=== synthesizeTerrainTile ===");
    {
        const f32 tileSize = 30000.0f;
        const u32 samples = 257;   // (257-1)/64 = 4, a power of two -- same as Default.ocland
        const TerrainNoiseParams noise;

        fmt::OcLandData home;
        check(synthesizeTerrainTile(TileCoord{0, 0}, 0.0f, 0.0f, tileSize, samples, noise, home),
              "tile (0,0) synthesizes");
        check(home.valid(), "the synthesized tile is internally consistent");
        check(home.sampleCount == samples, "sampleCount matches the request");
        check(std::fabs(home.spacingCm - tileSize / static_cast<f32>(samples - 1)) < 1e-3f,
              "spacing spans exactly one tile width");
        check(std::fabs(home.extentCm() - tileSize) < 1e-2f, "extentCm() reproduces the tile size");

        check(!synthesizeTerrainTile(TileCoord{0, 0}, 0.0f, 0.0f, tileSize, 1, noise, home),
              "a degenerate sampleCount (< kOcLandMinSamples) is refused");

        // THE SEAM CHECK: tile (0,0) and tile (1,0) share an edge at x = tileSize/2. Both are built
        // from the SAME terrainHeightAt, so the shared column must be bit-for-bit identical -- the
        // whole reason a ring of these can surround an authored section with no crack.
        fmt::OcLandData east;
        check(synthesizeTerrainTile(TileCoord{1, 0}, 0.0f, 0.0f, tileSize, samples, noise, east),
              "the eastward neighbour tile synthesizes");
        bool edgeMatches = true;
        f64 worstDiff = 0.0;
        for (u32 iy = 0; iy < samples; ++iy) {
            const f32 h0 = home.heightAt(samples - 1, iy);   // home's east edge
            const f32 h1 = east.heightAt(0, iy);             // east's west edge -- same world (x,y)
            const f64 diff = std::fabs(static_cast<f64>(h0) - static_cast<f64>(h1));
            worstDiff = std::fmax(worstDiff, diff);
            if (h0 != h1) edgeMatches = false;
        }
        check(edgeMatches, "adjacent tiles' shared edge heights are EXACTLY equal (worst diff " +
                            std::to_string(worstDiff) + "cm) -- no seam between them");

        // And the north neighbour, for the other axis.
        fmt::OcLandData north;
        check(synthesizeTerrainTile(TileCoord{0, 1}, 0.0f, 0.0f, tileSize, samples, noise, north),
              "the northward neighbour tile synthesizes");
        bool edgeMatchesN = true;
        for (u32 ix = 0; ix < samples; ++ix)
            if (home.heightAt(ix, samples - 1) != north.heightAt(ix, 0)) edgeMatchesN = false;
        check(edgeMatchesN, "the north edge matches its neighbour exactly too");

        // A tile far from the origin still agrees with a direct terrainHeightAt call at the same
        // world position -- the synthesis loop is not silently sampling the wrong (x, y).
        fmt::OcLandData far;
        check(synthesizeTerrainTile(TileCoord{7, -3}, 0.0f, 0.0f, tileSize, samples, noise, far),
              "a far tile synthesizes");
        const f32 expect = terrainHeightAt(far.originCm[0] + 10 * far.spacingCm,
                                           far.originCm[1] + 3 * far.spacingCm, noise);
        check(far.heightAt(10, 3) == expect,
              "a far tile's sample equals terrainHeightAt at that sample's own world position");
    }

    AVER_INFO("=== widenRimSkirts ===");
    {
        // 257 samples at 64 quads/node -- same tiling as Default.ocland -- gives a 4x4 grid of level-0
        // leaves (21 nodes total). The middle 2x2 block of that 4x4 grid touches none of the four
        // outer edges, which is what gives this test a genuine interior node to contrast against.
        fmt::OcLandData d;
        check(synthesizeTerrainTile(TileCoord{0, 0}, 0.0f, 0.0f, 30000.0f, 257,
                                    TerrainNoiseParams{}, d), "a 257-sample test section synthesizes");
        LandscapeTree tree;
        std::string why;
        check(tree.build(d, 64, &why), "builds (" + why + ")");

        f32 minRimBefore = 1e30f, maxRimBefore = -1e30f;
        for (const LandscapeNode& n : tree.nodes()) {
            const bool onRim = n.sampleX == 0 || n.sampleY == 0;   // level-0 nodes at (0,*) or (*,0)
            if (onRim && n.level == 0) {
                minRimBefore = std::fmin(minRimBefore, n.skirtCm);
                maxRimBefore = std::fmax(maxRimBefore, n.skirtCm);
            }
        }
        check(maxRimBefore < 5000.0f, "before widening, rim skirts are the small build()-computed "
                                       "default (got " + std::to_string(maxRimBefore) + "cm)");

        constexpr f32 kFloor = 12345.0f;
        tree.widenRimSkirts(kFloor);

        bool allRimWidened = true, anyInteriorUnchanged = false;
        for (const LandscapeNode& n : tree.nodes()) {
            const bool onRim = n.sampleX == 0 || n.sampleY == 0 ||
                                n.sampleX + n.spanQuads >= tree.nodes()[tree.root()].spanQuads ||
                                n.sampleY + n.spanQuads >= tree.nodes()[tree.root()].spanQuads;
            if (onRim) {
                if (n.skirtCm < kFloor) allRimWidened = false;
            } else if (n.skirtCm < kFloor) {
                anyInteriorUnchanged = true;
            }
        }
        check(allRimWidened, "every outer-rim node's skirt is now at least the floor");
        check(anyInteriorUnchanged, "an interior node's skirt is untouched -- this is selective, not "
                                     "a blanket increase over the whole tree");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
