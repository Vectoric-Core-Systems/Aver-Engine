#pragma once
// The landscape quadtree: which chunks to draw, at which detail, this frame.
// Pure CPU -- no rhi:: types, no device.
#include "aver/formats/OcLand.hpp"

#include <vector>

namespace aver::landscape {

using aver::f32;
using aver::u32;
using aver::u8;
using aver::usize;

inline constexpr u32 kInvalidNode = 0xFFFFFFFFu;

// The default node tessellation. Must stay a multiple of 8: Jolt's heightfield block size is 8.
inline constexpr u32 kDefaultNodeQuads = 64;

// One quadtree node: the source samples it covers, its bounding sphere, its error and its skirt.
struct LandscapeNode {
    u32 level = 0;              // 0 is the finest
    u32 sampleX = 0, sampleY = 0;   // the node's top-left SOURCE sample
    u32 stride = 1;             // source samples per rendered vertex step; 1 << level
    u32 spanQuads = 0;          // source quads the node covers on a side; nodeQuads * stride

    f32 centre[3] = {0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;

    f32 errorCm = 0.0f;         // largest vertical deviation of this node's tessellation from the samples

    f32 skirtCm = 0.0f;         // how far the skirt hangs below the rim

    u32 child[4] = {kInvalidNode, kInvalidNode, kInvalidNode, kInvalidNode};   // NW, NE, SW, SE
    bool leaf() const { return child[0] == kInvalidNode; }
};

// Six frustum planes, each (a,b,c,d) with a*x+b*y+c*z+d >= 0 INSIDE.
struct Frustum {
    f32 plane[6][4] = {};

    // Extracts the six planes from a row-major, ROW-VECTOR view-projection (translation in the last row).
    static Frustum fromViewProj(const f32 m[16]);

    // Is any part of the sphere inside? Conservative: a sphere straddling a plane counts as inside.
    bool intersectsSphere(const f32 centre[3], f32 radius) const;
};

// What a frame needs to know to pick levels.
struct SelectParams {
    f32 cameraCm[3] = {0.0f, 0.0f, 0.0f};

    f32 screenErrorPx = 2.0f;   // pixels of permitted error

    f32 projScale = 540.0f;     // viewportHeightPx / (2 * tan(fovY/2))

    // Cull against this when `useFrustum`. Off by default so a caller can select without a matrix.
    Frustum frustum;
    bool useFrustum = false;

    // Hard ceiling on submitted chunks. Over it, nodes are dropped in ascending screen error.
    u32 maxDraws = 192;
};

// What one selection produced.
struct SelectResult {
    std::vector<u32> nodes;     // indices into LandscapeTree::nodes()
    u32 culled = 0;             // rejected by the frustum
    u32 dropped = 0;            // lost to maxDraws
};

// The built quadtree over one section, and the per-frame level selection over it.
class LandscapeTree {
public:
    // Subdivides a section. Returns false and fills `why` if the section or `nodeQuads` is unusable.
    bool build(const fmt::OcLandData& data, u32 nodeQuads = kDefaultNodeQuads, std::string* why = nullptr);

    const std::vector<LandscapeNode>& nodes() const { return nodes_; }
    u32  root() const { return nodes_.empty() ? kInvalidNode : rootIndex_; }
    u32  levelCount() const { return levels_; }
    u32  nodeQuads() const { return nodeQuads_; }
    u32  verticesPerSide() const { return nodeQuads_ + 1; }

    // Picks the set of nodes to draw, with hysteresis on the refine threshold.
    void select(const SelectParams& p, SelectResult& out) const;
    // Forgets which nodes are refined, so the next select is unbiased.
    void resetHysteresis() const;

    // The largest error of any node at `level`, or 0 past the top.
    f32 levelErrorCm(u32 level) const {
        return level < levelError_.size() ? levelError_[level] : 0.0f;
    }

private:
    // Walks one node, emitting it or recursing into its children.
    void descend(u32 index, const SelectParams& p, SelectResult& out,
                 std::vector<f32>& errors) const;

    std::vector<LandscapeNode> nodes_;
    std::vector<f32> levelError_;       // the max errorCm across each level, for the skirt bound
    mutable std::vector<u8> refined_;   // hysteresis, one byte per node
    u32 rootIndex_ = kInvalidNode;
    u32 levels_ = 0;
    u32 nodeQuads_ = kDefaultNodeQuads;
};

} // namespace aver::landscape
