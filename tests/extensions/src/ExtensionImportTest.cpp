// ExtensionImportTest -- the part of the File menu's extension import dialog that needs no UI (sandbox/src/ExtensionImport.cpp, compiled here
// without ImGui): the importers read from extension.json through the loader, the input checks, the command line, the command protocol's
// progress and result lines (aver_ext_protocol.h), and the importer running a tool on a worker thread and reporting through the editor's
// notification queue. The "tool" is this executable started as `ExtensionImportTest fakecmd <in> <out> [flags]`, so no extension, no AverAssetC
// and no .NET are needed. The editor is not run.
#include "EditorNotifications.hpp"
#include "ExtensionImport.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
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

static void printMessage(const ext::CommandMessage& m) {
    std::printf("%s\n", ext::formatCommandMessage(m).c_str());
    std::fflush(stdout);
}

// The fake command: what an extension's tool prints. Its input names the behaviour: "bad" fails with exit 1, "refuse" reports ok:false with exit 0,
// "silent" prints no result line.
static int fakeCommand(int argc, char** argv) {
    const std::string in = argc > 2 ? argv[2] : "";
    const std::string out = argc > 3 ? argv[3] : "";
    bool dry = false;
    for (int i = 4; i < argc; ++i) dry = dry || std::strcmp(argv[i], "--dry-run") == 0;
    std::printf("[INFO ] fake: %s -> %s\n", in.c_str(), out.c_str());
    ext::CommandMessage progress;
    progress.kind = ext::MessageKind::Progress;
    progress.fraction = 0.5;
    progress.text = "halfway";
    printMessage(progress);
    if (in.find("hang") != std::string::npos) {   // a step that takes far longer than the test: only a cancel ends it
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(120));
        return 0;
    }
    ext::CommandMessage result;
    result.kind = ext::MessageKind::Result;
    if (in.find("silent") != std::string::npos) return 0;
    if (in.find("bad") != std::string::npos) {
        std::printf("[ERROR] fake: it is bad\n");
        result.ok = false;
        result.errors = 1;
        result.message = "it is bad";
        printMessage(result);
        return 1;
    }
    if (in.find("refuse") != std::string::npos) {
        result.ok = false;
        result.message = "this input is from an unsupported version";
        printMessage(result);
        return 0;
    }
    result.ok = true;
    result.open = dry ? std::string() : out + "/Proj/Proj.ocproject";
    result.report = out + "/Report.md";
    result.warnings = 2;
    result.message = "10 done, 2 left";
    printMessage(result);
    return 0;
}

static std::string slurpText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string manifest(const std::string& name, int abi, const std::string& importers) {
    return "{\n  \"name\": \"" + name + "\", \"displayName\": \"" + name + " Ext\", \"version\": \"1.0.0\", \"abi\": " + std::to_string(abi) +
           ", \"engine\": \">=0.1.0\",\n  \"importers\": " + importers + "\n}";
}

static const char* kFakeImporters = R"JSON([
  { "id": "fake", "menu": "Import Fake Thing...", "command": "fakecmd", "tooltip": "Tip", "description": "Describes it.",
    "input": { "kind": "folder", "detect": "fake.project" }, "outputDefault": "{input}_Out",
    "options": [ { "flag": "--overwrite", "label": "Replace" }, { "flag": "--dry-run", "label": "Check only", "default": true } ] },
  { "menu": "Import Fake File...", "command": "fakefile", "input": { "kind": "file", "patterns": ["*.fake", "*.fk"] } }
])JSON";

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "fakecmd") == 0) return fakeCommand(argc, argv);

    AVER_INFO("ExtensionImportTest");
    std::error_code ec;
    const fsys::path root = fsys::temp_directory_path() / "aver-extension-import-test";
    fsys::remove_all(root, ec);
    fsys::create_directories(root / "Extensions" / "Fake", ec);
    fsys::create_directories(root / "Extensions" / "Wrong", ec);
    fsys::create_directories(root / "Extensions" / "Old", ec);
    writeFileText((root / "Extensions" / "Fake" / "extension.json").string(), manifest("Fake", AVER_EXT_ABI, kFakeImporters));
    writeFileText((root / "Extensions" / "Wrong" / "extension.json").string(),
                  manifest("Wrong", 99, R"JSON([ { "menu": "Import Wrong...", "command": "wrongcmd" } ])JSON"));
    writeFileText((root / "Extensions" / "Old" / "extension.json").string(),
                  manifest("Old", 1, R"JSON([ { "menu": "Import Old...", "command": "oldcmd" } ])JSON"));

    // ---- the importers the loader lists ------------------------------------------------------------------------------------------
    AVER_INFO("-- the manifest --");
    ExtensionImporter importer;
    importer.scan((root / "Extensions").string());
    check(importer.entries().size() == 2, "two importers: the extensions with another ABI add none (" + std::to_string(importer.entries().size()) + ")");
    if (importer.entries().size() != 2) { AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures); return exitCode(ExitCode::Failed); }
    const ImporterEntry& entry = importer.entries()[0];
    const ImporterEntry& fileEntry = importer.entries()[1];
    const ext::ImporterInfo& info = entry.info;
    check(entry.extension == "Fake Ext" && info.id == "fake" && info.menu == "Import Fake Thing..." && info.command == "fakecmd" && info.input.kind == "folder" &&
              info.input.detect == "fake.project" && info.input.patterns.empty() && info.outputDefault == "{input}_Out",
          "id, menu, command, input and output default");
    check(info.options.size() == 2 && info.options[0].flag == "--overwrite" && !info.options[0].onByDefault && info.options[1].onByDefault, "options");
    check(entry.text.title == "Import Fake Thing" && entry.text.tooltip == "Tip" && entry.text.description == "Describes it.",
          "the title is the menu text without its dots; the tooltip and description come from the manifest");
    check(fileEntry.info.id == "fakefile" && fileEntry.info.input.kind == "file" && fileEntry.info.input.patterns.size() == 2 && fileEntry.text.tooltip.empty(),
          "a file importer: the id defaults to the command, the patterns are listed");
    {
        ext::ImporterInfo bare;
        bare.menu = "Import Bare...";
        bare.description = "Words.";
        const ImporterText t = importerText(bare);
        check(t.title == "Import Bare" && t.tooltip == "Words." && importPickerSpec(bare) == "*.*" && importPickerSpec(info) == "fake.project" &&
                  importPickerSpec(fileEntry.info) == "*.fake;*.fk",
              "the tooltip defaults to the description; the picker offers the patterns, else the detect file, else everything");
    }

    // ---- choosing the input -------------------------------------------------------------------------------------------------------
    AVER_INFO("-- choosing the input --");
    const fsys::path good = root / "good";
    fsys::create_directories(good / "sub", ec);
    writeFileText((good / "fake.project").string(), "version=4\n");
    writeFileText((root / "thing.fake").string(), "x");
    writeFileText((root / "thing.other").string(), "x");
    std::string why;
    check(importInputValid(info, good.string(), &why), "a folder with the detect file is accepted: " + why);
    check(!importInputValid(info, "", &why) && why == "Choose the input folder.", "an empty path is refused with a reason: " + why);
    check(!importInputValid(info, root.string(), &why) && contains(why, "fake.project"), "a folder without the detect file is refused: " + why);
    check(!importInputValid(info, (root / "nowhere").string(), &why) && contains(why, "no folder"), "a folder that does not exist is refused: " + why);
    check(fsys::path(importInputFrom(info, (good / "fake.project").string())) == good, "the detect file gives its folder");
    check(fsys::path(importInputFrom(info, (good / "sub").string())) == good, "a folder inside the input gives the input folder");
    check(importInputFrom(info, (root / "nowhere" / "file.txt").string()).empty(), "a path outside any input gives nothing");
    check(fsys::path(importDefaultOutDir(info, "C:/games/My Game")).filename() == "My Game_Out" && fsys::path(importDefaultOutDir(info, "C:/games/My Game/")).filename() == "My Game_Out",
          "the default output is a sibling named by outputDefault");
    {
        const ext::ImporterInfo& file = fileEntry.info;
        check(importInputValid(file, (root / "thing.fake").string(), &why), "a file importer accepts an existing file that matches a pattern: " + why);
        check(!importInputValid(file, (root / "thing.other").string(), &why) && contains(why, "*.fake, *.fk"), "and refuses one that matches none: " + why);
        check(!importInputValid(file, good.string(), &why) && !why.empty(), "and refuses a folder: " + why);
        check(!importInputValid(file, "", &why) && why == "Choose the input file.", "and an empty path: " + why);
        check(importInputFrom(file, "C:/a/b.fake") == "C:/a/b.fake", "its picked path is the input");
        check(fsys::path(importDefaultOutDir(file, "C:/models/ship.fake")).filename() == "ship_out", "the default output of a file is named from its stem; the default pattern is {input}_out");
    }

    // ---- the command line --------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the command line --");
    {
        ImportRequest r;
        r.command = "fakecmd";
        r.inDir = "/a b/in";
        r.outDir = "/out";
        check(importArgs(r) == std::vector<std::string>({"fakecmd", "/a b/in", "/out"}), "plain: <command> <in> <out> (paths stay whole arguments)");
        r.flags = {"--overwrite", "--dry-run"};
        check(importArgs(r) == std::vector<std::string>({"fakecmd", "/a b/in", "/out", "--overwrite", "--dry-run"}), "the ticked flags follow");
    }

    // ---- reading the command's output ---------------------------------------------------------------------------------------------
    AVER_INFO("-- the protocol lines --");
    {
        const std::string out =
            "[INFO ] x: a -> b\n"
            "{\"schemaVersion\":1,\"input\":\"x.gltf\",\"kind\":\"mesh\",\"ok\":true}\n"
            "{\"aver\":\"progress\",\"fraction\":0.25,\"text\":\"scenes\"}\n"
            "{\"aver\":\"progress\",\"fraction\":0.75,\"text\":\"scripts\"}\n"
            "{\"aver\":\"result\",\"ok\":true,\"open\":\"C:\\\\Out\\\\Game\\\\Game.ocproject\",\"report\":\"C:\\\\Out\\\\Report.md\",\"warnings\":3,\"errors\":0,\"message\":\"71 converted\"}\r\n";
        const ImportResult r = parseImportOutput(out, 0);
        check(r.ok() && r.openPath == "C:\\Out\\Game\\Game.ocproject" && r.reportPath == "C:\\Out\\Report.md" && r.warnings == 3 && r.errors == 0 && r.message == "71 converted",
              "the result's open and report paths, counts and message are read (escapes decoded)");
        check(r.fraction == 0.75 && r.progressText == "scripts", "the last progress line is kept");
        check(r.error.empty() && r.log == out, "no error on success; the whole output is kept");
        const ImportResult f = parseImportOutput("[INFO ] x\n[ERROR] x: it already holds a project; pass --overwrite\n", 1);
        check(!f.ok() && contains(f.error, "pass --overwrite") && f.openPath.empty(), "a failed run without a result carries the command's last error line");
        const ImportResult g = parseImportOutput("{\"aver\":\"result\",\"ok\":false,\"errors\":1,\"message\":\"unsupported version\"}\n", 1);
        check(!g.ok() && g.error == "unsupported version","a failed run with a result carries the result's message");
        const ImportResult h = parseImportOutput("{\"aver\":\"result\",\"ok\":false,\"message\":\"refused\"}\n", 0);
        check(!h.ok() && h.error == "refused", "ok:false is a failure even when the exit code is 0");
        const ImportResult n = parseImportOutput("nothing useful\n", 0);
        check(!n.ok() && !n.error.empty(), "a clean exit without a result line is not a success");
        const ImportResult s = parseImportOutput("{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fakecmd\"}\n", 0);
        check(!s.ok(), "the old summary line is not a result");
    }

    // ---- the tool -----------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- running the tool --");
    const std::string self = executableDir() + AVER_PATH_SEP "ExtensionImportTest" + kExecutableSuffix;
    check(fileExists(self), "this executable is the fake tool: " + self);
    ImportRequest req;
    req.command = "fakecmd";
    req.inDir = good.string();
    req.outDir = (root / "out").string();
    std::vector<std::string> seen;
    const ImportResult direct = runImport(req, self, [&](const std::string& line) { seen.push_back(line); });
    check(direct.ok() && direct.message == "10 done, 2 left" && direct.warnings == 2 && !direct.openPath.empty(), "runImport reads the result: " + direct.error);
    check(!seen.empty() && contains(seen.front(), "fake:") && seen.size() == 3, "its lines were handed over as they arrived (" + std::to_string(seen.size()) + ")");
    req.flags = {"--dry-run"};
    const ImportResult dry = runImport(req, self);
    check(dry.ok() && dry.openPath.empty() && !dry.reportPath.empty(), "a result with nothing to open names none");
    ImportRequest bad = req;
    bad.inDir = (root / "bad").string();
    const ImportResult failed = runImport(bad, self);
    check(!failed.ok() && failed.error == "it is bad", "a failing run reports the result's message: " + failed.error);
    ImportRequest refuse = req;
    refuse.inDir = (root / "refuse").string();
    const ImportResult refused = runImport(refuse, self);
    check(!refused.ok() && refused.error == "this input is from an unsupported version","a command can refuse its input itself, with a message: " + refused.error);
    ImportRequest silent = req;
    silent.inDir = (root / "silent").string();
    check(!runImport(silent, self).ok(), "a command that prints no result line has not succeeded");
    check(!runImport(req, (root / "no-such-tool").string()).started, "a missing tool is reported as not started");

    // ---- the importer on its worker thread -----------------------------------------------------------------------------------------
    AVER_INFO("-- the importer --");
    importer.setToolPath(self);
    notifications().clear();
    req.flags.clear();
    std::string startWhy;
    check(importer.start(0, req, &startWhy), "start() launches the tool: " + startWhy);
    std::string second;
    check(!importer.start(0, req, &second) && contains(second, "already running"), "a second start while it runs is refused");
    check(notifications().size() >= 1, "a progress notification is up while it runs");
    for (int i = 0; i < 600 && importer.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    importer.drawModal(1.0f);   // reaps the finished import
    check(!importer.running() && importer.lastResult().ok() && importer.lastResult().openPath.size() > 0, "it finishes and the result names the file to open");
    std::vector<Notification> shown;
    usize hidden = 0;
    notifications().tick(1.0, shown, hidden);
    bool finished = false;
    for (const Notification& n : shown)
        finished = finished || (n.title == "Import Fake Thing finished" && !n.hasProgress && n.severity == NotifySeverity::Warning && contains(n.body, "10 done, 2 left") && contains(n.body, "2 warning(s)"));
    check(finished, "and the notification became 'Import Fake Thing finished' (a warning: the result counts warnings)");
    check(importer.takeOpenRequest().empty(), "nothing asks the editor to open the result until the user presses the button");
    check(!importer.start(5, req, &startWhy), "an importer index that is gone is refused");

    importer.setToolPath((root / "no-such-tool").string());
    notifications().clear();
    check(importer.start(0, req, &startWhy), "a missing tool still starts a worker (the process fails inside it)");
    for (int i = 0; i < 100 && importer.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    importer.drawModal(1.0f);
    shown.clear();
    hidden = 0;
    notifications().tick(1.0, shown, hidden);
    bool failedNote = false;
    for (const Notification& n : shown) failedNote = failedNote || (n.title == "Import Fake Thing failed" && n.severity == NotifySeverity::Error);
    check(!importer.lastResult().ok() && failedNote, "and the notification became 'Import Fake Thing failed'");

    // ---- cancelling -----------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- cancelling --");
    ImportRequest hang = req;
    hang.inDir = (root / "hang").string();
    {
        ProcessControl control;
        std::atomic<bool> sawLine{false};
        ImportResult cancelled;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread worker([&] { cancelled = runImport(hang, self, [&](const std::string&) { sawLine.store(true); }, &control); });
        for (int i = 0; i < 200 && !sawLine.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        control.cancel();
        worker.join();
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(sawLine.load() && !cancelled.ok() && cancelled.error == "Cancelled." && took < 30.0,
              "a cancel ends a command that would run two minutes, and the result says so (" + std::to_string(took) + " s): " + cancelled.error);
    }
    importer.setToolPath(self);
    notifications().clear();
    check(importer.start(0, hang, &startWhy), "the importer starts the long command: " + startWhy);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    importer.cancel();
    for (int i = 0; i < 300 && importer.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    importer.drawModal(1.0f);
    shown.clear();
    hidden = 0;
    notifications().tick(1.0, shown, hidden);
    bool cancelledNote = false;
    for (const Notification& n : shown) cancelledNote = cancelledNote || (n.title == "Import Fake Thing cancelled" && n.severity == NotifySeverity::Warning);
    check(!importer.running() && cancelledNote, "Cancel ends it and the notification says 'cancelled', not 'failed'");
    const std::string importLog = slurpText((fsys::path(hang.outDir) / "Fake Ext-import.log").string());
    check(importLog.find("fake:") != std::string::npos && importLog.find("[50%] halfway") != std::string::npos,
          "the import's log is in the output folder, its progress lines as [50%] text");

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) fsys::remove_all(root, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
