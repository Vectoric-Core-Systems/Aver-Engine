#pragma once
// A dependency-free seam for "play this particle effect at this point": soft-body dust, vehicle skid smoke and any
// other system that wants a one-shot effect calls spawnEffectAt() and links nothing particle-shaped. Aver.Particles
// installs the spawner (setEffectSpawner); with none installed (AVER_MODULE_PARTICLES=OFF, or particles not wired
// yet) every call returns 0. Game thread only.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

namespace aver {

// effectId is fnv1a64 of the content-relative .ocparticle path (particles::effectId). `direction` need not be unit;
// zero means "the effect's authored direction". Returns particles accepted.
using EffectSpawnFn = u32 (*)(u64 effectId, const Vec3& position, const Vec3& direction, u32 count, void* user);

namespace detail {
struct EffectSpawner {
    EffectSpawnFn fn = nullptr;
    void* user = nullptr;
};
inline EffectSpawner& effectSpawner() {
    static EffectSpawner s;
    return s;
}
} // namespace detail

// Installs (or, with a null fn, clears) the process-wide spawner.
inline void setEffectSpawner(EffectSpawnFn fn, void* user) {
    detail::effectSpawner() = detail::EffectSpawner{fn, user};
}

// Spawns `count` particles of the effect at `position` along `direction`. 0 when none is installed.
inline u32 spawnEffectAt(u64 effectId, const Vec3& position, const Vec3& direction, u32 count = 1) {
    const detail::EffectSpawner& s = detail::effectSpawner();
    return s.fn ? s.fn(effectId, position, direction, count, s.user) : 0u;
}

} // namespace aver
