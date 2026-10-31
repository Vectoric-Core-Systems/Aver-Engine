/* The services the engine's tools offer an extension through AverExtHost::getService (docs/EXTENSIONS.md, "Services and interfaces").
 *
 * A service is a table of functions that starts with its own size. The extension asks for it by name and version; a host that does not
 * offer it answers null, and the extension then reports that to its user instead of failing. Which services exist depends on the host:
 * AverAssetC and the editor offer the ones below, a test host offers what it likes.
 */
#ifndef AVER_EXTENSIONS_AVER_EXT_SERVICES_H
#define AVER_EXTENSIONS_AVER_EXT_SERVICES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "aver.asset.import-model", version 1: the engine's own model import. One model file (glTF, GLB, FBX, OBJ) is written as meshes,
 * materials, textures, skeletons and clips under <contentDir>/<outSubdir>. */
#define AVER_EXT_SERVICE_IMPORT_MODEL "aver.asset.import-model"
#define AVER_EXT_SERVICE_IMPORT_MODEL_VERSION 1

/* What the host is asked to import. All strings are UTF-8 and valid for the call only. */
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
    int32_t sourceIndex;      /* the source file's node index; -1 when the format has none */
} AverExtModelPart;

/* The import's outcome. The host allocates every pointer in it; the caller gives the whole struct back to freeResult. */
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

typedef struct AverExtImportModelService {
    uint32_t size;            /* sizeof(AverExtImportModelService) */
    /* 0 when the import produced meshes; the details are in `out` either way. */
    int32_t (*importModel)(const AverExtModelArgs* args, AverExtModelResult* out);
    /* Releases what importModel allocated into `out`. */
    void (*freeResult)(AverExtModelResult* out);
} AverExtImportModelService;

#ifdef __cplusplus
}
#endif

#endif
