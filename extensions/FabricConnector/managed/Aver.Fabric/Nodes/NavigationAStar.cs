// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Godot's generic path-finding data structures: AStar3D, AStar2D (a point graph) and AStarGrid2D (a rectangular grid). They are plain data, so values
// cross to the engine (aver/synapse/NavGraph.hpp, through Aver.Nav.Abi) as Godot numbers: no axis map, no centimetres. A script that overrides
// _ComputeCost / _EstimateCost is called back by the search. docs/plans/fabric/G5_PARITY.md, N1.

using System.Reflection;
using System.Runtime.InteropServices;

namespace Aver.Fabric;

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate float AStarCostCallback(IntPtr user, long fromId, long toId);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate float AStarGridCostCallback(IntPtr user, int fromX, int fromY, int toX, int toY);

/// <summary>A graph of points joined by segments, searched with A* (Godot's AStar3D). Ids are 64-bit and chosen by the caller.</summary>
public class AStar3D : RefCounted
{
    private int _g;
    private AStarCostCallback? _computeCb, _estimateCb;

    public AStar3D()
    {
        if (!NavNative.Available) return;
        _g = NavNative.aver_astar_create();
        _computeCb = (_, from, to) => SafeCost(() => _ComputeCost(from, to));
        _estimateCb = (_, from, to) => SafeCost(() => _EstimateCost(from, to));
        RebindCallbacks(IsOverridden(nameof(_ComputeCost)), IsOverridden(nameof(_EstimateCost)));
    }

    /// <summary>The default cost is the engine's own (the same as the virtual's body); a script's override is called back.</summary>
    internal void RebindCallbacks(bool compute, bool estimate)
    {
        if (_g == 0 || _computeCb == null || _estimateCb == null) return;
        NavNative.aver_astar_set_callbacks(_g, compute ? Marshal.GetFunctionPointerForDelegate(_computeCb) : IntPtr.Zero,
                                           estimate ? Marshal.GetFunctionPointerForDelegate(_estimateCb) : IntPtr.Zero, IntPtr.Zero);
    }

    internal virtual bool IsOverridden(string name) => Overrides(name);

    ~AStar3D()
    {
        if (_g != 0 && NavNative.Available) NavNative.aver_astar_destroy(_g);
    }

    private bool Overrides(string name)
    {
        MethodInfo? m = GetType().GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, new[] { typeof(long), typeof(long) }, null);
        return m != null && m.DeclaringType != typeof(AStar3D);
    }

    private static float SafeCost(Func<float> f)
    {
        try { return f(); }
        catch (Exception ex) { GD.PushError($"AStar: a cost function threw {ex.GetType().Name}: {ex.Message}"); return 1f; }
    }

    /// <summary>Called for the cost of moving between two points that are joined (the default is their distance). Override it in a script.</summary>
    public virtual float _ComputeCost(long fromId, long toId) => GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));

    /// <summary>Called for the estimate of the cost from a point to the target (the default is their distance). Override it in a script.</summary>
    public virtual float _EstimateCost(long fromId, long toId) => GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));

    public long GetAvailablePointId() => _g == 0 ? 1 : NavNative.aver_astar_next_free_id(_g);

    public void AddPoint(long id, Vector3 position, float weightScale = 1f)
    {
        if (id < 0) { GD.PushError("AStar.add_point: can't add a point with a negative id"); return; }
        if (weightScale < 0f) { GD.PushError("AStar.add_point: can't set the weight scale below 0"); return; }
        if (_g != 0) NavNative.aver_astar_add_point(_g, id, position.X, position.Y, position.Z, weightScale);
    }

    public Vector3 GetPointPosition(long id)
    {
        if (_g == 0) return Vector3.Zero;
        var o = new float[3];
        if (NavNative.aver_astar_point_position(_g, id, o) == 0) { GD.PushError($"AStar: the point {id} doesn't exist"); return Vector3.Zero; }
        return new Vector3(o[0], o[1], o[2]);
    }

    public void SetPointPosition(long id, Vector3 position)
    {
        if (_g == 0) return;
        if (NavNative.aver_astar_set_point_position(_g, id, position.X, position.Y, position.Z) == 0) GD.PushError($"AStar: the point {id} doesn't exist");
    }

    public float GetPointWeightScale(long id)
    {
        if (_g == 0 || NavNative.aver_astar_has_point(_g, id) == 0) { GD.PushError($"AStar: the point {id} doesn't exist"); return 0f; }
        return NavNative.aver_astar_point_weight(_g, id);
    }

    public void SetPointWeightScale(long id, float weightScale)
    {
        if (weightScale < 0f) { GD.PushError("AStar.set_point_weight_scale: can't set the weight scale below 0"); return; }
        if (_g == 0 || NavNative.aver_astar_set_point_weight(_g, id, weightScale) == 0) GD.PushError($"AStar: the point {id} doesn't exist");
    }

    public void RemovePoint(long id)
    {
        if (_g == 0 || NavNative.aver_astar_remove_point(_g, id) == 0) GD.PushError($"AStar: the point {id} doesn't exist");
    }

    public bool HasPoint(long id) => _g != 0 && NavNative.aver_astar_has_point(_g, id) != 0;

    public long[] GetPointConnections(long id)
    {
        if (_g == 0) return System.Array.Empty<long>();
        if (NavNative.aver_astar_has_point(_g, id) == 0) { GD.PushError($"AStar: the point {id} doesn't exist"); return System.Array.Empty<long>(); }
        int n = NavNative.aver_astar_connections(_g, id, null, 0);
        var ids = new long[n];
        NavNative.aver_astar_connections(_g, id, ids, n);
        return ids;
    }

    public long[] GetPointIds()
    {
        if (_g == 0) return System.Array.Empty<long>();
        int n = NavNative.aver_astar_point_ids(_g, null, 0);
        var ids = new long[n];
        NavNative.aver_astar_point_ids(_g, ids, n);
        return ids;
    }

    public void SetPointDisabled(long id, bool disabled = true)
    {
        if (_g == 0 || NavNative.aver_astar_set_disabled(_g, id, disabled ? 1 : 0) == 0) GD.PushError($"AStar: the point {id} doesn't exist");
    }

    public bool IsPointDisabled(long id) => _g != 0 && NavNative.aver_astar_is_disabled(_g, id) != 0;

    public void ConnectPoints(long id, long toId, bool bidirectional = true)
    {
        if (id == toId) { GD.PushError("AStar.connect_points: can't connect a point to itself"); return; }
        if (_g == 0 || NavNative.aver_astar_connect(_g, id, toId, bidirectional ? 1 : 0) == 0) GD.PushError($"AStar.connect_points: the point {id} or {toId} doesn't exist");
    }

    public void DisconnectPoints(long id, long toId, bool bidirectional = true)
    {
        if (_g == 0) return;
        if (NavNative.aver_astar_has_point(_g, id) == 0 || NavNative.aver_astar_has_point(_g, toId) == 0) { GD.PushError($"AStar.disconnect_points: the point {id} or {toId} doesn't exist"); return; }
        NavNative.aver_astar_disconnect(_g, id, toId, bidirectional ? 1 : 0);
    }

    public bool ArePointsConnected(long id, long toId, bool bidirectional = true) => _g != 0 && NavNative.aver_astar_are_connected(_g, id, toId, bidirectional ? 1 : 0) != 0;

    public int GetPointCount() => _g == 0 ? 0 : NavNative.aver_astar_point_count(_g);
    public int GetPointCapacity() => GetPointCount();
    public void ReserveSpace(int numNodes) { if (numNodes < 0) GD.PushError("AStar.reserve_space: the capacity can't be negative"); }

    public void Clear() { if (_g != 0) NavNative.aver_astar_clear(_g); }

    /// <summary>The id of the closest point (-1 when there is none). Ties go to the smallest id.</summary>
    public long GetClosestPoint(Vector3 toPosition, bool includeDisabled = false) => _g == 0 ? -1 : NavNative.aver_astar_closest_point(_g, toPosition.X, toPosition.Y, toPosition.Z, includeDisabled ? 1 : 0);

    public Vector3 GetClosestPositionInSegment(Vector3 toPosition)
    {
        if (_g == 0) return Vector3.Zero;
        var o = new float[3];
        return NavNative.aver_astar_closest_in_segment(_g, toPosition.X, toPosition.Y, toPosition.Z, o) != 0 ? new Vector3(o[0], o[1], o[2]) : Vector3.Zero;
    }

    public long[] GetIdPath(long fromId, long toId, bool allowPartialPath = false)
    {
        if (_g == 0) return System.Array.Empty<long>();
        if (NavNative.aver_astar_has_point(_g, fromId) == 0) { GD.PushError($"AStar.get_id_path: the point {fromId} doesn't exist"); return System.Array.Empty<long>(); }
        if (NavNative.aver_astar_has_point(_g, toId) == 0) { GD.PushError($"AStar.get_id_path: the point {toId} doesn't exist"); return System.Array.Empty<long>(); }
        int n = NavNative.aver_astar_id_path(_g, fromId, toId, allowPartialPath ? 1 : 0, null, 0);
        var ids = new long[n];
        if (n > 0) NavNative.aver_astar_id_path(_g, fromId, toId, allowPartialPath ? 1 : 0, ids, n);
        return ids;
    }

    public Vector3[] GetPointPath(long fromId, long toId, bool allowPartialPath = false)
    {
        long[] ids = GetIdPath(fromId, toId, allowPartialPath);
        var pts = new Vector3[ids.Length];
        for (int i = 0; i < ids.Length; i++) pts[i] = GetPointPosition(ids[i]);
        return pts;
    }
}

/// <summary>The 2D form of <see cref="AStar3D"/> (Godot's AStar2D): the same graph with the third coordinate always zero.</summary>
public class AStar2D : RefCounted
{
    private readonly Inner _g;

    // The graph is an AStar3D whose virtuals forward to this object's, so a script overriding _ComputeCost on an AStar2D is honoured.
    private sealed class Inner : AStar3D
    {
        internal AStar2D? Owner;
        internal override bool IsOverridden(string name) => false;
        public override float _ComputeCost(long fromId, long toId) => Owner != null ? Owner._ComputeCost(fromId, toId) : base._ComputeCost(fromId, toId);
        public override float _EstimateCost(long fromId, long toId) => Owner != null ? Owner._EstimateCost(fromId, toId) : base._EstimateCost(fromId, toId);
    }

    public AStar2D()
    {
        _g = new Inner { Owner = this };
        // Re-register once the owner is known: the inner graph decided at construction whether its virtuals are overridden.
        Rebind();
    }

    private void Rebind() => _g.RebindCallbacks(Overrides(nameof(_ComputeCost)), Overrides(nameof(_EstimateCost)));

    private bool Overrides(string name)
    {
        MethodInfo? m = GetType().GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, new[] { typeof(long), typeof(long) }, null);
        return m != null && m.DeclaringType != typeof(AStar2D);
    }

    public virtual float _ComputeCost(long fromId, long toId) => GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));
    public virtual float _EstimateCost(long fromId, long toId) => GetPointPosition(fromId).DistanceTo(GetPointPosition(toId));

    private static Vector3 V3(Vector2 p) => new(p.X, p.Y, 0f);
    private static Vector2 V2(Vector3 p) => new(p.X, p.Y);

    public long GetAvailablePointId() => _g.GetAvailablePointId();
    public void AddPoint(long id, Vector2 position, float weightScale = 1f) => _g.AddPoint(id, V3(position), weightScale);
    public Vector2 GetPointPosition(long id) => V2(_g.GetPointPosition(id));
    public void SetPointPosition(long id, Vector2 position) => _g.SetPointPosition(id, V3(position));
    public float GetPointWeightScale(long id) => _g.GetPointWeightScale(id);
    public void SetPointWeightScale(long id, float weightScale) => _g.SetPointWeightScale(id, weightScale);
    public void RemovePoint(long id) => _g.RemovePoint(id);
    public bool HasPoint(long id) => _g.HasPoint(id);
    public long[] GetPointConnections(long id) => _g.GetPointConnections(id);
    public long[] GetPointIds() => _g.GetPointIds();
    public void SetPointDisabled(long id, bool disabled = true) => _g.SetPointDisabled(id, disabled);
    public bool IsPointDisabled(long id) => _g.IsPointDisabled(id);
    public void ConnectPoints(long id, long toId, bool bidirectional = true) => _g.ConnectPoints(id, toId, bidirectional);
    public void DisconnectPoints(long id, long toId, bool bidirectional = true) => _g.DisconnectPoints(id, toId, bidirectional);
    public bool ArePointsConnected(long id, long toId, bool bidirectional = true) => _g.ArePointsConnected(id, toId, bidirectional);
    public int GetPointCount() => _g.GetPointCount();
    public int GetPointCapacity() => _g.GetPointCapacity();
    public void ReserveSpace(int numNodes) => _g.ReserveSpace(numNodes);
    public void Clear() => _g.Clear();
    public long GetClosestPoint(Vector2 toPosition, bool includeDisabled = false) => _g.GetClosestPoint(V3(toPosition), includeDisabled);
    public Vector2 GetClosestPositionInSegment(Vector2 toPosition) => V2(_g.GetClosestPositionInSegment(V3(toPosition)));
    public long[] GetIdPath(long fromId, long toId, bool allowPartialPath = false) => _g.GetIdPath(fromId, toId, allowPartialPath);
    public Vector2[] GetPointPath(long fromId, long toId, bool allowPartialPath = false) => _g.GetPointPath(fromId, toId, allowPartialPath).Select(V2).ToArray();
}

/// <summary>A rectangular grid of cells searched with A* (Godot's AStarGrid2D). Set <see cref="Region"/> (or <see cref="Size"/>), then call
/// <see cref="Update"/>, which also clears every solid flag and weight.</summary>
public class AStarGrid2D : RefCounted
{
    public enum DiagonalModeEnum { Always = 0, Never = 1, AtLeastOneWalkable = 2, OnlyIfNoObstacles = 3, Max = 4 }
    public enum HeuristicEnum { Euclidean = 0, Manhattan = 1, Octile = 2, Chebyshev = 3, Max = 4 }
    public enum CellShapeEnum { Square = 0, IsometricRight = 1, IsometricDown = 2, Max = 3 }

    private int _g;
    private AStarGridCostCallback? _computeCb, _estimateCb;
    private Rect2I _region;
    private Vector2 _offset = Vector2.Zero, _cellSize = Vector2.One;
    private CellShapeEnum _shape = CellShapeEnum.Square;
    private bool _dirty = true, _jumping;
    private DiagonalModeEnum _diagonal = DiagonalModeEnum.Always;
    private HeuristicEnum _compute = HeuristicEnum.Euclidean, _estimate = HeuristicEnum.Euclidean;

    public AStarGrid2D()
    {
        if (!NavNative.Available) return;
        _g = NavNative.aver_agrid_create();
        _computeCb = (_, fx, fy, tx, ty) => SafeCost(() => _ComputeCost(new Vector2I(fx, fy), new Vector2I(tx, ty)));
        _estimateCb = (_, fx, fy, tx, ty) => SafeCost(() => _EstimateCost(new Vector2I(fx, fy), new Vector2I(tx, ty)));
        IntPtr compute = Overrides(nameof(_ComputeCost)) ? Marshal.GetFunctionPointerForDelegate(_computeCb) : IntPtr.Zero;
        IntPtr estimate = Overrides(nameof(_EstimateCost)) ? Marshal.GetFunctionPointerForDelegate(_estimateCb) : IntPtr.Zero;
        NavNative.aver_agrid_set_callbacks(_g, compute, estimate, IntPtr.Zero);
    }

    ~AStarGrid2D()
    {
        if (_g != 0 && NavNative.Available) NavNative.aver_agrid_destroy(_g);
    }

    private bool Overrides(string name)
    {
        MethodInfo? m = GetType().GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, new[] { typeof(Vector2I), typeof(Vector2I) }, null);
        return m != null && m.DeclaringType != typeof(AStarGrid2D);
    }

    private static float SafeCost(Func<float> f)
    {
        try { return f(); }
        catch (Exception ex) { GD.PushError($"AStarGrid2D: a cost function threw {ex.GetType().Name}: {ex.Message}"); return 1f; }
    }

    /// <summary>The cost of moving between two neighbouring cells; the default is the distance by the compute heuristic. Override it in a script.</summary>
    public virtual float _ComputeCost(Vector2I fromId, Vector2I toId) => Heuristic(_compute, fromId, toId);

    /// <summary>The estimate of the cost from a cell to the target; the default is the estimate heuristic. Override it in a script.</summary>
    public virtual float _EstimateCost(Vector2I fromId, Vector2I toId) => Heuristic(_estimate, fromId, toId);

    private static float Heuristic(HeuristicEnum h, Vector2I a, Vector2I b)
    {
        float dx = Math.Abs(a.X - b.X), dy = Math.Abs(a.Y - b.Y);
        switch (h)
        {
            case HeuristicEnum.Manhattan: return dx + dy;
            case HeuristicEnum.Octile: { float f = MathF.Sqrt(2f) - 1f; return dx < dy ? f * dx + dy : f * dy + dx; }
            case HeuristicEnum.Chebyshev: return MathF.Max(dx, dy);
            default: return MathF.Sqrt(dx * dx + dy * dy);
        }
    }

    // ---- settings ----

    public Rect2I Region
    {
        get => _region;
        set
        {
            if (value == _region) return;
            _region = value;
            _dirty = true;
            if (_g != 0) NavNative.aver_agrid_set_region(_g, value.Position.X, value.Position.Y, value.Size.X, value.Size.Y);
        }
    }

    /// <summary>The size of the region, whose position stays (Godot's deprecated size).</summary>
    public Vector2I Size { get => _region.Size; set => Region = new Rect2I(_region.Position, value); }

    public Vector2 Offset { get => _offset; set { _offset = value; _dirty = true; } }

    public Vector2 CellSize
    {
        get => _cellSize;
        set { _cellSize = value; _dirty = true; if (_g != 0) NavNative.aver_agrid_set_cell_size(_g, value.X, value.Y); }
    }

    public CellShapeEnum CellShape { get => _shape; set { _shape = value; _dirty = true; } }

    public bool JumpingEnabled { get => _jumping; set { _jumping = value; if (_g != 0) NavNative.aver_agrid_set_jumping(_g, value ? 1 : 0); } }

    public DiagonalModeEnum DiagonalMode
    {
        get => _diagonal;
        set { _diagonal = value; if (_g != 0) NavNative.aver_agrid_set_diagonal_mode(_g, (int)value); }
    }

    public HeuristicEnum DefaultComputeHeuristic
    {
        get => _compute;
        set { _compute = value; if (_g != 0) NavNative.aver_agrid_set_heuristics(_g, (int)_compute, (int)_estimate); }
    }

    public HeuristicEnum DefaultEstimateHeuristic
    {
        get => _estimate;
        set { _estimate = value; if (_g != 0) NavNative.aver_agrid_set_heuristics(_g, (int)_compute, (int)_estimate); }
    }

    public bool IsDirty() => _dirty;

    /// <summary>Allocates the grid for the region; every cell is open with weight 1 afterwards. Does nothing when nothing changed since the last call.</summary>
    public void Update()
    {
        if (!_dirty) return;
        if (_g != 0) NavNative.aver_agrid_update(_g);
        _dirty = false;
    }

    public bool IsInBounds(int x, int y) => _region.HasPoint(new Vector2I(x, y));
    public bool IsInBoundsv(Vector2I id) => _region.HasPoint(id);

    private bool Ready(string method)
    {
        if (_dirty) { GD.PushError($"AStarGrid2D.{method}: the grid is not initialized, call update() first"); return false; }
        return _g != 0;
    }

    public void SetPointSolid(Vector2I id, bool solid = true)
    {
        if (!Ready("set_point_solid")) return;
        if (!IsInBoundsv(id)) { GD.PushError($"AStarGrid2D.set_point_solid: the cell {id} is out of the region"); return; }
        NavNative.aver_agrid_set_solid(_g, id.X, id.Y, solid ? 1 : 0);
    }

    public bool IsPointSolid(Vector2I id)
    {
        if (!Ready("is_point_solid")) return false;
        if (!IsInBoundsv(id)) { GD.PushError($"AStarGrid2D.is_point_solid: the cell {id} is out of the region"); return false; }
        return NavNative.aver_agrid_is_solid(_g, id.X, id.Y) != 0;
    }

    public void SetPointWeightScale(Vector2I id, float weightScale)
    {
        if (!Ready("set_point_weight_scale")) return;
        if (!IsInBoundsv(id)) { GD.PushError($"AStarGrid2D.set_point_weight_scale: the cell {id} is out of the region"); return; }
        NavNative.aver_agrid_set_weight(_g, id.X, id.Y, weightScale);
    }

    public float GetPointWeightScale(Vector2I id)
    {
        if (!Ready("get_point_weight_scale")) return 0f;
        if (!IsInBoundsv(id)) { GD.PushError($"AStarGrid2D.get_point_weight_scale: the cell {id} is out of the region"); return 0f; }
        return NavNative.aver_agrid_weight(_g, id.X, id.Y);
    }

    private Rect2I Clip(Rect2I r)
    {
        int x0 = Math.Max(r.Position.X, _region.Position.X), y0 = Math.Max(r.Position.Y, _region.Position.Y);
        int x1 = Math.Min(r.Position.X + r.Size.X, _region.Position.X + _region.Size.X), y1 = Math.Min(r.Position.Y + r.Size.Y, _region.Position.Y + _region.Size.Y);
        return new Rect2I(x0, y0, Math.Max(x1 - x0, 0), Math.Max(y1 - y0, 0));
    }

    public void FillSolidRegion(Rect2I region, bool solid = true)
    {
        if (!Ready("fill_solid_region")) return;
        Rect2I r = Clip(region);
        if (r.HasArea()) NavNative.aver_agrid_fill_solid(_g, r.Position.X, r.Position.Y, r.Size.X, r.Size.Y, solid ? 1 : 0);
    }

    public void FillWeightScaleRegion(Rect2I region, float weightScale)
    {
        if (!Ready("fill_weight_scale_region")) return;
        Rect2I r = Clip(region);
        if (r.HasArea()) NavNative.aver_agrid_fill_weight(_g, r.Position.X, r.Position.Y, r.Size.X, r.Size.Y, weightScale);
    }

    /// <summary>Clears the grid and sets the region to an empty rectangle (Godot's clear). The other settings stay; the grid is clean until the next change.</summary>
    public void Clear()
    {
        _region = default;
        _dirty = false;
        if (_g != 0) NavNative.aver_agrid_clear(_g);
    }

    public Vector2 GetPointPosition(Vector2I id)
    {
        Vector2 half = _cellSize / 2f;
        return _shape switch
        {
            CellShapeEnum.IsometricRight => _offset + half + new Vector2(id.X + id.Y, id.Y - id.X) * half,
            CellShapeEnum.IsometricDown => _offset + half + new Vector2(id.X - id.Y, id.X + id.Y) * half,
            _ => _offset + new Vector2(id.X, id.Y) * _cellSize,
        };
    }

    /// <summary>The cells in a region as dictionaries with "id", "position", "solid" and "weight_scale".</summary>
    public Collections.Array<Collections.Dictionary> GetPointDataInRegion(Rect2I region)
    {
        var a = new Collections.Array<Collections.Dictionary>();
        if (!Ready("get_point_data_in_region")) return a;
        Rect2I r = Clip(region);
        for (int y = r.Position.Y; y < r.Position.Y + r.Size.Y; y++)
            for (int x = r.Position.X; x < r.Position.X + r.Size.X; x++)
            {
                var id = new Vector2I(x, y);
                var d = new Collections.Dictionary();
                d["id"] = id;
                d["position"] = GetPointPosition(id);
                d["solid"] = IsPointSolid(id);
                d["weight_scale"] = GetPointWeightScale(id);
                a.Add(d);
            }
        return a;
    }

    public Collections.Array<Vector2I> GetIdPath(Vector2I fromId, Vector2I toId, bool allowPartialPath = false)
    {
        var path = new Collections.Array<Vector2I>();
        if (!Ready("get_id_path")) return path;
        if (!IsInBoundsv(fromId) || !IsInBoundsv(toId)) { GD.PushError("AStarGrid2D.get_id_path: the start or the end is out of the region"); return path; }
        int n = NavNative.aver_agrid_id_path(_g, fromId.X, fromId.Y, toId.X, toId.Y, allowPartialPath ? 1 : 0, null, 0);
        if (n <= 0) return path;
        var cells = new int[n * 2];
        NavNative.aver_agrid_id_path(_g, fromId.X, fromId.Y, toId.X, toId.Y, allowPartialPath ? 1 : 0, cells, n);
        for (int i = 0; i < n; i++) path.Add(new Vector2I(cells[2 * i], cells[2 * i + 1]));
        return path;
    }

    public Vector2[] GetPointPath(Vector2I fromId, Vector2I toId, bool allowPartialPath = false)
    {
        Collections.Array<Vector2I> ids = GetIdPath(fromId, toId, allowPartialPath);
        var pts = new Vector2[ids.Count];
        for (int i = 0; i < pts.Length; i++) pts[i] = GetPointPosition(ids[i]);
        return pts;
    }
}
