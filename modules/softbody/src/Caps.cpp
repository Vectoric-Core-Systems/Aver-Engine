// Back skin and wall quads from a piece's visible triangles. See Caps.hpp.
#include "aver/softbody/Caps.hpp"

#include <algorithm>
#include <unordered_map>

namespace aver::softbody {
namespace {

constexpr u32 kNoVertex = 0xFFFFFFFFu;

struct EdgeRec {
    u32 da = 0, db = 0;        // draw vertices of the first triangle that used the edge, in its direction
    u32 wa = 0, wb = 0;        // their weld ids
    u32 count = 0;
};

u64 edgeKey(u32 wa, u32 wb) {
    const u32 lo = std::min(wa, wb), hi = std::max(wa, wb);
    return (static_cast<u64>(lo) << 32) | hi;
}

// Edges in first-use order (so every output is deterministic); `count` is how many visible triangles use each.
std::vector<EdgeRec> collectEdges(const u32* idx, u32 idxCount, const u32* weldOf, u32 n) {
    std::vector<EdgeRec> edges;
    std::unordered_map<u64, u32> lookup;
    lookup.reserve(idxCount);
    const u32 tris = idxCount / 3;
    for (u32 t = 0; t < tris; ++t) {
        for (u32 e = 0; e < 3; ++e) {
            const u32 a = idx[t * 3 + e], b = idx[t * 3 + (e + 1) % 3];
            if (a >= n || b >= n) continue;
            const u32 wa = weldOf[a], wb = weldOf[b];
            if (wa == wb) continue;   // a collapsed edge: a UV seam inside one particle is not an edge either
            const u64 key = edgeKey(wa, wb);
            const auto it = lookup.find(key);
            if (it == lookup.end()) {
                lookup.emplace(key, static_cast<u32>(edges.size()));
                edges.push_back(EdgeRec{a, b, wa, wb, 1});
            } else {
                ++edges[it->second].count;
            }
        }
    }
    return edges;
}

// A per-edge fraction in [0, 1): neighbouring walls start at different places of the tile, so a fracture face is
// not a repeating stripe. Order independent in (wa, wb).
f32 edgeHash(u32 wa, u32 wb) {
    u32 lo = std::min(wa, wb), hi = std::max(wa, wb);
    u32 x = lo * 0x9E3779B1u ^ (hi + 0x7F4A7C15u) * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return static_cast<f32>(x & 0xFFFFFFu) / 16777216.0f;
}

} // namespace

bool hasBoundaryEdges(const u32* idx, u32 idxCount, const u32* weldOf) {
    if (!idx || !weldOf || idxCount < 3) return false;
    u32 n = 0;
    for (u32 i = 0; i < idxCount; ++i) n = std::max(n, idx[i] + 1);
    for (const EdgeRec& e : collectEdges(idx, idxCount, weldOf, n))
        if (e.count == 1) return true;
    return false;
}

CapGeometry buildCaps(const u32* idx, u32 idxCount, const u32* weldOf, const Vec3* rest, const f32* uv, u32 n,
                      const CapSpec& spec) {
    CapGeometry g;
    if (!idx || !weldOf || idxCount < 3 || n == 0 || !(spec.thicknessCm > 0.0f)) return g;
    const f32 tile = spec.uvCmPerTile > 0.0f ? spec.uvCmPerTile : 40.0f;
    const u32 tris = idxCount / 3;

    // Walls: one quad per boundary edge, winding chosen so the quad faces away from its triangle. With the front
    // face normal N = (b - a) x (c - a), e = b - a and the back skin displaced along -N, N x e points into the
    // triangle, and (A0, B1, B0) and (A0, A1, B1) have normals along -(N x e): outward.
    const std::vector<EdgeRec> edges = collectEdges(idx, idxCount, weldOf, n);
    const f32 vBottom = spec.thicknessCm / tile;
    for (const EdgeRec& e : edges) {
        if (e.count != 1) continue;
        ++g.boundaryEdges;
        const f32 len = rest ? dist(rest[e.da], rest[e.db]) : 0.0f;
        const f32 u0 = edgeHash(e.wa, e.wb), u1 = u0 + len / tile;
        const u32 base = static_cast<u32>(g.wallVerts.size());
        g.wallVerts.push_back(CapVertex{e.da, 0.0f, u0, 0.0f});
        g.wallVerts.push_back(CapVertex{e.db, 0.0f, u1, 0.0f});
        g.wallVerts.push_back(CapVertex{e.da, 1.0f, u0, vBottom});
        g.wallVerts.push_back(CapVertex{e.db, 1.0f, u1, vBottom});
        for (const u32 k : {0u, 3u, 1u, 0u, 2u, 3u}) g.wallIndices.push_back(base + k);
    }

    // Back skin: one vertex per used draw vertex at depth 1 with the front's UV, the triangles reversed.
    if (spec.backSkin) {
        std::vector<u32> backOf(n, kNoVertex);
        for (u32 t = 0; t < tris; ++t) {
            u32 corner[3];
            bool ok = true;
            for (u32 k = 0; k < 3; ++k) {
                const u32 d = idx[t * 3 + k];
                if (d >= n) { ok = false; break; }
                if (backOf[d] == kNoVertex) {
                    backOf[d] = static_cast<u32>(g.backVerts.size());
                    g.backVerts.push_back(CapVertex{d, 1.0f, uv ? uv[d * 2] : 0.0f, uv ? uv[d * 2 + 1] : 0.0f});
                }
                corner[k] = backOf[d];
            }
            if (!ok) continue;
            g.backIndices.push_back(corner[0]);
            g.backIndices.push_back(corner[2]);
            g.backIndices.push_back(corner[1]);
        }
    }

    if (n + g.backVerts.size() + g.wallVerts.size() > spec.vertexLimit) {
        g = CapGeometry{};
        g.overLimit = true;
    }
    return g;
}

void smoothNormals(const Vec3* pos, u32 n, const u32* idx, u32 idxCount, const u32* weldOf, Vec3* outNormal) {
    for (u32 i = 0; i < n; ++i) outNormal[i] = Vec3{0, 0, 0};
    if (!idx || !weldOf || n == 0) return;
    u32 maxWeld = 0;
    for (u32 i = 0; i < n; ++i) maxWeld = std::max(maxWeld, weldOf[i]);
    std::vector<Vec3> group(static_cast<usize>(maxWeld) + 1, Vec3{0, 0, 0});
    const u32 tris = idxCount / 3;
    for (u32 t = 0; t < tris; ++t) {
        const u32 a = idx[t * 3 + 0], b = idx[t * 3 + 1], c = idx[t * 3 + 2];
        if (a >= n || b >= n || c >= n) continue;
        const Vec3 face = cross(pos[b] - pos[a], pos[c] - pos[a]);   // area weighted
        group[weldOf[a]] += face;
        group[weldOf[b]] += face;
        group[weldOf[c]] += face;
    }
    for (u32 i = 0; i < n; ++i) outNormal[i] = group[weldOf[i]].getSafeNormal();
}

void capPositions(const std::vector<CapVertex>& verts, const Vec3* pos, const Vec3* normal, f32 thicknessCm, Vec3* out) {
    for (usize i = 0; i < verts.size(); ++i) {
        const CapVertex& v = verts[i];
        out[i] = pos[v.src] - normal[v.src] * (v.depth * thicknessCm);
    }
}

} // namespace aver::softbody
