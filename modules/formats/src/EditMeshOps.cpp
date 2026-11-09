// EditMesh primitives, poly edit operations, booleans and UV tools. See EditMesh.hpp.
#include "aver/formats/EditMesh.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace aver::fmt::em {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;
constexpr double kPi = 3.14159265358979323846;

u64 dkey(u32 a, u32 b) { return (u64(a) << 32) | b; }

OpResult fail(const std::string& why) { OpResult r; r.message = why; return r; }

V3 newellOf(const EditMesh& m, const std::vector<u32>& v) {
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

bool faceOk(const EditMesh& m, const Face& f) {
    if (f.v.size() < 3) return false;
    for (const u32 i : f.v) if (i >= m.pos.size()) return false;
    return true;
}

std::vector<u32> uniqueSorted(std::vector<u32> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// A face's UV as an affine function of position, so new vertices placed in its plane get the UV the texture would give them there.
struct UvMap {
    V3 p0; V2 uv0; V3 gu, gv; bool ok = false;
    V2 at(const V3& p) const { const V3 d = p - p0; return {uv0.u + dot(gu, d), uv0.v + dot(gv, d)}; }
};
UvMap uvMapOf(const EditMesh& m, const Face& f) {
    UvMap r;
    const V3& p0 = m.pos[f.v[0]];
    r.p0 = p0; r.uv0 = f.uv[0];
    double best = 0; usize bi = 1, bj = 2;
    for (usize i = 1; i < f.v.size(); ++i)
        for (usize j = i + 1; j < f.v.size(); ++j) {
            const double a = length(cross(m.pos[f.v[i]] - p0, m.pos[f.v[j]] - p0));
            if (a > best) { best = a; bi = i; bj = j; }
        }
    if (best < 1e-12) return r;
    const V3 e1 = m.pos[f.v[bi]] - p0, e2 = m.pos[f.v[bj]] - p0, n = cross(e1, e2);
    const double inv = 1.0 / dot(n, n);
    const double du1 = f.uv[bi].u - f.uv[0].u, du2 = f.uv[bj].u - f.uv[0].u;
    const double dv1 = f.uv[bi].v - f.uv[0].v, dv2 = f.uv[bj].v - f.uv[0].v;
    r.gu = (cross(e2, n) * du1 + cross(n, e1) * du2) * inv;
    r.gv = (cross(e2, n) * dv1 + cross(n, e1) * dv2) * inv;
    r.ok = true;
    return r;
}

// UV for a polygon that has none: planar in its own plane, `tile` world units per 1.0.
void planarUvFor(const EditMesh& m, Face& f, double tile) {
    const V3 n = normalized(newellOf(m, f.v));
    V3 e1 = m.pos[f.v[1]] - m.pos[f.v[0]];
    e1 = normalized(e1 - n * dot(e1, n));
    const V3 e2 = cross(n, e1);
    f.uv.resize(f.v.size());
    for (usize i = 0; i < f.v.size(); ++i) {
        const V3 d = m.pos[f.v[i]] - m.pos[f.v[0]];
        f.uv[i] = {dot(d, e1) / tile, dot(d, e2) / tile};
    }
}

// ---- primitives -----------------------------------------------------------------------------------------------------------------------------------

void applyPivot(EditMesh& m, const PrimitiveOptions& o) {
    const Bounds3 b = bounds(m);
    if (!b.valid) return;
    const V3 shift{-(b.lo.x + b.hi.x) / 2, -(b.lo.y + b.hi.y) / 2, o.pivotAtBase ? -b.lo.z : -(b.lo.z + b.hi.z) / 2};
    for (V3& p : m.pos) p += shift;
}

EditMesh fromCsgYUp(csg::Mesh c, const PrimitiveOptions& o) {
    const double rot[12] = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0};   // Y-up generators -> engine +Z up (a proper rotation)
    csg::transform(c, rot);
    EditMesh m = fromCsg(c);
    for (Face& f : m.faces) f.material = o.material;
    return m;
}

// A closed prism: profile (x, z) counter-clockwise, extruded `depth` along Y (centred).
EditMesh extrudedProfile(const std::vector<V2>& prof, double depth) {
    EditMesh m;
    const u32 n = static_cast<u32>(prof.size());
    for (u32 i = 0; i < n; ++i) m.addVertex({prof[i].u, -depth / 2, prof[i].v});
    for (u32 i = 0; i < n; ++i) m.addVertex({prof[i].u, depth / 2, prof[i].v});
    std::vector<u32> front(n), back(n);
    for (u32 i = 0; i < n; ++i) { front[i] = i; back[n - 1 - i] = n + i; }
    m.addFace(front);
    m.addFace(back);
    for (u32 i = 0; i < n; ++i) m.addFace({i, n + i, n + (i + 1) % n, (i + 1) % n});
    return m;
}

void finishFlat(EditMesh& m, const PrimitiveOptions& o) {
    for (Face& f : m.faces) f.material = o.material;
    UvProjectionParams p;
    p.tile = o.uvTile;
    projectUv(m, {}, UvProjection::Box, p);
    applyPivot(m, o);
}

}  // namespace

EditMesh makeBox(const V3& size, const PrimitiveOptions& o) {
    const double s[3] = {size.x, size.y, size.z};
    EditMesh m = fromCsg(csg::makeBox(s, o.material));
    finishFlat(m, o);
    return m;
}

EditMesh makeSphere(double radius, int segments, int rings, const PrimitiveOptions& o) {
    EditMesh m = fromCsgYUp(csg::makeSphere(radius, segments, rings, true, o.material), o);
    for (Face& f : m.faces) f.smoothing = 1;
    applyPivot(m, o);
    return m;
}

EditMesh makeCylinder(double radius, double height, int sides, const PrimitiveOptions& o) {
    EditMesh m = fromCsgYUp(csg::makeCylinder(radius, height, sides, false, true, o.material), o);
    for (u32 i = 0; i < m.faces.size(); ++i) m.faces[i].smoothing = std::fabs(faceNormal(m, i).z) < 0.99 ? 1 : 0;
    applyPivot(m, o);
    return m;
}

EditMesh makeCone(double radius, double height, int sides, const PrimitiveOptions& o) {
    EditMesh m = fromCsgYUp(csg::makeCylinder(radius, height, sides, true, true, o.material), o);
    for (u32 i = 0; i < m.faces.size(); ++i) m.faces[i].smoothing = faceNormal(m, i).z < 0.99 ? 1 : 0;
    applyPivot(m, o);
    return m;
}

EditMesh makeTorus(double innerRadius, double outerRadius, int sides, int ringSides, const PrimitiveOptions& o) {
    EditMesh m = fromCsgYUp(csg::makeTorus(innerRadius, outerRadius, sides, ringSides, true, o.material), o);
    for (Face& f : m.faces) f.smoothing = 1;
    applyPivot(m, o);
    return m;
}

EditMesh makePlane(double sizeX, double sizeY, int segX, int segY, const PrimitiveOptions& o) {
    EditMesh m;
    segX = std::max(segX, 1); segY = std::max(segY, 1);
    for (int j = 0; j <= segY; ++j)
        for (int i = 0; i <= segX; ++i) m.addVertex({sizeX * (double(i) / segX - 0.5), sizeY * (double(j) / segY - 0.5), 0});
    const auto id = [&](int i, int j) { return static_cast<u32>(j * (segX + 1) + i); };
    for (int j = 0; j < segY; ++j)
        for (int i = 0; i < segX; ++i) {
            const u32 f = m.addFace({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)}, o.material);
            const double u0 = double(i) / segX, u1 = double(i + 1) / segX, v0 = double(j) / segY, v1 = double(j + 1) / segY;
            m.faces[f].uv = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        }
    applyPivot(m, o);
    return m;
}

EditMesh makeStairs(double width, double depth, double height, int steps, const PrimitiveOptions& o) {
    steps = std::max(steps, 1);
    const double run = depth / steps, rise = height / steps;
    std::vector<V2> prof{{0, 0}, {depth, 0}};
    for (int i = steps - 1; i >= 0; --i) {
        prof.push_back({(i + 1) * run, (i + 1) * rise});
        prof.push_back({i * run, (i + 1) * rise});
    }
    // consecutive collinear points (the riser and the last tread corner) are fine for the ear clipper but waste vertices: drop the repeats of the corner at x = depth
    EditMesh m = extrudedProfile(prof, width);
    finishFlat(m, o);
    return m;
}

EditMesh makeArch(double width, double legHeight, double wall, double depth, int arcSegments, const PrimitiveOptions& o) {
    arcSegments = std::max(arcSegments, 2);
    const double R = width / 2, Ri = std::max(R - wall, 1e-6);
    std::vector<V2> prof{{-R, 0}, {-Ri, 0}, {-Ri, legHeight}};
    for (int k = 1; k < arcSegments; ++k) { const double a = kPi - kPi * k / arcSegments; prof.push_back({Ri * std::cos(a), legHeight + Ri * std::sin(a)}); }
    prof.push_back({Ri, legHeight});
    prof.push_back({Ri, 0});
    prof.push_back({R, 0});
    prof.push_back({R, legHeight});
    for (int k = 1; k < arcSegments; ++k) { const double a = kPi * k / arcSegments; prof.push_back({R * std::cos(a), legHeight + R * std::sin(a)}); }
    prof.push_back({-R, legHeight});
    EditMesh m = extrudedProfile(prof, depth);
    finishFlat(m, o);
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Selection helpers and transforms
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<u32> verticesOfFaces(const EditMesh& m, const std::vector<u32>& faces) {
    std::vector<u32> v;
    for (const u32 f : faces) if (f < m.faces.size()) v.insert(v.end(), m.faces[f].v.begin(), m.faces[f].v.end());
    return uniqueSorted(std::move(v));
}
std::vector<u32> verticesOfEdges(const std::vector<Edge>& edges) {
    std::vector<u32> v;
    for (const Edge& e : edges) { v.push_back(e.first); v.push_back(e.second); }
    return uniqueSorted(std::move(v));
}
std::vector<u32> facesOfVertices(const EditMesh& m, const std::vector<u32>& verts) {
    const std::unordered_set<u32> s(verts.begin(), verts.end());
    std::vector<u32> out;
    for (u32 i = 0; i < m.faces.size(); ++i)
        for (const u32 v : m.faces[i].v) if (s.count(v)) { out.push_back(i); break; }
    return out;
}
std::vector<Edge> edgesOfFaces(const EditMesh& m, const std::vector<u32>& faces) {
    std::set<Edge> s;
    for (const u32 fi : faces) {
        if (fi >= m.faces.size()) continue;
        const Face& f = m.faces[fi];
        for (usize i = 0; i < f.v.size(); ++i) s.insert(makeEdge(f.v[i], f.v[(i + 1) % f.v.size()]));
    }
    return {s.begin(), s.end()};
}

void moveVertices(EditMesh& m, const std::vector<u32>& verts, const V3& d) {
    for (const u32 v : uniqueSorted(verts)) if (v < m.pos.size()) m.pos[v] += d;
}
void rotateVertices(EditMesh& m, const std::vector<u32>& verts, const V3& pivot, const V3& axis, double rad) {
    const V3 k = normalized(axis);
    if (length(k) == 0) return;
    const double c = std::cos(rad), s = std::sin(rad);
    for (const u32 v : uniqueSorted(verts)) {
        if (v >= m.pos.size()) continue;
        const V3 p = m.pos[v] - pivot;
        m.pos[v] = pivot + p * c + cross(k, p) * s + k * (dot(k, p) * (1 - c));   // Rodrigues
    }
}
void scaleVertices(EditMesh& m, const std::vector<u32>& verts, const V3& pivot, const V3& sc) {
    for (const u32 v : uniqueSorted(verts)) {
        if (v >= m.pos.size()) continue;
        const V3 p = m.pos[v] - pivot;
        m.pos[v] = pivot + V3{p.x * sc.x, p.y * sc.y, p.z * sc.z};
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Extrude / inset
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

OpResult regionExtrude(EditMesh& m, const std::vector<u32>& facesIn, double dist, double inset, double uvTile) {
    const std::vector<u32> faces = uniqueSorted(facesIn);
    if (faces.empty()) return fail("select at least one face");
    for (const u32 f : faces) if (f >= m.faces.size() || !faceOk(m, m.faces[f])) return fail("a selected face does not exist");
    EditMesh w = m;

    std::unordered_set<u64> sel;
    for (const u32 fi : faces) { const Face& f = w.faces[fi]; for (usize i = 0; i < f.v.size(); ++i) sel.insert(dkey(f.v[i], f.v[(i + 1) % f.v.size()])); }
    struct BEdge { u32 a, b, face; };
    std::vector<BEdge> boundary;
    for (const u32 fi : faces) {
        const Face& f = w.faces[fi];
        for (usize i = 0; i < f.v.size(); ++i) {
            const u32 a = f.v[i], b = f.v[(i + 1) % f.v.size()];
            if (!sel.count(dkey(b, a))) boundary.push_back({a, b, fi});
        }
    }
    V3 regionN;
    for (const u32 fi : faces) regionN += newellOf(w, w.faces[fi].v);
    regionN = normalized(regionN);
    if (length(regionN) == 0 && (dist != 0)) return fail("the selected faces face opposite ways; extrude them one region at a time");

    std::vector<UvMap> maps;
    std::vector<V3> oldN;
    for (const u32 fi : faces) { maps.push_back(uvMapOf(w, w.faces[fi])); oldN.push_back(normalized(newellOf(w, w.faces[fi].v))); }

    std::unordered_map<u32, u32> dup;
    std::unordered_map<u32, V3> inward;       // sum of inward in-plane edge normals
    std::unordered_map<u32, std::vector<V3>> inList;
    for (const BEdge& e : boundary) {
        if (!dup.count(e.a)) dup[e.a] = w.addVertex(w.pos[e.a]);
        if (!dup.count(e.b)) dup[e.b] = w.addVertex(w.pos[e.b]);
        const V3 n = normalized(newellOf(w, w.faces[e.face].v));
        const V3 in = cross(n, normalized(w.pos[e.b] - w.pos[e.a]));
        inList[e.a].push_back(in);
        inList[e.b].push_back(in);
    }
    const V3 delta = regionN * dist;
    const std::vector<u32> region = verticesOfFaces(w, faces);
    for (const u32 v : region) if (!dup.count(v)) w.pos[v] += delta;                    // interior vertices move in place
    for (const auto& [v, d] : dup) {
        V3 off;
        if (inset != 0) {
            const std::vector<V3>& l = inList[v];
            if (l.size() == 2) { const double den = 1.0 + dot(l[0], l[1]); if (den < 1e-3) return fail("the inset corner is too sharp"); off = (l[0] + l[1]) * (inset / den); }
            else { V3 s; for (const V3& x : l) s += x; off = normalized(s) * inset; }
        }
        w.pos[d] = w.pos[v] + delta + off;
    }
    // caps take the duplicates
    for (usize k = 0; k < faces.size(); ++k) {
        Face& f = w.faces[faces[k]];
        for (u32& v : f.v) { const auto it = dup.find(v); if (it != dup.end()) v = it->second; }
        if (inset != 0 && maps[k].ok) for (usize i = 0; i < f.v.size(); ++i) f.uv[i] = maps[k].at(w.pos[f.v[i]]);
    }
    // side quads: [a, b, b', a'] (the original ring keeps the neighbour's opposite edge)
    for (const BEdge& e : boundary) {
        const Face& owner = w.faces[e.face];
        const u32 q = w.addFace({e.a, e.b, dup[e.b], dup[e.a]}, owner.material, 0);
        Face& nf = w.faces[q];
        const usize ok = static_cast<usize>(std::find(faces.begin(), faces.end(), e.face) - faces.begin());
        if (std::fabs(dist) < 1e-12 && ok < maps.size() && maps[ok].ok) for (int c = 0; c < 4; ++c) nf.uv[c] = maps[ok].at(w.pos[nf.v[c]]);
        else planarUvFor(w, nf, uvTile);
    }
    if (inset != 0) {
        for (const BEdge& e : boundary)     // an inset that overshoots turns an edge around
            if (dot(w.pos[dup[e.b]] - w.pos[dup[e.a]], m.pos[e.b] - m.pos[e.a]) <= 0) return fail("the inset is wider than the face");
        for (usize k = 0; k < faces.size(); ++k)
            if (dot(normalized(newellOf(w, w.faces[faces[k]].v)), oldN[k]) <= 0.0) return fail("the inset is wider than the face");
    }
    OpResult r;
    r.ok = true;
    r.faces = faces;
    for (const auto& [v, d] : dup) r.verts.push_back(d);
    std::sort(r.verts.begin(), r.verts.end());
    m = std::move(w);
    return r;
}

}  // namespace

OpResult extrudeFaces(EditMesh& m, const std::vector<u32>& faces, double distance, double uvTile) {
    return regionExtrude(m, faces, distance, 0.0, uvTile);
}
OpResult insetFaces(EditMesh& m, const std::vector<u32>& faces, double amount) {
    if (amount < 0) return fail("the inset amount must not be negative");
    return regionExtrude(m, faces, 0.0, amount, 100.0);
}

OpResult extrudeEdges(EditMesh& m, const std::vector<Edge>& edges, const V3& delta, double uvTile) {
    if (edges.empty()) return fail("select at least one edge");
    std::unordered_map<u64, u32> dirCount;
    for (const Face& f : m.faces) if (faceOk(m, f)) for (usize i = 0; i < f.v.size(); ++i) ++dirCount[dkey(f.v[i], f.v[(i + 1) % f.v.size()])];
    EditMesh w = m;
    std::unordered_map<u32, u32> dup;
    OpResult r;
    std::vector<std::array<u32, 2>> todo;
    for (const Edge& e : edges) {
        const auto ab = dirCount.find(dkey(e.first, e.second)), ba = dirCount.find(dkey(e.second, e.first));
        const u32 nab = ab == dirCount.end() ? 0 : ab->second, nba = ba == dirCount.end() ? 0 : ba->second;
        if (nab + nba != 1) return fail("only edges that border a single face (an open boundary) can be extruded");
        todo.push_back(nab == 1 ? std::array<u32, 2>{e.first, e.second} : std::array<u32, 2>{e.second, e.first});
    }
    for (const auto& t : todo)
        for (const u32 v : t) if (!dup.count(v)) { dup[v] = w.addVertex(w.pos[v] + delta); r.verts.push_back(dup[v]); }
    for (const auto& t : todo) {
        // the face has a -> b, so the quad runs b -> a
        const u32 q = w.addFace({t[1], t[0], dup[t[0]], dup[t[1]]});
        planarUvFor(w, w.faces[q], uvTile);
        r.faces.push_back(q);
    }
    r.ok = true;
    m = std::move(w);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Bevel
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

OpResult bevelEdges(EditMesh& m, const std::vector<Edge>& edgesIn, double width, int segments) {
    if (edgesIn.empty()) return fail("select at least one edge");
    if (!(width > 0)) return fail("the bevel width must be positive");
    const int seg = std::clamp(segments, 1, 32);
    const Validation before = validate(m);
    if (!before.manifold()) return fail("bevel needs a manifold mesh: " + before.message);

    EditMesh w = m;
    const std::set<Edge> sel(edgesIn.begin(), edgesIn.end());
    struct Corner { u32 face, corner; };
    std::unordered_map<u64, Corner> dirMap;
    std::unordered_map<u32, std::vector<Corner>> vertFaces;
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        for (u32 c = 0; c < f.v.size(); ++c) { dirMap[dkey(f.v[c], f.v[(c + 1) % f.v.size()])] = {fi, c}; vertFaces[f.v[c]].push_back({fi, c}); }
    }
    for (const Edge& e : sel)
        if (!dirMap.count(dkey(e.first, e.second)) || !dirMap.count(dkey(e.second, e.first))) return fail("every beveled edge must be shared by two faces");

    std::vector<V3> faceN(m.faces.size());
    std::vector<UvMap> uvMaps(m.faces.size());
    for (u32 fi = 0; fi < m.faces.size(); ++fi) { faceN[fi] = normalized(newellOf(m, m.faces[fi].v)); uvMaps[fi] = uvMapOf(m, m.faces[fi]); }

    // replacement vertex lists per (face, corner), plus profile data
    std::map<std::pair<u32, u32>, std::vector<u32>> repl;           // (face, corner) -> vertices replacing the corner, in winding order
    std::map<std::pair<u32, u32>, std::vector<u32>> profile;        // (vertex, other end of the edge) -> [LF.., RF] ring-direction points, endpoints included
    std::map<Edge, std::array<std::vector<u32>, 2>> strip;          // edge -> {profile at first endpoint, at second}, each from LF to RF
    std::vector<Face> patches;

    const std::vector<u32> bevVerts = verticesOfEdges(std::vector<Edge>(sel.begin(), sel.end()));
    for (const u32 v : bevVerts) {
        // the ring of faces around v: face i's outgoing spoke goes to nb[i]
        const std::vector<Corner>& fc = vertFaces[v];
        std::vector<Corner> ring;
        std::vector<u32> nb;
        Corner cur = fc[0];
        for (usize guard = 0; guard <= fc.size(); ++guard) {
            const Face& f = m.faces[cur.face];
            const u32 n = f.v[(cur.corner + 1) % f.v.size()];
            ring.push_back(cur);
            nb.push_back(n);
            const auto it = dirMap.find(dkey(n, v));
            if (it == dirMap.end()) return fail("bevel needs a closed fan of faces around every beveled vertex");
            const Corner g = it->second;
            const Face& gf = m.faces[g.face];
            cur = {g.face, static_cast<u32>((g.corner + 1) % gf.v.size())};
            if (cur.face == ring[0].face) break;
        }
        const int M = static_cast<int>(ring.size());
        if (M != static_cast<int>(fc.size()) || M < 3) return fail("bevel needs a manifold fan of faces around every beveled vertex");
        std::vector<char> isSel(M);
        int s = 0;
        for (int i = 0; i < M; ++i) { isSel[i] = sel.count(makeEdge(v, nb[i])) ? 1 : 0; s += isSel[i]; }
        if (s != 1 && s != M) return fail("at a corner either one edge or all of its edges must be beveled");
        const auto md = [&](int i) { return ((i % M) + M) % M; };
        // rotate so that for s == 1 the selected spoke is the last one
        int shift = 0;
        if (s == 1) for (int i = 0; i < M; ++i) if (isSel[i]) shift = md(i + 1);
        const auto F = [&](int i) { return ring[md(i + shift)]; };
        const auto NB = [&](int i) { return nb[md(i + shift)]; };
        const auto SEL = [&](int i) { return isSel[md(i + shift)] != 0; };

        const V3 pv = m.pos[v];
        std::vector<u32> P(M, kNone);
        for (int i = 0; i < M; ++i) {
            const Corner c = F(i);
            const Face& f = m.faces[c.face];
            const V3 N = faceN[c.face];
            const V3 prev = m.pos[f.v[(c.corner + f.v.size() - 1) % f.v.size()]], next = m.pos[NB(i)];
            const V3 dIn = normalized(pv - prev), dOut = normalized(next - pv);
            const bool inSel = SEL(i - 1), outSel = SEL(i);
            if (!inSel && !outSel) continue;
            const V3 aIn = cross(N, dIn), aOut = cross(N, dOut);
            V3 x;
            if (inSel && outSel) {
                const double den = 1.0 + dot(aIn, aOut);
                if (den < 1e-3) return fail("a beveled corner is too sharp");
                x = pv + (aIn + aOut) * (width / den);
            } else if (inSel) {
                const double k = dot(aIn, dOut);
                if (k < 1e-4) return fail("a beveled corner is too sharp");
                const double t = width / k;
                if (t > 0.999 * length(next - pv)) return fail("the bevel width is larger than the neighbouring edge");
                x = pv + dOut * t;
            } else {
                const double k = -dot(aOut, dIn);
                if (k < 1e-4) return fail("a beveled corner is too sharp");
                const double t = width / k;
                if (t > 0.999 * length(pv - prev)) return fail("the bevel width is larger than the neighbouring edge");
                x = pv - dIn * t;
            }
            P[i] = w.addVertex(x);
        }
        // profile points on each selected spoke, from F(j)'s vertex to F(j+1)'s
        std::vector<std::vector<u32>> prof(M);
        for (int j = 0; j < M; ++j) {
            if (!SEL(j)) continue;
            const V3 a = w.pos[P[j]], b = w.pos[P[md(j + 1)]];
            prof[j].push_back(P[j]);
            for (int k = 1; k < seg; ++k) {
                const double t = double(k) / seg;
                prof[j].push_back(w.addVertex(a * ((1 - t) * (1 - t)) + pv * (2 * t * (1 - t)) + b * (t * t)));
            }
            prof[j].push_back(P[md(j + 1)]);
            // canonical LF -> RF order: LF holds a -> b with a < b, so at the lower endpoint the ring direction is already LF -> RF
            const u32 other = NB(j);
            std::vector<u32> q = prof[j];
            if (v > other) std::reverse(q.begin(), q.end());
            strip[makeEdge(v, other)][v < other ? 0 : 1] = std::move(q);
        }
        // corner replacement lists
        for (int i = 0; i < M; ++i) {
            const Corner c = F(i);
            std::vector<u32> list;
            const bool inSel = SEL(i - 1), outSel = SEL(i);
            if (inSel || outSel) list = {P[i]};
            else {
                const bool pre = SEL(i - 2), suf = SEL(i + 1);
                if (s == 1 && M == 3) {
                    // the one face left: v disappears and the profile closes it
                    const std::vector<u32>& pr = prof[M - 1];          // F(M-1)'s vertex -> F(0)'s, i.e. B .. A
                    list.push_back(P[i - 1 < 0 ? i - 1 + M : i - 1]);  // A (made by the face before)
                    for (usize k = pr.size() - 1; k-- > 1;) list.push_back(pr[k]);
                    list.push_back(P[md(i + 1)]);                      // B
                } else {
                    if (pre) list.push_back(P[md(i - 1)]);
                    list.push_back(v);
                    if (suf) list.push_back(P[md(i + 1)]);
                }
            }
            repl[{c.face, c.corner}] = std::move(list);
        }
        // corner patch
        if (s == M) {
            std::vector<u32> seq;
            for (int j = 0; j < M; ++j) { for (usize k = 0; k + 1 < prof[j].size(); ++k) seq.push_back(prof[j][k]); }
            std::reverse(seq.begin(), seq.end());
            Face p;
            p.v = std::move(seq);
            p.material = m.faces[F(0).face].material;
            patches.push_back(std::move(p));
        } else if (M > 3) {
            const std::vector<u32>& pr = prof[M - 1];
            Face p;
            p.v.push_back(P[0]);                                       // A
            for (usize k = pr.size() - 1; k-- > 1;) p.v.push_back(pr[k]);
            p.v.push_back(P[M - 1]);                                   // B
            p.v.push_back(v);
            p.material = m.faces[F(0).face].material;
            patches.push_back(std::move(p));
        }
    }

    // rebuild the faces
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const Face& old = m.faces[fi];
        Face nf;
        nf.material = old.material; nf.smoothing = old.smoothing;
        bool touched = false;
        for (u32 c = 0; c < old.v.size(); ++c) {
            const auto it = repl.find({fi, c});
            if (it == repl.end()) { nf.v.push_back(old.v[c]); nf.uv.push_back(old.uv[c]); continue; }
            touched = true;
            for (const u32 nv : it->second) {
                nf.v.push_back(nv);
                nf.uv.push_back(nv == old.v[c] ? old.uv[c] : (uvMaps[fi].ok ? uvMaps[fi].at(w.pos[nv]) : old.uv[c]));
            }
        }
        if (touched) w.faces[fi] = std::move(nf);
    }
    OpResult r;
    for (const Edge& e : sel) {
        const auto& st = strip[e];
        if (st[0].size() != st[1].size() || st[0].empty()) return fail("internal: bevel strip ends do not match");
        const Face& lf = m.faces[dirMap[dkey(e.first, e.second)].face];
        for (usize j = 0; j + 1 < st[0].size(); ++j) {
            const u32 q = w.addFace({st[1][j], st[0][j], st[0][j + 1], st[1][j + 1]}, lf.material, seg > 1 ? 1u : 0u);
            planarUvFor(w, w.faces[q], 100.0);
            r.faces.push_back(q);
        }
    }
    for (Face& p : patches) {
        p.uv.assign(p.v.size(), V2{});
        const u32 q = static_cast<u32>(w.faces.size());
        w.faces.push_back(std::move(p));
        planarUvFor(w, w.faces[q], 100.0);
        r.faces.push_back(q);
    }
    // sanity: still manifold, no new open edges, no flipped faces
    const Validation after = validate(w);
    if (!after.manifold() || after.openEdges != before.openEdges) return fail("the bevel is too wide for this mesh");
    for (u32 fi = 0; fi < m.faces.size(); ++fi)
        if (dot(normalized(newellOf(w, w.faces[fi].v)), faceN[fi]) < 0.2) return fail("the bevel is too wide for this mesh");
    r.ok = true;
    m = std::move(w);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Delete, merge, flip, subdivide, mirror
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
OpResult dropFaces(EditMesh& m, const std::unordered_set<u32>& drop) {
    if (drop.empty()) return fail("nothing selected");
    std::vector<Face> keep;
    for (u32 i = 0; i < m.faces.size(); ++i) if (!drop.count(i)) keep.push_back(std::move(m.faces[i]));
    m.faces = std::move(keep);
    OpResult r;
    r.ok = true;
    return r;
}
}  // namespace

OpResult deleteFaces(EditMesh& m, const std::vector<u32>& faces) {
    std::unordered_set<u32> d;
    for (const u32 f : faces) if (f < m.faces.size()) d.insert(f);
    return dropFaces(m, d);
}
OpResult deleteVertices(EditMesh& m, const std::vector<u32>& verts) {
    const std::vector<u32> f = facesOfVertices(m, verts);
    return dropFaces(m, std::unordered_set<u32>(f.begin(), f.end()));
}
OpResult deleteEdges(EditMesh& m, const std::vector<Edge>& edges) {
    const std::set<Edge> s(edges.begin(), edges.end());
    std::unordered_set<u32> d;
    for (u32 i = 0; i < m.faces.size(); ++i) {
        const Face& f = m.faces[i];
        for (usize c = 0; c < f.v.size(); ++c) if (s.count(makeEdge(f.v[c], f.v[(c + 1) % f.v.size()]))) { d.insert(i); break; }
    }
    return dropFaces(m, d);
}

OpResult mergeVertices(EditMesh& m, const std::vector<u32>& vertsIn) {
    const std::vector<u32> verts = uniqueSorted(vertsIn);
    if (verts.size() < 2) return fail("select at least two vertices to merge");
    V3 c;
    for (const u32 v : verts) { if (v >= m.pos.size()) return fail("a selected vertex does not exist"); c += m.pos[v]; }
    c = c * (1.0 / verts.size());
    const std::unordered_set<u32> s(verts.begin(), verts.end());
    m.pos[verts[0]] = c;
    for (Face& f : m.faces) for (u32& v : f.v) if (s.count(v)) v = verts[0];
    removeDegenerateFaces(m);
    OpResult r;
    r.ok = true;
    r.verts = {verts[0]};
    return r;
}

void flipFaces(EditMesh& m, const std::vector<u32>& faces) {
    const auto flip = [&](Face& f) { std::reverse(f.v.begin(), f.v.end()); std::reverse(f.uv.begin(), f.uv.end()); };
    if (faces.empty()) { for (Face& f : m.faces) flip(f); return; }
    for (const u32 i : uniqueSorted(faces)) if (i < m.faces.size()) flip(m.faces[i]);
}

OpResult subdivideFlat(EditMesh& m, const std::vector<u32>& facesIn) {
    std::unordered_set<u32> sel;
    if (facesIn.empty()) for (u32 i = 0; i < m.faces.size(); ++i) sel.insert(i);
    else for (const u32 f : facesIn) if (f < m.faces.size()) sel.insert(f);
    if (sel.empty()) return fail("nothing to subdivide");
    std::map<Edge, u32> mid;
    for (const u32 fi : sel) {
        const Face& f = m.faces[fi];
        if (!faceOk(m, f)) return fail("a face is malformed");
        for (usize i = 0; i < f.v.size(); ++i) {
            const Edge e = makeEdge(f.v[i], f.v[(i + 1) % f.v.size()]);
            if (!mid.count(e)) mid[e] = m.addVertex((m.pos[e.first] + m.pos[e.second]) * 0.5);
        }
    }
    OpResult r;
    std::vector<Face> out;
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        const usize n = f.v.size();
        if (!sel.count(fi)) {
            Face nf;
            nf.material = f.material; nf.smoothing = f.smoothing;
            for (usize i = 0; i < n; ++i) {
                nf.v.push_back(f.v[i]); nf.uv.push_back(f.uv[i]);
                const auto it = mid.find(makeEdge(f.v[i], f.v[(i + 1) % n]));
                if (it != mid.end()) { nf.v.push_back(it->second); nf.uv.push_back({(f.uv[i].u + f.uv[(i + 1) % n].u) / 2, (f.uv[i].v + f.uv[(i + 1) % n].v) / 2}); }
            }
            out.push_back(std::move(nf));
            continue;
        }
        V3 cen; V2 cuv;
        for (usize i = 0; i < n; ++i) { cen += m.pos[f.v[i]]; cuv.u += f.uv[i].u; cuv.v += f.uv[i].v; }
        cen = cen * (1.0 / n); cuv.u /= n; cuv.v /= n;
        const u32 cv = m.addVertex(cen);
        for (usize i = 0; i < n; ++i) {
            const usize p = (i + n - 1) % n, nx = (i + 1) % n;
            Face q;
            q.material = f.material; q.smoothing = f.smoothing;
            q.v = {f.v[i], mid[makeEdge(f.v[i], f.v[nx])], cv, mid[makeEdge(f.v[p], f.v[i])]};
            q.uv = {f.uv[i], {(f.uv[i].u + f.uv[nx].u) / 2, (f.uv[i].v + f.uv[nx].v) / 2}, cuv, {(f.uv[p].u + f.uv[i].u) / 2, (f.uv[p].v + f.uv[i].v) / 2}};
            r.faces.push_back(static_cast<u32>(out.size()));
            out.push_back(std::move(q));
        }
    }
    m.faces = std::move(out);
    r.ok = true;
    return r;
}

OpResult mirror(EditMesh& m, int axis, double at, double weldTol) {
    if (axis < 0 || axis > 2) return fail("axis must be 0, 1 or 2");
    if (m.faces.empty()) return fail("nothing to mirror");
    const u32 n0 = static_cast<u32>(m.pos.size());
    const usize f0 = m.faces.size();
    for (u32 i = 0; i < n0; ++i) { V3 p = m.pos[i]; p[axis] = 2 * at - p[axis]; m.addVertex(p); }
    for (usize i = 0; i < f0; ++i) {
        Face f = m.faces[i];
        for (u32& v : f.v) v += n0;
        std::reverse(f.v.begin(), f.v.end());
        std::reverse(f.uv.begin(), f.uv.end());
        m.faces.push_back(std::move(f));
    }
    weldVertices(m, weldTol > 0 ? weldTol : 1e-6);
    OpResult r;
    r.ok = true;
    for (usize i = f0; i < m.faces.size(); ++i) r.faces.push_back(static_cast<u32>(i));
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Boolean
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

bool boolean(const EditMesh& a, const EditMesh& b, csg::Op op, EditMesh& out, std::string* why) {
    const auto refuse = [&](const std::string& s) { if (why) *why = s; return false; };
    const auto solid = [&](const EditMesh& x, const char* name) -> std::string {
        if (x.faces.empty()) return std::string(name) + " has no faces";
        const Validation v = validate(x);
        if (!v.closed())
            return std::string(name) + " is not a closed solid (" + v.message + "); booleans need closed meshes, so fill holes or weld vertices first";
        if (volume(x) <= 0) return std::string(name) + " is inside out (its volume is not positive); flip its faces first";
        return {};
    };
    if (std::string s = solid(a, "The first mesh"); !s.empty()) return refuse(s);
    if (std::string s = solid(b, "The second mesh"); !s.empty()) return refuse(s);

    const auto nameOf = [](const EditMesh& m, int i) { return i >= 0 && i < static_cast<int>(m.materials.size()) && !m.materials[i].empty() ? m.materials[i] : "material_" + std::to_string(i); };
    EditMesh bb = b;
    std::vector<std::string> names = a.materials;
    int maxA = static_cast<int>(a.materials.size()) - 1;
    for (const Face& f : a.faces) maxA = std::max(maxA, f.material);
    for (int i = static_cast<int>(names.size()); i <= maxA; ++i) names.push_back(nameOf(a, i));
    for (Face& f : bb.faces) {
        const std::string n = nameOf(b, f.material);
        const auto it = std::find(names.begin(), names.end(), n);
        if (it == names.end()) { names.push_back(n); f.material = static_cast<int>(names.size() - 1); }
        else f.material = static_cast<int>(it - names.begin());
    }
    const csg::Mesh r = csg::combine(toCsg(a), toCsg(bb), op);
    EditMesh res = fromCsg(r);
    res.materials = names;
    if (res.faces.empty() && why) *why = "the result is empty";
    out = std::move(res);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// UV
// ---------------------------------------------------------------------------------------------------------------------------------------------------------

void projectUv(EditMesh& m, const std::vector<u32>& facesIn, UvProjection kind, const UvProjectionParams& p) {
    std::vector<u32> faces = facesIn;
    if (faces.empty()) for (u32 i = 0; i < m.faces.size(); ++i) faces.push_back(i);
    const double tile = p.tile > 0 ? p.tile : 100.0;
    const V3 axis = length(p.axis) > 0 ? normalized(p.axis) : V3{0, 0, 1};
    const V3 ref = std::fabs(axis.z) < 0.99 ? V3{0, 0, 1} : V3{0, 1, 0};
    const V3 U = normalized(cross(ref, axis)), V = cross(axis, U);
    const double rc = std::cos(p.rotationDegrees * kPi / 180), rs = std::sin(p.rotationDegrees * kPi / 180);
    const auto finish = [&](V2 uv) {
        uv.u *= p.scale.u; uv.v *= p.scale.v;
        return V2{uv.u * rc - uv.v * rs + p.offset.u, uv.u * rs + uv.v * rc + p.offset.v};
    };
    for (const u32 fi : faces) {
        if (fi >= m.faces.size()) continue;
        Face& f = m.faces[fi];
        if (!faceOk(m, f)) continue;
        f.uv.resize(f.v.size());
        const V3 n = faceNormal(m, fi);
        double prevU = 0;
        for (usize c = 0; c < f.v.size(); ++c) {
            const V3 q = m.pos[f.v[c]] - p.origin;
            V2 uv;
            switch (kind) {
            case UvProjection::Box: {
                const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
                const V3& pp = m.pos[f.v[c]];
                if (ax >= ay && ax >= az) uv = {(n.x >= 0 ? pp.y : -pp.y) / tile, pp.z / tile};
                else if (ay >= az) uv = {(n.y >= 0 ? -pp.x : pp.x) / tile, pp.z / tile};
                else uv = {(n.z >= 0 ? pp.x : -pp.x) / tile, pp.y / tile};
                break;
            }
            case UvProjection::Planar: uv = {dot(q, U) / tile, dot(q, V) / tile}; break;
            case UvProjection::Cylindrical:
            case UvProjection::Spherical: {
                double u = std::atan2(dot(q, V), dot(q, U)) / (2 * kPi);
                if (c > 0) { while (u - prevU > 0.5) u -= 1; while (u - prevU < -0.5) u += 1; }
                prevU = u;
                if (kind == UvProjection::Cylindrical) uv = {u, dot(q, axis) / tile};
                else { const double l = length(q); uv = {u, l > 0 ? 0.5 + std::asin(std::clamp(dot(q, axis) / l, -1.0, 1.0)) / kPi : 0.5}; }
                break;
            }
            }
            f.uv[c] = finish(uv);
        }
    }
}

void transformUv(EditMesh& m, const std::vector<u32>& facesIn, const V2& offset, double deg, const V2& scale) {
    std::vector<u32> faces = facesIn;
    if (faces.empty()) for (u32 i = 0; i < m.faces.size(); ++i) faces.push_back(i);
    const double rc = std::cos(deg * kPi / 180), rs = std::sin(deg * kPi / 180);
    for (const u32 fi : faces) {
        if (fi >= m.faces.size() || m.faces[fi].uv.empty()) continue;
        Face& f = m.faces[fi];
        V2 c;
        for (const V2& t : f.uv) { c.u += t.u; c.v += t.v; }
        c.u /= f.uv.size(); c.v /= f.uv.size();
        for (V2& t : f.uv) {
            const double du = (t.u - c.u) * scale.u, dv = (t.v - c.v) * scale.v;
            t = {c.u + du * rc - dv * rs + offset.u, c.v + du * rs + dv * rc + offset.v};
        }
    }
}

double texelDensity(const EditMesh& m, const std::vector<u32>& facesIn, double textureSize) {
    std::vector<u32> faces = facesIn;
    if (faces.empty()) for (u32 i = 0; i < m.faces.size(); ++i) faces.push_back(i);
    double uvArea = 0, worldArea = 0;
    for (const u32 fi : faces) {
        if (fi >= m.faces.size()) continue;
        const Face& f = m.faces[fi];
        if (f.uv.size() != f.v.size()) continue;
        double a = 0;
        for (usize i = 0; i < f.uv.size(); ++i) { const V2& p = f.uv[i]; const V2& q = f.uv[(i + 1) % f.uv.size()]; a += p.u * q.v - q.u * p.v; }
        uvArea += std::fabs(a) / 2;
        worldArea += faceArea(m, fi);
    }
    return worldArea > 0 ? std::sqrt(uvArea * textureSize * textureSize / worldArea) : 0.0;
}

}  // namespace aver::fmt::em
