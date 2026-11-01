#ifndef AVER_FRAMEWORK_RENDER_ABI_H
#define AVER_FRAMEWORK_RENDER_ABI_H

/* Render settings and statistics C ABI (Aver.Framework, P17 and P18): a script asks for a different render scale, MSAA, shadow cascades or light-shaft fog while
 * the game runs, and reads what the last frame drew. Same shape as framework_worldfx_abi.h: the script PUBLISHES, the host READS once per frame, at the frame
 * boundary (before the scene is built), and a key nobody has set leaves the project's own value alone. Clearing a key puts the project's value back.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/float only; 1 on success and 0 on refusal. All state lives in this DLL, single game thread.
 *
 * Post-processing (glow, debanding, exposure, tonemap) is not here: those are POSTVOLUME keys, set with aver_fw_post_set (framework_worldfx_abi.h).
 *
 * What a host does with each key, by host:
 *   shipped game  - SCALE, MSAA, SHADOW_*, VOLFOG_* apply. SHARPNESS and TAA are stored: the upscaler is chosen when the project starts (RENDER.AVERSR /
 *                   RENDER.TAA). LOD_THRESHOLD and OCCLUSION are stored: the shipped runtime has no LOD selection or occlusion culling.
 *   editor (Play) - MSAA, SHADOW_*, VOLFOG_*, LOD_THRESHOLD and OCCLUSION apply; SCALE (the viewport's own), SHARPNESS and TAA are stored.
 * A value outside a key's range is clamped, a non-finite one is refused. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_RENDER_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_render_abi_version(void);

/* ---- settings ------------------------------------------------------------------------------------------------ */
#define AVER_FW_RENDER_SCALE             1  /* 3D render scale, 0.25 .. 2 (the scene renders at present size times this) */
#define AVER_FW_RENDER_SHARPNESS         2  /* upscaler sharpness, 0 .. 1 (stored) */
#define AVER_FW_RENDER_TAA               3  /* temporal anti-aliasing, 0 or 1 (stored) */
#define AVER_FW_RENDER_MSAA              4  /* samples: 1, 2, 4 or 8 (snapped down to a power of two; the device clamps to what it supports) */
#define AVER_FW_RENDER_LOD_THRESHOLD     5  /* mesh LOD pixel-error threshold, 0 .. 64 (editor Play) */
#define AVER_FW_RENDER_OCCLUSION         6  /* occlusion culling, 0 or 1 (editor Play) */
#define AVER_FW_RENDER_SHADOW_CASCADES   7  /* sun shadow cascades, 1 .. 4 */
#define AVER_FW_RENDER_SHADOW_SPLIT      8  /* cascade split scheme, 0 (even) .. 1 (logarithmic) */
#define AVER_FW_RENDER_SHADOW_DISTANCE   9  /* farthest shadowed distance in cm, 0 = automatic */
#define AVER_FW_RENDER_SHADOW_FILTER    10  /* sun shadow filter width in shadow texels, 0 (hard) .. 4 */
#define AVER_FW_RENDER_VOLFOG_ANISO     11  /* light-shaft fog scattering anisotropy g, -0.95 .. 0.95 */
#define AVER_FW_RENDER_VOLFOG_LENGTH    12  /* light-shaft fog reach in cm, 100 .. 500000 */
#define AVER_FW_RENDER_VOLFOG_TEMPORAL  13  /* light-shaft fog history weight, 0 .. 0.98 */
#define AVER_FW_RENDER_VOLFOG_INTENSITY 14  /* multiplier on the sun's scattered light in the fog, 0 .. 16 */
#define AVER_FW_RENDER_KEY_COUNT        15  /* keys are 1 .. COUNT-1 */

/* 1 stored (clamped to the key's range), 0 for an unknown key or a non-finite value. */
AVER_FW_ABI int32_t aver_fw_render_set(int32_t key, float value);
/* Releases an override so the host restores the project's value. key 0 releases all. 0 for an unknown key. */
AVER_FW_ABI int32_t aver_fw_render_clear(int32_t key);
/* 1 and the stored value (out may be NULL) when the key is overridden, else 0 and out untouched. */
AVER_FW_ABI int32_t aver_fw_render_get(int32_t key, float* out);
/* Counts every change, so a host re-applies only when it moved. Starts at 0. Also the session boundary, as aver_fw_worldfx_tick is: the first call after
 * aver_fw_play_state() falls back to AVER_FW_PLAY_EDITOR drops every override and the statistics request (aver_fw_render_reset), so a Play session never leaks. */
AVER_FW_ABI int32_t aver_fw_render_revision(void);

/* ---- statistics ----------------------------------------------------------------------------------------------- */
/* What the last frame submitted, in the order of the AVER_FW_RENDER_STAT_* indices. Objects and triangles are counted from the scene's draw list, so they are
 * right when ray tracing or the ray-driven renderer paints the picture too; DRAW_CALLS is the number of raster scene draws (0 while a ray-driven frame
 * draws the scene itself). */
#define AVER_FW_RENDER_STAT_OBJECTS      0
#define AVER_FW_RENDER_STAT_TRIANGLES    1
#define AVER_FW_RENDER_STAT_DRAW_CALLS   2
#define AVER_FW_RENDER_STAT_TRANSLUCENT  3
#define AVER_FW_RENDER_STAT_SHADOW_DRAWS 4
#define AVER_FW_RENDER_STAT_FRAME        5   /* the host's frame number when these were published */
#define AVER_FW_RENDER_STAT_COUNT        6

/* Copies the last published statistics into out6 (six int32). Asking turns collection on: the host publishes nothing until a script has asked. Returns 1
 * when a frame has been published since the ask, else 0 (out6 is zeroed). */
AVER_FW_ABI int32_t aver_fw_render_stats(int32_t* out6);

/* ---- host side: once per frame -------------------------------------------------------------------------------- */
/* 1 once a script has asked for statistics (and the session has not ended), so the host can skip the counting otherwise. */
AVER_FW_ABI int32_t aver_fw_render_stats_wanted(void);
/* Publishes the frame's counters (six int32 in AVER_FW_RENDER_STAT_* order). */
AVER_FW_ABI void    aver_fw_render_publish_stats(const int32_t* in6);
/* Drops every override and the statistics request: the editor calls it when a Play session ends. */
AVER_FW_ABI void    aver_fw_render_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_RENDER_ABI_H */
