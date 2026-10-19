// Spawn-at-event: play an effect once, at a point, with no entity. The cross-feature contract soft-body dust and
// vehicle skid smoke call (through aver::spawnEffectAt from Aver.Core when they link no particle module).
//
// Semantics: a request creates `count` particles in every emitter of the system whose SPAWNAT is on, at `position`,
// with the effect's authored direction rotated so +Z maps onto `direction` (zero: no rotation). It is
// fire-and-forget; requests are queued and applied at the start of the next ParticleSystem::tick. Particles live in
// shared per-effect "event instances" that retire after 5 idle seconds, so a thousand skid puffs are one simulation.
// Game thread only. Returns the particles accepted after clamps (maxParticles per emitter, maxSpawnAtPerFrame), 0
// for an unknown effect (logged once per id) or when particles are not installed.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

namespace aver::particles {

struct SpawnAtParams {
    u64  effect = 0;                   // particles::effectId(path)
    Vec3 position{0, 0, 0};            // world, cm
    Vec3 direction{0, 0, 1};           // world; need not be unit; zero => no rotation
    u32  count = 1;                    // particles per participating emitter
    f32  speedScale = 1, sizeScale = 1, lifetimeScale = 1;
    Vec3 inheritedVelocity{0, 0, 0};   // cm/s, scaled by the effect's spawn.inheritvelocity
    f32  tint[4] = {1, 1, 1, 1};
    u32  seed = 0;                     // 0: the engine counter (deterministic per run)
};

u32 spawnAt(const SpawnAtParams& params);                                                // process-global ParticleSystem
u32 spawnAt(u64 effect, const Vec3& position, const Vec3& direction, u32 count = 1);     // the one-line form

// The rotation (row-vector convention) mapping +Z onto `direction`, translated to `position`. Exposed for tests.
Mat4 spawnAtTransform(const Vec3& position, const Vec3& direction);

class ParticleSystem;
// Points aver::spawnEffectAt (Aver.Core) at `system`; null clears it. The composition root calls this once.
void installEffectSpawner(ParticleSystem* system);

} // namespace aver::particles
