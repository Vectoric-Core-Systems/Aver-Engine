#include "aver/world/PcgSinks.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cstdio>
#include <tuple>

namespace aver::world {

bool pcgToInstanceData(const PcgResult& r, fmt::OcInstanceData& out, f32 cellCm, const std::vector<f32>& groupRadiusCm,
                       std::string* why) {
    out = fmt::OcInstanceData{};
    for (const PcgInstanceGroup& g : r.groups) {
        if (g.items.empty()) continue;
        fmt::OcInstanceGroup grp;
        grp.asset = g.mesh;
        grp.flags = fmt::kOcInstanceFlagCastShadow;
        grp.first = static_cast<u32>(out.transforms.size() / 12);
        grp.count = static_cast<u32>(g.items.size());
        for (const PcgInstance& i : g.items) {
            const Mat4 m = i.xf.toMatrix();
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 3; ++col) out.transforms.push_back(m.m[row][col]);
        }
        out.groups.push_back(std::move(grp));
    }
    if (!out.valid()) { if (why) *why = "instance table failed validation"; return false; }
    if (cellCm > 0.0f && !out.groups.empty()) {
        std::vector<f32> radii = groupRadiusCm;
        radii.resize(out.groups.size(), 0.0f);
        return fmt::cellOcInstances(out, cellCm, radii, why);
    }
    return true;
}

ChunkPayload pcgToChunkPayload(const PcgResult& r, const ChunkCoord& c, i32 chunkSizeCm) {
    ChunkPayload p;
    p.coord = c;
    p.chunkSizeCm = chunkSizeCm;
    const i64 ox = chunkOriginCmAxis(c.x, chunkSizeCm);
    const i64 oy = chunkOriginCmAxis(c.y, chunkSizeCm);
    const i64 oz = chunkOriginCmAxis(c.z, chunkSizeCm);

    struct Item { i32 seed; u32 group; const PcgInstance* inst; const PcgInstanceGroup* g; };
    std::vector<Item> items;
    for (usize gi = 0; gi < r.groups.size(); ++gi)
        for (const PcgInstance& i : r.groups[gi].items) {
            const f64 lx = static_cast<f64>(i.xf.position.x) - static_cast<f64>(ox);
            const f64 ly = static_cast<f64>(i.xf.position.y) - static_cast<f64>(oy);
            if (lx < 0.0 || ly < 0.0 || lx >= chunkSizeCm || ly >= chunkSizeCm) continue;
            items.push_back({i.seed, pcgHashString(r.groups[gi].mesh + "|" + r.groups[gi].material), &i, &r.groups[gi]});
        }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        const Vec3& pa = a.inst->xf.position;
        const Vec3& pb = b.inst->xf.position;
        return std::tie(a.seed, a.group, pa.x, pa.y, pa.z) < std::tie(b.seed, b.group, pb.x, pb.y, pb.z);
    });

    u32 dup = 0;
    for (usize k = 0; k < items.size(); ++k) {
        const Item& it = items[k];
        dup = (k > 0 && items[k - 1].seed == it.seed && items[k - 1].group == it.group) ? dup + 1 : 0;
        char hex[16];
        std::snprintf(hex, sizeof hex, "%08x", static_cast<u32>(it.seed));
        PayloadEntity e;
        e.name = "pcg_" + std::to_string(c.x) + "_" + std::to_string(c.y) + "_" + hex + (dup ? "_" + std::to_string(dup) : "");
        e.objectId = (static_cast<u64>(static_cast<u32>(it.seed)) << 32) | it.group;
        e.parent = -1;
        e.local.position = Vec3{static_cast<f32>(static_cast<f64>(it.inst->xf.position.x) - static_cast<f64>(ox)),
                                static_cast<f32>(static_cast<f64>(it.inst->xf.position.y) - static_cast<f64>(oy)),
                                static_cast<f32>(static_cast<f64>(it.inst->xf.position.z) - static_cast<f64>(oz))};
        e.local.rotation = it.inst->xf.rotation;
        e.local.scale = it.inst->xf.scale;
        e.hasMesh = true;
        e.mesh = fnv1a64(it.g->mesh);
        e.material = it.g->material;
        e.meshFlags = 1;   // kMeshRendererVisible, without dragging the scene header in here
        p.entities.push_back(std::move(e));
    }
    return p;
}

std::vector<PcgActorSpawn> pcgActorsIn(const PcgResult& r, const PcgBox& box) {
    std::vector<PcgActorSpawn> out;
    for (const PcgActorSpawn& a : r.actors) if (box.containsXY(a.xf.position)) out.push_back(a);
    std::stable_sort(out.begin(), out.end(), [](const PcgActorSpawn& a, const PcgActorSpawn& b) {
        return std::tie(a.seed, a.actorClass, a.xf.position.x, a.xf.position.y, a.xf.position.z) <
               std::tie(b.seed, b.actorClass, b.xf.position.x, b.xf.position.y, b.xf.position.z);
    });
    return out;
}

} // namespace aver::world
