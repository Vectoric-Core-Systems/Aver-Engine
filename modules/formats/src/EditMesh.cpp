// EditMesh core: geometry queries, validation, triangulation, welding, conversions to csg and .ocmesh, and the save / reopen pair. See EditMesh.hpp.
#include "aver/formats/EditMesh.hpp"

#include "aver/formats/LightmapUnwrap.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace aver::fmt::em {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

u64 dkey(u32 a, u32 b) { return (u64(a) << 32) | b; }

void setWhy(std::string* why, const std::string& s) { if (why) *why = s; }

// Newell normal (unnormalised, length = 2 * area) of a corner list.
V3 newell(const EditMesh& m, const std::vector<u32>& v) {
    V3 n;
    for (usize i = 0; i < v.size(); ++i) {
        const V3& a = m.pos[v[i]];
        const V3& b = m.pos[v[(i + 1) % v.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

bool goodFace(const EditMesh& m, const Face& f) {
    if (f.v.size() < 3) return false;
    for (const u32 i : f.v) if (i >= m.pos.size()) return false;
    return true;
}

// 2D projection dropping the dominant axis of the face normal (orientation kept: counter-clockwise stays positive).
struct Proj2 {
    int a = 0, b = 1;
    explicit Proj2(const V3& n) {
        const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        if (ax >= ay && ax >= az) { a = 1; b = 2; if (n.x < 0) std::swap(a, b); }
        else if (ay >= az) { a = 2; b = 0; if (n.y < 0) std::swap(a, b); }
        else { a = 0; b = 1; if (n.z < 0) std::swap(a, b); }
    }
    V2 operator()(const V3& p) const { return {p[a], p[b]}; }
};

double cross2(const V2& o, const V2& a, const V2& b) { return (a.u - o.u) * (b.v - o.v) - (a.v - o.v) * (b.u - o.u); }

bool inTri(const V2& p, const V2& a, const V2& b, const V2& c) {
    const double d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
    return d1 >= -1e-12 && d2 >= -1e-12 && d3 >= -1e-12;
}

bool convexFace(const EditMesh& m, const Face& f, const V3& n) {
    const Proj2 pj(n);
    const usize c = f.v.size();
    for (usize i = 0; i < c; ++i) {
        const V2 a = pj(m.pos[f.v[i]]), b = pj(m.pos[f.v[(i + 1) % c]]), d = pj(m.pos[f.v[(i + 2) % c]]);
        if (cross2(a, b, d) < -1e-12) return false;
    }
    return true;
}

double planarDeviation(const EditMesh& m, const Face& f, const V3& unitN) {
    if (f.v.size() <= 3) return 0.0;
    const V3 c = [&] { V3 s; for (const u32 i : f.v) s += m.pos[i]; return s * (1.0 / f.v.size()); }();
    double dev = 0, ext = 0;
    for (const u32 i : f.v) { dev = std::max(dev, std::fabs(dot(m.pos[i] - c, unitN))); ext = std::max(ext, length(m.pos[i] - c)); }
    return ext > 0 ? dev / ext : 0.0;
}

}  // namespace

u32 EditMesh::addFace(std::vector<u32> verts, int material, u32 smoothing) {
    Face f;
    f.uv.assign(verts.size(), V2{});
    f.v = std::move(verts);
    f.material = material;
    f.smoothing = smoothing;
    faces.push_back(std::move(f));
    return static_cast<u32>(faces.size() - 1);
}

V3 faceNormal(const EditMesh& m, u32 fi) {
    const Face& f = m.faces[fi];
    return goodFace(m, f) ? normalized(newell(m, f.v)) : V3{};
}
double faceArea(const EditMesh& m, u32 fi) {
    const Face& f = m.faces[fi];
    return goodFace(m, f) ? 0.5 * length(newell(m, f.v)) : 0.0;
}
V3 faceCentroid(const EditMesh& m, u32 fi) {
    const Face& f = m.faces[fi];
    V3 s;
    for (const u32 i : f.v) s += m.pos[i];
    return f.v.empty() ? s : s * (1.0 / f.v.size());
}

Validation validate(const EditMesh& m) {
    Validation r;
    std::unordered_map<u64, u32> dir;
    std::vector<char> used(m.pos.size(), 0);
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        if (!goodFace(m, f)) { ++r.badIndex; continue; }
        bool degenerate = false;
        for (usize i = 0; i < f.v.size() && !degenerate; ++i)
            for (usize j = i + 1; j < f.v.size(); ++j) if (f.v[i] == f.v[j]) { degenerate = true; break; }
        const V3 n = newell(m, f.v);
        double longest = 0;
        for (usize i = 0; i < f.v.size(); ++i) longest = std::max(longest, length(m.pos[f.v[(i + 1) % f.v.size()]] - m.pos[f.v[i]]));
        if (degenerate || 0.5 * length(n) <= 1e-9 * longest * longest) { ++r.degenerateFaces; continue; }
        if (planarDeviation(m, f, normalized(n)) > 1e-4) ++r.nonPlanarFaces;
        for (usize i = 0; i < f.v.size(); ++i) { used[f.v[i]] = 1; ++dir[dkey(f.v[i], f.v[(i + 1) % f.v.size()])]; }
    }
    for (const auto& [k, n] : dir) {
        const u32 a = u32(k >> 32), b = u32(k & 0xFFFFFFFFu);
        const auto rev = dir.find(dkey(b, a));
        const u32 nr = rev == dir.end() ? 0 : rev->second;
        if (a > b && nr > 0) continue;                       // pair handled from the a < b side
        const u32 total = n + nr;
        if (total == 1) ++r.openEdges;
        else if (total > 2) ++r.nonManifoldEdges;
        else if (n == 2 || nr == 2) ++r.inconsistentEdges;
    }
    for (const char u : used) if (!u) ++r.unusedVertices;
    if (!r.closed()) {
        std::string s;
        const auto add = [&](u32 n, const char* what) { if (n) { if (!s.empty()) s += ", "; s += std::to_string(n) + " " + what; } };
        add(r.badIndex, "faces with a bad corner list");
        add(r.degenerateFaces, "degenerate faces");
        add(r.openEdges, "open edges");
        add(r.nonManifoldEdges, "edges shared by more than two faces");
        add(r.inconsistentEdges, "edges with mixed winding");
        if (m.faces.empty()) s = "the mesh has no faces";
        r.message = s;
    }
    return r;
}

void triangulateFace(const EditMesh& m, u32 fi, std::vector<Tri>& out) {
    const Face& f = m.faces[fi];
    const u32 c = static_cast<u32>(f.v.size());
    if (!goodFace(m, f)) return;
    if (c == 3) { out.push_back({fi, {0, 1, 2}}); return; }
    const V3 n = newell(m, f.v);
    const auto fan = [&] { for (u32 i = 1; i + 1 < c; ++i) out.push_back({fi, {0, i, i + 1}}); };
    if (length(n) < 1e-300 || convexFace(m, f, n)) { fan(); return; }
    const Proj2 pj(n);
    std::vector<V2> p(c);
    for (u32 i = 0; i < c; ++i) p[i] = pj(m.pos[f.v[i]]);
    std::vector<u32> idx(c);
    for (u32 i = 0; i < c; ++i) idx[i] = i;
    std::vector<Tri> local;
    u32 guard = 0;
    while (idx.size() > 3 && guard++ < c * c + 8) {
        bool clipped = false;
        const u32 k = static_cast<u32>(idx.size());
        for (u32 i = 0; i < k; ++i) {
            const u32 a = idx[(i + k - 1) % k], b = idx[i], d = idx[(i + 1) % k];
            if (cross2(p[a], p[b], p[d]) <= 1e-14) continue;     // reflex or flat
            bool empty = true;
            for (u32 j = 0; j < k && empty; ++j) {
                const u32 q = idx[j];
                if (q == a || q == b || q == d) continue;
                if (inTri(p[q], p[a], p[b], p[d])) empty = false;
            }
            if (!empty) continue;
            local.push_back({fi, {a, b, d}});
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) { fan(); return; }
    }
    if (idx.size() == 3) local.push_back({fi, {idx[0], idx[1], idx[2]}});
    out.insert(out.end(), local.begin(), local.end());
}

std::vector<Tri> triangulate(const EditMesh& m) {
    std::vector<Tri> t;
    t.reserve(m.faces.size() * 2);
    for (u32 i = 0; i < m.faces.size(); ++i) triangulateFace(m, i, t);
    return t;
}

Bounds3 bounds(const EditMesh& m) {
    Bounds3 b;
    for (const Face& f : m.faces)
        for (const u32 i : f.v) {
            if (i >= m.pos.size()) continue;
            const V3& p = m.pos[i];
            if (!b.valid) { b.lo = b.hi = p; b.valid = true; continue; }
            for (int k = 0; k < 3; ++k) { b.lo[k] = std::min(b.lo[k], p[k]); b.hi[k] = std::max(b.hi[k], p[k]); }
        }
    return b;
}

double volume(const EditMesh& m) {
    double v = 0;
    for (const Tri& t : triangulate(m)) {
        const Face& f = m.faces[t.face];
        v += dot(m.pos[f.v[t.c[0]]], cross(m.pos[f.v[t.c[1]]], m.pos[f.v[t.c[2]]])) / 6.0;
    }
    return v;
}

double surfaceArea(const EditMesh& m) {
    double a = 0;
    for (const Tri& t : triangulate(m)) {
        const Face& f = m.faces[t.face];
        const V3& p0 = m.pos[f.v[t.c[0]]];
        a += 0.5 * length(cross(m.pos[f.v[t.c[1]]] - p0, m.pos[f.v[t.c[2]]] - p0));
    }
    return a;
}

void compact(EditMesh& m, std::vector<u32>* remap) {
    std::vector<u32> map(m.pos.size(), kNone);
    for (const Face& f : m.faces) for (const u32 i : f.v) if (i < map.size()) map[i] = 0;
    std::vector<V3> np;
    for (u32 i = 0; i < m.pos.size(); ++i) if (map[i] != kNone) { map[i] = static_cast<u32>(np.size()); np.push_back(m.pos[i]); }
    for (Face& f : m.faces) for (u32& i : f.v) i = map[i];
    m.pos = std::move(np);
    if (remap) *remap = std::move(map);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Welding and cleanup
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Grid {
    double cell;
    std::unordered_map<u64, std::vector<u32>> cells;
    static i64 q(double v, double cell) { return static_cast<i64>(std::floor(v / cell)); }
    static u64 key(i64 x, i64 y, i64 z) { return (u64(x) * 73856093u) ^ (u64(y) * 19349663u) ^ (u64(z) * 83492791u); }
    explicit Grid(double c) : cell(c) {}
    void add(const V3& p, u32 id) { cells[key(q(p.x, cell), q(p.y, cell), q(p.z, cell))].push_back(id); }
    // Calls f(id) for every stored id in the 27 cells around p.
    template <class F> void forNear(const V3& p, F&& f) const {
        const i64 cx = q(p.x, cell), cy = q(p.y, cell), cz = q(p.z, cell);
        for (i64 dx = -1; dx <= 1; ++dx) for (i64 dy = -1; dy <= 1; ++dy) for (i64 dz = -1; dz <= 1; ++dz) {
            const auto it = cells.find(key(cx + dx, cy + dy, cz + dz));
            if (it != cells.end()) for (const u32 id : it->second) f(id);
        }
    }
};

double diagonalOf(const EditMesh& m) {
    const Bounds3 b = bounds(m);
    return b.valid ? length(b.hi - b.lo) : 1.0;
}

}  // namespace

u32 removeDegenerateFaces(EditMesh& m) {
    u32 removed = 0;
    std::vector<Face> keep;
    keep.reserve(m.faces.size());
    for (Face& f : m.faces) {
        // consecutive repeats (also across the wrap)
        for (usize i = 0; i < f.v.size() && f.v.size() >= 2;) {
            const usize j = (i + 1) % f.v.size();
            if (f.v[i] == f.v[j]) { f.v.erase(f.v.begin() + j); f.uv.erase(f.uv.begin() + j); if (j < i) --i; }
            else ++i;
        }
        if (!goodFace(m, f)) { ++removed; continue; }
        const V3 n = newell(m, f.v);
        double longest = 0;
        for (usize i = 0; i < f.v.size(); ++i) longest = std::max(longest, length(m.pos[f.v[(i + 1) % f.v.size()]] - m.pos[f.v[i]]));
        if (0.5 * length(n) <= 1e-12 * longest * longest || longest == 0) { ++removed; continue; }
        keep.push_back(std::move(f));
    }
    m.faces = std::move(keep);
    return removed;
}

u32 weldVertices(EditMesh& m, double tol) {
    if (m.pos.empty()) return 0;
    if (tol <= 0) tol = diagonalOf(m) * 1e-6;
    Grid grid(std::max(tol, 1e-12) * 2.0);
    std::vector<u32> map(m.pos.size());
    u32 merged = 0;
    for (u32 i = 0; i < m.pos.size(); ++i) {
        u32 hit = kNone;
        grid.forNear(m.pos[i], [&](u32 id) { if (hit == kNone && length(m.pos[id] - m.pos[i]) <= tol) hit = id; });
        if (hit == kNone) { grid.add(m.pos[i], i); map[i] = i; }
        else { map[i] = hit; ++merged; }
    }
    for (Face& f : m.faces) for (u32& i : f.v) if (i < map.size()) i = map[i];
    removeDegenerateFaces(m);
    return merged;
}

u32 repairTJunctions(EditMesh& m, double tol) {
    if (m.faces.empty()) return 0;
    if (tol <= 0) tol = diagonalOf(m) * 1e-6;
    // vertices sorted by x so each edge looks only at its x-range
    std::vector<u32> used;
    {
        std::vector<char> u(m.pos.size(), 0);
        for (const Face& f : m.faces) for (const u32 i : f.v) u[i] = 1;
        for (u32 i = 0; i < u.size(); ++i) if (u[i]) used.push_back(i);
    }
    std::sort(used.begin(), used.end(), [&](u32 a, u32 b) { return m.pos[a].x < m.pos[b].x; });
    const auto firstAtLeast = [&](double x) {
        return std::lower_bound(used.begin(), used.end(), x, [&](u32 id, double v) { return m.pos[id].x < v; });
    };
    u32 inserted = 0;
    for (Face& f : m.faces) {
        for (usize i = 0; i < f.v.size(); ++i) {
            for (int pass = 0; pass < 64; ++pass) {              // an edge can take several vertices
                const usize j = (i + 1) % f.v.size();
                const V3 a = m.pos[f.v[i]], b = m.pos[f.v[j]];
                const V3 ab = b - a;
                const double len2 = dot(ab, ab);
                if (len2 <= tol * tol) break;
                u32 best = kNone;
                double bestT = 0;
                const double lo = std::min(a.x, b.x) - tol, hi = std::max(a.x, b.x) + tol;
                for (auto it = firstAtLeast(lo); it != used.end() && m.pos[*it].x <= hi; ++it) {
                    const u32 q = *it;
                    if (q == f.v[i] || q == f.v[j]) continue;
                    const double t = dot(m.pos[q] - a, ab) / len2;
                    if (t <= 0 || t >= 1) continue;
                    if (length(m.pos[q] - (a + ab * t)) > tol) continue;
                    if (length(m.pos[q] - a) <= tol || length(m.pos[q] - b) <= tol) continue;
                    if (std::find(f.v.begin(), f.v.end(), q) != f.v.end()) continue;
                    if (best == kNone || t < bestT) { best = q; bestT = t; }
                }
                if (best == kNone) break;
                const V2 ua = f.uv[i], ub = f.uv[j];
                f.v.insert(f.v.begin() + j, best);
                f.uv.insert(f.uv.begin() + j, V2{ua.u + (ub.u - ua.u) * bestT, ua.v + (ub.v - ua.v) * bestT});
                ++inserted;
            }
        }
    }
    return inserted;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// csg
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

csg::Mesh toCsg(const EditMesh& m) {
    csg::Mesh out;
    const auto corner = [&](const Face& f, u32 c) {
        csg::Vertex v;
        const V3& p = m.pos[f.v[c]];
        v.p[0] = p.x; v.p[1] = p.y; v.p[2] = p.z;
        v.uv[0] = f.uv[c].u; v.uv[1] = f.uv[c].v;
        return v;
    };
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        if (!goodFace(m, f)) continue;
        const V3 n = newell(m, f.v);
        if (length(n) < 1e-300) continue;
        if (f.v.size() == 3 || (planarDeviation(m, f, normalized(n)) <= 1e-6 && convexFace(m, f, n))) {
            csg::Polygon p;
            p.material = f.material;
            for (u32 c = 0; c < f.v.size(); ++c) p.v.push_back(corner(f, c));
            out.polys.push_back(std::move(p));
        } else {
            std::vector<Tri> tris;
            triangulateFace(m, fi, tris);
            for (const Tri& t : tris) {
                csg::Polygon p;
                p.material = f.material;
                for (int k = 0; k < 3; ++k) p.v.push_back(corner(f, t.c[k]));
                out.polys.push_back(std::move(p));
            }
        }
    }
    return out;
}

EditMesh fromCsg(const csg::Mesh& c, double weldTol) {
    EditMesh m;
    const csg::Bounds b = csg::bounds(c);
    double diag = 1.0;
    if (b.valid) diag = std::sqrt((b.hi[0] - b.lo[0]) * (b.hi[0] - b.lo[0]) + (b.hi[1] - b.lo[1]) * (b.hi[1] - b.lo[1]) + (b.hi[2] - b.lo[2]) * (b.hi[2] - b.lo[2]));
    const double tol = weldTol > 0 ? weldTol : std::max(diag * 1e-6, 5e-5);
    Grid grid(tol * 2.0);
    for (const csg::Polygon& p : c.polys) {
        if (p.v.size() < 3) continue;
        Face f;
        f.material = p.material;
        for (const csg::Vertex& cv : p.v) {
            const V3 pos{cv.p[0], cv.p[1], cv.p[2]};
            u32 hit = kNone;
            grid.forNear(pos, [&](u32 id) { if (hit == kNone && length(m.pos[id] - pos) <= tol) hit = id; });
            if (hit == kNone) { hit = m.addVertex(pos); grid.add(pos, hit); }
            f.v.push_back(hit);
            f.uv.push_back({cv.uv[0], cv.uv[1]});
        }
        // smooth where a supplied vertex normal leaves the polygon's plane normal
        const V3 pn = normalized(newell(m, f.v));
        for (const csg::Vertex& cv : p.v) {
            const V3 vn = normalized(V3{cv.n[0], cv.n[1], cv.n[2]});
            if (length(vn) > 0.5 && dot(vn, pn) < 0.9995) f.smoothing = 1;
        }
        m.faces.push_back(std::move(f));
    }
    removeDegenerateFaces(m);
    repairTJunctions(m, tol);
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// .ocmesh
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

OcMeshData toOcMesh(const EditMesh& m) {
    OcMeshData d;
    const std::vector<Tri> tris = triangulate(m);
    int slots = static_cast<int>(m.materials.size());
    for (const Face& f : m.faces) slots = std::max(slots, f.material + 1);
    slots = std::max(slots, 1);

    std::vector<V3> faceN(m.faces.size());
    std::unordered_map<u64, V3> smooth;
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        if (!goodFace(m, f)) continue;
        const V3 raw = newell(m, f.v);
        faceN[fi] = normalized(raw);
        if (f.smoothing) for (const u32 v : f.v) smooth[dkey(v, f.smoothing)] += raw;
    }

    std::vector<std::vector<const Tri*>> byMat(slots);
    for (const Tri& t : tris) byMat[std::clamp(m.faces[t.face].material, 0, slots - 1)].push_back(&t);

    for (int s = 0; s < slots; ++s) {
        if (byMat[s].empty()) continue;
        OcMeshSubmesh sm;
        sm.materialSlot = static_cast<u32>(s);
        sm.name = s < static_cast<int>(m.materials.size()) && !m.materials[s].empty() ? m.materials[s] : "material_" + std::to_string(s);
        sm.indexStart = static_cast<u32>(d.indices.size());
        sm.baseVertex = d.vertexCount();
        std::unordered_map<std::string, u32> seen;
        for (const Tri* t : byMat[s]) {
            const Face& f = m.faces[t->face];
            for (int k = 0; k < 3; ++k) {
                const u32 c = t->c[k], vi = f.v[c];
                V3 n = faceN[t->face];
                if (f.smoothing) { const V3 sn = normalized(smooth[dkey(vi, f.smoothing)]); if (length(sn) > 0.5) n = sn; }
                const float rec[5] = {float(n.x), float(n.y), float(n.z), float(f.uv[c].u), float(f.uv[c].v)};
                std::string key(reinterpret_cast<const char*>(rec), sizeof rec);
                key.append(reinterpret_cast<const char*>(&vi), sizeof vi);
                const auto it = seen.find(key);
                if (it != seen.end()) { d.indices.push_back(it->second); continue; }
                const u32 id = d.vertexCount();
                seen.emplace(std::move(key), id);
                const V3& p = m.pos[vi];
                d.positions.insert(d.positions.end(), {float(p.x), float(p.y), float(p.z)});
                d.normals.insert(d.normals.end(), {float(n.x), float(n.y), float(n.z)});
                d.uvs.insert(d.uvs.end(), {float(f.uv[c].u), float(f.uv[c].v)});
                d.indices.push_back(id);
            }
        }
        sm.indexCount = static_cast<u32>(d.indices.size()) - sm.indexStart;
        sm.vertexCount = d.vertexCount() - sm.baseVertex;
        d.submeshes.push_back(std::move(sm));
    }
    for (int s = 0; s < slots; ++s)
        d.materialSlots.push_back(s < static_cast<int>(m.materials.size()) && !m.materials[s].empty() ? m.materials[s] : "material_" + std::to_string(s));
    generateTangents(d);
    d.computeBounds();
    return d;
}

bool fromOcMesh(const OcMeshData& d, EditMesh& out, std::string* why) {
    if (!d.valid()) { setWhy(why, "the mesh data is not valid"); return false; }
    if (d.hasSkin()) { setWhy(why, "skinned meshes cannot be edited as polygons"); return false; }
    if (d.hasMorph()) { setWhy(why, "meshes with morph targets cannot be edited as polygons"); return false; }
    EditMesh m;
    m.materials = d.materialSlots;
    std::map<std::array<float, 3>, u32> weld;
    std::vector<u32> map(d.vertexCount());
    for (u32 i = 0; i < d.vertexCount(); ++i) {
        const std::array<float, 3> k{d.positions[i * 3], d.positions[i * 3 + 1], d.positions[i * 3 + 2]};
        const auto it = weld.find(k);
        if (it != weld.end()) { map[i] = it->second; continue; }
        map[i] = m.addVertex({k[0], k[1], k[2]});
        weld.emplace(k, map[i]);
    }
    std::vector<int> triMat(d.indices.size() / 3, 0);
    for (const OcMeshSubmesh& s : d.submeshes)
        for (u32 t = s.indexStart / 3; t < (s.indexStart + s.indexCount) / 3 && t < triMat.size(); ++t) triMat[t] = static_cast<int>(s.materialSlot);
    for (usize t = 0; t + 2 < d.indices.size(); t += 3) {
        const u32 a = d.indices[t], b = d.indices[t + 1], c = d.indices[t + 2];
        if (a >= d.vertexCount() || b >= d.vertexCount() || c >= d.vertexCount()) { setWhy(why, "an index runs past the vertices"); return false; }
        if (map[a] == map[b] || map[b] == map[c] || map[a] == map[c]) continue;
        Face f;
        f.v = {map[a], map[b], map[c]};
        f.material = triMat[t / 3];
        const u32 src[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) f.uv.push_back({d.uvs[src[k] * 2], d.uvs[src[k] * 2 + 1]});
        const V3 fn = normalized(cross(m.pos[f.v[1]] - m.pos[f.v[0]], m.pos[f.v[2]] - m.pos[f.v[0]]));
        for (int k = 0; k < 3; ++k) {
            const V3 n{d.normals[src[k] * 3], d.normals[src[k] * 3 + 1], d.normals[src[k] * 3 + 2]};
            if (dot(n, fn) < 0.9995) f.smoothing = 1;
        }
        m.faces.push_back(std::move(f));
    }
    if (m.faces.empty()) { setWhy(why, "the mesh has no triangles"); return false; }
    out = std::move(m);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Sidecar and save / load
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

u64 hashOcMesh(const OcMeshData& d) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    if (!d.positions.empty()) mix(d.positions.data(), d.positions.size() * sizeof(f32));
    if (!d.indices.empty()) mix(d.indices.data(), d.indices.size() * sizeof(u32));
    if (!d.uvs.empty()) mix(d.uvs.data(), d.uvs.size() * sizeof(f32));
    return h;
}

std::string serializeEditMesh(const EditMesh& m, u64 meshHash) {
    std::ostringstream o;
    char buf[160];
    o << "EDITMESH 1\n";
    std::snprintf(buf, sizeof buf, "hash %016llx\n", static_cast<unsigned long long>(meshHash));
    o << buf;
    for (const std::string& s : m.materials) o << "m " << s << "\n";
    for (const V3& p : m.pos) { std::snprintf(buf, sizeof buf, "v %.17g %.17g %.17g\n", p.x, p.y, p.z); o << buf; }
    for (const Face& f : m.faces) {
        o << "f " << f.material << " " << f.smoothing << " " << f.v.size();
        for (usize i = 0; i < f.v.size(); ++i) { std::snprintf(buf, sizeof buf, " %u %.9g %.9g", f.v[i], f.uv[i].u, f.uv[i].v); o << buf; }
        o << "\n";
    }
    return o.str();
}

bool parseEditMesh(const std::string& text, EditMesh& out, u64* meshHash) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("EDITMESH 1", 0) != 0) return false;
    EditMesh m;
    u64 hash = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("hash ", 0) == 0) hash = std::strtoull(line.c_str() + 5, nullptr, 16);
        else if (line.rfind("m ", 0) == 0) m.materials.push_back(line.substr(2));
        else if (line.rfind("v ", 0) == 0) {
            V3 p;
            if (std::sscanf(line.c_str() + 2, "%lf %lf %lf", &p.x, &p.y, &p.z) != 3) return false;
            m.pos.push_back(p);
        } else if (line.rfind("f ", 0) == 0) {
            std::istringstream ls(line.substr(2));
            Face f;
            usize n = 0;
            if (!(ls >> f.material >> f.smoothing >> n) || n < 3 || n > 100000) return false;
            for (usize i = 0; i < n; ++i) {
                u32 v; double u, vv;
                if (!(ls >> v >> u >> vv) || v >= m.pos.size()) return false;
                f.v.push_back(v);
                f.uv.push_back({u, vv});
            }
            m.faces.push_back(std::move(f));
        }
    }
    out = std::move(m);
    if (meshHash) *meshHash = hash;
    return true;
}

bool addLightmapUv1(OcMeshData& d, u32 atlasEdge, std::string* err) {
    UnwrapResult r;
    if (!unwrapForLightmap(d.positions.data(), d.normals.data(), d.vertexCount(), d.indices.data(), static_cast<u32>(d.indices.size()), atlasEdge, r, err))
        return false;
    OcMeshData n = d;
    const auto gather = [&](const std::vector<f32>& src, usize stride, std::vector<f32>& dst) {
        if (src.empty()) return;
        dst.resize(r.vertexSource.size() * stride);
        for (usize i = 0; i < r.vertexSource.size(); ++i)
            for (usize k = 0; k < stride; ++k) dst[i * stride + k] = src[usize(r.vertexSource[i]) * stride + k];
    };
    gather(d.positions, 3, n.positions);
    gather(d.normals, 3, n.normals);
    gather(d.uvs, 2, n.uvs);
    gather(d.tangents, 4, n.tangents);
    gather(d.colors, 4, n.colors);
    n.uvs1 = r.uv;
    n.indices = r.indices;
    // vertex ranges are per submesh no more: a submesh draws from the whole rebuilt buffer
    for (OcMeshSubmesh& s : n.submeshes) { s.baseVertex = 0; s.vertexCount = n.vertexCount(); }
    n.computeBounds();
    d = std::move(n);
    return true;
}

bool saveAuthoredMesh(const std::string& path, const EditMesh& m, const SaveOptions& o, std::string* why) {
    if (m.faces.empty()) { setWhy(why, "nothing to save: the mesh has no faces"); return false; }
    OcMeshData d = toOcMesh(m);
    if (o.lightmapAtlasEdge > 0 && !addLightmapUv1(d, o.lightmapAtlasEdge, why)) return false;
    if (o.cook && !o.cook(d, why)) return false;
    std::vector<u8> bytes;
    if (!writeOcMesh(d, bytes, why)) return false;
    // The hash is of what a reader will see, so a later re-save or re-import of the file by anything else breaks the tie.
    OcMeshData back;
    if (!parseOcMesh(bytes.data(), bytes.size(), back, why)) return false;
    const u64 hash = hashOcMesh(back);
    if (!saveOcMesh(path, d, why)) return false;
    std::ofstream side(path + ".edit", std::ios::binary | std::ios::trunc);
    EditMesh clean = m;
    compact(clean);                                       // vertices an edit left behind are not worth keeping
    const std::string text = serializeEditMesh(clean, hash);
    side.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!side) { setWhy(why, "the mesh was saved but " + path + ".edit could not be written"); return false; }
    return true;
}

bool loadAuthoredMesh(const std::string& path, EditMesh& out, bool* fromSidecar, std::string* why) {
    if (fromSidecar) *fromSidecar = false;
    OcMeshData d;
    if (!loadOcMesh(path, d, why)) return false;
    std::ifstream side(path + ".edit", std::ios::binary);
    if (side) {
        std::stringstream ss;
        ss << side.rdbuf();
        EditMesh em;
        u64 hash = 0;
        if (parseEditMesh(ss.str(), em, &hash) && hash == hashOcMesh(d)) {
            out = std::move(em);
            if (fromSidecar) *fromSidecar = true;
            return true;
        }
    }
    return fromOcMesh(d, out, why);
}

}  // namespace aver::fmt::em
