// The landscape CPU model: the quadtree, the LOD metric, frustum culling and chunk geometry.
// Exit code = failure count. No device, no GPU, no .NET.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/landscape/ChunkMesh.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/PhysicsBridge.hpp"
#if AVER_LANDSCAPE_TEST_RENDERER
#include "aver/landscape/LandscapeRenderer.hpp"
#endif
#if AVER_LANDSCAPE_TEST_PHYSICS
#include "aver/physics/physics_abi.h"
#endif

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::landscape;

static int g_checks = 0, g_failures = 0;

// Records one assertion and logs it.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Builds an n x n test section: two sine ridges plus a one-sample spike.
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
            if (ix == n / 3 && iy == n / 3) h += 1500.0f;      // a spike, one sample wide
            d.heights[static_cast<usize>(iy) * n + ix] = h;
        }
    }
    return d;
}

// Runs the suite. Returns the failure count.
int main() {
    const u32 N = 257;              // 4*64 + 1 -> 3 levels, 16+4+1 = 21 nodes
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

        LandscapeTree bad;
        check(!bad.build(terrain, 63, &why), "a non-power-of-two nodeQuads is refused (" + why + ")");
        fmt::OcLandData odd = makeTerrain(200);
        check(!bad.build(odd, 64, &why), "a sampleCount that is not (k*q)+1 is refused (" + why + ")");
    }

    AVER_INFO("=== the LOD metric ===");
    {
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

        p.cameraCm[0] = root.centre[0];
        p.cameraCm[1] = root.centre[1] - 200000.0f;      // 2 km out
        p.cameraCm[2] = root.centre[2] + 50000.0f;
        SelectResult far_;
        tree.resetHysteresis();
        tree.select(p, far_);
        check(far_.nodes.size() < near_.nodes.size(),
              "and far away selects fewer nodes (" + std::to_string(far_.nodes.size()) + " vs " +
              std::to_string(near_.nodes.size()) + ")");

        // True when the selection covers the section exactly once, by summed source area.
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

        tree.resetHysteresis();
        u32 flips = 0;
        usize last = 0;
        for (int step = 0; step < 40; ++step) {
            const f32 wobble = (step % 2 == 0) ? 0.0f : 200.0f;
            p.cameraCm[1] = root.centre[1] - 60000.0f - wobble;
            SelectResult r;
            tree.select(p, r);
            if (step > 0 && r.nodes.size() != last) ++flips;
            last = r.nodes.size();
        }
        check(flips <= 1, "a camera wobbling on a level boundary does not oscillate (" +
                          std::to_string(flips) + " changes over 40 frames)");

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
        const Mat4 proj = Mat4::perspectiveLH(1.0f, 1.6f, 10.0f, 100000.0f);
        // Mat4 is [4][4]; the frustum extractor takes a flat 16 in the same row-major order.
        f32 m[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[r*4 + c] = proj.m[r][c];
        const Frustum f = Frustum::fromViewProj(m);

        const f32 inFront[3] = {0.0f, 0.0f, 5000.0f};
        const f32 behind[3]  = {0.0f, 0.0f, -5000.0f};
        check(f.intersectsSphere(inFront, 100.0f), "a sphere in front of the eye is kept");
        check(!f.intersectsSphere(behind, 100.0f), "one behind it is culled");
        check(f.intersectsSphere(behind, 20000.0f),
              "a sphere large enough to straddle the eye is kept (culling is conservative)");

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

        ChunkMesh m0;
        check(buildChunkMesh(terrain, tree, 0, m0, 1000.0f), "a level-0 chunk builds");
        f32 worstNz = 1.0f;
        for (u32 v = 0; v < m0.skirtVertexStart; ++v) worstNz = std::fmin(worstNz, m0.vertices[v].nz);
        check(worstNz > 0.0f, "every surface normal has a positive +Z component (worst " +
                              std::to_string(worstNz) + ")");

        check(std::fabs(m0.vertices[0].u - m0.vertices[0].px / 1000.0f) < 1e-6f,
              "uv is world-aligned, not per-node");
    }

    AVER_INFO("=== the crack-free gate ===");
    {
        f32 worstRatio = 1e30f;
        u32 checked = 0;
        for (const LandscapeNode& n : tree.nodes()) {
            if (n.level + 1 >= tree.levelCount()) continue;   // the root has no coarser neighbour
            const u32 coarse = n.stride * 2;
            f32 gap = 0.0f;
            for (u32 s = 0; s + coarse <= n.spanQuads; s += coarse) {
                // One rim run: a start sample and a step direction.
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


    AVER_INFO("=== the physics bridge ===");
    {
        const u32 M = 33;                       // 32 quads: small, and a multiple of 8
        const u32 peakIx = 7, peakIy = 22;      // asymmetric, so a plain transpose cannot pass
        fmt::OcLandData d;
        d.sampleCount = M;
        d.spacingCm = 200.0f;
        d.originCm[0] = 1000.0f;
        d.originCm[1] = -3000.0f;
        d.originCm[2] = 0.0f;
        d.heights.assign(static_cast<usize>(M) * M, 0.0f);
        d.heights[static_cast<usize>(peakIy) * M + peakIx] = 900.0f;

        PhysicsHeightfield ph;
        check(toPhysicsHeightfield(d, ph), "the section converts");
        check(ph.sampleCount == M, "every sample is carried across, none cropped");
        check(ph.samples.size() == static_cast<usize>(M) * M, "and the array is the full square");

        const u32 px = peakIy, py = (M - 1) - peakIx;
        check(ph.samples[static_cast<usize>(py) * M + px] == 900.0f,
              "the peak sits at the transposed-and-row-flipped index");
        check(ph.samples[static_cast<usize>(peakIy) * M + peakIx] != 900.0f,
              "and NOT where a plain copy would have put it");
        check(std::fabs(ph.cornerCm[0] - (d.originCm[0] + d.extentCm())) < 0.01f,
              "the corner is the field's +X end, not its minimum corner");

#if AVER_LANDSCAPE_TEST_PHYSICS
        f32 want[3];
        d.worldAt(peakIx, peakIy, want);

        check(aver_phys_init() == 1, "physics starts");
        const int32_t body = aver_phys_add_heightfield(ph.samples.data(),
                                                       static_cast<int32_t>(ph.sampleCount),
                                                       ph.spacingCm,
                                                       ph.cornerCm[0], ph.cornerCm[1], ph.cornerCm[2]);
        check(body != 0, "the heightfield body is created");

        f32 hit[3] = {0,0,0}, nrm[3] = {0,0,0};
        const int32_t got = aver_phys_raycast(want[0], want[1], 5000.0f, 0.0f, 0.0f, -1.0f,
                                              20000.0f, hit, nrm);
        check(got == body, "a ray dropped where the peak is DRAWN hits the collision field");
        check(hit[2] > 600.0f,
              "and lands on the peak rather than the surrounding flat (z=" + std::to_string(hit[2]) + ")");

        f32 flat[3];
        d.worldAt(peakIy, peakIx, flat);        // indices swapped: where a plain transpose would put it
        f32 hit2[3] = {0,0,0};
        const int32_t got2 = aver_phys_raycast(flat[0], flat[1], 5000.0f, 0.0f, 0.0f, -1.0f,
                                               20000.0f, hit2, nrm);
        check(got2 == body, "the transposed position is still on the field");
        check(hit2[2] < 100.0f,
              "but it is FLAT there -- a plain transpose would have put the peak here (z=" +
              std::to_string(hit2[2]) + ")");
        aver_phys_shutdown();
#else
        AVER_INFO("  ..    physics not in this build; the index map above is still checked");
#endif
    }


#if AVER_LANDSCAPE_TEST_RENDERER
    AVER_INFO("=== the renderer's residency ===");
    {
        // A stand-in device that counts mesh creations and records every draw.
        struct Recorder final : rhi::IDevice {
            u32 created = 0, draws = 0;
            std::vector<rhi::MeshHandle> drawn;
            rhi::Backend backend() const override { return rhi::Backend::Null; }
            const char* adapterName() const override { return "recording device"; }
            rhi::IResourceFactory* resources() override { return nullptr; }
            rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
            void beginFrame() override {}
            void endFrame() override {}
            // Counts one upload and hands back a fresh handle. 0 for malformed geometry.
            rhi::MeshHandle createMesh(const rhi::MeshVertex* v, u32 vc,
                                       const u32* idx, u32 ic) override {
                if (!v || !idx || vc == 0 || ic == 0) return 0;
                ++created;
                return static_cast<rhi::MeshHandle>(created);   // 0 stays invalid
            }
            // Records one draw and which mesh it used.
            void drawMesh(rhi::MeshHandle m, const f32*, const f32*, f32, f32) override {
                ++draws;
                drawn.push_back(m);
            }
        };

        SelectParams p;
        const LandscapeNode& root = tree.nodes()[tree.root()];
        p.cameraCm[0] = root.centre[0];
        p.cameraCm[1] = root.centre[1];
        p.cameraCm[2] = root.centre[2];
        SelectResult sel;
        tree.resetHysteresis();
        tree.select(p, sel);

        f32 identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

        Recorder dev;
        LandscapeRenderer r(512);
        r.draw(dev, terrain, tree, sel, identity, 1000.0f);
        check(r.stats().submitted == sel.nodes.size(),
              "every selected node is submitted (" + std::to_string(r.stats().submitted) + " of " +
              std::to_string(sel.nodes.size()) + ")");
        check(r.stats().created == sel.nodes.size() + 1,
              "each uploaded one mesh, plus the always-resident root (" +
              std::to_string(r.stats().created) + ")");
        check(r.stats().skipped == 0, "nothing is skipped -- a skipped node is a hole in the ground");
        check(dev.draws == r.stats().submitted, "the device saw the same number of draws");

        const u32 createdBefore = dev.created;
        r.draw(dev, terrain, tree, sel, identity, 1000.0f);
        check(dev.created == createdBefore, "a second identical frame uploads nothing new");
        check(r.stats().created == 0, "and reports zero creations");

        Recorder dev2;
        LandscapeRenderer tiny(1);
        tiny.draw(dev2, terrain, tree, sel, identity, 1000.0f);
        check(dev2.created == 1, "a cache of 1 uploads exactly one mesh -- the root");
        check(tiny.stats().submitted >= 1, "and still submits geometry rather than nothing");
        bool unique = true;
        for (usize i = 0; i < dev2.drawn.size(); ++i)
            for (usize j = i + 1; j < dev2.drawn.size(); ++j)
                if (dev2.drawn[i] == dev2.drawn[j]) unique = false;
        check(unique, "no mesh is drawn twice in one frame (" +
                      std::to_string(dev2.drawn.size()) + " draws)");
        check(tiny.stats().skipped == 0,
              "at the cache cap nothing is skipped: everything falls back to the root (" +
              std::to_string(tiny.stats().substituted) + " substituted)");
        check(tiny.stats().substituted == sel.nodes.size(),
              "every selected node substituted, so the section is still fully covered");
    }
#endif


    AVER_INFO("=== a production-size section ===");
    {
        const u32 P = 1025;                       // 16*64 + 1
        const fmt::OcLandData big = makeTerrain(P);
        LandscapeTree bigTree;
        std::string why;
        check(bigTree.build(big, 64, &why), "1025 samples build (" + why + ")");
        check(bigTree.levelCount() == 5, "5 levels (got " + std::to_string(bigTree.levelCount()) + ")");
        check(bigTree.nodes().size() == 341,
              "256+64+16+4+1 = 341 nodes (got " + std::to_string(bigTree.nodes().size()) + ")");

        const LandscapeNode& r = bigTree.nodes()[bigTree.root()];
        check(r.spanQuads == 1024, "the root spans all 1024 quads (got " +
                                   std::to_string(r.spanQuads) + ")");
        check(r.stride == 16, "at stride 16");

        f32 worstL0 = 0.0f;
        for (const LandscapeNode& n : bigTree.nodes())
            if (n.level == 0) worstL0 = std::fmax(worstL0, n.errorCm);
        check(worstL0 == 0.0f, "geometric error is still exactly 0 at level 0");

        const ChunkCounts cc = chunkCounts(64);
        const usize perNode = cc.vertices * sizeof(LandVertex) + cc.indices * sizeof(u32);
        check(perNode > 240u * 1024u && perNode < 250u * 1024u,
              "a node is ~242 KiB as designed (" + std::to_string(perNode / 1024) + " KiB)");
        const usize fullTree = perNode * bigTree.nodes().size();
        check(fullTree > 75u * 1024u * 1024u && fullTree < 85u * 1024u * 1024u,
              "a fully resident section is ~80 MiB (" + std::to_string(fullTree / (1024*1024)) + " MiB)");

        SelectParams sp;
        sp.cameraCm[0] = r.centre[0];
        sp.cameraCm[1] = r.centre[1] - 50000.0f;
        sp.cameraCm[2] = r.centre[2] + 20000.0f;
        sp.maxDraws = 0;                          // unclamped, so tiling is testable
        SelectResult sr;
        bigTree.resetHysteresis();
        bigTree.select(sp, sr);
        u64 area = 0;
        for (u32 i : sr.nodes) {
            const LandscapeNode& n = bigTree.nodes()[i];
            area += static_cast<u64>(n.spanQuads) * n.spanQuads;
        }
        check(area == 1024ull * 1024ull,
              "a mid-range selection tiles 1024x1024 exactly (" + std::to_string(sr.nodes.size()) +
              " nodes, area " + std::to_string(area) + ")");

        SelectParams cp = sp;
        cp.cameraCm[1] = r.centre[1];
        cp.cameraCm[2] = r.centre[2] + 200.0f;
        cp.screenErrorPx = 0.01f;                 // refine everything, whatever the distance
        cp.maxDraws = 192;
        SelectResult cr;
        bigTree.resetHysteresis();
        bigTree.select(cp, cr);
        check(cr.nodes.size() == 192, "the clamp holds at exactly 192 at the real section size (got " +
                                      std::to_string(cr.nodes.size()) + ")");
        check(cr.dropped == 256 - 192,
              "and reports the 64 it dropped, rather than truncating quietly (" +
              std::to_string(cr.dropped) + ")");

        SelectParams uc = cp;
        uc.maxDraws = 0;
        SelectResult ur;
        bigTree.resetHysteresis();
        bigTree.select(uc, ur);
        check(ur.nodes.size() == 256, "unclamped, the same view selects all 256 level-0 nodes (got " +
                                      std::to_string(ur.nodes.size()) + ")");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
