#ifndef AVER_FRAMEWORK_DATALAYER_ABI_H
#define AVER_FRAMEWORK_DATALAYER_ABI_H

/* Data layers C ABI (Aver.Framework): lets C# and graphs read and set the runtime state of the level's named data layers
 * (DATALAYER records, docs/LEVEL_STREAMING.md).
 *
 * The layers and the streamer that honours them live in the game runtime, which the framework cannot link, so the host
 * installs a PROVIDER of function pointers and the framework relays the calls (the same shape as framework_terrain_abi.h).
 * With no provider every query answers 0 and leaves its outputs untouched. Strings are UTF-8, NUL-terminated, truncated
 * to the caller's buffer. Own version, bumped on additive change. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_DATALAYER_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_datalayer_abi_version(void);

/* A layer's state. Ordered: a placement is resident only when every layer it is in is at least LOADED; ACTIVATED additionally
 * lets it render and simulate. */
#define AVER_FW_DATALAYER_UNLOADED  0
#define AVER_FW_DATALAYER_LOADED    1
#define AVER_FW_DATALAYER_ACTIVATED 2

/* set_state results: 1 applied, 0 no such layer, -1 the layer is not a runtime layer (`runtime=0`), -2 a bad state value. */
typedef int32_t (AVER_FW_CALL* aver_fw_datalayer_count_fn)(void* user);
/* Copies the name of layer `index` (0-based); 1 if it exists. */
typedef int32_t (AVER_FW_CALL* aver_fw_datalayer_name_fn)(int32_t index, char* buf, int32_t cap, void* user);
/* The effective state of the named layer (never above its parent's); 1 if the layer exists. */
typedef int32_t (AVER_FW_CALL* aver_fw_datalayer_get_fn)(const char* name, int32_t* outState, void* user);
typedef int32_t (AVER_FW_CALL* aver_fw_datalayer_set_fn)(const char* name, int32_t state, void* user);
/* Pops the oldest finished state change (the streamer has acted on it); 1 if there was one. */
typedef int32_t (AVER_FW_CALL* aver_fw_datalayer_poll_fn)(char* nameBuf, int32_t nameCap, int32_t* outState, void* user);

AVER_FW_ABI int32_t aver_fw_set_datalayer_provider(aver_fw_datalayer_count_fn c, aver_fw_datalayer_name_fn n, aver_fw_datalayer_get_fn g,
                                                   aver_fw_datalayer_set_fn s, aver_fw_datalayer_poll_fn p, void* user);   /* always 1 */
AVER_FW_ABI int32_t aver_fw_datalayer_count(void);                                                  /* 0 with no provider */
AVER_FW_ABI int32_t aver_fw_datalayer_name(int32_t index, char* buf, int32_t cap);                  /* 0 with no provider */
AVER_FW_ABI int32_t aver_fw_datalayer_get_state(const char* name, int32_t* outState);               /* 0 with no provider */
AVER_FW_ABI int32_t aver_fw_datalayer_set_state(const char* name, int32_t state);                   /* 0 with no provider */
AVER_FW_ABI int32_t aver_fw_datalayer_poll_completed(char* nameBuf, int32_t nameCap, int32_t* outState);   /* 0 with no provider */

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_DATALAYER_ABI_H */
