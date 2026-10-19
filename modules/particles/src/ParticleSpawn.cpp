// The spawn-at free functions, the +Z-to-direction transform, and the Aver.Core seam installer.
#include "aver/particles/ParticleSpawn.hpp"

#include "aver/core/EffectSpawn.hpp"
#include "aver/particles/ParticleSystem.hpp"

#include <cmath>

namespace aver::particles {

Mat4 spawnAtTransform(const Vec3& position, const Vec3& direction) {
    Mat4 m = Mat4::translation(position);
    const Vec3 z = direction.getSafeNormal();
    if (z.sizeSquared() < 0.5f) return m;   // zero direction: no rotation
    const Vec3 hint = std::fabs(z.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 x = cross(hint, z).getSafeNormal();
    const Vec3 y = cross(z, x);
    m.m[0][0] = x.x; m.m[0][1] = x.y; m.m[0][2] = x.z;
    m.m[1][0] = y.x; m.m[1][1] = y.y; m.m[1][2] = y.z;
    m.m[2][0] = z.x; m.m[2][1] = z.y; m.m[2][2] = z.z;
    return m;
}

u32 spawnAt(const SpawnAtParams& params) { return particleSystem().spawnAt(params); }
u32 spawnAt(u64 effect, const Vec3& position, const Vec3& direction, u32 count) {
    SpawnAtParams p;
    p.effect = effect;
    p.position = position;
    p.direction = direction;
    p.count = count;
    return particleSystem().spawnAt(p);
}

namespace {
u32 spawnThroughCore(u64 effectId, const Vec3& position, const Vec3& direction, u32 count, void* user) {
    ParticleSystem* sys = static_cast<ParticleSystem*>(user);
    SpawnAtParams p;
    p.effect = effectId;
    p.position = position;
    p.direction = direction;
    p.count = count;
    return sys->spawnAt(p);
}
} // namespace

void installEffectSpawner(ParticleSystem* system) {
    if (system) aver::setEffectSpawner(&spawnThroughCore, system);
    else aver::setEffectSpawner(nullptr, nullptr);
}

} // namespace aver::particles
