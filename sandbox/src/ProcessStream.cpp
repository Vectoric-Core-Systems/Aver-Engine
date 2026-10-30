#include "ProcessStream.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace aver::editor {

namespace {

// Hands every complete line in `pending` to the callback and keeps the unfinished tail.
void drainLines(std::string& pending, const std::function<void(const std::string&)>& onLine, bool final) {
    if (!onLine) { pending.clear(); return; }
    size_t from = 0;
    for (size_t nl; (nl = pending.find('\n', from)) != std::string::npos; from = nl + 1) {
        size_t end = nl;
        if (end > from && pending[end - 1] == '\r') --end;
        onLine(pending.substr(from, end - from));
    }
    pending.erase(0, from);
    if (final && !pending.empty()) { onLine(pending); pending.clear(); }
}

#if defined(_WIN32)

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The Windows command-line quoting rules: backslashes before a quote are doubled, so is a trailing run.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else { out.append(slashes, L'\\'); out.push_back(c); }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

#endif

}  // namespace

#if defined(_WIN32)

ProcessOutput runProcess(const std::string& program, const std::vector<std::string>& args,
                         const std::function<void(const std::string&)>& onLine) {
    ProcessOutput result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return result;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // the read end must not leak into the child, or the drain never sees EOF
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;

    std::wstring cmd = quoteArg(widenUtf8(program));
    for (const std::string& a : args) cmd += L" " + quoteArg(widenUtf8(a));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);   // ours is closed before the drain, for the same EOF reason
    if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return result; }

    result.started = true;
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) {
        result.text.append(buf, got);
        pending.append(buf, got);
        drainLines(pending, onLine, false);
    }
    CloseHandle(rd);
    drainLines(pending, onLine, true);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exitCode = static_cast<int>(code);
    return result;
}

#else

ProcessOutput runProcess(const std::string& program, const std::vector<std::string>& args,
                         const std::function<void(const std::string&)>& onLine) {
    ProcessOutput result;
    int fds[2];
    if (::pipe(fds) != 0) return result;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    std::vector<std::string> storage;
    storage.push_back(program);
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, program.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (rc != 0) { ::close(fds[0]); return result; }

    result.started = true;
    std::string pending;
    char buf[4096];
    for (;;) {
        const ssize_t got = ::read(fds[0], buf, sizeof buf);
        if (got > 0) {
            result.text.append(buf, static_cast<size_t>(got));
            pending.append(buf, static_cast<size_t>(got));
            drainLines(pending, onLine, false);
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fds[0]);
    drainLines(pending, onLine, true);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

}  // namespace aver::editor
