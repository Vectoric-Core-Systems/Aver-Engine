#pragma once
// RibbonPack: static ribbon layout of a strand-hair groom, and the C++ twin of AverHairRibbons.hlsl (docs/plans/wave11/SOFTBODY_HAIR.md 2.16).
// Strands are ordered by a seeded rank, so each LOD (100/50/25/12% of strands) is an index prefix and one index range of the same vertex buffer.
// ribbonVertices mirrors CSRibbons line for line; RibbonPackTest pins its properties. A change to one side is a change to both.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace aver::render {

inline constexpr u32 kRibbonLodLevels = 4;

// 28 bytes; mirrors StructuredBuffer<RibbonPoint> in AverHairRibbons.hlsl.
struct RibbonPoint {
    u32 vtx = 0;
    u32 prev = 0;         // the point itself at the root
    u32 next = 0;         // the point itself at the tip
    f32 fraction = 0.0f;
    f32 widthCm = 0.0f;
    u32 strandHash = 0;   // low 8 bits colour the strand; all 32 drive the sub-pixel keep
    f32 rankFrac = 0.0f;
};
static_assert(sizeof(RibbonPoint) == 28, "RibbonPoint mirrors the HLSL struct");

// 48 bytes; mirrors cbuffer RibbonCB (b3) in AverHairRibbons.hlsl.
struct RibbonConstants {
    f32 cam[3] = {0, 0, 0};
    f32 pixelAt1cm = 0.0f;
    u32 pointCount = 0;
    u32 frameSeed = 0;
    f32 widthScale = 1.0f;
    f32 minPixels = 1.0f;
    f32 keepRank = 1.0f;
    f32 pad[3] = {0, 0, 0};
};
static_assert(sizeof(RibbonConstants) == 48, "RibbonConstants mirrors the HLSL cbuffer");

struct RibbonInput {
    const i32* strandStartCount2 = nullptr;   // {first render vertex, vertex count} per strand, render buffer order
    u32 strandCount = 0;
    const f32* widthRootCm = nullptr;
    const f32* widthTipCm = nullptr;
    u64 seed = 1;                             // the rank shuffle and strand hashes derive from it
    f32 lodFractions[kRibbonLodLevels] = {1.0f, 0.5f, 0.25f, 0.12f};
};

struct RibbonLayout {
    std::vector<RibbonPoint> points;          // strands in rank order
    std::vector<u32> indices;                 // 6 per segment, same order
    std::vector<u32> rankToStrand;
    u32 lodStrands[kRibbonLodLevels] = {};
    u32 lodPoints[kRibbonLodLevels] = {};     // over the first lodStrands[l] strands: dispatch size
    u32 lodIndices[kRibbonLodLevels] = {};    // index count of that level's part mesh
    u32 strandCount = 0;
    u32 vertexCount() const { return static_cast<u32>(points.size()) * 2u; }
};

// False with `why` for a strand table the draw cannot use (no strands, under 2 points, non-contiguous vertex range).
bool buildRibbonLayout(const RibbonInput& in, RibbonLayout& out, std::string* why = nullptr);

// Jolt render position (metres, Y up) to hair-local engine cm; the inverse of Convert.hpp's toJolt, scaled.
inline Vec3 ribbonJoltToLocalCm(const f32 j[3]) { return Vec3(-j[2] * 100.0f, j[0] * 100.0f, j[1] * 100.0f); }

// Shader hashes (the twin of hairMix / hairUnit): a 32-bit finaliser, and [0, 1) from its top 24 bits.
inline u32 ribbonMix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline f32 ribbonUnit(u32 h) { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }

struct RibbonCamera {
    Vec3 camLocal;
    f32 pixelAt1cm = 0.0f;
    f32 widthScale = 1.0f;
    f32 minPixels = 1.0f;
    u32 frameSeed = 0;
    f32 keepRank = 1.0f;
};

struct RibbonResult {
    f32 widthDrawn = 0.0f;     // W in cm; 0 when the strand is dropped
    f32 coverage = 1.0f;
    bool dropped = false;      // collapsed by the rank rule or the stochastic keep
    Vec3 tangent;
    Vec3 side;
};

// Jolt render positions of the point (pJ) and its neighbours (prevJ, nextJ); writes the two vertices.
RibbonResult ribbonVertices(const RibbonPoint& rp, const f32 pJ[3], const f32 prevJ[3], const f32 nextJ[3], const RibbonCamera& cam,
                            rhi::MeshVertex out[2]);

// uv.y as the shader writes it, and ribbonDecodeV reads it back: strand hash in the integer part, root-to-tip fraction in the rest.
inline f32 ribbonEncodeV(u32 strandHash, f32 fraction) {
    const f32 f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    return static_cast<f32>(strandHash & 255u) + f * 0.999f;
}
inline void ribbonDecodeV(f32 v, u32& hash8, f32& fraction) {
    const f32 fl = std::floor(v);
    hash8 = static_cast<u32>(fl);
    fraction = (v - fl) / 0.999f;
}

inline f32 ribbonPixelAt1cm(f32 fovYRadians, f32 viewportHeightPx) {
    return viewportHeightPx > 0.0f ? 2.0f * std::tan(fovYRadians * 0.5f) / viewportHeightPx : 0.0f;
}

// Fewer strands get fatter (sqrt of 1 / fraction) to keep coverage; capped so a 1% level does not draw tape.
inline f32 ribbonLodWidthBoost(f32 fractionDrawn) {
    const f32 f = fractionDrawn < 0.05f ? 0.05f : (fractionDrawn > 1.0f ? 1.0f : fractionDrawn);
    return std::sqrt(1.0f / f);
}

} // namespace aver::render
