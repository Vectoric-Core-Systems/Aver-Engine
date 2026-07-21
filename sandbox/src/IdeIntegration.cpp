#include "IdeIntegration.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
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
//
// ToolsMenu.cpp has its own widen/runCaptured pair and keeps them. Sharing one copy would be the
// tidier tree, but that file is being edited concurrently for the Content Browser and gutting a
// hundred lines out of it to save thirty here buys a merge conflict with someone else's work. The
// capture below is also a much smaller thing than the build one: one short line of ASCII from
// `vswhere -utf8`, not an interleaved MSBuild transcript that has to survive a code-page guess.
// ---------------------------------------------------------------------------------------------

#if defined(_WIN32)

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<usize>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::wstring(buf, n);
}

bool existsW(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Run a short-lived console tool and collect its stdout. Used for `vswhere` only, which is why
// there is no stderr plumbing: vswhere's diagnostics are not something the editor can act on, and
// an empty result already means "no Visual Studio" whatever the reason.
bool captureStdout(const std::wstring& cmdline, std::string& out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); // or the child keeps the read end alive

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline; // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }

    char buf[1024];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, 10000); // a hung vswhere must not hold the scan thread forever
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Start a GUI process and forget it. No wait anywhere: the caller is a click handler and the
// editor keeps drawing while the IDE takes its ten seconds to appear.
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

bool shellOpen(const std::string& path) {
    const std::wstring w = widen(path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32; // ShellExecute's documented success threshold
}

std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

// ---------------------------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------------------------

// Visual Studio, via the installer's own locator. Hard-coding a path or a version is what breaks
// the moment someone installs a Preview beside a Community, or the next major version moves out of
// "2022" — vswhere is Microsoft's answer to exactly that and it ships with every install.
void findVisualStudio(std::vector<IdeInfo>& out) {
    // vswhere is NOT on PATH. It lives in a fixed, versionless location under the x86 Program
    // Files by contract — that path is the one thing about a VS install that is documented not to
    // move, which is why it is the only literal here.
    std::wstring root = envVar(L"ProgramFiles(x86)");
    if (root.empty()) root = envVar(L"ProgramFiles"); // a 32-bit host, where the x86 variable is absent
    if (root.empty()) return;
    const std::wstring vswhere = root + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!existsW(vswhere)) return;

    // -prerelease so a machine with only a Preview installed is not reported as having no Visual
    // Studio at all. -utf8 because the product path can contain non-ASCII and the pipe is bytes.
    std::string raw;
    if (!captureStdout(quoteArg(vswhere) + L" -latest -prerelease -utf8 -property productPath", raw))
        return;

    // One path per line; take the first non-empty one.
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

// VS Code. The per-user install is the default one the installer offers and is the one that ends
// up on PATH, but a system-wide install and a PATH-only install both exist, so all three are
// tried. Code.exe is preferred over code.cmd everywhere: the batch file only re-launches the exe,
// and going through it would mean a cmd.exe and a console window for no gain.
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
        // PATH carries `code.cmd`, not the exe. The shim sits in <root>\bin, so the exe is one
        // level up — derived rather than assumed, and only accepted if it is really there.
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

// Rider. Toolbox and the standalone installer disagree about the folder name and both version it,
// so the directory is scanned rather than guessed — but nothing is reported unless `rider64.exe`
// is actually on disk where the scan says. A name match alone would be a guess, and offering an
// IDE that is not there is worse than not offering it.
void findRider(std::vector<IdeInfo>& out) {
    const std::wstring roots[] = {
        envVar(L"LOCALAPPDATA") + L"\\Programs",
        envVar(L"ProgramFiles") + L"\\JetBrains",
    };
    std::error_code ec;
    for (const std::wstring& root : roots) {
        if (root.size() <= 10) continue; // the environment variable was missing
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_directory(ec)) continue;
            const std::wstring leaf = e.path().filename().wstring();
            if (leaf.find(L"Rider") == std::wstring::npos) continue;
            const std::filesystem::path exe = e.path() / L"bin" / L"rider64.exe";
            if (!existsW(exe.wstring())) continue;

            IdeInfo ide;
            ide.kind = IdeKind::Rider;
            ide.name = "Rider";
            ide.exePath = narrow(exe.wstring());
            ide.canGoto = true;
            out.push_back(std::move(ide));
            return; // one is enough; a Toolbox machine can have three versions side by side
        }
    }
}

#else // !_WIN32

bool shellOpen(const std::string&) { return false; }
void findVisualStudio(std::vector<IdeInfo>&) {}
void findVsCode(std::vector<IdeInfo>&) {}
void findRider(std::vector<IdeInfo>&) {}

#endif

IdeInfo shellEntry() {
    IdeInfo ide;
    ide.kind = IdeKind::ShellDefault;
    ide.name = "Default Editor";
    ide.canGoto = false; // the shell has no way to be told a line
    return ide;
}

// The detection cache. A worker thread fills `found` once and sets `done`; the main thread reads
// `fallback` until then. There is no lock on the vectors because only one side ever writes each,
// and `done` (release/acquire through std::atomic) is what publishes the write.
struct Registry {
    std::vector<IdeInfo> fallback{shellEntry()};
    std::vector<IdeInfo> found;
    std::atomic<bool> done{false};
    std::once_flag started;
    std::thread worker;

    ~Registry() {
        // A scan in flight owns a process handle. Joining at shutdown is a wait measured in
        // milliseconds; a detached thread writing into a destroyed vector is not bounded at all.
        if (worker.joinable()) worker.join();
    }
};

Registry& registry() {
    static Registry r;
    return r;
}

void startScan() {
    Registry& r = registry();
    r.worker = std::thread([&r] {
        std::vector<IdeInfo> list;
        findVisualStudio(list);
        findVsCode(list);
        findRider(list);
        list.push_back(shellEntry()); // always last, always present
        r.found = std::move(list);
        r.done.store(true, std::memory_order_release);
    });
}

// ---------------------------------------------------------------------------------------------
// MSBuild output parsing
// ---------------------------------------------------------------------------------------------

std::string trimmed(const std::string& s) {
    usize a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool endsWith(const std::string& s, const char* suffix) {
    const usize n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// "12,34" / "12" / "12,34,12,40" / "12-15" -> line, col. Anything that does not start with a digit
// is not a position at all, which is how `MSBUILD : error ...` avoids being read as a file.
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

const std::vector<IdeInfo>& detectedIdes() {
    Registry& r = registry();
    std::call_once(r.started, startScan);
    // Acquire, pairing with the worker's release: the vector's contents are only visible once the
    // flag is, and reading `found` before that is what the fallback exists to avoid.
    return r.done.load(std::memory_order_acquire) ? r.found : r.fallback;
}

bool ideDetectionFinished() {
    Registry& r = registry();
    std::call_once(r.started, startScan);
    return r.done.load(std::memory_order_acquire);
}

const IdeInfo& preferredIde() { return detectedIdes().front(); }

bool openInIde(const IdeInfo& ide, const std::string& file, int line, int col) {
    if (file.empty()) return false;
#if defined(_WIN32)
    const std::wstring wfile = widen(file);
    const std::wstring wexe = widen(ide.exePath);

    switch (ide.kind) {
    case IdeKind::VsCode: {
        // `--goto file:line:col` is the documented form and takes ONE argument, so the whole
        // triple is quoted together — a path with a space would otherwise split before the colons.
        std::wstring pos = wfile;
        if (line > 0) {
            pos += L":" + std::to_wstring(line);
            pos += L":" + std::to_wstring(col > 0 ? col : 1);
        }
        return launchDetached(quoteArg(wexe) + (line > 0 ? L" --goto " : L" ") + quoteArg(pos));
    }
    case IdeKind::Rider:
        // JetBrains' command-line form. Unverified on this machine — no Rider is installed here —
        // so the file argument comes last and stands alone, which is the part that cannot be wrong.
        return launchDetached(quoteArg(wexe) +
                              (line > 0 ? L" --line " + std::to_wstring(line) : L"") +
                              L" " + quoteArg(wfile));
    case IdeKind::VisualStudio: {
        // /edit reuses a running instance instead of starting a second devenv, which is what
        // anyone clicking a second error expects.
        //
        // MEASURED, not assumed, on Visual Studio Community 2026: against an instance that is
        // ALREADY UP this lands the caret exactly on the requested line (asked for 16, the status
        // bar read `Ln: 16, Ch: 5`). Against a COLD start it does not — devenv runs the /command
        // before the document has finished loading, and the caret was left at line 6 of a file it
        // was asked to open at line 13. The file is still the right file, so the failure is a
        // caret in the wrong place rather than a wrong window, and there is no synchronisation
        // devenv offers from the command line to close the gap. The UI therefore promises "opens
        // it in Visual Studio" and this comment is where the caveat lives.
        std::wstring cmd = quoteArg(wexe) + L" /edit " + quoteArg(wfile);
        if (line > 0) cmd += L" /command " + quoteArg(L"Edit.GoTo " + std::to_wstring(line));
        return launchDetached(cmd);
    }
    case IdeKind::ShellDefault:
    default:
        // No position. This is what the editor has always done and it is the one path that works
        // on a machine with nothing detected, so it stays exactly as plain as it was.
        return shellOpen(file);
    }
#else
    (void)ide; (void)line; (void)col;
    return false;
#endif
}

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

        // MSBuild's canonical shape is `<origin>: <category> <code>: <text>`. The category token
        // is found FIRST because the origin is a Windows path and full of colons of its own.
        const usize ep = raw.find(": error ");
        const usize wp = raw.find(": warning ");
        usize sev = std::string::npos;
        if (ep != std::string::npos && (wp == std::string::npos || ep < wp)) { sev = ep; bl.isError = true; }
        else if (wp != std::string::npos) { sev = wp; bl.isWarning = true; }

        if (sev != std::string::npos) {
            const usize afterCat = sev + (bl.isError ? 8 : 10);

            // ---- origin: a file with a position, or something project-level with neither
            const std::string origin = trimmed(raw.substr(0, sev));
            if (!origin.empty() && origin.back() == ')') {
                const usize lp = origin.rfind('(');
                if (lp != std::string::npos) {
                    int ln = 0, cl = 0;
                    if (parsePosition(origin.substr(lp + 1, origin.size() - lp - 2), ln, cl)) {
                        bl.line = ln;
                        bl.col = cl;
                        std::filesystem::path p(trimmed(origin.substr(0, lp)));
                        // Relative paths are relative to where the build RAN, and an IDE launched
                        // from the editor's directory would resolve them against the wrong root.
                        if (p.is_relative() && !baseDir.empty())
                            p = std::filesystem::path(baseDir) / p;
                        std::filesystem::path norm = p.lexically_normal();
                        bl.file = norm.make_preferred().string();
                    }
                }
            }

            // ---- code and message. `CS1061: 'Foo' does not contain...`, but a diagnostic may
            // carry no code at all (`: error : ...`), so the token is only accepted when it looks
            // like one — no spaces before the colon that ends it.
            std::string rest = trimmed(raw.substr(afterCat));
            const usize colon = rest.find(':');
            if (colon != std::string::npos && rest.find(' ') > colon) {
                bl.code = rest.substr(0, colon);
                rest = trimmed(rest.substr(colon + 1));
            }

            // MSBuild appends the project that was building when the diagnostic came out. It is
            // the same project on every line here and it pushes the actual message off the right
            // edge, so it goes — but only when the brackets really do hold a project file, because
            // a compiler message is perfectly entitled to end in a square bracket.
            if (!rest.empty() && rest.back() == ']') {
                const usize lb = rest.rfind(" [");
                if (lb != std::string::npos) {
                    const std::string inner = rest.substr(lb + 2, rest.size() - lb - 3);
                    if (endsWith(inner, "proj") || endsWith(inner, ".sln"))
                        rest = trimmed(rest.substr(0, lb));
                }
            }
            bl.message = rest;

            // dotnet build prints each diagnostic where it happened and again under "Build
            // FAILED.". Two clickable copies would make the list overstate how much is wrong.
            if (bl.hasPosition()) {
                const std::string key = bl.file + "|" + std::to_string(bl.line) + "|" +
                                        std::to_string(bl.col) + "|" + bl.code + "|" + bl.message;
                if (!seen.insert(key).second) continue;
            }
        }
        out.push_back(std::move(bl));
    }
    return out;
}

} // namespace aver::editor
