// File I/O, well-known directories and the native open-file dialog.

#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#endif

namespace aver {

#if defined(_WIN32)
namespace {

// Converts a wide string to UTF-8.
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<usize>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// Converts a UTF-8 string to a wide string.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

// Asks the shell for a known folder's path. Empty on failure.
std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return {};
    std::string s = narrow(raw);
    CoTaskMemFree(raw);
    return s;
}

} // namespace
#endif

// The directory containing the running executable.
std::string executableDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    const auto p = w.find_last_of(L"\\/");
    if (p != std::wstring::npos) w = w.substr(0, p);
    return narrow(w);
#else
    return ".";
#endif
}

// The per-user, per-machine state directory.
std::string userDataDir() {
#if defined(_WIN32)
    const std::string local = knownFolder(FOLDERID_LocalAppData);
    if (!local.empty()) return local + "\\AverEngine";
#endif
    return executableDir();
}

// The user's Documents folder.
std::string documentsDir() {
#if defined(_WIN32)
    const std::string docs = knownFolder(FOLDERID_Documents);
    if (!docs.empty()) return docs;
#endif
    return executableDir();
}

// True if the path exists.
bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

// True if the path exists and is a directory.
bool directoryExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

// Creates the directory and every missing parent. True if it exists afterwards.
bool createDirectories(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::is_directory(path, ec);
}

// Reads a whole file into `out`. False if it cannot be read.
namespace {
FileTraceFn g_trace = nullptr;
void* g_traceUser = nullptr;
}

void setFileTrace(FileTraceFn fn, void* user) { g_trace = fn; g_traceUser = user; }

void traceFileOpen(const std::string& path) { if (g_trace) g_trace(path.c_str(), g_traceUser); }

bool readFileBytes(const std::string& path, std::vector<u8>& out) {
    if (g_trace) g_trace(path.c_str(), g_traceUser);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) return false;
    return true;
}

// Reads a whole file into `out` as text. False if it cannot be read.
bool readFileText(const std::string& path, std::string& out) {
    // Traced on ENTRY, before the open is attempted, so a path that fails to open is still
    // reported. A fallback that misses is exactly as informative as one that hits: it says the
    // package looked outside itself.
    if (g_trace) g_trace(path.c_str(), g_traceUser);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(out.data(), size)) return false;
    return true;
}

// Writes `size` bytes to the file, truncating it. False on failure.
bool writeFileBytes(const std::string& path, const void* data, usize size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (size > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

// Writes `text` to the file, truncating it. False on failure.
bool writeFileText(const std::string& path, const std::string& text) {
    return writeFileBytes(path, text.data(), text.size());
}

// Shows the shell's open-file dialog. False if cancelled or unavailable.
bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out) {
#if defined(_WIN32)
    // COM is initialised per call and undone, so nothing else inherits an apartment.
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = SUCCEEDED(ci);
    if (ci == RPC_E_CHANGED_MODE) { /* someone else owns the apartment; the dialog still works */ }
    else if (!weInitialised) return false;

    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
        const std::wstring wTitle = widen(title), wLabel = widen(filterLabel), wSpec = widen(spec);
        const COMDLG_FILTERSPEC filters[] = {{wLabel.c_str(), wSpec.c_str()}, {L"All files", L"*.*"}};
        dlg->SetTitle(wTitle.c_str());
        dlg->SetFileTypes(2, filters);
        if (!initialDir.empty()) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(widen(initialDir).c_str(), nullptr, IID_PPV_ARGS(&start))) && start) {
                dlg->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR raw = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                    out = narrow(raw);
                    CoTaskMemFree(raw);
                    ok = true;
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (weInitialised) CoUninitialize();
    return ok;
#else
    (void)title; (void)filterLabel; (void)spec; (void)initialDir; (void)out;
    return false;
#endif
}

} // namespace aver
