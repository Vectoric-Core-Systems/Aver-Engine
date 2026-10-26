// Saved damage: the `softbody` channel of Aver.Save's $State component. A save holds a delta against the pristine cage the
// loader rebuilds: the ordered tear log, per-beam set and damage, and per-particle position and velocity. Restoring replays the
// tear log through the topology-only half of the tear pipeline (planTear), then builds the bodies once (commitTear) with the
// saved state. Debris entities are not captured; the replay recreates them. See SoftState.hpp and docs/SOFTBODY.md 2.6.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_softbody_abi.h"

#include <algorithm>

namespace aver::softbody {

bool SoftBodySystem::saveState(scene::Entity e, std::string& out) const {
    const Entry* en = find(e);
    if (!en || en->failed) return false;
    const Cage& c = en->sim.cage;

    SoftState s;
    s.meshKey = en->saveMeshKey;
    s.assetKey = softAssetKey(en->asset);
    s.released = en->released;
    bool damaged = en->released;
    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
        const Beam& b = c.beams[bi];
        if (b.broken) damaged = true;
        if (b.rest != b.rest0 || b.plastic > 0.0f || b.damage > 0.0f) {
            s.beams.push_back(SoftStateBeam{bi, b.rest, b.plastic, b.damage});
            damaged = true;
        }
    }
    if (!damaged) return false;   // pristine: nothing to save

    // The tear log, with the breaks a pending tear has not yet reached appended as the last group so the loader gets to
    // the state the next tick would.
    s.tearGroups = en->tearLog;
    std::vector<u32> pending;
    for (u32 bi = 0; bi < c.beams.size(); ++bi)
        if (c.beams[bi].broken && (bi >= en->logged.size() || !en->logged[bi])) pending.push_back(bi);
    if (!pending.empty()) s.tearGroups.push_back(std::move(pending));

    // Positions and velocities from the live bodies; baked pieces and dust have the cage's last position and no velocity.
    const u32 n = static_cast<u32>(c.particles.size());
    s.particleCount = n;
    s.pos.resize(static_cast<usize>(n) * 3);
    s.vel.assign(static_cast<usize>(n) * 3, 0.0f);
    for (u32 i = 0; i < n; ++i) {
        s.pos[static_cast<usize>(i) * 3] = c.particles[i].pos.x;
        s.pos[static_cast<usize>(i) * 3 + 1] = c.particles[i].pos.y;
        s.pos[static_cast<usize>(i) * 3 + 2] = c.particles[i].pos.z;
    }
    std::vector<f32> pb, vb;
    for (const Piece& p : en->pieces) {
        if (!p.alive || !p.body || p.particles.empty()) continue;
        const i32 cnt = static_cast<i32>(p.particles.size());
        pb.resize(static_cast<usize>(cnt) * 3);
        vb.resize(static_cast<usize>(cnt) * 3);
        if (aver_phys_sb_state(p.body, pb.data(), vb.data(), cnt) != cnt) continue;
        for (i32 k = 0; k < cnt; ++k) {
            const usize pid = p.particles[static_cast<usize>(k)];
            for (usize a = 0; a < 3; ++a) {
                s.pos[pid * 3 + a] = pb[static_cast<usize>(k) * 3 + a];
                s.vel[pid * 3 + a] = vb[static_cast<usize>(k) * 3 + a];
            }
        }
    }

    // Particles whose piece is already gone (despawned debris, a killed root): the loader must not bring them back.
    for (u32 pid = 0; pid < n && pid < en->owner.size(); ++pid) {
        const u32 idx = en->owner[pid];
        if (idx < en->pieces.size() && (!en->pieces[idx].alive || en->pieces[idx].empty)) s.gone.push_back(pid);
    }

    out = encodeSoftState(s);
    return true;
}

bool SoftBodySystem::restoreState(scene::Entity e, std::string_view payload) {
    auto st = std::make_unique<SoftState>();
    SoftStateError err = SoftStateError::None;
    if (!decodeSoftState(payload, *st, &err)) {
        AVER_WARN("[SoftBody] entity {}: saved damage ignored ({})", e, softStateErrorText(err));
        return false;
    }
    // An entry that already exists was built without it: rebuild so the blob is applied.
    if (const auto it = entries_.find(e); it != entries_.end() && world_) {
        destroyEntry(*world_, it->second);
        entries_.erase(it);
    }
    pendingRestore_[e] = std::move(st);
    return true;
}

void SoftBodySystem::restoreBuild(scene::World& world, Entry& en) {
    Cage& c = en.sim.cage;
    const SoftState& st = *en.restore;
    restoring_ = true;

    // Beam deltas: the dented rest lengths, the set, the damage.
    bool plasticAny = false;
    for (const SoftStateBeam& b : st.beams) {
        if (b.beam >= c.beams.size()) continue;
        Beam& bm = c.beams[b.beam];
        bm.rest = b.rest; bm.plastic = b.plastic; bm.damage = b.damage;
        plasticAny = plasticAny || b.plastic > 0.0f;
    }
    {
        Piece& root = en.pieces.front();
        root.needsBody = true;
        root.plastic = plasticAny;
    }
    if (st.released) {
        for (Particle& q : c.particles) {
            q.pinned = false;
            q.reachCm = -1.0f;
            q.invMass = 1.0f / std::max(q.mass, 1e-4f);
        }
        en.released = true;
        en.pieces.front().anchored = false;
    }

    // The tear log, replayed group by group through the topology-only half of the pipeline.
    for (const std::vector<u32>& group : mergeTearGroups(st.tearGroups, opt_.restoreMaxReplayGroups)) {
        for (const u32 bi : group) {
            if (bi >= c.beams.size() || c.beams[bi].broken) continue;
            breakBeam(c, bi);
            const u32 idx = c.beams[bi].a < en.owner.size() ? en.owner[c.beams[bi].a] : kNone;
            if (idx < en.pieces.size() && en.pieces[idx].alive) en.pieces[idx].needsRebuild = true;
        }
        planTear(world, en);
    }
    en.brokenBeams = brokenBeamCount(c);

    // Pieces that had already gone are not recreated.
    for (const u32 pid : st.gone) {
        const u32 idx = pid < en.owner.size() ? en.owner[pid] : kNone;
        if (idx < en.pieces.size() && en.pieces[idx].alive) en.pieces[idx].pendingKill = true;
    }

    // The saved positions and velocities by particle id; particles the replay numbered beyond the saved count keep the cage's.
    const usize n = c.particles.size();
    en.restorePos.assign(n * 3, 0.0f);
    en.restoreVel.assign(n * 3, 0.0f);
    for (usize i = 0; i < n; ++i) {
        if (i < st.particleCount) {
            for (usize a = 0; a < 3; ++a) {
                en.restorePos[i * 3 + a] = st.pos[i * 3 + a];
                en.restoreVel[i * 3 + a] = st.vel[i * 3 + a];
            }
            c.particles[i].pos = Vec3{en.restorePos[i * 3], en.restorePos[i * 3 + 1], en.restorePos[i * 3 + 2]};
        } else {
            en.restorePos[i * 3] = c.particles[i].pos.x;
            en.restorePos[i * 3 + 1] = c.particles[i].pos.y;
            en.restorePos[i * 3 + 2] = c.particles[i].pos.z;
        }
    }

    commitTear(world, en);
    restoring_ = false;
    en.restorePos.clear();
    en.restoreVel.clear();
    en.restore.reset();
}

}  // namespace aver::softbody
