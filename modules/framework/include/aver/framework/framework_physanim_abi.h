#ifndef AVER_FRAMEWORK_PHYSANIM_ABI_H
#define AVER_FRAMEWORK_PHYSANIM_ABI_H

/* Physical animation C ABI (Aver.Framework): hit reactions, partial-body simulation, per-bone drive, death and
 * get-up on a skinned entity, relayed to the ragdoll module.
 *
 * Same conventions as framework_ik_abi.h (int32_t/int64_t/float only; 1 on success and 0 on refusal). Aver.Framework
 * does not link the ragdoll module, so a composition root installs ONE provider function and every call below forwards
 * to it; with no provider every call returns 0 and nothing happens. The provider is ragdoll::physAnimProvider
 * (modules/ragdoll/include/aver/ragdoll/RagdollSystem.hpp) with the ragdoll system as `user`. Own version, separate from
 * AVER_FW_ABI_VERSION_MINOR. Bones are named by fnv1a64 of the bone name (0 = the root part), a profile by the
 * ObjectId of its .ocphysanim (Assets.ObjectIdOf in C#). Semantics: docs/physics/PHYSICAL_ANIMATION.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_PHYSANIM_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_physanim_abi_version(void);

/* Provider operations; the argument usage of each is listed with it. The return value is 0 for a refusal. */
#define AVER_FW_PHYSANIM_OP_HIT       1   /* h0 bone hash; in[0..2] impulse kg*cm/s, in[3..5] world point, in[6] damage, in[7] radius (0 = a point hit) */
#define AVER_FW_PHYSANIM_OP_PROFILE   2   /* h0 profile ObjectId (0 clears back to the defaults) */
#define AVER_FW_PHYSANIM_OP_BLEND     3   /* in[0] weight 0..1, in[1] seconds */
#define AVER_FW_PHYSANIM_OP_SIMULATE  4   /* h0 bone hash: simulate that bone and everything below it, pin the rest; 0 = the whole body */
#define AVER_FW_PHYSANIM_OP_PART      5   /* h0 bone hash, h1 AVER_FW_PHYSANIM_PART_* kind, in[0] value */
#define AVER_FW_PHYSANIM_OP_DIE       6
#define AVER_FW_PHYSANIM_OP_GET_UP    7
#define AVER_FW_PHYSANIM_OP_STATE     8   /* outI[0] life, outI[1] facing, outI[2] bones still reacting; returns 1 when the entity has a ragdoll */

/* AVER_FW_PHYSANIM_OP_PART kinds. */
#define AVER_FW_PHYSANIM_PART_STRENGTH 0  /* powered motor strength 0..1, negative = inherit */
#define AVER_FW_PHYSANIM_PART_DAMPING  1  /* motor damping ratio, negative = default */
#define AVER_FW_PHYSANIM_PART_TORQUE   2  /* multiplier on the torque ceiling */
#define AVER_FW_PHYSANIM_PART_BLEND    3  /* weight of the simulated pose for the bone, 0..1 */
#define AVER_FW_PHYSANIM_PART_PINNED   4  /* 1 = follows the animation exactly while the rest simulates */

/* aver_fw_physanim_state: life is 0 Alive, 1 Dying, 2 Dead, 3 GettingUp; facing is 0 Upright, 1 Supine, 2 Prone, 3 Side. */

typedef int32_t (AVER_FW_CALL* aver_fw_physanim_fn)(int32_t op, int32_t entity, int64_t h0, int64_t h1,
                                                    const float* in, int32_t inCount, int64_t* outI, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_physanim_provider(aver_fw_physanim_fn fn, void* user);

/* A hit: the hit bone and its neighbours blend into the simulation, the part gets the impulse, and the ragdoll
 * removes itself when the reaction has run out. radius > 0 makes it a radial event around `point`. */
AVER_FW_ABI int32_t aver_fw_physanim_hit(int32_t entity, int64_t boneHash, float ix, float iy, float iz,
                                         float px, float py, float pz, float damage, float radius);
/* The entity physical animation profile (.ocphysanim ObjectId); 0 for the defaults. */
AVER_FW_ABI int32_t aver_fw_physanim_set_profile(int32_t entity, int64_t profileId);
/* Ramps the whole-body physical animation weight to `weight` over `seconds`. */
AVER_FW_ABI int32_t aver_fw_physanim_blend(int32_t entity, float weight, float seconds);
/* Simulates `boneHash` and everything below it, the rest follows the animation; 0 = the whole body. */
AVER_FW_ABI int32_t aver_fw_physanim_simulate_subtree(int32_t entity, int64_t boneHash);
/* One value of one bone drive, see AVER_FW_PHYSANIM_PART_*. */
AVER_FW_ABI int32_t aver_fw_physanim_set_part(int32_t entity, int64_t boneHash, int32_t kind, float value);
AVER_FW_ABI int32_t aver_fw_physanim_die(int32_t entity);
AVER_FW_ABI int32_t aver_fw_physanim_get_up(int32_t entity);
/* Life state, how the body lies and how many bones are still reacting. 1 when the entity has a ragdoll; otherwise 0
 * and the outputs read Alive / Upright / 0. Any output may be null. */
AVER_FW_ABI int32_t aver_fw_physanim_state(int32_t entity, int32_t* outLife, int32_t* outFacing, int32_t* outReacting);

#ifdef __cplusplus
}
#endif

#endif
