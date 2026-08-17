#pragma once
// The editor's Tools menu: the dropdown, the modals behind it, and the two shell-outs
// (`dotnet build`, Explorer / the .csproj handler).
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

// The Tools menu, its modals, the toolbar's Compile C# button, and the builds behind them.
class ToolsMenu {
public:
    ~ToolsMenu();

    // Swaps a freshly-built script assembly in. Returns false when no reload could happen at all.
    using ReloadFn = std::function<bool(const std::string& binDir, std::string* status)>;
    void setReloader(ReloadFn fn) { reload_ = std::move(fn); }

    // Binds the app's auto-compile-on-save flag. Null hides the menu item.
    void setAutoCompileFlag(bool* p) { autoCompile_ = p; }

    // Draws the Tools dropdown. Called from inside BeginMainMenuBar; owns its own BeginMenu.
    void drawMenu(const fmt::ProjectDesc& project);

    // Opens a creation modal from outside the Tools menu; drawModals draws it as usual.
    void openNewCsScript()  { open(Modal::CsScript); }
    void openNewCsClass()   { open(Modal::CsClass); }
    void openNewCppModule() { open(Modal::CppModule); }
    void openNewCppClass()  { open(Modal::CppClass); modules_ = listModules(); moduleSel_ = modules_.empty() ? -1 : 0; }

    // Draws every modal the menu opens. Called once per frame, outside the menu bar.
    void drawModals(const fmt::ProjectDesc& project, f32 dpi);

    // Draws the Compile C# split button: the face builds and reloads, the arrow opens the script
    // items. `iconTex` is the compile-status sprite sheet; 0 falls back to a drawn dot.
    void drawCompileButton(const fmt::ProjectDesc& project, f32 dpi, u64 iconTex);

    // Draws the build/reload/open items into whatever menu or popup is already open.
    void drawScriptItems(const fmt::ProjectDesc& project);

    // Screenshot aids: force a modal, a menu or a build for N frames.
    void armNewScript(bool on) { armScript_ = on ? 4 : 0; }
    void armToolsMenu(bool on) { armMenu_ = on; }
    void armCompileMenu(bool on) { armCompileMenu_ = on; }
    void armCompile(bool on) { armCompile_ = on ? 4 : 0; }
    // --reload-scripts [N]: fires Reload Scripts once, N frames in.
    void armReload(int frames) { armReload_ = frames > 0 ? frames : 20; }

    // Builds and reloads as the toolbar button does: silent on success, Compile modal on failure.
    void triggerToolbarCompile(const fmt::ProjectDesc& project);

    // True while a build is running.
    bool compiling() const { return compileThread_.joinable(); }

private:
    // Which popup drawModals should show.
    enum class Modal { None, CsScript, CsClass, CppModule, CppClass, Compile, Reload };

    void open(Modal m);
    void drawCsModal(const fmt::ProjectDesc& project, f32 dpi, CsKind kind);
    void drawCppModuleModal(f32 dpi);
    void drawCppClassModal(f32 dpi);
    // Draws Compile Scripts, or Reload Scripts when `reload`.
    void drawCompileModal(f32 dpi, bool reload);
    // Reaps a finished build on the main thread: logs it, and runs the reload if this was one.
    void reapCompile();

    // True when `dotnet` is on PATH. Resolved once.
    bool haveDotnet();

    // Starts a `dotnet build` on a worker thread.
    void startCompile(const std::string& csproj, const std::string& outDir, bool reload);

    // Throttled staleness check driving the toolbar light: newest .cs against the last build.
    void refreshScriptStatus(const fmt::ProjectDesc& project);

    // What the toolbar light reports.
    enum class ScriptStatus { NoProject, UpToDate, Stale, Building, Failed };
    ScriptStatus scriptStatus_ = ScriptStatus::NoProject;
    bool  lastBuildFailed_ = false;
    bool  openModalOnFail_ = false;
    bool  haveBuiltStamp_ = false;
    std::filesystem::file_time_type builtStamp_{}; // newest .cs mtime as of the last build we started
    double scanClock_ = -1.0;         // ImGui::GetTime() of the last staleness walk; -1 forces one

    Modal pending_ = Modal::None;
    int  armScript_ = 0;            // frames left to force the New Script modal open
    bool armMenu_ = false;
    bool armCompileMenu_ = false;
    int  armCompile_ = 0;           // frames left to fire the build once
    int  armReload_ = 0;            // frames left before the reload fires
    ReloadFn reload_;
    bool* autoCompile_ = nullptr;
    bool idesLogged_ = false;

    char name_[96] = {};            // shared by all four New ... modals; one at a time is open
    char purpose_[256] = {};        // New C++ Module only
    int  scriptParent_ = 0;         // index into the parent-class picker (0 = AverBehaviour)
    std::string error_, result_;
    std::vector<std::string> madeFiles_;
    std::string cmakeHint_;

    std::vector<ModuleInfo> modules_;
    int moduleSel_ = -1;

    // One off-thread `dotnet build` and everything the modal shows about it.
    struct Compile {
        std::atomic<bool> done{false};
        std::string output;
        std::vector<BuildLine> lines;
        int errors = 0, warnings = 0;
        int exitCode = -1;
        std::string csproj;
        std::string outDir;         // -o passed to dotnet; also where the host is pointed
        bool reload = false;
        bool reloaded = false;
        std::string reloadStatus;
        bool reloadOk = false;
    };
    std::shared_ptr<Compile> compile_;
    std::thread compileThread_;

    int dotnet_ = -1;               // -1 unknown, 0 absent, 1 present
};

} // namespace aver::editor
