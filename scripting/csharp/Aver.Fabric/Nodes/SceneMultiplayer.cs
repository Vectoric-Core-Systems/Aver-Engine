// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: SceneMultiplayer (Godot's default MultiplayerAPI) over any MultiplayerPeer. Peer admission (with the optional authentication handshake), the peer
// list, server relay (clients reach each other through the server) and RPC dispatch live here; the spawner / synchronizer protocol is in
// SceneMultiplayer.Replication.cs. Everything runs on the game thread inside Poll(). The wire format is Aver's own (docs/plans/fabric/G6_NETWORK.md, NET2).

using System.Diagnostics;
using System.Text;
using GArray = Aver.Fabric.Collections.Array;

namespace Aver.Fabric;

/// <summary>Little-endian packet writer for the multiplayer wire format.</summary>
internal sealed class NetWriter
{
    internal readonly List<byte> B = new();

    internal NetWriter U8(int v) { B.Add((byte)v); return this; }
    internal NetWriter U16(int v) { B.Add((byte)v); B.Add((byte)(v >> 8)); return this; }
    internal NetWriter U32(uint v) { for (int i = 0; i < 4; i++) B.Add((byte)(v >> (8 * i))); return this; }
    internal NetWriter I32(int v) => U32((uint)v);

    internal NetWriter Str(string s)
    {
        byte[] b = Encoding.UTF8.GetBytes(s);
        if (b.Length > 0xFFFF) throw new FormatException("string too long for a packet");
        U16(b.Length);
        B.AddRange(b);
        return this;
    }

    internal NetWriter Bytes(ReadOnlySpan<byte> b) { foreach (byte x in b) B.Add(x); return this; }
    internal Error Var(Variant v) => VariantBinary.Encode(v, B);
    internal byte[] ToArray() => B.ToArray();
}

/// <summary>Bounds-checked packet reader: any short read throws a FormatException the dispatcher turns into a dropped packet.</summary>
internal sealed class NetReader
{
    private readonly byte[] _d;
    internal int Pos;

    internal NetReader(byte[] data, int pos = 0) { _d = data; Pos = pos; }

    internal int Remaining => _d.Length - Pos;

    private void Need(int n) { if (n < 0 || Remaining < n) throw new FormatException("truncated packet"); }

    internal int U8() { Need(1); return _d[Pos++]; }
    internal int U16() { Need(2); int v = _d[Pos] | (_d[Pos + 1] << 8); Pos += 2; return v; }
    internal uint U32() { Need(4); uint v = BitConverter.ToUInt32(_d, Pos); Pos += 4; return v; }
    internal int I32() => (int)U32();

    internal string Str()
    {
        int n = U16();
        Need(n);
        string s = Encoding.UTF8.GetString(_d, Pos, n);
        Pos += n;
        return s;
    }

    internal byte[] Rest() { byte[] r = _d[Pos..]; Pos = _d.Length; return r; }

    internal Variant Var(bool allowObjects)
    {
        if (VariantBinary.Decode(_d.AsSpan(Pos), allowObjects, out Variant v, out int used) != Error.Ok || used <= 0) throw new FormatException("bad Variant");
        Pos += used;
        return v;
    }
}

/// <summary>Godot's SceneMultiplayer: the multiplayer API of a scene tree branch. Replace the peer to start a game: <c>Multiplayer.MultiplayerPeer = peer</c>.</summary>
public partial class SceneMultiplayer : MultiplayerApi
{
    public new class SignalName : MultiplayerApi.SignalName
    {
        public static readonly StringName PeerAuthenticating = "peer_authenticating";
        public static readonly StringName PeerAuthenticationFailed = "peer_authentication_failed";
        public static readonly StringName PeerPacket = "peer_packet";
    }

    public event Action<long> PeerAuthenticating { add => Connect(SignalName.PeerAuthenticating, Callable.From(value)); remove => Disconnect(SignalName.PeerAuthenticating, Callable.From(value)); }
    public event Action<long> PeerAuthenticationFailed { add => Connect(SignalName.PeerAuthenticationFailed, Callable.From(value)); remove => Disconnect(SignalName.PeerAuthenticationFailed, Callable.From(value)); }
    public event Action<long, byte[]> PeerPacket { add => Connect(SignalName.PeerPacket, Callable.From(value)); remove => Disconnect(SignalName.PeerPacket, Callable.From(value)); }

    // Packet commands (the low 7 bits of the first byte; the top bit says a node path follows the node id).
    private const int CmdRpc = 0, CmdConfirm = 1, CmdRelay = 2, CmdRelayed = 3, CmdRoster = 4, CmdRaw = 5, CmdAuth = 6, CmdAuthDone = 7,
                      CmdSpawn = 8, CmdDespawn = 9, CmdSync = 10, CmdDelta = 11;
    private const int PathFlag = 0x80;

    private sealed class Pending { public bool Local, Remote; public double Age; }

    private MultiplayerPeer _peer = new OfflineMultiplayerPeer();
    private Action<long>? _onConnected, _onDisconnected;
    private bool _refuse;
    private MultiplayerPeer.ConnectionStatus _lastStatus = MultiplayerPeer.ConnectionStatus.Connected;
    private readonly SortedSet<int> _peers = new();
    private readonly Dictionary<int, Pending> _pending = new();
    private int _remoteSender;
    private double _time;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private long _lastPollTicks = -1;

    // node path cache: ids we gave nodes and which peers confirmed them; paths peers gave us
    private uint _nextNodeId = 1;

    /// <summary>How many node-addressed packets carried the path and how many only the cached id (read by the tests).</summary>
    internal int FullRefsSent, CachedRefsSent;
    private readonly Dictionary<Node, uint> _nodeIds = new();
    private readonly Dictionary<int, HashSet<uint>> _confirmed = new();
    private readonly Dictionary<int, Dictionary<uint, string>> _recvPaths = new();

    /// <summary>Clients reach each other through the server, and learn of each other's connections, when true (default). Off, a client talks to the server only.</summary>
    public bool ServerRelay { get; set; } = true;

    /// <summary>Whether Variants that carry full objects may be decoded. Objects cannot be encoded here, so this only records the setting.</summary>
    public bool AllowObjectDecoding { get; set; }

    /// <summary>Called with (peer id, data) for each authentication packet; with it set, a new peer is admitted only after both sides called <see cref="CompleteAuth"/>.</summary>
    public Callable AuthCallback { get; set; }

    /// <summary>Seconds a peer may stay unauthenticated before it is disconnected.</summary>
    public double AuthTimeout { get; set; } = 3.0;

    /// <summary>The node whose path the packets are relative to (set by SceneTree.SetMultiplayer); empty means the tree's root window.</summary>
    public NodePath RootPath { get; set; } = new NodePath("");

    /// <summary>The largest synchronization packet (always-mode properties) a synchronizer may send, in bytes.</summary>
    public int MaxSyncPacketSize { get; set; } = 1350;

    /// <summary>The largest delta packet (on-change properties) a synchronizer may send, in bytes.</summary>
    public int MaxDeltaPacketSize { get; set; } = 65535;

    public bool RefuseNewConnections
    {
        get => _refuse;
        set { _refuse = value; _peer.RefuseNewConnections = value; }
    }

    public SceneMultiplayer() { _lastStatus = _peer.GetConnectionStatus(); }

    public override MultiplayerPeer? MultiplayerPeer
    {
        get => _peer;
        set => SetPeer(value);
    }

    private void SetPeer(MultiplayerPeer? peer)
    {
        peer ??= new OfflineMultiplayerPeer();
        if (ReferenceEquals(peer, _peer)) return;
        if (peer.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected)
        {
            Aver.Scripting.Log.Error("[Fabric] SceneMultiplayer: the supplied MultiplayerPeer must be connecting or connected");
            return;
        }
        if (_onConnected != null) { _peer.PeerConnected -= _onConnected; _peer.PeerDisconnected -= _onDisconnected!; }
        ClearState();
        _peer = peer;
        _peer.RefuseNewConnections = _refuse;
        _onConnected = id => OnTransportConnected((int)id);
        _onDisconnected = id => OnTransportDisconnected((int)id);
        _peer.PeerConnected += _onConnected;
        _peer.PeerDisconnected += _onDisconnected;
        _lastStatus = _peer.GetConnectionStatus();
        // a peer that is already connected to someone (an offline peer has nobody): nothing to announce
    }

    private void ClearState()
    {
        _peers.Clear();
        _pending.Clear();
        _confirmed.Clear();
        _recvPaths.Clear();
        _nodeIds.Clear();
        _remoteSender = 0;
        ReplicationReset();
    }

    public override bool HasMultiplayerPeer() => true;

    public override int GetUniqueId() => _peer.GetUniqueId();

    public override bool IsServer() => _peer.IsServer();

    public override int GetRemoteSenderId() => _remoteSender;

    public override int[] GetPeers() => _peers.ToArray();

    /// <summary>The peers that are connected but have not finished authenticating.</summary>
    public int[] GetAuthenticatingPeers() => _pending.Keys.OrderBy(k => k).ToArray();

    public void DisconnectPeer(long id) => _peer.DisconnectPeer((int)id);

    internal double Time => _time;

    // ---- polling ----

    /// <summary>Polls on a script's own call: the time since the last poll drives the synchronizers' intervals. The scene tree calls the timed overload with the frame's delta instead.</summary>
    public override void Poll()
    {
        long now = _clock.ElapsedTicks;
        double dt = _lastPollTicks < 0 ? 0 : (now - _lastPollTicks) / (double)Stopwatch.Frequency;
        _lastPollTicks = now;
        Tick(Math.Min(dt, 0.25));
    }

    internal void Tick(double dt)
    {
        _time += dt;
        _peer.Poll();
        while (_peer.GetAvailablePacketCount() > 0)
        {
            byte[] data = _peer.GetPacket();
            if (_peer.GetPacketError() != Error.Ok && data.Length == 0) break;
            Dispatch(_peer.GetPacketPeer(), data, _peer.GetPacketMode(), _peer.GetPacketChannel(), relayed: false);
        }
        CheckStatus();
        if (_pending.Count > 0) AgePending(dt);
        ReplicationTick(dt);
        if (_nodeIds.Count > 64 && ++_pruneTicks % 120 == 0) _nodeIds.Keys.Where(n => !GodotObject.IsInstanceValid(n)).ToList().ForEach(n => _nodeIds.Remove(n));
    }

    private int _pruneTicks;

    internal void PhysicsTick() => ReplicationPhysicsTick();

    private void CheckStatus()
    {
        MultiplayerPeer.ConnectionStatus now = _peer.GetConnectionStatus();
        if (now == _lastStatus) return;
        MultiplayerPeer.ConnectionStatus before = _lastStatus;
        _lastStatus = now;
        if (now == MultiplayerPeer.ConnectionStatus.Connected && before == MultiplayerPeer.ConnectionStatus.Connecting)
        {
            EmitSignal(SignalName.ConnectedToServer);
        }
        else if (now == MultiplayerPeer.ConnectionStatus.Disconnected)
        {
            if (before == MultiplayerPeer.ConnectionStatus.Connecting) EmitSignal(SignalName.ConnectionFailed);
            else
            {
                // the transport was closed or the server vanished: whoever is left is gone, quietly (the transport's own signal reported the server)
                _peers.Clear(); _pending.Clear(); _confirmed.Clear(); _recvPaths.Clear();
                ReplicationReset();
                if (!_peer.IsServer()) EmitSignal(SignalName.ServerDisconnected);
            }
        }
    }

    // ---- peers coming and going ----

    private void OnTransportConnected(int id)
    {
        if (AuthCallback.IsValid())
        {
            _pending[id] = new Pending();
            EmitSignal(SignalName.PeerAuthenticating, (long)id);
            return;
        }
        Admit(id);
    }

    private void Admit(int id)
    {
        _pending.Remove(id);
        _peers.Add(id);
        if (_peer.IsServer() && ServerRelay)
        {
            int[] others = _peers.Where(p => p != id).ToArray();
            if (others.Length > 0) SendRoster(id, 0, others);
            foreach (int o in others) SendRoster(o, 1, new[] { id });
        }
        EmitSignal(SignalName.PeerConnected, (long)id);
        ReplicationPeerAdmitted(id);
    }

    private void AddRelayed(int id)
    {
        if (id == _peer.GetUniqueId() || id <= 0 || !_peers.Add(id)) return;
        EmitSignal(SignalName.PeerConnected, (long)id);
        ReplicationPeerAdmitted(id);
    }

    private void RemovePeer(int id)
    {
        _confirmed.Remove(id);
        _recvPaths.Remove(id);
        ReplicationPeerRemoved(id);
        EmitSignal(SignalName.PeerDisconnected, (long)id);
    }

    private void OnTransportDisconnected(int id)
    {
        if (_pending.Remove(id))
        {
            EmitSignal(SignalName.PeerAuthenticationFailed, (long)id);
            return;
        }
        if (!_peers.Remove(id)) return;
        if (!_peer.IsServer() && id == MultiplayerPeer.TargetPeerServer)
        {
            // the server was the way to everyone else
            foreach (int other in _peers.ToArray()) { _peers.Remove(other); RemovePeer(other); }
        }
        RemovePeer(id);
        if (_peer.IsServer() && ServerRelay)
            foreach (int o in _peers.ToArray()) SendRoster(o, 2, new[] { id });
    }

    private void SendRoster(int to, int op, int[] ids)
    {
        var w = new NetWriter().U8(CmdRoster).U8(op).U16(ids.Length);
        foreach (int i in ids) w.I32(i);
        SendRaw(to, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0);
    }

    private void HandleRoster(int from, NetReader r)
    {
        if (_peer.IsServer() || from != MultiplayerPeer.TargetPeerServer) return;
        int op = r.U8();
        int n = r.U16();
        for (int i = 0; i < n; i++)
        {
            int id = r.I32();
            if (op == 2)
            {
                if (_peers.Remove(id)) RemovePeer(id);
            }
            else AddRelayed(id);
        }
    }

    // ---- authentication ----

    /// <summary>Sends authentication data to a peer that is still authenticating.</summary>
    public Error SendAuth(long id, byte[] data)
    {
        if (_peer.GetConnectionStatus() != MultiplayerPeer.ConnectionStatus.Connected) return Error.Unconfigured;
        if (!_pending.TryGetValue((int)id, out Pending? p)) return Error.InvalidParameter;
        if (p.Local) return Error.FileBadDrive;
        var w = new NetWriter().U8(CmdAuth).Bytes(data);
        return SendRaw((int)id, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0);
    }

    /// <summary>Says this side is satisfied with a peer; when both sides have, the peer is admitted.</summary>
    public Error CompleteAuth(long id)
    {
        if (_peer.GetConnectionStatus() != MultiplayerPeer.ConnectionStatus.Connected) return Error.Unconfigured;
        if (!_pending.TryGetValue((int)id, out Pending? p)) return Error.InvalidParameter;
        if (p.Local) return Error.FileBadDrive;
        p.Local = true;
        SendRaw((int)id, new[] { (byte)CmdAuthDone }, MultiplayerPeer.TransferModeEnum.Reliable, 0);
        if (p.Remote) Admit((int)id);
        return Error.Ok;
    }

    private void AgePending(double dt)
    {
        foreach (KeyValuePair<int, Pending> kv in _pending.ToArray())
        {
            kv.Value.Age += dt;
            if (kv.Value.Age < AuthTimeout) continue;
            _pending.Remove(kv.Key);
            _peer.DisconnectPeer(kv.Key);
            EmitSignal(SignalName.PeerAuthenticationFailed, (long)kv.Key);
        }
    }

    // ---- sending ----

    /// <summary>Sends bytes to a peer (0 = all, negative = all but one) that arrive there as the peer_packet signal.</summary>
    public Error SendBytes(byte[] bytes, int id = 0, MultiplayerPeer.TransferModeEnum mode = MultiplayerPeer.TransferModeEnum.Reliable, int channel = 0)
    {
        if (bytes == null || bytes.Length == 0)
        {
            Aver.Scripting.Log.Error("[Fabric] SceneMultiplayer.send_bytes: an empty packet cannot be sent");
            return Error.InvalidParameter;
        }
        if (_peer.GetConnectionStatus() != MultiplayerPeer.ConnectionStatus.Connected) return Error.Unconfigured;
        var w = new NetWriter().U8(CmdRaw).Bytes(bytes);
        return SendRaw(id, w.ToArray(), mode, channel);
    }

    /// <summary>Sends one packet to a target: a client's packet for anyone but the server travels inside a relay envelope the server unwraps.</summary>
    private Error SendRaw(int target, byte[] packet, MultiplayerPeer.TransferModeEnum mode, int channel)
    {
        MultiplayerPeer p = _peer;
        if (p.GetConnectionStatus() != MultiplayerPeer.ConnectionStatus.Connected) return Error.Unconfigured;
        int self = p.GetUniqueId();
        if (target == self) return Error.Ok;
        if (!p.IsServer() && target != MultiplayerPeer.TargetPeerServer)
        {
            if (!ServerRelay)
            {
                if (target > 0 || target == -MultiplayerPeer.TargetPeerServer)
                {
                    Aver.Scripting.Log.Error($"[Fabric] SceneMultiplayer: cannot reach peer {target}: server_relay is off, a client can talk to the server only");
                    return target > 0 ? Error.Unavailable : Error.Ok;
                }
                target = MultiplayerPeer.TargetPeerServer;   // a broadcast is the server alone
            }
            else
            {
                packet = new NetWriter().U8(CmdRelay).I32(target).Bytes(packet).ToArray();
                target = MultiplayerPeer.TargetPeerServer;
            }
        }
        p.TransferMode = mode;
        p.TransferChannel = channel;
        p.SetTargetPeer(target);
        return p.PutPacket(packet);
    }

    // ---- node references (the path cache) ----

    private Node? RootNode()
    {
        Window root = SceneTree.Instance.Root;
        return RootPath.IsEmpty ? root : root.GetNodeOrNull(RootPath) ?? (RootPath.ToString() == "/root" ? root : null);
    }

    /// <summary>The node's path relative to this API's root node, or null when it is not below it.</summary>
    internal string? RelativePath(Node n)
    {
        Node? root = RootNode();
        if (root == null || (n != root && !root.IsAncestorOf(n))) return null;
        return root.GetPathTo(n).ToString();
    }

    private IEnumerable<int> Audience(int target)
    {
        if (target > 0) return new[] { target };
        if (target == 0) return _peers;
        return _peers.Where(p => p != -target);
    }

    private bool AllConfirmed(int target, uint id)
    {
        foreach (int p in Audience(target))
            if (!_confirmed.TryGetValue(p, out HashSet<uint>? set) || !set.Contains(id)) return false;
        return true;
    }

    /// <summary>Starts a packet addressed to a node: the command, the node's id and, until every receiver confirmed the id, its path. Null when the node is outside the branch.</summary>
    internal NetWriter? BeginNodePacket(int cmd, Node node, int target)
    {
        string? rel = RelativePath(node);
        if (rel == null)
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: the node '{node.Name}' is not inside the branch of this MultiplayerAPI ({(RootPath.IsEmpty ? "/root" : RootPath.ToString())})");
            return null;
        }
        if (!_nodeIds.TryGetValue(node, out uint id)) _nodeIds[node] = id = _nextNodeId++;
        bool full = !AllConfirmed(target, id);
        if (full) FullRefsSent++; else CachedRefsSent++;
        var w = new NetWriter().U8(cmd | (full ? PathFlag : 0)).U32(id);
        if (full) w.Str(rel);
        return w;
    }

    /// <summary>Reads the node a packet is addressed to. A path is cached and confirmed to the sender; null (and an error unless <paramref name="quiet"/>) when the node is not there.</summary>
    private Node? ReadNodeRef(int from, int cmdByte, NetReader r, bool quiet)
    {
        uint id = r.U32();
        string? path;
        bool full = (cmdByte & PathFlag) != 0;
        if (full)
        {
            path = r.Str();
            if (!_recvPaths.TryGetValue(from, out Dictionary<uint, string>? cache)) _recvPaths[from] = cache = new Dictionary<uint, string>();
            if (cache.Count > 65536) cache.Clear();
            cache[id] = path;
        }
        else if (!(_recvPaths.TryGetValue(from, out Dictionary<uint, string>? c) && c.TryGetValue(id, out path)))
        {
            if (!quiet) Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} addressed a node by an id ({id}) this peer has no path for");
            return null;
        }
        Node? root = RootNode();
        Node? node = root?.GetNodeOrNull(path);
        if (node == null || root == null || (node != root && !root.IsAncestorOf(node)))
        {
            if (!quiet) Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} addressed the node '{path}', which does not exist here");
            return null;
        }
        if (full) SendRaw(from, new NetWriter().U8(CmdConfirm).U32(id).ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0);
        return node;
    }

    // ---- receiving ----

    private void Dispatch(int from, byte[] data, MultiplayerPeer.TransferModeEnum mode, int channel, bool relayed)
    {
        if (data.Length == 0) return;
        int cmdByte = data[0];
        int cmd = cmdByte & ~PathFlag;
        try
        {
            var r = new NetReader(data, 1);
            if (cmd == CmdAuth || cmd == CmdAuthDone)
            {
                if (relayed || !_pending.TryGetValue(from, out Pending? p)) return;
                if (cmd == CmdAuth)
                {
                    if (AuthCallback.IsValid()) AuthCallback.Call((long)from, (Variant)r.Rest());
                }
                else
                {
                    p.Remote = true;
                    if (p.Local) Admit(from);
                }
                return;
            }
            if (!_peers.Contains(from)) return;   // nothing but authentication from a peer that is not admitted
            switch (cmd)
            {
                case CmdRelay: if (!relayed) HandleRelay(from, r, mode, channel); break;
                case CmdRelayed:
                    if (!relayed && !_peer.IsServer() && from == MultiplayerPeer.TargetPeerServer)
                    {
                        int origin = r.I32();
                        Dispatch(origin, r.Rest(), mode, channel, relayed: true);
                    }
                    break;
                case CmdRoster: if (!relayed) HandleRoster(from, r); break;
                case CmdConfirm:
                {
                    uint id = r.U32();
                    if (!_confirmed.TryGetValue(from, out HashSet<uint>? set)) _confirmed[from] = set = new HashSet<uint>();
                    set.Add(id);
                    break;
                }
                case CmdRaw: EmitSignal(SignalName.PeerPacket, (long)from, (Variant)r.Rest()); break;
                case CmdRpc: HandleRpc(from, cmdByte, r); break;
                case CmdSpawn: HandleSpawn(from, cmdByte, r); break;
                case CmdDespawn: HandleDespawn(from, cmdByte, r); break;
                case CmdSync: HandleSync(from, cmdByte, r); break;
                case CmdDelta: HandleDelta(from, cmdByte, r); break;
            }
        }
        catch (FormatException ex)
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: a malformed packet from peer {from} was dropped ({ex.Message})");
        }
    }

    // A client's packet for other peers: the server delivers it here if it is addressed to the server too, and forwards it to the clients named.
    private void HandleRelay(int from, NetReader r, MultiplayerPeer.TransferModeEnum mode, int channel)
    {
        if (!_peer.IsServer()) return;
        if (!ServerRelay)
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} asked the server to relay a packet and server_relay is off");
            return;
        }
        int target = r.I32();
        byte[] inner = r.Rest();
        bool forMe;
        IEnumerable<int> forward;
        if (target == 0) { forMe = true; forward = _peers.Where(p => p != from); }
        else if (target > 0) { forMe = target == MultiplayerPeer.TargetPeerServer; forward = _peers.Contains(target) && target != from ? new[] { target } : System.Array.Empty<int>(); }
        else { forMe = -target != MultiplayerPeer.TargetPeerServer; forward = _peers.Where(p => p != from && p != -target); }
        byte[] wrapped = new NetWriter().U8(CmdRelayed).I32(from).Bytes(inner).ToArray();
        foreach (int f in forward.ToArray()) SendRaw(f, wrapped, mode, channel);
        if (forMe) Dispatch(from, inner, mode, channel, relayed: true);
    }

    // ---- RPC ----

    public override Error Rpc(int peer, GodotObject @object, StringName method, GArray? args = null)
    {
        if (@object is not Node node)
        {
            Aver.Scripting.Log.Error("[Fabric] MultiplayerAPI.rpc: only a node can be the target of an RPC");
            return Error.InvalidParameter;
        }
        return SendRpc(node, peer, method.ToString(), args?.ToArray() ?? System.Array.Empty<Variant>());
    }

    /// <summary>Godot's rpcp: runs the method here when the target includes this peer and the method is call_local, and sends it to the others.</summary>
    internal Error SendRpc(Node node, int target, string method, Variant[] args)
    {
        RpcEntry? cfg = node.FindRpc(method);
        if (cfg == null)
        {
            Aver.Scripting.Log.Error($"[Fabric] rpc: unable to get the RPC configuration for '{method}' on '{node.Name}'; mark the method with [Rpc]");
            return Error.InvalidParameter;
        }
        MultiplayerPeer.ConnectionStatus status = _peer.GetConnectionStatus();
        if (status == MultiplayerPeer.ConnectionStatus.Connecting)
        {
            Aver.Scripting.Log.Error("[Fabric] rpc: attempt to call an RPC while the multiplayer peer is not connected yet");
            return Error.Unconfigured;
        }
        if (status == MultiplayerPeer.ConnectionStatus.Disconnected)
        {
            Aver.Scripting.Log.Error("[Fabric] rpc: attempt to call an RPC while the multiplayer peer is disconnected");
            return Error.Unconfigured;
        }
        int self = _peer.GetUniqueId();
        bool includesSelf = target == 0 || target == self || (target < 0 && -target != self);
        if (includesSelf && cfg.CallLocal) CallLocal(node, cfg, args);
        if (target == self) return Error.Ok;
        if (target > 0 && !_peers.Contains(target))
        {
            Aver.Scripting.Log.Error($"[Fabric] rpc '{method}': peer {target} is not connected");
            return Error.InvalidParameter;
        }
        NetWriter? w = BeginNodePacket(CmdRpc, node, target);
        if (w == null) return Error.InvalidParameter;
        w.Str(cfg.Name).U16(args.Length);
        foreach (Variant a in args)
            if (w.Var(a) != Error.Ok)
            {
                Aver.Scripting.Log.Error($"[Fabric] rpc '{method}': an argument of type {a.VariantType} cannot be sent");
                return Error.InvalidParameter;
            }
        return SendRaw(target, w.ToArray(), cfg.Transfer, cfg.Channel);
    }

    private static void CallLocal(Node node, RpcEntry cfg, Variant[] args)
    {
        try { node.Call(cfg.Name, args); }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] rpc '{cfg.Name}' on '{node.Name}' threw in the local call: {ex.GetType().Name}: {ex.Message}"); }
    }

    private void HandleRpc(int from, int cmdByte, NetReader r)
    {
        Node? node = ReadNodeRef(from, cmdByte, r, quiet: false);
        string method = r.Str();
        int argc = r.U16();
        if (argc > r.Remaining) throw new FormatException("argument count");
        var args = new Variant[argc];
        for (int i = 0; i < argc; i++) args[i] = r.Var(AllowObjectDecoding);
        if (node == null) return;
        RpcEntry? cfg = node.FindRpc(method);
        if (cfg == null)
        {
            Aver.Scripting.Log.Error($"[Fabric] rpc: peer {from} called '{method}' on '{node.GetPath()}', which is not an RPC method of that node");
            return;
        }
        bool allowed = cfg.Mode == MultiplayerApi.RpcMode.AnyPeer || (cfg.Mode == MultiplayerApi.RpcMode.Authority && from == node.GetMultiplayerAuthority());
        if (!allowed)
        {
            Aver.Scripting.Log.Error($"[Fabric] rpc: '{cfg.Name}' is not allowed on node '{node.GetPath()}' from peer {from}. Mode is {ModeName(cfg.Mode)}, authority is {node.GetMultiplayerAuthority()}");
            return;
        }
        int previous = _remoteSender;
        _remoteSender = from;
        try { node.Call(cfg.Name, args); }
        catch (Exception ex) { Aver.Scripting.Log.Error($"[Fabric] rpc '{cfg.Name}' from peer {from} on '{node.GetPath()}' failed: {ex.GetType().Name}: {ex.Message}"); }
        finally { _remoteSender = previous; }
    }
}
