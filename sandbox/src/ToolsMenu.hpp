#pragma once
// The editor's Tools menu: the dropdown itself, every modal behind it, and the two shell-outs
// (`dotnet build`, Explorer / the .csproj handler).
//
// A whole file for one menu because SandboxApp.cpp is the editor's frame loop and does not need
// six more modals in it — and because the menu's job is precisely to keep two asymmetric things
// straight. C# items write into the PROJECT and need no rebuild; C++ items write into the ENGINE
// and do. Everything here exists to make that visible rather than something a user discovers by
// looking for a class that is not where Unreal would have put it.
#include "aver/formats/OcProject.hpp"
#include "aver/core/Types.hpp"

#include "EngineScaffold.hpp"
#include "IdeIntegration.hpp"
#include "ProjectScaffold.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aver::editor {

class ToolsMenu {
public:
    ~ToolsMenu();

    // How the editor swaps a freshly-built script assembly in: unload the old load context, load
    // `binDir`, and put a human-readable outcome in `status`. Returns false when the reload could
    // not happen at all (no host).
    //
    // A CALLBACK rather than a ScriptHost reference on purpose. This file already knows about the
    // project scaffold and two shell-outs; giving it the scripting module as well would mean an
    // `#if AVER_MODULE_SCRIPTING` in the menu, and a build with scripting off would need the menu
    // edited to compile. The app owns the host and installs this; with nothing installed the two
    // reload paths are disabled and say why.
    using ReloadFn = std::function<bool(const std::string& binDir, std::string* status)>;
    void setReloader(ReloadFn fn) { reload_ = std::move(fn); }

    // The Tools dropdown. Called from inside BeginMainMenuBar, and owns its own BeginMenu so the
    // `--tools-menu` screenshot aid can force the popup open with the menu bar as parent window.
    void drawMenu(const fmt::ProjectDesc& project);

    // Open a creation modal from OUTSIDE the Tools menu — the Content Browser's "Add" button routes here,
    // so the creation flows live in one place and the menu and the browser share them. The modal itself is
    // drawn by drawModals as usual.
    void openNewCsScript()  { open(Modal::CsScript); }
    void openNewCsClass()   { open(Modal::CsClass); }
    void openNewCppModule() { open(Modal::CppModule); }
    void openNewCppClass()  { open(Modal::CppClass); modules_ = listModules(); moduleSel_ = modules_.empty() ? -1 : 0; }

    // Every modal the menu opens. Called once per frame, outside the menu bar.
    void drawModals(const fmt::ProjectDesc& project, f32 dpi);

    // The toolbar's "Compile C#" button, in the spirit of UEFN's Build Verse: one click rebuilds the
    // project's scripts and hot-swaps them in with no editor restart, and the status ICON on the
    // button's own face says whether what is on disk has been built. Silent on success — the Compile
    // Scripts modal only pops if the build fails. `iconTex` is the UI id of the compile-status sprite
    // sheet (three tiles: built / failed / stale); 0 falls back to a drawn dot. Call in the menu-bar.
    void drawCompileButton(const fmt::ProjectDesc& project, f32 dpi, u64 iconTex);

    // Screenshot aids, in the family of --project-settings/--start-screen. Opt-in flags only:
    // no oracle gate passes them, and none of them changes the menu BAR, only what hangs off it.
    void armNewScript(bool on) { armScript_ = on ? 4 : 0; }
    void armToolsMenu(bool on) { armMenu_ = on; }
    void armCompile(bool on) { armCompile_ = on ? 4 : 0; }
    // --reload-scripts [N]: fire Reload Scripts once, N frames in. A COUNTDOWN rather than an
    // immediate shot, because the whole point of a reload is that it happens to a running editor
    // that already loaded something — firing it on frame 0 would prove nothing that init does not.
    void armReload(int frames) { armReload_ = frames > 0 ? frames : 20; }

    // Build + reload, as the toolbar button does it: a clean build stays silent and a failed one
    // opens the Compile modal on the errors.
    //
    // PUBLIC because it is no longer only the toolbar's. The actor editor's tab carries a Compile C#
    // button too -- editing an actor is editing C#, and a tab you have to leave to build is a tab
    // that does half a job. Exposing the action is better than each panel growing its own copy of
    // the build logic.
    void triggerToolbarCompile(const fmt::ProjectDesc& project);

    // Whether a build is running, so another panel can disable its own button rather than starting a
    // second job that startCompile would silently drop on the floor.
    bool compiling() const { return compileThread_.joinable(); }

private:
    enum class Modal { None, CsScript, CsClass, CppModule, CppClass, Compile, Reload };

    void open(Modal m);
    void drawCsModal(const fmt::ProjectDesc& project, f32 dpi, CsKind kind);
    void drawCppModuleModal(f32 dpi);
    void drawCppClassModal(f32 dpi);
    // One body, two popups: Compile Scripts and Reload Scripts differ by one step after the build
    // and by every line of explanation around it, and nothing else.
    void drawCompileModal(f32 dpi, bool reload);
    // Reap a finished build: log it, and run the reload if this was one. Called every frame from
    // drawModals so the swap happens on the MAIN thread — behaviours are constructed and their
    // hooks called there, and the build ran on another.
    void reapCompile();

    // `dotnet` on PATH, resolved once. Absent means Compile Scripts is disabled and says why,
    // rather than spawning nothing and reporting a meaningless exit code.
    bool haveDotnet();

    void startCompile(const std::string& csproj, const std::string& outDir, bool reload);

    // Throttled staleness check driving the toolbar light: newest .cs against the last build.
    void refreshScriptStatus(const fmt::ProjectDesc& project);

    // What the toolbar light reports. Building is transient; NoProject greys the button out. The
    // three the user asked for map straight on: UpToDate -> green tick, Stale -> yellow question,
    // Failed -> red no-entry.
    enum class ScriptStatus { NoProject, UpToDate, Stale, Building, Failed };
    ScriptStatus scriptStatus_ = ScriptStatus::NoProject;
    bool  lastBuildFailed_ = false;   // set by reapCompile; what tells Failed (red) from Stale (yellow)
    bool  openModalOnFail_ = false;   // the toolbar path sets it; reapCompile consumes it once
    bool  haveBuiltStamp_ = false;    // a build has run this session, so builtStamp_ is meaningful
    std::filesystem::file_time_type builtStamp_{}; // newest .cs mtime as of the last build we started
    double scanClock_ = -1.0;         // ImGui::GetTime() of the last staleness walk; -1 forces one

    Modal pending_ = Modal::None;   // opened by the menu, consumed by drawModals
    int  armScript_ = 0;            // --new-script: frames left to force the modal open
    bool armMenu_ = false;          // --tools-menu: hold the dropdown open
    int  armCompile_ = 0;           // --compile-scripts: frames left to fire the build once
    int  armReload_ = 0;            // --reload-scripts: frames left before the reload fires
    ReloadFn reload_;               // empty in a build with no scripting host
    bool idesLogged_ = false;       // the detected-IDE list is logged once, when the scan lands

    char name_[96] = {};            // shared by all four New ... modals; one at a time is open
    char purpose_[256] = {};        // New C++ Module only
    int  scriptParent_ = 0;         // New C# Script: index into the parent-class picker (0 = AverBehaviour)
    std::string error_, result_;
    std::vector<std::string> madeFiles_;
    std::string cmakeHint_;         // the line the user must add by hand, if any

    std::vector<ModuleInfo> modules_; // refreshed when New C++ Class opens
    int moduleSel_ = -1;

    // Compile Scripts runs off-thread: `dotnet build` takes seconds, and a frozen editor looks
    // like a hang rather than a build.
    struct Compile {
        std::atomic<bool> done{false};
        std::string output;
        // The same transcript, one entry per line, with the diagnostics among them parsed into a
        // file/line/column that can be clicked. Built on the BUILD thread beside `output` and
        // published by the same `done` store — parsing a few hundred lines is cheap, but doing it
        // in the draw call would redo it every frame the modal is open.
        std::vector<BuildLine> lines;
        int errors = 0, warnings = 0;
        int exitCode = -1;
        std::string csproj;
        std::string outDir;         // -o passed to dotnet; also where the host is pointed
        bool reload = false;        // swap the result in once the build succeeds
        bool reloaded = false;      // the swap has been attempted (once, on the main thread)
        std::string reloadStatus;   // what the host said about it, shown in the modal
        bool reloadOk = false;
    };
    std::shared_ptr<Compile> compile_;
    std::thread compileThread_;
    int dotnet_ = -1;               // -1 unknown, 0 absent, 1 present
};

} // namespace aver::editor
