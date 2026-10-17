// The inbox, impacts, plastic deformation (dents), the tear pipeline (rebuild + islands + debris), release and
// repair, and the debris lifecycle. Everything here runs inside tick(), in a fixed order.
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
        if (idx < en.pieces.size() && en.pieces[idx].alive) {
            en.pieces[idx].needsRebuild = true;
            en.pieces[idx].hotFor = std::max(en.pieces[idx].hotFor, opt_.hotSeconds);
        }
    }
    en.brokenBeams += static_cast<u32>(sorted.size());
    emit(en, en.pieces.empty() ? nullptr : &en.pieces.front(), SoftEventKind::Torn, sorted.front(), point, static_cast<f32>(sorted.size()));
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
    const DepositResult r = depositEnergy(c, dd, en.asset.destruct.impulseEfficiency, en.asset.destruct.maxKickCmS);
    if (r.broken) noteBroken(world, en, r.brokenBeams, d.point);

    const Vec3 dirN = d.dir.getSafeNormal();
    const bool radial = dirN.sizeSquared() < 0.5f;
    const f32 reach = d.radiusCm;
    for (Piece& p : en.pieces) {
        if (!p.alive || p.empty) continue;
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
                        if (!p.alive || p.empty) continue;
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
                tearWithin(en->sim.cage, it.point, it.radius, &broken);
                noteBroken(world, *en, broken, it.point);
                break;
            }
            case InboxItem::Kind::Repair:
                if (Entry* en = find(it.entity)) repairEntry(world, *en);
                break;
            case InboxItem::Kind::Release:
                if (Entry* en = find(it.entity)) releaseEntry(world, *en);
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

        const f32 radius = std::clamp(12.0f + 6.0f * std::sqrt(static_cast<f32>(std::max(r.verts, 1))), 12.0f, 80.0f);
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
        emit(*en, p, SoftEventKind::Impact, 0, point, r.closing);
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
        mr->flags |= scene::kMeshRendererVisible;
        mr->dirty = 1;
    }
    auto* sb = static_cast<scene::CSoftBody*>(world.addComponent(d, scene::kComponentSoftBody));
    sb->flags = scene::kSoftBodyPiece;
    needFlush_ = true;
    ++liveDebris_;
    (void)p;
    return d;
}

bool SoftBodySystem::tearPipeline(scene::World& world, Entry& en) {
    Cage& c = en.sim.cage;
    std::vector<u32> dirty;
    for (u32 i = 0; i < en.pieces.size(); ++i)
        if (en.pieces[i].alive && en.pieces[i].needsRebuild) dirty.push_back(i);
    if (dirty.empty()) return false;
    if (rebuildsThisTick_ >= opt_.maxRebuildsPerFrame) return false;   // carried: the pieces stay dirty
    ++rebuildsThisTick_;

    // 1. duplicate particles on the fans a break disconnected; 2. the draw side follows; 3. islands.
    resolveTears(c);
    en.owner.resize(c.particles.size(), kNone);
    for (const SplitEvent& ev : c.splitLog) en.owner[ev.newParticle] = en.owner[ev.oldParticle];
    std::vector<u32> sources;
    en.sim.binding.applySplits(c.splitLog, &sources);
    for (const u32 s : sources) en.renderSrc.push_back(s < en.renderSrc.size() ? en.renderSrc[s] : s);
    c.splitLog.clear();
    rebuildBends(c);
    c.topologyDirty = false;

    const u32 minTris = std::max(1u, en.asset.destruct.minPieceTriangles);
    const IslandPlan plan = planIslands(c, minTris);
    std::vector<u32> comp;
    countPieces(c, &comp);

    // Islands of each dirty piece, in plan order. State maps are taken before any body changes.
    std::map<u32, std::vector<u32>> groups;
    std::vector<std::vector<i32>> maps(plan.islands.size());
    for (u32 k = 0; k < plan.islands.size(); ++k) {
        const u32 idx = en.owner[plan.islands[k].particles.front()];
        if (idx >= en.pieces.size() || !en.pieces[idx].alive || !en.pieces[idx].needsRebuild) continue;
        if (plan.islands[k].triangles.empty()) {
            // A pinned fragment with no cloth left on it has nothing to simulate: it is not a body.
            for (const u32 pid : plan.islands[k].particles) en.owner[pid] = kNone;
            continue;
        }
        groups[idx].push_back(k);
        for (const u32 pid : plan.islands[k].particles) maps[k].push_back(static_cast<i32>(c.particles[pid].simIndex));
    }

    // Dust: one event per connected cluster, only for particles of the dirty pieces.
    {
        std::map<u32, std::vector<u32>> clusters;
        for (const u32 pid : plan.dust) {
            const u32 idx = pid < en.owner.size() ? en.owner[pid] : kNone;
            if (idx < en.pieces.size() && en.pieces[idx].alive && en.pieces[idx].needsRebuild) clusters[comp[pid]].push_back(pid);
        }
        for (const auto& [cid, parts] : clusters) {
            Vec3 sum{0, 0, 0};
            for (const u32 pid : parts) { sum += c.particles[pid].pos; en.owner[pid] = kNone; }
            emit(en, nullptr, SoftEventKind::Dust, 0, sum / static_cast<f32>(parts.size()), static_cast<f32>(parts.size()));
        }
    }

    u32 maxPieces = std::max(1u, en.asset.destruct.maxPieces);
    bool noDebris = false;
    if (const auto* de = world.component<CDestructible>(en.entity, destructibleType())) {
        if (de->maxPieces) maxPieces = de->maxPieces;
        noDebris = (de->flags & kDestructNoDebris) != 0;
    }
    auto aliveCount = [&] {
        u32 n = 0;
        for (const Piece& p : en.pieces) n += p.alive ? 1u : 0u;
        return n;
    };

    for (const u32 pidx : dirty) {
        const std::vector<u32> ks = groups.count(pidx) ? groups[pidx] : std::vector<u32>{};
        if (ks.empty()) {
            // Everything that was left is dust.
            Piece& d = en.pieces[pidx];
            if (d.body) { aver_phys_remove_body(d.body); byBody_.erase(d.body); d.body = 0; }
            if (d.debris) {
                destroyPiece(world, en, d);
            } else {
                d.empty = true;
                d.particles.clear(); d.beams.clear(); d.triangles.clear();
                d.drawIndices.clear(); d.renderVerts.clear(); d.sourceVertex.clear(); d.drawWorld.clear();
                d.needsRebuild = false;
                if (auto* mr = world.component<scene::CMeshRenderer>(en.entity, scene::kComponentMeshRenderer)) {
                    mr->flags &= ~scene::kMeshRendererVisible;
                    en.hidden = true;
                }
                ++d.epoch;
            }
            continue;
        }

        // Detached islands first, reading the old body.
        for (usize j = 1; j < ks.size(); ++j) {
            const IslandPlan::Island& isl = plan.islands[ks[j]];
            auto toDust = [&] {
                Vec3 sum{0, 0, 0};
                for (const u32 pid : isl.particles) { sum += c.particles[pid].pos; en.owner[pid] = kNone; }
                emit(en, nullptr, SoftEventKind::Dust, 0, sum / static_cast<f32>(isl.particles.size()), static_cast<f32>(isl.particles.size()));
            };
            if (noDebris || aliveCount() >= maxPieces) { toDust(); continue; }
            Piece np;
            np.id = en.nextPieceId++;
            np.debris = true;
            np.particles = isl.particles; np.beams = isl.beams; np.triangles = isl.triangles;
            np.anchored = isl.anchored;
            np.plastic = en.pieces[pidx].plastic;
            Vec3 sum{0, 0, 0};
            for (const u32 pid : isl.particles) sum += c.particles[pid].pos;
            np.origin = np.centroid = sum / static_cast<f32>(isl.particles.size());
            np.entity = makeDebrisEntity(world, en, np);
            en.pieces.push_back(std::move(np));
            const u32 newIdx = static_cast<u32>(en.pieces.size() - 1);
            Piece& ref = en.pieces[newIdx];
            if (!buildPieceBody(en, ref, 0, en.pieces[pidx].body, &maps[ks[j]], ref.plastic)) {
                AVER_WARN("[SoftBody] entity {}: a detached piece could not be built; it is dropped", en.entity);
                destroyPiece(world, en, ref);
                toDust();
                continue;
            }
            for (const u32 pid : isl.particles) en.owner[pid] = newIdx;
            if (auto* sb = world.component<scene::CSoftBody>(ref.entity, scene::kComponentSoftBody)) sb->body = ref.body;
            fillPieceDraw(en, ref);
            emit(en, &ref, SoftEventKind::Detached, 0, ref.centroid, isl.massKg);
        }

        // Then the piece keeps the first island and its body handle.
        Piece& d = en.pieces[pidx];
        const IslandPlan::Island& first = plan.islands[ks[0]];
        d.particles = first.particles; d.beams = first.beams; d.triangles = first.triangles;
        d.anchored = first.anchored;
        for (const u32 pid : first.particles) en.owner[pid] = pidx;
        if (!buildPieceBody(en, d, d.body, d.body, &maps[ks[0]], d.plastic))
            AVER_WARN("[SoftBody] entity {}: the torn body could not be rebuilt; it keeps its previous shape", en.entity);
        d.needsRebuild = false;
        fillPieceDraw(en, d);
        if (pidx == 0)
            if (auto* sb = world.component<scene::CSoftBody>(en.entity, scene::kComponentSoftBody)) sb->body = d.body;
    }
    return true;
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
    emit(en, en.pieces.empty() ? nullptr : &en.pieces.front(), SoftEventKind::Released, 0,
         en.pieces.empty() ? Vec3{0, 0, 0} : en.pieces.front().centroid, 0.0f);
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
    emit(again, again.pieces.empty() ? nullptr : &again.pieces.front(), SoftEventKind::Repaired, 0,
         again.pieces.empty() ? Vec3{0, 0, 0} : again.pieces.front().centroid, 0.0f);
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
            const bool asleep = p.body && !p.frozen && aver_phys_sb_is_active(p.body) == 0;
            p.asleepFor = asleep ? p.asleepFor + dt : 0.0f;
            if (!p.shrinking && p.asleepFor >= opt_.debrisSleepS && p.age >= life) p.shrinking = true;
            if (p.shrinking) {
                p.shrinkT += dt;
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
