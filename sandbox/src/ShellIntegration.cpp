#include "ShellIntegration.hpp"

#include "aver/core/Log.hpp"

#include <filesystem>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace aver::editor {
namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The shell APIs below want a real, absolute, backslash-separated path. A path assembled from a
// breadcrumb click can carry forward slashes, and SHFileOperation in particular silently does
// nothing with one.
std::wstring nativeAbsolute(const std::string& path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (ec) p = std::filesystem::path(path);
    return widen(p.make_preferred().string());
}
#endif

} // namespace

bool revealInFileManager(const std::string& path) {
#if defined(_WIN32)
    std::error_code ec;
    const std::filesystem::path p(path);
    // A directory is shown by opening it; a file is shown by opening its parent with it selected.
    // `explorer /select,` on a directory selects it inside its PARENT, which is not what "show me
    // this folder" means when the user is already looking at the folder list.
    if (std::filesystem::is_directory(p, ec)) {
        const std::wstring w = nativeAbsolute(path);
        const auto r = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (r <= 32) { AVER_WARN("[Shell] could not open '{}' in the file manager", path); return false; }
        return true;
    }
    const std::wstring args = L"/select,\"" + nativeAbsolute(path) + L"\"";
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    if (r <= 32) { AVER_WARN("[Shell] could not reveal '{}'", path); return false; }
    return true;
#else
    AVER_WARN("[Shell] revealing '{}' is not implemented on this platform", path);
    return false;
#endif
}

bool openWithShell(const std::string& path) {
#if defined(_WIN32)
    const std::wstring w = nativeAbsolute(path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) { AVER_WARN("[Shell] nothing is registered to open '{}'", path); return false; }
    return true;
#else
    AVER_WARN("[Shell] opening '{}' is not implemented on this platform", path);
    return false;
#endif
}

bool moveToRecycleBin(const std::string& path) {
#if defined(_WIN32)
    // pFrom is a DOUBLE-null-terminated list, not a plain string. Getting this wrong reads past the
    // buffer, so the terminator is built explicitly rather than relying on a std::wstring's own.
    const std::wstring w = nativeAbsolute(path);
    if (w.empty()) return false;
    std::vector<wchar_t> from(w.begin(), w.end());
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.data();
    // ALLOWUNDO is the whole point: it is what makes this the recycle bin rather than an unlink.
    // NOCONFIRMATION suppresses the SHELL's prompt only — the editor has already asked.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;

    const int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        AVER_WARN("[Shell] could not recycle '{}' (code {}{})", path, rc,
                  op.fAnyOperationsAborted ? ", aborted" : "");
        return false;
    }
    return true;
#else
    AVER_WARN("[Shell] recycling '{}' is not implemented on this platform", path);
    return false;
#endif
}

} // namespace aver::editor
