#pragma once
// Reciprocal velocity obstacles in the half-plane (ORCA) form, plus the small 2D linear program
// that picks the velocity closest to the preferred one inside every half-plane
// (van den Berg et al., "Reciprocal n-body collision avoidance", 2011). Pure arithmetic: the
// crowd simulation, its tests and the GPU shader's mirror all share these definitions.
//
// A line's FEASIBLE side is its left: det(direction, point - v) <= 0 holds for allowed v.
#include "aver/synapse/SteerMath.hpp"

#include <vector>

namespace aver::fmt { struct OcNavData; }

namespace aver::synapse {

struct OrcaLine {
    V2 point;
    V2 direction;   // unit
};

// The half-plane that keeps agent A (pos, vel, radius) clear of agent B for `timeHorizon` seconds.
// `share` is the fraction of the avoidance A takes: 0.5 for a reciprocal pair, 1 when B cannot
// move or is yielding to A, 0 when A has the right of way. Already-overlapping pairs use `dt`
// (one step) as the horizon so the push-apart is immediate.
OrcaLine orcaAgentLine(V2 posA, V2 velA, f32 radiusA, V2 posB, V2 velB, f32 radiusB, f32 share,
                       f32 timeHorizon, f32 dt);

// The half-plane that stops A closing on a static point `closest` (nearest point of a wall) faster
// than it can stop in `horizon` seconds. Allows sliding along the wall and forces motion away
// from it when already inside `radius`.
OrcaLine orcaWallLine(V2 posA, f32 radiusA, V2 closest, V2 fallbackNormal, f32 horizon);

struct OrcaScratch {
    std::vector<OrcaLine> proj;
};

// Solves for the velocity inside radius `maxSpeed` that satisfies every line, closest to
// `preferred`. The first `hardCount` lines (walls) are never relaxed; if the rest are
// contradictory the result minimises the worst violation of them instead.
V2 orcaSolve(const std::vector<OrcaLine>& lines, u32 hardCount, V2 preferred, f32 maxSpeed,
             OrcaScratch& scratch);

// Nearest point of the axis-aligned square [minX,minX+size]x[minY,minY+size] to p.
V2 closestPointOnCell(V2 p, f32 minX, f32 minY, f32 size);

// Wall half-planes for the blocked cells of `nav` near `pos`, nearest first, at most `maxLines`.
// A cell is a wall when it is outside the grid or not walkable, and borders a walkable cell.
// `reach` is how far from the agent's edge to look (maxSpeed * horizon). Appends to `out`.
void orcaGatherWalls(const fmt::OcNavData& nav, V2 pos, f32 radius, f32 reach, f32 horizon,
                     u32 maxLines, std::vector<OrcaLine>& out);

// True when the cell is a wall for avoidance purposes (outside the grid, or not walkable).
bool navCellBlocked(const fmt::OcNavData& nav, i32 cx, i32 cy);

} // namespace aver::synapse
