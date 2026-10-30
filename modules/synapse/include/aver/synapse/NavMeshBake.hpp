#pragma once
// Baking a polygon navigation mesh from triangles.
//
// PURE and thread-safe: triangles in, polygons out, no shared state, so a caller may run it on a worker thread. The method is a heightfield one: triangles are rasterised into columns of spans,
// walkable spans are filtered (slope, climb, headroom), eroded by the agent radius, split into floors by connectivity, and each
// floor is covered with rectangles that fit a plane. Several floors over the same ground plan, ramps and stairs up to the climb
// height come out connected where an agent can really step, and separate where it cannot.
//
// Triangle winding: the normal that counts is cross(b - a, c - a) computed on the coordinates as given; a floor faces up (+Z).
// A caller whose source is mirrored (another handedness) swaps two indices.
#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::synapse {

struct NavBakeParams {
    f32  cellSizeCm = 25.0f;
    f32  cellHeightCm = 25.0f;
    f32  agentHeightCm = 150.0f;
    f32  agentRadiusCm = 50.0f;
    f32  agentMaxClimbCm = 25.0f;
    f32  agentMaxSlopeDeg = 45.0f;
    f32  regionMinSize = 2.0f;           // a region needs at least size * size cells (the region minimum size)
    f32  regionMergeSize = 20.0f;        // stored; the rectangle cover does not merge by size
    f32  edgeMaxLengthCm = 0.0f;         // longer polygon edges are split; 0 = unlimited
    f32  edgeMaxError = 1.3f;            // stored; the rectangle cover has no contour to simplify
    i32  verticesPerPolygon = 6;         // 3 gives triangles; 4 or more gives the rectangles
    f32  detailSampleDistance = 6.0f;    // stored; heights are exact per cell instead
    f32  detailSampleMaxErrorCm = 25.0f; // how far a cell may sit off the plane of its rectangle
    bool filterLowHangingObstacles = false;
    bool filterLedgeSpans = false;
    bool filterWalkableLowHeightSpans = false;
    f32  borderSizeCm = 0.0f;
    bool useBounds = false;              // clip to [boundsMin, boundsMax]
    Vec3 boundsMin, boundsMax;
};

// A shape that discards the walkable surface it covers (an obstacle that affects the navigation mesh).
struct NavBakeObstacle {
    std::vector<Vec3> verts;             // a polygon in XY; empty for a circle
    Vec3 position;                       // the circle's centre, or ignored when verts is set
    f32  radius = 0.0f;
    f32  zMin = -1e30f, height = 0.0f;   // vertical range [zMin, zMin + height]; 0 = unbounded upwards
};

struct NavBakeStats {
    u32 columns = 0, spans = 0, walkableSpans = 0, regions = 0, regionsRemoved = 0, rectangles = 0;
};

struct NavBakeResult {
    std::vector<Vec3> verts;
    std::vector<std::vector<u32>> polys;
    NavBakeStats stats;
};

// `tris` holds three indices per triangle into `verts`. Returns false (and an empty result) for bad input or a grid over `maxCells`.
bool navBake(const std::vector<Vec3>& verts, const std::vector<u32>& tris, const std::vector<NavBakeObstacle>& obstacles,
             const NavBakeParams& params, NavBakeResult& out, std::string* why = nullptr, u64 maxCells = 40000000ull);

} // namespace aver::synapse
