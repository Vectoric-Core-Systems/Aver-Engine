#pragma once
/* Aver.Nav.Abi -- the plain-C seam onto polygon navigation (aver/synapse/NavMesh.hpp), its baker (NavMeshBake.hpp) and the generic A*
 * graphs (NavGraph.hpp), so Aver.Fabric's NavigationServer3D, NavigationRegion3D, NavigationAgent3D, AStar3D and AStarGrid2D drive them.
 * Conventions as docs/ABI.md section 13: setters return 1 on success and 0 on a rejected request (stale handle, bad argument), getters
 * return the value or a neutral one (0, -1 for "none") for a stale handle. Handles are int32, 0 invalid; maps, regions, links,
 * obstacles and agents share one id space (aver_nav_kind tells them apart). Single game thread, except aver_nav_bake*, which is pure
 * and may run on a worker thread.
 *
 * SPACE. Engine space (centimetres, +Z up). A polygon mesh crosses as world-space vertices (float x, y, z) and index lists.
 * NAVIGATION LAYERS are 32-bit masks (uint32 passed as int32 bit pattern). Costs are Godot's enter_cost / travel_cost multipliers.
 *
 * ENUMS (mirrored by hand in Aver.Fabric/Nodes/NavigationNative.cs and checked by AbiEnumTest):
 *   postprocess 0 corridor funnel, 1 edge centred, 2 none
 *   kind 0 none, 1 map, 2 region, 3 link, 4 obstacle, 5 agent
 *   point type 0 region, 1 link
 *   heuristic 0 euclidean, 1 manhattan, 2 octile, 3 chebyshev;  diagonal mode 0 always, 1 never, 2 at least one walkable, 3 only if no obstacles
 */
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_NAV_ABI_BUILD)
#    define AVER_NAV_API __declspec(dllexport)
#  else
#    define AVER_NAV_API __declspec(dllimport)
#  endif
#else
#  define AVER_NAV_API
#endif

#define AVER_NAV_ABI_VERSION 1

#define AVER_NAV_POST_FUNNEL        0
#define AVER_NAV_POST_EDGE_CENTERED 1
#define AVER_NAV_POST_NONE          2

#define AVER_NAV_KIND_NONE     0
#define AVER_NAV_KIND_MAP      1
#define AVER_NAV_KIND_REGION   2
#define AVER_NAV_KIND_LINK     3
#define AVER_NAV_KIND_OBSTACLE 4
#define AVER_NAV_KIND_AGENT    5

#define AVER_NAV_POINT_REGION 0
#define AVER_NAV_POINT_LINK   1

#define AVER_NAV_HEURISTIC_EUCLIDEAN 0
#define AVER_NAV_HEURISTIC_MANHATTAN 1
#define AVER_NAV_HEURISTIC_OCTILE    2
#define AVER_NAV_HEURISTIC_CHEBYSHEV 3

#define AVER_NAV_DIAGONAL_ALWAYS             0
#define AVER_NAV_DIAGONAL_NEVER              1
#define AVER_NAV_DIAGONAL_AT_LEAST_ONE       2
#define AVER_NAV_DIAGONAL_ONLY_IF_NO_OBSTACLE 3

/* aver_nav_bake parameter slots (a float array of AVER_NAV_BAKE_PARAM_COUNT; centimetres and degrees) */
#define AVER_NAV_BAKE_CELL_SIZE        0
#define AVER_NAV_BAKE_CELL_HEIGHT      1
#define AVER_NAV_BAKE_AGENT_HEIGHT     2
#define AVER_NAV_BAKE_AGENT_RADIUS     3
#define AVER_NAV_BAKE_AGENT_MAX_CLIMB  4
#define AVER_NAV_BAKE_AGENT_MAX_SLOPE  5
#define AVER_NAV_BAKE_REGION_MIN_SIZE  6
#define AVER_NAV_BAKE_REGION_MERGE_SIZE 7
#define AVER_NAV_BAKE_EDGE_MAX_LENGTH  8
#define AVER_NAV_BAKE_EDGE_MAX_ERROR   9
#define AVER_NAV_BAKE_VERTS_PER_POLY   10
#define AVER_NAV_BAKE_DETAIL_DISTANCE  11
#define AVER_NAV_BAKE_DETAIL_MAX_ERROR 12
#define AVER_NAV_BAKE_FILTER_LOW_HANGING 13
#define AVER_NAV_BAKE_FILTER_LEDGE     14
#define AVER_NAV_BAKE_FILTER_LOW_HEIGHT 15
#define AVER_NAV_BAKE_BORDER_SIZE      16
#define AVER_NAV_BAKE_USE_BOUNDS       17
#define AVER_NAV_BAKE_BOUNDS_MIN       18   /* three floats */
#define AVER_NAV_BAKE_BOUNDS_MAX       21   /* three floats */
#define AVER_NAV_BAKE_PARAM_COUNT      24

#ifdef __cplusplus
extern "C" {
#endif

/* Callbacks for scripted costs (AStar3D._compute_cost / _estimate_cost, AStarGrid2D likewise). */
typedef float (*aver_astar_cost_fn)(void* user, int64_t fromId, int64_t toId);
typedef float (*aver_astar_grid_cost_fn)(void* user, int32_t fromX, int32_t fromY, int32_t toX, int32_t toY);

AVER_NAV_API int32_t aver_nav_abi_version(void);
AVER_NAV_API void    aver_nav_reset(void);                       /* forgets every map, object, graph and bake result (tests) */

/* ---- maps ---------------------------------------------------------------------------------------------------------- */
AVER_NAV_API int32_t aver_nav_map_create(void);                  /* a new, active map */
AVER_NAV_API int32_t aver_nav_map_destroy(int32_t map);          /* its objects stay, detached */
AVER_NAV_API int32_t aver_nav_map_exists(int32_t map);
AVER_NAV_API int32_t aver_nav_map_set_active(int32_t map, int32_t active);
AVER_NAV_API int32_t aver_nav_map_is_active(int32_t map);
AVER_NAV_API int32_t aver_nav_map_set_params(int32_t map, float cellSizeCm, float cellHeightCm, float edgeMarginCm, float linkRadiusCm, int32_t useEdgeConnections);
AVER_NAV_API int32_t aver_nav_map_params(int32_t map, float* out5);   /* cell size, cell height, edge margin, link radius, use edge connections */
AVER_NAV_API int32_t aver_nav_map_update(int32_t map);           /* map_force_update: 1 when it rebuilt */
AVER_NAV_API int32_t aver_nav_map_iteration(int32_t map);        /* changes whenever the polygons are rebuilt; 0 for a stale handle */
AVER_NAV_API int32_t aver_nav_map_list(int32_t* outIds, int32_t cap);                     /* returns the count; fills at most cap */
AVER_NAV_API int32_t aver_nav_map_objects(int32_t map, int32_t kind, int32_t* outIds, int32_t cap);   /* kind 2 regions, 3 links, 4 obstacles, 5 agents */
AVER_NAV_API int32_t aver_nav_map_polygon_count(int32_t map);
AVER_NAV_API int32_t aver_nav_map_step(int32_t map, float dtSeconds);   /* avoidance: computes the safe velocity of every agent whose velocity was set */

/* ---- objects ------------------------------------------------------------------------------------------------------- */
AVER_NAV_API int32_t aver_nav_region_create(int32_t map);        /* map 0 makes a detached region */
AVER_NAV_API int32_t aver_nav_link_create(int32_t map);
AVER_NAV_API int32_t aver_nav_obstacle_create(int32_t map);
AVER_NAV_API int32_t aver_nav_agent_create(int32_t map);
AVER_NAV_API int32_t aver_nav_destroy(int32_t id);               /* a region, link, obstacle or agent */
AVER_NAV_API int32_t aver_nav_kind(int32_t id);
AVER_NAV_API int32_t aver_nav_set_map(int32_t id, int32_t map);
AVER_NAV_API int32_t aver_nav_get_map(int32_t id);               /* 0 for detached or stale */
AVER_NAV_API int32_t aver_nav_set_owner(int32_t id, int64_t owner);
AVER_NAV_API int64_t aver_nav_get_owner(int32_t id);

/* regions */
AVER_NAV_API int32_t aver_nav_region_set_enabled(int32_t region, int32_t enabled);
AVER_NAV_API int32_t aver_nav_region_set_layers(int32_t region, uint32_t layers);
AVER_NAV_API int32_t aver_nav_region_set_costs(int32_t region, float enterCost, float travelCost);
AVER_NAV_API int32_t aver_nav_region_set_edge_connections(int32_t region, int32_t useEdgeConnections);   /* default 1; 0 keeps it from joining other regions' edges */
AVER_NAV_API int32_t aver_nav_region_edge_connections(int32_t region);
AVER_NAV_API int32_t aver_nav_region_get(int32_t region, float* out4);   /* enabled, layers (as a float of the integer), enter cost, travel cost */
/* verts: nVerts * 3 floats; polyIndices: the polygons' indices back to back, polySizes: how many each has. 0 when an index is out of range. */
AVER_NAV_API int32_t aver_nav_region_set_mesh(int32_t region, const float* verts3, int32_t nVerts, const int32_t* polyIndices, const int32_t* polySizes, int32_t nPolys);
AVER_NAV_API int32_t aver_nav_region_polygon_count(int32_t region);      /* polygons the region holds */
AVER_NAV_API int32_t aver_nav_region_closest(int32_t region, const float* p3, float* outPoint3, float* outNormal3);
AVER_NAV_API int32_t aver_nav_region_connections(int32_t region, float* out6, int32_t cap);   /* edges shared with other regions: (ax,ay,az,bx,by,bz) each; returns the count */

/* links */
AVER_NAV_API int32_t aver_nav_link_set(int32_t link, int32_t enabled, int32_t bidirectional, uint32_t layers, float enterCost, float travelCost);
AVER_NAV_API int32_t aver_nav_link_set_ends(int32_t link, const float* start3, const float* end3);
AVER_NAV_API int32_t aver_nav_link_get(int32_t link, float* out11);      /* enabled, bidirectional, layers, enter, travel, start xyz, end xyz */
AVER_NAV_API int32_t aver_nav_link_connected(int32_t link);              /* how many of its ends attached to a polygon (0..2) after the last update */

/* obstacles */
AVER_NAV_API int32_t aver_nav_obstacle_set(int32_t obstacle, int32_t avoidance, int32_t paused, uint32_t layers, float radiusCm, float heightCm);
AVER_NAV_API int32_t aver_nav_obstacle_set_motion(int32_t obstacle, const float* position3, const float* velocity3);
AVER_NAV_API int32_t aver_nav_obstacle_set_vertices(int32_t obstacle, const float* verts3, int32_t nVerts);
AVER_NAV_API int32_t aver_nav_obstacle_get(int32_t obstacle, float* out6);   /* avoidance, paused, layers, radius, height, vertex count */

/* agents (avoidance only: following the path is the agent node's job, as in Godot) */
#define AVER_NAV_AGENT_AVOIDANCE        0   /* the float slots of aver_nav_agent_set_params */
#define AVER_NAV_AGENT_PAUSED           1
#define AVER_NAV_AGENT_RADIUS           2
#define AVER_NAV_AGENT_HEIGHT           3
#define AVER_NAV_AGENT_MAX_SPEED        4
#define AVER_NAV_AGENT_NEIGHBOR_DIST    5
#define AVER_NAV_AGENT_MAX_NEIGHBORS    6
#define AVER_NAV_AGENT_HORIZON_AGENTS   7
#define AVER_NAV_AGENT_HORIZON_OBSTACLES 8
#define AVER_NAV_AGENT_LAYERS           9
#define AVER_NAV_AGENT_MASK             10
#define AVER_NAV_AGENT_PRIORITY         11
#define AVER_NAV_AGENT_PARAM_COUNT      12
AVER_NAV_API int32_t aver_nav_agent_set_params(int32_t agent, const float* params12);
AVER_NAV_API int32_t aver_nav_agent_get_params(int32_t agent, float* out12);
AVER_NAV_API int32_t aver_nav_agent_set_position(int32_t agent, const float* p3);
AVER_NAV_API int32_t aver_nav_agent_set_velocity(int32_t agent, const float* v3);   /* the velocity the agent wants; marks it for the next step */
AVER_NAV_API int32_t aver_nav_agent_safe_velocity(int32_t agent, float* out3);      /* 1 and consumes the answer when a step computed one */

/* ---- queries ------------------------------------------------------------------------------------------------------- */
/* Runs a path query; returns the number of points (0: none). The result stays readable until the next query.
 * simplifyEpsilonCm > 0 simplifies the path. excluded/included are region ids; an empty included list means every region. */
AVER_NAV_API int32_t aver_nav_path_query(int32_t map, const float* start3, const float* target3, uint32_t layers, int32_t postprocess,
                                         float simplifyEpsilonCm, const int32_t* excluded, int32_t nExcluded, const int32_t* included, int32_t nIncluded);
AVER_NAV_API int32_t aver_nav_path_points(float* out3, int32_t cap);                    /* returns the point count */
AVER_NAV_API int32_t aver_nav_path_info(int32_t* types, int32_t* rids, int64_t* owners, int32_t cap);   /* per point; any of the three may be null */
AVER_NAV_API float   aver_nav_path_length(void);
AVER_NAV_API int32_t aver_nav_path_reached(void);                                       /* the search reached the polygon the target snapped to */
AVER_NAV_API float   aver_nav_path_gap(void);                                           /* distance from the last point to the requested target */
AVER_NAV_API int32_t aver_nav_map_closest(int32_t map, const float* p3, uint32_t layers, float* outPoint3, float* outNormal3, int32_t* outRegion, int64_t* outOwner);
AVER_NAV_API int32_t aver_nav_map_closest_segment(int32_t map, const float* a3, const float* b3, float* outPointOnMesh3, float* outPointOnSegment3);
/* r1..r3 are uniform [0,1) numbers from the caller, so the result is reproducible. */
AVER_NAV_API int32_t aver_nav_map_random_point(int32_t map, uint32_t layers, int32_t uniformly, float r1, float r2, float r3, float* out3);

/* ---- baking (pure; thread-safe) -------------------------------------------------------------------------------------- */
/* tris: indices, three per triangle. obstacles: for each, seven floats (px, py, pz, radius, zMin, height, vertexCount) followed by
 * vertexCount * 3 floats (a polygon in XY). Returns a result handle, 0 on failure (aver_nav_bake_error says why). */
AVER_NAV_API int32_t aver_nav_bake(const float* verts3, int32_t nVerts, const int32_t* tris, int32_t nTriIndices, const float* obstacles, int32_t nObstacles,
                                   const float* bakeParams, int32_t nParams);
AVER_NAV_API const char* aver_nav_bake_error(void);                                     /* of this thread's last failed bake */
AVER_NAV_API int32_t aver_nav_bake_counts(int32_t bake, int32_t* out3);                 /* vertices, polygons, total polygon indices */
AVER_NAV_API int32_t aver_nav_bake_get(int32_t bake, float* verts3, int32_t* polySizes, int32_t* polyIndices);
AVER_NAV_API int32_t aver_nav_bake_stats(int32_t bake, int32_t* out6);                  /* columns, spans, walkable spans, regions, regions removed, rectangles */
AVER_NAV_API int32_t aver_nav_bake_free(int32_t bake);

/* ---- A* point graphs (AStar3D / AStar2D) ---------------------------------------------------------------------------- */
AVER_NAV_API int32_t aver_astar_create(void);
AVER_NAV_API int32_t aver_astar_destroy(int32_t g);
AVER_NAV_API int32_t aver_astar_clear(int32_t g);
AVER_NAV_API int32_t aver_astar_add_point(int32_t g, int64_t id, float x, float y, float z, float weight);
AVER_NAV_API int32_t aver_astar_has_point(int32_t g, int64_t id);
AVER_NAV_API int32_t aver_astar_point_position(int32_t g, int64_t id, float* out3);
AVER_NAV_API float   aver_astar_point_weight(int32_t g, int64_t id);
AVER_NAV_API int32_t aver_astar_set_point_position(int32_t g, int64_t id, float x, float y, float z);
AVER_NAV_API int32_t aver_astar_set_point_weight(int32_t g, int64_t id, float weight);
AVER_NAV_API int32_t aver_astar_remove_point(int32_t g, int64_t id);
AVER_NAV_API int32_t aver_astar_set_disabled(int32_t g, int64_t id, int32_t disabled);
AVER_NAV_API int32_t aver_astar_is_disabled(int32_t g, int64_t id);
AVER_NAV_API int32_t aver_astar_point_count(int32_t g);
AVER_NAV_API int32_t aver_astar_point_ids(int32_t g, int64_t* outIds, int32_t cap);        /* returns the count */
AVER_NAV_API int64_t aver_astar_next_free_id(int32_t g);
AVER_NAV_API int32_t aver_astar_connect(int32_t g, int64_t id, int64_t to, int32_t bidirectional);
AVER_NAV_API int32_t aver_astar_disconnect(int32_t g, int64_t id, int64_t to, int32_t bidirectional);
AVER_NAV_API int32_t aver_astar_are_connected(int32_t g, int64_t id, int64_t to, int32_t bidirectional);
AVER_NAV_API int32_t aver_astar_connections(int32_t g, int64_t id, int64_t* outIds, int32_t cap);   /* outgoing; returns the count */
AVER_NAV_API int64_t aver_astar_closest_point(int32_t g, float x, float y, float z, int32_t includeDisabled);   /* -1 for none */
AVER_NAV_API int32_t aver_astar_closest_in_segment(int32_t g, float x, float y, float z, float* out3);
/* Either callback may be null (the default cost). `user` is handed back to them. */
AVER_NAV_API int32_t aver_astar_set_callbacks(int32_t g, aver_astar_cost_fn compute, aver_astar_cost_fn estimate, void* user);
/* The id path; returns the total count (0: none) and fills at most cap. */
AVER_NAV_API int32_t aver_astar_id_path(int32_t g, int64_t from, int64_t to, int32_t allowPartial, int64_t* outIds, int32_t cap);

/* ---- A* grids (AStarGrid2D) ---------------------------------------------------------------------------------------- */
AVER_NAV_API int32_t aver_agrid_create(void);
AVER_NAV_API int32_t aver_agrid_destroy(int32_t g);
AVER_NAV_API int32_t aver_agrid_set_region(int32_t g, int32_t x, int32_t y, int32_t w, int32_t h);   /* needs update() */
AVER_NAV_API int32_t aver_agrid_region(int32_t g, int32_t* out4);
AVER_NAV_API int32_t aver_agrid_set_cell_size(int32_t g, float x, float y);
AVER_NAV_API int32_t aver_agrid_set_offset(int32_t g, float x, float y);
AVER_NAV_API int32_t aver_agrid_set_diagonal_mode(int32_t g, int32_t mode);
AVER_NAV_API int32_t aver_agrid_diagonal_mode(int32_t g);
AVER_NAV_API int32_t aver_agrid_set_heuristics(int32_t g, int32_t compute, int32_t estimate);
AVER_NAV_API int32_t aver_agrid_heuristics(int32_t g, int32_t* out2);
AVER_NAV_API int32_t aver_agrid_set_jumping(int32_t g, int32_t on);
AVER_NAV_API int32_t aver_agrid_jumping(int32_t g);
AVER_NAV_API int32_t aver_agrid_update(int32_t g);
AVER_NAV_API int32_t aver_agrid_is_dirty(int32_t g);
AVER_NAV_API int32_t aver_agrid_clear(int32_t g);
AVER_NAV_API int32_t aver_agrid_set_solid(int32_t g, int32_t x, int32_t y, int32_t solid);
AVER_NAV_API int32_t aver_agrid_is_solid(int32_t g, int32_t x, int32_t y);
AVER_NAV_API int32_t aver_agrid_fill_solid(int32_t g, int32_t x, int32_t y, int32_t w, int32_t h, int32_t solid);
AVER_NAV_API int32_t aver_agrid_set_weight(int32_t g, int32_t x, int32_t y, float weight);
AVER_NAV_API float   aver_agrid_weight(int32_t g, int32_t x, int32_t y);
AVER_NAV_API int32_t aver_agrid_fill_weight(int32_t g, int32_t x, int32_t y, int32_t w, int32_t h, float weight);
AVER_NAV_API int32_t aver_agrid_set_callbacks(int32_t g, aver_astar_grid_cost_fn compute, aver_astar_grid_cost_fn estimate, void* user);
/* Cells as (x, y) int pairs; returns the total cell count (0: none) and fills at most cap cells. */
AVER_NAV_API int32_t aver_agrid_id_path(int32_t g, int32_t fx, int32_t fy, int32_t tx, int32_t ty, int32_t allowPartial, int32_t* out2, int32_t cap);
/* Positions (x, y) float pairs. */
AVER_NAV_API int32_t aver_agrid_point_path(int32_t g, int32_t fx, int32_t fy, int32_t tx, int32_t ty, int32_t allowPartial, float* out2, int32_t cap);
AVER_NAV_API int32_t aver_agrid_point_position(int32_t g, int32_t x, int32_t y, float* out2);

#ifdef __cplusplus
}
#endif
