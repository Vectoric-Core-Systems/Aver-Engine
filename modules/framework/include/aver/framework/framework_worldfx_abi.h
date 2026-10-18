#ifndef AVER_FRAMEWORK_WORLDFX_ABI_H
#define AVER_FRAMEWORK_WORLDFX_ABI_H

/* World effects C ABI (Aver.Framework): script overrides for the sun, fog and sky light, and named post-process
 * layers. Same shape as aver_fw_set_sky_clouds (framework_abi.h): the script PUBLISHES, the host READS each frame, and
 * a key nobody has set leaves the level exactly as authored.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/int64_t/float only; 1 on success and 0 on refusal.
 * All state lives in this DLL (no provider relay), single game thread. Names travel as fnv1a64 hashes
 * (Assets.ObjectIdOf in C#): a post LAYER is the hash of its name as written, a post KEY is the hash of the lower-case
 * key from kOcPostKeys (OcWorld.hpp), a VOLUME is the hash of the POSTVOLUME record's name as authored. A key that is
 * not in that table is refused and logged once.
 *
 * Order of a frame (both hosts): authored volumes (weight times aver_fw_post_volume_scale), then one transient
 * infinite volume per layer at priority 10000 + index, then blendPostVolumes, then the player's accessibility
 * override, which wins by design. Semantics: docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.4. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_WORLDFX_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_worldfx_abi_version(void);

/* Post layers and volume scales are bounded; a request beyond either is refused. */
#define AVER_FW_POST_MAX_LAYERS 16
#define AVER_FW_POST_MAX_SCALES 32
/* A layer a script never gave a weight engages by itself over this many seconds. */
#define AVER_FW_POST_AUTO_BLEND 0.15f

/* ---- environment ---------------------------------------------------------------------------------------------
 * Values are sanitised on the way in: non-finite is refused, lux / density / falloff / start / colour / sky light are
 * clamped to >= 0, temperature to 0..40000, max opacity to 0..1; the sun direction must have length > 0 and is stored
 * normalised. Components a key does not use are stored as 0. */
#define AVER_FW_ENV_SUN_DIR      1  /* v0..v2, world direction TOWARD the light (OcWorldEnv::sunDir) */
#define AVER_FW_ENV_SUN_LUX      2  /* v0 */
#define AVER_FW_ENV_SUN_COLOR    3  /* v0..v2 linear rgb */
#define AVER_FW_ENV_SUN_TEMP     4  /* v0 kelvin, 0 = use colour */
#define AVER_FW_ENV_FOG_DENSITY  5  /* v0 per cm */
#define AVER_FW_ENV_FOG_COLOR    6  /* v0..v2 */
#define AVER_FW_ENV_FOG_FALLOFF  7  /* v0 */
#define AVER_FW_ENV_FOG_HEIGHT   8  /* v0 world Z at which density is exactly fogDensity */
#define AVER_FW_ENV_FOG_START    9  /* v0 cm */
#define AVER_FW_ENV_FOG_MAXOPAC 10  /* v0 */
#define AVER_FW_ENV_SKY_LIGHT   11  /* v0 multiplier on the sky-hemisphere irradiance */

/* 1 stored, 0 for an unknown key or a refused value. */
AVER_FW_ABI int32_t aver_fw_env_set(int32_t key, float v0, float v1, float v2);
/* Releases an override so the host restores the level's authored value. key 0 releases all. 0 for an unknown key. */
AVER_FW_ABI int32_t aver_fw_env_clear(int32_t key);
/* 1 and the stored values (outV3 may be NULL) when the key is overridden, else 0 and outV3 untouched. */
AVER_FW_ABI int32_t aver_fw_env_get(int32_t key, float* outV3);

/* ---- post layers ---------------------------------------------------------------------------------------------
 * A layer is a named set of key values blended in as one infinite volume. A layer made by aver_fw_post_set alone
 * engages over AVER_FW_POST_AUTO_BLEND; one made (or later touched) by aver_fw_post_set_weight follows that weight.
 * Layer 0 is refused. */
AVER_FW_ABI int32_t aver_fw_post_set(int64_t layer, int64_t keyHash, float v0, float v1, float v2);
/* weight is clamped to 0..1 and reached linearly over blendSeconds (<= 0: at once), from where the layer is now. */
AVER_FW_ABI int32_t aver_fw_post_set_weight(int64_t layer, float weight, float blendSeconds);
/* Removes the layer at once (fade it with aver_fw_post_set_weight first to soften it): 1 if it existed. 0 removes
 * every layer and returns 1. */
AVER_FW_ABI int32_t aver_fw_post_clear(int64_t layer);
/* Scales an authored POSTVOLUME's weight (clamped to 0..1) from its current scale, 1 at first. weight < 0 removes
 * the scale (1 if there was one). The volume need not exist in the level. */
AVER_FW_ABI int32_t aver_fw_post_volume_weight(int64_t volumeNameHash, float weight, float blendSeconds);

/* ---- host side: once per frame, before reading ---------------------------------------------------------------- */
/* Advances the blends by dt seconds. Also the session boundary: the first tick after aver_fw_play_state() falls back to
 * AVER_FW_PLAY_EDITOR drops every override, layer and scale, so a Play session never leaks into the next one. The
 * editor therefore ticks every frame, Play or not. */
AVER_FW_ABI void    aver_fw_worldfx_tick(float dt);
AVER_FW_ABI int32_t aver_fw_post_layer_count(void);
/* Layers are listed in creation order. Any out pointer may be NULL. 0 for an index out of range. */
AVER_FW_ABI int32_t aver_fw_post_layer_info(int32_t index, int64_t* outLayer, float* outWeight, int32_t* outValueCount);
AVER_FW_ABI int32_t aver_fw_post_layer_value(int32_t index, int32_t valueIndex, int64_t* outKeyHash, float* outV3);
/* 1 and the current scale when the volume has one, else 0 and outWeight untouched. */
AVER_FW_ABI int32_t aver_fw_post_volume_scale(int64_t volumeNameHash, float* outWeight);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_WORLDFX_ABI_H */
