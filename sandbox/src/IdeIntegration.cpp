// IDE integration: detects Visual Studio, VS Code and Rider, opens files and projects in them,
// and parses MSBuild output into clickable diagnostics.

#include "IdeIntegration.hpp"

#include "aver/core/Log.hpp"

#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace aver::editor {
namespace {

// ---------------------------------------------------------------------------------------------
// Win32 plumbing
// ---------------------------------------------------------------------------------------------

#if defined(_WIN32)

// UTF-8 to UTF-16.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// UTF-16 to UTF-8.
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<usize>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Reads an environment variable. Empty if unset or too long.
std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::wstring(buf, n);
}

// True if the path names an existing regular file.
bool existsW(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

constexpr DWORD kCaptureTimeoutMs = 10000;

// Runs a short-lived console tool and collects its stdout. False on launch failure or timeout.
bool captureStdout(const std::wstring& cmdline, std::string& out) {
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
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;   // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }

    // Peek before every read: an anonymous pipe cannot be opened for overlapped I/O.
    bool timedOut = false;
    const ULONGLONG deadline = GetTickCount64() + kCaptureTimeoutMs;
    char buf[1024];
    for (;;) {
        if (GetTickCount64() >= deadline) { timedOut = true; break; }
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) { Sleep(5); continue; }
        DWORD got = 0;
        if (!ReadFile(rd, buf, sizeof buf, &got, nullptr) || got == 0) break;
        out.append(buf, got);
    }
    CloseHandle(rd);

    if (timedOut) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, kCaptureTimeoutMs);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return !timedOut;
}

// Starts a GUI process and forgets it. Does not wait.
bool launchDetached(const std::wstring& cmdline, DWORD flags = 0) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, flags,
                        nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Opens a path with the shell's default handler.
bool shellOpen(const std::string& path) {
    const std::wstring w = widen(path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;   // ShellExecute's documented success threshold
}

// Quotes one command-line argument, doubling any trailing backslash run per the Windows rules.
std::wstring quoteArg(const std::wstring& s) {
    usize trailing = 0;
    while (trailing < s.size() && s[s.size() - 1 - trailing] == L'\\') ++trailing;
    return L"\"" + s + std::wstring(trailing, L'\\') + L"\"";
}

// ---------------------------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------------------------

// Appends Visual Studio to the list if vswhere reports an install.
void findVisualStudio(std::vector<IdeInfo>& out) {
    // vswhere is not on PATH; its location under the x86 Program Files is fixed by contract.
    std::wstring root = envVar(L"ProgramFiles(x86)");
    if (root.empty()) root = envVar(L"ProgramFiles");
    if (root.empty()) return;
    const std::wstring vswhere = root + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!existsW(vswhere)) return;

    std::string raw;
    if (!captureStdout(quoteArg(vswhere) + L" -latest -prerelease -utf8 -property productPath", raw))
        return;

    std::string path;
    for (usize i = 0, n = raw.size(); i < n;) {
        const usize e = raw.find_first_of("\r\n", i);
        std::string t = raw.substr(i, (e == std::string::npos ? n : e) - i);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (!t.empty()) { path = t; break; }
        if (e == std::string::npos) break;
        i = e + 1;
    }
    if (path.empty() || !existsW(widen(path))) return;

    IdeInfo ide;
    ide.kind = IdeKind::VisualStudio;
    ide.name = "Visual Studio";
    ide.exePath = path;
    ide.canGoto = true;
    out.push_back(std::move(ide));
}

// Appends VS Code to the list if a per-user, system-wide or PATH install is found.
void findVsCode(std::vector<IdeInfo>& out) {
    std::wstring exe;
    const std::wstring candidates[] = {
        envVar(L"LOCALAPPDATA") + L"\\Programs\\Microsoft VS Code\\Code.exe",
        envVar(L"ProgramFiles") + L"\\Microsoft VS Code\\Code.exe",
        envVar(L"ProgramFiles(x86)") + L"\\Microsoft VS Code\\Code.exe",
    };
    for (const std::wstring& c : candidates)
        if (existsW(c)) { exe = c; break; }

    if (exe.empty()) {
        // PATH carries `code.cmd`, which sits in <root>\bin, so the exe is one level up.
        wchar_t found[MAX_PATH];
        if (SearchPathW(nullptr, L"code.cmd", nullptr, MAX_PATH, found, nullptr) > 0) {
            std::filesystem::path p(found);
            const std::filesystem::path guess = p.parent_path().parent_path() / L"Code.exe";
            if (existsW(guess.wstring())) exe = guess.wstring();
        }
    }
    if (exe.empty()) return;

    IdeInfo ide;
    ide.kind = IdeKind::VsCode;
    ide.name = "VS Code";
    ide.exePath = narrow(exe);
    ide.canGoto = true;
    out.push_back(std::move(ide));
}

// True if a folder name contains "rider", case-insensitively.
bool nameMentionsRider(const std::wstring& leaf) {
    std::wstring lower;
    lower.reserve(leaf.size());
    for (wchar_t c : leaf) lower.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c);
    return lower.find(L"rider") != std::wstring::npos;
}

// Looks for `<dir>\bin\rider64.exe`, recursing up to `depth` subdirectories.
bool findRiderExe(const std::filesystem::path& dir, int depth, std::wstring& exeOut) {
    const std::filesystem::path exe = dir / L"bin" / L"rider64.exe";
    if (existsW(exe.wstring())) { exeOut = exe.wstring(); return true; }
    if (depth <= 0) return false;

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc) || entryEc) continue;
        if (findRiderExe(it->path(), depth - 1, exeOut)) return true;
    }
    return false;
}

// Appends Rider to the list if rider64.exe is found under a Toolbox or standalone install root.
void findRider(std::vector<IdeInfo>& out) {
    std::vector<std::filesystem::path> roots;
    const auto addRoot = [&roots](const std::wstring& base, const wchar_t* tail) {
        if (!base.empty()) roots.emplace_back(base + tail);
    };
    const std::wstring localAppData = envVar(L"LOCALAPPDATA");
    addRoot(localAppData, L"\\Programs");
    addRoot(localAppData, L"\\JetBrains\\Toolbox\\apps");
    addRoot(envVar(L"ProgramFiles"), L"\\JetBrains");
    addRoot(envVar(L"ProgramFiles(x86)"), L"\\JetBrains");

    for (const std::filesystem::path& root : roots) {
        std::error_code ec;
        std::filesystem::directory_iterator it(root, ec);
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (!it->is_directory(entryEc) || entryEc) continue;
            if (!nameMentionsRider(it->path().filename().wstring())) continue;

            std::wstring exe;
            if (!findRiderExe(it->path(), 2, exe)) continue;

            IdeInfo ide;
            ide.kind = IdeKind::Rider;
            ide.name = "Rider";
            ide.exePath = narrow(exe);
            ide.canGoto = true;
            out.push_back(std::move(ide));
            return;
        }
    }
}

#else // !_WIN32

bool shellOpen(const std::string&) { return false; }
void findVisualStudio(std::vector<IdeInfo>&) {}
void findVsCode(std::vector<IdeInfo>&) {}
void findRider(std::vector<IdeInfo>&) {}

#endif

// The always-present "open with the shell's default handler" entry.
IdeInfo shellEntry() {
    IdeInfo ide;
    ide.kind = IdeKind::ShellDefault;
    ide.name = "Default Editor";
    ide.canGoto = false;
    return ide;
}

// Detection cache: a worker thread fills `found` and sets `done`; readers see `fallback` until then.
struct Registry {
    std::vector<IdeInfo> fallback{shellEntry()};
    std::vector<IdeInfo> found;
    std::atomic<bool> done{false};
    std::once_flag started;
    std::thread worker;

    // Joins the scan thread.
    ~Registry() {
        if (worker.joinable()) worker.join();
    }
};

// The process-wide detection cache.
Registry& registry() {
    static Registry r;
    return r;
}

// Starts the background IDE scan. Called once.
void startScan() {
    Registry& r = registry();
    r.worker = std::thread([&r] {
        std::vector<IdeInfo> list;
        try {
            findVisualStudio(list);
            findVsCode(list);
            findRider(list);
        } catch (const std::exception& e) {
            AVER_WARN("[Editor] IDE detection stopped early: {}", e.what());
        } catch (...) {
            AVER_WARN("[Editor] IDE detection stopped early");
        }
        list.push_back(shellEntry());
        r.found = std::move(list);
        r.done.store(true, std::memory_order_release);
    });
}

// ---------------------------------------------------------------------------------------------
// MSBuild output parsing
// ---------------------------------------------------------------------------------------------

// Strips leading and trailing spaces, tabs and a trailing CR.
std::string trimmed(const std::string& s) {
    usize a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// True if the string ends with the suffix.
bool endsWith(const std::string& s, const char* suffix) {
    const usize n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Parses an MSBuild position, "12,34" or "12" or "12,34,12,40". False if it does not start with a
// digit.
bool parsePosition(const std::string& s, int& line, int& col) {
    usize i = 0;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    int v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
    line = v;
    col = 0;
    if (i < s.size() && s[i] == ',') {
        ++i;
        if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            int c = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) c = c * 10 + (s[i++] - '0');
            col = c;
        }
    }
    return true;
}

} // namespace

// The detected IDEs, or the shell-only fallback while the scan is still running.
const std::vector<IdeInfo>& detectedIdes() {
    Registry& r = registry();
    std::call_once(r.started, startScan);
    return r.done.load(std::memory_order_acquire) ? r.found : r.fallback;
}

// True once the background IDE scan has published its result.
bool ideDetectionFinished() {
    Registry& r = registry();
    std::call_once(r.started, startScan);
    return r.done.load(std::memory_order_acquire);
}

// The IDE to open a project in: the first one detected.
const IdeInfo& preferredIde() { return detectedIdes().front(); }

// The IDE to jump to a file and line in: VS Code if present, else the first one detected.
const IdeInfo& preferredGotoIde() {
    const std::vector<IdeInfo>& ides = detectedIdes();
    for (const IdeInfo& ide : ides)
        if (ide.kind == IdeKind::VsCode) return ide;
    return ides.front();
}

// Opens a file in the given IDE, at a line and column when it supports one.
bool openInIde(const IdeInfo& ide, const std::string& file, int line, int col) {
    if (file.empty()) return false;
#if defined(_WIN32)
    const std::wstring wfile = widen(file);
    const std::wstring wexe = widen(ide.exePath);

    switch (ide.kind) {
    case IdeKind::VsCode: {
        // `--goto file:line:col` takes one argument, so the whole triple is quoted together.
        std::wstring pos = wfile;
        if (line > 0) {
            pos += L":" + std::to_wstring(line);
            pos += L":" + std::to_wstring(col > 0 ? col : 1);
        }
        return launchDetached(quoteArg(wexe) + (line > 0 ? L" --goto " : L" ") + quoteArg(pos));
    }
    case IdeKind::Rider:
        return launchDetached(quoteArg(wexe) +
                              (line > 0 ? L" --line " + std::to_wstring(line) : L"") +
                              L" " + quoteArg(wfile));
    case IdeKind::VisualStudio: {
        // devenv quirk: /command runs before a cold-started document loads, so the caret lands
        // only against an instance that is already up.
        std::wstring cmd = quoteArg(wexe) + L" /edit " + quoteArg(wfile);
        if (line > 0) cmd += L" /command " + quoteArg(L"Edit.GoTo " + std::to_wstring(line));
        return launchDetached(cmd);
    }
    case IdeKind::ShellDefault:
    default:
        return shellOpen(file);
    }
#else
    (void)ide; (void)line; (void)col;
    return false;
#endif
}

// Opens a C# project in the given IDE. VS Code gets the containing folder instead of the file.
bool openProjectInIde(const IdeInfo& ide, const std::string& csprojPath) {
    if (csprojPath.empty()) return false;
#if defined(_WIN32)
    if (ide.kind == IdeKind::VsCode) {
        const std::string dir = std::filesystem::path(csprojPath).parent_path().string();
        return launchDetached(quoteArg(widen(ide.exePath)) + L" " + quoteArg(widen(dir)));
    }
#endif
    return openInIde(ide, csprojPath, 0, 0);
}

// Splits MSBuild output into lines, tagging errors and warnings with file, position, code and a
// clickable label. Duplicate diagnostics are dropped; relative paths resolve against `baseDir`.
std::vector<BuildLine> parseBuildOutput(const std::string& output, const std::string& baseDir) {
    std::vector<BuildLine> out;
    std::unordered_set<std::string> seen;

    usize i = 0;
    const usize n = output.size();
    while (i <= n) {
        const usize e = output.find('\n', i);
        const usize stop = (e == std::string::npos) ? n : e;
        std::string raw = output.substr(i, stop - i);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        i = (e == std::string::npos) ? n + 1 : e + 1;

        BuildLine bl;
        bl.raw = raw;

        // MSBuild shape: `<origin>: <category> <code>: <text>`. The category is found first
        // because the origin is a Windows path full of colons.
        const usize ep = raw.find(": error ");
        const usize wp = raw.find(": warning ");
        usize sev = std::string::npos;
        if (ep != std::string::npos && (wp == std::string::npos || ep < wp)) { sev = ep; bl.isError = true; }
        else if (wp != std::string::npos) { sev = wp; bl.isWarning = true; }

        if (sev != std::string::npos) {
            const usize afterCat = sev + (bl.isError ? 8 : 10);

            // ---- origin: a file with a position, or something project-level with neither ----
            const std::string origin = trimmed(raw.substr(0, sev));
            if (!origin.empty() && origin.back() == ')') {
                const usize lp = origin.rfind('(');
                if (lp != std::string::npos) {
                    int ln = 0, cl = 0;
                    if (parsePosition(origin.substr(lp + 1, origin.size() - lp - 2), ln, cl)) {
                        bl.line = ln;
                        bl.col = cl;
                        std::filesystem::path p(trimmed(origin.substr(0, lp)));
                        if (p.is_relative() && !baseDir.empty())
                            p = std::filesystem::path(baseDir) / p;
                        std::filesystem::path norm = p.lexically_normal();
                        bl.file = norm.make_preferred().string();
                    }
                }
            }

            // ---- code and message ----
            std::string rest = trimmed(raw.substr(afterCat));
            const usize colon = rest.find(':');
            if (colon != std::string::npos && rest.find(' ') > colon) {
                bl.code = rest.substr(0, colon);
                rest = trimmed(rest.substr(colon + 1));
            }

            // ---- strip the trailing " [<project file>]" MSBuild appends ----
            if (!rest.empty() && rest.back() == ']') {
                const usize lb = rest.rfind(" [");
                if (lb != std::string::npos) {
                    const std::string inner = rest.substr(lb + 2, rest.size() - lb - 3);
                    if (endsWith(inner, "proj") || endsWith(inner, ".sln"))
                        rest = trimmed(rest.substr(0, lb));
                }
            }
            bl.message = rest;

            // ---- drop the second copy dotnet build prints under "Build FAILED." ----
            const std::string key =
                bl.hasPosition()
                    ? "pos|" + bl.file + "|" + std::to_string(bl.line) + "|" +
                          std::to_string(bl.col) + "|" + bl.code + "|" + bl.message
                    : "raw|" + trimmed(raw);
            if (!seen.insert(key).second) continue;

            // ---- the clickable label: file leaf, position, code, message ----
            if (bl.hasPosition()) {
                bl.label = std::filesystem::path(bl.file).filename().string();
                bl.label += "(" + std::to_string(bl.line);
                if (bl.col > 0) bl.label += "," + std::to_string(bl.col);
                bl.label += "): ";
                bl.label += bl.isError ? "error" : "warning";
                if (!bl.code.empty()) bl.label += " " + bl.code;
                bl.label += ": " + bl.message;
            }
        }
        out.push_back(std::move(bl));
    }
    return out;
}

} // namespace aver::editor
