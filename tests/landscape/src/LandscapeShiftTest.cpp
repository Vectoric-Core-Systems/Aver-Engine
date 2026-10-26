// The landscape and the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.2.6 item 6, R-4). Headless: no window, no GPU.
// Exit code = failure count.
//
// WHAT IS PINNED. A rebase moves every scene-frame position by -delta (a whole number of regions, 1,638,400 cm). The
// landscape must (1) keep its procedural height a function of the ABSOLUTE position, so the same hill is read at any
// origin; (2) keep texture coordinates world-aligned across frames; (3) move its quadtree with the data; (4) draw a mesh
// that was built before the rebase translated by exactly what a rebuild in the new frame would have produced, without
// building anything; and (5) with the game class, keep the sidecar and the level records absolute while its working data
// is origin-relative. The negative controls are the f32 path (which quantises past ~10 km) and a renderer that was not told.
#include "aver/core/Log.hpp"
#include "aver/landscape/ChunkMesh.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/TerrainNoise.hpp"
#include "aver/landscape/TerrainTile.hpp"
#include "aver/rhi/WorldShift.hpp"

#if AVER_LANDSCAPE_TEST_RENDERER
#include "aver/landscape/LandscapeRenderer.hpp"
#include "aver/rhi/RHI.hpp"
#endif

#ifdef AVER_LANDSCAPESHIFT_GAME
#include "aver/game/GameLandscape.hpp"
#include <filesystem>
#endif

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::landscape;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool bitEqual(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof a) == 0; }

static constexpr i64 kStep = 1'638'400;   // one region, cm

// An n x n section with relief, its sample (0,0) at (ox, oy).
static fmt::OcLandData makeTerrain(u32 n, f32 spacing, f32 ox, f32 oy) {
    fmt::OcLandData d;
    d.sampleCount = n;
    d.spacingCm = spacing;
    d.originCm[0] = ox;
    d.originCm[1] = oy;
    d.heights.resize(static_cast<usize>(n) * n);
    for (u32 iy = 0; iy < n; ++iy)
        for (u32 ix = 0; ix < n; ++ix) {
            const f32 fx = static_cast<f32>(ix), fy = static_cast<f32>(iy);
            d.heights[static_cast<usize>(iy) * n + ix] = 400.0f * std::sin(fx * 0.11f) + 250.0f * std::sin(fy * 0.27f);
        }
    return d;
}

int main() {
    AVER_INFO("LandscapeShiftTest");

    // ---- (1) the procedural height is a function of the absolute position ----
    {
        TerrainNoiseParams p;
        bool identical = true;
        for (int i = 0; i < 2000; ++i) {
            const f32 x = static_cast<f32>((i * 37) % 4001 - 2000) * 13.5f, y = static_cast<f32>((i * 91) % 3001 - 1500) * 17.25f;
            identical = identical && bitEqual(terrainHeightAtFrom(x, y, 0, 0, p), terrainHeightAt(x, y, p));
        }
        check(identical, "origin zero: terrainHeightAtFrom IS terrainHeightAt, bit for bit (identity)");

        // The same field with the f64 lattice: at a moderate origin the two paths agree to the f32 rounding of the old one.
        f32 worstMod = 0.0f;
        for (int i = 0; i < 1000; ++i) {
            const f32 x = static_cast<f32>((i * 53) % 2001 - 1000) * 11.5f, y = static_cast<f32>((i * 29) % 2001 - 1000) * 7.5f;
            const f32 viaFrom = terrainHeightAtFrom(x, y, kStep, 0, p);
            const f32 viaF32 = terrainHeightAt(static_cast<f32>(static_cast<f64>(kStep) + x), y, p);
            worstMod = std::fmax(worstMod, std::fabs(viaFrom - viaF32));
        }
        check(worstMod < 0.5f, "at one region out the f64 path reads the same field as the f32 one (worst difference " +
                                   std::to_string(worstMod) + " cm)");

        // One absolute point, read from two origins three regions apart: exactly the same height.
        const i64 o1 = 61 * kStep, o2 = 58 * kStep;
        bool sameAbs = true;
        f32 worstF32 = 0.0f;
        for (int i = 0; i < 1000; ++i) {
            const f32 x = static_cast<f32>((i * 41) % 4001 - 2000) * 0.5f, y = static_cast<f32>((i * 67) % 4001 - 2000) * 0.5f;
            const f32 a = terrainHeightAtFrom(x, y, o1, o1 / 2, p);
            const f32 b = terrainHeightAtFrom(x + static_cast<f32>(3 * kStep), y + static_cast<f32>(o1 / 2 - o2 / 2), o2, o2 / 2, p);
            sameAbs = sameAbs && bitEqual(a, b);
            // The control: the f32 absolute position quantises at 100 km.
            const f32 viaF32 = terrainHeightAt(static_cast<f32>(static_cast<f64>(o1) + x), static_cast<f32>(static_cast<f64>(o1 / 2) + y), p);
            worstF32 = std::fmax(worstF32, std::fabs(viaF32 - a));
        }
        check(sameAbs, "one absolute point, two origins: the height is bit-identical (independent of the current origin)");
        check(worstF32 > 0.05f, "negative control: the f32 absolute path is off by up to " + std::to_string(worstF32) + " cm at 100 km");
    }

    // ---- (2) a synthesised tile is the same tile in either frame ----
    {
        TerrainNoiseParams p;
        const TileCoord t{1, -1};
        const i64 oa = 59 * kStep;
        fmt::OcLandData a, b;
        const bool okA = synthesizeTerrainTile(t, 800000.0f, 12800.0f, 6400.0f, 65, p, a, oa, 0);
        const bool okB = synthesizeTerrainTile(t, 800000.0f - static_cast<f32>(kStep), 12800.0f, 6400.0f, 65, p, b, oa + kStep, 0);
        check(okA && okB, "both tiles synthesise");
        bool same = okA && okB && a.heights.size() == b.heights.size();
        for (usize i = 0; same && i < a.heights.size(); ++i) same = bitEqual(a.heights[i], b.heights[i]);
        check(same, "the same absolute tile read from two frames has bit-identical heights");
        check(okA && okB && bitEqual(a.originCm[0] - b.originCm[0], static_cast<f32>(kStep)), "its corner differs by exactly one region");
        fmt::OcLandData z0, z1;
        synthesizeTerrainTile(t, 800000.0f, 12800.0f, 6400.0f, 65, p, z0);
        synthesizeTerrainTile(t, 800000.0f, 12800.0f, 6400.0f, 65, p, z1, 0, 0);
        check(z0.heights == z1.heights, "default arguments: the origin-zero tile is today's tile (identity)");
    }

    // ---- (3) texture coordinates stay world-aligned across a rebase ----
    const u32 N = 129;
    {
        const f32 ox = 5000.0f, oy = -3200.0f;
        const fmt::OcLandData dA = makeTerrain(N, 100.0f, ox, oy);
        const fmt::OcLandData dB = makeTerrain(N, 100.0f, ox - static_cast<f32>(kStep), oy);
        LandscapeTree tree;
        std::string why;
        check(tree.build(dA, 64, &why), "the test section builds");
        ChunkMesh plain, atZero, mA, mB;
        const u32 node = tree.root();
        check(buildChunkMesh(dA, tree, node, plain, 1000.0f) && buildChunkMesh(dA, tree, node, atZero, 1000.0f, 0, 0),
              "meshes build");
        bool same = plain.vertices.size() == atZero.vertices.size();
        for (usize i = 0; same && i < plain.vertices.size(); ++i)
            same = bitEqual(plain.vertices[i].u, atZero.vertices[i].u) && bitEqual(plain.vertices[i].v, atZero.vertices[i].v);
        check(same, "origin zero: UVs are w / tile exactly (identity)");

        check(buildChunkMesh(dA, tree, node, mA, 1000.0f, 7 * kStep, 3 * kStep) &&
              buildChunkMesh(dB, tree, node, mB, 1000.0f, 8 * kStep, 3 * kStep), "meshes build at two origins");
        bool aligned = mA.vertices.size() == mB.vertices.size() && !mA.vertices.empty();
        f32 worst = 0.0f;
        for (usize i = 0; aligned && i < mA.vertices.size(); ++i) {
            const f64 du = static_cast<f64>(mA.vertices[i].u) - static_cast<f64>(mB.vertices[i].u);
            const f64 dv = static_cast<f64>(mA.vertices[i].v) - static_cast<f64>(mB.vertices[i].v);
            worst = std::fmax(worst, static_cast<f32>(std::fabs(du - std::round(du))));
            worst = std::fmax(worst, static_cast<f32>(std::fabs(dv - std::round(dv))));
        }
        check(aligned && worst < 5e-3f, "the same absolute vertex in two frames differs by whole tiles only (worst fraction " +
                                            std::to_string(worst) + ")");

        // ---- (4) the quadtree follows the data ----
        LandscapeTree moved, rebuilt;
        moved.build(dA, 64, &why);
        moved.translate(-static_cast<f32>(kStep), 0.0f);
        check(rebuilt.build(dB, 64, &why), "the rebuilt tree builds");
        bool nodesOk = moved.nodes().size() == rebuilt.nodes().size();
        f32 worstC = 0.0f;
        for (usize i = 0; nodesOk && i < moved.nodes().size(); ++i) {
            const LandscapeNode& a = moved.nodes()[i];
            const LandscapeNode& b = rebuilt.nodes()[i];
            nodesOk = bitEqual(a.radius, b.radius) && bitEqual(a.errorCm, b.errorCm) && bitEqual(a.skirtCm, b.skirtCm) && a.level == b.level;
            for (int k = 0; k < 3; ++k) worstC = std::fmax(worstC, std::fabs(a.centre[k] - b.centre[k]));
        }
        check(nodesOk, "translate keeps radius, error, skirt and level");
        check(worstC < 0.5f, "translate puts every node centre where a rebuild over the moved data does (worst " + std::to_string(worstC) + " cm)");
    }

#if AVER_LANDSCAPE_TEST_RENDERER
    // ---- (5) a mesh built before the rebase draws translated, and is what a rebuild would have made ----
    {
        struct RecordingDevice final : public rhi::IDevice {
            rhi::Backend backend() const override { return rhi::Backend::Null; }
            const char* adapterName() const override { return "recording device"; }
            rhi::IResourceFactory* resources() override { return nullptr; }
            rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
            void beginFrame() override {}
            void endFrame() override {}
            struct Draw { rhi::MeshHandle mesh; f32 world[16]; };
            std::vector<std::vector<rhi::MeshVertex>> meshes;   // handle - 1
            std::vector<Draw> draws;
            rhi::MeshHandle createMesh(const rhi::MeshVertex* v, u32 vcount, const u32*, u32 icount) override {
                if (vcount == 0 || icount == 0) return 0;
                meshes.emplace_back(v, v + vcount);
                return static_cast<rhi::MeshHandle>(meshes.size());
            }
            bool destroyMesh(rhi::MeshHandle) override { return true; }
            void drawMesh(rhi::MeshHandle m, const f32* world, const f32*, f32, f32) override {
                Draw d;
                d.mesh = m;
                std::memcpy(d.world, world, sizeof d.world);
                draws.push_back(d);
            }
        };

        const f32 identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const fmt::OcLandData dA = makeTerrain(N, 100.0f, -6400.0f, -6400.0f);
        fmt::OcLandData dB = dA;
        dB.originCm[0] -= static_cast<f32>(kStep);
        LandscapeTree treeA, treeB;
        std::string why;
        treeA.build(dA, 64, &why);
        treeB.build(dA, 64, &why);
        treeB.translate(-static_cast<f32>(kStep), 0.0f);

        SelectParams pa;
        pa.cameraCm[0] = 0.0f; pa.cameraCm[1] = 0.0f; pa.cameraCm[2] = 1500.0f;
        pa.screenErrorPx = 1.0f;
        pa.maxDraws = 64;
        SelectParams pb = pa;
        pb.cameraCm[0] -= static_cast<f32>(kStep);
        SelectResult selA, selB;
        treeA.select(pa, selA);
        treeB.select(pb, selB);
        check(!selA.nodes.empty() && selA.nodes == selB.nodes, "the same nodes are selected in either frame");

        RecordingDevice dev1;
        LandscapeRenderer r1(64);
        r1.draw(dev1, dA, treeA, selA, identity);
        const u32 builtBefore = r1.stats().created;
        check(builtBefore > 0 && r1.stats().translated == 0, "before the rebase: meshes are built and none is translated");

        // The rebase: the data and tree moved, the renderer is told the frame.
        r1.setOrigin(kStep, 0);
        dev1.draws.clear();
        r1.draw(dev1, dB, treeB, selB, identity);
        check(r1.stats().created == 0, "the rebase frame builds no mesh");
        check(r1.stats().submitted > 0 && r1.stats().translated == r1.stats().submitted, "every drawn mesh is translated");
        bool matrices = !dev1.draws.empty();
        for (const auto& d : dev1.draws) {
            matrices = matrices && d.world[12] == -static_cast<f32>(kStep) && d.world[13] == 0.0f && d.world[14] == 0.0f;
            for (int k = 0; k < 12; ++k) matrices = matrices && d.world[k] == identity[k];
        }
        check(matrices, "the translation is -(one region) in x on the caller's matrix, and nothing else");

        // A renderer born after the rebase builds the same nodes in the new frame; old + translation == new.
        RecordingDevice dev2;
        LandscapeRenderer r2(64);
        r2.setOrigin(kStep, 0);
        r2.draw(dev2, dB, treeB, selB, identity);
        check(r2.stats().translated == 0 && dev2.meshes.size() == dev1.meshes.size(), "a renderer made after the rebase builds in the new frame");
        bool geometry = dev2.meshes.size() == dev1.meshes.size();
        f32 worstPos = 0.0f, worstUv = 0.0f;
        bool exactRest = true;
        for (usize m = 0; geometry && m < dev1.meshes.size(); ++m) {
            geometry = dev1.meshes[m].size() == dev2.meshes[m].size();
            for (usize i = 0; geometry && i < dev1.meshes[m].size(); ++i) {
                const rhi::MeshVertex& o = dev1.meshes[m][i];
                const rhi::MeshVertex& n = dev2.meshes[m][i];
                worstPos = std::fmax(worstPos, std::fabs(o.px - static_cast<f32>(kStep) - n.px));
                worstPos = std::fmax(worstPos, std::fabs(o.py - n.py));
                exactRest = exactRest && bitEqual(o.pz, n.pz) && bitEqual(o.nx, n.nx) && bitEqual(o.ny, n.ny) && bitEqual(o.nz, n.nz);
                const f64 du = static_cast<f64>(o.u) - static_cast<f64>(n.u);
                worstUv = std::fmax(worstUv, static_cast<f32>(std::fabs(du - std::round(du))));
            }
        }
        check(geometry && worstPos < 0.5f, "a translated old mesh sits where the rebuilt one does (worst " + std::to_string(worstPos) + " cm)");
        check(exactRest, "heights and normals are untouched by the rebase");
        check(worstUv < 5e-3f, "texture coordinates agree to whole tiles (worst fraction " + std::to_string(worstUv) + ")");

        // The negative control: a renderer that was NOT told draws its old meshes where they were built.
        RecordingDevice dev3;
        LandscapeRenderer r3(64);
        r3.draw(dev3, dA, treeA, selA, identity);
        dev3.draws.clear();
        r3.draw(dev3, dB, treeB, selB, identity);
        check(r3.stats().translated == 0 && !dev3.draws.empty() && dev3.draws[0].world[12] == 0.0f,
              "negative control: an untold renderer draws the stale meshes untranslated (a 16 km error)");
    }
#endif

#ifdef AVER_LANDSCAPESHIFT_GAME
    // ---- (6) GameLandscape: absolute at the seams, origin-relative inside ----
    {
        using game::GameLandscape;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_landscape_shift_test";
        std::filesystem::create_directories(dir);
        const std::string path = (dir / "s.ocland").string();
        {
            fmt::OcLandData d;
            d.sampleCount = 65;
            d.spacingCm = 100.0f;
            d.originCm[0] = 1'000'000.0f;
            d.originCm[1] = 500.0f;
            d.heights.resize(65u * 65u);
            for (u32 y = 0; y < 65; ++y)
                for (u32 x = 0; x < 65; ++x) d.heights[y * 65 + x] = static_cast<f32>(x) * 20.0f;
            std::string why;
            check(fmt::saveOcLand(path, d, &why), "wrote the test section (" + why + ")");
        }
        GameLandscape land;
        check(land.loadSection(nullptr, path), "loaded the section at origin zero");
        f64 z0 = 0.0, z1 = 0.0;
        check(land.groundHeightAt(1'003'200.0, 3'200.0, z0), "groundHeightAt answers an absolute position");
        f32 n0 = 0.0f, n1 = 0.0f;
        check(land.scatterHeightAt(1'000'000.0f - 5000.0f, 1500.0f, n0), "scatterHeightAt answers past the rim (the noise)");

        rhi::ShiftWorldInfo ev;
        ev.deltaCm = rhi::ShiftVec3d{static_cast<f64>(kStep), 0.0, 0.0};
        ev.newOrigin = rhi::ShiftOrigin{kStep, 0};
        ev.generation = 1;
        land.onOriginShift(ev);
        check(land.data().originCm[0] == 1'000'000.0f - static_cast<f32>(kStep) && land.data().originCm[1] == 500.0f,
              "the section's origin followed the rebase (scene frame)");
        check(land.groundHeightAt(1'003'200.0, 3'200.0, z1) && std::fabs(z1 - z0) < 1e-3,
              "groundHeightAt still answers the same ABSOLUTE position with the same height");
        check(land.scatterHeightAt(1'000'000.0f - 5000.0f - static_cast<f32>(kStep), 1500.0f, n1) && std::fabs(n1 - n0) < 0.25f,
              "past the rim the same absolute hill is read from the new frame (" + std::to_string(n0) + " vs " + std::to_string(n1) + ")");
        f64 abs3[3] = {};
        land.homeOriginAbsCm(abs3);
        check(abs3[0] == 1'000'000.0 && abs3[1] == 500.0, "homeOriginAbsCm is exact and unmoved");

        const f32 originAfter = land.data().originCm[0];
        land.onOriginShift(ev);
        check(land.data().originCm[0] == originAfter, "applying the same event twice changes nothing (idempotent)");

        check(land.save(), "saved from the shifted frame");
        fmt::OcLandData back;
        std::string why;
        check(fmt::loadOcLand(path, back, &why) && back.originCm[0] == 1'000'000.0f && back.originCm[1] == 500.0f,
              "the sidecar still carries the ABSOLUTE origin (" + std::to_string(back.originCm[0]) + ")");

        land.setFrameOrigin(rhi::ShiftOrigin{0, 0});
        check(land.data().originCm[0] == 1'000'000.0f && land.groundHeightAt(1'003'200.0, 3'200.0, z1) && std::fabs(z1 - z0) < 1e-3,
              "back to origin zero: the original frame and heights, exactly");
        land.unload(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
#endif

    AVER_INFO("LandscapeShiftTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
