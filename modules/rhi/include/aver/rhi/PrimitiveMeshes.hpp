#pragma once
// The engine's built-in primitives beyond the cube and sphere: generated meshes with no file behind
// them, registered under "Meshes/<name>.ocmesh" (GameContent::registerBuiltins) and drawn by the actor
// preview from the same builders. All are unit-sized: they fit in [-1, 1]^3, Z up, so a placement's
// scale is a half-extent in cm exactly as for the unit cube. Winding is counter-clockwise seen from
// outside (as appendBox); every builder sets normals and orientToNormals settles the winding.
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::rhi::primitives {

inline constexpr f32 kPi = 3.14159265358979f;

// Flips any triangle whose face normal points against its vertices' normals.
inline void orientToNormals(const std::vector<MeshVertex>& v, std::vector<u32>& idx, usize from) {
    for (usize t = from; t + 2 < idx.size(); t += 3) {
        const MeshVertex& a = v[idx[t]];
        const MeshVertex& b = v[idx[t + 1]];
        const MeshVertex& c = v[idx[t + 2]];
        const f32 ex = b.px - a.px, ey = b.py - a.py, ez = b.pz - a.pz;
        const f32 fx = c.px - a.px, fy = c.py - a.py, fz = c.pz - a.pz;
        const f32 nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
        const f32 d = nx * (a.nx + b.nx + c.nx) + ny * (a.ny + b.ny + c.ny) + nz * (a.nz + b.nz + c.nz);
        if (d < 0.0f) std::swap(idx[t + 1], idx[t + 2]);
    }
}

// One point of a lathe profile: radius from the Z axis, height, the profile normal (radial, z) and v.
struct ProfilePoint { f32 r, z, nr, nz, v; };

// Revolves profile strips around +Z. Each strip is a connected run; strips meet at hard edges.
inline void appendLathe(std::vector<MeshVertex>& v, std::vector<u32>& idx,
                        const std::vector<std::vector<ProfilePoint>>& strips, u32 sectors) {
    const usize from = idx.size();
    for (const auto& s : strips) {
        const u32 base = static_cast<u32>(v.size());
        for (const ProfilePoint& p : s) {
            const f32 nl = std::sqrt(p.nr * p.nr + p.nz * p.nz);
            const f32 nr = nl > 0.0f ? p.nr / nl : 0.0f, nz = nl > 0.0f ? p.nz / nl : 1.0f;
            for (u32 sec = 0; sec <= sectors; ++sec) {
                const f32 th = 2.0f * kPi * (static_cast<f32>(sec) / static_cast<f32>(sectors));
                const f32 c = std::cos(th), sn = std::sin(th);
                v.push_back({p.r * c, p.r * sn, p.z, nr * c, nr * sn, nz,
                             static_cast<f32>(sec) / static_cast<f32>(sectors), p.v});
            }
        }
        const u32 stride = sectors + 1;
        for (usize i = 0; i + 1 < s.size(); ++i)
            for (u32 sec = 0; sec < sectors; ++sec) {
                const u32 a = base + static_cast<u32>(i) * stride + sec, b = a + stride;
                if (s[i].r > 0.0f || s[i + 1].r > 0.0f) {
                    if (s[i].r > 0.0f) { idx.push_back(a); idx.push_back(b); idx.push_back(a + 1); }
                    if (s[i + 1].r > 0.0f) { idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1); }
                }
            }
    }
    orientToNormals(v, idx, from);
}

// Flat quad (a, b, c, d in order around it) with one normal; uv corners 00, 10, 11, 01.
inline void appendQuad(std::vector<MeshVertex>& v, std::vector<u32>& idx, const f32 (&p)[4][3], const f32 (&n)[3]) {
    const usize from = idx.size();
    const u32 base = static_cast<u32>(v.size());
    const f32 uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; ++k) v.push_back({p[k][0], p[k][1], p[k][2], n[0], n[1], n[2], uv[k][0], uv[k][1]});
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    orientToNormals(v, idx, from);
}

inline void appendTri(std::vector<MeshVertex>& v, std::vector<u32>& idx, const f32 (&p)[3][3], const f32 (&n)[3]) {
    const usize from = idx.size();
    const u32 base = static_cast<u32>(v.size());
    const f32 uv[3][2] = {{0, 0}, {1, 0}, {0.5f, 1}};
    for (int k = 0; k < 3; ++k) v.push_back({p[k][0], p[k][1], p[k][2], n[0], n[1], n[2], uv[k][0], uv[k][1]});
    idx.insert(idx.end(), {base, base + 1, base + 2});
    orientToNormals(v, idx, from);
}

// 2 x 2 at z = 0, facing +Z (one-sided).
inline void appendPlane(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    appendQuad(v, idx, {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}, {0, 0, 1});
}

// Radius 1, z in [-1, 1], capped.
inline void appendCylinder(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    appendLathe(v, idx, {{{0, 1, 0, 1, 0}, {1, 1, 0, 1, 0.25f}},
                         {{1, 1, 1, 0, 0.25f}, {1, -1, 1, 0, 0.75f}},
                         {{1, -1, 0, -1, 0.75f}, {0, -1, 0, -1, 1}}}, 32);
}

// Base radius 1 at z = -1, apex at z = 1.
inline void appendCone(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    appendLathe(v, idx, {{{0, 1, 2, 1, 0}, {1, -1, 2, 1, 0.75f}},
                         {{1, -1, 0, -1, 0.75f}, {0, -1, 0, -1, 1}}}, 32);
}

// Radius 0.5, total height 2: two hemispheres on a band from z = -0.5 to 0.5.
inline void appendCapsule(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    constexpr u32 kHemi = 8;
    std::vector<ProfilePoint> s;
    for (int half = 0; half < 2; ++half)
        for (u32 r = 0; r <= kHemi; ++r) {
            const f32 phi = 0.5f * kPi * (static_cast<f32>(half) + static_cast<f32>(r) / kHemi);
            const f32 nr = std::sin(phi), nz = std::cos(phi);
            const f32 cz = half == 0 ? 0.5f : -0.5f;
            s.push_back({0.5f * nr, cz + 0.5f * nz, nr, nz, static_cast<f32>(half * kHemi + r) / (2 * kHemi)});
        }
    appendLathe(v, idx, {s}, 32);
}

// Ring of major radius 0.7 and tube radius 0.3, around Z.
inline void appendTorus(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    constexpr u32 kTube = 24;
    std::vector<ProfilePoint> s;
    for (u32 i = 0; i <= kTube; ++i) {
        const f32 a = 2.0f * kPi * static_cast<f32>(i) / kTube;
        s.push_back({0.7f + 0.3f * std::cos(a), 0.3f * std::sin(a), std::cos(a), std::sin(a), static_cast<f32>(i) / kTube});
    }
    appendLathe(v, idx, {s}, 48);
}

// Pipe: outer radius 1, inner 0.75, z in [-1, 1].
inline void appendTube(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    appendLathe(v, idx, {{{0.75f, 1, 0, 1, 0}, {1, 1, 0, 1, 0.1f}},
                         {{1, 1, 1, 0, 0.1f}, {1, -1, 1, 0, 0.5f}},
                         {{1, -1, 0, -1, 0.5f}, {0.75f, -1, 0, -1, 0.6f}},
                         {{0.75f, -1, -1, 0, 0.6f}, {0.75f, 1, -1, 0, 1}}}, 32);
}

// Square base at z = -1, apex at z = 1, flat faces.
inline void appendPyramid(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    const f32 k = 1.0f / std::sqrt(5.0f);
    appendTri(v, idx, {{1, -1, -1}, {1, 1, -1}, {0, 0, 1}}, {2 * k, 0, k});
    appendTri(v, idx, {{1, 1, -1}, {-1, 1, -1}, {0, 0, 1}}, {0, 2 * k, k});
    appendTri(v, idx, {{-1, 1, -1}, {-1, -1, -1}, {0, 0, 1}}, {-2 * k, 0, k});
    appendTri(v, idx, {{-1, -1, -1}, {1, -1, -1}, {0, 0, 1}}, {0, -2 * k, k});
    appendQuad(v, idx, {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}}, {0, 0, -1});
}

// A ramp: full height at x = -1, sloping down to the bottom edge at x = 1.
inline void appendWedge(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    const f32 k = 1.0f / std::sqrt(2.0f);
    appendQuad(v, idx, {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}}, {0, 0, -1});
    appendQuad(v, idx, {{-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}, {-1, -1, 1}}, {-1, 0, 0});
    appendQuad(v, idx, {{1, -1, -1}, {1, 1, -1}, {-1, 1, 1}, {-1, -1, 1}}, {k, 0, k});
    appendTri(v, idx, {{-1, -1, -1}, {1, -1, -1}, {-1, -1, 1}}, {0, -1, 0});
    appendTri(v, idx, {{-1, 1, -1}, {1, 1, -1}, {-1, 1, 1}}, {0, 1, 0});
}

// A banana, for scale: 2 long along X on a shallow arc (ends up), five soft ridges, a cut stem at -X
// and a blunt tip at +X. The editor places it at real size (about 20 cm).
inline void appendBanana(std::vector<MeshVertex>& v, std::vector<u32>& idx) {
    constexpr u32 kAlong = 64, kAround = 20;
    constexpr f32 kArc = 0.62f;                          // half the arc angle, radians
    const f32 bend = 0.9f / std::sin(kArc);              // arc radius: centreline x spans [-0.9, 0.9]
    const f32 lift = bend * (1.0f + std::cos(kArc)) * 0.5f;
    const auto smax = [](f32 a, f32 b) { const f32 d = a - b, k = 0.02f; return 0.5f * (a + b + std::sqrt(d * d + k * k)); };
    struct Frame { f32 c[3], e1[3], t[3]; f32 rad, ridge; };
    const auto frame = [&](f32 s) {
        Frame f{};
        const f32 a = -kArc + 2.0f * kArc * s;
        f.c[0] = bend * std::sin(a); f.c[1] = 0.0f; f.c[2] = lift - bend * std::cos(a);
        f.e1[0] = std::sin(a); f.e1[1] = 0.0f; f.e1[2] = -std::cos(a);   // away from the arc centre
        f.t[0] = std::cos(a); f.t[1] = 0.0f; f.t[2] = std::sin(a);
        const f32 u = (s - 0.1f) / 0.9f;                                 // the fruit; s < 0.1 is the stem
        const f32 body = (u > 0.0f && u < 1.0f) ? 0.21f * std::pow(std::sin(kPi * std::pow(u, 0.85f)), 0.6f) : 0.0f;
        const f32 stem = s < 0.16f ? 0.045f : 0.0f;
        f.rad = smax(smax(body, stem), 0.03f);
        f.ridge = (u > 0.0f && u < 1.0f) ? std::sin(kPi * u) : 0.0f;
        return f;
    };
    const auto at = [&](f32 s, f32 phi, f32 (&p)[3]) {
        const Frame f = frame(s);
        const f32 r = f.rad * (1.0f + 0.07f * f.ridge * std::cos(5.0f * phi));
        const f32 cp = std::cos(phi), sp = std::sin(phi);
        for (int k = 0; k < 3; ++k) p[k] = f.c[k] + r * (cp * f.e1[k] + sp * (k == 1 ? 1.0f : 0.0f));
    };
    const usize from = idx.size();
    const u32 base = static_cast<u32>(v.size());
    for (u32 i = 0; i <= kAlong; ++i) {
        const f32 s = static_cast<f32>(i) / kAlong;
        const Frame f = frame(s);
        for (u32 j = 0; j <= kAround; ++j) {
            const f32 phi = 2.0f * kPi * static_cast<f32>(j) / kAround;
            f32 p[3], ps0[3], ps1[3], pp0[3], pp1[3];
            const f32 ds = 0.5f / kAlong, dp = 0.25f * kPi / kAround;
            at(s, phi, p);
            at(std::fmax(0.0f, s - ds), phi, ps0); at(std::fmin(1.0f, s + ds), phi, ps1);
            at(s, phi - dp, pp0); at(s, phi + dp, pp1);
            const f32 a[3] = {ps1[0] - ps0[0], ps1[1] - ps0[1], ps1[2] - ps0[2]};
            const f32 b[3] = {pp1[0] - pp0[0], pp1[1] - pp0[1], pp1[2] - pp0[2]};
            f32 n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            const f32 out = n[0] * (p[0] - f.c[0]) + n[1] * (p[1] - f.c[1]) + n[2] * (p[2] - f.c[2]);
            const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (out < 0.0f ? -1.0f : 1.0f);
            for (f32& c : n) c = len != 0.0f ? c / len : 0.0f;
            v.push_back({p[0], p[1], p[2], n[0], n[1], n[2], static_cast<f32>(j) / kAround, s});
        }
    }
    const u32 stride = kAround + 1;
    for (u32 i = 0; i < kAlong; ++i)
        for (u32 j = 0; j < kAround; ++j) {
            const u32 a = base + i * stride + j, b = a + stride;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    // The cut stem and the tip: a fan each, facing along the centreline.
    for (int end = 0; end < 2; ++end) {
        const Frame f = frame(end == 0 ? 0.0f : 1.0f);
        const f32 sg = end == 0 ? -1.0f : 1.0f;
        const u32 ring = base + (end == 0 ? 0u : kAlong * stride);
        const u32 centre = static_cast<u32>(v.size());
        v.push_back({f.c[0], f.c[1], f.c[2], sg * f.t[0], sg * f.t[1], sg * f.t[2], 0.5f, end == 0 ? 0.0f : 1.0f});
        for (u32 j = 0; j < kAround; ++j) {
            const u32 rim = static_cast<u32>(v.size());
            for (u32 k = 0; k < 2; ++k) {
                MeshVertex m = v[ring + j + k];
                m.nx = sg * f.t[0]; m.ny = sg * f.t[1]; m.nz = sg * f.t[2];
                v.push_back(m);
            }
            idx.insert(idx.end(), {centre, rim, rim + 1});
        }
    }
    orientToNormals(v, idx, from);
}

struct BuiltinPrimitive {
    const char* path;     // the mesh id's source: fnv1a64 of this content-relative path
    const char* label;    // the editor's Add menu entry
    f32 spawnScale;       // the editor's Add scale, cm per unit (50: the Cube's 1 m)
    const char* look;     // the built-in surface look the editor's Add gives it, or nullptr
    void (*build)(std::vector<MeshVertex>&, std::vector<u32>&);
};

// In Add-menu order. Ids are path hashes, so entries can be reordered but never renamed.
inline constexpr BuiltinPrimitive kBuiltinPrimitives[] = {
    {"Meshes/plane.ocmesh",    "Plane",    50.0f, nullptr,    &appendPlane},
    {"Meshes/cylinder.ocmesh", "Cylinder", 50.0f, nullptr,    &appendCylinder},
    {"Meshes/cone.ocmesh",     "Cone",     50.0f, nullptr,    &appendCone},
    {"Meshes/capsule.ocmesh",  "Capsule",  50.0f, nullptr,    &appendCapsule},
    {"Meshes/torus.ocmesh",    "Torus",    50.0f, nullptr,    &appendTorus},
    {"Meshes/tube.ocmesh",     "Tube",     50.0f, nullptr,    &appendTube},
    {"Meshes/pyramid.ocmesh",  "Pyramid",  50.0f, nullptr,    &appendPyramid},
    {"Meshes/wedge.ocmesh",    "Wedge",    50.0f, nullptr,    &appendWedge},
    {"Meshes/banana.ocmesh",   "Banana",   10.0f, "M_Banana", &appendBanana},
};

// The entry for a content-relative path, or nullptr.
inline const BuiltinPrimitive* findBuiltinPrimitive(std::string_view path) {
    for (const BuiltinPrimitive& p : kBuiltinPrimitives)
        if (path == p.path) return &p;
    return nullptr;
}

// Axis-aligned bounds of what a builder made, padded so a flat shape is never zero-thick.
inline void primitiveBounds(const std::vector<MeshVertex>& v, f32 (&mn)[3], f32 (&mx)[3]) {
    for (int k = 0; k < 3; ++k) { mn[k] = 1e30f; mx[k] = -1e30f; }
    for (const MeshVertex& m : v) {
        const f32 p[3] = {m.px, m.py, m.pz};
        for (int k = 0; k < 3; ++k) { mn[k] = std::fmin(mn[k], p[k]); mx[k] = std::fmax(mx[k], p[k]); }
    }
    for (int k = 0; k < 3; ++k)
        if (mx[k] - mn[k] < 0.02f) { const f32 c = 0.5f * (mn[k] + mx[k]); mn[k] = c - 0.01f; mx[k] = c + 0.01f; }
}

} // namespace aver::rhi::primitives
