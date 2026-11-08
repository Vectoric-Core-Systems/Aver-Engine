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

static void testEventTriggers() {
    AVER_INFO("--- spawn / lifetime / timer triggers and payloads ---");
    ParticleEffect fx = baseEffect();
    fx.lifetimeMin = fx.lifetimeMax = 1.0f;
    fx.speedMin = fx.speedMax = 0.0f;
    fx.sizeStart = 10.0f; fx.sizeEnd = 20.0f;
    EventSpawn born; born.on = 2; born.emitter = 1; born.count = 1; born.payload = kEventPayloadAll;
    EventSpawn half; half.on = 3; half.emitter = 1; half.count = 1; half.at = 0.5f;
    EventSpawn tick; tick.on = 4; tick.emitter = 1; tick.count = 1; tick.at = 0.25f;
    fx.events = {born, half, tick};
    EmitterSim s;
    s.init(fx, {}, 1);
    SpawnRequest r; r.count = 2; r.tint = 0xFF0000FFu;
    s.spawn(r);
    std::vector<EventRecord> ev;
    s.step(0.1f, 0.1f, &ev);
    u32 births = 0, halves = 0, timers = 0;
    for (const EventRecord& e : ev) { births += e.type == EventOn::Spawn; halves += e.type == EventOn::Lifetime; timers += e.type == EventOn::Timer; }
    check(births == 2 && halves == 0 && timers == 0, "the first step reports the two births and nothing else");
    check(ev[0].rule == 0 && ev[0].tint != 0, "a birth record carries the rule index and a colour");
    ev.clear();
    s.step(0.2f, 0.3f, &ev);   // age 0.3: the 0.25 s timer fired once per particle
    timers = 0; for (const EventRecord& e : ev) timers += e.type == EventOn::Timer && e.rule == 2;
    check(timers == 2, "the timer fires once per particle when its age passes 0.25 s");
    ev.clear();
    s.step(0.25f, 0.55f, &ev);   // age 0.55: passes the 0.5 life fraction and the 0.5 s timer mark
    halves = timers = 0;
    for (const EventRecord& e : ev) { halves += e.type == EventOn::Lifetime && e.rule == 1; timers += e.type == EventOn::Timer; if (e.type == EventOn::Lifetime) checkNear(e.ageFraction, 0.55f, 0.01f, "the record holds the life fraction at the event"); }
    check(halves == 2, "the lifetime-fraction trigger fires once per particle at half its life");
    check(timers == 2, "and the timer fires again at 0.5 s");
    ev.clear();
    s.step(0.1f, 0.65f, &ev);
    halves = 0; for (const EventRecord& e : ev) halves += e.type == EventOn::Lifetime;
    check(halves == 0, "the fraction trigger does not fire twice");
    ev.clear();
    s.step(0.5f, 1.15f, &ev);   // dies: no timer or fraction record for a particle that is gone
    timers = 0; for (const EventRecord& e : ev) timers += e.type != EventOn::Death && e.type != EventOn::Timer ? 1u : 0u;
    check(timers == 0, "nothing but timers/deaths from the last step");

    // Payload: colour is the curve times tint, size the ratio to the birth size, age the life fraction; the user scalar scales speed.
    SpawnRequest cr;
    EventSpawn rule; rule.payload = kEventPayloadAll; rule.user = 3.0f;
    EventRecord rec; rec.tint = 0x11223344u; rec.sizeRatio = 1.5f; rec.ageFraction = 0.4f;
    applyEventPayload(rule, rec, cr);
    check(cr.tint == 0x11223344u && cr.sizeScale == 1.5f && cr.ageFraction == 0.4f && cr.speedScale == 3.0f, "applyEventPayload copies each requested field");
    SpawnRequest plain;
    applyEventPayload(EventSpawn{}, rec, plain);
    check(plain.tint == 0xFFFFFFFFu && plain.sizeScale == 1.0f && plain.ageFraction == 0.0f && plain.speedScale == 1.0f, "an empty payload changes nothing");

    // A child born with the age payload starts partway through its life.
    ParticleEffect child = baseEffect();
    child.lifetimeMin = child.lifetimeMax = 2.0f;
    EmitterSim cs;
    cs.init(child, {}, 5);
    SpawnRequest aged; aged.ageFraction = 0.5f;
    cs.spawn(aged);
    checkNear(cs.particles()[0].age, 1.0f, 1e-4f, "the age payload starts the child half-way through its life");

    // A timer faster than the step fires several times; death records still arrive for a rule-less trigger set.
    ParticleEffect fast = baseEffect();
    fast.lifetimeMin = fast.lifetimeMax = 5.0f;
    EventSpawn t2; t2.on = 4; t2.emitter = 1; t2.at = 0.1f;
    fast.events = {t2};
    EmitterSim fs;
    fs.init(fast, {}, 6);
    SpawnRequest one; one.count = 1;
    fs.spawn(one);
    std::vector<EventRecord> fe;
    fs.step(0.35f, 0.35f, &fe);
    check(fe.size() == 3, "a 0.35 s step crosses three 0.1 s timer marks");

    // Trigger and payload rules are the extended spelling; the GPU tier takes them (ParticleGpuEventTest holds the channel rules).
    EventSpawn d0; d0.on = 0; EventSpawn pay; pay.on = 1; pay.payload = kEventPayloadColor;
    check(!eventIsExtended(d0) && eventIsExtended(born) && eventIsExtended(half) && eventIsExtended(tick) && eventIsExtended(pay), "trigger and payload rules are the extended spelling");
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

// Wave 6 (P15): the ring shape and the modules that act over a run (orbit, box collision, accelerations) through the executor, not one call at a time.
static void testWave6Shapes() {
    AVER_INFO("--- ring shape and the wave 6 modules in a run ---");
    ParticleEffect fx = baseEffect();
    fx.shape = EmitterShape::Ring;
    fx.shapeSize = {100, 20, 40};   // outer radius, height, inner radius
    fx.speedMin = fx.speedMax = 0;
    {
        EmitterSim s;
        s.init(fx, {}, 5);
        SpawnRequest r; r.count = 4000;
        s.spawn(r);
        bool inBand = true;
        f64 r2 = 0;
        f32 zLo = 1e9f, zHi = -1e9f;
        for (const Particle& p : s.particles()) {
            const f32 rr = std::sqrt(p.position.x * p.position.x + p.position.y * p.position.y);
            inBand = inBand && rr >= 40.0f - 1e-3f && rr <= 100.0f + 1e-3f;
            r2 += static_cast<f64>(rr) * rr;
            zLo = std::fmin(zLo, p.position.z); zHi = std::fmax(zHi, p.position.z);
        }
        check(inBand, "ring: every particle is between the inner and the outer radius");
        checkNear(static_cast<f32>(r2 / s.particles().size()), 5800.0f, 250.0f, "ring: uniform over the area (mean r^2 = (inner^2 + outer^2) / 2)");
        check(zLo >= -10.001f && zHi <= 10.001f && zHi - zLo > 15.0f, "ring: spread over the height about the axis");
    }
    {
        ParticleEffect shell = fx;
        shell.shapeShell = true;
        EmitterSim s;
        s.init(shell, {}, 5);
        SpawnRequest r; r.count = 500;
        s.spawn(r);
        bool onRim = true;
        for (const Particle& p : s.particles()) onRim = onRim && std::fabs(std::sqrt(p.position.x * p.position.x + p.position.y * p.position.y) - 100.0f) < 1e-2f;
        check(onRim, "ring + shell: on the outer rim");
    }
    {
        ParticleEffect tilted = fx;
        tilted.shapeAxis = {1, 0, 0};
        EmitterSim s;
        s.init(tilted, {}, 5);
        SpawnRequest r; r.count = 1000;
        s.spawn(r);
        bool ok = true;
        for (const Particle& p : s.particles()) {
            const f32 rr = std::sqrt(p.position.y * p.position.y + p.position.z * p.position.z);
            ok = ok && std::fabs(p.position.x) <= 10.001f && rr >= 40.0f - 1e-3f && rr <= 100.0f + 1e-3f;
        }
        check(ok, "ring with axis +X: the ring lies in the YZ plane and the height runs along X");
        ParticleEffect zero = fx;
        zero.shapeAxis = {0, 0, 0};
        EmitterSim a, b;
        a.init(zero, {}, 5); b.init(fx, {}, 5);
        SpawnRequest rr; rr.count = 50;
        a.spawn(rr); b.spawn(rr);
        bool same = a.particles().size() == b.particles().size();
        for (usize i = 0; same && i < a.particles().size(); ++i)
            same = a.particles()[i].position.x == b.particles()[i].position.x && a.particles()[i].position.z == b.particles()[i].position.z;
        check(same, "ring with a zero axis reads as +Z");
        EmitterSim c;
        c.init(fx, {}, 5);
        EmitterSim d;
        d.init(fx, {}, 5);
        c.spawn(rr); d.spawn(rr);
        same = true;
        for (usize i = 0; i < c.particles().size(); ++i) same = same && std::memcmp(&c.particles()[i], &d.particles()[i], sizeof(Particle)) == 0;
        check(same, "ring: the same seed gives the same particles");
    }
    {   // a swirl: orbit about +Z turns the cloud, radial accel pushes it out
        ParticleEffect o = baseEffect();
        o.shape = EmitterShape::Disc; o.shapeSize = {50, 0, 0};
        o.speedMin = o.speedMax = 0;
        Module orbit = makeDefaultModule(ModuleKind::UpdateOrbit);
        orbit.p[0].value = 0.25f; orbit.p[1].value = 0.25f;   // a quarter turn a second
        o.modules.push_back(orbit);
        EmitterSim s;
        s.init(o, {}, 9);
        s.setOrigin({0, 0, 0});
        SpawnRequest r; r.count = 100;
        s.spawn(r);
        const std::vector<Particle> before = s.particles();
        for (int i = 0; i < 60; ++i) s.step(1.0f / 60.0f, i / 60.0f, nullptr);
        bool turned = true;
        for (usize i = 0; i < before.size(); ++i) {
            const Particle& a = before[i];
            const Particle& b = s.particles()[i];
            // (x, y) -> (-y, x) after a quarter turn; the radius is kept
            turned = turned && std::fabs(b.position.x + a.position.y) < 0.2f && std::fabs(b.position.y - a.position.x) < 0.2f;
        }
        check(turned, "orbit: one second at a quarter turn per second turns every particle by 90 degrees about +Z");
    }
    {   // box collision: a fountain landing on a box top stays above it
        ParticleEffect f = baseEffect();
        f.speedMin = f.speedMax = 200;
        f.direction = {0, 0, 1};
        f.spreadDeg = 20;
        f.gravity = {0, 0, -600};
        f.lifetimeMin = f.lifetimeMax = 5;
        Module box = makeDefaultModule(ModuleKind::UpdateCollideBox);
        box.p[0].value = 0; box.p[1].value = 0; box.p[2].value = -50;   // centre
        box.p[3].value = 400; box.p[4].value = 400; box.p[5].value = 50;   // half extents: the top face is z = 0
        f.modules.push_back(box);
        EmitterSim s;
        s.init(f, {}, 3);
        SpawnRequest r; r.count = 200;
        s.spawn(r);
        f32 lowest = 1e9f;
        for (int i = 0; i < 240; ++i) {
            s.step(1.0f / 120.0f, i / 120.0f, nullptr);
            for (const Particle& p : s.particles()) lowest = std::fmin(lowest, p.position.z);
        }
        check(lowest >= -1.0f, "box: particles falling onto a solid box do not sink into it");
    }
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
    testEventTriggers();
    testUserParamBinding();
    testSpawnModulesApplied();
    testWave6Shapes();
    AVER_INFO("EmitterSimTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
