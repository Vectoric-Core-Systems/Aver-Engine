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
};

enum class EventType : u8 { Death = 0, Collide = 1 };

// A death or collide the step observed, for the parent emitter's ONEVENT rules.
struct EventRecord {
    EventType type = EventType::Death;
    u32 id = 0;
    Vec3 position;
    Vec3 velocity;
};

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

private:
    Particle make(const SpawnRequest& req, u32 id, const std::vector<ResolvedModule>& spawnMods) const;
    void pushTrail(usize slot, Particle& p);

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
