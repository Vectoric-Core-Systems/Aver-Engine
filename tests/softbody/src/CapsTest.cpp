// Thick breaks: boundary walls, the back skin, generated UVs, winding, the vertex limit and the per-frame normals.
// Pure arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/Caps.hpp"
#include "aver/softbody/Cage.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static std::vector<u32> identityWeld(u32 n) {
    std::vector<u32> w(n);
    for (u32 i = 0; i < n; ++i) w[i] = i;
    return w;
}

// The cap vertices placed with the given normals and thickness.
static std::vector<Vec3> placed(const std::vector<sb::CapVertex>& verts, const std::vector<Vec3>& pos, const std::vector<Vec3>& nrm, f32 t) {
    std::vector<Vec3> out(verts.size());
    sb::capPositions(verts, pos.data(), nrm.data(), t, out.data());
    return out;
}

int main() {
    AVER_INFO("CapsTest");

    // A flat strip: one quad of two triangles in the XY plane, front normal +Z.
    const std::vector<Vec3> stripPos = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    const std::vector<u32> stripIdx = {0, 1, 2, 0, 2, 3};
    const std::vector<f32> stripUv = {0, 0, 1, 0, 1, 1, 0, 1};
    const std::vector<u32> stripWeld = identityWeld(4);

    AVER_INFO("a flat two-triangle strip");
    {
        sb::CapSpec spec;
        spec.thicknessCm = 2.0f;
        spec.uvCmPerTile = 20.0f;
        const sb::CapGeometry g = sb::buildCaps(stripIdx.data(), 6, stripWeld.data(), stripPos.data(), stripUv.data(), 4, spec);
        check(g.boundaryEdges == 4, "4 boundary edges (the diagonal is shared)");
        check(g.wallVerts.size() == 16 && g.wallIndices.size() == 24, "4 wall quads: 16 vertices and 24 indices");
        check(g.backVerts.size() == 4 && g.backIndices.size() == 6, "the back skin has one vertex per used vertex and the 2 triangles");
        check(!g.overLimit && !g.empty(), "not over the limit");

        std::vector<Vec3> nrm(4, Vec3{0, 0, 1});
        const std::vector<Vec3> wallPos = placed(g.wallVerts, stripPos, nrm, 2.0f);
        bool outward = true, valid = true;
        for (usize t = 0; t < g.wallIndices.size() / 3; ++t) {
            const Vec3& a = wallPos[g.wallIndices[t * 3]];
            const Vec3& b = wallPos[g.wallIndices[t * 3 + 1]];
            const Vec3& c = wallPos[g.wallIndices[t * 3 + 2]];
            const Vec3 n = cross(b - a, c - a);
            const Vec3 centre = (a + b + c) / 3.0f;
            outward = outward && dot(n, Vec3{centre.x - 5.0f, centre.y - 5.0f, 0.0f}) > 0.0f;
            valid = valid && g.wallIndices[t * 3] < wallPos.size() && g.wallIndices[t * 3 + 1] < wallPos.size() && g.wallIndices[t * 3 + 2] < wallPos.size();
        }
        check(outward, "every wall triangle faces away from the strip");
        check(valid, "wall indices are local to the wall vertices");

        bool uvOk = true;
        for (usize q = 0; q < g.wallVerts.size() / 4; ++q) {
            const sb::CapVertex* v = &g.wallVerts[q * 4];
            uvOk = uvOk && approx(v[1].u - v[0].u, 10.0f / 20.0f) && approx(v[0].v, 0.0f) && approx(v[2].v, 2.0f / 20.0f) && approx(v[3].v, 2.0f / 20.0f);
            uvOk = uvOk && v[0].u >= 0.0f && v[0].u < 1.0f && approx(v[0].u, v[2].u) && approx(v[1].u, v[3].u);
        }
        check(uvOk, "wall u spans edge length over the tile size, v spans thickness over it");
        std::set<int> distinct;
        for (usize q = 0; q < g.wallVerts.size() / 4; ++q) distinct.insert(static_cast<int>(g.wallVerts[q * 4].u * 1000.0f));
        check(distinct.size() >= 3, "neighbouring walls start at different places of the tile");

        bool back = true;
        for (usize t = 0; t < 2; ++t) {
            const u32 a = stripIdx[t * 3], b = stripIdx[t * 3 + 1], c = stripIdx[t * 3 + 2];
            back = back && g.backVerts[g.backIndices[t * 3]].src == a && g.backVerts[g.backIndices[t * 3 + 1]].src == c && g.backVerts[g.backIndices[t * 3 + 2]].src == b;
        }
        check(back, "the back skin reverses the winding");
        bool backUv = true;
        for (const sb::CapVertex& v : g.backVerts)
            backUv = backUv && v.depth == 1.0f && approx(v.u, stripUv[v.src * 2]) && approx(v.v, stripUv[v.src * 2 + 1]);
        check(backUv, "and keeps the front UV at depth 1");

        std::vector<Vec3> bn(4, Vec3{0, 0, 1});
        const std::vector<Vec3> backPos = placed(g.backVerts, stripPos, bn, 2.0f);
        bool depth = true;
        for (usize i = 0; i < backPos.size(); ++i) depth = depth && approx(backPos[i].z, -2.0f) && approx(backPos[i].x, stripPos[g.backVerts[i].src].x);
        check(depth, "capPositions offsets along -normal by exactly depth x thickness");
    }

    AVER_INFO("zero thickness, no back skin, and the vertex limit");
    {
        sb::CapSpec spec;
        spec.thicknessCm = 0.0f;
        sb::CapGeometry g = sb::buildCaps(stripIdx.data(), 6, stripWeld.data(), stripPos.data(), stripUv.data(), 4, spec);
        check(g.empty() && g.boundaryEdges == 0, "zero thickness builds nothing");
        spec.thicknessCm = 1.0f;
        spec.backSkin = false;
        g = sb::buildCaps(stripIdx.data(), 6, stripWeld.data(), stripPos.data(), stripUv.data(), 4, spec);
        check(g.backVerts.empty() && g.wallVerts.size() == 16, "backSkin off keeps the walls only");
        spec.backSkin = true;
        spec.vertexLimit = 8;
        g = sb::buildCaps(stripIdx.data(), 6, stripWeld.data(), stripPos.data(), stripUv.data(), 4, spec);
        check(g.overLimit && g.empty(), "over the vertex limit the piece draws uncapped");
    }

    AVER_INFO("builds are bit identical");
    {
        sb::CapSpec spec;
        spec.thicknessCm = 1.0f;
        const sb::CapGeometry a = sb::buildCaps(stripIdx.data(), 6, stripWeld.data(), stripPos.data(), stripUv.data(), 4, spec);
        const sb::CapGeometry b = sb::buildCaps(stripIdx.data(), 6, stripWeld.data(), stripPos.data(), stripUv.data(), 4, spec);
        check(a.wallVerts.size() == b.wallVerts.size() && std::memcmp(a.wallVerts.data(), b.wallVerts.data(), a.wallVerts.size() * sizeof(sb::CapVertex)) == 0 &&
              a.wallIndices == b.wallIndices && a.backIndices == b.backIndices, "two builds agree bit for bit");
    }

    AVER_INFO("a closed cube has no walls");
    {
        const std::vector<Vec3> pos = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}, {0, 0, 10}, {10, 0, 10}, {10, 10, 10}, {0, 10, 10}};
        const std::vector<u32> idx = {0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 1, 5, 0, 5, 4,  1, 2, 6, 1, 6, 5,  2, 3, 7, 2, 7, 6,  3, 0, 4, 3, 4, 7};
        const std::vector<u32> weld = identityWeld(8);
        sb::CapSpec spec;
        spec.thicknessCm = 1.0f;
        const sb::CapGeometry g = sb::buildCaps(idx.data(), static_cast<u32>(idx.size()), weld.data(), pos.data(), nullptr, 8, spec);
        check(g.boundaryEdges == 0 && g.wallVerts.empty(), "no boundary edges, no wall quads");
        check(g.backIndices.size() == idx.size(), "the back skin still mirrors the 12 triangles");
        check(!sb::hasBoundaryEdges(idx.data(), static_cast<u32>(idx.size()), weld.data()), "hasBoundaryEdges is false for a closed mesh");
        check(sb::hasBoundaryEdges(stripIdx.data(), 6, stripWeld.data()), "and true for the sheet");

        std::vector<Vec3> nrm(8);
        sb::smoothNormals(pos.data(), 8, idx.data(), static_cast<u32>(idx.size()), weld.data(), nrm.data());
        bool unit = true;
        for (const Vec3& n : nrm) unit = unit && approx(n.size(), 1.0f, 1e-4f);
        check(unit, "smooth normals are unit length");
    }

    AVER_INFO("a UV seam is not a boundary");
    {
        // Two quads side by side; the right quad uses duplicate vertices 6 and 7 for the shared edge (1, 4).
        const std::vector<Vec3> pos = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {0, 10, 0}, {10, 10, 0}, {20, 10, 0}, {10, 0, 0}, {10, 10, 0}};
        const std::vector<u32> idx = {0, 1, 4, 0, 4, 3,  6, 2, 5, 6, 5, 7};
        const std::vector<u32> weld = {0, 1, 2, 3, 4, 5, 1, 4};
        sb::CapSpec spec;
        spec.thicknessCm = 1.0f;
        const sb::CapGeometry g = sb::buildCaps(idx.data(), 12, weld.data(), pos.data(), nullptr, 8, spec);
        check(g.boundaryEdges == 6, "only the perimeter of the two quads is a boundary");
        std::vector<Vec3> nrm(8);
        sb::smoothNormals(pos.data(), 8, idx.data(), 12, weld.data(), nrm.data());
        check(approx(nrm[1].x, nrm[6].x) && approx(nrm[1].y, nrm[6].y) && approx(nrm[1].z, nrm[6].z) &&
              approx(nrm[4].z, nrm[7].z), "smooth normals are continuous across the seam");
    }

    AVER_INFO("a crack gives walls on both sides");
    {
        sb::Cage c;
        sb::GridSpec gs;
        gs.cols = 5; gs.rows = 3;
        gs.du = Vec3{10, 0, 0}; gs.dv = Vec3{0, 10, 0};
        gs.material = sb::addMaterial(c, sb::Material{});
        const std::vector<u32> at = sb::addGrid(c, gs);
        sb::build(c);

        auto draw = [&](std::vector<u32>& idx) {
            idx.clear();
            for (u32 t = 0; t < c.triangles.size(); ++t) {
                if (c.triDead[t]) continue;
                for (u32 k = 0; k < 3; ++k) idx.push_back(c.triangles[t].v[k]);
            }
        };
        std::vector<u32> idx;
        draw(idx);
        std::vector<u32> weld = identityWeld(static_cast<u32>(c.particles.size()));
        sb::CapSpec spec;
        spec.thicknessCm = 1.0f;
        std::vector<Vec3> rest;
        for (const sb::Particle& p : c.particles) rest.push_back(p.pos);
        const sb::CapGeometry before = sb::buildCaps(idx.data(), static_cast<u32>(idx.size()), weld.data(), rest.data(), nullptr, static_cast<u32>(rest.size()), spec);
        check(before.boundaryEdges == 12, "the intact 4 x 2 sheet has its 12 perimeter edges");

        // Cut the two vertical edges at x = 20.
        for (u32 bi = 0; bi < c.beams.size(); ++bi) {
            const sb::Beam& b = c.beams[bi];
            const Vec3& pa = c.particles[b.a].pos;
            const Vec3& pb = c.particles[b.b].pos;
            if (std::fabs(pa.x - 20.0f) < 1e-3f && std::fabs(pb.x - 20.0f) < 1e-3f) sb::breakBeam(c, bi);
        }
        const u32 created = sb::resolveTears(c);
        check(created == 3, "the crack duplicates the three particles on it");
        draw(idx);
        weld = identityWeld(static_cast<u32>(c.particles.size()));
        rest.clear();
        for (const sb::Particle& p : c.particles) rest.push_back(p.pos);
        const sb::CapGeometry after = sb::buildCaps(idx.data(), static_cast<u32>(idx.size()), weld.data(), rest.data(), nullptr, static_cast<u32>(rest.size()), spec);
        check(after.boundaryEdges == 16, "each side of the crack gains its own 2 boundary edges");
        u32 onCrack = 0;
        for (usize q = 0; q < after.wallVerts.size() / 4; ++q) {
            const sb::CapVertex* v = &after.wallVerts[q * 4];
            if (std::fabs(rest[v[0].src].x - 20.0f) < 1e-3f && std::fabs(rest[v[1].src].x - 20.0f) < 1e-3f) ++onCrack;
        }
        check(onCrack == 4, "four of the walls stand on the crack, two per side");
        (void)at;
    }

    AVER_INFO("backSkin policy");
    {
        check(sb::backSkinFor(sb::BackSkin::Auto, false, true), "auto: an open sheet gets it on the root");
        check(!sb::backSkinFor(sb::BackSkin::Auto, false, false) && sb::backSkinFor(sb::BackSkin::Auto, true, false), "auto: a closed mesh only on debris");
        check(!sb::backSkinFor(sb::BackSkin::Off, true, true) && sb::backSkinFor(sb::BackSkin::All, false, false) &&
              sb::backSkinFor(sb::BackSkin::Debris, true, false) && !sb::backSkinFor(sb::BackSkin::Debris, false, true), "off, all and debris");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
