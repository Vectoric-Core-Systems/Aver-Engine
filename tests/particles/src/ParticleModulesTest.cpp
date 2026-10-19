// The module stack and the RNG it draws from, with no GPU and no scene: golden random values, every module kind against
// hand-computed numbers, user-parameter binding, the module table itself and the curve evaluators. Exit code = failure
// count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleModules.hpp"
#include "aver/particles/ParticleRandom.hpp"

#include <cmath>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>

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

// A resolved module with named parameters overridden.
static ResolvedModule mod(ModuleKind kind, std::initializer_list<std::pair<const char*, f32>> params = {}) {
    Module m = makeDefaultModule(kind);
    const ModuleInfo& info = moduleInfo(kind);
    for (const auto& kv : params) {
        const int i = findModuleParam(info, kv.first);
        check(i >= 0, std::string("module ") + info.key + " has a parameter named " + kv.first);
        if (i >= 0) m.p[i].value = kv.second;
    }
    std::vector<ResolvedModule> out;
    resolveModules({m}, nullptr, out);
    return out.empty() ? ResolvedModule{} : out[0];
}

static Particle particleAt(Vec3 pos, Vec3 vel, u32 seed = 12345u) {
    Particle p{};
    p.position = pos;
    p.velocity = vel;
    p.lifetime = 10.0f;
    p.seed = seed;
    p.sizeScale = 1.0f;
    p.tint = 0xFFFFFFFFu;
    return p;
}

static void testRandom() {
    AVER_INFO("--- counter-based RNG ---");
    for (u32 i = 0; i < 32; ++i) {
        const RandGolden& g = kRandGolden[i];
        const f32 want = static_cast<f32>(g.bits24) * (1.0f / 16777216.0f);
        check(rand01(g.seed, g.stream) == want, "golden triple " + std::to_string(i) + " (seed " + std::to_string(g.seed) + ", stream " + std::to_string(g.stream) + ")");
    }
    // Order independence: a draw depends on (seed, stream) only.
    const f32 a = rand01(77, 3), b = rand01(77, 4);
    check(rand01(77, 4) == b && rand01(77, 3) == a, "draws do not depend on call order");
    check(rand01(77, 3) != rand01(77, 4), "different streams differ");
    // Range and mean over 10,000 draws.
    f64 sum = 0;
    f32 lo = 1, hi = 0;
    for (u32 i = 0; i < 10000; ++i) {
        const f32 v = rand01(particleSeed(1, i), kStreamSpeed);
        sum += v; lo = std::fmin(lo, v); hi = std::fmax(hi, v);
    }
    check(lo >= 0.0f && hi < 1.0f, "rand01 stays in [0,1)");
    checkNear(static_cast<f32>(sum / 10000.0), 0.5f, 0.02f, "rand01 mean is about 0.5");
    check(particleSeed(1, 0) != particleSeed(1, 1) && particleSeed(1, 0) != particleSeed(2, 0), "particle seeds differ by id and by emitter seed");
    check(particleSeed(9, 3) == particleSeed(9, 3), "particle seeds are pure");
}

static void testNoise() {
    AVER_INFO("--- lattice gradient noise ---");
    f32 lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 2000; ++i) {
        const f32 x = 0.137f * static_cast<f32>(i), y = 0.071f * static_cast<f32>(i), z = 0.029f * static_cast<f32>(i);
        const f32 v = gradNoise3(x, y, z, 5);
        lo = std::fmin(lo, v); hi = std::fmax(hi, v);
        check(std::isfinite(v), "noise is finite");
    }
    check(lo >= -1.5f && hi <= 1.5f && hi - lo > 0.5f, "noise spans a reasonable range");
    checkNear(gradNoise3(3.0f, -2.0f, 7.0f, 9), 0.0f, 1e-5f, "gradient noise is zero at lattice points");
    checkNear(gradNoise3(0.3f, 0.4f, 0.5f, 1), gradNoise3(0.3f, 0.4f, 0.5f, 1), 0.0f, "noise is deterministic");
    check(gradNoise3(0.3f, 0.4f, 0.5f, 1) != gradNoise3(0.3f, 0.4f, 0.5f, 2), "the seed changes the field");
    // Continuity: nearby points differ by a small amount.
    const f32 d = std::fabs(gradNoise3(1.5f, 2.5f, 3.5f, 4) - gradNoise3(1.5001f, 2.5f, 3.5f, 4));
    check(d < 0.01f, "noise is continuous");
}

static void testTable() {
    AVER_INFO("--- the module table ---");
    check(static_cast<u32>(ModuleKind::Count) == 13, "13 module kinds");
    std::set<std::string> keys;
    for (u32 k = 0; k < 13; ++k) {
        const ModuleInfo& info = moduleInfo(static_cast<ModuleKind>(k));
        check(info.kind == static_cast<ModuleKind>(k), std::string("table row ") + std::to_string(k) + " is in enum order");
        check(keys.insert(info.key).second, std::string("unique key ") + info.key);
        check(findModuleInfo(info.key) == &info, std::string("findModuleInfo(") + info.key + ")");
        check(info.paramCount >= 1 && info.paramCount <= kMaxModuleParams, std::string(info.key) + " parameter count in range");
        for (u32 i = 0; i < info.paramCount; ++i) check(findModuleParam(info, info.names[i]) == static_cast<int>(i), std::string(info.key) + " parameter " + info.names[i]);
    }
    check(findModuleInfo("UPDATE.FORCE") != nullptr, "keys match case-insensitively");
    check(findModuleInfo("update.nothing") == nullptr, "an unknown key is null");
    check(moduleInfo(ModuleKind::SpawnSizeScale).stage == ModuleStage::Spawn, "spawn stage");
    check(moduleInfo(ModuleKind::UpdateForce).stage == ModuleStage::Velocity, "force is a velocity-phase module");
    check(moduleInfo(ModuleKind::UpdateCollidePlane).stage == ModuleStage::Position, "collideplane is a position-phase module");
}

static void testDefaultsAreNoOps() {
    AVER_INFO("--- an unconfigured module changes nothing (except the two that default to a world feature) ---");
    ModuleEnv env;
    env.dt = 0.1f; env.time = 1.0f;
    for (u32 k = 0; k < 13; ++k) {
        const ModuleKind kind = static_cast<ModuleKind>(k);
        // curlnoise defaults to strength 100 and collideplane to the z = 0 plane: authored on purpose, see the README.
        if (kind == ModuleKind::UpdateCurlNoise || kind == ModuleKind::UpdateCollidePlane) continue;
        ResolvedModule m = mod(kind);
        Particle p = particleAt({10, -20, 30}, {5, 6, -7});
        const Particle before = p;
        if (m.stage == ModuleStage::Spawn) runSpawnModule(m, p, {100, 100, 100});
        else if (m.stage == ModuleStage::Velocity) runVelocityModule(m, env, p);
        else runPositionModule(m, env, p);
        check(p.position.x == before.position.x && p.position.y == before.position.y && p.position.z == before.position.z &&
              p.velocity.x == before.velocity.x && p.velocity.y == before.velocity.y && p.velocity.z == before.velocity.z &&
              p.sizeScale == before.sizeScale && p.tint == before.tint && p.rotation == before.rotation && p.age == before.age,
              std::string(moduleInfo(kind).key) + " at its defaults is a no-op");
    }
}

static void testVelocityModules() {
    AVER_INFO("--- velocity-phase modules ---");
    ModuleEnv env;
    env.dt = 0.5f;
    {   // force
        Particle p = particleAt({}, {1, 2, 3});
        runVelocityModule(mod(ModuleKind::UpdateForce, {{"x", 10}, {"y", -4}, {"z", 2}}), env, p);
        checkNear(p.velocity.x, 6.0f, 1e-5f, "force x: v += a dt");
        checkNear(p.velocity.y, 0.0f, 1e-5f, "force y");
        checkNear(p.velocity.z, 4.0f, 1e-5f, "force z");
    }
    {   // drag
        Particle p = particleAt({}, {100, 0, 0});
        runVelocityModule(mod(ModuleKind::UpdateDrag, {{"k", 0.001f}}), env, p);
        checkNear(p.velocity.x, 100.0f * (1.0f - 100.0f * 0.001f * 0.5f), 1e-3f, "drag: v -= v |v| k dt");
        Particle q = particleAt({}, {100, 50, 0});
        runVelocityModule(mod(ModuleKind::UpdateDrag, {{"k", 100.0f}}), env, q);
        check(q.velocity.x >= 0.0f && q.velocity.y >= 0.0f, "drag never reverses a velocity, however large k is");
        checkNear(q.velocity.size(), 0.0f, 1e-4f, "...it stops it");
    }
    {   // attractor
        Particle p = particleAt({0, 0, 0}, {0, 0, 0});
        runVelocityModule(mod(ModuleKind::UpdateAttractor, {{"x", 100}, {"strength", 20}}), env, p);
        checkNear(p.velocity.x, 10.0f, 1e-4f, "attractor pulls toward the point (no radius: no falloff)");
        checkNear(p.velocity.y, 0.0f, 1e-4f, "attractor along the line only");
        Particle far = particleAt({0, 0, 0}, {});
        runVelocityModule(mod(ModuleKind::UpdateAttractor, {{"x", 100}, {"strength", 20}, {"radius", 50}}), env, far);
        checkNear(far.velocity.x, 0.0f, 1e-6f, "beyond the radius the pull is zero");
        Particle near1 = particleAt({0, 0, 0}, {});
        runVelocityModule(mod(ModuleKind::UpdateAttractor, {{"x", 50}, {"strength", 20}, {"radius", 100}, {"falloff", 1}}), env, near1);
        checkNear(near1.velocity.x, 20.0f * 0.5f * 0.5f, 1e-4f, "falloff exponent 1: weight = 1 - d/radius = 0.5");
        Particle push = particleAt({0, 0, 0}, {});
        runVelocityModule(mod(ModuleKind::UpdateAttractor, {{"x", 100}, {"strength", -20}}), env, push);
        check(push.velocity.x < 0.0f, "a negative strength pushes away");
        Particle local = particleAt({1000, 0, 0}, {});
        ModuleEnv e2 = env;
        e2.origin = {1000, 0, 0};
        runVelocityModule(mod(ModuleKind::UpdateAttractor, {{"x", 100}, {"strength", 20}, {"local", 1}}), e2, local);
        checkNear(local.velocity.x, 10.0f, 1e-4f, "a local attractor is emitter-relative");
    }
    {   // vortex
        Particle p = particleAt({100, 0, 0}, {});
        runVelocityModule(mod(ModuleKind::UpdateVortex, {{"strength", 40}, {"lift", 6}}), env, p);
        check(std::fabs(dot(p.velocity, Vec3{1, 0, 0})) < 1e-4f, "vortex acceleration is perpendicular to the radius");
        checkNear(p.velocity.y, 20.0f, 1e-4f, "vortex tangent: axis x radius direction, strength * dt");
        checkNear(p.velocity.z, 3.0f, 1e-4f, "vortex lift along the axis");
        Particle onAxis = particleAt({0, 0, 50}, {});
        runVelocityModule(mod(ModuleKind::UpdateVortex, {{"strength", 40}}), env, onAxis);
        checkNear(onAxis.velocity.size(), 0.0f, 1e-6f, "a particle on the axis has no tangent");
    }
    {   // speed limit
        Particle p = particleAt({}, {300, 400, 0});
        runVelocityModule(mod(ModuleKind::UpdateSpeedLimit, {{"max", 100}}), env, p);
        checkNear(p.velocity.size(), 100.0f, 1e-3f, "speedlimit max clamps");
        checkNear(p.velocity.x / p.velocity.size(), 0.6f, 1e-4f, "...keeping the direction");
        Particle slow = particleAt({}, {3, 4, 0});
        runVelocityModule(mod(ModuleKind::UpdateSpeedLimit, {{"min", 50}}), env, slow);
        checkNear(slow.velocity.size(), 50.0f, 1e-3f, "speedlimit min raises");
        Particle off = particleAt({}, {300, 400, 0});
        runVelocityModule(mod(ModuleKind::UpdateSpeedLimit, {}), env, off);
        checkNear(off.velocity.size(), 500.0f, 1e-3f, "max 0 is off");
    }
    {   // curl noise: finite, deterministic, divergence free, scrolls with time
        const Vec3 c1 = curlNoiseField({123, -45, 67}, 0.01f, 0.0f, 0.2f, 2, 0);
        const Vec3 c2 = curlNoiseField({123, -45, 67}, 0.01f, 0.0f, 0.2f, 2, 0);
        check(std::isfinite(c1.x) && std::isfinite(c1.y) && std::isfinite(c1.z), "curl noise is finite");
        check(c1.x == c2.x && c1.y == c2.y && c1.z == c2.z, "curl noise is deterministic");
        const Vec3 later = curlNoiseField({123, -45, 67}, 0.01f, 5.0f, 0.2f, 2, 0);
        check(later.x != c1.x || later.y != c1.y || later.z != c1.z, "the field scrolls with time");
        // Divergence of the curl, by central differences in world space (freq 1 keeps noise space == world space).
        f32 worst = 0.0f, mag = 0.0f;
        for (int i = 0; i < 20; ++i) {
            const Vec3 q{0.37f * static_cast<f32>(i) + 0.11f, 0.23f * static_cast<f32>(i) - 1.3f, 0.41f * static_cast<f32>(i) + 0.7f};
            const f32 h = 0.05f;
            const f32 div = (curlNoiseField({q.x + h, q.y, q.z}, 1.0f, 0, 0, 1, 3).x - curlNoiseField({q.x - h, q.y, q.z}, 1.0f, 0, 0, 1, 3).x +
                             curlNoiseField({q.x, q.y + h, q.z}, 1.0f, 0, 0, 1, 3).y - curlNoiseField({q.x, q.y - h, q.z}, 1.0f, 0, 0, 1, 3).y +
                             curlNoiseField({q.x, q.y, q.z + h}, 1.0f, 0, 0, 1, 3).z - curlNoiseField({q.x, q.y, q.z - h}, 1.0f, 0, 0, 1, 3).z) / (2.0f * h);
            worst = std::fmax(worst, std::fabs(div));
            mag = std::fmax(mag, curlNoiseField(q, 1.0f, 0, 0, 1, 3).size());
        }
        check(mag > 0.1f, "the curl field has magnitude");
        check(worst < 0.5f * mag + 0.5f, "the curl field is nearly divergence free");
        Particle p = particleAt({123, -45, 67}, {});
        env.time = 0.0f;
        runVelocityModule(mod(ModuleKind::UpdateCurlNoise, {{"freq", 0.01f}, {"strength", 100}, {"scroll", 0.2f}, {"octaves", 2}}), env, p);
        const Vec3 expect = curlNoiseField({123, -45, 67}, 0.01f, 0.0f, 0.2f, 2, 0) * (100.0f * env.dt);
        checkNear(p.velocity.x, expect.x, 1e-4f, "curlnoise module = strength * curl * dt (x)");
        checkNear(p.velocity.z, expect.z, 1e-4f, "curlnoise module (z)");
    }
}

static void testPositionModules() {
    AVER_INFO("--- position-phase modules ---");
    ModuleEnv env;
    env.dt = 0.016f;
    {   // plane
        Particle p = particleAt({0, 0, -2}, {10, 0, -100});
        const u32 bits = runPositionModule(mod(ModuleKind::UpdateCollidePlane, {{"restitution", 0.5f}, {"friction", 0.25f}}), env, p);
        check((bits & kModuleCollided) != 0 && (bits & kModuleKilled) == 0, "a particle below the plane collides and survives");
        checkNear(p.position.z, 0.0f, 1e-5f, "it is pushed back onto the plane");
        checkNear(p.velocity.z, 50.0f, 1e-4f, "normal velocity reflects scaled by restitution");
        checkNear(p.velocity.x, 7.5f, 1e-4f, "tangential velocity scaled by 1 - friction");
        Particle again = p;
        again.position.z = -1.0f; again.velocity.z = -10.0f;
        const u32 bits2 = runPositionModule(mod(ModuleKind::UpdateCollidePlane, {{"restitution", 0.5f}}), env, again);
        check((bits2 & kModuleCollided) == 0, "the collide event fires once per particle");
        Particle above = particleAt({0, 0, 5}, {0, 0, -1});
        check(runPositionModule(mod(ModuleKind::UpdateCollidePlane), env, above) == 0, "above the plane: nothing");
        Particle moving = particleAt({0, 0, -1}, {0, 0, 20});   // already leaving: pushed out, velocity untouched
        runPositionModule(mod(ModuleKind::UpdateCollidePlane), env, moving);
        checkNear(moving.velocity.z, 20.0f, 1e-5f, "a particle already moving away keeps its velocity");
        Particle kill = particleAt({0, 0, -1}, {0, 0, -5});
        const u32 kb = runPositionModule(mod(ModuleKind::UpdateCollidePlane, {{"kill", 1}}), env, kill);
        check((kb & kModuleKilled) != 0 && !particleAlive(kill), "kill=1 kills on contact");
        Particle tilted = particleAt({0, 0, 0}, {0, 0, -10});
        runPositionModule(mod(ModuleKind::UpdateCollidePlane, {{"nx", 0}, {"ny", 0}, {"nz", 1}, {"d", 5}}), env, tilted);
        checkNear(tilted.position.z, 5.0f, 1e-5f, "plane offset d");
    }
    {   // sphere
        Particle p = particleAt({0, 0, 3}, {0, 0, -10});
        const u32 bits = runPositionModule(mod(ModuleKind::UpdateCollideSphere, {{"r", 10}, {"restitution", 1.0f}, {"friction", 0}}), env, p);
        check((bits & kModuleCollided) != 0, "inside the solid sphere: collides");
        checkNear(p.position.size(), 10.0f, 1e-4f, "pushed out onto the surface");
        checkNear(p.velocity.z, 10.0f, 1e-4f, "reflected");
        Particle outside = particleAt({0, 0, 30}, {0, 0, -10});
        check(runPositionModule(mod(ModuleKind::UpdateCollideSphere, {{"r", 10}}), env, outside) == 0, "outside: nothing");
    }
    {   // kill volume
        Particle in = particleAt({1, 1, 1}, {});
        check((runPositionModule(mod(ModuleKind::UpdateKillVolume, {{"hx", 5}, {"hy", 5}, {"hz", 5}}), env, in) & kModuleKilled) != 0, "inside a box: killed");
        Particle out = particleAt({9, 1, 1}, {});
        check(runPositionModule(mod(ModuleKind::UpdateKillVolume, {{"hx", 5}, {"hy", 5}, {"hz", 5}}), env, out) == 0, "outside a box: alive");
        Particle inv = particleAt({9, 1, 1}, {});
        check((runPositionModule(mod(ModuleKind::UpdateKillVolume, {{"hx", 5}, {"hy", 5}, {"hz", 5}, {"invert", 1}}), env, inv) & kModuleKilled) != 0, "invert: killed outside");
        Particle sph = particleAt({3, 4, 0}, {});
        check((runPositionModule(mod(ModuleKind::UpdateKillVolume, {{"shape", 1}, {"hx", 5}}), env, sph) & kModuleKilled) != 0, "on the boundary of a sphere of radius 5: killed");
        Particle sph2 = particleAt({3, 4, 1}, {});
        check(runPositionModule(mod(ModuleKind::UpdateKillVolume, {{"shape", 1}, {"hx", 5}}), env, sph2) == 0, "just outside a sphere of radius 5: alive");
    }
}

static void testSpawnModules() {
    AVER_INFO("--- spawn modules over 10,000 draws ---");
    f32 sLo = 10, sHi = -10;
    f64 sSum = 0;
    f32 rLo = 1e9f, rHi = -1e9f, wLo = 1e9f, wHi = -1e9f;
    bool tintOk = true;
    for (u32 i = 0; i < 10000; ++i) {
        Particle p = particleAt({}, {});
        p.seed = particleSeed(42, i);
        runSpawnModule(mod(ModuleKind::SpawnSizeScale, {{"min", 0.6f}, {"max", 1.4f}}), p, {});
        sLo = std::fmin(sLo, p.sizeScale); sHi = std::fmax(sHi, p.sizeScale); sSum += p.sizeScale;
        Particle q = particleAt({}, {});
        q.seed = p.seed;
        runSpawnModule(mod(ModuleKind::SpawnRotation, {{"minDeg", 10}, {"maxDeg", 50}, {"spinMin", -90}, {"spinMax", 90}}), q, {});
        rLo = std::fmin(rLo, q.rotation); rHi = std::fmax(rHi, q.rotation);
        wLo = std::fmin(wLo, q.angularVelocity); wHi = std::fmax(wHi, q.angularVelocity);
        Particle t = particleAt({}, {});
        t.seed = p.seed;
        runSpawnModule(mod(ModuleKind::SpawnColorTint, {{"r0", 0}, {"g0", 0}, {"b0", 0}, {"r1", 1}, {"g1", 1}, {"b1", 1}}), t, {});
        const u32 r = t.tint & 0xFF, g = (t.tint >> 8) & 0xFF, b = (t.tint >> 16) & 0xFF, a = t.tint >> 24;
        tintOk = tintOk && r == g && g == b && a == 255;
    }
    check(sLo >= 0.6f && sHi <= 1.4f && sHi - sLo > 0.7f, "sizescale spans [min, max]");
    checkNear(static_cast<f32>(sSum / 10000.0), 1.0f, 0.02f, "sizescale mean is the midpoint");
    check(rLo >= 10.0f * kDegToRad - 1e-4f && rHi <= 50.0f * kDegToRad + 1e-4f && rHi - rLo > 30.0f * kDegToRad, "rotation spans [minDeg, maxDeg] (radians)");
    check(wLo >= -90.0f * kDegToRad - 1e-4f && wHi <= 90.0f * kDegToRad + 1e-4f && wHi - wLo > 150.0f * kDegToRad, "spin spans [spinMin, spinMax] (radians/s)");
    check(tintOk, "colortint is a single lerp between its two colours, alpha 255");
    Particle p = particleAt({}, {1, 1, 1});
    runSpawnModule(mod(ModuleKind::SpawnInheritVelocity, {{"scale", 0.5f}}), p, {10, 20, 30});
    checkNear(p.velocity.x, 6.0f, 1e-5f, "inheritvelocity adds emitterVelocity * scale");
    checkNear(p.velocity.z, 16.0f, 1e-5f, "...on every axis");
}

static void testBindingAndOrder() {
    AVER_INFO("--- user-parameter binding and authored order ---");
    ParticleEffect fx;
    Module force = makeDefaultModule(ModuleKind::UpdateForce);
    force.p[0].userSlot = 2; force.p[0].comp = 1;   // x <- user[2].y
    force.p[1].value = 5;                           // y literal
    fx.modules.push_back(force);
    f32 user[kMaxUserParams][4] = {};
    user[2][1] = 8.0f;
    std::vector<ResolvedModule> r;
    resolveModules(fx.modules, user, r);
    check(r.size() == 1 && r[0].p[0] == 8.0f && r[0].p[1] == 5.0f, "a bound parameter reads the user slot component, a literal stays");
    user[2][1] = -3.0f;
    resolveModules(fx.modules, user, r);
    check(r.size() == 1 && r[0].p[0] == -3.0f, "a changed user value changes the next resolve");
    resolveModules(fx.modules, nullptr, r);
    check(r.size() == 1 && r[0].p[0] == 0.0f, "no user block: a bound parameter reads 0");

    fx.modules.clear();
    Module a = makeDefaultModule(ModuleKind::UpdateForce), b = makeDefaultModule(ModuleKind::UpdateDrag), c = makeDefaultModule(ModuleKind::UpdateCollideSphere);
    b.enabled = false;
    fx.modules = {c, a, b};
    resolveModules(fx.modules, nullptr, r);
    check(r.size() == 2 && r[0].kind == ModuleKind::UpdateCollideSphere && r[1].kind == ModuleKind::UpdateForce, "disabled modules drop out; authored order is kept");
    check(r[0].stage == ModuleStage::Position && r[1].stage == ModuleStage::Velocity, "stages ride along");
}

static void testCurves() {
    AVER_INFO("--- curves ---");
    ParticleEffect fx;
    fx.sizeStart = 10; fx.sizeEnd = 20;
    checkNear(evalSize(fx, 0.5f), 15.0f, 1e-5f, "no curve: the start/end pair");
    f32 out[4];
    check(!evalCurve(fx.sizeCurve, 0.5f, out), "an empty curve does not evaluate");
    fx.sizeCurve.n = 2; fx.sizeCurve.t[0] = 0; fx.sizeCurve.t[1] = 1; fx.sizeCurve.v[0][0] = 2; fx.sizeCurve.v[1][0] = 4;
    checkNear(evalSize(fx, 0.25f), 2.5f, 1e-5f, "two stops, linear");
    checkNear(evalSize(fx, -1.0f), 2.0f, 1e-6f, "clamped below");
    checkNear(evalSize(fx, 5.0f), 4.0f, 1e-6f, "clamped above");
    fx.sizeCurve.n = 3; fx.sizeCurve.t[1] = 0.5f; fx.sizeCurve.t[2] = 1; fx.sizeCurve.v[1][0] = 10; fx.sizeCurve.v[2][0] = 0;
    checkNear(evalSize(fx, 0.25f), 6.0f, 1e-5f, "three stops, first segment");
    checkNear(evalSize(fx, 0.75f), 5.0f, 1e-5f, "three stops, second segment");
    fx.sizeCurve.n = 4; fx.sizeCurve.t[0] = 0; fx.sizeCurve.t[1] = 0.1f; fx.sizeCurve.t[2] = 0.2f; fx.sizeCurve.t[3] = 1;
    fx.sizeCurve.v[0][0] = 0; fx.sizeCurve.v[1][0] = 1; fx.sizeCurve.v[2][0] = 1; fx.sizeCurve.v[3][0] = 0;
    checkNear(evalSize(fx, 0.15f), 1.0f, 1e-5f, "four stops, a plateau");
    checkNear(evalSize(fx, 0.6f), 0.5f, 1e-5f, "four stops, the ramp down");
    fx.colorStart[0] = 1; fx.colorEnd[0] = 0;
    evalColor(fx, 0.25f, out);
    checkNear(out[0], 0.75f, 1e-5f, "colour without a curve lerps start -> end");
    fx.colorCurve.n = 2; fx.colorCurve.t[1] = 1; fx.colorCurve.v[0][0] = 0.2f; fx.colorCurve.v[1][0] = 0.6f; fx.colorCurve.v[0][3] = 1; fx.colorCurve.v[1][3] = 0;
    evalColor(fx, 0.5f, out);
    checkNear(out[0], 0.4f, 1e-5f, "colour curve rgb");
    checkNear(out[3], 0.5f, 1e-5f, "colour curve alpha");
    check(packRgba8(1, 0.5f, 0, 1) == (255u | (128u << 8) | (0u << 16) | (255u << 24)), "packRgba8 rounds to bytes, R in the low byte");
}

int main() {
    AVER_INFO("ParticleModulesTest");
    testRandom();
    testNoise();
    testTable();
    testDefaultsAreNoOps();
    testVelocityModules();
    testPositionModules();
    testSpawnModules();
    testBindingAndOrder();
    testCurves();
    AVER_INFO("ParticleModulesTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
