#pragma once
// Aver.Physics -- JOLT'S DEBUG RENDERER AS A LINE FEED (wave 11, WORLD_STATE_DEBUG phase C).
// Additive: nothing in physics_abi.h changed. The module draws nothing itself. A host asks for a set of overlays, gets back a
// flat list of coloured lines (plus a few text labels) in the ENGINE's frame, and uploads them through whatever line path it
// has -- the editor's is rhi::LineVertex pairs, which these lines are byte for byte, so D3D12 and Vulkan agree by
// construction. The packaged game has no line-overlay path and none is added; the C# PhysicsDebugDraw exists for tools and tests.
//
// UNITS AND AXES ARE THE ENGINE'S: centimetres, +X forward, +Y right, +Z up. Colours are 0..1. Main thread, between steps,
// like the rest of the ABI. A build without JPH_DEBUG_RENDERER (cmake -DAVER_PHYSICS_DEBUG_RENDERER=OFF) answers
// aver_phys_debug_available() == 0 and every other call here does nothing and returns 0.
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
#define AVER_PHYS_DBG_BODIES           0x00000001u   /* wireframe shapes of bodies within the radius (meshes and heightfields excluded: see STATIC_TRIANGLES) */
#define AVER_PHYS_DBG_BOUNDS           0x00000002u   /* world-space bounding boxes (heavy shapes included) */
#define AVER_PHYS_DBG_COM              0x00000004u   /* centre-of-mass axes */
#define AVER_PHYS_DBG_VELOCITY         0x00000008u   /* linear (green) and angular (red) velocity arrows, in metres per second as drawn by Jolt */
#define AVER_PHYS_DBG_SLEEP            0x00000010u   /* sleep-test spheres and timers; with the default scheme sleeping bodies turn red */
#define AVER_PHYS_DBG_MASS             0x00000020u   /* mass and the box of equivalent inertia */
#define AVER_PHYS_DBG_CONSTRAINTS      0x00000040u   /* joint frames, ragdoll joints, vehicle wheels and suspension */
#define AVER_PHYS_DBG_LIMITS           0x00000080u   /* joint limit arcs */
#define AVER_PHYS_DBG_CONTACTS         0x00000100u   /* contact points and normals of the LAST fixed step (arms per-step capture) */
#define AVER_PHYS_DBG_SUPPORT_FACES    0x00000200u   /* supporting faces found by the narrow phase (arms per-step capture) */
#define AVER_PHYS_DBG_SOFT_VERTICES    0x00000400u
#define AVER_PHYS_DBG_SOFT_EDGES       0x00000800u   /* edge, bend, volume, skin and LRA constraints */
#define AVER_PHYS_DBG_CHARACTERS       0x00001000u   /* character capsules and ground normal (a CharacterVirtual is not a body) */
#define AVER_PHYS_DBG_STATIC_TRIANGLES 0x00002000u   /* static mesh / heightfield triangles inside the radius, through Shape::GetTrianglesStart */
#define AVER_PHYS_DBG_LINEAR_CAST      0x00004000u   /* where continuous collision ran (arms per-step capture) */
#define AVER_PHYS_DBG_LABELS           0x00008000u   /* handle, mass and speed text per moving body */
#define AVER_PHYS_DBG_ISLANDS          0x00010000u   /* draw bodies coloured by island (implies BODIES and the island scheme) */

/* how bodies are coloured: aver_phys_debug_set_color_scheme */
#define AVER_PHYS_DBG_SCHEME_MOTION    0   /* static grey, kinematic green, dynamic a colour per body (default) */
#define AVER_PHYS_DBG_SCHEME_SLEEP     1   /* active yellow, sleeping red */
#define AVER_PHYS_DBG_SCHEME_LAYER     2   /* a colour per collision layer */
#define AVER_PHYS_DBG_SCHEME_SHAPE     3   /* by shape kind: convex green, scaled yellow, compound orange, mesh red */
#define AVER_PHYS_DBG_SCHEME_ISLAND    4   /* a colour per simulation island */
#define AVER_PHYS_DBG_SCHEME_INSTANCE  5   /* a colour per body */
#define AVER_PHYS_DBG_SCHEME_COUNT     6

AVER_PHYS_API int32_t aver_phys_debug_set_color_scheme(int32_t scheme);   /* 1 on success, 0 for an unknown scheme */

/* Clears the buffers, runs Jolt's draw calls into them and returns the line count (clamped to maxLines). 0 with no world, flags
   == 0 or no renderer. `cameraXyz` (3 floats, engine cm) centres the radius and picks Jolt's level of detail; NULL means no
   culling. radiusCm <= 0 means unlimited. */
AVER_PHYS_API int32_t aver_phys_debug_collect(uint32_t flags, const float* cameraXyz, float radiusCm, int32_t maxLines);
AVER_PHYS_API int32_t aver_phys_debug_truncated(void);   /* 1 if the last collect hit maxLines */
/* Copies up to maxLines lines of the last collect, 12 floats each: (x y z r g b) of the start then of the end -- exactly two
   rhi::LineVertex. Returns the lines copied. */
AVER_PHYS_API int32_t aver_phys_debug_lines(float* outLines, int32_t maxLines);
AVER_PHYS_API int32_t aver_phys_debug_label_count(void);
/* One label of the last collect: position (engine cm), colour 0..1, UTF-8 text (truncated to textCap - 1, always terminated).
   0 for a bad index. Any out may be NULL. */
AVER_PHYS_API int32_t aver_phys_debug_label_get(int32_t index, float* outXyz, float* outRgb, uint8_t* outTextUtf8, int32_t textCap);
/* Contacts, supporting faces and continuous-collision markers are drawn by Jolt INSIDE the step, so they need asking for
   beforehand: this arms the capture for the CONTACTS, SUPPORT_FACES and LINEAR_CAST bits of `flags` (others ignored; 0 disarms).
   While armed, each fixed step clears and refills the capture, so after a frame's steps it holds the LAST step's lines, not a
   smear of all of them. Disarmed, none of it runs. A host calls it every frame the overlay is on. Returns the armed mask. */
AVER_PHYS_API int32_t aver_phys_debug_arm(uint32_t flags);

#ifdef __cplusplus
}
#endif
