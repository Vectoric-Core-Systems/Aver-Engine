// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 N1: NavigationServer3D, NavigationRegion3D / Link3D / Obstacle3D / Agent3D, NavigationMesh baking, AStar3D / AStar2D / AStarGrid2D against the
// engine's navigation module (Aver.Nav.Abi: pure arithmetic, no device). The geometry, the funnel, the costs and the search have their reference values in
// the C++ suites (NavMeshTest, NavMeshBakeTest, NavGraphTest, NavAbiTest); what is checked here is the Godot surface on top: metres and Godot's axes,
// property names, node lifecycles, the agent's signal sequence, the A* classes' answers (worked out by hand) and what the converter wrote.
// The suite needs build/bin/Aver.Nav.Abi; without it every test is skipped.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;
using IO = System.IO;

static class NavigationTests
{
    private static bool s_native;

    public static int RunAll()
    {
        s_native = EnsureNative();
        if (!s_native)
        {
            Console.WriteLine("Test: navigation (SKIP: build/bin/Aver.Nav.Abi was not found; build the Aver.Nav.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            Run("NavigationServer3D: one region, metres and Godot's axes, off-mesh snapping, closest point, owner", OneRegion);
            Run("NavigationServer3D: the funnel bends at the inner corner; edge-centred and none give the portal route", Funnel);
            Run("floors over the same ground plan stay apart; a ramp joins levels; closest point picks the floor by height", MultiFloor);
            Run("regions join across a seam within the edge margin, not beyond it, not with edge connections off", Seams);
            Run("navigation_layers filter regions; enabled; included and excluded regions", Layers);
            Run("enter_cost and travel_cost steer the route", Costs);
            Run("NavigationLink3D: joins islands, adds typed points with its rid and owner, one-way, layers, enabled, node transform", Links);
            Run("NavigationMesh: the resource API, create_from_mesh, duplicate, scene values", MeshResource);
            Run("baking: NavigationRegion3D.bake_navigation_mesh from a collision floor (parameters, bounds, signal)", BakeFloor);
            Run("baking: an obstacle with affect_navigation_mesh carves a hole the path goes round", BakeCarving);
            Run("baking: floors, a ramp within the slope, and a wall the agent radius shrinks away from", BakeFloorsAndWalls);
            Run("baking on a worker thread finishes through the frame pump and emits bake_finished", BakeAsync);
            Run("NavigationAgent3D: the signal sequence along a bent path", AgentSignals);
            Run("NavigationAgent3D: an unreachable target finishes without target_reached; retarget; distances", AgentUnreachable);
            Run("NavigationAgent3D: link_reached on both ends of a link; path_max_distance repaths", AgentLinks);
            Run("NavigationAgent3D: avoidance keeps two head-on agents apart, caps the speed and answers velocity_computed", AgentAvoidance);
            Run("NavigationObstacle3D: a round obstacle steers an agent; avoidance layers; paused", ObstacleAvoidance);
            Run("NavigationServer3D: maps, random points, region connections, rids of freed objects", ServerMisc);
            Run("AStar3D: hand-worked paths, weights, disabled points, one-way segments, partial paths, closest points", AStar3DPaths);
            Run("AStar3D: _ComputeCost and _EstimateCost overrides are called and obeyed", AStar3DOverrides);
            Run("AStar2D: the same graph in 2D", AStar2DPaths);
            Run("AStarGrid2D: region, update, solid cells, weights, positions, cell shapes", GridBasics);
            Run("AStarGrid2D: diagonal modes at a corner", GridDiagonals);
            Run("AStarGrid2D: heuristics, a wall, jumping, _ComputeCost override, partial paths", GridSearch);
            Run("the converter's NavGame output instantiates, links the floors and bakes the unbaked mesh (AverAssetC)", ConverterOutput);
        });
    }

    private static void Run(string name, Action body) =>
        T.Run(name, () => { NavigationServer3D.ResetForTests(); body(); });

    // ---- the native library ----

    private static string LibFile() =>
        OperatingSystem.IsWindows() ? "Aver.Nav.Abi.dll" : OperatingSystem.IsMacOS() ? "libAver.Nav.Abi.dylib" : "libAver.Nav.Abi.so";

    private static string? FindBin(string file)
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string bin = IO.Path.Combine(d.FullName, "build", "bin");
            if (File.Exists(IO.Path.Combine(bin, file))) return bin;
        }
        return null;
    }

    internal static bool EnsureNative()
    {
        string? bin = FindBin(LibFile());
        if (bin != null)
        {
            string beside = IO.Path.Combine(AppContext.BaseDirectory, LibFile());
            try { if (!File.Exists(beside) || File.GetLastWriteTimeUtc(beside) < File.GetLastWriteTimeUtc(IO.Path.Combine(bin, LibFile()))) File.Copy(IO.Path.Combine(bin, LibFile()), beside, true); }
            catch (IOException) { /* a copy from an earlier run is in use */ }
            NavNative.Reprobe();
        }
        return NavNative.Available;
    }

    // ---- builders ----

    private static Rid Map => NavigationServer3D.GetDefaultMapRid();

    /// <summary>A rectangle of navigation mesh at height y, in Godot's axes (x east, z south).</summary>
    private static NavigationMesh Quad(float x0, float z0, float x1, float z1, float y = 0f)
    {
        var m = new NavigationMesh { Vertices = new[] { new Vector3(x0, y, z0), new Vector3(x1, y, z0), new Vector3(x1, y, z1), new Vector3(x0, y, z1) } };
        m.AddPolygon(new[] { 0, 1, 2, 3 });
        return m;
    }

    private static NavigationRegion3D Region(NavigationMesh m, uint layers = 1, Vector3 at = default)
    {
        var r = new NavigationRegion3D { NavigationMesh = m, NavigationLayers = layers, Position = at };
        SceneTree.Instance.Root.AddChild(r);
        return r;
    }

    private static Vector3[] Path(Vector3 a, Vector3 b, uint layers = 1, bool optimize = true) => NavigationServer3D.MapGetPath(Map, a, b, optimize, layers);

    private static NavigationPathQueryResult3D Query(Vector3 a, Vector3 b, uint layers = 1, PathPostProcessing post = PathPostProcessing.CorridorFunnel, Action<NavigationPathQueryParameters3D>? tweak = null)
    {
        var q = new NavigationPathQueryParameters3D { Map = Map, StartPosition = a, TargetPosition = b, NavigationLayers = layers, PathPostprocessing = post };
        tweak?.Invoke(q);
        var r = new NavigationPathQueryResult3D();
        NavigationServer3D.QueryPath(q, r);
        return r;
    }

    private static float Len(Vector3[] p)
    {
        float s = 0;
        for (int i = 1; i < p.Length; i++) s += p[i].DistanceTo(p[i - 1]);
        return s;
    }

    private static bool Arrives(NavigationPathQueryResult3D r, Vector3 target) => r.ReachedTarget && r.Path.Length > 0 && r.Path[^1].DistanceTo(target) < 0.05f;

    // ---- tests ----

    private static void OneRegion()
    {
        var r = Region(Quad(-5, -5, 5, 5));
        T.Check("the region has a handle and is on the default map", r.GetRid().IsValid && r.GetNavigationMap() == Map && NavigationServer3D.RegionGetMap(r.GetRid()) == Map);
        T.Check("its owner id is the node's instance id", NavigationServer3D.RegionGetOwnerId(r.GetRid()) == r.GetInstanceId());
        Vector3[] p = Path(new Vector3(-4, 0, -4), new Vector3(4, 0, 4));
        T.Check("a straight line is two points", p.Length == 2);
        T.CheckNum("its length is the straight distance in metres", MathF.Sqrt(2f) * 8f, Len(p), 0.02f);
        T.CheckVec("start", new Vector3(-4, 0, -4), p[0], 0.02f);
        T.CheckVec("and end, in Godot's axes", new Vector3(4, 0, 4), p[^1], 0.02f);
        p = Path(new Vector3(-8, 0, -4), new Vector3(4, 0, 4));
        T.CheckVec("an off-mesh start snaps to the mesh edge", new Vector3(-5, 0, -4), p[0], 0.03f);
        T.CheckVec("the closest point above the mesh", new Vector3(2, 0, 3), NavigationServer3D.MapGetClosestPoint(Map, new Vector3(2, 5, 3)), 0.02f);
        T.CheckVec("and its normal faces +Y", Vector3.Up, NavigationServer3D.MapGetClosestPointNormal(Map, new Vector3(2, 5, 3)), 0.02f);
        T.Check("its owner is the region", NavigationServer3D.MapGetClosestPointOwner(Map, new Vector3(2, 5, 3)) == r.GetRid());
        T.CheckVec("the closest point beside the mesh", new Vector3(5, 0, 3), NavigationServer3D.MapGetClosestPoint(Map, new Vector3(9, 0, 3)), 0.02f);

        NavigationPathQueryResult3D q = Query(new Vector3(-4, 0, -4), new Vector3(4, 0, 4));
        T.Check("the result carries types, rids and owners by default",
                q.PathTypes.Length == 2 && q.PathTypes[0] == (int)PathSegmentType.Region && q.PathRids.Count == 2 && q.PathRids[0] == r.GetRid() && q.PathOwnerIds.Length == 2 && (ulong)q.PathOwnerIds[0] == r.GetInstanceId());
        T.CheckNum("and the length in metres", MathF.Sqrt(2f) * 8f, q.PathLength, 0.02f);
        q = Query(new Vector3(-4, 0, -4), new Vector3(4, 0, 4), 1, PathPostProcessing.CorridorFunnel, x => x.MetadataFlags = PathMetadataFlags.IncludeTypes);
        T.Check("metadata flags choose what comes back", q.PathTypes.Length == 2 && q.PathRids.Count == 0 && q.PathOwnerIds.Length == 0);

        // The region follows its node.
        r.Position = new Vector3(20, 2, 0);
        T.Frame(1.0 / 60.0, 1);
        p = Path(new Vector3(16, 2, -4), new Vector3(24, 2, 4));
        T.Check("a moved region is re-placed at the next physics frame", p.Length == 2 && T.Near(p[0], new Vector3(16, 2, -4), 0.03f) && T.Near(p[^1], new Vector3(24, 2, 4), 0.03f));
        r.QueueFree();
        T.Frame(1.0 / 60.0, 1);
        T.Check("a freed region leaves the map", NavigationServer3D.MapGetRegions(Map).Count == 0 && Path(new Vector3(16, 2, -4), new Vector3(24, 2, 4)).Length == 0);
    }

    private static void Funnel()
    {
        // An L: a long strip along +X and a column up +Z, sharing the edge (8,2)-(10,2) with a T-junction.
        var m = new NavigationMesh
        {
            Vertices = new[] { new Vector3(0, 0, 0), new Vector3(10, 0, 0), new Vector3(10, 0, 2), new Vector3(0, 0, 2), new Vector3(8, 0, 2), new Vector3(8, 0, 10), new Vector3(10, 0, 10) },
        };
        m.AddPolygon(new[] { 0, 1, 2, 3 });
        m.AddPolygon(new[] { 4, 2, 6, 5 });
        var r = Region(m);
        var a = new Vector3(1, 0, 1);
        var b = new Vector3(9, 0, 9);
        Vector3[] p = Path(a, b);
        T.Check("start, corner, goal", p.Length == 3);
        T.CheckVec("the corner is the inner vertex", new Vector3(8, 0, 2), p[1], 0.03f);
        T.CheckNum("the length through the corner", 2f * MathF.Sqrt(50f), Len(p), 0.03f);
        p = Path(a, b, 1, false);
        T.Check("edge-centred: the portal midpoint", p.Length == 3 && T.Near(p[1], new Vector3(9, 0, 2), 0.03f));
        NavigationPathQueryResult3D q = Query(a, b, 1, PathPostProcessing.None);
        T.Check("none: the raw corridor", q.Path.Length == 3);
        q = Query(a, b, 1, PathPostProcessing.CorridorFunnel, x => { x.SimplifyPath = true; x.SimplifyEpsilon = 5f; });
        T.Check("simplify_path with a large epsilon drops the corner", q.Path.Length == 2);
        q = Query(a, b, 1, PathPostProcessing.CorridorFunnel, x => { x.SimplifyPath = true; x.SimplifyEpsilon = 0.01f; });
        T.Check("and a small one keeps it", q.Path.Length == 3);
        T.CheckNum("the region reports the mesh's two polygons", 2, NavNative.aver_nav_map_polygon_count(unchecked((int)(uint)Map.Id)));
        T.Check("the path is reached", Arrives(Query(a, b), b));
        r.Enabled = true;
    }

    private static void MultiFloor()
    {
        Region(Quad(-5, -5, 5, 5, 0));
        var mezz = Region(Quad(-5, -5, 5, 5, 3));
        var ramp = new NavigationMesh { Vertices = new[] { new Vector3(5, 0, -5), new Vector3(8, 3, -5), new Vector3(8, 3, 5), new Vector3(5, 0, 5) } };
        ramp.AddPolygon(new[] { 0, 1, 2, 3 });
        var rampRegion = Region(ramp);
        Region(Quad(8, -5, 13, 5, 3));
        var start = new Vector3(0, 0, 0);
        var goal = new Vector3(11, 3, 0);
        NavigationPathQueryResult3D q = Query(start, goal);
        T.Check("up the ramp to the upper level in the east", Arrives(q, goal));
        T.Check("starting low and ending high", T.Near(q.Path[0].Y, 0f, 0.03f) && T.Near(q.Path[^1].Y, 3f, 0.03f));
        T.Check("the funnel keeps the straight line", q.Path.Length == 2 && T.Near(q.Path[0].DistanceTo(q.Path[1]), MathF.Sqrt(121f + 9f), 0.05f));
        Vector3[] edge = Path(start, goal, 1, false);
        T.Check("edge-centred points sit on the ramp's two edges, at their heights", edge.Length == 4 && T.Near(edge[1], new Vector3(5, 0, 0), 0.05f) && T.Near(edge[2], new Vector3(8, 3, 0), 0.05f));
        T.CheckVec("closest point picks the lower floor by height", new Vector3(0, 0, 0), NavigationServer3D.MapGetClosestPoint(Map, new Vector3(0, 0.2f, 0)), 0.03f);
        T.CheckVec("and the upper one", new Vector3(0, 3, 0), NavigationServer3D.MapGetClosestPoint(Map, new Vector3(0, 2.9f, 0)), 0.03f);
        T.Check("whose owner is the mezzanine region", NavigationServer3D.MapGetClosestPointOwner(Map, new Vector3(0, 2.9f, 0)) == mezz.GetRid());
        q = Query(start, new Vector3(0, 3, 0));
        T.Check("the floor above the start is not joined to it: the path stops short", !q.ReachedTarget && q.Path.Length > 0 && T.Near(q.Path[^1].Y, 0f, 0.03f));
        rampRegion.QueueFree();
        T.Frame(1.0 / 60.0, 1);
        T.Check("without the ramp the upper level is cut off", !Query(start, goal).ReachedTarget);
    }

    private static void Seams()
    {
        Region(Quad(0, 0, 10, 10));
        var b = Region(Quad(10.1f, 0, 20, 10));
        var a = new Vector3(5, 0, 5);
        var c = new Vector3(15, 0, 5);
        T.Check("joined across a 10 cm seam", Arrives(Query(a, c), c));
        T.CheckNum("the length is the straight line", 10f, Len(Path(a, c)), 0.05f);
        T.Check("region_get_connections sees the seam", NavigationServer3D.RegionGetConnectionsCount(b.GetRid()) >= 1);
        T.Check("with a pathway between the seam's ends", NavigationServer3D.RegionGetConnectionPathwayStart(b.GetRid(), 0).DistanceTo(NavigationServer3D.RegionGetConnectionPathwayEnd(b.GetRid(), 0)) > 5f);
        NavigationServer3D.MapSetEdgeConnectionMargin(Map, 0.05f);
        T.Check("a 5 cm margin no longer joins them", !Query(a, c).ReachedTarget);
        NavigationServer3D.MapSetEdgeConnectionMargin(Map, 0.25f);
        T.Check("back to 25 cm they join again", Query(a, c).ReachedTarget);
        NavigationServer3D.MapSetUseEdgeConnections(Map, false);
        T.Check("the map can turn edge connections off", !Query(a, c).ReachedTarget && !NavigationServer3D.MapGetUseEdgeConnections(Map));
        NavigationServer3D.MapSetUseEdgeConnections(Map, true);
        b.UseEdgeConnections = false;
        T.Check("a region can opt out of joining other regions", !Query(a, c).ReachedTarget);
    }

    private static void Layers()
    {
        Region(Quad(0, 0, 10, 10), 1);
        var gate = Region(Quad(10, 0, 20, 10), 2);
        var a = new Vector3(5, 0, 5);
        var b = new Vector3(15, 0, 5);
        T.Check("layer 1 cannot enter a layer-2 region", !Arrives(Query(a, b, 1), b));
        T.Check("layers 1|2 can", Arrives(Query(a, b, 3), b));
        gate.SetNavigationLayerValue(3, true);
        T.Check("set_navigation_layer_value sets one bit", gate.NavigationLayers == 6 && gate.GetNavigationLayerValue(3) && !gate.GetNavigationLayerValue(1));
        T.Check("layer 4 (bit 3) now opens it", Arrives(Query(a, b, 5), b) && !Arrives(Query(a, b, 1), b));
        gate.Enabled = false;
        T.Check("a disabled region is gone", !Arrives(Query(a, b, 7), b));
        gate.Enabled = true;
        T.Check("and back", Arrives(Query(a, b, 7), b));
        var ex = Query(a, b, 7, PathPostProcessing.CorridorFunnel, p => p.ExcludedRegions = new Collections.Array<Rid> { gate.GetRid() });
        T.Check("excluded regions are skipped", !Arrives(ex, b));
        var inc = Query(a, b, 7, PathPostProcessing.CorridorFunnel, p => p.IncludedRegions = new Collections.Array<Rid> { gate.GetRid() });
        T.Check("only the gate: the start (off it) snaps onto its edge and the walk is the gate's 5 m", Arrives(inc, b) && inc.Path[0].X > 9.9f && T.Near(Len(inc.Path), 5f, 0.05f));
    }

    private static void Costs()
    {
        Region(Quad(0, 0, 10, 10));                       // west
        var swamp = Region(Quad(10, 4, 20, 6));           // middle, short
        Region(Quad(10, 6, 20, 10));                      // north road
        Region(Quad(20, 0, 30, 10));                      // east
        Region(Quad(10, 0, 20, 4));                       // south road
        var a = new Vector3(5, 0, 5);
        var b = new Vector3(25, 0, 5);
        static bool Through(Vector3[] p) => p.Any(v => v.X > 10.05f && v.X < 19.95f && v.Z > 4.05f && v.Z < 5.95f);
        Vector3[] cheap = Path(a, b);
        T.Check("all costs 1: straight through", Len(cheap) is > 19.98f and < 20.05f);
        swamp.TravelCost = 10f;
        Vector3[] dear = Path(a, b);
        T.Check("a swamp at travel cost 10 is walked round: the path hugs the swamp's far edge, 20.2 m long", !Through(dear) && Len(dear) is > 20.1f and < 20.3f && dear[^1].DistanceTo(b) < 0.05f);
        T.Check("the cost is read back", NavigationServer3D.RegionGetTravelCost(swamp.GetRid()) == 10f && swamp.TravelCost == 10f);
        swamp.TravelCost = 1f;
        swamp.EnterCost = 100000f;
        T.Check("an enter cost keeps the path out", !Through(Path(a, b)));
        swamp.EnterCost = 0f;
        T.Check("and with both back to normal it goes through again", Len(Path(a, b)) < 20.05f);
        swamp.TravelCost = -3f;
        T.Check("costs are clamped at zero", swamp.TravelCost == 0f);
    }

    private static void Links()
    {
        var home = Region(Quad(0, 0, 10, 10));
        Region(Quad(30, 0, 40, 10));
        var link = new NavigationLink3D { StartPosition = new Vector3(9, 0, 3), EndPosition = new Vector3(31, 0, 3), Position = new Vector3(0, 0, 2) };
        SceneTree.Instance.Root.AddChild(link);
        var a = new Vector3(1, 0, 5);
        var b = new Vector3(39, 0, 5);
        NavigationPathQueryResult3D q = Query(a, b);
        T.Check("the link joins the island", Arrives(q, b));
        T.Check("a link adds its start and its end as typed points",
                q.PathTypes.Count(t => t == (int)PathSegmentType.Link) == 2 && q.PathTypes.Length == q.Path.Length);
        int first = Array.IndexOf(q.PathTypes, (int)PathSegmentType.Link);
        T.CheckVec("at the link's own positions, node transform applied (global start)", new Vector3(9, 0, 5), q.Path[first], 0.03f);
        T.CheckVec("and end", new Vector3(31, 0, 5), q.Path[first + 1], 0.03f);
        T.Check("link points carry the link's rid and owner", q.PathRids[first] == link.GetRid() && (ulong)q.PathOwnerIds[first] == link.GetInstanceId() && (ulong)q.PathOwnerIds[0] == home.GetInstanceId());
        T.CheckNum("the length is the straight distance (the funnel passes straight through)", 38f, q.PathLength, 0.1f);
        T.Check("the link attached at both ends", NavigationServer3D.LinkGetConnectedEnds(link.GetRid()) == 2);
        T.CheckVec("global positions follow the node", new Vector3(9, 0, 5), link.GlobalStartPosition, 1e-4f);
        T.Check("bidirectional by default", Arrives(Query(b, a), a));
        link.Bidirectional = false;
        T.Check("one-way: forward yes, back no", Arrives(Query(a, b), b) && !Arrives(Query(b, a), a));
        link.NavigationLayers = 4;
        T.Check("link layers are checked against the query", !Arrives(Query(a, b, 1), b));
        T.Check("the query layer matches the link's and the regions'", Arrives(Query(a, b, 5), b));
        link.NavigationLayers = 1;
        link.EndPosition = new Vector3(31, 50, 1);
        T.Check("an end far from any polygon attaches to nothing", NavigationServer3D.LinkGetConnectedEnds(link.GetRid()) == 1 && !Query(a, b).ReachedTarget);
        link.EndPosition = new Vector3(31, 0, 3);
        link.Enabled = false;
        T.Check("a disabled link", !Query(a, b).ReachedTarget);
        link.Enabled = true;
        link.GlobalEndPosition = new Vector3(32, 0, 5);
        T.CheckVec("global_end_position sets the local end through the node's transform", new Vector3(32, 0, 3), link.EndPosition, 1e-4f);
        T.Check("the moved link still joins", Arrives(Query(a, b), b));
        link.Position = new Vector3(0, 0, 4);
        T.Frame(1.0 / 60.0, 1);
        T.Check("and moving the node moves the link", Arrives(Query(a, b), b) && Query(a, b).Path.Any(v => T.Near(v, new Vector3(9, 0, 7), 0.05f)));
        link.QueueFree();
        T.Frame(1.0 / 60.0, 1);
        T.Check("a freed link is gone", !Query(a, b).ReachedTarget && NavigationServer3D.MapGetLinks(Map).Count == 0);
    }

    private static void MeshResource()
    {
        var m = new NavigationMesh();
        T.Check("Godot's defaults", m.CellSize == 0.25f && m.CellHeight == 0.25f && m.AgentHeight == 1.5f && m.AgentRadius == 0.5f && m.AgentMaxClimb == 0.25f && m.AgentMaxSlope == 45f &&
                m.RegionMinSize == 2f && m.RegionMergeSize == 20f && m.EdgeMaxLength == 0f && m.EdgeMaxError == 1.3f && m.VerticesPerPolygon == 6 && m.DetailSampleDistance == 6f &&
                m.DetailSampleMaxError == 1f && m.GeometryParsedGeometryType == NavigationMesh.ParsedGeometryTypeEnum.MeshInstances && m.GeometryCollisionMask == 0xFFFFFFFF &&
                m.GeometrySourceGeometryMode == NavigationMesh.SourceGeometryModeEnum.RootNodeChildren && m.SamplePartitionType == NavigationMesh.SamplePartitionTypeEnum.Watershed);
        m.Vertices = new[] { new Vector3(0, 0, 0), new Vector3(1, 0, 0), new Vector3(1, 0, 1) };
        m.AddPolygon(new[] { 0, 1, 2 });
        T.Check("vertices and polygons", m.GetPolygonCount() == 1 && m.GetPolygon(0).SequenceEqual(new[] { 0, 1, 2 }) && m.GetVertices().Length == 3 && m.Polygons.Count == 1);
        var copy = (NavigationMesh)m.Duplicate();
        copy.AddPolygon(new[] { 2, 1, 0 });
        T.Check("a duplicate has its own polygons", m.GetPolygonCount() == 1 && copy.GetPolygonCount() == 2 && copy.CellSize == 0.25f);
        m.SetCollisionMaskValue(3, false);
        T.Check("geometry_collision_mask bits", !m.GetCollisionMaskValue(3) && m.GetCollisionMaskValue(1));
        m.Clear();
        T.Check("clear", m.GetPolygonCount() == 0 && m.GetVertices().Length == 0);

        var box = new BoxMesh { Size = new Vector3(2, 1, 2) };
        m.CreateFromMesh(box);
        T.Check("create_from_mesh makes a polygon of every triangle (a box: 12)", m.GetPolygonCount() == 12 && m.GetVertices().Length == 36);

        // As a scene stores it.
        Variant v = VariantText.Parse("Object(NavigationMesh, \"vertices\": PackedVector3Array(-5, 0, -5, 5, 0, -5, 5, 0, 5, -5, 0, 5), \"polygons\": [PackedInt32Array(0, 1, 2, 3)], " +
                                      "\"cell_size\": 0.5, \"agent_radius\": 0.3, \"geometry_parsed_geometry_type\": 1, \"sample_partition_type\": 1, \"filter_baking_aabb\": AABB(0, 0, 0, 1, 1, 1))");
        var nm = v.AsGodotObject() as NavigationMesh;
        T.Check("an Object(NavigationMesh, ...) value is built with its vertices, polygons and parameters",
                nm != null && nm.GetVertices().Length == 4 && nm.GetPolygonCount() == 1 && nm.GetPolygon(0).Length == 4 && nm.CellSize == 0.5f && T.Near(nm.AgentRadius, 0.3f) &&
                nm.GeometryParsedGeometryType == NavigationMesh.ParsedGeometryTypeEnum.StaticColliders && nm.SamplePartitionType == NavigationMesh.SamplePartitionTypeEnum.Monotone &&
                nm.FilterBakingAabb.Size == new Vector3(1, 1, 1));
        if (nm == null) return;
        var r = Region(nm);
        Vector3[] p = Path(new Vector3(-4, 0, -4), new Vector3(4, 0, 4));
        T.Check("and a region built on it answers queries", p.Length == 2);
        var more = new NavigationMesh();
        more.Vertices = new[] { new Vector3(10, 0, -5), new Vector3(20, 0, -5), new Vector3(20, 0, 5), new Vector3(10, 0, 5) };
        more.AddPolygon(new[] { 0, 1, 2, 3 });
        int changed = 0;
        r.NavigationMeshChanged += () => changed++;
        r.NavigationMesh = more;
        T.Check("replacing the mesh emits navigation_mesh_changed and re-places the polygons", changed == 1 && Path(new Vector3(12, 0, 0), new Vector3(18, 0, 0)).Length == 2 && Query(new Vector3(-4, 0, -4), new Vector3(4, 0, 4)).Path.All(v => v.X > 9.9f));
        more.AddPolygon(new[] { 0, 1, 2 });
        more.EmitChanged();
        T.Check("changing the mesh in place emits it too", changed == 2);
        T.CheckVec("the region's bounds in global space", new Vector3(10, 0, -5), r.GetBounds().Position, 1e-4f);
    }

    /// <summary>A flat collision floor (top at y = 0) under a region, as a baking source.</summary>
    private static NavigationRegion3D FloorRegion(float size, NavigationMesh? nm = null)
    {
        nm ??= new NavigationMesh();
        nm.GeometryParsedGeometryType = NavigationMesh.ParsedGeometryTypeEnum.StaticColliders;
        nm.CellSize = 0.25f;
        nm.CellHeight = 0.25f;
        var region = new NavigationRegion3D { NavigationMesh = nm };
        var body = new StaticBody3D { Name = "Floor" };
        body.AddChild(new CollisionShape3D { Shape = new BoxShape3D { Size = new Vector3(size, 0.2f, size) }, Position = new Vector3(0, -0.1f, 0) });
        region.AddChild(body);
        SceneTree.Instance.Root.AddChild(region);
        return region;
    }

    private static void BakeFloor()
    {
        var nm = new NavigationMesh { AgentRadius = 0.5f, AgentHeight = 1.5f };
        var region = FloorRegion(20f, nm);
        int finished = 0;
        region.BakeFinished += () => finished++;
        T.Check("an unbaked mesh has no polygons", nm.GetPolygonCount() == 0 && !region.IsBaking());
        region.BakeNavigationMesh(false);
        T.Frame(1.0 / 60.0, 1);
        T.Check("a synchronous bake fills the mesh and emits bake_finished once", nm.GetPolygonCount() > 0 && nm.GetVertices().Length > 3 && finished == 1 && !region.IsBaking());
        Aabb b = region.GetBounds();
        T.Check("the walkable area is the 20 m floor shrunk by the agent radius on each side: about 19 m (one cell of rounding)",
                b.Size.X is > 18.4f and < 19.6f && b.Size.Z is > 18.4f and < 19.6f, "size " + b.Size);
        T.Check("it sits on the floor", nm.GetVertices().All(v => MathF.Abs(v.Y) < 0.3f));
        Vector3[] p = Path(new Vector3(-8, 0, 0), new Vector3(8, 0, 0));
        T.Check("a path crosses the baked floor in a straight line", p.Length == 2 && T.Near(p[0], new Vector3(-8, 0, 0), 0.3f) && T.Near(p[^1], new Vector3(8, 0, 0), 0.3f));
        T.CheckNum("sixteen metres long", 16f, Len(p), 0.5f);

        // A bigger agent radius shrinks it further; the bake is deterministic.
        nm.AgentRadius = 2f;
        region.BakeNavigationMesh(false);
        T.Check("agent_radius 2 shrinks the walkable area further", region.GetBounds().Size.X is > 15.4f and < 16.6f, "size " + region.GetBounds().Size);
        // Bounds.
        nm.AgentRadius = 0.5f;
        nm.FilterBakingAabb = new Aabb(new Vector3(-3, -1, -3), new Vector3(6, 3, 6));
        region.BakeNavigationMesh(false);
        T.Check("filter_baking_aabb clips the bake", region.GetBounds().Size.X is > 5f and < 6.3f);
        nm.FilterBakingAabb = default;
        // Parsed geometry: mesh instances only reads no colliders.
        nm.GeometryParsedGeometryType = NavigationMesh.ParsedGeometryTypeEnum.MeshInstances;
        region.BakeNavigationMesh(false);
        T.Check("parsing mesh instances only finds no geometry under a collision-only floor: the mesh is emptied", nm.GetPolygonCount() == 0);
        // Collision mask.
        nm.GeometryParsedGeometryType = NavigationMesh.ParsedGeometryTypeEnum.StaticColliders;
        nm.GeometryCollisionMask = 2;
        region.BakeNavigationMesh(false);
        T.Check("geometry_collision_mask skips bodies on other layers", nm.GetPolygonCount() == 0);
        nm.GeometryCollisionMask = 0xFFFFFFFF;
        region.BakeNavigationMesh(false);
        T.Check("and the full mask finds them again", nm.GetPolygonCount() > 0);
        // Source geometry mode: a group.
        var elsewhere = new StaticBody3D { Name = "Elsewhere", Position = new Vector3(100, 0, 0) };
        elsewhere.AddChild(new CollisionShape3D { Shape = new BoxShape3D { Size = new Vector3(10, 0.2f, 10) }, Position = new Vector3(0, -0.1f, 0) });
        SceneTree.Instance.Root.AddChild(elsewhere);
        elsewhere.AddToGroup("navigation_mesh_source_group");
        nm.GeometrySourceGeometryMode = NavigationMesh.SourceGeometryModeEnum.GroupsExplicit;
        region.BakeNavigationMesh(false);
        T.Check("groups_explicit reads the grouped nodes anywhere in the tree, in the region's space", nm.GetPolygonCount() > 0 && region.GetBounds().Position.X > 90f);
        // The signal, the server's own API.
        T.Check("a bake of source geometry with no triangles clears the mesh, runs the callback and reports no error (Godot's rule)",
                Do(() =>
                {
                    var stale = Quad(0, 0, 1, 1);
                    int called = 0, errors = T.ErrorCount();
                    NavigationServer3D.BakeFromSourceGeometryData(stale, new NavigationMeshSourceGeometryData3D(), Callable.From(() => called++));
                    return stale.GetPolygonCount() == 0 && called == 1 && T.ErrorCount() == errors;
                }));
    }

    private static bool Do(Func<bool> f) => f();

    private static void BakeCarving()
    {
        var nm = new NavigationMesh { AgentRadius = 0.5f };
        var region = FloorRegion(20f, nm);
        region.BakeNavigationMesh(false);
        var a = new Vector3(-8, 0, 0);
        var b = new Vector3(8, 0, 0);
        float open = Len(Path(a, b));
        T.CheckNum("with nothing in the way the path is straight (16 m)", 16f, open, 0.5f);

        var obstacle = new NavigationObstacle3D { Radius = 1.5f, Height = 2f, AffectNavigationMesh = true, CarveNavigationMesh = true, Position = new Vector3(0, 0, 0) };
        region.AddChild(obstacle);
        region.BakeNavigationMesh(false);
        Vector3[] p = Path(a, b);
        T.Check("the carved mesh has more polygons than before and a hole at the obstacle",
                nm.GetPolygonCount() > 1 && NavigationServer3D.MapGetClosestPoint(Map, new Vector3(0, 0, 0)).DistanceTo(Vector3.Zero) > 1.5f);
        T.Check("the path goes round it", p.Length >= 3 && Len(p) > open + 0.3f && p[^1].DistanceTo(b) < 0.4f);
        float minDist = float.MaxValue;
        for (int i = 1; i < p.Length; i++)
        {
            Vector3 s = p[i - 1], e = p[i], d = e - s;
            float t = d.LengthSquared() > 1e-9f ? Math.Clamp(-(s.X * d.X + s.Z * d.Z) / (d.X * d.X + d.Z * d.Z), 0f, 1f) : 0f;
            minDist = MathF.Min(minDist, new Vector3(s.X + d.X * t, 0, s.Z + d.Z * t).Length());
        }
        T.Check("and keeps the obstacle's radius clear (at least 1.5 m from its centre)", minDist >= 1.45f);

        obstacle.Radius = 0f;
        obstacle.Vertices = new[] { new Vector3(-0.5f, 0, -3), new Vector3(0.5f, 0, -3), new Vector3(0.5f, 0, 3), new Vector3(-0.5f, 0, 3) };
        region.BakeNavigationMesh(false);
        p = Path(new Vector3(-8, 0, 0), new Vector3(8, 0, 0));
        T.Check("a polygon obstacle (a wall 1 x 6 m) is carved too: the path goes round its end", p.Length >= 3 && p.Any(v => MathF.Abs(v.Z) >= 3f));

        obstacle.AffectNavigationMesh = false;
        obstacle.CarveNavigationMesh = false;
        region.BakeNavigationMesh(false);
        T.CheckNum("with affect_navigation_mesh off nothing is carved", open, Len(Path(a, b)), 0.1f);
    }

    private static void BakeFloorsAndWalls()
    {
        var nm = new NavigationMesh { AgentRadius = 0.5f, AgentHeight = 1.5f, AgentMaxClimb = 0.3f, AgentMaxSlope = 45f };
        var region = FloorRegion(20f, nm);
        // A wall 0.4 m thick across the floor with a 2 m gap at the middle.
        var wall = new StaticBody3D { Name = "Wall" };
        wall.AddChild(new CollisionShape3D { Shape = new BoxShape3D { Size = new Vector3(0.4f, 3f, 8f) }, Position = new Vector3(0, 1.5f, -5.5f) });
        wall.AddChild(new CollisionShape3D { Shape = new BoxShape3D { Size = new Vector3(0.4f, 3f, 8f) }, Position = new Vector3(0, 1.5f, 5.5f) });
        region.AddChild(wall);
        region.BakeNavigationMesh(false);
        Vector3[] p = Path(new Vector3(-6, 0, -6), new Vector3(6, 0, -6));
        T.Check("a wall with a gap: the path goes through the gap, not through the wall", p.Length >= 3 && p.Any(v => MathF.Abs(v.Z) < 1.2f && MathF.Abs(v.X) < 1f));
        T.Check("and never crosses the wall's line outside the gap", Enumerable.Range(1, p.Length - 1).All(i => (p[i - 1].X < 0) == (p[i].X < 0) || MathF.Abs((p[i - 1].Z + p[i].Z) * 0.5f) < 2f));

        // A kerb under agent_max_climb is stepped over; a step over it is not. (A strip across the whole floor, its top at the given height.)
        bool CrossesStrip(float top)
        {
            NavigationServer3D.ResetForTests();
            T.Reset();
            var n2 = new NavigationMesh { AgentRadius = 0.5f, AgentHeight = 1.5f, AgentMaxClimb = 0.3f };
            var r2 = FloorRegion(20f, n2);
            var strip = new StaticBody3D { Name = "Strip" };
            strip.AddChild(new CollisionShape3D { Shape = new BoxShape3D { Size = new Vector3(20f, top, 4f) }, Position = new Vector3(0, top * 0.5f, 0) });
            r2.AddChild(strip);
            r2.BakeNavigationMesh(false);
            return n2.GetPolygonCount() > 0 && Arrives(Query(new Vector3(-6, 0, -6), new Vector3(6, 0, 6)), new Vector3(6, 0, 6));
        }
        T.Check("a kerb of 0.2 m (under agent_max_climb 0.3) is stepped over: the floor on both sides is joined", CrossesStrip(0.2f));
        T.Check("a step of 1.1 m is not climbed: the two sides are not joined across it", !CrossesStrip(1.1f));
    }

    private static void BakeAsync()
    {
        var nm = new NavigationMesh();
        var region = FloorRegion(20f, nm);
        int finished = 0;
        region.BakeFinished += () => finished++;
        region.BakeNavigationMesh(true);
        T.Check("is_baking is true while the worker runs (or it already finished)", region.IsBaking() || finished > 0 || nm.GetPolygonCount() > 0);
        var clock = System.Diagnostics.Stopwatch.StartNew();
        while (finished == 0 && clock.ElapsedMilliseconds < 20000) { T.Frame(1.0 / 60.0, 1); Thread.Sleep(5); }
        T.Check("bake_finished arrives from a frame pump, once, with the mesh filled", finished == 1 && nm.GetPolygonCount() > 0 && !region.IsBaking());
        T.Check("a second bake while one runs is refused with an error", Do(() =>
        {
            region.BakeNavigationMesh(true);
            region.BakeNavigationMesh(true);
            bool refused = T.ErrorCount("already running") > 0 || !region.IsBaking();
            var c2 = System.Diagnostics.Stopwatch.StartNew();
            while (region.IsBaking() && c2.ElapsedMilliseconds < 20000) { T.Frame(1.0 / 60.0, 1); Thread.Sleep(5); }
            return refused;
        }));
        var empty = new NavigationRegion3D();
        SceneTree.Instance.Root.AddChild(empty);
        empty.BakeNavigationMesh();
        T.Check("a region with no NavigationMesh reports an error", T.ErrorCount("no NavigationMesh") > 0);
    }

    // ---- agents ----

    private sealed class Walker
    {
        public Node3D Body = new() { Name = "Walker" };
        public NavigationAgent3D Agent = new() { PathDesiredDistance = 0.3f, TargetDesiredDistance = 0.3f, PathMaxDistance = 5f };
        public readonly List<string> Events = new();
        public readonly List<Vector3> Waypoints = new();
        public readonly List<int> WaypointTypes = new();

        public Walker(Vector3 at)
        {
            Body.Position = at;
            Body.AddChild(Agent);
            Agent.PathChanged += () => Events.Add("path_changed");
            Agent.WaypointReached += d => { Events.Add("waypoint_reached"); Waypoints.Add(d["position"].AsVector3()); WaypointTypes.Add((int)d["type"].AsInt64()); };
            Agent.LinkReached += _ => Events.Add("link_reached");
            Agent.TargetReached += () => Events.Add("target_reached");
            Agent.NavigationFinished += () => Events.Add("navigation_finished");
            SceneTree.Instance.Root.AddChild(Body);
        }

        /// <summary>Walks to the agent's next position at <paramref name="speed"/> m/s until it is finished or the frames run out. Returns the frames used.</summary>
        public int Walk(float speed = 4f, int maxFrames = 3000)
        {
            const double dt = 1.0 / 60.0;
            for (int i = 0; i < maxFrames; i++)
            {
                Vector3 next = Agent.GetNextPathPosition();
                Vector3 d = next - Body.Position;
                float step = speed * (float)dt;
                Body.Position = d.Length() <= step ? next : Body.Position + d.Normalized() * step;
                T.Frame(dt, 1);
                if (Agent.IsNavigationFinished()) return i + 1;
            }
            return maxFrames;
        }
    }

    private static NavigationRegion3D LCorridor()
    {
        var m = new NavigationMesh
        {
            Vertices = new[] { new Vector3(0, 0, 0), new Vector3(10, 0, 0), new Vector3(10, 0, 2), new Vector3(0, 0, 2), new Vector3(8, 0, 2), new Vector3(8, 0, 10), new Vector3(10, 0, 10) },
        };
        m.AddPolygon(new[] { 0, 1, 2, 3 });
        m.AddPolygon(new[] { 4, 2, 6, 5 });
        return Region(m);
    }

    private static void AgentSignals()
    {
        LCorridor();
        var w = new Walker(new Vector3(1, 0, 1));
        T.Check("before a target the agent is finished and heads nowhere", w.Agent.IsNavigationFinished() && w.Agent.GetNextPathPosition() == w.Body.Position && w.Events.Count == 0);
        w.Agent.TargetPosition = new Vector3(9, 0, 9);
        T.Check("setting a target asks for a path but computes nothing until it is read", w.Events.Count == 0);
        Vector3 first = w.Agent.GetNextPathPosition();
        T.Check("the first read finds the path: path_changed, and the start waypoint is reached at once", w.Events.Count >= 1 && w.Events[0] == "path_changed" && w.Waypoints.Count == 1);
        T.CheckVec("the next position is the corner", new Vector3(8, 0, 2), first, 0.05f);
        T.CheckVec("the current path is start, corner, goal", new Vector3(8, 0, 2), w.Agent.GetCurrentNavigationPath()[1], 0.05f);
        T.Check("the path has three points and the index moved to 1", w.Agent.GetCurrentNavigationPath().Length == 3 && w.Agent.GetCurrentNavigationPathIndex() == 1);
        T.CheckNum("get_path_length is the whole path", 2f * MathF.Sqrt(50f), w.Agent.GetPathLength(), 0.05f);
        T.CheckVec("the final position is the target", new Vector3(9, 0, 9), w.Agent.GetFinalPosition(), 0.05f);
        T.Check("the target is reachable", w.Agent.IsTargetReachable());
        int frames = w.Walk();
        T.Check("the walker arrives well before the frame cap", frames < 1000 && w.Agent.IsNavigationFinished());
        T.CheckSeq("signals in order: path_changed, three waypoints (start, corner, goal), target_reached, navigation_finished",
                   new[] { "path_changed", "waypoint_reached", "waypoint_reached", "waypoint_reached", "target_reached", "navigation_finished" }, w.Events);
        T.CheckVec("the second waypoint is the corner", new Vector3(8, 0, 2), w.Waypoints[1], 0.05f);
        T.Check("the waypoints are region points", w.WaypointTypes.All(t => t == (int)PathSegmentType.Region));
        T.Check("is_target_reached and a tiny distance to the target", w.Agent.IsTargetReached() && w.Agent.DistanceToTarget() < 0.31f);
        int events = w.Events.Count;
        T.Frames(10, 1.0 / 60.0, 1);
        T.Check("a finished agent stays quiet", w.Events.Count == events);

        // Retarget: starts again with the same sequence.
        w.Events.Clear(); w.Waypoints.Clear();
        w.Agent.TargetPosition = new Vector3(1, 0, 1);
        w.Walk();
        T.Check("setting the target again repaths and finishes again (reached its own start)", w.Events[0] == "path_changed" && w.Events.Contains("target_reached") && w.Events[^1] == "navigation_finished");
        T.Check("is_target_reached was reset by the new target and is set again", w.Agent.IsTargetReached());
    }

    private static void AgentUnreachable()
    {
        LCorridor();
        var w = new Walker(new Vector3(1, 0, 1));
        w.Agent.TargetPosition = new Vector3(5, 0, 8);   // off the mesh: the L has nothing there
        w.Agent.GetNextPathPosition();
        T.Check("an off-mesh target is not reachable (the path ends on the mesh, short of it)", !w.Agent.IsTargetReachable() && w.Agent.GetFinalPosition().DistanceTo(new Vector3(5, 0, 8)) > 1f);
        w.Walk();
        T.Check("the agent still walks to the end of the path and finishes", w.Agent.IsNavigationFinished() && w.Events.Contains("navigation_finished"));
        T.Check("but target_reached never fires", !w.Events.Contains("target_reached") && !w.Agent.IsTargetReached());
        T.Check("the distance to the target is the real one", w.Agent.DistanceToTarget() > 1f);

        // A bigger desired distance makes the same target count as reached.
        var w2 = new Walker(new Vector3(1, 0, 1)) { };
        w2.Agent.TargetDesiredDistance = 6f;
        w2.Agent.TargetPosition = new Vector3(5, 0, 8);
        w2.Walk();
        T.Check("target_desired_distance 6 m: the end of the path is within it, so the target is reached", w2.Agent.IsTargetReached());

        // path_height_offset shifts the points the agent reports.
        var w3 = new Walker(new Vector3(1, -0.5f, 1));   // the mover sits one offset below the mesh
        w3.Agent.PathHeightOffset = 0.5f;
        w3.Agent.TargetPosition = new Vector3(9, 0, 9);
        Vector3 next = w3.Agent.GetNextPathPosition();
        T.CheckVec("path_height_offset lowers the reported position by the offset", new Vector3(8, -0.5f, 2), next, 0.05f);
    }

    private static void AgentLinks()
    {
        Region(Quad(0, 0, 10, 10));
        Region(Quad(30, 0, 40, 10));
        var link = new NavigationLink3D { StartPosition = new Vector3(9, 0, 5), EndPosition = new Vector3(31, 0, 5) };
        SceneTree.Instance.Root.AddChild(link);
        var w = new Walker(new Vector3(1, 0, 5));
        w.Agent.TargetPosition = new Vector3(39, 0, 5);
        w.Walk(8f);
        T.Check("the walk finishes at the far island", w.Agent.IsNavigationFinished() && w.Agent.IsTargetReached());
        T.Check("link_reached fires for the link's start and its end", w.Events.Count(e => e == "link_reached") == 2);
        T.Check("each link waypoint is reported as type link", w.WaypointTypes.Count(t => t == (int)PathSegmentType.Link) == 2);
        int firstLink = w.Events.IndexOf("link_reached");
        T.Check("link_reached follows the waypoint_reached of the same point", firstLink > 0 && w.Events[firstLink - 1] == "waypoint_reached");

        // path_max_distance: pushed off the path, the agent repaths.
        var w2 = new Walker(new Vector3(1, 0, 1));
        w2.Agent.PathMaxDistance = 1f;
        w2.Agent.TargetPosition = new Vector3(9, 0, 9);
        w2.Agent.GetNextPathPosition();
        T.Frame(1.0 / 60.0, 1);
        w2.Body.Position = new Vector3(2, 0, 8);
        int before = w2.Events.Count(e => e == "path_changed");
        T.Frame(1.0 / 60.0, 1);
        w2.Agent.GetNextPathPosition();
        T.Check("being further than path_max_distance from the path asks for a new one", w2.Events.Count(e => e == "path_changed") == before + 1);
    }

    private static void AgentAvoidance()
    {
        Walker Make(Vector3 at, float radius = 0.5f, uint layers = 1, uint mask = 1)
        {
            var w = new Walker(at);
            w.Agent.AvoidanceEnabled = true;
            w.Agent.Radius = radius;
            w.Agent.MaxSpeed = 3f;
            w.Agent.NeighborDistance = 10f;
            w.Agent.TimeHorizonAgents = 2f;
            w.Agent.AvoidanceLayers = layers;
            w.Agent.AvoidanceMask = mask;
            return w;
        }

        {
            var solo = Make(new Vector3(0, 0, 0));
            Vector3? safe = null;
            solo.Agent.VelocityComputed += v => safe = v;
            solo.Agent.Velocity = new Vector3(2, 0, 0);
            T.Check("nothing is answered before the navigation step", safe == null);
            T.Frame(1.0 / 60.0, 1);
            T.Check("alone: velocity_computed hands back the wanted velocity", safe != null && T.Near(safe.Value, new Vector3(2, 0, 0), 0.01f));
            safe = null;
            solo.Agent.Velocity = new Vector3(2, 1, 0);
            T.Frame(1.0 / 60.0, 1);
            T.Check("avoidance is in the ground plane: the wanted vertical part is kept (keep_y_velocity)", safe != null && T.Near(safe.Value, new Vector3(2, 1, 0), 0.01f));
            solo.Agent.KeepYVelocity = false;
            solo.Agent.Velocity = new Vector3(2, 1, 0);
            T.Frame(1.0 / 60.0, 1);
            T.Check("and keep_y_velocity = false drops it", safe != null && T.Near(safe.Value, new Vector3(2, 0, 0), 0.01f));
        }
        NavigationServer3D.ResetForTests();
        T.Reset();

        var a = Make(new Vector3(-3, 0, 0));
        var b = Make(new Vector3(3, 0, 0));
        Vector3 safeA = default, safeB = default;
        bool gotA = false, gotB = false;
        a.Agent.VelocityComputed += v => { safeA = v; gotA = true; };
        b.Agent.VelocityComputed += v => { safeB = v; gotB = true; };
        float minDist = float.MaxValue, maxSpeed = 0f;
        bool deviated = false;
        const double dt = 1.0 / 60.0;
        for (int i = 0; i < 240; i++)
        {
            gotA = gotB = false;
            a.Agent.Velocity = new Vector3(2, 0, 0);
            b.Agent.Velocity = new Vector3(-2, 0, 0);
            T.Frame(dt, 1);
            if (!gotA || !gotB) { T.Check("both agents answer every frame", false, "frame " + i); return; }
            maxSpeed = MathF.Max(maxSpeed, MathF.Max(safeA.Length(), safeB.Length()));
            if (MathF.Abs(safeA.Z) > 0.05f || MathF.Abs(safeB.Z) > 0.05f) deviated = true;
            a.Body.Position += safeA * (float)dt;
            b.Body.Position += safeB * (float)dt;
            minDist = MathF.Min(minDist, a.Body.Position.DistanceTo(b.Body.Position));
        }
        T.Check("head-on agents steer sideways", deviated);
        T.Check("and stay apart (at least 0.9 of the 1 m sum of radii)", minDist >= 0.9f, "closest " + minDist);
        T.Check("neither speeds past max_speed", maxSpeed <= 3.01f, "fastest " + maxSpeed);
        T.Check("and they pass each other", a.Body.Position.X > b.Body.Position.X);

        // Layers and masks: an agent whose mask does not include the other's layer ignores it.
        NavigationServer3D.ResetForTests();
        T.Reset();
        var c = Make(new Vector3(-1, 0, 0), 0.5f, 1, 2);
        var d2 = Make(new Vector3(1, 0, 0), 0.5f, 1, 2);
        Vector3 safeC = default;
        c.Agent.VelocityComputed += v => safeC = v;
        c.Agent.Velocity = new Vector3(2, 0, 0);
        d2.Agent.Velocity = new Vector3(-2, 0, 0);
        T.Frame(dt, 1);
        T.Check("avoidance_mask 2 does not see an agent on layer 1: the wanted velocity comes back", T.Near(safeC, new Vector3(2, 0, 0), 0.01f));
        c.Agent.SetAvoidanceMaskValue(1, true);
        c.Agent.Velocity = new Vector3(2, 0, 0);
        d2.Agent.Velocity = new Vector3(-2, 0, 0);
        T.Frame(dt, 1);
        T.Check("set_avoidance_mask_value(1, true) makes it see it: the velocity changes", !T.Near(safeC, new Vector3(2, 0, 0), 0.01f));
        T.Check("the layer values read back", c.Agent.GetAvoidanceMaskValue(1) && c.Agent.GetAvoidanceMaskValue(2) && c.Agent.GetAvoidanceLayerValue(1) && !c.Agent.GetAvoidanceLayerValue(2));
    }

    private static void ObstacleAvoidance()
    {
        var w = new Walker(new Vector3(-4, 0, 0.2f));
        w.Agent.AvoidanceEnabled = true;
        w.Agent.Radius = 0.5f;
        w.Agent.MaxSpeed = 3f;
        w.Agent.TimeHorizonObstacles = 3f;
        Vector3 safe = default;
        w.Agent.VelocityComputed += v => safe = v;
        var ob = new NavigationObstacle3D { Radius = 1f, Height = 2f };
        SceneTree.Instance.Root.AddChild(ob);
        T.Check("the obstacle has a handle on the map and Godot's defaults", ob.GetRid().IsValid && ob.AvoidanceEnabled && ob.Height == 2f && NavigationServer3D.MapGetObstacles(Map).Count == 1);
        w.Body.Position = new Vector3(-2.2f, 0, 0.2f);
        w.Agent.Velocity = new Vector3(2, 0, 0);
        T.Frame(1.0 / 60.0, 1);
        T.Check("a round obstacle ahead is steered around (the safe velocity differs from the wanted one)", !T.Near(safe, new Vector3(2, 0, 0), 0.01f) && safe.Length() <= 3.01f);
        ob.Paused = true;
        w.Agent.Velocity = new Vector3(2, 0, 0);
        T.Frame(1.0 / 60.0, 1);
        T.Check("a paused obstacle is ignored", T.Near(safe, new Vector3(2, 0, 0), 0.01f));
        ob.Paused = false;
        ob.AvoidanceLayers = 2;
        w.Agent.Velocity = new Vector3(2, 0, 0);
        T.Frame(1.0 / 60.0, 1);
        T.Check("avoidance_layers the agent's mask does not include hide the obstacle", T.Near(safe, new Vector3(2, 0, 0), 0.01f));
        T.Check("layer values", !ob.GetAvoidanceLayerValue(1) && ob.GetAvoidanceLayerValue(2));
        ob.SetAvoidanceLayerValue(1, true);
        w.Agent.Velocity = new Vector3(2, 0, 0);
        T.Frame(1.0 / 60.0, 1);
        T.Check("and set_avoidance_layer_value(1) brings it back", !T.Near(safe, new Vector3(2, 0, 0), 0.01f));
        ob.QueueFree();
        T.Frame(1.0 / 60.0, 1);
        T.Check("a freed obstacle leaves the map", NavigationServer3D.MapGetObstacles(Map).Count == 0);
    }

    private static void ServerMisc()
    {
        Rid map = NavigationServer3D.MapCreate();
        T.Check("a new map is active, with Godot's default cell size 0.25 m and an edge margin of 0.25 m", NavigationServer3D.MapIsActive(map) && T.Near(NavigationServer3D.MapGetCellSize(map), 0.25f) && T.Near(NavigationServer3D.MapGetEdgeConnectionMargin(map), 0.25f));
        NavigationServer3D.MapSetCellSize(map, 0.5f);
        NavigationServer3D.MapSetLinkConnectionRadius(map, 2f);
        T.Check("map parameters round-trip in metres", T.Near(NavigationServer3D.MapGetCellSize(map), 0.5f) && T.Near(NavigationServer3D.MapGetLinkConnectionRadius(map), 2f));
        Rid region = NavigationServer3D.RegionCreate();
        NavigationServer3D.RegionSetMap(region, map);
        NavigationServer3D.RegionSetNavigationMesh(region, Quad(0, 0, 10, 10));
        NavigationServer3D.RegionSetEnterCost(region, 2f);
        NavigationServer3D.RegionSetNavigationLayers(region, 6);
        NavigationServer3D.RegionSetOwnerId(region, 42);
        T.Check("a region made through the server: map, enabled, layers, costs, owner", NavigationServer3D.RegionGetMap(region) == map && NavigationServer3D.RegionGetEnabled(region) &&
                NavigationServer3D.RegionGetNavigationLayers(region) == 6 && NavigationServer3D.RegionGetEnterCost(region) == 2f && NavigationServer3D.RegionGetTravelCost(region) == 1f && NavigationServer3D.RegionGetOwnerId(region) == 42);
        T.Check("map_get_regions lists it", NavigationServer3D.MapGetRegions(map).Count == 1 && NavigationServer3D.MapGetRegions(map)[0] == region);
        T.Check("region_owns_point", NavigationServer3D.RegionOwnsPoint(region, new Vector3(5, 0, 5)) && !NavigationServer3D.RegionOwnsPoint(region, new Vector3(50, 0, 5)));
        uint iteration = NavigationServer3D.MapGetIterationId(map);
        NavigationServer3D.RegionSetEnabled(region, false);
        NavigationServer3D.MapForceUpdate(map);
        T.Check("map_force_update rebuilds and bumps the iteration id", NavigationServer3D.MapGetIterationId(map) != iteration);
        int changes = 0;
        NavigationServer3D.MapChanged += _ => changes++;
        NavigationServer3D.RegionSetEnabled(region, true);
        NavigationServer3D.MapForceUpdate(map);
        T.Check("and map_changed fires for it", changes == 1);
        Vector3 rp = NavigationServer3D.MapGetRandomPoint(map, 2, true);
        T.Check("a random point lies on the mesh", rp.X is >= -0.05f and <= 10.05f && rp.Z is >= -0.05f and <= 10.05f && MathF.Abs(rp.Y) < 0.05f);
        T.Check("none on a layer no region has", NavigationServer3D.MapGetRandomPoint(map, 1, true) == Vector3.Zero);
        Vector3 cs = NavigationServer3D.MapGetClosestPointToSegment(map, new Vector3(15, 1, 5), new Vector3(15, 1, 8));
        T.CheckVec("the mesh point closest to a segment", new Vector3(10, 0, 5), cs, 0.05f);
        T.Check("paths use the map's layers argument", NavigationServer3D.MapGetPath(map, new Vector3(1, 0, 1), new Vector3(9, 0, 9), true, 1).Length == 0 && NavigationServer3D.MapGetPath(map, new Vector3(1, 0, 1), new Vector3(9, 0, 9), true, 2).Length == 2);
        NavigationServer3D.FreeRid(region);
        T.Check("a freed region's handle goes stale: getters answer neutral values and the map is empty", !NavigationServer3D.RegionGetEnabled(region) && NavigationServer3D.RegionGetMap(region) == default && NavigationServer3D.MapGetRegions(map).Count == 0);
        NavigationServer3D.FreeRid(map);
        T.Check("a freed map is gone", !NavigationServer3D.MapIsActive(map) && NavigationServer3D.MapGetPath(map, Vector3.Zero, Vector3.One, true).Length == 0);

        Rid agent = NavigationServer3D.AgentCreate();
        NavigationServer3D.AgentSetMap(agent, Map);
        NavigationServer3D.AgentSetAvoidanceEnabled(agent, true);
        NavigationServer3D.AgentSetRadius(agent, 0.75f);
        T.Check("agents are in the map's list", NavigationServer3D.MapGetAgents(Map).Contains(agent) && NavigationServer3D.AgentGetAvoidanceEnabled(agent));
        T.Check("avoidance callbacks are registered and removed", Do(() =>
        {
            Vector3 got = default;
            NavigationServer3D.AgentSetAvoidanceCallback(agent, Callable.From((Vector3 v) => got = v));
            bool has = NavigationServer3D.AgentHasAvoidanceCallback(agent);
            NavigationServer3D.AgentSetAvoidanceCallback(agent, default);
            return has && !NavigationServer3D.AgentHasAvoidanceCallback(agent);
        }));
    }

    // ---- A* ----

    private static AStar3D Diamond()
    {
        var g = new AStar3D();
        g.AddPoint(1, new Vector3(0, 0, 0));
        g.AddPoint(2, new Vector3(1, 0, 0));
        g.AddPoint(3, new Vector3(2, 0, 0));
        g.AddPoint(4, new Vector3(1, 1, 0));
        g.AddPoint(5, new Vector3(10, 0, 0));
        g.ConnectPoints(1, 2); g.ConnectPoints(2, 3); g.ConnectPoints(1, 4); g.ConnectPoints(4, 3);
        return g;
    }

    private static void AStar3DPaths()
    {
        AStar3D g = Diamond();
        T.Check("points, ids and counts", g.GetPointCount() == 5 && g.GetPointIds().SequenceEqual(new long[] { 1, 2, 3, 4, 5 }) && g.HasPoint(4) && !g.HasPoint(9));
        T.Check("the smallest unused id", g.GetAvailablePointId() == 0);
        T.Check("connections, ascending, both directions", g.GetPointConnections(1).SequenceEqual(new long[] { 2, 4 }) && g.GetPointConnections(3).SequenceEqual(new long[] { 2, 4 }));
        T.Check("the straight way beats the detour", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 2, 3 }));
        Vector3[] pp = g.GetPointPath(1, 3);
        T.Check("and the point path carries the positions", pp.Length == 3 && pp[1] == new Vector3(1, 0, 0) && pp[2] == new Vector3(2, 0, 0));
        T.Check("from == to is the point itself", g.GetIdPath(2, 2).SequenceEqual(new long[] { 2 }));
        T.Check("a missing point gives nothing (and an error)", g.GetIdPath(1, 99).Length == 0 && T.ErrorCount("doesn't exist") > 0);
        T.Check("positions and weights", g.GetPointPosition(4) == new Vector3(1, 1, 0) && g.GetPointWeightScale(2) == 1f);

        T.CheckVec("the segment point closest to (0.5, 0.5, 0) is on the diagonal 1-4", new Vector3(0.5f, 0.5f, 0), g.GetClosestPositionInSegment(new Vector3(0.5f, 0.5f, 0)), 1e-4f);
        T.Check("closest point", g.GetClosestPoint(new Vector3(1.2f, 0.9f, 0)) == 4 && g.GetClosestPoint(new Vector3(9, 0, 0)) == 5);

        g.SetPointWeightScale(2, 5f);
        T.Check("a weight of 5 on the middle point sends the path round: 1-4-3 (1.41 + 1.41 < 5 + 1)", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 4, 3 }));
        g.SetPointDisabled(4);
        T.Check("with the detour disabled it goes through the heavy point again", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 2, 3 }) && g.IsPointDisabled(4));
        T.Check("a disabled point is skipped by get_closest_point unless asked for", g.GetClosestPoint(new Vector3(1.2f, 0.9f, 0)) == 2 && g.GetClosestPoint(new Vector3(1.2f, 0.9f, 0), true) == 4);
        g.SetPointDisabled(2);
        T.Check("with both disabled there is no way", g.GetIdPath(1, 3).Length == 0);
        g.SetPointDisabled(2, false); g.SetPointDisabled(4, false); g.SetPointWeightScale(2, 1f);

        g.ConnectPoints(3, 5, false);
        T.Check("a one-way segment goes forward", g.GetIdPath(1, 5).SequenceEqual(new long[] { 1, 2, 3, 5 }));
        T.Check("and not back", g.GetIdPath(5, 1).Length == 0 && g.ArePointsConnected(3, 5, false) && !g.ArePointsConnected(5, 3, false));
        T.Check("are_points_connected(bidirectional = true) accepts either direction", g.ArePointsConnected(5, 3));
        T.Check("the one-way segment is an outgoing connection of 3 only", g.GetPointConnections(3).Contains(5) && !g.GetPointConnections(5).Contains(3));
        g.DisconnectPoints(3, 5, false);
        T.Check("disconnect_points cuts it", g.GetIdPath(1, 5).Length == 0 && !g.ArePointsConnected(3, 5));
        T.Check("a partial path ends at the reachable point closest to the target", g.GetIdPath(1, 5, true).SequenceEqual(new long[] { 1, 2, 3 }) && g.GetPointPath(1, 5, true)[^1] == new Vector3(2, 0, 0));
        g.RemovePoint(2);
        T.Check("removing a point removes its segments: the path takes the detour", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 4, 3 }) && g.GetPointCount() == 4 && !g.GetPointConnections(1).Contains(2));
        g.AddPoint(0, Vector3.Zero);
        T.Check("get_available_point_id is the smallest unused id", g.GetAvailablePointId() == 2);
        g.RemovePoint(0);
        g.SetPointPosition(4, new Vector3(1, 0, 3));
        T.CheckNum("set_point_position changes the cost: the point path is longer", 2f * MathF.Sqrt(10f), Len(g.GetPointPath(1, 3)), 1e-4f);
        g.Clear();
        T.Check("clear", g.GetPointCount() == 0 && g.GetIdPath(1, 3).Length == 0);

        var errors = T.ErrorCount();
        g.AddPoint(-1, Vector3.Zero);
        g.ConnectPoints(7, 7);
        T.Check("a negative id and a self connection are refused with errors", !g.HasPoint(-1) && T.ErrorCount() >= errors + 2);
        g.AddPoint(7, new Vector3(1, 1, 1));
        g.AddPoint(7, new Vector3(2, 2, 2), 3f);
        T.Check("add_point on an existing id updates it", g.GetPointCount() == 1 && g.GetPointPosition(7) == new Vector3(2, 2, 2) && g.GetPointWeightScale(7) == 3f);
    }

    private sealed class DearEdge : AStar3D
    {
        public int Computes, Estimates;
        public override float _ComputeCost(long fromId, long toId)
        {
            Computes++;
            return fromId == 1 && toId == 2 ? 100f : GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));
        }

        public override float _EstimateCost(long fromId, long toId)
        {
            Estimates++;
            return GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));
        }
    }

    private sealed class ThrowingCost : AStar3D
    {
        public override float _ComputeCost(long fromId, long toId) => throw new InvalidOperationException("boom");
    }

    private static void AStar3DOverrides()
    {
        var g = new DearEdge();
        g.AddPoint(1, new Vector3(0, 0, 0)); g.AddPoint(2, new Vector3(1, 0, 0)); g.AddPoint(3, new Vector3(2, 0, 0)); g.AddPoint(4, new Vector3(1, 1, 0));
        g.ConnectPoints(1, 2); g.ConnectPoints(2, 3); g.ConnectPoints(1, 4); g.ConnectPoints(4, 3);
        long[] path = g.GetIdPath(1, 3);
        T.Check("_compute_cost is called and obeyed: the 1 -> 2 step costs 100, so the path takes the detour", path.SequenceEqual(new long[] { 1, 4, 3 }) && g.Computes > 0);
        T.Check("_estimate_cost is called too", g.Estimates > 0);
        T.Check("the opposite direction is not penalised (the cost is per direction)", g.GetIdPath(3, 1).SequenceEqual(new long[] { 3, 2, 1 }));

        var plain = Diamond();
        T.Check("a class that overrides nothing runs the engine's own search", plain.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 2, 3 }));

        var bad = new ThrowingCost();
        bad.AddPoint(1, Vector3.Zero); bad.AddPoint(2, new Vector3(1, 0, 0)); bad.ConnectPoints(1, 2);
        T.Check("a cost function that throws is reported and does not take the process down", bad.GetIdPath(1, 2).SequenceEqual(new long[] { 1, 2 }) && T.ErrorCount("boom") > 0);
    }

    private static void AStar2DPaths()
    {
        var g = new AStar2D();
        g.AddPoint(1, new Vector2(0, 0));
        g.AddPoint(2, new Vector2(1, 0));
        g.AddPoint(3, new Vector2(2, 0));
        g.AddPoint(4, new Vector2(1, 1));
        g.AddPoint(5, new Vector2(10, 10), 2f);
        g.ConnectPoints(1, 2); g.ConnectPoints(2, 3); g.ConnectPoints(1, 4); g.ConnectPoints(4, 3);
        T.Check("the straight way", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 2, 3 }));
        Vector2[] pp = g.GetPointPath(1, 3);
        T.Check("the point path is in 2D", pp.Length == 3 && pp[1] == new Vector2(1, 0));
        g.SetPointWeightScale(2, 4f);
        T.Check("weights", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 4, 3 }) && g.GetPointWeightScale(5) == 2f);
        T.Check("closest point and segment point", g.GetClosestPoint(new Vector2(9, 9)) == 5 && g.GetClosestPositionInSegment(new Vector2(0.5f, 0.2f)) == new Vector2(0.5f, 0f));
        g.SetPointDisabled(4);
        g.SetPointDisabled(2);
        T.Check("disabled points", g.GetIdPath(1, 3).Length == 0 && g.GetIdPath(1, 3, true).Length == 1);
        T.Check("point count and ids", g.GetPointCount() == 5 && g.GetPointIds().Length == 5 && g.HasPoint(5));

        var over = new Dear2D();
        over.AddPoint(1, new Vector2(0, 0)); over.AddPoint(2, new Vector2(1, 0)); over.AddPoint(3, new Vector2(2, 0)); over.AddPoint(4, new Vector2(1, 1));
        over.ConnectPoints(1, 2); over.ConnectPoints(2, 3); over.ConnectPoints(1, 4); over.ConnectPoints(4, 3);
        T.Check("a script's _compute_cost on an AStar2D is called", over.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 4, 3 }) && over.Computes > 0);
    }

    private sealed class Dear2D : AStar2D
    {
        public int Computes;
        public override float _ComputeCost(long fromId, long toId)
        {
            Computes++;
            return toId == 2 ? 50f : GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));
        }
    }

    // ---- AStarGrid2D ----

    private static AStarGrid2D Grid(int w, int h, AStarGrid2D.DiagonalModeEnum diag = AStarGrid2D.DiagonalModeEnum.Always)
    {
        var g = new AStarGrid2D { Region = new Rect2I(0, 0, w, h), DiagonalMode = diag };
        g.Update();
        return g;
    }

    private static float CellLen(IList<Vector2I> cells)
    {
        float s = 0;
        for (int i = 1; i < cells.Count; i++) s += cells[i].DistanceTo(cells[i - 1]);
        return s;
    }

    private static void GridBasics()
    {
        var g = new AStarGrid2D();
        T.Check("defaults: dirty, cell size 1, diagonal always, Euclidean heuristics, no jumping, square cells",
                g.IsDirty() && g.CellSize == Vector2.One && g.Offset == Vector2.Zero && g.DiagonalMode == AStarGrid2D.DiagonalModeEnum.Always &&
                g.DefaultComputeHeuristic == AStarGrid2D.HeuristicEnum.Euclidean && g.DefaultEstimateHeuristic == AStarGrid2D.HeuristicEnum.Euclidean && !g.JumpingEnabled && g.CellShape == AStarGrid2D.CellShapeEnum.Square);
        g.Region = new Rect2I(2, 3, 4, 5);
        T.Check("size follows the region", g.Size == new Vector2I(4, 5));
        T.Check("a path before update() is refused with an error", g.GetIdPath(new Vector2I(2, 3), new Vector2I(3, 3)).Count == 0 && T.ErrorCount("update()") > 0);
        g.Update();
        T.Check("update() makes it clean", !g.IsDirty());
        T.Check("bounds", g.IsInBounds(2, 3) && g.IsInBounds(5, 7) && !g.IsInBounds(6, 3) && !g.IsInBounds(1, 3) && g.IsInBoundsv(new Vector2I(3, 4)));
        g.SetPointSolid(new Vector2I(3, 3));
        g.SetPointWeightScale(new Vector2I(4, 4), 3f);
        T.Check("solid and weight read back", g.IsPointSolid(new Vector2I(3, 3)) && !g.IsPointSolid(new Vector2I(4, 3)) && g.GetPointWeightScale(new Vector2I(4, 4)) == 3f && g.GetPointWeightScale(new Vector2I(2, 3)) == 1f);
        g.FillSolidRegion(new Rect2I(2, 6, 4, 2));
        g.FillWeightScaleRegion(new Rect2I(4, 3, 2, 1), 2.5f);
        T.Check("regions fill solid and weight (clipped to the grid)", g.IsPointSolid(new Vector2I(5, 7)) && g.IsPointSolid(new Vector2I(2, 6)) && g.GetPointWeightScale(new Vector2I(5, 3)) == 2.5f);
        var data = g.GetPointDataInRegion(new Rect2I(3, 3, 2, 2));
        T.Check("get_point_data_in_region: id, position, solid, weight", data.Count == 4 && data[0]["id"].AsVector2I() == new Vector2I(3, 3) && data[0]["solid"].AsBool() && data[3]["weight_scale"].AsSingle() == 3f);
        g.Update();
        T.Check("update() with no change keeps the cells (Godot returns early when the grid is clean)", g.IsPointSolid(new Vector2I(3, 3)));
        g.CellSize = new Vector2(2, 3);
        T.Check("changing the cell size makes it dirty again", g.IsDirty());
        g.Update();
        T.Check("and update() then clears the cells", !g.IsPointSolid(new Vector2I(3, 3)));

        g.Offset = new Vector2(10, 20);
        g.Update();
        T.Check("point positions are offset + id * cell_size", g.GetPointPosition(new Vector2I(1, 2)) == new Vector2(12, 26));
        Vector2[] pp = g.GetPointPath(new Vector2I(2, 3), new Vector2I(4, 3));
        T.Check("the point path is made of those positions", pp.Length == 3 && pp[0] == new Vector2(14, 29) && pp[2] == new Vector2(18, 29));

        g.CellShape = AStarGrid2D.CellShapeEnum.IsometricRight;
        T.Check("isometric right: offset + half cell + (x + y, y - x) * half cell", g.GetPointPosition(new Vector2I(1, 0)) == new Vector2(10, 20) + new Vector2(1, 1.5f) + new Vector2(1, -1) * new Vector2(1, 1.5f));
        g.CellShape = AStarGrid2D.CellShapeEnum.IsometricDown;
        T.Check("isometric down: offset + half cell + (x - y, x + y) * half cell", g.GetPointPosition(new Vector2I(1, 0)) == new Vector2(10, 20) + new Vector2(1, 1.5f) + new Vector2(1, 1) * new Vector2(1, 1.5f));

        g.Size = new Vector2I(2, 2);
        g.Update();
        T.Check("size keeps the region's position", g.Region == new Rect2I(2, 3, 2, 2));
        T.Check("a cell outside the region is refused with an error", Do(() => { int e = T.ErrorCount(); g.SetPointSolid(new Vector2I(30, 30)); return T.ErrorCount() > e; }));
        g.Clear();
        T.Check("clear() empties the grid and its region (Godot's clear) and leaves it clean", g.Region == default && !g.IsDirty() && !g.IsInBounds(2, 3) && g.CellSize == new Vector2(2, 3));
    }

    private static void GridDiagonals()
    {
        // 2x2: (1,0) solid. The path (0,0) -> (1,1) cuts the corner or goes round.
        foreach (var (mode, expect) in new[]
        {
            (AStarGrid2D.DiagonalModeEnum.Always, 2), (AStarGrid2D.DiagonalModeEnum.Never, 3),
            (AStarGrid2D.DiagonalModeEnum.AtLeastOneWalkable, 2), (AStarGrid2D.DiagonalModeEnum.OnlyIfNoObstacles, 3),
        })
        {
            AStarGrid2D g = Grid(2, 2, mode);
            g.SetPointSolid(new Vector2I(1, 0));
            var p = g.GetIdPath(new Vector2I(0, 0), new Vector2I(1, 1));
            T.Check($"one solid corner, {mode}: {expect} cells", p.Count == expect, "got " + p.Count);
            if (expect == 3) T.Check($"{mode}: via (0,1)", p.Count == 3 && p[1] == new Vector2I(0, 1));
        }
        foreach (var (mode, expect) in new[]
        {
            (AStarGrid2D.DiagonalModeEnum.Always, 2), (AStarGrid2D.DiagonalModeEnum.Never, 0),
            (AStarGrid2D.DiagonalModeEnum.AtLeastOneWalkable, 0), (AStarGrid2D.DiagonalModeEnum.OnlyIfNoObstacles, 0),
        })
        {
            AStarGrid2D g = Grid(2, 2, mode);
            g.SetPointSolid(new Vector2I(1, 0));
            g.SetPointSolid(new Vector2I(0, 1));
            var p = g.GetIdPath(new Vector2I(0, 0), new Vector2I(1, 1));
            T.Check($"both corners solid, {mode}: {(expect == 0 ? "no way" : expect + " cells")}", p.Count == expect, "got " + p.Count);
        }
        AStarGrid2D open = Grid(5, 5, AStarGrid2D.DiagonalModeEnum.Always);
        var diag = open.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 4));
        T.Check("open grid, always: the diagonal, five cells", diag.Count == 5 && diag[2] == new Vector2I(2, 2));
        AStarGrid2D never = Grid(5, 5, AStarGrid2D.DiagonalModeEnum.Never);
        var stairs = never.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 4));
        T.Check("open grid, never: nine cells, every step orthogonal", stairs.Count == 9 && Enumerable.Range(1, 8).All(i => Math.Abs(stairs[i].X - stairs[i - 1].X) + Math.Abs(stairs[i].Y - stairs[i - 1].Y) == 1));
    }

    private sealed class DearCell : AStarGrid2D
    {
        public int Computes;
        public override float _ComputeCost(Vector2I fromId, Vector2I toId)
        {
            Computes++;
            return toId == new Vector2I(1, 1) ? 100f : base._ComputeCost(fromId, toId);
        }
    }

    private static void GridSearch()
    {
        // A wall down x = 2 with the only gap at y = 4.
        AStarGrid2D Walled(AStarGrid2D.DiagonalModeEnum mode)
        {
            AStarGrid2D g = Grid(5, 5, mode);
            for (int y = 0; y < 4; y++) g.SetPointSolid(new Vector2I(2, y));
            return g;
        }

        var never = Walled(AStarGrid2D.DiagonalModeEnum.Never);
        var p = never.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 0));
        T.Check("never: down 4, across 4, up 4: thirteen cells through the gap", p.Count == 13 && p.Contains(new Vector2I(2, 4)) && p[0] == new Vector2I(0, 0) && p[^1] == new Vector2I(4, 0));

        var always = Walled(AStarGrid2D.DiagonalModeEnum.Always);
        p = always.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 0));
        T.Check("always: nine cells, 4 diagonal and 4 straight steps (Euclidean cost 8 + 4 x 0.414 = 9.657)", p.Count == 9 && p.Contains(new Vector2I(2, 4)));
        T.CheckNum("the cost of the path found", 4f + 4f * MathF.Sqrt(2f), CellLen(p), 1e-3f);

        var manhattan = Walled(AStarGrid2D.DiagonalModeEnum.Never);
        manhattan.DefaultComputeHeuristic = AStarGrid2D.HeuristicEnum.Manhattan;
        manhattan.DefaultEstimateHeuristic = AStarGrid2D.HeuristicEnum.Manhattan;
        T.Check("manhattan heuristics give the same shortest walk with orthogonal steps", manhattan.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 0)).Count == 13);
        T.Check("the heuristics read back", manhattan.DefaultComputeHeuristic == AStarGrid2D.HeuristicEnum.Manhattan);
        foreach (AStarGrid2D.HeuristicEnum h in new[] { AStarGrid2D.HeuristicEnum.Euclidean, AStarGrid2D.HeuristicEnum.Octile, AStarGrid2D.HeuristicEnum.Chebyshev })
        {
            var gh = Walled(AStarGrid2D.DiagonalModeEnum.Always);
            gh.DefaultComputeHeuristic = h;
            gh.DefaultEstimateHeuristic = h;
            var ph = gh.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 0));
            T.Check($"{h} heuristics find a way through the gap in a handful of cells", ph.Count is >= 9 and <= 13 && ph[0] == new Vector2I(0, 0) && ph[^1] == new Vector2I(4, 0) && ph.Contains(new Vector2I(2, 4)));
        }

        // A weight of 10 on the middle cell sends the path round it.
        var wg = Grid(3, 3, AStarGrid2D.DiagonalModeEnum.Never);
        wg.SetPointWeightScale(new Vector2I(1, 0), 10f);
        var wp = wg.GetIdPath(new Vector2I(0, 0), new Vector2I(2, 0));
        T.Check("a heavy cell is walked round: (0,0) (0,1) (1,1) (2,1) (2,0)", wp.Count == 5 && wp[1] == new Vector2I(0, 1) && wp[2] == new Vector2I(1, 1) && wp[3] == new Vector2I(2, 1));

        // Jumping.
        AStarGrid2D jg = Grid(8, 8, AStarGrid2D.DiagonalModeEnum.Never);
        jg.JumpingEnabled = true;
        var jp = jg.GetIdPath(new Vector2I(0, 0), new Vector2I(7, 7));
        T.Check("jump point search: the same endpoints with fewer points than the walk (a straight run is one leg)", jg.JumpingEnabled && jp.Count >= 2 && jp.Count < 15 && jp[0] == new Vector2I(0, 0) && jp[^1] == new Vector2I(7, 7));
        AStarGrid2D jw = Walled(AStarGrid2D.DiagonalModeEnum.Never);
        jw.JumpingEnabled = true;
        var jwp = jw.GetIdPath(new Vector2I(0, 0), new Vector2I(4, 0));
        bool crossesGap = false, axisAligned = true;
        int legSum = 0;
        for (int i = 1; i < jwp.Count; i++)
        {
            Vector2I a0 = jwp[i - 1], a1 = jwp[i];
            axisAligned &= a0.X == a1.X || a0.Y == a1.Y;
            legSum += Math.Abs(a1.X - a0.X) + Math.Abs(a1.Y - a0.Y);
            if (a0.Y == 4 && a1.Y == 4 && Math.Min(a0.X, a1.X) <= 2 && Math.Max(a0.X, a1.X) >= 2) crossesGap = true;
        }
        T.Check("and it still finds the way through the gap: a few axis-aligned legs (down, across the gap, up) as long as the walk, thirteen cells' worth",
                jwp.Count is >= 3 and < 13 && jwp[0] == new Vector2I(0, 0) && jwp[^1] == new Vector2I(4, 0) && crossesGap && axisAligned && legSum == 12, string.Join(" ", jwp));

        // Partial paths.
        var blocked = Grid(5, 3, AStarGrid2D.DiagonalModeEnum.Never);
        for (int y = 0; y < 3; y++) blocked.SetPointSolid(new Vector2I(2, y));
        T.Check("a full wall: no path", blocked.GetIdPath(new Vector2I(0, 1), new Vector2I(4, 1)).Count == 0);
        var partial = blocked.GetIdPath(new Vector2I(0, 1), new Vector2I(4, 1), true);
        T.Check("allow_partial_path ends at the reachable cell closest to the target: (1,1)", partial.Count >= 2 && partial[^1] == new Vector2I(1, 1));
        T.Check("a solid start or end has no path", blocked.GetIdPath(new Vector2I(2, 1), new Vector2I(4, 1)).Count == 0 && blocked.GetIdPath(new Vector2I(0, 1), new Vector2I(2, 1)).Count == 0);
        T.Check("a path from a cell to itself is that cell", blocked.GetIdPath(new Vector2I(0, 1), new Vector2I(0, 1)).Count == 1);
        T.Check("out-of-bounds ends are refused with an error", Do(() => { int e = T.ErrorCount(); var r = blocked.GetIdPath(new Vector2I(0, 1), new Vector2I(40, 1)); return r.Count == 0 && T.ErrorCount() > e; }));

        // A script's cost.
        var dc = new DearCell { Region = new Rect2I(0, 0, 3, 3), DiagonalMode = AStarGrid2D.DiagonalModeEnum.Always };
        dc.Update();
        var dp = dc.GetIdPath(new Vector2I(0, 0), new Vector2I(2, 2));
        T.Check("_compute_cost makes the middle cell dear: the path avoids (1,1) although the diagonal is shortest", dc.Computes > 0 && !dp.Contains(new Vector2I(1, 1)) && dp[0] == new Vector2I(0, 0) && dp[^1] == new Vector2I(2, 2));
        var plain = Grid(3, 3);
        T.Check("without the override the diagonal goes through the middle", plain.GetIdPath(new Vector2I(0, 0), new Vector2I(2, 2)).Contains(new Vector2I(1, 1)));
    }

    // ---- the converter ----

    private static void ConverterOutput()
    {
        string exeName = "AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : "");
        string? bin = FindBin(exeName);
        if (bin == null) { Console.WriteLine("  SKIP: build/bin/AverAssetC was not found"); return; }
        string outDir = IO.Path.Combine(IO.Path.GetTempPath(), "aver-fabric-nav-conv-" + System.Environment.ProcessId);
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(IO.Path.Combine(bin, exeName)) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(IO.Path.Combine(T.DataDir(), "NavGame")); psi.ArgumentList.Add(outDir); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter runs on the NavGame fixture", proc.ExitCode == 0, "exit " + proc.ExitCode);
                if (proc.ExitCode != 0) return;
            }
            ResourceLoader.ContentRootOverride = IO.Path.Combine(outDir, "NavGame", "Content");
            Node main = ResourceLoader.Load<PackedScene>("res://main.tscn")!.Instantiate();
            SceneTree.Instance.Root.AddChild(main);
            T.Check("the level instantiates without an error", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

            var ground = main.GetNode<NavigationRegion3D>("Ground");
            var upper = main.GetNode<NavigationRegion3D>("Upper");
            var fresh = main.GetNode<NavigationRegion3D>("Fresh");
            T.Check("the regions have their meshes, layers and costs", ground.NavigationMesh != null && ground.NavigationMesh.GetPolygonCount() == 1 && ground.NavigationLayers == 3 && ground.EnterCost == 2f && ground.TravelCost == 1.5f && !ground.UseEdgeConnections);
            T.Check("the mesh's baking parameters arrive", T.Near(ground.NavigationMesh!.CellSize, 0.25f) && T.Near(ground.NavigationMesh.AgentRadius, 0.4f) && T.Near(ground.NavigationMesh.AgentHeight, 1.8f) && ground.NavigationMesh.SamplePartitionType == NavigationMesh.SamplePartitionTypeEnum.Monotone);
            T.Check("the external .tres mesh arrives too", upper.NavigationMesh != null && upper.NavigationMesh.GetPolygonCount() == 1 && upper.NavigationMesh.GetVertices().Length == 4 && upper.NavigationLayers == 1);
            T.CheckVec("the region's place goes through the axis map and back (Godot 10, 3, 0)", new Vector3(10, 3, 0), upper.Position, 1e-3f);
            T.Check("the unbaked region is disabled as the scene says", !fresh.Enabled && fresh.BakeOnReady);

            var link = main.GetNode<NavigationLink3D>("Stair");
            T.Check("the link keeps its ends, direction, layers and costs", !link.Bidirectional && link.NavigationLayers == 2 && link.EnterCost == 5f && link.TravelCost == 2f &&
                    T.Near(link.StartPosition, new Vector3(4, 0, 0)) && T.Near(link.EndPosition, new Vector3(8, 3, 0)));
            var crate = main.GetNode<NavigationObstacle3D>("Crate");
            T.Check("the round obstacle keeps radius, height, carving and layers", crate.Radius == 0.5f && crate.Height == 2f && crate.AffectNavigationMesh && crate.CarveNavigationMesh && crate.AvoidanceLayers == 5 && crate.Use3DAvoidance);
            var wall = main.GetNode<NavigationObstacle3D>("Wall");
            T.Check("the wall obstacle keeps its vertices", wall.Vertices.Length == 3 && wall.Vertices[1] == new Vector3(1, 0, -1) && wall.Height == 2.5f);
            var agent = main.GetNode<NavigationAgent3D>("Walker/NavigationAgent3D");
            T.Check("the agent keeps its properties", agent.TargetPosition == new Vector3(4, 0, 4) && T.Near(agent.PathDesiredDistance, 0.5f) && T.Near(agent.TargetDesiredDistance, 0.75f) && T.Near(agent.PathMaxDistance, 2.5f) &&
                    agent.NavigationLayers == 3 && agent.PathPostprocessing == PathPostProcessing.Edgecentered && agent.PathMetadataFlags == (PathMetadataFlags.IncludeTypes | PathMetadataFlags.IncludeRids | PathMetadataFlags.IncludeOriginalIds) &&
                    agent.SimplifyPath && T.Near(agent.SimplifyEpsilon, 0.1f));
            T.Check("and its avoidance properties", agent.AvoidanceEnabled && T.Near(agent.Radius, 0.4f) && T.Near(agent.Height, 1.8f) && T.Near(agent.MaxSpeed, 5f) && T.Near(agent.NeighborDistance, 8f) && agent.MaxNeighbors == 6 &&
                    T.Near(agent.TimeHorizonAgents, 1.5f) && agent.AvoidanceLayers == 3 && agent.AvoidanceMask == 3 && agent.Use3DAvoidance && agent.DebugEnabled);

            // The two floors are joined only by the one-way stair link on layer 2.
            T.Frame(1.0 / 60.0, 1);
            var start = new Vector3(-4, 0, -4);
            var goal = new Vector3(10, 3, 0);
            NavigationPathQueryResult3D q = Query(start, goal, 3);
            T.Check("with layers 1|2 the path climbs the stair link to the upper floor", Arrives(q, goal) && q.PathTypes.Count(t => t == (int)PathSegmentType.Link) == 2);
            T.Check("with layer 1 alone the link (layer 2) is closed", !Query(start, goal, 1).ReachedTarget);
            T.Check("and the way back down is closed (one-way)", !Query(goal, start, 3).ReachedTarget);
            T.CheckNum("the stair's link ends are where the Godot scene put them (4, 0, 0) -> (8, 3, 0)", 4f, q.Path[q.PathTypes.ToList().IndexOf((int)PathSegmentType.Link)].X, 0.05f);

            // The agent walks it.
            var body = main.GetNode<Node3D>("Walker");
            agent.TargetPosition = goal;
            agent.PathDesiredDistance = 0.3f;
            agent.TargetDesiredDistance = 0.3f;
            agent.NavigationLayers = 3;
            agent.AvoidanceEnabled = false;
            int links = 0;
            agent.LinkReached += _ => links++;
            for (int i = 0; i < 3000 && !agent.IsNavigationFinished(); i++)
            {
                Vector3 next = agent.GetNextPathPosition();
                Vector3 d = next - body.Position;
                body.Position = d.Length() <= 0.1f ? next : body.Position + d.Normalized() * 0.1f;
                T.Frame(1.0 / 60.0, 1);
            }
            T.Check("the converted agent walks the converted level up the stair to the goal", agent.IsTargetReached() && links == 2 && body.Position.DistanceTo(goal) < 0.5f);

            // The unbaked region bakes itself from its collision floor (it is disabled, so the mesh is not in the map).
            var clock = System.Diagnostics.Stopwatch.StartNew();
            while (fresh.NavigationMesh!.GetPolygonCount() == 0 && clock.ElapsedMilliseconds < 20000) { T.Frame(1.0 / 60.0, 1); Thread.Sleep(5); }
            T.Check("bake_on_ready baked the unbaked mesh from the level's collision floor", fresh.NavigationMesh.GetPolygonCount() > 0 && fresh.GetBounds().Size.X is > 3f and < 8.1f);
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            try { Directory.Delete(outDir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }
}
