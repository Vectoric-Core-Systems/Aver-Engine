#pragma once
// File > Import Godot Project...: runs `AverAssetC fabric` (the Fabric Connector's converter, docs/plans/fabric/G3_SCENES.md) as a
// child process on a worker thread, so the editor stays responsive, and offers to open the converted project.
//
// The part that needs no UI (checking a folder, building the command line, parsing the converter's summary, running it and
// collecting its output) is plain functions over plain data, so FabricImportTest runs it headless against the real tool.
// The modal that drives it lives at the bottom of FabricImport.cpp and is the only part that includes ImGui.
#include "aver/core/Types.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aver::editor {

struct FabricImportRequest {
    std::string godotDir;       // the folder holding project.godot
    std::string outDir;         // the converted project goes to <outDir>/<Name>/
    bool overwrite = false;     // replace a project already converted there
    bool dryRun = false;        // report only
    bool scenesOnly = false;    // leave the scripts unconverted (not offered in the dialog)
};

struct FabricImportResult {
    bool started = false;       // the converter process could be launched
    int exitCode = -1;
    bool haveSummary = false;   // the converter's closing JSON line was seen
    std::string projectDir;     // <outDir>/<Name>
    std::string projectName;
    std::string manifestPath;   // <projectDir>/<Name>.ocproject
    std::string reportPath;     // FabricReport.md
    int levels = 0, converted = 0, approximated = 0, unsupported = 0, scripts = 0, extensions = 0;
    bool scriptsConverted = false;
    bool dryRun = false;
    std::string log;            // everything the converter wrote
    std::string error;          // one line saying why it failed ("" on success)

    bool ok() const { return started && exitCode == 0 && haveSummary; }
};

// ---- the headless half ---------------------------------------------------------------------------------------------------

// True when `dir` holds a Godot 4 project.godot. `why` gets the reason when not.
bool isGodotProjectDir(const std::string& dir, std::string* why = nullptr);

// The project folder for whatever the file dialog returned: a project.godot, or any file or folder inside the project.
// Empty when none of the parents holds a project.godot.
std::string godotProjectDirFrom(const std::string& picked);

// Where the converted project goes by default: a sibling of the Godot folder named "<folder>_Aver".
std::string defaultFabricOutDir(const std::string& godotDir);

// AverAssetC beside the running executable; empty when it is not there.
std::string fabricConverterPath();

// The arguments after the program name: fabric <godotDir> <outDir> [--overwrite] [--dry-run] [--scenes-only].
std::vector<std::string> fabricConverterArgs(const FabricImportRequest& request);

// Reads the converter's output (its summary line, its last error) into a result. `exitCode` is the process's.
FabricImportResult parseFabricOutput(const std::string& output, int exitCode);

// Runs the converter to completion. `onLine` sees each line of its output as it arrives (it may be called from the calling thread
// only). A result with started == false means the tool could not be launched.
FabricImportResult runFabricImport(const FabricImportRequest& request, const std::string& converterPath,
                                   const std::function<void(const std::string&)>& onLine = {});

// ---- the editor half ----------------------------------------------------------------------------------------------------

class FabricImporter {
public:
    ~FabricImporter();

    // Asks the modal to open on the next frame.
    void open() { wantOpen_ = true; }

    // Starts the worker. False (and `why`) when one is already running or the converter is missing.
    bool start(const FabricImportRequest& request, std::string* why);

    bool running() const;

    // Draws the modal (when open) and reaps a finished import. Call every frame, outside the menu bar.
    void drawModal(f32 dpi);

    // The converted project the user chose to open, once (empty otherwise). The editor opens it.
    std::string takeOpenRequest();

    const FabricImportResult& lastResult() const { return last_; }

private:
    struct Job {
        std::atomic<bool> done{false};
        std::mutex lock;
        std::vector<std::string> lines;
        FabricImportResult result;
        u64 notification = 0;
    };

    void reap();

    bool wantOpen_ = false;
    std::shared_ptr<Job> job_;
    std::thread thread_;
    FabricImportResult last_;
    bool haveLast_ = false;
    std::string openRequest_;
    // the modal's fields
    char godotDirBuf_[512] = {};
    char outDirBuf_[512] = {};
    bool outEdited_ = false;
    bool overwrite_ = false;
    bool dryRun_ = false;
    std::string formError_;
    std::vector<std::string> shownLog_;
};

// The one importer the File menu and the shell share.
FabricImporter& fabricImporter();

}  // namespace aver::editor
