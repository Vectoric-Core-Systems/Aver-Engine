// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: SceneReplicationConfig (the property list a synchronizer replicates) and MultiplayerSynchronizer (Godot's node that copies properties from its
// authority to the other peers). The sending and receiving rules are in SceneMultiplayer.Replication.cs.

using GArray = Aver.Fabric.Collections.Array;
using GDict = Aver.Fabric.Collections.Dictionary;

namespace Aver.Fabric;

/// <summary>The properties a MultiplayerSynchronizer replicates and how (Godot's SceneReplicationConfig). A property path is relative to the synchronizer's root_path: ".:position", ":rotation:y", "Child:visible".</summary>
public class SceneReplicationConfig : Resource
{
    public enum ReplicationMode { Never = 0, Always = 1, OnChange = 2 }

    internal sealed class Prop
    {
        public NodePath Path = new("");
        public bool Spawn = true;
        public ReplicationMode Mode = ReplicationMode.Always;
    }

    internal readonly List<Prop> Props = new();

    private int Find(NodePath path) => Props.FindIndex(p => p.Path == path);

    /// <summary>Adds a property (spawn on, replicated every sync); a path that names no property, or one already there, is refused.</summary>
    public void AddProperty(NodePath path, int index = -1)
    {
        if (path == null || path.IsEmpty || path.GetSubnameCount() == 0)
        {
            Aver.Scripting.Log.Error($"[Fabric] SceneReplicationConfig: '{path}' does not name a property (write it as \".:property\")");
            return;
        }
        if (Find(path) >= 0) { Aver.Scripting.Log.Error($"[Fabric] SceneReplicationConfig: the property '{path}' is already in the list"); return; }
        var prop = new Prop { Path = path };
        if (index < 0 || index > Props.Count) Props.Add(prop);
        else Props.Insert(index, prop);
    }

    public void RemoveProperty(NodePath path)
    {
        int i = Find(path);
        if (i >= 0) Props.RemoveAt(i);
    }

    public bool HasProperty(NodePath path) => Find(path) >= 0;

    public GArray GetProperties() { var a = new GArray(); foreach (Prop p in Props) a.Add(p.Path); return a; }

    public int PropertyGetIndex(NodePath path) => Find(path);

    public bool PropertyGetSpawn(NodePath path) => Find(path) is var i and >= 0 && Props[i].Spawn;
    public void PropertySetSpawn(NodePath path, bool enabled) { if (Find(path) is var i and >= 0) Props[i].Spawn = enabled; }

    public ReplicationMode PropertyGetReplicationMode(NodePath path) => Find(path) is var i and >= 0 ? Props[i].Mode : ReplicationMode.Never;
    public void PropertySetReplicationMode(NodePath path, ReplicationMode mode) { if (Find(path) is var i and >= 0) Props[i].Mode = mode; }

    /// <summary>The older spelling of mode Always.</summary>
    public bool PropertyGetSync(NodePath path) => PropertyGetReplicationMode(path) == ReplicationMode.Always;
    public void PropertySetSync(NodePath path, bool enabled) { if (Find(path) is var i and >= 0) Props[i].Mode = enabled ? ReplicationMode.Always : Props[i].Mode == ReplicationMode.Always ? ReplicationMode.Never : Props[i].Mode; }

    /// <summary>The older spelling of mode OnChange.</summary>
    public bool PropertyGetWatch(NodePath path) => PropertyGetReplicationMode(path) == ReplicationMode.OnChange;
    public void PropertySetWatch(NodePath path, bool enabled) { if (Find(path) is var i and >= 0) Props[i].Mode = enabled ? ReplicationMode.OnChange : Props[i].Mode == ReplicationMode.OnChange ? ReplicationMode.Never : Props[i].Mode; }

    /// <summary>The list as scene text carries it: an array of {"path", "spawn", "replication_mode"} (what the converter writes for Object(SceneReplicationConfig, "properties": ...)).</summary>
    public Variant Properties
    {
        get
        {
            var a = new GArray();
            foreach (Prop p in Props) a.Add(new GDict { ["path"] = p.Path, ["spawn"] = p.Spawn, ["replication_mode"] = (int)p.Mode });
            return a;
        }
        set
        {
            Props.Clear();
            foreach (Variant item in value.AsGodotArray())
            {
                GDict? d = item.AsGodotDictionary();
                if (d == null || !d.TryGetValue("path", out Variant path)) continue;
                var prop = new Prop { Path = path.AsNodePath() };
                if (d.TryGetValue("spawn", out Variant sp)) prop.Spawn = sp.AsBool();
                if (d.TryGetValue("replication_mode", out Variant rm)) prop.Mode = (ReplicationMode)rm.AsInt32();
                else if (d.TryGetValue("sync", out Variant sy) || d.TryGetValue("watch", out Variant wa))
                {
                    bool sync = d.TryGetValue("sync", out Variant s1) && s1.AsBool(), watch = d.TryGetValue("watch", out Variant w1) && w1.AsBool();
                    prop.Mode = watch ? ReplicationMode.OnChange : sync ? ReplicationMode.Always : ReplicationMode.Never;
                }
                if (path.AsNodePath().GetSubnameCount() == 0 || Find(prop.Path) >= 0) continue;
                Props.Add(prop);
            }
        }
    }
}

/// <summary>Copies properties from the node's multiplayer authority to the other peers, every sync interval (Always), when they change (OnChange), and with a spawn (Godot's MultiplayerSynchronizer).</summary>
public class MultiplayerSynchronizer : Node
{
    public enum VisibilityUpdateModeEnum { Idle = 0, Physics = 1, None = 2 }

    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Synchronized = "synchronized";
        public static readonly StringName DeltaSynchronized = "delta_synchronized";
        public static readonly StringName VisibilityChanged = "visibility_changed";
    }

    public event Action Synchronized { add => Connect(SignalName.Synchronized, Callable.From(value)); remove => Disconnect(SignalName.Synchronized, Callable.From(value)); }
    public event Action DeltaSynchronized { add => Connect(SignalName.DeltaSynchronized, Callable.From(value)); remove => Disconnect(SignalName.DeltaSynchronized, Callable.From(value)); }
    public event Action<long> VisibilityChanged { add => Connect(SignalName.VisibilityChanged, Callable.From(value)); remove => Disconnect(SignalName.VisibilityChanged, Callable.From(value)); }

    /// <summary>The node whose properties the config names, relative to this node (default: the parent).</summary>
    public NodePath RootPath { get; set; } = new("..");

    public SceneReplicationConfig? ReplicationConfig { get; set; }

    /// <summary>Seconds between Always-mode syncs; 0 sends on every frame.</summary>
    public double ReplicationInterval { get; set; }

    /// <summary>Seconds between looks at the OnChange properties; 0 looks on every frame.</summary>
    public double DeltaInterval { get; set; }

    private bool _public = true;
    private readonly Dictionary<int, bool> _explicit = new();
    private readonly List<Callable> _filters = new();

    /// <summary>Whether every peer sees this synchronizer unless set_visibility_for says otherwise.</summary>
    public bool PublicVisibility
    {
        get => _public;
        set { if (_public == value) return; _public = value; if (IsInsideTree()) UpdateVisibility(); }
    }

    public VisibilityUpdateModeEnum VisibilityUpdateMode { get; set; } = VisibilityUpdateModeEnum.Idle;

    /// <summary>Shows (or hides) this synchronizer to one peer; peer 0 changes public_visibility instead.</summary>
    public void SetVisibilityFor(int peer, bool visible)
    {
        if (peer == 0) { PublicVisibility = visible; return; }
        _explicit[peer] = visible;
        if (IsInsideTree()) UpdateVisibility(peer);
    }

    public bool GetVisibilityFor(int peer) => _explicit.TryGetValue(peer, out bool v) ? v : _public;

    /// <summary>A filter is called with a peer id and returns whether that peer may see this synchronizer. A peer sees it when its visibility (set_visibility_for, else public_visibility) is on and every filter agrees.</summary>
    public void AddVisibilityFilter(Callable filter) { if (!_filters.Contains(filter)) _filters.Add(filter); }

    public void RemoveVisibilityFilter(Callable filter) => _filters.Remove(filter);

    /// <summary>Evaluates the filters again now, for one peer or (0) all, and acts on a change.</summary>
    public void UpdateVisibility(int forPeer = 0)
    {
        if (!IsInsideTree()) return;
        if (_api is { } api) api.RefreshVisibility(this, forPeer);
    }

    internal bool EvaluateVisibility(int peer)
    {
        bool vis = _explicit.TryGetValue(peer, out bool v) ? v : _public;
        if (!vis) return false;
        foreach (Callable f in _filters.ToArray())
        {
            if (!f.IsValid()) continue;
            try { if (!f.Call((long)peer).AsBool()) return false; }
            catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSynchronizer: a visibility filter threw: {ex.GetType().Name}: {ex.Message}"); return false; }
        }
        return true;
    }

    // ---- state the SceneMultiplayer keeps on the synchronizer ----

    internal SceneMultiplayer? _api;
    internal double SyncAccum, DeltaAccum;
    internal uint SyncSeq;
    internal bool WasAuthority;
    internal bool SizeWarned;
    internal readonly Dictionary<int, bool> Vis = new();
    internal readonly Dictionary<int, uint> RecvSeq = new();
    internal byte[]?[] LastWatch = System.Array.Empty<byte[]?>();

    public override void _EnterTree()
    {
        _api = Multiplayer as SceneMultiplayer;
        _api?.RegisterSynchronizer(this);
    }

    public override void _ExitTree()
    {
        _api?.UnregisterSynchronizer(this);
        _api = null;
        Vis.Clear();
        RecvSeq.Clear();
        WasAuthority = false;
    }

    internal override void CopyEngineState(Node copy)
    {
        base.CopyEngineState(copy);
        if (copy is not MultiplayerSynchronizer c) return;
        c.RootPath = RootPath;
        c.ReplicationConfig = ReplicationConfig;
        c.ReplicationInterval = ReplicationInterval;
        c.DeltaInterval = DeltaInterval;
        c._public = _public;
        c.VisibilityUpdateMode = VisibilityUpdateMode;
    }

    internal Node? SyncRoot() => GetNodeOrNull(RootPath);

    // The object and property a config entry names, in the root's subtree.
    private bool Resolve(int index, out GodotObject? target, out string property)
    {
        target = null;
        property = "";
        SceneReplicationConfig.Prop p = ReplicationConfig!.Props[index];
        Node? root = SyncRoot();
        if (root == null) return false;
        string names = p.Path.GetConcatenatedNames();
        target = names.Length == 0 || names == "." ? root : root.GetNodeOrNull(names);
        property = p.Path.GetConcatenatedSubnames();
        return target != null && property.Length > 0;
    }

    internal bool ReadProperty(int index, out Variant value)
    {
        value = default;
        if (!Resolve(index, out GodotObject? t, out string prop)) return false;
        value = t!.Get(prop);
        return true;
    }

    internal bool WriteProperty(int index, Variant value)
    {
        if (!Resolve(index, out GodotObject? t, out string prop)) return false;
        t!.Set(prop, value);
        return true;
    }

    internal IEnumerable<int> IndicesWhere(Func<SceneReplicationConfig.Prop, bool> pick)
    {
        if (ReplicationConfig == null) yield break;
        for (int i = 0; i < ReplicationConfig.Props.Count; i++)
            if (pick(ReplicationConfig.Props[i])) yield return i;
    }

    internal IEnumerable<int> AlwaysIndices() => IndicesWhere(p => p.Mode == SceneReplicationConfig.ReplicationMode.Always);
    internal IEnumerable<int> WatchIndices() => IndicesWhere(p => p.Mode == SceneReplicationConfig.ReplicationMode.OnChange);
    internal IEnumerable<int> SpawnIndices() => IndicesWhere(p => p.Spawn);
}
