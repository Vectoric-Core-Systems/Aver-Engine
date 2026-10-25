// Rope and cable: the rod chain behind a CSoftRope, its anchors, the tube drawn around it, and the cut.
#include "aver/softbody/SoftShapeSystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/physics/physics_softbody_ext_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0], p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

// Where an end of the rope wants to be: the offset in its anchor entity's space, or a world point.
Vec3 anchorTarget(scene::World& w, const CSoftRope& cr, int end) {
    const u32 ent = end == 0 ? cr.startEntity : cr.endEntity;
    const f32* o = end == 0 ? cr.startOffset : cr.endOffset;
    const Vec3 off{o[0], o[1], o[2]};
    if (ent != 0 && w.valid(ent)) return xformPoint(w.worldMatrix(ent), off);
    return off;
}

}  // namespace

bool SoftShapeSystem::createRopeBody(RopeEntry& r, const std::vector<Vec3>& rest, const std::vector<Vec3>* vel, bool seeded) {
    const u32 n = static_cast<u32>(rest.size());
    if (n < 3 || n > 65536u) return false;
    const SoftAsset& a = r.asset;
    const RopeParams& rp = a.rope;

    std::vector<f32> pos(static_cast<usize>(n) * 3), velBuf;
    Vec3 centre{0, 0, 0};
    for (u32 i = 0; i < n; ++i) {
        pos[i * 3] = rest[i].x; pos[i * 3 + 1] = rest[i].y; pos[i * 3 + 2] = rest[i].z;
        centre += rest[i];
    }
    centre = centre / static_cast<f32>(n);
    if (seeded && vel && vel->size() == n) {
        velBuf.resize(static_cast<usize>(n) * 3);
        for (u32 i = 0; i < n; ++i) {
            velBuf[i * 3] = (*vel)[i].x; velBuf[i * 3 + 1] = (*vel)[i].y; velBuf[i * 3 + 2] = (*vel)[i].z;
        }
    }

    f32 prm[AVER_PHYS_SB_P_COUNT];
    for (f32& f : prm) f = std::nanf("");
    prm[AVER_PHYS_SB_P_ITERATIONS] = static_cast<f32>(a.sim.iterations);
    prm[AVER_PHYS_SB_P_DAMPING] = a.sim.damping;
    prm[AVER_PHYS_SB_P_FRICTION] = a.material.friction;
    prm[AVER_PHYS_SB_P_RESTITUTION] = a.material.restitution;
    prm[AVER_PHYS_SB_P_GRAVITY_SCALE] = a.sim.gravityScale;
    prm[AVER_PHYS_SB_P_VERTEX_RADIUS_CM] = r.radiusCm;   // a rope collides through its vertices: the tube radius is the thickness
    prm[AVER_PHYS_SB_P_MAX_SPEED_CMS] = a.sim.maxSpeedCmS;
    prm[AVER_PHYS_SB_P_ALLOW_SLEEP] = a.sim.allowSleep ? 1.0f : 0.0f;
    prm[AVER_PHYS_SB_P_DOUBLE_SIDED] = 0.0f;
    prm[AVER_PHYS_SB_P_LAYER] = static_cast<f32>((r.spec.flags & kRopeNoWorld) ? opt_.noWorldLayer : opt_.shapeLayer);
    prm[AVER_PHYS_SB_P_IMPACTS] = 0.0f;

    const i32 b = aver_phys_sb_builder_begin(static_cast<i32>(n));
    if (!b) return false;
    const bool staged = aver_phys_sb_builder_vertices(b, pos.data(), r.plan.invMass.data(), seeded ? pos.data() : nullptr,
                                                      velBuf.empty() ? nullptr : velBuf.data()) &&
                        aver_phys_sbx_builder_rod_chain(b, 0, static_cast<i32>(n), rp.stretchCompliance, rp.bendTwistCompliance) &&
                        aver_phys_sb_builder_params(b, prm, AVER_PHYS_SB_P_COUNT);
    const i32 h = staged ? aver_phys_sb_builder_finish(b, centre.x, centre.y, centre.z, 0, 0, nullptr, nullptr, nullptr) : 0;
    if (!h) {
        aver_phys_sb_builder_cancel(b);
        return false;
    }
    aver_phys_set_entity(h, static_cast<i32>(r.entity));
    r.body = h;
    r.nodes = n;
    r.centroid = centre;

    // Where each staged rod ended up (Optimize reorders): usually the identity, which allows one bulk read per tick.
    r.rodSlots.assign(n - 1, 0);
    if (aver_phys_sbx_slots(h, 2, r.rodSlots.data(), static_cast<i32>(n - 1)) != static_cast<i32>(n - 1)) {
        for (u32 i = 0; i + 1 < n; ++i) r.rodSlots[i] = static_cast<i32>(i);
    }
    r.slotsIdentity = true;
    for (u32 i = 0; i + 1 < n; ++i) r.slotsIdentity = r.slotsIdentity && r.rodSlots[i] == static_cast<i32>(i);
    return true;
}

SoftShapeSystem::Built SoftShapeSystem::buildRope(scene::World& world, RopeEntry& r) {
    const scene::Entity e = r.entity;
    const auto* cr = world.component<CSoftRope>(e, ropeType());
    if (!cr) return Built::NotReady;

    r.spec = RopeSpecKey{};
    r.spec.assetId = cr->asset;
    r.spec.lengthCm = cr->lengthCm;
    r.spec.radiusCm = cr->radiusCm;
    r.spec.flags = cr->flags & kRopeNoWorld;

    const auto fail = [&](const char* msg) {
        AVER_WARN("[SoftShape] entity {}: {}; the rope is not simulated", e, msg);
        r.failed = true;
        return Built::Failed;
    };

    r.asset = makeSoftAsset(SoftKind::Rope, SoftPreset::Rope);
    r.assetStale = false;
    if (cr->asset) {
        const SoftAsset* src = host().softAsset(cr->asset);
        if (src && src->kind == SoftKind::Rope) r.asset = *src;
        else r.assetStale = true;
    }
    const RopeParams& rp = r.asset.rope;
    r.radiusCm = cr->radiusCm > 0.0f ? cr->radiusCm : rp.radiusCm;
    r.sides = rp.sides;

    // The tube is drawn through the entity's renderer: it gets one (the engine cube as a placeholder whose draw is replaced) if it has none.
    auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!mr) {
        mr = static_cast<scene::CMeshRenderer*>(world.addComponent(e, scene::kComponentMeshRenderer));
        if (mr) {
            *mr = scene::CMeshRenderer{};
            mr->mesh = shapeRopeMeshId();
            mr->flags = scene::kMeshRendererVisible;
            mr->dirty = 1;
        }
    }
    if (mr && mr->material == 0 && !rp.material.empty()) {
        mr->material = aver_scene_material(0, rp.material.c_str());
        mr->dirty = 1;
    }

    // A piece made by a cut starts from the cut rope's nodes; anything else from a planned curve between the two ends.
    bool seeded = false;
    std::vector<Vec3> rest;
    std::vector<Vec3> vel;
    if (cr->flags & kRopeTail) {
        const auto sit = seeds_.find(e);
        if (sit != seeds_.end()) {
            if (sit->second.pos.size() >= 3) {
                seeded = true;
                rest = sit->second.pos;
                vel = sit->second.vel;
            }
            seeds_.erase(sit);
        }
    }
    if (seeded) {
        r.plan = RopeBuild{};
        r.plan.nodes = static_cast<u32>(rest.size());
        r.plan.rest = rest;
        r.plan.restLengthCm = polylineLength(rest.data(), r.plan.nodes);
        r.plan.segmentCm = r.plan.restLengthCm / static_cast<f32>(std::max(r.plan.nodes - 1, 1u));
        r.plan.nodeMassKg = rp.massPerMeterKg * r.plan.segmentCm * 0.01f;
        r.plan.invMass.assign(r.plan.nodes, 1.0f / std::max(r.plan.nodeMassKg, 1e-4f));
    } else {
        RopeSpec rs;
        rs.lengthCm = cr->lengthCm;
        rs.slack = rp.slack;
        rs.segmentCm = rp.segmentCm;
        rs.radiusCm = r.radiusCm;
        rs.massPerMeterKg = rp.massPerMeterKg;
        rs.sides = rp.sides;
        rs.uvCmPerTile = rp.uvCmPerTile;
        rs.stretchCompliance = rp.stretchCompliance;
        rs.bendTwistCompliance = rp.bendTwistCompliance;
        rs.maxNodes = opt_.maxRopeNodes;
        if (!planRope(anchorTarget(world, *cr, 0), anchorTarget(world, *cr, 1), rs, r.plan)) {
            r.overCap = r.plan.error.find("over the limit") != std::string::npos;
            return fail(r.plan.error.c_str());
        }
        rest = r.plan.rest;
    }
    r.restLengthCm = r.plan.restLengthCm;

    if (!createRopeBody(r, rest, seeded ? &vel : nullptr, seeded)) return fail("the physics body could not be made");

    // Draw arrays: static topology and UVs (from the rest curve, so a stretching rope does not swim its texture).
    const u32 n = r.nodes;
    const u32 verts = tubeVertexCount(n, r.sides);
    r.nodeWorld = rest;
    r.rodRot.assign(n - 1, Quat::identity());
    r.haveRods = false;
    r.tubePos.resize(verts);
    r.tubeUv.resize(static_cast<usize>(verts) * 2);
    r.sourceVertex.assign(verts, kGeneratedVertex);
    tubeUvs(rest.data(), n, r.sides, rp.uvCmPerTile, r.tubeUv.data());
    tubeIndices(n, r.sides, r.tubeIdx);
    tubeVertices(r.nodeWorld.data(), n, nullptr, r.radiusCm, r.sides, r.tubePos.data());
    r.anchor[0] = Anchor{};
    r.anchor[1] = Anchor{};
    r.needsRead = true;
    r.needsPack = true;
    r.epoch = 1;
    updateAnchors(world, r);
    return Built::Ready;
}

void SoftShapeSystem::destroyRope(RopeEntry& r) {
    if (r.body) {
        aver_phys_remove_body(r.body);
        r.body = 0;
    }
}

void SoftShapeSystem::updateAnchors(scene::World& world, RopeEntry& r) {
    auto* cr = world.component<CSoftRope>(r.entity, ropeType());
    if (!cr || !r.body || r.nodes < 2) return;
    for (int end = 0; end < 2; ++end) {
        u32& ent = end == 0 ? cr->startEntity : cr->endEntity;
        const u32 noPin = end == 0 ? kRopeNoStartPin : kRopeNoEndPin;
        const i32 vertex = end == 0 ? 0 : static_cast<i32>(r.nodes) - 1;
        Anchor& an = r.anchor[end];
        // An anchor entity that is gone frees the end for good.
        if (ent != 0 && !world.valid(ent)) {
            ent = 0;
            cr->flags |= noPin;
        }
        if (cr->flags & noPin) {
            if (an.attached) {
                aver_phys_sbx_detach(r.body, vertex, -1.0f);
                an = Anchor{};
                r.needsRead = true;
            }
            continue;
        }
        const Vec3 target = anchorTarget(world, *cr, end);
        const f32 t[3] = {target.x, target.y, target.z};
        if (!an.attached) {
            if (aver_phys_sbx_attach(r.body, vertex, t, 0.0f)) {
                an.attached = true;
                an.hasTarget = true;
                an.target = target;
                r.needsRead = true;
            }
        } else if (!an.hasTarget || distSquared(target, an.target) > 1e-4f) {
            // Only a target that moved: move_attachment wakes the body, and a still anchor must let the rope sleep.
            if (aver_phys_sbx_move_attachment(r.body, vertex, t)) {
                an.target = target;
                an.hasTarget = true;
                r.needsRead = true;
            }
        }
    }
}

void SoftShapeSystem::updateRope(scene::World& world, RopeEntry& r, f32) {
    if (r.failed || !r.body) {
        writeRopeOut(world, r);
        return;
    }
    updateAnchors(world, r);

    const f32 d = dist(r.centroid, host().viewPos());
    if (!r.frozen && d > opt_.freezeDistanceCm) {
        aver_phys_sb_set_active(r.body, 0);
        r.frozen = true;
    } else if (r.frozen && d < opt_.freezeDistanceCm * 0.9f) {
        aver_phys_sb_set_active(r.body, 1);
        r.frozen = false;
        r.needsRead = true;
    }
    const bool active = !r.frozen && aver_phys_sb_is_active(r.body) != 0;
    if (!r.frozen && (active || r.needsRead)) {
        const i32 n = static_cast<i32>(r.nodes);
        r.posBuf.resize(static_cast<usize>(n) * 3);
        if (aver_phys_sb_state(r.body, r.posBuf.data(), nullptr, n) == n) {
            Vec3 sum{0, 0, 0};
            for (i32 i = 0; i < n; ++i) {
                r.nodeWorld[static_cast<usize>(i)] = Vec3{r.posBuf[static_cast<usize>(i) * 3], r.posBuf[static_cast<usize>(i) * 3 + 1],
                                                          r.posBuf[static_cast<usize>(i) * 3 + 2]};
                sum += r.nodeWorld[static_cast<usize>(i)];
            }
            r.centroid = sum / static_cast<f32>(std::max(n, 1));

            // The rods' orientations carry the twist: one bulk read when the slots are the identity, else one call per rod.
            const i32 rods = n - 1;
            r.rodBuf.assign(static_cast<usize>(rods) * 4, 0.0f);
            bool ok = true;
            if (r.slotsIdentity) {
                ok = aver_phys_sbx_rod_state(r.body, 0, rods, r.rodBuf.data(), nullptr) == rods;
            } else {
                for (i32 i = 0; i < rods && ok; ++i)
                    ok = aver_phys_sbx_rod_state(r.body, r.rodSlots[static_cast<usize>(i)], 1, &r.rodBuf[static_cast<usize>(i) * 4], nullptr) == 1;
            }
            if (ok) {
                for (i32 i = 0; i < rods; ++i)
                    r.rodRot[static_cast<usize>(i)] = Quat(r.rodBuf[static_cast<usize>(i) * 4], r.rodBuf[static_cast<usize>(i) * 4 + 1],
                                                           r.rodBuf[static_cast<usize>(i) * 4 + 2], r.rodBuf[static_cast<usize>(i) * 4 + 3]).normalized();
                r.haveRods = true;
            }
            tubeVertices(r.nodeWorld.data(), r.nodes, r.haveRods ? r.rodRot.data() : nullptr, r.radiusCm, r.sides, r.tubePos.data());
            r.needsRead = false;
            r.needsPack = true;
        }
    }
    writeRopeOut(world, r);
}

void SoftShapeSystem::writeRopeOut(scene::World& world, RopeEntry& r) {
    auto* cr = world.component<CSoftRope>(r.entity, ropeType());
    if (!cr) return;
    const bool awake = r.body && !r.frozen && aver_phys_sb_is_active(r.body) != 0;
    cr->outState = (awake ? kShapeAwake : 0u) | (r.frozen ? kShapeFrozen : 0u) | (r.overCap ? kShapeOverCap : 0u) | (r.failed ? kShapeFailed : 0u) |
                   (r.cutMade || (cr->flags & kRopeTail) ? kShapeCut : 0u) | (r.assetStale ? kShapeAssetStale : 0u) |
                   ((r.nodes > 0 ? r.nodes - 1 : 0u) << kShapeCountShift);
    if (r.failed || !r.body) return;
    const f32 len = polylineLength(r.nodeWorld.data(), r.nodes);
    cr->outLengthCm = len;
    cr->outStrain = r.restLengthCm > 1e-4f ? (len - r.restLengthCm) / r.restLengthCm : 0.0f;
}

// ---- cut -------------------------------------------------------------------------------------------------

bool SoftShapeSystem::cutRope(scene::World& world, scene::Entity rope, const Vec3& point, scene::Entity& other) {
    other = scene::kInvalidEntity;
    const auto it = ropes_.find(rope);
    if (it == ropes_.end()) return false;
    RopeEntry& r = it->second;
    if (r.failed || !r.body || r.nodes < 4 || r.nodeWorld.size() != r.nodes) return false;
    for (const CutRequest& c : cuts_)
        if (c.head == rope) return false;   // one cut per tick per rope
    auto* cr = world.component<CSoftRope>(rope, ropeType());
    if (!cr) return false;

    u32 k = nearestNode(r.nodeWorld.data(), r.nodes, point);
    k = std::clamp<u32>(k, 1u, r.nodes - 2u);
    const f32 reach = r.plan.segmentCm * 1.5f + r.radiusCm * 2.0f;
    if (dist(r.nodeWorld[k], point) > reach) return false;   // the point is not on the rope

    const scene::Entity tail = world.create("RopeTail");
    if (tail == scene::kInvalidEntity) return false;
    const Vec3 cutAt = r.nodeWorld[k];
    CSoftRope* tc = attachRope(world, tail, cutAt, Vec3{cr->endOffset[0], cr->endOffset[1], cr->endOffset[2]}, cr->asset, 0.0f, cr->radiusCm);
    if (!tc) {
        world.destroy(tail);
        return false;
    }
    tc->startEntity = 0;
    tc->endEntity = cr->endEntity;
    tc->flags = (cr->flags & (kRopeNoWorld | kRopeNoEndPin)) | kRopeNoStartPin | kRopeTail;
    // The head keeps its start and loses its end; both cut ends are free.
    cr->endEntity = 0;
    cr->endOffset[0] = cutAt.x; cr->endOffset[1] = cutAt.y; cr->endOffset[2] = cutAt.z;
    cr->flags |= kRopeNoEndPin | kRopeTail;
    r.cutMade = true;
    cuts_.push_back(CutRequest{rope, tail, k});
    other = tail;
    return true;
}

void SoftShapeSystem::processCuts(scene::World& world) {
    if (cuts_.empty()) return;
    std::vector<CutRequest> todo;
    todo.swap(cuts_);
    for (const CutRequest& c : todo) {
        const auto it = ropes_.find(c.head);
        if (it == ropes_.end() || it->second.failed || !it->second.body || c.node + 1 >= it->second.nodes) {
            if (world.valid(c.tail)) world.destroy(c.tail);   // the rope vanished first: no tail
            continue;
        }
        RopeEntry& r = it->second;
        const u32 n = r.nodes;
        std::vector<f32> pos(static_cast<usize>(n) * 3), vel(static_cast<usize>(n) * 3);
        if (aver_phys_sb_state(r.body, pos.data(), vel.data(), static_cast<i32>(n)) != static_cast<i32>(n)) {
            if (world.valid(c.tail)) world.destroy(c.tail);
            continue;
        }
        const auto at = [&](const std::vector<f32>& a, u32 i) { return Vec3{a[i * 3], a[i * 3 + 1], a[i * 3 + 2]}; };
        Seed head, tail;
        for (u32 i = 0; i <= c.node; ++i) { head.pos.push_back(at(pos, i)); head.vel.push_back(at(vel, i)); }
        for (u32 i = c.node; i < n; ++i) { tail.pos.push_back(at(pos, i)); tail.vel.push_back(at(vel, i)); }
        // Both pieces are rebuilt from their nodes by the sync that follows (the head's entry is dropped, the tail's does not exist yet).
        destroyRope(r);
        ropes_.erase(it);
        seeds_[c.head] = std::move(head);
        seeds_[c.tail] = std::move(tail);
    }
}

} // namespace aver::softbody
