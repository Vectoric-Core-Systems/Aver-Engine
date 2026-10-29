// The C seam onto polygon navigation, its baker and the A* graphs (nav_abi.h).

#include "aver/nav/nav_abi.h"

#include "aver/synapse/NavGraph.hpp"
#include "aver/synapse/NavMesh.hpp"
#include "aver/synapse/NavMeshBake.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using namespace aver;
using namespace aver::synapse;

static_assert(kNavPostCorridorFunnel == AVER_NAV_POST_FUNNEL && kNavPostEdgeCentered == AVER_NAV_POST_EDGE_CENTERED && kNavPostNone == AVER_NAV_POST_NONE, "nav_abi.h mirrors the post-processing modes");
static_assert(kNavPointRegion == AVER_NAV_POINT_REGION && kNavPointLink == AVER_NAV_POINT_LINK, "nav_abi.h mirrors the point types");
static_assert(AStarGrid::Octile == AVER_NAV_HEURISTIC_OCTILE && AStarGrid::Chebyshev == AVER_NAV_HEURISTIC_CHEBYSHEV, "heuristics");
static_assert(AStarGrid::OnlyIfNoObstacles == AVER_NAV_DIAGONAL_ONLY_IF_NO_OBSTACLE && AStarGrid::AtLeastOneWalkable == AVER_NAV_DIAGONAL_AT_LEAST_ONE, "diagonal modes");

namespace {

NavServer g_srv;
NavPathResult g_path;
std::map<i32, std::unique_ptr<AStarGraph>> g_graphs;
std::map<i32, std::unique_ptr<AStarGrid>> g_grids;
i32 g_nextGraph = 1, g_nextGrid = 1;

struct CbGraph { aver_astar_cost_fn compute = nullptr, estimate = nullptr; void* user = nullptr; };
struct CbGrid { aver_astar_grid_cost_fn compute = nullptr, estimate = nullptr; void* user = nullptr; };
std::map<i32, CbGraph> g_graphCb;
std::map<i32, CbGrid> g_gridCb;

std::mutex g_bakeMutex;
std::map<i32, NavBakeResult> g_bakes;
i32 g_nextBake = 1;
thread_local std::string t_bakeError;

AStarGraph* graph(i32 h) { auto it = g_graphs.find(h); return it == g_graphs.end() ? nullptr : it->second.get(); }
AStarGrid* grid(i32 h) { auto it = g_grids.find(h); return it == g_grids.end() ? nullptr : it->second.get(); }

Vec3 v3(const float* p) { return {p[0], p[1], p[2]}; }
void put3(float* o, const Vec3& v) { o[0] = v.x; o[1] = v.y; o[2] = v.z; }

template <class T>
i32 fillVec(const std::vector<T>& v, T* out, i32 cap) {
    if (out) for (i32 i = 0; i < cap && usize(i) < v.size(); ++i) out[i] = v[usize(i)];
    return i32(v.size());
}

}  // namespace

extern "C" {

AVER_NAV_API int32_t aver_nav_abi_version(void) { return AVER_NAV_ABI_VERSION; }

AVER_NAV_API void aver_nav_reset(void) {
    g_srv = NavServer();
    g_path = NavPathResult();
    g_graphs.clear(); g_grids.clear(); g_graphCb.clear(); g_gridCb.clear();
    std::lock_guard<std::mutex> lk(g_bakeMutex);
    g_bakes.clear();
}

// ---- maps
AVER_NAV_API int32_t aver_nav_map_create(void) { return i32(g_srv.createMap()); }
AVER_NAV_API int32_t aver_nav_map_destroy(int32_t map) { return g_srv.destroyMap(u32(map)) ? 1 : 0; }
AVER_NAV_API int32_t aver_nav_map_exists(int32_t map) { return g_srv.hasMap(u32(map)) ? 1 : 0; }
AVER_NAV_API int32_t aver_nav_map_set_active(int32_t map, int32_t active) {
    if (!g_srv.hasMap(u32(map))) return 0;
    g_srv.setMapActive(u32(map), active != 0);
    return 1;
}
AVER_NAV_API int32_t aver_nav_map_is_active(int32_t map) { return g_srv.mapActive(u32(map)) ? 1 : 0; }
AVER_NAV_API int32_t aver_nav_map_set_params(int32_t map, float cell, float cellHeight, float edgeMargin, float linkRadius, int32_t useEdge) {
    NavMapParams* p = g_srv.mapParams(u32(map));
    if (!p || cell <= 0 || cellHeight <= 0 || edgeMargin < 0 || linkRadius < 0) return 0;
    p->cellSizeCm = cell; p->cellHeightCm = cellHeight; p->edgeConnectionMarginCm = edgeMargin; p->linkConnectionRadiusCm = linkRadius;
    p->useEdgeConnections = useEdge != 0;
    g_srv.markDirty(u32(map));
    return 1;
}
AVER_NAV_API int32_t aver_nav_map_params(int32_t map, float* out) {
    const NavMapParams* p = g_srv.mapParams(u32(map));
    if (!p || !out) return 0;
    out[0] = p->cellSizeCm; out[1] = p->cellHeightCm; out[2] = p->edgeConnectionMarginCm; out[3] = p->linkConnectionRadiusCm; out[4] = p->useEdgeConnections ? 1.0f : 0.0f;
    return 1;
}
AVER_NAV_API int32_t aver_nav_map_update(int32_t map) { return g_srv.update(u32(map)) ? 1 : 0; }
AVER_NAV_API int32_t aver_nav_map_iteration(int32_t map) { return i32(g_srv.mapIteration(u32(map))); }
AVER_NAV_API int32_t aver_nav_map_list(int32_t* out, int32_t cap) {
    std::vector<i32> v;
    for (u32 m : g_srv.maps()) v.push_back(i32(m));
    return fillVec(v, out, cap);
}
AVER_NAV_API int32_t aver_nav_map_objects(int32_t map, int32_t kind, int32_t* out, int32_t cap) {
    std::vector<u32> src;
    switch (kind) {
    case AVER_NAV_KIND_REGION: src = g_srv.regionsOf(u32(map)); break;
    case AVER_NAV_KIND_LINK: src = g_srv.linksOf(u32(map)); break;
    case AVER_NAV_KIND_OBSTACLE: src = g_srv.obstaclesOf(u32(map)); break;
    case AVER_NAV_KIND_AGENT: src = g_srv.agentsOf(u32(map)); break;
    default: return 0;
    }
    std::vector<i32> v(src.begin(), src.end());
    return fillVec(v, out, cap);
}
AVER_NAV_API int32_t aver_nav_map_polygon_count(int32_t map) { return i32(g_srv.polygonCount(u32(map))); }
AVER_NAV_API int32_t aver_nav_map_step(int32_t map, float dt) {
    if (!g_srv.hasMap(u32(map))) return 0;
    g_srv.stepAvoidance(u32(map), dt);
    return 1;
}

// ---- objects
AVER_NAV_API int32_t aver_nav_region_create(int32_t map) { return i32(g_srv.createRegion(u32(map))); }
AVER_NAV_API int32_t aver_nav_link_create(int32_t map) { return i32(g_srv.createLink(u32(map))); }
AVER_NAV_API int32_t aver_nav_obstacle_create(int32_t map) { return i32(g_srv.createObstacle(u32(map))); }
AVER_NAV_API int32_t aver_nav_agent_create(int32_t map) { return i32(g_srv.createAgent(u32(map))); }
AVER_NAV_API int32_t aver_nav_destroy(int32_t id) { return g_srv.destroy(u32(id)) ? 1 : 0; }
AVER_NAV_API int32_t aver_nav_kind(int32_t id) {
    const u32 u = u32(id);
    if (g_srv.hasMap(u)) return AVER_NAV_KIND_MAP;
    if (g_srv.region(u)) return AVER_NAV_KIND_REGION;
    if (g_srv.link(u)) return AVER_NAV_KIND_LINK;
    if (g_srv.obstacle(u)) return AVER_NAV_KIND_OBSTACLE;
    if (g_srv.agent(u)) return AVER_NAV_KIND_AGENT;
    return AVER_NAV_KIND_NONE;
}
AVER_NAV_API int32_t aver_nav_set_map(int32_t id, int32_t map) {
    const u32 u = u32(id);
    if (map != 0 && !g_srv.hasMap(u32(map))) return 0;
    if (g_srv.region(u)) g_srv.setRegionMap(u, u32(map));
    else if (g_srv.link(u)) g_srv.setLinkMap(u, u32(map));
    else if (g_srv.obstacle(u)) g_srv.setObstacleMap(u, u32(map));
    else if (g_srv.agent(u)) g_srv.setAgentMap(u, u32(map));
    else return 0;
    return 1;
}
AVER_NAV_API int32_t aver_nav_get_map(int32_t id) {
    const u32 u = u32(id);
    if (auto* r = g_srv.region(u)) return i32(r->map);
    if (auto* l = g_srv.link(u)) return i32(l->map);
    if (auto* o = g_srv.obstacle(u)) return i32(o->map);
    if (auto* a = g_srv.agent(u)) return i32(a->map);
    return 0;
}
AVER_NAV_API int32_t aver_nav_set_owner(int32_t id, int64_t owner) {
    const u32 u = u32(id);
    if (auto* r = g_srv.region(u)) r->owner = u64(owner);
    else if (auto* l = g_srv.link(u)) l->owner = u64(owner);
    else if (auto* o = g_srv.obstacle(u)) o->owner = u64(owner);
    else if (auto* a = g_srv.agent(u)) a->owner = u64(owner);
    else return 0;
    return 1;
}
AVER_NAV_API int64_t aver_nav_get_owner(int32_t id) {
    const u32 u = u32(id);
    if (auto* r = g_srv.region(u)) return i64(r->owner);
    if (auto* l = g_srv.link(u)) return i64(l->owner);
    if (auto* o = g_srv.obstacle(u)) return i64(o->owner);
    if (auto* a = g_srv.agent(u)) return i64(a->owner);
    return 0;
}

// ---- regions
AVER_NAV_API int32_t aver_nav_region_set_enabled(int32_t region, int32_t enabled) {
    NavRegionData* r = g_srv.region(u32(region));
    if (!r) return 0;
    r->enabled = enabled != 0;
    g_srv.markDirty(r->map);
    return 1;
}
AVER_NAV_API int32_t aver_nav_region_set_layers(int32_t region, uint32_t layers) {
    NavRegionData* r = g_srv.region(u32(region));
    if (!r) return 0;
    r->layers = layers;
    g_srv.markDirty(r->map);
    return 1;
}
AVER_NAV_API int32_t aver_nav_region_set_costs(int32_t region, float enter, float travel) {
    NavRegionData* r = g_srv.region(u32(region));
    if (!r || enter < 0 || travel < 0) return 0;
    r->enterCost = enter; r->travelCost = travel;
    g_srv.markDirty(r->map);
    return 1;
}
AVER_NAV_API int32_t aver_nav_region_set_edge_connections(int32_t region, int32_t use) {
    NavRegionData* r = g_srv.region(u32(region));
    if (!r) return 0;
    r->useEdgeConnections = use != 0;
    g_srv.markDirty(r->map);
    return 1;
}
AVER_NAV_API int32_t aver_nav_region_edge_connections(int32_t region) {
    const NavRegionData* r = g_srv.region(u32(region));
    return r && r->useEdgeConnections ? 1 : 0;
}
AVER_NAV_API int32_t aver_nav_region_get(int32_t region, float* out) {
    const NavRegionData* r = g_srv.region(u32(region));
    if (!r || !out) return 0;
    out[0] = r->enabled ? 1.0f : 0.0f; out[1] = float(r->layers); out[2] = r->enterCost; out[3] = r->travelCost;
    return 1;
}
AVER_NAV_API int32_t aver_nav_region_set_mesh(int32_t region, const float* verts, int32_t nVerts, const int32_t* idx, const int32_t* sizes, int32_t nPolys) {
    if (nVerts < 0 || nPolys < 0 || (nVerts > 0 && !verts) || (nPolys > 0 && (!idx || !sizes))) return 0;
    std::vector<Vec3> vv;
    vv.reserve(usize(nVerts));
    for (i32 i = 0; i < nVerts; ++i) vv.push_back(v3(verts + 3 * i));
    std::vector<std::vector<u32>> pp;
    i32 at = 0;
    for (i32 p = 0; p < nPolys; ++p) {
        if (sizes[p] < 0) return 0;
        std::vector<u32> poly;
        for (i32 k = 0; k < sizes[p]; ++k) {
            if (idx[at + k] < 0) return 0;
            poly.push_back(u32(idx[at + k]));
        }
        at += sizes[p];
        pp.push_back(std::move(poly));
    }
    return g_srv.setRegionMesh(u32(region), std::move(vv), std::move(pp)) ? 1 : 0;
}
AVER_NAV_API int32_t aver_nav_region_polygon_count(int32_t region) {
    const NavRegionData* r = g_srv.region(u32(region));
    return r ? i32(r->polys.size()) : 0;
}
AVER_NAV_API int32_t aver_nav_region_closest(int32_t region, const float* p, float* outPoint, float* outNormal) {
    const NavClosest c = g_srv.closestPointOnRegion(u32(region), v3(p));
    if (!c.found) return 0;
    if (outPoint) put3(outPoint, c.point);
    if (outNormal) put3(outNormal, c.normal);
    return 1;
}
AVER_NAV_API int32_t aver_nav_region_connections(int32_t region, float* out, int32_t cap) {
    const auto v = g_srv.regionConnections(u32(region));
    if (out) for (i32 i = 0; i < cap && usize(i) < v.size(); ++i) { put3(out + 6 * i, v[usize(i)].first); put3(out + 6 * i + 3, v[usize(i)].second); }
    return i32(v.size());
}

// ---- links
AVER_NAV_API int32_t aver_nav_link_set(int32_t link, int32_t enabled, int32_t bidirectional, uint32_t layers, float enter, float travel) {
    NavLinkData* l = g_srv.link(u32(link));
    if (!l || enter < 0 || travel < 0) return 0;
    l->enabled = enabled != 0; l->bidirectional = bidirectional != 0; l->layers = layers; l->enterCost = enter; l->travelCost = travel;
    g_srv.markDirty(l->map);
    return 1;
}
AVER_NAV_API int32_t aver_nav_link_set_ends(int32_t link, const float* s, const float* e) {
    NavLinkData* l = g_srv.link(u32(link));
    if (!l || !s || !e) return 0;
    l->start = v3(s); l->end = v3(e);
    g_srv.markDirty(l->map);
    return 1;
}
AVER_NAV_API int32_t aver_nav_link_get(int32_t link, float* out) {
    const NavLinkData* l = g_srv.link(u32(link));
    if (!l || !out) return 0;
    out[0] = l->enabled ? 1.0f : 0.0f; out[1] = l->bidirectional ? 1.0f : 0.0f; out[2] = float(l->layers); out[3] = l->enterCost; out[4] = l->travelCost;
    put3(out + 5, l->start); put3(out + 8, l->end);
    return 1;
}
AVER_NAV_API int32_t aver_nav_link_connected(int32_t link) { return i32(g_srv.linkConnectedPolys(u32(link))); }

// ---- obstacles
AVER_NAV_API int32_t aver_nav_obstacle_set(int32_t obstacle, int32_t avoidance, int32_t paused, uint32_t layers, float radius, float height) {
    NavObstacleData* o = g_srv.obstacle(u32(obstacle));
    if (!o || radius < 0 || height < 0) return 0;
    o->avoidance = avoidance != 0; o->paused = paused != 0; o->layers = layers; o->radius = radius; o->height = height;
    return 1;
}
AVER_NAV_API int32_t aver_nav_obstacle_set_motion(int32_t obstacle, const float* pos, const float* vel) {
    NavObstacleData* o = g_srv.obstacle(u32(obstacle));
    if (!o || !pos || !vel) return 0;
    o->position = v3(pos); o->velocity = v3(vel);
    return 1;
}
AVER_NAV_API int32_t aver_nav_obstacle_set_vertices(int32_t obstacle, const float* verts, int32_t n) {
    NavObstacleData* o = g_srv.obstacle(u32(obstacle));
    if (!o || n < 0 || (n > 0 && !verts)) return 0;
    o->verts.clear();
    for (i32 i = 0; i < n; ++i) o->verts.push_back(v3(verts + 3 * i));
    return 1;
}
AVER_NAV_API int32_t aver_nav_obstacle_get(int32_t obstacle, float* out) {
    const NavObstacleData* o = g_srv.obstacle(u32(obstacle));
    if (!o || !out) return 0;
    out[0] = o->avoidance ? 1.0f : 0.0f; out[1] = o->paused ? 1.0f : 0.0f; out[2] = float(o->layers); out[3] = o->radius; out[4] = o->height; out[5] = float(o->verts.size());
    return 1;
}

// ---- agents
AVER_NAV_API int32_t aver_nav_agent_set_params(int32_t agent, const float* p) {
    NavAgentData* a = g_srv.agent(u32(agent));
    if (!a || !p) return 0;
    a->avoidance = p[AVER_NAV_AGENT_AVOIDANCE] != 0.0f; a->paused = p[AVER_NAV_AGENT_PAUSED] != 0.0f;
    a->radius = std::max(p[AVER_NAV_AGENT_RADIUS], 0.0f); a->height = std::max(p[AVER_NAV_AGENT_HEIGHT], 0.0f);
    a->maxSpeed = std::max(p[AVER_NAV_AGENT_MAX_SPEED], 0.0f); a->neighborDistance = std::max(p[AVER_NAV_AGENT_NEIGHBOR_DIST], 0.0f);
    a->maxNeighbors = u32(std::max(p[AVER_NAV_AGENT_MAX_NEIGHBORS], 0.0f));
    a->timeHorizonAgents = std::max(p[AVER_NAV_AGENT_HORIZON_AGENTS], 0.0f); a->timeHorizonObstacles = std::max(p[AVER_NAV_AGENT_HORIZON_OBSTACLES], 0.0f);
    a->avoidanceLayers = u32(p[AVER_NAV_AGENT_LAYERS]); a->avoidanceMask = u32(p[AVER_NAV_AGENT_MASK]); a->priority = std::clamp(p[AVER_NAV_AGENT_PRIORITY], 0.0f, 1.0f);
    return 1;
}
AVER_NAV_API int32_t aver_nav_agent_get_params(int32_t agent, float* o) {
    const NavAgentData* a = g_srv.agent(u32(agent));
    if (!a || !o) return 0;
    o[AVER_NAV_AGENT_AVOIDANCE] = a->avoidance ? 1.0f : 0.0f; o[AVER_NAV_AGENT_PAUSED] = a->paused ? 1.0f : 0.0f;
    o[AVER_NAV_AGENT_RADIUS] = a->radius; o[AVER_NAV_AGENT_HEIGHT] = a->height; o[AVER_NAV_AGENT_MAX_SPEED] = a->maxSpeed;
    o[AVER_NAV_AGENT_NEIGHBOR_DIST] = a->neighborDistance; o[AVER_NAV_AGENT_MAX_NEIGHBORS] = float(a->maxNeighbors);
    o[AVER_NAV_AGENT_HORIZON_AGENTS] = a->timeHorizonAgents; o[AVER_NAV_AGENT_HORIZON_OBSTACLES] = a->timeHorizonObstacles;
    o[AVER_NAV_AGENT_LAYERS] = float(a->avoidanceLayers); o[AVER_NAV_AGENT_MASK] = float(a->avoidanceMask); o[AVER_NAV_AGENT_PRIORITY] = a->priority;
    return 1;
}
AVER_NAV_API int32_t aver_nav_agent_set_position(int32_t agent, const float* p) {
    NavAgentData* a = g_srv.agent(u32(agent));
    if (!a || !p) return 0;
    a->position = v3(p);
    return 1;
}
AVER_NAV_API int32_t aver_nav_agent_set_velocity(int32_t agent, const float* v) {
    NavAgentData* a = g_srv.agent(u32(agent));
    if (!a || !v) return 0;
    a->velocity = v3(v);
    a->hasVelocity = true;
    a->hasSafe = false;
    return 1;
}
AVER_NAV_API int32_t aver_nav_agent_safe_velocity(int32_t agent, float* out) {
    NavAgentData* a = g_srv.agent(u32(agent));
    if (!a || !a->hasSafe) return 0;
    if (out) put3(out, a->safeVelocity);
    a->hasSafe = false;
    return 1;
}

// ---- queries
AVER_NAV_API int32_t aver_nav_path_query(int32_t map, const float* start, const float* target, uint32_t layers, int32_t post, float simplify,
                                         const int32_t* excluded, int32_t nEx, const int32_t* included, int32_t nIn) {
    g_path = NavPathResult();
    if (!start || !target || !g_srv.hasMap(u32(map)) || !g_srv.mapActive(u32(map))) return 0;
    NavPathQuery q;
    q.map = u32(map); q.start = v3(start); q.target = v3(target); q.layers = layers; q.postprocess = post;
    q.simplify = simplify > 0.0f; q.simplifyEpsilonCm = simplify;
    for (i32 i = 0; excluded && i < nEx; ++i) q.excludedRegions.push_back(u32(excluded[i]));
    for (i32 i = 0; included && i < nIn; ++i) q.includedRegions.push_back(u32(included[i]));
    g_path = g_srv.queryPath(q);
    return i32(g_path.points.size());
}
AVER_NAV_API int32_t aver_nav_path_points(float* out, int32_t cap) {
    if (out) for (i32 i = 0; i < cap && usize(i) < g_path.points.size(); ++i) put3(out + 3 * i, g_path.points[usize(i)]);
    return i32(g_path.points.size());
}
AVER_NAV_API int32_t aver_nav_path_info(int32_t* types, int32_t* rids, int64_t* owners, int32_t cap) {
    for (i32 i = 0; i < cap && usize(i) < g_path.points.size(); ++i) {
        if (types) types[i] = g_path.types[usize(i)];
        if (rids) rids[i] = i32(g_path.rids[usize(i)]);
        if (owners) owners[i] = i64(g_path.owners[usize(i)]);
    }
    return i32(g_path.points.size());
}
AVER_NAV_API float aver_nav_path_length(void) { return g_path.lengthCm; }
AVER_NAV_API int32_t aver_nav_path_reached(void) { return g_path.reachedTarget ? 1 : 0; }
AVER_NAV_API float aver_nav_path_gap(void) { return g_path.endGapCm; }
AVER_NAV_API int32_t aver_nav_map_closest(int32_t map, const float* p, uint32_t layers, float* outPoint, float* outNormal, int32_t* outRegion, int64_t* outOwner) {
    if (!p) return 0;
    const NavClosest c = g_srv.closestPoint(u32(map), v3(p), layers);
    if (!c.found) return 0;
    if (outPoint) put3(outPoint, c.point);
    if (outNormal) put3(outNormal, c.normal);
    if (outRegion) *outRegion = i32(c.region);
    if (outOwner) *outOwner = i64(c.owner);
    return 1;
}
AVER_NAV_API int32_t aver_nav_map_closest_segment(int32_t map, const float* a, const float* b, float* outMesh, float* outSeg) {
    if (!a || !b || !g_srv.hasMap(u32(map)) || g_srv.polygonCount(u32(map)) == 0) return 0;
    Vec3 onSeg;
    const Vec3 onMesh = g_srv.closestPointToSegment(u32(map), v3(a), v3(b), &onSeg);
    if (outMesh) put3(outMesh, onMesh);
    if (outSeg) put3(outSeg, onSeg);
    return 1;
}
AVER_NAV_API int32_t aver_nav_map_random_point(int32_t map, uint32_t layers, int32_t uniformly, float r1, float r2, float r3, float* out) {
    bool found = false;
    const Vec3 p = g_srv.randomPoint(u32(map), layers, uniformly != 0, r1, r2, r3, &found);
    if (!found) return 0;
    if (out) put3(out, p);
    return 1;
}

// ---- baking
AVER_NAV_API int32_t aver_nav_bake(const float* verts, int32_t nVerts, const int32_t* tris, int32_t nTri, const float* obstacles, int32_t nObstacles,
                                   const float* params, int32_t nParams) {
    t_bakeError.clear();
    if (nVerts < 0 || nTri < 0 || (nVerts > 0 && !verts) || (nTri > 0 && !tris)) { t_bakeError = "bad geometry arguments"; return 0; }
    std::vector<Vec3> vv;
    vv.reserve(usize(nVerts));
    for (i32 i = 0; i < nVerts; ++i) vv.push_back(v3(verts + 3 * i));
    std::vector<u32> tt;
    tt.reserve(usize(nTri));
    for (i32 i = 0; i < nTri; ++i) {
        if (tris[i] < 0) { t_bakeError = "a triangle index is negative"; return 0; }
        tt.push_back(u32(tris[i]));
    }
    NavBakeParams p;
    if (params && nParams >= AVER_NAV_BAKE_PARAM_COUNT) {
        p.cellSizeCm = params[AVER_NAV_BAKE_CELL_SIZE]; p.cellHeightCm = params[AVER_NAV_BAKE_CELL_HEIGHT];
        p.agentHeightCm = params[AVER_NAV_BAKE_AGENT_HEIGHT]; p.agentRadiusCm = params[AVER_NAV_BAKE_AGENT_RADIUS];
        p.agentMaxClimbCm = params[AVER_NAV_BAKE_AGENT_MAX_CLIMB]; p.agentMaxSlopeDeg = params[AVER_NAV_BAKE_AGENT_MAX_SLOPE];
        p.regionMinSize = params[AVER_NAV_BAKE_REGION_MIN_SIZE]; p.regionMergeSize = params[AVER_NAV_BAKE_REGION_MERGE_SIZE];
        p.edgeMaxLengthCm = params[AVER_NAV_BAKE_EDGE_MAX_LENGTH]; p.edgeMaxError = params[AVER_NAV_BAKE_EDGE_MAX_ERROR];
        p.verticesPerPolygon = i32(params[AVER_NAV_BAKE_VERTS_PER_POLY]); p.detailSampleDistance = params[AVER_NAV_BAKE_DETAIL_DISTANCE];
        p.detailSampleMaxErrorCm = params[AVER_NAV_BAKE_DETAIL_MAX_ERROR];
        p.filterLowHangingObstacles = params[AVER_NAV_BAKE_FILTER_LOW_HANGING] != 0.0f; p.filterLedgeSpans = params[AVER_NAV_BAKE_FILTER_LEDGE] != 0.0f;
        p.filterWalkableLowHeightSpans = params[AVER_NAV_BAKE_FILTER_LOW_HEIGHT] != 0.0f; p.borderSizeCm = params[AVER_NAV_BAKE_BORDER_SIZE];
        p.useBounds = params[AVER_NAV_BAKE_USE_BOUNDS] != 0.0f;
        p.boundsMin = v3(params + AVER_NAV_BAKE_BOUNDS_MIN); p.boundsMax = v3(params + AVER_NAV_BAKE_BOUNDS_MAX);
    } else if (params) {
        t_bakeError = "too few bake parameters";
        return 0;
    }
    std::vector<NavBakeObstacle> obs;
    const float* at = obstacles;
    for (i32 i = 0; i < nObstacles; ++i) {
        if (!at) { t_bakeError = "obstacle data missing"; return 0; }
        NavBakeObstacle o;
        o.position = v3(at); o.radius = at[3]; o.zMin = at[4]; o.height = at[5];
        const i32 n = i32(at[6]);
        for (i32 k = 0; k < n; ++k) o.verts.push_back(v3(at + 7 + 3 * k));
        at += 7 + 3 * n;
        obs.push_back(std::move(o));
    }
    NavBakeResult r;
    std::string why;
    if (!navBake(vv, tt, obs, p, r, &why)) { t_bakeError = why; return 0; }
    std::lock_guard<std::mutex> lk(g_bakeMutex);
    const i32 h = g_nextBake++;
    g_bakes[h] = std::move(r);
    return h;
}
AVER_NAV_API const char* aver_nav_bake_error(void) { return t_bakeError.c_str(); }
AVER_NAV_API int32_t aver_nav_bake_counts(int32_t bake, int32_t* out) {
    std::lock_guard<std::mutex> lk(g_bakeMutex);
    auto it = g_bakes.find(bake);
    if (it == g_bakes.end() || !out) return 0;
    i32 idx = 0;
    for (const auto& p : it->second.polys) idx += i32(p.size());
    out[0] = i32(it->second.verts.size()); out[1] = i32(it->second.polys.size()); out[2] = idx;
    return 1;
}
AVER_NAV_API int32_t aver_nav_bake_get(int32_t bake, float* verts, int32_t* sizes, int32_t* indices) {
    std::lock_guard<std::mutex> lk(g_bakeMutex);
    auto it = g_bakes.find(bake);
    if (it == g_bakes.end()) return 0;
    const NavBakeResult& r = it->second;
    if (verts) for (usize i = 0; i < r.verts.size(); ++i) put3(verts + 3 * i, r.verts[i]);
    i32 at = 0;
    for (usize p = 0; p < r.polys.size(); ++p) {
        if (sizes) sizes[p] = i32(r.polys[p].size());
        if (indices) for (u32 k : r.polys[p]) indices[at++] = i32(k);
    }
    return 1;
}
AVER_NAV_API int32_t aver_nav_bake_stats(int32_t bake, int32_t* out) {
    std::lock_guard<std::mutex> lk(g_bakeMutex);
    auto it = g_bakes.find(bake);
    if (it == g_bakes.end() || !out) return 0;
    const NavBakeStats& s = it->second.stats;
    out[0] = i32(s.columns); out[1] = i32(s.spans); out[2] = i32(s.walkableSpans); out[3] = i32(s.regions); out[4] = i32(s.regionsRemoved); out[5] = i32(s.rectangles);
    return 1;
}
AVER_NAV_API int32_t aver_nav_bake_free(int32_t bake) {
    std::lock_guard<std::mutex> lk(g_bakeMutex);
    return g_bakes.erase(bake) ? 1 : 0;
}

// ---- A* graphs
AVER_NAV_API int32_t aver_astar_create(void) {
    const i32 h = g_nextGraph++;
    g_graphs[h] = std::make_unique<AStarGraph>();
    return h;
}
AVER_NAV_API int32_t aver_astar_destroy(int32_t g) { g_graphCb.erase(g); return g_graphs.erase(g) ? 1 : 0; }
AVER_NAV_API int32_t aver_astar_clear(int32_t g) { AStarGraph* a = graph(g); if (!a) return 0; a->clear(); return 1; }
AVER_NAV_API int32_t aver_astar_add_point(int32_t g, int64_t id, float x, float y, float z, float w) {
    AStarGraph* a = graph(g);
    if (!a || id < 0) return 0;
    a->addPoint(id, {x, y, z}, w);
    return 1;
}
AVER_NAV_API int32_t aver_astar_has_point(int32_t g, int64_t id) { AStarGraph* a = graph(g); return a && a->hasPoint(id) ? 1 : 0; }
AVER_NAV_API int32_t aver_astar_point_position(int32_t g, int64_t id, float* out) {
    AStarGraph* a = graph(g);
    if (!a || !a->hasPoint(id) || !out) return 0;
    put3(out, a->pointPosition(id));
    return 1;
}
AVER_NAV_API float aver_astar_point_weight(int32_t g, int64_t id) { AStarGraph* a = graph(g); return a ? a->pointWeight(id) : 0.0f; }
AVER_NAV_API int32_t aver_astar_set_point_position(int32_t g, int64_t id, float x, float y, float z) {
    AStarGraph* a = graph(g);
    if (!a || !a->hasPoint(id)) return 0;
    a->setPointPosition(id, {x, y, z});
    return 1;
}
AVER_NAV_API int32_t aver_astar_set_point_weight(int32_t g, int64_t id, float w) {
    AStarGraph* a = graph(g);
    if (!a || !a->hasPoint(id) || w < 0) return 0;
    a->setPointWeight(id, w);
    return 1;
}
AVER_NAV_API int32_t aver_astar_remove_point(int32_t g, int64_t id) {
    AStarGraph* a = graph(g);
    if (!a || !a->hasPoint(id)) return 0;
    a->removePoint(id);
    return 1;
}
AVER_NAV_API int32_t aver_astar_set_disabled(int32_t g, int64_t id, int32_t d) {
    AStarGraph* a = graph(g);
    if (!a || !a->hasPoint(id)) return 0;
    a->setDisabled(id, d != 0);
    return 1;
}
AVER_NAV_API int32_t aver_astar_is_disabled(int32_t g, int64_t id) { AStarGraph* a = graph(g); return a && a->isDisabled(id) ? 1 : 0; }
AVER_NAV_API int32_t aver_astar_point_count(int32_t g) { AStarGraph* a = graph(g); return a ? i32(a->pointCount()) : 0; }
AVER_NAV_API int32_t aver_astar_point_ids(int32_t g, int64_t* out, int32_t cap) {
    AStarGraph* a = graph(g);
    return a ? fillVec(a->pointIds(), out, cap) : 0;
}
AVER_NAV_API int64_t aver_astar_next_free_id(int32_t g) { AStarGraph* a = graph(g); return a ? a->nextFreeId() : 0; }
AVER_NAV_API int32_t aver_astar_connect(int32_t g, int64_t id, int64_t to, int32_t bi) { AStarGraph* a = graph(g); return a && a->connect(id, to, bi != 0) ? 1 : 0; }
AVER_NAV_API int32_t aver_astar_disconnect(int32_t g, int64_t id, int64_t to, int32_t bi) {
    AStarGraph* a = graph(g);
    if (!a || !a->hasPoint(id) || !a->hasPoint(to)) return 0;
    a->disconnect(id, to, bi != 0);
    return 1;
}
AVER_NAV_API int32_t aver_astar_are_connected(int32_t g, int64_t id, int64_t to, int32_t bi) { AStarGraph* a = graph(g); return a && a->areConnected(id, to, bi != 0) ? 1 : 0; }
AVER_NAV_API int32_t aver_astar_connections(int32_t g, int64_t id, int64_t* out, int32_t cap) {
    AStarGraph* a = graph(g);
    return a ? fillVec(a->connections(id), out, cap) : 0;
}
AVER_NAV_API int64_t aver_astar_closest_point(int32_t g, float x, float y, float z, int32_t inc) { AStarGraph* a = graph(g); return a ? a->closestPoint({x, y, z}, inc != 0) : -1; }
AVER_NAV_API int32_t aver_astar_closest_in_segment(int32_t g, float x, float y, float z, float* out) {
    AStarGraph* a = graph(g);
    if (!a || !out) return 0;
    put3(out, a->closestPositionInSegment({x, y, z}));
    return 1;
}
AVER_NAV_API int32_t aver_astar_set_callbacks(int32_t g, aver_astar_cost_fn compute, aver_astar_cost_fn estimate, void* user) {
    AStarGraph* a = graph(g);
    if (!a) return 0;
    g_graphCb[g] = {compute, estimate, user};
    if (compute) a->setComputeCost([g, compute, user](i64 f, i64 t) { return compute(user, f, t); }); else a->setComputeCost(nullptr);
    if (estimate) a->setEstimateCost([g, estimate, user](i64 f, i64 t) { return estimate(user, f, t); }); else a->setEstimateCost(nullptr);
    return 1;
}
AVER_NAV_API int32_t aver_astar_id_path(int32_t g, int64_t from, int64_t to, int32_t partial, int64_t* out, int32_t cap) {
    AStarGraph* a = graph(g);
    if (!a) return 0;
    return fillVec(a->idPath(from, to, partial != 0), out, cap);
}

// ---- A* grids
AVER_NAV_API int32_t aver_agrid_create(void) {
    const i32 h = g_nextGrid++;
    g_grids[h] = std::make_unique<AStarGrid>();
    return h;
}
AVER_NAV_API int32_t aver_agrid_destroy(int32_t g) { g_gridCb.erase(g); return g_grids.erase(g) ? 1 : 0; }
AVER_NAV_API int32_t aver_agrid_set_region(int32_t g, int32_t x, int32_t y, int32_t w, int32_t h) {
    AStarGrid* a = grid(g);
    if (!a || w < 0 || h < 0) return 0;
    a->setRegion({x, y, w, h});
    return 1;
}
AVER_NAV_API int32_t aver_agrid_region(int32_t g, int32_t* out) {
    AStarGrid* a = grid(g);
    if (!a || !out) return 0;
    const auto r = a->region();
    out[0] = r.x; out[1] = r.y; out[2] = r.w; out[3] = r.h;
    return 1;
}
AVER_NAV_API int32_t aver_agrid_set_cell_size(int32_t g, float x, float y) { AStarGrid* a = grid(g); if (!a) return 0; a->setCellSize(x, y); return 1; }
AVER_NAV_API int32_t aver_agrid_set_offset(int32_t g, float x, float y) { AStarGrid* a = grid(g); if (!a) return 0; a->setOffset(x, y); return 1; }
AVER_NAV_API int32_t aver_agrid_set_diagonal_mode(int32_t g, int32_t m) {
    AStarGrid* a = grid(g);
    if (!a || m < 0 || m > 3) return 0;
    a->setDiagonalMode(AStarGrid::Diagonal(m));
    return 1;
}
AVER_NAV_API int32_t aver_agrid_diagonal_mode(int32_t g) { AStarGrid* a = grid(g); return a ? i32(a->diagonalMode()) : 0; }
AVER_NAV_API int32_t aver_agrid_set_heuristics(int32_t g, int32_t c, int32_t e) {
    AStarGrid* a = grid(g);
    if (!a || c < 0 || c > 3 || e < 0 || e > 3) return 0;
    a->setComputeHeuristic(AStarGrid::Heuristic(c));
    a->setEstimateHeuristic(AStarGrid::Heuristic(e));
    return 1;
}
AVER_NAV_API int32_t aver_agrid_heuristics(int32_t g, int32_t* out) {
    AStarGrid* a = grid(g);
    if (!a || !out) return 0;
    out[0] = i32(a->computeHeuristic()); out[1] = i32(a->estimateHeuristic());
    return 1;
}
AVER_NAV_API int32_t aver_agrid_set_jumping(int32_t g, int32_t on) { AStarGrid* a = grid(g); if (!a) return 0; a->setJumping(on != 0); return 1; }
AVER_NAV_API int32_t aver_agrid_jumping(int32_t g) { AStarGrid* a = grid(g); return a && a->jumping() ? 1 : 0; }
AVER_NAV_API int32_t aver_agrid_update(int32_t g) { AStarGrid* a = grid(g); if (!a) return 0; a->update(); return 1; }
AVER_NAV_API int32_t aver_agrid_is_dirty(int32_t g) { AStarGrid* a = grid(g); return a && a->isDirty() ? 1 : 0; }
AVER_NAV_API int32_t aver_agrid_clear(int32_t g) { AStarGrid* a = grid(g); if (!a) return 0; a->clear(); return 1; }
AVER_NAV_API int32_t aver_agrid_set_solid(int32_t g, int32_t x, int32_t y, int32_t s) {
    AStarGrid* a = grid(g);
    if (!a || a->isDirty() || !a->inBounds(x, y)) return 0;
    a->setSolid(x, y, s != 0);
    return 1;
}
AVER_NAV_API int32_t aver_agrid_is_solid(int32_t g, int32_t x, int32_t y) { AStarGrid* a = grid(g); return a && a->isSolid(x, y) ? 1 : 0; }
AVER_NAV_API int32_t aver_agrid_fill_solid(int32_t g, int32_t x, int32_t y, int32_t w, int32_t h, int32_t s) {
    AStarGrid* a = grid(g);
    if (!a || a->isDirty()) return 0;
    a->fillSolid({x, y, w, h}, s != 0);
    return 1;
}
AVER_NAV_API int32_t aver_agrid_set_weight(int32_t g, int32_t x, int32_t y, float w) {
    AStarGrid* a = grid(g);
    if (!a || a->isDirty() || !a->inBounds(x, y) || w < 0) return 0;
    a->setWeight(x, y, w);
    return 1;
}
AVER_NAV_API float aver_agrid_weight(int32_t g, int32_t x, int32_t y) { AStarGrid* a = grid(g); return a ? a->weight(x, y) : 0.0f; }
AVER_NAV_API int32_t aver_agrid_fill_weight(int32_t g, int32_t x, int32_t y, int32_t w, int32_t h, float weight) {
    AStarGrid* a = grid(g);
    if (!a || a->isDirty() || weight < 0) return 0;
    a->fillWeight({x, y, w, h}, weight);
    return 1;
}
AVER_NAV_API int32_t aver_agrid_set_callbacks(int32_t g, aver_astar_grid_cost_fn compute, aver_astar_grid_cost_fn estimate, void* user) {
    AStarGrid* a = grid(g);
    if (!a) return 0;
    g_gridCb[g] = {compute, estimate, user};
    if (compute) a->setComputeCost([compute, user](i32 fx, i32 fy, i32 tx, i32 ty) { return compute(user, fx, fy, tx, ty); }); else a->setComputeCost(nullptr);
    if (estimate) a->setEstimateCost([estimate, user](i32 fx, i32 fy, i32 tx, i32 ty) { return estimate(user, fx, fy, tx, ty); }); else a->setEstimateCost(nullptr);
    return 1;
}
AVER_NAV_API int32_t aver_agrid_id_path(int32_t g, int32_t fx, int32_t fy, int32_t tx, int32_t ty, int32_t partial, int32_t* out, int32_t cap) {
    AStarGrid* a = grid(g);
    if (!a) return 0;
    const auto p = a->idPath(fx, fy, tx, ty, partial != 0);
    if (out) for (i32 i = 0; i < cap && usize(i) < p.size(); ++i) { out[2 * i] = p[usize(i)].first; out[2 * i + 1] = p[usize(i)].second; }
    return i32(p.size());
}
AVER_NAV_API int32_t aver_agrid_point_path(int32_t g, int32_t fx, int32_t fy, int32_t tx, int32_t ty, int32_t partial, float* out, int32_t cap) {
    AStarGrid* a = grid(g);
    if (!a) return 0;
    const auto p = a->pointPath(fx, fy, tx, ty, partial != 0);
    if (out) for (i32 i = 0; i < cap && usize(i) < p.size(); ++i) { out[2 * i] = p[usize(i)].first; out[2 * i + 1] = p[usize(i)].second; }
    return i32(p.size());
}
AVER_NAV_API int32_t aver_agrid_point_position(int32_t g, int32_t x, int32_t y, float* out) {
    AStarGrid* a = grid(g);
    if (!a || !out) return 0;
    const auto p = a->pointPosition(x, y);
    out[0] = p.first; out[1] = p.second;
    return 1;
}

}  // extern "C"
