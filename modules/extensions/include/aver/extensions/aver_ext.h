/* The native contract between the engine and an extension (docs/EXTENSIONS.md).
 *
 * Plain C: structs, UTF-8 `const char*` and function pointers cross the boundary, nothing else. An extension links its own copy
 * of the engine's static libraries, so no C++ object, allocation or std type may be exchanged.
 *
 * An extension exports one function, aver_extension_entry. The engine calls it once, the first time one of the extension's
 * commands runs, passing the host it provides; the extension fills in the API it offers. Every struct starts with its own size so
 * a field appended later is recognised by the side that knows it; a reader never looks past `size`.
 */
#ifndef AVER_EXTENSIONS_AVER_EXT_H
#define AVER_EXTENSIONS_AVER_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this header's contract. An extension's manifest names the one it was built for; the engine refuses others. */
#define AVER_EXT_ABI 1

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

/* What the host is asked to import: one model file (glTF, GLB, FBX, OBJ) written as meshes, materials, textures, skeletons and clips under
 * <contentDir>/<outSubdir>. All strings are UTF-8 and valid for the call only. */
typedef struct AverExtModelArgs {
    uint32_t size;            /* sizeof(AverExtModelArgs) */
    const char* input;        /* absolute path of the model file */
    const char* contentDir;   /* the project's Content folder */
    const char* outSubdir;    /* content-relative folder for the meshes */
    const char* base;         /* name stem for the materials the import writes */
} AverExtModelArgs;

/* One mesh the import wrote, with where it sits in the model (engine centimetres). */
typedef struct AverExtModelPart {
    const char* meshAsset;    /* content-relative path of the .ocmesh */
    const char* name;         /* the source node's name */
    double x, y, z;
    int32_t node;             /* the source node's index in a glTF file; -1 when the importer has none */
} AverExtModelPart;

/* The import's outcome. The host allocates every pointer in it; the caller gives the whole struct back to freeModelResult. */
typedef struct AverExtModelResult {
    uint32_t size;            /* sizeof(AverExtModelResult); the caller sets it before the call */
    int32_t ok;               /* non-zero when the import produced meshes */
    const char* error;        /* why not, when !ok; never null */
    const AverExtModelPart* parts;
    uint32_t partCount;
    const char* const* notes; /* anything the importer dropped, for the report */
    uint32_t noteCount;
    void* hostData;           /* the host's own, untouched by the extension */
} AverExtModelResult;

/* Provided by the engine. Valid from aver_extension_entry until shutdown returns. */
typedef struct AverExtHost {
    uint32_t size;            /* sizeof(AverExtHost) */
    int32_t abi;              /* AVER_EXT_ABI of the engine */
    const char* engineVersion;    /* "0.7.0-1" */
    const char* executableDir;    /* the directory of the running executable */
    const char* extensionDir;     /* <executableDir>/Extensions/<Name> of this extension */

    /* One line to the engine's log (and so the Output Log). May be called from any thread. Null host functions are never passed. */
    void (*log)(int32_t level, const char* msg);
    /* 0 when the import produced meshes; the details are in `out` either way. */
    int32_t (*importModel)(const AverExtModelArgs* args, AverExtModelResult* out);
    void (*freeModelResult)(AverExtModelResult* result);
    /* Progress of a long command: fraction in [0, 1], or -1 when unknown. */
    void (*progress)(double fraction, const char* text);
} AverExtHost;

/* Filled in by the extension. */
typedef struct AverExtApi {
    uint32_t size;            /* sizeof(AverExtApi); the engine sets it before the call */
    /* Runs a command the manifest lists. argv[0] is the command's name, argv[1..argc-1] its arguments. Returns the exit code. */
    int32_t (*runCommand)(const char* name, int32_t argc, const char* const* argv);
    /* Called once before the library is unloaded. May be null. */
    void (*shutdown)(void);
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
