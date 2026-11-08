// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// NetSession: one end of a game connection. A server accepts clients, owns the NetObjects and replicates them (spawn, destroy, changed
// properties, RPCs); a client receives them. Single game thread: call Tick once per frame.

using System.Reflection;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework.Net;

public enum NetRole { None, Server, Client }
public enum NetSessionState { Idle, Connecting, Handshaking, Connected, Disconnected }

/// <summary>A client as the server sees it.</summary>
public sealed class NetConnection
{
    /// <summary>1 and up, assigned when the handshake completes; never reused within a session.</summary>
    public uint Id { get; internal set; }
    /// <summary>What the client sent in its hello (a name, a token).</summary>
    public string UserData { get; internal set; } = "";
    /// <summary>Where the client's player is looking from; the game sets it for distance relevance.</summary>
    public Vec3 ViewPosition;
    /// <summary>Anything the game wants to hang on the connection.</summary>
    public object? Tag;
    public bool Authenticated { get; internal set; }
    internal long Peer;
    internal readonly Dictionary<uint, NetSyncState> Spawned = new();
}

/// <summary>An RPC the receiver refused: the sender does not own the object, the method is unknown, the arguments do not decode.</summary>
public readonly record struct NetRpcRejection(NetObject? Object, string Method, NetConnection? From, string Reason);

public sealed class NetStats
{
    public long PacketsSent, PacketsReceived, SpawnsSent, DestroysSent, PropMessagesSent, UnreliablePropMessagesSent, RpcsSent, RpcsExecuted, RpcsRejected, Malformed;
}

public sealed class NetSession : IDisposable
{
    /// <summary>Bumped whenever the wire format changes; a client with another version is refused at the handshake.</summary>
    public const uint ProtocolVersion = 1;

    private enum Op : byte { Hello = 1, Welcome = 2, Reject = 3, Spawn = 4, Destroy = 5, Props = 6, Rpc = 7, Owner = 8 }

    private readonly INetTransport _transport;
    private readonly Dictionary<uint, NetObject> _objects = new();
    private readonly Dictionary<long, NetConnection> _byPeer = new();
    private readonly Dictionary<uint, NetConnection> _byId = new();
    private uint _nextNetId = 1, _nextConnId = 1;
    private long _serverPeer, _lastReplicateMs = long.MinValue;
    private string _helloData = "";
    private string _disconnectReason = "";

    public NetRole Role { get; private set; }
    public NetSessionState State { get; private set; }
    public bool IsServer => Role == NetRole.Server;
    public bool IsClient => Role == NetRole.Client;
    /// <summary>0 on the server; on a client the id the server assigned (0 until connected).</summary>
    public uint LocalConnectionId { get; private set; }
    public NetStats Stats { get; } = new();

    /// <summary>Minimum time between replication passes; 0 replicates on every Tick.</summary>
    public int SendIntervalMs { get; set; }
    /// <summary>How often unreliable properties are resent in full, so a lost packet only delays convergence.</summary>
    public int UnreliableResyncMs { get; set; } = 500;
    /// <summary>The objects a client owns are destroyed when it leaves (default); when false they stay, owned by nobody.</summary>
    public bool DestroyOwnedOnDisconnect { get; set; } = true;
    /// <summary>Server: returns null to accept a connecting client, or the reason to refuse it.</summary>
    public Func<NetConnection, string?>? ConnectionApproval { get; set; }
    /// <summary>Server: whether an object is replicated to a connection. Null means everything everywhere. See <see cref="NetRelevance"/>.</summary>
    public Func<NetObject, NetConnection, bool>? RelevanceFilter { get; set; }

    /// <summary>Server: a client finished the handshake.</summary>
    public event Action<NetConnection>? ClientConnected;
    /// <summary>Server: an authenticated client left (its owned objects are already handled).</summary>
    public event Action<NetConnection>? ClientDisconnected;
    /// <summary>Client: the handshake finished and <see cref="LocalConnectionId"/> is set.</summary>
    public event Action? Connected;
    /// <summary>Client: the connection ended or never completed; the argument says why.</summary>
    public event Action<string>? Disconnected;
    /// <summary>Client: a replicated object arrived, initial state applied.</summary>
    public event Action<NetObject>? ObjectSpawned;
    /// <summary>Client: a replicated object went away (destroyed, out of relevance, or the connection ended).</summary>
    public event Action<NetObject>? ObjectDestroyed;
    public event Action<NetRpcRejection>? RpcRejected;

    public NetSession(INetTransport transport) => _transport = transport ?? throw new ArgumentNullException(nameof(transport));

    public IEnumerable<NetObject> Objects => _objects.Values;
    public IReadOnlyCollection<NetConnection> Connections => _byId.Values;
    public NetObject? Find(uint netId) => _objects.GetValueOrDefault(netId);
    public NetConnection? FindConnection(uint id) => _byId.GetValueOrDefault(id);

    // ---- lifecycle ----------------------------------------------------------------------------

    public bool StartServer(string ip, int port, int maxClients, out int boundPort)
    {
        boundPort = 0;
        if (Role != NetRole.None) return false;
        if (!_transport.Listen(ip, port, maxClients, out boundPort)) return false;
        Role = NetRole.Server;
        State = NetSessionState.Connected;
        return true;
    }

    public bool StartClient(string ip, int port, string helloData = "")
    {
        if (Role != NetRole.None) return false;
        _serverPeer = _transport.Connect(ip, port);
        if (_serverPeer == 0) return false;
        Role = NetRole.Client;
        State = NetSessionState.Connecting;
        _helloData = helloData;
        return true;
    }

    /// <summary>Server: sends the client away (it is told why when <paramref name="reason"/> is given).</summary>
    public void Kick(NetConnection conn, string? reason = null)
    {
        if (reason is not null) { var w = new NetWriter(); w.U8((byte)Op.Reject); w.Str(reason); Send(conn.Peer, true, w); }
        _transport.Disconnect(conn.Peer);
    }

    /// <summary>Ends the session: a client leaves, a server drops every client. Objects are removed (OnNetDestroy runs).</summary>
    public void Stop()
    {
        if (Role == NetRole.None) return;
        if (IsClient && _serverPeer != 0) _transport.Disconnect(_serverPeer);
        if (IsServer) foreach (NetConnection c in _byId.Values.ToArray()) _transport.Disconnect(c.Peer);
        Pump();
        foreach (NetObject o in _objects.Values.ToArray()) RemoveLocal(o);
        _byId.Clear();
        _byPeer.Clear();
        Role = NetRole.None;
        State = NetSessionState.Idle;
        LocalConnectionId = 0;
    }

    public void Dispose()
    {
        Stop();
        _transport.Dispose();
    }

    /// <summary>Moves the network, handles what arrived, and (on a server, at <see cref="SendIntervalMs"/>) replicates.</summary>
    public void Tick(long nowMs = -1)
    {
        if (nowMs < 0) nowMs = Environment.TickCount64;
        Pump();
        if (IsServer && (SendIntervalMs <= 0 || _lastReplicateMs == long.MinValue || nowMs - _lastReplicateMs >= SendIntervalMs))
        {
            _lastReplicateMs = nowMs;
            Replicate(nowMs);
        }
    }

    // ---- server: spawn, destroy, ownership ----------------------------------------------------

    /// <summary>Server: makes an object of a registered type and replicates it to the connections it is relevant to.</summary>
    public NetObject Spawn(string typeName, uint ownerId = 0)
    {
        if (!IsServer) throw new InvalidOperationException("only the server spawns replicated objects");
        NetObject o = NetTypes.Create(typeName) ?? throw new ArgumentException("no NetObject type registered as " + typeName);
        Adopt(o, _nextNetId++, ownerId);
        Scan(o);
        o.NotifySpawn();
        return o;
    }

    public T Spawn<T>(uint ownerId = 0) where T : NetObject =>
        (T)Spawn(NetTypes.NameOf(typeof(T)) ?? throw new ArgumentException(typeof(T).Name + " is not registered with NetTypes"), ownerId);

    /// <summary>Server: removes the object here and on every client that has it.</summary>
    public void Destroy(NetObject o)
    {
        if (!IsServer) throw new InvalidOperationException("only the server destroys replicated objects");
        if (!_objects.ContainsKey(o.NetId)) return;
        var w = new NetWriter(); w.U8((byte)Op.Destroy); w.U32(o.NetId);
        foreach (NetConnection c in _byId.Values)
            if (c.Spawned.Remove(o.NetId)) { Send(c.Peer, true, w); Stats.DestroysSent++; }
        RemoveLocal(o);
    }

    /// <summary>Server: hands the object to another connection (0 = the server).</summary>
    public void SetOwner(NetObject o, uint ownerId)
    {
        if (!IsServer) throw new InvalidOperationException("only the server changes ownership");
        if (o.OwnerId == ownerId) return;
        uint previous = o.OwnerId;
        o.OwnerId = ownerId;
        var w = new NetWriter(); w.U8((byte)Op.Owner); w.U32(o.NetId); w.U32(ownerId);
        foreach (NetConnection c in _byId.Values)
        {
            if (!c.Spawned.TryGetValue(o.NetId, out NetSyncState? s)) continue;
            Send(c.Peer, true, w);
            // The new owner has not been sent the owner-only state yet.
            if (c.Id == ownerId)
                for (int i = 0; i < o.Info.Props.Length; i++) if (o.Info.Props[i].OwnerOnly) s.SentVersion[i] = 0;
        }
        o.NotifyOwner(previous);
    }

    private void Adopt(NetObject o, uint netId, uint owner)
    {
        o.Info = NetTypes.Info(o.GetType());
        o.Current = new byte[o.Info.Props.Length][];
        o.Version = new uint[o.Info.Props.Length];
        o.NetId = netId;
        o.OwnerId = owner;
        o.Session = this;
        _objects[netId] = o;
    }

    private void RemoveLocal(NetObject o)
    {
        _objects.Remove(o.NetId);
        try { o.NotifyDestroy(); } catch (Exception ex) { Log.Error($"[Net] OnNetDestroy threw: {ex.Message}"); }
        if (IsClient) ObjectDestroyed?.Invoke(o);
        o.Session = null;
    }

    // ---- server: replication ------------------------------------------------------------------

    // Serializes every replicated member and bumps its version when the bytes differ from the last pass.
    private static void Scan(NetObject o)
    {
        NetPropInfo[] props = o.Info.Props;
        for (int i = 0; i < props.Length; i++)
        {
            var w = new NetWriter();
            NetCodec.Write(w, props[i].Type, props[i].Get(o));
            byte[] now = w.ToArray();
            if (o.Current[i] is null || !now.AsSpan().SequenceEqual(o.Current[i])) { o.Current[i] = now; o.Version[i]++; }
        }
    }

    private bool IsRelevant(NetObject o, NetConnection c) =>
        RelevanceFilter is null || o.OwnerId == c.Id || RelevanceFilter(o, c);

    private void Replicate(long nowMs)
    {
        foreach (NetObject o in _objects.Values) Scan(o);
        foreach (NetConnection c in _byId.Values)
        {
            if (!c.Authenticated) continue;
            foreach (NetObject o in _objects.Values.OrderBy(x => x.NetId).ToArray())
            {
                bool relevant = IsRelevant(o, c);
                bool known = c.Spawned.TryGetValue(o.NetId, out NetSyncState? sync);
                if (!relevant)
                {
                    if (!known) continue;
                    var w = new NetWriter(); w.U8((byte)Op.Destroy); w.U32(o.NetId);
                    Send(c.Peer, true, w);
                    Stats.DestroysSent++;
                    c.Spawned.Remove(o.NetId);
                }
                else if (!known) SendSpawn(c, o, nowMs);
                else SendChanges(c, o, sync!, nowMs);
            }
        }
    }

    private bool Visible(NetPropInfo p, NetObject o, NetConnection c) => !p.OwnerOnly || o.OwnerId == c.Id;

    private void SendSpawn(NetConnection c, NetObject o, long nowMs)
    {
        var w = new NetWriter();
        w.U8((byte)Op.Spawn); w.U32(o.NetId); w.Str(o.Info.Name); w.U32(o.OwnerId);
        var sync = new NetSyncState { SentVersion = new uint[o.Info.Props.Length], LastUnreliableMs = nowMs };
        var idx = new List<int>();
        for (int i = 0; i < o.Info.Props.Length; i++) if (Visible(o.Info.Props[i], o, c)) idx.Add(i);
        WriteProps(w, o, idx);
        foreach (int i in idx) sync.SentVersion[i] = o.Version[i];
        c.Spawned[o.NetId] = sync;
        Send(c.Peer, true, w);
        Stats.SpawnsSent++;
    }

    private void SendChanges(NetConnection c, NetObject o, NetSyncState sync, long nowMs)
    {
        bool resync = o.Info.HasUnreliable && nowMs - sync.LastUnreliableMs >= UnreliableResyncMs;
        var reliable = new List<int>();
        var unreliable = new List<int>();
        for (int i = 0; i < o.Info.Props.Length; i++)
        {
            NetPropInfo p = o.Info.Props[i];
            if (!Visible(p, o, c)) continue;
            bool changed = sync.SentVersion[i] != o.Version[i];
            if (p.Reliable) { if (changed) reliable.Add(i); }
            else if (changed || resync) unreliable.Add(i);
        }
        if (resync) sync.LastUnreliableMs = nowMs;
        SendProps(c, o, sync, reliable, true);
        SendProps(c, o, sync, unreliable, false);
    }

    private void SendProps(NetConnection c, NetObject o, NetSyncState sync, List<int> idx, bool reliable)
    {
        if (idx.Count == 0) return;
        var w = new NetWriter();
        w.U8((byte)Op.Props); w.U32(o.NetId);
        WriteProps(w, o, idx);
        foreach (int i in idx) sync.SentVersion[i] = o.Version[i];
        Send(c.Peer, reliable, w);
        if (reliable) Stats.PropMessagesSent++; else Stats.UnreliablePropMessagesSent++;
    }

    private static void WriteProps(NetWriter w, NetObject o, List<int> idx)
    {
        w.Var((uint)idx.Count);
        foreach (int i in idx) { w.Var((uint)i); w.Bytes(o.Current[i]); }
    }

    // ---- RPC ----------------------------------------------------------------------------------

    internal void SendRpc(NetObject o, string method, object?[] args)
    {
        if (!o.Info.Rpcs.TryGetValue(method, out NetRpcInfo? rpc)) throw new ArgumentException($"{o.Info.Type.Name} has no [Rpc] method {method}");
        if (args.Length != rpc.ParamTypes.Length) throw new ArgumentException($"{method} takes {rpc.ParamTypes.Length} arguments");
        var w = new NetWriter();
        w.U8((byte)Op.Rpc); w.U32(o.NetId); w.Str(method);
        for (int i = 0; i < args.Length; i++) NetCodec.Write(w, rpc.ParamTypes[i], Coerce(args[i], rpc.ParamTypes[i]));

        switch (rpc.Target)
        {
            case RpcTarget.Server:
                if (IsServer) Execute(o, rpc, args);
                else if (State == NetSessionState.Connected) { Send(_serverPeer, rpc.Reliable, w); Stats.RpcsSent++; }
                break;
            case RpcTarget.Client:
                if (!IsServer) { Log.Warn($"[Net] {method} is a Client RPC: only the server may call it"); break; }
                if (o.OwnerId == 0) Execute(o, rpc, args);
                else if (_byId.TryGetValue(o.OwnerId, out NetConnection? owner) && owner.Spawned.ContainsKey(o.NetId)) { Send(owner.Peer, rpc.Reliable, w); Stats.RpcsSent++; }
                break;
            case RpcTarget.Multicast:
                if (!IsServer) { Log.Warn($"[Net] {method} is a Multicast RPC: only the server may call it"); break; }
                Execute(o, rpc, args);
                foreach (NetConnection c in _byId.Values)
                    if (c.Spawned.ContainsKey(o.NetId)) { Send(c.Peer, rpc.Reliable, w); Stats.RpcsSent++; }
                break;
        }
    }

    private static object? Coerce(object? v, Type t) =>
        v is null || v.GetType() == t || t.IsEnum || t.IsArray || t == typeof(Vec3) || t == typeof(Quat) || t == typeof(string) ? v : Convert.ChangeType(v, t);

    private void Execute(NetObject o, NetRpcInfo rpc, object?[] args)
    {
        try { rpc.Method.Invoke(o, args); Stats.RpcsExecuted++; }
        catch (TargetInvocationException ex) { Log.Error($"[Net] RPC {rpc.Name} threw: {ex.InnerException?.Message}"); }
    }

    private void Reject(NetObject? o, string method, NetConnection? from, string reason)
    {
        Stats.RpcsRejected++;
        RpcRejected?.Invoke(new NetRpcRejection(o, method, from, reason));
    }

    private void OnRpc(NetReader r, NetConnection? from)
    {
        uint id = r.U32();
        string method = r.Str();
        NetObject? o = Find(id);
        if (o is null) { if (IsServer) Reject(null, method, from, "unknown object"); return; }   // a client may race a destroy: drop quietly
        if (!o.Info.Rpcs.TryGetValue(method, out NetRpcInfo? rpc)) { Reject(o, method, from, "not an RPC method"); return; }
        if (IsServer)
        {
            if (rpc.Target != RpcTarget.Server) { Reject(o, method, from, "not a Server RPC"); return; }
            if (from is null || o.OwnerId != from.Id) { Reject(o, method, from, "sender is not the owner"); return; }
        }
        else if (rpc.Target == RpcTarget.Server) { Reject(o, method, null, "a Server RPC arrived at a client"); return; }
        var args = new object?[rpc.ParamTypes.Length];
        for (int i = 0; i < args.Length; i++) args[i] = NetCodec.Read(r, rpc.ParamTypes[i]);
        Execute(o, rpc, args);
    }

    // ---- receiving ----------------------------------------------------------------------------

    private void Send(long peer, bool reliable, NetWriter w)
    {
        if (_transport.Send(peer, reliable, w.ToArray())) Stats.PacketsSent++;
    }

    // Drains the transport.
    private void Pump()
    {
        while (_transport.Poll(out NetTransportEvent ev))
        {
            switch (ev.Type)
            {
                case NetTransportEventType.Connect: OnTransportConnect(ev.Peer); break;
                case NetTransportEventType.Disconnect: OnTransportDisconnect(ev.Peer); break;
                case NetTransportEventType.Data:
                    Stats.PacketsReceived++;
                    try { OnData(ev.Peer, ev.Data ?? Array.Empty<byte>()); }
                    catch (NetFormatException ex) { Stats.Malformed++; Log.Warn("[Net] malformed packet: " + ex.Message); }
                    break;
            }
        }
    }

    private void OnTransportConnect(long peer)
    {
        if (IsServer) { _byPeer[peer] = new NetConnection { Peer = peer }; return; }
        if (IsClient && peer == _serverPeer)
        {
            State = NetSessionState.Handshaking;
            var w = new NetWriter(); w.U8((byte)Op.Hello); w.U32(ProtocolVersion); w.Str(_helloData);
            Send(peer, true, w);
        }
    }

    private void OnTransportDisconnect(long peer)
    {
        if (IsServer)
        {
            if (!_byPeer.Remove(peer, out NetConnection? c)) return;
            if (!c.Authenticated) return;
            _byId.Remove(c.Id);
            foreach (NetObject o in _objects.Values.Where(x => x.OwnerId == c.Id).ToArray())
            {
                if (DestroyOwnedOnDisconnect) Destroy(o);
                else SetOwner(o, 0);
            }
            ClientDisconnected?.Invoke(c);
        }
        else if (IsClient && peer == _serverPeer)
        {
            bool wasUp = State == NetSessionState.Connected;
            foreach (NetObject o in _objects.Values.ToArray()) RemoveLocal(o);
            State = NetSessionState.Disconnected;
            LocalConnectionId = 0;
            _serverPeer = 0;
            string why = _disconnectReason.Length > 0 ? _disconnectReason : wasUp ? "connection closed" : "connect failed";
            Disconnected?.Invoke(why);
        }
    }

    private void OnData(long peer, byte[] data)
    {
        var r = new NetReader(data);
        var op = (Op)r.U8();
        if (IsServer)
        {
            if (!_byPeer.TryGetValue(peer, out NetConnection? c)) return;
            if (op == Op.Hello) { OnHello(c, r); return; }
            if (!c.Authenticated) return;   // nothing but a hello is taken from a stranger
            if (op == Op.Rpc) OnRpc(r, c);
            return;
        }
        if (!IsClient || peer != _serverPeer) return;
        switch (op)
        {
            case Op.Welcome: LocalConnectionId = r.U32(); State = NetSessionState.Connected; Connected?.Invoke(); break;
            case Op.Reject: _disconnectReason = "rejected: " + r.Str(); break;
            case Op.Spawn: OnSpawn(r); break;
            case Op.Destroy: { NetObject? o = Find(r.U32()); if (o is not null) RemoveLocal(o); break; }
            case Op.Props: { NetObject? o = Find(r.U32()); if (o is not null) ApplyProps(o, r, false); break; }
            case Op.Owner:
            {
                NetObject? o = Find(r.U32());
                uint owner = r.U32();
                if (o is not null) { uint prev = o.OwnerId; o.OwnerId = owner; o.NotifyOwner(prev); }
                break;
            }
            case Op.Rpc: if (State == NetSessionState.Connected) OnRpc(r, null); break;
        }
    }

    private void OnHello(NetConnection c, NetReader r)
    {
        if (c.Authenticated) return;
        uint version = r.U32();
        c.UserData = r.Str();
        string? refusal = version != ProtocolVersion ? $"protocol version {version}, server speaks {ProtocolVersion}" : ConnectionApproval?.Invoke(c);
        if (refusal is not null) { Kick(c, refusal); return; }
        c.Id = _nextConnId++;
        c.Authenticated = true;
        _byId[c.Id] = c;
        var w = new NetWriter(); w.U8((byte)Op.Welcome); w.U32(c.Id);
        Send(c.Peer, true, w);
        ClientConnected?.Invoke(c);
    }

    private void OnSpawn(NetReader r)
    {
        uint id = r.U32();
        string type = r.Str();
        uint owner = r.U32();
        if (_objects.ContainsKey(id)) { Stats.Malformed++; return; }
        NetObject? o = NetTypes.Create(type);
        if (o is null) { Stats.Malformed++; Log.Warn("[Net] spawn of unregistered type " + type); return; }
        Adopt(o, id, owner);
        ApplyProps(o, r, true);
        o.NotifySpawn();
        ObjectSpawned?.Invoke(o);
    }

    private void ApplyProps(NetObject o, NetReader r, bool initial)
    {
        uint n = r.Var();
        for (uint k = 0; k < n; k++)
        {
            int i = (int)r.Var();
            byte[] raw = r.Bytes();
            if (i >= o.Info.Props.Length) throw new NetFormatException("property index out of range");
            NetPropInfo p = o.Info.Props[i];
            object? old = p.Get(o);
            object? value = NetCodec.Read(new NetReader(raw), p.Type);
            bool same = o.Current[i] is { } cur && cur.AsSpan().SequenceEqual(raw);
            p.Set(o, value);
            o.Current[i] = raw;
            if (p.OnChanged is null || (same && !initial)) continue;
            try { p.OnChanged.Invoke(o, p.OnChanged.GetParameters().Length == 0 ? null : new[] { old }); }
            catch (TargetInvocationException ex) { Log.Error($"[Net] {p.Name} OnChanged threw: {ex.InnerException?.Message}"); }
        }
    }
}
