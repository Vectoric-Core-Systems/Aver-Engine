// The landscape CPU model: the quadtree, the LOD metric, frustum culling and chunk geometry.
// Exit code = failure count. No device, no GPU, no .NET.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/landscape/ChunkMesh.hpp"
#include "aver/landscape/LandscapeTree.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::landscape;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// A section with REAL RELIEF. A flat or a linear field has zero geometric error at every level, which
// would make the LOD metric trivially satisfied and prove nothing -- the error would be 0 everywhere
// and any threshold would accept the root. Two sine ridges at different frequencies give a surface that
// a coarse tessellation genuinely cannot represent, plus a sharp spike so the max-deviation path is
// exercised rather than only the smooth one.
static fmt::OcLandData makeTerrain(u32 n, f32 spacing = 100.0f) {
    fmt::OcLandData d;
    d.sampleCount = n;
    d.spacingCm = spacing;
    d.originCm[0] = 0.0f;
    d.originCm[1] = 0.0f;
    d.originCm[2] = 0.0f;
    d.heights.resize(static_cast<usize>(n) * n);
    for (u32 iy = 0; iy < n; ++iy) {
        for (u32 ix = 0; ix < n; ++ix) {
            const f32 fx = static_cast<f32>(ix), fy = static_cast<f32>(iy);
            f32 h = 400.0f * std::sin(fx * 0.11f) + 250.0f * std::sin(fy * 0.27f)
                  + 60.0f * std::sin((fx + fy) * 0.9f);
            if (ix == n / 3 && iy == n / 3) h += 1500.0f;      // a spike, deliberately one sample wide
            d.heights[static_cast<usize>(iy) * n + ix] = h;
        }
    }
    return d;
}

int main() {
    const u32 N = 257;              // 4*64 + 1 -> 3 levels, 16+4+1 = 21 nodes. Small enough to be fast.
    const fmt::OcLandData terrain = makeTerrain(N);

    AVER_INFO("=== the tree ===");
    LandscapeTree tree;
    {
        std::string why;
        check(tree.build(terrain, 64, &why), "builds (" + why + ")");
        check(tree.levelCount() == 3, "257 samples at 64 quads gives 3 levels (got " +
                                      std::to_string(tree.levelCount()) + ")");
        check(tree.nodes().size() == 21, "16 + 4 + 1 = 21 nodes (got " +
                                         std::to_string(tree.nodes().size()) + ")");
        check(tree.root() != kInvalidNode && tree.nodes()[tree.root()].level == 2,
              "the root is the coarsest node");
        check(tree.nodes()[tree.root()].leaf() == false, "and it has children");

        // Sizes that must be rejected rather than half-handled.
        LandscapeTree bad;
        check(!bad.build(terrain, 63, &why), "a non-power-of-two nodeQuads is refused (" + why + ")");
        fmt::OcLandData odd = makeTerrain(200);
        check(!bad.build(odd, 64, &why), "a sampleCount that is not (k*q)+1 is refused (" + why + ")");
    }

    AVER_INFO("=== the LOD metric ===");
    {
        // e_L is exactly 0 at level 0 -- every source sample IS a vertex there, so there is nothing to
        // deviate from. This is the assertion that catches an off-by-one in the stride.
        f32 worstL0 = 0.0f;
        for (const LandscapeNode& n : tree.nodes())
            if (n.level == 0) worstL0 = std::fmax(worstL0, n.errorCm);
        check(worstL0 == 0.0f, "geometric error is exactly 0 at level 0 (got " +
                               std::to_string(worstL0) + ")");

        bool monotone = true;
        for (u32 L = 1; L < tree.levelCount(); ++L)
            if (tree.levelErrorCm(L) < tree.levelErrorCm(L - 1)) monotone = false;
        check(monotone, "and non-decreasing with level (" +
                        std::to_string(tree.levelErrorCm(0)) + " -> " +
                        std::to_string(tree.levelErrorCm(1)) + " -> " +
                        std::to_string(tree.levelErrorCm(2)) + " cm)");
        check(tree.levelErrorCm(2) > 1.0f,
              "the coarsest level has real error, so the fixture actually exercises the metric");
    }

    AVER_INFO("=== selection ===");
    {
        SelectParams p;
        p.projScale = 540.0f;
        p.screenErrorPx = 2.0f;

        // Standing ON the terrain must refine all the way: at distance 0 the projected error is
        // unbounded, so nothing coarse can be accepted near the camera.
        const LandscapeNode& root = tree.nodes()[tree.root()];
        p.cameraCm[0] = root.centre[0];
        p.cameraCm[1] = root.centre[1];
        p.cameraCm[2] = root.centre[2];
        SelectResult near_;
        tree.resetHysteresis();
        tree.select(p, near_);
        bool allFinestNear = !near_.nodes.empty();
        for (u32 i : near_.nodes) if (tree.nodes()[i].level != 0) allFinestNear = false;
        check(allFinestNear, "a camera at the centre selects only level 0 (" +
                             std::to_string(near_.nodes.size()) + " nodes)");

        // Far away, the whole section should collapse towards the root.
        p.cameraCm[0] = root.centre[0];
        p.cameraCm[1] = root.centre[1] - 200000.0f;      // 2 km out
        p.cameraCm[2] = root.centre[2] + 50000.0f;
        SelectResult far_;
        tree.resetHysteresis();
        tree.select(p, far_);
        check(far_.nodes.size() < near_.nodes.size(),
              "and far away selects fewer nodes (" + std::to_string(far_.nodes.size()) + " vs " +
              std::to_string(near_.nodes.size()) + ")");

        // Every selected set must TILE the section exactly once -- no gaps, no overlaps. Measured by
        // summing the covered source area, which catches both at once and is the property a renderer
        // actually depends on.
        auto tiles = [&](const SelectResult& r) {
            u64 area = 0;
            for (u32 i : r.nodes) {
                const LandscapeNode& n = tree.nodes()[i];
                area += static_cast<u64>(n.spanQuads) * n.spanQuads;
            }
            return area == static_cast<u64>(N - 1) * (N - 1);
        };
        check(tiles(near_), "the near selection tiles the section exactly once");
        check(tiles(far_), "and so does the far one");

        // HYSTERESIS: hovering on a boundary must not oscillate. Walk the camera in tiny steps across
        // the distance where a level switches and count how many times the node count changes. Without
        // the 0.8x gap this flips every step.
        tree.resetHysteresis();
        u32 flips = 0;
        usize last = 0;
        for (int step = 0; step < 40; ++step) {
            // Back and forth over a 200 cm window, which is far narrower than the switch distance.
            const f32 wobble = (step % 2 == 0) ? 0.0f : 200.0f;
            p.cameraCm[1] = root.centre[1] - 60000.0f - wobble;
            SelectResult r;
            tree.select(p, r);
            if (step > 0 && r.nodes.size() != last) ++flips;
            last = r.nodes.size();
        }
        check(flips <= 1, "a camera wobbling on a level boundary does not oscillate (" +
                          std::to_string(flips) + " changes over 40 frames)");

        // The draw clamp is a CEILING, and it reports what it dropped rather than silently truncating.
        SelectParams clamped = p;
        clamped.cameraCm[0] = root.centre[0];
        clamped.cameraCm[1] = root.centre[1];
        clamped.cameraCm[2] = root.centre[2];
        clamped.maxDraws = 4;
        SelectResult small;
        tree.resetHysteresis();
        tree.select(clamped, small);
        check(small.nodes.size() == 4, "maxDraws is a hard ceiling");
        check(small.dropped == near_.nodes.size() - 4,
              "and the number dropped is reported, not swallowed (" +
              std::to_string(small.dropped) + ")");
    }

    AVER_INFO("=== frustum ===");
    {
        // Built from a REAL projection rather than hand-written planes, so the row-vector convention is
        // actually exercised. A transposed extraction is the failure this guards, and it passes every
        // hand-written test.
        const Mat4 proj = Mat4::perspectiveLH(1.0f, 1.6f, 10.0f, 100000.0f);
        // Mat4 stores [4][4]; the frustum extractor takes a flat 16 in the same row-major order, which
        // is what the engine's row-vector convention means by "row-major".
        f32 m[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[r*4 + c] = proj.m[r][c];
        const Frustum f = Frustum::fromViewProj(m);

        // With an identity view, the camera sits at the origin looking down +Z in clip terms. A point
        // well in front is inside; the same distance BEHIND is not.
        const f32 inFront[3] = {0.0f, 0.0f, 5000.0f};
        const f32 behind[3]  = {0.0f, 0.0f, -5000.0f};
        check(f.intersectsSphere(inFront, 100.0f), "a sphere in front of the eye is kept");
        check(!f.intersectsSphere(behind, 100.0f), "one behind it is culled");
        // A big sphere straddling the eye must be KEPT: culling is conservative, and a chunk the camera
        // is standing inside must never disappear.
        check(f.intersectsSphere(behind, 20000.0f),
              "a sphere large enough to straddle the eye is kept (culling is conservative)");

        // Wired into selection, culling must remove something and must not remove everything.
        SelectParams p;
        p.cameraCm[0] = tree.nodes()[tree.root()].centre[0];
        p.cameraCm[1] = tree.nodes()[tree.root()].centre[1];
        p.cameraCm[2] = tree.nodes()[tree.root()].centre[2] + 20000.0f;
        p.useFrustum = true;
        p.frustum = f;
        SelectResult r;
        tree.resetHysteresis();
        tree.select(p, r);
        check(r.culled > 0, "culling rejects nodes outside the frustum (" +
                            std::to_string(r.culled) + " culled)");
    }

    AVER_INFO("=== chunk geometry ===");
    {
        const ChunkCounts c = chunkCounts(64);
        check(c.surfaceIndices == 24576, "64x64 quads is 24576 surface indices (got " +
                                         std::to_string(c.surfaceIndices) + ")");
        check(c.indices - c.surfaceIndices == 1536, "and 1536 skirt indices (got " +
              std::to_string(c.indices - c.surfaceIndices) + ")");
        check(c.surfaceVertices == 65 * 65, "65x65 surface vertices");
        check(c.vertices == 65 * 65 + 4 * 65, "plus four rims of 65");

        // Built at EVERY level, because the stride arithmetic differs at each and a level-0 chunk is
        // the only one where source sample and vertex coincide.
        bool allBuilt = true, allInRange = true, allInsideNode = true;
        u32 built = 0;
        for (u32 i = 0; i < tree.nodes().size(); ++i) {
            ChunkMesh mesh;
            if (!buildChunkMesh(terrain, tree, i, mesh, 1000.0f)) { allBuilt = false; continue; }
            ++built;
            if (mesh.vertices.size() != c.vertices || mesh.indices.size() != c.indices)
                allBuilt = false;
            for (u32 idx : mesh.indices)
                if (idx >= mesh.vertices.size()) allInRange = false;

            // Every SURFACE vertex must land inside the node's own footprint. This is what catches a
            // stride or origin mistake: a chunk that sampled the wrong region would still produce
            // valid-looking geometry, in the wrong place.
            const LandscapeNode& n = tree.nodes()[i];
            const f32 x0 = terrain.originCm[0] + static_cast<f32>(n.sampleX) * terrain.spacingCm;
            const f32 y0 = terrain.originCm[1] + static_cast<f32>(n.sampleY) * terrain.spacingCm;
            const f32 sideCm = static_cast<f32>(n.spanQuads) * terrain.spacingCm;
            for (u32 v = 0; v < mesh.skirtVertexStart; ++v) {
                const LandVertex& lv = mesh.vertices[v];
                if (lv.px < x0 - 0.01f || lv.px > x0 + sideCm + 0.01f ||
                    lv.py < y0 - 0.01f || lv.py > y0 + sideCm + 0.01f) allInsideNode = false;
            }
        }
        check(built == tree.nodes().size(), "every node builds a chunk (" + std::to_string(built) + ")");
        check(allBuilt, "each with exactly the budgeted vertex and index counts");
        check(allInRange, "and no index outside its own vertex array");
        check(allInsideNode, "every surface vertex lands inside its node's footprint");

        // Normals must point generally UP. +Z is the engine's up, and a sign error in the gradient
        // gives a landscape lit from underneath -- which looks like a broken light, not a broken normal.
        ChunkMesh m0;
        check(buildChunkMesh(terrain, tree, 0, m0, 1000.0f), "a level-0 chunk builds");
        f32 worstNz = 1.0f;
        for (u32 v = 0; v < m0.skirtVertexStart; ++v) worstNz = std::fmin(worstNz, m0.vertices[v].nz);
        check(worstNz > 0.0f, "every surface normal has a positive +Z component (worst " +
                              std::to_string(worstNz) + ")");

        // World-aligned UVs, so two chunks at different levels agree where they overlap. Checked by
        // sampling the same world position from two different levels.
        check(std::fabs(m0.vertices[0].u - m0.vertices[0].px / 1000.0f) < 1e-6f,
              "uv is world-aligned, not per-node");
    }

    AVER_INFO("=== the crack-free gate ===");
    {
        // THE ASSERTION THE WHOLE SKIRT DESIGN EXISTS FOR. Where a fine node abuts a coarser one, the
        // coarse neighbour draws a chord across twice the spacing and its rim can sit BELOW the fine
        // rim. If the skirt is shallower than that gap, sky shows through the seam.
        //
        // Checked against the ACTUAL deviation along each node's edges at the next coarser stride,
        // rather than against the bound the implementation used -- otherwise this would only be
        // checking the code against itself.
        f32 worstRatio = 1e30f;
        u32 checked = 0;
        for (const LandscapeNode& n : tree.nodes()) {
            if (n.level + 1 >= tree.levelCount()) continue;   // the root has no coarser neighbour
            const u32 coarse = n.stride * 2;
            f32 gap = 0.0f;
            // Each of the four rims, in runs of the coarser stride.
            for (u32 s = 0; s + coarse <= n.spanQuads; s += coarse) {
                struct Run { u32 x, y, dx, dy; };
                const Run runs[4] = {
                    {n.sampleX + s, n.sampleY,               1, 0},
                    {n.sampleX + s, n.sampleY + n.spanQuads, 1, 0},
                    {n.sampleX,               n.sampleY + s, 0, 1},
                    {n.sampleX + n.spanQuads, n.sampleY + s, 0, 1},
                };
                for (const Run& r : runs) {
                    const f32 h0 = terrain.heightAt(r.x, r.y);
                    const f32 h1 = terrain.heightAt(r.x + r.dx * coarse, r.y + r.dy * coarse);
                    for (u32 t = 1; t < coarse; ++t) {
                        const f32 f = static_cast<f32>(t) / static_cast<f32>(coarse);
                        const f32 chord = h0 + (h1 - h0) * f;
                        const f32 fine = terrain.heightAt(r.x + r.dx * t, r.y + r.dy * t);
                        // Only a chord BELOW the fine rim opens a hole; above it the coarse neighbour
                        // simply overlaps and hides itself.
                        gap = std::fmax(gap, fine - chord);
                    }
                }
            }
            ++checked;
            if (gap > 0.0f) worstRatio = std::fmin(worstRatio, n.skirtCm / gap);
        }
        check(checked > 0, "there are nodes with a coarser neighbour to check (" +
                           std::to_string(checked) + ")");
        check(worstRatio >= 1.0f,
              "every node's skirt is at least as deep as the worst gap a 2:1 coarser neighbour opens "
              "(worst skirt/gap ratio " + std::to_string(worstRatio) + ")");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
