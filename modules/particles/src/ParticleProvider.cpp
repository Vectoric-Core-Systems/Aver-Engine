// The particle C ABI's provider: a switch over the ops of framework_particles_abi.h onto ParticleSystem.
#include "aver/particles/ParticleProvider.hpp"

#include "aver/particles/ParticleSystem.hpp"

namespace aver::particles {
namespace {

constexpr i32 kOpSpawnAt = 1, kOpSetParam = 2, kOpBurst = 3, kOpStop = 4, kOpModifiers = 5, kOpState = 6,
              kOpStats = 7, kOpBudget = 8, kOpGpuMode = 9;

} // namespace

i32 particleProvider(i32 op, i32 entity, i64 h0, i64 h1, const f32* in, f32* outF, i32* outI, void* user) {
    (void)outF;
    if (!user) return 0;
    ParticleSystem& sys = *static_cast<ParticleSystem*>(user);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    switch (op) {
    case kOpSpawnAt: {
        if (!in) return 0;
        SpawnAtParams p;
        p.effect = static_cast<u64>(h0);
        p.position = Vec3{in[0], in[1], in[2]};
        p.direction = Vec3{in[3], in[4], in[5]};
        p.inheritedVelocity = Vec3{in[6], in[7], in[8]};
        p.speedScale = in[9];
        p.sizeScale = in[10];
        p.lifetimeScale = in[11];
        for (int i = 0; i < 4; ++i) p.tint[i] = in[12 + i];
        const f32 count = in[16];
        if (!(count >= 1.0f)) return 0;
        p.count = static_cast<u32>(count > 1e6f ? 1e6f : count);
        return static_cast<i32>(sys.spawnAt(p));
    }
    case kOpSetParam:
        if (!in) return 0;
        sys.setParam(e, static_cast<u64>(h0), static_cast<u8>(h1 < 0 ? 0 : (h1 > 2 ? 2 : h1)), in);
        return 1;
    case kOpBurst:
        if (!in || !(in[0] >= 1.0f)) return 0;
        sys.burst(e, static_cast<i32>(in[0]));
        return 1;
    case kOpStop:
        sys.stop(e, in && in[0] != 0.0f);
        return 1;
    case kOpModifiers:
        if (!in) return 0;
        sys.setModifiers(e, in[0], in[1], in[2]);
        return 1;
    case kOpState: {
        if (!outI) return 0;
        u32 alive = 0, tier = 0;
        if (!sys.emitterState(e, alive, tier)) return 0;
        outI[0] = static_cast<i32>(alive);
        outI[1] = static_cast<i32>(tier);
        return 1;
    }
    case kOpStats: {
        if (!outI) return 0;
        const ParticleStats s = sys.stats();
        outI[0] = static_cast<i32>(s.gpuEmitters);
        outI[1] = static_cast<i32>(s.cpuEmitters);
        outI[2] = static_cast<i32>(s.gpuParticles);
        outI[3] = static_cast<i32>(s.cpuParticles);
        outI[4] = static_cast<i32>(s.spawnAtDropped);
        outI[5] = static_cast<i32>(s.gpuAllocatedKB);
        return 1;
    }
    case kOpBudget: {
        if (!in) return 0;
        ParticleBudget b = sys.budget();
        if (in[0] >= 0.0f) b.maxGpuParticles = static_cast<u32>(in[0]);
        if (in[1] >= 0.0f) b.maxCpuParticles = static_cast<u32>(in[1]);
        if (in[2] >= 0.0f) b.maxSpawnAtPerFrame = static_cast<u32>(in[2]);
        sys.setBudget(b);
        return 1;
    }
    case kOpGpuMode:
        if (!in) return 0;
        sys.setGpuMode(static_cast<int>(in[0]));
        return 1;
    default:
        return 0;
    }
}

} // namespace aver::particles
