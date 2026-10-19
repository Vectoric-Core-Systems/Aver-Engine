// Hand-run test for the CPU half of Aver.Particles: burst and continuous spawning, the emission
// accumulator, lifetime death, gravity/damping integration, and (wave 10) systems of emitters, events, script
// operations, the CPU budget and AutoDestroy. No RHI, no device -- ParticleSystem
// has none, by design (see its own header comment). Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleModules.hpp"
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


    // ---- wave 10: a system of emitters. A death event spawns into another emitter exactly one tick later, whatever
    //      their order in the file. ----
    for (int childFirst = 0; childFirst < 2; ++childFirst) {
        ParticleEffectLibrary lib;
        ParticleSystemDef def;
        ParticleEffect parent, child;
        parent.name = "Parent"; child.name = "Child";
        parent.emissionRate = 0.0f; parent.burstCount = 3; parent.maxParticles = 50;
        parent.lifetimeMin = parent.lifetimeMax = 0.25f;     // dies on the first 0.25 s tick
        parent.speedMin = parent.speedMax = 0.0f;
        child.emissionRate = 0.0f; child.burstCount = 0; child.maxParticles = 50;
        child.lifetimeMin = child.lifetimeMax = 10.0f;
        child.speedMin = child.speedMax = 0.0f;
        EventSpawn ev; ev.on = 0; ev.emitter = childFirst ? 0u : 1u; ev.count = 2;
        parent.events.push_back(ev);
        if (childFirst) { def.emitters = {child, parent}; } else { def.emitters = {parent, child}; }
        lib.setSystem(70 + static_cast<u64>(childFirst), def);

        const scene::Entity e = spawnEmitterEntity(world, 70 + static_cast<u64>(childFirst));
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.tick(world, 0.25f);
        u32 alive = 0;
        sys.forEachInstance([&](const InstanceView& v) {
            if (v.entity == e && v.effect->name == "Child") alive += v.aliveHint;
        });
        check(alive == 0, std::string("events: the child is empty on the tick the parent dies (child ") + (childFirst ? "first)" : "second)"));
        sys.tick(world, 0.25f);
        alive = 0;
        sys.forEachInstance([&](const InstanceView& v) {
            if (v.entity == e && v.effect->name == "Child") alive += v.aliveHint;
        });
        check(alive == 6, std::string("events: three deaths x two = six children one tick later (child ") + (childFirst ? "first)" : "second)"));
    }

    // ---- wave 10: script operations: parameters, bursts, modifiers, stop ----
    {
        ParticleEffectLibrary lib;
        ParticleSystemDef def;
        ParticleEffect fx;
        fx.emissionRate = 0.0f; fx.burstCount = 0; fx.maxParticles = 100;
        fx.lifetimeMin = fx.lifetimeMax = 10.0f;
        fx.speedMin = fx.speedMax = 0.0f;
        Module f = makeDefaultModule(ModuleKind::UpdateForce);
        f.p[0].userSlot = 0; f.p[0].comp = 0;
        fx.modules.push_back(f);
        def.emitters.push_back(fx);
        UserParam up; up.name = "Push"; up.kind = 0; up.v[0] = 0.0f;
        def.params.push_back(up);
        lib.setSystem(80, def);

        const scene::Entity e = spawnEmitterEntity(world, 80);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 0, "ops: nothing spawns without a burst");
        sys.burst(e, 4);
        const f32 v[4] = {50.0f, 0, 0, 0};
        sys.setParam(e, fnv1a64(std::string_view("Push")), 0, v);
        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 4, "ops: a queued burst spawns on the next tick");
        const auto* ps = sys.particles(e);
        check(ps && !ps->empty(), "ops: particles exist");
        if (ps && !ps->empty()) checkNear((*ps)[0].velocity.x, 5.0f, 1e-3f, "ops: the queued parameter reached the module this tick");
        u32 alive = 0, tier = 9;
        check(sys.emitterState(e, alive, tier) && alive == 4 && tier == 0, "ops: emitterState reports the CPU tier and the count");
        sys.setModifiers(e, 1.0f, 2.0f, 1.0f);
        sys.burst(e, 1);
        sys.tick(world, 0.1f);
        ps = sys.particles(e);
        check(ps && ps->size() == 5 && ps->back().sizeScale == 2.0f, "ops: the size modifier scales new particles");
        sys.stop(e, true);
        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 0, "ops: stop with killParticles clears the emitter");
        auto* c = world.component<scene::CParticleEmitter>(e, scene::kComponentParticleEmitter);
        check(c && (c->flags & scene::kParticleEmitterStopped), "ops: stop sets the component's stopped flag");
        const ParticleStats st = sys.stats();
        check(st.cpuEmitters == 1 && st.gpuEmitters == 0, "stats: one CPU emitter");
    }

    // ---- wave 10: the CPU budget drops new spawns ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 0.0f; fx.burstCount = 100; fx.maxParticles = 1000;
        fx.lifetimeMin = fx.lifetimeMax = 10.0f;
        lib.set(90, fx);
        const scene::Entity e = spawnEmitterEntity(world, 90);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        ParticleBudget b;
        b.maxCpuParticles = 30;
        sys.setBudget(b);
        sys.tick(world, 0.1f);
        check(sys.liveParticles(e) == 30, "budget: the CPU cap clamps the burst");
    }

    // ---- wave 10: AutoDestroy ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx;
        fx.emissionRate = 0.0f; fx.burstCount = 2; fx.maxParticles = 10;
        fx.lifetimeMin = fx.lifetimeMax = 0.5f;
        fx.speedMin = fx.speedMax = 0.0f;
        lib.set(91, fx);
        const scene::Entity e = spawnEmitterEntity(world, 91);
        auto* c = world.component<scene::CParticleEmitter>(e, scene::kComponentParticleEmitter);
        if (c) c->flags |= scene::kParticleEmitterAutoDestroy;
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.tick(world, 0.125f);
        check(sys.finished().empty(), "autodestroy: still playing, nothing finishes");
        if (c) c->flags |= scene::kParticleEmitterStopped;
        sys.tick(world, 0.125f);
        check(sys.finished().empty(), "autodestroy: stopped but particles alive: not yet");
        sys.tick(world, 0.25f);
        check(sys.finished().size() == 1 && sys.finished()[0] == e, "autodestroy: stopped and drained: finished, destruction queued");
        check(world.destroyPending(e), "autodestroy: the entity is queued for destruction");
        sys.finished().clear();
    }

    AVER_INFO("ParticleSystemTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
