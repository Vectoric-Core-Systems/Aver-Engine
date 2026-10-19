#ifndef AVER_FRAMEWORK_PARTICLES_ABI_H
#define AVER_FRAMEWORK_PARTICLES_ABI_H

/* Particle C ABI (Aver.Framework): spawn an effect at a point, drive an emitter's user parameters, burst, stop and
 * query, relayed to particles::ParticleSystem.
 *
 * Same conventions as framework_abi.h (int32_t/int64_t/float only, 1 on success and 0 on refusal). Aver.Framework does
 * not link the particle module, so a composition root installs ONE provider function and every call below forwards to it;
 * with no provider every call returns 0 and nothing is written, the answers an engine with no particles gives. The
 * provider is particles::particleProvider (modules/particles/include/aver/particles/ParticleProvider.hpp). Own version,
 * separate from AVER_FW_ABI_VERSION_MINOR. All hashes are fnv1a64 (Assets.ObjectIdOf in C# for an effect path; the
 * parameter name's UTF-8 bytes for a user parameter). Semantics: docs/PARTICLES.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI, AVER_FW_CALL */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_PARTICLES_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_particles_abi_version(void);

/* Provider operations; the argument usage of each is listed with it. `in` is a float array, outF / outI results. */
#define AVER_FW_PARTICLES_OP_SPAWN_AT   1   /* h0 effect; in[17]: pos3 dir3 vel3 speedScale sizeScale lifeScale tint4 count; returns accepted */
#define AVER_FW_PARTICLES_OP_SET_PARAM  2   /* entity, h0 name hash, h1 kind (0 float 1 vec3 2 colour), in[4]; returns 1 */
#define AVER_FW_PARTICLES_OP_BURST      3   /* entity, in[0] count */
#define AVER_FW_PARTICLES_OP_STOP       4   /* entity, in[0] killParticles (0/1) */
#define AVER_FW_PARTICLES_OP_MODIFIERS  5   /* entity, in[0..2] rateScale sizeScale speedScale */
#define AVER_FW_PARTICLES_OP_STATE      6   /* entity; outI[0] alive (approximate on the GPU tier), outI[1] tier (0 cpu 1 gpu) */
#define AVER_FW_PARTICLES_OP_STATS      7   /* outI[0..5] gpuEmitters cpuEmitters gpuParticles cpuParticles spawnAtDropped gpuAllocatedKB */
#define AVER_FW_PARTICLES_OP_BUDGET     8   /* in[0..2] maxGpu maxCpu maxSpawnAtPerFrame (<0 keeps) */
#define AVER_FW_PARTICLES_OP_GPU_MODE   9   /* in[0] 0 off 1 auto 2 on */

typedef int32_t (AVER_FW_CALL* aver_fw_particles_fn)(int32_t op, int32_t entity, int64_t h0, int64_t h1,
                                                     const float* in, float* outF, int32_t* outI, void* user);
/* Installs the provider. Passing null clears it. Always returns 1. */
AVER_FW_ABI int32_t aver_fw_set_particles_provider(aver_fw_particles_fn fn, void* user);

/* Plays the effect once at a world point, fired along (dx, dy, dz) (zero: the effect authored direction). Returns
 * the particles accepted (0: unknown effect, no provider, or the per-frame budget is spent). */
AVER_FW_ABI int32_t aver_fw_particles_spawn_at(int64_t effect, float px, float py, float pz,
                                               float dx, float dy, float dz, int32_t count);
/* As above with args16 = pos3 dir3 inheritedVelocity3 speedScale sizeScale lifetimeScale tint4. */
AVER_FW_ABI int32_t aver_fw_particles_spawn_at_ex(int64_t effect, const float* args16, int32_t count);

/* Sets a user parameter of the entity emitter (kind 0 float uses v0; 1 vec3 v0..v2; 2 colour v0..v3). Queued;
 * applied at the top of the next particle tick. Returns 1 when queued. */
AVER_FW_ABI int32_t aver_fw_particles_set_param(int32_t entity, int64_t nameHash, int32_t kind,
                                                float v0, float v1, float v2, float v3);
AVER_FW_ABI int32_t aver_fw_particles_burst(int32_t entity, int32_t count);
AVER_FW_ABI int32_t aver_fw_particles_stop(int32_t entity, int32_t killParticles);
AVER_FW_ABI int32_t aver_fw_particles_set_modifiers(int32_t entity, float rateScale, float sizeScale, float speedScale);
/* Writes the alive count (a few frames old on the GPU tier) and the tier (0 cpu, 1 gpu); untouched and 0 when the
 * entity has no ticked emitter. */
AVER_FW_ABI int32_t aver_fw_particles_emitter_state(int32_t entity, int32_t* outAlive, int32_t* outTier);
/* Writes six ints: gpuEmitters cpuEmitters gpuParticles cpuParticles spawnAtDropped gpuAllocatedKB. */
AVER_FW_ABI int32_t aver_fw_particles_stats(int32_t* outSix);
/* Budgets; a negative value keeps the current one. */
AVER_FW_ABI int32_t aver_fw_particles_set_budget(int32_t maxGpu, int32_t maxCpu, int32_t maxSpawnAtPerFrame);
/* 0 off (CPU only), 1 auto, 2 on. */
AVER_FW_ABI int32_t aver_fw_particles_set_gpu_mode(int32_t mode);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_PARTICLES_ABI_H */
