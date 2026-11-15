#include "aver/rhi/Meshlets.hpp"

#include <cmath>
#include <cstring>

namespace aver::rhi {

namespace {
u32 floatBits(f32 f) {
    u32 u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
} // namespace

MeshletBuffer buildMeshlets(const u32* indices, u32 indexCount, u32 vertexCount, const f32* positions, u32 strideBytes) {
    MeshletBuffer out;
    if (!indices || indexCount < 3 || vertexCount == 0) return out;

    struct Meshlet { u32 firstVertex, firstTriangle, vertexCount, triangleCount; };
    std::vector<Meshlet> meshlets;
    std::vector<u32> verts, tris;
    verts.reserve(indexCount / 2);
    tris.reserve(indexCount / 3);
    // Which meshlet last took each vertex, and its local index there.
    std::vector<u32> owner(vertexCount, ~0u);
    std::vector<u8> local(vertexCount, 0);

    Meshlet cur{0, 0, 0, 0};
    const auto flush = [&] {
        if (cur.triangleCount) meshlets.push_back(cur);
        cur = {static_cast<u32>(verts.size()), static_cast<u32>(tris.size()), 0, 0};
    };
    for (u32 t = 0; t + 3 <= indexCount; t += 3) {
        const u32 a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        const u32 id = static_cast<u32>(meshlets.size());
        const u32 fresh = (owner[a] != id) + (owner[b] != id && b != a) + (owner[c] != id && c != a && c != b);
        if (cur.vertexCount + fresh > kMeshletMaxVertices || cur.triangleCount == kMeshletMaxTriangles) flush();
        const u32 now = static_cast<u32>(meshlets.size());
        u32 l[3];
        const u32 g[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) {
            if (owner[g[k]] != now) {
                owner[g[k]] = now;
                local[g[k]] = static_cast<u8>(cur.vertexCount++);
                verts.push_back(g[k]);
            }
            l[k] = local[g[k]];
        }
        tris.push_back(l[0] | (l[1] << 8) | (l[2] << 16));
        ++cur.triangleCount;
    }
    flush();
    if (meshlets.empty()) return out;

    const u32 n = static_cast<u32>(meshlets.size());
    const u32 vertBase = n * kMeshletDescDwords;
    const u32 triBase = vertBase + static_cast<u32>(verts.size());
    out.meshletCount = n;
    out.words.resize(static_cast<size_t>(triBase) + tris.size());
    u32* w = out.words.data();
    for (u32 i = 0; i < n; ++i) {
        const Meshlet& m = meshlets[i];
        u32* d = w + static_cast<size_t>(i) * kMeshletDescDwords;
        d[0] = vertBase + m.firstVertex;
        d[1] = triBase + m.firstTriangle;
        d[2] = m.vertexCount | (m.triangleCount << 8);
        d[3] = 0;
        f32 centre[3] = {0, 0, 0}, radius = -1.0f;
        if (positions) {
            const auto pos = [&](u32 v) {
                return reinterpret_cast<const f32*>(reinterpret_cast<const u8*>(positions) + static_cast<size_t>(v) * strideBytes);
            };
            f32 lo[3], hi[3];
            const f32* p0 = pos(verts[m.firstVertex]);
            for (int a = 0; a < 3; ++a) lo[a] = hi[a] = p0[a];
            for (u32 k = 1; k < m.vertexCount; ++k) {
                const f32* p = pos(verts[m.firstVertex + k]);
                for (int a = 0; a < 3; ++a) {
                    if (p[a] < lo[a]) lo[a] = p[a];
                    if (p[a] > hi[a]) hi[a] = p[a];
                }
            }
            for (int a = 0; a < 3; ++a) centre[a] = 0.5f * (lo[a] + hi[a]);
            f32 r2 = 0.0f;
            for (u32 k = 0; k < m.vertexCount; ++k) {
                const f32* p = pos(verts[m.firstVertex + k]);
                const f32 dx = p[0] - centre[0], dy = p[1] - centre[1], dz = p[2] - centre[2];
                const f32 d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > r2) r2 = d2;
            }
            // Widened by one part in 10^4 so f32 rounding in the shader's plane test never culls a vertex on the sphere.
            radius = std::sqrt(r2) * 1.0001f + 1e-4f;
            if (!std::isfinite(radius) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) || !std::isfinite(centre[2]))
                radius = -1.0f;
        }
        d[4] = floatBits(centre[0]);
        d[5] = floatBits(centre[1]);
        d[6] = floatBits(centre[2]);
        d[7] = floatBits(radius);
    }
    std::memcpy(w + vertBase, verts.data(), verts.size() * sizeof(u32));
    std::memcpy(w + triBase, tris.data(), tris.size() * sizeof(u32));
    return out;
}

} // namespace aver::rhi
