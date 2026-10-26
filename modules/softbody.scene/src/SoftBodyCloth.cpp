// Cloth per frame: the skin palette and entity matrix pushed into the bodies, release, character proxies, and the
// distance tiers (iterations, freeze) with the active-particle budget.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/physics/physics_softbody_abi.h"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

// Writes a matrix as 16 row-major floats, the layout of the physics palette and world matrix.
void putMat(f32* dst, const Mat4& m) {
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c) dst[r * 4 + c] = m.m[r][c];
}

} // namespace

void SoftBodySystem::setCharacterProxy(scene::World& world, Entry& en, bool on) {
    if (!proxyFn_ || en.destructible || en.legacy) return;
    if (en.skeletonEntity == scene::kInvalidEntity || (en.spec.flags & kClothNoCharacterProxy)) return;
    if (on) {
        if (en.proxyOn) return;
        en.proxyOn = true;
        if (proxyUsers_[en.skeletonEntity]++ == 0) proxyFn_(en.skeletonEntity, true, proxyUser_);
    } else if (en.proxyOn) {
        en.proxyOn = false;
        const auto it = proxyUsers_.find(en.skeletonEntity);
        if (it != proxyUsers_.end() && --it->second == 0) {
            proxyUsers_.erase(it);
            if (world.valid(en.skeletonEntity)) proxyFn_(en.skeletonEntity, false, proxyUser_);
        }
    }
}

void SoftBodySystem::clothStep(scene::World& world, Entry& en, f32) {
    if (en.failed) return;
    if (!en.destructible && !en.legacy) {
        if (auto* cl = world.component<CCloth>(en.entity, clothType()))
            if (cl->flags & kClothRelease) {
                cl->flags &= ~kClothRelease;
                releaseEntry(world, en);
            }
    }
    bool any = false;
    for (const Piece& p : en.pieces) any = any || (p.alive && p.skinned && p.body && !p.frozen && !p.fieldHeld);
    const Mat4 W = world.worldMatrix(en.entity);
    if (!any) {
        en.lastWorld = W;
        return;
    }

    // The palette in the build frame: bind (world at build) x W0^-1 x skin x W(t), see aver_phys_sb_skin_world.
    std::vector<f32> jm(static_cast<usize>(en.jointCount) * 16);
    if (en.propPalette) {
        for (u32 j = 0; j < en.jointCount; ++j) putMat(jm.data() + j * 16, en.invBuildWorld);
    } else {
        u32 count = 0;
        const Mat4* pal = paletteSet_ ? (paletteFn_ ? paletteFn_(en.skeletonEntity, count, paletteUser_) : nullptr)
                                      : anim::animSystem().skinning(en.skeletonEntity, count);
        if (!pal || count < en.jointCount) return;   // keep last frame's pose
        for (u32 j = 0; j < en.jointCount; ++j) putMat(jm.data() + j * 16, en.invBuildWorld * pal[j]);
    }
    f32 w16[16];
    putMat(w16, W);
    bool moved = !en.hasLastWorld;
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c) moved = moved || std::fabs(W.m[r][c] - en.lastWorld.m[r][c]) > 1e-4f;
    const Vec3 now{W.m[3][0], W.m[3][1], W.m[3][2]};
    const Vec3 was{en.lastWorld.m[3][0], en.lastWorld.m[3][1], en.lastWorld.m[3][2]};
    const bool hard = en.hardSkinNext || dist(now, was) > opt_.teleportCm;

    for (Piece& p : en.pieces) {
        if (!p.alive || !p.skinned || !p.body || p.frozen || p.fieldHeld) continue;
        aver_phys_sb_skin_world(p.body, jm.data(), static_cast<i32>(en.jointCount), w16, hard ? 1 : 0);
        // A moving driver (an animated skeleton, a cart) keeps the body awake; a still prop may sleep.
        if (moved || !en.propPalette || hard) {
            aver_phys_sb_set_active(p.body, 1);
            p.needsRead = true;
        }
    }
    en.hardSkinNext = false;
    en.lastWorld = W;
    en.hasLastWorld = true;
}

void SoftBodySystem::rebase(const Vec3& d) {
    const auto shiftAll = [&d](std::vector<Vec3>& v) { for (Vec3& p : v) p -= d; };
    const auto shiftMat = [&d](Mat4& m) { m.m[3][0] -= d.x; m.m[3][1] -= d.y; m.m[3][2] -= d.z; };
    for (auto& [e, en] : entries_) {
        shiftMat(en.lastWorld);
        for (auto& dust : en.pendingDust) dust.first -= d;
        for (Particle& q : en.sim.cage.particles) q.pos -= d;
        for (Piece& p : en.pieces) {
            if (p.debris || p.baked) p.origin -= d;
            p.centroid -= d;
            p.boundsMin -= d;
            p.boundsMax -= d;
            shiftAll(p.drawWorld);
            shiftAll(p.capWorld);
        }
    }
    for (InboxItem& it : inbox_) {
        switch (it.kind) {
            case InboxItem::Kind::Damage: case InboxItem::Kind::Explode: case InboxItem::Kind::Tear: case InboxItem::Kind::Repair:
                it.point -= d;
                break;
            case InboxItem::Kind::FieldStart: case InboxItem::Kind::Pulse:
                shiftMat(it.field.world);
                break;
            default: break;
        }
    }
    for (auto& [h, t] : fields_) shiftMat(t.spec.world);
}

void SoftBodySystem::applyLodAndFreeze(scene::World& world, f32) {
    struct Ref { Entry* en; Piece* p; f32 d; f32 freezeAt; };
    std::vector<Ref> refs;
    for (auto& [e, en] : entries_) {
        if (en.failed) continue;
        f32 clothFreeze = opt_.freezeDistanceClothCm;
        if (const auto* cl = world.component<CCloth>(e, clothType()))
            if (cl->freezeDistanceCm > 0.0f) clothFreeze = cl->freezeDistanceCm;
        for (Piece& p : en.pieces) {
            if (!p.alive || p.empty || !p.body || p.fieldHeld) continue;   // a held piece is paused by a field, not by distance
            const f32 d = dist(p.centroid, viewPos_);
            const f32 fz = (en.destructible || en.legacy || p.debris) ? opt_.freezeDistanceDebrisCm : clothFreeze;
            refs.push_back({&en, &p, d, fz});

            const u32 tier = d < opt_.lodNearCm ? 0u : (d < opt_.lodMidCm ? 1u : 2u);
            if (!p.frozen && tier != p.lodTier) {
                f32 prm[AVER_PHYS_SB_P_COUNT];
                for (f32& f : prm) f = std::nanf("");
                const f32 factor = tier == 0 ? 1.0f : (tier == 1 ? 0.5f : 0.25f);
                prm[AVER_PHYS_SB_P_ITERATIONS] = std::max(2.0f, std::round(static_cast<f32>(en.asset.sim.iterations) * factor));
                aver_phys_sb_set_params(p.body, prm, AVER_PHYS_SB_P_COUNT);
                p.lodTier = tier;
            }
        }
    }

    // Distance freeze with a 10% hysteresis band.
    u32 active = 0;
    for (Ref& r : refs) {
        Piece& p = *r.p;
        if (!p.frozen && r.d > r.freezeAt) {
            aver_phys_sb_set_active(p.body, 0);
            p.frozen = true;
        }
    }
    for (const Ref& r : refs)
        if (!r.p->frozen) active += static_cast<u32>(r.p->particles.size());

    // Active-particle budget: the farthest freeze first; the nearest frozen ones wake when there is room.
    std::vector<Ref*> byFar;
    for (Ref& r : refs) byFar.push_back(&r);
    std::sort(byFar.begin(), byFar.end(), [](const Ref* a, const Ref* b) {
        if (a->d != b->d) return a->d > b->d;
        if (a->en->entity != b->en->entity) return a->en->entity < b->en->entity;
        return a->p->id < b->p->id;
    });
    for (Ref* r : byFar) {
        if (active <= opt_.maxActiveParticles) break;
        if (r->p->frozen) continue;
        aver_phys_sb_set_active(r->p->body, 0);
        r->p->frozen = true;
        active -= std::min<u32>(active, static_cast<u32>(r->p->particles.size()));
    }
    for (auto it = byFar.rbegin(); it != byFar.rend(); ++it) {
        Ref& r = **it;
        Piece& p = *r.p;
        if (!p.frozen || r.d >= r.freezeAt * 0.9f) continue;
        const u32 n = static_cast<u32>(p.particles.size());
        if (active + n > opt_.maxActiveParticles) continue;
        p.frozen = false;
        p.needsRead = true;
        aver_phys_sb_set_active(p.body, 1);
        active += n;
    }
}

}  // namespace aver::softbody
