#pragma once
// Everything the editor writes into a PROJECT folder: the `.ocproject` scaffold behind New Project,
// the C# script/class + `.csproj` behind Tools > New C# Script / New C# Class, and the upgrade an
// older project needs.
#include "aver/formats/OcProject.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// True when `name` is usable as both a directory name and a manifest name.
bool validateProjectName(const std::string& name, std::string* err);

// True when `name` is a legal type name: leading letter or '_', then letters/digits/'_', no keyword.
bool validateTypeName(const std::string& name, std::string* err);

// Writes `text` to `path`, refusing to touch a file that is already there.
bool writeNewFile(const std::string& path, const std::string& text, std::string* err);

// Creates `<location>\<name>\` with the manifest and Content tree. Refuses an existing folder.
bool scaffoldProject(const std::string& location, const std::string& name,
                     fmt::ProjectDesc& out, std::string* err);

// ---------------------------------------------------------------- New Project templates

// One template a New Project can be created from: a display name and description for the picker,
// an optional preview image, and the start map the scaffolded project's manifest should name.
struct TemplateInfo {
    std::string dir;          // absolute path of templates/<id> (or wherever the caller rooted it)
    std::string id;           // the folder name, e.g. "FirstPerson" -- the manifest carries no id
    std::string name;         // NAME -- display text in the picker
    std::string description;  // DESCRIPTION -- one line, rest-of-line free text
    std::string previewPath;  // absolute path to the preview image, or empty (picker draws a fallback)
    std::string startMap;     // STARTMAP, defaulted to "Maps/Default.ocmap" when the manifest omits it
};

// Every valid template found as an immediate subdirectory of `root`: `<sub>\<sub>.octemplate` must
// exist and parse. A subdirectory without one, or whose manifest fails to parse, is skipped and
// logged rather than failing the whole scan -- a missing, empty or entirely-malformed root all
// report zero templates the same way, which is what lets New Project fall back to "blank only"
// instead of erroring. Exposed separately from listTemplates() so a test can hand it a synthetic
// root rather than depending on the real shipped layout.
std::vector<TemplateInfo> listTemplatesIn(const std::string& root);

// listTemplatesIn(), rooted at the templates\ directory found by walking up from the running
// executable -- the same walk engineProjectReference() (see the .cpp) already uses to find
// scripting\csharp, so a shipped editor finds templates\ the identical way it finds its C# sources.
std::vector<TemplateInfo> listTemplates();

// Creates `<location>\<name>\` by copying `tmpl`'s Content tree verbatim and writing a FRESH
// manifest that names the project. Refuses an existing folder, exactly like scaffoldProject().
//
// NOTHING INSIDE THE COPIED TREE IS REWRITTEN: a graph's CLASS name, a level's own NAME, an asset's
// filename are the template's design, not the project's name -- the only thing genuinely name-bearing
// is the manifest this function writes fresh, from `name`. See the .cpp for the full reasoning.
bool scaffoldProjectFromTemplate(const std::string& location, const std::string& name,
                                 const TemplateInfo& tmpl, fmt::ProjectDesc& out, std::string* err);

// What a generated `.cs` derives, which decides the hooks the engine calls.
enum class CsKind {
    Behaviour,         // : AverBehaviour  — OnStart/OnUpdate/OnShutdown, runs today
    Actor,             // : AverActor      — a thing in the world with a transform + lifecycle
    Pawn,              // : AverPawn       — an Actor a controller can possess
    PlayerController,  // : AverPlayerController — input + camera; possesses a pawn
    GameMode,          // : AverGameMode   — per-world rules; names the default pawn/controller
    GameInstance,      // : AverGameInstance — process-wide state that spans levels
    PlainClass,        // no base, no hooks — data, helpers, anything that is just C#
};

// True for anything the engine drives by a lifecycle — Behaviour or any actor kind.
bool csKindIsScript(CsKind kind);

// True for the framework actor kinds, which need a reference to Aver.Framework.
bool csKindIsActor(CsKind kind);

// The human word for a kind, for menu labels and log lines.
const char* csKindNoun(CsKind kind);

// Writes `<project>\Content\Scripts\<name>.cs`, plus the folder's `.csproj` if it is not there yet.
// Never overwrites an existing `.cs`. `outCsproj` is set when this call generated the project file.
bool createScript(const fmt::ProjectDesc& proj, const std::string& name, CsKind kind,
                  std::string* outPath, bool* outCsproj, std::string* err);

// Absolute path of the scripts `.csproj`, whether or not it exists yet.
std::string scriptsCsprojPath(const fmt::ProjectDesc& proj);

// `<project>\Binaries\Scripts` — the one directory a project's script assemblies live in. Both
// `dotnet build -o` and the scripting host are pointed at this string, so it has one owner.
std::string scriptsBinaryDir(const fmt::ProjectDesc& proj);

// One thing an older project is missing, and what would be done about it.
struct ProjectFix {
    enum class Kind {
        CreateFolder,      // a Content subfolder the layout expects
        CreateCsproj,      // no Scripts.csproj at all
        AddReference,      // an engine assembly the .csproj does not name
        AddMaterialsGlob,  // the <Compile Include="..\Materials\**\*.cs" /> item
        RepointReference,  // a ProjectReference whose target does not exist
    };
    Kind kind = Kind::CreateFolder;
    std::string summary;   // one line, for the prompt
    std::string detail;    // the path, or the exact text that would be inserted
};

// Everything `inspectProject` found.
struct ProjectUpgrade {
    std::vector<ProjectFix> fixes;
    bool empty() const { return fixes.empty(); }
};

// Reports what `proj` is missing. Reads only; writes nothing. Empty means the project is current.
ProjectUpgrade inspectProject(const fmt::ProjectDesc& proj);

// Applies everything `inspectProject` found. The `.csproj` is merged, never regenerated. Returns
// false with `err` set on the first failure, having applied whatever came before it.
bool applyProjectUpgrade(const fmt::ProjectDesc& proj, const ProjectUpgrade& up, std::string* err);

// Copies a whole project tree to a NEW sibling folder named for `versionTag`, and returns the path
// of the manifest inside the copy. Empty with `err` set on failure.
//
// NEVER OVERWRITES. If the destination exists the name is numbered until one is free, because the
// entire promise of "work on a copy" is that nothing existing is touched -- silently reusing a
// directory that already holds somebody's project would break exactly the guarantee that makes
// copying the safe choice in the upgrade prompt.
//
// Lives here rather than in the browser because it is project-tree surgery like everything else in
// this file, and because a private member of an ImGui screen cannot be tested. It is the branch of
// the upgrade prompt that MOVES somebody's work, so it is the one that most needs a test.
std::string copyProjectTree(const std::string& manifestPath, const std::string& versionTag,
                            std::string* err);

} // namespace aver::editor
