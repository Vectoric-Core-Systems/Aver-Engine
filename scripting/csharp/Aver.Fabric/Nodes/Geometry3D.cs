// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Geometry nodes (G4_RENDER.md R7): MultiMesh and its instance node, MeshLibrary and GridMap, the CSG family, Sprite3D and Label3D. The converter bakes CSG trees, sprite
// quads and GridMap cells into meshes and child nodes; these classes hold Godot's properties and answer the API that makes sense once that is done.

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>An instance table drawn as one mesh (Godot's MultiMesh): a transform per instance, optionally a colour and four custom floats. The engine bakes the visible
/// instances of <see cref="Mesh"/> into one mesh whenever the table changes (at most once a frame; the engine limits the vertices). A colour multiplies the
/// albedo only through the material's vertex colour (<c>vertex_color_use_as_albedo</c>), as in Godot; custom data x and y reach the shader as the second UV set.</summary>
public class MultiMesh : Resource
{
    public enum TransformFormatEnum { Transform2D = 0, Transform3D = 1 }

    private Mesh? _mesh;
    private TransformFormatEnum _format = TransformFormatEnum.Transform3D;
    private bool _colors, _customData;
    private int _count;
    private int _visible = -1;
    private Transform3D[] _xf = System.Array.Empty<Transform3D>();
    private Color[] _col = System.Array.Empty<Color>();
    private Color[] _custom = System.Array.Empty<Color>();
    private int _id;
    private string _key = "";

    /// <summary>The mesh each instance draws. A mesh built at run time (ArrayMesh) or a file mesh.</summary>
    public Mesh? Mesh
    {
        get => _mesh;
        set { _mesh = value; if (_id != 0) RuntimeMeshBackend.Current?.SetSource(_id, SourcePath() ?? ""); }
    }

    /// <summary>Godot's default is 2D; this runtime draws 3D tables only (a 2D table is stored and not drawn), so a script that forgets to set it still works.</summary>
    public TransformFormatEnum TransformFormat { get => _format; set => _format = value; }

    public bool UseColors
    {
        get => _colors;
        set { _colors = value; if (_id != 0) RuntimeMeshBackend.Current?.SetFlags(_id, _colors, _customData); }
    }

    public bool UseCustomData
    {
        get => _customData;
        set { _customData = value; if (_id != 0) RuntimeMeshBackend.Current?.SetFlags(_id, _colors, _customData); }
    }

    public int InstanceCount
    {
        get => _count;
        set
        {
            value = Math.Max(0, value);
            int old = _count;
            _count = value;
            System.Array.Resize(ref _xf, value);
            System.Array.Resize(ref _col, value);
            System.Array.Resize(ref _custom, value);
            for (int i = old; i < value; i++) { _xf[i] = Transform3D.Identity; _col[i] = new Color(1, 1, 1, 1); _custom[i] = new Color(0, 0, 0, 0); }
            if (_id != 0) RuntimeMeshBackend.Current?.SetCount(_id, value);
        }
    }

    /// <summary>How many instances are drawn, the first N; -1 draws all.</summary>
    public int VisibleInstanceCount
    {
        get => _visible;
        set { _visible = value < 0 ? -1 : value; if (_id != 0) RuntimeMeshBackend.Current?.SetVisible(_id, _visible); }
    }

    public Aabb CustomAabb { get; set; }

    public void SetInstanceTransform(int instance, Transform3D transform)
    {
        if ((uint)instance >= (uint)_count) { GD.PushError("MultiMesh.set_instance_transform: instance " + instance + " is out of range"); return; }
        _xf[instance] = transform;
        if (_id != 0)
        {
            var t = new float[12];
            FabricSpace.ToAverMatrix12(transform, t);
            RuntimeMeshBackend.Current?.SetTransform(_id, instance, t);
        }
    }

    public Transform3D GetInstanceTransform(int instance) => (uint)instance < (uint)_count ? _xf[instance] : Transform3D.Identity;

    public void SetInstanceColor(int instance, Color color)
    {
        if ((uint)instance >= (uint)_count) { GD.PushError("MultiMesh.set_instance_color: instance " + instance + " is out of range"); return; }
        _col[instance] = color;
        if (_id != 0) RuntimeMeshBackend.Current?.SetColor(_id, instance, color);
    }

    public Color GetInstanceColor(int instance) => (uint)instance < (uint)_count ? _col[instance] : new Color(1, 1, 1, 1);

    public void SetInstanceCustomData(int instance, Color customData)
    {
        if ((uint)instance >= (uint)_count) { GD.PushError("MultiMesh.set_instance_custom_data: instance " + instance + " is out of range"); return; }
        _custom[instance] = customData;
        if (_id != 0) RuntimeMeshBackend.Current?.SetCustom(_id, instance, customData);
    }

    public Color GetInstanceCustomData(int instance) => (uint)instance < (uint)_count ? _custom[instance] : new Color(0, 0, 0, 0);

    private int Stride => 12 + (_colors ? 4 : 0) + (_customData ? 4 : 0);

    /// <summary>The whole table in Godot's layout: per instance the transform as three rows of (3 basis floats, 1 origin float), then the colour and the custom data
    /// when they are used.</summary>
    public float[] Buffer
    {
        get
        {
            int stride = Stride;
            var b = new float[_count * stride];
            for (int i = 0; i < _count; i++)
            {
                int o = i * stride;
                Transform3D t = _xf[i];
                b[o + 0] = t.Basis.Row0.X; b[o + 1] = t.Basis.Row0.Y; b[o + 2] = t.Basis.Row0.Z; b[o + 3] = t.Origin.X;
                b[o + 4] = t.Basis.Row1.X; b[o + 5] = t.Basis.Row1.Y; b[o + 6] = t.Basis.Row1.Z; b[o + 7] = t.Origin.Y;
                b[o + 8] = t.Basis.Row2.X; b[o + 9] = t.Basis.Row2.Y; b[o + 10] = t.Basis.Row2.Z; b[o + 11] = t.Origin.Z;
                int k = o + 12;
                if (_colors) { b[k++] = _col[i].R; b[k++] = _col[i].G; b[k++] = _col[i].B; b[k++] = _col[i].A; }
                if (_customData) { b[k++] = _custom[i].R; b[k++] = _custom[i].G; b[k++] = _custom[i].B; b[k++] = _custom[i].A; }
            }
            return b;
        }
        set
        {
            int stride = Stride;
            if (_count == 0 || value.Length != _count * stride)
            {
                if (_count != 0) GD.PushWarning("MultiMesh.buffer: " + value.Length + " floats do not fit " + _count + " instances of " + stride + "; the table is resized to fit");
                InstanceCount = value.Length / stride;
            }
            for (int i = 0; i < _count; i++)
            {
                int o = i * stride;
                _xf[i] = new Transform3D(new Basis(new Vector3(value[o], value[o + 4], value[o + 8]), new Vector3(value[o + 1], value[o + 5], value[o + 9]), new Vector3(value[o + 2], value[o + 6], value[o + 10])),
                                         new Vector3(value[o + 3], value[o + 7], value[o + 11]));
                int k = o + 12;
                if (_colors) { _col[i] = new Color(value[k], value[k + 1], value[k + 2], value[k + 3]); k += 4; }
                if (_customData) _custom[i] = new Color(value[k], value[k + 1], value[k + 2], value[k + 3]);
            }
            if (_id != 0) PushAll();
        }
    }

    /// <summary>The box around every instance's mesh box (the custom box when one is set).</summary>
    public Aabb GetAabb()
    {
        if (CustomAabb.Size != Vector3.Zero) return CustomAabb;
        if (_mesh == null || _count == 0) return default;
        Aabb m = _mesh.GetAabb();
        bool first = true;
        Aabb all = default;
        int n = _visible < 0 ? _count : Math.Min(_visible, _count);
        for (int i = 0; i < n; i++)
        {
            Aabb b = _xf[i] * m;
            if (first) { all = b; first = false; }
            else all = all.Merge(b);
        }
        return all;
    }

    private string? SourcePath() => MeshInstance3D.MeshAssetOf(_mesh);

    private void PushAll()
    {
        IRuntimeMeshBackend? b = RuntimeMeshBackend.Current;
        if (b == null || _id == 0) return;
        var xf = new float[_count * 12];
        for (int i = 0; i < _count; i++) FabricSpace.ToAverMatrix12(_xf[i], xf, i * 12);
        float[]? col = null, custom = null;
        if (_colors) { col = new float[_count * 4]; for (int i = 0; i < _count; i++) { col[i * 4] = _col[i].R; col[i * 4 + 1] = _col[i].G; col[i * 4 + 2] = _col[i].B; col[i * 4 + 3] = _col[i].A; } }
        if (_customData) { custom = new float[_count * 4]; for (int i = 0; i < _count; i++) { custom[i * 4] = _custom[i].R; custom[i * 4 + 1] = _custom[i].G; custom[i * 4 + 2] = _custom[i].B; custom[i * 4 + 3] = _custom[i].A; } }
        b.SetFlags(_id, _colors, _customData);
        b.SetSource(_id, SourcePath() ?? "");
        b.SetBuffer(_id, _count, xf, col, custom);
        b.SetVisible(_id, _visible);
    }

    /// <summary>The path the baked mesh is drawn by ("@rtmesh/multimesh/..."), making the engine's table the first time. Null without an engine.</summary>
    internal string? EngineAssetPath
    {
        get
        {
            IRuntimeMeshBackend? b = RuntimeMeshBackend.Current;
            if (b == null) return null;
            if (_id == 0)
            {
                _key = "mm" + GetInstanceId();
                _id = b.CreateMulti(_key);
                if (_id == 0) return null;
                PushAll();
            }
            return RuntimeMeshes.PathOf("multimesh/" + _key);
        }
    }
}

/// <summary>Draws a <see cref="MultiMesh"/> (Godot's MultiMeshInstance3D).</summary>
public class MultiMeshInstance3D : GeometryInstance3D
{
    private MultiMesh? _multimesh;

    public MultiMesh? Multimesh
    {
        get => _multimesh;
        set { _multimesh = value; Apply(); }
    }

    private void Apply() => PushMeshAndMaterial(_multimesh?.EngineAssetPath);

    internal override void OnMaterialOverrideChanged() => Apply();

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Apply();
    }

    internal override void OnVisibilityApplied(bool effective)
    {
        if (Self.IsValid && Self.HasComponent(Component.MeshRenderer)) Self.SetVisible(effective);
    }
}

/// <summary>The items a <see cref="GridMap"/> places: a mesh, its transform and collision shapes per item id (Godot's MeshLibrary).</summary>
public class MeshLibrary : Resource
{
    private sealed class Item
    {
        public string Name = "";
        public Mesh? Mesh;
        public Transform3D MeshTransform = Transform3D.Identity;
        public readonly List<(Shape3D Shape, Transform3D Transform)> Shapes = new();
    }

    private readonly SortedDictionary<int, Item> _items = new();

    /// <summary>The converter's form: an array of dictionaries with "id", "name" and "mesh".</summary>
    public Collections.Array Items
    {
        get => new(_items.Select(kv => (Variant)new Collections.Dictionary { ["id"] = kv.Key, ["name"] = kv.Value.Name }));
        set
        {
            foreach (Variant v in value)
            {
                Collections.Dictionary d = v.AsGodotDictionary();
                if (!d.TryGetValue("id", out Variant id)) continue;
                CreateItem(id.AsInt32());
                if (d.TryGetValue("name", out Variant name)) SetItemName(id.AsInt32(), name.AsString());
                if (d.TryGetValue("mesh", out Variant mesh) && mesh.AsGodotObject() is Mesh m) SetItemMesh(id.AsInt32(), m);
            }
        }
    }

    public void CreateItem(int id) { if (!_items.ContainsKey(id)) _items[id] = new Item(); }
    public void Clear() => _items.Clear();
    public void RemoveItem(int id) => _items.Remove(id);
    public int[] GetItemList() => _items.Keys.ToArray();
    public int GetLastUnusedItemId() => _items.Count == 0 ? 0 : _items.Keys.Max() + 1;
    public int FindItemByName(string name) => _items.FirstOrDefault(kv => kv.Value.Name == name, new KeyValuePair<int, Item>(-1, null!)).Key;
    public void SetItemName(int id, string name) { if (_items.TryGetValue(id, out Item? i)) i.Name = name; }
    public string GetItemName(int id) => _items.TryGetValue(id, out Item? i) ? i.Name : "";
    public void SetItemMesh(int id, Mesh? mesh) { if (_items.TryGetValue(id, out Item? i)) i.Mesh = mesh; }
    public Mesh? GetItemMesh(int id) => _items.TryGetValue(id, out Item? i) ? i.Mesh : null;
    public void SetItemMeshTransform(int id, Transform3D t) { if (_items.TryGetValue(id, out Item? i)) i.MeshTransform = t; }
    public Transform3D GetItemMeshTransform(int id) => _items.TryGetValue(id, out Item? i) ? i.MeshTransform : Transform3D.Identity;

    /// <summary>The item's collision shapes: shape, transform, shape, transform, ...</summary>
    public void SetItemShapes(int id, Collections.Array shapes)
    {
        if (!_items.TryGetValue(id, out Item? i)) return;
        i.Shapes.Clear();
        for (int k = 0; k + 1 < shapes.Count; k += 2)
            if (shapes[k].AsGodotObject() is Shape3D s) i.Shapes.Add((s, shapes[k + 1].AsTransform3D()));
    }

    public Collections.Array GetItemShapes(int id)
    {
        var a = new Collections.Array();
        if (_items.TryGetValue(id, out Item? i)) foreach ((Shape3D s, Transform3D t) in i.Shapes) { a.Add((Variant)s); a.Add((Variant)t); }
        return a;
    }
}

/// <summary>A grid of library items (Godot's GridMap). The converter turns the stored cells into one instanced node per item and a static collision body below this
/// node, so the level shows and collides as authored; <see cref="SetCellItem"/> at run time changes the stored cells and the answers of the query methods, not what is
/// drawn. The coordinate maths (<see cref="MapToLocal"/>, <see cref="LocalToMap"/>, the 24 orientations) is Godot's.</summary>
public class GridMap : Node3D
{
    private readonly Dictionary<Vector3I, (int Item, int Orientation)> _cells = new();

    public const int InvalidCellItem = -1;

    public MeshLibrary? MeshLibrary { get; set; }
    public Vector3 CellSize { get; set; } = new(2, 2, 2);
    public bool CellCenterX { get; set; } = true;
    public bool CellCenterY { get; set; } = true;
    public bool CellCenterZ { get; set; } = true;
    public float CellScale { get; set; } = 1f;
    public int CellOctantSize { get; set; } = 8;
    public uint CollisionLayer { get; set; } = 1;
    public uint CollisionMask { get; set; } = 1;
    public float CollisionPriority { get; set; } = 1f;
    public PhysicsMaterial? PhysicsMaterial { get; set; }
    public bool BakeNavigation { get; set; }

    /// <summary>Godot's stored form: a dictionary whose "cells" is a PackedInt32Array, three ints per cell (x | y &lt;&lt; 16, z, item | orientation &lt;&lt; 16 | layer &lt;&lt; 21).</summary>
    public Collections.Dictionary Data
    {
        get
        {
            var ints = new List<int>();
            foreach (var kv in _cells)
            {
                ints.Add((ushort)kv.Key.X | ((ushort)kv.Key.Y << 16));
                ints.Add((ushort)kv.Key.Z);
                ints.Add((kv.Value.Item & 0xFFFF) | ((kv.Value.Orientation & 0x1F) << 16));
            }
            return new Collections.Dictionary { ["cells"] = ints.ToArray() };
        }
        set
        {
            _cells.Clear();
            if (!value.TryGetValue("cells", out Variant cells)) return;
            int[] a = cells.AsInt32Array();
            for (int i = 0; i + 2 < a.Length; i += 3)
            {
                uint w0 = unchecked((uint)a[i]), w1 = unchecked((uint)a[i + 1]), v = unchecked((uint)a[i + 2]);
                int item = (int)(v & 0xFFFF);
                if (item == 0xFFFF) continue;
                _cells[new Vector3I(unchecked((short)(w0 & 0xFFFF)), unchecked((short)(w0 >> 16)), unchecked((short)(w1 & 0xFFFF)))] = (item, (int)((v >> 16) & 0x1F));
            }
        }
    }

    public void SetCellItem(Vector3I position, int item, int orientation = 0)
    {
        if (item < 0) { _cells.Remove(position); return; }
        if (orientation < 0 || orientation > 23) { GD.PushError("GridMap.set_cell_item: orientation " + orientation + " is not one of the 24"); return; }
        _cells[position] = (item, orientation);
    }

    public int GetCellItem(Vector3I position) => _cells.TryGetValue(position, out var c) ? c.Item : InvalidCellItem;
    public int GetCellItemOrientation(Vector3I position) => _cells.TryGetValue(position, out var c) ? c.Orientation : -1;
    public Basis GetCellItemBasis(Vector3I position) => _cells.TryGetValue(position, out var c) ? GetBasisWithOrthogonalIndex(c.Orientation) : Basis.Identity;
    public void Clear() => _cells.Clear();
    public Collections.Array<Vector3I> GetUsedCells() => new(_cells.Keys);
    public Collections.Array<Vector3I> GetUsedCellsByItem(int item) => new(_cells.Where(kv => kv.Value.Item == item).Select(kv => kv.Key));

    private Vector3 Offset => new(CellSize.X * 0.5f * (CellCenterX ? 1 : 0), CellSize.Y * 0.5f * (CellCenterY ? 1 : 0), CellSize.Z * 0.5f * (CellCenterZ ? 1 : 0));

    /// <summary>The position of a cell's centre (or corner, per the centre flags) in this node's local space.</summary>
    public Vector3 MapToLocal(Vector3I mapPosition) => new Vector3(mapPosition.X * CellSize.X, mapPosition.Y * CellSize.Y, mapPosition.Z * CellSize.Z) + Offset;

    /// <summary>The cell a local position falls in.</summary>
    public Vector3I LocalToMap(Vector3 localPosition) => new((int)MathF.Floor(localPosition.X / CellSize.X), (int)MathF.Floor(localPosition.Y / CellSize.Y), (int)MathF.Floor(localPosition.Z / CellSize.Z));

    /// <summary>The transform a cell's item is drawn with: its orientation (scaled by cell_scale) at the cell's position.</summary>
    public Transform3D GetCellTransform(Vector3I position)
    {
        Basis b = GetCellItemBasis(position);
        return new Transform3D(new Basis(b.Column0 * CellScale, b.Column1 * CellScale, b.Column2 * CellScale), MapToLocal(position));
    }

    // Godot's table of the 24 orthogonal bases (Basis(xx, xy, xz, yx, yy, yz, zx, zy, zz)).
    private static readonly sbyte[,] s_bases =
    {
        { 1, 0, 0, 0, 1, 0, 0, 0, 1 },   { 0, -1, 0, 1, 0, 0, 0, 0, 1 },  { -1, 0, 0, 0, -1, 0, 0, 0, 1 },  { 0, 1, 0, -1, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, -1, 0, 1, 0 },  { 0, 0, 1, 1, 0, 0, 0, 1, 0 },   { -1, 0, 0, 0, 0, 1, 0, 1, 0 },   { 0, 0, -1, -1, 0, 0, 0, 1, 0 },
        { 1, 0, 0, 0, -1, 0, 0, 0, -1 }, { 0, 1, 0, 1, 0, 0, 0, 0, -1 },  { -1, 0, 0, 0, 1, 0, 0, 0, -1 },  { 0, -1, 0, -1, 0, 0, 0, 0, -1 },
        { 1, 0, 0, 0, 0, 1, 0, -1, 0 },  { 0, 0, -1, 1, 0, 0, 0, -1, 0 }, { -1, 0, 0, 0, 0, -1, 0, -1, 0 },  { 0, 0, 1, -1, 0, 0, 0, -1, 0 },
        { 0, 0, 1, 0, 1, 0, -1, 0, 0 },  { 0, -1, 0, 0, 0, 1, -1, 0, 0 }, { 0, 0, -1, 0, -1, 0, -1, 0, 0 },  { 0, 1, 0, 0, 0, -1, -1, 0, 0 },
        { 0, 0, 1, 0, -1, 0, 1, 0, 0 },  { 0, 1, 0, 0, 0, 1, 1, 0, 0 },   { 0, 0, -1, 0, 1, 0, 1, 0, 0 },   { 0, -1, 0, 0, 0, -1, 1, 0, 0 },
    };

    /// <summary>One of the 24 orthogonal bases a cell can be turned to; the identity for an index out of range.</summary>
    public Basis GetBasisWithOrthogonalIndex(int index)
    {
        if (index < 0 || index > 23) return Basis.Identity;
        return new Basis(new Vector3(s_bases[index, 0], s_bases[index, 3], s_bases[index, 6]), new Vector3(s_bases[index, 1], s_bases[index, 4], s_bases[index, 7]),
                         new Vector3(s_bases[index, 2], s_bases[index, 5], s_bases[index, 8]));
    }

    /// <summary>The index of the orthogonal basis closest to <paramref name="basis"/> (the orientation to store), or -1 when it is not one of the 24.</summary>
    public int GetOrthogonalIndexFromBasis(Basis basis)
    {
        Basis b = basis.Orthonormalized();
        for (int i = 0; i < 24; i++)
            if (GetBasisWithOrthogonalIndex(i).IsEqualApprox(b)) return i;
        return -1;
    }
}

// ---- CSG --------------------------------------------------------------------------------------------------------------------------

/// <summary>A CSG node (Godot's CSGShape3D family). The converter combines the tree below a root with union, intersection and subtraction at convert time and gives the
/// root the baked mesh (so the root is a mesh instance of that mesh); the nodes below it are folded into it and do not exist at run time. A property changed at run time
/// is stored and does not rebuild the mesh.</summary>
public class CSGShape3D : MeshInstance3D
{
    public enum OperationEnum { Union = 0, Intersection = 1, Subtraction = 2 }

    public OperationEnum Operation { get; set; }
    public float Snap { get; set; } = 0.001f;
    public bool CalculateTangents { get; set; } = true;
    public bool UseCollision { get; set; }
    public uint CollisionLayer { get; set; } = 1;
    public uint CollisionMask { get; set; } = 1;
    public float CollisionPriority { get; set; } = 1f;
    public bool InvertFaces { get; set; }

    /// <summary>True for a shape that is not below another CSG node (the one that carries the baked mesh).</summary>
    public bool IsRootShape() => GetParent() is not CSGShape3D;
}

public class CSGPrimitive3D : CSGShape3D
{
    public bool FlipFaces { get; set; }
}

public class CSGCombiner3D : CSGShape3D
{
}

public class CSGBox3D : CSGPrimitive3D
{
    public Vector3 Size { get; set; } = Vector3.One;
    public Material? Material { get; set; }
}

public class CSGSphere3D : CSGPrimitive3D
{
    public float Radius { get; set; } = 0.5f;
    public int RadialSegments { get; set; } = 12;
    public int Rings { get; set; } = 6;
    public bool SmoothFaces { get; set; } = true;
    public Material? Material { get; set; }
}

public class CSGCylinder3D : CSGPrimitive3D
{
    public float Radius { get; set; } = 0.5f;
    public float Height { get; set; } = 2f;
    public int Sides { get; set; } = 8;
    public bool Cone { get; set; }
    public bool SmoothFaces { get; set; } = true;
    public Material? Material { get; set; }
}

public class CSGTorus3D : CSGPrimitive3D
{
    public float InnerRadius { get; set; } = 0.5f;
    public float OuterRadius { get; set; } = 1f;
    public int Sides { get; set; } = 8;
    public int RingSides { get; set; } = 6;
    public bool SmoothFaces { get; set; } = true;
    public Material? Material { get; set; }
}

public class CSGPolygon3D : CSGPrimitive3D
{
    public enum ModeEnum { Depth = 0, Spin = 1, Path = 2 }

    public Vector2[] Polygon { get; set; } = { new(0, 0), new(0, 1), new(1, 1), new(1, 0) };
    public ModeEnum Mode { get; set; }
    public float Depth { get; set; } = 1f;
    public float SpinDegrees { get; set; } = 360f;
    public int SpinSides { get; set; } = 5;
    public bool SmoothFaces { get; set; }
    public Material? Material { get; set; }
    public NodePath PathNode { get; set; } = new NodePath("");
    public int PathIntervalType { get; set; }
    public float PathInterval { get; set; } = 1f;
    public float PathSimplifyAngle { get; set; }
    public int PathRotation { get; set; } = 2;
    public bool PathLocal { get; set; }
    public bool PathContinuousU { get; set; } = true;
    public float PathUDistance { get; set; } = 1f;
    public bool PathJoined { get; set; }
}

/// <summary>A CSG node that brings a mesh into the tree. After conversion the root's mesh is the baked result of the tree.</summary>
public class CSGMesh3D : CSGPrimitive3D
{
    public Material? Material { get; set; }
}

// ---- Sprite3D and Label3D -----------------------------------------------------------------------------------------------------------

/// <summary>What Sprite3D and Label3D share (Godot's SpriteBase3D). The converter bakes the pixel size, the frame, the flips and the offset into the quad's geometry and
/// UVs, and the modulate colour, billboard mode, alpha cut, shading and sidedness into the sprite's material; the properties here are kept for scripts and are not pushed.</summary>
public class SpriteBase3D : GeometryInstance3D
{
    public enum AlphaCutMode { Disabled = 0, Discard = 1, OpaquePrepass = 2, Hash = 3 }

    private Mesh? _mesh;

    public float PixelSize { get; set; } = 0.01f;
    public bool Centered { get; set; } = true;
    public Vector2 Offset { get; set; }
    public bool FlipH { get; set; }
    public bool FlipV { get; set; }
    public Color Modulate { get; set; } = new(1, 1, 1, 1);
    public BaseMaterial3D.BillboardModeEnum Billboard { get; set; }
    public bool Transparent { get; set; } = true;
    public bool Shaded { get; set; }
    public bool DoubleSided { get; set; } = true;
    public AlphaCutMode AlphaCut { get; set; }
    public float AlphaScissorThreshold { get; set; } = 0.5f;
    public float AlphaHashScale { get; set; } = 1f;
    public bool NoDepthTest { get; set; }
    public bool FixedSize { get; set; }
    public int RenderPriority { get; set; }
    public int TextureFilter { get; set; } = 3;
    public int Axis { get; set; } = 2;
    public float SortingOffset { get; set; }

    /// <summary>The baked quad (written by the converter). A sprite made by a script has none and draws nothing.</summary>
    public Mesh? Mesh
    {
        get => _mesh;
        set { _mesh = value; Apply(); }
    }

    private void Apply() => PushMeshAndMaterial(MeshInstance3D.MeshAssetOf(_mesh));

    internal override void OnMaterialOverrideChanged() => Apply();

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Apply();
    }

    internal override void OnVisibilityApplied(bool effective)
    {
        if (Self.IsValid && Self.HasComponent(Component.MeshRenderer)) Self.SetVisible(effective);
    }
}

/// <summary>A textured quad in the world (Godot's Sprite3D), baked by the converter. The texture, frame and region properties are kept; changing them at run time does
/// not redraw the quad.</summary>
public class Sprite3D : SpriteBase3D
{
    public Texture2D? Texture { get; set; }
    public int Hframes { get; set; } = 1;
    public int Vframes { get; set; } = 1;
    public int Frame { get; set; }
    public Vector2I FrameCoords { get; set; }
    public bool RegionEnabled { get; set; }
    public Rect2 RegionRect { get; set; }

    /// <summary>The size of one frame in pixels when the texture's size is known (it is zero for a file texture at run time).</summary>
    public Rect2 GetItemRect()
    {
        Vector2 t = Texture?.GetSize() ?? Vector2.Zero;
        Vector2 f = RegionEnabled ? RegionRect.Size : t;
        f = new Vector2(f.X / Math.Max(1, Hframes), f.Y / Math.Max(1, Vframes));
        Vector2 size = f * PixelSize;
        Vector2 o = Offset * PixelSize;
        return new Rect2(Centered ? o - size / 2f : o, size);
    }
}

/// <summary>Text in the world (Godot's Label3D). The engine has no world-space text renderer yet: the properties and the text are kept, and nothing is drawn. Show text
/// with a UI label instead.</summary>
public class Label3D : GeometryInstance3D
{
    public string Text { get; set; } = "";
    public Resource? Font { get; set; }
    public int FontSize { get; set; } = 32;
    public int OutlineSize { get; set; } = 12;
    public float PixelSize { get; set; } = 0.005f;
    public Vector2 Offset { get; set; }
    public Color Modulate { get; set; } = new(1, 1, 1, 1);
    public Color OutlineModulate { get; set; } = new(0, 0, 0, 1);
    public HorizontalAlignment HorizontalAlignment { get; set; } = HorizontalAlignment.Center;
    public VerticalAlignment VerticalAlignment { get; set; } = VerticalAlignment.Center;
    public float LineSpacing { get; set; }
    public int AutowrapMode { get; set; }
    public float Width { get; set; } = 500f;
    public bool Uppercase { get; set; }
    public BaseMaterial3D.BillboardModeEnum Billboard { get; set; }
    public bool Shaded { get; set; }
    public bool DoubleSided { get; set; } = true;
    public bool NoDepthTest { get; set; }
    public bool FixedSize { get; set; }
    public int AlphaCut { get; set; }
    public float AlphaScissorThreshold { get; set; } = 0.5f;
    public float AlphaHashScale { get; set; } = 1f;
    public int TextureFilter { get; set; } = 3;
    public int RenderPriority { get; set; }
    public int OutlineRenderPriority { get; set; } = -1;
    public string Language { get; set; } = "";

    /// <summary>Always false: this runtime does not draw world-space text.</summary>
    public bool IsDrawn => false;
}
