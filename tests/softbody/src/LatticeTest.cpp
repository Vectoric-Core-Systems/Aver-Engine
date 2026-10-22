// The jelly lattice (Lattice.hpp): which cells are inside, the Kuhn tetrahedra, the conforming faces, the winding the physics
// ABI expects, snapping, the node cap, and the barycentric embedding of a render mesh. Pure arithmetic; no GPU, no scene, no Jolt.
//
// NOTHING IN HERE HAS BEEN RUN. The numbers (the volume tolerances for the sphere and the torus, the embedding error) are the
// design's starting values for the first headed pass; a failing threshold is a question about the number before it is a
// question about the code.
#include "aver/core/Log.hpp"
#include "aver/softbody/Lattice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

struct TriMesh {
    std::vector<f32> pos;
    std::vector<u32> idx;
    u32 vertexCount() const { return static_cast<u32>(pos.size() / 3); }
    u32 indexCount() const { return static_cast<u32>(idx.size()); }
    Vec3 at(u32 i) const { return Vec3{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}; }
};

static u32 addV(TriMesh& m, const Vec3& p) {
    m.pos.push_back(p.x); m.pos.push_back(p.y); m.pos.push_back(p.z);
    return m.vertexCount() - 1;
}

// |volume| of a closed triangle soup: sum of A . (B x C) / 6.
static f32 meshVolume(const TriMesh& m) {
    f64 six = 0.0;
    for (usize t = 0; t + 2 < m.idx.size(); t += 3) {
        const Vec3 a = m.at(m.idx[t]), b = m.at(m.idx[t + 1]), c = m.at(m.idx[t + 2]);
        six += static_cast<f64>(dot(a, cross(b, c)));
    }
    return static_cast<f32>(std::fabs(six) / 6.0);
}

// A box with a vertex per face corner (so every edge has a UV-seam style duplicate), cross(B - A, C - A) outward.
static TriMesh makeBox(const Vec3& half) {
    TriMesh m;
    struct Face { Vec3 n, u, v; };
    const Face faces[6] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
                           {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
    for (const Face& f : faces) {
        const Vec3 c{f.n.x * half.x, f.n.y * half.y, f.n.z * half.z};
        const Vec3 u{f.u.x * half.x, f.u.y * half.y, f.u.z * half.z}, v{f.v.x * half.x, f.v.y * half.y, f.v.z * half.z};
        const u32 a = addV(m, c - u - v), b = addV(m, c + u - v), cc = addV(m, c + u + v), d = addV(m, c - u + v);
        // Flip when the winding faces inward.
        const Vec3 nrm = cross(m.at(b) - m.at(a), m.at(cc) - m.at(a));
        if (dot(nrm, c) >= 0.0f) m.idx.insert(m.idx.end(), {a, b, cc, a, cc, d});
        else m.idx.insert(m.idx.end(), {a, cc, b, a, d, cc});
    }
    return m;
}

static TriMesh makeSphere(f32 radius, u32 rings, u32 segs) {
    TriMesh m;
    const u32 north = addV(m, Vec3{0, 0, radius});
    for (u32 r = 1; r < rings; ++r) {
        const f32 phi = kPi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s < segs; ++s) {
            const f32 th = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segs);
            addV(m, Vec3{radius * std::sin(phi) * std::cos(th), radius * std::sin(phi) * std::sin(th), radius * std::cos(phi)});
        }
    }
    const u32 south = addV(m, Vec3{0, 0, -radius});
    const auto ring = [&](u32 r, u32 s) { return 1 + (r - 1) * segs + (s % segs); };
    for (u32 s = 0; s < segs; ++s) m.idx.insert(m.idx.end(), {north, ring(1, s + 1), ring(1, s)});
    for (u32 r = 1; r + 1 < rings; ++r)
        for (u32 s = 0; s < segs; ++s) {
            m.idx.insert(m.idx.end(), {ring(r, s), ring(r, s + 1), ring(r + 1, s)});
            m.idx.insert(m.idx.end(), {ring(r, s + 1), ring(r + 1, s + 1), ring(r + 1, s)});
        }
    for (u32 s = 0; s < segs; ++s) m.idx.insert(m.idx.end(), {south, ring(rings - 1, s), ring(rings - 1, s + 1)});
    return m;
}

static TriMesh makeTorus(f32 major, f32 minor, u32 around, u32 tube) {
    TriMesh m;
    for (u32 i = 0; i < around; ++i)
        for (u32 j = 0; j < tube; ++j) {
            const f32 u = kTwoPi * static_cast<f32>(i) / static_cast<f32>(around), v = kTwoPi * static_cast<f32>(j) / static_cast<f32>(tube);
            addV(m, Vec3{(major + minor * std::cos(v)) * std::cos(u), (major + minor * std::cos(v)) * std::sin(u), minor * std::sin(v)});
        }
    const auto id = [&](u32 i, u32 j) { return (i % around) * tube + (j % tube); };
    for (u32 i = 0; i < around; ++i)
        for (u32 j = 0; j < tube; ++j) {
            m.idx.insert(m.idx.end(), {id(i, j), id(i + 1, j), id(i, j + 1)});
            m.idx.insert(m.idx.end(), {id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    return m;
}

static bool build(const TriMesh& m, const sb::LatticeSpec& spec, sb::Lattice& out) {
    return sb::buildLattice(m.pos.data(), m.vertexCount(), m.idx.data(), m.indexCount(), spec, out);
}

int main() {
    AVER_INFO("LatticeTest");

    AVER_INFO("closed meshes");
    {
        const TriMesh box = makeBox(Vec3{50, 50, 50});
        const TriMesh ball = makeSphere(50.0f, 12, 24);
        check(sb::meshIsClosed(box.pos.data(), box.vertexCount(), box.idx.data(), box.indexCount()), "a box with a vertex per face corner is closed once welded");
        check(sb::meshIsClosed(ball.pos.data(), ball.vertexCount(), ball.idx.data(), ball.indexCount()), "a UV sphere is closed");
        TriMesh open = ball;
        open.idx.resize(open.idx.size() - 3);
        check(!sb::meshIsClosed(open.pos.data(), open.vertexCount(), open.idx.data(), open.indexCount()), "a sphere missing a triangle is open");
        const f32 few[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const u32 one[3] = {0, 1, 2};
        check(!sb::meshIsClosed(few, 3, one, 3), "a single triangle is not a volume");
        check(!sb::meshIsClosed(nullptr, 0, nullptr, 0), "null input is refused");
    }

    AVER_INFO("a box fills with a conforming lattice");
    {
        const TriMesh box = makeBox(Vec3{50, 50, 50});
        sb::LatticeSpec spec;
        spec.cellCm = 20.0f;
        spec.maxNodes = 4096;
        sb::Lattice lat;
        const bool ok = build(box, spec, lat);
        check(ok && lat.error.empty(), "the box builds (" + lat.error + ")");
        if (ok) {
            check(lat.nodes.size() == 216, "a 5 x 5 x 5 cell lattice has 6 x 6 x 6 nodes");
            check(lat.tets.size() == 125 * 6 * 4, "six tetrahedra per cell");
            check(std::fabs(lat.restVolumeCm3 - 1.0e6f) < 1.0e3f, "the tetrahedra fill the box (1e6 cm^3)");
            check(std::fabs(lat.cellCm - 20.0f) < 1e-6f, "the cell is recorded");

            // Every tetrahedron has positive volume (the builder orients them).
            u32 negative = 0;
            for (usize t = 0; t < lat.tets.size(); t += 4)
                negative += sb::tetSignedVolume(lat.nodes[lat.tets[t]], lat.nodes[lat.tets[t + 1]], lat.nodes[lat.tets[t + 2]], lat.nodes[lat.tets[t + 3]]) <= 0.0f ? 1u : 0u;
            check(negative == 0, "no tetrahedron is inside out or flat");

            // Conforming: a triangle face is shared by at most two tetrahedra, and the ones shared once are the surface.
            std::map<std::array<u32, 3>, u32> faceUse;
            const int faceOf[4][3] = {{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}};
            for (usize t = 0; t < lat.tets.size(); t += 4)
                for (const auto& f : faceOf) {
                    std::array<u32, 3> key = {lat.tets[t + static_cast<usize>(f[0])], lat.tets[t + static_cast<usize>(f[1])], lat.tets[t + static_cast<usize>(f[2])]};
                    std::sort(key.begin(), key.end());
                    ++faceUse[key];
                }
            u32 once = 0, twice = 0, more = 0;
            for (const auto& kv : faceUse) { once += kv.second == 1; twice += kv.second == 2; more += kv.second > 2; }
            check(more == 0 && twice > 0, "every interior face is shared by exactly two tetrahedra");
            check(once == 6 * 25 * 2 && lat.surfaceFaces.size() / 3 == once, "the boundary is 300 triangles (six sides of 5 x 5 cells, two each)");

            // The winding the physics ABI wants: (C - A) x (B - A) points away from the middle of a convex lattice.
            Vec3 mid{0, 0, 0};
            for (const Vec3& n : lat.nodes) mid += n;
            mid = mid / static_cast<f32>(lat.nodes.size());
            u32 inward = 0;
            for (usize f = 0; f + 2 < lat.surfaceFaces.size(); f += 3) {
                const Vec3 a = lat.nodes[lat.surfaceFaces[f]], b = lat.nodes[lat.surfaceFaces[f + 1]], c = lat.nodes[lat.surfaceFaces[f + 2]];
                const Vec3 n = cross(c - a, b - a);
                inward += dot(n, (a + b + c) * (1.0f / 3.0f) - mid) <= 0.0f ? 1u : 0u;
            }
            check(inward == 0, "every surface face is wound outward for the physics ABI");

            // Edges: unique, ascending, in range.
            std::set<std::pair<u32, u32>> seen;
            bool inRange = true;
            for (usize e = 0; e + 1 < lat.edges.size(); e += 2) {
                inRange = inRange && lat.edges[e] < lat.edges[e + 1] && lat.edges[e + 1] < lat.nodes.size();
                seen.emplace(lat.edges[e], lat.edges[e + 1]);
            }
            check(inRange && seen.size() * 2 == lat.edges.size(), "the edges are unique pairs of valid nodes");
            check(lat.surfaceNodeCount == 216 - 64, "the surface nodes are the ones on the six sides (216 - 4 x 4 x 4 inside)");
        }
    }

    AVER_INFO("a sphere and a torus keep their volume");
    {
        const TriMesh ball = makeSphere(50.0f, 24, 48);
        sb::LatticeSpec spec;
        spec.cellCm = 8.0f;
        spec.snapCm = 3.0f;
        spec.maxNodes = 8000;
        sb::Lattice lat;
        const bool ok = build(ball, spec, lat);
        check(ok, "the sphere builds (" + lat.error + ")");
        if (ok) {
            const f32 ref = meshVolume(ball);
            const f32 ratio = lat.restVolumeCm3 / ref;
            AVER_INFO("  sphere: mesh volume {:.0f}, lattice {:.0f}, ratio {:.3f}, {} nodes, {} tetrahedra", ref, lat.restVolumeCm3, ratio, lat.nodes.size(), lat.tets.size() / 4);
            check(std::fabs(ratio - 1.0f) < 0.08f, "the tetrahedra's volume is within 8% of the sphere's");
            // Snapped nodes lie on the sphere (within the snap), so the lattice hugs it.
            f32 worstOut = 0.0f;
            for (usize f = 0; f < lat.surfaceFaces.size(); ++f) worstOut = std::max(worstOut, lat.nodes[lat.surfaceFaces[f]].size() - 50.0f);
            check(worstOut < 7.5f, "no boundary node is more than a cell diagonal outside the sphere");
        }
        const TriMesh ring = makeTorus(40.0f, 15.0f, 48, 20);
        sb::LatticeSpec ts;
        ts.cellCm = 6.0f;
        ts.snapCm = 2.0f;
        ts.maxNodes = 12000;
        sb::Lattice tl;
        const bool tok = build(ring, ts, tl);
        check(tok, "the torus builds (" + tl.error + ")");
        if (tok) {
            const f32 ratio = tl.restVolumeCm3 / meshVolume(ring);
            AVER_INFO("  torus: ratio {:.3f}, {} nodes", ratio, tl.nodes.size());
            check(std::fabs(ratio - 1.0f) < 0.10f, "and the torus's within 10%");
        }
    }

    AVER_INFO("embedding");
    {
        const TriMesh ball = makeSphere(50.0f, 16, 32);
        sb::LatticeSpec spec;
        spec.cellCm = 10.0f;
        spec.snapCm = 3.0f;
        spec.maxNodes = 8000;
        sb::Lattice lat;
        check(build(ball, spec, lat), "the sphere builds");
        sb::Embedding emb;
        sb::embedMesh(lat, ball.pos.data(), ball.vertexCount(), emb);
        check(emb.tet.size() == ball.vertexCount() && emb.bary.size() == ball.vertexCount() * 4, "one tetrahedron and four weights per render vertex");
        f32 worstSum = 0.0f, worstWeight = 0.0f;
        for (u32 v = 0; v < ball.vertexCount(); ++v) {
            f32 sum = 0.0f;
            for (u32 c = 0; c < 4; ++c) {
                const f32 w = emb.bary[v * 4 + c];
                sum += w;
                worstWeight = std::max(worstWeight, std::fabs(w));
            }
            worstSum = std::max(worstSum, std::fabs(sum - 1.0f));
        }
        check(worstSum < 1e-5f, "the weights of every vertex sum to 1 (within 1e-5)");
        check(worstWeight <= 2.0f + 1e-4f, "no weight is larger than 2 in magnitude");

        std::vector<Vec3> nodes = lat.nodes, out(ball.vertexCount());
        sb::embeddedPositions(lat, emb, nodes.data(), ball.vertexCount(), out.data());
        u32 off = 0;
        for (u32 v = 0; v < ball.vertexCount(); ++v) off += dist(out[v], ball.at(v)) > 0.05f ? 1u : 0u;
        AVER_INFO("  {} of {} vertices were pulled back onto their tetrahedron", off, ball.vertexCount());
        check(off * 10 <= ball.vertexCount(), "at rest the embedding reproduces the mesh (all but a tenth of the vertices exactly)");

        for (Vec3& n : nodes) n += Vec3{100, -30, 7};
        sb::embeddedPositions(lat, emb, nodes.data(), ball.vertexCount(), out.data());
        f32 worstShift = 0.0f;
        for (u32 v = 0; v < ball.vertexCount(); ++v) worstShift = std::max(worstShift, dist(out[v], ball.at(v) + Vec3{100, -30, 7}));
        check(worstShift < 0.05f, "moving every node moves every render vertex with it");
    }

    AVER_INFO("refusals");
    {
        const TriMesh ball = makeSphere(50.0f, 12, 24);
        TriMesh open = ball;
        open.idx.resize(open.idx.size() - 3);
        sb::LatticeSpec spec;
        spec.cellCm = 10.0f;
        sb::Lattice lat;
        check(!build(open, spec, lat) && lat.error.find("not closed") != std::string::npos, "an open mesh is refused with a reason");

        const TriMesh box = makeBox(Vec3{50, 50, 50});
        sb::LatticeSpec capped;
        capped.cellCm = 20.0f;
        capped.maxNodes = 50;
        check(!build(box, capped, lat) && lat.overCap, "a lattice over the node cap is refused and says so");

        sb::LatticeSpec tiny;
        tiny.cellCm = 0.01f;
        check(!build(box, tiny, lat) && lat.overCap, "a cell far smaller than the mesh is refused as over the limit");

        sb::LatticeSpec zero;
        zero.cellCm = 0.0f;
        check(!build(box, zero, lat) && !lat.error.empty(), "a zero cell is refused");

        const u32 bad[12] = {0, 1, 2, 0, 2, 3, 0, 3, 99, 0, 1, 3};
        check(!sb::buildLattice(box.pos.data(), box.vertexCount(), bad, 12, spec, lat), "an index out of range is refused");
    }

    AVER_INFO("determinism");
    {
        const TriMesh ball = makeSphere(50.0f, 16, 32);
        sb::LatticeSpec spec;
        spec.cellCm = 12.0f;
        spec.snapCm = 3.0f;
        sb::Lattice a, b;
        check(build(ball, spec, a) && build(ball, spec, b), "the sphere builds twice");
        check(a.tets == b.tets && a.surfaceFaces == b.surfaceFaces && a.edges == b.edges && a.nodes.size() == b.nodes.size(), "the topology is identical");
        bool same = true;
        for (usize i = 0; i < a.nodes.size(); ++i) same = same && a.nodes[i].x == b.nodes[i].x && a.nodes[i].y == b.nodes[i].y && a.nodes[i].z == b.nodes[i].z;
        check(same, "and so is every node, bit for bit");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
