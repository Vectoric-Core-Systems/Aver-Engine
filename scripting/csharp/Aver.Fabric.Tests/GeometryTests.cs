// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GeometryTests: runtime mesh building and the geometry nodes (G4 R7), against a recording backend (nothing here touches the native framework). ArrayMesh in Godot's array
// layout and what the engine is handed, SurfaceTool normals, tangents and indices, ImmediateMesh, MeshDataTool, MultiMesh's table and its engine form, MeshLibrary and
// GridMap's coordinates, the 24 orientations and the packed cells, and the CSG, Sprite3D and Label3D classes.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class GeometryTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("geometry: the classes and Godot's defaults (G4 R7)", Classes);
        T.Run("geometry: ArrayMesh in Godot's layout, and what the engine is handed (G4 R7)", ArrayMeshPush);
        T.Run("geometry: ArrayMesh refusals, strips, points, materials (G4 R7)", ArrayMeshEdges);
        T.Run("geometry: SurfaceTool normals, smooth groups, indices and tangents (G4 R7)", SurfaceToolTests);
        T.Run("geometry: ImmediateMesh and MeshDataTool (G4 R7)", ImmediateAndDataTool);
        T.Run("geometry: the Godot to engine matrix and tangent (G4 R7)", Spaces);
        T.Run("geometry: MultiMesh table, buffer layout and its engine form (G4 R7)", MultiMeshTable);
        T.Run("geometry: MultiMesh from the converter's text (G4 R7)", MultiMeshText);
        T.Run("geometry: MeshLibrary and GridMap (G4 R7)", GridMapTests);
        T.Run("geometry: CSG, Sprite3D and Label3D carry Godot's properties (G4 R7)", NodeProperties);
        T.Run("geometry: no engine, no engine mesh (G4 R7)", NoEngine);
    });

    // A backend that records what it is asked and keeps what it was given.
    private sealed class Fake : IRuntimeMeshBackend
    {
        public readonly List<string> Calls = new();
        public readonly Dictionary<int, string> Keys = new();
        public readonly Dictionary<int, List<EngineSurface>> Surfaces = new();
        public readonly Dictionary<int, string> MultiKeys = new();
        public readonly Dictionary<int, float[]> Transforms = new();
        public readonly Dictionary<int, float[]> Colors = new();
        public readonly Dictionary<int, string> Sources = new();
        public readonly Dictionary<int, int> Counts = new(), Visibles = new();
        public readonly Dictionary<int, (bool Colors, bool Custom)> Flags = new();
        private int _next = 1;

        public int CreateMesh(string key) { Calls.Add("create " + key); int id = _next++; Keys[id] = key; Surfaces[id] = new List<EngineSurface>(); return id; }
        public bool DestroyMesh(int id) { Calls.Add("destroy " + id); return true; }
        public bool ClearMesh(int id) { Calls.Add("clear " + id); Surfaces[id].Clear(); return true; }
        public int AddSurface(int id, EngineSurface s) { Calls.Add("surface " + id); Surfaces[id].Add(s); return Surfaces[id].Count - 1; }

        public int CreateMulti(string key) { Calls.Add("multi " + key); int id = _next++; MultiKeys[id] = key; return id; }
        public bool DestroyMulti(int id) { Calls.Add("destroy multi " + id); return true; }
        public bool SetSource(int id, string meshPath) { Calls.Add($"source {id} {meshPath}"); Sources[id] = meshPath; return true; }
        public bool SetFlags(int id, bool c, bool u) { Calls.Add($"flags {id} {c} {u}"); Flags[id] = (c, u); return true; }
        public bool SetCount(int id, int n) { Calls.Add($"count {id} {n}"); Counts[id] = n; return true; }
        public bool SetVisible(int id, int n) { Calls.Add($"visible {id} {n}"); Visibles[id] = n; return true; }
        public bool SetBuffer(int id, int n, float[]? t, float[]? c, float[]? u) { Calls.Add($"buffer {id} {n}"); Counts[id] = n; if (t != null) Transforms[id] = (float[])t.Clone(); if (c != null) Colors[id] = (float[])c.Clone(); return true; }
        public bool SetTransform(int id, int i, float[] t12) { Calls.Add($"transform {id} {i}"); System.Array.Copy(t12, 0, Transforms[id], i * 12, 12); return true; }
        public bool SetColor(int id, int i, Color c) { Calls.Add($"color {id} {i}"); return true; }
        public bool SetCustom(int id, int i, Color c) { Calls.Add($"custom {id} {i}"); return true; }
        public bool TryGetInfo(int id, out int instances, out int visible, out int drawn) { instances = Counts.GetValueOrDefault(id); visible = Visibles.GetValueOrDefault(id, -1); drawn = instances; return true; }
        public int Count(string prefix) => Calls.Count(c => c.StartsWith(prefix, StringComparison.Ordinal));
    }

    private static Fake Install()
    {
        var f = new Fake();
        RuntimeMeshBackend.Override = f;
        return f;
    }

    private static void Done() => RuntimeMeshBackend.Override = null;

    private static Collections.Array Arrays(Vector3[] v, Vector3[]? n = null, float[]? t = null, Color[]? c = null, Vector2[]? uv = null, int[]? idx = null)
    {
        var a = new Collections.Array();
        for (int i = 0; i < (int)Mesh.ArrayType.Max; i++) a.Add(default);
        a[(int)Mesh.ArrayType.Vertex] = v;
        if (n != null) a[(int)Mesh.ArrayType.Normal] = n;
        if (t != null) a[(int)Mesh.ArrayType.Tangent] = t;
        if (c != null) a[(int)Mesh.ArrayType.Color] = c;
        if (uv != null) a[(int)Mesh.ArrayType.TexUV] = uv;
        if (idx != null) a[(int)Mesh.ArrayType.Index] = idx;
        return a;
    }

    private static readonly Vector3[] Tri = { new(0, 0, 0), new(0, 1, 0), new(1, 0, 0) };   // clockwise seen from +Z: Godot's front face

    private static void Classes()
    {
        foreach (string name in new[] { "ArrayMesh", "ImmediateMesh", "SurfaceTool", "MeshDataTool", "MultiMesh", "MultiMeshInstance3D", "MeshLibrary", "GridMap", "CSGShape3D", "CSGCombiner3D",
                                        "CSGBox3D", "CSGSphere3D", "CSGCylinder3D", "CSGTorus3D", "CSGPolygon3D", "CSGMesh3D", "Sprite3D", "Label3D", "SpriteBase3D" })
            T.Check($"ClassDB knows {name}", ClassDB.ClassExists(name));
        T.Check("the tree: ArrayMesh and ImmediateMesh are Meshes; MultiMesh a Resource; SurfaceTool a RefCounted; the instance nodes are GeometryInstance3D",
            ClassDB.IsParentClass("ArrayMesh", "Mesh") && ClassDB.IsParentClass("ImmediateMesh", "Mesh") && ClassDB.IsParentClass("MultiMesh", "Resource") &&
            ClassDB.IsParentClass("SurfaceTool", "RefCounted") && ClassDB.IsParentClass("MultiMeshInstance3D", "GeometryInstance3D") && ClassDB.IsParentClass("Sprite3D", "GeometryInstance3D") &&
            ClassDB.IsParentClass("Label3D", "GeometryInstance3D") && ClassDB.IsParentClass("GridMap", "Node3D") && ClassDB.IsParentClass("CSGBox3D", "CSGShape3D"));
        T.Check("the enum numbers are Godot's",
            (int)Mesh.PrimitiveType.Triangles == 3 && (int)Mesh.PrimitiveType.TriangleStrip == 4 && (int)Mesh.ArrayType.Tangent == 2 && (int)Mesh.ArrayType.Index == 12 && (int)Mesh.ArrayType.Max == 13 &&
            (int)CSGShape3D.OperationEnum.Subtraction == 2 && (int)MultiMesh.TransformFormatEnum.Transform3D == 1);
        var mm = new MultiMesh();
        T.Check("MultiMesh: no instances, all visible, 3D here", mm.InstanceCount == 0 && mm.VisibleInstanceCount == -1 && mm.TransformFormat == MultiMesh.TransformFormatEnum.Transform3D && !mm.UseColors);
        var box = new CSGBox3D();
        var sph = new CSGSphere3D();
        var cyl = new CSGCylinder3D();
        var tor = new CSGTorus3D();
        T.Check("CSG defaults are Godot's: box 1 m, sphere r 0.5 with 12 x 6, cylinder r 0.5 h 2 with 8 sides, torus 0.5 to 1 with 8 and 6, snap 1 mm",
            box.Size == Vector3.One && sph.Radius == 0.5f && sph.RadialSegments == 12 && sph.Rings == 6 && cyl.Height == 2f && cyl.Sides == 8 && tor.InnerRadius == 0.5f && tor.OuterRadius == 1f &&
            tor.Sides == 8 && tor.RingSides == 6 && box.Snap == 0.001f && box.CalculateTangents && !box.UseCollision && box.CollisionLayer == 1);
        T.Check("a CSG root is one that has no CSG parent", box.IsRootShape());
        var s = new Sprite3D();
        T.Check("Sprite3D defaults: 1 cm pixels, centred, transparent, unshaded, double sided, one frame", s.PixelSize == 0.01f && s.Centered && s.Transparent && !s.Shaded && s.DoubleSided && s.Hframes == 1 && s.Vframes == 1);
        var l = new Label3D();
        T.Check("Label3D defaults: size 32, outline 12, 5 mm pixels, centred, never drawn here", l.FontSize == 32 && l.OutlineSize == 12 && l.PixelSize == 0.005f && l.HorizontalAlignment == HorizontalAlignment.Center && !l.IsDrawn);
        var g = new GridMap();
        T.Check("GridMap defaults: 2 m cells, centred, scale 1, octant 8", g.CellSize == new Vector3(2, 2, 2) && g.CellCenterX && g.CellCenterY && g.CellCenterZ && g.CellScale == 1f && g.CellOctantSize == 8);
    }

    private static void ArrayMeshPush()
    {
        Fake f = Install();
        try
        {
            var mesh = new ArrayMesh();
            var uv = new[] { new Vector2(0, 0), new Vector2(0, 1), new Vector2(1, 0) };
            var nrm = new[] { new Vector3(0, 0, 1), new Vector3(0, 0, 1), new Vector3(0, 0, 1) };
            var tan = new float[] { 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1 };
            var col = new[] { new Color(1, 0, 0, 1), new Color(0, 1, 0, 1), new Color(0, 0, 1, 0.5f) };
            mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri, nrm, tan, col, uv, new[] { 0, 1, 2 }));
            T.Check("a surface is held, with no engine call until something draws the mesh", mesh.GetSurfaceCount() == 1 && f.Calls.Count == 0);
            string? path = MeshInstance3D.MeshAssetOf(mesh);
            T.Check("the first draw creates the engine mesh under a runtime key", path != null && path.StartsWith("@rtmesh/array/", StringComparison.Ordinal) && f.Count("create array/") == 1, path ?? "null");
            T.Check("and sends the surface held so far", f.Count("surface 1") == 1 && f.Surfaces[1].Count == 1);
            EngineSurface e = f.Surfaces[1][0];
            T.Check("positions are in engine centimetres: Godot (0,1,0) is (0,0,100) and (1,0,0) is (0,100,0)",
                T.Near(e.Pos[3], 0) && T.Near(e.Pos[4], 0) && T.Near(e.Pos[5], 100) && T.Near(e.Pos[6], 0) && T.Near(e.Pos[7], 100) && T.Near(e.Pos[8], 0));
            T.Check("the normal +Z is engine -X", e.Nrm != null && T.Near(e.Nrm[0], -1) && T.Near(e.Nrm[1], 0) && T.Near(e.Nrm[2], 0));
            T.Check("the tangent +X is engine +Y with handedness +1 (the R1 contract, as the converter's glTF rule)", e.Tan != null && T.Near(e.Tan[0], 0) && T.Near(e.Tan[1], 1) && T.Near(e.Tan[2], 0) && T.Near(e.Tan[3], 1));
            T.Check("colour, UV and the index order pass through (Godot's clockwise front faces are the engine's order)",
                e.Col != null && T.Near(e.Col[0], 1) && T.Near(e.Col[5], 1) && T.Near(e.Col[11], 0.5f) && e.Uv != null && T.Near(e.Uv[3], 1) && e.Idx != null && e.Idx.SequenceEqual(new uint[] { 0, 1, 2 }));
            T.Check("an unused stream stays absent", e.Uv1 == null && e.Material == null);

            mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri));
            T.Check("a second surface is sent alone, once the engine mesh exists", f.Count("surface 1") == 2 && f.Count("clear") == 0 && f.Surfaces[1].Count == 2);
            T.Check("a surface with no normals sends none (the engine makes smooth ones) and no index list", f.Surfaces[1][1].Nrm == null && f.Surfaces[1][1].Idx == null);
            T.Check("asking again does not create a second engine mesh", MeshInstance3D.MeshAssetOf(mesh) == path && f.Count("create") == 1);

            var back = mesh.SurfaceGetArrays(0);
            T.Check("SurfaceGetArrays returns Godot's layout", back.Count == (int)Mesh.ArrayType.Max && back[(int)Mesh.ArrayType.Vertex].AsVector3Array().Length == 3 && back[(int)Mesh.ArrayType.Index].AsInt32Array().Length == 3 &&
                back[(int)Mesh.ArrayType.Tangent].AsFloat32Array().Length == 12 && back[(int)Mesh.ArrayType.TexUV2].IsNil);
            T.Check("with the vertices as set", back[(int)Mesh.ArrayType.Vertex].AsVector3Array()[2] == new Vector3(1, 0, 0));
            T.Check("sizes: array length, index length, primitive", mesh.SurfaceGetArrayLen(0) == 3 && mesh.SurfaceGetArrayIndexLen(0) == 3 && mesh.SurfaceGetArrayIndexLen(1) == 0 && mesh.SurfaceGetPrimitiveType(0) == Mesh.PrimitiveType.Triangles);
            Aabb bb = mesh.GetAabb();
            T.Check("the box is the vertices'", bb.Position == Vector3.Zero && bb.Size == new Vector3(1, 1, 0));

            mesh.ClearSurfaces();
            T.Check("ClearSurfaces empties the mesh and the engine's", mesh.GetSurfaceCount() == 0 && f.Count("clear 1") == 1 && f.Surfaces[1].Count == 0);
            mesh.SurfaceSetName(0, "x");
            T.Check("a surface that is not there has no name and no material", mesh.SurfaceGetName(0) == "" && mesh.SurfaceGetMaterial(0) == null);
        }
        finally { Done(); }
    }

    private static void ArrayMeshEdges()
    {
        Fake f = Install();
        try
        {
            var mesh = new ArrayMesh();
            mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri, new[] { Vector3.Up }));
            T.Check("a normal array of the wrong length is an error and adds nothing", T.ErrorCount("normal array") == 1 && mesh.GetSurfaceCount() == 0);
            mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri, null, null, null, null, new[] { 0, 1, 5 }));
            T.Check("an index past the vertices is an error", T.ErrorCount("outside") == 1 && mesh.GetSurfaceCount() == 0);
            mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(new[] { new Vector3(0, 0, 0), new Vector3(1, 0, 0) }));
            T.Check("a triangle surface that is not whole triangles is an error", T.ErrorCount("multiple of three") == 1 && mesh.GetSurfaceCount() == 0);
            mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, new Collections.Array());
            T.Check("an empty array set is an error", T.ErrorCount("vertex array is empty") == 1);

            // a strip of four vertices: triangles (0,1,2) and (2,1,3) with alternate winding
            var strip = new ArrayMesh();
            var v4 = new[] { new Vector3(0, 0, 0), new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(1, 1, 0) };
            strip.AddSurfaceFromArrays(Mesh.PrimitiveType.TriangleStrip, Arrays(v4));
            MeshInstance3D.MeshAssetOf(strip);
            T.Check("a triangle strip is expanded to triangles with alternating winding", f.Surfaces.Count == 1 && f.Surfaces[1][0].Idx!.SequenceEqual(new uint[] { 0, 1, 2, 2, 1, 3 }));

            // points and lines are kept, not drawn
            var pts = new ArrayMesh();
            pts.AddSurfaceFromArrays(Mesh.PrimitiveType.Points, Arrays(Tri));
            MeshInstance3D.MeshAssetOf(pts);
            T.Check("a point surface is held and not sent", pts.GetSurfaceCount() == 1 && f.Surfaces[2].Count == 0);
            T.Check("with one warning that says why", T.Logs.Any(l => l.Level == Aver.Scripting.Log.Level.Warn && l.Message.Contains("only triangle surfaces are drawn")));

            // materials: the surface's material is named by its file stem and re-sent when it changes
            var withMat = new ArrayMesh();
            withMat.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri));
            MeshInstance3D.MeshAssetOf(withMat);
            int id = f.Keys.First(kv => kv.Value.StartsWith("array/") && kv.Key > 2).Key;
            int clears = f.Count("clear");
            var mat = new StandardMaterial3D { ResourcePath = "res://materials/red.tres" };
            withMat.SurfaceSetMaterial(0, mat);
            T.Check("a material change clears and re-sends the surface", f.Count("clear") == clears + 1 && f.Surfaces[id].Count == 1);
            T.Check("the engine surface names the material by its file stem", f.Surfaces[id][0].Material == "red");
            T.Check("SurfaceGetMaterial answers it", withMat.SurfaceGetMaterial(0) == mat);
            withMat.SurfaceRemove(0);
            T.Check("SurfaceRemove drops the surface (engine too)", withMat.GetSurfaceCount() == 0 && f.Surfaces[id].Count == 0);
            withMat.SurfaceRemove(5);
            T.Check("removing one that is not there is an error", T.ErrorCount("no surface 5") == 1);
        }
        finally { Done(); }
    }

    private static void SurfaceToolTests()
    {
        // normals: a clockwise triangle seen from +Z faces +Z
        var st = new SurfaceTool();
        st.Begin(Mesh.PrimitiveType.Triangles);
        foreach (Vector3 v in Tri) st.AddVertex(v);
        st.GenerateNormals();
        var arrays = st.CommitToArrays();
        Vector3[] n = arrays[(int)Mesh.ArrayType.Normal].AsVector3Array();
        T.Check("generate_normals: a clockwise triangle gets its face normal (+Z)", n.Length == 3 && T.Near(n[0], new Vector3(0, 0, 1)) && T.Near(n[2], new Vector3(0, 0, 1)));
        st.Clear();
        st.Begin(Mesh.PrimitiveType.Triangles);
        foreach (Vector3 v in Tri) st.AddVertex(v);
        st.GenerateNormals(true);
        T.Check("flip inverts them", T.Near(st.CommitToArrays()[(int)Mesh.ArrayType.Normal].AsVector3Array()[0], new Vector3(0, 0, -1)));

        // two faces meeting at an edge (a floor facing +Z and a wall facing -Y): smooth averages at the shared vertices, flat keeps each face's normal
        Vector3[] roof =
        {
            new(0, 0, 0), new(0, 1, 0), new(1, 0, 0),    // faces +Z
            new(1, 0, 0), new(0, 0, 0), new(0, 0, 1),    // faces -Y; shares (0,0,0) and (1,0,0)
        };
        var smooth = new SurfaceTool();
        smooth.Begin(Mesh.PrimitiveType.Triangles);
        foreach (Vector3 v in roof) smooth.AddVertex(v);
        smooth.GenerateNormals();
        Vector3[] sn = smooth.CommitToArrays()[(int)Mesh.ArrayType.Normal].AsVector3Array();
        T.Check("smooth group 0: vertices at the same position share one normal, the average of the faces", T.Near(sn[0], sn[4]) && T.Near(sn[2], sn[3]) && T.Near(sn[0], new Vector3(0, -0.70710678f, 0.70710678f), 1e-3f));
        T.Check("and a vertex only one face uses keeps that face's normal", T.Near(sn[1], new Vector3(0, 0, 1)) && T.Near(sn[5], new Vector3(0, -1, 0)));
        var flat = new SurfaceTool();
        flat.Begin(Mesh.PrimitiveType.Triangles);
        flat.SetSmoothGroup(uint.MaxValue);
        foreach (Vector3 v in roof) flat.AddVertex(v);
        flat.GenerateNormals();
        Vector3[] fn = flat.CommitToArrays()[(int)Mesh.ArrayType.Normal].AsVector3Array();
        T.Check("smooth group -1: each face keeps its own normal at all its vertices", T.Near(fn[0], new Vector3(0, 0, 1)) && T.Near(fn[2], new Vector3(0, 0, 1)) && T.Near(fn[3], new Vector3(0, -1, 0)) && T.Near(fn[4], new Vector3(0, -1, 0)));
        T.Check("and the unit length holds", T.Near(fn[0].Length(), 1f) && T.Near(sn[0].Length(), 1f));

        // index / deindex
        var ix = new SurfaceTool();
        ix.Begin(Mesh.PrimitiveType.Triangles);
        foreach (Vector3 v in new[] { new Vector3(0, 0, 0), new Vector3(0, 1, 0), new Vector3(1, 0, 0), new Vector3(1, 0, 0), new Vector3(0, 1, 0), new Vector3(1, 1, 0) }) ix.AddVertex(v);
        ix.Index();
        var ia = ix.CommitToArrays();
        T.Check("index() merges identical vertices (6 become 4) and fills the index list", ia[(int)Mesh.ArrayType.Vertex].AsVector3Array().Length == 4 && ia[(int)Mesh.ArrayType.Index].AsInt32Array().Length == 6);
        ix.Deindex();
        var da = ix.CommitToArrays();
        T.Check("deindex() expands them again", da[(int)Mesh.ArrayType.Vertex].AsVector3Array().Length == 6 && da[(int)Mesh.ArrayType.Index].IsNil);

        // set_* applies to the vertices added after it; a stream is present once any vertex set it
        var attr = new SurfaceTool();
        attr.Begin(Mesh.PrimitiveType.Triangles);
        attr.SetUV(new Vector2(0.5f, 0.25f));
        attr.SetColor(new Color(0, 1, 0, 1));
        attr.AddVertex(Tri[0]);
        attr.SetUV(new Vector2(1, 1));
        attr.AddVertex(Tri[1]);
        attr.AddVertex(Tri[2]);
        var aa = attr.CommitToArrays();
        Vector2[] auv = aa[(int)Mesh.ArrayType.TexUV].AsVector2Array();
        T.Check("set_uv applies to the next vertices and carries on", auv[0] == new Vector2(0.5f, 0.25f) && auv[1] == new Vector2(1, 1) && auv[2] == new Vector2(1, 1));
        T.Check("set_color too; unset streams are absent", aa[(int)Mesh.ArrayType.Color].AsColorArray()[2] == new Color(0, 1, 0, 1) && aa[(int)Mesh.ArrayType.Normal].IsNil && aa[(int)Mesh.ArrayType.TexUV2].IsNil);

        // tangents: a clockwise quad in the XY plane with v = 1 - y: T = +X, and the handedness +1 (cross(N, T) = +Y points at decreasing v... v falls as y rises)
        var tq = new SurfaceTool();
        tq.Begin(Mesh.PrimitiveType.Triangles);
        Vector3[] q = { new(0, 0, 0), new(0, 1, 0), new(1, 1, 0), new(0, 0, 0), new(1, 1, 0), new(1, 0, 0) };   // clockwise seen from +Z
        foreach (Vector3 v in q)
        {
            tq.SetUV(new Vector2(v.X, 1f - v.Y));
            tq.AddVertex(v);
        }
        tq.GenerateNormals();
        tq.GenerateTangents();
        float[] tt = tq.CommitToArrays()[(int)Mesh.ArrayType.Tangent].AsFloat32Array();
        T.Check("generate_tangents: T follows increasing u (+X) at every vertex", tt.Length == 24 && T.Near(tt[0], 1f) && T.Near(tt[1], 0f) && T.Near(tt[2], 0f) && T.Near(tt[20], 1f));
        T.Check("with handedness +1 when v falls as y rises", T.Near(tt[3], 1f) && T.Near(tt[23], 1f));
        var tq2 = new SurfaceTool();
        tq2.Begin(Mesh.PrimitiveType.Triangles);
        foreach (Vector3 v in q) { tq2.SetUV(new Vector2(v.X, v.Y)); tq2.AddVertex(v); }
        tq2.GenerateNormals();
        tq2.GenerateTangents();
        T.Check("and -1 when v rises with y", T.Near(tq2.CommitToArrays()[(int)Mesh.ArrayType.Tangent].AsFloat32Array()[3], -1f));
        var noUv = new SurfaceTool();
        noUv.Begin(Mesh.PrimitiveType.Triangles);
        foreach (Vector3 v in Tri) noUv.AddVertex(v);
        noUv.GenerateTangents();
        T.Check("tangents without UVs and normals are an error", T.ErrorCount("generate_tangents") == 1);

        // commit
        var cm = new SurfaceTool();
        cm.Begin(Mesh.PrimitiveType.Triangles);
        cm.SetMaterial(new StandardMaterial3D { ResourcePath = "res://m/blue.tres" });
        foreach (Vector3 v in Tri) cm.AddVertex(v);
        ArrayMesh? m = cm.Commit();
        T.Check("commit makes an ArrayMesh with the surface and the material", m != null && m.GetSurfaceCount() == 1 && m.SurfaceGetMaterial(0)?.ResourcePath == "res://m/blue.tres");
        ArrayMesh? same = cm.Commit(m);
        T.Check("commit into an existing mesh adds a surface", ReferenceEquals(same, m) && m!.GetSurfaceCount() == 2);
        var empty = new SurfaceTool();
        empty.Begin(Mesh.PrimitiveType.Triangles);
        T.Check("committing nothing is an error and returns the mesh given", empty.Commit(null) == null && T.ErrorCount("no vertices") == 1);
        var noBegin = new SurfaceTool();
        noBegin.AddVertex(Vector3.Zero);
        T.Check("a vertex before begin() is an error", T.ErrorCount("call begin first") == 1);

        // create_from / append_from
        var cf = new SurfaceTool();
        cf.CreateFrom(m!, 0);
        var moved = new Transform3D(Basis.Identity, new Vector3(10, 0, 0));
        cf.AppendFrom(m!, 0, moved);
        ArrayMesh? two = cf.Commit();
        T.Check("create_from then append_from(transform) joins two copies, the second moved", two != null && two.SurfaceGetArrayLen(0) == 6 && two.SurfaceGetArrays(0)[(int)Mesh.ArrayType.Vertex].AsVector3Array()[3] == new Vector3(10, 0, 0));
        var fan = new SurfaceTool();
        fan.Begin(Mesh.PrimitiveType.Triangles);
        fan.AddTriangleFan(new[] { new Vector3(0, 0, 0), new Vector3(0, 1, 0), new Vector3(1, 1, 0), new Vector3(1, 0, 0) });
        T.Check("add_triangle_fan: four points make two triangles around the first", fan.CommitToArrays()[(int)Mesh.ArrayType.Vertex].AsVector3Array().Length == 6);
    }

    private static void ImmediateAndDataTool()
    {
        Fake f = Install();
        try
        {
            var im = new ImmediateMesh();
            im.SurfaceBegin(Mesh.PrimitiveType.Triangles, new StandardMaterial3D { ResourcePath = "res://m/green.tres" });
            im.SurfaceSetColor(new Color(1, 0, 0, 1));
            im.SurfaceSetUV(new Vector2(1, 0));
            foreach (Vector3 v in Tri) im.SurfaceAddVertex(v);
            T.Check("vertices wait for surface_end", im.GetSurfaceCount() == 0);
            im.SurfaceEnd();
            T.Check("surface_end makes the surface", im.GetSurfaceCount() == 1 && im.SurfaceGetMaterial(0) != null);
            string? path = MeshInstance3D.MeshAssetOf(im);
            T.Check("the engine mesh is keyed 'immediate/...'", path != null && path.StartsWith("@rtmesh/immediate/", StringComparison.Ordinal));
            EngineSurface e = f.Surfaces[1][0];
            T.Check("it was sent with the colour, the UV and the material, no normals", e.Col != null && T.Near(e.Col[0], 1) && e.Uv != null && T.Near(e.Uv[0], 1) && e.Nrm == null && e.Material == "green");
            im.ClearSurfaces();
            im.SurfaceBegin(Mesh.PrimitiveType.Triangles);
            im.SurfaceAddVertex(Tri[0]); im.SurfaceAddVertex(Tri[1]); im.SurfaceAddVertex(Tri[2]);
            im.SurfaceEnd();
            T.Check("a frame's rebuild: clear then a new surface", f.Count("clear 1") == 1 && f.Surfaces[1].Count == 1 && f.Surfaces[1][0].Col == null);
            im.SurfaceBegin(Mesh.PrimitiveType.Triangles);
            im.SurfaceBegin(Mesh.PrimitiveType.Triangles);
            T.Check("a second surface_begin before the end is an error", T.ErrorCount("already open") == 1);
            im.SurfaceEnd();
            im.SurfaceEnd();
            T.Check("surface_end with none open is an error", T.ErrorCount("no surface is open") == 1);
            im.SurfaceAddVertex(Vector3.Zero);
            T.Check("a vertex outside a surface is an error", T.ErrorCount("call surface_begin first") == 1);
        }
        finally { Done(); }

        var arr = new ArrayMesh();
        arr.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(new[] { new Vector3(0, 0, 0), new Vector3(0, 1, 0), new Vector3(1, 1, 0), new Vector3(1, 0, 0) }, null, null, null,
            new[] { new Vector2(0, 1), new Vector2(0, 0), new Vector2(1, 0), new Vector2(1, 1) }, new[] { 0, 1, 2, 0, 2, 3 }));
        var mdt = new MeshDataTool();
        T.Check("create_from_surface", mdt.CreateFromSurface(arr, 0) == Error.Ok && mdt.GetVertexCount() == 4 && mdt.GetFaceCount() == 2);
        T.Check("vertices, UVs and face corners read back", mdt.GetVertex(2) == new Vector3(1, 1, 0) && mdt.GetVertexUV(1) == new Vector2(0, 0) && mdt.GetFaceVertex(1, 1) == 2 && mdt.GetFaceVertex(1, 3) == -1);
        T.Check("a clockwise face's normal is +Z", T.Near(mdt.GetFaceNormal(0), new Vector3(0, 0, 1)));
        mdt.SetVertex(2, new Vector3(2, 2, 0));
        T.Check("edits stay in the tool until committed", arr.SurfaceGetArrays(0)[(int)Mesh.ArrayType.Vertex].AsVector3Array()[2] == new Vector3(1, 1, 0));
        T.Check("commit_to_surface adds a surface with the edit", mdt.CommitToSurface(arr) == Error.Ok && arr.GetSurfaceCount() == 2 && arr.SurfaceGetArrays(1)[(int)Mesh.ArrayType.Vertex].AsVector3Array()[2] == new Vector3(2, 2, 0));
        T.Check("the format reports the streams (vertex, uv, index)", mdt.GetFormat() == (1 | 16 | 4096));
        T.Check("a surface that is not there is refused", mdt.CreateFromSurface(arr, 9) == Error.InvalidParameter);
    }

    private static void Spaces()
    {
        var t = new float[12];
        FabricSpace.ToAverMatrix12(new Transform3D(Basis.Identity, new Vector3(1, 2, 3)), t);
        T.Check("identity with origin (1,2,3) m: unit rows and the translation in engine centimetres (-300, 100, 200)",
            T.Near(t[0], 1) && T.Near(t[4], 1) && T.Near(t[8], 1) && T.Near(t[1], 0) && T.Near(t[9], -300) && T.Near(t[10], 100) && T.Near(t[11], 200));
        var turn = new Transform3D(new Basis(Vector3.Up, MathF.PI / 2f), Vector3.Zero);   // +90 degrees about Godot's Y
        FabricSpace.ToAverMatrix12(turn, t);
        T.Check("a quarter turn about Godot's up: engine X goes to -Y, engine Y goes to +X, engine Z stays (row-vector form: row i is where axis i goes)",
            T.Near(t[0], 0) && T.Near(t[1], -1) && T.Near(t[2], 0) && T.Near(t[3], 1) && T.Near(t[4], 0) && T.Near(t[5], 0) && T.Near(t[6], 0) && T.Near(t[7], 0) && T.Near(t[8], 1));
        var scaled = new Transform3D(Basis.FromScale(new Vector3(2, 3, 4)), Vector3.Zero);
        FabricSpace.ToAverMatrix12(scaled, t);
        T.Check("a scale (x 2, y 3, z 4 in Godot) is engine (Y 2, Z 3, X 4)", T.Near(t[0], 4) && T.Near(t[4], 2) && T.Near(t[8], 3));
        FabricSpace.ToAverMatrix12(new Transform3D(Basis.FlipX, Vector3.Zero), t);
        T.Check("a mirror survives (determinant -1 in the 3x3 part)", T.Near(t[0] * (t[4] * t[8] - t[5] * t[7]) - t[1] * (t[3] * t[8] - t[5] * t[6]) + t[2] * (t[3] * t[7] - t[4] * t[6]), -1f));

        var tan = new float[4];
        FabricSpace.ToAverTangent(new Vector3(0, 0, 1), new Vector3(1, 0, 0), 1f, tan, 0);
        T.Check("ToAverTangent: normal +Z, tangent +X, w +1 gives engine (0,1,0) and +1", T.Near(tan[0], 0) && T.Near(tan[1], 1) && T.Near(tan[2], 0) && T.Near(tan[3], 1));
        FabricSpace.ToAverTangent(new Vector3(0, 0, 1), new Vector3(1, 0, 0), -1f, tan, 0);
        T.Check("and w -1 flips the handedness", T.Near(tan[3], -1));
        FabricSpace.ToAverTangent(new Vector3(0, 0, 1), Vector3.Zero, 1f, tan, 0);
        T.Check("a zero tangent is 'no tangent' (all zeros)", tan.All(x => x == 0f));
        FabricSpace.ToAverTangent(new Vector3(0, 0, 1), new Vector3(1, 0, 1), 1f, tan, 0);
        T.Check("a tangent with a normal component is made orthogonal to the normal", T.Near(tan[0], 0) && T.Near(tan[1], 1) && T.Near(tan[2], 0));
    }

    private static void MultiMeshTable()
    {
        Fake f = Install();
        try
        {
            var mm = new MultiMesh { UseColors = true, InstanceCount = 3 };
            var fileMesh = new BoxMesh { ResourcePath = "res://Meshes/Rock.ocmesh" };
            mm.Mesh = fileMesh;
            T.Check("a MultiMesh not drawn yet has made nothing", f.Calls.Count == 0);
            mm.SetInstanceTransform(1, new Transform3D(Basis.Identity, new Vector3(1, 0, 0)));
            mm.SetInstanceColor(2, new Color(0.25f, 0.5f, 0.75f, 1f));
            string? path = mm.EngineAssetPath;
            T.Check("the first draw creates the engine table, named under 'multimesh/'", path != null && path.StartsWith("@rtmesh/multimesh/mm", StringComparison.Ordinal) && f.Count("multi mm") == 1, path ?? "null");
            T.Check("flags first, then the source (a content path: res:// stripped), then the whole table, then the visible count",
                f.Calls.IndexOf("flags 1 True False") < f.Calls.IndexOf("source 1 Meshes/Rock.ocmesh") && f.Calls.IndexOf("source 1 Meshes/Rock.ocmesh") < f.Calls.IndexOf("buffer 1 3") && f.Calls.Contains("visible 1 -1"));
            T.Check("instance 1's transform: origin (1,0,0) m is engine (0, 100, 0) at the translation slot", f.Transforms[1][12 + 9] == 0 && T.Near(f.Transforms[1][12 + 10], 100) && T.Near(f.Transforms[1][12 + 0], 1));
            T.Check("colours go with it, white by default", f.Colors[1].Length == 12 && T.Near(f.Colors[1][0], 1) && T.Near(f.Colors[1][8], 0.25f) && T.Near(f.Colors[1][10], 0.75f));

            mm.SetInstanceTransform(0, new Transform3D(Basis.Identity, new Vector3(0, 0, -2)));
            T.Check("a transform set later is one call, written into the engine's table", f.Count("transform 1 0") == 1 && T.Near(f.Transforms[1][9], 200) && f.Count("buffer") == 1);
            mm.SetInstanceColor(0, new Color(1, 0, 0, 1));
            mm.SetInstanceCustomData(0, new Color(1, 2, 3, 4));
            T.Check("colour and custom data are single calls", f.Count("color 1 0") == 1 && f.Count("custom 1 0") == 1);
            mm.VisibleInstanceCount = 2;
            T.Check("the visible count is sent", f.Visibles[1] == 2);
            mm.InstanceCount = 5;
            T.Check("a bigger table is sent as a count; new instances are identity and white", f.Counts[1] == 5 && mm.GetInstanceColor(4) == new Color(1, 1, 1, 1) && mm.GetInstanceTransform(4).IsEqualApprox(Transform3D.Identity));
            mm.Mesh = new BoxMesh { ResourcePath = "res://Meshes/Other.ocmesh" };
            T.Check("a new mesh is a new source", f.Sources[1] == "Meshes/Other.ocmesh");
            mm.UseCustomData = true;
            T.Check("changing the flags is sent", f.Flags[1] == (true, true));

            // bounds
            var m2 = new MultiMesh { InstanceCount = 2, Mesh = new MeshWithBox(new Aabb(new Vector3(-1, -1, -1), new Vector3(2, 2, 2))) };
            m2.SetInstanceTransform(1, new Transform3D(Basis.Identity, new Vector3(10, 0, 0)));
            Aabb box = m2.GetAabb();
            T.Check("GetAabb spans every instance's mesh box", T.Near(box.Position.X, -1) && T.Near(box.End.X, 11));
            m2.VisibleInstanceCount = 1;
            T.Check("and only the visible ones", T.Near(m2.GetAabb().End.X, 1));

            // a mesh built at run time is the source by its runtime path
            var rt = new ArrayMesh();
            rt.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri));
            var m3 = new MultiMesh { InstanceCount = 1, Mesh = rt };
            string? p3 = m3.EngineAssetPath;
            int id3 = f.MultiKeys.First(kv => kv.Value == "mm" + m3.GetInstanceId()).Key;
            T.Check("a script-built mesh is named by its runtime path", p3 != null && f.Sources[id3].StartsWith("@rtmesh/array/", StringComparison.Ordinal));
            T.Check("InstanceCount is clamped at zero", new MultiMesh { InstanceCount = -4 }.InstanceCount == 0);
            mm.SetInstanceTransform(99, Transform3D.Identity);
            T.Check("an instance out of range is an error", T.ErrorCount("out of range") == 1);

            // the Godot buffer layout
            var b = new MultiMesh { UseColors = true, UseCustomData = true, InstanceCount = 2 };
            b.SetInstanceTransform(1, new Transform3D(new Basis(new Vector3(1, 0, 0), new Vector3(0, 2, 0), new Vector3(0, 0, 3)), new Vector3(7, 8, 9)));
            b.SetInstanceColor(1, new Color(0.1f, 0.2f, 0.3f, 0.4f));
            b.SetInstanceCustomData(1, new Color(5, 6, 7, 8));
            float[] buf = b.Buffer;
            T.Check("the buffer is 40 floats: per instance 12 transform floats (3 rows of basis x, y, z and origin), 4 colour, 4 custom", buf.Length == 40);
            T.CheckNum("origin x", 7, buf[20 + 3]); T.CheckNum("origin y", 8, buf[20 + 7]); T.CheckNum("origin z", 9, buf[20 + 11]);
            T.CheckNum("scale x at its row", 1, buf[20 + 0]); T.CheckNum("scale y at its row", 2, buf[20 + 5]); T.CheckNum("scale z at its row", 3, buf[20 + 10]);
            T.CheckNum("colour g", 0.2f, buf[20 + 13]); T.CheckNum("colour a", 0.4f, buf[20 + 15]);
            T.CheckNum("custom x", 5, buf[20 + 16]); T.CheckNum("custom w", 8, buf[20 + 19]);
            var again = new MultiMesh { UseColors = true, UseCustomData = true };
            again.Buffer = buf;
            T.Check("and a buffer set back gives the same table", again.InstanceCount == 2 && again.GetInstanceTransform(1).Origin == new Vector3(7, 8, 9) && again.GetInstanceColor(1) == new Color(0.1f, 0.2f, 0.3f, 0.4f) &&
                again.GetInstanceCustomData(1) == new Color(5, 6, 7, 8));
        }
        finally { Done(); }
    }

    // A mesh with a known box, for the bounds checks.
    private sealed class MeshWithBox : Mesh
    {
        private readonly Aabb _b;
        public MeshWithBox(Aabb b) => _b = b;
        public override Aabb GetAabb() => _b;
    }

    private static void MultiMeshText()
    {
        // The converter's form, as the level file writes it.
        Variant v = VariantText.Parse("Object(MultiMesh, \"transform_format\": 1, \"use_colors\": true, \"use_custom_data\": false, \"instance_count\": 2, \"visible_instance_count\": -1, " +
            "\"buffer\": PackedFloat32Array(1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1,  1, 0, 0, 6, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0.5))");
        MultiMesh? mm = v.AsGodotObject() as MultiMesh;
        T.Check("Object(MultiMesh, ...) builds a MultiMesh through ClassDB", mm != null);
        if (mm == null) return;
        T.Check("with two instances, colours on, all visible", mm.InstanceCount == 2 && mm.UseColors && !mm.UseCustomData && mm.VisibleInstanceCount == -1);
        T.CheckVec("instance 0's origin comes from the buffer", new Vector3(5, 0, 0), mm.GetInstanceTransform(0).Origin);
        T.CheckVec("instance 1's origin", new Vector3(6, 0, 0), mm.GetInstanceTransform(1).Origin);
        T.Check("colours: red, then green at half alpha", mm.GetInstanceColor(0) == new Color(1, 0, 0, 1) && mm.GetInstanceColor(1) == new Color(0, 1, 0, 0.5f));
        T.Check("the buffer reads back the same 32 floats", mm.Buffer.Length == 32 && mm.Buffer[3] == 5 && mm.Buffer[16 + 3] == 6);

        // properties set in another order: the buffer decides the count when none is set
        MultiMesh? late = VariantText.Parse("Object(MultiMesh, \"use_colors\": true, \"buffer\": PackedFloat32Array(1,0,0,1, 0,1,0,2, 0,0,1,3, 1,1,1,1))").AsGodotObject() as MultiMesh;
        T.Check("a buffer with no instance_count sizes the table", late != null && late.InstanceCount == 1 && late.GetInstanceTransform(0).Origin == new Vector3(1, 2, 3));
    }

    private static void GridMapTests()
    {
        var g = new GridMap();
        T.CheckVec("map_to_local centres a cell: (0,0,0) is (1,1,1) with 2 m cells", new Vector3(1, 1, 1), g.MapToLocal(new Vector3I(0, 0, 0)));
        T.CheckVec("(1,0,-1) is (3,1,-1)", new Vector3(3, 1, -1), g.MapToLocal(new Vector3I(1, 0, -1)));
        g.CellCenterY = false;
        T.CheckVec("without centring on Y the cell's floor is its origin", new Vector3(1, 0, 1), g.MapToLocal(Vector3I.Zero));
        g.CellCenterY = true;
        T.Check("local_to_map floors: (2.5, 0.1, -0.1) is cell (1, 0, -1)", g.LocalToMap(new Vector3(2.5f, 0.1f, -0.1f)) == new Vector3I(1, 0, -1));
        T.Check("map_to_local then local_to_map returns the cell", g.LocalToMap(g.MapToLocal(new Vector3I(-3, 4, 7))) == new Vector3I(-3, 4, 7));

        // the stored form the converter keeps: cells (0,0,0 item 0), (1,0,0 item 0 orientation 10), (0,1,2 item 1), (-1,0,0 item 0)
        g.Data = new Collections.Dictionary { ["cells"] = new[] { 0, 0, 0, 1, 0, 655360, 65536, 2, 1, 65535, 0, 0 } };
        T.Check("the packed cells decode: four cells", g.GetUsedCells().Count == 4);
        T.Check("items and orientations", g.GetCellItem(new Vector3I(1, 0, 0)) == 0 && g.GetCellItemOrientation(new Vector3I(1, 0, 0)) == 10 && g.GetCellItem(new Vector3I(0, 1, 2)) == 1 && g.GetCellItem(new Vector3I(-1, 0, 0)) == 0);
        T.Check("an empty cell answers -1", g.GetCellItem(new Vector3I(9, 9, 9)) == GridMap.InvalidCellItem && g.GetCellItemOrientation(new Vector3I(9, 9, 9)) == -1);
        T.Check("by item", g.GetUsedCellsByItem(0).Count == 3 && g.GetUsedCellsByItem(1).Count == 1);
        Collections.Dictionary round = g.Data;
        var g2 = new GridMap { Data = round };
        T.Check("data round trips", g2.GetUsedCells().Count == 4 && g2.GetCellItemOrientation(new Vector3I(1, 0, 0)) == 10 && g2.GetCellItem(new Vector3I(-1, 0, 0)) == 0);
        Basis b10 = g.GetCellItemBasis(new Vector3I(1, 0, 0));
        T.Check("orientation 10 is a half turn about Y: diag(-1, 1, -1)", T.Near(b10.Column0.X, -1) && T.Near(b10.Column1.Y, 1) && T.Near(b10.Column2.Z, -1));
        Transform3D cellT = g.GetCellTransform(new Vector3I(1, 0, 0));
        T.CheckVec("the cell transform puts it at its position", new Vector3(3, 1, 1), cellT.Origin);
        g.SetCellItem(new Vector3I(1, 0, 0), -1);
        T.Check("set_cell_item with -1 clears the cell", g.GetCellItem(new Vector3I(1, 0, 0)) == -1 && g.GetUsedCells().Count == 3);
        g.SetCellItem(new Vector3I(5, 5, 5), 2, 40);
        T.Check("an orientation outside 0..23 is an error", T.ErrorCount("orientation 40") == 1 && g.GetCellItem(new Vector3I(5, 5, 5)) == -1);
        g.Clear();
        T.Check("clear", g.GetUsedCells().Count == 0);

        // the 24 bases
        var seen = new HashSet<string>();
        bool ortho = true, proper = true, roundTrip = true;
        for (int i = 0; i < 24; i++)
        {
            Basis b = g.GetBasisWithOrthogonalIndex(i);
            seen.Add($"{b.Row0}|{b.Row1}|{b.Row2}");
            ortho = ortho && T.Near(b.Row0.Dot(b.Row1), 0) && T.Near(b.Row0.Dot(b.Row2), 0) && T.Near(b.Row1.Dot(b.Row2), 0) && T.Near(b.Row0.Length(), 1) && T.Near(b.Row1.Length(), 1) && T.Near(b.Row2.Length(), 1);
            proper = proper && T.Near(b.Determinant(), 1f);
            roundTrip = roundTrip && g.GetOrthogonalIndexFromBasis(b) == i;
        }
        T.Check("24 distinct bases", seen.Count == 24);
        T.Check("each orthonormal", ortho);
        T.Check("each a proper rotation (determinant +1)", proper);
        T.Check("get_orthogonal_index_from_basis inverts the table", roundTrip);
        T.Check("index 0 is the identity and an index out of range answers it too", g.GetBasisWithOrthogonalIndex(0).IsEqualApprox(Basis.Identity) && g.GetBasisWithOrthogonalIndex(24).IsEqualApprox(Basis.Identity));
        T.Check("16: rows (0,0,1) (0,1,0) (-1,0,0), as Godot lists them", g.GetBasisWithOrthogonalIndex(16).Row0 == new Vector3(0, 0, 1) && g.GetBasisWithOrthogonalIndex(16).Row2 == new Vector3(-1, 0, 0));
        T.Check("a basis that is not one of the 24 has no index", g.GetOrthogonalIndexFromBasis(new Basis(Vector3.Up, 0.3f)) == -1);

        // the library
        Variant lib = VariantText.Parse("Object(MeshLibrary, \"items\": [{\"id\": 0, \"name\": \"Block\"}, {\"id\": 3, \"name\": \"Pillar\"}])");
        MeshLibrary? ml = lib.AsGodotObject() as MeshLibrary;
        T.Check("Object(MeshLibrary, items) builds a library", ml != null && ml.GetItemList().SequenceEqual(new[] { 0, 3 }));
        if (ml == null) return;
        T.Check("names and the search by name", ml.GetItemName(3) == "Pillar" && ml.FindItemByName("Block") == 0 && ml.FindItemByName("Nope") == -1);
        T.Check("the next free id", ml.GetLastUnusedItemId() == 4);
        ml.SetItemMeshTransform(3, new Transform3D(Basis.Identity, new Vector3(0, 0.5f, 0)));
        ml.SetItemShapes(3, new Collections.Array { (Variant)new BoxShape3D(), (Variant)Transform3D.Identity });
        T.Check("mesh transform and shapes", ml.GetItemMeshTransform(3).Origin == new Vector3(0, 0.5f, 0) && ml.GetItemShapes(3).Count == 2);
        ml.RemoveItem(0);
        T.Check("remove_item", ml.GetItemList().SequenceEqual(new[] { 3 }));
        ml.CreateItem(7);
        ml.Clear();
        T.Check("clear", ml.GetItemList().Length == 0);
    }

    private static void NodeProperties()
    {
        // the converter's property names reach the classes by reflection
        var box = (CSGBox3D)ClassDB.Build("CSGBox3D", new (string, Variant)[] { ("size", new Vector3(2, 3, 4)), ("operation", 2), ("use_collision", true), ("collision_layer", 4), ("invert_faces", true) }).AsGodotObject()!;
        T.Check("CSGBox3D: size, operation, use_collision, layer, invert_faces",
            box.Size == new Vector3(2, 3, 4) && box.Operation == CSGShape3D.OperationEnum.Subtraction && box.UseCollision && box.CollisionLayer == 4 && box.InvertFaces);
        var poly = (CSGPolygon3D)ClassDB.Build("CSGPolygon3D", new (string, Variant)[] { ("polygon", new Vector2[] { new(0, 0), new(2, 0), new(2, 1) }), ("mode", 1), ("spin_degrees", 90f), ("depth", 3f) }).AsGodotObject()!;
        T.Check("CSGPolygon3D: polygon, mode, spin, depth", poly.Polygon.Length == 3 && poly.Mode == CSGPolygon3D.ModeEnum.Spin && poly.SpinDegrees == 90f && poly.Depth == 3f);
        var sprite = (Sprite3D)ClassDB.Build("Sprite3D", new (string, Variant)[] { ("pixel_size", 0.5f), ("billboard", 2), ("hframes", 2), ("frame", 1), ("alpha_cut", 1), ("flip_h", true), ("centered", false), ("offset", new Vector2(4, 2)) }).AsGodotObject()!;
        T.Check("Sprite3D: pixel_size, billboard, frames, alpha cut, flip, centred, offset",
            sprite.PixelSize == 0.5f && sprite.Billboard == BaseMaterial3D.BillboardModeEnum.FixedY && sprite.Hframes == 2 && sprite.Frame == 1 && sprite.AlphaCut == SpriteBase3D.AlphaCutMode.Discard &&
            sprite.FlipH && !sprite.Centered && sprite.Offset == new Vector2(4, 2));
        var label = (Label3D)ClassDB.Build("Label3D", new (string, Variant)[] { ("text", "Hello"), ("pixel_size", 0.02f), ("font_size", 48), ("horizontal_alignment", 0), ("modulate", new Color(1, 0, 0, 1)) }).AsGodotObject()!;
        T.Check("Label3D: text, pixel_size, font_size, alignment, modulate", label.Text == "Hello" && label.PixelSize == 0.02f && label.FontSize == 48 && label.HorizontalAlignment == HorizontalAlignment.Left && label.Modulate == new Color(1, 0, 0, 1));
        var grid = (GridMap)ClassDB.Build("GridMap", new (string, Variant)[] { ("cell_size", new Vector3(4, 4, 4)), ("cell_center_y", false), ("collision_layer", 2) }).AsGodotObject()!;
        T.Check("GridMap: cell_size, centring, collision layer", grid.CellSize == new Vector3(4, 4, 4) && !grid.CellCenterY && grid.CollisionLayer == 2);
        T.Check("none of that warned about a property the class lacks", T.Logs.All(l => !l.Message.Contains("has no property")));
        T.Check("Sprite3D.GetItemRect: 1 frame of an unknown-size texture is empty", sprite.GetItemRect().Size == Vector2.Zero);
    }

    private static void NoEngine()
    {
        RuntimeMeshBackend.Override = null;
        var mesh = new ArrayMesh();
        mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, Arrays(Tri));
        T.Check("without an engine an ArrayMesh is held in memory and has no engine path", mesh.GetSurfaceCount() == 1 && MeshInstance3D.MeshAssetOf(mesh) == null);
        var mm = new MultiMesh { InstanceCount = 2 };
        mm.SetInstanceTransform(0, Transform3D.Identity);
        T.Check("and a MultiMesh keeps its table", mm.EngineAssetPath == null && mm.GetInstanceTransform(1).IsEqualApprox(Transform3D.Identity));
        var mi = new MeshInstance3D { Mesh = mesh };
        var mmi = new MultiMeshInstance3D { Multimesh = mm };
        T.Check("the instance nodes accept them without an engine", mi.Mesh == mesh && mmi.Multimesh == mm);
    }
}
