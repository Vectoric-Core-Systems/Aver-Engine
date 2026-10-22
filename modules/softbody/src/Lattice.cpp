// The jelly lattice: inside test by ray parity, Kuhn tetrahedra, boundary faces, snapping, and the barycentric embedding.
#include "aver/softbody/Lattice.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace aver::softbody {
namespace {

constexpr u32 kBadNode = 0xFFFFFFFFu;

struct QKey {
    i64 x = 0, y = 0, z = 0;
    bool operator==(const QKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct QKeyHash {
    usize operator()(const QKey& k) const {
        u64 h = static_cast<u64>(k.x) * 0x9E3779B97F4A7C15ull;
        h ^= (static_cast<u64>(k.y) + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= (static_cast<u64>(k.z) + 0x1B873593ull) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return static_cast<usize>(h);
    }
};

// Welded vertex ids: vertices closer than weldCm (on the quantisation grid) share one.
std::vector<u32> weldIds(const f32* p, u32 n, f32 weldCm) {
    std::vector<u32> ids(n);
    std::unordered_map<QKey, u32, QKeyHash> seen;
    seen.reserve(n);
    const f32 inv = 1.0f / std::max(weldCm, 1e-6f);
    for (u32 i = 0; i < n; ++i) {
        QKey k;
        k.x = static_cast<i64>(std::llround(p[i * 3 + 0] * inv));
        k.y = static_cast<i64>(std::llround(p[i * 3 + 1] * inv));
        k.z = static_cast<i64>(std::llround(p[i * 3 + 2] * inv));
        const u32 fresh = static_cast<u32>(seen.size());
        ids[i] = seen.emplace(k, fresh).first->second;
    }
    return ids;
}

// The point of triangle (a, b, c) nearest to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const Vec3 bp = p - b;
    const f32 d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const f32 d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const f32 denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

Vec3 at(const f32* p, u32 i) { return Vec3{p[i * 3 + 0], p[i * 3 + 1], p[i * 3 + 2]}; }

struct FaceRec {
    u32 count = 0, opp = 0, a = 0, b = 0, c = 0;
};

struct TetFrame {
    Vec3 p0{0, 0, 0}, c1{0, 0, 0}, c2{0, 0, 0}, c3{0, 0, 0};
    bool ok = false;
};

}  // namespace

f32 tetSignedVolume(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
    return dot(cross(b - a, c - a), d - a) / 6.0f;
}

bool meshIsClosed(const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount, f32 weldCm) {
    if (!positions || !indices || vertexCount < 4 || indexCount < 12 || indexCount % 3 != 0) return false;
    for (u32 i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount) return false;
    const std::vector<u32> w = weldIds(positions, vertexCount, weldCm);
    std::unordered_map<u64, u32> count;
    count.reserve(indexCount);
    const auto edge = [&](u32 a, u32 b) {
        const u64 key = (static_cast<u64>(std::min(a, b)) << 32) | static_cast<u64>(std::max(a, b));
        ++count[key];
    };
    u32 used = 0;
    for (u32 t = 0; t < indexCount / 3; ++t) {
        const u32 a = w[indices[t * 3]], b = w[indices[t * 3 + 1]], c = w[indices[t * 3 + 2]];
        if (a == b || b == c || a == c) continue;   // a degenerate triangle covers nothing
        edge(a, b); edge(b, c); edge(c, a);
        ++used;
    }
    if (used < 4) return false;
    for (const auto& kv : count)
        if (kv.second & 1u) return false;
    return true;
}

bool buildLattice(const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount, const LatticeSpec& spec, Lattice& out) {
    out = Lattice{};
    const auto fail = [&out](const char* why) {
        out.error = why;
        return false;
    };
    if (!positions || !indices || vertexCount < 4 || indexCount < 12 || indexCount % 3 != 0) return fail("jelly: the mesh has no triangles to fill");
    if (!(spec.cellCm > 0.0f) || !std::isfinite(spec.cellCm)) return fail("jelly: the cell size must be > 0");
    for (u32 i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount) return fail("jelly: a triangle index is out of range");
    for (u32 i = 0; i < vertexCount * 3u; ++i)
        if (!std::isfinite(positions[i])) return fail("jelly: the mesh has a non-finite vertex");
    if (!meshIsClosed(positions, vertexCount, indices, indexCount))
        return fail("jelly: the mesh is not closed (every edge must be shared by two triangles)");

    const u32 maxNodes = std::min<u32>(std::max<u32>(spec.maxNodes, 4u), 65535u);
    const f32 cell = spec.cellCm;
    out.cellCm = cell;

    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < vertexCount; ++i) {
        const Vec3 p = at(positions, i);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const Vec3 ext = hi - lo;
    if (static_cast<f64>(ext.x) / cell > 1024.0 || static_cast<f64>(ext.y) / cell > 1024.0 || static_cast<f64>(ext.z) / cell > 1024.0) {
        out.overCap = true;
        return fail("jelly: the cell size is too small for this mesh (over the limit)");
    }
    const auto cellsOn = [cell](f32 e) { return std::max<u32>(1u, static_cast<u32>(std::ceil(e / cell - 1e-3f))); };
    const u32 nx = cellsOn(ext.x), ny = cellsOn(ext.y), nz = cellsOn(ext.z);
    if (static_cast<u64>(nx) * ny * nz > 2000000ull) {
        out.overCap = true;
        return fail("jelly: the cell size is too small for this mesh (over the limit)");
    }
    // The grid is centred on the mesh, so the margin is the same on both sides of each axis.
    const Vec3 org{lo.x - (static_cast<f32>(nx) * cell - ext.x) * 0.5f, lo.y - (static_cast<f32>(ny) * cell - ext.y) * 0.5f,
                   lo.z - (static_cast<f32>(nz) * cell - ext.z) * 0.5f};

    // ---- inside test: ray parity along +X through every (j, k) row centre, nudged off the grid so a ray never grazes an edge ----
    const f32 y0 = org.y + 0.5f * cell + cell * 0.00137f, z0 = org.z + 0.5f * cell + cell * 0.00291f;
    std::vector<std::pair<u32, f32>> hits;   // (row, x of the crossing)
    const u32 triCount = indexCount / 3;
    for (u32 t = 0; t < triCount; ++t) {
        const Vec3 a = at(positions, indices[t * 3]), b = at(positions, indices[t * 3 + 1]), c = at(positions, indices[t * 3 + 2]);
        const f32 d = (b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z);
        if (std::fabs(d) < 1e-12f) continue;   // edge-on to the ray
        const f32 minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
        const f32 minZ = std::min({a.z, b.z, c.z}), maxZ = std::max({a.z, b.z, c.z});
        i64 j0 = static_cast<i64>(std::ceil((minY - y0) / cell)), j1 = static_cast<i64>(std::floor((maxY - y0) / cell));
        i64 k0 = static_cast<i64>(std::ceil((minZ - z0) / cell)), k1 = static_cast<i64>(std::floor((maxZ - z0) / cell));
        j0 = std::max<i64>(j0, 0); j1 = std::min<i64>(j1, static_cast<i64>(ny) - 1);
        k0 = std::max<i64>(k0, 0); k1 = std::min<i64>(k1, static_cast<i64>(nz) - 1);
        for (i64 k = k0; k <= k1; ++k)
            for (i64 j = j0; j <= j1; ++j) {
                const f32 py = y0 + static_cast<f32>(j) * cell, pz = z0 + static_cast<f32>(k) * cell;
                const f32 u = ((py - a.y) * (c.z - a.z) - (c.y - a.y) * (pz - a.z)) / d;
                const f32 v = ((b.y - a.y) * (pz - a.z) - (py - a.y) * (b.z - a.z)) / d;
                if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
                hits.emplace_back(static_cast<u32>(k) * ny + static_cast<u32>(j), a.x + u * (b.x - a.x) + v * (c.x - a.x));
            }
    }
    std::sort(hits.begin(), hits.end());

    std::vector<u8> inside(static_cast<usize>(nx) * ny * nz, 0);
    {
        usize h = 0;
        for (u32 k = 0; k < nz; ++k)
            for (u32 j = 0; j < ny; ++j) {
                const u32 row = k * ny + j;
                while (h < hits.size() && hits[h].first < row) ++h;
                usize e = h;
                while (e < hits.size() && hits[e].first == row) ++e;
                usize c = h;
                for (u32 i = 0; i < nx; ++i) {
                    const f32 cx = org.x + (static_cast<f32>(i) + 0.5f) * cell;
                    while (c < e && hits[c].second < cx) ++c;
                    inside[(static_cast<usize>(k) * ny + j) * nx + i] = ((c - h) & 1u) ? 1 : 0;
                }
                h = e;
            }
    }
    hits.clear();
    hits.shrink_to_fit();

    // ---- nodes and Kuhn tetrahedra of the kept cubes ----
    std::unordered_map<u64, u32> nodeOf;
    const auto nodeAt = [&](u32 i, u32 j, u32 k) -> u32 {
        const u64 key = (static_cast<u64>(k) * (ny + 1) + j) * (nx + 1) + i;
        const auto it = nodeOf.find(key);
        if (it != nodeOf.end()) return it->second;
        if (out.nodes.size() >= maxNodes) return kBadNode;
        const u32 id = static_cast<u32>(out.nodes.size());
        out.nodes.push_back(Vec3{org.x + static_cast<f32>(i) * cell, org.y + static_cast<f32>(j) * cell, org.z + static_cast<f32>(k) * cell});
        nodeOf.emplace(key, id);
        return id;
    };
    static const int kPerms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    bool any = false;
    for (u32 k = 0; k < nz; ++k)
        for (u32 j = 0; j < ny; ++j)
            for (u32 i = 0; i < nx; ++i) {
                if (!inside[(static_cast<usize>(k) * ny + j) * nx + i]) continue;
                any = true;
                for (const auto& p : kPerms) {
                    int off[4][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}};
                    off[1][p[0]] = 1;
                    off[2][p[0]] = 1;
                    off[2][p[1]] = 1;
                    u32 v[4];
                    for (int q = 0; q < 4; ++q) {
                        v[q] = nodeAt(i + static_cast<u32>(off[q][0]), j + static_cast<u32>(off[q][1]), k + static_cast<u32>(off[q][2]));
                        if (v[q] == kBadNode) {
                            out.overCap = true;
                            return fail("jelly: the lattice needs more nodes than the node cap (over the limit); raise cellCm or maxNodes");
                        }
                    }
                    if (tetSignedVolume(out.nodes[v[0]], out.nodes[v[1]], out.nodes[v[2]], out.nodes[v[3]]) < 0.0f) std::swap(v[2], v[3]);
                    out.tets.insert(out.tets.end(), {v[0], v[1], v[2], v[3]});
                }
            }
    if (!any || out.tets.empty()) return fail("jelly: the lattice is empty (the cell is larger than the mesh)");
    const u32 tetCount = static_cast<u32>(out.tets.size() / 4);

    // ---- edges ----
    {
        std::vector<u64> keys;
        keys.reserve(static_cast<usize>(tetCount) * 6);
        for (u32 t = 0; t < tetCount; ++t)
            for (u32 x = 0; x < 4; ++x)
                for (u32 y = x + 1; y < 4; ++y) {
                    const u32 a = out.tets[t * 4 + x], b = out.tets[t * 4 + y];
                    keys.push_back((static_cast<u64>(std::min(a, b)) << 32) | static_cast<u64>(std::max(a, b)));
                }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        out.edges.reserve(keys.size() * 2);
        for (const u64 k : keys) {
            out.edges.push_back(static_cast<u32>(k >> 32));
            out.edges.push_back(static_cast<u32>(k & 0xFFFFFFFFull));
        }
    }

    // ---- boundary faces: a tetrahedron face no other tetrahedron has ----
    std::unordered_map<u64, FaceRec> faces;
    faces.reserve(static_cast<usize>(tetCount) * 3);
    static const int kFace[4][3] = {{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}};
    for (u32 t = 0; t < tetCount; ++t) {
        const u32* v = &out.tets[t * 4];
        for (int f = 0; f < 4; ++f) {
            u32 s[3] = {v[kFace[f][0]], v[kFace[f][1]], v[kFace[f][2]]};
            std::sort(s, s + 3);
            const u64 key = static_cast<u64>(s[0]) | (static_cast<u64>(s[1]) << 16) | (static_cast<u64>(s[2]) << 32);
            FaceRec& r = faces[key];
            if (r.count++ == 0) {
                r.opp = v[f];
                r.a = v[kFace[f][0]]; r.b = v[kFace[f][1]]; r.c = v[kFace[f][2]];
            }
        }
    }
    std::vector<std::pair<u64, FaceRec>> boundary;
    for (const auto& kv : faces)
        if (kv.second.count == 1) boundary.push_back(kv);
    std::sort(boundary.begin(), boundary.end(), [](const auto& x, const auto& y) { return x.first < y.first; });

    std::vector<u8> onSurface(out.nodes.size(), 0);
    for (const auto& kv : boundary) {
        onSurface[kv.second.a] = onSurface[kv.second.b] = onSurface[kv.second.c] = 1;
    }
    for (const u8 s : onSurface) out.surfaceNodeCount += s;

    // ---- snap boundary nodes onto the surface ----
    const f32 snap = std::min(spec.snapCm, 0.45f * cell);
    if (snap > 0.0f) {
        std::vector<Vec3> tlo(triCount), thi(triCount);
        for (u32 t = 0; t < triCount; ++t) {
            const Vec3 a = at(positions, indices[t * 3]), b = at(positions, indices[t * 3 + 1]), c = at(positions, indices[t * 3 + 2]);
            tlo[t] = {std::min({a.x, b.x, c.x}) - snap, std::min({a.y, b.y, c.y}) - snap, std::min({a.z, b.z, c.z}) - snap};
            thi[t] = {std::max({a.x, b.x, c.x}) + snap, std::max({a.y, b.y, c.y}) + snap, std::max({a.z, b.z, c.z}) + snap};
        }
        for (usize n = 0; n < out.nodes.size(); ++n) {
            if (!onSurface[n]) continue;
            const Vec3 p = out.nodes[n];
            f32 bestSq = snap * snap;
            Vec3 best = p;
            bool found = false;
            for (u32 t = 0; t < triCount; ++t) {
                if (p.x < tlo[t].x || p.x > thi[t].x || p.y < tlo[t].y || p.y > thi[t].y || p.z < tlo[t].z || p.z > thi[t].z) continue;
                const Vec3 q = closestOnTriangle(p, at(positions, indices[t * 3]), at(positions, indices[t * 3 + 1]), at(positions, indices[t * 3 + 2]));
                const f32 dsq = distSquared(q, p);
                if (dsq <= bestSq) { bestSq = dsq; best = q; found = true; }
            }
            if (found) out.nodes[n] = best;
        }
    }

    // ---- orient the boundary faces so (C - A) x (B - A) points away from the tetrahedron's fourth vertex ----
    out.surfaceFaces.reserve(boundary.size() * 3);
    for (const auto& kv : boundary) {
        const FaceRec& r = kv.second;
        const Vec3 a = out.nodes[r.a], b = out.nodes[r.b], c = out.nodes[r.c], o = out.nodes[r.opp];
        const Vec3 n = cross(c - a, b - a);
        const Vec3 mid = (a + b + c) * (1.0f / 3.0f);
        if (dot(n, mid - o) >= 0.0f) out.surfaceFaces.insert(out.surfaceFaces.end(), {r.a, r.b, r.c});
        else out.surfaceFaces.insert(out.surfaceFaces.end(), {r.a, r.c, r.b});
    }

    f32 vol = 0.0f;
    for (u32 t = 0; t < tetCount; ++t)
        vol += std::fabs(tetSignedVolume(out.nodes[out.tets[t * 4]], out.nodes[out.tets[t * 4 + 1]], out.nodes[out.tets[t * 4 + 2]],
                                         out.nodes[out.tets[t * 4 + 3]]));
    out.restVolumeCm3 = vol;
    return true;
}

// ---- embedding ----------------------------------------------------------------------------------------------

void embedMesh(const Lattice& lat, const f32* positions, u32 vertexCount, Embedding& out) {
    out.tet.assign(vertexCount, 0u);
    out.bary.assign(static_cast<usize>(vertexCount) * 4, 0.0f);
    const u32 tetCount = static_cast<u32>(lat.tets.size() / 4);
    if (tetCount == 0 || !positions) {
        for (u32 v = 0; v < vertexCount; ++v) out.bary[static_cast<usize>(v) * 4] = 1.0f;
        return;
    }

    std::vector<TetFrame> frames(tetCount);
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vec3& n : lat.nodes) {
        lo = {std::min(lo.x, n.x), std::min(lo.y, n.y), std::min(lo.z, n.z)};
        hi = {std::max(hi.x, n.x), std::max(hi.y, n.y), std::max(hi.z, n.z)};
    }
    for (u32 t = 0; t < tetCount; ++t) {
        const Vec3 p0 = lat.nodes[lat.tets[t * 4]];
        const Vec3 e1 = lat.nodes[lat.tets[t * 4 + 1]] - p0, e2 = lat.nodes[lat.tets[t * 4 + 2]] - p0, e3 = lat.nodes[lat.tets[t * 4 + 3]] - p0;
        const f32 det = dot(e1, cross(e2, e3));
        TetFrame& f = frames[t];
        f.p0 = p0;
        if (std::fabs(det) < 1e-9f) continue;
        const f32 inv = 1.0f / det;
        f.c1 = cross(e2, e3) * inv;
        f.c2 = cross(e3, e1) * inv;
        f.c3 = cross(e1, e2) * inv;
        f.ok = true;
    }

    // A uniform bucket grid over the tetrahedra's boxes, one bucket per cell edge.
    const f32 bs = std::max(lat.cellCm, 1e-3f);
    const auto dimOf = [bs](f32 a, f32 b) { return std::clamp<u32>(static_cast<u32>(std::ceil((b - a) / bs)), 1u, 128u); };
    const u32 bx = dimOf(lo.x, hi.x), by = dimOf(lo.y, hi.y), bz = dimOf(lo.z, hi.z);
    const auto bucketOf = [&](f32 v, f32 base, u32 n) {
        return static_cast<u32>(std::clamp<i64>(static_cast<i64>(std::floor((v - base) / bs)), 0, static_cast<i64>(n) - 1));
    };
    std::vector<std::vector<u32>> buckets(static_cast<usize>(bx) * by * bz);
    for (u32 t = 0; t < tetCount; ++t) {
        Vec3 tlo{1e30f, 1e30f, 1e30f}, thi{-1e30f, -1e30f, -1e30f};
        for (u32 q = 0; q < 4; ++q) {
            const Vec3& n = lat.nodes[lat.tets[t * 4 + q]];
            tlo = {std::min(tlo.x, n.x), std::min(tlo.y, n.y), std::min(tlo.z, n.z)};
            thi = {std::max(thi.x, n.x), std::max(thi.y, n.y), std::max(thi.z, n.z)};
        }
        for (u32 k = bucketOf(tlo.z, lo.z, bz); k <= bucketOf(thi.z, lo.z, bz); ++k)
            for (u32 j = bucketOf(tlo.y, lo.y, by); j <= bucketOf(thi.y, lo.y, by); ++j)
                for (u32 i = bucketOf(tlo.x, lo.x, bx); i <= bucketOf(thi.x, lo.x, bx); ++i)
                    buckets[(static_cast<usize>(k) * by + j) * bx + i].push_back(t);
    }

    for (u32 v = 0; v < vertexCount; ++v) {
        const Vec3 q = at(positions, v);
        f32 bestMin = -1e30f;
        f32 bestW[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        u32 bestTet = 0;
        const auto consider = [&](u32 t) {
            const TetFrame& f = frames[t];
            if (!f.ok) return;
            const Vec3 d = q - f.p0;
            const f32 w1 = dot(d, f.c1), w2 = dot(d, f.c2), w3 = dot(d, f.c3);
            const f32 w[4] = {1.0f - w1 - w2 - w3, w1, w2, w3};
            const f32 m = std::min(std::min(w[0], w[1]), std::min(w[2], w[3]));
            if (m > bestMin) {
                bestMin = m;
                bestTet = t;
                for (int c = 0; c < 4; ++c) bestW[c] = w[c];
            }
        };
        const u32 qi = bucketOf(q.x, lo.x, bx), qj = bucketOf(q.y, lo.y, by), qk = bucketOf(q.z, lo.z, bz);
        for (const u32 t : buckets[(static_cast<usize>(qk) * by + qj) * bx + qi]) consider(t);
        if (bestMin < -1e-4f) {
            for (i64 dk = -1; dk <= 1; ++dk)
                for (i64 dj = -1; dj <= 1; ++dj)
                    for (i64 di = -1; di <= 1; ++di) {
                        const i64 ni = static_cast<i64>(qi) + di, nj = static_cast<i64>(qj) + dj, nk = static_cast<i64>(qk) + dk;
                        if (ni < 0 || nj < 0 || nk < 0 || ni >= bx || nj >= by || nk >= bz) continue;
                        for (const u32 t : buckets[(static_cast<usize>(nk) * by + static_cast<usize>(nj)) * bx + static_cast<usize>(ni)]) consider(t);
                    }
        }
        if (bestMin < -1e-4f)
            for (u32 t = 0; t < tetCount; ++t) consider(t);   // far outside the lattice: nearest by extrapolation
        // Extrapolation is allowed while every weight stays within 2 in magnitude; a vertex farther out than that is pulled
        // back onto the tetrahedron (weights clamped to >= 0 and renormalised), so the cap holds and the weights still sum to 1.
        bool tooFar = false;
        for (int c = 0; c < 4; ++c) tooFar = tooFar || std::fabs(bestW[c]) > 2.0f;
        if (tooFar) {
            f32 sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                bestW[c] = std::max(bestW[c], 0.0f);
                sum += bestW[c];
            }
            if (sum > 1e-6f) for (int c = 0; c < 4; ++c) bestW[c] /= sum;
            else { bestW[0] = 1.0f; bestW[1] = bestW[2] = bestW[3] = 0.0f; }
        }
        out.tet[v] = bestTet;
        for (int c = 0; c < 4; ++c) out.bary[static_cast<usize>(v) * 4 + static_cast<usize>(c)] = bestW[c];
    }
}

void embeddedPositions(const Lattice& lat, const Embedding& emb, const Vec3* nodeWorld, u32 vertexCount, Vec3* out) {
    for (u32 v = 0; v < vertexCount; ++v) {
        const u32 t = v < emb.tet.size() ? emb.tet[v] : 0u;
        Vec3 p{0, 0, 0};
        if ((static_cast<usize>(t) * 4 + 3) < lat.tets.size() && (static_cast<usize>(v) * 4 + 3) < emb.bary.size())
            for (u32 c = 0; c < 4; ++c) p += nodeWorld[lat.tets[static_cast<usize>(t) * 4 + c]] * emb.bary[static_cast<usize>(v) * 4 + c];
        out[v] = p;
    }
}

}  // namespace aver::softbody
