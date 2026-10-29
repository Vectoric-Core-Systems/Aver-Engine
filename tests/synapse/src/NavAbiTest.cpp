// The plain-C seam onto polygon navigation (nav_abi.h): maps, regions and links, path queries, the baker, avoidance, the A* graphs with
// scripted costs, and the neutral answers for stale handles. This is the surface Aver.Fabric P/Invokes.
#include "aver/core/Log.hpp"
#include "aver/nav/nav_abi.h"

#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) <= eps; }

static int32_t makeRect(int32_t map, float x0, float y0, float x1, float y1, float z, uint32_t layers = 1) {
    const int32_t r = aver_nav_region_create(map);
    const float v[12] = {x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z};
    const int32_t idx[4] = {0, 1, 2, 3}, sizes[1] = {4};
    aver_nav_region_set_mesh(r, v, 4, idx, sizes, 1);
    aver_nav_region_set_layers(r, layers);
    return r;
}

static float g_costArg = 0;
static float computeCb(void* user, int64_t from, int64_t to) {
    ++*static_cast<int*>(user);
    return (from == 0 && to == 2) ? 100.0f : 1.0f;
}
static float estimateCb(void*, int64_t, int64_t) { return 0.0f; }
static float gridCost(void*, int32_t, int32_t, int32_t tx, int32_t ty) { return (tx == 1 && ty == 0) ? 100.0f : 1.0f; }

int main() {
    AVER_INFO("NavAbiTest");
    check(aver_nav_abi_version() == AVER_NAV_ABI_VERSION, "version");

    AVER_INFO("maps, regions, links and queries");
    {
        aver_nav_reset();
        const int32_t map = aver_nav_map_create();
        check(map > 0 && aver_nav_kind(map) == AVER_NAV_KIND_MAP && aver_nav_map_exists(map) && aver_nav_map_is_active(map), "a new map is active");
        float prm[5];
        check(aver_nav_map_params(map, prm) && near(prm[0], 25) && near(prm[2], 25) && near(prm[3], 100) && prm[4] == 1.0f, "Godot's map defaults (25 cm cells, 25 cm margin, 1 m link radius)");
        check(aver_nav_map_set_params(map, 50, 50, 30, 150, 1) && aver_nav_map_params(map, prm) && near(prm[3], 150), "set_params");
        check(!aver_nav_map_set_params(map, 0, 50, 30, 150, 1), "a zero cell size is refused");
        aver_nav_map_set_params(map, 25, 25, 25, 100, 1);

        const int32_t a = makeRect(map, 0, 0, 1000, 1000, 0);
        const int32_t b = makeRect(map, 1000, 0, 2000, 1000, 0, 2);
        check(aver_nav_kind(a) == AVER_NAV_KIND_REGION && aver_nav_get_map(a) == map, "region kind and map");
        check(aver_nav_region_polygon_count(a) == 1, "one polygon");
        aver_nav_set_owner(b, 4242);
        check(aver_nav_get_owner(b) == 4242, "owner");
        const int32_t it0 = aver_nav_map_iteration(map);
        check(aver_nav_map_update(map) == 1 && aver_nav_map_iteration(map) == it0 + 1 && aver_nav_map_update(map) == 0, "force update rebuilds once");
        check(aver_nav_map_polygon_count(map) == 2, "two polygons");

        const float s[3] = {500, 500, 0}, t[3] = {1500, 500, 0};
        check(aver_nav_path_query(map, s, t, 1, 0, 0, nullptr, 0, nullptr, 0) > 0 && aver_nav_path_gap() > 400.0f, "layer 1 stops at the edge of the layer-2 region");
        const int32_t n = aver_nav_path_query(map, s, t, 3, 0, 0, nullptr, 0, nullptr, 0);
        float pts[12];
        int32_t types[4], rids[4];
        int64_t owners[4];
        check(n == 2 && aver_nav_path_points(pts, 4) == 2 && near(pts[3], 1500), "layers 1|2 reach it");
        check(aver_nav_path_info(types, rids, owners, 4) == 2 && owners[1] == 4242 && rids[1] == b && types[1] == AVER_NAV_POINT_REGION, "per-point region, owner and type");
        check(aver_nav_path_reached() == 1 && near(aver_nav_path_length(), 1000.0f), "reached; length");
        check(aver_nav_path_query(map, s, t, 3, 0, 0, &b, 1, nullptr, 0) > 0 && aver_nav_path_gap() > 400.0f, "an excluded region: the target snaps back into the other one");
        aver_nav_region_set_enabled(b, 0);
        check(aver_nav_path_query(map, s, t, 3, 0, 0, nullptr, 0, nullptr, 0) > 0 && aver_nav_path_gap() > 400.0f, "a disabled region");
        aver_nav_region_set_enabled(b, 1);
        aver_nav_region_set_costs(b, 0.0f, 5.0f);
        float rg[4];
        check(aver_nav_region_get(b, rg) && rg[0] == 1.0f && rg[1] == 2.0f && near(rg[3], 5.0f), "region getters");

        float cp[3], cn[3];
        int32_t cr = 0;
        int64_t co = 0;
        const float far_[3] = {3000, 500, 40};
        check(aver_nav_map_closest(map, far_, 0xFFFFFFFFu, cp, cn, &cr, &co) && near(cp[0], 2000) && cr == b && co == 4242 && near(cn[2], 1.0f), "closest point, region and owner");
        float onMesh[3], onSeg[3];
        const float sa[3] = {1500, -500, 100}, sb[3] = {1500, -100, 100};
        check(aver_nav_map_closest_segment(map, sa, sb, onMesh, onSeg) && near(onMesh[1], 0, 1) && near(onSeg[1], -100, 1), "closest to a segment");
        float rp[3];
        check(aver_nav_map_random_point(map, 1, 1, 0.5f, 0.2f, 0.2f, rp) && rp[0] >= 0 && rp[0] <= 1000, "a random point on the layer");
        check(aver_nav_map_random_point(map, 4, 1, 0.5f, 0.2f, 0.2f, rp) == 0, "none on an empty layer");

        const int32_t link = aver_nav_link_create(map);
        const float ls[3] = {900, 500, 0}, le[3] = {1900, 500, 0};
        check(aver_nav_link_set_ends(link, ls, le) && aver_nav_link_set(link, 1, 0, 1, 0.0f, 1.0f), "link setters");
        aver_nav_map_update(map);
        check(aver_nav_link_connected(link) == 2, "attached at both ends");
        float lk[11];
        check(aver_nav_link_get(link, lk) && lk[0] == 1 && lk[1] == 0 && near(lk[5], 900) && near(lk[8], 1900), "link getters");
        int32_t objs[8];
        check(aver_nav_map_objects(map, AVER_NAV_KIND_REGION, objs, 8) == 2 && aver_nav_map_objects(map, AVER_NAV_KIND_LINK, objs, 8) == 1, "objects of a map");
        check(aver_nav_destroy(link) && aver_nav_kind(link) == AVER_NAV_KIND_NONE && !aver_nav_destroy(link), "destroy; a stale link");

        // post-processing and simplify through the seam
        aver_nav_region_set_layers(b, 1);
        const float far2[3] = {1900, 900, 0};
        check(aver_nav_path_query(map, s, far2, 1, AVER_NAV_POST_EDGE_CENTERED, 0, nullptr, 0, nullptr, 0) == 3, "edge centred: start, portal, end");
        check(aver_nav_path_query(map, s, far2, 1, AVER_NAV_POST_FUNNEL, 100.0f, nullptr, 0, nullptr, 0) == 2, "simplify");

        check(aver_nav_map_destroy(map) && aver_nav_get_map(a) == 0 && !aver_nav_map_exists(map), "destroying a map detaches its regions");
        check(aver_nav_path_query(map, s, t, 1, 0, 0, nullptr, 0, nullptr, 0) == 0, "stale map: no path");
        check(!aver_nav_region_set_enabled(9999, 1) && aver_nav_region_polygon_count(9999) == 0 && aver_nav_get_owner(9999) == 0, "stale handles answer neutrally");
        const int32_t bad[3] = {0, 1, 9};
        const int32_t sz[1] = {3};
        const float v3[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        check(!aver_nav_region_set_mesh(a, v3, 3, bad, sz, 1), "an index out of range is refused");
    }

    AVER_INFO("baking through the seam");
    {
        aver_nav_reset();
        const float v[12] = {0, 0, 0, 1000, 0, 0, 1000, 1000, 0, 0, 1000, 0};
        const int32_t tris[6] = {0, 1, 2, 0, 2, 3};
        float p[AVER_NAV_BAKE_PARAM_COUNT] = {};
        p[AVER_NAV_BAKE_CELL_SIZE] = 25; p[AVER_NAV_BAKE_CELL_HEIGHT] = 25; p[AVER_NAV_BAKE_AGENT_HEIGHT] = 150; p[AVER_NAV_BAKE_AGENT_RADIUS] = 50;
        p[AVER_NAV_BAKE_AGENT_MAX_CLIMB] = 25; p[AVER_NAV_BAKE_AGENT_MAX_SLOPE] = 45; p[AVER_NAV_BAKE_REGION_MIN_SIZE] = 2; p[AVER_NAV_BAKE_VERTS_PER_POLY] = 6;
        p[AVER_NAV_BAKE_DETAIL_MAX_ERROR] = 25;
        const int32_t h = aver_nav_bake(v, 4, tris, 6, nullptr, 0, p, AVER_NAV_BAKE_PARAM_COUNT);
        int32_t counts[3], stats[6];
        check(h > 0 && aver_nav_bake_counts(h, counts) && counts[0] == 4 && counts[1] == 1 && counts[2] == 4, "one rectangle comes out");
        std::vector<float> bv(static_cast<size_t>(counts[0]) * 3);
        std::vector<int32_t> bs(static_cast<size_t>(counts[1]));
        std::vector<int32_t> bi(static_cast<size_t>(counts[2]));
        check(aver_nav_bake_get(h, bv.data(), bs.data(), bi.data()), "the mesh is readable");
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < counts[0]; ++i) { lo = std::fmin(lo, bv[size_t(i) * 3]); hi = std::fmax(hi, bv[size_t(i) * 3]); }
        check(near(lo, 50) && near(hi, 950), "eroded by the radius");
        check(aver_nav_bake_stats(h, stats) && stats[3] == 1 && stats[5] == 1, "stats: one region, one rectangle");

        // The result goes straight into a region.
        const int32_t map = aver_nav_map_create();
        const int32_t r = aver_nav_region_create(map);
        check(aver_nav_region_set_mesh(r, bv.data(), counts[0], bi.data(), bs.data(), counts[1]), "load the baked mesh");
        const float s[3] = {100, 100, 0}, t[3] = {900, 900, 0};
        check(aver_nav_path_query(map, s, t, 1, 0, 0, nullptr, 0, nullptr, 0) == 2 && aver_nav_path_reached() == 1, "and route over it");
        check(aver_nav_bake_free(h) && !aver_nav_bake_counts(h, counts) && !aver_nav_bake_free(h), "free; a freed bake is stale");

        check(aver_nav_bake(v, 4, tris, 6, nullptr, 0, p, 3) == 0 && std::strlen(aver_nav_bake_error()) > 0, "too few parameters is refused with a reason");
        const int32_t badTris[3] = {0, 1, 99};
        check(aver_nav_bake(v, 4, badTris, 3, nullptr, 0, p, AVER_NAV_BAKE_PARAM_COUNT) == 0, "a triangle index out of range");

        // An obstacle in the flat buffer: seven floats then the polygon.
        const float obs[7] = {500, 500, 0, 150, -10, 200, 0};
        const int32_t h2 = aver_nav_bake(v, 4, tris, 6, obs, 1, p, AVER_NAV_BAKE_PARAM_COUNT);
        const int32_t m2 = aver_nav_map_create();
        const int32_t r2 = aver_nav_region_create(m2);
        aver_nav_bake_counts(h2, counts);
        bv.assign(size_t(counts[0]) * 3, 0);
        bs.assign(size_t(counts[1]), 0);
        bi.assign(size_t(counts[2]), 0);
        aver_nav_bake_get(h2, bv.data(), bs.data(), bi.data());
        aver_nav_region_set_mesh(r2, bv.data(), counts[0], bi.data(), bs.data(), counts[1]);
        float cp[3];
        const float centre[3] = {500, 500, 0};
        check(aver_nav_map_closest(m2, centre, 0xFFFFFFFFu, cp, nullptr, nullptr, nullptr) && std::hypot(cp[0] - 500, cp[1] - 500) > 150.0f, "the circle obstacle carves a hole");

        // Bakes run on another thread (the managed side does).
        int32_t h3 = 0;
        std::thread th([&] { h3 = aver_nav_bake(v, 4, tris, 6, nullptr, 0, p, AVER_NAV_BAKE_PARAM_COUNT); });
        th.join();
        check(h3 > 0 && aver_nav_bake_counts(h3, counts) && counts[1] == 1, "a bake from a worker thread");
    }

    AVER_INFO("avoidance through the seam");
    {
        aver_nav_reset();
        const int32_t map = aver_nav_map_create();
        const int32_t a = aver_nav_agent_create(map), b = aver_nav_agent_create(map);
        float prm[AVER_NAV_AGENT_PARAM_COUNT] = {};
        prm[AVER_NAV_AGENT_AVOIDANCE] = 1; prm[AVER_NAV_AGENT_RADIUS] = 50; prm[AVER_NAV_AGENT_HEIGHT] = 180; prm[AVER_NAV_AGENT_MAX_SPEED] = 300;
        prm[AVER_NAV_AGENT_NEIGHBOR_DIST] = 5000; prm[AVER_NAV_AGENT_MAX_NEIGHBORS] = 10; prm[AVER_NAV_AGENT_HORIZON_AGENTS] = 2; prm[AVER_NAV_AGENT_HORIZON_OBSTACLES] = 2;
        prm[AVER_NAV_AGENT_LAYERS] = 1; prm[AVER_NAV_AGENT_MASK] = 1; prm[AVER_NAV_AGENT_PRIORITY] = 1;
        check(aver_nav_agent_set_params(a, prm) && aver_nav_agent_set_params(b, prm), "agent parameters");
        float back[AVER_NAV_AGENT_PARAM_COUNT];
        check(aver_nav_agent_get_params(a, back) && back[AVER_NAV_AGENT_RADIUS] == 50 && back[AVER_NAV_AGENT_MASK] == 1, "read back");
        const float pa[3] = {0, 0, 0}, pb[3] = {400, 0, 0}, va[3] = {300, 0, 0}, vb[3] = {-300, 0, 0};
        aver_nav_agent_set_position(a, pa); aver_nav_agent_set_position(b, pb);
        float out[3];
        check(!aver_nav_agent_safe_velocity(a, out), "no answer before a step");
        aver_nav_agent_set_velocity(a, va); aver_nav_agent_set_velocity(b, vb);
        aver_nav_map_step(map, 1.0f / 30.0f);
        check(aver_nav_agent_safe_velocity(a, out) && std::fabs(out[1]) > 1.0f, "head-on: a steers sideways");
        check(!aver_nav_agent_safe_velocity(a, out), "the answer is consumed");
        aver_nav_destroy(b);
        const int32_t o = aver_nav_obstacle_create(map);
        float verts[12] = {400, -1000, 0, 400, 1000, 0, 500, 1000, 0, 500, -1000, 0};
        check(aver_nav_obstacle_set_vertices(o, verts, 4) && aver_nav_obstacle_set(o, 1, 0, 1, 0.0f, 300.0f), "obstacle");
        float og[6];
        check(aver_nav_obstacle_get(o, og) && og[0] == 1 && og[5] == 4, "obstacle getters");
        aver_nav_agent_set_velocity(a, va);
        const float pa2[3] = {300, 0, 0};
        aver_nav_agent_set_position(a, pa2);
        aver_nav_obstacle_set(o, 1, 1, 1, 0.0f, 300.0f);   // paused
        aver_nav_map_step(map, 1.0f / 30.0f);
        aver_nav_agent_safe_velocity(a, out);
        check(out[0] > 250.0f, "a paused obstacle does nothing");
        aver_nav_obstacle_set(o, 1, 0, 1, 0.0f, 300.0f);
        aver_nav_agent_set_velocity(a, va);
        aver_nav_map_step(map, 1.0f / 30.0f);
        aver_nav_agent_safe_velocity(a, out);
        check(out[0] < 150.0f, "a wall ahead slows the agent");
    }

    AVER_INFO("A* graphs through the seam");
    {
        aver_nav_reset();
        const int32_t g = aver_astar_create();
        aver_astar_add_point(g, 0, 0, 0, 0, 1); aver_astar_add_point(g, 1, 1, 1, 0, 1); aver_astar_add_point(g, 2, 2, 0, 0, 1);
        aver_astar_connect(g, 0, 1, 1); aver_astar_connect(g, 1, 2, 1); aver_astar_connect(g, 0, 2, 1);
        int64_t path[8];
        check(aver_astar_id_path(g, 0, 2, 0, path, 8) == 2 && path[0] == 0 && path[1] == 2, "the direct edge");
        int calls = 0;
        check(aver_astar_set_callbacks(g, computeCb, estimateCb, &calls), "callbacks");
        check(aver_astar_id_path(g, 0, 2, 0, path, 8) == 3 && path[1] == 1 && calls > 0, "_compute_cost makes the direct edge dear");
        aver_astar_set_callbacks(g, nullptr, nullptr, nullptr);
        check(aver_astar_id_path(g, 0, 2, 0, path, 8) == 2, "cleared callbacks: the default again");
        check(aver_astar_point_count(g) == 3 && aver_astar_next_free_id(g) == 3 && aver_astar_has_point(g, 1), "counts and ids");
        int64_t ids[8];
        check(aver_astar_point_ids(g, ids, 8) == 3 && ids[2] == 2, "point ids");
        check(aver_astar_connections(g, 0, ids, 8) == 2, "connections");
        float pos[3];
        check(aver_astar_point_position(g, 1, pos) && near(pos[0], 1) && near(pos[1], 1), "position");
        check(aver_astar_closest_point(g, 1.9f, 0.2f, 0, 0) == 2, "closest point");
        aver_astar_set_disabled(g, 2, 1);
        check(aver_astar_is_disabled(g, 2) && aver_astar_id_path(g, 0, 2, 0, path, 8) == 0, "a disabled goal");
        check(aver_astar_remove_point(g, 2) && !aver_astar_has_point(g, 2) && !aver_astar_remove_point(g, 2), "remove");
        check(aver_astar_destroy(g) && !aver_astar_destroy(g) && aver_astar_point_count(g) == 0, "destroy; stale");
        check(!aver_astar_add_point(aver_astar_create(), -1, 0, 0, 0, 1), "a negative id is refused");
    }
    {
        const int32_t g = aver_agrid_create();
        check(aver_agrid_set_region(g, 0, 0, 5, 5) && aver_agrid_is_dirty(g), "a region marks the grid dirty");
        check(!aver_agrid_set_solid(g, 1, 1, 1), "solid before update is refused");
        aver_agrid_update(g);
        check(!aver_agrid_is_dirty(g), "update");
        aver_agrid_set_diagonal_mode(g, AVER_NAV_DIAGONAL_NEVER);
        aver_agrid_set_heuristics(g, AVER_NAV_HEURISTIC_MANHATTAN, AVER_NAV_HEURISTIC_MANHATTAN);
        int32_t hs[2];
        check(aver_agrid_heuristics(g, hs) && hs[0] == 1 && aver_agrid_diagonal_mode(g) == 1, "settings read back");
        int32_t cells[40];
        check(aver_agrid_id_path(g, 0, 0, 4, 4, 0, cells, 20) == 9 && cells[16] == 4 && cells[17] == 4, "NEVER: nine cells");
        aver_agrid_set_diagonal_mode(g, AVER_NAV_DIAGONAL_ALWAYS);
        aver_agrid_set_heuristics(g, AVER_NAV_HEURISTIC_OCTILE, AVER_NAV_HEURISTIC_OCTILE);
        aver_agrid_set_jumping(g, 1);
        check(aver_agrid_jumping(g) && aver_agrid_id_path(g, 0, 0, 4, 4, 0, cells, 20) == 2, "jumping: start and goal");
        aver_agrid_set_jumping(g, 0);
        aver_agrid_set_solid(g, 2, 2, 1);
        check(aver_agrid_is_solid(g, 2, 2), "solid");
        aver_agrid_fill_solid(g, 0, 4, 5, 1, 1);
        check(aver_agrid_id_path(g, 0, 0, 4, 4, 0, cells, 20) == 0 && aver_agrid_id_path(g, 0, 0, 4, 4, 1, cells, 20) > 0, "walled off; partial gives the closest");
        aver_agrid_fill_solid(g, 0, 4, 5, 1, 0);
        aver_agrid_set_weight(g, 1, 1, 7.0f);
        check(near(aver_agrid_weight(g, 1, 1), 7.0f), "weight");
        aver_agrid_set_cell_size(g, 2, 3);
        aver_agrid_set_offset(g, 10, 20);
        aver_agrid_update(g);
        float pp[2];
        check(aver_agrid_point_position(g, 1, 2, pp) && near(pp[0], 12) && near(pp[1], 26), "positions");
        float path[20];
        check(aver_agrid_point_path(g, 0, 0, 2, 0, 0, path, 10) == 3 && near(path[4], 14), "point path");
        check(aver_agrid_set_callbacks(g, gridCost, nullptr, nullptr), "grid callbacks");
        aver_agrid_set_diagonal_mode(g, AVER_NAV_DIAGONAL_NEVER);
        aver_agrid_set_heuristics(g, AVER_NAV_HEURISTIC_MANHATTAN, AVER_NAV_HEURISTIC_MANHATTAN);
        check(aver_agrid_id_path(g, 0, 0, 2, 0, 0, cells, 20) == 5 && cells[2] == 0 && cells[3] == 1, "_compute_cost steers round the dear cell");
        check(aver_agrid_destroy(g) && aver_agrid_id_path(g, 0, 0, 2, 0, 0, cells, 20) == 0, "destroy; stale");
    }
    (void)g_costArg;

    AVER_INFO(g_failures == 0 ? "NavAbiTest passed" : "NavAbiTest FAILED");
    return g_failures == 0 ? 0 : 1;
}
