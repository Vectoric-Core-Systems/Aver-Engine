// FabricImportTest -- the part of File > Import Godot Project... that needs no UI (sandbox/src/FabricImport.cpp, compiled here without
// ImGui): the folder checks, the command line, reading the converter's closing summary, and the importer running the real AverAssetC
// on a worker thread and reporting through the editor's notification queue. The editor itself is not run.
#include "FabricImport.hpp"

#include "EditorNotifications.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace aver;
using namespace aver::editor;
namespace fsys = std::filesystem;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

int main() {
    AVER_INFO("FabricImportTest");
    std::error_code ec;
    const std::string data = AVER_FABRIC_DATA;
    const fsys::path root = fsys::temp_directory_path() / "aver-fabric-import-test";
    fsys::remove_all(root, ec);
    fsys::create_directories(root, ec);

    // ---- folder checks -----------------------------------------------------------------------------------------------------------
    AVER_INFO("-- choosing the project --");
    std::string why;
    check(isGodotProjectDir(data + "/ConvertGame", &why), "a folder with a Godot 4 project.godot is accepted: " + why);
    check(!isGodotProjectDir("", &why) && !why.empty(), "an empty path is refused with a reason");
    check(!isGodotProjectDir(root.string(), &why) && contains(why, "project.godot"), "a folder without project.godot is refused: " + why);
    {
        const fsys::path old = root / "godot3";
        fsys::create_directories(old, ec);
        writeFileText((old / "project.godot").string(), "config_version=4\n\n[application]\nconfig/name=\"Old\"\n");
        check(!isGodotProjectDir(old.string(), &why) && contains(why, "Godot 3"), "a Godot 3 project is refused and named: " + why);
    }
    const fsys::path expected = fsys::path(data + "/ConvertGame");
    check(fsys::path(godotProjectDirFrom(data + "/ConvertGame/project.godot")) == expected, "the dialog's project.godot gives its folder");
    check(fsys::path(godotProjectDirFrom(data + "/ConvertGame/props/crate.tscn")) == expected, "a file inside the project gives the project folder");
    check(fsys::path(godotProjectDirFrom(data + "/ConvertGame/props")) == expected, "and so does a folder inside it");
    check(godotProjectDirFrom((root / "nowhere" / "file.txt").string()).empty(), "a path outside any project gives nothing");
    check(fsys::path(defaultFabricOutDir("C:/games/My Game")).filename() == "My Game_Aver" && fsys::path(defaultFabricOutDir("C:/games/My Game/")).filename() == "My Game_Aver",
          "the default output is a sibling named <folder>_Aver");

    // ---- the command line --------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the command line --");
    {
        FabricImportRequest r;
        r.godotDir = "/a b/godot";
        r.outDir = "/out";
        check(fabricConverterArgs(r) == std::vector<std::string>({"fabric", "/a b/godot", "/out"}), "plain: fabric <godot> <out> (paths stay whole arguments)");
        r.overwrite = true; r.dryRun = true; r.scenesOnly = true;
        check(fabricConverterArgs(r) == std::vector<std::string>({"fabric", "/a b/godot", "/out", "--overwrite", "--dry-run", "--scenes-only"}), "with every option");
    }

    // ---- reading the converter's output -----------------------------------------------------------------------------------------
    AVER_INFO("-- the converter's summary --");
    {
        const std::string out =
            "[INFO ] fabric: a -> b\n"
            "{\"schemaVersion\":1,\"input\":\"x.gltf\",\"kind\":\"mesh\",\"ok\":true}\n"
            "{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fabric\",\"input\":\"in\",\"output\":\"C:\\\\Out\\\\Game\",\"project\":\"Game\",\"levels\":3,\"converted\":10,"
            "\"approximated\":4,\"unsupported\":2,\"scripts\":5,\"scriptsConverted\":true,\"extensions\":1,\"report\":\"C:\\\\Out\\\\FabricReport.md\",\"dryRun\":false,\"exitCode\":0}\n";
        const FabricImportResult r = parseFabricOutput(out, 0);
        check(r.ok() && r.projectName == "Game" && r.projectDir == "C:\\Out\\Game" && r.reportPath == "C:\\Out\\FabricReport.md", "the project, folder and report path are read (escapes decoded)");
        check(r.levels == 3 && r.converted == 10 && r.approximated == 4 && r.unsupported == 2 && r.scripts == 5 && r.extensions == 1 && r.scriptsConverted, "and the counts");
        check(r.manifestPath == "C:\\Out\\Game/Game.ocproject", "the manifest to open is <project>/<Name>.ocproject");
        const FabricImportResult f = parseFabricOutput("[INFO ] fabric: x\n[ERROR] fabric: it already holds a converted project; pass --overwrite\n", 1);
        check(!f.ok() && contains(f.error, "pass --overwrite") && f.manifestPath.empty(), "a failed run carries the converter's last error line");
        const FabricImportResult n = parseFabricOutput("nothing useful\n", 0);
        check(!n.ok() && !n.error.empty(), "a clean exit without a summary is not a success");
    }

    // ---- the real converter -------------------------------------------------------------------------------------------------------
    AVER_INFO("-- running AverAssetC --");
    const std::string converter = fabricConverterPath();
    check(!converter.empty(), "AverAssetC is beside the test: " + converter);
    if (converter.empty()) { AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures); return exitCode(ExitCode::Failed); }

    // The script step needs dotnet and the staged tool; without them the import is scenes only (the test is about the importer).
    const bool haveScriptTool = fileExists(executableDir() + "/Tools/Aver.Fabric.Convert.dll") && fileExists(executableDir() + "/Scripting/Aver.Fabric.dll");

    FabricImportRequest req;
    req.godotDir = data + "/ConvertGame";
    req.outDir = (root / "out").string();
    req.scenesOnly = !haveScriptTool;
    std::vector<std::string> seen;
    const FabricImportResult direct = runFabricImport(req, converter, [&](const std::string& line) { seen.push_back(line); });
    check(direct.ok() && direct.projectName == "ConvertGame" && direct.levels == 2, "runFabricImport converts ConvertGame: " + direct.error);
    check(fileExists(direct.manifestPath) && fileExists(direct.reportPath), "the manifest and the report it names exist");
    check(!seen.empty() && contains(seen.front() + seen.back(), "fabric"), "its lines were handed over as they arrived (" + std::to_string(seen.size()) + ")");

    const FabricImportResult again = runFabricImport(req, converter);
    check(!again.ok() && contains(again.error, "--overwrite"), "importing into the same folder again says to overwrite: " + again.error);
    req.overwrite = true;
    check(runFabricImport(req, converter).ok(), "and does with overwrite");
    FabricImportRequest bad = req;
    bad.godotDir = (root / "godot3").string();
    check(!runFabricImport(bad, converter).ok(), "a folder with a Godot 3 project fails in the converter too");
    check(!runFabricImport(req, (root / "no-such-tool").string()).started, "a missing converter is reported as not started");

    // ---- the importer on its worker thread -----------------------------------------------------------------------------------------
    AVER_INFO("-- the importer --");
    FabricImporter& importer = fabricImporter();
    notifications().clear();
    req.outDir = (root / "threaded").string();
    req.overwrite = false;
    std::string startWhy;
    check(importer.start(req, &startWhy), "start() launches the converter: " + startWhy);
    std::string second;
    check(!importer.start(req, &second) && contains(second, "already running"), "a second start while it runs is refused");
    check(notifications().size() >= 1, "a progress notification is up while it runs");
    for (int i = 0; i < 600 && importer.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    importer.drawModal(1.0f);   // reaps the finished import
    check(!importer.running() && importer.lastResult().ok() && fileExists(importer.lastResult().manifestPath), "it finishes and the result names the project to open");
    std::vector<Notification> shown;
    usize hidden = 0;
    notifications().tick(1.0, shown, hidden);
    bool finished = false;
    for (const Notification& n : shown) finished = finished || (n.title == "Godot project imported" && !n.hasProgress && n.severity != NotifySeverity::Error);
    check(finished, "and the notification became 'Godot project imported'");
    check(importer.takeOpenRequest().empty(), "nothing asks the editor to open the project until the user presses the button");

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) fsys::remove_all(root, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
