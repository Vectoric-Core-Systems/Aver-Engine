#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver {

// A shared library loaded at run time, for the backends that have no import library to link against:
// the Vulkan loader, DXC, the .NET host. LoadLibraryW/GetProcAddress/FreeLibrary on Windows,
// dlopen/dlsym/dlclose everywhere else.
//
// Free functions over an opaque handle and not a class, on purpose: every caller already kept its
// HMODULE in a struct of its own (VulkanApi, ScriptHost's Impl, a function-local static), and each
// decides for itself whether the library is ever unloaded. This keeps those decisions where they are.

// Loads `name`, UTF-8. A bare name searches the platform's usual places (beside the executable and
// the system directories on Windows, the dynamic linker's path elsewhere); a path loads that file.
// Null on failure, with the reason in libraryError().
void* loadLibrary(const char* name);

// Tries each of `names` in order and returns the first that loads, writing the name that did into
// `loaded` when it is non-null. Null when none does; libraryError() then describes the LAST miss.
void* loadFirstLibrary(const char* const* names, usize count, const char** loaded = nullptr);

// The address of an exported symbol, or null -- also for a null `lib`.
void* librarySymbol(void* lib, const char* symbol);

// Unloads a library loadLibrary returned. Null is ignored.
void freeLibrary(void* lib);

// Why the most recent loadLibrary on this thread failed, in the OS's words. Empty if it did not.
std::string libraryError();

} // namespace aver
