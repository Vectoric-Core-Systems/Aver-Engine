// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Runtime mesh building (G4_RENDER.md R7): ArrayMesh, ImmediateMesh, SurfaceTool and MeshDataTool with Godot's array layout. A surface is kept in Godot numbers;
// the engine is handed the same surface in its own space (centimetres, +Z up) through the runtime-mesh ABI. Godot's front faces are clockwise and the axis map
// has determinant -1, so a triangle list goes to the engine in the order it is stored.

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>A surface as the engine takes it: engine-space arrays. <c>Pos</c> is 3 floats per vertex, <c>Nrm</c> 3, <c>Uv</c> 2, <c>Col</c> 4, <c>Uv1</c> 2, <c>Tan</c> 4.</summary>
internal sealed class EngineSurface
{
    public float[] Pos = System.Array.Empty<float>();
    public float[]? Nrm, Uv, Col, Uv1, Tan;
    public uint[]? Idx;
    public string? Material;
}

/// <summary>The calls the runtime-mesh classes make on the engine. The engine's is <see cref="NativeRuntimeMeshBackend"/>; a test installs a recording one.</summary>
internal interface IRuntimeMeshBackend
{
    int CreateMesh(string key);
    bool DestroyMesh(int id);
    bool ClearMesh(int id);
    int AddSurface(int id, EngineSurface surface);

    int CreateMulti(string key);
    bool DestroyMulti(int id);
    bool SetSource(int id, string meshPath);
    bool SetFlags(int id, bool useColors, bool useCustom);
    bool SetCount(int id, int count);
    bool SetVisible(int id, int count);
    bool SetBuffer(int id, int count, float[]? transforms12, float[]? colors4, float[]? custom4);
    bool SetTransform(int id, int index, float[] t12);
    bool SetColor(int id, int index, Color color);
    bool SetCustom(int id, int index, Color custom);
    bool TryGetInfo(int id, out int instances, out int visible, out int drawn);
}

internal sealed class NativeRuntimeMeshBackend : IRuntimeMeshBackend
{
    public int CreateMesh(string key) => RuntimeMeshes.Create(key);
    public bool DestroyMesh(int id) => RuntimeMeshes.Destroy(id);
    public bool ClearMesh(int id) => RuntimeMeshes.Clear(id);
    public int AddSurface(int id, EngineSurface s) => RuntimeMeshes.AddSurface(id, s.Pos, s.Nrm, s.Uv, s.Col, s.Uv1, s.Tan, s.Idx, s.Material);

    public int CreateMulti(string key) => MultiMeshes.Create(key);
    public bool DestroyMulti(int id) => MultiMeshes.Destroy(id);
    public bool SetSource(int id, string meshPath) => MultiMeshes.SetSource(id, meshPath);
    public bool SetFlags(int id, bool useColors, bool useCustom) => MultiMeshes.SetFlags(id, useColors, useCustom);
    public bool SetCount(int id, int count) => MultiMeshes.SetCount(id, count);
    public bool SetVisible(int id, int count) => MultiMeshes.SetVisible(id, count);
    public bool SetBuffer(int id, int count, float[]? t, float[]? c, float[]? u) => MultiMeshes.SetBuffer(id, count, t, c, u);
    public bool SetTransform(int id, int index, float[] t12) => MultiMeshes.SetTransform(id, index, t12);
    public bool SetColor(int id, int index, Color c) => MultiMeshes.SetColor(id, index, c.R, c.G, c.B, c.A);
    public bool SetCustom(int id, int index, Color c) => MultiMeshes.SetCustom(id, index, c.R, c.G, c.B, c.A);
    public bool TryGetInfo(int id, out int instances, out int visible, out int drawn) => MultiMeshes.TryGetInfo(id, out instances, out visible, out drawn, out _);
}

internal static class RuntimeMeshBackend
{
    private static readonly NativeRuntimeMeshBackend s_native = new();

    /// <summary>A test's recording backend, or null for the engine's.</summary>
    internal static IRuntimeMeshBackend? Override;

    /// <summary>Where calls go: the override, else the engine when nodes live in it, else nowhere (a headless tree builds meshes in memory only).</summary>
    internal static IRuntimeMeshBackend? Current => Override ?? (NodeHost.Current.IsEngine ? s_native : null);
}

/// <summary>One surface of an ArrayMesh or ImmediateMesh in Godot numbers.</summary>
internal sealed class MeshSurface
{
    public Mesh.PrimitiveType Primitive = Mesh.PrimitiveType.Triangles;
    public Vector3[] Vertices = System.Array.Empty<Vector3>();
    public Vector3[]? Normals;
    public float[]? Tangents;      // 4 per vertex: xyz and handedness
    public Color[]? Colors;
    public Vector2[]? Uv, Uv2;
    public int[]? Indices;
    public int[]? Bones;
    public float[]? Weights;
    public Material? Material;
    public string Name = "";

    private static bool Present(Variant v) => !v.IsNil;

    /// <summary>Reads Godot's array layout (index = <see cref="Mesh.ArrayType"/>). Null with the reason when the arrays do not make a surface.</summary>
    internal static MeshSurface? FromArrays(Mesh.PrimitiveType primitive, Collections.Array arrays, out string error)
    {
        error = "";
        Variant Slot(Mesh.ArrayType t) => (int)t < arrays.Count ? arrays[(int)t] : default;
        Variant vtx = Slot(Mesh.ArrayType.Vertex);
        if (!Present(vtx)) { error = "the vertex array is empty"; return null; }
        var s = new MeshSurface { Primitive = primitive, Vertices = vtx.AsVector3Array() };
        int n = s.Vertices.Length;
        if (n == 0) { error = "the vertex array is empty"; return null; }
        Variant v = Slot(Mesh.ArrayType.Normal);
        if (Present(v)) { s.Normals = v.AsVector3Array(); if (s.Normals.Length == 0) s.Normals = null; else if (s.Normals.Length != n) { error = "the normal array has " + s.Normals.Length + " entries for " + n + " vertices"; return null; } }
        v = Slot(Mesh.ArrayType.Tangent);
        if (Present(v)) { s.Tangents = v.AsFloat32Array(); if (s.Tangents.Length == 0) s.Tangents = null; else if (s.Tangents.Length != n * 4) { error = "the tangent array needs 4 floats per vertex"; return null; } }
        v = Slot(Mesh.ArrayType.Color);
        if (Present(v)) { s.Colors = v.AsColorArray(); if (s.Colors.Length == 0) s.Colors = null; else if (s.Colors.Length != n) { error = "the colour array does not match the vertex count"; return null; } }
        v = Slot(Mesh.ArrayType.TexUV);
        if (Present(v)) { s.Uv = v.AsVector2Array(); if (s.Uv.Length == 0) s.Uv = null; else if (s.Uv.Length != n) { error = "the UV array does not match the vertex count"; return null; } }
        v = Slot(Mesh.ArrayType.TexUV2);
        if (Present(v)) { s.Uv2 = v.AsVector2Array(); if (s.Uv2.Length == 0) s.Uv2 = null; else if (s.Uv2.Length != n) { error = "the UV2 array does not match the vertex count"; return null; } }
        v = Slot(Mesh.ArrayType.Bones);
        if (Present(v)) { int[] b = v.AsInt32Array(); if (b.Length == 0) b = v.AsFloat32Array().Select(f => (int)f).ToArray(); s.Bones = b.Length == 0 ? null : b; }
        v = Slot(Mesh.ArrayType.Weights);
        if (Present(v)) { float[] w = v.AsFloat32Array(); s.Weights = w.Length == 0 ? null : w; }
        v = Slot(Mesh.ArrayType.Index);
        if (Present(v))
        {
            s.Indices = v.AsInt32Array();
            if (s.Indices.Length == 0) s.Indices = null;
            else foreach (int i in s.Indices) if (i < 0 || i >= n) { error = "an index (" + i + ") is outside the " + n + " vertices"; return null; }
        }
        if (primitive == Mesh.PrimitiveType.Triangles && (s.Indices?.Length ?? n) % 3 != 0) { error = "a triangle surface needs a multiple of three indices (or vertices)"; return null; }
        return s;
    }

    /// <summary>The arrays back out in Godot's layout.</summary>
    internal Collections.Array ToArrays()
    {
        var a = new Collections.Array();
        for (int i = 0; i < (int)Mesh.ArrayType.Max; i++) a.Add(default);
        a[(int)Mesh.ArrayType.Vertex] = (Variant)(Vector3[])Vertices.Clone();
        if (Normals != null) a[(int)Mesh.ArrayType.Normal] = (Variant)(Vector3[])Normals.Clone();
        if (Tangents != null) a[(int)Mesh.ArrayType.Tangent] = (Variant)(float[])Tangents.Clone();
        if (Colors != null) a[(int)Mesh.ArrayType.Color] = (Variant)(Color[])Colors.Clone();
        if (Uv != null) a[(int)Mesh.ArrayType.TexUV] = (Variant)(Vector2[])Uv.Clone();
        if (Uv2 != null) a[(int)Mesh.ArrayType.TexUV2] = (Variant)(Vector2[])Uv2.Clone();
        if (Bones != null) a[(int)Mesh.ArrayType.Bones] = (Variant)(int[])Bones.Clone();
        if (Weights != null) a[(int)Mesh.ArrayType.Weights] = (Variant)(float[])Weights.Clone();
        if (Indices != null) a[(int)Mesh.ArrayType.Index] = (Variant)(int[])Indices.Clone();
        return a;
    }

    internal Aabb Bounds()
    {
        if (Vertices.Length == 0) return default;
        Vector3 lo = Vertices[0], hi = Vertices[0];
        foreach (Vector3 p in Vertices)
        {
            lo = new Vector3(MathF.Min(lo.X, p.X), MathF.Min(lo.Y, p.Y), MathF.Min(lo.Z, p.Z));
            hi = new Vector3(MathF.Max(hi.X, p.X), MathF.Max(hi.Y, p.Y), MathF.Max(hi.Z, p.Z));
        }
        return new Aabb(lo, hi - lo);
    }

    /// <summary>The surface in engine space, or null with why not (only triangle surfaces are drawn). Normals are left out when there are none (the engine computes
    /// smooth ones); a tangent needs a normal.</summary>
    internal EngineSurface? ToEngine(out string why)
    {
        why = "";
        if (Primitive != Mesh.PrimitiveType.Triangles && Primitive != Mesh.PrimitiveType.TriangleStrip)
        {
            why = "only triangle surfaces are drawn (" + Primitive + " has no counterpart)";
            return null;
        }
        int n = Vertices.Length;
        var e = new EngineSurface { Pos = new float[n * 3] };
        for (int i = 0; i < n; i++)
        {
            Aver.Scene.Vec3 p = FabricSpace.ToAverPosition(Vertices[i]);
            e.Pos[i * 3] = p.X; e.Pos[i * 3 + 1] = p.Y; e.Pos[i * 3 + 2] = p.Z;
        }
        if (Normals != null)
        {
            e.Nrm = new float[n * 3];
            for (int i = 0; i < n; i++)
            {
                Aver.Scene.Vec3 d = FabricSpace.ToAverDirection(Normals[i]);
                e.Nrm[i * 3] = d.X; e.Nrm[i * 3 + 1] = d.Y; e.Nrm[i * 3 + 2] = d.Z;
            }
        }
        if (Uv != null) { e.Uv = new float[n * 2]; for (int i = 0; i < n; i++) { e.Uv[i * 2] = Uv[i].X; e.Uv[i * 2 + 1] = Uv[i].Y; } }
        if (Uv2 != null) { e.Uv1 = new float[n * 2]; for (int i = 0; i < n; i++) { e.Uv1[i * 2] = Uv2[i].X; e.Uv1[i * 2 + 1] = Uv2[i].Y; } }
        if (Colors != null) { e.Col = new float[n * 4]; for (int i = 0; i < n; i++) { e.Col[i * 4] = Colors[i].R; e.Col[i * 4 + 1] = Colors[i].G; e.Col[i * 4 + 2] = Colors[i].B; e.Col[i * 4 + 3] = Colors[i].A; } }
        if (Tangents != null && Normals != null)
        {
            e.Tan = new float[n * 4];
            for (int i = 0; i < n; i++)
                FabricSpace.ToAverTangent(Normals[i], new Vector3(Tangents[i * 4], Tangents[i * 4 + 1], Tangents[i * 4 + 2]), Tangents[i * 4 + 3] < 0f ? -1f : 1f, e.Tan, i * 4);
        }
        if (Primitive == Mesh.PrimitiveType.TriangleStrip)
        {
            int count = Indices?.Length ?? n;
            var list = new List<uint>();
            for (int i = 0; i + 2 < count; i++)
            {
                uint a = (uint)(Indices != null ? Indices[i] : i), b = (uint)(Indices != null ? Indices[i + 1] : i + 1), c = (uint)(Indices != null ? Indices[i + 2] : i + 2);
                if (a == b || b == c || a == c) continue;   // a degenerate triangle joins strips
                if ((i & 1) == 0) { list.Add(a); list.Add(b); list.Add(c); }
                else { list.Add(b); list.Add(a); list.Add(c); }
            }
            if (list.Count == 0) { why = "the strip has no triangle"; return null; }
            e.Idx = list.ToArray();
        }
        else if (Indices != null)
        {
            e.Idx = new uint[Indices.Length];
            for (int i = 0; i < Indices.Length; i++) e.Idx[i] = (uint)Indices[i];
        }
        else if (n % 3 != 0)
        {
            why = "a triangle surface without indices needs a multiple of three vertices";
            return null;
        }
        if (Material is { ResourcePath.Length: > 0 }) e.Material = System.IO.Path.GetFileNameWithoutExtension(Material.ResourcePath);
        else if (Material is { ResourceName.Length: > 0 }) e.Material = Material.ResourceName;
        return e;
    }
}

/// <summary>Keeps one engine mesh in step with a list of surfaces.</summary>
internal sealed class EngineMeshLink
{
    private int _id;
    private string _key = "";
    private bool _warned;

    internal string? Key => _id != 0 ? _key : null;

    /// <summary>The runtime key for this mesh, creating the engine mesh (and sending every surface so far) the first time. Null without an engine.</summary>
    internal string? Ensure(string prefix, ulong instanceId, IReadOnlyList<MeshSurface> surfaces)
    {
        IRuntimeMeshBackend? b = RuntimeMeshBackend.Current;
        if (b == null) return null;
        if (_id == 0)
        {
            _key = prefix + "/" + instanceId;
            _id = b.CreateMesh(_key);
            if (_id == 0) return null;
            foreach (MeshSurface s in surfaces) Push(b, s);
        }
        return RuntimeMeshes.PathOf(_key);
    }

    internal void Added(MeshSurface s)
    {
        if (_id == 0) return;
        IRuntimeMeshBackend? b = RuntimeMeshBackend.Current;
        if (b != null) Push(b, s);
    }

    internal void Reset(IReadOnlyList<MeshSurface> surfaces)
    {
        if (_id == 0) return;
        IRuntimeMeshBackend? b = RuntimeMeshBackend.Current;
        if (b == null) return;
        b.ClearMesh(_id);
        foreach (MeshSurface s in surfaces) Push(b, s);
    }

    private void Push(IRuntimeMeshBackend b, MeshSurface s)
    {
        EngineSurface? e = s.ToEngine(out string why);
        if (e == null)
        {
            if (!_warned) { _warned = true; Aver.Scripting.Log.Warn($"[Fabric] mesh '{_key}': a surface is not drawn: {why}"); }
            return;
        }
        if (b.AddSurface(_id, e) < 0) Aver.Scripting.Log.Warn($"[Fabric] mesh '{_key}': the engine refused a surface of {s.Vertices.Length} vertices");
    }
}

/// <summary>A mesh built from arrays (Godot's ArrayMesh). Surfaces are added with <see cref="AddSurfaceFromArrays"/> in Godot's array layout (vertex, normal,
/// tangent as 4 floats per vertex, colour, UV, UV2, bones, weights, index). Triangle and triangle-strip surfaces are drawn; points and lines are kept and not drawn.
/// Custom arrays, bone weights, blend shapes and LODs are accepted and ignored.</summary>
public class ArrayMesh : Mesh
{
    private readonly List<MeshSurface> _surfaces = new();
    private readonly EngineMeshLink _link = new();

    public Aabb CustomAabb { get; set; }

    public void AddSurfaceFromArrays(PrimitiveType primitive, Collections.Array arrays, Collections.Array<Collections.Array>? blendShapes = null, Collections.Dictionary? lods = null, ArrayFormat flags = 0)
    {
        MeshSurface? s = MeshSurface.FromArrays(primitive, arrays, out string error);
        if (s == null)
        {
            GD.PushError("ArrayMesh.add_surface_from_arrays: " + error);
            return;
        }
        _surfaces.Add(s);
        _link.Added(s);
        EmitChanged();
    }

    public override int GetSurfaceCount() => _surfaces.Count;

    public void ClearSurfaces()
    {
        _surfaces.Clear();
        _link.Reset(_surfaces);
        EmitChanged();
    }

    public void SurfaceRemove(int surfIdx)
    {
        if (surfIdx < 0 || surfIdx >= _surfaces.Count) { GD.PushError("ArrayMesh.surface_remove: no surface " + surfIdx); return; }
        _surfaces.RemoveAt(surfIdx);
        _link.Reset(_surfaces);
        EmitChanged();
    }

    public Collections.Array SurfaceGetArrays(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].ToArrays() : new Collections.Array();
    public int SurfaceGetArrayLen(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Vertices.Length : 0;
    public int SurfaceGetArrayIndexLen(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Indices?.Length ?? 0 : 0;
    public PrimitiveType SurfaceGetPrimitiveType(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Primitive : PrimitiveType.Triangles;
    public int GetBlendShapeCount() => 0;

    public void SurfaceSetName(int surfIdx, string name) { if (surfIdx >= 0 && surfIdx < _surfaces.Count) _surfaces[surfIdx].Name = name; }
    public string SurfaceGetName(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Name : "";
    public int SurfaceFindByName(string name) => _surfaces.FindIndex(s => s.Name == name);

    public override Material? SurfaceGetMaterial(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Material : null;

    public override void SurfaceSetMaterial(int surfIdx, Material? material)
    {
        if (surfIdx < 0 || surfIdx >= _surfaces.Count) { GD.PushError("ArrayMesh.surface_set_material: no surface " + surfIdx); return; }
        _surfaces[surfIdx].Material = material;
        _link.Reset(_surfaces);
        EmitChanged();
    }

    public override Aabb GetAabb()
    {
        if (CustomAabb.Size != Vector3.Zero) return CustomAabb;
        bool first = true;
        Aabb all = default;
        foreach (MeshSurface s in _surfaces)
        {
            Aabb b = s.Bounds();
            if (first) { all = b; first = false; }
            else all = all.Merge(b);
        }
        return all;
    }

    internal override string? EngineAssetPath => _link.Ensure("array", GetInstanceId(), _surfaces);
    internal IReadOnlyList<MeshSurface> Surfaces => _surfaces;
}

/// <summary>A mesh a script fills every frame (Godot's ImmediateMesh): <see cref="SurfaceBegin"/>, the setters and <see cref="SurfaceAddVertex"/> for each vertex,
/// <see cref="SurfaceEnd"/>. Triangle and triangle-strip surfaces are drawn; points and lines are kept and not drawn. Each end re-sends the surface, so build
/// what you can once and clear only when it changes.</summary>
public class ImmediateMesh : Mesh
{
    private readonly List<MeshSurface> _surfaces = new();
    private readonly EngineMeshLink _link = new();
    private PrimitiveType _primitive;
    private Material? _material;
    private bool _open;
    private readonly List<Vector3> _v = new(), _n = new();
    private readonly List<float> _t = new();
    private readonly List<Color> _c = new();
    private readonly List<Vector2> _uv = new(), _uv2 = new();
    private Vector3 _cn;
    private Plane _ct = new(0, 0, 0, 1);
    private Color _cc = new(1, 1, 1, 1);
    private Vector2 _cuv, _cuv2;
    private bool _hasN, _hasT, _hasC, _hasUv, _hasUv2;

    public void SurfaceBegin(PrimitiveType primitive, Material? material = null)
    {
        if (_open) { GD.PushError("ImmediateMesh.surface_begin: a surface is already open"); return; }
        _open = true;
        _primitive = primitive;
        _material = material;
        _v.Clear(); _n.Clear(); _t.Clear(); _c.Clear(); _uv.Clear(); _uv2.Clear();
        _hasN = _hasT = _hasC = _hasUv = _hasUv2 = false;
        _cn = default; _ct = new Plane(0, 0, 0, 1); _cc = new Color(1, 1, 1, 1); _cuv = default; _cuv2 = default;
    }

    public void SurfaceSetNormal(Vector3 normal) { _cn = normal; _hasN = true; }
    public void SurfaceSetTangent(Plane tangent) { _ct = tangent; _hasT = true; }
    public void SurfaceSetColor(Color color) { _cc = color; _hasC = true; }
    public void SurfaceSetUV(Vector2 uv) { _cuv = uv; _hasUv = true; }
    public void SurfaceSetUV2(Vector2 uv2) { _cuv2 = uv2; _hasUv2 = true; }

    public void SurfaceAddVertex(Vector3 vertex)
    {
        if (!_open) { GD.PushError("ImmediateMesh.surface_add_vertex: call surface_begin first"); return; }
        _v.Add(vertex); _n.Add(_cn);
        _t.Add(_ct.Normal.X); _t.Add(_ct.Normal.Y); _t.Add(_ct.Normal.Z); _t.Add(_ct.D);
        _c.Add(_cc); _uv.Add(_cuv); _uv2.Add(_cuv2);
    }

    public void SurfaceAddVertex2D(Vector2 vertex) => SurfaceAddVertex(new Vector3(vertex.X, vertex.Y, 0f));

    public void SurfaceEnd()
    {
        if (!_open) { GD.PushError("ImmediateMesh.surface_end: no surface is open"); return; }
        _open = false;
        if (_v.Count == 0) return;
        var s = new MeshSurface
        {
            Primitive = _primitive, Vertices = _v.ToArray(), Material = _material,
            Normals = _hasN ? _n.ToArray() : null, Tangents = _hasT ? _t.ToArray() : null, Colors = _hasC ? _c.ToArray() : null,
            Uv = _hasUv ? _uv.ToArray() : null, Uv2 = _hasUv2 ? _uv2.ToArray() : null,
        };
        _surfaces.Add(s);
        _link.Added(s);
        EmitChanged();
    }

    public void ClearSurfaces()
    {
        _surfaces.Clear();
        _open = false;
        _link.Reset(_surfaces);
        EmitChanged();
    }

    public override int GetSurfaceCount() => _surfaces.Count;
    public override Material? SurfaceGetMaterial(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Material : null;

    public override void SurfaceSetMaterial(int surfIdx, Material? material)
    {
        if (surfIdx < 0 || surfIdx >= _surfaces.Count) return;
        _surfaces[surfIdx].Material = material;
        _link.Reset(_surfaces);
    }

    public PrimitiveType SurfaceGetPrimitiveType(int surfIdx) => surfIdx >= 0 && surfIdx < _surfaces.Count ? _surfaces[surfIdx].Primitive : PrimitiveType.Triangles;

    public override Aabb GetAabb()
    {
        bool first = true;
        Aabb all = default;
        foreach (MeshSurface s in _surfaces)
        {
            Aabb b = s.Bounds();
            if (first) { all = b; first = false; }
            else all = all.Merge(b);
        }
        return all;
    }

    internal override string? EngineAssetPath => _link.Ensure("immediate", GetInstanceId(), _surfaces);
    internal IReadOnlyList<MeshSurface> Surfaces => _surfaces;
}

/// <summary>Builds a mesh a vertex at a time (Godot's SurfaceTool). The setters apply to the vertices added after them; <see cref="GenerateNormals"/> and
/// <see cref="GenerateTangents"/> follow Godot's rules (clockwise front faces, smooth groups) but are not bit-identical to its generators (the tangents are
/// accumulated per vertex from the UV derivatives, not by MikkTSpace). Bone indices and weights, custom channels and blend shapes are not kept.</summary>
public class SurfaceTool : RefCounted
{
    private struct Vert
    {
        public Vector3 V, N;
        public float Tx, Ty, Tz, Tw;
        public Color C;
        public Vector2 Uv, Uv2;
        public int Smooth;
    }

    private readonly List<Vert> _verts = new();
    private readonly List<int> _indices = new();
    private Mesh.PrimitiveType _primitive = Mesh.PrimitiveType.Triangles;
    private bool _began;
    private Material? _material;
    private bool _hasN, _hasT, _hasC, _hasUv, _hasUv2;
    private Vert _cur;

    private void ResetCurrent()
    {
        _cur = new Vert { C = new Color(1, 1, 1, 1), Tw = 1f };
        _hasN = _hasT = _hasC = _hasUv = _hasUv2 = false;
    }

    public void Begin(Mesh.PrimitiveType primitive)
    {
        Clear();
        _primitive = primitive;
        _began = true;
    }

    public void Clear()
    {
        _verts.Clear();
        _indices.Clear();
        _material = null;
        _began = false;
        ResetCurrent();
    }

    public void SetNormal(Vector3 normal) { _cur.N = normal; _hasN = true; }
    public void SetTangent(Plane tangent) { _cur.Tx = tangent.Normal.X; _cur.Ty = tangent.Normal.Y; _cur.Tz = tangent.Normal.Z; _cur.Tw = tangent.D; _hasT = true; }
    public void SetColor(Color color) { _cur.C = color; _hasC = true; }
    public void SetUV(Vector2 uv) { _cur.Uv = uv; _hasUv = true; }
    public void SetUV2(Vector2 uv2) { _cur.Uv2 = uv2; _hasUv2 = true; }

    /// <summary>-1 makes the next vertices flat-shaded; 0 (the default) and above smooth with the vertices of the same group at the same position.</summary>
    public void SetSmoothGroup(uint index) => _cur.Smooth = unchecked((int)index);

    public void SetMaterial(Material? material) => _material = material;

    public void AddVertex(Vector3 vertex)
    {
        if (!_began) { GD.PushError("SurfaceTool.add_vertex: call begin first"); return; }
        Vert v = _cur;
        v.V = vertex;
        _verts.Add(v);
    }

    public void AddIndex(int index)
    {
        if (!_began) { GD.PushError("SurfaceTool.add_index: call begin first"); return; }
        _indices.Add(index);
    }

    /// <summary>Adds a fan of triangles around the first vertex.</summary>
    public void AddTriangleFan(Vector3[] vertices, Vector2[]? uvs = null, Color[]? colors = null, Vector2[]? uv2s = null, Vector3[]? normals = null, Plane[]? tangents = null)
    {
        if (_primitive != Mesh.PrimitiveType.Triangles) { GD.PushError("SurfaceTool.add_triangle_fan: the primitive type must be Triangles"); return; }
        for (int i = 1; i + 1 < vertices.Length; i++)
            foreach (int k in new[] { 0, i, i + 1 })
            {
                if (colors != null && k < colors.Length) SetColor(colors[k]);
                if (uvs != null && k < uvs.Length) SetUV(uvs[k]);
                if (uv2s != null && k < uv2s.Length) SetUV2(uv2s[k]);
                if (normals != null && k < normals.Length) SetNormal(normals[k]);
                if (tangents != null && k < tangents.Length) SetTangent(tangents[k]);
                AddVertex(vertices[k]);
            }
    }

    /// <summary>Merges vertices that are identical in every stream and fills the index list.</summary>
    public void Index()
    {
        if (_indices.Count > 0) return;
        var map = new Dictionary<(Vector3, Vector3, float, float, float, float, Color, Vector2, Vector2, int), int>();
        var unique = new List<Vert>();
        foreach (Vert v in _verts)
        {
            var key = (v.V, v.N, v.Tx, v.Ty, v.Tz, v.Tw, v.C, v.Uv, v.Uv2, v.Smooth);
            if (!map.TryGetValue(key, out int at)) { at = unique.Count; unique.Add(v); map[key] = at; }
            _indices.Add(at);
        }
        _verts.Clear();
        _verts.AddRange(unique);
    }

    /// <summary>Expands the index list back into one vertex per index.</summary>
    public void Deindex()
    {
        if (_indices.Count == 0) return;
        var expanded = new List<Vert>(_indices.Count);
        foreach (int i in _indices) if (i >= 0 && i < _verts.Count) expanded.Add(_verts[i]);
        _verts.Clear();
        _verts.AddRange(expanded);
        _indices.Clear();
    }

    private int TriangleCount => (_indices.Count > 0 ? _indices.Count : _verts.Count) / 3;
    private int At(int k) => _indices.Count > 0 ? _indices[k] : k;

    /// <summary>Face normals added up per position and smooth group; Godot's front faces are clockwise, so a face normal is cross(c - a, b - a). Flat groups (-1)
    /// keep their face normal. <paramref name="flip"/> negates the result.</summary>
    public void GenerateNormals(bool flip = false)
    {
        if (_primitive != Mesh.PrimitiveType.Triangles) { GD.PushError("SurfaceTool.generate_normals: only triangle surfaces have normals to generate"); return; }
        // Work on a deindexed copy so that a vertex used by several faces can take a different normal in each (flat) or share (smooth).
        bool wasIndexed = _indices.Count > 0;
        Deindex();
        var faceN = new Vector3[_verts.Count / 3];
        for (int t = 0; t < faceN.Length; t++)
        {
            Vector3 a = _verts[t * 3].V, b = _verts[t * 3 + 1].V, c = _verts[t * 3 + 2].V;
            faceN[t] = (c - a).Cross(b - a).Normalized();
        }
        var sums = new Dictionary<(Vector3, int), Vector3>();
        for (int i = 0; i < faceN.Length * 3; i++)
        {
            Vert v = _verts[i];
            if (v.Smooth < 0) continue;
            var key = (v.V, v.Smooth);
            sums[key] = sums.TryGetValue(key, out Vector3 s) ? s + faceN[i / 3] : faceN[i / 3];
        }
        for (int i = 0; i < faceN.Length * 3; i++)
        {
            Vert v = _verts[i];
            Vector3 n = v.Smooth < 0 ? faceN[i / 3] : sums[(v.V, v.Smooth)].Normalized();
            v.N = flip ? -n : n;
            _verts[i] = v;
        }
        _hasN = true;
        if (wasIndexed) Index();
    }

    /// <summary>Tangents from the UV derivatives: T along increasing u, handedness chosen so that cross(N, T) * w points at decreasing v (Godot's and glTF's
    /// normal-map convention). Needs normals and UVs.</summary>
    public void GenerateTangents()
    {
        if (!_hasUv || !_hasN) { GD.PushError("SurfaceTool.generate_tangents: UVs and normals are needed first"); return; }
        if (_primitive != Mesh.PrimitiveType.Triangles) { GD.PushError("SurfaceTool.generate_tangents: only triangle surfaces have tangents to generate"); return; }
        var tAcc = new Vector3[_verts.Count];
        var bAcc = new Vector3[_verts.Count];
        for (int t = 0; t < TriangleCount; t++)
        {
            int ia = At(t * 3), ib = At(t * 3 + 1), ic = At(t * 3 + 2);
            if (ia < 0 || ib < 0 || ic < 0 || ia >= _verts.Count || ib >= _verts.Count || ic >= _verts.Count) continue;
            Vector3 e1 = _verts[ib].V - _verts[ia].V, e2 = _verts[ic].V - _verts[ia].V;
            Vector2 d1 = _verts[ib].Uv - _verts[ia].Uv, d2 = _verts[ic].Uv - _verts[ia].Uv;
            float det = d1.X * d2.Y - d2.X * d1.Y;
            if (MathF.Abs(det) < 1e-20f) continue;
            float inv = 1f / det;
            Vector3 tan = (e1 * d2.Y - e2 * d1.Y) * inv;     // dP/du
            Vector3 dPdv = (e2 * d1.X - e1 * d2.X) * inv;    // dP/dv
            foreach (int i in new[] { ia, ib, ic }) { tAcc[i] += tan; bAcc[i] += dPdv; }
        }
        for (int i = 0; i < _verts.Count; i++)
        {
            Vert v = _verts[i];
            Vector3 n = v.N;
            Vector3 t = tAcc[i] - n * n.Dot(tAcc[i]);
            if (t.LengthSquared() < 1e-20f) { v.Tx = v.Ty = v.Tz = 0f; v.Tw = 1f; _verts[i] = v; continue; }
            t = t.Normalized();
            Vector3 bitangent = -bAcc[i];   // decreasing v
            v.Tx = t.X; v.Ty = t.Y; v.Tz = t.Z;
            v.Tw = n.Cross(t).Dot(bitangent) < 0f ? -1f : 1f;
            _verts[i] = v;
        }
        _hasT = true;
    }

    private Collections.Array BuildArrays()
    {
        int n = _verts.Count;
        var a = new Collections.Array();
        for (int i = 0; i < (int)Mesh.ArrayType.Max; i++) a.Add(default);
        a[(int)Mesh.ArrayType.Vertex] = (Variant)_verts.Select(v => v.V).ToArray();
        if (_hasN) a[(int)Mesh.ArrayType.Normal] = (Variant)_verts.Select(v => v.N).ToArray();
        if (_hasT)
        {
            var t = new float[n * 4];
            for (int i = 0; i < n; i++) { t[i * 4] = _verts[i].Tx; t[i * 4 + 1] = _verts[i].Ty; t[i * 4 + 2] = _verts[i].Tz; t[i * 4 + 3] = _verts[i].Tw; }
            a[(int)Mesh.ArrayType.Tangent] = (Variant)t;
        }
        if (_hasC) a[(int)Mesh.ArrayType.Color] = (Variant)_verts.Select(v => v.C).ToArray();
        if (_hasUv) a[(int)Mesh.ArrayType.TexUV] = (Variant)_verts.Select(v => v.Uv).ToArray();
        if (_hasUv2) a[(int)Mesh.ArrayType.TexUV2] = (Variant)_verts.Select(v => v.Uv2).ToArray();
        if (_indices.Count > 0) a[(int)Mesh.ArrayType.Index] = (Variant)_indices.ToArray();
        return a;
    }

    /// <summary>The arrays in Godot's layout, ready for <see cref="ArrayMesh.AddSurfaceFromArrays"/>.</summary>
    public Collections.Array CommitToArrays() => BuildArrays();

    /// <summary>Adds the surface to <paramref name="existing"/> (or a new <see cref="ArrayMesh"/>) and returns the mesh.</summary>
    public ArrayMesh? Commit(ArrayMesh? existing = null, Mesh.ArrayFormat flags = 0)
    {
        if (_verts.Count == 0) { GD.PushError("SurfaceTool.commit: no vertices were added"); return existing; }
        ArrayMesh mesh = existing ?? new ArrayMesh();
        int before = mesh.GetSurfaceCount();
        mesh.AddSurfaceFromArrays(_primitive, BuildArrays(), null, null, flags);
        if (mesh.GetSurfaceCount() > before && _material != null) mesh.SurfaceSetMaterial(before, _material);
        return mesh;
    }

    /// <summary>Starts from a surface of an ArrayMesh (the arrays, the primitive and the material).</summary>
    public void CreateFrom(Mesh existing, int surface)
    {
        if (existing is not ArrayMesh am || surface < 0 || surface >= am.Surfaces.Count) { GD.PushError("SurfaceTool.create_from: not an ArrayMesh surface"); return; }
        Begin(am.Surfaces[surface].Primitive);
        AppendFrom(existing, surface, Transform3D.Identity);
        _material = am.Surfaces[surface].Material;
    }

    /// <summary>Appends a surface, transformed, to what is held.</summary>
    public void AppendFrom(Mesh existing, int surface, Transform3D transform)
    {
        if (existing is not ArrayMesh am || surface < 0 || surface >= am.Surfaces.Count) { GD.PushError("SurfaceTool.append_from: not an ArrayMesh surface"); return; }
        MeshSurface s = am.Surfaces[surface];
        int baseIndex = _verts.Count;
        Basis nb = transform.Basis.Inverse().Transposed();
        for (int i = 0; i < s.Vertices.Length; i++)
        {
            var v = new Vert { V = transform * s.Vertices[i], C = s.Colors != null ? s.Colors[i] : new Color(1, 1, 1, 1), Tw = 1f };
            if (s.Normals != null) { v.N = (nb * s.Normals[i]).Normalized(); _hasN = true; }
            if (s.Tangents != null) { Vector3 t = (transform.Basis * new Vector3(s.Tangents[i * 4], s.Tangents[i * 4 + 1], s.Tangents[i * 4 + 2])).Normalized(); v.Tx = t.X; v.Ty = t.Y; v.Tz = t.Z; v.Tw = s.Tangents[i * 4 + 3]; _hasT = true; }
            if (s.Colors != null) _hasC = true;
            if (s.Uv != null) { v.Uv = s.Uv[i]; _hasUv = true; }
            if (s.Uv2 != null) { v.Uv2 = s.Uv2[i]; _hasUv2 = true; }
            _verts.Add(v);
        }
        if (s.Indices != null) foreach (int i in s.Indices) _indices.Add(baseIndex + i);
        else if (_indices.Count > 0) for (int i = 0; i < s.Vertices.Length; i++) _indices.Add(baseIndex + i);
        _began = true;
    }
}

/// <summary>Reads and edits the vertices and faces of one surface of an ArrayMesh (Godot's MeshDataTool). Triangle surfaces only; edges are not offered.</summary>
public class MeshDataTool : RefCounted
{
    private MeshSurface? _s;
    private Material? _material;

    public Error CreateFromSurface(ArrayMesh mesh, int surface)
    {
        if (surface < 0 || surface >= mesh.Surfaces.Count || mesh.Surfaces[surface].Primitive != Mesh.PrimitiveType.Triangles) return Error.InvalidParameter;
        MeshSurface src = mesh.Surfaces[surface];
        _s = new MeshSurface
        {
            Vertices = (Vector3[])src.Vertices.Clone(), Normals = (Vector3[]?)src.Normals?.Clone(), Tangents = (float[]?)src.Tangents?.Clone(), Colors = (Color[]?)src.Colors?.Clone(),
            Uv = (Vector2[]?)src.Uv?.Clone(), Uv2 = (Vector2[]?)src.Uv2?.Clone(), Indices = (int[]?)src.Indices?.Clone(),
        };
        _material = src.Material;
        return Error.Ok;
    }

    public void Clear() { _s = null; _material = null; }

    public Error CommitToSurface(ArrayMesh mesh, ulong compressionFlags = 0)
    {
        if (_s == null) return Error.InvalidParameter;
        mesh.AddSurfaceFromArrays(Mesh.PrimitiveType.Triangles, _s.ToArrays());
        if (_material != null) mesh.SurfaceSetMaterial(mesh.GetSurfaceCount() - 1, _material);
        return Error.Ok;
    }

    public int GetVertexCount() => _s?.Vertices.Length ?? 0;
    public int GetFaceCount() => _s == null ? 0 : (_s.Indices?.Length ?? _s.Vertices.Length) / 3;
    public Vector3 GetVertex(int idx) => _s != null && idx >= 0 && idx < _s.Vertices.Length ? _s.Vertices[idx] : default;
    public void SetVertex(int idx, Vector3 vertex) { if (_s != null && idx >= 0 && idx < _s.Vertices.Length) _s.Vertices[idx] = vertex; }
    public Vector3 GetVertexNormal(int idx) => _s?.Normals != null && idx >= 0 && idx < _s.Normals.Length ? _s.Normals[idx] : default;
    public void SetVertexNormal(int idx, Vector3 normal) { if (_s?.Normals != null && idx >= 0 && idx < _s.Normals.Length) _s.Normals[idx] = normal; }
    public Plane GetVertexTangent(int idx) => _s?.Tangents != null && idx >= 0 && idx * 4 + 3 < _s.Tangents.Length
        ? new Plane(_s.Tangents[idx * 4], _s.Tangents[idx * 4 + 1], _s.Tangents[idx * 4 + 2], _s.Tangents[idx * 4 + 3]) : default;
    public Color GetVertexColor(int idx) => _s?.Colors != null && idx >= 0 && idx < _s.Colors.Length ? _s.Colors[idx] : new Color(1, 1, 1, 1);
    public void SetVertexColor(int idx, Color color) { if (_s?.Colors != null && idx >= 0 && idx < _s.Colors.Length) _s.Colors[idx] = color; }
    public Vector2 GetVertexUV(int idx) => _s?.Uv != null && idx >= 0 && idx < _s.Uv.Length ? _s.Uv[idx] : default;
    public void SetVertexUV(int idx, Vector2 uv) { if (_s?.Uv != null && idx >= 0 && idx < _s.Uv.Length) _s.Uv[idx] = uv; }
    public Vector2 GetVertexUV2(int idx) => _s?.Uv2 != null && idx >= 0 && idx < _s.Uv2.Length ? _s.Uv2[idx] : default;
    public void SetVertexUV2(int idx, Vector2 uv) { if (_s?.Uv2 != null && idx >= 0 && idx < _s.Uv2.Length) _s.Uv2[idx] = uv; }
    public void SetMaterial(Material? material) => _material = material;
    public Material? GetMaterial() => _material;

    /// <summary>The vertex index of corner 0, 1 or 2 of a face.</summary>
    public int GetFaceVertex(int face, int vertex)
    {
        if (_s == null || face < 0 || face >= GetFaceCount() || vertex < 0 || vertex > 2) return -1;
        return _s.Indices != null ? _s.Indices[face * 3 + vertex] : face * 3 + vertex;
    }

    /// <summary>A face's normal (clockwise front faces, so cross(c - a, b - a)).</summary>
    public Vector3 GetFaceNormal(int face)
    {
        int a = GetFaceVertex(face, 0), b = GetFaceVertex(face, 1), c = GetFaceVertex(face, 2);
        if (a < 0) return default;
        return (_s!.Vertices[c] - _s.Vertices[a]).Cross(_s.Vertices[b] - _s.Vertices[a]).Normalized();
    }

    public int GetFormat() => _s == null ? 0 : (int)Mesh.ArrayFormat.FormatVertex | (_s.Normals != null ? 2 : 0) | (_s.Tangents != null ? 4 : 0) | (_s.Colors != null ? 8 : 0) | (_s.Uv != null ? 16 : 0) | (_s.Uv2 != null ? 32 : 0) | (_s.Indices != null ? 4096 : 0);
}
