// PhysicalSurfacesTest -- the project's physical-surface table and the landscape's surfaces (docs/plans/wave11/SHAPES_MATERIALS.md).
//
//   1. Content/Physics/Surfaces.ocsurf loads into the physics registry; a missing file is silence, a damaged one a reason,
//   2. an authored number beats a landscape layer's find-or-add; a layer the table does not name is registered with its own numbers,
//   3. the landscape heightfield's cell palette is the dominant paint layer of each cell (against a brute-force argmax),
//   4. through the physics world: a ray on the painted part of a landscape reports its layer's surface, an unpainted section is
//      its base layer's surface everywhere, and a layer table with no surface names builds the plain heightfield as before.
//
// A temp project and a temp .ocland. No device and no window; the physics world is a real one.
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/formats/OcLand.hpp"
#include "aver/formats/OcSurf.hpp"
#include "aver/game/GameLandscape.hpp"
#include "aver/game/PhysicalSurfaces.hpp"
#include "aver/landscape/Paint.hpp"
#include "aver/landscape/PhysicsBridge.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_shape_builder_abi.h"
#  include "aver/physics/physics_surface_abi.h"
#endif

using namespace aver;
using namespace aver::game;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static GameLandscape::LayerInfo layer(const char* name, const char* surface, f32 friction, f32 restitution) {
    GameLandscape::LayerInfo l;
    l.name = name; l.surface = surface; l.friction = friction; l.restitution = restitution;
    return l;
}

// A deterministic stream.
static u32 lcg(u32& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

#if AVER_MODULE_PHYSICS
static bool surfaceNumbers(const char* name, f32& friction, f32& restitution, i32& tag, std::string& registeredName) {
    const i32 id = aver_phys_surface_id(name);
    u8 buf[64] = {};
    if (aver_phys_surface_info(id, &friction, &restitution, &tag, buf, static_cast<i32>(sizeof buf)) != 1) return false;
    registeredName = reinterpret_cast<const char*>(buf);
    return true;
}
#endif

static void testTable(const fs::path& root) {
    AVER_INFO("=== Surfaces.ocsurf -> the registry ===");
    const std::string content = (root / "Content").string();
    check(loadPhysicalSurfaces(content) == 0, "a project with no table loads nothing, quietly");
    std::string err;
    check(loadPhysicalSurfaces(content, &err) == 0 && err.empty(), "and reports no error for it");

    fmt::OcSurfData d;
    d.surfaces.push_back({"TableGravel", 0.7f, -1.0f, 2});
    d.surfaces.push_back({"TableTurf", 0.9f, 0.05f, 1});
    d.surfaces.push_back({"TableMud", 0.15f, -1.0f, 0});
    check(fmt::saveOcSurf(physicalSurfacesPath(content), d, &err), "wrote the table: " + err);
    check(physicalSurfacesPath(content).find("Physics") != std::string::npos, "the table lives under Content/Physics");
    const u32 n = loadPhysicalSurfaces(content, &err);
#if AVER_MODULE_PHYSICS
    check(n == 3, "three surfaces registered");
    f32 f = 0, r = 0; i32 tag = 0; std::string name;
    check(surfaceNumbers("TableGravel", f, r, tag, name) && name == "TableGravel" && near(f, 0.7f) && r < 0.0f && tag == 2,
          "TableGravel carries its friction and tag; its restitution is unset");
    check(surfaceNumbers("TableTurf", f, r, tag, name) && near(f, 0.9f) && near(r, 0.05f) && tag == 1, "TableTurf carries all three numbers");
#else
    check(n == 0, "without the physics module nothing is registered");
#endif

    { std::ofstream bad(physicalSurfacesPath(content), std::ios::binary | std::ios::trunc); bad << "OCSURF 1\nSURFACE X friction nope\n"; }
    err.clear();
    check(loadPhysicalSurfaces(content, &err) == 0 && !err.empty(), "a damaged table registers nothing and says why: " + err);
    check(fmt::saveOcSurf(physicalSurfacesPath(content), d, &err), "restored the table");
}

static void testFindOrAdd() {
    AVER_INFO("=== an authored table beats a layer's find-or-add ===");
#if AVER_MODULE_PHYSICS
    f32 f = 0, r = 0; i32 tag = 0; std::string name;
    check(!registerSurfaceIfAbsent("TableTurf", 0.1f, 0.0f), "a name the table holds is not overwritten by a layer");
    check(surfaceNumbers("TableTurf", f, r, tag, name) && near(f, 0.9f), "TableTurf still has the authored 0.9");
    check(registerSurfaceIfAbsent("LayerOnlyStone", 0.3f, 0.2f), "a name the table does not hold is registered by the layer");
    check(surfaceNumbers("LayerOnlyStone", f, r, tag, name) && near(f, 0.3f) && near(r, 0.2f), "with the layer's numbers");
    check(!registerSurfaceIfAbsent("LayerOnlyStone", 0.99f, 0.99f), "a second layer of the same name changes nothing");
    check(surfaceNumbers("LayerOnlyStone", f, r, tag, name) && near(f, 0.3f), "LayerOnlyStone is still 0.3");

    // A surface only interned by a shape (no name) is not "registered": a layer may name it.
    const i32 interned = aver_phys_surface_id("OnlyInterned");
    const i32 shape = aver_phys_shape_box(10, 10, 10, interned, nullptr, 0);
    check(shape != 0 && registerSurfaceIfAbsent("OnlyInterned", 0.4f, 0.0f), "a surface only interned by a shape can still be registered by name");
    check(!registerSurfaceIfAbsent("", 0.5f, 0.0f), "an empty name is refused");
#else
    check(!registerSurfaceIfAbsent("X", 0.1f, 0.0f), "without the physics module nothing registers");
#endif
}

// The bridge's cell palette against a brute-force argmax of the same weights.
static void testCellPalette() {
    AVER_INFO("=== the cell palette is the dominant paint layer ===");
    constexpr u32 n = 17;
    fmt::OcLandData d;
    d.sampleCount = n;
    d.spacingCm = 100.0f;
    d.originCm[0] = 500.0f; d.originCm[1] = -300.0f;
    d.heights.assign(static_cast<usize>(n) * n, 0.0f);
    d.layerNames = {"A", "B", "C"};
    const usize nn = static_cast<usize>(n) * n;
    d.weights.assign(2 * nn, 0);
    u32 seed = 12345;
    for (usize i = 0; i < nn; ++i) {
        const u32 w1 = lcg(seed) % 256;
        const u32 w2 = lcg(seed) % (256 - w1);
        d.weights[i] = static_cast<u8>(w1);
        d.weights[nn + i] = static_cast<u8>(w2);
    }
    check(d.layersValid(), "a three-layer section with random weights is valid");

    landscape::PhysicsHeightfield hf;
    const std::vector<i32> surfaces = {1001, 1002, 1003};
    check(landscape::toPhysicsHeightfield(d, hf, &surfaces), "toPhysicsHeightfield accepts the layers' surfaces");
    check(hf.palette == surfaces && hf.cellPalette.size() == static_cast<usize>(n - 1) * (n - 1), "a palette of the three surfaces and one entry per cell");

    int compared = 0, mismatches = 0, viaWeightsAt = 0, weightsAtMismatches = 0;
    for (u32 cy = 0; cy + 1 < n; ++cy)
        for (u32 cx = 0; cx + 1 < n; ++cx) {
            const u32 ix0 = n - 2 - cy, iy0 = cx;   // physics row cy+(0,1) is section column n-2-cy+(0,1); physics column cx+(0,1) is section row cx+(0,1)
            int sum[3] = {};
            for (const u32 ix : {ix0, ix0 + 1})
                for (const u32 iy : {iy0, iy0 + 1})
                    for (u32 s = 0; s < 3; ++s) sum[s] += d.weightAt(s, ix, iy);
            const bool tie = sum[0] == sum[1] || sum[0] == sum[2] || sum[1] == sum[2];
            int best = 0;
            for (int s = 1; s < 3; ++s) if (sum[s] > sum[best]) best = s;
            // A tie goes to the lower layer in both; the float comparison below skips ties because it cannot settle them.
            ++compared;
            if (hf.palette[hf.cellPalette[static_cast<usize>(cy) * (n - 1) + cx]] != surfaces[static_cast<usize>(best)]) ++mismatches;
            if (!tie) {
                f32 w[8] = {};
                landscape::layerWeightsAt(d, d.originCm[0] + (static_cast<f32>(ix0) + 0.5f) * d.spacingCm, d.originCm[1] + (static_cast<f32>(iy0) + 0.5f) * d.spacingCm, w);
                ++viaWeightsAt;
                if (landscape::dominantLayer(w, 3) != static_cast<u32>(best)) ++weightsAtMismatches;
            }
        }
    check(compared > 200 && mismatches == 0, "every cell's palette entry is the argmax of its four corners' weights (" + std::to_string(compared) + " cells)");
    check(viaWeightsAt > 200 && weightsAtMismatches == 0, "and landscape::dominantLayer over layerWeightsAt at the cell middle agrees (" + std::to_string(viaWeightsAt) + " cells)");

    // Layers that share a surface share a palette entry.
    landscape::PhysicsHeightfield shared;
    const std::vector<i32> twoSame = {7, 7, 9};
    landscape::toPhysicsHeightfield(d, shared, &twoSame);
    check(shared.palette.size() == 2 && shared.palette[0] == 7 && shared.palette[1] == 9, "two layers of one surface are one palette entry");
    // No surface for any layer, no pointer, an empty vector: the plain heightfield of before.
    landscape::PhysicsHeightfield plain;
    const std::vector<i32> zeros = {0, 0, 0};
    landscape::toPhysicsHeightfield(d, plain, &zeros);
    check(plain.cellPalette.empty() && plain.palette.empty(), "all-zero surfaces: no palette");
    landscape::toPhysicsHeightfield(d, plain, nullptr);
    check(plain.cellPalette.empty() && plain.samples.size() == nn, "no surfaces given: no palette, samples as before");
    // An unpainted section is its base layer's surface everywhere.
    fmt::OcLandData flat;
    flat.sampleCount = 5; flat.spacingCm = 100.0f; flat.heights.assign(25, 0.0f);
    landscape::PhysicsHeightfield unpainted;
    const std::vector<i32> baseOnly = {42};
    landscape::toPhysicsHeightfield(flat, unpainted, &baseOnly);
    bool allBase = unpainted.palette == std::vector<i32>{42} && unpainted.cellPalette.size() == 16;
    for (const u8 c : unpainted.cellPalette) allBase = allBase && c == 0;
    check(allBase, "an unpainted section is its base layer's surface in every cell");
    // Too few surfaces for the layers: ignored rather than read past the end.
    landscape::PhysicsHeightfield shortList;
    const std::vector<i32> tooFew = {5};
    landscape::toPhysicsHeightfield(d, shortList, &tooFew);
    check(shortList.cellPalette.empty(), "fewer surfaces than layers builds no palette");
}

#if AVER_MODULE_PHYSICS
static i32 surfaceUnder(f32 x, f32 y) {
    f32 f[AVER_PHYS_HITF_COUNT] = {};
    i32 iv[AVER_PHYS_HITI_COUNT] = {};
    const i32 body = aver_phys_raycast_detail(x, y, 5000.0f, 0, 0, -1, 20000.0f, 0xFFFFFFFFu, 0, 0, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT);
    return body ? iv[AVER_PHYS_HITI_SURFACE] : -1;
}

static void testLandscapePhysics(const fs::path& root) {
    AVER_INFO("=== the landscape heightfield carries its layers' surfaces ===");
    const std::string path = (root / "land.ocland").string();
    {
        // 65 x 65, 100 cm apart, origin (0,0): a plane rising 20 cm per sample along +X.
        fmt::OcLandData d;
        d.sampleCount = 65; d.spacingCm = 100.0f;
        d.heights.resize(65u * 65u);
        for (u32 y = 0; y < 65; ++y)
            for (u32 x = 0; x < 65; ++x) d.heights[y * 65 + x] = static_cast<f32>(x) * 20.0f;
        std::string why;
        check(fmt::saveOcLand(path, d, &why), "wrote the test section (" + why + ")");
    }
    const i32 turf = aver::surfaceIdFromName("TableTurf"), stone = aver::surfaceIdFromName("LayerOnlyStone");

    GameLandscape land;
    check(land.loadSection(nullptr, path), "loaded the section");
    const std::vector<GameLandscape::LayerInfo> table = {layer("Grass", "TableTurf", 0.1f, 0.0f), layer("Rock", "LayerOnlyStone", 0.3f, 0.2f)};
    land.setLayerTable(table);
    f32 f = 0, r = 0; i32 tag = 0; std::string name;
    check(surfaceNumbers("TableTurf", f, r, tag, name) && near(f, 0.9f), "setLayerTable did not overwrite the authored TableTurf");

    // Unpainted: the base layer everywhere.
    land.rebuildCollisionIfStale();
    check(surfaceUnder(1600.0f, 1600.0f) == turf && surfaceUnder(5000.0f, 300.0f) == turf, "an unpainted section reports its base layer's surface under any ray");

    // Paint Rock around (3200, 3200). Loading the section again drops the layer table and marks the collision stale, so the
    // rebuild below sees the paint.
    check(land.loadSection(nullptr, path), "reloaded the section");
    land.setLayerTable(table);
    u32 idx = 0;
    check(land.ensureLayer("Rock", idx) && idx == 1, "Rock is section layer 1");
    landscape::PaintParams pp;
    pp.centerCm[0] = 3200.0f; pp.centerCm[1] = 3200.0f; pp.radiusCm = 800.0f; pp.layer = idx;
    land.beginWeightStroke();
    land.paint(nullptr, pp, 1.0f);
    std::vector<u8> before, after;
    u32 x0, y0, x1, y1, planes;
    check(land.endWeightStroke(before, after, x0, y0, x1, y1, planes), "the paint stroke changed the weights");
    land.rebuildCollisionIfStale();
    check(surfaceUnder(3200.0f, 3200.0f) == stone, "a ray at the middle of the painted area reports Rock's surface");
    check(surfaceUnder(400.0f, 400.0f) == turf && surfaceUnder(5800.0f, 600.0f) == turf, "rays far from the paint report the base layer's");

    land.unload(nullptr);
}
#endif

int main() {
    const fs::path root = fs::temp_directory_path() / "aver_physical_surfaces_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content", ec);

#if AVER_MODULE_PHYSICS
    aver_phys_init();   // shapes need a world, and the landscape builds its collision in it
#endif
    testTable(root);
    testFindOrAdd();
    testCellPalette();
#if AVER_MODULE_PHYSICS
    testLandscapePhysics(root);
    aver_phys_shutdown();
#endif

    fs::remove_all(root, ec);
    AVER_INFO("=== {} ===", g_failures == 0 ? "ok" : "FAILED");
    return g_failures;
}
