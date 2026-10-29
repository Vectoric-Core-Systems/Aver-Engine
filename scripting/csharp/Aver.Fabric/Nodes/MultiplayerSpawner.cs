// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: MultiplayerSpawner (Godot's node that makes the authority's scenes appear on the other peers). The authority adds a spawnable scene (or calls
// spawn) under spawn_path; every visible peer instances the same scene, with the synchronizers' spawn-mode state, and frees it when the authority does.

namespace Aver.Fabric;

/// <summary>Replicates nodes added below spawn_path from the spawner's multiplayer authority to the other peers (Godot's MultiplayerSpawner).</summary>
public class MultiplayerSpawner : Node
{
    public new class SignalName : Node.SignalName
    {
        public static readonly StringName Spawned = "spawned";
        public static readonly StringName Despawned = "despawned";
    }

    /// <summary>Emitted on a remote peer when the authority's spawn arrived and the node was added.</summary>
    public event Action<Node> Spawned { add => Connect(SignalName.Spawned, Callable.From(value)); remove => Disconnect(SignalName.Spawned, Callable.From(value)); }

    /// <summary>Emitted on a remote peer just before a despawned node is freed.</summary>
    public event Action<Node> Despawned { add => Connect(SignalName.Despawned, Callable.From(value)); remove => Disconnect(SignalName.Despawned, Callable.From(value)); }

    /// <summary>The node the spawned scenes live under, relative to the spawner. Empty (the default) spawns nothing.</summary>
    public NodePath SpawnPath { get; set; } = new("");

    /// <summary>The most nodes this spawner replicates at once; 0 means no limit.</summary>
    public uint SpawnLimit { get; set; }

    /// <summary>Makes the node for <see cref="Spawn"/> from its data, on every peer. It must return a node that is not in the tree; the spawner adds it.</summary>
    public Callable SpawnFunction { get; set; }

    private readonly List<string> _scenes = new();

    /// <summary>The scenes that replicate when added under spawn_path (the scene file paths; the position in this list is what travels on the wire).</summary>
    public string[] SpawnableScenes
    {
        get => _scenes.ToArray();
        set { _scenes.Clear(); if (value != null) _scenes.AddRange(value); }
    }

    public void AddSpawnableScene(string path)
    {
        if (_scenes.Contains(path)) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSpawner: '{path}' is already a spawnable scene"); return; }
        _scenes.Add(path);
    }

    public void ClearSpawnableScenes() => _scenes.Clear();
    public int GetSpawnableSceneCount() => _scenes.Count;
    public string GetSpawnableScene(int index) => index >= 0 && index < _scenes.Count ? _scenes[index] : "";

    /// <summary>Calls spawn_function with the data, adds the node it returns under spawn_path and replicates it. Only the multiplayer authority may; null when it could not.</summary>
    public Node? Spawn(Variant data = default)
    {
        if (!IsInsideTree() || _api == null) { Aver.Scripting.Log.Error("[Fabric] MultiplayerSpawner.spawn: the spawner is not in the tree"); return null; }
        if (!IsMultiplayerAuthority()) { Aver.Scripting.Log.Error("[Fabric] MultiplayerSpawner.spawn: only the multiplayer authority can spawn"); return null; }
        if (!SpawnFunction.IsValid()) { Aver.Scripting.Log.Error("[Fabric] MultiplayerSpawner.spawn: spawn_function is not set"); return null; }
        Node? parent = SpawnNode();
        if (parent == null) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSpawner.spawn: spawn_path '{SpawnPath}' names no node"); return null; }
        if (SpawnLimit > 0 && Local.Count >= SpawnLimit) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSpawner.spawn: the spawn limit ({SpawnLimit}) is reached"); return null; }
        Node? node;
        try { node = SpawnFunction.Call(data).AsGodotObject() as Node; }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSpawner.spawn: spawn_function threw: {ex.GetType().Name}: {ex.Message}"); return null; }
        if (node == null || node.GetParent() != null || node.IsInsideTree())
        {
            Aver.Scripting.Log.Error("[Fabric] MultiplayerSpawner.spawn: spawn_function must return a new node that is not in the tree");
            return null;
        }
        _custom = (node, data);
        try { parent.AddChild(node); }
        finally { _custom = null; }
        return node;
    }

    // ---- what the SceneMultiplayer keeps on the spawner ----

    internal SceneMultiplayer? _api;
    internal readonly Dictionary<Node, SpawnInfo> Local = new();
    internal readonly Dictionary<uint, Node> Remote = new();
    internal uint NextNetId = 1;
    private Node? _watched;
    private (Node Node, Variant Data)? _custom;
    private Action<Node>? _entered, _exiting;

    /// <summary>The node spawn_path names, or null.</summary>
    internal Node? SpawnNode() => SpawnPath.IsEmpty ? null : GetNodeOrNull(SpawnPath);

    /// <summary>The position of a scene file in the spawnable list, or -1. Paths compare by the file they stand for ("res://a.tscn" and "Maps/a.ocworld" are one scene).</summary>
    internal int SceneIndexOf(string sceneFilePath)
    {
        if (sceneFilePath.Length == 0) return -1;
        string key = ScenePathKey(sceneFilePath);
        for (int i = 0; i < _scenes.Count; i++) if (ScenePathKey(_scenes[i]) == key) return i;
        return -1;
    }

    internal override void CopyEngineState(Node copy)
    {
        base.CopyEngineState(copy);
        if (copy is not MultiplayerSpawner c) return;
        c.SpawnPath = SpawnPath;
        c.SpawnLimit = SpawnLimit;
        c.SpawnFunction = SpawnFunction;
        c.SpawnableScenes = SpawnableScenes;
    }

    internal static string ScenePathKey(string path)
    {
        string ext = System.IO.Path.GetExtension(path).ToLowerInvariant();
        return ext is ".tscn" or ".scn" ? SceneTree.ScenePathMapper(path) : path;
    }

    public override void _EnterTree()
    {
        _api = Multiplayer as SceneMultiplayer;
        _api?.RegisterSpawner(this);
        EnsureBound();
    }

    public override void _ExitTree()
    {
        Unbind();
        _api?.UnregisterSpawner(this);
        _api = null;
    }

    // Follows spawn_path: the spawn node may be created after the spawner or the path changed.
    internal void EnsureBound()
    {
        Node? now = IsInsideTree() ? SpawnNode() : null;
        if (ReferenceEquals(now, _watched)) return;
        Unbind();
        if (now == null) return;
        _watched = now;
        _entered = OnChildEntered;
        _exiting = OnChildExiting;
        now.ChildEnteredTree += _entered;
        now.ChildExitingTree += _exiting;
    }

    private void Unbind()
    {
        if (_watched != null && _watched.IsAlive)
        {
            _watched.ChildEnteredTree -= _entered!;
            _watched.ChildExitingTree -= _exiting!;
        }
        _watched = null;
    }

    private void OnChildEntered(Node child)
    {
        if (_api == null || !IsMultiplayerAuthority() || Local.ContainsKey(child)) return;
        if (_custom is { } c && ReferenceEquals(c.Node, child)) { _api.TrackSpawn(this, child, -1, c.Data); return; }
        int index = SceneIndexOf(child.SceneFilePath);
        if (index >= 0) _api.TrackSpawn(this, child, index, default);
    }

    private void OnChildExiting(Node child) => _api?.UntrackSpawn(this, child);
}

/// <summary>A node the spawner's authority replicated: which scene (or custom data) made it and which peers have it.</summary>
internal sealed class SpawnInfo
{
    public MultiplayerSpawner Spawner = null!;
    public Node Node = null!;
    public uint NetId;
    public int SceneIndex;
    public Variant Data;
    public readonly HashSet<int> SentTo = new();
}
