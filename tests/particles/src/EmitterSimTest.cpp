// The CPU executor, with no scene, no GPU and no renderer: determinism, the budget, ids, the closed-form v1 trajectory,
// shapes, linked-ribbon ring allocation, trail rings, events and user-parameter binding. Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/EmitterSim.hpp"
#include "aver/particles/ParticleRandom.hpp"

#include <cmath>
#include <cstring>
#include <set>
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
    AVER_ERROR("   FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
    ++g_failures;
}

static ParticleEffect baseEffect() {
    ParticleEffect fx;
    fx.maxParticles = 1000;
    fx.lifetimeMin = fx.lifetimeMax = 10.0f;
    fx.speedMin = fx.speedMax = 100.0f;
    fx.spreadDeg = 0.0f;
    return fx;
}

static void testDeterminism() {
    AVER_INFO("--- determinism ---");
    ParticleEffect fx = baseEffect();
    fx.shape = EmitterShape::Sphere;
    fx.shapeSize = {50, 0, 0};
    fx.spreadDeg = 45.0f;
    fx.speedMin = 50; fx.speedMax = 150;
    fx.lifetimeMin = 1; fx.lifetimeMax = 3;
    fx.gravity = {0, 0, -100};
    Module m = makeDefaultModule(ModuleKind::SpawnSizeScale);
    m.p[0].value = 0.5f; m.p[1].value = 2.0f;
    fx.modules.push_back(m);
    Module c = makeDefaultModule(ModuleKind::UpdateCurlNoise);
    fx.modules.push_back(c);

    EmitterSim a, b;
    a.init(fx, {}, 777);
    b.init(fx, {}, 777);
    SpawnRequest req;
    req.xf = Mat4::translation({10, 20, 30});
    req.count = 200;
    a.spawn(req); b.spawn(req);
    for (int i = 0; i < 30; ++i) { a.step(0.016f, 0.016f * static_cast<f32>(i), nullptr); b.step(0.016f, 0.016f * static_cast<f32>(i), nullptr); }
    check(a.particles().size() == b.particles().size() && a.particles().size() > 0, "same seed: same population");
    check(a.particles().size() == b.particles().size() &&
          std::memcmp(a.particles().data(), b.particles().data(), a.particles().size() * sizeof(Particle)) == 0,
          "same seed: byte-identical particle arrays after 30 steps");

    EmitterSim c2;
    c2.init(fx, {}, 778);
    c2.spawn(req);
    for (int i = 0; i < 30; ++i) c2.step(0.016f, 0.016f * static_cast<f32>(i), nullptr);
    bool differs = c2.particles().size() != a.particles().size();
    if (!differs) differs = std::memcmp(a.particles().data(), c2.particles().data(), a.particles().size() * sizeof(Particle)) != 0;
    check(differs, "a different emitter seed gives different particles");

    // The request seed decorrelates two otherwise identical spawns from one sim.
    EmitterSim d;
    d.init(fx, {}, 5);
    SpawnRequest r1 = req, r2 = req;
    r1.count = r2.count = 1;
    r1.seed = 11; r2.seed = 12;
    d.spawn(r1); d.spawn(r2);
    check(d.particles().size() == 2 && (d.particles()[0].position.x != d.particles()[1].position.x), "request seeds decorrelate spawns");
}

static void testBudgetAndIds() {
    AVER_INFO("--- budget and ids ---");
    ParticleEffect fx = baseEffect();
    fx.maxParticles = 10;
    EmitterSim s;
    s.init(fx, {}, 1);
    SpawnRequest r;
    r.count = 25;
    check(s.spawn(r) == 10, "over budget: only maxParticles are accepted");
    check(s.particles().size() == 10, "the array holds maxParticles");
    check(s.nextId() == 25, "every requested particle consumed an id");
    check(s.spawn(r) == 0, "a full emitter accepts nothing (no eviction)");
    std::set<u32> ids;
    bool increasing = true;
    u32 last = 0;
    for (const Particle& p : s.particles()) { ids.insert(p.id); increasing = increasing && p.id >= last; last = p.id; }
    check(ids.size() == 10, "ids are unique");
    check(increasing, "ids increase along the array");
    s.step(100.0f, 100.0f, nullptr);   // everything dies
    check(s.particles().empty(), "lifetime death empties the array");
    SpawnRequest r1; r1.count = 3;
    s.spawn(r1);
    check(s.particles().size() == 3 && s.particles().front().id == 50, "ids are never reused after deaths");
}

static void testClosedForm() {
    AVER_INFO("--- a v1 effect reproduces the closed-form gravity + damping trajectory ---");
    ParticleEffect fx = baseEffect();
    fx.speedMin = fx.speedMax = 300.0f;
    fx.direction = {0, 0, 1};
    fx.gravity = {0, 0, -981};
    fx.damping = 0.4f;
    EmitterSim s;
    s.init(fx, {}, 3);
    SpawnRequest r; r.count = 1;
    s.spawn(r);
    const f32 dt = 0.016f;
    f64 v = 300.0, z = 0.0;
    for (int i = 0; i < 60; ++i) {
        s.step(dt, dt * static_cast<f32>(i), nullptr);
        v = (v + -981.0 * dt) * (1.0 - 0.4 * dt);
        z += v * dt;
    }
    check(s.particles().size() == 1, "the particle is alive after 60 steps");
    if (!s.particles().empty()) {
        checkNear(s.particles()[0].velocity.z, static_cast<f32>(v), 1e-2f, "velocity matches the recurrence");
        checkNear(s.particles()[0].position.z, static_cast<f32>(z), 1e-2f, "position matches the recurrence");
    }
}

static void testShapes() {
    AVER_INFO("--- shapes ---");
    auto spawnMany = [](ParticleEffect fx, u32 n) {
        EmitterSim s;
        s.init(fx, {}, 99);
        SpawnRequest r; r.count = n;
        s.spawn(r);
        return s.particles();
    };
    ParticleEffect fx = baseEffect();
    fx.maxParticles = 5000;
    {
        fx.shape = EmitterShape::Point;
        bool ok = true;
        for (const Particle& p : spawnMany(fx, 100)) ok = ok && p.position.sizeSquared() == 0.0f;
        check(ok, "point: every particle at the origin");
    }
    {
        fx.shape = EmitterShape::Sphere; fx.shapeSize = {30, 0, 0};
        f32 maxR = 0; Vec3 mean{};
        const auto ps = spawnMany(fx, 4000);
        for (const Particle& p : ps) { maxR = std::fmax(maxR, p.position.size()); mean += p.position; }
        check(maxR <= 30.0001f, "sphere: inside the radius");
        check(maxR > 28.0f, "sphere: reaches near the radius");
        check((mean * (1.0f / 4000.0f)).size() < 2.0f, "sphere: centred on the origin");
        fx.shapeShell = true;
        bool onSurface = true;
        for (const Particle& p : spawnMany(fx, 200)) onSurface = onSurface && std::fabs(p.position.size() - 30.0f) < 1e-3f;
        check(onSurface, "sphere shell: on the surface");
        fx.shapeShell = false;
    }
    {
        fx.shape = EmitterShape::Box; fx.shapeSize = {10, 20, 0};
        bool ok = true;
        for (const Particle& p : spawnMany(fx, 500)) ok = ok && std::fabs(p.position.x) <= 10.0f && std::fabs(p.position.y) <= 20.0f && p.position.z == 0.0f;
        check(ok, "box: inside the half-extents, a zero extent stays flat");
    }
    {
        fx.shape = EmitterShape::Cylinder; fx.shapeSize = {15, 40, 0};
        bool ok = true; f32 maxZ = 0;
        for (const Particle& p : spawnMany(fx, 1000)) {
            ok = ok && std::sqrt(p.position.x * p.position.x + p.position.y * p.position.y) <= 15.0001f && std::fabs(p.position.z) <= 20.0001f;
            maxZ = std::fmax(maxZ, std::fabs(p.position.z));
        }
        check(ok && maxZ > 18.0f, "cylinder: radius r, height h centred on the origin");
        fx.shapeShell = true;
        bool side = true;
        for (const Particle& p : spawnMany(fx, 200)) side = side && std::fabs(std::sqrt(p.position.x * p.position.x + p.position.y * p.position.y) - 15.0f) < 1e-3f;
        check(side, "cylinder shell: on the side surface");
        fx.shapeShell = false;
    }
    {
        fx.shape = EmitterShape::Disc; fx.shapeSize = {25, 0, 0};
        bool ok = true;
        for (const Particle& p : spawnMany(fx, 500)) ok = ok && p.position.z == 0.0f && p.position.size() <= 25.0001f;
        check(ok, "disc: flat, inside the radius");
    }
    {   // direction, scales and the transform
        fx.shape = EmitterShape::Point;
        fx.direction = {0, 0, 1};
        fx.speedMin = fx.speedMax = 100.0f;
        fx.spreadDeg = 0.0f;
        EmitterSim s;
        s.init(fx, {}, 4);
        SpawnRequest r;
        r.xf = Mat4::translation({5, 6, 7});
        r.count = 1;
        r.speedScale = 2.0f; r.sizeScale = 3.0f; r.lifeScale = 0.5f; r.tint = 0x11223344u;
        r.addVelocity = {1, 2, 3};
        s.spawn(r);
        const Particle& p = s.particles()[0];
        checkNear(p.position.x, 5.0f, 1e-5f, "the request transform places the particle");
        checkNear(p.velocity.z, 203.0f, 1e-3f, "speed * speedScale along the authored direction, plus addVelocity");
        checkNear(p.velocity.x, 1.0f, 1e-4f, "addVelocity x");
        checkNear(p.lifetime, 5.0f, 1e-5f, "lifeScale");
        checkNear(p.sizeScale, 3.0f, 1e-6f, "sizeScale");
        check(p.tint == 0x11223344u, "tint");
        // A 90 degree yaw maps the authored +X direction onto +Y.
        fx.direction = {1, 0, 0};
        EmitterSim s2;
        s2.init(fx, {}, 4);
        SpawnRequest r2;
        r2.xf.m[0][0] = 0; r2.xf.m[0][1] = 1; r2.xf.m[1][0] = -1; r2.xf.m[1][1] = 0;
        s2.spawn(r2);
        checkNear(s2.particles()[0].velocity.y, 100.0f, 1e-3f, "the transform rotates the authored direction");
    }
    {   // cone
        fx.direction = {0, 0, 1};
        fx.spreadDeg = 30.0f;
        fx.speedMin = fx.speedMax = 1.0f;
        f32 minCos = 1;
        for (const Particle& p : spawnMany(fx, 2000)) minCos = std::fmin(minCos, p.velocity.z / p.velocity.size());
        check(minCos >= std::cos(30.0f * kDegToRad) - 1e-4f, "cone: stays within the half-angle");
        check(minCos < std::cos(28.0f * kDegToRad), "cone: fills the half-angle");
        fx.spreadDeg = 180.0f;
        f32 lowZ = 1;
        for (const Particle& p : spawnMany(fx, 2000)) lowZ = std::fmin(lowZ, p.velocity.z / p.velocity.size());
        check(lowZ < -0.9f, "spread 180 reaches the far hemisphere");
    }
}

static void testLinkedRibbon() {
    AVER_INFO("--- linked ribbon ring ---");
    ParticleEffect fx = baseEffect();
    fx.render = RenderKind::Ribbon;
    fx.linked = true;
    fx.linkedCapacity = 4;
    fx.lifetimeMin = fx.lifetimeMax = 1.0f;
    fx.speedMin = fx.speedMax = 0.0f;
    EmitterSim s;
    s.init(fx, {}, 8);
    check(s.linked() && s.capacity() == 4 && s.particles().size() == 4, "a linked ring is a fixed slot array");
    check(s.aliveCount() == 0, "empty at first");
    SpawnRequest r; r.count = 1;
    for (int i = 0; i < 3; ++i) s.spawn(r);
    check(s.aliveCount() == 3, "three spawns, three live slots");
    check(s.particles()[0].id == 0 && s.particles()[1].id == 1 && s.particles()[2].id == 2, "slot = id mod capacity");
    check(!particleAlive(s.particles()[3]), "the fourth slot is still dead");
    s.spawn(r); s.spawn(r);   // ids 3 and 4: slot 3, then slot 0 again
    check(s.aliveCount() == 4 && s.particles()[0].id == 4 && s.particles()[3].id == 3, "the ring wraps and overwrites the oldest");
    s.step(0.6f, 0.6f, nullptr);
    s.step(0.6f, 1.2f, nullptr);   // every particle ages 1.2 > lifetime 1
    check(s.aliveCount() == 0 && s.particles().size() == 4, "everything dies; the ring never compacts");
}

static void testTrailRing() {
    AVER_INFO("--- trail ring ---");
    ParticleEffect fx = baseEffect();
    fx.render = RenderKind::Ribbon;
    fx.trailPoints = 5;            // ring stores 4
    fx.trailSegmentCm = 9.9f;
    fx.speedMin = fx.speedMax = 100.0f;
    fx.direction = {1, 0, 0};
    EmitterSim s;
    s.init(fx, {}, 2);
    check(s.trailRing() == 4, "the ring stores trailPoints - 1 points");
    SpawnRequest r; r.count = 1;
    s.spawn(r);
    check(s.trail().size() == 4, "one particle owns one ring");
    check((s.particles()[0].trail & 0xFF) == 0 && ((s.particles()[0].trail >> 8) & 0xFF) == 1, "spawn writes point 0");
    // 100 cm/s with a 9.9 cm segment: dt 0.05 moves 5 cm per step, a point every second step.
    for (int i = 0; i < 4; ++i) s.step(0.05f, 0.05f * static_cast<f32>(i), nullptr);
    u32 count = (s.particles()[0].trail >> 8) & 0xFF;
    check(count == 3, "after 20 cm of travel: the spawn point plus two pushed");
    for (int i = 0; i < 40; ++i) s.step(0.05f, 0.05f * static_cast<f32>(i), nullptr);
    count = (s.particles()[0].trail >> 8) & 0xFF;
    check(count == 4, "the count saturates at the ring size");
    const u32 head = s.particles()[0].trail & 0xFF;
    const Vec4 newest = s.trail()[head];
    check(std::fabs(newest.x - s.particles()[0].position.x) <= 10.0001f, "the newest point trails the particle by at most one segment");
    // Compaction keeps each ring with its particle.
    EmitterSim t;
    ParticleEffect fx2 = fx;
    fx2.lifetimeMin = 0.3f; fx2.lifetimeMax = 5.0f;
    t.init(fx2, {}, 6);
    SpawnRequest r5; r5.count = 6;
    t.spawn(r5);
    for (int i = 0; i < 30; ++i) t.step(0.05f, 0.05f * static_cast<f32>(i), nullptr);
    bool ok = !t.particles().empty() && t.trail().size() == t.particles().size() * t.trailRing();
    for (usize i = 0; i < t.particles().size() && ok; ++i) {
        const Particle& p = t.particles()[i];
        const Vec4 n = t.trail()[i * t.trailRing() + (p.trail & 0xFF)];
        ok = ok && std::fabs(n.x - p.position.x) <= 10.0001f;
    }
    check(ok, "after deaths and compaction every ring still trails its own particle");
}

static void testEvents() {
    AVER_INFO("--- events ---");
    ParticleEffect fx = baseEffect();
    fx.lifetimeMin = fx.lifetimeMax = 0.2f;
    fx.speedMin = fx.speedMax = 0.0f;
    EventSpawn death; death.on = 0; death.emitter = 1; death.count = 2;
    fx.events.push_back(death);
    EmitterSim s;
    s.init(fx, {}, 1);
    SpawnRequest r; r.count = 5;
    s.spawn(r);
    std::vector<EventRecord> ev;
    s.step(0.1f, 0.1f, &ev);
    check(ev.empty(), "nothing has died yet");
    s.step(0.15f, 0.25f, &ev);
    check(ev.size() == 5, "five particles died: five death events");
    std::set<u32> ids;
    for (const EventRecord& e : ev) { ids.insert(e.id); check(e.type == EventType::Death, "typed as death"); }
    check(ids.size() == 5, "once per particle");
    ev.clear();
    s.step(0.1f, 0.35f, &ev);
    check(ev.empty(), "and not again");

    // Collide: once per particle however many bounces.
    ParticleEffect fx2 = baseEffect();
    fx2.speedMin = fx2.speedMax = 0.0f;
    fx2.gravity = {0, 0, -981};
    Module plane = makeDefaultModule(ModuleKind::UpdateCollidePlane);
    plane.p[3].value = -50.0f;   // a floor 50 cm below
    fx2.modules.push_back(plane);
    EventSpawn hit; hit.on = 1; hit.emitter = 1; hit.count = 1;
    fx2.events.push_back(hit);
    EmitterSim c;
    c.init(fx2, {}, 2);
    SpawnRequest r2; r2.count = 3;
    c.spawn(r2);
    u32 collides = 0;
    for (int i = 0; i < 120; ++i) {
        std::vector<EventRecord> e2;
        c.step(0.016f, 0.016f * static_cast<f32>(i), &e2);
        for (const EventRecord& x : e2) collides += x.type == EventType::Collide ? 1u : 0u;
    }
    check(collides == 3, "three particles bounce repeatedly: exactly three collide events");

    // No rules: no events are recorded at all.
    EmitterSim q;
    ParticleEffect none = baseEffect();
    none.lifetimeMin = none.lifetimeMax = 0.1f;
    q.init(none, {}, 3);
    q.spawn(r);
    std::vector<EventRecord> e3;
    q.step(0.2f, 0.2f, &e3);
    check(e3.empty(), "an effect with no ONEVENT rules records none");
}

static void testUserParamBinding() {
    AVER_INFO("--- user parameter binding ---");
    ParticleEffect fx = baseEffect();
    fx.speedMin = fx.speedMax = 0.0f;
    Module f = makeDefaultModule(ModuleKind::UpdateForce);
    f.p[0].userSlot = 0; f.p[0].comp = 0;
    fx.modules.push_back(f);
    UserParam up; up.name = "Wind"; up.kind = 0; up.v[0] = 100.0f;
    EmitterSim s;
    s.init(fx, {up}, 1);
    SpawnRequest r; r.count = 1;
    s.spawn(r);
    s.step(0.1f, 0.1f, nullptr);
    checkNear(s.particles()[0].velocity.x, 10.0f, 1e-4f, "the parameter's default drives the module");
    const f32 v[4] = {500.0f, 0, 0, 0};
    s.setUser(0, v);
    s.step(0.1f, 0.2f, nullptr);
    checkNear(s.particles()[0].velocity.x, 60.0f, 1e-3f, "a changed parameter changes the next step");
}

static void testSpawnModulesApplied() {
    AVER_INFO("--- spawn modules run at spawn ---");
    ParticleEffect fx = baseEffect();
    Module sz = makeDefaultModule(ModuleKind::SpawnSizeScale);
    sz.p[0].value = 2.0f; sz.p[1].value = 2.0f;
    fx.modules.push_back(sz);
    Module inh = makeDefaultModule(ModuleKind::SpawnInheritVelocity);
    inh.p[0].value = 0.5f;
    fx.modules.push_back(inh);
    EmitterSim s;
    s.init(fx, {}, 1);
    SpawnRequest r; r.count = 1; r.velocity = {100, 0, 0}; r.sizeScale = 3.0f;
    s.spawn(r);
    checkNear(s.particles()[0].sizeScale, 6.0f, 1e-5f, "sizescale multiplies the request's size scale");
    checkNear(s.particles()[0].velocity.x, 50.0f, 1e-4f, "inheritvelocity adds the emitter velocity * scale");
    // Spawn modules do not run during step.
    s.step(0.1f, 0.1f, nullptr);
    checkNear(s.particles()[0].sizeScale, 6.0f, 1e-5f, "spawn modules do not rerun");
}

int main() {
    AVER_INFO("EmitterSimTest");
    testDeterminism();
    testBudgetAndIds();
    testClosedForm();
    testShapes();
    testLinkedRibbon();
    testTrailRing();
    testEvents();
    testUserParamBinding();
    testSpawnModulesApplied();
    AVER_INFO("EmitterSimTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
