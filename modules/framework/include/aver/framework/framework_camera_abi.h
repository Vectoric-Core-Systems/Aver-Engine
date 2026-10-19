#ifndef AVER_FRAMEWORK_CAMERA_ABI_H
#define AVER_FRAMEWORK_CAMERA_ABI_H

/* Gameplay camera C ABI (Aver.Framework): spring-arm rigs, view-target blends, the modifier stack and the pose read,
 * relayed to the camera rig system.
 *
 * Same conventions as framework_abi.h (int32_t/float only, 1 on success and 0 on refusal). Aver.Framework does not
 * link the camera module, so a composition root installs ONE provider function and every call below forwards to it;
 * with no provider every call returns 0 and nothing is written. The provider is camera::cameraRigProvider
 * (modules/camera/include/aver/camera/CameraRigSystem.hpp). Every op except the two reads is DEFERRED to the system's
 * next drive and applied in arrival order, which keeps a replay deterministic. Adding and removing the CCameraRig
 * component and writing its fields uses the generic scene ABI, as motion matching does; aver_fw_camera_attach exists
 * because attaching must also seed the preset and add CCamera. Own version, separate from AVER_FW_ABI_VERSION_MINOR.
 * Semantics: docs/CAMERAS.md.
 *
 * PACKING. The provider's `in` array is floats. An integer argument (preset, entity, curve, kind, priority, tag, flags,
 * id, cut) travels as the BIT PATTERN of an int32 (memcpy), never as a converted value: an entity id has 31 significant
 * bits and a float keeps 24. Float arguments travel as themselves. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_CAMERA_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_camera_abi_version(void);

/* Camera modes (CCameraRig::mode). */
#define AVER_FW_CAM_MODE_CHASE        0
#define AVER_FW_CAM_MODE_ORBIT        1
#define AVER_FW_CAM_MODE_FIRSTPERSON  2
#define AVER_FW_CAM_MODE_SEAT         3
#define AVER_FW_CAM_MODE_TOPDOWN      4
#define AVER_FW_CAM_MODE_FIXED        5

/* Presets (CameraRigPresets.hpp). */
#define AVER_FW_CAM_PRESET_CHASE         0
#define AVER_FW_CAM_PRESET_VEHICLECHASE  1
#define AVER_FW_CAM_PRESET_ORBIT         2
#define AVER_FW_CAM_PRESET_FIRSTPERSON   3
#define AVER_FW_CAM_PRESET_SEAT          4
#define AVER_FW_CAM_PRESET_TOPDOWN       5
#define AVER_FW_CAM_PRESET_FIXED         6

/* Blend curves. 0 is the zero-filled default. */
#define AVER_FW_CAM_CURVE_EASEINOUT  0
#define AVER_FW_CAM_CURVE_LINEAR     1
#define AVER_FW_CAM_CURVE_EASEIN     2
#define AVER_FW_CAM_CURVE_EASEOUT    3

/* Modifier kinds. */
#define AVER_FW_CAM_MOD_FOVKICK       0
#define AVER_FW_CAM_MOD_FOVSCALE      1
#define AVER_FW_CAM_MOD_PUNCH         2
#define AVER_FW_CAM_MOD_ROLLLEAN      3
#define AVER_FW_CAM_MOD_ARMSCALE      4
#define AVER_FW_CAM_MOD_SOCKETOFFSET  5

/* Modifier flags. */
#define AVER_FW_CAM_MODF_EXCLUSIVE      1   /* while at full strength, lower-priority modifiers of the same kind add nothing */
#define AVER_FW_CAM_MODF_MOTION         2   /* gated by Reduce motion even if the kind is not by default */
#define AVER_FW_CAM_MODF_NOMOTIONGATE   4   /* not gated even if the kind is by default */

/* CCameraRig::flags. Negative sense, so zero-filled storage is a working Chase rig. */
#define AVER_FW_CAM_RIG_NOCOLLISION   0x00000001
#define AVER_FW_CAM_RIG_NOINHERITYAW  0x00000002
#define AVER_FW_CAM_RIG_NOINHERITPITCH 0x00000004
#define AVER_FW_CAM_RIG_NOINHERITROLL 0x00000008
#define AVER_FW_CAM_RIG_NOFADE        0x00000010
#define AVER_FW_CAM_RIG_LOOKATTARGET  0x00000020
#define AVER_FW_CAM_RIG_DISABLED      0x00000040
#define AVER_FW_CAM_RIG_APPLYPRESET   0x40000000   /* one-shot: the system expands `preset` over the tuning fields, then clears it */
#define AVER_FW_CAM_RIG_CUT           0x80000000u  /* one-shot: the system snaps this rig's lag state next tick, then clears it */

/* CCameraRig::outState. */
#define AVER_FW_CAM_STATE_ACTIVE     0x1
#define AVER_FW_CAM_STATE_BLENDING   0x2
#define AVER_FW_CAM_STATE_COLLIDING  0x4
#define AVER_FW_CAM_STATE_FADED      0x8
#define AVER_FW_CAM_STATE_LAGGING    0x10

/* Provider operations; the argument usage of each is listed with it. */
#define AVER_FW_CAM_OP_ATTACH             1   /* entity = rig; in: preset(i) target(i) */
#define AVER_FW_CAM_OP_SET_VIEW_TARGET    2   /* entity = rig (0 = the legacy pawn camera); in: blendSeconds curve(i) cut(i) */
#define AVER_FW_CAM_OP_CLEAR_VIEW_TARGET  3   /* in: blendSeconds curve(i); back to automatic selection */
#define AVER_FW_CAM_OP_PUSH_MODIFIER      4   /* in: kind(i) priority(i) inS holdS outS a b c d tag(i) flags(i); out i[0] = id */
#define AVER_FW_CAM_OP_REMOVE_MODIFIER    5   /* in: id(i) outSeconds (<0 = its own) */
#define AVER_FW_CAM_OP_CLEAR_MODIFIERS    6   /* in: tag(i) (0 = all); out i[0] = count */
#define AVER_FW_CAM_OP_SHAKE_AT           7   /* in: x y z radius intensity falloffPower */
#define AVER_FW_CAM_OP_POSE               8   /* outF: x y z yawRad pitchRad rollRad fovYRad nearCm farCm; outI: activeRigEntity stateBits (bit0 blending) */
#define AVER_FW_CAM_OP_RIG_STATE          9   /* entity; outF: armCm blendAlpha; outI: outState */
#define AVER_FW_CAM_OP_CUT               10   /* entity (0 = the active rig): snap the lag state, no blend */

/* `cap` is the capacity of BOTH out arrays: 1 for the id and count ops, 9 for POSE, 2 for RIG_STATE. */
typedef int32_t (AVER_FW_CALL* aver_fw_camera_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                  float* outF, int32_t* outI, int32_t cap, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_camera_provider(aver_fw_camera_fn fn, void* user);

/* Adds a CCameraRig seeded from `preset` (and a CCamera) to `entity`. `targetEntity` is the entity the arm follows;
 * 0 means the rig's parent, else the rig's own entity. 1 on success. */
AVER_FW_ABI int32_t aver_fw_camera_attach(int32_t entity, int32_t preset, int32_t targetEntity);
/* Makes `entity` the view (0 = the legacy pawn camera), blending over blendSeconds with `curve`, or `cut` to snap. */
AVER_FW_ABI int32_t aver_fw_camera_set_view_target(int32_t entity, float blendSeconds, int32_t curve, int32_t cut);
/* Back to automatic selection (the best rig under the possessed pawn, else the legacy camera). */
AVER_FW_ABI int32_t aver_fw_camera_clear_view_target(float blendSeconds, int32_t curve);
/* Pushes a camera modifier; `outId` (may be null) receives its id to pass to remove. holdS < 0 holds until removed. */
AVER_FW_ABI int32_t aver_fw_camera_push_modifier(int32_t kind, int32_t priority, float inS, float holdS, float outS,
                                                 float a, float b, float c, float d, int32_t tag, int32_t flags, int32_t* outId);
/* Starts a modifier's blend out (outSeconds < 0 = its own). */
AVER_FW_ABI int32_t aver_fw_camera_remove_modifier(int32_t id, float outSeconds);
/* Blends out every modifier with this tag (0 = all). Returns the count, 0 with no provider. */
AVER_FW_ABI int32_t aver_fw_camera_clear_modifiers(int32_t tag);
/* Shake that fades with distance from the current camera: intensity * (1 - d/radius)^falloffPower. */
AVER_FW_ABI int32_t aver_fw_camera_shake_at(float x, float y, float z, float radiusCm, float intensity, float falloffPower);
/* outPose9: x y z yawRad pitchRad rollRad fovYRad nearCm farCm. outRigAndState2: active rig entity, state bits (bit 0 blending). */
AVER_FW_ABI int32_t aver_fw_camera_pose(float* outPose9, int32_t* outRigAndState2);
/* outArmAndBlend2: arm length after collision, blend alpha. outState: AVER_FW_CAM_STATE_* bits. 0 when the entity has no rig. */
AVER_FW_ABI int32_t aver_fw_camera_rig_state(int32_t entity, float* outArmAndBlend2, int32_t* outState);
/* Snaps the lag state of `entity` (0 = the active rig). */
AVER_FW_ABI int32_t aver_fw_camera_cut(int32_t entity);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_CAMERA_ABI_H */
