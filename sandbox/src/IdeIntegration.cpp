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

// How long the whole capture below may take. It bounds the READ, which is the part that can hang:
// a child that never writes and never exits leaves a blocking ReadFile blocked, and the scan thread
// is joined at shutdown, so an unbounded read here is an editor that never closes.
constexpr DWORD kCaptureTimeoutMs = 10000;

// Run a short-lived console tool and collect its stdout. Used for `vswhere` only, which is why
// there is no stderr plumbing: vswhere's diagnostics are not something the editor can act on, and
// an empty result already means "no Visual Studio" whatever the reason.
//
// Returns false on timeout as well as on failure to launch: a truncated `vswhere` answer is a
// truncated path, and half a path is worse than no Visual Studio.
bool captureStdout(const std::wstring& cmdline, std::string& out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); // or the child keeps the read end alive

    // STARTF_USESTDHANDLES makes the child's stdin whatever is in hStdInput, and a null one is a
    // handle it cannot read from — a console tool that decides to prompt would fail in a way that
    // depends on how it checks. NUL gives it an immediate EOF, which is the answer meant here.
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
    std::wstring mutableCmd = cmdline; // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }

    // Peek before every read so no read can block. An anonymous pipe cannot be opened for
    // overlapped I/O, so a deadline over a poll is what is left; the alternative is a second thread
    // per launch to make a blocking read cancellable, which is more machinery than one `vswhere`
    // deserves. Peek failing means the write end is gone, which is the normal end of the output.
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

    // Killed rather than abandoned: a child that ignored its deadline would otherwise outlive the
    // editor that started it, and the wait below would spend the timeout a second time.
    if (timedOut) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, kCaptureTimeoutMs);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return !timedOut;
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

// Quote one argument for a command line the child will parse with the standard Windows rules.
// Backslashes are only special in front of a quote, where each PAIR becomes one backslash — so a
// path ending in one (`C:\`, the only shape that reaches this) would otherwise escape the closing
// quote and swallow the rest of the line. Doubling the trailing run is what those rules ask for.
// Embedded quotes are not handled because a Windows path cannot contain one.
std::wstring quoteArg(const std::wstring& s) {
    usize trailing = 0;
    while (trailing < s.size() && s[s.size() - 1 - trailing] == L'\\') ++trailing;
    return L"\"" + s + std::wstring(trailing, L'\\') + L"\"";
}

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

bool nameMentionsRider(const std::wstring& leaf) {
    std::wstring lower;
    lower.reserve(leaf.size());
    for (wchar_t c : leaf) lower.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c);
    return lower.find(L"rider") != std::wstring::npos;
}

// `<dir>\bin\rider64.exe`, or the same a level or two further down. The standalone installer puts
// bin\ straight under the versioned folder; the Toolbox has historically kept a channel and a
// version directory in between (`Rider\ch-0\<version>\bin`) and newer Toolbox layouts do not. Two
// levels covers both without turning into a walk of the whole drive.
//
// `directory_iterator`'s error_code overload only covers CONSTRUCTION — the increment throws — so
// the iterator is stepped by hand. An unreadable directory ends this branch of the scan and nothing
// else; the caller has other roots to try.
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

// Rider. Toolbox and the standalone installer disagree about the folder name and both version it,
// so the directory is scanned rather than guessed — but nothing is reported unless `rider64.exe`
// is actually on disk where the scan says. A name match alone would be a guess, and offering an
// IDE that is not there is worse than not offering it.
//
// None of this has been exercised against a real install: no Rider is present on this machine, so
// the roots below are what JetBrains documents and what the Toolbox is reported to use, not
// something that has been seen to work. The failure mode is silence — Rider simply not offered.
void findRider(std::vector<IdeInfo>& out) {
    // Built from the environment rather than assumed, and a root is skipped when its variable is
    // ABSENT: a 32-bit host has no %ProgramFiles(x86)%, and appending to an empty string would
    // produce a relative path that the scan would then resolve against the editor's own directory.
    std::vector<std::filesystem::path> roots;
    const auto addRoot = [&roots](const std::wstring& base, const wchar_t* tail) {
        if (!base.empty()) roots.emplace_back(base + tail);
    };
    const std::wstring localAppData = envVar(L"LOCALAPPDATA");
    addRoot(localAppData, L"\\Programs");
    addRoot(localAppData, L"\\JetBrains\\Toolbox\\apps"); // the Toolbox's own install root
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
        // Nothing detection can hit is worth the editor for. This thread's exceptions have no one
        // to catch them — an escaping one is std::terminate, so a filesystem error under someone's
        // %LOCALAPPDATA% would take the whole process down while they were editing. Whatever was
        // found before the throw is kept, and the shell entry below still makes the list usable.
        try {
            findVisualStudio(list);
            findVsCode(list);
            findRider(list);
        } catch (const std::exception& e) {
            AVER_WARN("[Editor] IDE detection stopped early: {}", e.what());
        } catch (...) {
            AVER_WARN("[Editor] IDE detection stopped early");
        }
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

const IdeInfo& preferredGotoIde() {
    const std::vector<IdeInfo>& ides = detectedIdes();
    // The two preferences differ on purpose. Opening a project is something Visual Studio does
    // properly and is what someone with both installed usually means by "open my scripts", so that
    // keeps the detection order. Landing a caret on a line is the opposite way round: VS Code's
    // `--goto` is the only jump measured to work from cold on this machine, while devenv's
    // /command runs before the document has loaded and leaves the caret where it was (see the
    // Visual Studio case in openInIde). Sending every diagnostic click to the IDE whose jump is
    // known not to arrive is the one outcome worth reordering for.
    //
    // Only VS Code is promoted. Rider's form is documented but untested here, so it is left where
    // detection put it rather than moved up on a guess.
    for (const IdeInfo& ide : ides)
        if (ide.kind == IdeKind::VsCode) return ide;
    return ides.front();
}

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
            // FAILED.", and the modal's header is these entries counted — so a surviving second
            // copy is not merely a repeated row, it is a header that contradicts MSBuild's own
            // "N Error(s)" line two rows below it.
            //
            // Two diagnostics are the same one when they point at the same place; one with NO
            // position — the `CSC :` and `MSBUILD :` origins — has nothing but its text to be the
            // same by, so that is what it is matched on. Restricted to diagnostics: ordinary
            // output repeats itself legitimately and every line of it has to survive.
            const std::string key =
                bl.hasPosition()
                    ? "pos|" + bl.file + "|" + std::to_string(bl.line) + "|" +
                          std::to_string(bl.col) + "|" + bl.code + "|" + bl.message
                    : "raw|" + trimmed(raw);
            if (!seen.insert(key).second) continue;

            // The file's LEAF, not the absolute path MSBuild emitted. The path is what pushed the
            // message off the right edge of the panel; with it in the tooltip instead, the error
            // itself fits on one clickable row.
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
