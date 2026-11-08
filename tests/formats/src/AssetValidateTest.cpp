// AssetValidateTest -- the asset validator's built-in rules against a fixture content tree, plus rule
// registration. Every bad fixture is built beside a good one so a rule that flagged everything fails.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/AssetValidate.hpp"
#include "aver/formats/OcMesh.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) AVER_INFO("   PASS  {}", what);
    else      { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A file that is only a PNG header: the rule reads dimensions and nothing else.
static void putPng(const fs::path& p, u32 w, u32 h) {
    std::string s("\x89PNG\r\n\x1a\n", 8);
    s += std::string("\0\0\0\x0d", 4) + "IHDR";
    for (u32 v : {w, h}) for (int sh = 24; sh >= 0; sh -= 8) s += static_cast<char>((v >> sh) & 0xFF);
    s += std::string("\x08\x06\0\0\0", 5);
    put(p, s);
}

static u32 countRule(const fmt::ValidateReport& r, const std::string& rule, const std::string& asset) {
    u32 n = 0;
    for (const auto& i : r.issues) if (i.rule == rule && i.asset == asset) ++n;
    return n;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "aver-assetvalidate-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path c = root / "Content";

    putPng(c / "Textures/ok.png", 64, 32);
    putPng(c / "Textures/npot.png", 24, 12);     // multiple of 4, not a power of two
    putPng(c / "Textures/odd.png", 30, 30);      // not a multiple of 4

    put(c / "Materials/MG_Good.ocgraph",
        "OCGRAPH 1\nDOMAIN material\nNODE a ConstFloat3 0 0\nNODE b Multiply 10 0\nLINK a.value b.a\n");
    put(c / "Materials/MG_BadLink.ocgraph",
        "OCGRAPH 1\nDOMAIN material\nNODE a ConstFloat3 0 0\nLINK a.value ghost.a\n");
    put(c / "Materials/MG_Dup.ocgraph",
        "OCGRAPH 1\nDOMAIN material\nNODE a ConstFloat3 0 0\nNODE a Multiply 10 0\n");
    put(c / "Materials/MG_Gameplay.ocgraph", "OCGRAPH 1\nDOMAIN gameplay\nNODE a Add 0 0\n");

    put(c / "Materials/M_Good.ocmat",
        "OCMAT 1\n# Textures/not_a_ref.png in a comment\nSHADER standard\nTEX baseColor {path:Textures/ok.png} uv0 sRGB\n"
        "GRAPHREF Materials/MG_Good.ocgraph\n");
    put(c / "Materials/M_Missing.ocmat",
        "OCMAT 1\nSHADER standard\nTEX baseColor {path:Textures/gone.png} uv0 sRGB\n");
    put(c / "Materials/M_NoHeader.ocmat", "SHADER standard\n");
    put(c / "Materials/M_WrongDomain.ocmat", "OCMAT 1\nSHADER standard\nGRAPHREF Materials/MG_Gameplay.ocgraph\n");

    fmt::OcMeshData good;
    good.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    good.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    good.uvs = {0, 0, 1, 0, 0, 1};
    good.indices = {0, 1, 2};
    good.materialSlots = {"A"};
    good.submeshes.push_back({"s0", 0, 0, 3, 0, 3});
    fmt::OcMeshData badSlot = good;
    badSlot.submeshes[0].materialSlot = 3;
    std::string why;
    fs::create_directories(c / "Meshes");
    check(fmt::saveOcMesh((c / "Meshes/good.ocmesh").string(), good, &why), "fixture mesh saved");
    fmt::saveOcMesh((c / "Meshes/badslot.ocmesh").string(), badSlot, &why);
    put(c / "Meshes/junk.ocmesh", "not a mesh");

    put(c / "Maps/L.ocworld",
        "OCWORLD 1\nNAME L\n# Meshes/missing_in_comment.ocmesh\nPLACE Meshes/good.ocmesh 0 0 0 0 0 0 1 name A\n"
        "PLACE Meshes/nope.ocmesh 0 0 0 0 0 0 1 name B\nPLACE Meshes/Cube.ocmesh 0 0 0 0 0 0 1 name C\n"
        "PLACE Engine/Builtin.ocmesh 0 0 0 0 0 0 1 name D\n"
        "  COMPONENT Foo asset=Vehicles\\Gone.ocvehicle\n");
    put(c / "Maps/Clean.ocworld", "OCWORLD 1\nNAME Clean\nPLACE Meshes/good.ocmesh 0 0 0 0 0 0 1 name A\n");

    fmt::ValidateContext ctx;
    ctx.contentDir = c.string();
    const fmt::AssetValidator v;
    const fmt::ValidateReport r = v.validateAll(ctx);
    for (const auto& i : r.issues) AVER_INFO("   {}", fmt::formatValidateIssue(i));

    check(r.filesChecked > 10, "files with applicable rules are counted");
    check(countRule(r, "missing-reference", "Maps/Clean.ocworld") == 0, "clean level: no issues");
    check(countRule(r, "missing-reference", "Maps/L.ocworld") == 3,
          "level: missing mesh, missing built-in mesh, missing backslash-style reference; comment ignored");
    check(countRule(r, "missing-reference", "Materials/M_Missing.ocmat") == 1, "material: missing texture found");
    check(countRule(r, "missing-reference", "Materials/M_Good.ocmat") == 0, "material: comment path and valid refs ignored");

    ctx.extraRoots.push_back((root / "Engine").string());
    put(root / "Engine/Engine/Builtin.ocmesh", "x");
    fmt::ValidateReport r2;
    v.validateFile(ctx, "Maps/L.ocworld", r2);
    check(r2.issues.size() == 2, "an extra root resolves built-in content");

    check(countRule(r, "ocgraph-parse", "Materials/MG_Good.ocgraph") == 0, "good graph passes");
    check(countRule(r, "ocgraph-parse", "Materials/MG_BadLink.ocgraph") == 1, "link to unknown node flagged");
    check(countRule(r, "ocgraph-parse", "Materials/MG_Dup.ocgraph") == 1, "duplicate node id flagged");
    check(countRule(r, "ocmat-parse", "Materials/M_WrongDomain.ocmat") == 1, "GRAPHREF to a non-material graph flagged");
    check(countRule(r, "ocmat-parse", "Materials/M_NoHeader.ocmat") == 1, "material without an OCMAT header flagged");
    check(countRule(r, "ocmat-parse", "Materials/M_Good.ocmat") == 0, "GRAPHREF to a material graph passes");

    check(countRule(r, "mesh-slots", "Meshes/good.ocmesh") == 0, "good mesh passes");
    check(countRule(r, "mesh-slots", "Meshes/badslot.ocmesh") >= 1, "out-of-range material slot flagged");
    check(countRule(r, "mesh-slots", "Meshes/junk.ocmesh") == 1, "an unreadable mesh is flagged");

    check(countRule(r, "texture-dimensions", "Textures/ok.png") == 0, "64x32 passes");
    check(countRule(r, "texture-dimensions", "Textures/npot.png") == 1, "24x12 reported");
    check(countRule(r, "texture-dimensions", "Textures/odd.png") == 1, "30x30 reported");
    for (const auto& i : r.issues) {
        if (i.asset == "Textures/npot.png") check(i.severity == fmt::ValidateSeverity::Info, "non-power-of-two is info");
        if (i.asset == "Textures/odd.png")  check(i.severity == fmt::ValidateSeverity::Warning, "non-multiple-of-4 is a warning");
    }
    check(r.count(fmt::ValidateSeverity::Error) >= 6, "error count reflects the broken fixtures");

    // Rules are registrable and scoped by extension.
    fmt::AssetValidator custom(std::vector<fmt::ValidateRule>{});
    custom.addRule({"no-txt", {".txt"}, [](const fmt::ValidateContext&, const std::string& rel, fmt::ValidateReport& rep) {
                        rep.issues.push_back({fmt::ValidateSeverity::Warning, "no-txt", rel, "text files are not allowed"});
                    }});
    put(c / "notes.txt", "hi");
    const fmt::ValidateReport r3 = custom.validateAll(ctx);
    check(r3.issues.size() == 1 && r3.issues[0].asset == "notes.txt" && r3.filesChecked == 1,
          "a custom rule runs only on its extension");

    {
        fmt::ValidateContext none;
        none.contentDir = (root / "nope").string();
        const fmt::ValidateReport e = v.validateAll(none);
        check(e.issues.empty() && e.filesChecked == 0, "a missing content dir yields an empty report");
    }

    fs::remove_all(root, ec);
    AVER_INFO("[assetvalidate-test] RESULT: {}", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
