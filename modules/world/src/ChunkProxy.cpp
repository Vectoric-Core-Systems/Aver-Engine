#include "aver/world/ChunkProxy.hpp"

#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"
#include "aver/world/LevelFlatten.hpp"

#include <chrono>

namespace aver::world {

bool buildChunkProxy(const ChunkPayload& payload, const std::function<const fmt::OcMeshData*(u64)>& meshOf, const ChunkProxyOptions& opt,
                     ChunkProxyResult& out, std::string* why) {
    out = ChunkProxyResult{};
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count(); };

    const f64 originX = static_cast<f64>(chunkOriginCmAxis(payload.coord.x, payload.chunkSizeCm));
    const f64 originY = static_cast<f64>(chunkOriginCmAxis(payload.coord.y, payload.chunkSizeCm));
    const f64 originZ = static_cast<f64>(chunkOriginCmAxis(payload.coord.z, payload.chunkSizeCm));

    // Entity poses in absolute centimetres, parents first (the payload is topologically ordered).
    std::vector<Pose64> poses(payload.entities.size());
    std::vector<fmt::OcMergeItem> items;
    for (usize i = 0; i < payload.entities.size(); ++i) {
        const PayloadEntity& e = payload.entities[i];
        Pose64 local;
        local.pos[0] = e.local.position.x; local.pos[1] = e.local.position.y; local.pos[2] = e.local.position.z;
        local.rot = e.local.rotation;
        local.scale = e.local.scale;
        if (e.parent >= 0 && static_cast<usize>(e.parent) < i) {
            poses[i] = composePose(poses[static_cast<usize>(e.parent)], local);
        } else {
            local.pos[0] += originX; local.pos[1] += originY; local.pos[2] += originZ;
            poses[i] = local;
        }

        if (!e.hasMesh || (e.meshFlags & 1u) == 0) { ++out.skipped; continue; }
        if (opt.layers) {
            PlacementExtras x;
            if (getPlacementExtras(e, x) && !fmt::hlodLayersAllowed(x.layers, *opt.layers)) { ++out.skipped; continue; }
        }
        if (!items.empty() && (items.size() >= opt.maxItems || elapsed() > opt.timeBudgetMs)) { out.truncated = true; break; }
        const fmt::OcMeshData* mesh = meshOf ? meshOf(e.mesh) : nullptr;
        if (!mesh) { ++out.skipped; continue; }
        fmt::OcMergeItem it;
        it.mesh = mesh;
        fmt::ocMergeMatrix(poses[i].pos, poses[i].rot, poses[i].scale, it.m);
        it.material = e.material;
        items.push_back(std::move(it));
    }
    out.items = static_cast<u32>(items.size());

    fmt::OcHlodCellBuild b;
    if (!fmt::buildHlodProxy(items, originX, originY, static_cast<f32>(payload.chunkSizeCm), opt.hlodCm, opt.hlod, b, why)) {
        out.elapsedMs = elapsed();
        return false;
    }
    out.mesh = std::move(b.mesh);
    out.trianglesIn = b.trianglesIn;
    out.elapsedMs = elapsed();
    return true;
}

} // namespace aver::world
