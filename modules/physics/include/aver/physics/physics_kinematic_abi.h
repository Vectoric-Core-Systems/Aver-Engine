#pragma once
// Aver.Physics: a kinematic-body character controller with move_and_slide semantics.
// Docs: docs/physics/KINEMATIC.md.
//
// OPT-IN, ADDITIVE. The CharacterVirtual-based character API (physics_character_abi.h) is untouched and keeps working. This is a SECOND
// controller, for a kinematic RIGID body of any convex shape (a capsule, a box, a compound): the script sets the velocity (gravity too),
// aver_phys_kc_move_and_slide moves the body by velocity * dt in up to max_slides slide iterations of aver_phys_body_test_motion, classifies
// every contact as floor / wall / ceiling against up_direction, and writes the slid velocity back.
// No imported defaults are built into any call except the creation defaults listed below; an importer sets every option
// explicitly (the C# KinematicCharacter does, from KinematicCharacterOptions.Default).
//
// Units and axes are the engine's: centimetres, cm/s, radians, +X forward, +Y right, +Z up, left-handed. `up` is an engine-axes direction.
// Returns 1/0 with aver_phys_last_error unless stated. Everything is a between-step call, on the thread that steps the world.
// The controller's settings and results are NOT simulation state: physics snapshots do not capture them (a script that rolls back
// re-issues velocity and calls move_and_slide again; the body's own pose is in the snapshot).
//
// The body must be KINEMATIC (aver_phys_body_motion_type == AVER_PHYS_MOTION_KINEMATIC) and not a mesh, heightfield or soft body. The
// controller teleports it (aver_phys_body_set_position) and never gives it a velocity. What it collides with is the body's own filter: its
// 16-layer object layer, its 32-bit layer/mask, collision exceptions and joint exclusion, exactly as aver_phys_body_test_motion has
// it; sensors never block. Destroying the body kills the controller (every call then fails with a bad handle).
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

#define AVER_PHYS_KINEMATIC_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_kinematic_abi_version(void);

/* motion mode (AVER_PHYS_KCP_MOTION_MODE) */
#define AVER_PHYS_KCM_GROUNDED 0   /* contacts are floor / wall / ceiling against `up`; floor snap, platforms, stop-on-slope apply */
#define AVER_PHYS_KCM_FLOATING 1   /* every contact is a wall; no floor logic (a space or top-down body) */

/* what happens to the velocity when the body leaves a moving platform (AVER_PHYS_KCP_PLATFORM_ON_LEAVE) */
#define AVER_PHYS_KCL_ADD_VELOCITY         0   /* the platform's last velocity is added to the body's */
#define AVER_PHYS_KCL_ADD_UPWARD_VELOCITY  1   /* the same, but a downward platform velocity is dropped (only sideways and up) */
#define AVER_PHYS_KCL_DO_NOTHING           2

/* float parameters. Booleans are 0 / 1, angles radians, lengths centimetres.
   CREATION DEFAULTS (this new object's own, not any existing call's): MOTION_MODE grounded, MAX_SLIDES 6, FLOOR_MAX_ANGLE 0.785398 (45 deg),
   WALL_MIN_SLIDE_ANGLE 0.261799 (15 deg), FLOOR_SNAP_LENGTH 10, SAFE_MARGIN 0.1, FLOOR_STOP_ON_SLOPE 1, FLOOR_CONSTANT_SPEED 0, FLOOR_BLOCK_ON_WALL 1,
   SLIDE_ON_CEILING 1, PLATFORM_ON_LEAVE add velocity; platform floor layers all bits, platform wall layers none; up (0,0,1). */
#define AVER_PHYS_KCP_MOTION_MODE          0
#define AVER_PHYS_KCP_MAX_SLIDES           1    /* 1..64 */
#define AVER_PHYS_KCP_FLOOR_MAX_ANGLE      2    /* 0..pi: a surface within this angle of `up` is floor */
#define AVER_PHYS_KCP_WALL_MIN_SLIDE_ANGLE 3    /* floating mode: a head-on hit within this angle of the velocity stops instead of sliding */
#define AVER_PHYS_KCP_FLOOR_SNAP_LENGTH    4    /* >= 0: how far down the body is pulled back onto the floor after it was on it */
#define AVER_PHYS_KCP_SAFE_MARGIN          5    /* >= 0: the gap kept to surfaces (and the recovery distance) */
#define AVER_PHYS_KCP_FLOOR_STOP_ON_SLOPE  6    /* standing on a slope with only gravity pulling: no sliding down */
#define AVER_PHYS_KCP_FLOOR_CONSTANT_SPEED 7    /* the same speed up and down slopes as on the flat */
#define AVER_PHYS_KCP_FLOOR_BLOCK_ON_WALL  8    /* a body on the floor does not climb a wall (steep surface) */
#define AVER_PHYS_KCP_SLIDE_ON_CEILING     9    /* 0: a ceiling hit kills the upward velocity instead of sliding along the ceiling */
#define AVER_PHYS_KCP_PLATFORM_ON_LEAVE    10   /* AVER_PHYS_KCL_* */
#define AVER_PHYS_KCP_COUNT                11

/* result state bits (aver_phys_kc_state, AVER_PHYS_KCI_I_STATE) */
#define AVER_PHYS_KCS_FLOOR   1
#define AVER_PHYS_KCS_WALL    2
#define AVER_PHYS_KCS_CEILING 4

/* Registers a controller on a kinematic body. Returns its handle (not the body's), or 0: dead body, not kinematic, soft/mesh/heightfield,
   or the body already has a controller. */
AVER_PHYS_API int32_t aver_phys_kc_create(int32_t body);
AVER_PHYS_API int32_t aver_phys_kc_destroy(int32_t kc);
AVER_PHYS_API int32_t aver_phys_kc_exists(int32_t kc);
AVER_PHYS_API int32_t aver_phys_kc_count(void);
/* The body handle the controller drives, or 0. */
AVER_PHYS_API int32_t aver_phys_kc_body(int32_t kc);

AVER_PHYS_API int32_t aver_phys_kc_set_param(int32_t kc, int32_t param, float value);
AVER_PHYS_API int32_t aver_phys_kc_param(int32_t kc, int32_t param, float* outValue);
/* The up direction (world, any length, normalised here; zero refused). */
AVER_PHYS_API int32_t aver_phys_kc_set_up(int32_t kc, const float* up3);
AVER_PHYS_API int32_t aver_phys_kc_up(int32_t kc, float* outUp3);
/* Platform floor / wall layers: a platform whose layer bits meet the mask carries the body while the body
   stands on it (floor) or leans on it (wall). A body without a layer filter has the single bit of its 16-layer user layer. */
AVER_PHYS_API int32_t aver_phys_kc_set_platform_layers(int32_t kc, uint32_t floorLayers, uint32_t wallLayers);
AVER_PHYS_API int32_t aver_phys_kc_platform_layers(int32_t kc, uint32_t* outFloorLayers, uint32_t* outWallLayers);

/* The velocity the script drives (cm/s, world). move_and_slide reads it and writes it back slid along what it hit. */
AVER_PHYS_API int32_t aver_phys_kc_set_velocity(int32_t kc, const float* velocity3);
AVER_PHYS_API int32_t aver_phys_kc_velocity(int32_t kc, float* outVelocity3);

/* Move-and-slide for a step of `dt` seconds. The body is moved (teleported) and the results below are rebuilt. outCollided (optional)
   is 1 when at least one collision was recorded. Refused for dt <= 0 or non-finite. */
AVER_PHYS_API int32_t aver_phys_kc_move_and_slide(int32_t kc, float dt, int32_t* outCollided);
/* Forgets the results and the platform (after a teleport, a respawn, a level change). The velocity is kept. */
AVER_PHYS_API int32_t aver_phys_kc_clear(int32_t kc);

/* ---- results of the last move_and_slide ---- */
AVER_PHYS_API int32_t aver_phys_kc_state(int32_t kc, int32_t* outFlags);   /* AVER_PHYS_KCS_* bits: is_on_floor, is_on_wall, is_on_ceiling */

/* The info block. Writes the first min(nF, COUNT) floats and min(nI, COUNT) ints (either array may be null). Vectors are the index of their X. */
#define AVER_PHYS_KCI_F_FLOOR_NORMAL      0    /* 3: get_floor_normal; zero when not on the floor */
#define AVER_PHYS_KCI_F_WALL_NORMAL       3    /* 3: get_wall_normal */
#define AVER_PHYS_KCI_F_PLATFORM_VELOCITY 6    /* 3: get_platform_velocity (at the contact that carried the body) */
#define AVER_PHYS_KCI_F_REAL_VELOCITY     9    /* 3: get_real_velocity = position change / dt */
#define AVER_PHYS_KCI_F_LAST_MOTION       12   /* 3: get_last_motion, the last iteration's travel */
#define AVER_PHYS_KCI_F_POSITION_DELTA    15   /* 3: get_position_delta, this call's whole displacement */
#define AVER_PHYS_KCI_F_COUNT             18
#define AVER_PHYS_KCI_I_STATE             0    /* AVER_PHYS_KCS_* */
#define AVER_PHYS_KCI_I_PLATFORM_BODY     1    /* the body carrying this one (get_platform_rid), or 0 */
#define AVER_PHYS_KCI_I_COLLISIONS        2    /* get_slide_collision_count */
#define AVER_PHYS_KCI_I_COUNT             3
AVER_PHYS_API int32_t aver_phys_kc_info(int32_t kc, float* outF, int32_t nF, int32_t* outI, int32_t nI);

/* One slide collision (get_slide_collision(i), in the order they happened; the platform-carry move counts as the first when it hit
   something). Writes the first min(nF, COUNT) floats and min(nI, COUNT) ints. ANGLE is the angle between the normal and `up` in radians. */
#define AVER_PHYS_KCC_F_POINT             0    /* 3: on the collider */
#define AVER_PHYS_KCC_F_NORMAL            3    /* 3: away from the collider */
#define AVER_PHYS_KCC_F_COLLIDER_VELOCITY 6    /* 3: the collider's velocity at the point */
#define AVER_PHYS_KCC_F_REMAINDER         9    /* 3: the motion left undone by that iteration */
#define AVER_PHYS_KCC_F_TRAVEL            12   /* 3: what that iteration moved the body by */
#define AVER_PHYS_KCC_F_DEPTH             15
#define AVER_PHYS_KCC_F_ANGLE             16
#define AVER_PHYS_KCC_F_COUNT             17
#define AVER_PHYS_KCC_I_COLLIDER          0
#define AVER_PHYS_KCC_I_ENTITY            1
#define AVER_PHYS_KCC_I_SUBSHAPE          2
#define AVER_PHYS_KCC_I_COUNT             3
AVER_PHYS_API int32_t aver_phys_kc_collision(int32_t kc, int32_t index, float* outF, int32_t nF, int32_t* outI, int32_t nI);

#ifdef __cplusplus
}
#endif
