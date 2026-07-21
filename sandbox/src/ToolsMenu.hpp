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
#include "ProjectScaffold.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aver::editor {

class ToolsMenu {
public:
    ~ToolsMenu();

    // The Tools dropdown. Called from inside BeginMainMenuBar, and owns its own BeginMenu so the
    // `--tools-menu` screenshot aid can force the popup open with the menu bar as parent window.
    void drawMenu(const fmt::ProjectDesc& project);

    // Every modal the menu opens. Called once per frame, outside the menu bar.
    void drawModals(const fmt::ProjectDesc& project, f32 dpi);

    // Screenshot aids, in the family of --project-settings/--start-screen. Opt-in flags only:
    // no oracle gate passes them, and none of them changes the menu BAR, only what hangs off it.
    void armNewScript(bool on) { armScript_ = on ? 4 : 0; }
    void armToolsMenu(bool on) { armMenu_ = on; }
    void armCompile(bool on) { armCompile_ = on ? 4 : 0; }

private:
    enum class Modal { None, CsScript, CsClass, CppModule, CppClass, Compile };

    void open(Modal m);
    void drawCsModal(const fmt::ProjectDesc& project, f32 dpi, CsKind kind);
    void drawCppModuleModal(f32 dpi);
    void drawCppClassModal(f32 dpi);
    void drawCompileModal(f32 dpi);

    // `dotnet` on PATH, resolved once. Absent means Compile Scripts is disabled and says why,
    // rather than spawning nothing and reporting a meaningless exit code.
    bool haveDotnet();

    void startCompile(const std::string& csproj);

    Modal pending_ = Modal::None;   // opened by the menu, consumed by drawModals
    int  armScript_ = 0;            // --new-script: frames left to force the modal open
    bool armMenu_ = false;          // --tools-menu: hold the dropdown open
    int  armCompile_ = 0;           // --compile-scripts: frames left to fire the build once

    char name_[96] = {};            // shared by all four New ... modals; one at a time is open
    char purpose_[256] = {};        // New C++ Module only
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
        int exitCode = -1;
        std::string csproj;
    };
    std::shared_ptr<Compile> compile_;
    std::thread compileThread_;
    int dotnet_ = -1;               // -1 unknown, 0 absent, 1 present
};

} // namespace aver::editor
