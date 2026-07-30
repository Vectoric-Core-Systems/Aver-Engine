#pragma once
// The landscape quadtree: which chunks to draw, at which detail, this frame.
//
// PURE CPU. Nothing here includes an `rhi::` type or touches a device, and that is load-bearing
// rather than tidy: it is what lets every number below be checked headlessly, and what keeps "which
// chunks are visible" a decision this module owns rather than one smeared across a renderer.
//
// THE SHAPE. A section (one `.ocland` file, see OcLand.hpp) is subdivided into nodes. Every node
// renders the SAME vertex count at every level -- 65x65 by default -- and covers a correspondingly
// larger area of source samples as the level rises. So for a 1025-sample section at 64 quads per node:
//
//   level 0   16x16 = 256 nodes, each   64 source quads across   (stride 1)
//   level 1    8x8  =  64 nodes, each  128 source quads across   (stride 2)
//   level 2    4x4  =  16 nodes, each  256                       (stride 4)
//   level 3    2x2  =   4 nodes, each  512                       (stride 8)
//   level 4    1x1  =   1 node,        1024                      (stride 16)
//                    341 nodes total
//
// A CONSTANT vertex count per node is the whole point. It makes memory, draw cost and skirt overhead
// predictable at every level, and it makes the draw count logarithmic in view distance rather than
// linear in area -- which is the difference between a horizon that costs a few hundred draws and one
// that costs tens of thousands.
//
// 65 rather than 64 because 2^6+1 SHARES its edge row with the neighbour. Two adjacent nodes at the
// same level then agree on their shared vertices exactly, so there is no crack to hide at all in the
// common case. Cracks only appear across a LEVEL change, which is what the skirts are for.
#include "aver/formats/OcLand.hpp"

#include <vector>

namespace aver::landscape {

using aver::f32;
using aver::u32;
using aver::u8;
using aver::usize;

inline constexpr u32 kInvalidNode = 0xFFFFFFFFu;

// The default node tessellation. 64 quads is a multiple of 8, which matters downstream: Jolt's
// heightfield block size is 8, so a node's sample span divides its blocks exactly.
inline constexpr u32 kDefaultNodeQuads = 64;

struct LandscapeNode {
    u32 level = 0;              // 0 is the finest
    u32 sampleX = 0, sampleY = 0;   // the node's top-left SOURCE sample
    u32 stride = 1;             // source samples per rendered vertex step; 1 << level
    u32 spanQuads = 0;          // source quads the node covers on a side; nodeQuads * stride

    // A BOUNDING SPHERE, not the AABB it is derived from. Selection is radial (see `select`), and a
    // sphere is the only fit that is invariant under the direction the camera looks from -- an AABB
    // tested by planar depth picks a different level for the same node depending on where in the
    // frustum it sits, which shows as a seam that moves when the camera turns.
    f32 centre[3] = {0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;

    // GEOMETRIC ERROR, in centimetres: the largest vertical distance between a source sample inside
    // this node and the surface the node's own tessellation actually draws. Identically 0 at level 0,
    // where every sample is a vertex, and non-decreasing with level. This is the quantity that gets
    // projected to pixels; a distance band would be a guess about the camera instead.
    f32 errorCm = 0.0f;

    // How far this node's skirt hangs below its rim. Sized to the largest vertical gap a
    // 2:1-balanced coarser neighbour could open along a shared edge, so a skirt of this depth cannot
    // be seen through. See LandscapeTree::build for the derivation.
    f32 skirtCm = 0.0f;

    u32 child[4] = {kInvalidNode, kInvalidNode, kInvalidNode, kInvalidNode};   // NW, NE, SW, SE
    bool leaf() const { return child[0] == kInvalidNode; }
};

// Six frustum planes, each (a,b,c,d) with a*x+b*y+c*z+d >= 0 INSIDE.
struct Frustum {
    f32 plane[6][4] = {};

    // Extracted from a row-major, ROW-VECTOR view-projection -- the engine's convention, translation
    // in the last row. That matters to every line of the implementation: with row vectors
    // `clip = [x y z 1] * M`, so clip.x is built from M's COLUMN 0, not its row 0. Taking rows here is
    // the classic way to get a frustum that is silently transposed and culls things in front of the
    // camera while keeping things behind it.
    static Frustum fromViewProj(const f32 m[16]);

    // Is any part of the sphere inside? Conservative: a sphere straddling a plane counts as inside.
    bool intersectsSphere(const f32 centre[3], f32 radius) const;
};

// What a frame needs to know to pick levels.
struct SelectParams {
    f32 cameraCm[3] = {0.0f, 0.0f, 0.0f};

    // Pixels of permitted error. The metric FORMAT_SPECS.md already designates
    // (LodDesc::ScreenErrorThreshold), rather than distance bands invented here.
    //
    // 2.0 is a reasoned starting value and NOT a measured one: below about 1 px the metric is
    // dominated by the u16 height quantisation step, and 2 px is roughly where a level switch moves a
    // silhouette by less than the eye follows at 1080p.
    f32 screenErrorPx = 2.0f;

    // viewportHeightPx / (2 * tan(fovY/2)). Passed in rather than derived from a matrix so this stays
    // callable with no camera at all, which is what the tests do.
    f32 projScale = 540.0f;

    // Cull against this when `useFrustum`. Off by default so a caller can select without a matrix.
    Frustum frustum;
    bool useFrustum = false;

    // A HARD CEILING on submitted chunks, and not a suggestion. The transient constant ring is 1 MiB
    // and a submitted draw costs about 2 KiB across the shadow, voxelisation and lit passes, so ~192
    // draws is already 37% of the ring. Past exhaustion the allocator returns 0, the constant buffer
    // is silently not bound, and shading is wrong with one logged error -- so the clamp has to be here
    // rather than trusted to a budget nobody enforces.
    //
    // When selection exceeds it, nodes are dropped in ASCENDING screen error: the ones that matter
    // least go first. Callers that care are told how many were lost.
    u32 maxDraws = 192;
};

struct SelectResult {
    std::vector<u32> nodes;     // indices into LandscapeTree::nodes()
    u32 culled = 0;             // rejected by the frustum
    u32 dropped = 0;            // lost to maxDraws -- NOT silent, see SelectParams::maxDraws
};

class LandscapeTree {
public:
    // Subdivide a section. `nodeQuads` must divide (sampleCount - 1) a whole number of times by 2 all
    // the way down, which is why sections are sized (k * nodeQuads) + 1.
    bool build(const fmt::OcLandData& data, u32 nodeQuads = kDefaultNodeQuads, std::string* why = nullptr);

    const std::vector<LandscapeNode>& nodes() const { return nodes_; }
    u32  root() const { return nodes_.empty() ? kInvalidNode : rootIndex_; }
    u32  levelCount() const { return levels_; }
    u32  nodeQuads() const { return nodeQuads_; }
    u32  verticesPerSide() const { return nodeQuads_ + 1; }

    // Pick the set to draw. Descends from the root, stopping where a node's projected error is under
    // the threshold.
    //
    // HYSTERESIS is stateful, and deliberately: a node that has refined stays refined until its error
    // falls to 0.8x the threshold. Without it a camera hovering on a boundary switches level every
    // frame, and the pop is far more visible at 2 Hz than the extra detail is worth. `resetHysteresis`
    // exists so a test -- or a teleport -- can ask for the unbiased answer.
    void select(const SelectParams& p, SelectResult& out) const;
    void resetHysteresis() const;

    // The error a node at `level` would have. Exposed because the skirt derivation and its test both
    // need it, and recomputing it in the test would be checking the code against itself.
    f32 levelErrorCm(u32 level) const {
        return level < levelError_.size() ? levelError_[level] : 0.0f;
    }

private:
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
