// The tick that turns a scene::CParticleEmitter's clock into live particles.
//
// EVERY EFFECT-SHAPED DECISION IN HERE IS DATA, NEVER A BRANCH ON WHAT KIND OF EFFECT THIS IS. There
// is no "if this is smoke" anywhere below -- only ParticleEffect's own numbers. That is what keeps a
// rising smoke column, falling snow, a burst of embers and a waterfall's mist four different VALUES
// of the same loop rather than four code paths a fifth effect would need a fifth path for.
#include "aver/particles/ParticleSystem.hpp"

#include "aver/scene/ComponentPool.hpp"

#include <algorithm>
#include <cmath>

namespace aver::particles {
namespace {

// splitmix32 -- the SAME mixing constants modules/render.pcg/src/PcgVolume.cpp's hash32 uses, so two
// independent per-entity RNG streams in this engine agree on what "good enough for visuals" mixing
// looks like. Not a shared call: PcgVolume's is private to that translation unit, and duplicating a
// five-line integer hash here is cheaper than a cross-module dependency for it.
u32 hash32(u32 x) {
    u32 z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

// A stable per-entity seed, so replaying the SAME scene reproduces the SAME particles -- the
// determinism this codebase measures everything else by (see aver-render-nondeterminism notes: a
// probe run is only trustworthy if a rerun agrees). 0 is reserved (CParticleEmitter::seed == 0 means
// "not yet assigned"), so a hash landing on it is nudged to 1.
u32 seedFor(scene::Entity e) {
    const u32 h = hash32(static_cast<u32>(e));
    return h == 0 ? 1u : h;
}

// Applies a row-vector world matrix to a POINT (translation included) -- the same manual expansion
// VoxiRenderer.cpp and modules/anim/src/Pose.cpp already use; Math.hpp has no Mat4*Vec3 operator.
Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

// The same expansion with the translation row dropped (w = 0) -- a DIRECTION, not a position. Reuses
// the world matrix's own scale/rotation as-is rather than decomposing it, so a non-uniformly scaled
// emitter stretches its spawn shape and its base direction the same way a mesh under it would
// stretch; simple, and exact for the uniform-scale case this slice's test content actually uses.
Vec3 transformDirection(const Mat4& m, const Vec3& d) {
    return {d.x * m.m[0][0] + d.y * m.m[1][0] + d.z * m.m[2][0],
            d.x * m.m[0][1] + d.y * m.m[1][1] + d.z * m.m[2][1],
            d.x * m.m[0][2] + d.y * m.m[1][2] + d.z * m.m[2][2]};
}

// Uniform float in [lo, hi]. hi <= lo is read as "the fixed value lo", not swapped or asserted: an
// author who mistypes min > max gets lo every time, not a crash.
f32 uniform(std::mt19937& rng, f32 lo, f32 hi) {
    if (hi <= lo) return lo;
    std::uniform_real_distribution<f32> d(lo, hi);
    return d(rng);
}

// A random offset in the emitter's LOCAL space, from the effect's emission shape.
Vec3 shapeOffset(const ParticleEffect& fx, std::mt19937& rng) {
    switch (fx.shape) {
    case EmitterShape::Sphere: {
        // Rejection sampling inside the unit ball, then scaled by the radius -- exact, and cheap at
        // the particle counts ParticleEffect::maxParticles targets.
        std::uniform_real_distribution<f32> d(-1.0f, 1.0f);
        Vec3 p;
        do { p = {d(rng), d(rng), d(rng)}; } while (p.sizeSquared() > 1.0f);
        return p * fx.shapeSize.x;
    }
    case EmitterShape::Box: {
        const f32 x = fx.shapeSize.x > 0.0f ? uniform(rng, -fx.shapeSize.x, fx.shapeSize.x) : 0.0f;
        const f32 y = fx.shapeSize.y > 0.0f ? uniform(rng, -fx.shapeSize.y, fx.shapeSize.y) : 0.0f;
        const f32 z = fx.shapeSize.z > 0.0f ? uniform(rng, -fx.shapeSize.z, fx.shapeSize.z) : 0.0f;
        return {x, y, z};
    }
    case EmitterShape::Point:
    default:
        return {0, 0, 0};
    }
}

// A random direction inside a cone of half-angle `spreadDeg` around `axis` (both already
// world-space, `axis` already unit length). cos(theta) is drawn uniformly over
// [cos(spreadDeg), 1] rather than theta itself, so the sample is uniform over SOLID ANGLE within the
// cone -- spreadDeg = 180 is therefore a genuinely isotropic full-sphere burst, not one biased
// toward the poles. spreadDeg = 0 always returns `axis` exactly: uniform() above returns its low
// bound when the range collapses, so cos(theta) is pinned to 1.
Vec3 coneSample(const Vec3& axis, f32 spreadDeg, std::mt19937& rng) {
    const f32 spreadRad = spreadDeg * kDegToRad;
    const f32 cosSpread = std::cos(spreadRad < 0.0f ? 0.0f : (spreadRad > kPi ? kPi : spreadRad));
    const f32 cosTheta = uniform(rng, cosSpread, 1.0f);
    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const f32 phi = uniform(rng, 0.0f, kTwoPi);

    // An arbitrary basis perpendicular to axis: pick whichever world axis is LEAST parallel to it,
    // so the cross product below never degenerates.
    const Vec3 hint = std::fabs(axis.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 right = cross(hint, axis).getSafeNormal();
    const Vec3 up = cross(axis, right);

    return axis * cosTheta + right * (sinTheta * std::cos(phi)) + up * (sinTheta * std::sin(phi));
}

Particle spawnParticle(const ParticleEffect& fx, const Mat4& worldXf, std::mt19937& rng) {
    Particle p{};
    p.position = transformPoint(worldXf, shapeOffset(fx, rng));

    Vec3 axis = transformDirection(worldXf, fx.direction).getSafeNormal();
    if (axis.sizeSquared() < 0.5f) axis = Vec3{0, 0, 1};   // a degenerate direction still emits somewhere
    const Vec3 dir = coneSample(axis, fx.spreadDeg, rng);

    p.velocity = dir * uniform(rng, fx.speedMin, fx.speedMax);
    p.age = 0.0f;
    // A non-positive draw would divide by zero the moment ParticleRenderer computes age/lifetime;
    // 1 ms floors it instead of asserting on an author's typo.
    p.lifetime = std::max(0.001f, uniform(rng, fx.lifetimeMin, fx.lifetimeMax));
    p.seed = rng();   // forward-looking only this slice -- see Particle::seed's own comment.
    return p;
}

// Integrates every particle by `dt` under DECIDED's physics scope -- constant gravity, velocity
// damping, nothing else -- and compacts out anything that has aged past its lifetime. Semi-implicit
// Euler (velocity updates before position), the same integrator order the rest of this engine's
// CPU-side motion uses.
void updateParticles(std::vector<Particle>& ps, const ParticleEffect& fx, f32 dt) {
    usize w = 0;
    for (usize r = 0; r < ps.size(); ++r) {
        Particle p = ps[r];
        p.age += dt;
        if (p.age >= p.lifetime) continue;   // dies here; not written back, so the array compacts

        p.velocity += fx.gravity * dt;
        if (fx.damping > 0.0f) {
            const f32 keep = std::max(0.0f, 1.0f - fx.damping * dt);
            p.velocity *= keep;
        }
        p.position += p.velocity * dt;

        ps[w++] = p;
    }
    ps.resize(w);
}

} // namespace

void ParticleSystem::tick(scene::World& world, f32 dt) {
    // Retire state for entities that are gone -- the same pattern AnimSystem::tick uses for its own
    // posed_ map, and the same reason: a recycled entity slot must not inherit a stranger's particles.
    for (auto it = emitters_.begin(); it != emitters_.end(); ) {
        if (world.valid(it->first) && !world.destroyPending(it->first)) ++it;
        else it = emitters_.erase(it);
    }
    order_.erase(std::remove_if(order_.begin(), order_.end(),
                                [&](scene::Entity e) { return emitters_.find(e) == emitters_.end(); }),
                order_.end());

    scene::ComponentPool* pool = world.pool(scene::kComponentParticleEmitter);
    if (!pool) return;

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* c = static_cast<scene::CParticleEmitter*>(pool->dataAt(i));
        if (!c) continue;

        if (c->seed == 0) c->seed = seedFor(e);   // first time this component has been ticked

        auto [it, inserted] = emitters_.try_emplace(e);
        if (inserted) {
            it->second.rng.seed(c->seed);
            order_.push_back(e);
        }
        EmitterState& st = it->second;

        const ParticleEffect* fx = effects_ ? effects_->find(c->effect) : nullptr;
        st.effect = fx;

        const bool playing = !(c->flags & scene::kParticleEmitterStopped);
        if (playing && !st.wasPlaying) {
            // The stopped -> playing edge: the clock and the burst both restart, exactly once, no
            // matter how long tick() has been called for this entity before now.
            c->age = 0.0f;
            st.burstFired = false;
        }
        st.wasPlaying = playing;

        if (!fx) {
            // No effect resolved -- a bad id, or DECIDED 3's loader has not landed one yet. The clock
            // still advances, matching AnimSystem's "the clock advances even without a rig" so a
            // scrubbing inspector sees something; nothing spawns, and there is nothing to update.
            if (playing) c->age += dt;
            continue;
        }

        u32 toSpawn = 0;
        if (playing) {
            c->age += dt;
            if (!st.burstFired) {
                toSpawn += fx->burstCount;
                st.burstFired = true;
            }
            if (fx->emissionRate > 0.0f) {
                c->emitAccum += fx->emissionRate * dt;
                const u32 whole = static_cast<u32>(c->emitAccum);
                toSpawn += whole;
                c->emitAccum -= static_cast<f32>(whole);
            }
        }

        if (toSpawn > 0) {
            const Mat4& worldXf = world.worldMatrix(e);
            const u32 room = fx->maxParticles > st.particles.size()
                                ? fx->maxParticles - static_cast<u32>(st.particles.size()) : 0;
            const u32 spawnNow = std::min(toSpawn, room);
            // OVER BUDGET DROPS NEW SPAWNS; IT DOES NOT EVICT OLD ONES. An effect that asks for more
            // than maxParticles this frame just looks a little sparser rather than aging its oldest,
            // most-visible particles out early to make room for brand new ones nobody would notice.
            st.particles.reserve(std::min<usize>(fx->maxParticles, st.particles.size() + spawnNow));
            for (u32 n = 0; n < spawnNow; ++n) st.particles.push_back(spawnParticle(*fx, worldXf, st.rng));
        }

        updateParticles(st.particles, *fx, dt);
    }
}

u32 ParticleSystem::liveParticles(scene::Entity e) const {
    auto it = emitters_.find(e);
    return it == emitters_.end() ? 0u : static_cast<u32>(it->second.particles.size());
}

const std::vector<Particle>* ParticleSystem::particles(scene::Entity e) const {
    auto it = emitters_.find(e);
    return it == emitters_.end() ? nullptr : &it->second.particles;
}

ParticleSystem& particleSystem() {
    static ParticleSystem s;
    return s;
}

} // namespace aver::particles
