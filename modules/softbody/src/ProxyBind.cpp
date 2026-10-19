// Closest-point barycentric binding of a render mesh to a proxy cage. See ProxyBind.hpp.
#include "aver/softbody/ProxyBind.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aver::softbody {
namespace {

struct RestTri {
    Vec3 a, b, c;
    bool ok = false;
};

// Ericson, Real-Time Collision Detection 5.1.5.
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

// Barycentric coordinates of p projected onto the plane of (a, b, c); false for a degenerate triangle.
bool planeBary(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, f32 out[3], Vec3* projected = nullptr) {
    const Vec3 n = cross(b - a, c - a);
    const f32 nn = dot(n, n);
    if (!(nn > 1e-20f)) return false;
    const Vec3 q = p - n * (dot(p - a, n) / nn);
    out[0] = dot(cross(c - b, q - b), n) / nn;
    out[1] = dot(cross(a - c, q - c), n) / nn;
    out[2] = 1.0f - out[0] - out[1];
    if (projected) *projected = q;
    return true;
}

Vec3 unitOr(const Vec3& v, const Vec3& fallback) {
    const Vec3 n = v.getSafeNormal();
    return n.sizeSquared() > 0.5f ? n : fallback;
}

struct Grid {
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
    f32 cell = 1.0f;
    i32 nx = 1, ny = 1, nz = 1;
    std::vector<u32> start, list;

    i32 clampI(i32 v, i32 n) const { return std::clamp(v, 0, n - 1); }
    u32 index(i32 x, i32 y, i32 z) const { return static_cast<u32>((z * ny + y) * nx + x); }
    bool contains(const Vec3& p) const {
        return p.x >= lo.x && p.y >= lo.y && p.z >= lo.z && p.x <= hi.x && p.y <= hi.y && p.z <= hi.z;
    }
};

struct Nearest {
    u32 tri = kNone;
    f32 dist2 = 1e30f;
    Vec3 point{0, 0, 0};
};

Nearest queryGrid(const Grid& g, const std::vector<RestTri>& tris, const Vec3& p) {
    Nearest best;
    if (!g.contains(p)) return best;
    const i32 cx = g.clampI(static_cast<i32>(std::floor((p.x - g.lo.x) / g.cell)), g.nx);
    const i32 cy = g.clampI(static_cast<i32>(std::floor((p.y - g.lo.y) / g.cell)), g.ny);
    const i32 cz = g.clampI(static_cast<i32>(std::floor((p.z - g.lo.z) / g.cell)), g.nz);
    const i32 maxK = std::max({g.nx, g.ny, g.nz});
    for (i32 k = 0; k <= maxK; ++k) {
        for (i32 z = std::max(cz - k, 0); z <= std::min(cz + k, g.nz - 1); ++z)
            for (i32 y = std::max(cy - k, 0); y <= std::min(cy + k, g.ny - 1); ++y)
                for (i32 x = std::max(cx - k, 0); x <= std::min(cx + k, g.nx - 1); ++x) {
                    if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != k) continue;
                    const u32 ci = g.index(x, y, z);
                    for (u32 s = g.start[ci]; s < g.start[ci + 1]; ++s) {
                        const u32 t = g.list[s];
                        const RestTri& rt = tris[t];
                        const Vec3 q = closestOnTriangle(p, rt.a, rt.b, rt.c);
                        const f32 d2 = distSquared(p, q);
                        // Ties go to the lower triangle id so the bind does not depend on cell order.
                        if (d2 < best.dist2 || (d2 == best.dist2 && t < best.tri)) { best.dist2 = d2; best.tri = t; best.point = q; }
                    }
                }
        if (best.tri != kNone && std::sqrt(best.dist2) <= static_cast<f32>(k) * g.cell) break;
    }
    return best;
}

} // namespace

void smoothParticleNormals(const Cage& cage, std::vector<Vec3>& out) {
    out.assign(cage.particles.size(), Vec3{0, 0, 0});
    for (u32 t = 0; t < cage.triangles.size(); ++t) {
        if (t < cage.triDead.size() && cage.triDead[t]) continue;
        const Triangle& tri = cage.triangles[t];
        const Vec3 face = cross(cage.particles[tri.v[1]].pos - cage.particles[tri.v[0]].pos, cage.particles[tri.v[2]].pos - cage.particles[tri.v[0]].pos);
        for (u32 k = 0; k < 3; ++k) out[tri.v[k]] += face;
    }
    for (Vec3& n : out) n = n.getSafeNormal();
}

bool bindRenderToProxy(const SimMeshInput& render, const Cage& proxyCage, const std::vector<Vec3>& bindLocal, f32 maxDistanceCm,
                       ProxyBind& out, std::string* why) {
    const auto fail = [&](const std::string& msg) {
        if (why) *why = msg;
        return false;
    };
    out = ProxyBind{};
    if (!render.positions || render.vertexCount == 0 || !render.indices || render.indexCount < 3) return fail("the render mesh has no triangles");
    if (bindLocal.size() < proxyCage.baseParticleCount || proxyCage.triangles.empty()) return fail("the proxy cage has no triangles");

    // The proxy at rest in mesh-local space.
    std::vector<RestTri> tris(proxyCage.triangles.size());
    std::vector<Vec3> smooth(proxyCage.baseParticleCount, Vec3{0, 0, 0});
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    f32 edgeSum = 0.0f;
    u32 valid = 0;
    for (u32 t = 0; t < tris.size(); ++t) {
        const Triangle& tri = proxyCage.triangles[t];
        RestTri& rt = tris[t];
        rt.a = bindLocal[tri.v0[0]]; rt.b = bindLocal[tri.v0[1]]; rt.c = bindLocal[tri.v0[2]];
        const Vec3 face = cross(rt.b - rt.a, rt.c - rt.a);
        rt.ok = dot(face, face) > 1e-20f;
        if (!rt.ok) continue;
        ++valid;
        for (u32 k = 0; k < 3; ++k) smooth[tri.v0[k]] += face;
        for (const Vec3* v : {&rt.a, &rt.b, &rt.c}) {
            lo = {std::min(lo.x, v->x), std::min(lo.y, v->y), std::min(lo.z, v->z)};
            hi = {std::max(hi.x, v->x), std::max(hi.y, v->y), std::max(hi.z, v->z)};
        }
        edgeSum += std::max({dist(rt.a, rt.b), dist(rt.b, rt.c), dist(rt.c, rt.a)});
    }
    if (valid == 0) return fail("the proxy cage has no usable triangles");
    for (Vec3& n : smooth) n = n.getSafeNormal();

    // The render mesh's diagonal gives the default bound.
    Vec3 rlo{1e30f, 1e30f, 1e30f}, rhi{-1e30f, -1e30f, -1e30f};
    for (u32 v = 0; v < render.vertexCount; ++v) {
        const f32* p = render.positions + static_cast<usize>(v) * 3;
        rlo = {std::min(rlo.x, p[0]), std::min(rlo.y, p[1]), std::min(rlo.z, p[2])};
        rhi = {std::max(rhi.x, p[0]), std::max(rhi.y, p[1]), std::max(rhi.z, p[2])};
    }
    const f32 limit = maxDistanceCm > 0.0f ? maxDistanceCm : std::max(0.05f * dist(rlo, rhi), 0.01f);
    out.maxDistanceCm = limit;

    // A uniform grid over the proxy triangles, padded by the bound so a point outside it fails at once.
    Grid g;
    g.lo = lo - Vec3{limit, limit, limit};
    g.hi = hi + Vec3{limit, limit, limit};
    const Vec3 span = g.hi - g.lo;
    g.cell = std::max({1.5f * edgeSum / static_cast<f32>(valid), std::max({span.x, span.y, span.z}) / 64.0f, 1e-3f});
    g.nx = std::max(1, static_cast<i32>(std::ceil(span.x / g.cell)));
    g.ny = std::max(1, static_cast<i32>(std::ceil(span.y / g.cell)));
    g.nz = std::max(1, static_cast<i32>(std::ceil(span.z / g.cell)));
    const usize cells = static_cast<usize>(g.nx) * g.ny * g.nz;
    g.start.assign(cells + 1, 0);
    const auto cellRange = [&](const RestTri& rt, i32& x0, i32& y0, i32& z0, i32& x1, i32& y1, i32& z1) {
        const Vec3 tl{std::min({rt.a.x, rt.b.x, rt.c.x}), std::min({rt.a.y, rt.b.y, rt.c.y}), std::min({rt.a.z, rt.b.z, rt.c.z})};
        const Vec3 th{std::max({rt.a.x, rt.b.x, rt.c.x}), std::max({rt.a.y, rt.b.y, rt.c.y}), std::max({rt.a.z, rt.b.z, rt.c.z})};
        x0 = g.clampI(static_cast<i32>(std::floor((tl.x - g.lo.x) / g.cell)), g.nx); x1 = g.clampI(static_cast<i32>(std::floor((th.x - g.lo.x) / g.cell)), g.nx);
        y0 = g.clampI(static_cast<i32>(std::floor((tl.y - g.lo.y) / g.cell)), g.ny); y1 = g.clampI(static_cast<i32>(std::floor((th.y - g.lo.y) / g.cell)), g.ny);
        z0 = g.clampI(static_cast<i32>(std::floor((tl.z - g.lo.z) / g.cell)), g.nz); z1 = g.clampI(static_cast<i32>(std::floor((th.z - g.lo.z) / g.cell)), g.nz);
    };
    for (u32 t = 0; t < tris.size(); ++t) {
        if (!tris[t].ok) continue;
        i32 x0, y0, z0, x1, y1, z1;
        cellRange(tris[t], x0, y0, z0, x1, y1, z1);
        for (i32 z = z0; z <= z1; ++z)
            for (i32 y = y0; y <= y1; ++y)
                for (i32 x = x0; x <= x1; ++x) ++g.start[g.index(x, y, z) + 1];
    }
    for (usize c = 0; c < cells; ++c) g.start[c + 1] += g.start[c];
    g.list.assign(g.start[cells], 0);
    std::vector<u32> cursor(g.start.begin(), g.start.end() - 1);
    for (u32 t = 0; t < tris.size(); ++t) {
        if (!tris[t].ok) continue;
        i32 x0, y0, z0, x1, y1, z1;
        cellRange(tris[t], x0, y0, z0, x1, y1, z1);
        for (i32 z = z0; z <= z1; ++z)
            for (i32 y = y0; y <= y1; ++y)
                for (i32 x = x0; x <= x1; ++x) g.list[cursor[g.index(x, y, z)]++] = t;
    }

    out.homeTri.assign(render.vertexCount, 0);
    out.bary.assign(static_cast<usize>(render.vertexCount) * 3, 0.0f);
    out.offset.assign(render.vertexCount, 0.0f);
    for (u32 v = 0; v < render.vertexCount; ++v) {
        const f32* pp = render.positions + static_cast<usize>(v) * 3;
        const Vec3 p{pp[0], pp[1], pp[2]};
        const Nearest nr = queryGrid(g, tris, p);
        if (nr.tri == kNone || std::sqrt(nr.dist2) > limit) {
            char buf[160];
            if (nr.tri == kNone) std::snprintf(buf, sizeof buf, "render vertex %u is farther than %.2f cm from every proxy triangle", v, static_cast<double>(limit));
            else std::snprintf(buf, sizeof buf, "render vertex %u is %.2f cm from the proxy (limit %.2f cm)", v, static_cast<double>(std::sqrt(nr.dist2)), static_cast<double>(limit));
            return fail(buf);
        }
        const RestTri& rt = tris[nr.tri];
        const Triangle& tri = proxyCage.triangles[nr.tri];
        f32 b[3];
        Vec3 q;
        if (!planeBary(p, rt.a, rt.b, rt.c, b, &q)) return fail("a proxy triangle is degenerate");
        const Vec3 ns = unitOr(smooth[tri.v0[0]] * b[0] + smooth[tri.v0[1]] * b[1] + smooth[tri.v0[2]] * b[2], cross(rt.b - rt.a, rt.c - rt.a).getSafeNormal());
        out.homeTri[v] = nr.tri;
        out.bary[static_cast<usize>(v) * 3 + 0] = b[0];
        out.bary[static_cast<usize>(v) * 3 + 1] = b[1];
        out.bary[static_cast<usize>(v) * 3 + 2] = b[2];
        out.offset[v] = dot(p - q, ns);
    }

    // The proxy triangle each render triangle follows: the one nearest its centroid.
    const u32 renderTris = render.indexCount / 3;
    out.triTri.assign(renderTris, 0);
    for (u32 r = 0; r < renderTris; ++r) {
        Vec3 c{0, 0, 0};
        for (u32 k = 0; k < 3; ++k) {
            const f32* pp = render.positions + static_cast<usize>(render.indices[r * 3 + k]) * 3;
            c += Vec3{pp[0], pp[1], pp[2]};
        }
        const Nearest nr = queryGrid(g, tris, c / 3.0f);
        out.triTri[r] = nr.tri != kNone ? nr.tri : out.homeTri[render.indices[r * 3]];
    }
    return true;
}

Vec3 proxyEvaluate(const ProxyBind& bind, u32 renderVertex, u32 tri, const Cage& cage, const Vec3* smoothNormal) {
    if (renderVertex >= bind.homeTri.size()) return Vec3{0, 0, 0};
    const u32 home = bind.homeTri[renderVertex];
    if (home >= cage.triangles.size()) return Vec3{0, 0, 0};
    if (tri >= cage.triangles.size()) tri = home;
    f32 b[3] = {bind.bary[static_cast<usize>(renderVertex) * 3], bind.bary[static_cast<usize>(renderVertex) * 3 + 1], bind.bary[static_cast<usize>(renderVertex) * 3 + 2]};
    const Triangle& tt = cage.triangles[tri];
    if (tri != home) {
        // Re-express the vertex's rest position in the other triangle (plane projection, extrapolating).
        const Triangle& ht = cage.triangles[home];
        const Vec3 qRest = cage.particles[ht.v0[0]].rest * b[0] + cage.particles[ht.v0[1]].rest * b[1] + cage.particles[ht.v0[2]].rest * b[2];
        f32 b2[3];
        if (planeBary(qRest, cage.particles[tt.v0[0]].rest, cage.particles[tt.v0[1]].rest, cage.particles[tt.v0[2]].rest, b2)) {
            b[0] = b2[0]; b[1] = b2[1]; b[2] = b2[2];
        }
    }
    const Vec3& pa = cage.particles[tt.v[0]].pos;
    const Vec3& pb = cage.particles[tt.v[1]].pos;
    const Vec3& pc = cage.particles[tt.v[2]].pos;
    const Vec3 q = pa * b[0] + pb * b[1] + pc * b[2];
    Vec3 n = cross(pb - pa, pc - pa);
    if (smoothNormal) n = smoothNormal[tt.v[0]] * b[0] + smoothNormal[tt.v[1]] * b[1] + smoothNormal[tt.v[2]] * b[2];
    n = n.getSafeNormal();
    return q + n * (bind.offset[renderVertex] * bind.offsetScale);
}

} // namespace aver::softbody
