// The inbox, impacts, plastic deformation (dents), release and repair, and the debris lifecycle. Everything here runs
// inside tick(), in a fixed order. The tear pipeline (rebuild + islands + debris) is SoftBodyTear.cpp.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

f32 or1(f32 v) { return v > 0.0f ? v : 1.0f; }

struct ImpactRec {
    i32 soft = 0, other = 0, index = 0, verts = 0;
    f32 point[3] = {0, 0, 0}, normal[3] = {0, 0, 0}, closing = 0.0f, mass = 0.0f;
};

}  // namespace

void SoftBodySystem::noteBroken(scene::World&, Entry& en, const std::vector<u32>& beams, const Vec3& point) {
    if (beams.empty()) return;
    const Cage& c = en.sim.cage;
    std::vector<u32> sorted = beams;
    std::sort(sorted.begin(), sorted.end());
    for (const u32 b : sorted) {
        const u32 pa = c.beams[b].a;
        const u32 idx = pa < en.owner.size() ? en.owner[pa] : kNone;
        if (idx < en.pieces.size() && en.pieces[idx].alive && !en.pieces[idx].baked) {
            en.pieces[idx].needsRebuild = true;
            en.pieces[idx].hotFor = std::max(en.pieces[idx].hotFor, opt_.hotSeconds);
        }
    }
    en.brokenBeams += static_cast<u32>(sorted.size());
    emit(en, en.pieces.empty() ? nullptr : &en.pieces.front(), SoftEventKind::Torn, sorted.front(), point, static_cast<f32>(sorted.size()));
    emitFx(en, FxSlot::Tear, point, Vec3{0, 0, 1}, static_cast<f32>(sorted.size()));
}

void SoftBodySystem::depositOn(scene::World& world, Entry& en, Piece*, const DamageDeposit& d, f32 impactScale, bool kick, SoftEventKind) {
    if (en.failed) return;
    Cage& c = en.sim.cage;
    if (en.destructible) {
        const auto* de = world.component<CDestructible>(en.entity, SoftBodySystem::destructibleType());
        if (de && (de->flags & kDestructIndestructible)) return;
    }
    DamageDeposit dd = d;
    dd.energyJ *= impactScale;
    const DepositResult r = depositEnergy(c, dd, en.asset.destruct.impulseEfficiency, en.asset.destruct.maxKickCmS,
                                          en.bakedBeams.empty() ? nullptr : &en.bakedBeams);
    if (r.broken) noteBroken(world, en, r.brokenBeams, d.point);

    en.hit = HitInfo{};
    en.hit.valid = true;
    en.hit.kind = d.kind;
    en.hit.energyJ = dd.energyJ;
    en.hit.radiusCm = d.radiusCm;
    en.hit.beamsDamaged = r.beamsDamaged;
    en.hit.broken = r.broken;
    en.hit.tick = tickCount_;

    const Vec3 dirN = d.dir.getSafeNormal();
    const bool radial = dirN.sizeSquared() < 0.5f;
    const f32 reach = d.radiusCm;
    for (Piece& p : en.pieces) {
        if (!p.alive || p.empty || p.baked) continue;
        const Vec3 q{std::clamp(d.point.x, p.boundsMin.x, p.boundsMax.x), std::clamp(d.point.y, p.boundsMin.y, p.boundsMax.y),
                     std::clamp(d.point.z, p.boundsMin.z, p.boundsMax.z)};
        if (dist(q, d.point) > reach) continue;
        p.hotFor = std::max(p.hotFor, opt_.hotSeconds);
        if (kick && r.kickSpeedCmS > 0.0f && p.body && !p.frozen) {
            const f32 centre[3] = {d.point.x, d.point.y, d.point.z};
            const f32 dir[3] = {radial ? 0.0f : dirN.x, radial ? 0.0f : dirN.y, radial ? 1.0f : dirN.z};
            aver_phys_sb_impulse(p.body, centre, reach, dir, r.kickSpeedCmS, radial ? 1.0f : 0.0f, 2.0f);
        }
    }
}

void SoftBodySystem::applyInbox(scene::World& world) {
    if (inbox_.empty()) return;
    std::vector<InboxItem> items;
    items.swap(inbox_);
    std::vector<InboxItem> carry;
    for (InboxItem& it : items) {
        switch (it.kind) {
            case InboxItem::Kind::Wind:
                aver_phys_sb_set_wind(it.dir.x, it.dir.y, it.dir.z, it.a, it.b, it.c);
                wind_ = it.dir;
                break;
            case InboxItem::Kind::Budget:
                opt_.maxLivePieces = it.budget.maxPieces;
                opt_.maxActiveParticles = it.budget.maxParticles;
                opt_.maxRebuildsPerFrame = std::max(1u, it.budget.maxRebuilds);
                if (it.budget.debrisLifetimeS > 0.0f) opt_.debrisLifetimeS = it.budget.debrisLifetimeS;
                break;
            case InboxItem::Kind::Preset:
                if (auto* cl = world.component<CCloth>(it.entity, clothType())) cl->preset = it.preset;
                else if (auto* de = world.component<CDestructible>(it.entity, destructibleType())) de->preset = it.preset;
                break;
            case InboxItem::Kind::Damage: {
                if (depositsThisTick_ >= opt_.maxDepositsPerFrame) { carry.push_back(it); break; }
                Entry* en = find(it.entity);
                if (!en || en->failed) break;
                ++depositsThisTick_;
                DamageDeposit d;
                d.point = it.point; d.dir = it.dir; d.radiusCm = it.radius; d.energyJ = it.energy; d.kind = it.depositKind;
                f32 scale = 1.0f;
                if (const auto* de = world.component<CDestructible>(en->entity, destructibleType())) scale = or1(de->impactScale);
                depositOn(world, *en, nullptr, d, scale, d.kind != static_cast<u8>(DepositKind::Impact), SoftEventKind::Impact);
                break;
            }
            case InboxItem::Kind::Explode: {
                for (auto& [e, en] : entries_) {
                    if (en.failed) continue;
                    f32 nearest = 1e30f;
                    for (const Piece& p : en.pieces) {
                        if (!p.alive || p.empty || p.baked) continue;
                        const Vec3 q{std::clamp(it.point.x, p.boundsMin.x, p.boundsMax.x), std::clamp(it.point.y, p.boundsMin.y, p.boundsMax.y),
                                     std::clamp(it.point.z, p.boundsMin.z, p.boundsMax.z)};
                        nearest = std::min(nearest, dist(q, it.point));
                    }
                    if (nearest > it.radius) continue;
                    if (depositsThisTick_ >= opt_.maxDepositsPerFrame) break;
                    ++depositsThisTick_;
                    const f32 fall = 1.0f - nearest / it.radius;
                    DamageDeposit d;
                    d.point = it.point; d.radiusCm = it.radius; d.energyJ = it.energy * fall * fall;
                    d.kind = static_cast<u8>(DepositKind::Explosion);
                    f32 scale = 1.0f;
                    if (const auto* de = world.component<CDestructible>(e, destructibleType())) scale = or1(de->impactScale);
                    depositOn(world, en, nullptr, d, scale, false, SoftEventKind::Impact);
                    // The radial shove goes to every piece in reach, cloth included.
                    for (Piece& p : en.pieces) {
                        if (!p.alive || p.empty || !p.body || p.frozen) continue;
                        const f32 centre[3] = {it.point.x, it.point.y, it.point.z};
                        const f32 up[3] = {0, 0, 1};
                        aver_phys_sb_impulse(p.body, centre, it.radius, up, it.impulse, 1.0f, 1.0f);
                        p.hotFor = std::max(p.hotFor, opt_.hotSeconds);
                    }
                }
                break;
            }
            case InboxItem::Kind::Tear: {
                Entry* en = find(it.entity);
                if (!en || en->failed) break;
                std::vector<u32> broken;
                if (en->bakedBeams.empty()) {
                    tearWithin(en->sim.cage, it.point, it.radius, &broken);
                } else {
                    // A baked piece is no longer a body: its edges are not cut.
                    Cage& c = en->sim.cage;
                    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
                        const Beam& b = c.beams[bi];
                        if (b.broken || (bi < en->bakedBeams.size() && en->bakedBeams[bi])) continue;
                        if (distSquared((c.particles[b.a].pos + c.particles[b.b].pos) * 0.5f, it.point) > it.radius * it.radius) continue;
                        breakBeam(c, bi);
                        broken.push_back(bi);
                    }
                }
                noteBroken(world, *en, broken, it.point);
                break;
            }
            case InboxItem::Kind::Repair:
                if (Entry* en = find(it.entity)) repairEntry(world, *en);
                break;
            case InboxItem::Kind::Release:
                if (Entry* en = find(it.entity)) releaseEntry(world, *en);
                break;
            case InboxItem::Kind::Pulse:
                applyField(world, it.field, stepSeconds_, true);
                ++stats_.fieldsApplied;
                break;
            case InboxItem::Kind::FieldStart: {
                Transient t;
                t.spec = it.field;
                t.spec.perSecond = true;
                t.remainingS = it.lifetime;
                fields_[it.handle] = std::move(t);
                break;
            }
            case InboxItem::Kind::FieldStop:
                fields_.erase(it.handle);
                break;
            case InboxItem::Kind::Asleep:
                if (Entry* en = find(it.entity)) en->forceAsleep = it.flag;
                break;
            case InboxItem::Kind::ClearDebris:
                if (Entry* en = find(it.entity)) {
                    for (Piece& p : en->pieces)
                        if (p.alive && p.debris) destroyPiece(world, *en, p);
                    needFlush_ = true;
                }
                break;
        }
    }
    inbox_.insert(inbox_.begin(), carry.begin(), carry.end());
}

void SoftBodySystem::pollImpacts(scene::World& world) {
    const i32 n = aver_phys_sb_impact_count();
    if (n <= 0) return;
    std::vector<ImpactRec> recs;
    recs.reserve(static_cast<usize>(n));
    for (i32 i = 0; i < n; ++i) {
        ImpactRec r;
        r.index = i;
        if (!aver_phys_sb_impact_get(i, &r.soft, &r.other, r.point, r.normal, &r.closing, &r.mass, &r.verts)) continue;
        recs.push_back(r);
    }
    std::sort(recs.begin(), recs.end(), [](const ImpactRec& a, const ImpactRec& b) {
        if (a.soft != b.soft) return a.soft < b.soft;
        if (a.other != b.other) return a.other < b.other;
        return a.index < b.index;
    });
    for (const ImpactRec& r : recs) {
        const auto bit = byBody_.find(r.soft);
        if (bit == byBody_.end()) continue;
        Entry* en = find(bit->second.first);
        if (!en || en->failed || !en->destructible) continue;
        if (r.closing < en->asset.destruct.minImpactCmS) continue;
        if (depositsThisTick_ >= opt_.maxDepositsPerFrame) break;
        ++depositsThisTick_;

        const f32 radius = std::clamp(12.0f + 6.0f * std::sqrt(static_cast<f32>(std::max(r.verts, 1))), 12.0f, 80.0f) * en->asset.destruct.impactRadiusScale;
        const Vec3 point{r.point[0], r.point[1], r.point[2]};
        f32 localMass = 0.0f;
        for (const Particle& q : en->sim.cage.particles)
            if (distSquared(q.pos, point) < radius * radius) localMass += q.mass;
        const f32 v = r.closing * 0.01f;
        const f32 reduced = r.mass > 0.0f && localMass > 0.0f ? (r.mass * localMass) / (r.mass + localMass) : localMass;
        DamageDeposit d;
        d.point = point; d.dir = Vec3{-r.normal[0], -r.normal[1], -r.normal[2]}; d.radiusCm = radius;
        d.energyJ = 0.5f * reduced * v * v;
        d.kind = static_cast<u8>(DepositKind::Impact);
        f32 scale = 1.0f;
        if (const auto* de = world.component<CDestructible>(en->entity, destructibleType())) scale = or1(de->impactScale);
        Piece* p = pieceById(*en, bit->second.second);
        depositOn(world, *en, p, d, scale, false, SoftEventKind::Impact);
        if (en->hit.valid && en->hit.tick == tickCount_) en->hit.closingCmS = r.closing;
        emit(*en, p, SoftEventKind::Impact, 0, point, r.closing);
        emitFx(*en, FxSlot::Impact, point, Vec3{r.normal[0], r.normal[1], r.normal[2]}, r.closing);
    }
}

void SoftBodySystem::plasticStep(scene::World& world, Entry& en, f32 dt) {
    const bool tearableCloth = !en.destructible && en.asset.cloth.tearable;
    bool dents = en.destructible;
    if (en.destructible)
        if (const auto* de = world.component<CDestructible>(en.entity, destructibleType()))
            if (de->flags & kDestructIndestructible) dents = false;
    if (!dents && !tearableCloth) return;
    if (en.stepAccum < opt_.damageEvalEveryNSteps) return;

    bool hot = tearableCloth || en.maxStrain > opt_.watchStrain;
    for (const Piece& p : en.pieces) hot = hot || (p.alive && p.hotFor > 0.0f);
    en.stepAccum = 0;
    if (!hot) return;

    Cage& c = en.sim.cage;
    DamageConfig cfg;
    cfg.dt = std::max(dt, 1.0f / 120.0f);
    cfg.tearable = true;
    const DamageResult r = evaluateStrain(c, cfg);
    en.maxStrain = r.maxStrain;

    if (!r.dirtyBeams.empty()) {
        struct Edit { std::vector<i32> slots; std::vector<f32> rest; std::vector<i32> bends; };
        std::map<u32, Edit> edits;
        std::vector<u8> touched(c.particles.size(), 0);
        for (const u32 bi : r.dirtyBeams) {
            const Beam& b = c.beams[bi];
            if (b.slot == kNone || b.broken) continue;
            const u32 idx = b.a < en.owner.size() ? en.owner[b.a] : kNone;
            if (idx >= en.pieces.size() || !en.pieces[idx].alive) continue;
            Edit& ed = edits[idx];
            ed.slots.push_back(static_cast<i32>(b.slot));
            ed.rest.push_back(b.rest);
            touched[b.a] = touched[b.b] = 1;
        }
        for (const Bend& bd : c.bends) {
            if (bd.slot == kNone || c.beams[bd.beam].broken) continue;
            if (!(touched[bd.v[0]] || touched[bd.v[1]] || touched[bd.v[2]] || touched[bd.v[3]])) continue;
            const u32 idx = bd.v[0] < en.owner.size() ? en.owner[bd.v[0]] : kNone;
            const auto it = edits.find(idx);
            if (it != edits.end()) it->second.bends.push_back(static_cast<i32>(bd.slot));
        }
        for (auto& [idx, ed] : edits) {
            Piece& p = en.pieces[idx];
            if (!p.body || p.frozen) continue;
            aver_phys_sb_set_edge_rest(p.body, ed.slots.data(), ed.rest.data(), static_cast<i32>(ed.slots.size()));
            if (!ed.bends.empty()) aver_phys_sb_rest_bends_from_pose(p.body, ed.bends.data(), static_cast<i32>(ed.bends.size()));
            p.plastic = true;
            p.hotFor = std::max(p.hotFor, 0.5f);   // it is still creeping: keep watching
        }
    }
    if (!r.brokenBeams.empty()) {
        Vec3 where{0, 0, 0};
        const Beam& b0 = c.beams[r.brokenBeams.front()];
        where = (c.particles[b0.a].pos + c.particles[b0.b].pos) * 0.5f;
        noteBroken(world, en, r.brokenBeams, where);
    }
}

scene::Entity SoftBodySystem::makeDebrisEntity(scene::World& world, Entry& en, Piece& p) {
    const scene::Entity d = world.create("SoftPiece");
    if (d == scene::kInvalidEntity) return d;
    world.setLocalPosition(d, p.origin);
    if (const auto* src = world.component<scene::CMeshRenderer>(en.entity, scene::kComponentMeshRenderer)) {
        auto* mr = static_cast<scene::CMeshRenderer*>(world.addComponent(d, scene::kComponentMeshRenderer));
        *mr = *src;
        // Invisible until the draw side has a resident mesh for it: with the bit set, a piece the draw side has not built
        // yet draws the WHOLE authored mesh at its centroid (the scene walk draws the authored mesh for a zero handle).
        mr->flags &= ~scene::kMeshRendererVisible;
        mr->dirty = 1;
    }
    auto* sb = static_cast<scene::CSoftBody*>(world.addComponent(d, scene::kComponentSoftBody));
    sb->flags = scene::kSoftBodyPiece;
    needFlush_ = true;
    ++liveDebris_;
    return d;
}

void SoftBodySystem::releaseEntry(scene::World& world, Entry& en) {
    if (en.failed || en.released) return;
    Cage& c = en.sim.cage;
    for (Particle& q : c.particles) {
        q.pinned = false;
        q.reachCm = -1.0f;
        q.invMass = 1.0f / std::max(q.mass, 1e-4f);
    }
    en.released = true;
    ++rebuildsThisTick_;
    for (Piece& p : en.pieces) {
        if (!p.alive || !p.body) continue;
        std::vector<i32> map;
        map.reserve(p.particles.size());
        for (const u32 pid : p.particles) map.push_back(static_cast<i32>(c.particles[pid].simIndex));
        p.anchored = false;
        buildPieceBody(en, p, p.body, p.body, &map, false);
    }
    const Vec3 where = en.pieces.empty() ? Vec3{0, 0, 0} : en.pieces.front().centroid;
    emit(en, en.pieces.empty() ? nullptr : &en.pieces.front(), SoftEventKind::Released, 0, where, 0.0f);
    emitFx(en, FxSlot::Release, where, Vec3{0, 0, 1}, 0.0f);
    (void)world;
}

void SoftBodySystem::repairEntry(scene::World& world, Entry& en) {
    const scene::Entity e = en.entity;
    destroyEntry(world, en);
    const auto it = entries_.find(e);
    Entry fresh;
    fresh.entity = e;
    it->second = std::move(fresh);
    if (!buildEntry(world, it->second)) {
        entries_.erase(it);
        return;
    }
    Entry& again = it->second;
    ++rebuildsThisTick_;
    const Vec3 where = again.pieces.empty() ? Vec3{0, 0, 0} : again.pieces.front().centroid;
    emit(again, again.pieces.empty() ? nullptr : &again.pieces.front(), SoftEventKind::Repaired, 0, where, 0.0f);
    emitFx(again, FxSlot::Release, where, Vec3{0, 0, 1}, 0.0f);
}

void SoftBodySystem::debrisLifecycle(scene::World& world, f32 dt) {
    for (auto& [e, en] : entries_) {
        if (en.failed) continue;
        f32 life = en.asset.destruct.debrisLifetimeS > 0.0f ? en.asset.destruct.debrisLifetimeS : opt_.debrisLifetimeS;
        if (const auto* de = world.component<CDestructible>(e, destructibleType()))
            if (de->debrisLifetimeS > 0.0f) life = de->debrisLifetimeS;
        for (Piece& p : en.pieces) {
            if (!p.alive || !p.debris) continue;
            p.age += dt;
            // A baked piece has no body and is asleep for good; a held one (a Sleep field) is not resting, it is paused.
            const bool asleep = p.baked || (p.body && !p.frozen && !p.fieldHeld && aver_phys_sb_is_active(p.body) == 0);
            p.asleepFor = asleep ? p.asleepFor + dt : 0.0f;
            if (!p.baked && p.body && asleep && opt_.bakeEnabled && en.asset.destruct.bake && p.asleepFor >= opt_.bakeAfterS) bakePiece(world, en, p);
            if (!p.shrinking && p.asleepFor >= opt_.debrisSleepS && p.age >= life) p.shrinking = true;
            if (p.shrinking) {
                p.shrinkT += dt;
                if (p.baked && p.entity != scene::kInvalidEntity && opt_.despawnShrinkS > 0.0f) {
                    // A baked piece is a plain mesh: its despawn shrink is the entity's scale, no GPU work.
                    const f32 s = std::max(0.0f, 1.0f - p.shrinkT / opt_.despawnShrinkS);
                    world.setLocalScale(p.entity, Vec3{s, s, s});
                    needFlush_ = true;
                }
                if (p.shrinkT >= opt_.despawnShrinkS) {
                    destroyPiece(world, en, p);
                    needFlush_ = true;
                }
            }
        }
    }
}

void SoftBodySystem::evictDebris(scene::World&) {
    struct Cand { Entry* en; Piece* p; f32 age; f32 d; };
    std::vector<Cand> live;
    for (auto& [e, en] : entries_)
        for (Piece& p : en.pieces)
            if (p.alive && p.debris && !p.shrinking) live.push_back({&en, &p, p.age, dist(p.centroid, viewPos_)});
    if (live.size() <= opt_.maxLivePieces) return;
    std::sort(live.begin(), live.end(), [](const Cand& a, const Cand& b) {
        if (a.age != b.age) return a.age > b.age;
        if (a.d != b.d) return a.d > b.d;
        if (a.en->entity != b.en->entity) return a.en->entity < b.en->entity;
        return a.p->id < b.p->id;
    });
    const usize excess = live.size() - opt_.maxLivePieces;
    for (usize i = 0; i < excess; ++i) live[i].p->shrinking = true;
}

}  // namespace aver::softbody
