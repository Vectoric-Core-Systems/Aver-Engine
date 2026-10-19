// The soft-body proxy tool. See SoftProxyTool.hpp.
#include "SoftProxyTool.hpp"

#include "aver/core/Math.hpp"

#if AVER_MODULE_TRIFACTOR
#  include "aver/trifactor/ClusterBuilder.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace aver::editor {
namespace {

struct CellKey {
    i64 x, y, z;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
    usize operator()(const CellKey& k) const {
        return static_cast<usize>(static_cast<u64>(k.x) * 73856093ull ^ static_cast<u64>(k.y) * 19349663ull ^ static_cast<u64>(k.z) * 83492791ull);
    }
};

// Welds by quantised position (first seen wins) and drops the triangles that collapse. Positions only: UV seams and split
// normals are exactly what the sim weld removes, so the proxy is the welded shape.
void weldMesh(const fmt::OcMeshData& in, f32 weldCm, std::vector<f32>& pos, std::vector<u32>& idx) {
    const f32 cell = std::max(weldCm, 1e-4f);
    std::unordered_map<CellKey, u32, CellHash> ids;
    std::vector<u32> remap(in.vertexCount());
    for (u32 v = 0; v < in.vertexCount(); ++v) {
        const f32* p = in.positions.data() + static_cast<usize>(v) * 3;
        const CellKey k{static_cast<i64>(std::llround(p[0] / cell)), static_cast<i64>(std::llround(p[1] / cell)), static_cast<i64>(std::llround(p[2] / cell))};
        const auto it = ids.find(k);
        if (it != ids.end()) { remap[v] = it->second; continue; }
        const u32 id = static_cast<u32>(pos.size() / 3);
        ids.emplace(k, id);
        remap[v] = id;
        pos.insert(pos.end(), {p[0], p[1], p[2]});
    }
    for (usize t = 0; t + 2 < in.indices.size(); t += 3) {
        const u32 a = remap[in.indices[t]], b = remap[in.indices[t + 1]], c = remap[in.indices[t + 2]];
        if (a == b || b == c || a == c) continue;
        idx.push_back(a); idx.push_back(b); idx.push_back(c);
    }
}

void smoothNormalsOf(const std::vector<f32>& pos, const std::vector<u32>& idx, std::vector<f32>& normals) {
    const usize n = pos.size() / 3;
    std::vector<Vec3> acc(n, Vec3{0, 0, 0});
    for (usize t = 0; t + 2 < idx.size(); t += 3) {
        const Vec3 a{pos[idx[t] * 3], pos[idx[t] * 3 + 1], pos[idx[t] * 3 + 2]};
        const Vec3 b{pos[idx[t + 1] * 3], pos[idx[t + 1] * 3 + 1], pos[idx[t + 1] * 3 + 2]};
        const Vec3 c{pos[idx[t + 2] * 3], pos[idx[t + 2] * 3 + 1], pos[idx[t + 2] * 3 + 2]};
        const Vec3 face = cross(b - a, c - a);
        acc[idx[t]] += face; acc[idx[t + 1]] += face; acc[idx[t + 2]] += face;
    }
    normals.resize(n * 3);
    for (usize i = 0; i < n; ++i) {
        const Vec3 nrm = acc[i].getSafeNormal();
        normals[i * 3] = nrm.x; normals[i * 3 + 1] = nrm.y; normals[i * 3 + 2] = nrm.z;
    }
}

}  // namespace

bool proxyToolAvailable() {
#if AVER_MODULE_TRIFACTOR
    return true;
#else
    return false;
#endif
}

bool cookProxyMesh(const fmt::OcMeshData& in, const ProxyCookRequest& req, fmt::OcMeshData& out, ProxyCookResult& info) {
    info = ProxyCookResult{};
    info.renderTriangles = static_cast<u32>(in.indices.size() / 3);
    const auto fail = [&](const std::string& why) {
        info.ok = false;
        info.why = why;
        return false;
    };
    if (in.vertexCount() == 0 || in.indices.size() < 3) return fail("the mesh has no triangles");

    std::vector<f32> pos;
    std::vector<u32> idx;
    weldMesh(in, req.weldCm, pos, idx);
    info.weldedVertices = static_cast<u32>(pos.size() / 3);
    info.weldedTriangles = static_cast<u32>(idx.size() / 3);
    if (info.weldedTriangles == 0) return fail("every triangle collapses under the weld");
    if (info.weldedVertices <= req.targetParticles) return fail("the mesh welds to " + std::to_string(info.weldedVertices) + " particles, already under the target of " + std::to_string(req.targetParticles) + "; it needs no proxy");

    // A closed triangle mesh has about two triangles per vertex, so twice the particles asked for is the triangle target.
    const f32 ratio = std::clamp(2.0f * static_cast<f32>(req.targetParticles) / static_cast<f32>(info.weldedTriangles), 0.001f, 0.99f);
    info.ratio = ratio;

    fmt::OcMeshData m;
    m.positions = std::move(pos);
    m.indices = std::move(idx);
    smoothNormalsOf(m.positions, m.indices, m.normals);
    m.uvs.assign(static_cast<usize>(m.vertexCount()) * 2, 0.0f);
    m.computeBounds();

#if AVER_MODULE_TRIFACTOR
    std::string why;
    if (!trifactor::simplifyMesh(m, ratio, &why)) return fail("the simplifier refused: " + why);
    info.proxyVertices = m.vertexCount();
    info.proxyTriangles = static_cast<u32>(m.indices.size() / 3);
    out = std::move(m);
    info.ok = true;
    char b[200];
    std::snprintf(b, sizeof b, "%u -> %u triangles, %u -> %u particles", info.weldedTriangles, info.proxyTriangles, info.weldedVertices, info.proxyVertices);
    info.why = b;
    return true;
#else
    (void)out;
    return fail("this build has no Trifactor module (AVER_MODULE_TRIFACTOR is off), which the proxy tool simplifies with");
#endif
}

ProxyCookResult cookProxyFile(const std::string& meshPath, const std::string& outPath, const ProxyCookRequest& req) {
    ProxyCookResult info;
    fmt::OcMeshData in, proxy;
    std::string why;
    if (!fmt::loadOcMesh(meshPath, in, &why)) {
        info.why = "could not read " + meshPath + ": " + why;
        return info;
    }
    if (!cookProxyMesh(in, req, proxy, info)) return info;
    if (!fmt::saveOcMesh(outPath, proxy, &why)) {
        info.ok = false;
        info.why = "could not write " + outPath + ": " + why;
        return info;
    }
    info.outPath = outPath;
    return info;
}

bool SoftProxyTool::cook(const std::string& contentDir, const std::string& meshRel, const std::string& proxyRel) {
    last = cookProxyFile(contentDir + "/" + meshRel, contentDir + "/" + proxyRel, request);
    return last.ok;
}

SoftProxyTool& softProxyTool() {
    static SoftProxyTool t;
    return t;
}

}  // namespace aver::editor
