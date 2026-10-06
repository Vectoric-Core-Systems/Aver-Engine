#pragma once
// Projected decals as the renderer sees them: the host-facing SceneDecal, the 192-byte GPU record, the
// packing between them, a CPU mirror of the projector maths in shaders/voxi_decal.hlsli (used by tests;
// the shader is the one that runs), and the mip chain builder for decal images.
// Depends on Aver.Core only. See docs/rendering/DECALS.md.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace aver::voxi {

// Decals shaded per frame, nearest-and-largest kept when more are submitted. Every pixel of a surface
// tests each one, so this is a cost cap as much as a memory one.
inline constexpr u32 kMaxSceneDecals = 128;

// PackedDecal::ext.w bits (voxi_decal.hlsli's AVER_DECAL_*).
inline constexpr u32 kDecalPackColour    = 0x01;   // paints base colour
inline constexpr u32 kDecalPackNormal    = 0x02;   // perturbs the normal
inline constexpr u32 kDecalPackRough     = 0x04;   // paints roughness and metallic
inline constexpr u32 kDecalPackHasBase   = 0x08;   // reads a base colour image
inline constexpr u32 kDecalPackHasNormal = 0x10;   // reads a normal image
inline constexpr u32 kDecalPackHasOrm    = 0x20;   // reads a roughness/metallic image
inline constexpr u32 kDecalPackTextured  = 0x40;   // any image: skipped where no bindless table is bound

inline constexpr u32 kDecalUnboundTexture = 0xFFFFFFFFu;

// A decal in world space, filled by the host from scene::gatherDecals.
struct SceneDecal {
    f32 world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // row-vector projector matrix, scale included
    f32 halfExtentsCm[3] = {50, 50, 50};   // in the matrix's local units
    f32 tint[3] = {1, 1, 1};               // linear
    f32 opacity = 1.0f;
    f32 normalStrength = 1.0f;
    f32 roughness = 1.0f;                  // multiplies the ORM green; the value itself without an ORM image
    f32 metallic = 0.0f;
    f32 edgeFade = 0.1f;                   // fraction of the half extent
    f32 angleFadeStartDeg = 60.0f, angleFadeEndDeg = 85.0f;
    f32 fadeDistanceCm = 0.0f;             // 0 = never
    i32 sortOrder = 0;
    // Which channels may paint. The host sets roughnessMetal only when an ORM image or an explicit
    // roughness/metallic was authored, so an untouched decal leaves the surface's roughness alone.
    bool colour = true, normal = true, roughnessMetal = false;
    u64 baseId = 0, normalId = 0, ormId = 0;   // registerDecalTexture ids; 0 = none
    f32 uvScale[2] = {1, 1};
    f32 uvOffset[2] = {0, 0};
};

// The bindless indices a decal's images resolved to (kDecalUnboundTexture = not resident).
struct DecalTextureIndices {
    u32 base = kDecalUnboundTexture, normal = kDecalUnboundTexture, orm = kDecalUnboundTexture;
};

// One decal as the GPU reads it (AverDecalRec in voxi_decal.hlsli). The first four float4s are all a
// pixel outside the box ever reads.
struct PackedDecal {
    f32 r0[4], r1[4], r2[4];   // world-minus-eye -> box-local: xyz of row i is row i of the 3x3, w the translation i
    f32 ext[4];                // xyz half extents (box-local), w the flag bits as a u32
    f32 tint[4];               // rgb tint (linear), a opacity
    f32 shade[4];              // normal strength, roughness, metallic, edge fade
    f32 ang[4];                // x cos(full-strength angle), y cos(zero angle), zw uv scale
    f32 uvo[4];                // xy uv offset
    u32 tex[4];                // bindless indices: base, normal, orm
    f32 ax[4], ay[4], az[4];   // unit world axes of box-local X (projection), Y (u), Z (v up)
};
static_assert(sizeof(PackedDecal) == 192, "PackedDecal is the HLSL AverDecalRec ABI");

inline f32 decalBitsToFloat(u32 v) { f32 f; std::memcpy(&f, &v, sizeof f); return f; }
inline u32 decalFloatToBits(f32 f) { u32 v; std::memcpy(&v, &f, sizeof v); return v; }

// The world-space centre and bounding-sphere radius of a decal's box.
inline void decalBounds(const SceneDecal& d, f32 centre[3], f32& radius) {
    for (int i = 0; i < 3; ++i) centre[i] = d.world[12 + i];
    f32 r2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const f32* row = &d.world[a * 4];
        const f32 len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        const f32 e = len * d.halfExtentsCm[a];
        r2 += e * e;
    }
    radius = std::sqrt(r2);
}

// 1 up to 60% of the fade distance, then a linear ramp to 0 at the fade distance. 0 = never fades.
inline f32 decalDistanceFade(f32 distanceCm, f32 fadeDistanceCm) {
    if (!(fadeDistanceCm > 0.0f)) return 1.0f;
    const f32 start = 0.6f * fadeDistanceCm;
    if (distanceCm <= start) return 1.0f;
    return std::max(0.0f, 1.0f - (distanceCm - start) / (fadeDistanceCm - start));
}

// Paint order: lower sortOrder first, then farther from the eye first, so a nearer decal of the same
// order lands on top. `distA/distB` are the centre distances.
inline bool decalPaintsBefore(i32 orderA, f32 distA, i32 orderB, f32 distB) {
    if (orderA != orderB) return orderA < orderB;
    return distA > distB;
}

// Packs a decal relative to the eye. The 3x3 is inverted in double and the translation folded in
// with the eye subtracted, so the shader's local = (wpos - eye) * A + t keeps its precision far from
// the origin (a decal a kilometre away would otherwise jitter by centimetres).
// False for a degenerate matrix or extents.
inline bool packSceneDecal(const SceneDecal& d, const f32 eye[3], const DecalTextureIndices& tx, PackedDecal& out) {
    f64 m[3][3];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m[r][c] = d.world[r * 4 + c];
    const f64 det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                  - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                  + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
    for (int i = 0; i < 3; ++i) if (!(d.halfExtentsCm[i] > 0.0f) || !std::isfinite(d.halfExtentsCm[i])) return false;

    const f64 inv = 1.0 / det;
    f64 a[3][3];   // inverse by cofactors
    a[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    a[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    a[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    a[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    a[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    a[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    a[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    a[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    a[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;

    // local = (p - t0) * A with p = eye + prel: translation = (eye - t0) * A.
    const f64 rel[3] = {static_cast<f64>(eye[0]) - d.world[12], static_cast<f64>(eye[1]) - d.world[13],
                        static_cast<f64>(eye[2]) - d.world[14]};
    f64 t[3];
    for (int j = 0; j < 3; ++j) t[j] = rel[0] * a[0][j] + rel[1] * a[1][j] + rel[2] * a[2][j];

    PackedDecal p{};
    f32* rows[3] = {p.r0, p.r1, p.r2};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) rows[i][j] = static_cast<f32>(a[i][j]);
        rows[i][3] = static_cast<f32>(t[i]);
    }

    u32 flags = 0;
    const bool hasBase = d.baseId != 0 && tx.base != kDecalUnboundTexture;
    const bool hasNormal = d.normal && d.normalId != 0 && tx.normal != kDecalUnboundTexture;
    const bool hasOrm = d.ormId != 0 && tx.orm != kDecalUnboundTexture;
    if (d.colour) flags |= kDecalPackColour;
    if (hasNormal) flags |= kDecalPackNormal | kDecalPackHasNormal;
    if (d.roughnessMetal) flags |= kDecalPackRough;
    if (hasBase) flags |= kDecalPackHasBase;
    if (hasOrm) flags |= kDecalPackHasOrm;
    if (hasBase || hasNormal || hasOrm) flags |= kDecalPackTextured;
    if (!(flags & (kDecalPackColour | kDecalPackNormal | kDecalPackRough))) return false;

    for (int i = 0; i < 3; ++i) p.ext[i] = d.halfExtentsCm[i];
    p.ext[3] = decalBitsToFloat(flags);
    for (int i = 0; i < 3; ++i) p.tint[i] = std::max(d.tint[i], 0.0f);
    p.tint[3] = std::min(std::max(d.opacity, 0.0f), 1.0f);
    p.shade[0] = d.normalStrength;
    p.shade[1] = d.roughness;
    p.shade[2] = d.metallic;
    p.shade[3] = std::min(std::max(d.edgeFade, 1e-3f), 1.0f);
    const f32 start = std::min(d.angleFadeStartDeg, 89.0f);
    const f32 end = std::max(std::min(d.angleFadeEndDeg, 90.0f), start + 0.5f);
    p.ang[0] = std::cos(start * kDegToRad);
    p.ang[1] = std::cos(end * kDegToRad);
    p.ang[2] = d.uvScale[0];
    p.ang[3] = d.uvScale[1];
    p.uvo[0] = d.uvOffset[0];
    p.uvo[1] = d.uvOffset[1];
    p.tex[0] = hasBase ? tx.base : kDecalUnboundTexture;
    p.tex[1] = hasNormal ? tx.normal : kDecalUnboundTexture;
    p.tex[2] = hasOrm ? tx.orm : kDecalUnboundTexture;
    p.tex[3] = 0;
    f32* axes[3] = {p.ax, p.ay, p.az};
    for (int i = 0; i < 3; ++i) {
        const f32* row = &d.world[i * 4];
        const f32 len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        const f32 s = len > 0.0f ? 1.0f / len : 0.0f;
        for (int j = 0; j < 3; ++j) axes[i][j] = row[j] * s;
    }
    out = p;
    return true;
}

// ---- CPU mirror of voxi_decal.hlsli's projector ----------------------------------------------------

struct DecalProjection {
    bool inside = false;     // the point is inside the box
    f32  weight = 0.0f;      // edge fade * angle fade (before opacity and image alpha)
    f32  u = 0.0f, v = 0.0f; // image coordinates after scale and offset
    Vec3 local{0, 0, 0};     // box-local position
};

// `prel` is the surface point minus the eye, `geoN` the unit geometric normal facing the viewer.
inline DecalProjection decalProject(const PackedDecal& d, const Vec3& prel, const Vec3& geoN) {
    DecalProjection o;
    o.local = Vec3{prel.x * d.r0[0] + prel.y * d.r1[0] + prel.z * d.r2[0] + d.r0[3],
                   prel.x * d.r0[1] + prel.y * d.r1[1] + prel.z * d.r2[1] + d.r1[3],
                   prel.x * d.r0[2] + prel.y * d.r1[2] + prel.z * d.r2[2] + d.r2[3]};
    const f32 qx = std::fabs(o.local.x) / d.ext[0];
    const f32 qy = std::fabs(o.local.y) / d.ext[1];
    const f32 qz = std::fabs(o.local.z) / d.ext[2];
    if (std::max(qx, std::max(qy, qz)) >= 1.0f) return o;
    o.inside = true;
    const f32 edge = std::max(d.shade[3], 1e-4f);
    auto sat = [](f32 x) { return std::min(std::max(x, 0.0f), 1.0f); };
    f32 w = std::min(sat((1.0f - qx) / edge), std::min(sat((1.0f - qy) / edge), sat((1.0f - qz) / edge)));
    const f32 c = -(geoN.x * d.ax[0] + geoN.y * d.ax[1] + geoN.z * d.ax[2]);
    w *= sat((c - d.ang[1]) / std::max(d.ang[0] - d.ang[1], 1e-4f));
    o.weight = w;
    o.u = (o.local.y / d.ext[1] * 0.5f + 0.5f) * d.ang[2] + d.uvo[0];
    o.v = (0.5f - o.local.z / d.ext[2] * 0.5f) * d.ang[3] + d.uvo[1];
    return o;
}

// The decal's tangent-space normal as a world-space normal on the surface it lands on: tangent along
// the decal's u axis, bitangent toward its v-up axis, both projected onto the surface.
inline Vec3 decalWorldNormal(const PackedDecal& d, const Vec3& nTS, const Vec3& geoN) {
    const Vec3 ay{d.ay[0], d.ay[1], d.ay[2]};
    const Vec3 az{d.az[0], d.az[1], d.az[2]};
    Vec3 T = ay - geoN * dot(geoN, ay);
    if (T.sizeSquared() < 1e-8f) T = az - geoN * dot(geoN, az);
    T = T.getSafeNormal();
    Vec3 B = cross(geoN, T);
    if (dot(B, az) < 0.0f) B = B * -1.0f;
    return (T * nTS.x + B * nTS.y + geoN * nTS.z).getSafeNormal();
}

// ---- decal images --------------------------------------------------------------------------------

enum class DecalImageKind : u8 {
    Colour,   // sRGB colour with coverage in alpha: averaged in linear light, weighted by alpha
    Normal,   // tangent-space normal: averaged then renormalised
    Data,     // roughness/metallic: plain average
};

inline u32 decalMipCount(u32 w, u32 h) {
    u32 n = 1;
    for (u32 m = std::max(w, h); m > 1; m >>= 1) ++n;
    return n;
}

inline f32 decalSrgbToLinear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline f32 decalLinearToSrgb(f32 c) {
    c = std::min(std::max(c, 0.0f), 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Builds the whole chain from a tightly packed RGBA8 image, level 0 first (a copy of the source).
// Each level halves with a 2x2 box, clamped at odd edges.
inline std::vector<std::vector<u8>> buildDecalMips(const u8* rgba, u32 w, u32 h, DecalImageKind kind) {
    std::vector<std::vector<u8>> levels;
    if (!rgba || w == 0 || h == 0) return levels;
    levels.emplace_back(rgba, rgba + static_cast<usize>(w) * h * 4u);
    u32 cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        const u32 nw = std::max(cw / 2, 1u), nh = std::max(ch / 2, 1u);
        const std::vector<u8>& src = levels.back();
        std::vector<u8> dst(static_cast<usize>(nw) * nh * 4u);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                const u32 x0 = std::min(x * 2, cw - 1), x1 = std::min(x * 2 + 1, cw - 1);
                const u32 y0 = std::min(y * 2, ch - 1), y1 = std::min(y * 2 + 1, ch - 1);
                const u8* px[4] = {&src[(static_cast<usize>(y0) * cw + x0) * 4], &src[(static_cast<usize>(y0) * cw + x1) * 4],
                                   &src[(static_cast<usize>(y1) * cw + x0) * 4], &src[(static_cast<usize>(y1) * cw + x1) * 4]};
                u8* o = &dst[(static_cast<usize>(y) * nw + x) * 4];
                if (kind == DecalImageKind::Colour) {
                    f32 a = 0.0f, sum[3] = {0, 0, 0};
                    for (const u8* p : px) {
                        const f32 pa = p[3] / 255.0f;
                        a += pa;
                        for (int c = 0; c < 3; ++c) sum[c] += decalSrgbToLinear(p[c] / 255.0f) * pa;
                    }
                    for (int c = 0; c < 3; ++c)
                        o[c] = static_cast<u8>(std::lround(255.0f * decalLinearToSrgb(a > 0.0f ? sum[c] / a : 0.0f)));
                    o[3] = static_cast<u8>(std::lround(255.0f * a * 0.25f));
                } else if (kind == DecalImageKind::Normal) {
                    f32 n[3] = {0, 0, 0};
                    for (const u8* p : px) for (int c = 0; c < 3; ++c) n[c] += p[c] / 255.0f * 2.0f - 1.0f;
                    const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    for (int c = 0; c < 3; ++c)
                        o[c] = static_cast<u8>(std::lround(255.0f * ((len > 1e-6f ? n[c] / len : (c == 2 ? 1.0f : 0.0f)) * 0.5f + 0.5f)));
                    o[3] = 255;
                } else {
                    for (int c = 0; c < 4; ++c)
                        o[c] = static_cast<u8>((px[0][c] + px[1][c] + px[2][c] + px[3][c] + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
        cw = nw; ch = nh;
    }
    return levels;
}

} // namespace aver::voxi
