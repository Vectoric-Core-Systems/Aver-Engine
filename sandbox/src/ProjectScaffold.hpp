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

} // namespace aver::editor
