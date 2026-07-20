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

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<usize>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return {};
    std::string s = narrow(raw);
    CoTaskMemFree(raw);
    return s;
}

} // namespace
#endif

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

std::string userDataDir() {
#if defined(_WIN32)
    const std::string local = knownFolder(FOLDERID_LocalAppData);
    if (!local.empty()) return local + "\\AverEngine";
#endif
    return executableDir();
}

std::string documentsDir() {
#if defined(_WIN32)
    const std::string docs = knownFolder(FOLDERID_Documents);
    if (!docs.empty()) return docs;
#endif
    return executableDir();
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

bool directoryExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool createDirectories(const std::string& path) {
    std::error_code ec;
    // create_directories reports false for an already-existing path, which is a success here.
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::is_directory(path, ec);
}

bool readFileBytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) return false;
    return true;
}

bool readFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(out.data(), size)) return false;
    return true;
}

bool writeFileBytes(const std::string& path, const void* data, usize size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (size > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

bool writeFileText(const std::string& path, const std::string& text) {
    return writeFileBytes(path, text.data(), text.size());
}

bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out) {
#if defined(_WIN32)
    // The dialog is COM, and the engine thread has not initialised COM for itself. Do it per call
    // and undo it, so nothing else in the process inherits an apartment it did not ask for.
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
