// FabricE2ETest -- the whole Fabric Connector through its real command line, on the hand-written Godot project
// tests/fabric/data/E2EGame: `AverAssetC fabric <godot-dir> <out-dir>` with no option runs the scene and asset converter, the
// script converter (Aver.Fabric.Convert on dotnet, GDScript and GodotSharp, compiled against Aver.Fabric), copies the GDExtension
// plugin and writes the merged report.
//
// What is asserted: the exit code and the summary line; the project, level, prefab and input files, each read back with the
// engine's own loader; the converted scripts present, compiled and rated converted in the report; the plugin copied, declared in
// Fabric/project.json and placed as a GDExtensionNode; the report's counts (the summary line, the jsonl and exactly the items that
// are unsupported or approximated); then the other modes (--overwrite, --dry-run, --scenes-only, --scripts-only) and a failure.
//
// The plugin is FabricTestExt, built by this repository from C (tests/fabric/gdext_test); its library is copied into a staged copy
// of the fixture under all three platform names, so no binary lives in the tree and the counts do not depend on the OS.
// Skips (exit 77) when the script converter or dotnet is not available: those are built by the Aver.Fabric.Convert target.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcInput.hpp"
#include "aver/formats/OcPrefab.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"
#include "fabric/FabricProcess.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
namespace fsys = std::filesystem;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static fabric::ProcessOutput runFabric(const std::string& exe, const std::vector<std::string>& args) {
    std::vector<std::string> full = {"fabric"};
    full.insert(full.end(), args.begin(), args.end());
    return fabric::runProcess(exe, full);
}

// One line of FabricReport.jsonl, the fields the test looks at.
struct Entry {
    std::string kind, file, node, type, property, rating, reason;
};

static std::string field(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    size_t at = line.find(needle);
    if (at == std::string::npos) return {};
    std::string out;
    for (at += needle.size(); at < line.size() && line[at] != '"'; ++at) {
        if (line[at] == '\\' && at + 1 < line.size()) ++at;
        out.push_back(line[at]);
    }
    return out;
}

static std::vector<Entry> readReport(const std::string& path) {
    std::vector<Entry> out;
    std::istringstream in(slurp(path));
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        out.push_back(Entry{field(line, "kind"), field(line, "file"), field(line, "node"), field(line, "type"), field(line, "property"),
                            field(line, "rating"), field(line, "reason")});
    }
    return out;
}

static long number(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = line.find(needle);
    return at == std::string::npos ? -1 : std::strtol(line.c_str() + at + needle.size(), nullptr, 10);
}

static std::string summaryLine(const std::string& text) {
    std::istringstream in(text);
    std::string found;
    for (std::string line; std::getline(in, line);)
        if (line.rfind("{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fabric\"", 0) == 0) found = line;
    return found;
}

static int find(const fmt::OcWorldData& w, std::initializer_list<const char*> path) {
    int parent = -1;
    for (const char* name : path) {
        int found = -1;
        for (usize i = 0; i < w.placements.size(); ++i)
            if (w.placements[i].parent == parent && w.placements[i].name == name) { found = static_cast<int>(i); break; }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

static const std::string* prop(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find(key)) return v;
    return nullptr;
}

static void copyTree(const fsys::path& from, const fsys::path& to) {
    std::error_code ec;
    fsys::create_directories(to, ec);
    fsys::copy(from, to, fsys::copy_options::recursive | fsys::copy_options::overwrite_existing, ec);
}

int main() {
    AVER_INFO("FabricE2ETest");
    const fsys::path exe = AVER_ASSETC_EXE;
    const fsys::path extDir = exe.parent_path() / "Extensions" / "FabricConnector";
    const fsys::path toolDll = extDir / "tools" / "Aver.Fabric.Convert.dll";
    const fsys::path refsDll = extDir / "managed" / "Aver.Fabric.dll";
    const fsys::path pluginLib = AVER_FABRIC_TEST_EXT;
    std::error_code ec;
    check(fsys::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(fsys::exists(pluginLib, ec), "the test plugin was built: " + pluginLib.string());
    if (g_failures) return exitCode(ExitCode::Failed);
    if (fabric::findDotnet().empty() || !fsys::exists(toolDll, ec) || !fsys::exists(refsDll, ec)) {
        AVER_WARN("skipped: dotnet, Extensions/FabricConnector/tools/Aver.Fabric.Convert.dll or managed/Aver.Fabric.dll is not available ({})", exe.parent_path().string());
        return 77;
    }

    const fsys::path root = fsys::temp_directory_path() / "aver-fabric-e2e-test";
    fsys::remove_all(root, ec);

    // The Godot project, staged: the fixture plus the plugin's library under the three platform names its .gdextension lists.
    const fsys::path godot = root / "GodotProject";
    copyTree(fsys::path(AVER_FABRIC_DATA) / "E2EGame", godot);
    for (const char* name : {"FabricTestExt.dll", "libFabricTestExt.so", "libFabricTestExt.dylib"})
        fsys::copy_file(pluginLib, godot / "addons" / "fabrictest" / name, fsys::copy_options::overwrite_existing, ec);
    check(fsys::exists(godot / "project.godot", ec) && fsys::exists(godot / "addons" / "fabrictest" / "libFabricTestExt.so", ec), "the Godot project is staged");

    // ---- the whole conversion --------------------------------------------------------------------------------------------------
    AVER_INFO("-- AverAssetC fabric <godot-dir> <out-dir> --");
    const std::string out1 = (root / "out").string();
    const fabric::ProcessOutput run1 = runFabric(exe.string(), {godot.string(), out1});
    if (run1.exitCode != 0) AVER_INFO("{}", run1.text);
    check(run1.started && run1.exitCode == 0, "it exits 0 (got " + std::to_string(run1.exitCode) + ")");
    const std::string summary = summaryLine(run1.text);
    check(!summary.empty() && contains(summary, "\"exitCode\":0") && contains(summary, "\"project\":\"E2EGame\"") && contains(summary, "\"levels\":2") &&
              contains(summary, "\"scripts\":3") && contains(summary, "\"scriptsConverted\":true") && contains(summary, "\"extensions\":1") && contains(summary, "\"dryRun\":false"),
          "the summary line: project E2EGame, 2 levels, 3 scripts converted, 1 GDExtension");

    const std::string proj = out1 + "/E2EGame";
    const std::string content = proj + "/Content";

    // ---- project, levels, prefab, input ----------------------------------------------------------------------------------------
    AVER_INFO("-- the project files load --");
    fmt::ProjectDesc pd;
    std::string err;
    check(fmt::loadOcproject(proj + "/E2EGame.ocproject", pd, &err), "E2EGame.ocproject loads: " + err);
    check(pd.startMap == "Maps/main.ocworld", "its start level is Maps/main.ocworld (got " + pd.startMap + ")");

    fmt::OcWorldData main;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", main, &err), "Maps/main.ocworld loads: " + err);
    fmt::OcWorldData crateLevel;
    check(fmt::loadOcworld(content + "/Maps/props/crate.ocworld", crateLevel, &err), "Maps/props/crate.ocworld loads: " + err);
    fmt::OcPrefabData prefab;
    check(fmt::loadOcPrefab(content + "/Prefabs/props/crate.ocprefab", prefab, &err) && prefab.valid(), "Prefabs/props/crate.ocprefab loads: " + err);
    fmt::OcInputData input;
    check(fmt::loadOcinput(content + "/Input/E2EGame.ocinput", input, &err) && input.actions.size() == 2, "Input/E2EGame.ocinput loads with its two actions: " + err);

    // the placements the scripts and the plugin made
    const auto classOf = [&](std::initializer_list<const char*> path) {
        const int i = find(main, path);
        return i < 0 ? std::string("(missing)") : main.placements[static_cast<usize>(i)].className;
    };
    check(classOf({"Main", "Player"}) == "Player" && classOf({"Main", "Door"}) == "Door", "the nodes with a script are placements of the script's class (Player, Door)");
    check(classOf({"Session"}) == "Session", "the autoload is a top-level placement of its script's class");
    const int session = find(main, {"Session"});
    const std::string* persistent = prop(main, session, "_persistent");
    check(persistent && *persistent == "1", "and is persistent");
    check(classOf({"Main", "Crate"}) == "RigidBody3D" && find(main, {"Main", "Crate", "Body"}) >= 0, "the instanced crate is flattened into the level");
    check(classOf({"Main", "Probe"}) == "GDExtensionNode", "the node of the plugin's class is a GDExtensionNode placement");
    const std::string* extClass = prop(main, find(main, {"Main", "Probe"}), "extension_class");
    check(extClass && *extClass == "\"FabricTestNode\"", "naming the native class in extension_class");
    const std::string* speed = prop(main, find(main, {"Main", "Probe"}), "speed");
    check(speed && *speed == "2.5", "and keeping the node's own property for the plugin");

    // ---- the scripts -----------------------------------------------------------------------------------------------------------
    AVER_INFO("-- scripts converted and compiled --");
    for (const char* cs : {"Player", "Door", "Session"})
        check(fileExists(content + "/Scripts/" + cs + ".cs") && !contains(slurp(content + "/Scripts/" + cs + ".cs"), "// FABRIC: Aver.Fabric has no"),
              std::string("Content/Scripts/") + cs + ".cs was written with no compile error marked in it");
    check(fileExists(content + "/Scripts/Scripts.csproj"), "Scripts.csproj was written beside them (or the engine checkout was not found: see the report)") ;
    const std::string player = slurp(content + "/Scripts/Player.cs");
    check(contains(player, "class Player") && contains(player, "CharacterBody3D") && contains(player, "MoveAndSlide"), "player.gd became a Player : CharacterBody3D that calls MoveAndSlide");
    check(contains(slurp(content + "/Scripts/Door.cs"), "using Aver.Fabric;"), "Door.cs now uses Aver.Fabric instead of Godot");

    const std::vector<Entry> report = readReport(out1 + "/FabricReport.jsonl");
    std::map<std::string, std::string> scriptRating;
    std::map<std::string, std::string> scriptReason;
    for (const Entry& e : report)
        if (e.kind == "script" && (e.type == "gdscript" || e.type == "csharp")) { scriptRating[e.file] = e.rating; scriptReason[e.file] = e.reason; }
    check(scriptRating.size() == 3 && scriptRating["res://scripts/player.gd"] == "converted" && scriptRating["res://scripts/session.gd"] == "converted" &&
              scriptRating["res://scripts/Door.cs"] == "converted",
          "the report rates all three scripts converted (they compile against Aver.Fabric)");
    check(contains(scriptReason["res://scripts/player.gd"], "compiles against Aver.Fabric") && contains(scriptReason["res://scripts/Door.cs"], "compile"), "and says they compile");
    check(std::none_of(report.begin(), report.end(), [](const Entry& e) { return e.kind == "script" && e.reason.find("later package") != std::string::npos; }),
          "no script entry still says its conversion is for a later package");

    // ---- the GDExtension --------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the GDExtension plugin --");
    check(fileExists(content + "/addons/fabrictest/fabrictest.gdextension"), "the .gdextension was copied to Content at its res:// path");
    for (const char* lib : {"FabricTestExt.dll", "libFabricTestExt.so", "libFabricTestExt.dylib"})
        check(fileExists(content + "/addons/fabrictest/" + lib), std::string("the library ") + lib + " was copied beside it");
    const std::string projectJson = slurp(proj + "/Fabric/project.json");
    check(contains(projectJson, "\"extensions\": [") && contains(projectJson, "\"path\": \"res://addons/fabrictest/fabrictest.gdextension\"") &&
              contains(projectJson, "\"classes\": [\"FabricTestNode\"]") && contains(projectJson, "\"hostLibrary\": true") && contains(projectJson, "\"missing\": []"),
          "Fabric/project.json declares the plugin, its class and that the host library is present");
    bool extReport = false, probeReport = false;
    for (const Entry& e : report) {
        if (e.kind == "file" && e.type == "GDExtension" && e.file == "res://addons/fabrictest/fabrictest.gdextension") extReport = e.rating == "approximated";
        if (e.kind == "node" && e.node == "Probe" && e.type == "FabricTestNode") probeReport = e.rating == "approximated";
    }
    check(extReport && probeReport, "the report rates the plugin and its node approximated (hosted, not run against a real godot-cpp plugin)");
    check(std::none_of(report.begin(), report.end(), [](const Entry& e) { return e.type == "GDExtensionLibrary"; }), "no library is reported missing (the android entry is not asked for)");

    // ---- the report's counts ---------------------------------------------------------------------------------------------------
    AVER_INFO("-- the merged report --");
    std::map<std::string, int> counts;
    for (const Entry& e : report) ++counts[e.rating];
    check(number(summary, "converted") == counts["converted"] && number(summary, "approximated") == counts["approximated"] &&
              number(summary, "unsupported") == counts["unsupported"],
          "the summary line's counts are the jsonl's (" + std::to_string(counts["converted"]) + " / " + std::to_string(counts["approximated"]) + " / " +
              std::to_string(counts["unsupported"]) + ")");
    std::set<std::string> unsupported;
    for (const Entry& e : report)
        if (e.rating == "unsupported") unsupported.insert(e.kind + ":" + e.type + ":" + e.node + ":" + e.property);
    const std::set<std::string> wantUnsupported = {"node:XROrigin3D:Music:", "node:Sprite2D:Logo:", "setting:::rendering/renderer/rendering_method"};
    check(unsupported == wantUnsupported, "exactly the XR node, the 2D sprite and the renderer setting are unsupported (" + std::to_string(unsupported.size()) + " item(s))");
    if (unsupported != wantUnsupported)
        for (const std::string& u : unsupported) AVER_INFO("        unsupported: {}", u);
    const std::string md = slurp(out1 + "/FabricReport.md");
    check(contains(md, "# Fabric conversion report: E2E Game") && contains(md, "**Result: " + std::to_string(counts["converted"]) + " converted, " +
                                                                            std::to_string(counts["approximated"]) + " approximated, " + std::to_string(counts["unsupported"]) + " unsupported.**"),
          "FabricReport.md opens with the merged result line");
    check(contains(md, "| script |") && contains(md, "| node |") && contains(md, "| file |") && contains(md, "### Top unsupported features") && contains(md, "XROrigin3D"),
          "and has the per-category table and the top unsupported features");

    // ---- the other modes ---------------------------------------------------------------------------------------------------------
    AVER_INFO("-- --overwrite, --dry-run, --scenes-only, --scripts-only --");
    {
        const fabric::ProcessOutput again = runFabric(exe.string(), {godot.string(), out1});
        check(again.exitCode == 1 && contains(again.text, "--overwrite"), "converting into a folder that holds the project is refused without --overwrite (exit 1)");
        const std::string jsonlBefore = slurp(out1 + "/FabricReport.jsonl");
        const fabric::ProcessOutput over = runFabric(exe.string(), {godot.string(), out1, "--overwrite"});
        check(over.exitCode == 0 && slurp(out1 + "/FabricReport.jsonl") == jsonlBefore, "with --overwrite it converts again and the report is the same byte for byte");

        const std::string out2 = (root / "dry").string();
        const fabric::ProcessOutput dry = runFabric(exe.string(), {godot.string(), out2, "--dry-run"});
        check(dry.exitCode == 0 && contains(summaryLine(dry.text), "\"dryRun\":true") && fileExists(out2 + "/FabricReport.md") && fileExists(out2 + "/FabricReport.jsonl") &&
                  !fsys::exists(out2 + "/E2EGame", ec),
              "--dry-run writes the report and no project");
        check(readReport(out2 + "/FabricReport.jsonl").size() == report.size(), "and finds the same items as the real run");

        const std::string out3 = (root / "scenes").string();
        const fabric::ProcessOutput scenes = runFabric(exe.string(), {godot.string(), out3, "--scenes-only"});
        check(scenes.exitCode == 0 && contains(summaryLine(scenes.text), "\"scriptsConverted\":false") && fileExists(out3 + "/E2EGame/Content/Maps/main.ocworld") &&
                  !fsys::exists(out3 + "/E2EGame/Content/Scripts", ec) && fileExists(out3 + "/E2EGame/Content/addons/fabrictest/fabrictest.gdextension"),
              "--scenes-only converts the scenes and the plugin and leaves the scripts alone");
        const std::vector<Entry> scenesReport = readReport(out3 + "/FabricReport.jsonl");
        check(std::count_if(scenesReport.begin(), scenesReport.end(), [](const Entry& e) { return e.kind == "script" && e.rating == "unsupported" && e.reason.find("--scenes-only") != std::string::npos; }) == 3,
              "whose report says the three scripts were skipped");

        const fabric::ProcessOutput scripts = runFabric(exe.string(), {godot.string(), out3, "--scripts-only"});
        check(scripts.exitCode == 0 && contains(summaryLine(scripts.text), "\"scriptsConverted\":true") && fileExists(out3 + "/E2EGame/Content/Scripts/Player.cs"),
              "--scripts-only then converts the scripts into that project");
        const std::vector<Entry> finalReport = readReport(out3 + "/FabricReport.jsonl");
        check(std::count_if(finalReport.begin(), finalReport.end(), [](const Entry& e) { return e.kind == "script" && e.rating == "converted"; }) == 3 &&
                  std::none_of(finalReport.begin(), finalReport.end(), [](const Entry& e) { return e.reason.find("--scenes-only") != std::string::npos; }),
              "and the report now rates them converted, keeping the scene items");
        check(finalReport.size() == report.size(), "the two-step conversion ends with the one-step conversion's report");

        const fabric::ProcessOutput noProject = runFabric(exe.string(), {godot.string(), (root / "never").string(), "--scripts-only"});
        check(noProject.exitCode == 1, "--scripts-only without a converted project fails");
        const fabric::ProcessOutput bad = runFabric(exe.string(), {godot.string(), out3, "--scenes-only", "--scripts-only"});
        check(bad.exitCode == 2, "--scenes-only with --scripts-only is a usage error (exit 2)");
        const fabric::ProcessOutput nothing = runFabric(exe.string(), {(root / "nothing-here").string(), (root / "x").string()});
        check(nothing.exitCode == 1, "a folder with no project.godot is a hard failure (exit 1)");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) fsys::remove_all(root, ec);   // kept on failure, for looking at
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
