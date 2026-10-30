// ExtensionImportTest -- the part of the File menu's extension import dialog that needs no UI (sandbox/src/ExtensionImport.cpp, compiled here
// without ImGui): the importers read from extension.json through the loader, the input checks, the command line, the command's closing summary,
// and the importer running a tool on a worker thread and reporting through the editor's notification queue. The "tool" is this executable
// started as `ExtensionImportTest fakecmd <in> <out> [flags]`, so no extension, no AverAssetC and no .NET are needed. The editor is not run.
#include "EditorNotifications.hpp"
#include "ExtensionImport.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <chrono>
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

static std::string jsonEscape(const std::string& s) {
    std::string r;
    for (const char c : s) { if (c == '\\' || c == '"') r.push_back('\\'); r.push_back(c); }
    return r;
}

// The fake command: what an extension's tool prints.
static int fakeCommand(int argc, char** argv) {
    const std::string in = argc > 2 ? argv[2] : "";
    const std::string out = argc > 3 ? argv[3] : "";
    bool dry = false;
    for (int i = 4; i < argc; ++i) dry = dry || std::strcmp(argv[i], "--dry-run") == 0;
    std::printf("[INFO ] fake: %s -> %s\n", in.c_str(), out.c_str());
    std::printf("{\"schemaVersion\":1,\"input\":\"x\",\"kind\":\"other\",\"ok\":true}\n");
    if (in.find("bad") != std::string::npos) {
        std::printf("[ERROR] fake: it is bad\n");
        return 1;
    }
    std::printf("{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fakecmd\",\"input\":\"%s\",\"output\":\"%s\",\"project\":\"Proj\",\"levels\":3,\"converted\":10,"
                "\"unsupported\":2,\"scripts\":5,\"scriptsConverted\":false,\"report\":\"%s\",\"dryRun\":%s,\"exitCode\":0}\n",
                jsonEscape(in).c_str(), jsonEscape(out + "/Proj").c_str(), jsonEscape(out + "/Report.md").c_str(), dry ? "true" : "false");
    return 0;
}

static const char* kManifest = R"JSON({
  "name": "Fake", "displayName": "Fake Ext", "version": "1.0.0", "abi": 1, "engine": ">=0.1.0",
  "importers": [ {
    "menu": "Import Fake Thing...", "command": "fakecmd", "input": "folder", "detect": "fake.project",
    "detectVersion": { "key": "version=", "min": 3, "message": "Too old: version {value}." },
    "opensResult": true, "tooltip": "Tip", "description": "Describes it.", "inputLabel": "Fake thing", "outputSuffix": "_Out", "logTag": "Fk",
    "openLabel": "Open it", "busyText": "Busy...",
    "options": [ { "flag": "--overwrite", "label": "Replace" }, { "flag": "--dry-run", "label": "Check only", "default": true } ],
    "notify": { "running": "Importing fake", "done": "Fake imported", "checked": "Fake checked", "failed": "Fake failed" },
    "summary": { "text": "{converted} done, {unsupported} left", "brief": "{converted} done", "warnIfNonZero": "unsupported",
                 "notes": [ { "text": "Scripts skipped ({scripts})", "when": "scripts", "unless": "scriptsConverted" },
                            { "text": "never", "when": "nothing" } ] }
  } ]
})JSON";

static const char* kBadManifest = R"JSON({
  "name": "Wrong", "displayName": "Wrong", "version": "1.0.0", "abi": 99, "engine": ">=0.1.0",
  "importers": [ { "menu": "Import Wrong...", "command": "wrongcmd", "input": "folder" } ]
})JSON";

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "fakecmd") == 0) return fakeCommand(argc, argv);

    AVER_INFO("ExtensionImportTest");
    std::error_code ec;
    const fsys::path root = fsys::temp_directory_path() / "aver-extension-import-test";
    fsys::remove_all(root, ec);
    fsys::create_directories(root / "Extensions" / "Fake", ec);
    fsys::create_directories(root / "Extensions" / "Wrong", ec);
    writeFileText((root / "Extensions" / "Fake" / "extension.json").string(), kManifest);
    writeFileText((root / "Extensions" / "Wrong" / "extension.json").string(), kBadManifest);

    // ---- the importers the loader lists ------------------------------------------------------------------------------------------
    AVER_INFO("-- the manifest --");
    ExtensionImporter importer;
    importer.scan((root / "Extensions").string());
    check(importer.entries().size() == 1, "one importer: the refused extension adds none (" + std::to_string(importer.entries().size()) + ")");
    if (importer.entries().size() != 1) { AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures); return exitCode(ExitCode::Failed); }
    const ImporterEntry& entry = importer.entries()[0];
    const ext::ImporterInfo& info = entry.info;
    check(entry.extension == "Fake Ext" && info.menu == "Import Fake Thing..." && info.command == "fakecmd" && info.input == "folder" && info.detect == "fake.project" &&
              info.opensResult,
          "menu, command, input, detect and opensResult");
    check(info.detectKey == "version=" && info.detectMin == 3 && info.options.size() == 2 && info.options[0].flag == "--overwrite" && !info.options[0].onByDefault &&
              info.options[1].onByDefault && info.notes.size() == 2 && info.summaryWarnKey == "unsupported",
          "version check, options and notes");
    check(entry.text.title == "Import Fake Thing" && entry.text.inputLabel == "Fake thing" && entry.text.logTag == "Fk" && entry.text.done == "Fake imported",
          "the title is the menu text without its dots; the wording comes from the manifest");
    {
        ext::ImporterInfo bare;
        bare.menu = "Import Bare...";
        bare.command = "x";
        const ImporterText t = importerText(bare, "Ext Name");
        check(t.title == "Import Bare" && t.inputLabel == "Input" && t.outputSuffix == "_Imported" && t.logTag == "Ext Name" && t.busyText == "Working..." && t.failed == "Import failed",
              "an importer that names no wording gets the defaults");
    }

    // ---- choosing the input -------------------------------------------------------------------------------------------------------
    AVER_INFO("-- choosing the input --");
    const fsys::path good = root / "good";
    const fsys::path old = root / "old";
    fsys::create_directories(good / "sub", ec);
    fsys::create_directories(old, ec);
    writeFileText((good / "fake.project").string(), "version=4\n");
    writeFileText((old / "fake.project").string(), "name=x\nversion=2\n");
    std::string why;
    check(importInputValid(info, good.string(), &why), "a folder with the detect file is accepted: " + why);
    check(!importInputValid(info, "", &why) && why == "Choose the folder of a Fake thing.", "an empty path is refused with a reason: " + why);
    check(!importInputValid(info, root.string(), &why) && contains(why, "fake.project"), "a folder without the detect file is refused: " + why);
    check(!importInputValid(info, old.string(), &why) && why == "Too old: version 2.", "a file below the minimum is refused with the manifest's message: " + why);
    check(fsys::path(importInputFrom(info, (good / "fake.project").string())) == good, "the detect file gives its folder");
    check(fsys::path(importInputFrom(info, (good / "sub").string())) == good, "a folder inside the input gives the input folder");
    check(importInputFrom(info, (root / "nowhere" / "file.txt").string()).empty(), "a path outside any input gives nothing");
    check(fsys::path(importDefaultOutDir(info, "C:/games/My Game")).filename() == "My Game_Out" && fsys::path(importDefaultOutDir(info, "C:/games/My Game/")).filename() == "My Game_Out",
          "the default output is a sibling named <folder><suffix>");
    {
        ext::ImporterInfo file;
        file.menu = "Import File...";
        file.command = "f";
        file.input = "file";
        check(importInputValid(file, (good / "fake.project").string(), &why), "a file importer accepts an existing file");
        check(!importInputValid(file, good.string(), &why) && !why.empty(), "and refuses a folder: " + why);
        check(importInputFrom(file, "C:/a/b.txt") == "C:/a/b.txt", "its picked path is the input");
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
    AVER_INFO("-- the closing summary --");
    {
        const std::string out =
            "[INFO ] x: a -> b\n"
            "{\"schemaVersion\":1,\"input\":\"x.gltf\",\"kind\":\"mesh\",\"ok\":true}\n"
            "{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fakecmd\",\"input\":\"in\",\"output\":\"C:\\\\Out\\\\Game\",\"project\":\"Game\",\"levels\":3,\"converted\":10,"
            "\"unsupported\":2,\"scripts\":5,\"scriptsConverted\":true,\"report\":\"C:\\\\Out\\\\Report.md\",\"dryRun\":false,\"exitCode\":0}\n";
        const ImportResult r = parseImportOutput("fakecmd", out, 0);
        check(r.ok() && r.projectName == "Game" && r.projectDir == "C:\\Out\\Game" && r.reportPath == "C:\\Out\\Report.md", "the project, folder and report path are read (escapes decoded)");
        check(r.value("converted") == "10" && r.value("levels") == "3" && r.truthy("scriptsConverted") && !r.truthy("dryRun") && r.value("nothing").empty(), "and every key as text");
        check(r.manifestPath == "C:\\Out\\Game/Game.ocproject", "the manifest to open is <project>/<Name>.ocproject");
        check(expandSummary("{converted} done, {unsupported} left; {missing}.", r) == "10 done, 2 left; .", "{key} in the wording becomes the summary's value");
        check(summaryNotes(info, r).empty(), "no note when the scripts were converted");
        check(!parseImportOutput("othercmd", out, 0).ok(), "a summary of another command is not this command's summary");
        const ImportResult f = parseImportOutput("fakecmd", "[INFO ] x\n[ERROR] x: it already holds a project; pass --overwrite\n", 1);
        check(!f.ok() && contains(f.error, "pass --overwrite") && f.manifestPath.empty(), "a failed run carries the command's last error line");
        const ImportResult n = parseImportOutput("fakecmd", "nothing useful\n", 0);
        check(!n.ok() && !n.error.empty(), "a clean exit without a summary is not a success");
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
    check(direct.ok() && direct.projectName == "Proj" && direct.value("levels") == "3", "runImport reads the summary: " + direct.error);
    check(!seen.empty() && contains(seen.front(), "fake:") && seen.size() == 3, "its lines were handed over as they arrived (" + std::to_string(seen.size()) + ")");
    {
        const std::vector<std::string> notes = summaryNotes(info, direct);
        check(notes.size() == 1 && notes[0] == "Scripts skipped (5)", "the note shows when scripts exist and were not converted");
        check(expandSummary(info.summaryText, direct) == "10 done, 2 left", "the summary line is built from the manifest's text");
    }
    req.flags = {"--dry-run"};
    const ImportResult dry = runImport(req, self);
    check(dry.ok() && dry.dryRun && dry.manifestPath.empty(), "a dry run has nothing to open");
    ImportRequest bad = req;
    bad.inDir = (root / "bad").string();
    const ImportResult failed = runImport(bad, self);
    check(!failed.ok() && contains(failed.error, "it is bad"), "a failing run reports the tool's error: " + failed.error);
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
    check(!importer.running() && importer.lastResult().ok() && importer.lastResult().manifestPath.size() > 0, "it finishes and the result names the project to open");
    std::vector<Notification> shown;
    usize hidden = 0;
    notifications().tick(1.0, shown, hidden);
    bool finished = false;
    for (const Notification& n : shown) finished = finished || (n.title == "Fake imported" && !n.hasProgress && n.severity == NotifySeverity::Warning && contains(n.body, "10 done"));
    check(finished, "and the notification became 'Fake imported' (a warning: the summary has unsupported items)");
    check(importer.takeOpenRequest().empty(), "nothing asks the editor to open the project until the user presses the button");
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
    for (const Notification& n : shown) failedNote = failedNote || (n.title == "Fake failed" && n.severity == NotifySeverity::Error);
    check(!importer.lastResult().ok() && failedNote, "and the notification became 'Fake failed'");

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) fsys::remove_all(root, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
