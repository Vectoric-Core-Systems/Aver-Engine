// Soft-body fields on the system side: the API that queues pulses and transient fields, the CSoftField component step,
// and the six things a field does to the pieces in it (push, hurt, release anchors, kill, hold asleep). The geometry
// and the maths are Aver.SoftBody's Fields.hpp. Order inside a tick: pulses (arrival order, in applyInbox), component
// fields (ascending entity id), transient fields (ascending handle); time is the fixed-step count. docs/SOFTBODY.md 2.4.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

bool finiteSpec(const FieldSpec& f) {
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            if (!std::isfinite(f.world.m[r][c])) return false;
    return std::isfinite(f.extents.x) && std::isfinite(f.extents.y) && std::isfinite(f.extents.z) && std::isfinite(f.dir.x) && std::isfinite(f.dir.y) &&
           std::isfinite(f.dir.z) && std::isfinite(f.strength) && std::isfinite(f.energyJ) && std::isfinite(f.falloff) && std::isfinite(f.turbulence) &&
           std::isfinite(f.wakeCmS) && static_cast<u32>(f.kind) < kFieldKindCount && static_cast<u32>(f.shape) < kFieldShapeCount;
}

}  // namespace

u32 SoftBodySystem::fieldTargetOf(const Entry& en, const Piece& p) const {
    if (p.debris) return kFieldTargetDebris;
    return en.destructible ? kFieldTargetDestructible : kFieldTargetCloth;
}

u32 SoftBodySystem::piecesInReach(const FieldSpec& f) const {
    u32 n = 0;
    for (const auto& [e, en] : entries_) {
        if (en.failed) continue;
        for (const Piece& p : en.pieces) {
            if (!p.alive || p.empty || p.baked || p.pendingKill) continue;
            if (!(fieldTargetOf(en, p) & f.targets)) continue;
            if (!fieldOverlapsBounds(f, p.boundsMin, p.boundsMax)) continue;
            ++n;
        }
    }
    return n;
}

// ---- the API ---------------------------------------------------------------------------------------------

u32 SoftBodySystem::pulseField(const FieldSpec& f) {
    if (!finiteSpec(f)) return 0;
    InboxItem it;
    it.kind = InboxItem::Kind::Pulse;
    it.field = f;
    inbox_.push_back(std::move(it));
    return piecesInReach(f);
}

u32 SoftBodySystem::startField(const FieldSpec& f, f32 lifetimeS) {
    if (!finiteSpec(f) || !std::isfinite(lifetimeS) || !(lifetimeS > 0.0f)) return 0;
    const u32 handle = nextFieldHandle_++;
    liveHandles_.insert(handle);
    InboxItem it;
    it.kind = InboxItem::Kind::FieldStart;
    it.field = f;
    it.field.perSecond = true;
    it.handle = handle;
    it.lifetime = lifetimeS;
    inbox_.push_back(std::move(it));
    return handle;
}

bool SoftBodySystem::stopField(u32 handle) {
    if (liveHandles_.erase(handle) == 0) return false;
    InboxItem it;
    it.kind = InboxItem::Kind::FieldStop;
    it.handle = handle;
    inbox_.push_back(std::move(it));
    return true;
}

u32 SoftBodySystem::impact(Vec3 point, Vec3 dir, f32 radiusCm, f32 energyJ, f32 kickCmS) {
    if (!(radiusCm > 0.0f) || !(energyJ >= 0.0f) || !std::isfinite(point.x + point.y + point.z + dir.x + dir.y + dir.z + radiusCm + energyJ + kickCmS)) return 0;
    FieldSpec dmg;
    dmg.kind = FieldKind::Damage;
    dmg.shape = FieldShape::Sphere;
    dmg.world = Mat4::translation(point);
    dmg.extents = Vec3{radiusCm, radiusCm, radiusCm};
    dmg.dir = dir;
    dmg.energyJ = energyJ;
    dmg.falloff = 2.0f;   // the (1 - d/r)^2 of an impact deposit
    const u32 reach = pulseField(dmg);
    if (kickCmS != 0.0f) {
        FieldSpec kick = dmg;
        kick.kind = dir.sizeSquared() > 1e-8f ? FieldKind::DirectionalForce : FieldKind::RadialForce;
        kick.strength = kickCmS;
        kick.energyJ = 0.0f;
        kick.falloff = 1.0f;
        pulseField(kick);
    }
    return reach;
}

scene::Entity SoftBodySystem::spawnFieldEntity(scene::World& world, const FieldSpec& f, f32 lifetimeS) {
    if (!finiteSpec(f) || !std::isfinite(lifetimeS) || !(lifetimeS > 0.0f)) return scene::kInvalidEntity;
    const scene::Entity e = world.create("SoftField");
    if (e == scene::kInvalidEntity) return e;
    CSoftField* c = attachSoftField(world, e);
    if (!c) {
        world.destroy(e);
        return scene::kInvalidEntity;
    }
    world.setLocalTransform(e, transformFromMatrix(f.world));
    c->kind = static_cast<u32>(f.kind);
    c->shape = static_cast<u32>(f.shape);
    c->targets = f.targets;
    c->extentX = f.extents.x; c->extentY = f.extents.y; c->extentZ = f.extents.z;
    c->strength = f.strength;
    c->energyJPerS = f.energyJ;
    c->falloff = f.falloff;
    c->dirX = f.dir.x; c->dirY = f.dir.y; c->dirZ = f.dir.z;
    c->turbulence = f.turbulence;
    c->flags = f.perSecond ? 0u : kFieldOnce;
    spawnedLife_[e] = lifetimeS;
    needFlush_ = true;
    return e;
}

bool SoftBodySystem::specOfComponent(scene::World& world, scene::Entity e, FieldSpec& out) {
    const u32 type = softFieldType();
    if (!type) return false;
    const auto* c = world.component<CSoftField>(e, type);
    if (!c || c->kind >= kFieldKindCount || c->shape >= kFieldShapeCount) return false;
    FieldSpec f;
    f.kind = static_cast<FieldKind>(c->kind);
    f.shape = static_cast<FieldShape>(c->shape);
    f.targets = c->targets ? (c->targets & kFieldTargetAll) : (f.kind == FieldKind::Kill ? kFieldTargetDebris : kFieldTargetAll);
    f.perSecond = (c->flags & kFieldOnce) == 0;
    f.world = world.worldMatrix(e);
    f.extents = Vec3{c->extentX > 0.0f ? c->extentX : 100.0f, c->extentY > 0.0f ? c->extentY : 100.0f, c->extentZ > 0.0f ? c->extentZ : 100.0f};
    f.dir = Vec3{c->dirX, c->dirY, c->dirZ};
    f.strength = c->strength;
    f.energyJ = c->energyJPerS;
    f.falloff = c->falloff > 0.0f ? c->falloff : 1.0f;
    f.turbulence = c->turbulence;
    out = f;
    return true;
}

// ---- release ---------------------------------------------------------------------------------------------

// Un-pins the listed particles and rebuilds the pieces that owned them, the way releaseEntry does for a whole cloth.
void SoftBodySystem::releaseAnchorsIn(scene::World&, Entry& en, const std::vector<u32>& particles) {
    if (particles.empty()) return;
    Cage& c = en.sim.cage;
    std::vector<u8> affected(en.pieces.size(), 0);
    for (const u32 pid : particles) {
        Particle& q = c.particles[pid];
        q.pinned = false;
        q.reachCm = -1.0f;
        q.invMass = 1.0f / std::max(q.mass, 1e-4f);
        const u32 idx = pid < en.owner.size() ? en.owner[pid] : kNone;
        if (idx < en.pieces.size()) affected[idx] = 1;
    }
    ++rebuildsThisTick_;
    for (u32 i = 0; i < en.pieces.size(); ++i) {
        Piece& p = en.pieces[i];
        if (!affected[i] || !p.alive || !p.body) continue;
        bool pinned = false;
        for (const u32 pid : p.particles) pinned = pinned || c.particles[pid].pinned;
        p.anchored = pinned;
        std::vector<i32> map;
        map.reserve(p.particles.size());
        for (const u32 pid : p.particles) map.push_back(static_cast<i32>(c.particles[pid].simIndex));
        buildPieceBody(en, p, p.body, p.body, &map, p.plastic);
        p.hotFor = std::max(p.hotFor, opt_.hotSeconds);
    }
    const Vec3 where = en.pieces.empty() ? Vec3{0, 0, 0} : en.pieces.front().centroid;
    emit(en, en.pieces.empty() ? nullptr : &en.pieces.front(), SoftEventKind::Released, 0, where, 0.0f);
    emitFx(en, FxSlot::Release, where, Vec3{0, 0, 1}, 0.0f);
}

// ---- applying one field ----------------------------------------------------------------------------------

u32 SoftBodySystem::applyField(scene::World& world, const FieldSpec& fIn, f32 seconds, bool once) {
    FieldSpec f = fIn;
    if (once) f.perSecond = false;
    const FieldFrame fr = makeFieldFrame(f);
    u32 touchedTotal = 0;

    // Velocity changes to one piece's free particles: one add_velocity call, only if something would move enough to matter.
    auto push = [&](Entry& en, Piece& p, const FieldFrame& frame, f32 secs) {
        if (!p.body || p.frozen || p.fieldHeld) return;
        const Cage& c = en.sim.cage;
        std::vector<i32> verts;
        std::vector<f32> dv;
        f32 maxMag2 = 0.0f;
        for (u32 i = 0; i < p.particles.size(); ++i) {
            const Particle& q = c.particles[p.particles[i]];
            if (q.pinned || q.invMass <= 0.0f) continue;
            const f32 w = fieldWeight(frame, q.pos);
            if (!(w > 0.0f)) continue;
            const Vec3 d = fieldVelocityDelta(frame, q.pos, w, secs, stepClock_);
            if (d.sizeSquared() <= 0.0f) continue;
            verts.push_back(static_cast<i32>(i));
            dv.push_back(d.x); dv.push_back(d.y); dv.push_back(d.z);
            maxMag2 = std::max(maxMag2, d.sizeSquared());
        }
        if (verts.empty()) return;
        // A sleeping piece wakes only if the field would change a vertex velocity by wakeCmS.
        if (aver_phys_sb_is_active(p.body) == 0 && maxMag2 < frame.spec.wakeCmS * frame.spec.wakeCmS) return;
        aver_phys_sb_add_velocity(p.body, verts.data(), dv.data(), static_cast<i32>(verts.size()));
        p.needsRead = true;
        p.hotFor = std::max(p.hotFor, opt_.hotSeconds);
    };

    for (auto& [e, en] : entries_) {
        if (en.failed) continue;
        // Which pieces of this entry are in reach and targeted.
        std::vector<u8> reach(en.pieces.size(), 0);
        u32 reachCount = 0;
        for (u32 pi = 0; pi < en.pieces.size(); ++pi) {
            const Piece& p = en.pieces[pi];
            if (!p.alive || p.empty || p.baked || p.pendingKill) continue;
            if (!(fieldTargetOf(en, p) & f.targets)) continue;
            if (!fieldOverlapsBounds(f, p.boundsMin, p.boundsMax)) continue;
            reach[pi] = 1;
            ++reachCount;
        }
        if (!reachCount) continue;
        touchedTotal += reachCount;

        switch (f.kind) {
            case FieldKind::RadialForce:
            case FieldKind::DirectionalForce:
                for (u32 pi = 0; pi < en.pieces.size(); ++pi)
                    if (reach[pi]) push(en, en.pieces[pi], fr, seconds);
                break;

            case FieldKind::Damage: {
                if (!en.destructible && !en.asset.cloth.tearable) break;   // plain cloth only creeps
                if (en.destructible)
                    if (const auto* de = world.component<CDestructible>(en.entity, destructibleType()))
                        if (de->flags & kDestructIndestructible) break;
                std::vector<f32> w;
                fieldBeamWeights(en.sim.cage, f, w);
                // Only the beams of pieces the field targets and reaches take energy.
                for (u32 bi = 0; bi < w.size(); ++bi) {
                    const u32 idx = en.sim.cage.beams[bi].a < en.owner.size() ? en.owner[en.sim.cage.beams[bi].a] : kNone;
                    if (idx >= en.pieces.size() || !reach[idx]) w[bi] = 0.0f;
                }
                f32 scale = 1.0f;
                if (const auto* de = world.component<CDestructible>(en.entity, destructibleType())) scale = de->impactScale > 0.0f ? de->impactScale : 1.0f;
                const f32 energy = f.energyJ * (f.perSecond ? seconds : 1.0f) * scale;
                if (!(energy > 0.0f)) break;
                const DepositResult r = depositWeighted(en.sim.cage, w, energy, en.asset.destruct.impulseEfficiency, en.asset.destruct.maxKickCmS);
                en.hit = HitInfo{};
                en.hit.valid = true;
                en.hit.kind = static_cast<u8>(DepositKind::Gameplay);
                en.hit.energyJ = energy;
                en.hit.radiusCm = f.extents.x;
                en.hit.beamsDamaged = r.beamsDamaged;
                en.hit.broken = r.broken;
                en.hit.tick = tickCount_;
                if (r.broken) noteBroken(world, en, r.brokenBeams, fr.centre);
                for (u32 pi = 0; pi < en.pieces.size(); ++pi)
                    if (reach[pi]) en.pieces[pi].hotFor = std::max(en.pieces[pi].hotFor, opt_.hotSeconds);
                if (r.kickSpeedCmS > 0.0f) {
                    FieldSpec k = f;
                    k.kind = f.dir.sizeSquared() > 1e-8f ? FieldKind::DirectionalForce : FieldKind::RadialForce;
                    k.strength = r.kickSpeedCmS;
                    k.perSecond = false;
                    const FieldFrame kf = makeFieldFrame(k);
                    for (u32 pi = 0; pi < en.pieces.size(); ++pi)
                        if (reach[pi]) push(en, en.pieces[pi], kf, 1.0f);
                }
                break;
            }

            case FieldKind::ReleaseAnchors: {
                std::vector<u32> pins;
                anchorsInField(en.sim.cage, f, pins);
                std::vector<u32> mine;
                for (const u32 pid : pins) {
                    const u32 idx = pid < en.owner.size() ? en.owner[pid] : kNone;
                    if (idx < en.pieces.size() && reach[idx]) mine.push_back(pid);
                }
                releaseAnchorsIn(world, en, mine);
                break;
            }

            case FieldKind::Kill:
                for (u32 pi = 0; pi < en.pieces.size(); ++pi) {
                    if (!reach[pi]) continue;
                    Piece& p = en.pieces[pi];
                    if (!(fieldWeight(fr, p.centroid) > 0.0f) && f.shape != FieldShape::Everywhere) continue;
                    if (p.debris) {
                        p.shrinking = true;   // the existing shrink-then-destroy
                    } else {
                        const u32 n = static_cast<u32>(p.particles.size());
                        const Vec3 centre = p.centroid;
                        killPiece(world, en, p);   // a root: hidden, kept, so Repair restores it
                        emit(en, nullptr, SoftEventKind::Dust, 0, centre, static_cast<f32>(n));
                        emitFx(en, FxSlot::Dust, centre, Vec3{0, 0, 1}, static_cast<f32>(n), n);
                    }
                }
                break;

            case FieldKind::Sleep:
                for (u32 pi = 0; pi < en.pieces.size(); ++pi) {
                    if (!reach[pi]) continue;
                    Piece& p = en.pieces[pi];
                    if (!p.body || p.frozen) continue;
                    if (!(fieldWeight(fr, p.centroid) > 0.0f) && f.shape != FieldShape::Everywhere) continue;
                    if (!p.fieldHeld) {
                        aver_phys_sb_set_active(p.body, 0);
                        p.fieldHeld = true;
                    }
                    p.heldTick = fieldTick_;
                }
                break;
        }
    }
    return touchedTotal;
}

// ---- the per-tick step -----------------------------------------------------------------------------------

void SoftBodySystem::fieldStep(scene::World& world, f32 stepSeconds) {
    // Component fields in ascending entity id. A kFieldOnce field is a pulse and then disables itself.
    if (const u32 type = softFieldType()) {
        if (scene::ComponentPool* pool = world.pool(type)) {
            std::vector<scene::Entity> ids;
            for (usize i = 0; i < pool->size(); ++i) ids.push_back(pool->entityAt(i));
            std::sort(ids.begin(), ids.end());
            for (const scene::Entity e : ids) {
                auto* c = world.component<CSoftField>(e, type);
                if (!c) continue;
                if (c->flags & kFieldDisabled) {
                    c->outState &= ~kFieldStateActive;
                    if (!(c->outState & kFieldStateFired)) c->outTouched = 0;   // a once field keeps what its one pulse reached
                    continue;
                }
                FieldSpec spec;
                if (!specOfComponent(world, e, spec)) continue;
                const bool once = (c->flags & kFieldOnce) != 0;
                c->outTouched = applyField(world, spec, stepSeconds, once);
                ++stats_.fieldsApplied;
                c->outState = kFieldStateActive | (once ? kFieldStateFired : 0u);
                if (once) {
                    c->flags |= kFieldDisabled;
                    c->outState &= ~kFieldStateActive;
                }
            }
        }
    }

    // Transient fields in ascending handle.
    for (auto it = fields_.begin(); it != fields_.end();) {
        applyField(world, it->second.spec, stepSeconds, false);
        ++stats_.fieldsApplied;
        it->second.remainingS -= stepSeconds;
        if (it->second.remainingS <= 0.0f) {
            liveHandles_.erase(it->first);
            it = fields_.erase(it);
        } else {
            ++it;
        }
    }

    // Holds: pieces setAsleep keeps, and pieces a Sleep field did not reach this tick are released.
    for (auto& [e, en] : entries_) {
        if (en.failed) continue;
        for (Piece& p : en.pieces) {
            if (!p.alive || !p.body || p.baked) continue;
            if (en.forceAsleep && !p.frozen) {
                if (!p.fieldHeld) {
                    aver_phys_sb_set_active(p.body, 0);
                    p.fieldHeld = true;
                }
                p.heldTick = fieldTick_;
            }
            if (p.fieldHeld && p.heldTick != fieldTick_) {
                p.fieldHeld = false;
                p.needsRead = true;
                aver_phys_sb_set_active(p.body, 1);
            }
        }
    }

    // Spawned field entities remove themselves after their lifetime.
    for (auto it = spawnedLife_.begin(); it != spawnedLife_.end();) {
        it->second -= stepSeconds;
        if (it->second <= 0.0f || !world.valid(it->first)) {
            if (world.valid(it->first)) world.destroy(it->first);
            needFlush_ = true;
            it = spawnedLife_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace aver::softbody
