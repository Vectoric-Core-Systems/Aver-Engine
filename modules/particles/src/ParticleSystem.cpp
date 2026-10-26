// The tick that turns a scene::CParticleEmitter's clock (and queued spawn-at requests) into live particles.
//
// Effect-specific behaviour comes from ParticleEffect's own numbers, never from a branch on what kind of effect this is.
// A rising smoke column, falling snow and a waterfall's mist are different values of the same loop.
#include "aver/particles/ParticleSystem.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleRandom.hpp"
#include "aver/scene/ComponentPool.hpp"

#include <algorithm>
#include <cstring>

namespace aver::particles {
namespace {

constexpr f32 kEventInstanceIdleSeconds = 5.0f;
constexpr usize kMaxGpuJobs = 4096;
constexpr usize kMaxInboxPerEmitter = 4096;

// A stable per-entity seed, so replaying the SAME scene reproduces the SAME particles. 0 is reserved
// (CParticleEmitter::seed == 0 means "not yet assigned"), so a hash landing on it is nudged to 1.
u32 seedFor(scene::Entity e) {
    const u32 h = hash32(static_cast<u32>(e));
    return h == 0 ? 1u : h;
}

u32 bitsOf(f32 f) { u32 u; std::memcpy(&u, &f, sizeof u); return u; }

// Alive particles of one emitter instance: the GPU tier's value is a few frames old, the CPU count is exact.
template <class EmitterInstanceT>
u32 aliveOf(const EmitterInstanceT& ei) { return ei.gpu ? ei.aliveHint : ei.sim.aliveCount(); }

// Event relationships inside one system: the first emitter whose rule names a child is that child's parent.
struct Topology {
    std::vector<int> parent, rule, depth, order;
};
Topology topologyOf(const ParticleSystemDef& def) {
    const usize n = def.emitters.size();
    Topology t;
    t.parent.assign(n, -1);
    t.rule.assign(n, -1);
    t.depth.assign(n, 0);
    for (usize p = 0; p < n; ++p)
        for (usize r = 0; r < def.emitters[p].events.size(); ++r) {
            const usize c = def.emitters[p].events[r].emitter;
            if (c < n && c != p && t.parent[c] < 0) { t.parent[c] = static_cast<int>(p); t.rule[c] = static_cast<int>(r); }
        }
    for (usize i = 0; i < n; ++i) {
        int d = 0, cur = static_cast<int>(i);
        while (t.parent[static_cast<usize>(cur)] >= 0 && d <= static_cast<int>(n)) { cur = t.parent[static_cast<usize>(cur)]; ++d; }
        t.depth[i] = d;
        t.order.push_back(static_cast<int>(i));
    }
    std::stable_sort(t.order.begin(), t.order.end(), [&](int a, int b) { return t.depth[static_cast<usize>(a)] < t.depth[static_cast<usize>(b)]; });
    return t;
}

bool hasEvents(const ParticleSystemDef& def) {
    for (const ParticleEffect& e : def.emitters) if (!e.events.empty()) return true;
    return false;
}

void packRequest(const SpawnRequest& r, u32 count, GpuSpawnRequest& out) {
    std::memcpy(out.xf, &r.xf.m[0][0], sizeof out.xf);
    out.velocity[0] = r.velocity.x; out.velocity[1] = r.velocity.y; out.velocity[2] = r.velocity.z;
    out.count = count;
    out.addVelocity[0] = r.addVelocity.x; out.addVelocity[1] = r.addVelocity.y; out.addVelocity[2] = r.addVelocity.z;
    out.speedScale = r.speedScale;
    out.sizeScale = r.sizeScale;
    out.lifeScale = r.lifeScale;
    out.tint = r.tint;
    out.seed = r.seed;
}

} // namespace

u32 packGpuModules(const ParticleEffect& fx, u32 (*outRows)[4]) {
    u32 n = 0;
    for (const Module& m : fx.modules) {
        if (!m.enabled || static_cast<u32>(m.kind) >= static_cast<u32>(ModuleKind::Count) || n >= kGpuMaxModules) continue;
        u32* r0 = outRows[n * kGpuModuleRows + 0];
        u32* r1 = outRows[n * kGpuModuleRows + 1];
        u32* r2 = outRows[n * kGpuModuleRows + 2];
        u32 refs[kMaxModuleParams] = {};
        for (u32 i = 0; i < kMaxModuleParams; ++i) {
            const ModuleParam& p = m.p[i];
            (i < 4 ? r0 : r1)[i & 3u] = bitsOf(p.value);
            if (p.userSlot >= 0 && static_cast<u32>(p.userSlot) < kMaxUserParams)
                refs[i] = 1u + static_cast<u32>(p.userSlot) * 4u + (p.comp & 3u);
        }
        r2[0] = refs[0] | (refs[1] << 8) | (refs[2] << 16) | (refs[3] << 24);
        r2[1] = refs[4] | (refs[5] << 8) | (refs[6] << 16) | (refs[7] << 24);
        r2[2] = static_cast<u32>(m.kind);
        r2[3] = static_cast<u32>(moduleInfo(m.kind).stage);
        ++n;
    }
    return n;
}

ParticleSystem::~ParticleSystem() { reset(); }

void ParticleSystem::releaseAllInstances() {
    for (auto& kv : systems_) releaseInstance(kv.second);
    for (auto& kv : eventSystems_) releaseInstance(kv.second);
    systems_.clear();
    eventSystems_.clear();
    order_.clear();
    gpuJobs_.clear();
    gpuShifts_.clear();
}

void ParticleSystem::setGpuTier(IParticleGpuTier* tier) {
    if (tier == gpuTier_) return;
    // Existing GPU instances belong to the old tier: drop them (they restart on whichever tier is chosen next).
    releaseAllInstances();
    gpuTier_ = tier;
}

bool ParticleSystem::wantGpu(const ParticleEffect& fx, u32 componentFlags) const {
    if (!gpuUsable()) return false;
    if (componentFlags & scene::kParticleEmitterForceCpu) return false;
    if (componentFlags & scene::kParticleEmitterForceGpu) return true;
    switch (fx.simTier) {
    case SimTier::Cpu: return false;
    case SimTier::Gpu: return true;
    case SimTier::Auto:
    default: {
        const u32 size = (fx.render == RenderKind::Ribbon && fx.linked) ? fx.linkedCapacity : fx.maxParticles;
        return gpuMode_ == 2 || size >= 2048u;
    }
    }
}

void ParticleSystem::releaseEmitter(EmitterInstance& ei) {
    if (ei.gpu && ei.gpuHandle && gpuTier_) gpuTier_->release(ei.gpuHandle);
    gpuCapacityUsed_ = gpuCapacityUsed_ >= ei.gpuCap ? gpuCapacityUsed_ - ei.gpuCap : 0u;
    ei.gpu = false;
    ei.gpuHandle = 0;
    ei.gpuParent = 0;
    ei.gpuCap = 0;
}

void ParticleSystem::releaseInstance(SystemInstance& si) {
    for (EmitterInstance& ei : si.emitters) releaseEmitter(ei);
}

void ParticleSystem::buildInstance(SystemInstance& si, const ParticleSystemDef& def, u32 seed, u32 flags) {
    releaseInstance(si);
    const usize n = def.emitters.size();
    si.emitters.clear();
    si.emitters.resize(n);
    for (u32 i = 0; i < kMaxUserParams; ++i)
        for (u32 k = 0; k < 4; ++k) si.user[i][k] = 0.0f;
    for (usize i = 0; i < def.params.size() && i < kMaxUserParams; ++i)
        for (u32 k = 0; k < 4; ++k) si.user[i][k] = def.params[i].v[k];

    std::vector<bool> want(n, false);
    bool anyWant = false;
    for (usize i = 0; i < n; ++i) { want[i] = wantGpu(def.emitters[i], flags); anyWant = anyWant || want[i]; }
    const bool linkedGroup = hasEvents(def);
    if (linkedGroup) for (usize i = 0; i < n; ++i) want[i] = anyWant;   // event-linked emitters share a tier

    const Topology topo = topologyOf(def);
    bool groupFailed = false;
    for (int oi : topo.order) {
        const usize i = static_cast<usize>(oi);
        EmitterInstance& ei = si.emitters[i];
        const u32 emitterSeed = i == 0 ? seed : hash32(seed ^ (static_cast<u32>(i) * 0x85EBCA6Bu));
        ei.sim.init(def.emitters[i], def.params, emitterSeed);
        ei.seed = emitterSeed == 0 ? 1u : emitterSeed;
        if (!want[i] || groupFailed) continue;

        const ParticleEffect& fx = def.emitters[i];
        const u32 cap = gpuTier_->capacityFor(fx);
        u32 parentHandle = 0;
        const int p = topo.parent[i];
        if (p >= 0) parentHandle = si.emitters[static_cast<usize>(p)].gpuHandle;
        bool ok = cap != 0 && gpuCapacityUsed_ + cap <= budget_.maxGpuParticles && (p < 0 || parentHandle != 0);
        u32 handle = 0;
        if (ok) handle = gpuTier_->acquire(fx, parentHandle);
        if (!handle) {
            if (warnedEffects_.emplace(si.effectId ^ 0x6770755F61637175ull, true).second)
                AVER_WARN("[Particles] effect {} emitter {} stays on the CPU (GPU budget exhausted or allocation refused)",
                          si.effectId, i);
            if (linkedGroup) groupFailed = true;
            continue;
        }
        ei.gpu = true;
        ei.gpuHandle = handle;
        ei.gpuParent = parentHandle;
        ei.gpuDepth = static_cast<u32>(topo.depth[i]);
        ei.gpuCap = cap;
        gpuCapacityUsed_ += cap;
        if (p >= 0 && topo.rule[i] >= 0) {
            const EventSpawn& rule = def.emitters[static_cast<usize>(p)].events[static_cast<usize>(topo.rule[i])];
            ei.evType = rule.on;
            ei.evCount = rule.count;
            ei.evInherit = rule.inheritVelocity;
        }
    }
    if (groupFailed) {
        for (EmitterInstance& ei : si.emitters) releaseEmitter(ei);
    }
}

void ParticleSystem::shiftWorld(const rhi::ShiftVec3d& deltaCm) {
    if (deltaCm.x == 0.0 && deltaCm.y == 0.0 && deltaCm.z == 0.0) return;
    const Vec3 d{static_cast<f32>(deltaCm.x), static_cast<f32>(deltaCm.y), static_cast<f32>(deltaCm.z)};
    const auto shiftRequest = [&](SpawnRequest& r) { r.xf.m[3][0] -= d.x; r.xf.m[3][1] -= d.y; r.xf.m[3][2] -= d.z; };
    const auto shiftInstance = [&](SystemInstance& si) {
        si.origin -= d;
        si.prevOrigin -= d;   // else the next tick reads a 16 km "velocity" and every spawn inherits it
        for (EmitterInstance& ei : si.emitters) {
            if (ei.gpu) {
                if (gpuTier_ && ei.gpuHandle != 0) {
                    GpuShift s;
                    s.handle = ei.gpuHandle;
                    s.delta[0] = d.x; s.delta[1] = d.y; s.delta[2] = d.z;
                    gpuShifts_.push_back(s);
                }
            } else {
                ei.sim.shiftWorld(d);
            }
            for (SpawnRequest& r : ei.inbox) shiftRequest(r);
            for (SpawnRequest& r : ei.nextInbox) shiftRequest(r);
            for (EventRecord& ev : ei.events) ev.position -= d;   // scratch, rebuilt by the next step; kept consistent anyway
        }
    };
    for (auto& kv : systems_) shiftInstance(kv.second);
    for (auto& kv : eventSystems_) shiftInstance(kv.second);
    for (SpawnAtRequest& q : spawnAtQueue_) shiftRequest(q.req);
    // A job this tick built and the renderer has not recorded yet is in the old frame too (normally none: the shift runs
    // at the top of the update, after the last frame's prePass drained them).
    for (GpuJob& j : gpuJobs_) {
        j.center[0] -= d.x; j.center[1] -= d.y; j.center[2] -= d.z;
        j.cb.h.origin[0] -= d.x; j.cb.h.origin[1] -= d.y; j.cb.h.origin[2] -= d.z;
        for (u32 i = 0; i < j.cb.h.requestCount && i < kGpuMaxRequests; ++i) {
            j.cb.req[i].xf[12] -= d.x; j.cb.req[i].xf[13] -= d.y; j.cb.req[i].xf[14] -= d.z;
        }
        for (GpuSpawnRequest& r : j.extra) { r.xf[12] -= d.x; r.xf[13] -= d.y; r.xf[14] -= d.z; }
    }
}

void ParticleSystem::reset() {
    releaseAllInstances();
    ops_.clear();
    spawnAtQueue_.clear();
    finished_.clear();
    spawnAtThisFrame_ = 0;
    gpuCapacityUsed_ = 0;
    cpuCountCache_ = 0;
}

// ---- script operations ----
void ParticleSystem::setParam(scene::Entity e, u64 nameHash, u8 kind, const f32 v[4]) {
    EntityOp op;
    op.kind = EntityOp::SetParam; op.entity = e; op.nameHash = nameHash; op.paramKind = kind;
    for (int i = 0; i < 4; ++i) op.v[i] = v[i];
    ops_.push_back(op);
}
void ParticleSystem::burst(scene::Entity e, i32 count) {
    EntityOp op;
    op.kind = EntityOp::Burst; op.entity = e; op.count = count;
    ops_.push_back(op);
}
void ParticleSystem::stop(scene::Entity e, bool killParticles) {
    EntityOp op;
    op.kind = EntityOp::Stop; op.entity = e; op.count = killParticles ? 1 : 0;
    ops_.push_back(op);
}
void ParticleSystem::setModifiers(scene::Entity e, f32 rateScale, f32 sizeScale, f32 speedScale) {
    EntityOp op;
    op.kind = EntityOp::Modifiers; op.entity = e;
    op.v[0] = rateScale; op.v[1] = sizeScale; op.v[2] = speedScale;
    ops_.push_back(op);
}

void ParticleSystem::applyOp(const EntityOp& op, SystemInstance& si, const ParticleSystemDef& def, bool& stopOut) {
    switch (op.kind) {
    case EntityOp::SetParam:
        for (usize i = 0; i < def.params.size() && i < kMaxUserParams; ++i) {
            if (fnv1a64(std::string_view(def.params[i].name)) != op.nameHash) continue;
            const u32 comps = op.paramKind == 0 ? 1u : (op.paramKind == 1 ? 3u : 4u);
            for (u32 k = 0; k < comps; ++k) si.user[i][k] = op.v[k];
            break;
        }
        break;
    case EntityOp::Burst:
        if (op.count > 0) si.burstExtra += op.count;
        break;
    case EntityOp::Stop:
        stopOut = true;
        if (op.count != 0) si.rebuild = true;
        break;
    case EntityOp::Modifiers:
        si.rateScale = std::max(0.0f, op.v[0]);
        si.sizeScale = std::max(0.0f, op.v[1]);
        si.speedScale = std::max(0.0f, op.v[2]);
        break;
    }
}

void ParticleSystem::applyOps(scene::World& world) {
    if (ops_.empty()) return;
    std::vector<EntityOp> keep;
    for (const EntityOp& op : ops_) {
        auto it = systems_.find(op.entity);
        if (it == systems_.end()) {
            if (world.valid(op.entity) && !world.destroyPending(op.entity)) keep.push_back(op);   // not ticked yet
            continue;
        }
        const ParticleSystemDef* def = effects_ ? effects_->findSystem(it->second.effectId) : nullptr;
        if (!def) continue;
        bool stopped = false;
        applyOp(op, it->second, *def, stopped);
        if (stopped) {
            auto* c = world.component<scene::CParticleEmitter>(op.entity, scene::kComponentParticleEmitter);
            if (c) c->flags |= scene::kParticleEmitterStopped;
        }
    }
    ops_ = std::move(keep);
    if (ops_.size() > 4096) ops_.erase(ops_.begin(), ops_.end() - 4096);
}

// ---- spawn-at ----
u32 ParticleSystem::spawnAt(const SpawnAtParams& p) {
    if (!effects_ || p.count == 0) return 0;
    const ParticleSystemDef* def = effects_->findSystem(p.effect);
    if (!def) {
        if (warnedEffects_.emplace(p.effect, true).second)
            AVER_WARN("[Particles] spawnAt: no effect registered under id {}", p.effect);
        return 0;
    }
    u32 participants = 0, perEmitter = p.count;
    for (const ParticleEffect& fx : def->emitters) {
        if (!fx.spawnAtTarget) continue;
        ++participants;
        perEmitter = std::min(perEmitter, std::max(1u, fx.maxParticles));
    }
    if (participants == 0) return 0;
    const u32 remaining = budget_.maxSpawnAtPerFrame > spawnAtThisFrame_ ? budget_.maxSpawnAtPerFrame - spawnAtThisFrame_ : 0u;
    if (static_cast<u64>(perEmitter) * participants > remaining) perEmitter = remaining / participants;
    if (perEmitter == 0) {
        spawnAtDropped_ += p.count * participants;
        return 0;
    }
    SpawnAtRequest q;
    q.effect = p.effect;
    q.req.xf = spawnAtTransform(p.position, p.direction);
    q.req.velocity = p.inheritedVelocity;
    q.req.count = perEmitter;
    q.req.speedScale = p.speedScale;
    q.req.sizeScale = p.sizeScale;
    q.req.lifeScale = p.lifetimeScale;
    q.req.tint = packRgba8(p.tint[0], p.tint[1], p.tint[2], p.tint[3]);
    q.req.seed = p.seed != 0 ? p.seed : hash32(++spawnAtCounter_ * 0x9E3779B1u) | 1u;
    spawnAtQueue_.push_back(q);
    const u32 total = perEmitter * participants;
    spawnAtThisFrame_ += total;
    if (spawnAtQueue_.size() > 65536) spawnAtQueue_.erase(spawnAtQueue_.begin(), spawnAtQueue_.begin() + 32768);
    return total;
}

// ---- the per-system step shared by entity emitters and spawn-at event instances ----
void ParticleSystem::emitGpuJob(SystemInstance& si, const ParticleSystemDef& def, usize emitter, const StepCtx& cx,
                                const std::vector<SpawnRequest>& reqs) {
    EmitterInstance& ei = si.emitters[emitter];
    const ParticleEffect& fx = def.emitters[emitter];
    if (gpuJobs_.size() >= kMaxGpuJobs) return;

    const bool linked = fx.render == RenderKind::Ribbon && fx.linked;
    const bool trail = fx.render == RenderKind::Ribbon && !fx.linked;
    const u32 capacity = linked ? std::clamp<u32>(fx.linkedCapacity, 2u, kMaxLinkedCapacity) : std::max(1u, fx.maxParticles);

    gpuJobs_.emplace_back();
    GpuJob& job = gpuJobs_.back();
    job.handle = ei.gpuHandle;
    job.parent = ei.gpuParent;
    job.order = ei.gpuDepth;
    job.center[0] = si.origin.x; job.center[1] = si.origin.y; job.center[2] = si.origin.z;
    job.boundsRadius = fx.boundsRadiusCm;
    job.cullDistance = fx.cullDistanceCm;

    GpuSimHeader& h = job.cb.h;
    h.dt = cx.dt;
    h.time = cx.time;
    h.capacity = capacity;
    h.emitterSeed = ei.seed;
    h.gravity[0] = fx.gravity.x; h.gravity[1] = fx.gravity.y; h.gravity[2] = fx.gravity.z;
    h.damping = fx.damping;
    h.origin[0] = si.origin.x; h.origin[1] = si.origin.y; h.origin[2] = si.origin.z;
    h.flags = (linked ? kGpuFlagLinked : 0u) | (trail ? kGpuFlagTrail : 0u);
    for (const EventSpawn& ev : fx.events) h.flags |= ev.on == 0 ? kGpuFlagDeathEvent : kGpuFlagCollideEvent;
    if (fx.subuvFrames > 1 && fx.subuvFps > 0.0f) h.flags |= kGpuFlagSubuvStart;
    h.shape = static_cast<u32>(fx.shape);
    h.shell = fx.shapeShell ? 1u : 0u;
    h.shapeSize[0] = fx.shapeSize.x; h.shapeSize[1] = fx.shapeSize.y; h.shapeSize[2] = fx.shapeSize.z;
    h.spreadDeg = fx.spreadDeg;
    h.direction[0] = fx.direction.x; h.direction[1] = fx.direction.y; h.direction[2] = fx.direction.z;
    h.speedMin = fx.speedMin; h.speedMax = fx.speedMax;
    h.lifeMin = fx.lifetimeMin; h.lifeMax = fx.lifetimeMax;
    h.trailRing = trail ? std::clamp<u32>(fx.trailPoints, 2u, kMaxTrailPoints) - 1u : 0u;
    h.trailSegment = fx.trailSegmentCm;
    h.subuvFrames = fx.subuvFrames;
    h.subuvFps = fx.subuvFps;
    if (ei.gpuParent != 0) {
        h.evType = ei.evType;
        h.evCount = ei.evCount;
        h.evInherit = ei.evInherit;
        h.evActive = 1;
    }
    h.drawMult = trail ? h.trailRing : 1u;

    for (u32 i = 0; i < kMaxUserParams; ++i)
        for (u32 k = 0; k < 4; ++k) job.cb.user[i][k] = si.user[i][k];
    h.moduleCount = packGpuModules(fx, job.cb.mod);

    u32 total = 0;
    for (usize i = 0; i < reqs.size(); ++i) {
        const u32 count = std::min(reqs[i].count, capacity);
        if (count == 0) continue;
        GpuSpawnRequest gr;
        packRequest(reqs[i], count, gr);
        if (h.requestCount < kGpuMaxRequests) {
            job.cb.req[h.requestCount++] = gr;
            h.spawnTotal += count;
        } else {
            job.extra.push_back(gr);
        }
        total += count;
    }
    h.firstId = ei.nextId;
    ei.nextId += total;
    ei.aliveHint = gpuTier_ ? gpuTier_->aliveApprox(ei.gpuHandle) : 0u;
}

void ParticleSystem::stepSystem(SystemInstance& si, const ParticleSystemDef& def, const StepCtx& cx) {
    const usize n = std::min(si.emitters.size(), def.emitters.size());
    // Events routed during the PREVIOUS tick become spawns now, for every emitter before any steps: one tick of latency
    // on both tiers whatever the emitter order.
    for (usize i = 0; i < n; ++i) {
        EmitterInstance& ei = si.emitters[i];
        ei.inbox.insert(ei.inbox.end(), ei.nextInbox.begin(), ei.nextInbox.end());
        ei.nextInbox.clear();
    }

    std::vector<SpawnRequest> reqs;
    for (usize i = 0; i < n; ++i) {
        EmitterInstance& ei = si.emitters[i];
        const ParticleEffect& fx = def.emitters[i];
        reqs.clear();
        reqs.swap(ei.inbox);

        u32 toSpawn = 0;
        if (cx.playing) {
            if (cx.fireBurst) toSpawn += fx.burstCount;
            if (fx.emissionRate > 0.0f) {
                f32& acc = (i == 0 && cx.accum0) ? *cx.accum0 : ei.accum;
                acc += fx.emissionRate * cx.dt * si.rateScale;
                const u32 whole = static_cast<u32>(acc);
                toSpawn += whole;
                acc -= static_cast<f32>(whole);
            }
        }
        if (si.burstExtra > 0) toSpawn += static_cast<u32>(si.burstExtra);

        // OVER BUDGET DROPS NEW SPAWNS; IT DOES NOT EVICT OLD ONES. The CPU tier is bounded by the room left this tick.
        if (!ei.gpu) toSpawn = std::min(toSpawn, cpuSpawnRoom_);
        if (toSpawn > 0) {
            SpawnRequest r;
            r.xf = cx.xf;
            r.velocity = si.velocity;
            r.count = toSpawn;
            r.speedScale = si.speedScale;
            r.sizeScale = si.sizeScale;
            reqs.push_back(r);
        }

        if (ei.gpu) {
            emitGpuJob(si, def, i, cx, reqs);
            continue;
        }

        ei.sim.bind(fx);
        for (usize s = 0; s < def.params.size() && s < kMaxUserParams; ++s) ei.sim.setUser(static_cast<u32>(s), si.user[s]);
        ei.sim.setOrigin(si.origin);
        for (const SpawnRequest& r : reqs) {
            const u32 accepted = ei.sim.spawn(r);
            cpuSpawnRoom_ = cpuSpawnRoom_ > accepted ? cpuSpawnRoom_ - accepted : 0u;
        }
        ei.events.clear();
        ei.sim.step(cx.dt, cx.time, fx.events.empty() ? nullptr : &ei.events);

        for (const EventRecord& ev : ei.events) {
            for (const EventSpawn& rule : fx.events) {
                if (rule.on != static_cast<u8>(ev.type) || rule.emitter >= n || rule.emitter == i) continue;
                EmitterInstance& child = si.emitters[rule.emitter];
                if (child.gpu || child.nextInbox.size() >= kMaxInboxPerEmitter) continue;
                SpawnRequest cr;
                cr.xf = Mat4::translation(ev.position);
                cr.count = rule.count;
                cr.addVelocity = ev.velocity * rule.inheritVelocity;
                cr.seed = (ev.id * 2654435761u) ^ (rule.emitter + 1u);
                child.nextInbox.push_back(cr);
            }
        }
        ei.aliveHint = ei.sim.aliveCount();
    }
    si.burstExtra = 0;
}

void ParticleSystem::tick(scene::World& world, f32 dt) {
    spawnAtThisFrame_ = 0;
    cpuSpawnRoom_ = budget_.maxCpuParticles > cpuCountCache_ ? budget_.maxCpuParticles - cpuCountCache_ : 0u;

    // Retire state for entities that are gone -- a recycled entity slot must not inherit a stranger's particles.
    for (auto it = systems_.begin(); it != systems_.end(); ) {
        if (world.valid(it->first) && !world.destroyPending(it->first) &&
            world.component<scene::CParticleEmitter>(it->first, scene::kComponentParticleEmitter)) { ++it; continue; }
        releaseInstance(it->second);
        it = systems_.erase(it);
    }
    order_.erase(std::remove_if(order_.begin(), order_.end(),
                                [&](scene::Entity e) { return systems_.find(e) == systems_.end(); }),
                order_.end());

    applyOps(world);

    // ---- spawn-at requests -> shared event instances ----
    for (const SpawnAtRequest& q : spawnAtQueue_) {
        const ParticleSystemDef* def = effects_ ? effects_->findSystem(q.effect) : nullptr;
        if (!def) continue;
        auto [it, inserted] = eventSystems_.try_emplace(q.effect);
        SystemInstance& si = it->second;
        if (inserted || si.emitters.size() != def->emitters.size()) {
            si.effectId = q.effect;
            buildInstance(si, *def, hash32(static_cast<u32>(q.effect) ^ static_cast<u32>(q.effect >> 32)) | 1u, 0u);
        }
        si.idle = 0.0f;
        for (usize i = 0; i < si.emitters.size() && i < def->emitters.size(); ++i)
            if (def->emitters[i].spawnAtTarget) si.emitters[i].inbox.push_back(q.req);
    }
    spawnAtQueue_.clear();

    // ---- entity emitters ----
    scene::ComponentPool* pool = world.pool(scene::kComponentParticleEmitter);
    const usize finishedBefore = finished_.size();   // the host clears the list; only this tick's entries are destroyed here
    for (usize i = 0; pool && i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* c = static_cast<scene::CParticleEmitter*>(pool->dataAt(i));
        if (!c || world.destroyPending(e)) continue;

        if (c->seed == 0) c->seed = seedFor(e);   // first time this component has been ticked

        const ParticleSystemDef* def = effects_ ? effects_->findSystem(c->effect) : nullptr;
        auto [it, inserted] = systems_.try_emplace(e);
        SystemInstance& si = it->second;
        if (inserted) order_.push_back(e);

        const bool playing = !(c->flags & scene::kParticleEmitterStopped);
        if (playing && !si.wasPlaying) {
            // The stopped -> playing edge: the clock and the burst both restart, exactly once.
            c->age = 0.0f;
            si.burstFired = false;
        }
        si.wasPlaying = playing;

        if (!def) {
            // No effect resolved -- a bad id, or the project's effects have not loaded yet. The clock still advances so
            // a scrubbing inspector sees something; nothing spawns.
            if (playing) c->age += dt;
            if (!si.emitters.empty()) { releaseInstance(si); si.emitters.clear(); }
            si.effectId = c->effect;
            continue;
        }

        if (inserted || si.effectId != c->effect || si.emitters.size() != def->emitters.size() || si.rebuild) {
            si.effectId = c->effect;
            si.rebuild = false;
            buildInstance(si, *def, c->seed, c->flags);
        }

        const Mat4 xf = world.worldMatrix(e);
        si.origin = Vec3{xf.m[3][0], xf.m[3][1], xf.m[3][2]};
        if (si.havePrev && dt > 1e-6f) {
            si.velocity = (si.origin - si.prevOrigin) * (1.0f / dt);
            if (si.velocity.sizeSquared() > 1e10f) si.velocity = Vec3{0, 0, 0};   // a teleport is not a velocity
        } else {
            si.velocity = Vec3{0, 0, 0};
        }
        si.prevOrigin = si.origin;
        si.havePrev = true;

        StepCtx cx;
        cx.dt = dt;
        cx.playing = playing;
        cx.fireBurst = playing && !si.burstFired;
        cx.accum0 = &c->emitAccum;
        cx.xf = xf;
        if (playing) c->age += dt;
        cx.time = c->age;
        si.burstFired = si.burstFired || playing;
        stepSystem(si, *def, cx);

        if ((c->flags & scene::kParticleEmitterAutoDestroy) && !playing) {
            u32 alive = 0;
            for (const EmitterInstance& ei : si.emitters) alive += aliveOf(ei);
            bool pendingSpawns = false;
            for (const EmitterInstance& ei : si.emitters) pendingSpawns = pendingSpawns || !ei.nextInbox.empty();
            if (alive == 0 && !pendingSpawns && !si.rebuild) finished_.push_back(e);
        }
    }
    for (usize i = finishedBefore; i < finished_.size(); ++i) world.destroy(finished_[i]);

    // ---- spawn-at event instances ----
    for (auto it = eventSystems_.begin(); it != eventSystems_.end(); ) {
        SystemInstance& si = it->second;
        const ParticleSystemDef* def = effects_ ? effects_->findSystem(si.effectId) : nullptr;
        if (!def || si.emitters.size() != def->emitters.size()) {
            releaseInstance(si);
            it = eventSystems_.erase(it);
            continue;
        }
        si.age += dt;
        StepCtx cx;
        cx.dt = dt;
        cx.time = si.age;
        cx.playing = false;
        cx.xf = Mat4::identity();
        stepSystem(si, *def, cx);
        u32 alive = 0;
        bool queued = false;
        for (const EmitterInstance& ei : si.emitters) {
            alive += aliveOf(ei);
            queued = queued || !ei.nextInbox.empty();
        }
        si.idle = (alive == 0 && !queued) ? si.idle + dt : 0.0f;
        if (si.idle > kEventInstanceIdleSeconds) {
            releaseInstance(si);
            it = eventSystems_.erase(it);
        } else {
            ++it;
        }
    }

    u32 cpu = 0;
    for (const auto& kv : systems_)
        for (const EmitterInstance& ei : kv.second.emitters) cpu += ei.gpu ? 0u : static_cast<u32>(ei.sim.particles().size());
    for (const auto& kv : eventSystems_)
        for (const EmitterInstance& ei : kv.second.emitters) cpu += ei.gpu ? 0u : static_cast<u32>(ei.sim.particles().size());
    cpuCountCache_ = cpu;
}

u32 ParticleSystem::liveParticles(scene::Entity e) const {
    auto it = systems_.find(e);
    if (it == systems_.end() || it->second.emitters.empty()) return 0u;
    return aliveOf(it->second.emitters[0]);
}

const std::vector<Particle>* ParticleSystem::particles(scene::Entity e) const {
    auto it = systems_.find(e);
    if (it == systems_.end() || it->second.emitters.empty() || it->second.emitters[0].gpu) return nullptr;
    return &it->second.emitters[0].sim.particles();
}

bool ParticleSystem::emitterState(scene::Entity e, u32& alive, u32& tier) const {
    auto it = systems_.find(e);
    if (it == systems_.end() || it->second.emitters.empty()) return false;
    alive = 0;
    tier = 0;
    for (const EmitterInstance& ei : it->second.emitters) {
        alive += aliveOf(ei);
        if (ei.gpu) tier = 1;
    }
    return true;
}

ParticleStats ParticleSystem::stats() const {
    ParticleStats s;
    auto add = [&](const SystemInstance& si) {
        for (const EmitterInstance& ei : si.emitters) {
            if (ei.gpu) { ++s.gpuEmitters; s.gpuParticles += ei.aliveHint; }
            else        { ++s.cpuEmitters; s.cpuParticles += ei.sim.aliveCount(); }
        }
    };
    for (const auto& kv : systems_) add(kv.second);
    for (const auto& kv : eventSystems_) add(kv.second);
    s.spawnAtDropped = spawnAtDropped_;
    s.gpuAllocatedKB = gpuTier_ ? static_cast<u32>(std::min<u64>(gpuTier_->allocatedBytes() / 1024ull, 0xFFFFFFFFull)) : 0u;
    return s;
}

ParticleSystem& particleSystem() {
    static ParticleSystem s;
    return s;
}

} // namespace aver::particles
