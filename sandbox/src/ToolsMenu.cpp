// The Tools menu and the toolbar's Compile C# split button: builds and reloads a project's C#,
// bakes its materials, scaffolds new scripts/classes/modules, and reports build diagnostics.

#include "ToolsMenu.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"

#include <cmath>

#include <filesystem>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace aver::editor {
namespace {

#if defined(_WIN32)

// UTF-8 to UTF-16.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Child-process bytes to UTF-8, re-reading as the OEM code page when they are not already UTF-8.
std::string toUtf8(const std::string& raw) {
    if (raw.empty()) return raw;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.c_str(),
                            static_cast<int>(raw.size()), nullptr, 0) > 0)
        return raw;

    const UINT cp = GetOEMCP();
    const int n = MultiByteToWideChar(cp, 0, raw.c_str(), static_cast<int>(raw.size()), nullptr, 0);
    if (n <= 0) return raw;
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(cp, 0, raw.c_str(), static_cast<int>(raw.size()), w.data(), n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return raw;
    std::string out(static_cast<usize>(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, out.data(), m, nullptr, nullptr);
    return out;
}

// Runs a command line to completion, capturing stdout and stderr down one pipe into `out`.
bool runCaptured(const std::wstring& cmdline, const std::wstring& cwd, std::string& out, int& exitCode) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;   // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                   &si, &pi);
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }

    std::string raw;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) raw.append(buf, got);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    out = toUtf8(raw);
    exitCode = static_cast<int>(code);
    return true;
}

// True if an executable is on PATH.
bool findOnPath(const wchar_t* exe) {
    wchar_t found[MAX_PATH];
    return SearchPathW(nullptr, exe, L".exe", MAX_PATH, found, nullptr) > 0;
}

// Hands a path to the shell's default handler.
bool shellOpen(const std::string& path) {
    const std::wstring w = widen(path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;   // ShellExecute's documented success threshold
}

#else // !_WIN32

bool runCaptured(const std::wstring&, const std::wstring&, std::string&, int&) { return false; }
bool findOnPath(const wchar_t*) { return false; }
bool shellOpen(const std::string&) { return false; }

#endif

// Runs avermatc over the assembly in `scriptsOutDir`, writing .ocmat files beside it under
// Binaries\Materials. False when the tool or the assembly is absent, which is not an error.
bool bakeMaterials(const std::string& scriptsOutDir, std::string& log, int& exitCode) {
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path binaries = fs::path(scriptsOutDir).parent_path();
    const fs::path assembly = fs::path(scriptsOutDir) / "Scripts.dll";
    if (!fs::exists(assembly, ec)) { log = "no Scripts.dll to read materials from"; return false; }

#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    const fs::path tool = fs::path(exe).parent_path() / "Tools" / "avermatc.dll";
    if (!fs::exists(tool, ec)) return false;

    const fs::path outDir = binaries / "Materials";
    fs::create_directories(outDir, ec);

    const std::wstring cmd = L"dotnet \"" + tool.wstring() + L"\" --assembly \"" +
                             assembly.wstring() + L"\" --out \"" + outDir.wstring() + L"\"";
    if (!runCaptured(cmd, fs::path(exe).parent_path().wstring(), log, exitCode)) {
        log = "could not start dotnet to bake materials";
        exitCode = -1;
        return true;
    }
    return true;
#else
    (void)binaries; (void)exitCode;
    log = "material baking is implemented for Windows only";
    return false;
#endif
}

#if AVER_WITH_IMGUI
// A tooltip on the last item that shows even when that item is disabled.
void tip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

// Draws what a generated C# file can and cannot reach at runtime.
void explainScriptReach(bool behaviour) {
    ImGui::PushTextWrapPos(0.0f);
    if (behaviour) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1),
            "This WILL run. Tools > Reload Scripts builds it into <project>\\Binaries\\Scripts and "
            "loads it into the running editor - no restart. The hooks are called on the main "
            "thread from the frame loop.");
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1),
            "This compiles into the same assembly as the project's behaviours and is loaded with "
            "them. It has no hooks, so the engine never calls it by itself.");
    }
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1),
        "A script can reach the log and the render modules' live settings and GPU capabilities "
        "(Voxi, Pbr). It CANNOT reach the scene: there are no actor, transform, input or asset "
        "APIs yet, because the generic scene layer is unbuilt (docs/STATUS.md \xC2\xA7""9.1).");
    ImGui::PopTextWrapPos();
}

void showError(const std::string& e)  { if (!e.empty()) ImGui::TextColored(ImVec4(0.93f,0.42f,0.38f,1), "%s", e.c_str()); }
void showResult(const std::string& r) { if (!r.empty()) ImGui::TextColored(ImVec4(0.45f,0.85f,0.45f,1), "%s", r.c_str()); }

// Shows the one build line the user must add by hand, with a copy-to-clipboard button.
void showCmakeHint(const char* what, const std::string& line, f32 dpi) {
    if (line.empty()) return;
    ImGui::Spacing();
    ImGui::TextUnformatted(what);
    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1), "%s", line.c_str());
    if (ImGui::Button("Copy line", ImVec2(110.0f * dpi, 0))) ImGui::SetClipboardText(line.c_str());
}
#endif

} // namespace

// Joins any build thread still running.
ToolsMenu::~ToolsMenu() {
    if (compileThread_.joinable()) compileThread_.join();
}

// Queues a modal to open next frame and clears the previous one's state.
void ToolsMenu::open(Modal m) {
    pending_ = m;
    name_[0] = '\0';
    error_.clear();
    result_.clear();
    madeFiles_.clear();
    cmakeHint_.clear();
}

// True if `dotnet` is on PATH. Probed once and cached.
bool ToolsMenu::haveDotnet() {
    if (dotnet_ < 0) dotnet_ = findOnPath(L"dotnet") ? 1 : 0;
    return dotnet_ == 1;
}

// ---------------------------------------------------------------------------------------------
// the dropdown
// ---------------------------------------------------------------------------------------------

// Draws the Tools dropdown in the menu bar.
void ToolsMenu::drawMenu(const fmt::ProjectDesc& project) {
#if !AVER_WITH_IMGUI
    (void)project;
#else
    // --tools-menu (screenshot aid). Re-issued every frame: the popup ID belongs to this window.
    if (armMenu_) ImGui::OpenPopup("Tools");

    if (!ImGui::BeginMenu("Tools")) return;

    drawScriptItems(project);

    ImGui::EndMenu();
#endif
}

// ---------------------------------------------------------------------------------------------
// modals
// ---------------------------------------------------------------------------------------------

// Reaps a finished build, starts IDE detection, and draws every modal this menu owns. Runs every
// frame whether or not anything is open.
void ToolsMenu::drawModals(const fmt::ProjectDesc& project, f32 dpi) {
    reapCompile();
    const std::vector<IdeInfo>& ides = detectedIdes();
    if (!idesLogged_ && ideDetectionFinished()) {
        idesLogged_ = true;
        for (const IdeInfo& i : ides)
            AVER_INFO("[Editor] IDE detected: {} ({})", i.name,
                      i.exePath.empty() ? std::string("shell association") : i.exePath);
    }
#if !AVER_WITH_IMGUI
    (void)project; (void)dpi;
#else
    // --new-script (screenshot aid), gated on the same predicate as the menu item.
    if (armScript_ > 0) { if (project.valid() && pending_ == Modal::None) open(Modal::CsScript); --armScript_; }

    // --compile-scripts and --reload-scripts, gated on exactly what enables their menu items.
    const auto fireBuild = [&](int& arm, bool reload) {
        if (arm <= 0) return;
        const std::string csproj = project.valid() ? scriptsCsprojPath(project) : std::string();
        const bool ready = !csproj.empty() && fileExists(csproj) && haveDotnet() &&
                           !compileThread_.joinable() && (!reload || reload_ != nullptr);
        if (ready && arm == 1) {
            open(reload ? Modal::Reload : Modal::Compile);
            startCompile(csproj, scriptsBinaryDir(project), reload);
        }
        --arm;
    };
    fireBuild(armCompile_, false);
    fireBuild(armReload_, true);

    switch (pending_) {
        case Modal::CsScript:  ImGui::OpenPopup("New C# Script"); break;
        case Modal::CsClass:   ImGui::OpenPopup("New C# Class"); break;
        case Modal::CppModule: ImGui::OpenPopup("New C++ Module"); break;
        case Modal::CppClass:  ImGui::OpenPopup("New C++ Class"); break;
        case Modal::Compile:   ImGui::OpenPopup("Compile Scripts"); break;
        case Modal::Reload:    ImGui::OpenPopup("Reload Scripts"); break;
        case Modal::None:      break;
    }
    pending_ = Modal::None;

    drawCsModal(project, dpi, CsKind::Behaviour);
    drawCsModal(project, dpi, CsKind::PlainClass);
    drawCppModuleModal(dpi);
    drawCppClassModal(dpi);
    drawCompileModal(dpi, false);
    drawCompileModal(dpi, true);
#endif
}

#if AVER_WITH_IMGUI
// Draws the New C# Script / New C# Class modal, including the parent-class picker.
void ToolsMenu::drawCsModal(const fmt::ProjectDesc& project, f32 dpi, CsKind kind) {
    const bool behaviour = kind == CsKind::Behaviour;
    const char* title = behaviour ? "New C# Script" : "New C# Class";

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mv->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600.0f * dpi, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize)) return;

    ImGui::TextDisabled("PROJECT-side. Adding this needs no engine rebuild.");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted(behaviour ? "Script name" : "Class name");
    ImGui::PushItemWidth(-1);
    const bool submitted = ImGui::InputText("##csname", name_, sizeof name_,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::TextDisabled("Becomes a C# class, so: letters, digits and underscores only.");

    // One row of the parent-class picker.
    struct ParentOption { CsKind kind; const char* label; const char* blurb; };
    static const ParentOption kParents[] = {
        { CsKind::Behaviour,        "AverBehaviour",    "Raw hooks (OnStart/OnUpdate). The one that RUNS today." },
        { CsKind::Actor,            "Actor",            "A thing in the world with a transform and a lifecycle." },
        { CsKind::Pawn,             "Pawn",             "An Actor a controller can possess." },
        { CsKind::PlayerController, "PlayerController", "Input + camera; possesses a Pawn." },
        { CsKind::GameMode,         "GameMode",         "Per-world rules; names the default pawn + controller." },
        { CsKind::GameInstance,     "GameInstance",     "Process-wide state; survives a level change." },
    };
    CsKind effectiveKind = CsKind::PlainClass;
    if (behaviour) {
        if (scriptParent_ < 0 || scriptParent_ >= (int)IM_ARRAYSIZE(kParents)) scriptParent_ = 0;
        ImGui::Spacing();
        ImGui::TextUnformatted("Parent class");
        ImGui::PushItemWidth(-1);
        if (ImGui::BeginCombo("##parent", kParents[scriptParent_].label)) {
            for (int i = 0; i < (int)IM_ARRAYSIZE(kParents); ++i) {
                const bool sel = scriptParent_ == i;
                if (ImGui::Selectable(kParents[i].label, sel)) scriptParent_ = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kParents[i].blurb);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        effectiveKind = kParents[scriptParent_].kind;
        ImGui::TextDisabled("%s", kParents[scriptParent_].blurb);
    } else {
        ImGui::TextDisabled("A plain class - no lifecycle hooks.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Writes to %s", project.scriptsDir().c_str());

    ImGui::Spacing();
    ImGui::Separator();
    if (csKindIsActor(effectiveKind)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.25f, 1));
        ImGui::TextWrapped("This compiles, but does NOT tick yet - the actor runtime (per-frame tick, "
                           "spawning, possession) is still being built. AverBehaviour scripts run today.");
        ImGui::PopStyleColor();
    } else {
        explainScriptReach(behaviour);
    }
    ImGui::Separator();

    showError(error_);
    showResult(result_);

    ImGui::Spacing();
    const bool create = ImGui::Button(behaviour ? "Create Script" : "Create Class", ImVec2(150.0f * dpi, 0));
    if (create || submitted) {
        error_.clear(); result_.clear();
        std::string path; bool madeCsproj = false;
        if (createScript(project, name_, effectiveKind, &path, &madeCsproj, &error_)) {
            name_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Draws the New C++ Module modal, which writes into the engine tree.
void ToolsMenu::drawCppModuleModal(f32 dpi) {
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mv->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(660.0f * dpi, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("New C++ Module", nullptr, ImGuiWindowFlags_NoResize)) return;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1),
        "This writes into the ENGINE, not into your project. `.ocproject` carries no build "
        "integration, so there is nowhere project-side for C++ to live - unlike Unreal, where game "
        "C++ sits in the project. The module lands in the engine's modules\\ folder and the ENGINE "
        "must be rebuilt before any of it exists.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Module folder name");
    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##modname", "terrain, or render.terrain", name_, sizeof name_);
    ImGui::PopItemWidth();
    ImGui::TextDisabled("Lowercase, dots for tiers - matching modules\\render.voxi, modules\\rhi.d3d12.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Purpose (one line, for the README)");
    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##modpurpose", "What this module will own.", purpose_, sizeof purpose_);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (name_[0]) {
        ImGui::TextDisabled("Creates modules\\%s\\  -> target %s, namespace aver::%s",
                            name_, moduleTargetName(name_).c_str(), moduleLeaf(name_).c_str());
    } else {
        ImGui::TextDisabled("Creates modules\\<name>\\ with CMakeLists.txt, README.md, include\\ and src\\.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "The top-level CMakeLists.txt is NOT edited. Wiring a module into the build is a "
        "deliberate act - every skeleton already under modules\\ is deliberately unwired - so the "
        "line to add is yours to add, and the README repeats it.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    showError(error_);
    if (!madeFiles_.empty()) {
        showResult(result_);
        for (const std::string& f : madeFiles_) ImGui::TextDisabled("  %s", f.c_str());
        showCmakeHint("Add this to the top-level CMakeLists.txt yourself:", cmakeHint_, dpi);
    }

    ImGui::Spacing();
    if (ImGui::Button("Create Module", ImVec2(160.0f * dpi, 0))) {
        error_.clear(); result_.clear(); madeFiles_.clear(); cmakeHint_.clear();
        if (createCppModule(name_, purpose_, &madeFiles_, &error_)) {
            AVER_INFO("[Editor] new C++ module '{}': {} files. Add to the top-level CMakeLists: add_subdirectory(modules/{})",
                      name_, madeFiles_.size(), name_);
            name_[0] = '\0'; purpose_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Draws the New C++ Class modal: pick an engine module, get a .hpp/.cpp pair.
void ToolsMenu::drawCppClassModal(f32 dpi) {
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mv->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(660.0f * dpi, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("New C++ Class", nullptr, ImGuiWindowFlags_NoResize)) return;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1),
        "This writes into the ENGINE, not into your project. The pair lands in an existing engine "
        "module and the ENGINE must be rebuilt before it exists. If you are looking for the place "
        "to put game code, that is Tools > New C# Script.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::Spacing();

    const ModuleInfo* mod = (moduleSel_ >= 0 && moduleSel_ < (int)modules_.size())
                                ? &modules_[static_cast<usize>(moduleSel_)] : nullptr;

    ImGui::TextUnformatted("Module");
    ImGui::PushItemWidth(-1);
    if (ImGui::BeginCombo("##module", mod ? mod->dir.c_str() : "<no modules found>")) {
        for (int i = 0; i < (int)modules_.size(); ++i) {
            const ModuleInfo& m = modules_[static_cast<usize>(i)];
            const std::string label = m.built ? m.dir : m.dir + "   (skeleton - no CMakeLists.txt)";
            if (ImGui::Selectable(label.c_str(), moduleSel_ == i)) moduleSel_ = i;
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::TextUnformatted("Class name");
    ImGui::PushItemWidth(-1);
    const bool submitted = ImGui::InputText("##cppname", name_, sizeof name_,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (mod) {
        ImGui::TextDisabled("Writes modules\\%s\\include\\aver\\%s\\%s.hpp", mod->dir.c_str(),
                            mod->includeSub.c_str(), name_[0] ? name_ : "<Name>");
        ImGui::TextDisabled("   and modules\\%s\\src\\%s.cpp", mod->dir.c_str(), name_[0] ? name_ : "<Name>");
        ImGui::TextDisabled("namespace %s   (read from the module, not guessed)", mod->nameSpace.c_str());
        if (!mod->built)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1),
                               "%s has no CMakeLists.txt - it is a skeleton and builds nothing yet.", mod->dir.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "The module's CMakeLists.txt is NOT edited. `aver_add_module` takes an explicit SOURCES "
        "list and never globs, so the new .cpp compiles into nothing until the line below is added "
        "by hand - which is also the point at which someone decides it belongs in the build.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    showError(error_);
    if (!madeFiles_.empty()) {
        showResult(result_);
        for (const std::string& f : madeFiles_) ImGui::TextDisabled("  %s", f.c_str());
        showCmakeHint("Add this under SOURCES in that module's CMakeLists.txt:", cmakeHint_, dpi);
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(mod == nullptr);
    if ((ImGui::Button("Create Class", ImVec2(150.0f * dpi, 0)) || submitted) && mod) {
        error_.clear(); result_.clear(); madeFiles_.clear(); cmakeHint_.clear();
        if (createCppClass(*mod, name_, &madeFiles_, &cmakeHint_, &error_)) {
            if (!cmakeHint_.empty())
                AVER_INFO("[Editor] new C++ class '{}' in {}. CMake: {}", name_, mod->dir, cmakeHint_);
            else
                AVER_INFO("[Editor] new C++ class '{}' in {}", name_, mod->dir);
            name_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Draws the Compile Scripts / Reload Scripts modal: status, the transcript, and clickable errors.
void ToolsMenu::drawCompileModal(f32 dpi, bool reload) {
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mv->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(860.0f * dpi, 520.0f * dpi), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(reload ? "Reload Scripts" : "Compile Scripts", nullptr,
                                ImGuiWindowFlags_NoResize))
        return;

    const bool running = compile_ && !compile_->done.load();
    if (compile_) {
        ImGui::TextDisabled("dotnet build %s", compile_->csproj.c_str());
        ImGui::TextDisabled("        -> %s", compile_->outDir.c_str());
    }

    if (running) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "Building...");
    } else if (compile_) {
        if (compile_->exitCode == 0)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Build succeeded (exit 0).");
        else
            ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "Build FAILED (exit %d).", compile_->exitCode);

        if (compile_->reload && compile_->reloaded) {
            ImGui::TextColored(compile_->reloadOk ? ImVec4(0.45f, 0.85f, 0.45f, 1)
                                                  : ImVec4(0.93f, 0.42f, 0.38f, 1),
                               "%s", compile_->reloadStatus.c_str());
        } else if (compile_->reload) {
            ImGui::TextDisabled("Not reloaded - the build has to succeed first.");
        }
    }

    const IdeInfo& ide = preferredGotoIde();
    if (compile_ && !running && (compile_->errors > 0 || compile_->warnings > 0)) {
        ImGui::TextDisabled(ide.canGoto
                                ? "%d error(s), %d warning(s).  Click one to open it in %s, at that line."
                                : "%d error(s), %d warning(s).  Click one to open it in %s - which has no "
                                  "way to be told a line, so it opens at the top.",
                            compile_->errors, compile_->warnings, ide.name.c_str());
    }

    ImGui::Separator();
    // No wrapping, horizontal scroll instead: the clipper needs a predictable row height.
    ImGui::BeginChild("##buildout", ImVec2(0, -46.0f * dpi), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (compile_ && !compile_->lines.empty()) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(compile_->lines.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const BuildLine& bl = compile_->lines[static_cast<usize>(i)];
                if (!bl.hasPosition()) { ImGui::TextUnformatted(bl.raw.c_str()); continue; }

                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, bl.isError ? ImVec4(0.93f, 0.42f, 0.38f, 1)
                                                                : ImVec4(0.95f, 0.72f, 0.25f, 1));
                const bool clicked = ImGui::Selectable(bl.label.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\nline %d, column %d\n\nClick to open in %s.",
                                      bl.file.c_str(), bl.line, bl.col, ide.name.c_str());
                if (clicked && !openInIde(ide, bl.file, bl.line, bl.col))
                    AVER_WARN("[Editor] could not open {} in {}", bl.file, ide.name);
                ImGui::PopID();
            }
        }
    } else if (running) {
        ImGui::TextDisabled("(waiting for dotnet)");
    }
    ImGui::EndChild();

    ImGui::PushTextWrapPos(0.0f);
    if (reload)
        ImGui::TextDisabled("Every behaviour that was live got OnShutdown, and the newly-loaded ones "
                            "got OnStart. Nothing is carried across the swap - a behaviour's fields "
                            "start again from their initialisers.");
    else
        ImGui::TextDisabled("Compiling is all this does: the editor keeps running whatever it loaded "
                            "at startup. Use Tools > Reload Scripts to swap the new build in.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
#endif // AVER_WITH_IMGUI

namespace {

// Newest write time among *.cs under `dir`, skipping obj/ and bin/. False if there is no source.
bool newestCsTime(const std::string& dir, std::filesystem::file_time_type& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return false;
    bool any = false;
    fs::file_time_type newest{};
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            const std::string name = it->path().filename().string();
            if (name == "obj" || name == "bin") it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() != ".cs") continue;
        const fs::file_time_type t = fs::last_write_time(it->path(), ec);
        if (ec) continue;
        if (!any || t > newest) { newest = t; any = true; }
    }
    out = newest;
    return any;
}

// Newest write time among *.dll directly in `dir`. False if there is none.
bool newestDllTime(const std::string& dir, std::filesystem::file_time_type& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return false;
    bool any = false;
    fs::file_time_type newest{};
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->path().extension() != ".dll") continue;
        const fs::file_time_type t = fs::last_write_time(it->path(), ec);
        if (ec) continue;
        if (!any || t > newest) { newest = t; any = true; }
    }
    out = newest;
    return any;
}


// Newest .cs across both directories the project's assembly is built from: Scripts and Materials.
bool newestProjectCsTime(const std::string& scriptsDir, std::filesystem::file_time_type& out) {
    namespace fs = std::filesystem;
    const std::string materialsDir =
        (fs::path(scriptsDir).parent_path() / "Materials").string();

    bool any = false;
    fs::file_time_type newest{};
    for (const std::string& dir : {scriptsDir, materialsDir}) {
        fs::file_time_type t{};
        if (!newestCsTime(dir, t)) continue;
        if (!any || t > newest) { newest = t; any = true; }
    }
    out = newest;
    return any;
}

} // namespace

// Joins a finished build thread, records its result, and performs the assembly swap on a reload.
void ToolsMenu::reapCompile() {
    if (!compile_ || !compile_->done.load() || !compileThread_.joinable()) return;
    compileThread_.join();

    const char* what = compile_->reload ? "Reload Scripts" : "Compile Scripts";
    if (compile_->exitCode == 0) AVER_INFO("[Editor] {}: {} built cleanly", what, compile_->csproj);
    else AVER_ERROR("[Editor] {}: dotnet build exited {}", what, compile_->exitCode);

    lastBuildFailed_ = compile_->exitCode != 0;
    scanClock_ = -1.0;   // force the status light to re-read this frame
    if (openModalOnFail_) {
        openModalOnFail_ = false;
        if (lastBuildFailed_) open(compile_->reload ? Modal::Reload : Modal::Compile);
    }

    if (!compile_->reload || compile_->exitCode != 0 || compile_->reloaded) return;

    compile_->reloaded = true;
    std::string status;
    compile_->reloadOk = reload_ && reload_(compile_->outDir, &status);
    compile_->reloadStatus = status.empty() ? std::string("No scripting host to reload into.") : status;
    if (compile_->reloadOk) AVER_INFO("[Editor] Reload Scripts: {}", compile_->reloadStatus);
    else AVER_ERROR("[Editor] Reload Scripts: {}", compile_->reloadStatus);
}

// Starts a background `dotnet build` of `csproj` into `outDir`, optionally reloading afterwards.
void ToolsMenu::startCompile(const std::string& csproj, const std::string& outDir, bool reload) {
    if (compileThread_.joinable()) return;

    // Record the source state being built: the status light needs it to tell Stale from Failed.
    {
        const std::string scriptsDir = std::filesystem::path(csproj).parent_path().string();
        std::filesystem::file_time_type stamp{};
        haveBuiltStamp_ = newestProjectCsTime(scriptsDir, stamp);
        builtStamp_ = stamp;
    }

    auto job = std::make_shared<Compile>();
    job->csproj = csproj;
    job->outDir = outDir;
    job->reload = reload;
    compile_ = job;

    const std::string dir = std::filesystem::path(csproj).parent_path().string();
    compileThread_ = std::thread([job, csproj, dir, outDir] {
        std::string out;
        int code = -1;
#if defined(_WIN32)
        const std::wstring cmd = L"dotnet build \"" + widen(csproj) + L"\" --nologo -o \"" +
                                 widen(outDir) + L"\"";
        if (!runCaptured(cmd, widen(dir), out, code)) {
            out = "Could not start dotnet.";
            code = -1;
        }
#else
        (void)csproj; (void)dir; (void)outDir;
        out = "Compile Scripts is implemented for Windows only.";
#endif
        // Parsed here, where `dir` is known: MSBuild's relative paths are relative to it.
        job->lines = parseBuildOutput(out, dir);
        for (const BuildLine& bl : job->lines) {
            if (bl.isError) ++job->errors;
            else if (bl.isWarning) ++job->warnings;
        }
        // ---- materials: baked here, and a bake failure does not fail the compile ----
        if (code == 0) {
            std::string bakeLog;
            int bakeCode = -1;
            if (bakeMaterials(outDir, bakeLog, bakeCode) && bakeCode != 0) {
                out += "\n[materials] avermatc exited " + std::to_string(bakeCode) + "\n" + bakeLog;
            } else if (!bakeLog.empty()) {
                out += "\n[materials] " + bakeLog;
            }
        }

        job->output = std::move(out);
        job->exitCode = code;
        job->done.store(true);   // last: the UI thread reads output/exitCode once this is set
    });
}

// Starts the build the toolbar's Compile C# button asks for: silent on success, errors on failure.
void ToolsMenu::triggerToolbarCompile(const fmt::ProjectDesc& project) {
    if (compileThread_.joinable()) return;
    const std::string csproj = scriptsCsprojPath(project);
    if (csproj.empty() || !fileExists(csproj) || !haveDotnet()) return;
    openModalOnFail_ = true;
    startCompile(csproj, scriptsBinaryDir(project), reload_ != nullptr);
}

#if AVER_WITH_IMGUI
// Recomputes the toolbar status light: building, no project, stale, failed or up to date.
void ToolsMenu::refreshScriptStatus(const fmt::ProjectDesc& project) {
    if (compileThread_.joinable()) { scriptStatus_ = ScriptStatus::Building;  return; }
    if (!project.valid())          { scriptStatus_ = ScriptStatus::NoProject; return; }

    const std::string csproj = scriptsCsprojPath(project);
    if (csproj.empty() || !fileExists(csproj)) {
        scriptStatus_ = ScriptStatus::UpToDate;
        return;
    }

    // Throttled: a directory walk every frame is waste. -1 forces the next scan.
    const double now = ImGui::GetTime();
    if (scanClock_ >= 0.0 && now - scanClock_ < 0.5) return;
    scanClock_ = now;

    const std::string scriptsDir = std::filesystem::path(csproj).parent_path().string();
    std::filesystem::file_time_type newestCs{};
    if (!newestProjectCsTime(scriptsDir, newestCs)) { scriptStatus_ = ScriptStatus::UpToDate; return; }

    if (haveBuiltStamp_) {
        if (newestCs > builtStamp_)   scriptStatus_ = ScriptStatus::Stale;
        else if (lastBuildFailed_)    scriptStatus_ = ScriptStatus::Failed;
        else                          scriptStatus_ = ScriptStatus::UpToDate;
        return;
    }

    // Nothing built this session: fall back to the assembly's own timestamp.
    std::filesystem::file_time_type newestDll{};
    const bool anyDll = newestDllTime(scriptsBinaryDir(project), newestDll);
    scriptStatus_ = (anyDll && newestDll >= newestCs) ? ScriptStatus::UpToDate : ScriptStatus::Stale;
}

// Draws the build/reload/open items into whatever menu or popup is currently open. Shared by the
// Tools menu and the Compile C# button's dropdown.
void ToolsMenu::drawScriptItems(const fmt::ProjectDesc& project) {
    const bool haveProject = project.valid();

    const char* kNoProject = "Open or create a project first - C# lives in the\nproject's Content\\Scripts folder.";

    ImGui::TextDisabled("New scripts & classes: Content Browser  >  + Add");
    ImGui::Separator();
    const std::string csproj = haveProject ? scriptsCsprojPath(project) : std::string();
    const std::string binDir = haveProject ? scriptsBinaryDir(project) : std::string();
    const bool haveCsproj = !csproj.empty() && fileExists(csproj);
    const bool dotnetOk = haveDotnet();
    const bool canCompile = haveProject && haveCsproj && dotnetOk && !compileThread_.joinable();
    if (ImGui::MenuItem("Compile Scripts", nullptr, false, canCompile)) {
        open(Modal::Compile);
        startCompile(csproj, binDir, false);
    }
    tip(!haveProject  ? kNoProject
        : !dotnetOk   ? "dotnet was not found on PATH, so there is nothing to build with.\nInstall the .NET SDK and restart the editor."
        : !haveCsproj ? "This project has no Content\\Scripts\\Scripts.csproj yet.\nUse New C# Script or New C# Class to generate one."
        : compileThread_.joinable() ? "A build is already running."
        : "dotnet build into Binaries\\Scripts.\nThe editor keeps running whatever it loaded - use Reload Scripts to swap it in.");

    if (ImGui::MenuItem("Reload Scripts", nullptr, false, canCompile && reload_ != nullptr)) {
        open(Modal::Reload);
        startCompile(csproj, binDir, true);
    }
    tip(!haveProject  ? kNoProject
        : !reload_    ? "This build has no scripting host, so there is nothing to reload into.\n(-DAVER_MODULE_SCRIPTING=OFF, or the host declined at startup.)"
        : !dotnetOk   ? "dotnet was not found on PATH, so there is nothing to build with.\nInstall the .NET SDK and restart the editor."
        : !haveCsproj ? "This project has no Content\\Scripts\\Scripts.csproj yet.\nUse New C# Script or New C# Class to generate one."
        : compileThread_.joinable() ? "A build is already running."
        : "Rebuild, then unload and reload the project's scripts in place.\nRunning behaviours get OnShutdown, the new ones get OnStart.\nNo editor restart, and no state is carried across.");

    if (autoCompile_) {
        const bool canAuto = haveProject && haveCsproj && dotnetOk;
        ImGui::BeginDisabled(!canAuto);
        ImGui::MenuItem("Auto-compile on Save", nullptr, autoCompile_);
        ImGui::EndDisabled();
        tip(!haveProject  ? kNoProject
            : !dotnetOk   ? "dotnet was not found on PATH, so there is nothing to build with."
            : !haveCsproj ? "This project has no Content\\Scripts\\Scripts.csproj yet."
            : "Rebuild and reload whenever a .cs under Content changes on disk - saving in Visual\n"
              "Studio is enough. Edits are debounced, so a Save All is ONE build, not one per file.\n"
              "bin\\ and obj\\ are ignored: the build writes .cs there itself, and reacting to\n"
              "those would compile in a loop forever.");
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Open Project Folder", nullptr, false, haveProject)) {
        if (!shellOpen(project.dir)) AVER_WARN("[Editor] could not open {}", project.dir);
    }
    tip(haveProject ? "Opens the project folder in Explorer."
                    : "Open or create a project first - there is no folder to show.");

    const std::vector<IdeInfo>& ides = detectedIdes();
    const char* kOpenBlocked = !haveProject ? kNoProject
        : "There is no Content\\Scripts\\Scripts.csproj to open yet.\nUse New C# Script or New C# Class to generate one.";

    if (ides.size() > 1) {
        if (ImGui::BeginMenu("Open Scripts In", haveCsproj)) {
            for (const IdeInfo& ide : ides) {
                if (ImGui::MenuItem(ide.name.c_str())) {
                    if (!openProjectInIde(ide, csproj))
                        AVER_WARN("[Editor] could not open {} in {}", csproj, ide.name);
                }
                tip(ide.kind == IdeKind::VsCode
                        ? "Opens the Content\\Scripts FOLDER - handing Code a .csproj\nwould just show you the XML."
                    : ide.kind == IdeKind::ShellDefault
                        ? "Hands Scripts.csproj to whatever is registered for .csproj."
                        : "Opens Content\\Scripts\\Scripts.csproj.");
            }
            ImGui::EndMenu();
        }
        tip(haveCsproj ? "Every code editor found on this machine, best first." : kOpenBlocked);
    } else {
        const IdeInfo& ide = ides.front();
        const std::string label = "Open Scripts in " + ide.name;
        if (ImGui::MenuItem(label.c_str(), nullptr, false, haveCsproj)) {
            if (!openProjectInIde(ide, csproj))
                AVER_WARN("[Editor] could not open {} in {}", csproj, ide.name);
        }
        tip(!haveCsproj ? kOpenBlocked
            : !ideDetectionFinished()
                ? "Still looking for installed IDEs. Until that finishes this hands\nScripts.csproj to the shell, which is what it always did."
                : "No IDE was detected, so Scripts.csproj goes to whatever is\nregistered for .csproj - normally Visual Studio.");
    }
}

// Draws the toolbar's Compile C# split button: a status icon and label, plus a dropdown arrow.
void ToolsMenu::drawCompileButton(const fmt::ProjectDesc& project, f32 dpi, u64 iconTex) {
    (void)dpi;
    refreshScriptStatus(project);

    const bool building = scriptStatus_ == ScriptStatus::Building;
    const std::string csproj = project.valid() ? scriptsCsprojPath(project) : std::string();
    const bool canCompile = !building && project.valid() && haveDotnet() &&
                            !csproj.empty() && fileExists(csproj);

    // The icon sheet is three tiles: 0 built, 1 failed, 2 stale. Building and no-project use stale.
    int tile = 2;
    if      (scriptStatus_ == ScriptStatus::UpToDate) tile = 0;
    else if (scriptStatus_ == ScriptStatus::Failed)   tile = 1;
    // ".NET" rather than "C#" because the build is no longer C#-only: `dotnet build` picks the
    // compiler from each project's extension, so a Scripts.fsproj beside Scripts.csproj is built by
    // the same command and the same button. Naming the language was always naming the wrong thing --
    // what this builds is a project, and the project decides its language.
    const char* tip =
          scriptStatus_ == ScriptStatus::UpToDate ? "Scripts are built and live."
        : scriptStatus_ == ScriptStatus::Failed   ? "The last script build failed - click to see the errors."
        : scriptStatus_ == ScriptStatus::Building ? "Compiling..."
        : scriptStatus_ == ScriptStatus::NoProject ? "Open a project to compile its scripts."
        : !haveDotnet()                            ? "dotnet was not found on PATH. Install the .NET SDK."
        :                                            "Scripts changed since the last build - click Compile .NET.";

    ImGuiStyle& st = ImGui::GetStyle();
    const float ih = ImGui::GetFrameHeight() - st.FramePadding.y * 2.0f;   // icon square, inside padding
    const char* label = "Compile .NET";
    const ImVec2 tsz = ImGui::CalcTextSize(label);
    const float gap = st.ItemInnerSpacing.x;
    const ImVec2 btnSize(st.FramePadding.x * 2.0f + ih + gap + tsz.x, 0.0f);

    // ItemSpacing 0 so the two halves read as one split control.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, st.ItemSpacing.y));

    ImGui::BeginDisabled(!canCompile);
    const bool clicked = ImGui::Button("##compilecs", btnSize);   // empty label: the face is drawn below
    ImGui::EndDisabled();
    if (clicked) triggerToolbarCompile(project);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);

    // Draw the icon and label onto the button's own rectangle.
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    const float  cy   = (rmin.y + rmax.y) * 0.5f;
    const float  ix   = rmin.x + st.FramePadding.x;
    const bool   dim  = !canCompile;
    ImDrawList*  dl   = ImGui::GetWindowDrawList();

    if (iconTex) {
        // Slice the three-tile sheet by U: tile t spans [t/3, (t+1)/3].
        const ImVec2 uv0((float)tile / 3.0f, 0.0f), uv1((float)(tile + 1) / 3.0f, 1.0f);
        const ImU32 tint = dim ? IM_COL32(255, 255, 255, 120) : IM_COL32(255, 255, 255, 255);
        dl->AddImage(static_cast<ImTextureID>(iconTex),
                     ImVec2(ix, cy - ih * 0.5f), ImVec2(ix + ih, cy + ih * 0.5f), uv0, uv1, tint);
    } else {
        // No texture staged: a state-coloured dot instead.
        const ImU32 dot = tile == 0 ? IM_COL32(70,180,80,255)
                        : tile == 1 ? IM_COL32(205,55,50,255)
                                    : IM_COL32(225,195,45,255);
        dl->AddCircleFilled(ImVec2(ix + ih * 0.5f, cy), ih * 0.42f, dot);
    }
    dl->AddText(ImVec2(ix + ih + gap, cy - tsz.y * 0.5f),
                ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text), label);

    // --- the dropdown half, never disabled with the face ---
    ImGui::SameLine();
    const float aw = ImGui::GetFrameHeight() * 0.72f;
    const bool arrow = ImGui::Button("##compilecsmenu", ImVec2(aw, 0.0f));
    if (arrow) ImGui::OpenPopup("##compilecsitems");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload, auto-compile on save, and open the scripts.");

    const ImVec2 amin = ImGui::GetItemRectMin();
    const ImVec2 amax = ImGui::GetItemRectMax();
    // A hairline on the shared edge, so the pair reads as one split control.
    dl->AddLine(ImVec2(amin.x, amin.y + st.FramePadding.y),
                ImVec2(amin.x, amax.y - st.FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_Separator));
    const ImVec2 ac((amin.x + amax.x) * 0.5f, (amin.y + amax.y) * 0.5f);
    const float  ar = ImGui::GetFontSize() * 0.22f;
    const ImU32  acol = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddTriangleFilled(ImVec2(ac.x - ar, ac.y - ar * 0.5f),
                          ImVec2(ac.x + ar, ac.y - ar * 0.5f),
                          ImVec2(ac.x,      ac.y + ar * 0.7f), acol);

    ImGui::PopStyleVar();

    // --arm-compile-menu (screenshot aid), re-issued every frame: the popup ID belongs to this window.
    if (armCompileMenu_) ImGui::OpenPopup("##compilecsitems");

    // Anchored under the arrow: a popup not opened by an item interaction is placed at the mouse.
    ImGui::SetNextWindowPos(ImVec2(amin.x, amax.y), ImGuiCond_Always);
    if (ImGui::BeginPopup("##compilecsitems")) {
        drawScriptItems(project);
        ImGui::EndPopup();
    }
}
#else
void ToolsMenu::refreshScriptStatus(const fmt::ProjectDesc&) {}
void ToolsMenu::drawScriptItems(const fmt::ProjectDesc&) {}
void ToolsMenu::drawCompileButton(const fmt::ProjectDesc&, f32, u64) {}
#endif // AVER_WITH_IMGUI

} // namespace aver::editor
