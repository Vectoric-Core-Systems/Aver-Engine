#ifndef AVER_FRAMEWORK_MORPH_ABI_H
#define AVER_FRAMEWORK_MORPH_ABI_H

/* Blend-shape and facial-animation C ABI (Aver.Framework): morph target weights, face poses, the face rig and lip-sync,
 * relayed to the morph system.
 *
 * Same conventions as framework_ik_abi.h (int32_t/int64_t/float only, 1 on success and 0 on refusal).
 * Aver.Framework does not link the animation module, so a composition root installs ONE provider function and every
 * call below forwards to it; with no provider every call returns 0 and nothing is written. The provider is
 * anim::morphProvider (modules/anim.scene/include/aver/anim/MorphSystem.hpp), with the MorphSystem as `user`.
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. Strings never cross: every name is the fnv1a64 hash of the name
 * (Assets.ObjectIdOf in C#). Weights set here are transient (not saved) and are released when Play stops.
 * SET_* and LIPSYNC_* attach CMorphTargets to an entity that has a mesh and none yet, and refuse a dead entity or one with
 * no mesh. Semantics: docs/ANIM_MORPH_FACE.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_MORPH_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_morph_abi_version(void);

/* Provider operations; the argument usage of each is listed with it. */
#define AVER_FW_MORPH_OP_SET_TARGET     1   /* h0 target name hash, in[0] weight, in[1] blend seconds (<0 = 0) */
#define AVER_FW_MORPH_OP_SET_POSE       2   /* h0 face pose name hash, in[0] weight, in[1] blend seconds */
#define AVER_FW_MORPH_OP_CLEAR          3   /* h0 name hash (0 = all), h1 kind mask (1 targets, 2 poses, 3 both), in[0] blend seconds */
#define AVER_FW_MORPH_OP_GET_WEIGHT     4   /* h0 target hash; outF[0] final weight; returns 1 when the entity has a morph frame (the id need not be active) */
#define AVER_FW_MORPH_OP_SET_RIG        5   /* h0 .ocface ObjectId (0 = the built-in defaults) */
#define AVER_FW_MORPH_OP_LIPSYNC_PLAY   6   /* h0 .ocvis ObjectId, in[0] rate (0 = 1), in[1] start seconds, in[2] strength (<0 = 1) */
#define AVER_FW_MORPH_OP_LIPSYNC_STOP   7   /* in[0] blend-out seconds (<0 = 0.1) */
#define AVER_FW_MORPH_OP_LIPSYNC_TIME   8   /* in[0] seconds: slave the clock to an audio voice playhead */
#define AVER_FW_MORPH_OP_LIPSYNC_QUEUE  9   /* h0 symbol hash (a phoneme or viseme name), in[0] start seconds from now, in[1] duration, in[2] intensity */
#define AVER_FW_MORPH_OP_LIPSYNC_STATE 10   /* outF[0] time, outF[1] length (0 = open-ended), outF[2] strength; returns 1 while playing */
#define AVER_FW_MORPH_OP_INFO          11   /* outI[0] declared target count, outI[1] rig pose count, outI[2] rig hash; outF[0] master weight; returns 1 when the entity has CMorphTargets */

typedef int32_t (AVER_FW_CALL* aver_fw_morph_fn)(int32_t op, int32_t entity, int64_t h0, int64_t h1, int64_t h2,
                                                 const float* in, int32_t inCount, float* outF, int64_t* outI, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_morph_provider(aver_fw_morph_fn fn, void* user);

/* A morph target weight (named in the mesh) or a face pose weight (a rig pose, an ARKit shape or a viseme), blended in over
 * blendSeconds. Script overrides win over animation until cleared. */
AVER_FW_ABI int32_t aver_fw_morph_set_target(int32_t entity, int64_t target, float weight, float blendSeconds);
AVER_FW_ABI int32_t aver_fw_morph_set_pose(int32_t entity, int64_t pose, float weight, float blendSeconds);
/* name 0 = every override; kindMask 1 targets, 2 poses, 3 both. */
AVER_FW_ABI int32_t aver_fw_morph_clear(int32_t entity, int64_t name, int32_t kindMask, float blendSeconds);
/* The FINAL weight of a target (0 when inactive). Returns 1 when the entity has a morph frame. */
AVER_FW_ABI int32_t aver_fw_morph_get_weight(int32_t entity, int64_t target, float* outWeight);
AVER_FW_ABI int32_t aver_fw_morph_set_rig(int32_t entity, int64_t rigAsset);

/* Lip-sync: play an authored .ocvis track, queue single visemes (a TTS result), or slave the clock to an audio voice. */
AVER_FW_ABI int32_t aver_fw_morph_lipsync_play(int32_t entity, int64_t track, float rate, float startSeconds, float strength);
AVER_FW_ABI int32_t aver_fw_morph_lipsync_stop(int32_t entity, float blendOutSeconds);
AVER_FW_ABI int32_t aver_fw_morph_lipsync_set_time(int32_t entity, float seconds);
AVER_FW_ABI int32_t aver_fw_morph_lipsync_queue(int32_t entity, int64_t symbol, float startSeconds, float durationSeconds, float intensity);
AVER_FW_ABI int32_t aver_fw_morph_lipsync_state(int32_t entity, float* outTimeLengthStrength /* 3 floats */);

/* outI3: declared target count, rig pose count, rig hash. outF1: master weight. Returns 1 when the entity has CMorphTargets. */
AVER_FW_ABI int32_t aver_fw_morph_info(int32_t entity, int64_t* outI3, float* outF1);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_MORPH_ABI_H */
