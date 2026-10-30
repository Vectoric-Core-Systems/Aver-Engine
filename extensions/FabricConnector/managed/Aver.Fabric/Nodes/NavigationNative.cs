// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke into Aver.Nav.Abi (modules/nav.abi, nav_abi.h): polygon navigation maps, the navmesh baker and the A* graphs. The enums restate the
// header's constants and are checked against it by AbiEnumTest; the declarations are checked against the header's parameter names and types.

using System.Runtime.InteropServices;

namespace Aver.Fabric;

/// <summary>AVER_NAV_POST_*: the path post-processing modes (Godot's PathPostProcessing).</summary>
internal enum NavPost { Funnel = 0, EdgeCentered = 1, None = 2 }

/// <summary>AVER_NAV_KIND_*.</summary>
internal enum NavKind { None = 0, Map = 1, Region = 2, Link = 3, Obstacle = 4, Agent = 5 }

/// <summary>AVER_NAV_POINT_*: what a path point is (Godot's PathSegmentType).</summary>
internal enum NavPoint { Region = 0, Link = 1 }

/// <summary>AVER_NAV_HEURISTIC_* (Godot's AStarGrid2D.Heuristic).</summary>
internal enum NavHeuristic { Euclidean = 0, Manhattan = 1, Octile = 2, Chebyshev = 3 }

/// <summary>AVER_NAV_DIAGONAL_* (Godot's AStarGrid2D.DiagonalMode).</summary>
internal enum NavDiagonal { Always = 0, Never = 1, AtLeastOne = 2, OnlyIfNoObstacle = 3 }

/// <summary>AVER_NAV_BAKE_*: the slots of the parameter array of aver_nav_bake.</summary>
internal enum NavBake
{
    CellSize = 0, CellHeight = 1, AgentHeight = 2, AgentRadius = 3, AgentMaxClimb = 4, AgentMaxSlope = 5, RegionMinSize = 6, RegionMergeSize = 7,
    EdgeMaxLength = 8, EdgeMaxError = 9, VertsPerPoly = 10, DetailDistance = 11, DetailMaxError = 12, FilterLowHanging = 13, FilterLedge = 14,
    FilterLowHeight = 15, BorderSize = 16, UseBounds = 17, BoundsMin = 18, BoundsMax = 21, ParamCount = 24,
}

/// <summary>AVER_NAV_AGENT_*: the slots of the parameter array of aver_nav_agent_set_params.</summary>
internal enum NavAgent
{
    Avoidance = 0, Paused = 1, Radius = 2, Height = 3, MaxSpeed = 4, NeighborDist = 5, MaxNeighbors = 6, HorizonAgents = 7, HorizonObstacles = 8,
    Layers = 9, Mask = 10, Priority = 11, ParamCount = 12,
}

/// <summary>The exports of Aver.Nav.Abi. The library is looked for beside the executable (NativeResolver); a host without it makes
/// <see cref="Available"/> false and the navigation classes quietly do nothing.</summary>
internal static class NavNative
{
    private const string Lib = "Aver.Nav.Abi";

    private static int s_state; // 0 unknown, 1 loaded, 2 missing

    internal static bool Available
    {
        get
        {
            if (s_state == 0)
            {
                try { s_state = aver_nav_abi_version() >= 1 ? 1 : 2; }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { s_state = 2; }
            }
            return s_state == 1;
        }
    }

    /// <summary>Forgets a failed probe (a test that has just copied the library beside itself).</summary>
    internal static void Reprobe() { if (s_state == 2) s_state = 0; }

    internal static string Str(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_abi_version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_nav_reset();

    // ---- maps ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_create();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_destroy(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_exists(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_set_active(int map, int active);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_is_active(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_set_params(int map, float cellSizeCm, float cellHeightCm, float edgeMarginCm, float linkRadiusCm, int useEdgeConnections);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_params(int map, float[] out5);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_update(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_iteration(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_list(int[]? outIds, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_objects(int map, int kind, int[]? outIds, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_polygon_count(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_step(int map, float dtSeconds);

    // ---- objects ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_create(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_link_create(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_obstacle_create(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_agent_create(int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_destroy(int id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_kind(int id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_set_map(int id, int map);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_get_map(int id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_set_owner(int id, long owner);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_nav_get_owner(int id);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_set_enabled(int region, int enabled);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_set_layers(int region, uint layers);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_set_costs(int region, float enterCost, float travelCost);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_set_edge_connections(int region, int useEdgeConnections);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_edge_connections(int region);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_get(int region, float[] out4);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_set_mesh(int region, float[]? verts3, int nVerts, int[]? polyIndices, int[]? polySizes, int nPolys);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_polygon_count(int region);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_closest(int region, float[] p3, float[]? outPoint3, float[]? outNormal3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_region_connections(int region, float[]? out6, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_link_set(int link, int enabled, int bidirectional, uint layers, float enterCost, float travelCost);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_link_set_ends(int link, float[] start3, float[] end3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_link_get(int link, float[] out11);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_link_connected(int link);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_obstacle_set(int obstacle, int avoidance, int paused, uint layers, float radiusCm, float heightCm);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_obstacle_set_motion(int obstacle, float[] position3, float[] velocity3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_obstacle_set_vertices(int obstacle, float[]? verts3, int nVerts);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_obstacle_get(int obstacle, float[] out6);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_agent_set_params(int agent, float[] params12);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_agent_get_params(int agent, float[] out12);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_agent_set_position(int agent, float[] p3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_agent_set_velocity(int agent, float[] v3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_agent_safe_velocity(int agent, float[] out3);

    // ---- queries ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_path_query(int map, float[] start3, float[] target3, uint layers, int postprocess,
        float simplifyEpsilonCm, int[]? excluded, int nExcluded, int[]? included, int nIncluded);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_path_points(float[]? out3, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_path_info(int[]? types, int[]? rids, long[]? owners, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern float aver_nav_path_length();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_path_reached();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern float aver_nav_path_gap();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_closest(int map, float[] p3, uint layers, float[]? outPoint3, float[]? outNormal3, out int outRegion, out long outOwner);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_closest_segment(int map, float[] a3, float[] b3, float[]? outPointOnMesh3, float[]? outPointOnSegment3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_map_random_point(int map, uint layers, int uniformly, float r1, float r2, float r3, float[] out3);

    // ---- baking ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_bake(float[] verts3, int nVerts, int[] tris, int nTriIndices, float[]? obstacles, int nObstacles, float[] bakeParams, int nParams);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr aver_nav_bake_error();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_bake_counts(int bake, int[] out3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_bake_get(int bake, float[] verts3, int[] polySizes, int[] polyIndices);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_bake_stats(int bake, int[] out6);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_nav_bake_free(int bake);

    // ---- A* point graphs ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_create();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_destroy(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_clear(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_add_point(int g, long id, float x, float y, float z, float weight);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_has_point(int g, long id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_point_position(int g, long id, float[] out3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern float aver_astar_point_weight(int g, long id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_set_point_position(int g, long id, float x, float y, float z);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_set_point_weight(int g, long id, float weight);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_remove_point(int g, long id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_set_disabled(int g, long id, int disabled);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_is_disabled(int g, long id);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_point_count(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_point_ids(int g, long[]? outIds, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_astar_next_free_id(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_connect(int g, long id, long to, int bidirectional);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_disconnect(int g, long id, long to, int bidirectional);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_are_connected(int g, long id, long to, int bidirectional);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_connections(int g, long id, long[]? outIds, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_astar_closest_point(int g, float x, float y, float z, int includeDisabled);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_closest_in_segment(int g, float x, float y, float z, float[] out3);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_set_callbacks(int g, IntPtr compute, IntPtr estimate, IntPtr user);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_astar_id_path(int g, long from, long to, int allowPartial, long[]? outIds, int cap);

    // ---- A* grids ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_create();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_destroy(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_region(int g, int x, int y, int w, int h);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_region(int g, int[] out4);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_cell_size(int g, float x, float y);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_offset(int g, float x, float y);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_diagonal_mode(int g, int mode);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_diagonal_mode(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_heuristics(int g, int compute, int estimate);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_heuristics(int g, int[] out2);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_jumping(int g, int on);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_jumping(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_update(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_is_dirty(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_clear(int g);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_solid(int g, int x, int y, int solid);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_is_solid(int g, int x, int y);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_fill_solid(int g, int x, int y, int w, int h, int solid);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_weight(int g, int x, int y, float weight);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern float aver_agrid_weight(int g, int x, int y);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_fill_weight(int g, int x, int y, int w, int h, float weight);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_set_callbacks(int g, IntPtr compute, IntPtr estimate, IntPtr user);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_id_path(int g, int fx, int fy, int tx, int ty, int allowPartial, int[]? out2, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_point_path(int g, int fx, int fy, int tx, int ty, int allowPartial, float[]? out2, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_agrid_point_position(int g, int x, int y, float[] out2);
}

/// <summary>Godot-space to engine-space helpers for the navigation seam (FabricSpace is the one map).</summary>
internal static class NavSpace
{
    internal static float[] ToEngine(Vector3 p)
    {
        Aver.Scene.Vec3 v = FabricSpace.ToAverPosition(p);
        return new[] { v.X, v.Y, v.Z };
    }

    internal static void ToEngineInto(Vector3 p, float[] dst, int at)
    {
        Aver.Scene.Vec3 v = FabricSpace.ToAverPosition(p);
        dst[at] = v.X; dst[at + 1] = v.Y; dst[at + 2] = v.Z;
    }

    internal static Vector3 FromEngine(float[] a, int at = 0) => FabricSpace.FromAverPosition(new Aver.Scene.Vec3(a[at], a[at + 1], a[at + 2]));

    internal static Vector3 VelocityFromEngine(float[] a) => FabricSpace.FromAverPosition(new Aver.Scene.Vec3(a[0], a[1], a[2]));
}
