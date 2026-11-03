#pragma once
// File menu import entries that extensions add (docs/EXTENSIONS.md, "The editor"). Each `importers` entry of an installed extension's
// extension.json becomes a menu item and a dialog: pick the input, run `AverAssetC <command> <in> <out> [flags]` as a child process on a worker
// thread (so the editor stays responsive, and no extension code runs in the editor's process), and show what the command reports through the
// command protocol (aver_ext_protocol.h): a progress bar, its log lines, and its result message with the warning count and the Open and Report
// buttons. The dialog knows no command: everything it shows is the manifest's menu text and tooltip or the command's own lines.
//
// The part that needs no UI (checking an input, building the command line, reading the command's protocol lines, running it and collecting
// its output) is plain functions over plain data, so ExtensionImportTest runs it headless. The modal that drives it lives at the bottom of
// ExtensionImport.cpp and is the only part that includes ImGui.
#include "aver/core/Types.hpp"
#include "ProcessStream.hpp"
#include "aver/extensions/Extensions.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aver::editor {

struct ImportRequest {
    std::string command;                // the extension command AverAssetC is asked to run
    std::string inDir;                  // the folder (or file) to import
    std::string outDir;                 // where the command writes
    std::vector<std::string> flags;     // the ticked options' flags, appended after the paths
};

struct ImportResult {
    bool started = false;               // the AverAssetC process could be launched
    int exitCode = -1;
    bool haveResult = false;            // the command's result line was seen
    bool succeeded = false;             // that line's ok
    std::string openPath;               // its "open": the file the Open button opens
    std::string reportPath;             // its "report"
    std::string message;                // its "message"
    int warnings = 0;
    int errors = 0;
    double fraction = -1.0;             // the last progress line's fraction (-1: unknown) and text
    std::string progressText;
    std::string log;                    // everything the command wrote
    std::string error;                  // one line saying why it failed ("" on success)

    bool ok() const { return started && exitCode == 0 && haveResult && succeeded; }
};

// The wording of one importer with every default filled in.
struct ImporterText {
    std::string title, tooltip, description;
};
ImporterText importerText(const ext::ImporterInfo& info);

// ---- the headless half ---------------------------------------------------------------------------------------------------

// True when `input` is valid for the importer: a folder (holding its `input.detect` file, if it names one), or an existing file for a "file"
// importer whose name matches `input.patterns` (when it lists any). `why` gets the reason when not.
bool importInputValid(const ext::ImporterInfo& info, const std::string& input, std::string* why = nullptr);

// The input for whatever the file dialog returned: for a "file" importer the file itself, else the folder holding it (the nearest parent that
// holds the `detect` file when there is one; empty when none does).
std::string importInputFrom(const ext::ImporterInfo& info, const std::string& picked);

// The pattern list the file dialog is given: `input.patterns`, else the `detect` file, else everything ("*.*").
std::string importPickerSpec(const ext::ImporterInfo& info);

// Where the result goes by default: beside the input, named by `outputDefault` ("{input}_out") with {input} the input's folder or file stem.
std::string importDefaultOutDir(const ext::ImporterInfo& info, const std::string& input);

// AverAssetC beside the running executable; empty when it is not there.
std::string importToolPath();

// The arguments after the program name: <command> <inDir> <outDir> [flags...].
std::vector<std::string> importArgs(const ImportRequest& request);

// Reads the command's output (its protocol lines, its last error) into a result. `exitCode` is the process's.
ImportResult parseImportOutput(const std::string& output, int exitCode);

// Runs the command to completion. `onLine` sees each line of its output as it arrives (it may be called from the calling thread only). A
// result with started == false means the tool could not be launched. `control` can cancel it from another thread; the result then says so.
ImportResult runImport(const ImportRequest& request, const std::string& toolPath, const std::function<void(const std::string&)>& onLine = {},
                       ProcessControl* control = nullptr);

// ---- the editor half ----------------------------------------------------------------------------------------------------

struct ImporterEntry {
    std::string extension;       // the extension's display name
    ext::ImporterInfo info;
    ImporterText text;
};

class ExtensionImporter {
public:
    ~ExtensionImporter();

    // Reads the importers of the Ready extensions under `root` (default: <executable dir>/Extensions). Replaces what was read before.
    void scan(const std::string& root = {});
    // scan() on first use only.
    const std::vector<ImporterEntry>& entries();

    // The program that runs the commands: AverAssetC beside the executable unless a test points it elsewhere.
    void setToolPath(std::string path) { toolOverride_ = std::move(path); }
    std::string toolPath() const { return toolOverride_.empty() ? importToolPath() : toolOverride_; }

    // Asks the modal to open on the next frame for entry `index`.
    void open(usize index);

    // Starts the worker for entry `index`. False (and `why`) when one is already running or AverAssetC is missing.
    bool start(usize index, const ImportRequest& request, std::string* why);

    bool running() const;

    // Ends the running import, with everything it started (Cancel in the window). Nothing when none is running.
    void cancel();

    // The File menu items, one per importer. `track` is told each item's id ("file.import.<importer id>").
    void drawMenuItems(const std::function<void(const char* id)>& track);

    // The start screen's buttons, one per importer that names a `startButton`, each `width` wide.
    void drawStartButtons(f32 width, f32 height);

    // Draws the modal (when open) and reaps a finished import. Call every frame, outside the menu bar.
    void drawModal(f32 dpi);

    // The file the user chose to open from the result, once (empty otherwise). The editor opens it.
    std::string takeOpenRequest();

    const ImportResult& lastResult() const { return last_; }

private:
    struct Job {
        std::atomic<bool> done{false};
        std::mutex lock;
        std::vector<std::string> lines;
        double fraction = -1.0;       // the last progress line, under `lock`
        std::string progressText;
        // When it started and when the command last printed anything, under `lock`: a long silence is shown, so a slow step and a hang can be told apart.
        std::chrono::steady_clock::time_point started, lastOutput;
        std::string logPath;          // <output folder>/<extension>-import.log: every line the command printed, kept after the editor closes
        ProcessControl control;
        ImportResult result;
        u64 notification = 0;
        ImporterEntry entry;
    };
    struct Form {
        char inBuf[512] = {};
        char outBuf[512] = {};
        bool outEdited = false;
        std::vector<char> optionOn;
        std::string error;
    };

    void reap();

    bool scanned_ = false;
    std::string toolOverride_;
    std::vector<ImporterEntry> entries_;
    std::vector<Form> forms_;
    usize wantOpen_ = static_cast<usize>(-1);
    usize active_ = 0;                 // the entry the modal shows
    usize lastEntry_ = static_cast<usize>(-1);
    std::shared_ptr<Job> job_;
    std::thread thread_;
    ImportResult last_;
    bool haveLast_ = false;
    std::string openRequest_;
    std::vector<std::string> shownLog_;
};

// The one importer the File menu and the shell share.
ExtensionImporter& extensionImporter();

}  // namespace aver::editor
