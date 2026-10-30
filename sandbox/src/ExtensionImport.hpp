#pragma once
// File menu import entries that extensions add (docs/plans/fabric/EXTENSION.md section 3.5). Each `importers` entry of an installed extension's
// extension.json becomes a menu item and a dialog: pick a folder, warn when its `detect` file is missing, run `AverAssetC <command> <in> <out>`
// as a child process on a worker thread (so the editor stays responsive, and no extension code runs in the editor's process), show its
// progress lines, and offer to open the result.
//
// The part that needs no UI (checking an input, building the command line, reading the command's closing summary, running it and collecting
// its output) is plain functions over plain data, so ExtensionImportTest runs it headless. The modal that drives it lives at the bottom of
// ExtensionImport.cpp and is the only part that includes ImGui.
#include "aver/core/Types.hpp"
#include "aver/extensions/Extensions.hpp"

#include <atomic>
#include <functional>
#include <map>
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
    bool haveSummary = false;           // the command's closing JSON line was seen
    std::string projectDir;             // the summary's "output"
    std::string projectName;            // "project"
    std::string manifestPath;           // <projectDir>/<projectName>.ocproject (what "opens the result")
    std::string reportPath;             // "report"
    bool dryRun = false;                // "dryRun"
    std::map<std::string, std::string> values;   // every key of the summary line, as text ("true", "12", the string)
    std::string log;                    // everything the command wrote
    std::string error;                  // one line saying why it failed ("" on success)

    bool ok() const { return started && exitCode == 0 && haveSummary; }
    // The summary value `key`, "" when absent.
    std::string value(const std::string& key) const;
    // True when the value is "true" or a non-zero number.
    bool truthy(const std::string& key) const;
};

// The wording of one importer with every default filled in (the manifest names only what differs).
struct ImporterText {
    std::string title, tooltip, description, inputLabel, outputLabel, outputSuffix, openLabel, busyText, logTag;
    std::string running, done, checked, failed;
};
ImporterText importerText(const ext::ImporterInfo& info, const std::string& extensionName);

// ---- the headless half ---------------------------------------------------------------------------------------------------

// True when `dir` is a valid input for the importer: a folder holding its `detect` file (whose `detectVersion` key, if it has one, reaches the
// minimum), or an existing file for a "file" importer. `why` gets the reason when not.
bool importInputValid(const ext::ImporterInfo& info, const std::string& dir, std::string* why = nullptr);

// The input for whatever the file dialog returned: the `detect` file, or any file or folder inside the input. Empty when none of the parents
// holds the detect file.
std::string importInputFrom(const ext::ImporterInfo& info, const std::string& picked);

// Where the result goes by default: a sibling of the input folder named "<folder><outputSuffix>".
std::string importDefaultOutDir(const ext::ImporterInfo& info, const std::string& inDir);

// AverAssetC beside the running executable; empty when it is not there.
std::string importToolPath();

// The arguments after the program name: <command> <inDir> <outDir> [flags...].
std::vector<std::string> importArgs(const ImportRequest& request);

// Reads the command's output (its closing summary line `{"schemaVersion":1,"summary":true,"kind":"<command>",...}`, its last error) into a
// result. `exitCode` is the process's.
ImportResult parseImportOutput(const std::string& command, const std::string& output, int exitCode);

// `text` with each {key} replaced by the summary's value.
std::string expandSummary(const std::string& text, const ImportResult& result);

// The notes of the importer that apply to this result.
std::vector<std::string> summaryNotes(const ext::ImporterInfo& info, const ImportResult& result);

// Runs the command to completion. `onLine` sees each line of its output as it arrives (it may be called from the calling thread only). A
// result with started == false means the tool could not be launched.
ImportResult runImport(const ImportRequest& request, const std::string& toolPath, const std::function<void(const std::string&)>& onLine = {});

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

    // The File menu items, one per importer. `track` is told each item's id ("file.import.<command>").
    void drawMenuItems(const std::function<void(const char* id)>& track);

    // Draws the modal (when open) and reaps a finished import. Call every frame, outside the menu bar.
    void drawModal(f32 dpi);

    // The project the user chose to open, once (empty otherwise). The editor opens it.
    std::string takeOpenRequest();

    const ImportResult& lastResult() const { return last_; }

private:
    struct Job {
        std::atomic<bool> done{false};
        std::mutex lock;
        std::vector<std::string> lines;
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
