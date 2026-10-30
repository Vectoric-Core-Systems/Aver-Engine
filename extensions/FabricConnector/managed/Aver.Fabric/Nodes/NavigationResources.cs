// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// NavigationMesh (the polygons of a region and the parameters it is baked with), NavigationMeshSourceGeometryData3D (the triangles a bake reads) and
// NavigationMeshGenerator (parsing the scene tree for triangles and baking them with the engine's baker). Godot's space and units. N1.

namespace Aver.Fabric;

/// <summary>A navigation mesh: polygons over a vertex list (Godot's NavigationMesh) and the settings it is baked with. Vertices are in the space of the
/// node that holds the mesh; polygons are index lists, clockwise seen from above as Godot writes them (the engine accepts either winding).</summary>
public class NavigationMesh : Resource
{
    public enum SamplePartitionTypeEnum { Watershed = 0, Monotone = 1, Layers = 2, Max = 3 }
    public enum ParsedGeometryTypeEnum { MeshInstances = 0, StaticColliders = 1, Both = 2, Max = 3 }
    public enum SourceGeometryModeEnum { RootNodeChildren = 0, GroupsWithChildren = 1, GroupsExplicit = 2, Max = 3 }

    private Vector3[] _vertices = System.Array.Empty<Vector3>();
    private List<int[]> _polygons = new();

    // ---- baking parameters, Godot's names and defaults (metres) ----
    public SamplePartitionTypeEnum SamplePartitionType { get; set; } = SamplePartitionTypeEnum.Watershed;
    public ParsedGeometryTypeEnum GeometryParsedGeometryType { get; set; } = ParsedGeometryTypeEnum.MeshInstances;
    public uint GeometryCollisionMask { get; set; } = 0xFFFFFFFF;
    public SourceGeometryModeEnum GeometrySourceGeometryMode { get; set; } = SourceGeometryModeEnum.RootNodeChildren;
    public StringName GeometrySourceGroupName { get; set; } = "navigation_mesh_source_group";
    public float CellSize { get; set; } = 0.25f;
    public float CellHeight { get; set; } = 0.25f;
    public float BorderSize { get; set; }
    public float AgentHeight { get; set; } = 1.5f;
    public float AgentRadius { get; set; } = 0.5f;
    public float AgentMaxClimb { get; set; } = 0.25f;
    public float AgentMaxSlope { get; set; } = 45f;
    public float RegionMinSize { get; set; } = 2f;
    public float RegionMergeSize { get; set; } = 20f;
    public float EdgeMaxLength { get; set; }
    public float EdgeMaxError { get; set; } = 1.3f;
    public int VerticesPerPolygon { get; set; } = 6;
    public float DetailSampleDistance { get; set; } = 6f;
    public float DetailSampleMaxError { get; set; } = 1f;
    public bool FilterLowHangingObstacles { get; set; }
    public bool FilterLedgeSpans { get; set; }
    public bool FilterWalkableLowHeightSpans { get; set; }
    public Aabb FilterBakingAabb { get; set; }
    public Vector3 FilterBakingAabbOffset { get; set; }

    // ---- the mesh ----

    /// <summary>The vertices (Godot's <c>vertices</c> property, a PackedVector3Array).</summary>
    public Vector3[] Vertices { get => _vertices; set { _vertices = value ?? System.Array.Empty<Vector3>(); } }

    /// <summary>The polygons (Godot's <c>polygons</c> property: an array of PackedInt32Array).</summary>
    public Collections.Array Polygons
    {
        get
        {
            var a = new Collections.Array();
            foreach (int[] p in _polygons) a.Add(Variant.From(p));
            return a;
        }
        set
        {
            _polygons.Clear();
            foreach (Variant v in value) _polygons.Add(v.AsInt32Array());
        }
    }

    public void SetVertices(Vector3[] vertices) => Vertices = vertices;
    public Vector3[] GetVertices() => _vertices;
    public void AddPolygon(int[] polygon) => _polygons.Add((int[])polygon.Clone());
    public int GetPolygonCount() => _polygons.Count;
    public int[] GetPolygon(int idx) => idx >= 0 && idx < _polygons.Count ? (int[])_polygons[idx].Clone() : System.Array.Empty<int>();
    public void ClearPolygons() => _polygons.Clear();

    /// <summary>Removes the vertices and the polygons.</summary>
    public void Clear()
    {
        _vertices = System.Array.Empty<Vector3>();
        _polygons.Clear();
    }

    /// <summary>Makes a polygon of every triangle of a mesh (Godot's create_from_mesh).</summary>
    public void CreateFromMesh(Mesh mesh)
    {
        Clear();
        if (!NavGeometry.TryMeshFaces(mesh, out List<Vector3>? faces) || faces == null) return;
        var verts = new List<Vector3>(faces.Count);
        for (int i = 0; i + 2 < faces.Count; i += 3)
        {
            int at = verts.Count;
            verts.Add(faces[i]); verts.Add(faces[i + 1]); verts.Add(faces[i + 2]);
            _polygons.Add(new[] { at, at + 1, at + 2 });
        }
        _vertices = verts.ToArray();
    }

    public bool GetCollisionMaskValue(int layerNumber) => (GeometryCollisionMask & (1u << (layerNumber - 1))) != 0;

    public void SetCollisionMaskValue(int layerNumber, bool value) =>
        GeometryCollisionMask = value ? GeometryCollisionMask | (1u << (layerNumber - 1)) : GeometryCollisionMask & ~(1u << (layerNumber - 1));

    public override Resource Duplicate(bool subresources = false)
    {
        var copy = (NavigationMesh)base.Duplicate(subresources);
        copy._vertices = (Vector3[])_vertices.Clone();
        copy._polygons = _polygons.Select(p => (int[])p.Clone()).ToList();
        return copy;
    }
}

/// <summary>The triangles a bake reads (Godot's NavigationMeshSourceGeometryData3D): vertices as flat xyz floats and triangle indices, both in the space of the
/// root node the geometry was parsed against, plus obstructions that discard geometry.</summary>
public class NavigationMeshSourceGeometryData3D : Resource
{
    private readonly List<float> _vertices = new();
    private readonly List<int> _indices = new();
    private readonly List<Collections.Dictionary> _obstructions = new();

    public float[] Vertices { get => _vertices.ToArray(); set { _vertices.Clear(); _vertices.AddRange(value); } }
    public int[] Indices { get => _indices.ToArray(); set { _indices.Clear(); _indices.AddRange(value); } }

    /// <summary>Obstructions as Godot stores them: dictionaries with "vertices" (Vector3[]), "elevation", "height" and "carve".</summary>
    public Collections.Array ProjectedObstructions
    {
        get { var a = new Collections.Array(); foreach (var d in _obstructions) a.Add(d); return a; }
        set { _obstructions.Clear(); foreach (Variant v in value) _obstructions.Add(v.AsGodotDictionary()); }
    }

    public void SetVertices(float[] vertices) => Vertices = vertices;
    public float[] GetVertices() => Vertices;
    public void SetIndices(int[] indices) => Indices = indices;
    public int[] GetIndices() => Indices;
    public bool HasData() => _vertices.Count > 0 && _indices.Count > 0;

    public void Clear()
    {
        _vertices.Clear();
        _indices.Clear();
        _obstructions.Clear();
    }

    /// <summary>Appends triangles given as vertex triples (<paramref name="faces"/>, three per triangle) transformed by <paramref name="xform"/>.</summary>
    public void AddFaces(Vector3[] faces, Transform3D xform)
    {
        if (faces.Length % 3 != 0) { GD.PushError("NavigationMeshSourceGeometryData3D.add_faces: the face array must hold triples of vertices"); return; }
        int baseIndex = _vertices.Count / 3;
        for (int i = 0; i < faces.Length; i++)
        {
            Vector3 v = xform * faces[i];
            _vertices.Add(v.X); _vertices.Add(v.Y); _vertices.Add(v.Z);
            _indices.Add(baseIndex + i);
        }
    }

    /// <summary>Appends a vertex array and the triangle indices into it.</summary>
    public void AddMeshArray(Collections.Array meshArray, Transform3D xform)
    {
        if (meshArray.Count <= (int)Mesh.ArrayType.Index) { GD.PushError("NavigationMeshSourceGeometryData3D.add_mesh_array: the array is too short"); return; }
        Vector3[] verts = meshArray[(int)Mesh.ArrayType.Vertex].AsVector3Array();
        Variant idx = meshArray[(int)Mesh.ArrayType.Index];
        int[] indices = idx.IsNil ? Enumerable.Range(0, verts.Length).ToArray() : idx.AsInt32Array();
        AppendTriangles(verts, indices, xform);
    }

    public void AddMesh(Mesh mesh, Transform3D xform)
    {
        if (!NavGeometry.TryMeshFaces(mesh, out List<Vector3>? faces) || faces == null) { GD.PushWarning("NavigationMeshSourceGeometryData3D.add_mesh: this mesh class has no triangles to read"); return; }
        AddFaces(faces.ToArray(), xform);
    }

    public void AppendArrays(float[] vertices, int[] indices)
    {
        int baseIndex = _vertices.Count / 3;
        _vertices.AddRange(vertices);
        foreach (int i in indices) _indices.Add(baseIndex + i);
    }

    public void Merge(NavigationMeshSourceGeometryData3D other) { AppendArrays(other.Vertices, other.Indices); _obstructions.AddRange(other._obstructions); }

    public Aabb GetBounds()
    {
        if (_vertices.Count < 3) return default;
        Vector3 lo = new(float.MaxValue, float.MaxValue, float.MaxValue), hi = new(float.MinValue, float.MinValue, float.MinValue);
        for (int i = 0; i + 2 < _vertices.Count; i += 3)
        {
            var v = new Vector3(_vertices[i], _vertices[i + 1], _vertices[i + 2]);
            lo = new Vector3(Math.Min(lo.X, v.X), Math.Min(lo.Y, v.Y), Math.Min(lo.Z, v.Z));
            hi = new Vector3(Math.Max(hi.X, v.X), Math.Max(hi.Y, v.Y), Math.Max(hi.Z, v.Z));
        }
        return new Aabb(lo, hi - lo);
    }

    public void AddProjectedObstruction(Vector3[] vertices, float elevation, float height, bool carve)
    {
        if (vertices.Length < 3) { GD.PushError("NavigationMeshSourceGeometryData3D.add_projected_obstruction: an obstruction needs at least three vertices"); return; }
        var d = new Collections.Dictionary();
        d["vertices"] = Variant.From(vertices);
        d["elevation"] = elevation;
        d["height"] = height;
        d["carve"] = carve;
        _obstructions.Add(d);
    }

    public void ClearProjectedObstructions() => _obstructions.Clear();

    private void AppendTriangles(Vector3[] verts, int[] indices, Transform3D xform)
    {
        int baseIndex = _vertices.Count / 3;
        foreach (Vector3 v in verts)
        {
            Vector3 t = xform * v;
            _vertices.Add(t.X); _vertices.Add(t.Y); _vertices.Add(t.Z);
        }
        foreach (int i in indices) _indices.Add(baseIndex + i);
    }

    internal IReadOnlyList<Collections.Dictionary> Obstructions => _obstructions;
}

/// <summary>Triangles out of the resource classes Fabric has (the sources a navigation bake reads).</summary>
internal static class NavGeometry
{
    private static bool s_warned;

    /// <summary>Triangle vertex triples of a mesh: BoxMesh and ArrayMesh exactly. Any other class has no readable triangles.</summary>
    internal static bool TryMeshFaces(Mesh mesh, out List<Vector3>? faces)
    {
        faces = null;
        switch (mesh)
        {
            case BoxMesh box:
                faces = BoxFaces(box.Size);
                return true;
            case ArrayMesh am:
                faces = new List<Vector3>();
                foreach (MeshSurface s in am.Surfaces)
                {
                    int[] idx = s.Indices ?? Enumerable.Range(0, s.Vertices.Length).ToArray();
                    if (s.Primitive == Mesh.PrimitiveType.Triangles)
                    {
                        for (int i = 0; i + 2 < idx.Length; i += 3) { faces.Add(s.Vertices[idx[i]]); faces.Add(s.Vertices[idx[i + 1]]); faces.Add(s.Vertices[idx[i + 2]]); }
                    }
                    else if (s.Primitive == Mesh.PrimitiveType.TriangleStrip)
                    {
                        for (int i = 0; i + 2 < idx.Length; i++)
                        {
                            bool odd = (i & 1) != 0;
                            faces.Add(s.Vertices[idx[i]]); faces.Add(s.Vertices[idx[odd ? i + 2 : i + 1]]); faces.Add(s.Vertices[idx[odd ? i + 1 : i + 2]]);
                        }
                    }
                }
                return true;
            default:
                return false;
        }
    }

    /// <summary>Twelve triangles of a box centred on the origin, front faces outward (counter-clockwise seen from outside).</summary>
    internal static List<Vector3> BoxFaces(Vector3 size)
    {
        Vector3 h = size * 0.5f;
        var f = new List<Vector3>();
        void Quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d) { f.Add(a); f.Add(b); f.Add(c); f.Add(a); f.Add(c); f.Add(d); }
        // each quad is listed counter-clockwise as seen from outside
        Quad(new Vector3(-h.X, h.Y, h.Z), new Vector3(h.X, h.Y, h.Z), new Vector3(h.X, h.Y, -h.Z), new Vector3(-h.X, h.Y, -h.Z));        // +Y (the top)
        Quad(new Vector3(-h.X, -h.Y, -h.Z), new Vector3(h.X, -h.Y, -h.Z), new Vector3(h.X, -h.Y, h.Z), new Vector3(-h.X, -h.Y, h.Z));    // -Y
        Quad(new Vector3(h.X, -h.Y, h.Z), new Vector3(h.X, -h.Y, -h.Z), new Vector3(h.X, h.Y, -h.Z), new Vector3(h.X, h.Y, h.Z));       // +X
        Quad(new Vector3(-h.X, -h.Y, -h.Z), new Vector3(-h.X, -h.Y, h.Z), new Vector3(-h.X, h.Y, h.Z), new Vector3(-h.X, h.Y, -h.Z));    // -X
        Quad(new Vector3(-h.X, -h.Y, h.Z), new Vector3(h.X, -h.Y, h.Z), new Vector3(h.X, h.Y, h.Z), new Vector3(-h.X, h.Y, h.Z));       // +Z
        Quad(new Vector3(h.X, -h.Y, -h.Z), new Vector3(-h.X, -h.Y, -h.Z), new Vector3(-h.X, h.Y, -h.Z), new Vector3(h.X, h.Y, -h.Z));    // -Z
        return f;
    }

    /// <summary>Triangles of a collision shape: box and concave exactly; the rest by the shape's bounding box (a warning once).</summary>
    internal static List<Vector3>? ShapeFaces(Shape3D shape)
    {
        switch (shape)
        {
            case BoxShape3D box: return BoxFaces(box.Size);
            case ConcavePolygonShape3D concave: return concave.Data.Length >= 3 ? concave.Data.Take(concave.Data.Length / 3 * 3).ToList() : null;
            case ConvexPolygonShape3D convex when convex.Points.Length > 0:
                return Bounds(convex.Points, "ConvexPolygonShape3D");
            case SphereShape3D sphere:
                return Bounds(new[] { new Vector3(-sphere.Radius, -sphere.Radius, -sphere.Radius), new Vector3(sphere.Radius, sphere.Radius, sphere.Radius) }, "SphereShape3D");
            case CapsuleShape3D cap:
                return Bounds(new[] { new Vector3(-cap.Radius, -cap.Height * 0.5f, -cap.Radius), new Vector3(cap.Radius, cap.Height * 0.5f, cap.Radius) }, "CapsuleShape3D");
            case CylinderShape3D cyl:
                return Bounds(new[] { new Vector3(-cyl.Radius, -cyl.Height * 0.5f, -cyl.Radius), new Vector3(cyl.Radius, cyl.Height * 0.5f, cyl.Radius) }, "CylinderShape3D");
            default:
                return null;
        }
    }

    private static List<Vector3> Bounds(Vector3[] points, string name)
    {
        if (!s_warned)
        {
            s_warned = true;
            GD.PushWarning($"Navigation baking: {name} is read as its bounding box (only BoxShape3D and ConcavePolygonShape3D give exact triangles).");
        }
        Vector3 lo = points[0], hi = points[0];
        foreach (Vector3 p in points)
        {
            lo = new Vector3(Math.Min(lo.X, p.X), Math.Min(lo.Y, p.Y), Math.Min(lo.Z, p.Z));
            hi = new Vector3(Math.Max(hi.X, p.X), Math.Max(hi.Y, p.Y), Math.Max(hi.Z, p.Z));
        }
        var faces = BoxFaces(hi - lo);
        Vector3 c = (lo + hi) * 0.5f;
        for (int i = 0; i < faces.Count; i++) faces[i] += c;
        return faces;
    }

    internal static void ResetWarnings() => s_warned = false;
}

/// <summary>Parses the scene tree for triangles and bakes navigation meshes (Godot's NavigationMeshGenerator, and the work NavigationServer3D's parse and
/// bake calls do). The bake itself is the engine's (heightfield rasteriser and rectangle cover, <c>aver/synapse/NavMeshBake.hpp</c>).</summary>
public static class NavigationMeshGenerator
{
    /// <summary>Parses <paramref name="rootNode"/> and bakes <paramref name="navigationMesh"/> in one call.</summary>
    public static void Bake(NavigationMesh navigationMesh, Node rootNode)
    {
        var data = new NavigationMeshSourceGeometryData3D();
        Parse(navigationMesh, data, rootNode, default);
        BakeNow(navigationMesh, data, default);
    }

    /// <summary>Removes the polygons of the mesh.</summary>
    public static void Clear(NavigationMesh navigationMesh) { navigationMesh.Clear(); navigationMesh.EmitChanged(); }

    // ---- parsing ----

    internal static void Parse(NavigationMesh nm, NavigationMeshSourceGeometryData3D data, Node root, Callable callback)
    {
        Transform3D toRoot = root is Node3D r3 && r3.IsInsideTree() ? r3.GlobalTransform.AffineInverse() : Transform3D.Identity;
        bool meshes = nm.GeometryParsedGeometryType != NavigationMesh.ParsedGeometryTypeEnum.StaticColliders;
        bool colliders = nm.GeometryParsedGeometryType != NavigationMesh.ParsedGeometryTypeEnum.MeshInstances;
        switch (nm.GeometrySourceGeometryMode)
        {
            case NavigationMesh.SourceGeometryModeEnum.RootNodeChildren:
                foreach (Node child in root.GetChildren()) Visit(child, true);
                break;
            case NavigationMesh.SourceGeometryModeEnum.GroupsWithChildren:
                foreach (Node n in Node.NodesInGroup(nm.GeometrySourceGroupName.ToString())) Visit(n, true);
                break;
            default:
                foreach (Node n in Node.NodesInGroup(nm.GeometrySourceGroupName.ToString())) Visit(n, false);
                break;
        }
        if (!callback.IsNull) callback.Call();

        void Visit(Node n, bool recurse)
        {
            if (n is Node3D n3 && n3.IsInsideTree())
            {
                Transform3D xf = toRoot * n3.GlobalTransform;
                if (n is MeshInstance3D mi && meshes && mi.Mesh != null) data.AddMesh(mi.Mesh, xf);
                if (n is StaticBody3D body && colliders && (body.CollisionLayer & nm.GeometryCollisionMask) != 0)
                {
                    foreach (Node c in body.GetChildren())
                    {
                        if (c is CollisionShape3D cs && !cs.Disabled && cs.Shape != null && NavGeometry.ShapeFaces(cs.Shape) is { } faces)
                            data.AddFaces(faces.ToArray(), toRoot * cs.GlobalTransform);
                    }
                }
                if (n is NavigationObstacle3D ob && ob.AffectNavigationMesh) AddObstruction(ob, xf);
            }
            if (!recurse) return;
            foreach (Node c in n.GetChildren()) Visit(c, true);
        }

        void AddObstruction(NavigationObstacle3D ob, Transform3D xf)
        {
            Vector3[] poly;
            if (ob.Vertices.Length >= 3)
            {
                poly = ob.Vertices.Select(v => xf * v).ToArray();
            }
            else if (ob.Radius > 0f)
            {
                poly = new Vector3[12];
                for (int i = 0; i < 12; i++)
                {
                    float a = i * MathF.PI * 2f / 12f;
                    poly[i] = xf.Origin + new Vector3(MathF.Cos(a) * ob.Radius, 0f, MathF.Sin(a) * ob.Radius);
                }
            }
            else return;
            data.AddProjectedObstruction(poly, xf.Origin.Y, ob.Height, ob.CarveNavigationMesh || ob.AffectNavigationMesh);
        }
    }

    // ---- baking ----

    /// <summary>The inputs of one bake, in engine arrays: gathered on the calling thread so the bake itself touches no Godot object.</summary>
    private sealed class Job
    {
        public float[] Verts = System.Array.Empty<float>();
        public int[] Tris = System.Array.Empty<int>();
        public float[] Obstacles = System.Array.Empty<float>();
        public int ObstacleCount;
        public float[] Params = new float[(int)NavBake.ParamCount];
    }

    private static Job Gather(NavigationMesh nm, NavigationMeshSourceGeometryData3D data)
    {
        var job = new Job();
        float[] v = data.Vertices;
        int[] idx = data.Indices;
        job.Verts = new float[v.Length];
        for (int i = 0; i + 2 < v.Length; i += 3) NavSpace.ToEngineInto(new Vector3(v[i], v[i + 1], v[i + 2]), job.Verts, i);
        job.Tris = new int[idx.Length - idx.Length % 3];
        for (int i = 0; i + 2 < idx.Length; i += 3)   // the axis map mirrors space: swapping two indices keeps floors facing up
        {
            job.Tris[i] = idx[i]; job.Tris[i + 1] = idx[i + 2]; job.Tris[i + 2] = idx[i + 1];
        }
        var obs = new List<float>();
        foreach (Collections.Dictionary d in data.Obstructions)
        {
            Vector3[] poly = d["vertices"].AsVector3Array();
            float elevation = d["elevation"].AsSingle(), height = d["height"].AsSingle();
            obs.AddRange(new[] { 0f, 0f, 0f, 0f, elevation * FabricSpace.CmPerMetre, Math.Max(height, 0f) * FabricSpace.CmPerMetre, poly.Length });
            foreach (Vector3 pt in poly) obs.AddRange(NavSpace.ToEngine(new Vector3(pt.X, elevation, pt.Z)));
            job.ObstacleCount++;
        }
        job.Obstacles = obs.ToArray();

        float[] p = job.Params;
        const float cm = FabricSpace.CmPerMetre;
        p[(int)NavBake.CellSize] = nm.CellSize * cm; p[(int)NavBake.CellHeight] = nm.CellHeight * cm;
        p[(int)NavBake.AgentHeight] = nm.AgentHeight * cm; p[(int)NavBake.AgentRadius] = nm.AgentRadius * cm;
        p[(int)NavBake.AgentMaxClimb] = nm.AgentMaxClimb * cm; p[(int)NavBake.AgentMaxSlope] = nm.AgentMaxSlope;
        p[(int)NavBake.RegionMinSize] = nm.RegionMinSize; p[(int)NavBake.RegionMergeSize] = nm.RegionMergeSize;
        p[(int)NavBake.EdgeMaxLength] = nm.EdgeMaxLength * cm; p[(int)NavBake.EdgeMaxError] = nm.EdgeMaxError;
        p[(int)NavBake.VertsPerPoly] = nm.VerticesPerPolygon; p[(int)NavBake.DetailDistance] = nm.DetailSampleDistance;
        p[(int)NavBake.DetailMaxError] = nm.DetailSampleMaxError * cm;
        p[(int)NavBake.FilterLowHanging] = nm.FilterLowHangingObstacles ? 1 : 0; p[(int)NavBake.FilterLedge] = nm.FilterLedgeSpans ? 1 : 0;
        p[(int)NavBake.FilterLowHeight] = nm.FilterWalkableLowHeightSpans ? 1 : 0; p[(int)NavBake.BorderSize] = nm.BorderSize * cm;
        if (nm.FilterBakingAabb.Size != Vector3.Zero)
        {
            Aabb b = nm.FilterBakingAabb;
            Vector3 lo = new(float.MaxValue, float.MaxValue, float.MaxValue), hi = new(float.MinValue, float.MinValue, float.MinValue);
            for (int c = 0; c < 8; c++)
            {
                var corner = b.Position + nm.FilterBakingAabbOffset + new Vector3((c & 1) != 0 ? b.Size.X : 0, (c & 2) != 0 ? b.Size.Y : 0, (c & 4) != 0 ? b.Size.Z : 0);
                float[] e = NavSpace.ToEngine(corner);
                lo = new Vector3(Math.Min(lo.X, e[0]), Math.Min(lo.Y, e[1]), Math.Min(lo.Z, e[2]));
                hi = new Vector3(Math.Max(hi.X, e[0]), Math.Max(hi.Y, e[1]), Math.Max(hi.Z, e[2]));
            }
            p[(int)NavBake.UseBounds] = 1;
            p[(int)NavBake.BoundsMin] = lo.X; p[(int)NavBake.BoundsMin + 1] = lo.Y; p[(int)NavBake.BoundsMin + 2] = lo.Z;
            p[(int)NavBake.BoundsMax] = hi.X; p[(int)NavBake.BoundsMax + 1] = hi.Y; p[(int)NavBake.BoundsMax + 2] = hi.Z;
        }
        return job;
    }

    private static int RunNative(Job job, out string error)
    {
        error = "";
        int h = NavNative.aver_nav_bake(job.Verts, job.Verts.Length / 3, job.Tris, job.Tris.Length, job.Obstacles, job.ObstacleCount, job.Params, job.Params.Length);
        if (h == 0) error = NavNative.Str(NavNative.aver_nav_bake_error());
        return h;
    }

    private static void Apply(NavigationMesh nm, int bake)
    {
        var counts = new int[3];
        NavNative.aver_nav_bake_counts(bake, counts);
        var verts = new float[counts[0] * 3];
        var sizes = new int[counts[1]];
        var indices = new int[counts[2]];
        NavNative.aver_nav_bake_get(bake, verts, sizes, indices);
        NavNative.aver_nav_bake_free(bake);
        var v = new Vector3[counts[0]];
        for (int i = 0; i < v.Length; i++) v[i] = NavSpace.FromEngine(verts, i * 3);
        nm.Clear();
        nm.Vertices = v;
        int at = 0;
        foreach (int s in sizes)
        {
            nm.AddPolygon(indices.AsSpan(at, s).ToArray());
            at += s;
        }
        nm.EmitChanged();
    }

    /// <summary>Godot's rule for source geometry with no triangles: the mesh is cleared, <c>changed</c> is emitted and the callback runs; no error.</summary>
    private static void EmptyBake(NavigationMesh nm, Callable callback)
    {
        nm.Clear();
        nm.EmitChanged();
        if (!callback.IsNull) callback.Call();
    }

    internal static void BakeNow(NavigationMesh nm, NavigationMeshSourceGeometryData3D data, Callable callback)
    {
        if (!NavNative.Available) { GD.PushError("Navigation baking needs Aver.Nav.Abi beside the executable"); return; }
        if (!data.HasData()) { EmptyBake(nm, callback); return; }
        int h = RunNative(Gather(nm, data), out string error);
        if (h == 0) { GD.PushError("Navigation mesh baking failed: " + error); return; }
        Apply(nm, h);
        if (!callback.IsNull) callback.Call();
    }

    internal static void BakeAsync(NavigationMesh nm, NavigationMeshSourceGeometryData3D data, Callable callback)
    {
        if (!NavNative.Available) { GD.PushError("Navigation baking needs Aver.Nav.Abi beside the executable"); return; }
        if (!data.HasData()) { EmptyBake(nm, callback); return; }
        Job job = Gather(nm, data);
        NavigationServer3D.BakeStarted(nm);
        Task.Run(() =>
        {
            int h = RunNative(job, out string error);
            NavigationServer3D.PostCompletion(() =>
            {
                NavigationServer3D.BakeEnded(nm);
                if (h == 0) GD.PushError("Navigation mesh baking failed: " + error);
                else Apply(nm, h);
                if (!callback.IsNull) callback.Call();
            });
        });
    }
}
