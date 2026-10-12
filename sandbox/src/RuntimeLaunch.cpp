// Starts AverEngineRuntime(.exe) on a level, detached from the editor. See RuntimeLaunch.hpp.

#include "RuntimeLaunch.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include "ProcessRun.hpp"   // launchDetached, widen
#endif

namespace aver::editor {
namespace {

#if defined(_WIN32)

// UTF-8 to UTF-16, for CreateProcessW. Same conversion as IdeIntegration.cpp's own widen() --
// copied rather than shared because that one is private to its translation unit.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quotes one command-line argument, doubling any trailing backslash run per the Windows rules --
// same shape as IdeIntegration.cpp's quoteArg, copied for the same reason as widen() above.
std::wstring quoteArg(const std::wstring& s) {
    usize trailing = 0;
    while (trailing < s.size() && s[s.size() - 1 - trailing] == L'\\') ++trailing;
    return L"\"" + s + std::wstring(trailing, L'\\') + L"\"";
}

#else

// One argument for /bin/sh: single-quoted, which expands nothing.
std::string shQuote(const std::string& s) {
    std::string q = "'";
    for (const char c : s) {
        if (c == '\'') q += "'\\''";
        else q.push_back(c);
    }
    return q + "'";
}

#endif // _WIN32

} // namespace

std::string runtimeExecutablePath() {
    const std::string path = executableDir() + AVER_PATH_SEP "AverEngineRuntime" + kExecutableSuffix;
    return fileExists(path) ? path : std::string();
}

bool launchRuntime(const std::string& projectManifestPath, const std::string& levelPath,
                   const std::string& extraArgs, std::string* why) {
#if defined(_WIN32)
    const std::string exe = runtimeExecutablePath();
    if (exe.empty()) {
        if (why) *why = "AverEngineRuntime.exe was not found beside the editor";
        return false;
    }

    // GameConfig (Runtime/src/GameApp.cpp) reads --project for the manifest and takes the bare
    // level argument via isLevelFile; both come from disk, so there is nothing else to pass.
    // ABSOLUTE, because the runtime starts in its own directory: a level opened from a relative
    // command-line path would otherwise resolve against the wrong folder.
    std::error_code ec;
    const std::string absManifest = std::filesystem::absolute(projectManifestPath, ec).string();
    const std::string absLevel    = std::filesystem::absolute(levelPath, ec).string();
    std::wstring cmdline = quoteArg(widen(exe)) +
                           L" --project " + quoteArg(widen(absManifest.empty() ? projectManifestPath : absManifest)) +
                           L" " + quoteArg(widen(absLevel.empty() ? levelPath : absLevel));
    // Raw, not quoted as one argument: this is the user's own command line text (e.g. "--seed 42
    // --windowed"), appended the way a shell would continue it, not a single path or value.
    if (!extraArgs.empty()) cmdline += L" " + widen(extraArgs);
    const std::wstring workDir = widen(executableDir());

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // CreateProcessW may write into its command-line buffer, hence the non-const `cmdline` above.
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.c_str(), &si, &pi)) {
        if (why) *why = "CreateProcessW failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    // Detached on purpose: this starts a second game, not a child the editor is responsible for.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    // The Windows branch's command line, for /bin/sh, started detached (ProcessRun.hpp).
    const std::string exe = runtimeExecutablePath();
    if (exe.empty()) {
        if (why) *why = "AverEngineRuntime was not found beside the editor";
        return false;
    }
    std::error_code ec;
    const std::string absManifest = std::filesystem::absolute(projectManifestPath, ec).string();
    const std::string absLevel    = std::filesystem::absolute(levelPath, ec).string();
    std::string cmdline = shQuote(exe) + " --project " + shQuote(absManifest.empty() ? projectManifestPath : absManifest) +
                          " " + shQuote(absLevel.empty() ? levelPath : absLevel);
    if (!extraArgs.empty()) cmdline += " " + extraArgs;   // the user's own text, as on Windows
    if (!launchDetached(widen(cmdline), widen(executableDir()))) {
        if (why) *why = "could not start AverEngineRuntime";
        return false;
    }
    return true;
#endif
}

} // namespace aver::editor
