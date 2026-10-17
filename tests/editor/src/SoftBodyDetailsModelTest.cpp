// The Cloth and Destructible Details sections without a window: the preset rows and what picking one writes,
// the canonical COMPONENT record (keys at their default are omitted), the attach rules, the numbers the sliders
// show, "Create .ocsoft" bookkeeping, and the paint tool (brush selection in SIM space, strokes and undo, rule
// counts, save and reload, the viewport ray). Header-only model plus SoftBodyPaintTool.cpp compiled directly with
// AVER_WITH_IMGUI undefined; the widgets themselves are ImGui and covered by the owner's headed run.
#include "PlacedComponentModel.hpp"
#include "SoftBodyDetailsModel.hpp"
#include "SoftBodyPaintTool.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;
using namespace aver::editor;
namespace sm = aver::editor::softmodel;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Two 10x10 quads side by side in XZ, UV-seamed: render vertices 1/4 and 2/7 are doubled, so 8 weld to 6.
static fmt::OcMeshData seamMesh() {
    fmt::OcMeshData m;
    const f32 pos[24] = {0, 0, 0, 10, 0, 0, 10, 0, 10, 0, 0, 10, 10, 0, 0, 20, 0, 0, 20, 0, 10, 10, 0, 10};
    const f32 uv[16] = {0, 0, 0.5f, 0, 0.5f, 1, 0, 1, 0.5f, 0, 1, 0, 1, 1, 0.5f, 1};
    m.positions.assign(pos, pos + 24);
    m.uvs.assign(uv, uv + 16);
    for (u32 i = 0; i < 8; ++i) { m.normals.push_back(0); m.normals.push_back(-1); m.normals.push_back(0); }
    m.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    m.computeBounds();
    return m;
}

static const PlacedKind& kind(const char* type) { return *findPlacedKind(type); }

int main() {
    AVER_INFO("SoftBodyDetailsModelTest");

    AVER_INFO("the preset rows and what picking one writes");
    {
        check(sm::kPresetRows.size() == 5, "five rows: cloth light and heavy, metal, wood, glass-like");
        check(std::string(sm::presetRow(softbody::SoftPreset::Glass).key) == "glass" && std::string(sm::presetRow(softbody::SoftPreset::Metal).label) == "Metal",
              "keys and labels");
        fmt::OcPlacedComponent r = makeNewRecord(kind("Destructible"));
        check(r.type == "Destructible" && r.find("preset") && *r.find("preset") == "metal", "a new Destructible record seeds preset=metal");
        check(makeNewRecord(kind("Cloth")).find("preset") && *makeNewRecord(kind("Cloth")).find("preset") == "cloth_light", "and a Cloth record preset=cloth_light");
        r = sm::applyPreset(r, softbody::SoftPreset::Glass);
        check(*r.find("preset") == "glass" && r.kv.size() == 1, "picking a preset replaces the key, never adds a second");
        softbody::SoftPreset p;
        check(sm::presetOf(r, softbody::SoftPreset::Metal, p) && p == softbody::SoftPreset::Glass, "and it reads back");
        check(sm::presetOf(fmt::OcPlacedComponent{}, softbody::SoftPreset::ClothLight, p) && p == softbody::SoftPreset::ClothLight, "an absent key is the type default");
        sm::setKey(r, "preset", "rubber");
        check(!sm::presetOf(r, softbody::SoftPreset::Metal, p), "an unknown key is reported");
    }

    AVER_INFO("the canonical record");
    {
        fmt::OcPlacedComponent r;
        r.type = "Cloth";
        r.kv = {{"asset", "Cloth/cape.ocsoft"}, {"preset", "cloth_heavy"}, {"wind", "1"}, {"stiffness", "2"}, {"reach", "1.0"}, {"freeze", "0"}, {"tearable", "0"}, {"world", "1"}};
        sm::canonicalize(r);
        check(!r.find("wind") && !r.find("reach") && !r.find("freeze") && !r.find("tearable") && !r.find("world"), "keys at their default are dropped");
        check(r.find("stiffness") && *r.find("stiffness") == "2" && r.find("asset") && r.find("preset"), "authored values, the preset and the asset stay");
        fmt::OcPlacedComponent again = r;
        sm::canonicalize(again);
        check(again.kv == r.kv, "canonicalising twice changes nothing");
        fmt::OcPlacedComponent d;
        d.type = "Destructible";
        d.kv = {{"toughness", "1"}, {"impact", "3"}, {"lifetime", "0"}, {"maxpieces", "8"}, {"indestructible", "0"}, {"nodebris", "1"}};
        sm::canonicalize(d);
        check(!d.find("toughness") && !d.find("lifetime") && !d.find("indestructible"), "the same for a Destructible");
        check(d.find("impact") && d.find("maxpieces") && d.find("nodebris"), "and its authored keys stay");
    }

    AVER_INFO("the attach rules");
    {
        AttachContext c;
        c.levelOwned = true;
        c.hasMesh = true;
        std::string why;
        check(canAttach(c, kind("Cloth"), &why) && canAttach(c, kind("Destructible"), &why), "a placed mesh may take either");
        c.hasMesh = false;
        check(!canAttach(c, kind("Cloth"), &why) && why.find("mesh") != std::string::npos, "a Cloth needs a mesh");
        c.hasMesh = true;
        c.hasDestructible = true;
        check(!canAttach(c, kind("Cloth"), &why) && why.find("Destructible") != std::string::npos, "a Cloth is refused on a Destructible");
        c.hasDestructible = false;
        c.hasCloth = true;
        check(!canAttach(c, kind("Destructible"), &why) && why.find("Cloth") != std::string::npos, "and the other way round");
        check(placedAssetExts(kind("Cloth")).size() == 1 && std::string(placedAssetExts(kind("Cloth"))[0]) == ".ocsoft", "both pick .ocsoft assets");
        check(kPlacedKindCount >= 10 && std::string(kPlacedKinds[kPlacedKindCount - 2].type) == "Cloth" && std::string(kPlacedKinds[kPlacedKindCount - 1].type) == "Destructible",
              "the two kinds follow Smart Object");
    }

    AVER_INFO("the numbers the sliders show, and Create .ocsoft");
    {
        const sm::MaterialView metal = sm::viewOfPreset(true, softbody::SoftPreset::Metal);
        check(std::fabs(metal.yieldStrain - 0.010f) < 1e-6f && std::fabs(metal.areaDensityKgM2 - 7.8f) < 1e-4f && metal.iterations == 8, "metal reads 0.010 yield, 7.8 kg/m2, 8 iterations");
        const sm::MaterialView cloth = sm::viewOfPreset(false, softbody::SoftPreset::ClothLight);
        check(cloth.compliance == 0.0f && std::fabs(cloth.dragCoeff - 1.0f) < 1e-6f, "cloth light is inextensible with drag 1");
        check(sm::assetPathForMesh("Meshes/Door.ocmesh") == "Meshes/Door.ocsoft" && sm::assetPathForMesh("Cape.ocmesh") == "Cape.ocsoft" && sm::assetPathForMesh("").empty(),
              "the asset sits beside its mesh and shares its stem");
        fmt::OcPlacedComponent r = makeNewRecord(kind("Cloth"));
        r = sm::assignAsset(r, "Meshes/Cape.ocsoft");
        check(r.find("asset") && *r.find("asset") == "Meshes/Cape.ocsoft", "assigning writes the asset key");
        r = sm::assignAsset(r, "");
        check(!r.find("asset"), "and clearing removes it");
        check(sm::assetProblem(5, "", true).find("Content") != std::string::npos && sm::assetProblem(5, "a.ocsoft", false).find("load") != std::string::npos &&
              sm::assetProblem(0, "", false).empty() && sm::assetProblem(5, "a.ocsoft", true).empty(), "the red line names the actual problem");
        const softbody::SoftAsset starter = sm::starterAsset(true, softbody::SoftPreset::Glass);
        check(starter.kind == softbody::SoftKind::Destructible && starter.preset == softbody::SoftPreset::Glass && starter.valid(), "a starter asset is valid");
    }

    AVER_INFO("the paint tool");
    {
        SoftBodyPaintTool tool;
        const fmt::OcMeshData mesh = seamMesh();
        const softbody::SoftAsset base = softbody::makeSoftAsset(softbody::SoftKind::Cloth, softbody::SoftPreset::ClothLight);
        check(tool.load(mesh, base, "") && tool.active(), "a mesh loads");
        check(tool.particleCount() == 6, "8 render vertices weld to 6 sim particles");
        check(tool.pinnedCount() == 0 && tool.reachCount() == 0 && !tool.dirty() && !tool.stale(), "nothing is painted yet");

        // Brush selection is in sim space: a seam is one particle.
        check(tool.select(Vec3{10, 0, 5}, 6.0f).size() == 2, "a brush on the seam selects the two welded particles, not four vertices");
        check(tool.select(Vec3{10, 0, 5}, 100.0f).size() == 6, "a big brush selects every particle once");
        check(tool.select(Vec3{500, 0, 500}, 5.0f).empty(), "and a brush in the air selects none");

        // A stroke is one undo entry.
        tool.brush.mode = SoftBodyPaintTool::Mode::Pin;
        tool.beginStroke();
        tool.paint(tool.select(Vec3{10, 0, 5}, 6.0f));
        tool.paint(tool.select(Vec3{0, 0, 5}, 6.0f));
        tool.endStroke();
        check(tool.pinnedCount() == 4 && tool.dirty(), "two daubs pin four particles");
        check(tool.undo() && tool.pinnedCount() == 0, "one undo takes the whole stroke back");
        check(tool.redo() && tool.pinnedCount() == 4, "and redo puts it back");
        check(!tool.redo(), "nothing more to redo");

        tool.brush.mode = SoftBodyPaintTool::Mode::Reach;
        tool.brush.reach01 = 0.5f;
        tool.paint(tool.select(Vec3{20, 0, 5}, 6.0f));
        check(tool.reachCount() == 2 && tool.reachByte(5) == 127, "reach paints a byte (0.5 -> 127) and unpins what it covers");
        tool.brush.mode = SoftBodyPaintTool::Mode::Erase;
        tool.paint(tool.select(Vec3{500, 0, 500}, 5.0f));
        tool.paint(tool.select(Vec3{0, 0, 5}, 6.0f));
        check(tool.pinnedCount() == 2, "erase clears pins");
        Vec3 hit;
        check(tool.pick(Vec3{9, -50, 5}, Vec3{0, 1, 0}, hit) && std::fabs(hit.x - 9.0f) < 1e-3f && std::fabs(hit.y) < 1e-3f, "a ray finds the sheet");

        // Rules: the live counts.
        softbody::PaintRule above;
        above.type = softbody::PaintRule::Type::Above;
        above.axis = 0;
        above.valueCm = 15.0f;
        softbody::PaintRule sphere;
        sphere.type = softbody::PaintRule::Type::Sphere;
        sphere.centreCm[0] = 0.0f; sphere.centreCm[2] = 5.0f; sphere.radiusCm = 8.0f;
        softbody::PaintRule mask;
        mask.type = softbody::PaintRule::Type::Mask;
        mask.texture = "none";
        check(tool.ruleCount(above) == 2, "x >= 15 pins the two particles on the far edge");
        check(tool.ruleCount(sphere) == 2, "a sphere at the near edge pins two");
        check(tool.ruleCount(mask) == 0, "a mask with no texture provider pins none");
        tool.setProviders([](const std::string&, f32 u, f32, f32& r, f32& g) { r = u < 0.5f ? 0.1f : 0.9f; g = 1.0f; return true; }, {});
        check(tool.ruleCount(mask) == 2, "with a sampler it pins the two particles whose UV reads dark");
        tool.addRule(above);
        check(tool.rules().size() == 1 && tool.undo() && tool.rules().empty(), "adding a rule is one undo entry");
        tool.redo();
        tool.removeRule(0);
        check(tool.rules().empty(), "and so is removing it");

        // Saving and reloading.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_softbody_paint_test";
        std::filesystem::create_directories(dir);
        const std::string file = (dir / "banner.ocsoft").string();
        std::string why;
        check(!tool.save(&why), "no path yet: saving is refused with a reason");
        tool.setAssetPath(file);
        check(tool.dirty() && tool.save(&why) && !tool.dirty(), "with a path it saves and is clean");
        softbody::SoftAsset back;
        check(softbody::loadSoftAsset(file, back, &why), "the file loads");
        check(back.paint.pin.size() == 6 && back.paint.meshKey != 0 && !back.paint.stale(back.paint.meshKey), "pins and the mesh key are in it");
        SoftBodyPaintTool again;
        check(again.load(mesh, back, file) && !again.stale() && again.pinnedCount() == tool.pinnedCount(), "reloaded: same pins, not stale");
        fmt::OcMeshData moved = mesh;
        moved.positions[8] += 3.0f;
        SoftBodyPaintTool other;
        check(other.load(moved, back, file) && other.stale() && other.pinnedCount() == 0, "a moved vertex makes the paint stale and ignores it");
        other.brush.mode = SoftBodyPaintTool::Mode::Pin;
        other.paint(other.select(Vec3{0, 0, 0}, 3.0f));
        check(!other.stale(), "painting again makes it current");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);

        // The viewport feed.
        SoftBodyPaintTool view;
        check(view.load(mesh, base, ""), "loads again for the viewport");
        view.entityWorld = Mat4::translation(Vec3{100, 0, 0});
        view.brush.mode = SoftBodyPaintTool::Mode::Pin;
        view.brush.radiusCm = 8.0f;
        check(view.viewportPaint(Vec3{105, -50, 5}, Vec3{0, 1, 0}, true), "a world ray through the translated entity hits the sheet");
        check(view.pinnedCount() == 4, "and the brush pinned the four corners of the quad it landed in");
        view.viewportPaint(Vec3{105, -50, 5}, Vec3{0, 1, 0}, false);
        check(view.undo() && view.pinnedCount() == 0, "releasing the button ended the stroke: one undo");
        std::vector<PaintLine> lines;
        view.paint(view.select(Vec3{0, 0, 0}, 3.0f));
        view.collectLines(view.entityWorld, view.hover(), lines);
        check(lines.size() >= 3 + 72, "a pinned particle draws a cross and the brush draws three circles");
        f32 r, g, b;
        SoftBodyPaintTool::reachColour(0, r, g, b);
        const bool hard = g > r;
        SoftBodyPaintTool::reachColour(254, r, g, b);
        check(hard && r > g, "reach colours run green (hard skin) to red");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
