#ifndef AVER_PREFAB_ABI_H
#define AVER_PREFAB_ABI_H

/* Aver.Prefab.Abi -- the plain-C seam a game script spawns and queries prefab instances through.
 *
 * Entities cross as int32_t, exactly as in scene_abi.h. The DLL holds NO prefab state: the host (the
 * editor, the runtime) owns the PrefabSystem and installs function pointers with aver_prefab_set_host,
 * the same shape aver_fw_set_anim_curve_provider has. Until a host is installed every call returns 0.
 * Setters and queries return 0 for failure and a positive value (or the entity) for success. */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PREFAB_ABI_BUILD)
#    define AVER_PREFAB_API __declspec(dllexport)
#  else
#    define AVER_PREFAB_API __declspec(dllimport)
#  endif
#else
#  define AVER_PREFAB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_PREFAB_ABI_VERSION_MAJOR 1
/* MINOR 1 adds aver_prefab_last_error, the reason channel. Additive only -- every existing
 * entry point keeps its 0 / non-zero return exactly, which is the whole design; see that function. */
#define AVER_PREFAB_ABI_VERSION_MINOR 1
#define AVER_PREFAB_ABI_VERSION \
    ((AVER_PREFAB_ABI_VERSION_MAJOR << 16) | AVER_PREFAB_ABI_VERSION_MINOR)

/* What the host provides. Every member may be NULL; the matching call then returns 0. */
typedef struct AverPrefabHost {
    void* user;
    /* Spawns an instance of the prefab at `path` (content-relative), parented to `parent` (0 = none),
     * at xyz centimetres, yaw/pitch/roll degrees and a uniform scale. Returns the instance root. */
    int32_t (*spawn)(void* user, const char* path, int32_t parent, const float* xyz,
                     const float* yawPitchRoll, float scale);
    int32_t (*destroy)(void* user, int32_t root);                       /* 1 on success */
    int32_t (*root_of)(void* user, int32_t entity);                     /* instance root, 0 if none */
    int32_t (*find)(void* user, int32_t root, const char* nodePath);    /* entity of a node, 0 if none */
    int32_t (*revert)(void* user, int32_t root);                        /* drop every override */
    int32_t (*override_count)(void* user, int32_t root);                /* how many overrides it has */
} AverPrefabHost;

/* The version this DLL was built with, so a caller can catch a stale binary. */
AVER_PREFAB_API int32_t aver_prefab_abi_version(void);

/* WHY the last call failed, as an aver::AbiError (modules/core/include/aver/core/ErrorCodes.hpp):
   0 ok, -1 bad handle, -3 not initialised (no host installed), -5 unsupported (the host lacks that
   function), -6 invalid argument (null or empty spawn path, or a spawn the host refused).
   The host callbacks carry no reason, so a host refusal reads as -1 for a root-taking call and -6 for
   spawn; the reason cannot be finer without changing AverPrefabHost.

   THREAD-LOCAL, and per-DLL: it reports failures seen by this DLL's own calls. Aver.Core is linked
   statically into each ABI DLL, so this slot and aver_scene_last_error's are different slots.

   SET ON SUCCESS TOO (to 0), so a stale reason cannot outlive the failure that produced it.
   aver_prefab_root_of / is_instance returning 0 ("not part of an instance") and
   aver_prefab_override_count returning 0 ("no overrides") are ordinary answers and record 0 when a
   host is installed. */
AVER_PREFAB_API int32_t aver_prefab_last_error(void);

/* Installs (or, with NULL, removes) the host. The struct is copied. */
AVER_PREFAB_API void aver_prefab_set_host(const AverPrefabHost* host);

AVER_PREFAB_API int32_t aver_prefab_spawn(const char* path, int32_t parent, float x, float y, float z,
                                          float yaw, float pitch, float roll, float scale);
AVER_PREFAB_API int32_t aver_prefab_destroy(int32_t root);
/* The instance root `entity` belongs to; 0 when it is not part of an instance. */
AVER_PREFAB_API int32_t aver_prefab_root_of(int32_t entity);
/* 1 when `entity` is part of a prefab instance, 0 otherwise. */
AVER_PREFAB_API int32_t aver_prefab_is_instance(int32_t entity);
/* The entity at `nodePath` ("" is the root, "5", "3/5") inside `root`'s instance; 0 if absent. */
AVER_PREFAB_API int32_t aver_prefab_find(int32_t root, const char* nodePath);
AVER_PREFAB_API int32_t aver_prefab_revert(int32_t root);
AVER_PREFAB_API int32_t aver_prefab_override_count(int32_t root);

#ifdef __cplusplus
}
#endif

#endif /* AVER_PREFAB_ABI_H */
