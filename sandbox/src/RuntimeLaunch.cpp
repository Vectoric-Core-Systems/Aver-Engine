// Starts AverEngineRuntime.exe on a level, detached from the editor. See RuntimeLaunch.hpp.

#include "RuntimeLaunch.hpp"

#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
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

#endif // _WIN32

} // namespace

std::string runtimeExecutablePath() {
    const std::string path = executableDir() + "\\AverEngineRuntime.exe";
    return fileExists(path) ? path : std::string();
}

bool launchRuntime(const std::string& projectManifestPath, const std::string& levelPath, std::string* why) {
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
    (void)projectManifestPath;
    (void)levelPath;
    if (why) *why = "launching the runtime is only implemented on Windows";
    return false;
#endif
}

} // namespace aver::editor
