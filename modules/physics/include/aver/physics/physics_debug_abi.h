#pragma once
// Aver.Physics: Jolt's debug renderer as a line feed. The host requests overlays and uploads the returned lines itself.
// Engine units and axes (cm, +X forward, +Y right, +Z up). Main thread, between steps. Without JPH_DEBUG_RENDERER every call
// except the availability check returns 0.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_PHYS_DEBUG_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_debug_abi_version(void);
AVER_PHYS_API int32_t aver_phys_debug_available(void);   /* 1 when built with JPH_DEBUG_RENDERER */

/* what to draw: OR these into the flags of aver_phys_debug_collect */
#define AVER_PHYS_DBG_BODIES           0x00000001u   /* meshes and heightfields: see STATIC_TRIANGLES */
#define AVER_PHYS_DBG_BOUNDS           0x00000002u
#define AVER_PHYS_DBG_COM              0x00000004u
#define AVER_PHYS_DBG_VELOCITY         0x00000008u   /* linear green, angular red */
#define AVER_PHYS_DBG_SLEEP            0x00000010u
#define AVER_PHYS_DBG_MASS             0x00000020u
#define AVER_PHYS_DBG_CONSTRAINTS      0x00000040u
#define AVER_PHYS_DBG_LIMITS           0x00000080u
#define AVER_PHYS_DBG_CONTACTS         0x00000100u   /* last fixed step; needs aver_phys_debug_arm */
#define AVER_PHYS_DBG_SUPPORT_FACES    0x00000200u   /* last fixed step; needs aver_phys_debug_arm */
#define AVER_PHYS_DBG_SOFT_VERTICES    0x00000400u
#define AVER_PHYS_DBG_SOFT_EDGES       0x00000800u   /* edge, bend, volume, skin and LRA constraints */
#define AVER_PHYS_DBG_CHARACTERS       0x00001000u   /* a CharacterVirtual is not a body */
#define AVER_PHYS_DBG_STATIC_TRIANGLES 0x00002000u
#define AVER_PHYS_DBG_LINEAR_CAST      0x00004000u   /* last fixed step; needs aver_phys_debug_arm */
#define AVER_PHYS_DBG_LABELS           0x00008000u
#define AVER_PHYS_DBG_ISLANDS          0x00010000u   /* implies BODIES and the island scheme */

/* how bodies are coloured: aver_phys_debug_set_color_scheme. MOTION (default), SLEEP, LAYER, SHAPE, ISLAND, INSTANCE, COUNT = 6 */
#define AVER_PHYS_DBG_SCHEME_MOTION    0
#define AVER_PHYS_DBG_SCHEME_SLEEP     1
#define AVER_PHYS_DBG_SCHEME_LAYER     2
#define AVER_PHYS_DBG_SCHEME_SHAPE     3
#define AVER_PHYS_DBG_SCHEME_ISLAND    4
#define AVER_PHYS_DBG_SCHEME_INSTANCE  5
#define AVER_PHYS_DBG_SCHEME_COUNT     6

AVER_PHYS_API int32_t aver_phys_debug_set_color_scheme(int32_t scheme);   /* 1 on success, 0 for an unknown scheme */

/* Clears the buffers, runs Jolt's draw calls and returns the line count (clamped to maxLines). 0 with no world, flags == 0 or
   no renderer. cameraXyz (engine cm) centres the radius and picks the level of detail; NULL = no culling. radiusCm <= 0 = unlimited. */
AVER_PHYS_API int32_t aver_phys_debug_collect(uint32_t flags, const float* cameraXyz, float radiusCm, int32_t maxLines);
AVER_PHYS_API int32_t aver_phys_debug_truncated(void);   /* 1 if the last collect hit maxLines */
/* Copies up to maxLines lines of the last collect, 12 floats each: (x y z r g b) of the start, then of the end. That is
   exactly two rhi::LineVertex. Returns the lines copied. */
AVER_PHYS_API int32_t aver_phys_debug_lines(float* outLines, int32_t maxLines);
AVER_PHYS_API int32_t aver_phys_debug_label_count(void);
/* One label of the last collect: position (engine cm), colour 0..1, UTF-8 text (truncated to textCap - 1, always terminated).
   0 for a bad index. Any out may be NULL. */
AVER_PHYS_API int32_t aver_phys_debug_label_get(int32_t index, float* outXyz, float* outRgb, uint8_t* outTextUtf8, int32_t textCap);
/* Arms the per-step capture for the CONTACTS, SUPPORT_FACES and LINEAR_CAST bits (others ignored; 0 disarms). Call every frame
   the overlay is on. Returns the armed mask. */
AVER_PHYS_API int32_t aver_phys_debug_arm(uint32_t flags);

#ifdef __cplusplus
}
#endif
