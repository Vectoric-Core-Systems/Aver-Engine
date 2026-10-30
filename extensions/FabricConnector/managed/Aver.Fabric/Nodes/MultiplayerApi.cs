// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: MultiplayerApi (Godot's MultiplayerAPI), the node-facing half of the high-level multiplayer: Node.Multiplayer, authority, Rpc / RpcId /
// RpcConfig, Callable.Rpc and the SceneTree's per-branch APIs. SceneMultiplayer.cs is the implementation, MultiplayerSpawner.cs and
// MultiplayerSynchronizer.cs the replication nodes. Wire format and rules: docs/plans/fabric/G6_NETWORK.md, NET2.

using System.Reflection;
using GArray = Aver.Fabric.Collections.Array;
using GDict = Aver.Fabric.Collections.Dictionary;

namespace Aver.Fabric;

/// <summary>The API behind a branch of the scene tree: its multiplayer peer, the peers it sees and the RPC entry point (Godot's MultiplayerAPI).</summary>
public abstract class MultiplayerApi : RefCounted
{
    /// <summary>Who may call an RPC: nobody, any peer, or only the node's multiplayer authority.</summary>
    public enum RpcMode { Disabled = 0, AnyPeer = 1, Authority = 2 }

    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName PeerConnected = "peer_connected";
        public static readonly StringName PeerDisconnected = "peer_disconnected";
        public static readonly StringName ConnectedToServer = "connected_to_server";
        public static readonly StringName ConnectionFailed = "connection_failed";
        public static readonly StringName ServerDisconnected = "server_disconnected";
    }

    public event Action<long> PeerConnected { add => Connect(SignalName.PeerConnected, Callable.From(value)); remove => Disconnect(SignalName.PeerConnected, Callable.From(value)); }
    public event Action<long> PeerDisconnected { add => Connect(SignalName.PeerDisconnected, Callable.From(value)); remove => Disconnect(SignalName.PeerDisconnected, Callable.From(value)); }
    public event Action ConnectedToServer { add => Connect(SignalName.ConnectedToServer, Callable.From(value)); remove => Disconnect(SignalName.ConnectedToServer, Callable.From(value)); }
    public event Action ConnectionFailed { add => Connect(SignalName.ConnectionFailed, Callable.From(value)); remove => Disconnect(SignalName.ConnectionFailed, Callable.From(value)); }
    public event Action ServerDisconnected { add => Connect(SignalName.ServerDisconnected, Callable.From(value)); remove => Disconnect(SignalName.ServerDisconnected, Callable.From(value)); }

    /// <summary>The transport. An API starts with an <see cref="OfflineMultiplayerPeer"/> (id 1, the server, no packets); assigning null puts it back.</summary>
    public abstract MultiplayerPeer? MultiplayerPeer { get; set; }

    public abstract bool HasMultiplayerPeer();
    public abstract int GetUniqueId();
    public abstract bool IsServer();

    /// <summary>The peer whose RPC is running right now, or 0 outside an RPC (and in a local call).</summary>
    public abstract int GetRemoteSenderId();

    /// <summary>The ids of the connected peers, ascending (a client sees the server and, with server_relay, the other clients).</summary>
    public abstract int[] GetPeers();

    /// <summary>Moves the network: receives, dispatches and (for the scene API) replicates. The scene tree calls it once per frame.</summary>
    public abstract void Poll();

    /// <summary>Calls <paramref name="method"/> of <paramref name="object"/> on the peer (0 = all, negative = all but that peer).</summary>
    public abstract Error Rpc(int peer, GodotObject @object, StringName method, GArray? args = null);

    public virtual Error ObjectConfigurationAdd(GodotObject? @object, Variant configuration) => Error.Unavailable;
    public virtual Error ObjectConfigurationRemove(GodotObject? @object, Variant configuration) => Error.Unavailable;

    public static MultiplayerApi CreateDefaultInterface() => new SceneMultiplayer();

    /// <summary>The class a new scene tree makes its default API from; only "SceneMultiplayer" exists.</summary>
    public static StringName GetDefaultInterface() => "SceneMultiplayer";

    public static void SetDefaultInterface(StringName @interface)
    {
        if (@interface.ToString() != "SceneMultiplayer") Aver.Scripting.Log.Error($"[Fabric] MultiplayerAPI: there is no interface '{@interface}' (SceneMultiplayer is the only one)");
    }

    /// <summary>The text of an RPC mode, as Godot's error messages spell it.</summary>
    internal static string ModeName(RpcMode m) => m switch { RpcMode.AnyPeer => "any_peer", RpcMode.Disabled => "disabled", _ => "authority" };
}

/// <summary>One RPC method's settings (what a [Rpc] attribute or Node.RpcConfig records).</summary>
internal sealed record RpcEntry(string Name, MultiplayerApi.RpcMode Mode, bool CallLocal, MultiplayerPeer.TransferModeEnum Transfer, int Channel)
{
    private static readonly Dictionary<System.Type, Dictionary<string, RpcEntry>> s_byType = new();

    /// <summary>The [Rpc] methods of a type and its bases, by method name.</summary>
    internal static Dictionary<string, RpcEntry> Of(System.Type type)
    {
        lock (s_byType)
        {
            if (s_byType.TryGetValue(type, out Dictionary<string, RpcEntry>? hit)) return hit;
            var map = new Dictionary<string, RpcEntry>(StringComparer.Ordinal);
            for (System.Type? t = type; t != null && t != typeof(object); t = t.BaseType)
                foreach (MethodInfo m in t.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                    if (m.GetCustomAttribute<RpcAttribute>() is { } a && !map.ContainsKey(m.Name))
                        map[m.Name] = new RpcEntry(m.Name, a.Mode, a.CallLocal, a.TransferMode, a.TransferChannel);
            s_byType[type] = map;
            return map;
        }
    }

    internal GDict ToDictionary() => new()
    {
        ["rpc_mode"] = (int)Mode, ["transfer_mode"] = (int)Transfer, ["call_local"] = CallLocal, ["channel"] = Channel,
    };

    internal static RpcEntry FromDictionary(string name, GDict d)
    {
        MultiplayerApi.RpcMode mode = d.TryGetValue("rpc_mode", out Variant m) ? (MultiplayerApi.RpcMode)m.AsInt32() : MultiplayerApi.RpcMode.Authority;
        MultiplayerPeer.TransferModeEnum tm = d.TryGetValue("transfer_mode", out Variant t) ? (MultiplayerPeer.TransferModeEnum)t.AsInt32() : MultiplayerPeer.TransferModeEnum.Reliable;
        bool local = d.TryGetValue("call_local", out Variant c) && c.AsBool();
        int ch = d.TryGetValue("channel", out Variant h) ? h.AsInt32() : 0;
        return new RpcEntry(name, mode, local, tm, ch);
    }
}

public partial class Node
{
    private int _authority = MultiplayerPeer.TargetPeerServer;
    private Dictionary<string, RpcEntry?>? _rpcOverrides;

    // A spawned node takes the exact name the authority gave it, auto-generated "@Class@2" names included (the Name setter would strip the '@').
    internal void AdoptName(string name)
    {
        if (GetParent() == null && name.Length > 0) _name = name;
    }

    /// <summary>The multiplayer API of the branch this node is in: the one a SceneTree.SetMultiplayer call named for an ancestor path, else the tree's default. Null outside the tree.</summary>
    public MultiplayerApi? Multiplayer => IsInsideTree() ? SceneTree.Instance.GetMultiplayer(new NodePath(GetPath2())) : null;

    /// <summary>The peer that owns this node: it may call authority-mode RPCs and sends the node's synchronizer state. The server (1) unless set.</summary>
    public int GetMultiplayerAuthority() => _authority;

    /// <summary>Gives the node (and, by default, every node below it) to a peer.</summary>
    public void SetMultiplayerAuthority(int id, bool recursive = true)
    {
        _authority = id;
        if (!recursive) return;
        foreach (Node c in ChildrenSnapshot()) c.SetMultiplayerAuthority(id, true);
    }

    /// <summary>True when this peer owns the node (always true offline).</summary>
    public bool IsMultiplayerAuthority() => Multiplayer is { } mp ? mp.GetUniqueId() == _authority : _authority == MultiplayerPeer.TargetPeerServer;

    // ---- RPC ----

    /// <summary>Calls an [Rpc] method on every other peer (and here too when it is call_local). The same method must exist on the node of the same path on the other side.</summary>
    public Error Rpc(StringName method, params Variant[] args) => RpcInternal(MultiplayerPeer.TargetPeerBroadcast, method.ToString(), args);

    /// <summary>Calls an [Rpc] method on one peer (negative: on everyone but that peer).</summary>
    public Error RpcId(long peerId, StringName method, params Variant[] args) => RpcInternal((int)peerId, method.ToString(), args);

    private Error RpcInternal(int target, string method, Variant[] args)
    {
        if (!IsInsideTree())
        {
            Aver.Scripting.Log.Error($"[Fabric] rpc '{method}': the node '{Name}' is not inside the tree");
            return Error.Unconfigured;
        }
        return Multiplayer is SceneMultiplayer mp ? mp.SendRpc(this, target, method, args ?? System.Array.Empty<Variant>()) : Error.Unconfigured;
    }

    /// <summary>Sets (or, with nil, removes) one method's RPC settings: a dictionary with rpc_mode, transfer_mode, call_local and channel (Godot's rpc_config).</summary>
    public void RpcConfig(StringName method, Variant config)
    {
        _rpcOverrides ??= new Dictionary<string, RpcEntry?>(StringComparer.Ordinal);
        string name = method.ToString();
        foreach (string c in MemberAccess.Candidates(name))   // a Godot-spelled name settles on the C# method it stands for
            if (GetType().GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy).Any(m => m.Name == c)) { name = c; break; }
        if (config.IsNil) { _rpcOverrides[name] = null; return; }
        _rpcOverrides[name] = config.AsGodotDictionary() is { } d ? RpcEntry.FromDictionary(name, d) : null;
    }

    /// <summary>Every RPC method of this node as a dictionary of settings, by name (Godot's get_rpc_config).</summary>
    public Variant GetRpcConfig()
    {
        var all = new GDict();
        foreach (KeyValuePair<string, RpcEntry> kv in RpcEntry.Of(GetType())) all[kv.Key] = kv.Value.ToDictionary();
        if (_rpcOverrides != null)
            foreach (KeyValuePair<string, RpcEntry?> kv in _rpcOverrides)
            {
                if (kv.Value == null) all.Remove(kv.Key);
                else all[kv.Key] = kv.Value.ToDictionary();
            }
        return all;
    }

    /// <summary>The settings a call by this name uses: an override first, then the attribute. The name may be written as Godot or as C# does.</summary>
    internal RpcEntry? FindRpc(string method)
    {
        Dictionary<string, RpcEntry> declared = RpcEntry.Of(GetType());
        foreach (string c in MemberAccess.Candidates(method))
        {
            if (_rpcOverrides != null && _rpcOverrides.TryGetValue(c, out RpcEntry? o)) { if (o != null) return o; continue; }
            if (declared.TryGetValue(c, out RpcEntry? e)) return e;
        }
        return null;
    }
}

public partial class SceneTree
{
    private MultiplayerApi? _defaultMultiplayer;
    private readonly Dictionary<string, MultiplayerApi> _customMultiplayers = new(StringComparer.Ordinal);

    /// <summary>Whether the tree polls its multiplayer APIs at the head of every process frame (Godot's multiplayer_poll). Off, a script calls <c>Poll()</c> itself.</summary>
    public bool MultiplayerPoll { get; set; } = true;

    /// <summary>The API for a path: the one set for the deepest ancestor path of it, else the default. Without a path the default.</summary>
    public MultiplayerApi GetMultiplayer(NodePath? forPath = null)
    {
        _defaultMultiplayer ??= new SceneMultiplayer();
        if (forPath == null || forPath.IsEmpty || _customMultiplayers.Count == 0) return _defaultMultiplayer;
        string path = forPath.ToString();
        int colon = path.IndexOf(':');
        if (colon >= 0) path = path[..colon];
        MultiplayerApi? best = null;
        int bestLen = -1;
        foreach (KeyValuePair<string, MultiplayerApi> kv in _customMultiplayers)
        {
            string k = kv.Key;
            bool within = path == k || (path.Length > k.Length && path.StartsWith(k, StringComparison.Ordinal) && path[k.Length] == '/');
            if (within && k.Length > bestLen) { best = kv.Value; bestLen = k.Length; }
        }
        return best ?? _defaultMultiplayer;
    }

    /// <summary>Gives a branch its own API. With no path it replaces the default one. Set this before the branch's nodes enter the tree: spawners and synchronizers attach to the API they find then.</summary>
    public void SetMultiplayer(MultiplayerApi multiplayer, NodePath? rootPath = null)
    {
        ArgumentNullException.ThrowIfNull(multiplayer);
        if (rootPath == null || rootPath.IsEmpty)
        {
            _defaultMultiplayer = multiplayer;
            return;
        }
        string key = rootPath.ToString();
        if (!key.StartsWith('/')) key = "/root/" + key;
        key = key.TrimEnd('/');
        _customMultiplayers[key] = multiplayer;
        if (multiplayer is SceneMultiplayer sm) sm.RootPath = new NodePath(key);
    }

    // Every API the tree owns, once each, for the frame poll.
    private List<MultiplayerApi> AllMultiplayers()
    {
        var all = new List<MultiplayerApi>();
        if (_defaultMultiplayer != null) all.Add(_defaultMultiplayer);
        foreach (MultiplayerApi a in _customMultiplayers.Values) if (!all.Contains(a)) all.Add(a);
        return all;
    }

    internal void PollMultiplayers(double dt, bool physics)
    {
        if (!MultiplayerPoll || (_defaultMultiplayer == null && _customMultiplayers.Count == 0)) return;   // a game that never asks for an API pays nothing
        foreach (MultiplayerApi api in AllMultiplayers())
        {
            if (api is not SceneMultiplayer sm) { if (!physics) api.Poll(); continue; }
            if (physics) sm.PhysicsTick();
            else sm.Tick(dt);
        }
    }
}
