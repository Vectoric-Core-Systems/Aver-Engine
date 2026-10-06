#ifndef AVER_FRAMEWORK_BLACKBOARD_ABI_H
#define AVER_FRAMEWORK_BLACKBOARD_ABI_H

/* Blackboard C ABI (Aver.Framework): typed per-entity AI blackboard keys, relayed to Synapse.
 *
 * Same conventions as framework_abi.h (int32_t/int64_t/float/const char* only, out-pointers for
 * results, 1 on success and 0 on refusal). Aver.Framework does not link Aver.Synapse.Scene, so a
 * composition root installs ONE provider function and every call below forwards to it; with no
 * provider everything returns 0. The provider for Synapse is synapse::blackboardRelay
 * (modules/synapse.scene/include/aver/synapse/SynapseBt.hpp). Semantics: docs/BLACKBOARD_BT.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_BLACKBOARD_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_blackboard_abi_version(void);

/* Value types (0 = the key is not defined). */
#define AVER_FW_BB_NONE   0
#define AVER_FW_BB_BOOL   1
#define AVER_FW_BB_INT    2
#define AVER_FW_BB_FLOAT  3
#define AVER_FW_BB_VEC3   4
#define AVER_FW_BB_STRING 5
#define AVER_FW_BB_ENTITY 6

#define AVER_FW_BB_SCOPE_AGENT  0   /* private to the entity                  */
#define AVER_FW_BB_SCOPE_SHARED 1   /* lives on the entity's team board       */

/* Provider operations. `type` is an AVER_FW_BB_* code. Get: *i / f[0..2] / text receive the value
 * (Bool, Int and Entity in *i; Float in f[0]; Vec3 in f[0..2]; String in text). Set mirrors that and
 * defines the key (agent scope) when it is missing. Define: *i is the scope. SetTeam: `key` is the
 * team name. */
#define AVER_FW_BB_OP_TYPE     0
#define AVER_FW_BB_OP_GET      1
#define AVER_FW_BB_OP_SET      2
#define AVER_FW_BB_OP_RESET    3
#define AVER_FW_BB_OP_DEFINE   4
#define AVER_FW_BB_OP_SETTEAM  5

typedef int32_t (AVER_FW_CALL* aver_fw_blackboard_fn)(int32_t op, int32_t entity, const char* key,
                                                       int32_t type, int64_t* i, float* f, char* text,
                                                       int32_t textCap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_blackboard_provider(aver_fw_blackboard_fn fn, void* user);

/* The key's AVER_FW_BB_* type, 0 when the key or the entity's board does not exist. */
AVER_FW_ABI int32_t aver_fw_bb_type(int32_t e, const char* key);

/* Reads. Return 1 and write the output, or 0 (output untouched) when the key is missing, the stored
 * value cannot convert to the asked type, or there is no provider. Numeric types convert between
 * themselves (Bool/Int/Float/Entity). get_string writes at most cap-1 bytes and a terminator. */
AVER_FW_ABI int32_t aver_fw_bb_get_bool(int32_t e, const char* key, int32_t* out);
AVER_FW_ABI int32_t aver_fw_bb_get_int(int32_t e, const char* key, int64_t* out);
AVER_FW_ABI int32_t aver_fw_bb_get_float(int32_t e, const char* key, float* out);
AVER_FW_ABI int32_t aver_fw_bb_get_vec3(int32_t e, const char* key, float* out3);
AVER_FW_ABI int32_t aver_fw_bb_get_string(int32_t e, const char* key, char* buf, int32_t cap);
AVER_FW_ABI int32_t aver_fw_bb_get_entity(int32_t e, const char* key, int32_t* out);

/* Writes. Define the key on first use (agent scope); 0 when an existing key's type cannot take the
 * value (a String into an Int key, say). */
AVER_FW_ABI int32_t aver_fw_bb_set_bool(int32_t e, const char* key, int32_t value);
AVER_FW_ABI int32_t aver_fw_bb_set_int(int32_t e, const char* key, int64_t value);
AVER_FW_ABI int32_t aver_fw_bb_set_float(int32_t e, const char* key, float value);
AVER_FW_ABI int32_t aver_fw_bb_set_vec3(int32_t e, const char* key, float x, float y, float z);
AVER_FW_ABI int32_t aver_fw_bb_set_string(int32_t e, const char* key, const char* value);
AVER_FW_ABI int32_t aver_fw_bb_set_entity(int32_t e, const char* key, int32_t value);

/* Restores a key to its default. */
AVER_FW_ABI int32_t aver_fw_bb_reset(int32_t e, const char* key);
/* Defines a key with a type and scope (AVER_FW_BB_SCOPE_*). 0 when it exists with another type. */
AVER_FW_ABI int32_t aver_fw_bb_define(int32_t e, const char* key, int32_t type, int32_t scope);
/* Puts the entity on a team ("" = the default team); its Shared keys then live on that board. */
AVER_FW_ABI int32_t aver_fw_bb_set_team(int32_t e, const char* team);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_BLACKBOARD_ABI_H */
