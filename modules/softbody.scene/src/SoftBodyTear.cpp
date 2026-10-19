// The tear pipeline in two halves. planTear is topology and the piece table: resolve the tears, follow them in the
// render binding, plan the islands, decide which become pieces and which dust. It touches no physics, no entity and
// raises no event, so a saved tear log can be replayed through it (SoftBodySave.cpp). commitTear then makes the
// bodies, the debris entities and the events for what planTear left marked. Live ticks call them back to back.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {

// The beams that broke since the last run, as one group of the log (ascending ids).
void SoftBodySystem::appendTearLog(Entry& en) {
    const Cage& c = en.sim.cage;
    if (en.logged.size() < c.beams.size()) en.logged.resize(c.beams.size(), 0);
    std::vector<u32> group;
    for (u32 bi = 0; bi < c.beams.size(); ++bi)
        if (c.beams[bi].broken && !en.logged[bi]) { group.push_back(bi); en.logged[bi] = 1; }
    if (!group.empty()) en.tearLog.push_back(std::move(group));
}

// A piece with nothing left of it. A debris piece is destroyed; a root keeps its entity, hidden, so Repair restores it.
void SoftBodySystem::killPiece(scene::World& world, Entry& en, Piece& p) {
    if (p.body) {
        aver_phys_remove_body(p.body);
        byBody_.erase(p.body);
        p.body = 0;
    }
    if (p.debris) {
        destroyPiece(world, en, p);
        needFlush_ = true;
        return;
    }
    if (p.capEntity != scene::kInvalidEntity) {
        if (world.valid(p.capEntity)) world.destroy(p.capEntity);
        p.capEntity = scene::kInvalidEntity;
    }
    p.empty = true;
    p.pendingKill = false;
    p.needsBody = false;
    p.needsRebuild = false;
    p.particles.clear(); p.beams.clear(); p.triangles.clear();
    p.drawIndices.clear(); p.renderVerts.clear(); p.sourceVertex.clear(); p.drawUv.clear(); p.drawWeld.clear(); p.drawEvalTri.clear(); p.drawWorld.clear();
    p.caps = CapGeometry{};
    p.capWorld.clear(); p.capIndices.clear(); p.capSource.clear(); p.capUv.clear();
    if (auto* mr = world.component<scene::CMeshRenderer>(en.entity, scene::kComponentMeshRenderer)) {
        mr->flags &= ~scene::kMeshRendererVisible;
        mr->dirty = 1;
        en.hidden = true;
    }
    ++p.epoch;
}

bool SoftBodySystem::planTear(scene::World& world, Entry& en) {
    Cage& c = en.sim.cage;
    std::vector<u32> dirty;
    for (u32 i = 0; i < en.pieces.size(); ++i)
        if (en.pieces[i].alive && !en.pieces[i].pendingKill && en.pieces[i].needsRebuild) dirty.push_back(i);
    if (dirty.empty()) return false;
    appendTearLog(en);

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

    // Dust: one cluster per connected set of dust particles, only for particles of the dirty pieces.
    {
        std::map<u32, std::vector<u32>> clusters;
        for (const u32 pid : plan.dust) {
            const u32 idx = pid < en.owner.size() ? en.owner[pid] : kNone;
            if (idx < en.pieces.size() && en.pieces[idx].alive && en.pieces[idx].needsRebuild) clusters[comp[pid]].push_back(pid);
        }
        for (const auto& [cid, parts] : clusters) {
            Vec3 sum{0, 0, 0};
            for (const u32 pid : parts) { sum += c.particles[pid].pos; en.owner[pid] = kNone; }
            en.pendingDust.emplace_back(sum / static_cast<f32>(parts.size()), static_cast<u32>(parts.size()));
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
        for (const Piece& p : en.pieces) n += p.alive && !p.pendingKill ? 1u : 0u;
        return n;
    };

    for (const u32 pidx : dirty) {
        const std::vector<u32> ks = groups.count(pidx) ? groups[pidx] : std::vector<u32>{};
        if (ks.empty()) {
            // Everything that was left is dust.
            en.pieces[pidx].pendingKill = true;
            en.pieces[pidx].needsRebuild = false;
            en.pieces[pidx].needsBody = false;
            continue;
        }

        // Detached islands become new debris pieces (bodies and entities come at commit), or dust past the cap.
        for (usize j = 1; j < ks.size(); ++j) {
            const IslandPlan::Island& isl = plan.islands[ks[j]];
            if (noDebris || (!restoring_ && aliveCount() >= maxPieces)) {
                Vec3 sum{0, 0, 0};
                for (const u32 pid : isl.particles) { sum += c.particles[pid].pos; en.owner[pid] = kNone; }
                en.pendingDust.emplace_back(sum / static_cast<f32>(isl.particles.size()), static_cast<u32>(isl.particles.size()));
                continue;
            }
            Piece np;
            np.id = en.nextPieceId++;
            np.debris = true;
            np.particles = isl.particles; np.beams = isl.beams; np.triangles = isl.triangles;
            np.anchored = isl.anchored;
            np.plastic = en.pieces[pidx].plastic;
            np.massKg = isl.massKg;
            np.needsBody = true;
            np.srcPiece = pidx;
            np.stateMap = maps[ks[j]];
            Vec3 sum{0, 0, 0};
            for (const u32 pid : isl.particles) sum += c.particles[pid].pos;
            np.origin = np.centroid = sum / static_cast<f32>(isl.particles.size());
            en.pieces.push_back(std::move(np));
            const u32 newIdx = static_cast<u32>(en.pieces.size() - 1);
            for (const u32 pid : isl.particles) en.owner[pid] = newIdx;
        }

        // The piece keeps the first island and its body handle.
        Piece& d = en.pieces[pidx];
        const IslandPlan::Island& first = plan.islands[ks[0]];
        d.particles = first.particles; d.beams = first.beams; d.triangles = first.triangles;
        d.anchored = first.anchored;
        for (const u32 pid : first.particles) en.owner[pid] = pidx;
        d.needsRebuild = false;
        d.needsBody = true;
        d.srcPiece = pidx;
        d.stateMap = maps[ks[0]];
    }
    return true;
}

void SoftBodySystem::commitTear(scene::World& world, Entry& en) {
    Cage& c = en.sim.cage;

    // Pieces with nothing left.
    for (u32 i = 0; i < en.pieces.size(); ++i)
        if (en.pieces[i].pendingKill) killPiece(world, en, en.pieces[i]);

    // Dust clusters.
    for (const auto& [centre, count] : en.pendingDust) {
        emit(en, nullptr, SoftEventKind::Dust, 0, centre, static_cast<f32>(count));
        emitFx(en, FxSlot::Dust, centre, Vec3{0, 0, 1}, static_cast<f32>(count), count);
    }
    en.pendingDust.clear();

    // The saved state of a restored cage, per particle, handed to the builder in each piece's vertex order.
    const bool restored = !en.restorePos.empty();
    auto stateOf = [&](const Piece& p, std::vector<f32>& pos, std::vector<f32>& vel) {
        pos.resize(p.particles.size() * 3);
        vel.resize(p.particles.size() * 3);
        for (usize i = 0; i < p.particles.size(); ++i)
            for (usize a = 0; a < 3; ++a) {
                pos[i * 3 + a] = en.restorePos[static_cast<usize>(p.particles[i]) * 3 + a];
                vel[i * 3 + a] = en.restoreVel[static_cast<usize>(p.particles[i]) * 3 + a];
            }
    };

    // New debris pieces first: they read their starting state from the parent's body before it is rebuilt.
    for (u32 idx = 0; idx < en.pieces.size(); ++idx) {
        if (!en.pieces[idx].alive || !en.pieces[idx].needsBody || !en.pieces[idx].debris || en.pieces[idx].body != 0) continue;
        en.pieces[idx].entity = makeDebrisEntity(world, en, en.pieces[idx]);
        Piece& ref = en.pieces[idx];
        const i32 srcBody = !restored && ref.srcPiece < en.pieces.size() ? en.pieces[ref.srcPiece].body : 0;
        std::vector<f32> sp, sv;
        if (restored) stateOf(ref, sp, sv);
        const bool ok = buildPieceBody(en, ref, 0, srcBody, srcBody ? &ref.stateMap : nullptr, ref.plastic, restored ? sp.data() : nullptr, restored ? sv.data() : nullptr);
        if (!ok) {
            AVER_WARN("[SoftBody] entity {}: a detached piece could not be built; it is dropped", en.entity);
            Vec3 sum{0, 0, 0};
            const u32 n = static_cast<u32>(ref.particles.size());
            for (const u32 pid : ref.particles) { sum += c.particles[pid].pos; en.owner[pid] = kNone; }
            destroyPiece(world, en, ref);
            if (n) {
                emit(en, nullptr, SoftEventKind::Dust, 0, sum / static_cast<f32>(n), static_cast<f32>(n));
                emitFx(en, FxSlot::Dust, sum / static_cast<f32>(n), Vec3{0, 0, 1}, static_cast<f32>(n), n);
            }
            continue;
        }
        if (auto* sb = world.component<scene::CSoftBody>(ref.entity, scene::kComponentSoftBody)) sb->body = ref.body;
        ref.needsBody = false;
        fillPieceDraw(en, ref);
        emit(en, &ref, SoftEventKind::Detached, 0, ref.centroid, ref.massKg);
        if (!restoring_) {
            // The piece flies the way it was moving: its mean velocity as the effect's direction.
            Vec3 dir{0, 0, 1};
            std::vector<f32> vel(ref.particles.size() * 3);
            if (aver_phys_sb_state(ref.body, nullptr, vel.data(), static_cast<i32>(ref.particles.size())) == static_cast<i32>(ref.particles.size())) {
                Vec3 sum{0, 0, 0};
                for (usize i = 0; i < ref.particles.size(); ++i) sum += Vec3{vel[i * 3], vel[i * 3 + 1], vel[i * 3 + 2]};
                if (sum.sizeSquared() > 1e-6f) dir = sum.getSafeNormal();
            }
            emitFx(en, FxSlot::Detach, ref.centroid, dir, ref.massKg);
        }
    }

    // Then every piece that keeps its body handle (or, restoring, the root's first body).
    for (u32 idx = 0; idx < en.pieces.size(); ++idx) {
        Piece& d = en.pieces[idx];
        if (!d.alive || !d.needsBody) continue;
        std::vector<f32> sp, sv;
        if (restored) stateOf(d, sp, sv);
        const bool fresh = d.body == 0;
        const i32 oldBody = d.body;
        if (!buildPieceBody(en, d, oldBody, restored ? 0 : oldBody, restored || fresh ? nullptr : &d.stateMap, d.plastic,
                            restored ? sp.data() : nullptr, restored ? sv.data() : nullptr))
            AVER_WARN("[SoftBody] entity {}: the torn body could not be rebuilt; it keeps its previous shape", en.entity);
        d.needsBody = false;
        d.needsRebuild = false;
        fillPieceDraw(en, d);
        if (idx == 0)
            if (auto* sb = world.component<scene::CSoftBody>(en.entity, scene::kComponentSoftBody)) sb->body = d.body;
    }
}

bool SoftBodySystem::tearPipeline(scene::World& world, Entry& en) {
    bool dirty = false;
    for (const Piece& p : en.pieces) dirty = dirty || (p.alive && !p.pendingKill && p.needsRebuild);
    if (!dirty) return false;
    if (rebuildsThisTick_ >= opt_.maxRebuildsPerFrame) return false;   // carried: the pieces stay dirty
    ++rebuildsThisTick_;
    if (!planTear(world, en)) return false;
    commitTear(world, en);
    return true;
}

} // namespace aver::softbody
