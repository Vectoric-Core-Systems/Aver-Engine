#pragma once
// Synapse navigation: reading a baked grid, and finding a path across it.
//
// PURE. No scene, no physics, no device -- a grid and arithmetic. That is what lets the whole
// pathfinder be tested against a hand-authored grid with a wall in it, which is exactly what
// tests/synapse does. The join to a live world is Aver.Synapse.Scene, one tier up, the same split
// Aver.Anim / Aver.Anim.Scene already uses and for the same reason.
#include "aver/core/Math.hpp"
#include "aver/formats/OcNav.hpp"

#include <vector>

namespace aver::synapse {

// Where a world position lands in the grid. False when it is outside the baked footprint -- which
// is an ordinary answer, not an error: a level is bigger than the part of it anyone can walk on.
bool worldToCell(const fmt::OcNavData& nav, f32 xCm, f32 yCm, u32& outX, u32& outY);

// The CENTRE of a cell, in world space, at the floor height the bake recorded.
Vec3 cellToWorld(const fmt::OcNavData& nav, u32 x, u32 y);

// The nearest walkable cell to a world position, searched outwards in rings up to `maxRingCells`.
// False when nothing walkable is within that radius.
//
// WHY A SEARCH RATHER THAN A LOOKUP: an agent standing on a slope, or half a centimetre inside a
// wall, or on a cell the bake called unwalkable because a pebble was in it, must still be able to
// ask for a path. Snapping is the difference between "the AI stopped working" and "the AI set off
// from slightly over there".
bool nearestWalkable(const fmt::OcNavData& nav, f32 xCm, f32 yCm, u32 maxRingCells,
                     u32& outX, u32& outY);

// Assigns a connected-region id to every walkable cell, and 0 to the rest. Returns how many regions
// it found. Run by the BAKER, once; a loaded grid already carries the answer.
//
// The point of it is the O(1) refusal in findPath: two cells in different regions have no path
// between them, and discovering that by exhausting an A* over a large grid costs thousands of
// expansions to learn something a comparison could have said immediately.
//
// Regions saturate at 65535 rather than wrapping. A wrapped id would alias two unconnected regions
// into one and make the precheck answer "reachable" for a pair that is not -- the precise failure
// the precheck exists to prevent.
u32 buildRegions(fmt::OcNavData& nav);

// Whether an agent may step directly between two ADJACENT cells: both walkable, in the same region,
// and no floor-height difference greater than maxStepCm. Diagonals additionally require both shared
// orthogonal neighbours to be steppable, so a path never squeezes through the corner where two
// walls meet.
bool canStep(const fmt::OcNavData& nav, u32 ax, u32 ay, u32 bx, u32 by);

enum class PathStatus {
    Found,        // a complete path from start to goal
    Partial,      // the node budget ran out; the path reaches the closest point found so far
    Unreachable,  // start and goal are in different regions -- refused in O(1), no search run
    OffMesh,      // start or goal is not on (or near) the baked grid at all
    Invalid,      // the grid itself is unusable
};

struct PathRequest {
    f32 startXCm = 0.0f, startYCm = 0.0f;
    f32 goalXCm  = 0.0f, goalYCm  = 0.0f;
    // How far to look for a walkable cell when the endpoints are not on one. 0 means "no snapping":
    // a start that is not exactly on a walkable cell is OffMesh.
    u32 snapRingCells = 4;
    // THE PER-QUERY CEILING. A* on an open grid expands a lot of nodes for a long path, and this
    // runs on the one game thread -- there is no job system to post it to (ARCHITECTURE.md names
    // that as still open). Past this many expansions the search stops and returns the best partial
    // path rather than the frame.
    u32 maxExpansions = 8000;
};

struct PathResult {
    PathStatus status = PathStatus::Invalid;
    // World-space waypoints, start first. STRING-PULLED: consecutive cells in a straight clear line
    // collapse into one segment, so a path across open ground is two points rather than forty.
    std::vector<Vec3> points;
    u32 expansions = 0;   // what the search actually cost, so a budget can be tuned against numbers
};

// Finds a path. Allocates its own working state per call; a caller making many queries per frame
// should be budgeting them anyway (see PathRequest::maxExpansions).
PathResult findPath(const fmt::OcNavData& nav, const PathRequest& req);

// True when every cell on the straight line between two cell centres is steppable. The string-pull
// uses it, and so does a "can I just walk at it" check that wants no path at all.
bool lineOfSight(const fmt::OcNavData& nav, u32 ax, u32 ay, u32 bx, u32 by);

} // namespace aver::synapse
