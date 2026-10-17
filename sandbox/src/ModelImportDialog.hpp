#pragma once
// Editor: model import (FBX and glTF) -- the one writer both formats share, the FBX options panel of the
// Import dialog, the .ocimport record and Reimport. Design: docs/plans/wave9/FBX_IMPORT.md 2.8-2.9.
//
// THE LOGIC HERE IS HEADLESS. Everything above the AVER_WITH_IMGUI line (extension routing, the writer,
// the record, reimport selection) builds and runs with no UI -- tests/editor ModelImportDialogTest
// compiles this file with AVER_WITH_IMGUI undefined, the AnimGraphEditorTest arrangement. Moving a file to
// the recycle bin is the one OS call, and it comes in through setModelImportRecycler so the test needs no
// shell.
#include "aver/core/Types.hpp"
#include "aver/formats/FbxImport.hpp"
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/ImportRecord.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver {

// ---- which importer a file goes to ----

enum class ModelFormat { None, Gltf, Fbx };

struct ModelExtRoute { const char* ext; ModelFormat format; };

// The routing table, lower-case extensions with the dot.
const std::vector<ModelExtRoute>& modelExtensionRoutes();
// By extension, case-insensitively. None for anything else (a plain file copy).
ModelFormat modelFormatOfPath(const std::string& path);
const char* modelFormatName(ModelFormat f);          // "gltf", "fbx", ""

// ---- the shared writer ----

// What an import wrote. Same shape (and the same fields) as the old GltfImportSummary, which is now an
// alias of this.
struct ModelImportSummary {
    u32 meshesWritten = 0, rigsWritten = 0, clipsWritten = 0, materialsWritten = 0, texturesWritten = 0;
    u32 notes = 0;                                   // unsupported features and report lines worth reading
    std::vector<fmt::ImportRecordOutput> outputs;    // every file written, relative to destDir
    std::string recordPath;                          // the .ocimport written, "" for none
};

// Moves a file to the recycle bin. The editor build defaults to editor::moveToRecycleBin; a headless
// build has none until one is set. Replacing a file without one fails the replacement and says so.
using ModelRecycleFn = bool (*)(const std::string& path);
void setModelImportRecycler(ModelRecycleFn fn);

// Writes a parsed import into destDir: materials and textures (cooked under contentDir), one .ocmesh per
// mesh, object clips, a scene .ocworld (when the source had several placements, an object clip or a
// camera), skeletons and clips. glTF and FBX both come through here. Returns false only for a hard failure;
// a clean parse that writes nothing new returns true with an all-zero summary.
bool writeImportedModel(fmt::GltfImportResult& res, const std::string& src, const std::string& destDir,
                        const std::string& contentDir, bool overwrite, ModelImportSummary& out, std::string* why);

// Parse + write an .fbx, and leave `<destDir>/<stem>.ocimport` beside the outputs.
bool importFbxToDir(const std::string& src, const std::string& destDir, const std::string& contentDir,
                    bool overwrite, const fmt::FbxImportOptions& opt, ModelImportSummary& out, std::string* why);

// Defined in ModelImportProvider.cpp (the editor build only): routes by extension to importFbxToDir or
// importGltfToDir. FBX uses the options the Import dialog armed for this path, else the defaults.
bool importModelFile(const std::string& src, const std::string& destDir, const std::string& contentDir,
                     bool overwrite, ModelImportSummary& out, std::string* why);

// ---- the record ----

// A record for an import of `src`: SOURCE project-relative when src is inside projectDir (forward
// slashes), else absolute; size and file time read from the file.
fmt::ImportRecord makeImportRecord(const std::string& format, const std::string& src, const std::string& projectDir,
                                   const std::string& optionsText, const std::vector<fmt::ImportRecordOutput>& outputs);
// Writes `<destDir>/<stem>.ocimport`, replacing the old record but keeping the unknown lines it carried.
bool writeModelImportRecord(const std::string& destDir, const std::string& stem, fmt::ImportRecord rec,
                            std::string* why, std::string* writtenPath = nullptr);
// True when the source file's size or time differs from what the record stored.
bool importSourceChanged(const fmt::ImportRecord& rec, const std::string& projectDir);
// The *.ocimport files in a folder (not recursive), sorted.
std::vector<std::string> listImportRecords(const std::string& dir);

// ---- Reimport ----

struct ReimportTarget {
    std::string recordPath;
    fmt::ImportRecord record;
    std::string sourcePath;          // absolute
    bool sourceExists = false;       // false: the action is disabled and the UI shows sourcePath
};
// For a .ocimport itself, or for any file named by an OUTPUT line of a record in its folder.
bool findReimportTarget(const std::string& filePath, const std::string& projectDir, ReimportTarget& out);
// Re-runs the import with the stored options (or `overrideOpts`) over the same outputs: the old files go to
// the recycle bin through the overwrite path.
bool reimportModel(const ReimportTarget& t, const std::string& contentDir, const fmt::FbxImportOptions* overrideOpts,
                   ModelImportSummary& out, std::string* why);

// ---- the script ABI provider (ModelImportProvider.cpp, the editor build only) ----

// What the provider needs from the editor.
struct ModelImportHost {
    std::function<std::string()> contentDir;                 // the open project's Content folder, "" for none
    std::function<std::string()> projectDir;                 // the open project's folder, "" for none
    std::function<void(const std::string&)> imported;        // after a success: refresh the folder, reload meshes
};
// Installs aver_fw_set_import_provider; runs imports synchronously on the calling thread.
void installModelImportProvider(const ModelImportHost& host);

// ---- the editor hooks (Import dialog, Content Browser) ----

#if AVER_WITH_IMGUI
// Draws the FBX options of the Import dialog when `sourcePath` is an .fbx; nothing otherwise.
void drawFbxImportOptionsPanel(const char* sourcePath);
// Forgets the panel's state (call when the dialog opens).
void modelImportDialogReset();
// Arms the panel's options for the next import of `sourcePath` (the Import button), and disarms after.
void modelImportArmDialogOptions(const char* sourcePath);
void modelImportDisarm();
// importModelFile's lookup: true and the options when armed for exactly this path.
bool modelImportTakeArmedOptions(const std::string& sourcePath, fmt::FbxImportOptions& out);

// The Content Browser context menu: "Reimport" and "Import options..." for a record or an output file.
void modelReimportMenuItems(const std::string& fullPath, const std::string& projectDir, const std::string& contentDir);
// Draws the Import options modal; call once per frame from the Content Browser.
void drawModelImportPopups(const std::string& projectDir, const std::string& contentDir);

struct ModelReimportResult {
    bool ok = false;
    std::string title, body, destDir;
    ModelImportSummary summary;
};
// A finished Reimport (menu item or modal), handed over once.
bool takeModelReimportResult(ModelReimportResult& out);
#endif

} // namespace aver
