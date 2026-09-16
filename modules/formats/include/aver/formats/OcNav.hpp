#pragma once
// .ocnav -- baked navigation for a level, as an AVR1 container.
//
// A 2.5D GRID, not a polygonal navmesh. One cell per `cellSizeCm` square of the XY plane, each
// carrying the height of the floor under it, whether an agent fits there, and which connected region
// it belongs to. The engine has no voxelisation, contour-tracing or triangulation code anywhere
// (grep for recast/detour/navmesh returns nothing outside vendored Jolt), so a polygon mesh would be
// a project of its own; a grid is cheap to bake, cheap to search, trivial to draw as an overlay, and
// its weakness -- coarseness around tight geometry -- is visible rather than subtle.
//
// 2.5D, NOT 3D: exactly one floor per cell. A bridge over a path is not representable, and neither
// is a second storey. That is a real limit and the reason `NLNK` is reserved below.
//
// BAKED IN THE EDITOR, NEVER AT RUNTIME. The bake raycasts the physics world, and terrain collision
// exists only in the editor (SandboxApp::rebuildLandscapeCollision; grep add_heightfield over
// Runtime finds nothing). So the bake happens in the one place the ground is actually
// there, and ships its answer as an asset. It is also far too slow for a frame: aver_phys_raycast
// resolves its hit by linear scan over every body (PhysicsWorld.cpp:565), so a bake is
// O(cells x bodies).
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

inline constexpr u8 kOcNavWalkable = 1u << 0;

// One cell. Laid out so there is no implicit padding -- a file writes fields explicitly and would
// not care, but a struct whose size depends on the compiler is a bad thing to hand to a renderer.
struct OcNavCell {
    f32 floorZCm = 0.0f;
    // 0 when the cell is not walkable. Otherwise a connected-region id: two cells with different
    // ids have NO path between them, which lets a query refuse in O(1) instead of discovering it by
    // exhausting the search. u16 rather than u8 deliberately -- 255 regions is reachable in a level
    // with a lot of separated interiors, and silent aliasing would defeat the precheck entirely.
    u16 regionId = 0;
    u8  flags    = 0;
    u8  reserved = 0;
};

struct OcNavData {
    f32 cellSizeCm = 50.0f;
    // The world position of the MIN corner of cell (0,0). Cell centres are origin + (x+0.5)*cell.
    f32 originXCm = 0.0f, originYCm = 0.0f;
    u32 widthCells = 0, heightCells = 0;

    // THE BAKE'S OWN PARAMETERS, carried so a mismatch is detectable rather than silent. An agent
    // wider than the bake assumed will clip corners, and the only way anyone finds out is if the
    // numbers are here to compare.
    f32 agentRadiusCm = 34.0f;    // matches AverCharacter.Radius (Character.cs:65)
    f32 agentHeightCm = 180.0f;   // matches AverCharacter.Height (Character.cs:62)
    f32 maxSlopeDeg   = 50.0f;    // matches the character controller's own hardcoded limit
    // The biggest floor-height difference an agent may cross between neighbouring cells. This is
    // what stops a path walking up a cliff one 50cm step at a time -- without it, a grid with a
    // height field is a grid with no walls.
    f32 maxStepCm     = 40.0f;

    // Row-major: cell (x, y) is at y * widthCells + x. Empty for a level with no baked navigation,
    // which is every level until somebody bakes one.
    std::vector<OcNavCell> cells;

    const OcNavCell* at(u32 x, u32 y) const {
        if (x >= widthCells || y >= heightCells) return nullptr;
        const usize i = static_cast<usize>(y) * widthCells + x;
        return i < cells.size() ? &cells[i] : nullptr;
    }
    OcNavCell* at(u32 x, u32 y) {
        return const_cast<OcNavCell*>(static_cast<const OcNavData*>(this)->at(x, y));
    }

    // True when the dimensions agree with the cell count and the parameters are usable.
    bool valid() const;
};

bool loadOcNav(const std::string& path, OcNavData& out, std::string* why = nullptr);
bool saveOcNav(const std::string& path, const OcNavData& in, std::string* why = nullptr);
bool parseOcNav(const u8* bytes, usize size, OcNavData& out, std::string* why = nullptr);
bool writeOcNav(const OcNavData& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
