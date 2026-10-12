#include "aver/core/DynamicLibrary.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>

namespace aver {
namespace {

thread_local std::string t_error;

#if defined(_WIN32)
std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<usize>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string describe(DWORD code) {
    char buf[256] = {};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                                   0, buf, sizeof buf, nullptr);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.empty()) {
        char num[32];
        std::snprintf(num, sizeof num, "error %lu", static_cast<unsigned long>(code));
        s = num;
    }
    return s;
}
#endif

}  // namespace

void* loadLibrary(const char* name) {
    t_error.clear();
    if (!name || !*name) { t_error = "no library name given"; return nullptr; }
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(widen(name).c_str());
    if (!h) t_error = std::string(name) + ": " + describe(GetLastError());
    return reinterpret_cast<void*>(h);
#else
    // RTLD_LOCAL: a loaded backend's symbols must not become the resolution of some later library's
    // undefined ones. RTLD_NOW: a library missing a dependency fails here, where the error names it,
    // and not at the first call into it.
    void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* why = dlerror();
        t_error = why ? why : (std::string(name) + ": dlopen failed");
    }
    return h;
#endif
}

void* loadFirstLibrary(const char* const* names, usize count, const char** loaded) {
    for (usize i = 0; i < count; ++i) {
        if (void* h = loadLibrary(names[i])) {
            if (loaded) *loaded = names[i];
            return h;
        }
    }
    if (loaded) *loaded = nullptr;
    return nullptr;
}

void* librarySymbol(void* lib, const char* symbol) {
    if (!lib || !symbol) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), symbol));
#else
    return dlsym(lib, symbol);
#endif
}

void freeLibrary(void* lib) {
    if (!lib) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

std::string libraryError() { return t_error; }

} // namespace aver
