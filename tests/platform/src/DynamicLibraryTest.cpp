// aver/core/DynamicLibrary.hpp -- loading a shared library at run time. Exit code = failure count.
//
// Every backend that has no import library to link against goes through this: the Vulkan loader, DXC,
// the .NET host. A wrong answer here is never a crash at the call site -- it is a backend that reports
// "no driver" or "no shader compiler" on a machine that has both -- so the contract is checked against
// libraries the operating system itself guarantees, not against anything the engine ships.
#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// A library every process on the platform already has mapped, and a function it exports.
#if defined(_WIN32)
static const char* kSystemLibrary = "kernel32.dll";
static const char* kSystemSymbol  = "GetCurrentProcessId";
#elif defined(__APPLE__)
static const char* kSystemLibrary = "/usr/lib/libSystem.B.dylib";
static const char* kSystemSymbol  = "getpid";
#else
static const char* kSystemLibrary = "libc.so.6";
static const char* kSystemSymbol  = "getpid";
#endif

int main() {
    {
        void* lib = loadLibrary(kSystemLibrary);
        check(lib != nullptr, std::string("the system library loads: ") + kSystemLibrary);
        check(librarySymbol(lib, kSystemSymbol) != nullptr, std::string("...and exports ") + kSystemSymbol);
        check(librarySymbol(lib, "aver_no_such_symbol_anywhere") == nullptr,
              "a symbol it does not export resolves to null, not to something arbitrary");
        freeLibrary(lib);
    }
    {
        void* lib = loadLibrary("aver-no-such-library-anywhere");
        check(lib == nullptr, "a library that does not exist does not load");
        check(!libraryError().empty(), "...and the failure says why, so a backend can log it");
    }
    {
        // The fallback chain every caller uses: the first name that loads wins, and the miss before it
        // is not an error worth stopping for.
        const char* names[] = {"aver-no-such-library-anywhere", kSystemLibrary};
        const char* loaded = nullptr;
        void* lib = loadFirstLibrary(names, 2, &loaded);
        check(lib != nullptr, "loadFirstLibrary falls past a missing name to one that exists");
        check(loaded != nullptr && std::string(loaded) == kSystemLibrary, "...and says which name loaded");
        freeLibrary(lib);
    }
    freeLibrary(nullptr);
    check(true, "freeLibrary(nullptr) is harmless");
    check(librarySymbol(nullptr, kSystemSymbol) == nullptr, "librarySymbol on a null handle is null");

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
