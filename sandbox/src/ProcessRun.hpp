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

// The editor is a Windows program today; these exist so a non-Windows configure still compiles the
// call sites rather than needing its own #if around each one. A caller that wants to say something
// useful about the platform says it at the call site -- see ToolsMenu.cpp's "implemented for
// Windows only" messages -- because only the call site knows what the user was trying to do.
inline std::wstring widen(const std::string&) { return {}; }
inline std::string toUtf8(const std::string& raw) { return raw; }
inline bool runCaptured(const std::wstring&, const std::wstring&, std::string&, int&) { return false; }

#endif

} // namespace aver::editor
