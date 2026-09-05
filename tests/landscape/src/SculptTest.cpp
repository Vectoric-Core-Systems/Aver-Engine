// The sculpt editor's CPU model: brush edits (raise/lower/smooth/flatten), the ray-vs-heightfield
// pick a cursor needs, and (guarded) that editing a section invalidates exactly the resident meshes
// an edit could have touched. Exit code = failure count. No device, no GPU, no .NET.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/landscape/HeightfieldRay.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/Sculpt.hpp"
#if AVER_LANDSCAPE_TEST_RENDERER
#include "aver/landscape/LandscapeRenderer.hpp"
#include "aver/rhi/RHI.hpp"
#include <unordered_set>
#endif

#include <cmath>
#include <filesystem>
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

// A flat n x n section at `h`, spacing 100 cm, origin at (0,0,0).
static fmt::OcLandData makeFlat(u32 n, f32 h, f32 spacing = 100.0f) {
    fmt::OcLandData d;
    d.sampleCount = n;
    d.spacingCm = spacing;
    d.heights.assign(static_cast<usize>(n) * n, h);
    return d;
}

int main() {
    AVER_INFO("=== brushRect ===");
    {
        fmt::OcLandData d = makeFlat(65, 500.0f);   // origin (0,0,0), extent 6400 cm, centre index 32
        BrushParams p;
        p.centerCm[0] = 3200.0f; p.centerCm[1] = 3200.0f;
        p.radiusCm = 1000.0f;
        const BrushRect r = brushRect(d, p);
        check(!r.empty, "a centred brush overlaps the grid");
        check(r.x0 == 22 && r.x1 == 42 && r.y0 == 22 && r.y1 == 42,
              "rect matches radius/spacing exactly (got [" + std::to_string(r.x0) + "," +
              std::to_string(r.y0) + "]-[" + std::to_string(r.x1) + "," + std::to_string(r.y1) + "])");

        BrushParams off = p;
        off.centerCm[0] = 1'000'000.0f;   // nowhere near the grid
        check(brushRect(d, off).empty, "a brush centred far off-grid is empty");

        BrushParams zero = p;
        zero.radiusCm = 0.0f;
        check(brushRect(d, zero).empty, "a zero-radius brush is empty");
    }

    AVER_INFO("=== raise / lower ===");
    {
        fmt::OcLandData d = makeFlat(65, 500.0f);
        BrushParams p;
        p.centerCm[0] = 3200.0f; p.centerCm[1] = 3200.0f;   // sample (32,32), exactly
        p.radiusCm = 1000.0f;                                // 10 samples
        p.strength = 200.0f;
        p.mode = BrushMode::Raise;

        const f32 before = d.heightAt(32, 32);
        const BrushRect r = applyBrush(d, p, 1.0f);
        check(!r.empty, "the stroke touched samples");
        check(std::fabs(d.heightAt(32, 32) - (before + 200.0f)) < 0.01f,
              "full strength at the centre (dist 0, falloff 1): " + std::to_string(d.heightAt(32, 32)));
        check(std::fabs(d.heightAt(37, 32) - (before + 100.0f)) < 0.5f,
              "half strength at half the radius (smoothstep(0.5) == 0.5): " +
              std::to_string(d.heightAt(37, 32)));
        check(std::fabs(d.heightAt(42, 32) - before) < 0.01f,
              "no change exactly AT the rim (falloff 0): " + std::to_string(d.heightAt(42, 32)));
        check(d.heightAt(0, 0) == before, "the far corner, outside the radius, is untouched");
        check(d.boundsMax[2] > d.boundsMin[2], "bounds were recomputed after a real edit");
        check(std::fabs(d.boundsMax[2] - d.heightAt(32, 32)) < 0.01f,
              "boundsMax.z tracks the new peak, not the old flat height");

        fmt::OcLandData lo = makeFlat(65, 500.0f);
        BrushParams pl = p; pl.mode = BrushMode::Lower;
        applyBrush(lo, pl, 1.0f);
        check(std::fabs(lo.heightAt(32, 32) - (before - 200.0f)) < 0.01f,
              "lower moves the centre down by the same amount raise moves it up");

        // A brush entirely off-grid changes nothing at all.
        fmt::OcLandData untouched = makeFlat(65, 500.0f);
        BrushParams farAway = p; farAway.centerCm[0] = 9'000'000.0f;
        const BrushRect empty = applyBrush(untouched, farAway, 1.0f);
        check(empty.empty, "an off-grid stroke reports an empty rect");
        check(untouched.heightAt(32, 32) == 500.0f, "...and genuinely left the data alone");
    }

    AVER_INFO("=== flatten ===");
    {
        fmt::OcLandData d = makeFlat(65, 500.0f);
        BrushParams p;
        p.centerCm[0] = 3200.0f; p.centerCm[1] = 3200.0f;
        p.radiusCm = 1000.0f;
        p.mode = BrushMode::Flatten;
        p.flattenTargetCm = 800.0f;

        // Ticked like a held mouse button (amount << 1 per tick), not one all-or-nothing click --
        // see Sculpt.cpp's own comment on why a single amount=1 tick would already saturate near the
        // centre. Converges geometrically; 30 ticks at 0.1 is (1-0.4)^30, far under any float epsilon.
        f32 prevDist = std::fabs(d.heightAt(32, 32) - 800.0f);
        bool monotoneCloser = true;
        for (int tick = 0; tick < 30; ++tick) {
            applyBrush(d, p, 0.1f);
            const f32 dist = std::fabs(d.heightAt(32, 32) - 800.0f);
            if (dist > prevDist + 1e-4f) monotoneCloser = false;
            prevDist = dist;
        }
        check(monotoneCloser, "each tick moves the centre no farther from the target");
        check(std::fabs(d.heightAt(32, 32) - 800.0f) < 0.5f,
              "30 gentle ticks converge to within half a centimetre of the target (got " +
              std::to_string(d.heightAt(32, 32)) + ")");
        check(d.heightAt(42, 32) == 500.0f, "exactly at the rim, flatten never moves anything (falloff 0)");
        check(d.heightAt(0, 0) == 500.0f, "outside the radius entirely, untouched");
    }

    AVER_INFO("=== smooth ===");
    {
        // A flat plane with one lone spike -- the same shape LandscapeTreeTest's own fixture uses a
        // spike for: it makes the effect of blending unambiguous, because everything BUT the spike
        // starts at the same value.
        fmt::OcLandData d = makeFlat(65, 500.0f);
        d.heights[static_cast<usize>(32) * 65 + 32] = 1500.0f;

        BrushParams p;
        p.centerCm[0] = 3200.0f; p.centerCm[1] = 3200.0f;
        p.radiusCm = 350.0f;     // 3.5 samples: covers the spike's immediate neighbours
        p.mode = BrushMode::Smooth;

        for (int tick = 0; tick < 30; ++tick) applyBrush(d, p, 0.15f);

        check(d.heightAt(32, 32) < 1000.0f,
              "the spike is substantially smoothed down (got " +
              std::to_string(d.heightAt(32, 32)) + ")");
        check(d.heightAt(31, 32) > 500.0f,
              "and its immediate neighbour rose -- smoothing blends both ways, not just clamps down "
              "(got " + std::to_string(d.heightAt(31, 32)) + ")");
        check(d.heightAt(52, 32) == 500.0f,
              "ten samples away (2000 cm, well outside the 350 cm radius) is untouched exactly");
        check(d.boundsMin[2] == 500.0f, "the flat baseline is still the recomputed minimum");
    }

    AVER_INFO("=== raycastHeightfield: a flat plane ===");
    {
        fmt::OcLandData d = makeFlat(33, 200.0f);   // 32 quads, flat at z=200
        const f32 ro[3] = {1500.0f, 1500.0f, 5000.0f};
        const f32 rd[3] = {0.0f, 0.0f, -1.0f};
        HeightfieldHit hit;
        check(raycastHeightfield(d, ro, rd, hit), "a straight-down ray over the grid hits");
        check(std::fabs(hit.posCm[2] - 200.0f) < 0.01f, "at the plane's exact height");
        check(std::fabs(hit.posCm[0] - 1500.0f) < 0.01f && std::fabs(hit.posCm[1] - 1500.0f) < 0.01f,
              "and directly under the ray, for a vertical ray");
        check(std::fabs(hit.distCm - 4800.0f) < 0.01f, "distance is origin.z minus the plane's height");

        // An angled ray: still hits the same plane, at the XY a straight-line projection predicts.
        const f32 ro2[3] = {0.0f, 0.0f, 1200.0f};
        const f32 rd2[3] = {1.0f, 0.0f, -1.0f};   // 45 degrees down +X; crosses z=200 at x=1000
        HeightfieldHit hit2;
        check(raycastHeightfield(d, ro2, rd2, hit2), "a 45-degree ray also hits");
        check(std::fabs(hit2.posCm[0] - 1000.0f) < 1.0f, "at the XY a straight projection predicts (x=" +
              std::to_string(hit2.posCm[0]) + ")");
        check(std::fabs(hit2.posCm[2] - 200.0f) < 1.0f, "on the surface, not above or below it");

        // A ray that never reaches the grid's footprint at all.
        const f32 ro3[3] = {-9000.0f, -9000.0f, 500.0f};
        const f32 rd3[3] = {-1.0f, 0.0f, 0.0f};   // runs away from the grid, parallel to the ground
        HeightfieldHit hit3;
        check(!raycastHeightfield(d, ro3, rd3, hit3), "a ray that never enters the footprint misses");

        // A ray over the grid but budgeted too short to reach the surface.
        const f32 ro4[3] = {1500.0f, 1500.0f, 50000.0f};
        HeightfieldHit hit4;
        check(!raycastHeightfield(d, ro4, rd, hit4, /*maxDistCm*/ 1000.0f),
              "a ray budgeted shorter than the drop to the surface misses");

        // Degenerate input is refused, not crashed on.
        fmt::OcLandData bad;
        HeightfieldHit hitBad;
        check(!raycastHeightfield(bad, ro, rd, hitBad), "an empty section is refused");
    }

    AVER_INFO("=== raycastHeightfield: a real peak ===");
    {
        // The same asymmetric-peak shape LandscapeTreeTest's physics-bridge section uses, so a
        // transposed read (the bug that section exists to catch on the physics side) would also be
        // visible here as a hit landing on the wrong sample.
        const u32 M = 33;
        const u32 peakIx = 7, peakIy = 22;
        fmt::OcLandData d;
        d.sampleCount = M;
        d.spacingCm = 200.0f;
        d.originCm[0] = 1000.0f; d.originCm[1] = -3000.0f; d.originCm[2] = 0.0f;
        d.heights.assign(static_cast<usize>(M) * M, 0.0f);
        d.heights[static_cast<usize>(peakIy) * M + peakIx] = 900.0f;

        f32 peakWorld[3];
        d.worldAt(peakIx, peakIy, peakWorld);
        const f32 ro[3] = {peakWorld[0], peakWorld[1], 5000.0f};
        const f32 rd[3] = {0.0f, 0.0f, -1.0f};
        HeightfieldHit hit;
        check(raycastHeightfield(d, ro, rd, hit), "a ray dropped where the peak is hits");
        check(hit.posCm[2] > 850.0f,
              "and lands on the peak, not the surrounding flat (z=" + std::to_string(hit.posCm[2]) + ")");

        f32 flatWorld[3];
        d.worldAt(peakIy, peakIx, flatWorld);   // indices swapped, same check LandscapeTreeTest makes
        const f32 ro2[3] = {flatWorld[0], flatWorld[1], 5000.0f};
        HeightfieldHit hit2;
        check(raycastHeightfield(d, ro2, rd, hit2), "the transposed position is still on the grid");
        check(hit2.posCm[2] < 50.0f,
              "but flat there -- a transposed read would have put the peak here (z=" +
              std::to_string(hit2.posCm[2]) + ")");
    }

    AVER_INFO("=== a sculpt reaches disk ===");
    {
        const u32 N = 17;
        fmt::OcLandData d;
        d.sampleCount = N;
        d.spacingCm = 100.0f;
        d.heights.assign(static_cast<usize>(N) * N, 300.0f);

        BrushParams p;
        p.centerCm[0] = 800.0f; p.centerCm[1] = 800.0f;   // sample (8,8), the centre of a 17x17 grid
        p.radiusCm = 900.0f;
        p.strength = 400.0f;
        p.mode = BrushMode::Raise;
        applyBrush(d, p, 1.0f);
        const f32 sculpted = d.heightAt(8, 8);
        check(sculpted > 300.0f, "the in-memory centre sample actually rose");

        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-sculpt-test";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = (dir / "sculpted.ocland").string();

        std::string why;
        check(fmt::saveOcLand(path, d, &why), "the sculpted section saves (" + why + ")");
        fmt::OcLandData back;
        check(fmt::loadOcLand(path, back, &why), "and loads back (" + why + ")");

        // Heights are u16-quantised across their own observed range on save -- see OcLand.hpp -- so
        // the round trip is within one quantisation step, the same tolerance tests/formats'
        // LandscapeTest.cpp uses for its own round trip.
        const f32 lo = d.boundsMin[2], hi = d.boundsMax[2];
        const f32 step = (hi - lo) / 65535.0f;
        check(std::fabs(back.heightAt(8, 8) - sculpted) <= step + 0.01f,
              "the sculpted height survives the save/load round trip (wrote " +
              std::to_string(sculpted) + ", read back " + std::to_string(back.heightAt(8, 8)) + ")");
        check(back.heightAt(0, 0) == 300.0f || std::fabs(back.heightAt(0, 0) - 300.0f) <= step,
              "an untouched corner is still the original flat height");

        std::filesystem::remove_all(dir, ec);
    }

#if AVER_LANDSCAPE_TEST_RENDERER
    AVER_INFO("=== forgetOverlapping invalidates only what an edit could have touched ===");
    {
        // A stand-in device that only counts and records -- the same trick LandscapeEvictTest's
        // CountingDevice and LandscapeTreeTest's own Recorder both use, so this needs no GPU either.
        struct CountingDevice final : rhi::IDevice {
            u32 nextHandle = 1, created = 0, destroyed = 0;
            std::unordered_set<u32> live;
            rhi::Backend backend() const override { return rhi::Backend::Null; }
            const char* adapterName() const override { return "counting device"; }
            rhi::IResourceFactory* resources() override { return nullptr; }
            rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
            void beginFrame() override {}
            void endFrame() override {}
            rhi::MeshHandle createMesh(const rhi::MeshVertex*, u32 vc, const u32*, u32 ic) override {
                if (vc == 0 || ic == 0) return 0;
                const rhi::MeshHandle h = nextHandle++;
                live.insert(h); ++created;
                return h;
            }
            bool destroyMesh(rhi::MeshHandle m) override {
                if (!live.erase(m)) return false;
                ++destroyed;
                return true;
            }
            void drawMesh(rhi::MeshHandle, const f32*, const f32*, f32, f32) override {}
        };

        // Same fixture as LandscapeTreeTest: 257 samples, 64 quads/node -> 3 levels, 21 nodes, a 4x4
        // grid of level-0 leaves each covering a 64-sample-square corner of the section.
        const u32 N = 257;
        fmt::OcLandData terrain = makeFlat(N, 500.0f, 100.0f);
        LandscapeTree tree;
        std::string why;
        check(tree.build(terrain, 64, &why), "the fixture tree builds (" + why + ")");

        // Refine to the leaves everywhere and draw unclamped, so every one of the 16 level-0 corners
        // (plus their ancestors) actually becomes resident -- forgetOverlapping has nothing to prove
        // if only the root ever got created.
        SelectParams sp;
        sp.cameraCm[0] = terrain.originCm[0] + terrain.extentCm() * 0.5f;
        sp.cameraCm[1] = terrain.originCm[1] + terrain.extentCm() * 0.5f;
        sp.cameraCm[2] = 500.0f;
        sp.screenErrorPx = 0.001f;
        sp.maxDraws = 0;
        SelectResult sel;
        tree.select(sp, sel);
        check(sel.nodes.size() == 16, "an unclamped, maximally-refined selection is the 16 leaves (got " +
                                      std::to_string(sel.nodes.size()) + ")");

        CountingDevice dev;
        LandscapeRenderer r(4096);
        const f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        r.draw(dev, terrain, tree, sel, world);
        check(dev.live.size() == 17, "16 leaves plus the always-resident root are all uploaded (got " +
                                     std::to_string(dev.live.size()) + ")");

        // Find the level-0 node that owns the grid's (0,0) corner and the one that owns its FAR
        // (192,192) corner -- diagonally opposite, so a small edit near one cannot graze the other.
        u32 nearNode = kInvalidNode, farNode = kInvalidNode;
        for (u32 i = 0; i < tree.nodes().size(); ++i) {
            const LandscapeNode& n = tree.nodes()[i];
            if (n.level != 0) continue;
            if (n.sampleX == 0 && n.sampleY == 0) nearNode = i;
            if (n.sampleX == 192 && n.sampleY == 192) farNode = i;
        }
        check(nearNode != kInvalidNode && farNode != kInvalidNode, "both corner leaves were found");

        // A brush touching only the near corner's samples.
        BrushParams p;
        p.centerCm[0] = terrain.originCm[0] + 300.0f;
        p.centerCm[1] = terrain.originCm[1] + 300.0f;
        p.radiusCm = 250.0f;
        p.mode = BrushMode::Raise;
        const BrushRect touched = applyBrush(terrain, p, 1.0f);
        check(!touched.empty && touched.x1 < 60, "the edit stayed inside the near corner's own footprint");

        // Only the near leaf and the (whole-grid) root have footprints overlapping the touched rect
        // -- the level-1 ancestors were never created at all (every level-0 child uploaded directly,
        // so residentAncestor's fallback never triggered), and the far leaf is on the other side of
        // the section entirely.
        const u32 forgotten = r.forgetOverlapping(dev, tree, touched.x0, touched.y0, touched.x1, touched.y1);
        check(forgotten == 2, "exactly the near leaf and the root were forgotten, nothing else (got " +
                              std::to_string(forgotten) + ")");
        check(dev.live.size() == 15, "residency dropped by exactly those two (got " +
                                     std::to_string(dev.live.size()) + ")");

        // Redrawing the SAME selection is the proof the OTHER 15 residents were genuinely left
        // alone: if forgetOverlapping had over-forgotten, this would recreate more than 2.
        const u32 createdBefore = dev.created;
        r.draw(dev, terrain, tree, sel, world);
        check(dev.created - createdBefore == 2,
              "the next draw rebuilds exactly the two that were forgotten -- the far corner leaf and "
              "every other leaf were never disturbed");
        check(dev.live.size() == 17, "residency is back to 16 leaves + root");

        // A second edit, this time inside the FAR corner leaf's own footprint (samples [192,256] on
        // each axis) -- a different pair (that leaf, plus the root again) is forgotten, proving the
        // targeting follows where the edit actually was rather than always hitting the same nodes.
        const u32 forgottenFar = r.forgetOverlapping(dev, tree, 200, 200, 205, 205);
        check(forgottenFar == 2, "the far leaf that actually owns those samples, plus the root -- not "
                                 "the near leaf this rect nowhere overlaps (got " +
                                 std::to_string(forgottenFar) + ")");
        const u32 createdBefore2 = dev.created;
        r.draw(dev, terrain, tree, sel, world);
        check(dev.created - createdBefore2 == 2, "and exactly those two are what the next draw rebuilt");
    }
#endif

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
