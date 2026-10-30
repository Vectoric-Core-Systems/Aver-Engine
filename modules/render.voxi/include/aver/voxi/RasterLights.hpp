#pragma once
// Raster local lights: the host half of the omni/spot path that needs no ray tracing and runs on Vulkan. Pure and header-only so
// the tests run it without a device: culling, ranking, the capped light list, the parametric curves (linear window, decay exponent,
// cone-edge exponent), the shadow-tile budget and the tile matrices. voxi.hlsl (AVER_LOCAL_RASTER) reads `Block`; the constants below
// are mirrored there (RasterLightsTest checks the text).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/voxi/SceneLight.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace aver::voxi::rl {

inline constexpr u32 kMaxLights = 32;        // AVER_RL_MAX_LIGHTS
inline constexpr u32 kLightVecs = 7;         // AVER_RL_LIGHT_VECS: float4s per light
inline constexpr u32 kMaxTiles = 24;         // AVER_RL_MAX_TILES: shadow tiles per frame (a spot is 1, an omni 6)
inline constexpr u32 kMaxShadowLights = 8;
inline constexpr u32 kCellPx = 256;          // the atlas is a grid of 256 px cells
inline constexpr f32 kMaxSpotShadowHalfAngleDeg = 75.0f;   // a wider cone is shadowed with a cube
inline constexpr f32 kShadowNearCm = 3.0f;

// Light flag bits, in the float packed at vec 3 .w (AVER_RL_F_*).
inline constexpr u32 kFlagParametric = 1, kFlagNegative = 2, kFlagShadowed = 4, kFlagCube = 8;

// Two render-layer masks stored inverted (0 = every layer, 20 bits) share a layer: the same rule as aver::scene::masksOverlap (RenderMasks.hpp),
// repeated here because the renderer does not link the scene module. A test pins the two together.
inline constexpr bool layersOverlap(u32 a, u32 b) { return ((~a) & (~b) & 0xFFFFFu) != 0; }

// The default shadow bias / normal bias of a parametric light that stores 0, and how the bias maps to Aver world units.
inline constexpr f32 kParametricDefaultShadowBias = 0.1f, kParametricDefaultNormalBias = 2.0f;
inline constexpr f32 kBiasCmPerUnit = 10.0f;   // bias 0.1 -> 1 cm pushed toward the light; approximate (the unit is not a length)

// What the shader reads, appended to the VoxiFrame cbuffer after the engine's own fields. Float4-aligned throughout.
struct Block {
    f32 header[4];                        // x = lights, y = tiles, z = 1 / atlas pixels, w = 1 when the atlas is bound
    f32 light[kMaxLights][kLightVecs][4];
    f32 tileVP[kMaxTiles][16];            // row-major, the sun cascades' convention (memcpy of a Mat4)
    f32 tileUv[kMaxTiles][4];             // xy = atlas offset, zw = size, all in [0,1]
};
static_assert(sizeof(Block) == 16 + kMaxLights * kLightVecs * 16 + kMaxTiles * 64 + kMaxTiles * 16 && sizeof(Block) % 16 == 0,
              "Block is the HLSL tail of cbuffer VoxiFrame under AVER_LOCAL_RASTER");

// ---- curves (CPU mirror of voxi_locallights_raster.hlsli) ------------------------------------------------------------------------------

// Aver's window, (1 - (d/R)^4)^2, and the parametric model's linear one, 1 - d/R. Distances in the same unit.
inline f32 windowAver(f32 d, f32 range) {
    const f32 x = d / std::max(range, 1e-3f);
    const f32 w = std::min(std::max(1.0f - x * x * x * x, 0.0f), 1.0f);
    return w * w;
}
inline f32 windowLinear(f32 d, f32 range) { return std::max(1.0f - d / std::max(range, 1e-3f), 0.0f); }

// Factor on the irradiance at 1 m at distance dCm: Aver = (100/max(d, r))^2 * window; parametric = window * (1 m / max(d, r))^decay.
inline f32 distanceFactor(f32 dCm, f32 rangeCm, f32 radiusCm, bool parametric, f32 decay) {
    const f32 d = std::max(dCm, std::max(radiusCm, 0.01f));
    if (dCm >= rangeCm) return 0.0f;
    if (!parametric) return windowAver(dCm, rangeCm) * (100.0f / d) * (100.0f / d);
    return windowLinear(dCm, rangeCm) * std::pow(100.0f / d, decay);
}

// Spot cone factor for `cosAxis` = dot(light->receiver, axis). Aver: smoothstep inner..outer. Parametric: 1 - rim^(1/angleAttenuation).
inline f32 coneFactor(f32 cosAxis, f32 cosOuter, f32 cosInner, bool parametric, f32 angleAttenuation) {
    if (cosAxis <= cosOuter) return 0.0f;
    if (!parametric) {
        const f32 t = std::min(std::max((cosAxis - cosOuter) / std::max(cosInner - cosOuter, 1e-3f), 0.0f), 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    const f32 rim = std::max(1e-4f, (1.0f - cosAxis) / std::max(1.0f - cosOuter, 1e-6f));
    return 1.0f - std::pow(rim, 1.0f / std::max(angleAttenuation, 1e-3f));
}

// Light fade by camera distance: 1 until `begin`, 0 at begin + length; no fade when both are 0.
inline f32 lightFade(f32 distCm, f32 beginCm, f32 lengthCm) {
    if (!(beginCm > 0.0f) && !(lengthCm > 0.0f)) return 1.0f;
    if (!(lengthCm > 0.0f)) return distCm <= beginCm ? 1.0f : 0.0f;
    return std::min(std::max(1.0f - (distCm - beginCm) / lengthCm, 0.0f), 1.0f);
}
// Shadow weight by camera distance: 1 until `shadowCm` (0 = begin), then it fades out over lengthCm (a tenth of the distance when 0).
inline f32 shadowFade(f32 distCm, f32 beginCm, f32 lengthCm, f32 shadowCm) {
    if (!(beginCm > 0.0f) && !(lengthCm > 0.0f) && !(shadowCm > 0.0f)) return 1.0f;
    const f32 start = shadowCm > 0.0f ? shadowCm : beginCm;
    const f32 span = lengthCm > 0.0f ? lengthCm : std::max(start * 0.1f, 1.0f);
    return std::min(std::max(1.0f - (distCm - start) / span, 0.0f), 1.0f);
}

// ---- the range a light reaches ---------------------------------------------------------------------------------------------------------

inline f32 effectiveRangeCm(const SceneLight& s) {
    f32 range = s.rangeCm;
    if (!(range > 0.0f)) {
        const f32 e1m = pointIrradiance1m(s.intensityCd);
        const f32 maxColour = std::max(s.colour[0], std::max(s.colour[1], s.colour[2]));
        const f32 reach = 100.0f * std::sqrt(std::max(e1m * maxColour, 0.0f) / kLightRangeCutoff);
        range = std::min(std::max(reach, std::max(s.radiusCm, 1.0f) * 4.0f), kLightAutoRangeMaxCm);
    }
    return std::min(range, kLightRangeMaxCm);
}

// ---- frustum ----------------------------------------------------------------------------------------------------------------------------

struct Frustum {
    f32 plane[6][4] = {};
    bool valid = false;
};

// From a row-major, row-vector view-projection with depth [0,1] (the engine's camera convention). Planes are normalised, inside = positive.
inline Frustum frustumFromViewProj(const f32 vp[16]) {
    Frustum f;
    const auto col = [&](int c, f32 out[4]) { for (int r = 0; r < 4; ++r) out[r] = vp[r * 4 + c]; };
    f32 c0[4], c1[4], c2[4], c3[4];
    col(0, c0); col(1, c1); col(2, c2); col(3, c3);
    for (int i = 0; i < 4; ++i) {
        f.plane[0][i] = c3[i] + c0[i];   // left
        f.plane[1][i] = c3[i] - c0[i];   // right
        f.plane[2][i] = c3[i] + c1[i];   // bottom
        f.plane[3][i] = c3[i] - c1[i];   // top
        f.plane[4][i] = c2[i];           // near (depth >= 0)
        f.plane[5][i] = c3[i] - c2[i];   // far
    }
    for (auto& p : f.plane) {
        const f32 n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (!(n > 1e-9f)) return f;
        for (int i = 0; i < 4; ++i) p[i] /= n;
    }
    f.valid = true;
    return f;
}
inline bool sphereInFrustum(const Frustum& f, const f32 c[3], f32 r) {
    if (!f.valid) return true;
    for (const auto& p : f.plane)
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r) return false;
    return true;
}

// ---- the shadow atlas: a grid of cells, power-of-two squares, first fit ------------------------------------------------------------------

class AtlasPacker {
public:
    static constexpr u32 kGrid = 16;   // 16 x 16 cells; with kCellPx = 256 that is a 4096 px atlas
    void clear() { std::memset(occ_, 0, sizeof(occ_)); used_ = 0; }
    // A square of `cells` x `cells` (1, 2 or 4) placed on a multiple of its own size; false when there is no room.
    bool alloc(u32 cells, u32& cx, u32& cy) {
        if (cells == 0 || cells > kGrid) return false;
        for (u32 y = 0; y + cells <= kGrid; y += cells)
            for (u32 x = 0; x + cells <= kGrid; x += cells)
                if (free(x, y, cells)) { mark(x, y, cells, true); cx = x; cy = y; used_ += cells * cells; return true; }
        return false;
    }
    void release(u32 cx, u32 cy, u32 cells) { mark(cx, cy, cells, false); used_ -= cells * cells; }
    u32 usedCells() const { return used_; }
    bool occupied(u32 x, u32 y) const { return occ_[y][x]; }

private:
    bool free(u32 x, u32 y, u32 n) const {
        for (u32 j = 0; j < n; ++j) for (u32 i = 0; i < n; ++i) if (occ_[y + j][x + i]) return false;
        return true;
    }
    void mark(u32 x, u32 y, u32 n, bool v) { for (u32 j = 0; j < n; ++j) for (u32 i = 0; i < n; ++i) occ_[y + j][x + i] = v; }
    bool occ_[kGrid][kGrid] = {};
    u32 used_ = 0;
};

// ---- shadow tile matrices ------------------------------------------------------------------------------------------------------------------

// Face order of a cube shadow, shared with the shader's face pick: +X, -X, +Y, -Y, +Z, -Z.
inline Vec3 faceForward(u32 face) {
    switch (face) {
    case 0: return {1, 0, 0};
    case 1: return {-1, 0, 0};
    case 2: return {0, 1, 0};
    case 3: return {0, -1, 0};
    case 4: return {0, 0, 1};
    default: return {0, 0, -1};
    }
}
inline Mat4 faceViewProj(const Vec3& pos, u32 face, f32 nearCm, f32 farCm) {
    const Vec3 fwd = faceForward(face);
    const Vec3 up = face >= 4 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    return Mat4::lookAtDirLH(pos, fwd, up) * Mat4::perspectiveLH(1.5707963268f, 1.0f, nearCm, farCm);
}
inline Mat4 spotViewProj(const Vec3& pos, const Vec3& axis, f32 halfAngleRad, f32 nearCm, f32 farCm) {
    const Vec3 up = std::fabs(axis.z) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    // A few percent wider than the cone so the filter's taps at the edge still find depth.
    const f32 fov = 2.0f * std::atan(std::tan(halfAngleRad) * 1.04f);
    return Mat4::lookAtDirLH(pos, axis, up) * Mat4::perspectiveLH(fov, 1.0f, nearCm, farCm);
}
// The cube face a world offset (receiver - light) falls in; the same rule as the shader's.
inline u32 cubeFaceOf(const Vec3& d) {
    const f32 ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) return d.x >= 0.0f ? 0u : 1u;
    if (ay >= az) return d.y >= 0.0f ? 2u : 3u;
    return d.z >= 0.0f ? 4u : 5u;
}

// ---- building the frame's list --------------------------------------------------------------------------------------------------------------

struct Tile {
    Mat4 viewProj;
    u32  cx = 0, cy = 0, cells = 0;   // atlas cell origin and size in cells
    u32  light = 0;                   // index into the packed list
    u32  face = 0;                    // cube face (0 for a spot)
    u32  pixels() const { return cells * kCellPx; }
};

struct Stats {
    u32 considered = 0;     // point and spot lights offered
    u32 skippedKind = 0;    // rectangles: not lit by this path
    u32 faded = 0;          // distance fade took them out
    u32 culled = 0;         // range sphere outside the frustum
    u32 overCap = 0;        // dropped for the kMaxLights cap
    u32 shadowed = 0;
    u32 shadowDenied = 0;   // wanted a shadow, no tile budget left
};

struct Input {
    const SceneLight* lights = nullptr;
    u32 count = 0;
    f32 eye[3] = {0, 0, 0};
    const f32* viewProj = nullptr;   // 16 floats, or null for no frustum culling
    bool shadows = true;             // false when the atlas / depth pipelines do not exist
};

struct Result {
    Block block;
    std::vector<Tile> tiles;
    Stats stats;
    u32 lightCount = 0;
    std::vector<u32> sourceIndex;    // sourceIndex[i] = index into Input::lights of packed light i
};

inline f32 importanceOf(const SceneLight& s, f32 distCm, f32 fade) {
    const f32 maxColour = std::max(s.colour[0], std::max(s.colour[1], s.colour[2]));
    const f32 e1m = pointIrradiance1m(s.intensityCd) * maxColour * fade;
    const f32 d = std::max(distCm, 50.0f);
    return e1m * 1e4f / (d * d);
}

inline void build(const Input& in, Result& out) {
    out = Result{};
    std::memset(&out.block, 0, sizeof(out.block));
    struct Cand { u32 idx; f32 score, fade, dist; };
    std::vector<Cand> cands;
    const Frustum fr = in.viewProj ? frustumFromViewProj(in.viewProj) : Frustum{};
    for (u32 i = 0; i < in.count; ++i) {
        const SceneLight& s = in.lights[i];
        if (s.kind != 0 && s.kind != 1) { ++out.stats.skippedKind; continue; }
        ++out.stats.considered;
        const f32 dx = s.pos[0] - in.eye[0], dy = s.pos[1] - in.eye[1], dz = s.pos[2] - in.eye[2];
        const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const f32 fade = lightFade(dist, s.fadeBeginCm, s.fadeLengthCm);
        if (!(fade > 0.0f) || !(s.intensityCd > 0.0f)) { ++out.stats.faded; continue; }
        if (!sphereInFrustum(fr, s.pos, effectiveRangeCm(s))) { ++out.stats.culled; continue; }
        cands.push_back({i, importanceOf(s, dist, fade), fade, dist});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if (cands.size() > kMaxLights) { out.stats.overCap = static_cast<u32>(cands.size()) - kMaxLights; cands.resize(kMaxLights); }

    AtlasPacker packer;
    u32 shadowRank = 0;
    for (u32 li = 0; li < cands.size(); ++li) {
        const Cand& c = cands[li];
        const SceneLight& s = in.lights[c.idx];
        const bool spot = s.kind == 1;
        const f32 range = effectiveRangeCm(s);
        const f32 e1m = pointIrradiance1m(s.intensityCd) * c.fade;
        f32 (*v)[4] = out.block.light[li];

        v[0][0] = s.pos[0]; v[0][1] = s.pos[1]; v[0][2] = s.pos[2]; v[0][3] = std::max(s.radiusCm, 1.0f);
        for (int k = 0; k < 3; ++k) v[1][k] = s.colour[k] * e1m;
        v[1][3] = range;
        for (int k = 0; k < 3; ++k) v[2][k] = s.axis[k];
        v[2][3] = spot ? 1.0f : 0.0f;
        const f32 decay = s.parametric ? std::max(s.attenuation, 0.0f) : 2.0f;
        const f32 outerCos = spot ? s.outerCos : -1.0f;
        v[3][0] = decay;
        v[3][1] = outerCos;
        v[3][2] = s.parametric ? 1.0f / std::max(s.spotAttenuation, 1e-3f) : std::min(std::max(s.innerCos, outerCos + 1e-3f), 1.0f);
        u32 flags = (s.parametric ? kFlagParametric : 0u) | (s.negative ? kFlagNegative : 0u);
        v[4][0] = s.parametric ? s.specularScale : 1.0f;
        v[4][1] = 0.0f;
        v[4][2] = static_cast<f32>(s.cullInv & 0xFFFFFu);          // light cull mask, inverted: the shader skips an instance it excludes
        v[4][3] = static_cast<f32>(s.shadowCasterInv & 0xFFFFFu);  // shadow_caster_mask, inverted: read by the CPU shadow pass only

        // Shadow tiles: the light must cast, the shadow must not have faded out, and the atlas must have room.
        const f32 sf = shadowFade(c.dist, s.fadeBeginCm, s.fadeLengthCm, s.fadeShadowCm);
        const bool wantsShadow = in.shadows && s.castShadows && sf > 0.0f && shadowRank < kMaxShadowLights;
        if (wantsShadow) {
            const f32 halfAngle = spot ? std::acos(std::min(std::max(s.outerCos, -1.0f), 1.0f)) : 0.0f;
            const bool cube = !spot || halfAngle > kMaxSpotShadowHalfAngleDeg * 0.017453292f;
            const u32 faces = cube ? 6u : 1u;
            const u32 cells = cube ? (shadowRank < 2 ? 2u : 1u) : (shadowRank < 2 ? 4u : 2u);
            if (out.tiles.size() + faces <= kMaxTiles) {
                u32 cx[6], cy[6], got = 0;
                for (; got < faces; ++got) if (!packer.alloc(cells, cx[got], cy[got])) break;
                if (got == faces) {
                    const u32 base = static_cast<u32>(out.tiles.size());
                    const Vec3 pos{s.pos[0], s.pos[1], s.pos[2]};
                    const f32 tileTexels = static_cast<f32>(cells * kCellPx);
                    for (u32 f = 0; f < faces; ++f) {
                        Tile t;
                        t.viewProj = cube ? faceViewProj(pos, f, kShadowNearCm, range)
                                          : spotViewProj(pos, Vec3{s.axis[0], s.axis[1], s.axis[2]}, halfAngle, kShadowNearCm, range);
                        t.cx = cx[f]; t.cy = cy[f]; t.cells = cells; t.light = li; t.face = f;
                        out.tiles.push_back(t);
                    }
                    flags |= kFlagShadowed | (cube ? kFlagCube : 0u);
                    v[4][1] = sf;
                    v[5][0] = static_cast<f32>(base);
                    v[5][1] = static_cast<f32>(faces);
                    v[5][2] = (s.shadowBias > 0.0f ? s.shadowBias : kParametricDefaultShadowBias) * kBiasCmPerUnit;
                    v[5][3] = s.shadowNormalBias > 0.0f ? s.shadowNormalBias : kParametricDefaultNormalBias;
                    const f32 halfTan = cube ? 1.0f : std::tan(halfAngle) * 1.04f;
                    v[6][0] = 2.0f * halfTan / tileTexels;                         // world size of a texel per unit distance
                    v[6][1] = 1.0f + std::min(s.radiusCm * 0.1f, 3.0f);            // filter radius in texels (light_size)
                    v[6][2] = kShadowNearCm;
                    v[6][3] = range;
                    ++shadowRank;
                    ++out.stats.shadowed;
                } else {
                    for (u32 g = 0; g < got; ++g) packer.release(cx[g], cy[g], cells);
                    ++out.stats.shadowDenied;
                }
            } else {
                ++out.stats.shadowDenied;
            }
        }
        v[3][3] = static_cast<f32>(flags);
        out.sourceIndex.push_back(c.idx);
    }
    out.lightCount = static_cast<u32>(cands.size());

    constexpr f32 kAtlasPx = static_cast<f32>(AtlasPacker::kGrid * kCellPx);
    for (u32 t = 0; t < out.tiles.size(); ++t) {
        std::memcpy(out.block.tileVP[t], &out.tiles[t].viewProj.m[0][0], sizeof(out.block.tileVP[t]));
        const Tile& tile = out.tiles[t];
        out.block.tileUv[t][0] = static_cast<f32>(tile.cx * kCellPx) / kAtlasPx;
        out.block.tileUv[t][1] = static_cast<f32>(tile.cy * kCellPx) / kAtlasPx;
        out.block.tileUv[t][2] = static_cast<f32>(tile.cells * kCellPx) / kAtlasPx;
        out.block.tileUv[t][3] = static_cast<f32>(tile.cells * kCellPx) / kAtlasPx;
    }
    out.block.header[0] = static_cast<f32>(out.lightCount);
    out.block.header[1] = static_cast<f32>(out.tiles.size());
    out.block.header[2] = 1.0f / kAtlasPx;
    out.block.header[3] = 0.0f;
}

}  // namespace aver::voxi::rl
