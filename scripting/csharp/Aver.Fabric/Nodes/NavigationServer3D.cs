// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// NavigationServer3D (maps, regions, links, agents and obstacles as RIDs; path queries; baking) and the query parameter / result objects, over the engine's
// polygon navigation (Aver.Nav.Abi). Godot's space and units (metres, +Y up); the engine's centimetres and +Z up appear only inside NavSpace.
// docs/plans/fabric/G5_PARITY.md, N1.

namespace Aver.Fabric;

/// <summary>Which pathfinding algorithm a query uses (Godot's NavigationPathQueryParameters3D.PathfindingAlgorithm).</summary>
public enum PathfindingAlgorithm { Astar = 0 }

/// <summary>How a path found over the polygon corridor is post-processed (Godot's PathPostProcessing).</summary>
public enum PathPostProcessing { CorridorFunnel = 0, Edgecentered = 1, None = 2 }

/// <summary>What a query result carries per path point (Godot's PathMetadataFlags).</summary>
[Flags]
public enum PathMetadataFlags { IncludeNone = 0, IncludeTypes = 1, IncludeRids = 2, IncludeOriginalIds = 4, IncludeOwners = 8, IncludeAll = 15 }

/// <summary>What a path point is (Godot's PathSegmentType).</summary>
public enum PathSegmentType { Region = 0, Link = 1 }

/// <summary>A path query (Godot's NavigationPathQueryParameters3D). Start and target are global positions in metres.</summary>
public class NavigationPathQueryParameters3D : RefCounted
{
    public Rid Map { get; set; }
    public Vector3 StartPosition { get; set; }
    public Vector3 TargetPosition { get; set; }
    public uint NavigationLayers { get; set; } = 1;
    public PathfindingAlgorithm PathfindingAlgorithm { get; set; } = PathfindingAlgorithm.Astar;
    public PathPostProcessing PathPostprocessing { get; set; } = PathPostProcessing.CorridorFunnel;
    public PathMetadataFlags MetadataFlags { get; set; } = PathMetadataFlags.IncludeAll;
    public bool SimplifyPath { get; set; }
    /// <summary>The simplification tolerance in metres.</summary>
    public float SimplifyEpsilon { get; set; }
    public Collections.Array<Rid> ExcludedRegions { get; set; } = new();
    public Collections.Array<Rid> IncludedRegions { get; set; } = new();
}

/// <summary>The answer to a path query (Godot's NavigationPathQueryResult3D). The per-point arrays are as long as <see cref="Path"/> when the query's
/// metadata flags asked for them, empty otherwise.</summary>
public class NavigationPathQueryResult3D : RefCounted
{
    public Vector3[] Path { get; set; } = System.Array.Empty<Vector3>();
    public int[] PathTypes { get; set; } = System.Array.Empty<int>();
    public Collections.Array<Rid> PathRids { get; set; } = new();
    public long[] PathOwnerIds { get; set; } = System.Array.Empty<long>();
    public float PathLength { get; set; }

    /// <summary>True when the last path point is the polygon the target snapped to (a path to somewhere short of the target is partial). Aver extension.</summary>
    public bool ReachedTarget { get; internal set; }

    public void Reset()
    {
        Path = System.Array.Empty<Vector3>();
        PathTypes = System.Array.Empty<int>();
        PathRids = new Collections.Array<Rid>();
        PathOwnerIds = System.Array.Empty<long>();
        PathLength = 0f;
        ReachedTarget = false;
    }
}

/// <summary>The navigation server: maps, regions, links, avoidance agents and obstacles, path queries and navigation mesh baking. Handles are
/// <see cref="Rid"/>s. A query sees every change made before it (Godot applies changes at the next physics sync; <see cref="MapForceUpdate"/> exists and
/// does the rebuild now). Without Aver.Nav.Abi beside the executable every call is a quiet no-op that answers empty.</summary>
public static class NavigationServer3D
{
    private static Rid R(int id) => new((ulong)(uint)id);
    private static int I(Rid r) => (int)(uint)r.Id;
    private static bool Ok => NavNative.Available;

    private static int s_defaultMap;
    private static bool s_active = true;
    private static readonly Dictionary<int, Callable> s_avoidanceCallbacks = new();

    /// <summary>Emitted when a map's polygons were rebuilt after a change (Godot's map_changed).</summary>
    public static event Action<Rid>? MapChanged;

    // The nodes that follow a transform and the agents that step avoidance (registered by the nodes themselves).
    internal static readonly List<NavigationRegion3D> Regions = new();
    internal static readonly List<NavigationLink3D> Links = new();
    internal static readonly List<NavigationObstacle3D> Obstacles = new();
    internal static readonly List<NavigationAgent3D> Agents = new();
    private static long s_lastStepFrame = -1;
    private static readonly System.Collections.Concurrent.ConcurrentQueue<Action> s_completions = new();
    private static readonly HashSet<NavigationMesh> s_baking = new();

    /// <summary>Forgets everything (the managed registrations and the engine's maps). For tests and for a level change that drops the world.</summary>
    internal static void ResetForTests()
    {
        if (NavNative.Available) NavNative.aver_nav_reset();
        s_defaultMap = 0;
        s_active = true;
        s_avoidanceCallbacks.Clear();
        s_obstacleMotion.Clear();
        Regions.Clear(); Links.Clear(); Obstacles.Clear(); Agents.Clear();
        s_lastStepFrame = -1;
        while (s_completions.TryDequeue(out _)) { }
        s_baking.Clear();
        MapChanged = null;
    }

    /// <summary>Turns navigation processing on or off (Godot's set_active). Off stops the avoidance step; queries still answer.</summary>
    public static void SetActive(bool active) => s_active = active;

    // ---- maps ----

    /// <summary>The map the 3D world uses (<see cref="World3D.NavigationMap"/>): made on first use.</summary>
    public static Rid GetDefaultMapRid()
    {
        if (!Ok) return default;
        if (s_defaultMap == 0 || NavNative.aver_nav_map_exists(s_defaultMap) == 0) s_defaultMap = NavNative.aver_nav_map_create();
        return R(s_defaultMap);
    }

    public static Rid MapCreate() => Ok ? R(NavNative.aver_nav_map_create()) : default;

    public static void MapSetActive(Rid map, bool active) { if (Ok) NavNative.aver_nav_map_set_active(I(map), active ? 1 : 0); }
    public static bool MapIsActive(Rid map) => Ok && NavNative.aver_nav_map_is_active(I(map)) != 0;

    /// <summary>Godot keeps the map's "up" for its 2D-in-3D avoidance; here it is always +Y.</summary>
    public static void MapSetUp(Rid map, Vector3 up) { }
    public static Vector3 MapGetUp(Rid map) => Vector3.Up;

    private static float[] Params(Rid map)
    {
        var p = new float[5] { 25, 25, 25, 100, 1 };
        if (Ok) NavNative.aver_nav_map_params(I(map), p);
        return p;
    }

    private static void SetParam(Rid map, int index, float valueCm)
    {
        if (!Ok) return;
        float[] p = Params(map);
        p[index] = valueCm;
        NavNative.aver_nav_map_set_params(I(map), p[0], p[1], p[2], p[3], (int)p[4]);
    }

    public static void MapSetCellSize(Rid map, float cellSize) => SetParam(map, 0, cellSize * FabricSpace.CmPerMetre);
    public static float MapGetCellSize(Rid map) => Params(map)[0] / FabricSpace.CmPerMetre;
    public static void MapSetCellHeight(Rid map, float cellHeight) => SetParam(map, 1, cellHeight * FabricSpace.CmPerMetre);
    public static float MapGetCellHeight(Rid map) => Params(map)[1] / FabricSpace.CmPerMetre;
    public static void MapSetEdgeConnectionMargin(Rid map, float margin) => SetParam(map, 2, margin * FabricSpace.CmPerMetre);
    public static float MapGetEdgeConnectionMargin(Rid map) => Params(map)[2] / FabricSpace.CmPerMetre;
    public static void MapSetLinkConnectionRadius(Rid map, float radius) => SetParam(map, 3, radius * FabricSpace.CmPerMetre);
    public static float MapGetLinkConnectionRadius(Rid map) => Params(map)[3] / FabricSpace.CmPerMetre;
    public static void MapSetUseEdgeConnections(Rid map, bool enabled) => SetParam(map, 4, enabled ? 1 : 0);
    public static bool MapGetUseEdgeConnections(Rid map) => Params(map)[4] != 0;

    /// <summary>Applies pending changes to the map now (Godot's map_force_update).</summary>
    public static void MapForceUpdate(Rid map)
    {
        if (!Ok) return;
        if (NavNative.aver_nav_map_update(I(map)) != 0) Rebuilt(I(map));
    }

    private static void Rebuilt(int map) => MapChanged?.Invoke(R(map));

    public static uint MapGetIterationId(Rid map) => Ok ? (uint)NavNative.aver_nav_map_iteration(I(map)) : 0;

    public static Collections.Array<Rid> GetMaps()
    {
        var a = new Collections.Array<Rid>();
        if (!Ok) return a;
        int n = NavNative.aver_nav_map_list(null, 0);
        var ids = new int[n];
        NavNative.aver_nav_map_list(ids, n);
        foreach (int id in ids) a.Add(R(id));
        return a;
    }

    private static Collections.Array<Rid> Objects(Rid map, NavKind kind)
    {
        var a = new Collections.Array<Rid>();
        if (!Ok) return a;
        int n = NavNative.aver_nav_map_objects(I(map), (int)kind, null, 0);
        var ids = new int[n];
        NavNative.aver_nav_map_objects(I(map), (int)kind, ids, n);
        foreach (int id in ids) a.Add(R(id));
        return a;
    }

    public static Collections.Array<Rid> MapGetRegions(Rid map) => Objects(map, NavKind.Region);
    public static Collections.Array<Rid> MapGetLinks(Rid map) => Objects(map, NavKind.Link);
    public static Collections.Array<Rid> MapGetAgents(Rid map) => Objects(map, NavKind.Agent);
    public static Collections.Array<Rid> MapGetObstacles(Rid map) => Objects(map, NavKind.Obstacle);

    // ---- map queries ----

    /// <summary>A path across the map between two global positions. <paramref name="optimize"/> true gives the funnel-smoothed path, false the portal-centred one.</summary>
    public static Vector3[] MapGetPath(Rid map, Vector3 origin, Vector3 destination, bool optimize, uint navigationLayers = 1)
    {
        var q = new NavigationPathQueryParameters3D
        {
            Map = map, StartPosition = origin, TargetPosition = destination, NavigationLayers = navigationLayers,
            PathPostprocessing = optimize ? PathPostProcessing.CorridorFunnel : PathPostProcessing.Edgecentered,
        };
        var r = new NavigationPathQueryResult3D();
        QueryPath(q, r);
        return r.Path;
    }

    /// <summary>Runs a path query and fills <paramref name="result"/>.</summary>
    public static void QueryPath(NavigationPathQueryParameters3D parameters, NavigationPathQueryResult3D result)
    {
        result.Reset();
        if (!Ok || !parameters.Map.IsValid) return;
        int[] ex = parameters.ExcludedRegions.Select(I).ToArray();
        int[] inc = parameters.IncludedRegions.Select(I).ToArray();
        float eps = parameters.SimplifyPath ? Math.Max(parameters.SimplifyEpsilon, 0f) * FabricSpace.CmPerMetre : 0f;
        if (parameters.SimplifyPath && eps <= 0f) eps = 0f;
        int n = NavNative.aver_nav_path_query(I(parameters.Map), NavSpace.ToEngine(parameters.StartPosition), NavSpace.ToEngine(parameters.TargetPosition), parameters.NavigationLayers,
                                              (int)ToNative(parameters.PathPostprocessing), eps, ex, ex.Length, inc, inc.Length);
        if (n <= 0) return;
        var pts = new float[n * 3];
        NavNative.aver_nav_path_points(pts, n);
        var path = new Vector3[n];
        for (int i = 0; i < n; i++) path[i] = NavSpace.FromEngine(pts, i * 3);
        var types = new int[n];
        var rids = new int[n];
        var owners = new long[n];
        NavNative.aver_nav_path_info(types, rids, owners, n);
        PathMetadataFlags f = parameters.MetadataFlags;
        result.Path = path;
        if ((f & PathMetadataFlags.IncludeTypes) != 0) result.PathTypes = types;
        if ((f & PathMetadataFlags.IncludeRids) != 0) result.PathRids = new Collections.Array<Rid>(rids.Select(R));
        if ((f & PathMetadataFlags.IncludeOwners) != 0) result.PathOwnerIds = owners;
        result.PathLength = NavNative.aver_nav_path_length() / FabricSpace.CmPerMetre;
        result.ReachedTarget = NavNative.aver_nav_path_reached() != 0;
    }

    private static NavPost ToNative(PathPostProcessing p) => p switch { PathPostProcessing.Edgecentered => NavPost.EdgeCentered, PathPostProcessing.None => NavPost.None, _ => NavPost.Funnel };

    public static Vector3 MapGetClosestPoint(Rid map, Vector3 toPoint) => Closest(map, toPoint, out _, out _, out _);
    public static Vector3 MapGetClosestPointNormal(Rid map, Vector3 toPoint) { Closest(map, toPoint, out Vector3 n, out _, out _); return n; }
    public static Rid MapGetClosestPointOwner(Rid map, Vector3 toPoint) { Closest(map, toPoint, out _, out Rid r, out _); return r; }

    private static Vector3 Closest(Rid map, Vector3 p, out Vector3 normal, out Rid region, out long owner)
    {
        normal = Vector3.Zero; region = default; owner = 0;
        if (!Ok) return Vector3.Zero;
        var pt = new float[3];
        var nm = new float[3];
        if (NavNative.aver_nav_map_closest(I(map), NavSpace.ToEngine(p), 0xFFFFFFFFu, pt, nm, out int reg, out owner) == 0) return Vector3.Zero;
        normal = new Vector3(nm[1], nm[2], -nm[0]);   // a direction: engine (x, y, z) back to Godot (y, z, -x)
        region = R(reg);
        return NavSpace.FromEngine(pt);
    }

    /// <summary>The point of the navigation mesh closest to a segment (Godot's map_get_closest_point_to_segment).</summary>
    public static Vector3 MapGetClosestPointToSegment(Rid map, Vector3 start, Vector3 end, bool useCollision = false)
    {
        if (!Ok) return Vector3.Zero;
        var onMesh = new float[3];
        var onSeg = new float[3];
        return NavNative.aver_nav_map_closest_segment(I(map), NavSpace.ToEngine(start), NavSpace.ToEngine(end), onMesh, onSeg) != 0 ? NavSpace.FromEngine(onMesh) : Vector3.Zero;
    }

    private static readonly RandomNumberGenerator s_rng = new();

    /// <summary>A random point on the map's polygons that match the layers (<paramref name="uniformly"/> weights polygons by area).</summary>
    public static Vector3 MapGetRandomPoint(Rid map, uint navigationLayers, bool uniformly)
    {
        if (!Ok) return Vector3.Zero;
        var o = new float[3];
        return NavNative.aver_nav_map_random_point(I(map), navigationLayers, uniformly ? 1 : 0, s_rng.Randf(), s_rng.Randf(), s_rng.Randf(), o) != 0 ? NavSpace.FromEngine(o) : Vector3.Zero;
    }

    // ---- regions ----

    public static Rid RegionCreate() => Ok ? R(NavNative.aver_nav_region_create(0)) : default;
    public static void RegionSetMap(Rid region, Rid map) { if (Ok) NavNative.aver_nav_set_map(I(region), I(map)); }
    public static Rid RegionGetMap(Rid region) => Ok ? R(NavNative.aver_nav_get_map(I(region))) : default;
    public static void RegionSetEnabled(Rid region, bool enabled) { if (Ok) NavNative.aver_nav_region_set_enabled(I(region), enabled ? 1 : 0); }
    public static bool RegionGetEnabled(Rid region) { var o = new float[4]; return Ok && NavNative.aver_nav_region_get(I(region), o) != 0 && o[0] != 0; }
    public static void RegionSetUseEdgeConnections(Rid region, bool enabled) { if (Ok) NavNative.aver_nav_region_set_edge_connections(I(region), enabled ? 1 : 0); }
    public static bool RegionGetUseEdgeConnections(Rid region) => Ok && NavNative.aver_nav_region_edge_connections(I(region)) != 0;
    public static void RegionSetNavigationLayers(Rid region, uint layers) { if (Ok) NavNative.aver_nav_region_set_layers(I(region), layers); }
    public static uint RegionGetNavigationLayers(Rid region) { var o = new float[4]; return Ok && NavNative.aver_nav_region_get(I(region), o) != 0 ? (uint)o[1] : 0u; }
    public static void RegionSetEnterCost(Rid region, float enterCost) { if (Ok) { var o = new float[4]; NavNative.aver_nav_region_get(I(region), o); NavNative.aver_nav_region_set_costs(I(region), Math.Max(enterCost, 0f), o[3]); } }
    public static float RegionGetEnterCost(Rid region) { var o = new float[4]; return Ok && NavNative.aver_nav_region_get(I(region), o) != 0 ? o[2] : 0f; }
    public static void RegionSetTravelCost(Rid region, float travelCost) { if (Ok) { var o = new float[4]; NavNative.aver_nav_region_get(I(region), o); NavNative.aver_nav_region_set_costs(I(region), o[2], Math.Max(travelCost, 0f)); } }
    public static float RegionGetTravelCost(Rid region) { var o = new float[4]; return Ok && NavNative.aver_nav_region_get(I(region), o) != 0 ? o[3] : 1f; }
    public static void RegionSetOwnerId(Rid region, ulong ownerId) { if (Ok) NavNative.aver_nav_set_owner(I(region), unchecked((long)ownerId)); }
    public static ulong RegionGetOwnerId(Rid region) => Ok ? unchecked((ulong)NavNative.aver_nav_get_owner(I(region))) : 0;

    /// <summary>Gives the region the polygons of a navigation mesh, placed with <paramref name="transform"/> (the region node's global transform).</summary>
    public static void RegionSetNavigationMesh(Rid region, NavigationMesh? mesh, Transform3D? transform = null)
    {
        if (!Ok) return;
        if (mesh == null || mesh.GetPolygonCount() == 0) { NavNative.aver_nav_region_set_mesh(I(region), null, 0, null, null, 0); return; }
        Transform3D t = transform ?? Transform3D.Identity;
        Vector3[] v = mesh.Vertices;
        var verts = new float[v.Length * 3];
        for (int i = 0; i < v.Length; i++) NavSpace.ToEngineInto(t * v[i], verts, i * 3);
        var sizes = new int[mesh.GetPolygonCount()];
        var idx = new List<int>();
        for (int p = 0; p < sizes.Length; p++)
        {
            int[] poly = mesh.GetPolygon(p);
            sizes[p] = poly.Length;
            idx.AddRange(poly);
        }
        NavNative.aver_nav_region_set_mesh(I(region), verts, v.Length, idx.ToArray(), sizes, sizes.Length);
    }

    /// <summary>How many edge connections the region has to other regions (Godot's region_get_connections_count).</summary>
    public static int RegionGetConnectionsCount(Rid region)
    {
        if (!Ok) return 0;
        MapForceUpdate(RegionGetMap(region));
        return NavNative.aver_nav_region_connections(I(region), null, 0);
    }

    public static Vector3 RegionGetConnectionPathwayStart(Rid region, int connection) => Pathway(region, connection, 0);
    public static Vector3 RegionGetConnectionPathwayEnd(Rid region, int connection) => Pathway(region, connection, 3);

    private static Vector3 Pathway(Rid region, int connection, int at)
    {
        if (!Ok) return Vector3.Zero;
        MapForceUpdate(RegionGetMap(region));
        int n = NavNative.aver_nav_region_connections(I(region), null, 0);
        if (connection < 0 || connection >= n) return Vector3.Zero;
        var o = new float[n * 6];
        NavNative.aver_nav_region_connections(I(region), o, n);
        return NavSpace.FromEngine(o, connection * 6 + at);
    }

    /// <summary>Whether a global point lies on the region's polygons (within the map's cell height).</summary>
    public static bool RegionOwnsPoint(Rid region, Vector3 point)
    {
        if (!Ok) return false;
        var pt = new float[3];
        if (NavNative.aver_nav_region_closest(I(region), NavSpace.ToEngine(point), pt, null) == 0) return false;
        Vector3 c = NavSpace.FromEngine(pt);
        return c.DistanceTo(point) <= Math.Max(MapGetCellHeight(RegionGetMap(region)), 0.01f);
    }

    // ---- links ----

    public static Rid LinkCreate() => Ok ? R(NavNative.aver_nav_link_create(0)) : default;
    public static void LinkSetMap(Rid link, Rid map) { if (Ok) NavNative.aver_nav_set_map(I(link), I(map)); }
    public static Rid LinkGetMap(Rid link) => Ok ? R(NavNative.aver_nav_get_map(I(link))) : default;

    private static float[] LinkInfo(Rid link)
    {
        var o = new float[11] { 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0 };
        if (Ok) NavNative.aver_nav_link_get(I(link), o);
        return o;
    }

    private static void LinkWrite(Rid link, float[] o) =>
        NavNative.aver_nav_link_set(I(link), (int)o[0], (int)o[1], (uint)o[2], o[3], o[4]);

    public static void LinkSetEnabled(Rid link, bool enabled) { if (!Ok) return; var o = LinkInfo(link); o[0] = enabled ? 1 : 0; LinkWrite(link, o); }
    public static bool LinkGetEnabled(Rid link) => LinkInfo(link)[0] != 0;
    public static void LinkSetBidirectional(Rid link, bool bidirectional) { if (!Ok) return; var o = LinkInfo(link); o[1] = bidirectional ? 1 : 0; LinkWrite(link, o); }
    public static bool LinkIsBidirectional(Rid link) => LinkInfo(link)[1] != 0;
    public static void LinkSetNavigationLayers(Rid link, uint layers) { if (!Ok) return; var o = LinkInfo(link); o[2] = layers; LinkWrite(link, o); }
    public static uint LinkGetNavigationLayers(Rid link) => (uint)LinkInfo(link)[2];
    public static void LinkSetEnterCost(Rid link, float enterCost) { if (!Ok) return; var o = LinkInfo(link); o[3] = Math.Max(enterCost, 0f); LinkWrite(link, o); }
    public static float LinkGetEnterCost(Rid link) => LinkInfo(link)[3];
    public static void LinkSetTravelCost(Rid link, float travelCost) { if (!Ok) return; var o = LinkInfo(link); o[4] = Math.Max(travelCost, 0f); LinkWrite(link, o); }
    public static float LinkGetTravelCost(Rid link) => LinkInfo(link)[4];
    public static void LinkSetOwnerId(Rid link, ulong ownerId) { if (Ok) NavNative.aver_nav_set_owner(I(link), unchecked((long)ownerId)); }
    public static ulong LinkGetOwnerId(Rid link) => Ok ? unchecked((ulong)NavNative.aver_nav_get_owner(I(link))) : 0;

    public static void LinkSetStartPosition(Rid link, Vector3 position) => LinkSetEnds(link, position, null);
    public static void LinkSetEndPosition(Rid link, Vector3 position) => LinkSetEnds(link, null, position);
    public static Vector3 LinkGetStartPosition(Rid link) => NavSpace.FromEngine(LinkInfo(link), 5);
    public static Vector3 LinkGetEndPosition(Rid link) => NavSpace.FromEngine(LinkInfo(link), 8);

    private static void LinkSetEnds(Rid link, Vector3? start, Vector3? end)
    {
        if (!Ok) return;
        var o = LinkInfo(link);
        float[] s = start.HasValue ? NavSpace.ToEngine(start.Value) : new[] { o[5], o[6], o[7] };
        float[] e = end.HasValue ? NavSpace.ToEngine(end.Value) : new[] { o[8], o[9], o[10] };
        NavNative.aver_nav_link_set_ends(I(link), s, e);
    }

    /// <summary>How many of the link's ends attached to a polygon after the last update (Aver extension for tests and debugging).</summary>
    public static int LinkGetConnectedEnds(Rid link) => Ok ? NavNative.aver_nav_link_connected(I(link)) : 0;

    // ---- agents (avoidance) ----

    public static Rid AgentCreate() => Ok ? R(NavNative.aver_nav_agent_create(0)) : default;
    public static void AgentSetMap(Rid agent, Rid map) { if (Ok) NavNative.aver_nav_set_map(I(agent), I(map)); }
    public static Rid AgentGetMap(Rid agent) => Ok ? R(NavNative.aver_nav_get_map(I(agent))) : default;

    private static float[] AgentInfo(Rid agent)
    {
        var o = new float[(int)NavAgent.ParamCount];
        if (Ok) NavNative.aver_nav_agent_get_params(I(agent), o);
        return o;
    }

    private static void AgentSet(Rid agent, NavAgent slot, float value)
    {
        if (!Ok) return;
        var o = AgentInfo(agent);
        o[(int)slot] = value;
        NavNative.aver_nav_agent_set_params(I(agent), o);
    }

    public static void AgentSetAvoidanceEnabled(Rid agent, bool enabled) => AgentSet(agent, NavAgent.Avoidance, enabled ? 1 : 0);
    public static bool AgentGetAvoidanceEnabled(Rid agent) => AgentInfo(agent)[(int)NavAgent.Avoidance] != 0;
    public static void AgentSetPaused(Rid agent, bool paused) => AgentSet(agent, NavAgent.Paused, paused ? 1 : 0);
    public static bool AgentGetPaused(Rid agent) => AgentInfo(agent)[(int)NavAgent.Paused] != 0;
    public static void AgentSetRadius(Rid agent, float radius) => AgentSet(agent, NavAgent.Radius, radius * FabricSpace.CmPerMetre);
    public static void AgentSetHeight(Rid agent, float height) => AgentSet(agent, NavAgent.Height, height * FabricSpace.CmPerMetre);
    public static void AgentSetMaxSpeed(Rid agent, float maxSpeed) => AgentSet(agent, NavAgent.MaxSpeed, maxSpeed * FabricSpace.CmPerMetre);
    public static void AgentSetNeighborDistance(Rid agent, float distance) => AgentSet(agent, NavAgent.NeighborDist, distance * FabricSpace.CmPerMetre);
    public static void AgentSetMaxNeighbors(Rid agent, int count) => AgentSet(agent, NavAgent.MaxNeighbors, count);
    public static void AgentSetTimeHorizonAgents(Rid agent, float seconds) => AgentSet(agent, NavAgent.HorizonAgents, seconds);
    public static void AgentSetTimeHorizonObstacles(Rid agent, float seconds) => AgentSet(agent, NavAgent.HorizonObstacles, seconds);
    public static void AgentSetAvoidanceLayers(Rid agent, uint layers) => AgentSet(agent, NavAgent.Layers, layers);
    public static void AgentSetAvoidanceMask(Rid agent, uint mask) => AgentSet(agent, NavAgent.Mask, mask);
    public static void AgentSetAvoidancePriority(Rid agent, float priority) => AgentSet(agent, NavAgent.Priority, priority);
    public static void AgentSetPosition(Rid agent, Vector3 position) { if (Ok) NavNative.aver_nav_agent_set_position(I(agent), NavSpace.ToEngine(position)); }

    /// <summary>Sets the velocity the agent wants. The safe velocity arrives through the avoidance callback after the next navigation step.</summary>
    public static void AgentSetVelocity(Rid agent, Vector3 velocity) { if (Ok) NavNative.aver_nav_agent_set_velocity(I(agent), NavSpace.ToEngine(velocity)); }

    /// <summary>Sets the callback that receives the safe velocity (<c>Callable(Vector3 safeVelocity)</c>).</summary>
    public static void AgentSetAvoidanceCallback(Rid agent, Callable callback) { if (callback.IsNull) s_avoidanceCallbacks.Remove(I(agent)); else s_avoidanceCallbacks[I(agent)] = callback; }
    public static bool AgentHasAvoidanceCallback(Rid agent) => s_avoidanceCallbacks.ContainsKey(I(agent));

    // ---- obstacles ----

    public static Rid ObstacleCreate() => Ok ? R(NavNative.aver_nav_obstacle_create(0)) : default;
    public static void ObstacleSetMap(Rid obstacle, Rid map) { if (Ok) NavNative.aver_nav_set_map(I(obstacle), I(map)); }
    public static Rid ObstacleGetMap(Rid obstacle) => Ok ? R(NavNative.aver_nav_get_map(I(obstacle))) : default;

    private static float[] ObstacleInfo(Rid o)
    {
        var f = new float[6];
        if (Ok) NavNative.aver_nav_obstacle_get(I(o), f);
        return f;
    }

    private static void ObstacleSet(Rid o, int slot, float value)
    {
        if (!Ok) return;
        var f = ObstacleInfo(o);
        f[slot] = value;
        NavNative.aver_nav_obstacle_set(I(o), (int)f[0], (int)f[1], (uint)f[2], f[3], f[4]);
    }

    public static void ObstacleSetAvoidanceEnabled(Rid obstacle, bool enabled) => ObstacleSet(obstacle, 0, enabled ? 1 : 0);
    public static bool ObstacleGetAvoidanceEnabled(Rid obstacle) => ObstacleInfo(obstacle)[0] != 0;
    public static void ObstacleSetPaused(Rid obstacle, bool paused) => ObstacleSet(obstacle, 1, paused ? 1 : 0);
    public static bool ObstacleGetPaused(Rid obstacle) => ObstacleInfo(obstacle)[1] != 0;
    public static void ObstacleSetAvoidanceLayers(Rid obstacle, uint layers) => ObstacleSet(obstacle, 2, layers);
    public static uint ObstacleGetAvoidanceLayers(Rid obstacle) => (uint)ObstacleInfo(obstacle)[2];
    public static void ObstacleSetRadius(Rid obstacle, float radius) => ObstacleSet(obstacle, 3, radius * FabricSpace.CmPerMetre);
    public static float ObstacleGetRadius(Rid obstacle) => ObstacleInfo(obstacle)[3] / FabricSpace.CmPerMetre;
    public static void ObstacleSetHeight(Rid obstacle, float height) => ObstacleSet(obstacle, 4, height * FabricSpace.CmPerMetre);
    public static float ObstacleGetHeight(Rid obstacle) => ObstacleInfo(obstacle)[4] / FabricSpace.CmPerMetre;

    // The engine takes position and velocity together; each setter keeps the other's last value.
    private static readonly Dictionary<int, (Vector3 Position, Vector3 Velocity)> s_obstacleMotion = new();

    public static void ObstacleSetPosition(Rid obstacle, Vector3 position)
    {
        s_obstacleMotion.TryGetValue(I(obstacle), out var m);
        ObstacleSetMotion(obstacle, position, m.Velocity);
    }

    public static void ObstacleSetVelocity(Rid obstacle, Vector3 velocity)
    {
        s_obstacleMotion.TryGetValue(I(obstacle), out var m);
        ObstacleSetMotion(obstacle, m.Position, velocity);
    }

    /// <summary>Sets position and velocity together (the obstacle node's way; the two setters above each need the other value).</summary>
    internal static void ObstacleSetMotion(Rid obstacle, Vector3 position, Vector3 velocity)
    {
        s_obstacleMotion[I(obstacle)] = (position, velocity);
        if (Ok) NavNative.aver_nav_obstacle_set_motion(I(obstacle), NavSpace.ToEngine(position), NavSpace.ToEngine(velocity));
    }

    /// <summary>Sets the obstacle's wall polygon from GLOBAL positions (empty for a round obstacle).</summary>
    public static void ObstacleSetVertices(Rid obstacle, Vector3[] vertices)
    {
        if (!Ok) return;
        var v = new float[vertices.Length * 3];
        for (int i = 0; i < vertices.Length; i++) NavSpace.ToEngineInto(vertices[i], v, i * 3);
        NavNative.aver_nav_obstacle_set_vertices(I(obstacle), v, vertices.Length);
    }

    // ---- RID housekeeping ----

    /// <summary>Frees a map, region, link, agent or obstacle.</summary>
    public static void FreeRid(Rid rid)
    {
        if (!Ok || !rid.IsValid) return;
        int id = I(rid);
        s_avoidanceCallbacks.Remove(id);
        if (NavNative.aver_nav_kind(id) == (int)NavKind.Map) NavNative.aver_nav_map_destroy(id);
        else NavNative.aver_nav_destroy(id);
    }

    // ---- baking ----

    /// <summary>Collects triangles from the scene under <paramref name="rootNode"/> as the navigation mesh's parsing settings say.</summary>
    public static void ParseSourceGeometryData(NavigationMesh navigationMesh, NavigationMeshSourceGeometryData3D sourceGeometryData, Node rootNode, Callable callback = default) =>
        NavigationMeshGenerator.Parse(navigationMesh, sourceGeometryData, rootNode, callback);

    /// <summary>Bakes the triangles into the navigation mesh, now.</summary>
    public static void BakeFromSourceGeometryData(NavigationMesh navigationMesh, NavigationMeshSourceGeometryData3D sourceGeometryData, Callable callback = default) =>
        NavigationMeshGenerator.BakeNow(navigationMesh, sourceGeometryData, callback);

    /// <summary>Bakes on a worker thread; the mesh is updated and the callback called from a later frame's pump.</summary>
    public static void BakeFromSourceGeometryDataAsync(NavigationMesh navigationMesh, NavigationMeshSourceGeometryData3D sourceGeometryData, Callable callback = default) =>
        NavigationMeshGenerator.BakeAsync(navigationMesh, sourceGeometryData, callback);

    public static bool IsBakingNavigationMesh(NavigationMesh navigationMesh) { lock (s_baking) return s_baking.Contains(navigationMesh); }

    internal static void BakeStarted(NavigationMesh m) { lock (s_baking) s_baking.Add(m); }
    internal static void BakeEnded(NavigationMesh m) { lock (s_baking) s_baking.Remove(m); }
    internal static void PostCompletion(Action a) => s_completions.Enqueue(a);

    // ---- the per-frame pump ----

    /// <summary>Runs what must happen once per physics frame: finished bakes are applied, nodes that follow a transform push it, and avoidance steps.
    /// Called by the first navigation node that processes in a frame; <paramref name="delta"/> is the physics step.</summary>
    internal static void Process(double delta)
    {
        long frame = NodeHost.Current.IsEngine ? Aver.Framework.Engine.PhysicsFrames : SceneTree.Instance.PhysicsFrameCount;
        if (frame == s_lastStepFrame) return;
        s_lastStepFrame = frame;
        while (s_completions.TryDequeue(out Action? done)) done();
        foreach (NavigationRegion3D r in Regions.ToArray()) r.SyncTransform();
        foreach (NavigationLink3D l in Links.ToArray()) l.SyncTransform();
        foreach (NavigationObstacle3D o in Obstacles.ToArray()) o.SyncTransform();
        foreach (NavigationAgent3D a in Agents.ToArray()) a.SyncToServer();
        if (!Ok) return;
        foreach (Rid map in GetMaps())
        {
            if (NavNative.aver_nav_map_update(I(map)) != 0) Rebuilt(I(map));
            if (!s_active || NavNative.aver_nav_map_is_active(I(map)) == 0) continue;
            NavNative.aver_nav_map_step(I(map), (float)delta);
        }
        foreach (NavigationAgent3D a in Agents.ToArray()) a.CollectSafeVelocity();
    }

    internal static bool TryGetAvoidanceCallback(Rid agent, out Callable callback) => s_avoidanceCallbacks.TryGetValue(I(agent), out callback);
}
