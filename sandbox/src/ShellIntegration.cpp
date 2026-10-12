// The desktop's verbs for a path: reveal it, open it, recycle it. The Windows shell on Windows; the
// file manager, xdg-open and the freedesktop trash on Linux; Finder, `open` and the Trash on macOS.
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
#else
#include "ProcessRun.hpp"   // runCaptured, launchDetached, findOnPath, widen
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace aver::editor {
namespace {

#if defined(_WIN32)
// UTF-8 to UTF-16.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A wide, absolute, backslash-separated path. SHFileOperation silently does nothing with a forward
// slash, so the shell APIs below only ever see this form.
std::wstring nativeAbsolute(const std::string& path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (ec) p = std::filesystem::path(path);
    return widen(p.make_preferred().string());
}
#endif
#if !defined(_WIN32)
std::string absoluteOf(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::absolute(path, ec);
    return (ec ? std::filesystem::path(path) : p).lexically_normal().string();
}

// One argument for /bin/sh: single-quoted, which expands nothing.
std::string shQuote(const std::string& s) {
    std::string q = "'";
    for (const char c : s) {
        if (c == '\'') q += "'\\''";
        else q.push_back(c);
    }
    return q + "'";
}

bool runQuiet(const std::string& cmdline) {
    std::string out;
    int code = -1;
    return runCaptured(widen(cmdline), std::wstring(), out, code) && code == 0;
}

#if !defined(__APPLE__)
// A file:// URL for an absolute path, percent-encoding everything but the unreserved set and '/'.
std::string fileUrl(const std::string& abs) {
    static const char* hex = "0123456789ABCDEF";
    std::string url = "file://";
    for (const unsigned char c : abs) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') url.push_back(static_cast<char>(c));
        else { url.push_back('%'); url.push_back(hex[c >> 4]); url.push_back(hex[c & 15]); }
    }
    return url;
}

// The freedesktop.org trash, home trash only, per the Trash specification: reserve a name by creating
// its .trashinfo with O_EXCL, then rename the item into files/. A rename cannot cross filesystems, so
// an item on another volume goes to `gio trash`, which knows the per-volume .Trash-$uid directories.
bool trashXdg(const std::string& abs) {
    std::string data;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x == '/') data = x;
    else if (const char* h = std::getenv("HOME"); h && *h) data = std::string(h) + "/.local/share";
    else return false;
    const std::string trash = data + "/Trash";
    std::error_code ec;
    std::filesystem::create_directories(trash + "/files", ec);
    std::filesystem::create_directories(trash + "/info", ec);

    const std::string base = std::filesystem::path(abs).filename().string();
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &lt);
    for (int n = 0; n < 1000; ++n) {
        const std::string name = n == 0 ? base : base + "." + std::to_string(n);
        const std::string info = trash + "/info/" + name + ".trashinfo";
        const int fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; return false; }
        const std::string url = fileUrl(abs).substr(7);   // the Path key wants the encoded path, no scheme
        const std::string text = "[Trash Info]\nPath=" + url + "\nDeletionDate=" + stamp + "\n";
        const bool wrote = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
        ::close(fd);
        if (wrote && std::rename(abs.c_str(), (trash + "/files/" + name).c_str()) == 0) return true;
        const int err = errno;
        std::remove(info.c_str());
        if (err == EXDEV && findOnPath("gio")) return runQuiet("gio trash " + shQuote(abs));
        return false;
    }
    return false;
}
#endif
#endif

} // namespace

// Opens the file manager with `path` selected. A directory opens showing itself.
bool revealInFileManager(const std::string& path) {
#if defined(_WIN32)
    std::error_code ec;
    const std::filesystem::path p(path);
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
    const std::string abs = absoluteOf(path);
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(abs, ec);
#if defined(__APPLE__)
    // Finder: a directory opens, a file is shown selected in its folder.
    if (launchDetached(widen(std::string(isDir ? "open " : "open -R ") + shQuote(abs)), std::wstring())) return true;
#else
    // The file manager's own "show this item" call (Nautilus, Dolphin, Nemo, COSMIC Files all answer
    // org.freedesktop.FileManager1), then the folder in whatever xdg-open picks.
    if (!isDir && findOnPath("dbus-send") &&
        runQuiet("dbus-send --session --dest=org.freedesktop.FileManager1 --type=method_call "
                 "/org/freedesktop/FileManager1 org.freedesktop.FileManager1.ShowItems "
                 "array:string:" + shQuote(fileUrl(abs)) + " string:''"))
        return true;
    const std::string folder = isDir ? abs : std::filesystem::path(abs).parent_path().string();
    if (launchDetached(widen("xdg-open " + shQuote(folder)), std::wstring())) return true;
#endif
    AVER_WARN("[Shell] could not reveal '{}'", path);
    return false;
#endif
}

// Hands `path` to whatever the shell has registered for its extension.
bool openWithShell(const std::string& path) {
#if defined(_WIN32)
    const std::wstring w = nativeAbsolute(path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) { AVER_WARN("[Shell] nothing is registered to open '{}'", path); return false; }
    return true;
#else
#if defined(__APPLE__)
    const char* opener = "open ";
#else
    const char* opener = "xdg-open ";
#endif
    if (launchDetached(widen(opener + shQuote(absoluteOf(path))), std::wstring())) return true;
    AVER_WARN("[Shell] nothing is registered to open '{}'", path);
    return false;
#endif
}

// Moves `path` to the recycle bin. Works on directories too.
bool moveToRecycleBin(const std::string& path) {
#if defined(_WIN32)
    // pFrom is a DOUBLE-null-terminated list, so the terminator is built explicitly.
    const std::wstring w = nativeAbsolute(path);
    if (w.empty()) return false;
    std::vector<wchar_t> from(w.begin(), w.end());
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.data();
    // FOF_WANTNUKEWARNING is required: FOF_NOCONFIRMATION otherwise answers "Yes to All" to the
    // shell's "cannot be recycled — delete permanently?" prompt and still returns 0.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING |
                FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;

    const int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        AVER_WARN("[Shell] could not recycle '{}' (code {}{})", path, rc,
                  op.fAnyOperationsAborted ? ", aborted" : "");
        return false;
    }
    return true;
#else
    const std::string abs = absoluteOf(path);
#if defined(__APPLE__)
    // /usr/bin/trash ships with macOS 15; before it, Finder does the same over AppleScript.
    const bool ok = ::access("/usr/bin/trash", X_OK) == 0
                        ? runQuiet("/usr/bin/trash " + shQuote(abs))
                        : runQuiet("osascript -e " + shQuote("tell application \"Finder\" to delete POSIX file \"" + abs + "\""));
#else
    const bool ok = trashXdg(abs);
#endif
    if (!ok) AVER_WARN("[Shell] could not move '{}' to the trash", path);
    return ok;
#endif
}

} // namespace aver::editor
