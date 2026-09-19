#pragma once
// Synapse navigation: turning a world into a grid.
//
// STILL PURE. The bake does not call physics -- it calls two function pointers the CALLER installs,
// and the caller is the only thing in the chain that knows what a body is. That is not ceremony:
// it is what lets the baker be tested against a hand-written world (a floor, a wall, a ramp,
// arithmetic) instead of against a level and a screenshot, and it is the same host-installs-the-
// function-pointer seam AnimSystem::setResolver and aver_fw_set_anim_curve_provider already use.
//
// BAKING IS AN EDITOR COMMAND, NEVER A RUNTIME COST. That is a decision, not an accident -- but
// NOT for the reason this comment used to give. It said terrain collision was editor-only, and
// that is false: game::GameLandscape::rebuildCollision (Runtime/src/GameLandscape.cpp) adds the
// heightfield body from the runtime's own level load, so a shipped game can see the ground too.
// The real reasons are that the bake is far too slow for a frame (see OcNav.hpp's own note on the
// per-raycast linear scan) and that the two probes below are installed in exactly one place in the
// tree, sandbox/src/NavBakeCommand.cpp. The result ships as an asset.
#include "aver/core/Math.hpp"
#include "aver/formats/OcNav.hpp"

#include <string>

namespace aver::synapse {

// Finds the floor under a column. Returns false for a column with nothing under it at all -- a hole
// in the level, or a cell hanging over the void -- which becomes an unwalkable cell, not an error.
//
// `outNormalZ` is the UP component of the surface normal, already normalised, which is all the slope
// test needs. Handing back the whole vector would invite callers to use it for something the bake
// does not model: exactly one floor per cell, no orientation carried.
using FloorProbeFn = bool (*)(void* user, f32 xCm, f32 yCm, f32 topZCm, f32 depthCm,
                              f32* outZCm, f32* outNormalZ);

// Whether an agent of this size standing at (x, y, z) has room to stand. False makes the cell
// unwalkable. A caller with no notion of headroom may pass nullptr for this and every floor becomes
// walkable if its slope allows -- stated here because a silently-always-true probe and an absent one
// must not look the same from inside the baker.
using HeadroomProbeFn = bool (*)(void* user, f32 xCm, f32 yCm, f32 zCm,
                                 f32 radiusCm, f32 heightCm);

struct BakeParams {
    // The MIN corner of cell (0,0). Cell centres are origin + (i + 0.5) * cellSizeCm.
    f32 originXCm = 0.0f, originYCm = 0.0f;
    f32 cellSizeCm = 50.0f;
    u32 widthCells = 0, heightCells = 0;

    // Where the downward probe starts and how far it reaches. The default pair spans 50 m above the
    // origin plane down to 150 m below it, which covers every level this engine has shipped; a level
    // taller than that gets a bake that is silently missing its upper floors, so these are settings
    // rather than constants.
    f32 sampleTopZCm = 5000.0f;
    f32 sampleDepthCm = 20000.0f;

    // Carried into the grid so a runtime mismatch is detectable. Defaults track AverCharacter.
    f32 agentRadiusCm = 34.0f;
    f32 agentHeightCm = 180.0f;
    f32 maxSlopeDeg = 50.0f;
    f32 maxStepCm = 40.0f;
};

struct BakeStats {
    u32 cellsProbed = 0;
    u32 cellsWalkable = 0;
    u32 rejectedNoFloor = 0;    // nothing under the column
    u32 rejectedSlope = 0;      // floor too steep to stand on
    u32 rejectedHeadroom = 0;   // floor fine, but an agent would not fit
    u32 regions = 0;
};

// Bakes a grid. Overwrites `out` entirely, including running buildRegions, so the result is
// immediately usable by findPath without a second step somebody can forget.
//
// COST: one floor probe and at most one headroom probe per cell, and aver_phys_raycast resolves its
// hit by a LINEAR SCAN over every body (PhysicsWorld.cpp), so a real bake is O(cells x bodies). A
// 200 x 200 grid over a level with 2000 bodies is 40,000 rays each touching 2000 shapes. That is
// offline and paid once; it is stated because it is the number that makes somebody's Bake button
// appear to hang, and nothing about it is a bug.
bool bakeNav(const BakeParams& p, FloorProbeFn floor, HeadroomProbeFn headroom, void* user,
             fmt::OcNavData& out, BakeStats* stats = nullptr, std::string* why = nullptr);

} // namespace aver::synapse
