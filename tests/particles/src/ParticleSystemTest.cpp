// Hand-run test for the CPU half of Aver.Particles: burst and continuous spawning, the emission
// accumulator, lifetime death, and gravity/damping integration. No RHI, no device -- ParticleSystem
// has none, by design (see its own header comment). Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::particles;

static int g_checks = 0;
static int g_failures = 0;

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

// A fresh entity carrying a CParticleEmitter pointed at `effectId`. Every test gets its own entity
// so one test's live particles can never leak into another's counts.
static scene::Entity spawnEmitterEntity(scene::World& world, u64 effectId) {
    const scene::Entity e = world.create("ParticleSystemTest.Emitter");
    auto* c = static_cast<scene::CParticleEmitter*>(world.addComponent(e, scene::kComponentParticleEmitter));
    check(c != nullptr, "CParticleEmitter attaches");
    if (c) c->effect = effectId;
    return e;
}

int main() {
    AVER_INFO("ParticleSystemTest");
    scene::World& world = scene::World::instance();

    // ---- a burst fires exactly once, on the stopped -> playing edge, never again while playing ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 0.0f;          // burst-only: a one-shot spark, not a continuous stream
        fx.burstCount = 5;
        fx.maxParticles = 100;
        fx.lifetimeMin = fx.lifetimeMax = 10.0f;   // long: nothing dies mid-test
        fx.direction = {0, 0, 1};
        fx.spreadDeg = 0.0f;
        fx.speedMin = fx.speedMax = 200.0f;
        fx.gravity = {0, 0, 0};
        lib.set(1, fx);

        const scene::Entity e = spawnEmitterEntity(world, 1);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);

        sys.tick(world, 0.016f);
        check(sys.liveParticles(e) == 5, "burst spawns exactly burstCount on the first tick");

        sys.tick(world, 0.016f);
        check(sys.liveParticles(e) == 5, "a second tick, still playing, fires the burst no further times");

        // Zero spread, fixed speed: velocity is deterministic, so position after two ticks is checkable.
        const auto* ps = sys.particles(e);
        check(ps != nullptr && !ps->empty(), "particles(e) returns the live array");
        if (ps && !ps->empty()) {
            const Particle& p = (*ps)[0];
            checkNear(p.velocity.x, 0.0f, 1e-4f, "zero spread: velocity.x stays 0");
            checkNear(p.velocity.y, 0.0f, 1e-4f, "zero spread: velocity.y stays 0");
            checkNear(p.velocity.z, 200.0f, 1e-3f, "zero spread, no gravity: velocity.z stays the drawn speed");
            // Position integrates AFTER spawn, in the SAME tick it was born in (tick() spawns then
            // updates), so one particle sees dt applied twice across these two 0.016f ticks: once on
            // the tick it spawned, once on the tick after.
            checkNear(p.position.z, 200.0f * 0.032f, 0.05f, "position integrated over both ticks");
        }
    }

    // ---- the emission accumulator carries a fractional particle across ticks, exactly ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 8.0f;   // with dt = 0.125 (both exact in binary), 8*0.125 = 1.0 exactly:
        fx.burstCount = 0;         // no fp-rounding ambiguity about which tick a spawn lands on.
        fx.maxParticles = 100;
        fx.lifetimeMin = fx.lifetimeMax = 10.0f;
        fx.direction = {0, 0, 1};
        fx.spreadDeg = 0.0f;
        fx.speedMin = fx.speedMax = 50.0f;
        lib.set(2, fx);

        const scene::Entity e = spawnEmitterEntity(world, 2);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);

        sys.tick(world, 0.125f);
        check(sys.liveParticles(e) == 1, "exactly one particle per tick at rate*dt == 1.0");
        sys.tick(world, 0.125f);
        check(sys.liveParticles(e) == 2, "the accumulator keeps paying out one per tick, not zero or two");
        sys.tick(world, 0.125f);
        check(sys.liveParticles(e) == 3, "three ticks, three particles");
    }

    // ---- a particle dies exactly when its drawn lifetime elapses ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 0.0f;
        fx.burstCount = 1;
        fx.maxParticles = 10;
        fx.lifetimeMin = fx.lifetimeMax = 0.5f;   // 0.25 + 0.25 == 0.5 exactly in float
        fx.direction = {0, 0, 1};
        fx.spreadDeg = 0.0f;
        fx.speedMin = fx.speedMax = 0.0f;         // stays put: only the clock matters here
        lib.set(3, fx);

        const scene::Entity e = spawnEmitterEntity(world, 3);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);

        sys.tick(world, 0.25f);
        check(sys.liveParticles(e) == 1, "the burst particle is alive at age 0.25 < lifetime 0.5");
        sys.tick(world, 0.25f);
        check(sys.liveParticles(e) == 0, "and dead at age 0.5 >= lifetime 0.5");
    }

    // ---- constant gravity and velocity damping, and nothing else (DECIDED: no physics coupling) ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 0.0f;
        fx.burstCount = 1;
        fx.maxParticles = 10;
        fx.lifetimeMin = fx.lifetimeMax = 10.0f;
        fx.direction = {0, 0, 1};
        fx.spreadDeg = 0.0f;
        fx.speedMin = fx.speedMax = 0.0f;     // starts at rest: isolates gravity + damping alone
        fx.gravity = {0, 0, -981.0f};          // cm/s^2, a real-world-ish "down"
        fx.damping = 0.5f;                     // 50%/second
        lib.set(4, fx);

        const scene::Entity e = spawnEmitterEntity(world, 4);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);

        // One tick: spawns at rest, THEN this same tick's update applies gravity*dt to velocity,
        // damping to the result, and integrates position from the damped velocity -- see
        // ParticleSystem.cpp's updateParticles for the exact order.
        const f32 dt = 0.1f;
        sys.tick(world, dt);
        const auto* ps = sys.particles(e);
        check(ps != nullptr && ps->size() == 1, "one burst particle exists after the first tick");
        if (ps && ps->size() == 1) {
            const Particle& p = (*ps)[0];
            const f32 vAfterGravity = -981.0f * dt;                  // -98.1
            const f32 vExpected = vAfterGravity * (1.0f - 0.5f * dt);   // damping applied after gravity
            checkNear(p.velocity.z, vExpected, 0.05f, "velocity: gravity then damping, in that order");
            checkNear(p.position.z, vExpected * dt, 0.05f, "position integrates from the POST-damping velocity");
        }
    }

    // ---- stopping and restarting resets the clock and re-arms the burst ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 0.0f;
        fx.burstCount = 3;
        fx.maxParticles = 100;
        fx.lifetimeMin = fx.lifetimeMax = 10.0f;
        fx.direction = {0, 0, 1};
        fx.speedMin = fx.speedMax = 10.0f;
        lib.set(5, fx);

        const scene::Entity e = spawnEmitterEntity(world, 5);
        auto* c = world.component<scene::CParticleEmitter>(e, scene::kComponentParticleEmitter);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);

        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 3, "the burst fires once on attach (zero-filled flags == playing)");

        check(c != nullptr, "the component round-trips through World::component");
        if (c) c->flags |= scene::kParticleEmitterStopped;
        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 3, "stopped: no new spawns, and existing particles are untouched");

        if (c) c->flags &= ~scene::kParticleEmitterStopped;
        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 6, "restarted: the burst re-fires on top of what was already alive");
    }

    AVER_INFO("ParticleSystemTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
