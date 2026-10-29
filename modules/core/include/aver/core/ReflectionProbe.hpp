// Reflection probes (G4 R8, docs/plans/fabric/G4_RENDER.md): the placement, box-projection and blending maths of Godot's ReflectionProbe, and the
// octahedral mapping a captured environment would be stored in. Header-only and pure. The capture and the shading that samples it are NOT in the
// engine yet (G4_RENDER.md R8 says why and what the next step is); what is here is what the framework registry answers script queries with, and what
// that step will use unchanged.
//
// Engine space: centimetres, +Z up, left-handed. A probe is an oriented box (the node's transform) of influence; its capture point is the box centre
// shifted by `originOffset`. Box projection corrects a reflection ray so it hits the box walls where the capture saw them.
#pragma once
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver {

enum class ProbeAmbient : i32 { Disabled = 0, Environment = 1, Colour = 2 };   // Godot's ReflectionProbe.AmbientMode
enum class ProbeUpdate : i32 { Once = 0, Always = 1 };                          // Godot's ReflectionProbe.UpdateMode

inline constexpr u32 kProbeMaxActive = 4;   // probes that can blend on one surface

struct ReflectionProbeDesc {
    // Instance matrix: three basis rows then the translation (row vectors), centimetres. The basis rotates (and scales) the box.
    f32 world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    f32 halfExtentCm[3] = {1000, 1000, 1000};   // Godot size / 2, axis-mapped, before the matrix's scale
    f32 originOffsetCm[3] = {0, 0, 0};          // capture point relative to the centre, in the probe's local axes
    f32 intensity = 1.0f;
    f32 blendDistanceCm = 100.0f;               // the fade inside the box walls
    f32 maxDistanceCm = 0.0f;                   // beyond this from the camera the probe is off; 0 = never
    bool boxProjection = false;
    bool interior = false;                      // the sky does not light this space
    ProbeAmbient ambient = ProbeAmbient::Environment;
    f32 ambientColour[3] = {0, 0, 0};           // linear, used when ambient == Colour
    f32 ambientEnergy = 1.0f;
    ProbeUpdate update = ProbeUpdate::Once;
    u32 cullMask = 0xFFFFFFFFu;                 // layers captured
    u32 reflectionMask = 0xFFFFFFFFu;           // layers that see this probe
    u32 resolution = 256;                       // face size the capture would use
};

namespace probe {

struct Frame {
    // world -> local (the box's own axes, cm): out_j = dot(p, row_j.xyz) + row_j.w; and the basis rows to carry a local direction back to the world.
    f32 toLocal[3][4];
    f32 basis[3][3];
    bool ok = false;
};

inline Frame frameOf(const ReflectionProbeDesc& p) {
    Frame f{};
    const f32* w = p.world;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) f.basis[i][j] = w[i * 3 + j];
    const f32 a = w[0], b = w[1], c = w[2], d = w[3], e = w[4], g2 = w[5], g = w[6], h = w[7], i = w[8];
    const f32 det = a * (e * i - g2 * h) - b * (d * i - g2 * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-20f)) return f;
    const f32 inv = 1.0f / det;
    const f32 m[3][3] = {{(e * i - g2 * h) * inv, (c * h - b * i) * inv, (b * g2 - c * e) * inv},
                         {(g2 * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * g2) * inv},
                         {(d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv}};
    for (int j = 0; j < 3; ++j) {
        f.toLocal[j][0] = m[0][j]; f.toLocal[j][1] = m[1][j]; f.toLocal[j][2] = m[2][j];
        f.toLocal[j][3] = -(w[9] * m[0][j] + w[10] * m[1][j] + w[11] * m[2][j]);
    }
    f.ok = true;
    return f;
}

inline Vec3 toLocalPoint(const Frame& f, const Vec3& p) {
    return {p.x * f.toLocal[0][0] + p.y * f.toLocal[0][1] + p.z * f.toLocal[0][2] + f.toLocal[0][3],
            p.x * f.toLocal[1][0] + p.y * f.toLocal[1][1] + p.z * f.toLocal[1][2] + f.toLocal[1][3],
            p.x * f.toLocal[2][0] + p.y * f.toLocal[2][1] + p.z * f.toLocal[2][2] + f.toLocal[2][3]};
}

// A world direction in the box's local axes (no translation).
inline Vec3 toLocalDir(const Frame& f, const Vec3& d) {
    return {d.x * f.toLocal[0][0] + d.y * f.toLocal[0][1] + d.z * f.toLocal[0][2],
            d.x * f.toLocal[1][0] + d.y * f.toLocal[1][1] + d.z * f.toLocal[1][2],
            d.x * f.toLocal[2][0] + d.y * f.toLocal[2][1] + d.z * f.toLocal[2][2]};
}

inline Vec3 toWorldDir(const Frame& f, const Vec3& d) {
    return {d.x * f.basis[0][0] + d.y * f.basis[1][0] + d.z * f.basis[2][0],
            d.x * f.basis[0][1] + d.y * f.basis[1][1] + d.z * f.basis[2][1],
            d.x * f.basis[0][2] + d.y * f.basis[1][2] + d.z * f.basis[2][2]};
}

// The capture point in the world.
inline Vec3 capturePoint(const ReflectionProbeDesc& p) {
    const f32* w = p.world;
    const Vec3 o = {p.originOffsetCm[0], p.originOffsetCm[1], p.originOffsetCm[2]};
    return {w[9] + o.x * w[0] + o.y * w[3] + o.z * w[6],
            w[10] + o.x * w[1] + o.y * w[4] + o.z * w[7],
            w[11] + o.x * w[2] + o.y * w[5] + o.z * w[8]};
}

// How far a point is inside the box, in cm of the NEAREST wall (negative outside). Distances are along the local axes, so a scaled box measures
// true centimetres of the scaled wall.
inline f32 depthInside(const ReflectionProbeDesc& p, const Frame& f, const Vec3& worldPos) {
    const Vec3 l = toLocalPoint(f, worldPos);
    const f32 sx = std::sqrt(f.basis[0][0] * f.basis[0][0] + f.basis[0][1] * f.basis[0][1] + f.basis[0][2] * f.basis[0][2]);
    const f32 sy = std::sqrt(f.basis[1][0] * f.basis[1][0] + f.basis[1][1] * f.basis[1][1] + f.basis[1][2] * f.basis[1][2]);
    const f32 sz = std::sqrt(f.basis[2][0] * f.basis[2][0] + f.basis[2][1] * f.basis[2][1] + f.basis[2][2] * f.basis[2][2]);
    const f32 dx = (p.halfExtentCm[0] - std::fabs(l.x)) * sx;
    const f32 dy = (p.halfExtentCm[1] - std::fabs(l.y)) * sy;
    const f32 dz = (p.halfExtentCm[2] - std::fabs(l.z)) * sz;
    return std::min({dx, dy, dz});
}

// Influence at a point: 0 outside the box, 1 deeper than the blend distance, a linear ramp between (Godot's blend_distance fades inward from the walls).
inline f32 influence(const ReflectionProbeDesc& p, const Frame& f, const Vec3& worldPos) {
    if (!f.ok) return 0.0f;
    const f32 d = depthInside(p, f, worldPos);
    if (d <= 0.0f) return 0.0f;
    if (p.blendDistanceCm <= 1e-3f) return 1.0f;
    return std::min(d / p.blendDistanceCm, 1.0f);
}

// The direction to look the capture up with for a surface at `worldPos` reflecting `dir` (unit, world). Without box projection it is `dir`. With it the ray
// is intersected with the probe box and the result is the direction from the capture point to the hit, so a wall appears where it really is.
inline Vec3 boxProject(const ReflectionProbeDesc& p, const Frame& f, const Vec3& worldPos, const Vec3& dir) {
    if (!p.boxProjection || !f.ok) return dir;
    const Vec3 lp = toLocalPoint(f, worldPos);
    const Vec3 ld = toLocalDir(f, dir);
    const f32 half[3] = {p.halfExtentCm[0], p.halfExtentCm[1], p.halfExtentCm[2]};
    const f32 o[3] = {lp.x, lp.y, lp.z}, v[3] = {ld.x, ld.y, ld.z};
    for (int k = 0; k < 3; ++k) if (std::fabs(o[k]) > half[k]) return dir;   // outside the box: the probe does not light this point
    // Exit distance of the ray from the slab box: the smallest of the per-axis far hits.
    f32 tFar = 1e30f;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(v[k]) < 1e-8f) continue;
        const f32 t1 = (half[k] - o[k]) / v[k], t2 = (-half[k] - o[k]) / v[k];
        tFar = std::min(tFar, std::max(t1, t2));
    }
    if (!(tFar > 0.0f) || tFar > 1e29f) return dir;   // outside the box or parallel to everything: no correction
    const Vec3 hit = {o[0] + v[0] * tFar - p.originOffsetCm[0], o[1] + v[1] * tFar - p.originOffsetCm[1], o[2] + v[2] * tFar - p.originOffsetCm[2]};
    const Vec3 hw = toWorldDir(f, hit);
    const Vec3 n = hw.getSafeNormal();
    return n.sizeSquared() > 0.0f ? n : dir;
}

// ---- choosing probes for a view ----

struct Pick {
    u32 index = 0;      // into the caller's array
    f32 weight = 0.0f;  // the share of this probe's light in the final blend, after the nearer / smaller probes took theirs
};

struct Selection {
    Pick pick[kProbeMaxActive];
    u32 count = 0;
    f32 rest = 1.0f;    // what is left for the sky / default environment
};

// A probe is live for a camera when it has intensity and, with a max distance, the camera is within it of the box centre.
inline bool live(const ReflectionProbeDesc& p, const Vec3& camera) {
    if (!(p.intensity > 0.0f)) return false;
    if (p.maxDistanceCm > 0.0f) {
        const Vec3 c = {p.world[9], p.world[10], p.world[11]};
        if ((c - camera).size() > p.maxDistanceCm) return false;
    }
    return true;
}

// The probes that light a surface at `worldPos`, smallest box first (a small probe inside a big one is the more specific), up to kProbeMaxActive. Each
// takes `influence * remaining`; what is left is `rest`. Probes whose reflection mask shares no layer with `receiverMask` are skipped.
inline Selection select(const ReflectionProbeDesc* probes, u32 count, const Vec3& worldPos, const Vec3& camera, u32 receiverMask = 0xFFFFFFFFu) {
    struct Cand { u32 i; f32 w; f32 volume; };
    std::vector<Cand> c;
    for (u32 i = 0; i < count; ++i) {
        const ReflectionProbeDesc& p = probes[i];
        if (!live(p, camera) || (p.reflectionMask & receiverMask) == 0u) continue;
        const Frame f = frameOf(p);
        const f32 w = influence(p, f, worldPos);
        if (w <= 0.0f) continue;
        c.push_back({i, w, p.halfExtentCm[0] * p.halfExtentCm[1] * p.halfExtentCm[2]});
    }
    std::stable_sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.volume < b.volume; });
    Selection s;
    for (const Cand& k : c) {
        if (s.count >= kProbeMaxActive || s.rest <= 1e-4f) break;
        const f32 share = k.w * s.rest;
        s.pick[s.count++] = {k.i, share * probes[k.i].intensity};
        s.rest -= share;
    }
    return s;
}

// ---- octahedral storage of a captured environment ----
// A direction <-> a point of the unit square (Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors"). The axis is the engine's
// +Z: the octahedron's "up" pole is the centre of the square.

inline f32 signNotZero(f32 v) { return v >= 0.0f ? 1.0f : -1.0f; }

inline void octEncode(const Vec3& dir, f32& u, f32& v) {
    const f32 l1 = std::fabs(dir.x) + std::fabs(dir.y) + std::fabs(dir.z);
    const f32 inv = l1 > 0.0f ? 1.0f / l1 : 0.0f;
    f32 px = dir.x * inv, py = dir.y * inv;
    const f32 pz = dir.z * inv;
    if (pz < 0.0f) {
        const f32 ox = (1.0f - std::fabs(py)) * signNotZero(px);
        const f32 oy = (1.0f - std::fabs(px)) * signNotZero(py);
        px = ox; py = oy;
    }
    u = px * 0.5f + 0.5f;
    v = py * 0.5f + 0.5f;
}

inline Vec3 octDecode(f32 u, f32 v) {
    f32 x = u * 2.0f - 1.0f, y = v * 2.0f - 1.0f;
    const f32 z = 1.0f - std::fabs(x) - std::fabs(y);
    if (z < 0.0f) {
        const f32 ox = (1.0f - std::fabs(y)) * signNotZero(x);
        const f32 oy = (1.0f - std::fabs(x)) * signNotZero(y);
        x = ox; y = oy;
    }
    return Vec3{x, y, z}.getSafeNormal();
}

// Cube faces in the engine's axes: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z. The face of a direction and its position on that face in [0,1]^2.
inline u32 cubeFace(const Vec3& d, f32& s, f32& t) {
    const f32 ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    u32 face; f32 ma, sc, tc;
    if (ax >= ay && ax >= az) { face = d.x >= 0 ? 0u : 1u; ma = ax; sc = d.x >= 0 ? -d.y : d.y; tc = -d.z; }
    else if (ay >= az)        { face = d.y >= 0 ? 2u : 3u; ma = ay; sc = d.y >= 0 ? d.x : -d.x; tc = -d.z; }
    else                      { face = d.z >= 0 ? 4u : 5u; ma = az; sc = d.y; tc = d.z >= 0 ? d.x : -d.x; }
    const f32 inv = ma > 0.0f ? 0.5f / ma : 0.0f;
    // Clamped: on an edge a fused multiply-add (arm64 Clang) lands a hair outside the face.
    s = std::clamp(sc * inv + 0.5f, 0.0f, 1.0f);
    t = std::clamp(tc * inv + 0.5f, 0.0f, 1.0f);
    return face;
}

// The inverse of cubeFace: the unit direction of the point (s, t) on a face.
inline Vec3 cubeDirection(u32 face, f32 s, f32 t) {
    const f32 sc = s * 2.0f - 1.0f, tc = t * 2.0f - 1.0f;
    Vec3 d;
    switch (face) {
        case 0:  d = {1.0f, -sc, -tc}; break;
        case 1:  d = {-1.0f, sc, -tc}; break;
        case 2:  d = {sc, 1.0f, -tc}; break;
        case 3:  d = {-sc, -1.0f, -tc}; break;
        case 4:  d = {tc, sc, 1.0f}; break;
        default: d = {-tc, sc, -1.0f}; break;
    }
    return d.getSafeNormal();
}

// Which blur level of a prefiltered capture a surface of this roughness reads: linear in roughness over the chain.
inline f32 roughnessToMip(f32 roughness, u32 mipCount) {
    if (mipCount <= 1) return 0.0f;
    return std::clamp(roughness, 0.0f, 1.0f) * static_cast<f32>(mipCount - 1);
}

} // namespace probe
} // namespace aver
