// The tick that turns every scene::CParticleEmitter (and every queued spawn-at request) into live particles. No
// RHI, no GPU, no device -- ParticleRenderer.hpp is the half that draws what this produces, the same split
// aver::anim::AnimSystem (no RHI) and render.skin's SkinnedScene (RHI) use.
//
// TWO TIERS, ONE DEFINITION. An emitter instance simulates either on the CPU (an EmitterSim per instance) or, when the
// renderer has installed a GPU tier (IParticleGpuTier) and the emitter is big enough or asks for it, on the GPU: the
// tick then produces plain-data GpuJobs the renderer drains in its prePass. The rate accumulator, the stopped ->
// playing burst edge and "over budget drops the new spawn" run here for both tiers, so rate behaviour is identical.
// A tier is chosen when an instance is created and never changes mid-life.
#pragma once
#include "aver/particles/EmitterSim.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleGpuTypes.hpp"
#include "aver/particles/ParticleSpawn.hpp"
#include "aver/particles/ParticleTypes.hpp"
#include "aver/scene/World.hpp"

#include <unordered_map>
#include <vector>

namespace aver::particles {

// One emitter's state as the 0.7 renderer saw it: its live particles and the effect they belong to, valid as of the
// last tick(). Kept for callers that only count particles; the renderer uses InstanceView.
struct EmitterView {
    scene::Entity entity;
    const ParticleEffect* effect;
    const std::vector<Particle>* particles;
};

// What the renderer needs to draw one emitter instance.
struct InstanceView {
    scene::Entity entity = scene::kInvalidEntity;   // invalid for a spawn-at event instance
    u64 effectId = 0;
    u32 emitterIndex = 0;
    const ParticleEffect* effect = nullptr;
    Vec3 center{0, 0, 0};                           // world-space bounds centre (the emitter's origin)
    const std::vector<Particle>* particles = nullptr;   // CPU tier; null on the GPU tier
    const std::vector<Vec4>* trail = nullptr;       // CPU trail ring, trailRing points per particle
    u32 trailRing = 0;
    bool linked = false;
    u32 gpuHandle = 0;                              // 0 on the CPU tier
    u32 aliveHint = 0;                              // exact on the CPU tier, up to three frames old on the GPU
};

// The GPU half the renderer installs. ParticleSystem never sees an RHI type.
class IParticleGpuTier {
public:
    virtual ~IParticleGpuTier() = default;
    // True while kernels, pipelines and the pool exist.
    virtual bool usable() const = 0;
    // A pool range for one emitter instance; 0 when refused (the instance then runs on the CPU). `parent` is the
    // handle whose events this instance consumes (0 none); it must have been acquired first.
    virtual u32 acquire(const ParticleEffect& fx, u32 parent) = 0;
    virtual void release(u32 handle) = 0;
    // Alive particles a few frames ago; never stalls.
    virtual u32 aliveApprox(u32 handle) const = 0;
    virtual u64 allocatedBytes() const = 0;
    // Pool capacity (particles) an instance of `fx` would take; the budget counts this.
    virtual u32 capacityFor(const ParticleEffect& fx) const = 0;
};

struct ParticleBudget {
    u32 maxGpuParticles = 2000000;
    u32 maxCpuParticles = 200000;
    u32 maxSpawnAtPerFrame = 2048;
};

struct ParticleStats {
    u32 gpuEmitters = 0, cpuEmitters = 0;
    u32 gpuParticles = 0, cpuParticles = 0;
    u32 spawnAtDropped = 0;
    u32 gpuAllocatedKB = 0;
};

class ParticleSystem {
public:
    ~ParticleSystem();

    // Advances every scene::CParticleEmitter's clock and its particles by `dt`, applies queued script operations and
    // spawn-at requests first. CALLED UNCONDITIONALLY, not from a gameplay tick group -- matching AnimSystem::tick's
    // reasoning: an artist scrubbing a preview outside Play mode should still see an effect play.
    void tick(scene::World& world, f32 dt);

    // The library tick() resolves CParticleEmitter::effect through. Not owned.
    void setEffectLibrary(ParticleEffectLibrary* lib) { effects_ = lib; }
    ParticleEffectLibrary* effectLibrary() const { return effects_; }

    // ---- tiers ----
    // Installs (or clears) the GPU tier. Existing GPU instances are dropped (their handles belong to the old tier).
    void setGpuTier(IParticleGpuTier* tier);
    // 0 off (everything on the CPU), 1 auto (the default), 2 on (every emitter that can, on the GPU).
    void setGpuMode(int mode) { gpuMode_ = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }
    int gpuMode() const { return gpuMode_; }
    bool gpuUsable() const { return gpuTier_ && gpuTier_->usable() && gpuMode_ != 0; }
    void setBudget(const ParticleBudget& b) { budget_ = b; }
    const ParticleBudget& budget() const { return budget_; }
    ParticleStats stats() const;

    // This tick's GPU work, in the order it must be recorded; the renderer clears it after recording.
    std::vector<GpuJob>& gpuJobs() { return gpuJobs_; }

    // ---- reads ----
    // Calls `fn(const EmitterView&)` once per CPU-tier emitter that has at least one live particle, in the order each
    // emitter was first seen. Entity instances only.
    template <class Fn>
    void forEachEmitter(Fn&& fn) const {
        for (scene::Entity e : order_) {
            auto it = systems_.find(e);
            if (it == systems_.end()) continue;
            const SystemInstance& si = it->second;
            const ParticleSystemDef* def = effects_ ? effects_->findSystem(si.effectId) : nullptr;
            if (!def) continue;
            for (usize i = 0; i < si.emitters.size() && i < def->emitters.size(); ++i) {
                const EmitterInstance& ei = si.emitters[i];
                if (ei.gpu || ei.sim.particles().empty()) continue;
                fn(EmitterView{e, &def->emitters[i], &ei.sim.particles()});
            }
        }
    }

    // Calls `fn(const InstanceView&)` once per live emitter instance (entity and spawn-at event instances, CPU and
    // GPU tier), in authored emitter order within each system. Skips instances with no effect resolved.
    template <class Fn>
    void forEachInstance(Fn&& fn) const {
        auto visit = [&](scene::Entity e, const SystemInstance& si) {
            const ParticleSystemDef* def = effects_ ? effects_->findSystem(si.effectId) : nullptr;
            if (!def) return;
            for (usize i = 0; i < si.emitters.size() && i < def->emitters.size(); ++i) {
                const EmitterInstance& ei = si.emitters[i];
                InstanceView v;
                v.entity = e;
                v.effectId = si.effectId;
                v.emitterIndex = static_cast<u32>(i);
                v.effect = &def->emitters[i];
                v.center = si.origin;
                v.gpuHandle = ei.gpu ? ei.gpuHandle : 0u;
                v.aliveHint = ei.aliveHint;
                if (!ei.gpu) {
                    v.particles = &ei.sim.particles();
                    v.trail = &ei.sim.trail();
                    v.trailRing = ei.sim.trailRing();
                    v.linked = ei.sim.linked();
                } else {
                    v.linked = def->emitters[i].render == RenderKind::Ribbon && def->emitters[i].linked;
                }
                fn(v);
            }
        };
        for (scene::Entity e : order_) {
            auto it = systems_.find(e);
            if (it != systems_.end()) visit(e, it->second);
        }
        for (const auto& kv : eventSystems_) visit(scene::kInvalidEntity, kv.second);
    }

    u32 liveEmitters() const { return static_cast<u32>(systems_.size()); }
    // Alive particles of this entity's emitter 0 (exact on the CPU tier, a few frames old on the GPU).
    u32 liveParticles(scene::Entity e) const;
    // This entity's emitter-0 CPU particles, or nullptr if it carries no emitter this system has ticked or runs on
    // the GPU. Valid until the next tick().
    const std::vector<Particle>* particles(scene::Entity e) const;

    // ---- script operations (queued, applied at the top of the next tick in call order) ----
    void setParam(scene::Entity e, u64 nameHash, u8 kind, const f32 v[4]);
    void burst(scene::Entity e, i32 count);
    void stop(scene::Entity e, bool killParticles);
    void setModifiers(scene::Entity e, f32 rateScale, f32 sizeScale, f32 speedScale);
    // alive: emitter-0..n total; tier: 0 cpu, 1 gpu. False when the entity has no ticked emitter.
    bool emitterState(scene::Entity e, u32& alive, u32& tier) const;

    // ---- spawn-at ----
    u32 spawnAt(const SpawnAtParams& p);

    // Entities whose AutoDestroy emitter stopped and drained this tick (already queued for destruction). The host
    // raises Particles.EffectFinished for each and clears the list.
    std::vector<scene::Entity>& finished() { return finished_; }

    // Drops every instance, queued job and queued request (Play stop, level change, shutdown) and releases GPU ranges.
    void reset();

private:
    struct EmitterInstance {
        EmitterSim sim;
        bool gpu = false;
        u32 gpuHandle = 0;
        u32 gpuParent = 0;
        u32 gpuDepth = 0;
        u32 gpuCap = 0;                        // pool capacity this instance holds against the budget
        u32 seed = 1;                          // the emitter seed (the GPU twin of EmitterSim::init's)
        u32 evType = 0, evCount = 0;           // the parent rule this GPU instance consumes
        f32 evInherit = 0;
        f32 accum = 0;                         // emitters 1..n; emitter 0 uses CParticleEmitter::emitAccum
        u32 nextId = 0;                        // GPU: the id counter the CPU owns
        u32 aliveHint = 0;
        std::vector<SpawnRequest> inbox;       // spawns to apply in this step (spawn-at requests)
        std::vector<SpawnRequest> nextInbox;   // event spawns routed during this tick; applied one tick later
        std::vector<EventRecord> events;       // scratch
    };
    struct SystemInstance {
        u64 effectId = 0;
        std::vector<EmitterInstance> emitters;
        f32 user[kMaxUserParams][4] = {};
        Vec3 origin{0, 0, 0};
        Vec3 prevOrigin{0, 0, 0};
        Vec3 velocity{0, 0, 0};
        bool havePrev = false;
        bool wasPlaying = false;
        bool burstFired = false;
        f32 age = 0;                           // event instances own their clock; entity instances use the component's
        f32 idle = 0;
        f32 rateScale = 1, sizeScale = 1, speedScale = 1;
        i32 burstExtra = 0;
        bool rebuild = false;
    };
    struct EntityOp {
        enum Kind : u8 { SetParam, Burst, Stop, Modifiers } kind = SetParam;
        scene::Entity entity = scene::kInvalidEntity;
        u64 nameHash = 0;
        u8 paramKind = 0;
        f32 v[4] = {0, 0, 0, 0};
        i32 count = 0;
    };
    struct StepCtx {
        f32 dt = 0, time = 0;
        bool playing = false;       // emission allowed
        bool fireBurst = false;     // the stopped -> playing edge's authored bursts
        f32* accum0 = nullptr;      // emitter 0's accumulator (the component's emitAccum), or null
        Mat4 xf;
    };

    void buildInstance(SystemInstance& si, const ParticleSystemDef& def, u32 seed, u32 flags);
    void releaseInstance(SystemInstance& si);
    void releaseEmitter(EmitterInstance& ei);
    void applyOps(scene::World& world);
    void applyOp(const EntityOp& op, SystemInstance& si, const ParticleSystemDef& def, u32& flagsOut, bool& stopOut);
    void stepSystem(SystemInstance& si, const ParticleSystemDef& def, const StepCtx& cx);
    void emitGpuJob(SystemInstance& si, const ParticleSystemDef& def, usize emitter, const StepCtx& cx,
                    const std::vector<SpawnRequest>& reqs);
    bool wantGpu(const ParticleEffect& fx, u32 componentFlags) const;

    std::vector<scene::Entity> order_;
    std::unordered_map<scene::Entity, SystemInstance> systems_;
    std::unordered_map<u64, SystemInstance> eventSystems_;
    std::vector<EntityOp> ops_;
    struct SpawnAtRequest {
        u64 effect = 0;
        SpawnRequest req;
    };
    std::vector<SpawnAtRequest> spawnAtQueue_;
    std::vector<GpuJob> gpuJobs_;
    std::vector<scene::Entity> finished_;
    std::unordered_map<u64, bool> warnedEffects_;
    ParticleEffectLibrary* effects_ = nullptr;
    IParticleGpuTier* gpuTier_ = nullptr;
    int gpuMode_ = 1;
    ParticleBudget budget_;
    u32 spawnAtThisFrame_ = 0;
    u32 spawnAtDropped_ = 0;
    u32 spawnAtCounter_ = 0;
    u32 gpuCapacityUsed_ = 0;
    u32 cpuCountCache_ = 0;
    u32 cpuSpawnRoom_ = 0;
};

// The process-global system, matching scene::World::instance() / aver::anim::animSystem().
ParticleSystem& particleSystem();

} // namespace aver::particles
