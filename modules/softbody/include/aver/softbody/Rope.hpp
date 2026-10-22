// Rope: the pure half of ropes and cables (docs/plans/wave11/SOFTBODY_HAIR.md 2.4) -- the rest curve of a chain of Cosserat rod
// nodes, and the tube drawn around them.
//
// The simulation is Jolt's rods (aver_phys_sbx_builder_rod_chain). This header plans the nodes (a sagging parabola between two
// anchors, resampled to even arc length so every rod starts at the same length) and builds the draw geometry from the
// simulated node positions and, when they are known, the rods' orientations, so a twisting cable twists its texture.
// Core only: no scene, no GPU, no Jolt.
#pragma once

#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::softbody {

struct RopeSpec {
    f32 lengthCm = 0.0f;               // rest arc length; 0 = the distance between the ends x slack
    f32 slack = 1.02f;                 // >= 1
    f32 segmentCm = 12.0f;
    f32 radiusCm = 1.2f;
    f32 massPerMeterKg = 0.4f;
    u32 sides = 8;
    f32 uvCmPerTile = 20.0f;
    f32 stretchCompliance = 1e-8f;
    f32 bendTwistCompliance = 1e-3f;
    u32 maxNodes = 4000;
};

struct RopeBuild {
    u32 nodes = 0;
    std::vector<Vec3> rest;            // node positions, the first at `a` and the last at `b`
    std::vector<f32> invMass;          // every node 1 / nodeMassKg; pinning an end is the caller's (aver_phys_sbx_attach)
    f32 nodeMassKg = 0.0f;
    f32 restLengthCm = 0.0f;           // the length of the planned polyline
    f32 segmentCm = 0.0f;              // restLengthCm / (nodes - 1)
    std::string error;
};

// A rest curve between `a` and `b`: a parabola sagging toward -Z whose arc length is the rope's length (straight when the
// length does not exceed the distance), resampled to even arc length. At least 3 nodes (Jolt's rod chain needs a bend-twist
// pair). False with `out.error` set for non-finite input, a zero length or more nodes than `maxNodes`.
bool planRope(const Vec3& a, const Vec3& b, const RopeSpec& spec, RopeBuild& out);

// Vertices of the tube: one ring per node, `sides + 1` vertices per ring (the last repeats the first so the texture can wrap),
// ring-major. rodRotation: nodeCount - 1 world orientations (the engine +X axis rotated by one is the rod direction, its
// twist the Bishop frame; aver_phys_sbx_rod_state), or null for parallel-transport frames. Each ring is perpendicular to the
// local tangent whatever the rods say.
inline u32 tubeVertexCount(u32 nodeCount, u32 sides) { return nodeCount * (sides + 1); }
void tubeVertices(const Vec3* nodes, u32 nodeCount, const Quat* rodRotation, f32 radiusCm, u32 sides, Vec3* outPos);

// 2 floats per tube vertex: u = arc length along the rope in tiles of `uvCmPerTile`, v = k / sides around.
void tubeUvs(const Vec3* nodes, u32 nodeCount, u32 sides, f32 uvCmPerTile, f32* outUv);

// The static triangle list of the tube, wound so that cross(B - A, C - A) points outward (the render mesh convention).
// The ends are open.
void tubeIndices(u32 nodeCount, u32 sides, std::vector<u32>& out);

// Sum of the segment lengths.
f32 polylineLength(const Vec3* nodes, u32 count);
// The node nearest to `p` (0 for an empty list).
u32 nearestNode(const Vec3* nodes, u32 count, const Vec3& p);

}  // namespace aver::softbody
