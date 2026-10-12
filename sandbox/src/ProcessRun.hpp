// RUN A CONSOLE TOOL, WAIT FOR IT, READ WHAT IT SAID -- one copy, for the translation units that
// share it.
//
// WHY IT MOVED. runCaptured() was a file-static in ToolsMenu.cpp, where it served `dotnet build`,
// avermatc and stage-game.ps1. RevisionControl.cpp needs the identical thing for `git` -- same
// pipe, same NUL stdin, same CREATE_NO_WINDOW, same "wait, then read the exit code" contract -- and
// a second hand-written copy of it is how this tree acquired the ones named below. So the whole
// helper (with the two encoding conversions it cannot work without) is promoted into a header the
// way Phase B of the SandboxApp split promoted every other helper that grew a second caller, and
// ToolsMenu.cpp includes it instead of declaring it.
//
// THE THREE PIPE HAZARDS THIS ENCODES, none of them obvious and all of them paid for once already:
// the read end must NOT be inheritable or the drain never sees EOF and the loop below hangs; stdin
// must be a real NUL handle rather than nothing, or a child that reads stdin blocks forever behind
// a console it does not have; and the write end must be closed in THIS process before the drain
// starts, for the same EOF reason. A caller writing its own CreateProcessW gets all three wrong in
// some order, which is the actual argument for there being one of these.
//
// IT BLOCKS, DELIBERATELY AND WITHOUT A DEADLINE. WaitForSingleObject(INFINITE) is correct for the
// callers it has -- a build the user asked for and is watching, a git query over a local
// repository -- and wrong for anything on a UI thread that must stay responsive, which is why
// ToolsMenu.cpp runs its builds on a std::thread. A caller that needs a timeout wants
// IdeIntegration.cpp's captureStdout shape instead (see below), not a flag bolted onto this.
//
// THE OTHER COPIES IN THIS TREE ARE DELIBERATELY LEFT ALONE, because neither is the same function:
//   * sandbox/src/IdeIntegration.cpp -- captureStdout(). A DIFFERENT CONTRACT: it polls with
//     PeekNamedPipe against a 10-second deadline and TerminateProcess()es on expiry, because it is
//     interrogating IDE probes that may never exit. Folding it into this would mean giving every
//     caller a timeout or giving that one none.
//   * modules/formats.roslyn/src/AverDesign.cpp -- runCapture(). A DIFFERENT MODULE: Aver.Formats.
//     Roslyn is a library the editor links, and a library may not include a sandbox/src header.
//     Deduplicating that one means promoting the helper into Aver.Platform, which is a wider change
//     than this commit is, and is worth doing on its own terms rather than as a side effect.
#pragma once
#include "aver/core/Types.hpp"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace aver::editor {

#if defined(_WIN32)

// UTF-8 to UTF-16, for the W-suffixed API.
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Child-process bytes to UTF-8, re-reading as the OEM code page when they are not already UTF-8.
// A console tool inherits no code page from us, so its diagnostics arrive in whatever the console
// default is; handing those bytes to ImGui unconverted is what turns an accented path in a build
// error into mojibake.
inline std::string toUtf8(const std::string& raw) {
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
//
// FALSE MEANS THE PROCESS NEVER STARTED, and is a different answer from a non-zero `exitCode`:
// "git is not installed" and "git said no" are different things to tell the user, and a caller that
// collapses them reports a missing tool as a failing command. Nothing here is written to `out`
// unless the child ran.
//
// THE TWO STREAMS SHARE ONE PIPE on purpose: a tool's diagnostics are interleaved with its output
// in the order it produced them, which is the order the reader wants to see. A caller that needs
// them apart -- and a machine-readable format like git's porcelain is exactly such a caller -- must
// ask the tool for a format it can parse regardless, not hope stderr stayed out of the way.
inline bool runCaptured(const std::wstring& cmdline, const std::wstring& cwd, std::string& out, int& exitCode) {
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

#else // !_WIN32

// THE SAME THREE FUNCTIONS, for a POSIX editor. The callers build Windows-shaped command lines --
// quoteArg()'d paths joined with spaces -- and those mean the same thing to /bin/sh for every path a
// project can contain, so the line is handed to `sh -c` as it is. wchar_t is UTF-32 here, so widen()
// and narrow() are a real UTF-8 <-> UTF-32 conversion, not the byte copy they would be on Windows.

// UTF-8 to UTF-32. A malformed byte becomes U+FFFD and does not end the string.
inline std::wstring widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (usize i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + static_cast<usize>(len) > s.size()) { w.push_back(0xFFFD); ++i; continue; }
        u32 cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<usize>(k)]) & 0x3Fu);
        w.push_back(static_cast<wchar_t>(cp));
        i += static_cast<usize>(len);
    }
    return w;
}

// UTF-32 to UTF-8.
inline std::string narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        const u32 cp = static_cast<u32>(wc);
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return s;
}

// A child's output is already UTF-8 under any locale a desktop sets, which is the one case Windows'
// OEM-code-page fallback exists for.
inline std::string toUtf8(const std::string& raw) { return raw; }

// Runs `cmdline` through /bin/sh to completion, stdout and stderr down one pipe into `out`, stdin
// from /dev/null. The Windows contract: false means the process never started. The same pipe hazard
// is handled -- the write end is closed here before the drain, so the loop sees EOF when the child
// exits.
inline bool runCaptured(const std::wstring& cmdline, const std::wstring& cwd, std::string& out, int& exitCode) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    const std::string dir = narrow(cwd);
    if (!dir.empty()) posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
    const std::string line = narrow(cmdline);
    char sh[] = "/bin/sh", dashC[] = "-c";
    char* argv[] = {sh, dashC, const_cast<char*>(line.c_str()), nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/bin/sh", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (rc != 0) { ::close(fds[0]); return false; }
    std::string raw;
    char buf[4096];
    for (;;) {
        const ssize_t got = ::read(fds[0], buf, sizeof buf);
        if (got > 0) { raw.append(buf, static_cast<usize>(got)); continue; }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    out = raw;
    // A program sh could not find exits 127: kept as an exit code, since the shell itself did start.
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

// Starts `cmdline` through /bin/sh and does not wait for it: a second program the editor starts and
// is not responsible for (the standalone runtime, an IDE, the file manager). Double-forked so the
// editor never has to reap it and never leaves a zombie, and in a session of its own so closing the
// editor does not take it down. Only async-signal-safe calls happen between fork() and exec().
inline bool launchDetached(const std::wstring& cmdline, const std::wstring& cwd) {
    const std::string line = narrow(cmdline);
    const std::string dir = narrow(cwd);
    const pid_t first = ::fork();
    if (first < 0) return false;
    if (first == 0) {
        ::setsid();
        if (::fork() != 0) ::_exit(0);
        if (!dir.empty() && ::chdir(dir.c_str()) != 0) ::_exit(127);
        const int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) { ::dup2(nul, 0); ::dup2(nul, 1); ::dup2(nul, 2); if (nul > 2) ::close(nul); }
        ::execl("/bin/sh", "sh", "-c", line.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(first, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// True if `exe` names an executable program on PATH.
inline bool findOnPath(const std::string& exe) {
    const char* path = std::getenv("PATH");
    if (!path || exe.empty()) return false;
    const std::string dirs(path);
    for (usize b = 0; b <= dirs.size();) {
        usize e = dirs.find(':', b);
        if (e == std::string::npos) e = dirs.size();
        const std::string candidate = (e > b ? dirs.substr(b, e - b) : std::string(".")) + "/" + exe;
        if (::access(candidate.c_str(), X_OK) == 0) return true;
        b = e + 1;
    }
    return false;
}
#endif

} // namespace aver::editor
