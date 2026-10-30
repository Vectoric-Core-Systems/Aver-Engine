// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The navigation nodes of Godot's 3D set: NavigationRegion3D (a navigation mesh in the world), NavigationLink3D (a one-off connection between two
// points), NavigationObstacle3D (avoidance and bake-time carving) and NavigationAgent3D (path following and avoidance for the node it is under).
// Godot's space and units; the engine's navigation map does the work (NavigationServer3D). docs/plans/fabric/G5_PARITY.md, N1.

namespace Aver.Fabric;

/// <summary>A piece of navigation mesh placed in the world (Godot's NavigationRegion3D). Its mesh is in the node's own space and follows the node.</summary>
public class NavigationRegion3D : Node3D
{
    public new class SignalName : Node3D.SignalName
    {
        public static readonly StringName NavigationMeshChanged = "navigation_mesh_changed";
        public static readonly StringName BakeFinished = "bake_finished";
    }

    public event Action NavigationMeshChanged { add => Connect(SignalName.NavigationMeshChanged, Callable.From(value)); remove => Disconnect(SignalName.NavigationMeshChanged, Callable.From(value)); }
    public event Action BakeFinished { add => Connect(SignalName.BakeFinished, Callable.From(value)); remove => Disconnect(SignalName.BakeFinished, Callable.From(value)); }

    private NavigationMesh? _mesh;
    private bool _enabled = true, _useEdge = true;
    private uint _layers = 1;
    private float _enter, _travel = 1f;
    private Rid _region, _mapOverride;
    private Transform3D _lastPushed;
    private bool _pushedOnce, _baking;

    public NavigationMesh? NavigationMesh
    {
        get => _mesh;
        set
        {
            if (_mesh == value) return;
            if (_mesh != null) _mesh.Changed -= OnMeshChanged;
            _mesh = value;
            if (_mesh != null) _mesh.Changed += OnMeshChanged;
            PushMesh();
            EmitSignal(SignalName.NavigationMeshChanged);
        }
    }

    public bool Enabled { get => _enabled; set { _enabled = value; if (_region.IsValid) NavigationServer3D.RegionSetEnabled(_region, value); } }
    public bool UseEdgeConnections { get => _useEdge; set { _useEdge = value; if (_region.IsValid) NavigationServer3D.RegionSetUseEdgeConnections(_region, value); } }
    public uint NavigationLayers { get => _layers; set { _layers = value; if (_region.IsValid) NavigationServer3D.RegionSetNavigationLayers(_region, value); } }
    public float EnterCost { get => _enter; set { _enter = Math.Max(value, 0f); if (_region.IsValid) NavigationServer3D.RegionSetEnterCost(_region, _enter); } }
    public float TravelCost { get => _travel; set { _travel = Math.Max(value, 0f); if (_region.IsValid) NavigationServer3D.RegionSetTravelCost(_region, _travel); } }

    public bool GetNavigationLayerValue(int layerNumber) => (NavigationLayers & (1u << (layerNumber - 1))) != 0;

    public void SetNavigationLayerValue(int layerNumber, bool value) =>
        NavigationLayers = value ? NavigationLayers | (1u << (layerNumber - 1)) : NavigationLayers & ~(1u << (layerNumber - 1));

    /// <summary>The server's handle of this region (made on first use; empty without Aver.Nav.Abi).</summary>
    public Rid GetRid()
    {
        EnsureRegion();
        return _region;
    }

    public Rid GetRegionRid() => GetRid();

    public Rid GetNavigationMap() => _mapOverride.IsValid ? _mapOverride : (_region.IsValid ? NavigationServer3D.RegionGetMap(_region) : default);

    public void SetNavigationMap(Rid navigationMap)
    {
        _mapOverride = navigationMap;
        EnsureRegion();
        if (_region.IsValid && IsInsideTree()) NavigationServer3D.RegionSetMap(_region, navigationMap);
    }

    /// <summary>The bounds of the navigation mesh in global space.</summary>
    public Aabb GetBounds()
    {
        if (_mesh == null || _mesh.Vertices.Length == 0) return default;
        Transform3D t = IsInsideTree() ? GlobalTransform : Transform3D.Identity;
        Vector3 lo = t * _mesh.Vertices[0], hi = lo;
        foreach (Vector3 v in _mesh.Vertices)
        {
            Vector3 w = t * v;
            lo = new Vector3(Math.Min(lo.X, w.X), Math.Min(lo.Y, w.Y), Math.Min(lo.Z, w.Z));
            hi = new Vector3(Math.Max(hi.X, w.X), Math.Max(hi.Y, w.Y), Math.Max(hi.Z, w.Z));
        }
        return new Aabb(lo, hi - lo);
    }

    public bool IsBaking() => _baking;

    /// <summary>Aver extension, written by the converter for a mesh that was never baked in the editor: bake it from the geometry below this node when the node is
    /// ready (Godot leaves such a mesh empty until <see cref="BakeNavigationMesh"/> is called).</summary>
    public bool BakeOnReady { get; set; }

    internal override void OnReadying()
    {
        base.OnReadying();
        if (BakeOnReady && _mesh != null && _mesh.GetPolygonCount() == 0) BakeNavigationMesh(true);
    }

    /// <summary>Bakes the navigation mesh from the geometry under this node (the mesh's own settings say which), on a worker thread or now. Emits
    /// <c>bake_finished</c> when it is done.</summary>
    public void BakeNavigationMesh(bool onThread = true)
    {
        if (_mesh == null) { GD.PushError("NavigationRegion3D.bake_navigation_mesh: the region has no NavigationMesh"); return; }
        if (_baking) { GD.PushError("NavigationRegion3D.bake_navigation_mesh: a bake is already running"); return; }
        var data = new NavigationMeshSourceGeometryData3D();
        NavigationServer3D.ParseSourceGeometryData(_mesh, data, this);
        _baking = true;
        Callable done = Callable.From(OnBakeDone);
        if (onThread) NavigationServer3D.BakeFromSourceGeometryDataAsync(_mesh, data, done);
        else NavigationServer3D.BakeFromSourceGeometryData(_mesh, data, done);
        if (!NavNative.Available) _baking = false;
    }

    private void OnBakeDone()
    {
        _baking = false;
        EmitSignal(SignalName.BakeFinished);
    }

    private void OnMeshChanged()
    {
        PushMesh();
        EmitSignal(SignalName.NavigationMeshChanged);
    }

    private void EnsureRegion()
    {
        if (_region.IsValid || !NavNative.Available) return;
        _region = NavigationServer3D.RegionCreate();
        NavigationServer3D.RegionSetOwnerId(_region, GetInstanceId());
        NavigationServer3D.RegionSetEnabled(_region, _enabled);
        NavigationServer3D.RegionSetUseEdgeConnections(_region, _useEdge);
        NavigationServer3D.RegionSetNavigationLayers(_region, _layers);
        NavigationServer3D.RegionSetEnterCost(_region, _enter);
        NavigationServer3D.RegionSetTravelCost(_region, _travel);
    }

    private void PushMesh()
    {
        if (!_region.IsValid || !IsInsideTree()) return;
        _lastPushed = GlobalTransform;
        _pushedOnce = true;
        NavigationServer3D.RegionSetNavigationMesh(_region, _mesh, _lastPushed);
    }

    /// <summary>Re-places the mesh when the node moved.</summary>
    internal void SyncTransform()
    {
        if (!_region.IsValid || !IsInsideTree()) return;
        if (!_pushedOnce || !GlobalTransform.IsEqualApprox(_lastPushed)) PushMesh();
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        EnsureRegion();
        if (!_region.IsValid) return;
        NavigationServer3D.RegionSetMap(_region, _mapOverride.IsValid ? _mapOverride : NavigationServer3D.GetDefaultMapRid());
        _pushedOnce = false;
        PushMesh();
        if (!NavigationServer3D.Regions.Contains(this)) NavigationServer3D.Regions.Add(this);
    }

    internal override void OnExitingTree()
    {
        NavigationServer3D.Regions.Remove(this);
        if (_region.IsValid)
        {
            NavigationServer3D.FreeRid(_region);
            _region = default;
        }
        base.OnExitingTree();
    }

    internal override bool UsesInternalPhysicsProcess => true;
    internal override void InternalPhysicsProcess(double delta) => NavigationServer3D.Process(delta);
}

/// <summary>A connection between two points of a navigation map (Godot's NavigationLink3D), for jumps, ladders and teleports. Start and end are in the node's
/// own space.</summary>
public class NavigationLink3D : Node3D
{
    private bool _enabled = true, _bidirectional = true;
    private uint _layers = 1;
    private float _enter, _travel = 1f;
    private Vector3 _start, _end;
    private Rid _link, _mapOverride;
    private Transform3D _lastPushed;
    private bool _pushedOnce;

    public bool Enabled { get => _enabled; set { _enabled = value; if (_link.IsValid) NavigationServer3D.LinkSetEnabled(_link, value); } }
    public bool Bidirectional { get => _bidirectional; set { _bidirectional = value; if (_link.IsValid) NavigationServer3D.LinkSetBidirectional(_link, value); } }
    public uint NavigationLayers { get => _layers; set { _layers = value; if (_link.IsValid) NavigationServer3D.LinkSetNavigationLayers(_link, value); } }
    public float EnterCost { get => _enter; set { _enter = Math.Max(value, 0f); if (_link.IsValid) NavigationServer3D.LinkSetEnterCost(_link, _enter); } }
    public float TravelCost { get => _travel; set { _travel = Math.Max(value, 0f); if (_link.IsValid) NavigationServer3D.LinkSetTravelCost(_link, _travel); } }
    public Vector3 StartPosition { get => _start; set { _start = value; Push(); } }
    public Vector3 EndPosition { get => _end; set { _end = value; Push(); } }

    public Vector3 GlobalStartPosition
    {
        get => IsInsideTree() ? GlobalTransform * _start : _start;
        set => StartPosition = IsInsideTree() ? GlobalTransform.AffineInverse() * value : value;
    }

    public Vector3 GlobalEndPosition
    {
        get => IsInsideTree() ? GlobalTransform * _end : _end;
        set => EndPosition = IsInsideTree() ? GlobalTransform.AffineInverse() * value : value;
    }

    public bool GetNavigationLayerValue(int layerNumber) => (NavigationLayers & (1u << (layerNumber - 1))) != 0;

    public void SetNavigationLayerValue(int layerNumber, bool value) =>
        NavigationLayers = value ? NavigationLayers | (1u << (layerNumber - 1)) : NavigationLayers & ~(1u << (layerNumber - 1));

    public Rid GetRid()
    {
        EnsureLink();
        return _link;
    }

    public Rid GetNavigationMap() => _mapOverride.IsValid ? _mapOverride : (_link.IsValid ? NavigationServer3D.LinkGetMap(_link) : default);

    public void SetNavigationMap(Rid navigationMap)
    {
        _mapOverride = navigationMap;
        EnsureLink();
        if (_link.IsValid && IsInsideTree()) NavigationServer3D.LinkSetMap(_link, navigationMap);
    }

    private void EnsureLink()
    {
        if (_link.IsValid || !NavNative.Available) return;
        _link = NavigationServer3D.LinkCreate();
        NavigationServer3D.LinkSetOwnerId(_link, GetInstanceId());
        NavigationServer3D.LinkSetEnabled(_link, _enabled);
        NavigationServer3D.LinkSetBidirectional(_link, _bidirectional);
        NavigationServer3D.LinkSetNavigationLayers(_link, _layers);
        NavigationServer3D.LinkSetEnterCost(_link, _enter);
        NavigationServer3D.LinkSetTravelCost(_link, _travel);
    }

    private void Push()
    {
        if (!_link.IsValid || !IsInsideTree()) return;
        _lastPushed = GlobalTransform;
        _pushedOnce = true;
        NavigationServer3D.LinkSetStartPosition(_link, _lastPushed * _start);
        NavigationServer3D.LinkSetEndPosition(_link, _lastPushed * _end);
    }

    internal void SyncTransform()
    {
        if (!_link.IsValid || !IsInsideTree()) return;
        if (!_pushedOnce || !GlobalTransform.IsEqualApprox(_lastPushed)) Push();
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        EnsureLink();
        if (!_link.IsValid) return;
        NavigationServer3D.LinkSetMap(_link, _mapOverride.IsValid ? _mapOverride : NavigationServer3D.GetDefaultMapRid());
        _pushedOnce = false;
        Push();
        if (!NavigationServer3D.Links.Contains(this)) NavigationServer3D.Links.Add(this);
    }

    internal override void OnExitingTree()
    {
        NavigationServer3D.Links.Remove(this);
        if (_link.IsValid)
        {
            NavigationServer3D.FreeRid(_link);
            _link = default;
        }
        base.OnExitingTree();
    }

    internal override bool UsesInternalPhysicsProcess => true;
    internal override void InternalPhysicsProcess(double delta) => NavigationServer3D.Process(delta);
}

/// <summary>Something avoidance agents steer around (Godot's NavigationObstacle3D): a round obstacle with a <see cref="Radius"/>, or a wall polygon
/// (<see cref="Vertices"/>, in the node's space) that agents are kept out of (wound clockwise seen from above) or inside (counter-clockwise). It can also
/// carve a hole in a navigation mesh when that is baked (<see cref="AffectNavigationMesh"/>).</summary>
public class NavigationObstacle3D : Node3D
{
    private float _radius, _height = 1f;
    private Vector3[] _vertices = System.Array.Empty<Vector3>();
    private Vector3 _velocity;
    private bool _avoidance = true, _paused;
    private uint _avoidanceLayers = 1;
    private Rid _obstacle, _mapOverride;
    private Transform3D _lastPushed;
    private bool _pushedOnce;

    public float Radius { get => _radius; set { _radius = Math.Max(value, 0f); if (_obstacle.IsValid) NavigationServer3D.ObstacleSetRadius(_obstacle, _radius); } }
    public float Height { get => _height; set { _height = Math.Max(value, 0f); if (_obstacle.IsValid) NavigationServer3D.ObstacleSetHeight(_obstacle, _height); } }
    public Vector3 Velocity { get => _velocity; set { _velocity = value; PushMotion(); } }
    public bool AvoidanceEnabled { get => _avoidance; set { _avoidance = value; if (_obstacle.IsValid) NavigationServer3D.ObstacleSetAvoidanceEnabled(_obstacle, value); } }
    public uint AvoidanceLayers { get => _avoidanceLayers; set { _avoidanceLayers = value; if (_obstacle.IsValid) NavigationServer3D.ObstacleSetAvoidanceLayers(_obstacle, value); } }
    public bool Paused { get => _paused; set { _paused = value; if (_obstacle.IsValid) NavigationServer3D.ObstacleSetPaused(_obstacle, value); } }
    public bool AffectNavigationMesh { get; set; }
    public bool CarveNavigationMesh { get; set; }
    public bool Use3DAvoidance { get; set; }

    /// <summary>The wall polygon in the node's space (fewer than three vertices: a round obstacle).</summary>
    public Vector3[] Vertices { get => _vertices; set { _vertices = value ?? System.Array.Empty<Vector3>(); PushVertices(); } }

    public bool GetAvoidanceLayerValue(int layerNumber) => (AvoidanceLayers & (1u << (layerNumber - 1))) != 0;

    public void SetAvoidanceLayerValue(int layerNumber, bool value) =>
        AvoidanceLayers = value ? AvoidanceLayers | (1u << (layerNumber - 1)) : AvoidanceLayers & ~(1u << (layerNumber - 1));

    public Rid GetRid()
    {
        Ensure();
        return _obstacle;
    }

    public Rid GetNavigationMap() => _mapOverride.IsValid ? _mapOverride : (_obstacle.IsValid ? NavigationServer3D.ObstacleGetMap(_obstacle) : default);

    public void SetNavigationMap(Rid navigationMap)
    {
        _mapOverride = navigationMap;
        Ensure();
        if (_obstacle.IsValid && IsInsideTree()) NavigationServer3D.ObstacleSetMap(_obstacle, navigationMap);
    }

    private void Ensure()
    {
        if (_obstacle.IsValid || !NavNative.Available) return;
        _obstacle = NavigationServer3D.ObstacleCreate();
        NavigationServer3D.ObstacleSetAvoidanceEnabled(_obstacle, _avoidance);
        NavigationServer3D.ObstacleSetPaused(_obstacle, _paused);
        NavigationServer3D.ObstacleSetAvoidanceLayers(_obstacle, _avoidanceLayers);
        NavigationServer3D.ObstacleSetRadius(_obstacle, _radius);
        NavigationServer3D.ObstacleSetHeight(_obstacle, _height);
    }

    private void PushMotion()
    {
        if (_obstacle.IsValid && IsInsideTree()) NavigationServer3D.ObstacleSetMotion(_obstacle, GlobalPosition, _velocity);
    }

    private void PushVertices()
    {
        if (!_obstacle.IsValid || !IsInsideTree()) return;
        Transform3D t = GlobalTransform;
        NavigationServer3D.ObstacleSetVertices(_obstacle, _vertices.Select(v => t * v).ToArray());
    }

    internal void SyncTransform()
    {
        if (!_obstacle.IsValid || !IsInsideTree()) return;
        if (!_pushedOnce || !GlobalTransform.IsEqualApprox(_lastPushed))
        {
            _lastPushed = GlobalTransform;
            _pushedOnce = true;
            PushMotion();
            PushVertices();
        }
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Ensure();
        if (!_obstacle.IsValid) return;
        NavigationServer3D.ObstacleSetMap(_obstacle, _mapOverride.IsValid ? _mapOverride : NavigationServer3D.GetDefaultMapRid());
        _pushedOnce = false;
        SyncTransform();
        if (!NavigationServer3D.Obstacles.Contains(this)) NavigationServer3D.Obstacles.Add(this);
    }

    internal override void OnExitingTree()
    {
        NavigationServer3D.Obstacles.Remove(this);
        if (_obstacle.IsValid)
        {
            NavigationServer3D.FreeRid(_obstacle);
            _obstacle = default;
        }
        base.OnExitingTree();
    }

    internal override bool UsesInternalPhysicsProcess => true;
    internal override void InternalPhysicsProcess(double delta) => NavigationServer3D.Process(delta);
}

/// <summary>Finds a path for its parent node and follows it, and (with avoidance on) turns the parent's wanted velocity into a safe one (Godot's
/// NavigationAgent3D). The agent does not move anything: a script reads <see cref="GetNextPathPosition"/> and moves the parent.</summary>
public class NavigationAgent3D : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName PathChanged = "path_changed";
        public static readonly StringName TargetReached = "target_reached";
        public static readonly StringName WaypointReached = "waypoint_reached";
        public static readonly StringName LinkReached = "link_reached";
        public static readonly StringName NavigationFinished = "navigation_finished";
        public static readonly StringName VelocityComputed = "velocity_computed";
    }

    public event Action PathChanged { add => Connect(SignalName.PathChanged, Callable.From(value)); remove => Disconnect(SignalName.PathChanged, Callable.From(value)); }
    public event Action TargetReached { add => Connect(SignalName.TargetReached, Callable.From(value)); remove => Disconnect(SignalName.TargetReached, Callable.From(value)); }
    public event Action<Collections.Dictionary> WaypointReached { add => Connect(SignalName.WaypointReached, Callable.From(value)); remove => Disconnect(SignalName.WaypointReached, Callable.From(value)); }
    public event Action<Collections.Dictionary> LinkReached { add => Connect(SignalName.LinkReached, Callable.From(value)); remove => Disconnect(SignalName.LinkReached, Callable.From(value)); }
    public event Action NavigationFinished { add => Connect(SignalName.NavigationFinished, Callable.From(value)); remove => Disconnect(SignalName.NavigationFinished, Callable.From(value)); }
    public event Action<Vector3> VelocityComputed { add => Connect(SignalName.VelocityComputed, Callable.From(value)); remove => Disconnect(SignalName.VelocityComputed, Callable.From(value)); }

    private Rid _agent, _mapOverride;
    private readonly NavigationPathQueryParameters3D _query = new();
    private readonly NavigationPathQueryResult3D _result = new();
    private Vector3 _target;
    private bool _targetSubmitted, _targetReached, _finished = true;
    private int _pathIndex;
    private long _updateFrame = -1;
    private Vector3 _velocity;
    private bool _velocitySubmitted;

    // ---- path settings ----
    public float PathDesiredDistance { get; set; } = 1f;
    public float TargetDesiredDistance { get; set; } = 1f;
    public float PathHeightOffset { get; set; }
    public float PathMaxDistance { get; set; } = 5f;
    public bool SimplifyPath { get; set; }
    public float SimplifyEpsilon { get; set; }
    public PathfindingAlgorithm PathfindingAlgorithm { get; set; } = PathfindingAlgorithm.Astar;
    public PathPostProcessing PathPostprocessing { get; set; } = PathPostProcessing.CorridorFunnel;
    public PathMetadataFlags PathMetadataFlags { get; set; } = PathMetadataFlags.IncludeAll;
    public uint NavigationLayers { get; set; } = 1;
    public bool DebugEnabled { get; set; }
    public bool DebugUseCustom { get; set; }
    public Color DebugPathCustomColor { get; set; } = new(1, 1, 1, 1);
    public float DebugPathCustomPointSize { get; set; } = 4f;

    public bool GetNavigationLayerValue(int layerNumber) => (NavigationLayers & (1u << (layerNumber - 1))) != 0;

    public void SetNavigationLayerValue(int layerNumber, bool value) =>
        NavigationLayers = value ? NavigationLayers | (1u << (layerNumber - 1)) : NavigationLayers & ~(1u << (layerNumber - 1));

    /// <summary>Where the agent is going. Setting it (even to the same value) asks for a new path.</summary>
    public Vector3 TargetPosition
    {
        get => _target;
        set
        {
            _target = value;
            _targetSubmitted = true;
            RequestRepath();
        }
    }

    private void RequestRepath()
    {
        _result.Reset();
        _targetReached = false;
        _finished = false;
        _updateFrame = -1;
    }

    // ---- avoidance settings ----
    private bool _avoidance;
    private float _radius = 0.5f, _height = 1f, _maxSpeed = 10f, _neighbor = 50f, _horizonAgents = 1f, _horizonObstacles;
    private int _maxNeighbors = 10;
    private uint _avoidLayers = 1, _avoidMask = 1;
    private float _priority = 1f;

    public bool AvoidanceEnabled { get => _avoidance; set { _avoidance = value; PushParams(); } }
    public float Radius { get => _radius; set { _radius = Math.Max(value, 0f); PushParams(); } }
    public float Height { get => _height; set { _height = Math.Max(value, 0f); PushParams(); } }
    public float MaxSpeed { get => _maxSpeed; set { _maxSpeed = Math.Max(value, 0f); PushParams(); } }
    public float NeighborDistance { get => _neighbor; set { _neighbor = Math.Max(value, 0f); PushParams(); } }
    public int MaxNeighbors { get => _maxNeighbors; set { _maxNeighbors = Math.Max(value, 0); PushParams(); } }
    public float TimeHorizonAgents { get => _horizonAgents; set { _horizonAgents = Math.Max(value, 0f); PushParams(); } }
    public float TimeHorizonObstacles { get => _horizonObstacles; set { _horizonObstacles = Math.Max(value, 0f); PushParams(); } }
    public uint AvoidanceLayers { get => _avoidLayers; set { _avoidLayers = value; PushParams(); } }
    public uint AvoidanceMask { get => _avoidMask; set { _avoidMask = value; PushParams(); } }
    public float AvoidancePriority { get => _priority; set { _priority = Math.Clamp(value, 0f, 1f); PushParams(); } }
    public bool Use3DAvoidance { get; set; }
    public bool KeepYVelocity { get; set; } = true;
    public bool DebugEnabledAvoidance { get; set; }

    public bool GetAvoidanceLayerValue(int layerNumber) => (AvoidanceLayers & (1u << (layerNumber - 1))) != 0;

    public void SetAvoidanceLayerValue(int layerNumber, bool value) =>
        AvoidanceLayers = value ? AvoidanceLayers | (1u << (layerNumber - 1)) : AvoidanceLayers & ~(1u << (layerNumber - 1));

    public bool GetAvoidanceMaskValue(int maskNumber) => (AvoidanceMask & (1u << (maskNumber - 1))) != 0;

    public void SetAvoidanceMaskValue(int maskNumber, bool value) =>
        AvoidanceMask = value ? AvoidanceMask | (1u << (maskNumber - 1)) : AvoidanceMask & ~(1u << (maskNumber - 1));

    /// <summary>The velocity the parent wants (m/s). With avoidance on, the engine answers with <c>velocity_computed</c> after the next navigation step.</summary>
    public Vector3 Velocity { get => _velocity; set => SetVelocity(value); }

    public void SetVelocity(Vector3 velocity)
    {
        _velocity = velocity;
        if (!_avoidance || !_agent.IsValid) return;
        NavigationServer3D.AgentSetPosition(_agent, ParentPosition());
        NavigationServer3D.AgentSetVelocity(_agent, velocity);
        _velocitySubmitted = true;
    }

    /// <summary>Like <see cref="SetVelocity"/>, for the frame a parent was teleported.</summary>
    public void SetVelocityForced(Vector3 velocity) => SetVelocity(velocity);

    // ---- map and handle ----

    public Rid GetRid()
    {
        Ensure();
        return _agent;
    }

    public Rid GetNavigationMap() => _mapOverride.IsValid ? _mapOverride : (GetParent() is Node3D p && p.GetWorld3D() is { } w ? w.NavigationMap : NavigationServer3D.GetDefaultMapRid());

    public void SetNavigationMap(Rid navigationMap)
    {
        _mapOverride = navigationMap;
        Ensure();
        if (_agent.IsValid) NavigationServer3D.AgentSetMap(_agent, navigationMap);
        RequestRepath();
    }

    private Node3D? ParentNode => GetParent() as Node3D;
    private Vector3 ParentPosition() => ParentNode is { } p && p.IsInsideTree() ? p.GlobalPosition : Vector3.Zero;

    private void Ensure()
    {
        if (_agent.IsValid || !NavNative.Available) return;
        _agent = NavigationServer3D.AgentCreate();
        NavigationServer3D.AgentSetAvoidanceCallback(_agent, default);
        PushParams();
    }

    private void PushParams()
    {
        if (!_agent.IsValid) return;
        var p = new float[(int)NavAgent.ParamCount];
        p[(int)NavAgent.Avoidance] = _avoidance ? 1 : 0;
        p[(int)NavAgent.Paused] = 0;
        p[(int)NavAgent.Radius] = _radius * FabricSpace.CmPerMetre;
        p[(int)NavAgent.Height] = _height * FabricSpace.CmPerMetre;
        p[(int)NavAgent.MaxSpeed] = _maxSpeed * FabricSpace.CmPerMetre;
        p[(int)NavAgent.NeighborDist] = _neighbor * FabricSpace.CmPerMetre;
        p[(int)NavAgent.MaxNeighbors] = _maxNeighbors;
        p[(int)NavAgent.HorizonAgents] = _horizonAgents;
        p[(int)NavAgent.HorizonObstacles] = _horizonObstacles;
        p[(int)NavAgent.Layers] = _avoidLayers;
        p[(int)NavAgent.Mask] = _avoidMask;
        p[(int)NavAgent.Priority] = _priority;
        NavNative.aver_nav_agent_set_params(unchecked((int)(uint)_agent.Id), p);
    }

    // ---- the path ----

    /// <summary>The next position to head for: refreshes the path if needed and passes waypoints the parent has reached.</summary>
    public Vector3 GetNextPathPosition()
    {
        UpdateNavigation();
        Vector3[] path = _result.Path;
        if (path.Length == 0) return ParentPosition();
        return path[Math.Min(_pathIndex, path.Length - 1)] - new Vector3(0, PathHeightOffset, 0);
    }

    public bool IsNavigationFinished()
    {
        UpdateNavigation();
        return _finished;
    }

    public bool IsTargetReached() => _targetReached;

    /// <summary>Whether the last path point is within <see cref="TargetDesiredDistance"/> of the target.</summary>
    public bool IsTargetReachable() => TargetDesiredDistance >= GetFinalPosition().DistanceTo(_target);

    public float DistanceToTarget() => ParentPosition().DistanceTo(_target);

    public Vector3 GetFinalPosition()
    {
        UpdateNavigation();
        Vector3[] path = _result.Path;
        return path.Length == 0 ? Vector3.Zero : path[^1] - new Vector3(0, PathHeightOffset, 0);
    }

    public Vector3[] GetCurrentNavigationPath() => (Vector3[])_result.Path.Clone();
    public int GetCurrentNavigationPathIndex() => _pathIndex;
    public NavigationPathQueryResult3D GetCurrentNavigationResult() => _result;

    /// <summary>The length of the current path in metres, from the first point.</summary>
    public float GetPathLength() => _result.PathLength;

    private void UpdateNavigation()
    {
        if (!_targetSubmitted) return;
        Node3D? parent = ParentNode;
        if (parent == null || !parent.IsInsideTree()) return;
        long frame = NodeHost.Current.IsEngine ? Aver.Framework.Engine.PhysicsFrames : SceneTree.Instance.PhysicsFrameCount;
        if (_updateFrame == frame) return;
        _updateFrame = frame;

        Vector3 origin = parent.GlobalPosition;
        bool reload = false;
        Vector3[] path = _result.Path;
        if (path.Length == 0) reload = true;
        else if (_pathIndex > 0)
        {
            // too far from the line between the waypoint passed and the one ahead
            Vector3 a = path[_pathIndex - 1] - new Vector3(0, PathHeightOffset, 0);
            Vector3 b = path[Math.Min(_pathIndex, path.Length - 1)] - new Vector3(0, PathHeightOffset, 0);
            Vector3 ab = b - a;
            float t = ab.LengthSquared() > 1e-8f ? Math.Clamp((origin - a).Dot(ab) / ab.LengthSquared(), 0f, 1f) : 0f;
            if (origin.DistanceTo(a + ab * t) >= PathMaxDistance) reload = true;
        }

        if (reload)
        {
            _query.StartPosition = origin;
            _query.TargetPosition = _target;
            _query.NavigationLayers = NavigationLayers;
            _query.MetadataFlags = PathMetadataFlags;
            _query.PathfindingAlgorithm = PathfindingAlgorithm;
            _query.PathPostprocessing = PathPostprocessing;
            _query.SimplifyPath = SimplifyPath;
            _query.SimplifyEpsilon = SimplifyEpsilon;
            _query.Map = GetNavigationMap();
            NavigationServer3D.QueryPath(_query, _result);
            _finished = false;
            _pathIndex = 0;
            EmitSignal(SignalName.PathChanged);
            path = _result.Path;
        }
        if (path.Length == 0) return;

        if (!_finished)
        {
            while (origin.DistanceTo(path[_pathIndex] - new Vector3(0, PathHeightOffset, 0)) < PathDesiredDistance)
            {
                int[] types = _result.PathTypes;
                int type = types.Length == path.Length ? types[_pathIndex] : -1;
                var details = new Collections.Dictionary();
                details["position"] = path[_pathIndex];
                details["type"] = type;
                details["rid"] = _result.PathRids.Count == path.Length ? Variant.From(_result.PathRids[_pathIndex]) : Variant.From(new Rid());
                details["owner"] = _result.PathOwnerIds.Length == path.Length ? _result.PathOwnerIds[_pathIndex] : 0L;
                EmitSignal(SignalName.WaypointReached, details);
                if (type == (int)PathSegmentType.Link) EmitSignal(SignalName.LinkReached, details);

                _pathIndex++;
                if (_pathIndex == path.Length)
                {
                    CheckDistanceToTarget();
                    _pathIndex--;
                    _finished = true;
                    _targetSubmitted = false;
                    EmitSignal(SignalName.NavigationFinished);
                    break;
                }
            }
        }
    }

    private void CheckDistanceToTarget()
    {
        if (_targetReached) return;
        if (DistanceToTarget() < TargetDesiredDistance)
        {
            _targetReached = true;
            EmitSignal(SignalName.TargetReached);
        }
    }

    // ---- avoidance, driven by the server pass ----

    internal void SyncToServer()
    {
        if (_agent.IsValid && IsInsideTree() && ParentNode != null) NavigationServer3D.AgentSetPosition(_agent, ParentPosition());
    }

    internal void CollectSafeVelocity()
    {
        if (!_agent.IsValid || !_velocitySubmitted) return;
        var o = new float[3];
        if (NavNative.aver_nav_agent_safe_velocity(unchecked((int)(uint)_agent.Id), o) == 0) return;
        _velocitySubmitted = false;
        Vector3 safe = NavSpace.VelocityFromEngine(o);
        if (!KeepYVelocity) safe = new Vector3(safe.X, 0f, safe.Z);   // avoidance is in the ground plane; the wanted vertical part is kept unless told not to
        if (NavigationServer3D.TryGetAvoidanceCallback(_agent, out Callable cb)) cb.Call(safe);
        EmitSignal(SignalName.VelocityComputed, safe);
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Ensure();
        if (!_agent.IsValid) return;
        NavigationServer3D.AgentSetMap(_agent, GetNavigationMap());
        if (!NavigationServer3D.Agents.Contains(this)) NavigationServer3D.Agents.Add(this);
        SyncToServer();
        RequestRepath();
        _finished = !_targetSubmitted;
    }

    internal override void OnExitingTree()
    {
        NavigationServer3D.Agents.Remove(this);
        if (_agent.IsValid)
        {
            NavigationServer3D.FreeRid(_agent);
            _agent = default;
        }
        base.OnExitingTree();
    }

    internal override bool UsesInternalPhysicsProcess => true;
    internal override void InternalPhysicsProcess(double delta) => NavigationServer3D.Process(delta);
}
