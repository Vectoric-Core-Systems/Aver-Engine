// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2 test support. Aver.Fabric has one SceneTree per process (groups, GUI and input are process-wide), so the "peers" of these tests are branches of
// that one tree, each with its own SceneMultiplayer (SceneTree.SetMultiplayer with a root path) and its own transport: exactly the per-branch multiplayer
// Godot offers for running several peers in one process. Three transports: real ENetMultiplayerPeers over 127.0.0.1, the same over a seeded lossy UDP
// proxy (the protocol's SimNetwork is C++ only), and an in-memory MultiplayerPeer that records every send (mode, channel, target).

using Aver.Fabric;

/// <summary>A node with one RPC method of every kind, logging each call with the sender id it saw.</summary>
class NetNode : Node
{
    public readonly List<string> Log = new();
    public int Sender => Multiplayer?.GetRemoteSenderId() ?? -1;

    [Rpc] public void AuthCall(int x) => Log.Add($"AuthCall {x} <{Sender}>");
    [Rpc(MultiplayerApi.RpcMode.AnyPeer)] public void Any(string s) => Log.Add($"Any {s} <{Sender}>");
    [Rpc(MultiplayerApi.RpcMode.AnyPeer, CallLocal = true)] public void Both(int x) => Log.Add($"Both {x} <{Sender}>");
    [Rpc(MultiplayerApi.RpcMode.AnyPeer, TransferMode = MultiplayerPeer.TransferModeEnum.Unreliable)] public void Fast(int x) => Log.Add($"Fast {x}");
    [Rpc(MultiplayerApi.RpcMode.AnyPeer, TransferMode = MultiplayerPeer.TransferModeEnum.UnreliableOrdered, TransferChannel = 1)] public void Ordered(int x) => Log.Add($"Ordered {x}");
    [Rpc(MultiplayerApi.RpcMode.Disabled)] public void Off() => Log.Add("Off");
    [Rpc(MultiplayerApi.RpcMode.AnyPeer)] public void Throws() => throw new InvalidOperationException("boom");
    [Rpc(MultiplayerApi.RpcMode.AnyPeer)]
    public void Args(int a, string b, Vector3 c, Aver.Fabric.Collections.Array d, Aver.Fabric.Collections.Dictionary e, byte[] f) =>
        Log.Add($"Args {a} {b} {c} {d.Count} {e.Count} {f.Length}");
    public void NotRpc() => Log.Add("NotRpc");

    [Export] public int Score;
    [Export] public Vector3 Pos;
    [Export] public string Tag = "";
    [Export] public int Hp = 100;
}

/// <summary>One simulated process: a branch of the tree with its own API and transport.</summary>
sealed class NetPeer
{
    public string Name = "";
    public Node Root = null!;
    public SceneMultiplayer Mp = null!;
    public MultiplayerPeer Net = null!;
    public readonly List<string> Events = new();
    public int Id => Mp.GetUniqueId();
    public TNode Find<TNode>(string path) where TNode : Node => Root.GetNode<TNode>(path);
    public NetNode Game => Root.GetNode<NetNode>("Game");

    public void Record()
    {
        Mp.PeerConnected += id => Events.Add("connected " + id);
        Mp.PeerDisconnected += id => Events.Add("disconnected " + id);
        Mp.ConnectedToServer += () => Events.Add("connected_to_server");
        Mp.ConnectionFailed += () => Events.Add("connection_failed");
        Mp.ServerDisconnected += () => Events.Add("server_disconnected");
    }
}

/// <summary>A server and its clients, each in its own branch of the tree.</summary>
sealed class NetCluster
{
    public readonly List<NetPeer> Peers = new();
    public LossyProxy? Proxy;
    public NetPeer Server => Peers[0];
    public NetPeer C(int i) => Peers[i];

    public void Step(double dt = 1.0 / 60.0)
    {
        Proxy?.Pump();
        T.Frame(dt);
    }

    public bool Until(Func<bool> done, int ms = 8000)
    {
        long end = System.Environment.TickCount64 + ms;
        while (System.Environment.TickCount64 < end)
        {
            Step();
            if (done()) return true;
            Thread.Sleep(1);
        }
        Step();
        return done();
    }

    /// <summary>Runs frames for a while (real time), for things that must NOT happen.</summary>
    public void Settle(int ms = 150) => Until(() => false, ms);

    public bool FullyConnected(int timeoutMs = 10000) =>
        Until(() => Peers.All(p => p.Mp.GetPeers().Length == Peers.Count - 1), timeoutMs);

    public void Close()
    {
        foreach (NetPeer p in Peers) { try { p.Net.Close(); } catch (Exception) { } }
        Proxy?.Close();
    }

    /// <summary>A server and <paramref name="clients"/> clients on real loopback sockets. <paramref name="build"/> fills each branch (called after its API is set).</summary>
    public static NetCluster Start(int clients, Action<NetPeer> build, bool lossy = false, double loss = 0.1, double dup = 0.05, int minDelay = 0, int maxDelay = 25, bool relay = true, int seed = 7)
    {
        var c = new NetCluster();
        NetPeer server = c.Add("S", build, relay);
        var net = new ENetMultiplayerPeer();
        net.SetBindIp("127.0.0.1");
        if (net.CreateServer(0, 8, 2) != Error.Ok) throw new InvalidOperationException("cannot create the server");
        server.Net = net;
        server.Mp.MultiplayerPeer = net;
        int port = net.Host!.GetLocalPort();
        if (lossy)
        {
            c.Proxy = new LossyProxy(port, seed) { Loss = loss, Duplicate = dup, MinDelayMs = minDelay, MaxDelayMs = maxDelay };
            port = c.Proxy.Port;
        }
        for (int i = 1; i <= clients; i++)
        {
            NetPeer p = c.Add("C" + i, build, relay);
            var cn = new ENetMultiplayerPeer();
            if (cn.CreateClient("127.0.0.1", port, 2) != Error.Ok) throw new InvalidOperationException("cannot create the client");
            if (lossy) ((ENetPacketPeer)cn.Host!.GetPeers()[0].AsGodotObject()!).SetTimeout(64, 20000, 600000);
            p.Net = cn;
            p.Mp.MultiplayerPeer = cn;
        }
        return c;
    }

    /// <summary>Another client that joins an ENet cluster that is already running (a late joiner).</summary>
    public NetPeer AddLateClient(Action<NetPeer> build, bool relay = true)
    {
        NetPeer p = Add("C" + Peers.Count, build, relay);
        var cn = new ENetMultiplayerPeer();
        int port = Proxy?.Port ?? ((ENetMultiplayerPeer)Server.Net).Host!.GetLocalPort();
        if (cn.CreateClient("127.0.0.1", port, 2) != Error.Ok) throw new InvalidOperationException("cannot create the late client");
        p.Net = cn;
        p.Mp.MultiplayerPeer = cn;
        return p;
    }

    /// <summary>The same cluster over the in-memory hub.</summary>
    public static NetCluster StartMem(MemHub hub, int clients, Action<NetPeer> build, bool relay = true)
    {
        var c = new NetCluster();
        NetPeer server = c.Add("S", build, relay);
        server.Net = hub.AddServer();
        server.Mp.MultiplayerPeer = server.Net;
        for (int i = 1; i <= clients; i++)
        {
            NetPeer p = c.Add("C" + i, build, relay);
            p.Net = hub.AddClient();
            p.Mp.MultiplayerPeer = p.Net;
        }
        return c;
    }

    /// <summary>Gives a node (found by path in every branch) to a peer: authority is local state, so every process is told.</summary>
    public void SetAuthority(string path, int id, bool recursive = true)
    {
        foreach (NetPeer p in Peers) p.Root.GetNode(path).SetMultiplayerAuthority(id, recursive);
    }

    private NetPeer Add(string name, Action<NetPeer> build, bool relay)
    {
        var p = new NetPeer { Name = name, Root = new Node { Name = name }, Mp = new SceneMultiplayer { ServerRelay = relay } };
        SceneTree.Instance.Root.AddChild(p.Root);
        SceneTree.Instance.SetMultiplayer(p.Mp, p.Root.GetPath());
        p.Record();
        build(p);
        Peers.Add(p);
        return p;
    }

    /// <summary>The usual content: a "Game" NetNode in each branch.</summary>
    public static void GameOnly(NetPeer p) => p.Root.AddChild(new NetNode { Name = "Game" });
}

/// <summary>A seeded UDP proxy on loopback between clients and one server: drops, duplicates and delays datagrams, in both directions.</summary>
sealed class LossyProxy
{
    private readonly PacketPeerUDP _down = new();
    private readonly Dictionary<string, (PacketPeerUDP Up, string Ip, int Port)> _ups = new();
    private readonly List<(long Due, long Seq, Action Send)> _queue = new();
    private readonly Random _rng;
    private readonly int _serverPort;
    private long _seq;

    public int Port { get; }
    public double Loss, Duplicate;
    public int MinDelayMs, MaxDelayMs;
    public int Dropped, Duplicated, Passed;

    public LossyProxy(int serverPort, int seed)
    {
        _serverPort = serverPort;
        _rng = new Random(seed);
        if (_down.Bind(0, "127.0.0.1") != Error.Ok) throw new InvalidOperationException("proxy bind");
        Port = _down.GetLocalPort();
    }

    private void Schedule(Action send)
    {
        if (_rng.NextDouble() < Loss) { Dropped++; return; }
        int copies = _rng.NextDouble() < Duplicate ? 2 : 1;
        if (copies == 2) Duplicated++;
        for (int i = 0; i < copies; i++)
        {
            long due = System.Environment.TickCount64 + _rng.Next(MinDelayMs, MaxDelayMs + 1);
            _queue.Add((due, _seq++, send));
        }
    }

    public void Pump()
    {
        while (_down.GetAvailablePacketCount() > 0)
        {
            byte[] data = _down.GetPacket();
            string ip = _down.GetPacketIP();
            int port = _down.GetPacketPort();
            string key = ip + ":" + port;
            if (!_ups.TryGetValue(key, out var up))
            {
                var peer = new PacketPeerUDP();
                peer.ConnectToHost("127.0.0.1", _serverPort);
                _ups[key] = up = (peer, ip, port);
            }
            PacketPeerUDP u = up.Up;
            Schedule(() => u.PutPacket(data));
        }
        foreach (var up in _ups.Values.ToArray())
        {
            while (up.Up.GetAvailablePacketCount() > 0)
            {
                byte[] data = up.Up.GetPacket();
                string ip = up.Ip; int port = up.Port;
                Schedule(() => { _down.SetDestAddress(ip, port); _down.PutPacket(data); });
            }
        }
        long now = System.Environment.TickCount64;
        foreach (var item in _queue.Where(q => q.Due <= now).OrderBy(q => q.Due).ThenBy(q => q.Seq).ToArray())
        {
            _queue.Remove(item);
            Passed++;
            item.Send();
        }
    }

    public void Close()
    {
        _down.Close();
        foreach (var u in _ups.Values) u.Up.Close();
    }
}

/// <summary>An in-memory transport hub. Every send is logged (mode, channel, target). Loss, duplication and reordering are seeded and apply to the unreliable modes only
/// (reliable packets are delivered in order, as ENet delivers them), so a test can check what the multiplayer layer does with a link that misbehaves.</summary>
sealed class MemHub
{
    internal sealed record Pkt(int From, int To, byte[] Data, MultiplayerPeer.TransferModeEnum Mode, int Channel, long Due, long Seq);

    public readonly List<MemPeer> Peers = new();
    public readonly List<(int Target, MultiplayerPeer.TransferModeEnum Mode, int Channel, int Length)> SentLog = new();
    public double Loss, Duplicate;
    public int MaxDelayTicks;
    public int Dropped;
    private readonly Random _rng;
    private long _tick, _seq;
    private readonly List<Pkt> _flight = new();
    private readonly Dictionary<(int, int), long> _lastReliableDue = new();

    public MemHub(int seed = 3) { _rng = new Random(seed); }

    public MemPeer AddServer()
    {
        var p = new MemPeer(this, 1, server: true) { Status = MultiplayerPeer.ConnectionStatus.Connected };
        Peers.Add(p);
        return p;
    }

    public MemPeer AddClient()
    {
        var p = new MemPeer(this, 2 + Peers.Count * 7 + _rng.Next(0, 5), server: false) { Status = MultiplayerPeer.ConnectionStatus.Connecting };
        Peers.Add(p);
        return p;
    }

    public MemPeer Server => Peers[0];

    internal void Send(int from, int to, byte[] data, MultiplayerPeer.TransferModeEnum mode, int channel)
    {
        bool reliable = mode == MultiplayerPeer.TransferModeEnum.Reliable;
        if (!reliable && _rng.NextDouble() < Loss) { Dropped++; return; }
        long due = _tick + (MaxDelayTicks > 0 && !reliable ? _rng.Next(0, MaxDelayTicks + 1) : 0);
        if (reliable)
        {
            _lastReliableDue.TryGetValue((from, to), out long last);
            due = Math.Max(due, last);
            _lastReliableDue[(from, to)] = due;
        }
        _flight.Add(new Pkt(from, to, (byte[])data.Clone(), mode, channel, due, _seq++));
        if (!reliable && _rng.NextDouble() < Duplicate) _flight.Add(new Pkt(from, to, (byte[])data.Clone(), mode, channel, due + _rng.Next(0, 2), _seq++));
    }

    internal void Tick() => _tick++;

    internal IEnumerable<Pkt> DueFor(int to)
    {
        var due = _flight.Where(p => p.To == to && p.Due <= _tick).OrderBy(p => p.Due).ThenBy(p => p.Seq).ToList();
        foreach (Pkt p in due) _flight.Remove(p);
        return due;
    }

    internal void DropFor(int id) => _flight.RemoveAll(p => p.To == id || p.From == id);
}

sealed class MemPeer : MultiplayerPeer
{
    private readonly MemHub _hub;
    private readonly bool _server;
    private readonly Queue<(int From, byte[] Data, TransferModeEnum Mode, int Channel)> _in = new();
    private (int From, byte[] Data, TransferModeEnum Mode, int Channel) _cur;
    private readonly Dictionary<(int, int), long> _lastOrdered = new();
    private readonly HashSet<int> _known = new();
    private int _target;
    private readonly int _id;
    private bool _announced;

    public ConnectionStatus Status;

    public MemPeer(MemHub hub, int id, bool server) { _hub = hub; _id = id; _server = server; }

    public override void SetTargetPeer(int id) => _target = id;
    public override int GetPacketPeer() => _cur.From;
    public override int GetPacketChannel() => _cur.Channel;
    public override TransferModeEnum GetPacketMode() => _cur.Mode;
    public override ConnectionStatus GetConnectionStatus() => Status;
    public override int GetUniqueId() => Status == ConnectionStatus.Disconnected ? 0 : _id;
    public override bool IsServer() => _server;
    public override bool IsServerRelaySupported() => true;
    public override int GetAvailablePacketCount() => _in.Count;

    internal override Error PopPacket(out byte[] data)
    {
        if (_in.Count == 0) { data = System.Array.Empty<byte>(); return Error.Unavailable; }
        _cur = _in.Dequeue();
        data = _cur.Data;
        return Error.Ok;
    }

    public override Error PutPacket(byte[] buffer)
    {
        if (Status != ConnectionStatus.Connected) return Error.Unconfigured;
        _hub.SentLog.Add((_target, TransferMode, TransferChannel, buffer.Length));
        IEnumerable<MemPeer> others = _hub.Peers.Where(p => p != this && p.Status == ConnectionStatus.Connected && (_server ? p._known.Contains(_id) : p._server));
        if (_target > 0)
        {
            MemPeer? t = others.FirstOrDefault(p => p._id == _target);
            if (t == null) return Error.InvalidParameter;
            others = new[] { t };
        }
        else if (_target < 0) others = others.Where(p => p._id != -_target);
        foreach (MemPeer p in others) _hub.Send(_id, p._id, buffer, TransferMode, TransferChannel);
        return Error.Ok;
    }

    public override void Poll()
    {
        if (_server) _hub.Tick();
        if (!_announced && Status == ConnectionStatus.Connecting)
        {
            // the first poll of a client completes the connection; the server learns of it on its next poll
            _announced = true;
            Status = ConnectionStatus.Connected;
            MemPeer srv = _hub.Server;
            srv._known.Add(_id);
            _known.Add(1);
            EmitPeerConnected(1);
            srv._pendingConnects.Enqueue(_id);
        }
        while (_pendingConnects.Count > 0) EmitPeerConnected(_pendingConnects.Dequeue());
        while (_pendingGone.Count > 0)
        {
            int g = _pendingGone.Dequeue();
            _known.Remove(g);
            if (!_server && g == 1) Status = ConnectionStatus.Disconnected;
            EmitPeerDisconnected(g);
        }
        foreach (MemHub.Pkt p in _hub.DueFor(_id).ToList())
        {
            if (p.Mode == TransferModeEnum.UnreliableOrdered)
            {
                if (_lastOrdered.TryGetValue((p.From, p.Channel), out long last) && p.Seq <= last) continue;
                _lastOrdered[(p.From, p.Channel)] = p.Seq;
            }
            _in.Enqueue((p.From, p.Data, p.Mode, p.Channel));
        }
    }

    private readonly Queue<int> _pendingConnects = new();
    private readonly Queue<int> _pendingGone = new();

    public override void Close()
    {
        if (Status == ConnectionStatus.Disconnected) return;
        Status = ConnectionStatus.Disconnected;
        _hub.DropFor(_id);
        foreach (MemPeer p in _hub.Peers.Where(p => p != this && p._known.Contains(_id))) p._pendingGone.Enqueue(_id);
        if (!_server) _hub.Server._known.Remove(_id);
        _in.Clear();
    }

    public override void DisconnectPeer(int peer, bool force = false)
    {
        MemPeer? p = _hub.Peers.FirstOrDefault(x => x._id == peer);
        if (p == null || !_known.Contains(peer)) return;
        p._pendingGone.Enqueue(_id);
        p.Status = ConnectionStatus.Disconnected;
        _known.Remove(peer);
        _pendingGone.Enqueue(peer);
    }
}
