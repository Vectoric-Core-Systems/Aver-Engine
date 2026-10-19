// The 13 modules' arithmetic on the CPU. particles_common.hlsli repeats it line for line; change one, change both.
#include "aver/particles/ParticleModules.hpp"

#include "aver/particles/ParticleRandom.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace aver::particles {
namespace {

const ModuleInfo kModules[] = {
    {ModuleKind::SpawnSizeScale, "spawn.sizescale", ModuleStage::Spawn, 2,
     {"min", "max"}, {1, 1}},
    {ModuleKind::SpawnColorTint, "spawn.colortint", ModuleStage::Spawn, 6,
     {"r0", "g0", "b0", "r1", "g1", "b1"}, {1, 1, 1, 1, 1, 1}},
    {ModuleKind::SpawnRotation, "spawn.rotation", ModuleStage::Spawn, 4,
     {"minDeg", "maxDeg", "spinMin", "spinMax"}, {0, 0, 0, 0}},
    {ModuleKind::SpawnInheritVelocity, "spawn.inheritvelocity", ModuleStage::Spawn, 1,
     {"scale"}, {0}},
    {ModuleKind::UpdateForce, "update.force", ModuleStage::Velocity, 3,
     {"x", "y", "z"}, {0, 0, 0}},
    {ModuleKind::UpdateDrag, "update.drag", ModuleStage::Velocity, 1,
     {"k"}, {0}},
    {ModuleKind::UpdateAttractor, "update.attractor", ModuleStage::Velocity, 7,
     {"x", "y", "z", "strength", "radius", "falloff", "local"}, {0, 0, 0, 0, 0, 1, 0}},
    {ModuleKind::UpdateVortex, "update.vortex", ModuleStage::Velocity, 6,
     {"ax", "ay", "az", "strength", "radius", "lift"}, {0, 0, 1, 0, 0, 0}},
    {ModuleKind::UpdateCurlNoise, "update.curlnoise", ModuleStage::Velocity, 5,
     {"freq", "strength", "scroll", "octaves", "seed"}, {0.01f, 100, 0.2f, 1, 0}},
    {ModuleKind::UpdateSpeedLimit, "update.speedlimit", ModuleStage::Velocity, 2,
     {"max", "min"}, {0, 0}},
    {ModuleKind::UpdateCollidePlane, "update.collideplane", ModuleStage::Position, 7,
     {"nx", "ny", "nz", "d", "restitution", "friction", "kill"}, {0, 0, 1, 0, 0.4f, 0.2f, 0}},
    {ModuleKind::UpdateCollideSphere, "update.collidesphere", ModuleStage::Position, 7,
     {"x", "y", "z", "r", "restitution", "friction", "kill"}, {0, 0, 0, 0, 0.4f, 0.2f, 0}},
    {ModuleKind::UpdateKillVolume, "update.killvolume", ModuleStage::Position, 8,
     {"shape", "cx", "cy", "cz", "hx", "hy", "hz", "invert"}, {0, 0, 0, 0, 0, 0, 0, 0}},
};
static_assert(sizeof(kModules) / sizeof(kModules[0]) == static_cast<usize>(ModuleKind::Count),
              "one ModuleInfo per ModuleKind, in enum order");

bool eqCI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Reflects the velocity about `n` when it is heading into the surface: normal part scaled by restitution, tangential
// part by (1 - friction).
void bounce(Particle& p, const Vec3& n, f32 restitution, f32 friction) {
    const f32 vn = dot(p.velocity, n);
    if (vn >= 0.0f) return;
    const Vec3 vt = p.velocity - n * vn;
    p.velocity = vt * (1.0f - clamp01(friction)) - n * (vn * std::max(0.0f, restitution));
}

u32 collideResult(Particle& p, bool kill) {
    u32 r = 0;
    if (kill) { p.age = p.lifetime; r |= kModuleKilled; }
    const u32 flags = p.spawnInfo >> 16;
    if ((flags & kParticleFlagCollided) == 0) {
        p.spawnInfo |= (kParticleFlagCollided << 16);
        r |= kModuleCollided;
    }
    return r;
}

// One channel of the curl-noise potential: an octave sum of the lattice noise, each channel on its own lattice seed.
f32 potential(u32 channel, const Vec3& n, int octaves, u32 seed) {
    f32 sum = 0.0f, amp = 1.0f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * gradNoise3(n.x * freq, n.y * freq, n.z * freq, seed + channel);
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum;
}

} // namespace

const ModuleInfo& moduleInfo(ModuleKind kind) {
    const usize i = static_cast<usize>(kind);
    return kModules[i < static_cast<usize>(ModuleKind::Count) ? i : 0];
}

const ModuleInfo* findModuleInfo(std::string_view key) {
    for (const ModuleInfo& m : kModules)
        if (eqCI(m.key, key)) return &m;
    return nullptr;
}

int findModuleParam(const ModuleInfo& info, std::string_view name) {
    for (u32 i = 0; i < info.paramCount; ++i)
        if (eqCI(info.names[i], name)) return static_cast<int>(i);
    return -1;
}

Module makeDefaultModule(ModuleKind kind) {
    Module m;
    m.kind = kind;
    const ModuleInfo& info = moduleInfo(kind);
    for (u32 i = 0; i < kMaxModuleParams; ++i) {
        m.p[i].value = i < info.paramCount ? info.defaults[i] : 0.0f;
        m.p[i].userSlot = -1;
        m.p[i].comp = 0;
    }
    return m;
}

void resolveModules(const std::vector<Module>& modules, const f32 (*user)[4], std::vector<ResolvedModule>& out) {
    out.clear();
    for (const Module& m : modules) {
        if (!m.enabled || static_cast<u32>(m.kind) >= static_cast<u32>(ModuleKind::Count)) continue;
        ResolvedModule r;
        r.kind = m.kind;
        r.stage = moduleInfo(m.kind).stage;
        for (u32 i = 0; i < kMaxModuleParams; ++i) {
            const ModuleParam& mp = m.p[i];
            if (mp.userSlot >= 0 && static_cast<u32>(mp.userSlot) < kMaxUserParams && user)
                r.p[i] = user[mp.userSlot][mp.comp & 3u];
            else if (mp.userSlot >= 0)
                r.p[i] = 0.0f;
            else
                r.p[i] = mp.value;
        }
        out.push_back(r);
    }
}

void runSpawnModule(const ResolvedModule& m, Particle& p, const Vec3& emitterVelocity) {
    switch (m.kind) {
    case ModuleKind::SpawnSizeScale:
        p.sizeScale *= lerpf(m.p[0], m.p[1], rand01(p.seed, kStreamSizeScale));
        break;
    case ModuleKind::SpawnColorTint: {
        const f32 t = rand01(p.seed, kStreamTint);
        p.tint = packRgba8(lerpf(m.p[0], m.p[3], t), lerpf(m.p[1], m.p[4], t), lerpf(m.p[2], m.p[5], t), 1.0f);
        break;
    }
    case ModuleKind::SpawnRotation:
        p.rotation = lerpf(m.p[0], m.p[1], rand01(p.seed, kStreamRotation)) * kDegToRad;
        p.angularVelocity = lerpf(m.p[2], m.p[3], rand01(p.seed, kStreamSpin)) * kDegToRad;
        break;
    case ModuleKind::SpawnInheritVelocity:
        p.velocity += emitterVelocity * m.p[0];
        break;
    default:
        break;
    }
}

Vec3 curlNoiseField(const Vec3& worldPos, f32 freq, f32 time, f32 scroll, int octaves, u32 seed) {
    const int oct = octaves < 1 ? 1 : (octaves > 4 ? 4 : octaves);
    const u32 ns = hash32(seed);
    const Vec3 n{worldPos.x * freq, worldPos.y * freq, worldPos.z * freq + scroll * time};
    const f32 e = 0.01f, inv2e = 0.5f / e;
    auto P = [&](u32 ch, f32 dx, f32 dy, f32 dz) { return potential(ch, Vec3{n.x + dx, n.y + dy, n.z + dz}, oct, ns); };
    Vec3 c;
    c.x = (P(2, 0, e, 0) - P(2, 0, -e, 0) - (P(1, 0, 0, e) - P(1, 0, 0, -e))) * inv2e;
    c.y = (P(0, 0, 0, e) - P(0, 0, 0, -e) - (P(2, e, 0, 0) - P(2, -e, 0, 0))) * inv2e;
    c.z = (P(1, e, 0, 0) - P(1, -e, 0, 0) - (P(0, 0, e, 0) - P(0, 0, -e, 0))) * inv2e;
    return c;
}

void runVelocityModule(const ResolvedModule& m, const ModuleEnv& env, Particle& p) {
    switch (m.kind) {
    case ModuleKind::UpdateForce:
        p.velocity += Vec3{m.p[0], m.p[1], m.p[2]} * env.dt;
        break;
    case ModuleKind::UpdateDrag: {
        const f32 speed = p.velocity.size();
        const f32 keep = 1.0f - std::min(speed * m.p[0] * env.dt, 1.0f);   // never reverses the velocity
        p.velocity *= std::max(keep, 0.0f);
        break;
    }
    case ModuleKind::UpdateAttractor: {
        const Vec3 target = Vec3{m.p[0], m.p[1], m.p[2]} + (m.p[6] != 0.0f ? env.origin : Vec3{0, 0, 0});
        const Vec3 d = target - p.position;
        const f32 len = d.size();
        if (len > 1e-4f) {
            f32 w = 1.0f;
            const f32 radius = m.p[4];
            if (radius > 0.0f) {
                w = len >= radius ? 0.0f : std::pow(1.0f - len / radius, std::max(m.p[5], 0.0f));
            }
            p.velocity += d * (m.p[3] * w / len * env.dt);
        }
        break;
    }
    case ModuleKind::UpdateVortex: {
        const Vec3 axis = Vec3{m.p[0], m.p[1], m.p[2]}.getSafeNormal();
        if (axis.sizeSquared() < 0.5f) break;
        const Vec3 r = p.position - env.origin;
        const Vec3 rp = r - axis * dot(r, axis);
        const f32 rl = rp.size();
        if (rl > 1e-4f) {
            const f32 radius = m.p[4];
            const f32 w = radius > 0.0f ? clamp01(1.0f - rl / radius) : 1.0f;
            const Vec3 tangent = cross(axis, rp) * (1.0f / rl);
            p.velocity += (tangent * (m.p[3] * w) + axis * m.p[5]) * env.dt;
        }
        break;
    }
    case ModuleKind::UpdateCurlNoise: {
        const Vec3 c = curlNoiseField(p.position, m.p[0], env.time, m.p[2], static_cast<int>(m.p[3]),
                                      static_cast<u32>(static_cast<i32>(m.p[4])));
        p.velocity += c * (m.p[1] * env.dt);
        break;
    }
    case ModuleKind::UpdateSpeedLimit: {
        const f32 speed = p.velocity.size();
        if (speed > 1e-6f) {
            if (m.p[0] > 0.0f && speed > m.p[0]) p.velocity *= m.p[0] / speed;
            else if (m.p[1] > 0.0f && speed < m.p[1]) p.velocity *= m.p[1] / speed;
        }
        break;
    }
    default:
        break;
    }
}

u32 runPositionModule(const ResolvedModule& m, const ModuleEnv& env, Particle& p) {
    (void)env;
    switch (m.kind) {
    case ModuleKind::UpdateCollidePlane: {
        const Vec3 n = Vec3{m.p[0], m.p[1], m.p[2]}.getSafeNormal();
        if (n.sizeSquared() < 0.5f) return 0;
        const f32 s = dot(n, p.position) - m.p[3];
        if (s >= 0.0f) return 0;
        p.position -= n * s;
        bounce(p, n, m.p[4], m.p[5]);
        return collideResult(p, m.p[6] != 0.0f);
    }
    case ModuleKind::UpdateCollideSphere: {
        const f32 r = m.p[3];
        if (!(r > 0.0f)) return 0;
        const Vec3 c{m.p[0], m.p[1], m.p[2]};
        const Vec3 d = p.position - c;
        const f32 len = d.size();
        if (len >= r) return 0;
        const Vec3 n = len > 1e-5f ? d * (1.0f / len) : Vec3{0, 0, 1};
        p.position = c + n * r;
        bounce(p, n, m.p[4], m.p[5]);
        return collideResult(p, m.p[6] != 0.0f);
    }
    case ModuleKind::UpdateKillVolume: {
        const Vec3 d = p.position - Vec3{m.p[1], m.p[2], m.p[3]};
        bool inside;
        if (m.p[0] < 0.5f) inside = std::fabs(d.x) <= m.p[4] && std::fabs(d.y) <= m.p[5] && std::fabs(d.z) <= m.p[6];
        else               inside = d.sizeSquared() <= m.p[4] * m.p[4];
        const bool invert = m.p[7] != 0.0f;
        if (inside != invert) { p.age = p.lifetime; return kModuleKilled; }
        return 0;
    }
    default:
        return 0;
    }
}

bool evalCurve(const Curve4& c, f32 t, f32 out[4]) {
    if (c.n < 2) return false;
    const u32 n = c.n > 4 ? 4u : c.n;
    if (t <= c.t[0]) { for (int k = 0; k < 4; ++k) out[k] = c.v[0][k]; return true; }
    if (t >= c.t[n - 1]) { for (int k = 0; k < 4; ++k) out[k] = c.v[n - 1][k]; return true; }
    for (u32 i = 0; i + 1 < n; ++i) {
        if (t <= c.t[i + 1]) {
            const f32 span = c.t[i + 1] - c.t[i];
            const f32 u = span > 1e-8f ? (t - c.t[i]) / span : 1.0f;
            for (int k = 0; k < 4; ++k) out[k] = lerpf(c.v[i][k], c.v[i + 1][k], u);
            return true;
        }
    }
    for (int k = 0; k < 4; ++k) out[k] = c.v[n - 1][k];
    return true;
}

f32 evalSize(const ParticleEffect& fx, f32 t) {
    f32 v[4];
    if (evalCurve(fx.sizeCurve, t, v)) return v[0];
    return lerpf(fx.sizeStart, fx.sizeEnd, t);
}

void evalColor(const ParticleEffect& fx, f32 t, f32 out[4]) {
    if (evalCurve(fx.colorCurve, t, out)) return;
    for (int k = 0; k < 4; ++k) out[k] = lerpf(fx.colorStart[k], fx.colorEnd[k], t);
}

} // namespace aver::particles
