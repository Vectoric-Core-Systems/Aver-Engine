// Making the Jolt body for a piece of an entry's cage, and the draw mesh description that goes with it.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

f32 lodFactor(u32 tier) { return tier == 1 ? 0.5f : (tier == 2 ? 0.25f : 1.0f); }

} // namespace

bool SoftBodySystem::buildPieceBody(Entry& en, Piece& p, i32 replaceBody, i32 stateSrcBody, const std::vector<i32>* stateMap, bool carryPose) {
    Cage& c = en.sim.cage;
    const u32 n = static_cast<u32>(p.particles.size());
    if (n == 0 || n > 65536u) return false;
    const SoftAsset& a = en.asset;

    std::vector<i32> local(c.particles.size(), -1);
    for (u32 i = 0; i < n; ++i) local[p.particles[i]] = static_cast<i32>(i);

    std::vector<f32> rest(static_cast<usize>(n) * 3), inv(n);
    Vec3 centre{0, 0, 0};
    for (u32 i = 0; i < n; ++i) {
        const Particle& q = c.particles[p.particles[i]];
        rest[i * 3 + 0] = q.rest.x; rest[i * 3 + 1] = q.rest.y; rest[i * 3 + 2] = q.rest.z;
        inv[i] = q.pinned ? 0.0f : q.invMass;
        centre += q.rest;
    }
    centre = centre / static_cast<f32>(n);

    std::vector<i32> faces;
    for (const u32 t : p.triangles) {
        const Triangle& tri = c.triangles[t];
        const i32 l0 = local[tri.v[0]], l1 = local[tri.v[1]], l2 = local[tri.v[2]];
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        faces.push_back(l0); faces.push_back(l1); faces.push_back(l2);
    }
    std::vector<i32> edges;
    std::vector<f32> edgeRest, edgeComp;
    std::vector<u32> edgeBeams;
    for (const u32 bi : p.beams) {
        const Beam& b = c.beams[bi];
        if (b.broken) continue;
        const i32 la = local[b.a], lb = local[b.b];
        if (la < 0 || lb < 0 || la == lb) continue;
        edges.push_back(la); edges.push_back(lb);
        edgeRest.push_back(b.rest);
        edgeComp.push_back(c.materials[b.material].compliance);
        edgeBeams.push_back(bi);
    }
    std::vector<i32> bends;
    std::vector<f32> bendComp;
    std::vector<u32> bendIds;
    for (u32 k = 0; k < c.bends.size(); ++k) {
        const Bend& bd = c.bends[k];
        if (c.beams[bd.beam].broken) continue;
        i32 q[4];
        bool inside = true;
        for (int j = 0; j < 4; ++j) { q[j] = local[bd.v[j]]; inside = inside && q[j] >= 0; }
        if (!inside) continue;
        for (int j = 0; j < 4; ++j) bends.push_back(q[j]);
        bendComp.push_back(bd.compliance);
        bendIds.push_back(k);
    }
    if (faces.empty()) return false;

    // Skin: the root piece and any anchored piece stay tethered; free debris falls clear of the entity.
    const bool useSkin = (!p.debris || p.anchored) && !en.released;
    std::vector<i32> joints;
    std::vector<f32> weights, reach;
    bool anySkin = false;
    if (useSkin) {
        joints.assign(static_cast<usize>(n) * 4, 0);
        weights.assign(static_cast<usize>(n) * 4, 0.0f);
        reach.assign(n, -1.0f);
        const bool meshSkin = !en.propPalette && !en.sim.joint4.empty();
        for (u32 i = 0; i < n; ++i) {
            const u32 pid = p.particles[i];
            const Particle& q = c.particles[pid];
            if (q.reachCm < 0.0f) continue;
            // A duplicate takes its origin's skin influences.
            const u32 src = q.origin;
            if (meshSkin) {
                for (u32 k = 0; k < 4; ++k) {
                    joints[i * 4 + k] = en.sim.joint4[src * 4 + k];
                    weights[i * 4 + k] = en.sim.weight4[src * 4 + k];
                }
            } else {
                joints[i * 4] = 0;
                weights[i * 4] = 1.0f;
            }
            reach[i] = q.reachCm;
            anySkin = true;
        }
    }
    p.skinned = anySkin;

    f32 prm[AVER_PHYS_SB_P_COUNT];
    for (f32& f : prm) f = std::nanf("");
    const Material& m = c.materials.size() > 1 ? c.materials[1] : c.materials[0];
    prm[AVER_PHYS_SB_P_ITERATIONS] = std::max(2.0f, std::round(static_cast<f32>(a.sim.iterations) * lodFactor(p.lodTier)));
    prm[AVER_PHYS_SB_P_DAMPING] = a.sim.damping;
    prm[AVER_PHYS_SB_P_FRICTION] = m.friction;
    prm[AVER_PHYS_SB_P_RESTITUTION] = m.restitution;
    prm[AVER_PHYS_SB_P_GRAVITY_SCALE] = a.sim.gravityScale;
    prm[AVER_PHYS_SB_P_VERTEX_RADIUS_CM] = a.sim.vertexRadiusCm;
    prm[AVER_PHYS_SB_P_MAX_SPEED_CMS] = a.sim.maxSpeedCmS;
    prm[AVER_PHYS_SB_P_ALLOW_SLEEP] = a.sim.allowSleep ? 1.0f : 0.0f;
    prm[AVER_PHYS_SB_P_DOUBLE_SIDED] = en.destructible ? 0.0f : 1.0f;
    prm[AVER_PHYS_SB_P_IMPACTS] = en.destructible ? 1.0f : 0.0f;
    const bool noWorld = !en.destructible && (en.spec.flags & kClothNoWorld) != 0;
    prm[AVER_PHYS_SB_P_LAYER] = static_cast<f32>(noWorld ? opt_.proxyLayer : opt_.clothLayer);

    const i32 b = aver_phys_sb_builder_begin(static_cast<i32>(n));
    if (!b) return false;
    aver_phys_sb_builder_vertices(b, rest.data(), inv.data(), nullptr, nullptr);
    aver_phys_sb_builder_faces(b, faces.data(), static_cast<i32>(faces.size() / 3));
    if (!edgeRest.empty())
        aver_phys_sb_builder_edges(b, edges.data(), edgeRest.data(), edgeComp.data(), static_cast<i32>(edgeRest.size()));
    if (!bendComp.empty())
        aver_phys_sb_builder_bends(b, bends.data(), bendComp.data(), static_cast<i32>(bendComp.size()));
    if (anySkin)
        aver_phys_sb_builder_skin(b, joints.data(), weights.data(), reach.data(), a.cloth.backstopCm, static_cast<i32>(en.jointCount));
    aver_phys_sb_builder_params(b, prm, AVER_PHYS_SB_P_COUNT);

    std::vector<i32> edgeSlot(edgeRest.size(), -1), bendSlot(bendComp.size(), -1);
    const i32 h = aver_phys_sb_builder_finish(b, centre.x, centre.y, centre.z, replaceBody, stateSrcBody,
                                              stateMap && !stateMap->empty() ? stateMap->data() : nullptr,
                                              edgeSlot.empty() ? nullptr : edgeSlot.data(), bendSlot.empty() ? nullptr : bendSlot.data());
    if (!h) {
        aver_phys_sb_builder_cancel(b);
        return false;
    }
    if (replaceBody) byBody_.erase(replaceBody);
    p.body = h;
    byBody_[h] = {en.entity, p.id};
    aver_phys_set_entity(h, static_cast<i32>(en.entity));

    for (u32 i = 0; i < n; ++i) c.particles[p.particles[i]].simIndex = i;
    for (usize k = 0; k < edgeBeams.size(); ++k) c.beams[edgeBeams[k]].slot = static_cast<u32>(edgeSlot[k]);
    for (usize k = 0; k < bendIds.size(); ++k) c.bends[bendIds[k]].slot = static_cast<u32>(bendSlot[k]);
    p.edgeSlot = edgeSlot;
    p.bendSlot = bendSlot;

    if (carryPose && !bendSlot.empty()) aver_phys_sb_rest_bends_from_pose(h, bendSlot.data(), static_cast<i32>(bendSlot.size()));
    if (!en.destructible && !(en.spec.flags & kClothNoWind) && !en.legacy) {
        const f32 wind = en.windScale;
        aver_phys_sb_set_aero(h, std::clamp(a.cloth.dragCoeff * wind, 0.0f, 2.0f), std::clamp(a.cloth.liftCoeff * wind, 0.0f, 2.0f), 0);
    }
    p.needsRebuild = false;
    p.needsRead = true;
    return true;
}

void SoftBodySystem::fillPieceDraw(Entry& en, Piece& p) {
    const RenderBinding& rb = en.sim.binding;
    std::vector<u32> tris;
    for (const u32 st : p.triangles) {
        if (st + 1 >= rb.simStart.size()) continue;
        for (u32 s = rb.simStart[st]; s < rb.simStart[st + 1]; ++s) tris.push_back(rb.simList[s]);
    }
    std::sort(tris.begin(), tris.end());
    std::vector<u32> localOf(rb.vertexCount(), kNone);
    p.renderVerts.clear();
    p.sourceVertex.clear();
    p.drawIndices.clear();
    for (const u32 r : tris)
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = rb.indices[r * 3 + k];
            if (v >= localOf.size()) continue;
            if (localOf[v] == kNone) {
                localOf[v] = static_cast<u32>(p.renderVerts.size());
                p.renderVerts.push_back(v);
                p.sourceVertex.push_back(v < en.renderSrc.size() ? en.renderSrc[v] : v);
            }
            p.drawIndices.push_back(localOf[v]);
        }
    p.drawWorld.assign(p.renderVerts.size(), Vec3{0, 0, 0});
    p.empty = tris.empty();
    ++p.epoch;
    refreshDraw(en, p);
}

bool SoftBodySystem::initialPiece(Entry& en) {
    Cage& c = en.sim.cage;
    en.pieces.clear();
    Piece p;
    p.id = en.nextPieceId++;
    p.entity = en.entity;
    for (u32 i = 0; i < c.particles.size(); ++i) p.particles.push_back(i);
    for (u32 i = 0; i < c.beams.size(); ++i) p.beams.push_back(i);
    for (u32 i = 0; i < c.triangles.size(); ++i) p.triangles.push_back(i);
    for (const Particle& q : c.particles) p.anchored = p.anchored || q.pinned;
    en.owner.assign(c.particles.size(), 0);
    en.renderSrc.resize(en.sim.binding.vertexCount());
    for (u32 i = 0; i < en.renderSrc.size(); ++i) en.renderSrc[i] = i;
    en.pieces.push_back(std::move(p));
    Piece& root = en.pieces.back();
    if (!buildPieceBody(en, root, 0, 0, nullptr, false)) {
        en.pieces.clear();
        return false;
    }
    fillPieceDraw(en, root);
    return true;
}

} // namespace aver::softbody
