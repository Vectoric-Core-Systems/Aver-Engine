// The CPU executor: one emitter instance's particles, spawned and stepped from a ParticleEffect and its module
// stack. Scene-free and RHI-free, so a caller (ParticleSystem per entity, the editor preview, a test) owns one
// directly. It is the old spawnParticle / updateParticles rebuilt on the module table, and the reference the GPU
// kernels are tested against: the same counter-based RNG, the same per-tick order (ParticleModules.hpp).
#pragma once
#include "aver/particles/ParticleModules.hpp"
#include "aver/particles/ParticleTypes.hpp"

#include <vector>

namespace aver::particles {

// A request to create `count` particles at a transform. `xf` places the shape and rotates the authored direction.
struct SpawnRequest {
    Mat4 xf;
    Vec3 velocity{0, 0, 0};      // the emitter's own velocity, scaled by spawn.inheritvelocity
    Vec3 addVelocity{0, 0, 0};   // added unconditionally (event children inheriting their parent's velocity)
    u32  count = 1;
    f32  speedScale = 1, sizeScale = 1, lifeScale = 1;
    u32  tint = 0xFFFFFFFFu;     // RGBA8; spawn.colortint replaces it
    u32  seed = 0;               // mixed into the emitter seed; 0 leaves it alone
    f32  ageFraction = 0;        // the particle is born this far through its life (event payload)
};

using EventType = EventOn;

// An event the step observed, for the parent emitter's event rules. `rule` indexes ParticleEffect::events for the
// Spawn / Lifetime / Timer triggers (several rules can share a kind); Death and Collide match by type.
struct EventRecord {
    EventType type = EventType::Death;
    u32 id = 0;
    Vec3 position;
    Vec3 velocity;
    u32 rule = 0;
    u32 tint = 0xFFFFFFFFu;   // the particle's colour at the event, RGBA8 (the payload)
    f32 sizeRatio = 1.0f;     // its size relative to its birth size, times its own scale
    f32 ageFraction = 0.0f;   // how far through its life it was
};

// Fills a child's spawn request from the parent's event record and the rule's payload bits and user scalar.
inline void applyEventPayload(const EventSpawn& rule, const EventRecord& ev, SpawnRequest& cr) {
    if (rule.payload & kEventPayloadColor) cr.tint = ev.tint;
    if (rule.payload & kEventPayloadSize) cr.sizeScale = ev.sizeRatio;
    if (rule.payload & kEventPayloadAge) cr.ageFraction = ev.ageFraction;
    if (rule.user != 0.0f) cr.speedScale = rule.user;
}

class EmitterSim {
public:
    // `fx` must outlive this object or be rebound (bind) before the next spawn/step; the instance keeps a pointer so
    // an edited effect takes effect on the next tick.
    void init(const ParticleEffect& fx, const std::vector<UserParam>& params, u32 emitterSeed);
    void bind(const ParticleEffect& fx) { fx_ = &fx; }

    void setUser(u32 slot, const f32 v[4]);
    const f32* userData() const { return &user_[0][0]; }   // kMaxUserParams x 4, flat

    // The emitter's world position for the update stages that are emitter-relative.
    void setOrigin(const Vec3& o) { origin_ = o; }

    // Creates up to req.count particles; returns how many were accepted (cap = maxParticles, or the ring for a
    // linked ribbon, which overwrites its oldest). Every requested particle consumes an id, accepted or not.
    u32 spawn(const SpawnRequest& req);

    // Ages, integrates and runs the update modules by dt; `time` is the emitter's age (scrolls curl noise).
    // Death and collide events matching an ONEVENT rule are appended to *eventsOut when it is non-null.
    // Compaction keeps the surviving order (a linked ribbon never compacts).
    void step(f32 dt, f32 time, std::vector<EventRecord>* eventsOut);
    // Spawn-trigger records for particles created since the last step are delivered by that step.

    // Live particles; for a linked ribbon the whole slot ring, dead slots having age >= lifetime.
    const std::vector<Particle>& particles() const { return particles_; }
    u32 aliveCount() const;
    u32 nextId() const { return nextId_; }
    bool linked() const { return linked_; }
    u32 capacity() const { return capacity_; }

    // Trail ring (trail ribbons): `trailRing()` points per particle slot, particle i owning
    // [i * trailRing(), (i + 1) * trailRing()). Empty for any other render kind.
    const std::vector<Vec4>& trail() const { return trail_; }
    u32 trailRing() const { return ring_; }

    void clear();

    // The floating origin moved (LARGE_WORLD 2.2.6): the emitter origin, every particle slot and every trail point are
    // `delta` smaller (plain f32 subtraction, the same one the scene's roots take). A trail point's w, an age, is kept.
    void shiftWorld(const Vec3& delta);

private:
    Particle make(const SpawnRequest& req, u32 id, const std::vector<ResolvedModule>& spawnMods) const;
    void pushTrail(usize slot, Particle& p);

    EventRecord makeEvent(const Particle& p, EventType type, u32 rule) const;

    std::vector<EventRecord> birthEvents_;   // Spawn-trigger records waiting for the next step
    const ParticleEffect* fx_ = nullptr;
    u32 emitterSeed_ = 1;
    f32 user_[kMaxUserParams][4] = {};
    Vec3 origin_{0, 0, 0};

    std::vector<Particle> particles_;
    std::vector<Vec4> trail_;
    u32 ring_ = 0;           // trail points stored per slot (0 unless a trail ribbon)
    bool linked_ = false;
    u32 capacity_ = 0;       // linked ring size
    u32 nextId_ = 0;
    std::vector<ResolvedModule> resolved_, spawnMods_, velMods_, posMods_;   // scratch
};

} // namespace aver::particles
