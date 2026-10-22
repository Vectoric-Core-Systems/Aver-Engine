// The Groom tab (sandbox/src/HairGroomTab.cpp, compiled here with no ImGui so only its model, load/save/undo/edit half exists), the pure groom
// editing rules of HairDetailsModel.hpp and the Show > Hair overlay feeds of HairDebugOverlay.hpp. No window, no device, no physics world.
// Exit code = failure count.
//
// NOTHING IN HERE HAS BEEN RUN: written against the headers.
#include "HairDebugOverlay.hpp"
#include "HairGroomTab.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcHair.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// A flat square scalp of n x n quads, size cm across, embedded in a groom (z = 0, +Z up), one joint.
static fmt::OcHairScalp flatScalp(int n, f32 size) {
    fmt::OcHairScalp s;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            s.positions.push_back((static_cast<f32>(i) / static_cast<f32>(n) - 0.5f) * size);
            s.positions.push_back((static_cast<f32>(j) / static_cast<f32>(n) - 0.5f) * size);
            s.positions.push_back(0.0f);
            s.joints.insert(s.joints.end(), {0, 0, 0, 0});
            s.weights.insert(s.weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
        }
    const int row = n + 1;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const u32 a = static_cast<u32>(j * row + i), b = a + 1, c = a + static_cast<u32>(row), d = c + 1;
            s.triangles.insert(s.triangles.end(), {a, c, b, b, c, d});
        }
    return s;
}

static fmt::OcHairData embeddedGroom() {
    fmt::OcHairData g = hairmodel::starterGroom();
    g.generate.strands = 200;
    g.generate.clumps = 10;
    g.hasScalp = true;
    g.scalp = flatScalp(8, 16.0f);
    g.scalpJoints = {"head"};
    return g;
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_hair_groom_editor_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    AVER_INFO("=== the model: starter groom, sanitize, budget ===");
    {
        fmt::OcHairData g = hairmodel::starterGroom();
        std::string why;
        check(g.valid(&why), "the starter groom is valid as it stands: " + why);
        check(g.materials.size() == 1 && g.looks.size() == 1, "it has a material and a look");

        fmt::OcHairData bad = g;
        bad.sim.iterationsPerSecond = 1.0e9f;
        bad.sim.simFraction = 0.0f;
        bad.sim.grid[1] = 1000.0f;
        bad.generate.strands = 0;
        bad.generate.segments = 1;
        bad.generate.lengthCm[0] = 50.0f;
        bad.generate.lengthCm[1] = 10.0f;
        bad.generate.styleWeight = 2.0f;
        bad.lod.distances[0] = 900.0f;
        bad.lod.distances[1] = 100.0f;
        bad.lod.strandFractions[1] = 3.0f;
        bad.materials[0].gravity.minFraction = bad.materials[0].gravity.maxFraction = 0.5f;
        bad.looks[0].widthRootCm = 0.0f;
        bad.looks[0].baseColor[0] = -1.0f;
        fmt::OcHairProxy p;
        p.name = "";
        p.shape = fmt::OcHairProxyShape::Box;
        p.sizeCm[0] = 0.0f;
        bad.proxies.push_back(p);
        hairmodel::sanitize(bad);
        check(bad.valid(&why), "sanitize turns a groom of out-of-range numbers into a valid one: " + why);
        const fmt::OcHairData again = bad;
        hairmodel::sanitize(bad);
        check(fmt::ocHairHeadText(bad) == fmt::ocHairHeadText(again), "sanitize is idempotent");

        fmt::OcHairData big = g;
        big.generate.strands = 20000;
        big.generate.segments = 12;
        const hairmodel::GroomBudget b = hairmodel::groomBudget(big);
        check(b.renderStrands == 20000 && b.renderVertices == 260000 && b.simStrands == 2000, "budget: strands, vertices and the 10% simulated");
        const double mb = static_cast<double>(b.drawBytes) / (1024.0 * 1024.0);
        check(mb > 30.0 && mb < 45.0 && !b.overBudget, "budget: a 20k-strand groom of 13 points is about 37 MB, under 96 (" + std::to_string(mb) + ")");
        big.generate.strands = 40000;
        big.generate.segments = 30;
        check(hairmodel::groomBudget(big).overBudget, "budget: 40k strands of 31 points is over the hair VRAM budget");
        big.maxRenderStrands = 100;
        big.generate.strands = 500;
        check(hairmodel::groomBudget(big).overStrandCap, "budget: more strands than MAXSTRANDS is flagged");
    }

    AVER_INFO("=== the model: advice, projection, content paths, LOD rows ===");
    {
        fmt::OcHairData g = hairmodel::starterGroom();
        const hairmodel::GroomBudget b = hairmodel::groomBudget(g);
        const auto lines = hairmodel::groomAdvice(g, b, false);
        bool saysScalp = false, saysJoints = false;
        for (const std::string& l : lines) {
            saysScalp = saysScalp || l.find("No scalp mesh") != std::string::npos;
            saysJoints = saysJoints || l.find("No scalp joints") != std::string::npos;
        }
        check(saysScalp && saysJoints, "advice: a fresh groom is told it needs a scalp and joints");

        const f32 p[3] = {1.0f, 2.0f, 3.0f};
        f32 u = 0, v = 0;
        hairmodel::projectPoint(hairmodel::PreviewView::Front, p, u, v);
        check(u == 1.0f && v == 3.0f, "projection: front is X right, Z up");
        hairmodel::projectPoint(hairmodel::PreviewView::Side, p, u, v);
        check(u == 2.0f && v == 3.0f, "projection: side is Y right, Z up");
        hairmodel::projectPoint(hairmodel::PreviewView::Top, p, u, v);
        check(u == 1.0f && v == 2.0f, "projection: top is X right, Y up");
        const f32 lo[3] = {-10, -10, 0}, hi[3] = {10, 10, 20};
        const hairmodel::PreviewFit fit = hairmodel::fitPreview(hairmodel::PreviewView::Front, lo, hi, 224.0f, 224.0f, 12.0f);
        f32 x = 0, y = 0;
        hairmodel::toPixels(fit, 0.0f, 10.0f, 224.0f, 224.0f, x, y);
        check(std::fabs(x - 112.0f) < 1e-3f && std::fabs(y - 112.0f) < 1e-3f, "fit: the bounds' centre lands in the middle of the box");
        hairmodel::toPixels(fit, 0.0f, 20.0f, 224.0f, 224.0f, x, y);
        check(y >= 12.0f - 1e-3f && y < 112.0f, "fit: the top of the bounds is inside the margin and above the centre");

        const std::filesystem::path deep = dir / "Content" / "Hair" / "Sub";
        std::filesystem::create_directories(deep, ec);
        const std::filesystem::path meshFile = dir / "Content" / "Meshes" / "Head.ocmesh";
        std::filesystem::create_directories(meshFile.parent_path(), ec);
        { FILE* f = std::fopen(meshFile.string().c_str(), "wb"); if (f) std::fclose(f); }
        const std::string found = hairmodel::resolveContentPath((deep / "G.ochair").string(), "Meshes/Head.ocmesh");
        check(!found.empty() && std::filesystem::equivalent(found, meshFile, ec), "content path: found by walking up from the groom's folder to Content");
        check(hairmodel::resolveContentPath((deep / "G.ochair").string(), "Meshes/Nope.ocmesh").empty(), "content path: a missing mesh is empty");
        check(hairmodel::resolveContentPath((deep / "G.ochair").string(), "").empty(), "content path: an empty name is empty");

        const auto rows = hairmodel::lodRows(g.lod, 1000);
        check(rows.size() == 4 && rows[0].strandsDrawn == 1000 && rows[1].fromCm == g.lod.distances[0] && rows[3].strandsDrawn == 120, "LOD rows: distances and strands drawn");
        const fmt::OcHairProxy np = hairmodel::newProxy({});
        const fmt::OcHairProxy np2 = hairmodel::newProxy({np});
        check(np.name == "proxy" && np2.name == "proxy1", "new proxy: names do not repeat");
    }

    AVER_INFO("=== the preview ===");
    {
        const std::string file = (dir / "Preview.ochair").string();
        std::string why;
        check(fmt::saveOcHair(file, embeddedGroom(), &why), "an embedded-scalp groom saves: " + why);
        HairGroomEditor ed(file);
        check(ed.loaded(), "and loads in the tab: " + ed.loadError());
        const hairmodel::GroomPreview& pv = ed.preview();
        check(pv.ok && pv.strandsShown > 0 && !pv.roots.empty(), "preview: roots and guide strands for a generated groom: " + pv.error);
        bool onScalp = pv.roots.size() >= 3;
        for (usize i = 2; i < pv.roots.size(); i += 3) onScalp = onScalp && std::fabs(pv.roots[i]) < 1.0e-3f;
        check(onScalp, "preview: every root is on the scalp (z = 0)");
        check(pv.segments.size() >= 6 && pv.scalpEdges.size() >= 6, "preview: segments and scalp edges");
        check(pv.boundsMax[2] > pv.boundsMin[2], "preview: the strands rise above the scalp, so the bounds have height");
        check(ed.advice().empty() || ed.advice().front().find("Not valid") == std::string::npos, "advice: nothing says the groom is invalid");
    }

    AVER_INFO("=== the tab: edits, undo, save ===");
    {
        const std::string file = (dir / "Edit.ochair").string();
        std::string why;
        fmt::saveOcHair(file, embeddedGroom(), &why);
        HairGroomEditor ed(file);
        check(ed.loaded() && !ed.dirty() && !ed.canUndo(), "opens clean with nothing to undo");
        check(ed.records().strands.empty() && !ed.records().hasScalp, "the strands and the embedded scalp are held beside the records");

        const u32 before = ed.records().generate.strands;
        ed.edit([](fmt::OcHairData& g) { g.generate.strands = 500; });
        check(ed.records().generate.strands == 500 && ed.dirty() && ed.canUndo(), "an edit changes the records, marks dirty and opens an undo step");
        ed.editInGesture([](fmt::OcHairData& g) { g.generate.strands = 600; });
        check(ed.records().generate.strands == 600, "an edit inside a gesture changes without a new step");
        ed.undo();
        check(ed.records().generate.strands == before && !ed.canUndo() && ed.canRedo(), "one undo undoes the whole gesture");
        ed.redo();
        check(ed.records().generate.strands == 600, "redo puts the whole gesture back (the step ends at its last value)");

        ed.edit([](fmt::OcHairData& g) { g.generate.lengthCm[0] = 90.0f; g.generate.lengthCm[1] = 5.0f; g.sim.grid[0] = 9999.0f; });
        check(ed.records().generate.lengthCm[1] >= ed.records().generate.lengthCm[0] && ed.records().sim.grid[0] <= 128.0f, "edits are clamped to what valid() accepts");

        check(ed.addMaterial() && ed.records().materials.size() == 2 && ed.records().materials.back().index == 1, "add material takes the next free index");
        check(ed.removeMaterial(1) && ed.records().materials.size() == 1, "remove material takes it out");
        check(!ed.removeMaterial(0), "the last material stays");
        check(ed.addLook() && ed.records().looks.size() == 2, "add look");
        check(ed.removeLook(1) && !ed.removeLook(0), "remove look, and the last stays");

        fmt::OcHairProxy p = hairmodel::newProxy(ed.records().proxies, "head");
        check(ed.addProxy(p) && ed.records().proxies.size() == 1, "add proxy");
        check(ed.removeProxy(0) && ed.records().proxies.empty() && !ed.removeProxy(0), "remove proxy, and an out-of-range index is refused");
        check(ed.useStarterProxies() && !ed.records().proxies.empty(), "the humanoid starter set fills the list");

        check(ed.save(&why), "save writes: " + why);
        check(!ed.dirty(), "saving clears dirty");
        fmt::OcHairData back;
        check(fmt::loadOcHair(file, back, &why), "the saved file loads: " + why);
        check(back.generate.strands == 600 && back.hasScalp && back.scalp.vertexCount() == 81 && back.scalpJoints.size() == 1, "and holds the edits, the embedded scalp and the joints");
        check(back.valid(&why), "and is valid: " + why);

        // A dirty tab keeps its edits when the file changes under it.
        ed.edit([](fmt::OcHairData& g) { g.generate.strands = 777; });
        ed.onFileChanged();
        check(ed.records().generate.strands == 777, "a dirty tab keeps its edits when the file changes");
    }

    AVER_INFO("=== the tab: stored strands ===");
    {
        fmt::OcHairData g = embeddedGroom();
        for (int s = 0; s < 5; ++s) {
            g.strands.start.push_back(static_cast<u32>(g.strands.points.size() / 3));
            for (int k = 0; k < 4; ++k) g.strands.points.insert(g.strands.points.end(), {static_cast<f32>(s), 0.0f, static_cast<f32>(k) * 2.0f});
            g.strands.material.push_back(0);
        }
        g.strands.start.push_back(static_cast<u32>(g.strands.points.size() / 3));
        const std::string file = (dir / "Stored.ochair").string();
        std::string why;
        check(fmt::saveOcHair(file, g, &why), "a groom with stored strands saves: " + why);
        HairGroomEditor ed(file);
        check(ed.loaded() && ed.storedStrandCount() == 5, "the tab reports the stored strands");
        check(ed.budget().renderStrands == 5 && ed.budget().renderVertices == 20, "the budget reads the stored strands, not GENERATE");
        const hairmodel::GroomPreview& pv = ed.preview();
        check(pv.ok && pv.strandsShown == 5, "the preview draws the stored strands");
        ed.clearStoredStrands();
        check(ed.storedStrandCount() == 0 && ed.dirty() && ed.records().generate.present, "dropping the stored strands turns GENERATE on and marks dirty");
        check(ed.save(&why), "and saves: " + why);
        fmt::OcHairData back;
        fmt::loadOcHair(file, back, nullptr);
        check(back.strands.empty() && back.hasScalp, "the saved groom has no strands and keeps its scalp");
        std::string importWhy;
        check(!ed.importCurves((dir / "NoSuchFile.gltf").string(), &importWhy) && !importWhy.empty(), "importing a missing file fails with a reason");
    }

    AVER_INFO("=== the factory and unreadable files ===");
    {
        check(makeHairGroomEditor((dir / "Nothing.txt").string()) == nullptr, "the factory takes only .ochair");
        const std::string junk = (dir / "Junk.ochair").string();
        { FILE* f = std::fopen(junk.c_str(), "wb"); if (f) { std::fputs("not an avr container", f); std::fclose(f); } }
        HairGroomEditor ed(junk);
        std::string why;
        check(!ed.loaded() && !ed.loadError().empty() && !ed.save(&why), "an unreadable file is reported and refuses to save");
        check(makeHairGroomEditor(junk) != nullptr, "but the factory still opens a tab to say so");
        const std::string fresh = (dir / "Fresh.ochair").string();
        check(writeStarterHairGroom(fresh, &why), "New Hair Groom writes the starter: " + why);
        check(!writeStarterHairGroom(fresh, &why), "and never overwrites an existing file");
        HairGroomEditor opened(fresh);
        check(opened.loaded() && opened.records().generate.present && !opened.dirty(), "the new asset opens in the tab, clean");
    }

    AVER_INFO("=== the overlay feeds ===");
    {
        const Mat4 id = Mat4::identity();
        HairDebugFrame f;
        const f32 sphere[3] = {10, 0, 0};
        hairDebugFeedProxy(fmt::OcHairProxyShape::Sphere, sphere, id, 1, 1, 1, f);
        check(f.lines.size() == 96, "a sphere is three 16-segment circles (96 vertices): " + std::to_string(f.lines.size()));
        f.clear();
        const f32 capsule[3] = {5, 6, 0};
        hairDebugFeedProxy(fmt::OcHairProxyShape::Capsule, capsule, id, 1, 1, 1, f);
        check(f.lines.size() == 104, "a capsule is two circles, two caps and four rails (104 vertices): " + std::to_string(f.lines.size()));
        f.clear();
        const f32 box[3] = {4, 5, 6};
        hairDebugFeedProxy(fmt::OcHairProxyShape::Box, box, id, 1, 1, 1, f);
        check(f.lines.size() == 24, "a box is twelve edges: " + std::to_string(f.lines.size()));
        f.clear();
        Mat4 moved = Mat4::identity();
        moved.m[3][0] = 100.0f;
        moved.m[3][1] = 200.0f;
        moved.m[3][2] = 300.0f;
        const std::vector<f32> seg = {0, 0, 0, 1, 2, 3};
        hairDebugFeedGuides(seg, moved, 1, 1, 1, f);
        check(f.lines.size() == 2 && f.lines[0].px == 100.0f && f.lines[1].px == 101.0f && f.lines[1].pz == 303.0f, "guides go through the owner's world matrix (row-vector convention)");
        HairDebugOptions o;
        check(!o.any(), "the overlay is off by default");
    }

    std::filesystem::remove_all(dir, ec);
    if (g_failures == 0) AVER_INFO("HairGroomEditorTest: {} checks passed", g_checks);
    else AVER_ERROR("HairGroomEditorTest: {} of {} checks FAILED", g_failures, g_checks);
    return g_failures;
}
