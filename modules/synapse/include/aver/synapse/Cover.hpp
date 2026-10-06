#pragma once
// Cover points, cover queries, reservations and squad primitives. Pure: positions on the ground
// plane, the baked grid for generation and line checks, and function pointers for anything that
// needs a live world (a physics raycast for line of sight, a path query for cost).
//
// A cover point sits where an agent stands. `dir` points from it at the obstacle that protects
// it. It protects against a threat when the threat lies inside the point's arc on the obstacle's
// side AND the straight line from threat to point is blocked.
#include "aver/formats/OcNav.hpp"
#include "aver/synapse/SteerMath.hpp"

#include <unordered_map>
#include <vector>

namespace aver::synapse {

enum class CoverHeight : u8 { Low = 0, High = 1 };   // Low: crouch cover. High: full-height.

struct CoverPoint {
    u32 id = 0;                    // stable until the point is removed; never reused
    V2  pos;
    V2  dir;                       // unit, toward the protecting obstacle
    CoverHeight height = CoverHeight::High;
    f32 arcHalfAngleDeg = 70.0f;   // how far off `dir` a threat may be and still be covered
    bool authored = false;         // false: generated from the grid
    bool enabled = true;
    u32 owner = 0;                 // marker entity for authored points, 0 otherwise
};

// True when the straight segment a->b is blocked. Supplied by a host that can raycast.
using BlockedFn = bool (*)(void* user, V2 a, V2 b);
// Path cost between two points in cm, or a negative number when unreachable.
using PathCostFn = f32 (*)(void* user, V2 from, V2 to);

// True when a cell strictly between the endpoints' cells is blocked (outside the grid or not
// walkable). The endpoints' own cells are ignored: the agent and the threat stand somewhere.
bool navSegmentBlocked(const fmt::OcNavData& nav, V2 a, V2 b);

struct CoverGenParams {
    f32 minSpacingCm = 150.0f;      // generated points are at least this far apart
    u32 maxPoints    = 4096;
    CoverHeight height = CoverHeight::High;
    // Optional confirmation that the blocked neighbour is a real wall and not a hole or cliff.
    // `towardBlocked` is the unit direction from the candidate to the blocked cell.
    bool (*isWall)(void* user, V2 cellCentre, V2 towardBlocked) = nullptr;
    void* user = nullptr;
};

struct CoverQuery {
    V2  from;                          // the seeker
    V2  threat;
    f32 maxSeekCm       = 2500.0f;     // candidates must cost no more than this to reach
    f32 minThreatDistCm = 300.0f;      // too close to the threat to be useful
    f32 maxThreatDistCm = 1.0e9f;
    // Reject points that bring the seeker this much nearer the threat than it is now.
    f32 maxCloserCm     = 400.0f;
    bool requireHigh    = false;       // only full-height cover
    u32 seeker          = 0;           // reservations held by this id do not block it
    const fmt::OcNavData* nav = nullptr;   // line check when `blocked` is null
    BlockedFn   blocked = nullptr;     void* blockedUser = nullptr;
    PathCostFn  pathCost = nullptr;    void* pathUser = nullptr;   // straight distance when null
    u32 maxPathCalls    = 12;          // path queries run on this many nearest candidates only
};

struct CoverResult {
    u32 id = 0;
    V2  pos;
    f32 cost = 0.0f;
};

class CoverReservations {
public:
    // Reserves `coverId` for `owner` for ttlSec (<= 0: until released). False when another owner
    // holds it. An owner holds one point at a time: reserving moves its previous claim.
    bool reserve(u32 coverId, u32 owner, f32 ttlSec);
    void release(u32 coverId, u32 owner);
    void releaseAll(u32 owner);
    u32  ownerOf(u32 coverId) const;               // 0 when free
    bool reservedByOther(u32 coverId, u32 owner) const;
    u32  coverOf(u32 owner) const;                 // 0 when the owner holds none
    void tick(f32 dt);                             // expires timed claims
    usize size() const { return byCover_.size(); }
    // Every owner currently holding a claim (order unspecified); lets a system drop dead owners.
    void owners(std::vector<u32>& out) const;

private:
    struct Entry { u32 owner = 0; f32 ttlSec = 0.0f; bool timed = false; };
    std::unordered_map<u32, Entry> byCover_;
};

class CoverMap {
public:
    u32  addAuthored(V2 pos, V2 dir, CoverHeight h = CoverHeight::High, f32 arcHalfAngleDeg = 70.0f,
                     u32 owner = 0);
    // Replaces every generated point with candidates found beside walls of `nav`: walkable cells
    // with a blocked 4-neighbour, plus outside corners. Returns how many were added.
    u32  generateFromNav(const fmt::OcNavData& nav, const CoverGenParams& p);
    void clearGenerated();
    void clearAuthored();
    void clear();
    bool setEnabled(u32 id, bool enabled);
    bool remove(u32 id);

    const CoverPoint* find(u32 id) const;
    CoverPoint* findMutable(u32 id);
    const std::vector<CoverPoint>& points() const { return points_; }

    // Whether `c` shields an agent standing on it from `threat`. See the header comment.
    bool protectsFrom(const CoverPoint& c, V2 threat, const fmt::OcNavData* nav, BlockedFn blocked,
                      void* blockedUser) const;

    // Best point for the query (lowest path cost) that protects, is enabled and is not reserved
    // by someone else. Ties go to the lower id. `res` may be null.
    bool query(const CoverQuery& q, const CoverReservations* res, CoverResult& out) const;
    // query() then reserve, atomically: two seekers can never be given the same point.
    bool claim(const CoverQuery& q, CoverReservations& res, f32 ttlSec, CoverResult& out) const;

private:
    std::vector<CoverPoint> points_;
    u32 nextId_ = 1;
};

// ---- squads ---------------------------------------------------------------------------------------

enum class SquadRole : u8 { None = 0, Anchor = 1, FlankLeft = 2, FlankRight = 3, Support = 4 };

struct SquadMemberIn {
    u32 id = 0;
    V2  pos;
};

// Nearest to the target anchors; the next two flank left and right; the rest support. Ties by id.
// `outRoles` has n entries, aligned with `members`.
void squadAssignRoles(const SquadMemberIn* members, u32 n, V2 target, SquadRole* outRoles);

// A point on the circle of radius `radiusCm` around the target, `angleDeg` either side of the
// bearing from target to squad centre. FlankRight rotates toward +Y, FlankLeft toward -Y.
V2 squadFlankPosition(V2 squadCentre, V2 target, SquadRole side, f32 radiusCm, f32 angleDeg);

// Displacement that moves `self` out of every neighbour's spacing circle; zero when satisfied.
V2 squadSpacingPush(V2 self, const V2* others, u32 n, f32 spacingCm);

// A support slot behind the anchor (away from the target), spread sideways by slot index
// (0, +1, -1, +2, ...) at `spacingCm`.
V2 squadSupportPosition(V2 anchor, V2 target, f32 behindCm, u32 slot, f32 spacingCm);

} // namespace aver::synapse
