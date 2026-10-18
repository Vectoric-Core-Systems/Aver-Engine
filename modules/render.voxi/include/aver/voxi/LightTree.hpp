#pragma once
// Light tree: a BVH over the frame's non-directional lights, for stochastic many-light direct lighting
// (docs/plans/wave4/MANY_LIGHTS.md section 3.1 to 3.3, docs/rendering/MANY_LIGHTS.md). Depends on Aver.Core only
// (the SceneLight.hpp precedent), so the whole structure is unit-testable without a device.
//
// The tree is packed one 80-byte record per node (TreeNodeRec, the AverLightRec size) after the light grid in the
// light buffer (t18). voxi_lighttree.hlsli reads it; the CPU twins below (importance, sample, pdfOf) are the
// functions the tests check against the light list's cheap irradiance. No light, reservoir or visibility is kept
// between pixels or frames: the tree is rebuilt from the current light set only (patent rule, section 2).
#include "aver/core/Types.hpp"
#include "aver/voxi/SceneLight.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace aver::voxi {

inline constexpr u32 kMaxTreeLights = 4000;     // == VoxiRenderer::kMaxListLights
inline constexpr u32 kLightTreeMaxDepth = 40;   // leaves sit at depth <= this; the shader's descent loop bound
inline constexpr u32 kTreeFlagLeaf = 1u;        // TreeNodeRec::flags bits (mirrored in voxi_lighttree.hlsli)
inline constexpr u32 kTreeFlagNoShadow = 2u;    // every light in the node casts no shadow
inline constexpr u32 kTreeFlagRect = 4u;        // some light in the node is a rectangle (keeps the 0.1 receiver floor)

// One tree node, as the GPU reads it. Child and parent fields are NODE indices (the node's place in the array,
// -1 = none), not buffer records: the header record carries the buffer record of node 0. A leaf has left = -1 and
// right = the light's index in the light list.
struct TreeNodeRec {
    f32 bmin[3], phi;           // AABB of the lights' centres (cm); power in irradiance-at-1-m units
    f32 bmax[3], range;         // AABB max; largest range (cm)
    f32 axis[3], cosThetaO;     // orientation cone: unit axis (zero length = omni) and cos of its half angle
    f32 cosThetaE;              // cos of the emission spread added to the cone (spot outer cosine, 0 rect, -1 omni)
    f32 left, right, parent;    // node indices; leaf: left = -1, right = list index
    f32 maxRadius;              // largest emitter radius (cm)
    f32 flags;                  // kTreeFlag* bits, exact in a float
    f32 spare[2];
};
static_assert(sizeof(TreeNodeRec) == 80, "TreeNodeRec is one AverLightRec-sized record");
static_assert(offsetof(TreeNodeRec, bmin) == 0 && offsetof(TreeNodeRec, bmax) == 16 && offsetof(TreeNodeRec, axis) == 32 &&
                  offsetof(TreeNodeRec, cosThetaE) == 48 && offsetof(TreeNodeRec, left) == 52 &&
                  offsetof(TreeNodeRec, parent) == 60 && offsetof(TreeNodeRec, maxRadius) == 64 &&
                  offsetof(TreeNodeRec, flags) == 68,
              "field offsets are the table in MANY_LIGHTS.md 3.1 (voxi_lighttree.hlsli reads them by float index)");

// The luminance of a light's cheap irradiance at p for a receiver with unit normal n: a CPU twin of
// voxi_rt.hlsli rdLightIrradianceCheap (range window, inverse square, N.L to the centre, spot cone). The tree's
// bound is checked against it.
inline f32 lightCheapLuminance(const PackedLight& l, const f32 p[3], const f32 n[3]) {
    const u32 kind = static_cast<u32>(l.axisKind[3] + 0.5f) % 8u;
    const f32 lum = 0.2126f * l.radianceRange[0] + 0.7152f * l.radianceRange[1] + 0.0722f * l.radianceRange[2];
    if (kind == kLightKindDirectional) {
        const f32 dn = l.axisKind[0] * n[0] + l.axisKind[1] * n[1] + l.axisKind[2] * n[2];
        const f32 len = std::sqrt(l.axisKind[0] * l.axisKind[0] + l.axisKind[1] * l.axisKind[1] + l.axisKind[2] * l.axisKind[2]);
        return lum * std::clamp(dn / std::max(len, 1e-8f), 0.0f, 1.0f);
    }
    const f32 tx = l.posRadius[0] - p[0], ty = l.posRadius[1] - p[1], tz = l.posRadius[2] - p[2];
    const f32 d2 = tx * tx + ty * ty + tz * tz;
    const f32 range = l.radianceRange[3];
    if (d2 >= range * range) return 0.0f;
    const f32 x2 = d2 / (range * range);
    const f32 win = std::clamp(1.0f - x2 * x2, 0.0f, 1.0f);
    const f32 invD = 1.0f / std::sqrt(std::max(d2, 1e-8f));
    f32 ndl = std::clamp((n[0] * tx + n[1] * ty + n[2] * tz) * invD, 0.0f, 1.0f);
    if (kind == kLightKindRect) {
        if (-(tx * l.axisKind[0] + ty * l.axisKind[1] + tz * l.axisKind[2]) <= 0.0f) return 0.0f;
        ndl = std::max(ndl, 0.1f);
    }
    if (kind == kLightKindSpot) {
        const f32 c = -(tx * l.axisKind[0] + ty * l.axisKind[1] + tz * l.axisKind[2]) * invD;
        if (c <= l.shape[1]) return 0.0f;
    }
    const f32 r = l.posRadius[3];
    return lum * ((1e4f / std::max(d2, r * r)) * (win * win) * ndl);
}

// The tree header record (first record of the tree tail), documented field by field in MANY_LIGHTS.md 3.1.
//   posRadius   x root node index (0)  y node count  z buffer record of node 0  w buffer record of the leaf map
//   radianceRange x default S  y default M  z hit M  w weight clamp (0 = off)
//   axisKind    x 1 when valid
// The leaf map is a float pool (20 per record) mapping a light-list index to its leaf node, so the estimator can
// find the exact light's tree probability.
struct TreeHeader {
    u32 nodeCount = 0;
    u32 nodeBaseRec = 0;
    u32 leafMapRec = 0;
    u32 samples = 1, candidates = 8, hitCandidates = 4;
    f32 weightClamp = 0.0f;
};
inline PackedLight packTreeHeader(const TreeHeader& h) {
    PackedLight p{};
    p.posRadius[0] = 0.0f;
    p.posRadius[1] = static_cast<f32>(h.nodeCount);
    p.posRadius[2] = static_cast<f32>(h.nodeBaseRec);
    p.posRadius[3] = static_cast<f32>(h.leafMapRec);
    p.radianceRange[0] = static_cast<f32>(h.samples);
    p.radianceRange[1] = static_cast<f32>(h.candidates);
    p.radianceRange[2] = static_cast<f32>(h.hitCandidates);
    p.radianceRange[3] = h.weightClamp;
    p.axisKind[0] = 1.0f;
    p.shape[2] = p.shape[3] = -1.0f;
    return p;
}

class LightTree {
public:
    // Builds over `count` non-directional lights in any order (the light list's order). False (and not valid) on a
    // NaN, infinite or negative power, no lights, or more than kMaxTreeLights.
    bool build(const PackedLight* lights, u32 count);
    // The same set of lights in another order: rewrites only the leaves' list indices (and the leaf map). False when
    // the set differs, or nothing was built.
    bool fill(const PackedLight* lights, u32 count);

    bool valid() const { return valid_; }
    u32 lightCount() const { return static_cast<u32>(canon_.size()); }
    u32 nodeCount() const { return static_cast<u32>(nodes_.size()); }
    const std::vector<TreeNodeRec>& nodes() const { return nodes_; }
    // Order-independent key of the light set (equal sets key equal; a changed light changes it).
    u64 key() const { return key_; }
    static u64 keyOf(const PackedLight* lights, u32 count);
    // The leaf node of a light-list index, or -1.
    i32 leafOfList(u32 listIndex) const { return listIndex < leafMap_.size() ? leafMap_[listIndex] : -1; }
    // Leaf map as the GPU reads it, indexed by light-list index (-1 for entries that are not tree lights).
    // `listCount` is the length of the whole light list (lights, then the sun).
    std::vector<f32> leafMapFloats(u32 listCount) const;
    // Deepest leaf (root = 0).
    u32 depth() const;

    // ---- CPU twins of voxi_lighttree.hlsli ----
    // Conservative bound of a node's lights' cheap irradiance luminance at p (unit normal n).
    static f32 importance(const TreeNodeRec& node, const f32 p[3], const f32 n[3]);
    // Descends from the root choosing a child in proportion to its importance; u in [0,1). False when the root
    // (or every path) has zero importance. `pdf` is the probability of the returned light.
    bool sample(const f32 p[3], const f32 n[3], f32 u, u32& listIndex, f32& pdf) const;
    // The probability sample() returns the light at leaf node `leaf` with, walked up through the parent pointers.
    f32 pdfOf(u32 leaf, const f32 p[3], const f32 n[3]) const;

private:
    bool buildOver(const PackedLight* lights, u32 count, const std::vector<u32>& order);
    u32 buildRange(u32 lo, u32 hi, u32 depth, u32 parent);
    void rewriteLeaves(const std::vector<u32>& order);

    bool valid_ = false;
    u64 key_ = 0;
    std::vector<PackedLight> canon_;       // the lights in canonical order
    std::vector<TreeNodeRec> leafRecs_;    // per canonical id: its leaf, list index filled by rewriteLeaves
    std::vector<u32> morton_;              // per canonical id (sorted by Morton code before the build)
    std::vector<u32> sorted_;              // canonical ids in Morton order, the build's working range
    std::vector<u32> leafOfCanon_;         // canonical id -> leaf node
    std::vector<i32> leafMap_;             // list index -> leaf node (-1 none)
    std::vector<TreeNodeRec> nodes_;
};

} // namespace aver::voxi
