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

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Child-process bytes -> UTF-8, which is what ImGui will be asked to draw. `dotnet` writes UTF-8
// when its output is redirected on current SDKs, but that is not contractual and a stray
// mis-decoded byte renders as a black box for the rest of the line — so it is validated, and
// anything that is not UTF-8 is re-read as the OEM code page rather than shown broken.
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

// Run a command line with stdout AND stderr captured into `out`. Both go down one pipe on
// purpose: MSBuild interleaves errors with the surrounding context, and two separately-drained
// streams would show them in an order that never happened.
bool runCaptured(const std::wstring& cmdline, const std::wstring& cwd, std::string& out, int& exitCode) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    // Only the write end may be inherited; leaving the read end inheritable keeps a handle alive
    // in the child and the drain loop below would never see EOF.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // STARTF_USESTDHANDLES means the child's stdin is whatever hStdInput says, and a null one is a
    // handle it cannot read from. NUL gives an immediate EOF instead, which is the answer intended
    // for a build nobody is sitting in front of — an MSBuild task that decided to read stdin would
    // otherwise fail in whatever way its own error handling picked.
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
    std::wstring mutableCmd = cmdline; // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                   &si, &pi);
    CloseHandle(wr); // the parent's copy, or the child holds the pipe open forever
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

bool findOnPath(const wchar_t* exe) {
    wchar_t found[MAX_PATH];
    return SearchPathW(nullptr, exe, L".exe", MAX_PATH, found, nullptr) > 0;
}

// Hand a path to the shell. Used for the project folder (Explorer) and for Scripts.csproj
// (whatever is registered for .csproj). Deliberately not a hard-coded devenv.exe: locating a
// Visual Studio install needs vswhere, and the association is what the user has actually chosen.
bool shellOpen(const std::string& path) {
    const std::wstring w = widen(path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32; // ShellExecute's documented success threshold
}

#else // !_WIN32

bool runCaptured(const std::wstring&, const std::wstring&, std::string&, int&) { return false; }
bool findOnPath(const wchar_t*) { return false; }
bool shellOpen(const std::string&) { return false; }

#endif

#if AVER_WITH_IMGUI
// A tooltip that also shows for a DISABLED item. ImGui's SetItemTooltip deliberately will not,
// and a greyed-out row with no explanation is the exact thing this menu must not have.
void tip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

// What a generated C# file will and will not be able to do, said in the modal AND in the file
// header. It used to say "this will not run"; it now says what runs and what a script still
// cannot reach, which is the same job — the thing someone must not discover an hour later.
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

// The one build line the user must add by hand, with a button that puts it on the clipboard.
// Retyping a path from a screenshot of a dialog is how a manual step becomes a typo.
void showCmakeHint(const char* what, const std::string& line, f32 dpi) {
    if (line.empty()) return;
    ImGui::Spacing();
    ImGui::TextUnformatted(what);
    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1), "%s", line.c_str());
    if (ImGui::Button("Copy line", ImVec2(110.0f * dpi, 0))) ImGui::SetClipboardText(line.c_str());
}
#endif

} // namespace

ToolsMenu::~ToolsMenu() {
    // A build in flight owns a pipe and a process handle; blocking here is a shutdown that waits,
    // which is preferable to a detached thread writing into a destroyed object.
    if (compileThread_.joinable()) compileThread_.join();
}

void ToolsMenu::open(Modal m) {
    pending_ = m;
    name_[0] = '\0';
    error_.clear();
    result_.clear();
    madeFiles_.clear();
    cmakeHint_.clear();
}

bool ToolsMenu::haveDotnet() {
    if (dotnet_ < 0) dotnet_ = findOnPath(L"dotnet") ? 1 : 0;
    return dotnet_ == 1;
}

// ---------------------------------------------------------------------------------------------
// the dropdown
// ---------------------------------------------------------------------------------------------

void ToolsMenu::drawMenu(const fmt::ProjectDesc& project) {
#if !AVER_WITH_IMGUI
    (void)project;
#else
    // --tools-menu (screenshot aid). Re-issued every frame rather than latched: the popup ID is
    // derived from the menu-bar window, so this has to run with that window current, and there is
    // no other way to photograph a dropdown in a run that cannot move the mouse.
    if (armMenu_) ImGui::OpenPopup("Tools");

    if (!ImGui::BeginMenu("Tools")) return;

    const bool haveProject = project.valid();
    const bool haveEngine = !engineRoot().empty();

    const char* kNoProject = "Open or create a project first - C# lives in the\nproject's Content\\Scripts folder.";
    const char* kNoEngine  = "The engine source tree is not beside this executable,\nso there is no modules\\ folder to write into.";

    // Two groups, labelled, because the asymmetry between them is the thing most likely to
    // surprise someone arriving from Unreal — where game C++ lives in the project.
    ImGui::SeparatorText("PROJECT - C#  (no engine rebuild)");
    if (ImGui::MenuItem("New C# Script...", nullptr, false, haveProject)) open(Modal::CsScript);
    tip(haveProject ? "An AverBehaviour with lifecycle hooks, in Content\\Scripts.\nReload Scripts builds it and runs it." : kNoProject);
    if (ImGui::MenuItem("New C# Class...", nullptr, false, haveProject)) open(Modal::CsClass);
    tip(haveProject ? "A plain class, no lifecycle hooks, in Content\\Scripts." : kNoProject);

    ImGui::SeparatorText("ENGINE - C++  (needs an engine rebuild)");
    if (ImGui::MenuItem("New C++ Module... (engine)", nullptr, false, haveEngine)) open(Modal::CppModule);
    tip(haveEngine ? "Scaffolds a new module in the ENGINE's modules\\ folder,\nnot in your project. .ocproject has no build integration."
                   : kNoEngine);
    if (ImGui::MenuItem("New C++ Class... (engine module)", nullptr, false, haveEngine)) {
        open(Modal::CppClass);
        modules_ = listModules();
        moduleSel_ = modules_.empty() ? -1 : 0;
    }
    tip(haveEngine ? "Adds a .hpp/.cpp pair to an existing ENGINE module.\nGame C++ does not live in the project - see the modal."
                   : kNoEngine);

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

    // Reload is a SEPARATE item and not a checkbox on Compile: they fail differently and a user
    // reaches for them at different moments. Compile answers "does it build"; Reload answers
    // "does it do what I meant", and it swaps live behaviours out from under a running editor.
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

    ImGui::Separator();
    if (ImGui::MenuItem("Open Project Folder", nullptr, false, haveProject)) {
        if (!shellOpen(project.dir)) AVER_WARN("[Editor] could not open {}", project.dir);
    }
    tip(haveProject ? "Opens the project folder in Explorer."
                    : "Open or create a project first - there is no folder to show.");

    // Named after what is actually installed rather than after Visual Studio in hope. The list is
    // never empty — the shell fallback is always its last entry — so there is always exactly one
    // item or exactly one submenu, and the no-project / no-csproj tooltips are unchanged.
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

    ImGui::EndMenu();
#endif
}

// ---------------------------------------------------------------------------------------------
// modals
// ---------------------------------------------------------------------------------------------

void ToolsMenu::drawModals(const fmt::ProjectDesc& project, f32 dpi) {
    // Outside the UI guard, and first: a finished build has a thread to join and possibly an
    // assembly swap to perform, neither of which is a drawing concern. This is the one call that
    // runs every frame whatever is open, so it is where the reap belongs.
    reapCompile();
    // Starts the IDE scan on the first frame rather than on the first click. drawMenu only runs
    // while the dropdown is actually open, so without this the first person to open Tools would
    // get the shell fallback and then watch the item rename itself a few frames later.
    //
    // Logged once when it lands, from the MAIN thread rather than from the scan: what the editor
    // decided is on this machine is the first thing anyone asks when a menu item names the wrong
    // program, and a submenu is not something a bug report can paste.
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
    // --new-script, gated on the SAME predicate as the menu item, so a run with no project proves
    // the item really is disabled rather than merely looking it — headless capture cannot open a
    // menu and click.
    if (armScript_ > 0) { if (project.valid() && pending_ == Modal::None) open(Modal::CsScript); --armScript_; }

    // --compile-scripts and --reload-scripts, same family and the same reasoning: gated on exactly
    // what enables the menu item, so a run that produces no modal has demonstrated a real disabled
    // state. Reload additionally needs a host, which is the one thing a screenshot cannot assert.
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
// Shared by New C# Script and New C# Class: same folder, same validation, same refusal to
// overwrite, and the same honest warning. Only the template and the wording differ.
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

    // The parent-class picker, only on New C# Script. New C# Class is always a plain class, so it
    // shows no picker. The order is deliberate: AverBehaviour first because it is the one that runs
    // today, then the gameplay types top-down (Actor -> Pawn/Controller, then GameMode/Instance).
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
        // The one thing a scaffolded actor must not surprise anyone with: it compiles, but the
        // runtime that would call its hooks is not built yet.
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
            result_ = "Created " + path + (madeCsproj ? "  (+ Scripts.csproj)" : "");
            name_[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

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
            result_ = "Created " + std::to_string(madeFiles_.size()) + " files:";
            cmakeHint_ = "add_subdirectory(modules/" + std::string(name_) + ")";
            name_[0] = '\0'; purpose_[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

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
            // The label carries the module's build state: a skeleton has no CMakeLists at all, so
            // a class added there compiles nowhere until someone writes one.
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
            result_ = "Created in " + mod->dir + ":";
            name_[0] = '\0';
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

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

        // The reload line is separate from the build line because they are separate outcomes: a
        // build can succeed and the swap still find nothing to load, and reporting one number for
        // both is how "it said it worked" becomes a bug report.
        if (compile_->reload && compile_->reloaded) {
            ImGui::TextColored(compile_->reloadOk ? ImVec4(0.45f, 0.85f, 0.45f, 1)
                                                  : ImVec4(0.93f, 0.42f, 0.38f, 1),
                               "%s", compile_->reloadStatus.c_str());
        } else if (compile_->reload) {
            ImGui::TextDisabled("Not reloaded - the build has to succeed first.");
        }
    }

    // Which editor a click will actually reach. Asked once per frame rather than per line, and it
    // is also what starts detection: the first Tools frame kicks off the scan and gets the shell
    // fallback, so nothing here ever waits on vswhere.
    //
    // The GOTO preference, not the general one. Everything in this panel is a jump to a line, and
    // the editor that opens a project best is not the one that lands a caret best.
    const IdeInfo& ide = preferredGotoIde();
    if (compile_ && !running && (compile_->errors > 0 || compile_->warnings > 0)) {
        // Says which editor, and says plainly when that editor cannot be told a line — a click
        // that opens the file at the top is a different promise from one that lands on the error.
        ImGui::TextDisabled(ide.canGoto
                                ? "%d error(s), %d warning(s).  Click one to open it in %s, at that line."
                                : "%d error(s), %d warning(s).  Click one to open it in %s - which has no "
                                  "way to be told a line, so it opens at the top.",
                            compile_->errors, compile_->warnings, ide.name.c_str());
    }

    ImGui::Separator();
    // The whole transcript, scrollable: an exit code alone cannot tell anyone which line of which
    // file the compiler objected to, and that is the only thing a failed build is asked.
    //
    // No text wrapping, and a horizontal scrollbar instead. The clipper below can only skip rows it
    // can predict the height of, and a wrapped line is however many rows the current width makes it
    // — one long path would then throw off every scroll position under it. A terminal would have
    // scrolled that line sideways too.
    ImGui::BeginChild("##buildout", ImVec2(0, -46.0f * dpi), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (compile_ && !compile_->lines.empty()) {
        // Clipped, because each line is now its own item — a Selectable with an ID, a style push
        // and a hover test, where the transcript used to be a single TextUnformatted. A build that
        // restores packages runs to hundreds of lines and the panel shows about thirty of them.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(compile_->lines.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const BuildLine& bl = compile_->lines[static_cast<usize>(i)];
                // Anything without a position — MSBuild's banners, its summary counts, and any line
                // this failed to understand — is printed exactly as it arrived. Output that was not
                // parsed is still output somebody has to be able to read.
                if (!bl.hasPosition()) { ImGui::TextUnformatted(bl.raw.c_str()); continue; }

                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, bl.isError ? ImVec4(0.93f, 0.42f, 0.38f, 1)
                                                                : ImVec4(0.95f, 0.72f, 0.25f, 1));
                // Selectable rather than Text: it gives the row a hover highlight, so a clickable
                // line looks clickable before anyone discovers it by accident.
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

// Newest write time among *.cs under `dir`, skipping obj/ and bin/. `dotnet build` drops generated
// sources (AssemblyInfo, GlobalUsings) into obj\, and their mtime bumps on every build — counting
// them would make the source look perpetually newer than itself. Returns false when the tree holds
// no source at all, which the caller reads as "nothing to build".
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

// Newest *.dll under `dir` — the built assembly's timestamp. Used ONLY across editor restarts, when
// this session has not built anything yet: within a session the in-memory record is authoritative
// because it alone knows a build failed.
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

} // namespace

// Outside the ImGui guard: joining the build thread and swapping the assemblies are neither of
// them UI, and a build must be reaped in a build with no editor chrome just the same.
void ToolsMenu::reapCompile() {
    if (!compile_ || !compile_->done.load() || !compileThread_.joinable()) return;
    compileThread_.join();

    const char* what = compile_->reload ? "Reload Scripts" : "Compile Scripts";
    if (compile_->exitCode == 0) AVER_INFO("[Editor] {}: {} built cleanly", what, compile_->csproj);
    else AVER_ERROR("[Editor] {}: dotnet build exited {}", what, compile_->exitCode);

    // The toolbar's Compile C# light reads this: a build stays failed (red) until a later one
    // succeeds, and the toolbar path pops the errors on failure — Build Verse, but it shows what
    // broke. The menu items open their own modal up front, so they never set openModalOnFail_.
    lastBuildFailed_ = compile_->exitCode != 0;
    scanClock_ = -1.0;   // force the light to re-read now rather than up to a throttle-tick later
    if (openModalOnFail_) {
        openModalOnFail_ = false;
        // Match the job's own modal so the title and footer read right; a failed reload never
        // reloaded, so either variant shows only the build errors regardless.
        if (lastBuildFailed_) open(compile_->reload ? Modal::Reload : Modal::Compile);
    }

    // A failed build must NOT unload: the editor would be left with no scripts at all because of a
    // typo, which is a far worse outcome than carrying on with the previous ones.
    if (!compile_->reload || compile_->exitCode != 0 || compile_->reloaded) return;

    compile_->reloaded = true;
    std::string status;
    compile_->reloadOk = reload_ && reload_(compile_->outDir, &status);
    compile_->reloadStatus = status.empty() ? std::string("No scripting host to reload into.") : status;
    if (compile_->reloadOk) AVER_INFO("[Editor] Reload Scripts: {}", compile_->reloadStatus);
    else AVER_ERROR("[Editor] Reload Scripts: {}", compile_->reloadStatus);
}

void ToolsMenu::startCompile(const std::string& csproj, const std::string& outDir, bool reload) {
    if (compileThread_.joinable()) return; // the menu item is disabled meanwhile; belt and braces

    // Record the source state we are about to build. It is the only thing that lets the toolbar
    // light tell "edited since this build" (Stale) from "this build failed and nothing changed"
    // (Failed) — file mtimes cannot express a failure, since a failed build leaves the old .dll.
    {
        const std::string scriptsDir = std::filesystem::path(csproj).parent_path().string();
        std::filesystem::file_time_type stamp{};
        haveBuiltStamp_ = newestCsTime(scriptsDir, stamp);
        builtStamp_ = stamp;
    }

    auto job = std::make_shared<Compile>();
    job->csproj = csproj;
    job->outDir = outDir;
    job->reload = reload;
    compile_ = job;

    // The working directory is the Scripts folder so relative paths in MSBuild's diagnostics read
    // the way they do in a terminal opened there.
    const std::string dir = std::filesystem::path(csproj).parent_path().string();
    compileThread_ = std::thread([job, csproj, dir, outDir] {
        std::string out;
        int code = -1;
#if defined(_WIN32)
        // `-o` and not the .csproj's own OutputPath: the editor has to know this directory too,
        // and a project scaffolded before that was true would otherwise build where nothing looks.
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
        // Parsed HERE and not in the draw call. `dir` is the directory the build ran in, which is
        // what MSBuild's relative paths are relative to — resolving them anywhere else would
        // produce a path that opens nothing.
        job->lines = parseBuildOutput(out, dir);
        for (const BuildLine& bl : job->lines) {
            if (bl.isError) ++job->errors;
            else if (bl.isWarning) ++job->warnings;
        }
        job->output = std::move(out);
        job->exitCode = code;
        job->done.store(true); // last: the UI thread reads output/exitCode once this is set
    });
}

void ToolsMenu::triggerToolbarCompile(const fmt::ProjectDesc& project) {
    if (compileThread_.joinable()) return;
    const std::string csproj = scriptsCsprojPath(project);
    if (csproj.empty() || !fileExists(csproj) || !haveDotnet()) return;
    openModalOnFail_ = true;   // silent on success, the errors on failure — that is the whole button
    // reload when a host is present: the button's promise is "make the new code live", which is the
    // reload path. With no host it degrades to a plain compile and the light still tracks the result.
    startCompile(csproj, scriptsBinaryDir(project), reload_ != nullptr);
}

#if AVER_WITH_IMGUI
void ToolsMenu::refreshScriptStatus(const fmt::ProjectDesc& project) {
    if (compileThread_.joinable()) { scriptStatus_ = ScriptStatus::Building;  return; }
    if (!project.valid())          { scriptStatus_ = ScriptStatus::NoProject; return; }

    const std::string csproj = scriptsCsprojPath(project);
    if (csproj.empty() || !fileExists(csproj)) {
        // No scripts project yet: genuinely nothing to build, which the user asked to read as green.
        scriptStatus_ = ScriptStatus::UpToDate;
        return;
    }

    // A directory walk every frame is waste; a script is not edited at 60 Hz. Keep the last verdict
    // between scans, and let -1 force the first one.
    const double now = ImGui::GetTime();
    if (scanClock_ >= 0.0 && now - scanClock_ < 0.5) return;
    scanClock_ = now;

    const std::string scriptsDir = std::filesystem::path(csproj).parent_path().string();
    std::filesystem::file_time_type newestCs{};
    if (!newestCsTime(scriptsDir, newestCs)) { scriptStatus_ = ScriptStatus::UpToDate; return; }

    if (haveBuiltStamp_) {
        // Priority is the user's wording: unbuilt CHANGES win over a stale failure, so a fresh edit
        // reads yellow even if the last build was red.
        if (newestCs > builtStamp_)   scriptStatus_ = ScriptStatus::Stale;   // edited since we built
        else if (lastBuildFailed_)    scriptStatus_ = ScriptStatus::Failed;  // known-broken, unchanged
        else                          scriptStatus_ = ScriptStatus::UpToDate;
        return;
    }

    // Nothing built this session: fall back to the assembly's own timestamp so the light is honest
    // across restarts. Missing or older-than-source output means the changes are unbuilt.
    std::filesystem::file_time_type newestDll{};
    const bool anyDll = newestDllTime(scriptsBinaryDir(project), newestDll);
    scriptStatus_ = (anyDll && newestDll >= newestCs) ? ScriptStatus::UpToDate : ScriptStatus::Stale;
}

void ToolsMenu::drawCompileButton(const fmt::ProjectDesc& project, f32 dpi, u64 iconTex) {
    (void)dpi;
    refreshScriptStatus(project);

    const bool building = scriptStatus_ == ScriptStatus::Building;
    const std::string csproj = project.valid() ? scriptsCsprojPath(project) : std::string();
    const bool canCompile = !building && project.valid() && haveDotnet() &&
                            !csproj.empty() && fileExists(csproj);

    // Which sprite tile the state shows. The sheet is three tiles: 0 built, 1 failed, 2 stale. There
    // is NO spinner: while a build runs it stays on the stale (?) tile, per the request, and so does
    // "no project" (nothing is known-built).
    int tile = 2;
    if      (scriptStatus_ == ScriptStatus::UpToDate) tile = 0;
    else if (scriptStatus_ == ScriptStatus::Failed)   tile = 1;
    const char* tip =
          scriptStatus_ == ScriptStatus::UpToDate ? "C# is built and live."
        : scriptStatus_ == ScriptStatus::Failed   ? "The last C# build failed - click to see the errors."
        : scriptStatus_ == ScriptStatus::Building ? "Compiling C#..."
        : scriptStatus_ == ScriptStatus::NoProject ? "Open a project to compile its C# scripts."
        : !haveDotnet()                            ? "dotnet was not found on PATH. Install the .NET SDK."
        :                                            "C# changed since the last build - click Compile C#.";

    // One button carrying the icon AND the label, so the status icon is PART of the button rather
    // than a pip beside it. Sized to fit the icon, an inner gap, and "Compile C#".
    ImGuiStyle& st = ImGui::GetStyle();
    const float ih = ImGui::GetFrameHeight() - st.FramePadding.y * 2.0f;   // icon square, inside padding
    const char* label = "Compile C#";
    const ImVec2 tsz = ImGui::CalcTextSize(label);
    const float gap = st.ItemInnerSpacing.x;
    const ImVec2 btnSize(st.FramePadding.x * 2.0f + ih + gap + tsz.x, 0.0f);

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
    const bool   dim  = !canCompile;   // disabled: dim the icon and the text together
    ImDrawList*  dl   = ImGui::GetWindowDrawList();

    if (iconTex) {
        // Slice the three-tile sheet by U: tile t spans [t/3, (t+1)/3].
        const ImVec2 uv0((float)tile / 3.0f, 0.0f), uv1((float)(tile + 1) / 3.0f, 1.0f);
        const ImU32 tint = dim ? IM_COL32(255, 255, 255, 120) : IM_COL32(255, 255, 255, 255);
        dl->AddImage(static_cast<ImTextureID>(iconTex),
                     ImVec2(ix, cy - ih * 0.5f), ImVec2(ix + ih, cy + ih * 0.5f), uv0, uv1, tint);
    } else {
        // No texture staged: a small state-coloured dot so there is still a signal.
        const ImU32 dot = tile == 0 ? IM_COL32(70,180,80,255)
                        : tile == 1 ? IM_COL32(205,55,50,255)
                                    : IM_COL32(225,195,45,255);
        dl->AddCircleFilled(ImVec2(ix + ih * 0.5f, cy), ih * 0.42f, dot);
    }
    dl->AddText(ImVec2(ix + ih + gap, cy - tsz.y * 0.5f),
                ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text), label);
}
#else
void ToolsMenu::refreshScriptStatus(const fmt::ProjectDesc&) {}
void ToolsMenu::drawCompileButton(const fmt::ProjectDesc&, f32, u64) {}
#endif // AVER_WITH_IMGUI

} // namespace aver::editor
