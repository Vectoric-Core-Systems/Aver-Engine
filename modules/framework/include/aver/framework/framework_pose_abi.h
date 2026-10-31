#ifndef AVER_FRAMEWORK_POSE_ABI_H
#define AVER_FRAMEWORK_POSE_ABI_H

/* Pose seam C ABI (Aver.Framework): read and write the bones of an animated entity from a script, relayed to the
 * pose seam of the animation module.
 *
 * Same conventions as framework_ik_abi.h (int32_t/int64_t/float only; 1 on success and 0 on refusal, except the two
 * calls documented to return a count). Aver.Framework does not link the animation module, so a composition root
 * installs ONE provider function and every call below forwards to it; with no provider every call returns 0 and
 * nothing is written. The provider is anim::poseProvider (modules/anim.scene/include/aver/anim/PoseSeam.hpp), with the
 * pose seam system as `user`. Own version, separate from AVER_FW_ABI_VERSION_MINOR.
 *
 * Units are the engine's: centimetres, the skeleton's own axes, quaternions xyzw. A transform crosses as 10 floats:
 * position xyz, rotation xyzw, scale xyz. Bones are the skeleton's own indices (parents before children). Names cross
 * as UTF-8 into a caller buffer. Writes take effect at the next animation tick; reads see the pose of the last tick.
 * Semantics and the order against the control rig, IK rig and ragdoll: docs/ANIM_POSE_SEAM.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_POSE_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_pose_abi_version(void);

/* Channels of a bone write (the flags argument); they match kPoseSeam* in PoseSeam.hpp. */
#define AVER_FW_POSE_POSITION 0x1
#define AVER_FW_POSE_ROTATION 0x2
#define AVER_FW_POSE_SCALE    0x4
#define AVER_FW_POSE_ALL      0x7
#define AVER_FW_POSE_MODEL    0x8    /* the value is in the skeleton's model space; otherwise parent-local */
#define AVER_FW_POSE_ONCE     0x10   /* dropped after the tick that applied it */

/* Read selector bits (aver_fw_pose_read). */
#define AVER_FW_POSE_READ_MODEL       0x1   /* model space; otherwise parent-local */
#define AVER_FW_POSE_READ_BEFORE_SEAM 0x2   /* the pose just before the seam's overrides were applied */

/* Provider operations; the argument usage of each is listed with it. The return value is 0 for a refusal. */
#define AVER_FW_POSE_OP_BONE_COUNT 1    /* returns the bone count (0 = the entity has no posed skeleton) */
#define AVER_FW_POSE_OP_FIND       2    /* h0 name hash (fnv1a64); outI[0] bone index */
#define AVER_FW_POSE_OP_INFO       3    /* h0 bone; outI[0] parent (-1 root); outF[0..9] rest transform, parent-local; outStr the name */
#define AVER_FW_POSE_OP_READ       4    /* h0 bone, h1 AVER_FW_POSE_READ_* bits; outF[0..9] */
#define AVER_FW_POSE_OP_WRITE      5    /* h0 bone, h1 AVER_FW_POSE_* flags, in[0..9] transform, in[10] amount 0..1 when inCount > 10 */
#define AVER_FW_POSE_OP_CLEAR      6    /* h0 bone (-1 = every bone) */
#define AVER_FW_POSE_OP_WEIGHT     7    /* in[0] master weight 0..1 */
#define AVER_FW_POSE_OP_ENABLE     8    /* h0 0 or 1 */
#define AVER_FW_POSE_OP_OVERRIDES  9    /* returns how many bones carry an override */
#define AVER_FW_POSE_OP_WRITE_RANGE 10  /* h0 flags, h1 first bone, in: inCount / 10 transforms for consecutive bones */

typedef int32_t (AVER_FW_CALL* aver_fw_pose_fn)(int32_t op, int32_t entity, int64_t h0, int64_t h1, int64_t h2,
                                                const float* in, int32_t inCount, float* outF, int64_t* outI,
                                                char* outStr, int32_t outStrLen, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_pose_provider(aver_fw_pose_fn fn, void* user);

/* The bone count of the entity's posed skeleton; 0 for none (no provider, no skeleton, not ticked yet). */
AVER_FW_ABI int32_t aver_fw_pose_bone_count(int32_t entity);
/* The bone whose name hashes to `nameHash`, or -1. */
AVER_FW_ABI int32_t aver_fw_pose_find_bone(int32_t entity, int64_t nameHash);
/* A bone's name (into nameBuf, UTF-8, NUL-terminated even when cut), parent and rest transform (10 floats). 1 on success. */
AVER_FW_ABI int32_t aver_fw_pose_bone_info(int32_t entity, int32_t bone, char* nameBuf, int32_t nameBufLen,
                                           int32_t* outParent, float* outRest10);
/* The bone's pose as 10 floats (see AVER_FW_POSE_READ_*). 1 on success. */
AVER_FW_ABI int32_t aver_fw_pose_read(int32_t entity, int32_t bone, int32_t readFlags, float* out10);
/* Sets one bone's override: 10 floats and a blend amount. 1 on success, 0 for an unposed entity or a bad bone. */
AVER_FW_ABI int32_t aver_fw_pose_write(int32_t entity, int32_t bone, int32_t flags, const float* in10, float amount);
/* Sets `count` consecutive bones from `first` in one call (10 floats each, amount 1). */
AVER_FW_ABI int32_t aver_fw_pose_write_range(int32_t entity, int32_t first, int32_t count, int32_t flags, const float* in);
/* Drops one bone's override, or all of them for bone -1. */
AVER_FW_ABI int32_t aver_fw_pose_clear(int32_t entity, int32_t bone);
/* The master weight of the entity's overrides (default 1) and the off switch (default on). */
AVER_FW_ABI int32_t aver_fw_pose_set_weight(int32_t entity, float weight);
AVER_FW_ABI int32_t aver_fw_pose_set_enabled(int32_t entity, int32_t enabled);
/* How many bones carry an override. */
AVER_FW_ABI int32_t aver_fw_pose_override_count(int32_t entity);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_POSE_ABI_H */
