#include "ToolsMenu.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/core/Log.hpp"

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

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline; // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                   &si, &pi);
    CloseHandle(wr); // the parent's copy, or the child holds the pipe open forever
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

// The wrapped amber "this does not run yet" paragraph, identical wherever a generated C# file is
// promised. Said in the modal AND in the file header: a script that silently never runs is the
// kind of thing someone otherwise discovers an hour later, by watching nothing happen.
void warnNoClr() {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1),
        "This will NOT run. The engine cannot host the CLR in-process yet "
        "(docs/STATUS.md \xC2\xA7""4d), so nothing loads or calls it. It compiles - Tools > Compile "
        "Scripts is real - and it is editable against the real Aver.Scripting API; execution is "
        "not wired up.");
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
    tip(haveProject ? "A behaviour with lifecycle hooks, in Content\\Scripts.\nIt compiles; nothing executes it yet." : kNoProject);
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
    const bool haveCsproj = !csproj.empty() && fileExists(csproj);
    const bool dotnetOk = haveDotnet();
    const bool canCompile = haveProject && haveCsproj && dotnetOk && !compileThread_.joinable();
    if (ImGui::MenuItem("Compile Scripts", nullptr, false, canCompile)) {
        open(Modal::Compile);
        startCompile(csproj);
    }
    tip(!haveProject  ? kNoProject
        : !dotnetOk   ? "dotnet was not found on PATH, so there is nothing to build with.\nInstall the .NET SDK and restart the editor."
        : !haveCsproj ? "This project has no Content\\Scripts\\Scripts.csproj yet.\nUse New C# Script or New C# Class to generate one."
        : compileThread_.joinable() ? "A build is already running."
        : "dotnet build on Content\\Scripts\\Scripts.csproj.\nCompiling is all this does - the engine still cannot run scripts.");

    ImGui::Separator();
    if (ImGui::MenuItem("Open Project Folder", nullptr, false, haveProject)) {
        if (!shellOpen(project.dir)) AVER_WARN("[Editor] could not open {}", project.dir);
    }
    tip(haveProject ? "Opens the project folder in Explorer."
                    : "Open or create a project first - there is no folder to show.");

    if (ImGui::MenuItem("Open in Visual Studio", nullptr, false, haveCsproj)) {
        if (!shellOpen(csproj)) AVER_WARN("[Editor] could not open {}", csproj);
    }
    tip(!haveProject  ? kNoProject
        : !haveCsproj ? "There is no Content\\Scripts\\Scripts.csproj to open yet.\nUse New C# Script or New C# Class to generate one."
        : "Opens Content\\Scripts\\Scripts.csproj through the shell, so whatever\nis registered for .csproj handles it - normally Visual Studio.");

    ImGui::EndMenu();
#endif
}

// ---------------------------------------------------------------------------------------------
// modals
// ---------------------------------------------------------------------------------------------

void ToolsMenu::drawModals(const fmt::ProjectDesc& project, f32 dpi) {
#if !AVER_WITH_IMGUI
    (void)project; (void)dpi;
#else
    // --new-script, gated on the SAME predicate as the menu item, so a run with no project proves
    // the item really is disabled rather than merely looking it — headless capture cannot open a
    // menu and click.
    if (armScript_ > 0) { if (project.valid() && pending_ == Modal::None) open(Modal::CsScript); --armScript_; }

    // --compile-scripts, same family and the same reasoning: gated on exactly what enables the
    // menu item, so a run that produces no modal has demonstrated a real disabled state.
    if (armCompile_ > 0) {
        const std::string csproj = project.valid() ? scriptsCsprojPath(project) : std::string();
        if (!csproj.empty() && fileExists(csproj) && haveDotnet() && !compileThread_.joinable()) {
            open(Modal::Compile);
            startCompile(csproj);
            armCompile_ = 0;
        } else {
            --armCompile_;
        }
    }

    switch (pending_) {
        case Modal::CsScript:  ImGui::OpenPopup("New C# Script"); break;
        case Modal::CsClass:   ImGui::OpenPopup("New C# Class"); break;
        case Modal::CppModule: ImGui::OpenPopup("New C++ Module"); break;
        case Modal::CppClass:  ImGui::OpenPopup("New C++ Class"); break;
        case Modal::Compile:   ImGui::OpenPopup("Compile Scripts"); break;
        case Modal::None:      break;
    }
    pending_ = Modal::None;

    drawCsModal(project, dpi, CsKind::Behaviour);
    drawCsModal(project, dpi, CsKind::PlainClass);
    drawCppModuleModal(dpi);
    drawCppClassModal(dpi);
    drawCompileModal(dpi);
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
    ImGui::TextDisabled(behaviour ? "OnStart / OnUpdate hooks included."
                                  : "A plain class - no lifecycle hooks.");

    ImGui::Spacing();
    ImGui::TextDisabled("Writes to %s", project.scriptsDir().c_str());

    ImGui::Spacing();
    ImGui::Separator();
    warnNoClr();
    ImGui::Separator();

    showError(error_);
    showResult(result_);

    ImGui::Spacing();
    const bool create = ImGui::Button(behaviour ? "Create Script" : "Create Class", ImVec2(150.0f * dpi, 0));
    if (create || submitted) {
        error_.clear(); result_.clear();
        std::string path; bool madeCsproj = false;
        if (createScript(project, name_, kind, &path, &madeCsproj, &error_)) {
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

void ToolsMenu::drawCompileModal(f32 dpi) {
    // Reaped here rather than in the menu: this is the one place that runs every frame regardless
    // of what is open, so a finished build is logged once and the thread is never left dangling.
    if (compile_ && compile_->done.load() && compileThread_.joinable()) {
        compileThread_.join();
        if (compile_->exitCode == 0) AVER_INFO("[Editor] Compile Scripts: {} built cleanly", compile_->csproj);
        else AVER_ERROR("[Editor] Compile Scripts: dotnet build exited {}", compile_->exitCode);
    }

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mv->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(860.0f * dpi, 520.0f * dpi), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Compile Scripts", nullptr, ImGuiWindowFlags_NoResize)) return;

    const bool running = compile_ && !compile_->done.load();
    if (compile_) ImGui::TextDisabled("dotnet build %s", compile_->csproj.c_str());

    if (running) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "Building...");
    } else if (compile_) {
        if (compile_->exitCode == 0)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Build succeeded (exit 0).");
        else
            ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "Build FAILED (exit %d).", compile_->exitCode);
    }

    ImGui::Separator();
    // The whole transcript, scrollable: an exit code alone cannot tell anyone which line of which
    // file the compiler objected to, and that is the only thing a failed build is asked.
    ImGui::BeginChild("##buildout", ImVec2(0, -46.0f * dpi), ImGuiChildFlags_Borders);
    // Wrapped, not horizontally scrolled: MSBuild prefixes every diagnostic with an absolute
    // path, so the part anyone actually needs — the error — starts well off the right edge and
    // would have to be scrolled to. Wrapping is uglier and readable; the alternative is neither.
    ImGui::PushTextWrapPos(0.0f);
    if (compile_ && !compile_->output.empty()) ImGui::TextUnformatted(compile_->output.c_str());
    else if (running) ImGui::TextDisabled("(waiting for dotnet)");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Compiling is all this does: the engine still has no in-process CLR host, "
                        "so nothing here will be executed (docs/STATUS.md \xC2\xA7""4d).");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
#endif // AVER_WITH_IMGUI

void ToolsMenu::startCompile(const std::string& csproj) {
    if (compileThread_.joinable()) return; // the menu item is disabled meanwhile; belt and braces

    auto job = std::make_shared<Compile>();
    job->csproj = csproj;
    compile_ = job;

    // The working directory is the Scripts folder so relative paths in MSBuild's diagnostics read
    // the way they do in a terminal opened there.
    const std::string dir = std::filesystem::path(csproj).parent_path().string();
    compileThread_ = std::thread([job, csproj, dir] {
        std::string out;
        int code = -1;
#if defined(_WIN32)
        const std::wstring cmd = L"dotnet build \"" + widen(csproj) + L"\" --nologo";
        if (!runCaptured(cmd, widen(dir), out, code)) {
            out = "Could not start dotnet.";
            code = -1;
        }
#else
        (void)csproj; (void)dir;
        out = "Compile Scripts is implemented for Windows only.";
#endif
        job->output = std::move(out);
        job->exitCode = code;
        job->done.store(true); // last: the UI thread reads output/exitCode once this is set
    });
}

} // namespace aver::editor
