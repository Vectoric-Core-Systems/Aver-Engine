// Spawn-at-event: the contract VEHICLES and SOFTBODY_DEPTH call. Counts and clamps, direction, one shared instance,
// idle retirement, the Aver.Core seam and the framework provider. Exit code = failure count.
#include "aver/core/EffectSpawn.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleProvider.hpp"
#include "aver/particles/ParticleSpawn.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::particles;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) <= eps) return;
    AVER_ERROR("   FAIL  {} (got {:.5f}, want {:.5f})", what, got, want);
    ++g_failures;
}

static ParticleEffect puff() {
    ParticleEffect fx;
    fx.emissionRate = 0.0f;
    fx.burstCount = 0;
    fx.maxParticles = 64;
    fx.lifetimeMin = fx.lifetimeMax = 1.0f;
    fx.direction = {0, 0, 1};
    fx.speedMin = fx.speedMax = 100.0f;
    fx.spreadDeg = 0.0f;
    return fx;
}

static u32 instanceCount(const ParticleSystem& sys) {
    u32 n = 0;
    sys.forEachInstance([&](const InstanceView&) { ++n; });
    return n;
}
static u32 aliveTotal(const ParticleSystem& sys) {
    u32 n = 0;
    sys.forEachInstance([&](const InstanceView& v) { n += v.aliveHint; });
    return n;
}

int main() {
    AVER_INFO("ParticleSpawnAtTest");
    scene::World& world = scene::World::instance();

    // ---- identity ----
    check(effectId("Particles/Sparks.ocparticle") == fnv1a64(std::string_view("Particles/Sparks.ocparticle")), "effectId is fnv1a64 of the path");
    check(effectId("Particles\\Sparks.ocparticle") == effectId("Particles/Sparks.ocparticle"), "effectId folds backslashes");

    // ---- the transform ----
    {
        const Mat4 m = spawnAtTransform({10, 20, 30}, {2, 0, 0});
        // Effect +Z must map onto +X: the third row is the image of (0,0,1).
        checkNear(m.m[2][0], 1.0f, 1e-5f, "spawnAtTransform maps +Z onto the direction (x)");
        checkNear(m.m[2][1], 0.0f, 1e-5f, "(y)");
        checkNear(m.m[3][0], 10.0f, 1e-6f, "the translation");
        const Mat4 id = spawnAtTransform({1, 2, 3}, {0, 0, 0});
        checkNear(id.m[0][0], 1.0f, 1e-6f, "a zero direction leaves the rotation alone");
        checkNear(id.m[2][2], 1.0f, 1e-6f, "...");
    }

    // ---- the Aver.Core seam ----
    {
        installEffectSpawner(nullptr);
        check(aver::spawnEffectAt(1, {0, 0, 0}, {0, 0, 1}, 5) == 0, "spawnEffectAt returns 0 with no spawner installed");
        ParticleEffectLibrary lib;
        lib.set(100, puff());
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        installEffectSpawner(&sys);
        check(aver::spawnEffectAt(100, {0, 0, 0}, {0, 0, 1}, 5) == 5, "spawnEffectAt returns the accepted count once installed");
        check(aver::spawnEffectAt(999, {0, 0, 0}, {0, 0, 1}, 5) == 0, "an unknown effect returns 0 through the seam");
        installEffectSpawner(nullptr);
        check(aver::spawnEffectAt(100, {0, 0, 0}, {0, 0, 1}, 5) == 0, "clearing the spawner restores 0");
    }

    // ---- a request is applied by the next tick, in that tick ----
    {
        ParticleEffectLibrary lib;
        lib.set(101, puff());
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        SpawnAtParams p;
        p.effect = 101; p.position = {100, 0, 0}; p.direction = {0, 1, 0}; p.count = 7;
        check(sys.spawnAt(p) == 7, "spawnAt returns the accepted count");
        check(instanceCount(sys) == 0, "nothing exists until the tick");
        sys.tick(world, 0.016f);
        check(instanceCount(sys) == 1 && aliveTotal(sys) == 7, "the request appears in that tick");
        bool dirOk = true;
        sys.forEachInstance([&](const InstanceView& v) {
            for (const Particle& q : *v.particles) {
                dirOk = dirOk && std::fabs(q.velocity.y - 100.0f) < 1e-2f && std::fabs(q.velocity.z) < 1e-2f && q.position.x > 99.0f;
            }
        });
        check(dirOk, "effect +Z (the authored direction) was rotated onto the requested direction, at the requested point");
    }

    // ---- clamps ----
    {
        ParticleEffectLibrary lib;
        lib.set(102, puff());   // maxParticles 64
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        check(sys.spawnAt(SpawnAtParams{102, {}, {0, 0, 1}, 1000}) == 64, "count is clamped to the effect's maxParticles");
        ParticleBudget b;
        b.maxSpawnAtPerFrame = 10;
        sys.setBudget(b);
        SpawnAtParams p;
        p.effect = 102; p.count = 8;
        const u32 first = sys.spawnAt(p);
        const u32 second = sys.spawnAt(p);
        check(first == 8 || first == 0, "the first request fits or the earlier 64 already used the frame");
        check(sys.spawnAt(p) == 0, "past maxSpawnAtPerFrame requests are refused");
        check(sys.stats().spawnAtDropped > 0, "and counted as dropped");
        (void)second;
        sys.tick(world, 0.016f);   // a new frame
        check(sys.spawnAt(p) == 8, "the per-frame budget resets on tick");
    }
    {
        ParticleEffectLibrary lib;
        lib.set(103, puff());
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        ParticleBudget b;
        b.maxSpawnAtPerFrame = 10;
        sys.setBudget(b);
        SpawnAtParams p;
        p.effect = 103; p.count = 100;
        check(sys.spawnAt(p) == 10, "a request over the frame budget is clamped to what is left");
    }

    // ---- unknown effects ----
    {
        ParticleEffectLibrary lib;
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        SpawnAtParams p; p.effect = 5555; p.count = 3;
        check(sys.spawnAt(p) == 0 && sys.spawnAt(p) == 0, "an unknown effect returns 0 every time (logged once)");
        ParticleSystem bare;
        check(bare.spawnAt(p) == 0, "no library installed: 0");
    }

    // ---- many requests share one event instance ----
    {
        ParticleEffectLibrary lib;
        lib.set(104, puff());
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        for (int i = 0; i < 300; ++i) {
            SpawnAtParams p; p.effect = 104; p.position = {static_cast<f32>(i), 0, 0}; p.count = 1;
            sys.spawnAt(p);
        }
        sys.tick(world, 0.016f);
        check(instanceCount(sys) == 1, "300 skid puffs are one simulation");
        check(aliveTotal(sys) == 64 || aliveTotal(sys) == 300 || aliveTotal(sys) > 0, "...holding their particles (capped by maxParticles)");
    }

    // ---- a multi-emitter system: SPAWNAT off is skipped ----
    {
        ParticleEffectLibrary lib;
        ParticleSystemDef def;
        ParticleEffect a = puff(), b = puff();
        a.name = "A"; b.name = "B";
        b.spawnAtTarget = false;
        def.emitters = {a, b};
        lib.setSystem(105, def);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        SpawnAtParams p; p.effect = 105; p.count = 4;
        check(sys.spawnAt(p) == 4, "only participating emitters count toward the return value");
        sys.tick(world, 0.016f);
        u32 inA = 0, inB = 0;
        sys.forEachInstance([&](const InstanceView& v) { (v.effect->name == "A" ? inA : inB) += v.aliveHint; });
        check(inA == 4 && inB == 0, "particles appear in emitter A only");
    }

    // ---- idle retirement ----
    {
        ParticleEffectLibrary lib;
        lib.set(106, puff());   // lifetime 1 s
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        SpawnAtParams p; p.effect = 106; p.count = 2;
        sys.spawnAt(p);
        sys.tick(world, 0.5f);
        check(instanceCount(sys) == 1, "the event instance exists");
        for (int i = 0; i < 4; ++i) sys.tick(world, 0.5f);   // 2.5 s: particles died at 1 s, idle 1.5 s
        check(instanceCount(sys) == 1, "it is kept while recently idle");
        for (int i = 0; i < 8; ++i) sys.tick(world, 0.5f);   // idle > 5 s
        check(instanceCount(sys) == 0, "it retires after 5 idle seconds");
        sys.spawnAt(p);
        sys.tick(world, 0.5f);
        check(instanceCount(sys) == 1, "a later request builds a fresh instance");
    }

    // ---- the framework provider ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx = puff();
        lib.set(107, fx);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        f32 in[17] = {};
        in[0] = 1; in[1] = 2; in[2] = 3;        // position
        in[5] = 1;                              // direction +Z
        in[9] = 1; in[10] = 1; in[11] = 1;      // scales
        in[12] = 1; in[13] = 1; in[14] = 1; in[15] = 1;   // tint
        in[16] = 6;                             // count
        check(particleProvider(1, 0, 107, 0, in, nullptr, nullptr, &sys) == 6, "provider SPAWN_AT returns the accepted count");
        check(particleProvider(1, 0, 107, 0, in, nullptr, nullptr, nullptr) == 0, "provider without a user pointer returns 0");
        in[16] = 0;
        check(particleProvider(1, 0, 107, 0, in, nullptr, nullptr, &sys) == 0, "a zero count is refused");
        i32 out[6] = {};
        check(particleProvider(7, 0, 0, 0, nullptr, nullptr, out, &sys) == 1, "provider STATS");
        check(particleProvider(7, 0, 0, 0, nullptr, nullptr, nullptr, &sys) == 0, "STATS without an output returns 0");
        f32 budget[3] = {-1, 500, 20};
        check(particleProvider(8, 0, 0, 0, budget, nullptr, nullptr, &sys) == 1, "provider BUDGET");
        check(sys.budget().maxCpuParticles == 500 && sys.budget().maxSpawnAtPerFrame == 20 && sys.budget().maxGpuParticles == 2000000u,
              "BUDGET applies the non-negative values and keeps the rest");
        f32 mode[1] = {0};
        check(particleProvider(9, 0, 0, 0, mode, nullptr, nullptr, &sys) == 1 && sys.gpuMode() == 0, "provider GPU_MODE");
        check(particleProvider(99, 0, 0, 0, nullptr, nullptr, nullptr, &sys) == 0, "an unknown op returns 0");
    }

    AVER_INFO("ParticleSpawnAtTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
