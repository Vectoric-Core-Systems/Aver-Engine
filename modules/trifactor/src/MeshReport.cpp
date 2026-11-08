#include "aver/trifactor/MeshReport.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace aver::trifactor {

BuilderStaleness classifyBuilder(u32 stored, u32 current) {
    if (stored == current) return BuilderStaleness::Current;
    if (stored == 0) return BuilderStaleness::Unknown;
    return stored < current ? BuilderStaleness::Stale : BuilderStaleness::Newer;
}

const char* stalenessLabel(BuilderStaleness s) {
    switch (s) {
        case BuilderStaleness::Current: return "current";
        case BuilderStaleness::Stale:   return "stale";
        case BuilderStaleness::Unknown: return "unknown";
        case BuilderStaleness::Newer:   return "newer";
    }
    return "unknown";
}

std::string stalenessWarning(u32 stored, u32 current) {
    switch (classifyBuilder(stored, current)) {
        case BuilderStaleness::Current: return {};
        case BuilderStaleness::Stale:
            return "Cooked by Trifactor builder v" + std::to_string(stored) + "; this build writes v" +
                   std::to_string(current) + ". Re-cook to refresh the LOD ladder. It still loads.";
        case BuilderStaleness::Unknown:
            return "Cooked before builder versions were recorded. Re-cook to refresh the LOD ladder. It still loads.";
        case BuilderStaleness::Newer:
            return "Cooked by a newer Trifactor builder (v" + std::to_string(stored) + ") than this build (v" +
                   std::to_string(current) + "). Its clusters may be ignored.";
    }
    return {};
}

Eligibility virtualizable(const fmt::OcMeshData& m) {
    Eligibility e;
    if (m.positions.empty() || m.indices.empty()) e.why = "no geometry";
    else if (m.hasSkin()) e.why = "skinned meshes stay on discrete LOD";
    else if (m.hasMorph()) e.why = "morph targets stay on discrete LOD";
    else if (!m.hasMeshlets()) e.why = "no meshlets (not cooked with Trifactor)";
    else if (m.coarserLods.empty()) e.why = "no coarser LOD levels";
    else if (!m.meshletsMaterialPure) e.why = "meshlets may straddle submeshes (cooked before builder v3)";
    else e.ok = true;
    return e;
}

namespace {

struct WeldKey {
    u32 x, y, z;
    bool operator==(const WeldKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct WeldHash {
    usize operator()(const WeldKey& k) const {
        u64 h = u64(k.x) * 0x9E3779B1ull;
        h = (h ^ (k.y + 0x7F4A7C15ull + (h << 6) + (h >> 2))) * 0x85EBCA6Bull;
        h = (h ^ (k.z + 0x7F4A7C15ull + (h << 6) + (h >> 2))) * 0xC2B2AE35ull;
        return static_cast<usize>(h ^ (h >> 29));
    }
};

} // namespace

LodReport measureLodDag(const LodDag& dag, const fmt::OcMeshData& mesh) {
    LodReport rep;
    const u32 vc = mesh.vertexCount();

    // Weld by exact position bits (-0 folded to +0) so attribute seams do not read as borders.
    std::vector<u32> weld(vc);
    {
        std::unordered_map<WeldKey, u32, WeldHash> seen;
        seen.reserve(vc);
        for (u32 v = 0; v < vc; ++v) {
            u32 b[3];
            for (int a = 0; a < 3; ++a) {
                f32 f = mesh.positions[usize(v) * 3 + a];
                if (f == 0.0f) f = 0.0f;
                std::memcpy(&b[a], &f, 4);
            }
            weld[v] = seen.try_emplace(WeldKey{b[0], b[1], b[2]}, v).first->second;
        }
    }

    std::vector<u32> groupOf(dag.clusters.size(), fmt::kInvalidClusterId);
    for (const ClusterGroupNode& g : dag.groupNodes)
        for (u32 i = 0; i < g.childClusterCount; ++i) groupOf[dag.groupChildren[g.childClusterStart + i]] = g.id;

    rep.levels.resize(dag.levels.size());
    for (u32 L = 0; L < dag.levelCount(); ++L) {
        LevelReport& lr = rep.levels[L];
        lr.clusters = static_cast<u32>(dag.levels[L].size());

        struct E { u32 group; u32 tris; bool cross; };
        std::unordered_map<u64, E> edges;
        std::vector<u32> groupsSeen;
        for (const u32 cid : dag.levels[L]) {
            const Cluster& c = dag.clusters[cid];
            const u32 g = groupOf[cid];
            if (g != fmt::kInvalidClusterId) groupsSeen.push_back(g);
            for (usize t = 0; t + 2 < c.triangles.size(); t += 3) {
                ++lr.triangles;
                u32 w[3];
                for (int k = 0; k < 3; ++k) w[k] = weld[c.vertices[c.triangles[t + k]]];
                for (int k = 0; k < 3; ++k) {
                    u32 a = w[k], b = w[(k + 1) % 3];
                    if (a == b) continue;
                    if (a > b) std::swap(a, b);
                    const u64 key = (u64(a) << 32) | b;
                    auto [it, fresh] = edges.try_emplace(key, E{g, 1, false});
                    if (!fresh) { ++it->second.tris; if (it->second.group != g) it->second.cross = true; }
                }
            }
        }
        std::sort(groupsSeen.begin(), groupsSeen.end());
        lr.groups = static_cast<u32>(std::unique(groupsSeen.begin(), groupsSeen.end()) - groupsSeen.begin());
        lr.edges = edges.size();
        for (const auto& kv : edges) {
            if (kv.second.tris == 1) ++lr.openEdges;
            else if (kv.second.cross) ++lr.lockedEdges;
        }
        if (L + 1 < dag.levelCount()) rep.lockedEdgesTotal += lr.lockedEdges;
    }
    if (!rep.levels.empty()) rep.coarsestTriangles = rep.levels.back().triangles;
    return rep;
}

} // namespace aver::trifactor
