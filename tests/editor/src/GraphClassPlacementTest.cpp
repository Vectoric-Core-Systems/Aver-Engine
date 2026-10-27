// GraphClassPlacementTest -- the pure half of "drag a graph CLASS into the viewport":
// sandbox/src/GraphClassPlacement.hpp (the CLASS scan, the placement record, the role refusal, the
// marker) and the class-root / tombstone rules in LevelClassSave.hpp.
//
// WHY A HEADLESS SUITE. SandboxApp is add_executable-only, so the ImGui menu, the drop and the gizmo cannot
// be reached from here; they are covered by `Sandbox.exe --class-place-test`. What can be pinned here is
// the part whose failures are silent: a scan that accepts a file the C# parser rejects (the class is
// offered, then "is not declared" on click), a record that does not round-trip, a root lookup that loops
// on a corrupt hierarchy, and a tombstone that is not honoured (a deleted actor coming back on reload).
#include "GraphClassPlacement.hpp"
#include "LevelClassSave.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef AVER_REPO_ROOT
#define AVER_REPO_ROOT "."
#endif

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static bool parse(const std::string& text, editor::GraphClassDecl& d) {
    return editor::parseGraphClassDecl(text, d);
}

int main() {
    AVER_INFO("=== parseGraphClassDecl follows the C# parser's CLASS rules ===");
    {
        editor::GraphClassDecl d;
        check(parse("OCGRAPH 1\nCLASS AN_Foo\n", d) && d.className == "AN_Foo" && d.parent == "Actor",
              "CLASS with no parent defaults to Actor (HostBridge's BaseRegistryName default)");
        check(parse("OCGRAPH 1\nCLASS AN_Foo Character view=firstperson\n", d) && d.parent == "Character",
              "the positional token after the name is the parent");
        check(parse("OCGRAPH 1\nCLASS AN_Foo mesh=Meshes/x.ocmesh\n", d) && d.parent == "Actor",
              "a key=value in the parent slot is NOT a parent -- 'mesh=...' as a parent would never seal");
        check(parse("OCGRAPH 1\nclass AN_Lower GameMode\n", d) && d.className == "AN_Lower" && d.parent == "GameMode",
              "the record name is case-insensitive");
        check(parse("OCGRAPH 1\r\nCLASS AN_Crlf Pawn\r\nCOMP a Scene\r\n", d) && d.className == "AN_Crlf" &&
              d.parent == "Pawn" && d.componentCount == 1,
              "CRLF line endings leave no carriage return in the name or the parent");
        check(!parse("OCGRAPH 1\n# CLASS AN_Nope\n", d), "a commented-out CLASS declares nothing");
        check(parse("OCGRAPH 1\n   \tCLASS AN_Indented\n", d) && d.className == "AN_Indented", "leading blanks are skipped");
        check(!parse("OCGRAPH 1\nNAME Plain\n", d), "no CLASS record -> not a class");
        check(!parse("OCGRAPH 1\nCLASS\n", d), "CLASS with no name is rejected, as the C# parser does");
        check(!parse("OCGRAPH 1\nCLASS AN_A\nCLASS AN_B\n", d), "a second CLASS record rejects the file");
        check(!parse("OCGRAPH 1\nDOMAIN material\nCLASS AN_Mat\n", d), "a material graph carrying a CLASS is not placeable");
        check(parse("OCGRAPH 1\nDOMAIN gameplay\nCLASS AN_Game\n", d), "DOMAIN gameplay is placeable");
        check(parse("OCGRAPH 1\nDOMAIN GAMEPLAY\nCLASS AN_Game\n", d), "  and the domain name is case-insensitive");
        check(!parse("OCGRAPH 1\nDOMAIN vfx\nCLASS AN_Vfx\n", d), "an unknown domain is not placeable");
        check(!parse("OCGRAPH 1\nDOMAIN\nCLASS AN_Blank\n", d), "a blank DOMAIN is refused, like the C# parser");
        check(parse("OCGRAPH 1\nCLASS AN_Three\nCOMP a Scene\nCOMP b Mesh parent=a\nCOMP c Scene parent=a\n", d) &&
              d.componentCount == 3, "COMP records are counted for the picker tooltip");
        check(parse("OCGRAPH 1\nDESCRIPTION the CLASS of things\nCLASS AN_Desc\n", d) && d.className == "AN_Desc",
              "a record name inside another record's text is not a record");
    }

    AVER_INFO("=== scanGraphClasses over a content tree ===");
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "aver_graphclassplacement_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        writeFile(root / "Scripts" / "B_Second.ocgraph", "OCGRAPH 1\nCLASS AN_Beta Actor\n");
        writeFile(root / "Scripts" / "Nested" / "Deeper" / "A_First.OCGRAPH", "OCGRAPH 1\nCLASS AN_Alpha Character\nCOMP body Mesh\n");
        writeFile(root / "Materials" / "M_Glow.ocgraph", "OCGRAPH 1\nDOMAIN material\nNAME Glow\n");
        writeFile(root / "Scripts" / "NoClass.ocgraph", "OCGRAPH 1\nNAME Plain\n");
        writeFile(root / "Scripts" / "Dup1.ocgraph", "OCGRAPH 1\nCLASS AN_Dup\n");
        writeFile(root / "Scripts" / "Dup2.ocgraph", "OCGRAPH 1\nCLASS AN_Dup\n");
        writeFile(root / "Scripts" / "notes.txt", "CLASS AN_NotAGraph\n");

        const std::vector<editor::GraphClassDecl> found = editor::scanGraphClasses(root.string());
        check(found.size() == 4, "four classes found: a nested one, an upper-case extension, and the two duplicates");
        if (found.size() == 4) {
            check(found[0].className == "AN_Alpha" && found[1].className == "AN_Beta" &&
                  found[2].className == "AN_Dup" && found[3].className == "AN_Dup",
                  "sorted by class name");
            check(found[0].relPath == "Scripts/Nested/Deeper/A_First.OCGRAPH",
                  "relPath is content-relative with '/' separators");
            check(found[0].parent == "Character" && found[0].componentCount == 1, "parent and COMP count are carried");
            check(!found[2].duplicate && found[3].duplicate,
                  "the duplicate flag is set on the second declaration of a name only");
            check(found[2].path < found[3].path, "  and the first by path keeps the plain entry");
        }
        check(editor::scanGraphClasses((root / "does_not_exist").string()).empty(),
              "a missing directory returns empty and does not throw");
        check(editor::scanGraphClasses("").empty(), "an empty content directory returns empty");
        fs::remove_all(root, ec);
    }

    AVER_INFO("=== the FirstPerson template's real graphs ===");
    {
        const std::string scripts = std::string(AVER_REPO_ROOT) + "/templates/FirstPerson/Content/Scripts/";
        editor::GraphClassDecl d;
        check(parse(readFile(scripts + "AN_FPCharacter.ocgraph"), d) && d.className == "AN_FPCharacter" &&
              d.parent == "Character" && d.componentCount >= 1,
              "AN_FPCharacter: class AN_FPCharacter, parent Character, at least its body COMP");
        check(parse(readFile(scripts + "AN_FPRules.ocgraph"), d) && d.parent == "GameMode",
              "AN_FPRules: parent GameMode, which the picker must then be able to refuse");
        check(parse(readFile(scripts + "AN_FPTarget.ocgraph"), d) && d.className == "AN_FPTarget",
              "AN_FPTarget parses (key=value attributes after the parent)");
    }

    AVER_INFO("=== makeClassPlacement is the record the loader already reads ===");
    {
        const fmt::OcWorldPlacement p = editor::makeClassPlacement("AN_Foo", Vec3{100, -20, 5}, 90.0);
        check(p.asset == "none" && p.className == "AN_Foo" && p.sx == 1 && p.sy == 1 && p.sz == 1 &&
              p.collide && !p.snapToGround, "asset none, scale 1, collide, not snapped");
        fmt::OcWorldData d;
        d.name = "T";
        d.placements.push_back(p);
        const std::string text = fmt::writeOcworld(d);
        check(text.find("PLACE none") != std::string::npos, "the written text has a 'PLACE none' line");
        check(text.find("class AN_Foo") != std::string::npos, "  carrying 'class AN_Foo'");
        fmt::OcWorldData back;
        std::string err;
        check(fmt::parseOcworld(text, back, &err) && back.placements.size() == 1, "it parses back: " + err);
        if (back.placements.size() == 1) {
            const fmt::OcWorldPlacement& q = back.placements[0];
            check(q.className == "AN_Foo" && std::fabs(q.x - 100) < 1e-6 && std::fabs(q.y + 20) < 1e-6 &&
                  std::fabs(q.z - 5) < 1e-6 && std::fabs(q.yaw - 90) < 1e-6 && q.sx == 1,
                  "same class, position, yaw and scale after the round trip");
        }
    }

    AVER_INFO("=== classPlaceRefusal ===");
    {
        editor::ClassRoleBits none;
        check(editor::classPlaceRefusal(none) == nullptr, "a plain class is placeable");
        editor::ClassRoleBits b;
        b = editor::ClassRoleBits{}; b.gameMode = true;
        check(editor::classPlaceRefusal(b) && *editor::classPlaceRefusal(b), "GameMode is refused with a reason");
        b = editor::ClassRoleBits{}; b.controller = true;
        check(editor::classPlaceRefusal(b) && *editor::classPlaceRefusal(b), "a controller is refused with a reason");
        b = editor::ClassRoleBits{}; b.gameInstance = true;
        check(editor::classPlaceRefusal(b) && *editor::classPlaceRefusal(b), "a GameInstance is refused with a reason");
        b = editor::ClassRoleBits{}; b.abstractClass = true;
        check(editor::classPlaceRefusal(b) && *editor::classPlaceRefusal(b), "an abstract class is refused with a reason");
        check(editor::classPlaceRefusalText(b, "AN_Base").find("'AN_Base'") == 0,
              "the abstract text names the class");
        check(editor::classPlaceRefusalText(none, "AN_Ok").empty(), "and a placeable class has no text");
    }

    AVER_INFO("=== isGraphAssetExt ===");
    check(editor::isGraphAssetExt(".ocgraph"), ".ocgraph is a graph asset");
    check(!editor::isGraphAssetExt(".ocmesh") && !editor::isGraphAssetExt(".ocgraph.bak") && !editor::isGraphAssetExt(""),
          "a mesh, a backup suffix and an empty extension are not");

    AVER_INFO("=== classRootOf: a click on a part selects the root ===");
    {
        // 10 is a root; 11 its child; 12 a grandchild; 20 a removed instance's root with child 21; 30 unrelated.
        std::map<int32_t, int32_t> parent = {{11, 10}, {12, 11}, {21, 20}, {30, 0}};
        const auto parentOf = [&](int32_t e) { const auto it = parent.find(e); return it == parent.end() ? 0 : it->second; };
        std::vector<editor::LevelClassInstance> live = {{0, 10}, {1, 20}};
        live[1].removed = true;
        check(editor::classRootOf(11, parentOf, live) == 10, "a part resolves to its root");
        check(editor::classRootOf(10, parentOf, live) == 10, "a root resolves to itself");
        check(editor::classRootOf(12, parentOf, live) == 10, "a grandchild resolves to the root");
        check(editor::classRootOf(30, parentOf, live) == 0, "an unrelated entity belongs to no class");
        check(editor::classRootOf(20, parentOf, live) == 0 && editor::classRootOf(21, parentOf, live) == 0,
              "a removed instance is not matched, nor are its parts");
        check(editor::classRootOf(0, parentOf, live) == 0, "hit == 0 -> 0");
        parent[10] = 12;   // 10 -> 12 -> 11 -> 10: a corrupt hierarchy
        std::vector<editor::LevelClassInstance> none = {{0, 99}};
        check(editor::classRootOf(11, parentOf, none) == 0, "a parent cycle terminates");
    }

    AVER_INFO("=== appendClassPlacements honours the tombstone ===");
    {
        std::vector<fmt::OcWorldPlacement> authored(3);
        for (auto& p : authored) { p.x = 1; p.y = 2; p.z = 3; p.sx = p.sy = p.sz = 1; }
        authored[0].className = "AN_Unresolved";
        authored[1].className = "AN_Deleted";
        authored[2].className = "AN_Added";
        std::vector<editor::LevelClassInstance> live = {{1, 0}, {2, 42}};
        live[0].removed = true;
        const auto transformOf = [](int32_t e, Transform& xf) {
            if (e != 42) return false;
            xf.position = Vec3{7, 8, 9};
            xf.scale = Vec3{1, 1, 1};
            return true;
        };
        std::vector<fmt::OcWorldPlacement> out;
        editor::appendClassPlacements(authored, live, transformOf, out);
        check(out.size() == 2 && out[0].className == "AN_Unresolved" && out[1].className == "AN_Added",
              "the removed placement is omitted, the unresolved one is written as read");
        check(out.size() == 2 && out[1].x == 7 && out[1].y == 8 && out[1].z == 9,
              "a session-added placement is written at its live pose");
    }

    AVER_INFO("=== appendClassPlacements writes the parent slot it is given ===");
    {
        std::vector<fmt::OcWorldPlacement> authored(3);
        for (auto& p : authored) { p.className = "AN_A"; p.parent = 99; }
        std::vector<editor::LevelClassInstance> live = {{0, 10}, {1, 11}};
        live[1].removed = true;
        std::vector<fmt::OcWorldPlacement> out;
        editor::appendClassPlacements(authored, live, [](int32_t, Transform&) { return false; },
                                      [](std::size_t k, int32_t e) { return e == 10 ? 4 : static_cast<int32_t>(k) - 3; }, out);
        check(out.size() == 2 && out[0].parent == 4, "a live actor gets the slot its entity's parent maps to");
        check(out.size() == 2 && out[1].parent == -1, "one without an entity gets what the callback says, not the stale file index");
    }

    AVER_INFO("=== classMarkerSegments ===");
    {
        std::vector<editor::MarkerSeg> a, b;
        const Vec3 c{10, 20, 30};
        editor::classMarkerSegments(c, 25.0f, false, a);
        editor::classMarkerSegments(c, 25.0f, true, b);
        check(a.size() == 3 && b.size() == 3, "three axis segments");
        if (a.size() == 3) {
            check(a[0].a.x == -15 && a[0].b.x == 35 && a[0].a.y == 20 && a[0].b.z == 30, "X segment runs c +- half along X");
            check(a[1].a.y == -5 && a[1].b.y == 45 && a[1].a.x == 10, "Y segment runs c +- half along Y");
            check(a[2].a.z == 5 && a[2].b.z == 55 && a[2].a.y == 20, "Z segment runs c +- half along Z");
        }
        check(a.size() == 3 && b.size() == 3 &&
              (a[0].rgb[0] != b[0].rgb[0] || a[0].rgb[1] != b[0].rgb[1] || a[0].rgb[2] != b[0].rgb[2]),
              "the selected colour differs");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
