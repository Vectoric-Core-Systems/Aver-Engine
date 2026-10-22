#pragma once
// RibbonPack -- the pure half of the strand-hair draw: the static ribbon layout of a groom and the C++ twin of AverHairRibbons.hlsl.
// docs/plans/wave11/SOFTBODY_HAIR.md 2.16.
//
// THE LAYOUT. Jolt hands back one position per render vertex of every render strand. The draw wants, per strand, a row of ribbon points (two vertices
// each, left and right) and an index buffer joining neighbouring points, in an order where ANY PREFIX of strands is a uniform thinning of the groom:
// the strands are put in a seeded random order (their RANK), points and indices follow that order, and the four LOD levels are index prefixes of 100%,
// 50%, 25% and 12% of the strands (HairLod.hpp's fractions). A level is therefore one index range of the same vertex buffer, which is what
// IDevice::createPosedPartMesh builds. buildRibbonLayout is deterministic: the same strand table, widths and seed give the same layout, bit for bit.
//
// THE TWIN. ribbonVertices() is AverHairRibbons.hlsl's CSRibbons for one point, line for line (the discipline anim::skinVertices keeps with skin.hlsl):
// the camera-facing side, the one-pixel minimum width with its coverage, the stochastic keep, the uv encoding. RibbonPackTest pins its properties; a
// change to one side is a change to both.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace aver::render {

inline constexpr u32 kRibbonLodLevels = 4;

// One ribbon point, 28 bytes: StructuredBuffer<RibbonPoint> in AverHairRibbons.hlsl.
struct RibbonPoint {
    u32 vtx = 0;          // render vertex index of this point
    u32 prev = 0;         // the point before it (itself at the root)
    u32 next = 0;         // and after it (itself at the tip)
    f32 fraction = 0.0f;  // 0 root .. 1 tip
    f32 widthCm = 0.0f;   // width before scaling
    u32 strandHash = 0;   // low 8 bits colour the strand; all 32 drive the sub-pixel keep
    f32 rankFrac = 0.0f;  // rank / strand count
};
static_assert(sizeof(RibbonPoint) == 28, "RibbonPoint mirrors the HLSL struct");

// cbuffer RibbonCB in AverHairRibbons.hlsl (b3), 48 bytes.
struct RibbonConstants {
    f32 cam[3] = {0, 0, 0};   // camera in hair-local engine centimetres
    f32 pixelAt1cm = 0.0f;    // local cm per pixel per cm of distance
    u32 pointCount = 0;
    u32 frameSeed = 0;
    f32 widthScale = 1.0f;
    f32 minPixels = 1.0f;
    f32 keepRank = 1.0f;
    f32 pad[3] = {0, 0, 0};
};
static_assert(sizeof(RibbonConstants) == 48, "RibbonConstants mirrors the HLSL cbuffer");

struct RibbonInput {
    const i32* strandStartCount2 = nullptr;   // {first render vertex, vertex count} per strand, in render buffer order
    u32 strandCount = 0;
    const f32* widthRootCm = nullptr;         // per strand
    const f32* widthTipCm = nullptr;
    u64 seed = 1;                             // the rank shuffle and the strand hashes derive from it (the asset hash)
    f32 lodFractions[kRibbonLodLevels] = {1.0f, 0.5f, 0.25f, 0.12f};
};

struct RibbonLayout {
    std::vector<RibbonPoint> points;          // strands in rank order
    std::vector<u32> indices;                 // 6 per segment, same order
    std::vector<u32> rankToStrand;            // strand index (render buffer order) of each rank
    u32 lodStrands[kRibbonLodLevels] = {};
    u32 lodPoints[kRibbonLodLevels] = {};     // points in the first lodStrands[l] strands: the dispatch size of a level
    u32 lodIndices[kRibbonLodLevels] = {};    // indices of those strands: the index count of the level's part mesh
    u32 strandCount = 0;
    u32 vertexCount() const { return static_cast<u32>(points.size()) * 2u; }
};

// False with `why` for a strand table the draw cannot use (no strands, a strand under 2 points, a vertex index range that is not contiguous).
bool buildRibbonLayout(const RibbonInput& in, RibbonLayout& out, std::string* why = nullptr);

// ---- the twin of CSRibbons ----

// The hair-local engine position (centimetres) of a Jolt render position (metres, Y up): the inverse of Convert.hpp's toJolt, scaled.
inline Vec3 ribbonJoltToLocalCm(const f32 j[3]) { return Vec3(-j[2] * 100.0f, j[0] * 100.0f, j[1] * 100.0f); }

// The hashes of the shader. hairMix is a 32-bit finaliser; hairUnit maps it to [0, 1) from its top 24 bits.
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
    f32 widthScale = 1.0f;     // CHair::widthScale times the level's boost
    f32 minPixels = 1.0f;
    u32 frameSeed = 0;
    f32 keepRank = 1.0f;
};

struct RibbonResult {
    f32 widthDrawn = 0.0f;     // W: the ribbon width written, centimetres (0 when the strand is dropped)
    f32 coverage = 1.0f;       // w / max(w, one pixel)
    bool dropped = false;      // collapsed by the rank rule or the stochastic keep
    Vec3 tangent;
    Vec3 side;
};

// One point's two vertices. pJ / prevJ / nextJ are Jolt render positions of the point and its neighbours.
RibbonResult ribbonVertices(const RibbonPoint& rp, const f32 pJ[3], const f32 prevJ[3], const f32 nextJ[3], const RibbonCamera& cam,
                            rhi::MeshVertex out[2]);

// uv.y as the shader writes it, and back (the pixel shader's decode): the strand's 8-bit hash in the integer part, the root-to-tip fraction in the rest.
inline f32 ribbonEncodeV(u32 strandHash, f32 fraction) {
    const f32 f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    return static_cast<f32>(strandHash & 255u) + f * 0.999f;
}
inline void ribbonDecodeV(f32 v, u32& hash8, f32& fraction) {
    const f32 fl = std::floor(v);
    hash8 = static_cast<u32>(fl);
    fraction = (v - fl) / 0.999f;
}

// Pixel footprint of the view at 1 cm distance for a perspective projection: 2 tan(fovY / 2) / viewport height in pixels.
inline f32 ribbonPixelAt1cm(f32 fovYRadians, f32 viewportHeightPx) {
    return viewportHeightPx > 0.0f ? 2.0f * std::tan(fovYRadians * 0.5f) / viewportHeightPx : 0.0f;
}

// Fewer, fatter strands keep the coverage: the width boost of a level that draws `fractionDrawn` of the strands, 1 at 1 and growing as the square
// root of its reciprocal, capped (a 1% level would otherwise draw tape).
inline f32 ribbonLodWidthBoost(f32 fractionDrawn) {
    const f32 f = fractionDrawn < 0.05f ? 0.05f : (fractionDrawn > 1.0f ? 1.0f : fractionDrawn);
    return std::sqrt(1.0f / f);
}

} // namespace aver::render
