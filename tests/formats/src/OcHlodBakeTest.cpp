// OcHlodBakeTest -- the per-cell HLOD bake: proxies recorded in the .ocstream with a content hash, only changed cells rebuilt, layer and
// dynamic-placement exclusions, cancellation, the triangle-budget heuristic, and that a bounds save keeps the HLOD records. CPU + disk.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcHlodBake.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcStream.hpp"
#include "aver/formats/OcWorld.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static fmt::OcMeshData cube() {
    fmt::OcMeshData m;
    const f32 c[8][3] = {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}, {0, 0, 100}, {100, 0, 100}, {100, 100, 100}, {0, 100, 100}};
    for (const auto& v : c) {
        m.positions.insert(m.positions.end(), {v[0], v[1], v[2]});
        m.normals.insert(m.normals.end(), {0, 0, 1});
        m.uvs.insert(m.uvs.end(), {0, 0});
    }
    m.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    m.materialSlots = {"stone"};
    fmt::OcMeshSubmesh sm; sm.name = "stone"; sm.indexCount = 36; sm.vertexCount = 8;
    m.submeshes = {sm};
    m.computeBounds();
    return m;
}

static fmt::OcWorldPlacement place(f64 x, f64 y, u64 id) {
    fmt::OcWorldPlacement p;
    p.asset = "Meshes/cube.ocmesh";
    p.x = x; p.y = y; p.z = 0;
    p.placementId = id;
    return p;
}

int main() {
    AVER_INFO("=== OcHlodBakeTest ===");
    const fs::path root = fs::temp_directory_path() / "aver_hlod_bake_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path content = root / "Content", project = root / "Project";
    fs::create_directories(content / "Meshes");
    fs::create_directories(project);
    std::string err;
    check(fmt::saveOcMesh((content / "Meshes" / "cube.ocmesh").string(), cube(), &err), "fixture mesh saved: " + err);

    // Cells of 1000 cm: A = (0,0) with three cubes, B = (3,0) with one, C = (0,2) with a layered one.
    fmt::OcWorldData w;
    w.name = "L";
    w.stream.enabled = true; w.stream.cellCm = 1000; w.stream.loadCm = 2000; w.stream.evictCm = 3000;
    w.dataLayers = {{"Always", fmt::OcDataLayerState::Activated, false, ""}, {"Gameplay", fmt::OcDataLayerState::Activated, true, ""},
                    {"Night", fmt::OcDataLayerState::Unloaded, false, ""}};
    w.placements = {place(100, 100, 1), place(400, 100, 2), place(700, 100, 3), place(3100, 100, 4), place(100, 2100, 5)};
    w.placements[4].layers = {"Gameplay"};
    w.placements[1].layers = {"Always"};

    fmt::OcStreamBakeOptions so;
    fmt::OcStreamBakeReport sr;
    check(fmt::bakeOcStream(w, "L", content.string(), project.string(), so, sr, &err), "stream data baked: " + err);

    {   // no streaming data yet -> refuses
        fmt::OcWorldData bare;
        fmt::OcHlodReport r;
        check(!fmt::bakeHlod(bare, "X", content.string(), project.string(), {}, r, &err), "a level without streaming data cannot bake HLOD");
    }

    fmt::OcHlodOptions opt;
    opt.hlodCm = 9000;
    fmt::OcHlodReport rep;
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), opt, rep, &err), "bake runs: " + err);
    check(rep.cells == 2 && rep.built == 2 && rep.fresh == 0, "two cells built (the Gameplay-layer placement's cell is not a cell)");
    check(rep.excludedByLayer == 1 && rep.placementsMerged == 4, "runtime=1 layer placement excluded from HLOD; always-on layer kept");
    check(w.stream.hlodCm == 9000.0f, "STREAM hlod= recorded on the level");

    fmt::OcStreamData sd;
    check(fmt::loadOcStream((project / w.stream.dataPath).string(), sd, &err) && sd.hlod.size() == 2, "two HLOD records in the .ocstream");
    bool filesOk = true, localOk = true;
    for (const fmt::OcStreamHlod& h : sd.hlod) {
        fmt::OcMeshData pm;
        filesOk = filesOk && fmt::loadOcMesh((project / h.proxy).string(), pm) && !pm.indices.empty() && h.level == 1 && h.sourceHash != 0;
        // vertices are relative to the cell's minimum corner
        localOk = localOk && pm.boundsMin.x >= -1.0f && pm.boundsMax.x <= 1000.0f + 1.0f && pm.boundsMax.y <= 1001.0f;
    }
    check(filesOk, "each proxy loads, level 1, hashed");
    check(localOk, "proxy vertices are cell-local");
    const std::vector<fmt::OcStreamHlod> first = sd.hlod;

    // second run: everything fresh
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), opt, rep, &err) && rep.fresh == 2 && rep.built == 0, "unchanged level: both cells fresh, nothing rebuilt");
    fmt::loadOcStream((project / w.stream.dataPath).string(), sd);
    check(sd.hlod == first, "records unchanged");

    // move a cube inside cell A: only A rebuilds
    w.placements[0].x += 50;
    if (w.placements[0].hasBounds) { w.placements[0].boundsMin[0] += 50; w.placements[0].boundsMax[0] += 50; }
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), opt, rep, &err) && rep.built == 1 && rep.fresh == 1, "moving one placement rebuilds only its cell");
    fmt::loadOcStream((project / w.stream.dataPath).string(), sd);
    usize changed = 0;
    for (const fmt::OcStreamHlod& h : sd.hlod)
        for (const fmt::OcStreamHlod& o : first)
            if (h.cx == o.cx && h.cy == o.cy && h.sourceHash != o.sourceHash) ++changed;
    check(changed == 1, "exactly one cell hash changed");

    // a bounds save (editor) keeps the proxies
    check(fmt::writeOcStreamFor(w, "L", project.string(), sd.foliage, &err), "stream data rewritten from bounds: " + err);
    fmt::OcStreamData after;
    fmt::loadOcStream((project / w.stream.dataPath).string(), after);
    check(after.hlod == sd.hlod, "writeOcStreamFor keeps the HLOD records");

    // force
    fmt::OcHlodOptions forced = opt; forced.force = true;
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), forced, rep, &err) && rep.built == 2, "force rebuilds every cell");

    // a changed parameter changes the hash
    fmt::OcHlodOptions other = opt; other.maxTriangles = 100;
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), other, rep, &err) && rep.built == 2, "a different budget invalidates the cached proxies");

    // layer-excluded and dynamic placements; stale proxy removal
    w.placements[3].layers = {"Night"};   // B's only placement: Night starts unloaded
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), other, rep, &err) && rep.removed == 1 && rep.cells == 1, "a cell with no eligible content loses its proxy");
    fmt::loadOcStream((project / w.stream.dataPath).string(), after);
    check(after.hlod.size() == 1, "and its record");
    size_t files = 0;
    for (const auto& e : fs::directory_iterator(project / "Binaries" / "Streaming" / "L" / "hlod")) { (void)e; ++files; }
    check(files == 1, "and its file");
    w.placements[3].layers.clear();
    w.placements[2].className = "Door";
    w.placements[1].visible = false;
    check(fmt::bakeHlod(w, "L", content.string(), project.string(), other, rep, &err) && rep.placementsMerged >= 2, "class and hidden placements are not merged");
    check(rep.placementsExcluded >= 3, "excluded: class, hidden, layer");
    w.placements[2].className.clear(); w.placements[1].visible = true;

    {   // simplify callback gets the budget; merge-only otherwise
        fmt::OcHlodOptions s2 = opt;
        s2.minTriangles = 1; s2.maxTriangles = 6; s2.force = true;
        u32 seenBudget = 0;
        s2.simplify = [&](fmt::OcMeshData& m, u32 budget) {
            seenBudget = budget;
            m.indices.resize(budget * 3);
            m.submeshes.resize(1);
            m.submeshes[0].indexCount = budget * 3;
            return true;
        };
        check(fmt::bakeHlod(w, "L", content.string(), project.string(), s2, rep, &err) && seenBudget > 0 && seenBudget <= 6, "the simplifier is asked for the clamped budget");
        fmt::loadOcStream((project / w.stream.dataPath).string(), after);
        fmt::OcMeshData pm;
        bool small = true;
        for (const fmt::OcStreamHlod& h : after.hlod) small = small && fmt::loadOcMesh((project / h.proxy).string(), pm) && pm.indices.size() / 3 <= 6;
        check(small, "proxies on disk respect the budget");
    }
    {   // cancel
        fmt::OcHlodOptions c = opt; c.force = true;
        int calls = 0;
        c.cancel = [&] { return ++calls > 1; };
        check(fmt::bakeHlod(w, "L", content.string(), project.string(), c, rep, &err) && rep.cancelled && rep.built == 1, "cancel stops between cells with the finished one written");
        fmt::loadOcStream((project / w.stream.dataPath).string(), after);
        check(after.hlod.size() == 2, "and the unfinished cell keeps its previous record");
    }

    {   // pure helpers
        fmt::OcHlodOptions o;
        check(fmt::hlodTargetTriangles(100000, 6400, 90000, o) >= o.minTriangles && fmt::hlodTargetTriangles(100000, 6400, 90000, o) <= o.maxTriangles, "target within the clamp");
        check(fmt::hlodTargetTriangles(100000, 6400, 20000, o) >= fmt::hlodTargetTriangles(100000, 6400, 90000, o), "nearer proxy distance is worth more triangles");
        std::vector<fmt::OcDataLayer> d = {{"A", fmt::OcDataLayerState::Activated, false, ""}, {"B", fmt::OcDataLayerState::Activated, false, "C"},
                                           {"C", fmt::OcDataLayerState::Loaded, false, ""}};
        check(fmt::hlodLayersAllowed({}, d) && fmt::hlodLayersAllowed({"A", "ghost"}, d), "no layer, or only always-on or undefined layers: allowed");
        check(!fmt::hlodLayersAllowed({"B"}, d) && !fmt::hlodLayersAllowed({"C"}, d), "a layer under a not-activated parent, or itself not activated: excluded");
    }

    fs::remove_all(root, ec);
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
