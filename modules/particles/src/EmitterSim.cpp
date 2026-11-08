// EmitterSim: spawn and step on the CPU. particles_sim.hlsl is the GPU twin; keep the arithmetic in step.
#include "aver/particles/EmitterSim.hpp"

#include "aver/particles/ParticleRandom.hpp"

#include <algorithm>
#include <cmath>

namespace aver::particles {
namespace {

// Row-vector world matrix applied to a point / a direction (Math.hpp has no Mat4*Vec3 operator).
Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}
Vec3 transformDirection(const Mat4& m, const Vec3& d) {
    return {d.x * m.m[0][0] + d.y * m.m[1][0] + d.z * m.m[2][0],
            d.x * m.m[0][1] + d.y * m.m[1][1] + d.z * m.m[2][1],
            d.x * m.m[0][2] + d.y * m.m[1][2] + d.z * m.m[2][2]};
}

f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// A position in the emitter's LOCAL space from the effect's shape; draws come from fixed streams of the particle's seed.
Vec3 shapeOffset(const ParticleEffect& fx, u32 seed) {
    const f32 a = rand01(seed, kStreamShapeA), b = rand01(seed, kStreamShapeB), c = rand01(seed, kStreamShapeC);
    switch (fx.shape) {
    case EmitterShape::Sphere: {
        const f32 z = 2.0f * a - 1.0f;
        const f32 phi = kTwoPi * b;
        const f32 s = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const f32 r = fx.shapeShell ? 1.0f : std::cbrt(c);
        const f32 k = r * fx.shapeSize.x;
        return {s * std::cos(phi) * k, s * std::sin(phi) * k, z * k};
    }
    case EmitterShape::Box:
        return {fx.shapeSize.x > 0.0f ? (2.0f * a - 1.0f) * fx.shapeSize.x : 0.0f,
                fx.shapeSize.y > 0.0f ? (2.0f * b - 1.0f) * fx.shapeSize.y : 0.0f,
                fx.shapeSize.z > 0.0f ? (2.0f * c - 1.0f) * fx.shapeSize.z : 0.0f};
    case EmitterShape::Cylinder: {
        const f32 phi = kTwoPi * a;
        const f32 r = (fx.shapeShell ? 1.0f : std::sqrt(b)) * fx.shapeSize.x;
        return {std::cos(phi) * r, std::sin(phi) * r, (2.0f * c - 1.0f) * 0.5f * fx.shapeSize.y};
    }
    case EmitterShape::Disc: {
        const f32 phi = kTwoPi * a;
        const f32 r = (fx.shapeShell ? 1.0f : std::sqrt(b)) * fx.shapeSize.x;
        return {std::cos(phi) * r, std::sin(phi) * r, 0.0f};
    }
    case EmitterShape::Ring: {
        const f32 outer = std::max(fx.shapeSize.x, 0.0f);
        const f32 inner = std::min(std::max(fx.shapeSize.z, 0.0f), outer);
        const f32 phi = kTwoPi * a;
        const f32 r = fx.shapeShell ? outer : std::sqrt(lerpf(inner * inner, outer * outer, b));
        const f32 h = (2.0f * c - 1.0f) * 0.5f * fx.shapeSize.y;
        Vec3 axis = fx.shapeAxis.getSafeNormal();
        if (axis.sizeSquared() < 0.5f) axis = Vec3{0, 0, 1};
        const Vec3 hint = std::fabs(axis.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        const Vec3 right = cross(hint, axis).getSafeNormal();
        const Vec3 up = cross(axis, right);
        return right * (std::cos(phi) * r) + up * (std::sin(phi) * r) + axis * h;
    }
    case EmitterShape::Point:
    default:
        return {0, 0, 0};
    }
}

// A direction inside a cone of half-angle spreadDeg around the unit `axis`: cos(theta) uniform over [cos(spread), 1]
// so the sample is uniform over solid angle (180 is an isotropic burst).
Vec3 coneSample(const Vec3& axis, f32 spreadDeg, u32 seed) {
    const f32 spreadRad = spreadDeg * kDegToRad;
    const f32 cosSpread = std::cos(spreadRad < 0.0f ? 0.0f : (spreadRad > kPi ? kPi : spreadRad));
    const f32 cosTheta = lerpf(cosSpread, 1.0f, rand01(seed, kStreamConeCos));
    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const f32 phi = kTwoPi * rand01(seed, kStreamConePhi);
    const Vec3 hint = std::fabs(axis.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 right = cross(hint, axis).getSafeNormal();
    const Vec3 up = cross(axis, right);
    return axis * cosTheta + right * (sinTheta * std::cos(phi)) + up * (sinTheta * std::sin(phi));
}

} // namespace

void EmitterSim::init(const ParticleEffect& fx, const std::vector<UserParam>& params, u32 emitterSeed) {
    fx_ = &fx;
    emitterSeed_ = emitterSeed == 0 ? 1u : emitterSeed;
    for (u32 i = 0; i < kMaxUserParams; ++i)
        for (u32 k = 0; k < 4; ++k) user_[i][k] = 0.0f;
    for (usize i = 0; i < params.size() && i < kMaxUserParams; ++i)
        for (u32 k = 0; k < 4; ++k) user_[i][k] = params[i].v[k];
    linked_ = fx.render == RenderKind::Ribbon && fx.linked;
    capacity_ = linked_ ? std::clamp<u32>(fx.linkedCapacity, 2u, kMaxLinkedCapacity) : 0u;
    ring_ = (fx.render == RenderKind::Ribbon && !fx.linked)
                ? std::clamp<u32>(fx.trailPoints, 2u, kMaxTrailPoints) - 1u : 0u;
    clear();
    nextId_ = 0;
}

void EmitterSim::setUser(u32 slot, const f32 v[4]) {
    if (slot >= kMaxUserParams) return;
    for (u32 k = 0; k < 4; ++k) user_[slot][k] = v[k];
}

void EmitterSim::clear() {
    particles_.clear();
    trail_.clear();
    if (linked_) particles_.assign(capacity_, Particle{});
}

void EmitterSim::shiftWorld(const Vec3& delta) {
    origin_ -= delta;
    for (Particle& p : particles_) p.position -= delta;
    for (Vec4& t : trail_) { t.x -= delta.x; t.y -= delta.y; t.z -= delta.z; }
}

u32 EmitterSim::aliveCount() const {
    if (!linked_) return static_cast<u32>(particles_.size());
    u32 n = 0;
    for (const Particle& p : particles_) n += particleAlive(p) ? 1u : 0u;
    return n;
}

Particle EmitterSim::make(const SpawnRequest& req, u32 id, const std::vector<ResolvedModule>& spawnMods) const {
    const ParticleEffect& fx = *fx_;
    Particle p{};
    const u32 effSeed = req.seed != 0 ? (emitterSeed_ ^ hash32(req.seed)) : emitterSeed_;
    p.seed = particleSeed(effSeed, id);
    p.id = id;
    p.position = transformPoint(req.xf, shapeOffset(fx, p.seed));

    Vec3 axis = transformDirection(req.xf, fx.direction).getSafeNormal();
    if (axis.sizeSquared() < 0.5f) axis = Vec3{0, 0, 1};   // a degenerate direction still emits somewhere
    const Vec3 dir = coneSample(axis, fx.spreadDeg, p.seed);
    const f32 speed = lerpf(fx.speedMin, fx.speedMax, rand01(p.seed, kStreamSpeed)) * req.speedScale;
    p.velocity = dir * speed + req.addVelocity;
    p.lifetime = std::max(0.001f, lerpf(fx.lifetimeMin, fx.lifetimeMax, rand01(p.seed, kStreamLifetime)) * req.lifeScale);
    p.age = std::clamp(req.ageFraction, 0.0f, 0.95f) * p.lifetime;
    p.sizeScale = req.sizeScale;
    p.tint = req.tint;
    u32 frame = 0;
    if (fx.subuvFrames > 1 && fx.subuvFps > 0.0f)
        frame = std::min<u32>(static_cast<u32>(rand01(p.seed, kStreamStartFrame) * static_cast<f32>(fx.subuvFrames)),
                              fx.subuvFrames - 1u);
    p.spawnInfo = frame & 0xFFFFu;
    for (const ResolvedModule& m : spawnMods) runSpawnModule(m, p, req.velocity);
    return p;
}

void EmitterSim::pushTrail(usize slot, Particle& p) {
    if (ring_ == 0) return;
    const u32 head = p.trail & 0xFFu, count = (p.trail >> 8) & 0xFFu;
    const Vec4 newest = trail_[slot * ring_ + head];
    const Vec3 d = p.position - Vec3{newest.x, newest.y, newest.z};
    if (count > 0 && d.sizeSquared() < fx_->trailSegmentCm * fx_->trailSegmentCm) return;
    const u32 nh = count == 0 ? 0u : (head + 1u) % ring_;
    trail_[slot * ring_ + nh] = Vec4{p.position.x, p.position.y, p.position.z, p.age};
    p.trail = nh | (std::min(count + 1u, ring_) << 8);
}

u32 EmitterSim::spawn(const SpawnRequest& req) {
    if (!fx_) return 0;
    const ParticleEffect& fx = *fx_;
    resolveModules(fx.modules, user_, resolved_);
    spawnMods_.clear();
    for (const ResolvedModule& m : resolved_)
        if (m.stage == ModuleStage::Spawn) spawnMods_.push_back(m);

    std::vector<u32> birthRules;
    for (usize r = 0; r < fx.events.size(); ++r)
        if (fx.events[r].on == static_cast<u8>(EventOn::Spawn)) birthRules.push_back(static_cast<u32>(r));
    const auto noteBirth = [&](const Particle& p) {
        for (u32 r : birthRules) if (birthEvents_.size() < 4096) birthEvents_.push_back(makeEvent(p, EventOn::Spawn, r));
    };

    u32 accepted = 0;
    for (u32 i = 0; i < req.count; ++i) {
        const u32 id = nextId_++;
        if (linked_) {
            const usize slot = id % capacity_;
            particles_[slot] = make(req, id, spawnMods_);
            noteBirth(particles_[slot]);
            ++accepted;
            continue;
        }
        if (particles_.size() >= fx.maxParticles) continue;   // over budget drops the new spawn; nothing is evicted
        Particle p = make(req, id, spawnMods_);
        noteBirth(p);
        particles_.push_back(p);
        if (ring_ != 0) {
            const usize slot = particles_.size() - 1;
            trail_.resize(particles_.size() * ring_, Vec4{});
            trail_[slot * ring_] = Vec4{p.position.x, p.position.y, p.position.z, 0.0f};
            particles_.back().trail = 0u | (1u << 8);
        }
        ++accepted;
    }
    return accepted;
}

EventRecord EmitterSim::makeEvent(const Particle& p, EventType type, u32 rule) const {
    const ParticleEffect& fx = *fx_;
    EventRecord r;
    r.type = type;
    r.id = p.id;
    r.position = p.position;
    r.velocity = p.velocity;
    r.rule = rule;
    const f32 t = p.lifetime > 0.0f ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 1.0f;
    f32 c[4];
    evalColor(fx, t, c);
    const f32 a[4] = {(p.tint & 0xFFu) / 255.0f, ((p.tint >> 8) & 0xFFu) / 255.0f, ((p.tint >> 16) & 0xFFu) / 255.0f, (p.tint >> 24) / 255.0f};
    r.tint = packRgba8(c[0] * a[0], c[1] * a[1], c[2] * a[2], c[3] * a[3]);
    const f32 birth = evalSize(fx, 0.0f);
    r.sizeRatio = (birth > 1e-4f ? evalSize(fx, t) / birth : 1.0f) * p.sizeScale;
    r.ageFraction = t;
    return r;
}

void EmitterSim::step(f32 dt, f32 time, std::vector<EventRecord>* eventsOut) {
    if (!fx_) return;
    const ParticleEffect& fx = *fx_;
    if (eventsOut) eventsOut->insert(eventsOut->end(), birthEvents_.begin(), birthEvents_.end());
    birthEvents_.clear();
    std::vector<u32> lifeRules, timerRules;
    for (usize r = 0; r < fx.events.size(); ++r) {
        if (fx.events[r].on == static_cast<u8>(EventOn::Lifetime) && fx.events[r].at > 0.0f && fx.events[r].at < 1.0f) lifeRules.push_back(static_cast<u32>(r));
        else if (fx.events[r].on == static_cast<u8>(EventOn::Timer) && fx.events[r].at > 1e-3f) timerRules.push_back(static_cast<u32>(r));
    }
    if (!eventsOut) { lifeRules.clear(); timerRules.clear(); }
    resolveModules(fx.modules, user_, resolved_);
    velMods_.clear(); posMods_.clear();
    for (const ResolvedModule& m : resolved_) {
        if (m.stage == ModuleStage::Velocity) velMods_.push_back(m);
        else if (m.stage == ModuleStage::Position) posMods_.push_back(m);
    }
    bool wantDeath = false, wantCollide = false;
    for (const EventSpawn& e : fx.events) { wantDeath |= e.on == static_cast<u8>(EventOn::Death); wantCollide |= e.on == static_cast<u8>(EventOn::Collide); }
    if (!eventsOut) wantDeath = wantCollide = false;

    ModuleEnv env;
    env.origin = origin_;
    env.time = time;
    env.dt = dt;
    const f32 damp = fx.damping > 0.0f ? std::max(0.0f, 1.0f - fx.damping * dt) : 1.0f;

    // Velocity and position stages for a living particle. False when a position module kills it.
    const auto advance = [&](Particle& p) {
        p.velocity += fx.gravity * dt;
        if (fx.damping > 0.0f) p.velocity *= damp;
        for (const ResolvedModule& m : velMods_) runVelocityModule(m, env, p);
        p.position += p.velocity * dt;
        for (const ResolvedModule& m : posMods_) {
            const u32 bits = runPositionModule(m, env, p);
            if ((bits & kModuleCollided) && wantCollide) eventsOut->push_back(makeEvent(p, EventType::Collide, 0));
            if (bits & kModuleKilled) return false;
        }
        return true;
    };

    usize w = 0;
    for (usize r = 0; r < particles_.size(); ++r) {
        Particle p = particles_[r];
        if (linked_ && !particleAlive(p)) continue;   // an empty ring slot

        p.age += dt;
        if (p.age >= p.lifetime || !advance(p)) {
            // Dies here. A compacting array does not write it back, so it drops out; a linked slot is emptied in place.
            if (wantDeath) eventsOut->push_back(makeEvent(p, EventType::Death, 0));
            if (linked_) { particles_[r].age = 0.0f; particles_[r].lifetime = 0.0f; }
            continue;
        }
        p.rotation += p.angularVelocity * dt;

        // Lifetime-fraction and timer triggers, for a particle that survived the step: a crossing between the previous age and this one.
        for (u32 ri : lifeRules) {
            const f32 at = fx.events[ri].at * p.lifetime;
            if (p.age - dt < at && p.age >= at) eventsOut->push_back(makeEvent(p, EventOn::Lifetime, ri));
        }
        for (u32 ri : timerRules) {
            const f32 period = fx.events[ri].at;
            const f32 before = std::floor(std::max(0.0f, p.age - dt) / period), after = std::floor(p.age / period);
            for (f32 k = before + 1.0f; k <= after && k < before + 5.0f; k += 1.0f) eventsOut->push_back(makeEvent(p, EventOn::Timer, ri));
        }

        if (linked_) { particles_[r] = p; continue; }

        if (ring_ != 0) {
            // The trail ring moves with its particle under compaction.
            if (w != r) std::copy(trail_.begin() + static_cast<isize>(r * ring_),
                                  trail_.begin() + static_cast<isize>((r + 1) * ring_),
                                  trail_.begin() + static_cast<isize>(w * ring_));
            particles_[w] = p;
            pushTrail(w, particles_[w]);
        } else {
            particles_[w] = p;
        }
        ++w;
    }
    if (!linked_) {
        particles_.resize(w);
        if (ring_ != 0) trail_.resize(w * ring_);
    }
}

} // namespace aver::particles
