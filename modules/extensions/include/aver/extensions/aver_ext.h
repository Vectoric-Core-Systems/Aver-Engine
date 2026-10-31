/* The native contract between the engine and an extension (docs/EXTENSIONS.md).
 *
 * Plain C: structs, UTF-8 `const char*` and function pointers cross the boundary, nothing else. An extension links its own copy
 * of the engine's static libraries, so no C++ object, allocation or std type may be exchanged.
 *
 * An extension exports one function, aver_extension_entry. The engine calls it once, the first time one of the extension's
 * commands runs, passing the host it provides; the extension fills in the API it offers. Every struct starts with its own size so
 * a field appended later is recognised by the side that knows it; a reader never looks past `size`.
 *
 * What the host offers beyond the fields below is a named, versioned service (AverExtHost::getService, aver_ext_services.h); what an
 * extension offers beyond its commands is a named, versioned interface (AverExtApi::getInterface). Names are reverse-DNS-style:
 * lower-case segments of [a-z0-9-] joined by '.', at least two segments, the engine's own under "aver.". A version is a positive
 * integer that changes only when the table's existing fields change meaning; a field appended to the end keeps the version, and the
 * reader tells it by the table's `size`.
 */
#ifndef AVER_EXTENSIONS_AVER_EXT_H
#define AVER_EXTENSIONS_AVER_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this header's contract. An extension's manifest names the one it was built for; the engine refuses others.
 * 2: services and interfaces replace the fixed model-import fields of the host (1 is not loaded any more). */
#define AVER_EXT_ABI 2

#if defined(_WIN32)
#define AVER_EXT_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define AVER_EXT_EXPORT __attribute__((visibility("default")))
#else
#define AVER_EXT_EXPORT
#endif

/* AverExtHost::log levels: the engine's own LogLevel values (aver/core/Log.hpp). */
#define AVER_EXT_LOG_TRACE    0
#define AVER_EXT_LOG_INFO     1
#define AVER_EXT_LOG_WARN     2
#define AVER_EXT_LOG_ERROR    3
#define AVER_EXT_LOG_CRITICAL 4
#define AVER_EXT_LOG_FATAL    5

/* Provided by the engine. Valid from aver_extension_entry until shutdown returns. */
typedef struct AverExtHost {
    uint32_t size;            /* sizeof(AverExtHost) */
    int32_t abi;              /* AVER_EXT_ABI of the engine */
    const char* engineVersion;    /* "0.7.0-1" */
    const char* executableDir;    /* the directory of the running executable */
    const char* extensionDir;     /* <executableDir>/Extensions/<Name> of this extension */

    /* One line to the engine's log (and so the Output Log). May be called from any thread. Null host functions are never passed. */
    void (*log)(int32_t level, const char* msg);
    /* Progress of a long command: fraction in [0, 1], or -1 when unknown. A command that runs under a tool also prints progress lines
     * (aver_ext_protocol.h); this is for a host that shows progress itself. */
    void (*progress)(double fraction, const char* text);
    /* The service `name` at `version` (aver_ext_services.h): a table that starts with its own size, owned by the host and valid until
     * shutdown returns. Null when the host does not offer that name at that version. May be called from any thread. */
    const void* (*getService)(const char* name, uint32_t version);
} AverExtHost;

/* Filled in by the extension. */
typedef struct AverExtApi {
    uint32_t size;            /* sizeof(AverExtApi); the engine sets it before the call */
    /* Runs a command the manifest lists. argv[0] is the command's name, argv[1..argc-1] its arguments. Returns the exit code. */
    int32_t (*runCommand)(const char* name, int32_t argc, const char* const* argv);
    /* Called once before the library is unloaded. May be null. */
    void (*shutdown)(void);
    /* The interface `name` at `version` this extension offers: a table that starts with its own size, owned by the extension and valid
     * until shutdown returns. Null when it offers none by that name and version. The function itself may be null. */
    const void* (*getInterface)(const char* name, uint32_t version);
} AverExtApi;

/* The extension's only required export, declared by the extension with AVER_EXT_EXPORT:
 *     AVER_EXT_EXPORT int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out);
 * It returns 0 on success, and 1 when host->abi is one the extension does not support. */
typedef int32_t (*AverExtensionEntryFn)(const AverExtHost* host, AverExtApi* out);
#define AVER_EXT_ENTRY_NAME "aver_extension_entry"

#ifdef __cplusplus
}
#endif

#endif
