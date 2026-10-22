// Jelly: the lattice body behind a CSoftVolume, its readback, and the embedded render mesh.
#include "aver/softbody/SoftShapeSystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/physics/physics_softbody_ext_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

f32 or1(f32 v) { return v > 0.0f ? v : 1.0f; }

Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0], p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

f32 uniformScaleOf(const Mat4& m) { return Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.size(); }

std::vector<i32> toI32(const std::vector<u32>& src) {
    std::vector<i32> out(src.size());
    for (usize i = 0; i < src.size(); ++i) out[i] = static_cast<i32>(src[i]);
    return out;
}

}  // namespace

SoftShapeSystem::Built SoftShapeSystem::buildVolume(scene::World& world, VolumeEntry& v) {
    const scene::Entity e = v.entity;
    const auto* cv = world.component<CSoftVolume>(e, volumeType());
    const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!cv || !mr || mr->mesh == 0) return Built::NotReady;
    const fmt::OcMeshData* mesh = host().meshData(mr->mesh);
    if (!mesh) return Built::NotReady;

    v.spec = VolumeSpec{};
    v.spec.meshId = mr->mesh;
    v.spec.assetId = cv->asset;
    v.spec.preset = cv->preset;
    v.spec.flags = cv->flags;
    v.spec.stiffness = or1(cv->stiffnessScale);
    v.spec.cell = cv->cellCm;
    v.spec.gravity = or1(cv->gravityScale);
    v.spec.damping = or1(cv->dampingScale);
    v.mesh = mesh;

    const auto fail = [&](const char* msg) {
        AVER_WARN("[SoftShape] entity {}: {}; it keeps its authored mesh", e, msg);
        v.failed = true;
        return Built::Failed;
    };
    if (mesh->positions.empty() || mesh->indices.empty()) return fail("the mesh has no triangles to fill");

    // The asset: .ocsoft (kind "jelly") + the component's scales, or the jelly preset alone. `preset` is reserved.
    v.asset = makeSoftAsset(SoftKind::Jelly, SoftPreset::Jelly);
    v.assetStale = false;
    if (cv->asset) {
        const SoftAsset* src = host().softAsset(cv->asset);
        if (src && src->kind == SoftKind::Jelly) v.asset = *src;
        else v.assetStale = true;
    }
    const SoftAsset& a = v.asset;

    // The lattice is built in mesh-local space (the cell scaled by the entity's scale), simulated in world space.
    const Mat4 W0 = world.worldMatrix(e);
    const f32 scale = std::max(uniformScaleOf(W0), 1e-3f);
    const f32 cellWorld = cv->cellCm > 0.0f ? cv->cellCm : a.jelly.cellCm;
    LatticeSpec ls;
    ls.cellCm = cellWorld / scale;
    ls.snapCm = a.jelly.snapCm / scale;
    ls.maxNodes = std::min(a.jelly.maxNodes, std::max(opt_.maxVolumeNodes, 4u));
    const u32 vertexCount = mesh->vertexCount();
    if (!buildLattice(mesh->positions.data(), vertexCount, mesh->indices.data(), static_cast<u32>(mesh->indices.size()), ls, v.lattice)) {
        v.overCap = v.lattice.overCap;
        return fail(v.lattice.error.c_str());
    }
    embedMesh(v.lattice, mesh->positions.data(), vertexCount, v.emb);

    const u32 n = static_cast<u32>(v.lattice.nodes.size());
    v.nodeWorld.resize(n);
    std::vector<f32> rest(static_cast<usize>(n) * 3);
    Vec3 centre{0, 0, 0};
    for (u32 i = 0; i < n; ++i) {
        v.nodeWorld[i] = xformPoint(W0, v.lattice.nodes[i]);
        rest[i * 3] = v.nodeWorld[i].x; rest[i * 3 + 1] = v.nodeWorld[i].y; rest[i * 3 + 2] = v.nodeWorld[i].z;
        centre += v.nodeWorld[i];
    }
    centre = centre / static_cast<f32>(n);

    // A node's mass is a quarter of each tetrahedron it belongs to, at the asset's density (cm^3 -> m^3).
    std::vector<f32> mass(n, 0.0f);
    const u32 tetCount = static_cast<u32>(v.lattice.tets.size() / 4);
    for (u32 t = 0; t < tetCount; ++t) {
        const u32* q = &v.lattice.tets[static_cast<usize>(t) * 4];
        const f32 vol = std::fabs(tetSignedVolume(v.nodeWorld[q[0]], v.nodeWorld[q[1]], v.nodeWorld[q[2]], v.nodeWorld[q[3]]));
        for (int c = 0; c < 4; ++c) mass[q[c]] += vol * 1e-6f * a.jelly.densityKgM3 * 0.25f;
    }
    std::vector<f32> inv(n);
    for (u32 i = 0; i < n; ++i) inv[i] = 1.0f / std::max(mass[i], 1e-4f);

    const std::vector<i32> faces = toI32(v.lattice.surfaceFaces);
    const std::vector<i32> edges = toI32(v.lattice.edges);
    const std::vector<i32> tets = toI32(v.lattice.tets);
    const u32 edgeCount = static_cast<u32>(edges.size() / 2);
    v.edgeRest.resize(edgeCount);
    std::vector<f32> edgeComp(edgeCount, a.jelly.edgeCompliance / v.spec.stiffness);
    for (u32 k = 0; k < edgeCount; ++k)
        v.edgeRest[k] = dist(v.nodeWorld[static_cast<u32>(edges[k * 2])], v.nodeWorld[static_cast<u32>(edges[k * 2 + 1])]);
    std::vector<f32> volComp(tetCount, a.jelly.volumeCompliance / v.spec.stiffness);

    f32 prm[AVER_PHYS_SB_P_COUNT];
    for (f32& f : prm) f = std::nanf("");
    const bool noWorld = (cv->flags & kVolNoWorld) != 0;
    prm[AVER_PHYS_SB_P_ITERATIONS] = static_cast<f32>(a.sim.iterations);
    prm[AVER_PHYS_SB_P_DAMPING] = a.sim.damping * v.spec.damping;
    prm[AVER_PHYS_SB_P_FRICTION] = a.material.friction;
    prm[AVER_PHYS_SB_P_RESTITUTION] = a.material.restitution;
    prm[AVER_PHYS_SB_P_GRAVITY_SCALE] = (cv->flags & kVolNoGravity) ? 0.0f : a.sim.gravityScale * v.spec.gravity;
    prm[AVER_PHYS_SB_P_VERTEX_RADIUS_CM] = jellyVertexRadiusCm(a.jelly, cellWorld);
    prm[AVER_PHYS_SB_P_MAX_SPEED_CMS] = a.sim.maxSpeedCmS;
    prm[AVER_PHYS_SB_P_ALLOW_SLEEP] = a.sim.allowSleep ? 1.0f : 0.0f;
    prm[AVER_PHYS_SB_P_DOUBLE_SIDED] = 0.0f;
    prm[AVER_PHYS_SB_P_LAYER] = static_cast<f32>(noWorld ? opt_.noWorldLayer : opt_.shapeLayer);
    prm[AVER_PHYS_SB_P_IMPACTS] = 0.0f;

    const i32 b = aver_phys_sb_builder_begin(static_cast<i32>(n));
    if (!b) return fail("the physics world refused a soft body builder");
    const bool staged = aver_phys_sb_builder_vertices(b, rest.data(), inv.data(), nullptr, nullptr) &&
                        aver_phys_sb_builder_faces(b, faces.data(), static_cast<i32>(faces.size() / 3)) &&
                        aver_phys_sb_builder_edges(b, edges.data(), v.edgeRest.data(), edgeComp.data(), static_cast<i32>(edgeCount)) &&
                        aver_phys_sbx_builder_volumes(b, tets.data(), volComp.data(), static_cast<i32>(tetCount)) &&
                        aver_phys_sb_builder_params(b, prm, AVER_PHYS_SB_P_COUNT);
    const i32 h = staged ? aver_phys_sb_builder_finish(b, centre.x, centre.y, centre.z, 0, 0, nullptr, nullptr, nullptr) : 0;
    if (!h) {
        aver_phys_sb_builder_cancel(b);
        return fail("the physics body could not be made");
    }
    aver_phys_set_entity(h, static_cast<i32>(e));

    v.body = h;
    v.centroid = centre;
    v.indices = mesh->indices;
    v.sourceVertex.resize(vertexCount);
    for (u32 i = 0; i < vertexCount; ++i) v.sourceVertex[i] = i;
    v.renderWorld.resize(vertexCount);
    embeddedPositions(v.lattice, v.emb, v.nodeWorld.data(), vertexCount, v.renderWorld.data());
    v.needsRead = true;
    v.needsPack = true;
    v.epoch = 1;
    return Built::Ready;
}

void SoftShapeSystem::destroyVolume(VolumeEntry& v) {
    if (v.body) {
        aver_phys_remove_body(v.body);
        v.body = 0;
    }
}

void SoftShapeSystem::updateVolume(scene::World& world, VolumeEntry& v) {
    if (v.failed || !v.body) {
        writeVolumeOut(world, v);
        return;
    }
    // Distance freeze with a 10% hysteresis band.
    const f32 d = dist(v.centroid, host().viewPos());
    if (!v.frozen && d > opt_.freezeDistanceCm) {
        aver_phys_sb_set_active(v.body, 0);
        v.frozen = true;
    } else if (v.frozen && d < opt_.freezeDistanceCm * 0.9f) {
        aver_phys_sb_set_active(v.body, 1);
        v.frozen = false;
        v.needsRead = true;
    }
    v.awake = !v.frozen && aver_phys_sb_is_active(v.body) != 0;
    if (!v.frozen && (v.awake || v.needsRead)) {
        const i32 n = static_cast<i32>(v.nodeWorld.size());
        v.posBuf.resize(static_cast<usize>(n) * 3);
        if (aver_phys_sb_state(v.body, v.posBuf.data(), nullptr, n) == n) {
            Vec3 sum{0, 0, 0};
            for (i32 i = 0; i < n; ++i) {
                v.nodeWorld[static_cast<usize>(i)] = Vec3{v.posBuf[static_cast<usize>(i) * 3], v.posBuf[static_cast<usize>(i) * 3 + 1],
                                                          v.posBuf[static_cast<usize>(i) * 3 + 2]};
                sum += v.nodeWorld[static_cast<usize>(i)];
            }
            v.centroid = sum / static_cast<f32>(std::max(n, 1));
            embeddedPositions(v.lattice, v.emb, v.nodeWorld.data(), static_cast<u32>(v.renderWorld.size()), v.renderWorld.data());
            v.needsRead = false;
            v.needsPack = true;
        }
    }
    writeVolumeOut(world, v);
}

void SoftShapeSystem::writeVolumeOut(scene::World& world, VolumeEntry& v) {
    auto* cv = world.component<CSoftVolume>(v.entity, volumeType());
    if (!cv) return;
    u32 state = (v.awake ? kShapeAwake : 0u) | (v.frozen ? kShapeFrozen : 0u) | (v.overCap ? kShapeOverCap : 0u) | (v.failed ? kShapeFailed : 0u) |
                (v.assetStale ? kShapeAssetStale : 0u);
    cv->outState = state;
    cv->outNodes = static_cast<u32>(v.nodeWorld.size());
    if (v.failed || !v.body) return;
    if (v.awake || v.needsPack) {
        f32 rest = 0.0f, now = 0.0f;
        cv->outVolumeRatio = aver_phys_sbx_volume(v.body, &rest, &now) && rest > 0.0f ? now / rest : 0.0f;
        f32 worst = 0.0f;
        for (usize k = 0; k < v.edgeRest.size(); ++k) {
            const u32 a = v.lattice.edges[k * 2], b = v.lattice.edges[k * 2 + 1];
            if (v.edgeRest[k] > 1e-6f) worst = std::max(worst, std::fabs(dist(v.nodeWorld[a], v.nodeWorld[b]) / v.edgeRest[k] - 1.0f));
        }
        cv->outMaxStretch = worst;
    }
}

} // namespace aver::softbody
