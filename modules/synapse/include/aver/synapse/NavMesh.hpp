#pragma once
// Synapse polygon navigation: maps made of regions (convex polygons in world space), links between them, path queries with layers,
// costs and funnel smoothing, and RVO-style avoidance. Navigation-server semantics.
//
// WHY NEXT TO THE GRID. `.ocnav` (Nav.hpp) is a 2.5D grid with ONE floor per cell and no layers or costs; a bridge over a road, a second
// storey or a link between two roofs cannot be said in it. This is the polygon form the grid's header reserved room for. The grid, the
// crowd and the existing AI keep working unchanged; the two forms share the ORCA core (CrowdOrca.hpp) and nothing else.
//
// PURE like the rest of Synapse: arithmetic on vectors, no scene, no physics, no device. Engine space: centimetres, +Z up; the ground
// plane is XY. Nothing here knows about other axis conventions; the caller converts at the boundary.
#include "aver/core/Math.hpp"
#include "aver/synapse/SteerMath.hpp"

#include <map>
#include <unordered_map>
#include <vector>

namespace aver::synapse {

// Path post-processing.
inline constexpr i32 kNavPostCorridorFunnel = 0;
inline constexpr i32 kNavPostEdgeCentered   = 1;
inline constexpr i32 kNavPostNone           = 2;

// Which per-point arrays a path result carries (path metadata flags). The engine always fills them; the flags select what the
// managed layer hands out.
inline constexpr u32 kNavMetaTypes  = 1;
inline constexpr u32 kNavMetaRids   = 2;
inline constexpr u32 kNavMetaOrigins = 4;
inline constexpr u32 kNavMetaOwners = 8;

// Path point types.
inline constexpr i32 kNavPointRegion = 0;
inline constexpr i32 kNavPointLink   = 1;

struct NavMapParams {
    f32  cellSizeCm            = 25.0f;
    f32  cellHeightCm          = 25.0f;
    f32  edgeConnectionMarginCm = 25.0f;   // how far apart two regions' edges may be and still join
    f32  linkConnectionRadiusCm = 100.0f;  // how far a link end may be from a polygon and still attach
    bool useEdgeConnections    = true;
};

struct NavRegionData {
    u32  id = 0, map = 0;
    bool enabled = true;
    bool useEdgeConnections = true;          // join other regions' edges within the map's margin (a region's own polygons always join)
    u32  layers  = 1;
    f32  enterCost  = 0.0f;
    f32  travelCost = 1.0f;
    u64  owner = 0;
    std::vector<Vec3> verts;                 // world space
    std::vector<std::vector<u32>> polys;     // indices into verts, any winding (normalised on build)
};

struct NavLinkData {
    u32  id = 0, map = 0;
    bool enabled = true, bidirectional = true;
    u32  layers = 1;
    f32  enterCost = 0.0f, travelCost = 1.0f;
    u64  owner = 0;
    Vec3 start, end;
};

struct NavObstacleData {
    u32  id = 0, map = 0;
    bool avoidance = false;
    bool paused = false;
    u32  layers = 1;
    u64  owner = 0;
    Vec3 position, velocity;
    f32  radius = 0.0f, height = 0.0f;
    std::vector<Vec3> verts;                 // a wall polygon (XY); empty for a round obstacle
};

struct NavAgentData {
    u32  id = 0, map = 0;
    bool avoidance = false;
    bool paused = false;
    u64  owner = 0;
    Vec3 position, velocity;                 // velocity = the one the agent wants
    f32  radius = 50.0f, height = 100.0f, maxSpeed = 1000.0f;
    f32  neighborDistance = 5000.0f;
    u32  maxNeighbors = 10;
    f32  timeHorizonAgents = 1.0f, timeHorizonObstacles = 0.0f;
    u32  avoidanceLayers = 1, avoidanceMask = 1;
    f32  priority = 1.0f;
    // Filled by NavServer::stepAvoidance.
    bool hasVelocity = false;                // set_velocity was called since the last step
    bool hasSafe = false;
    Vec3 safeVelocity;
};

struct NavPathQuery {
    u32  map = 0;
    Vec3 start, target;
    u32  layers = 1;
    i32  postprocess = kNavPostCorridorFunnel;
    bool simplify = false;
    f32  simplifyEpsilonCm = 0.0f;
    std::vector<u32> excludedRegions, includedRegions;   // an empty included list means every region
};

struct NavPathResult {
    std::vector<Vec3> points;
    std::vector<i32>  types;     // kNavPointRegion / kNavPointLink
    std::vector<u32>  rids;      // region or link id of the point
    std::vector<u64>  owners;    // the owner id set on that region or link
    f32  lengthCm = 0.0f;
    bool reachedTarget = false;  // the search reached the polygon the target snapped to (false: a partial path to the closest reachable spot)
    f32  endGapCm = 0.0f;        // distance from the last point to the requested target (an off-mesh target ends on the mesh, short of it)
    u32  expansions = 0;
};

struct NavClosest {
    Vec3 point, normal;
    u32  region = 0;
    u64  owner = 0;
    bool found = false;
};

class NavServer {
public:
    // Maps. A map is created active.
    u32  createMap();
    bool destroyMap(u32 id);                 // its regions, links, obstacles and agents stay, detached (map 0)
    bool hasMap(u32 id) const;
    void setMapActive(u32 id, bool on);
    bool mapActive(u32 id) const;
    NavMapParams* mapParams(u32 id);
    const NavMapParams* mapParams(u32 id) const;
    u32  mapIteration(u32 id) const;         // changes whenever the map's polygons are rebuilt
    std::vector<u32> maps() const;

    // Objects. Each setter that changes the geometry marks the map dirty; a query rebuilds a dirty map first.
    u32 createRegion(u32 map);
    u32 createLink(u32 map);
    u32 createObstacle(u32 map);
    u32 createAgent(u32 map);
    bool destroy(u32 id);                    // any of the four kinds
    NavRegionData*   region(u32 id);
    NavLinkData*     link(u32 id);
    NavObstacleData* obstacle(u32 id);
    NavAgentData*    agent(u32 id);
    const NavRegionData*   region(u32 id) const;
    const NavLinkData*     link(u32 id) const;
    std::vector<u32> regionsOf(u32 map) const;
    std::vector<u32> linksOf(u32 map) const;
    std::vector<u32> agentsOf(u32 map) const;
    std::vector<u32> obstaclesOf(u32 map) const;

    void setRegionMap(u32 region, u32 map);
    void setLinkMap(u32 link, u32 map);
    void setObstacleMap(u32 obstacle, u32 map);
    void setAgentMap(u32 agent, u32 map);
    // Replaces a region's polygons. Out-of-range indices are refused (false).
    bool setRegionMesh(u32 region, std::vector<Vec3> verts, std::vector<std::vector<u32>> polys);
    void markDirty(u32 map);

    // Rebuilds the map's polygon graph if anything changed (a forced update). Returns true when it rebuilt.
    bool update(u32 map);

    // Path queries. `points` is empty when the map has no usable polygon.
    NavPathResult queryPath(const NavPathQuery& q);
    NavClosest closestPoint(u32 map, const Vec3& p, u32 layers = 0xFFFFFFFFu);
    NavClosest closestPointOnRegion(u32 region, const Vec3& p);
    Vec3 closestPointToSegment(u32 map, const Vec3& a, const Vec3& b, Vec3* onSegment = nullptr);
    // r1, r2, r3 are uniform [0,1) numbers from the caller's generator, so the result is reproducible.
    Vec3 randomPoint(u32 map, u32 layers, bool uniformly, f32 r1, f32 r2, f32 r3, bool* found = nullptr);
    // Number of polygons, and the world-space edge connections (a, b) of a region.
    u32  polygonCount(u32 map);
    std::vector<std::pair<Vec3, Vec3>> regionConnections(u32 region);
    u32  linkConnectedPolys(u32 link);       // how many polygon ends the link attached to (0..2)

    // Avoidance: computes safeVelocity for every agent of the map that has avoidance on and had its velocity set since the last step.
    void stepAvoidance(u32 map, f32 dt);

private:
    struct Conn {
        u32  to = 0;
        Vec3 a, b;                           // the shared portal, as seen leaving this polygon (a = right end, b = left end)
        u32  link = 0;                       // non-zero: a link crossing
        Vec3 linkStart, linkEnd;
    };
    struct Poly {
        u32  region = 0, local = 0;
        std::vector<Vec3> pts;               // counter-clockwise in XY
        Vec3 centre, lo, hi;
        u32  layers = 1;
        f32  enterCost = 0.0f, travelCost = 1.0f;
        std::vector<Conn> conns;
    };
    struct Map {
        u32 id = 0;
        bool active = true, dirty = true;
        NavMapParams params;
        u32 iteration = 0;
        std::vector<Poly> polys;
        f32 minTravel = 1.0f;
        std::map<u32, u32> linkAttached;     // link id -> attached ends
    };

    u32 nextId_ = 1;
    std::map<u32, Map> maps_;
    std::map<u32, NavRegionData> regions_;
    std::map<u32, NavLinkData> links_;
    std::map<u32, NavObstacleData> obstacles_;
    std::map<u32, NavAgentData> agents_;

    void rebuild(Map& m);
    Poly* polyAt(Map& m, u32 i) { return i < m.polys.size() ? &m.polys[i] : nullptr; }
    bool eligible(const Poly& p, const NavPathQuery& q) const;
    friend struct NavQueryImpl;
};

// ---- Geometry helpers the tests and the baker share ----

// Closest point on triangle abc to p (Ericson).
Vec3 navClosestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);
// Closest point on segment ab to p.
Vec3 navClosestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b);
// Closest point on the segments (p1,q1) and (p2,q2); returns the squared distance.
f32  navSegmentSegment(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3* c1, Vec3* c2);
// Signed area of a polygon in the XY plane (positive: counter-clockwise).
f32  navSignedAreaXY(const std::vector<Vec3>& poly);
// Ramer-Douglas-Peucker in 3D.
std::vector<Vec3> navSimplify(const std::vector<Vec3>& pts, f32 epsilon, std::vector<u32>* keptIndices = nullptr);

} // namespace aver::synapse
