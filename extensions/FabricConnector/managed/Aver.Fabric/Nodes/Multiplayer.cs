// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET1: MultiplayerPeer (Godot's abstract transport for the high-level multiplayer API), ENetMultiplayerPeer over the reliable-UDP host, and
// OfflineMultiplayerPeer. The MultiplayerAPI on top of it (SceneMultiplayer, RPC, spawner, synchronizer) is a later package: until it lands a script
// polls the peer itself, Poll() once per frame, as it would a peer it had not handed to a SceneTree.

namespace Aver.Fabric;

/// <summary>The transport behind Godot's multiplayer API: peers with ids, packets with a mode and a channel, and connect / disconnect signals.</summary>
public abstract class MultiplayerPeer : PacketPeer
{
    public new class SignalName : GodotObject.SignalName
    {
        public static readonly StringName PeerConnected = "peer_connected";
        public static readonly StringName PeerDisconnected = "peer_disconnected";
    }

    public enum TransferModeEnum { Unreliable = 0, UnreliableOrdered = 1, Reliable = 2 }

    public enum ConnectionStatus { Disconnected = 0, Connecting = 1, Connected = 2 }

    /// <summary>The target of a send meaning every peer.</summary>
    public const int TargetPeerBroadcast = 0;

    /// <summary>The target of a send meaning the server, and the server's own id.</summary>
    public const int TargetPeerServer = 1;

    public event Action<long> PeerConnected { add => Connect(SignalName.PeerConnected, Callable.From(value)); remove => Disconnect(SignalName.PeerConnected, Callable.From(value)); }
    public event Action<long> PeerDisconnected { add => Connect(SignalName.PeerDisconnected, Callable.From(value)); remove => Disconnect(SignalName.PeerDisconnected, Callable.From(value)); }

    public virtual TransferModeEnum TransferMode { get; set; } = TransferModeEnum.Reliable;
    public virtual int TransferChannel { get; set; }
    public virtual bool RefuseNewConnections { get; set; }

    private static readonly Random s_rng = new();

    /// <summary>A random client id: 2 up to 2^31 - 1 (1 is the server's).</summary>
    public uint GenerateUniqueId() => (uint)s_rng.NextInt64(2, 0x7FFFFFFF);

    public abstract void SetTargetPeer(int id);
    public abstract int GetPacketPeer();
    public abstract int GetPacketChannel();
    public abstract TransferModeEnum GetPacketMode();
    public abstract void Poll();
    public abstract void Close();
    public abstract void DisconnectPeer(int peer, bool force = false);
    public abstract ConnectionStatus GetConnectionStatus();
    public abstract int GetUniqueId();
    public abstract bool IsServer();
    public abstract bool IsServerRelaySupported();
    public virtual int GetMaxPacketSize() => 1 << 24;

    protected void EmitPeerConnected(int id) => EmitSignal(SignalName.PeerConnected, (long)id);
    protected void EmitPeerDisconnected(int id) => EmitSignal(SignalName.PeerDisconnected, (long)id);
}

/// <summary>A multiplayer peer that is always the connected server and never talks to anyone (Godot's OfflineMultiplayerPeer).</summary>
public class OfflineMultiplayerPeer : MultiplayerPeer
{
    public override void SetTargetPeer(int id) { }
    public override int GetPacketPeer() => 1;
    public override int GetPacketChannel() => 0;
    public override TransferModeEnum GetPacketMode() => TransferModeEnum.Reliable;
    public override void Poll() { }
    public override void Close() { }
    public override void DisconnectPeer(int peer, bool force = false) { }
    public override ConnectionStatus GetConnectionStatus() => ConnectionStatus.Connected;
    public override int GetUniqueId() => TargetPeerServer;
    public override bool IsServer() => true;
    public override bool IsServerRelaySupported() => false;
    public override int GetMaxPacketSize() => 1;
    public override int GetAvailablePacketCount() => 0;
    internal override Error PopPacket(out byte[] data) { data = System.Array.Empty<byte>(); return Error.Unavailable; }
    public override Error PutPacket(byte[] buffer) => Error.Ok;
}

/// <summary>Multiplayer over the reliable-UDP host (Godot's ENetMultiplayerPeer). The server is peer 1; a client picks a random id and sends it as the connect data.</summary>
public class ENetMultiplayerPeer : MultiplayerPeer
{
    // ENet channels: 0 is reserved (Godot uses it for its own handshake), 1 carries reliable and ordered traffic, 2 unreliable, and a script's
    // transfer_channel n > 0 is channel 2 + n.
    private const int SyschConfig = 0, SyschReliable = 1, SyschUnreliable = 2, SyschMax = 3;

    private enum Mode { None, Server, Client }

    private sealed record Packet(int From, byte[] Data, int Channel, TransferModeEnum Mode);

    private ENetConnection? _host;
    private Mode _mode = Mode.None;
    private ConnectionStatus _status = ConnectionStatus.Disconnected;
    private int _uniqueId;
    private int _target;
    private string _bindIp = "*";
    private readonly Dictionary<int, ENetPacketPeer> _peers = new();
    private readonly Dictionary<long, int> _ids = new();
    private readonly Queue<Packet> _incoming = new();
    private Packet? _current;

    /// <summary>The underlying host (null until a server or client is created).</summary>
    public ENetConnection? Host => _host;

    public Error SetBindIp(string ip)
    {
        if (ip != "*" && !NetUtil.Address(ip, out _)) return Error.InvalidParameter;
        _bindIp = ip;
        return Error.Ok;
    }

    public string GetBindIp() => _bindIp;

    public Error CreateServer(int port, int maxClients = 32, int maxChannels = 0, int inBandwidth = 0, int outBandwidth = 0)
    {
        if (_mode != Mode.None) return Error.AlreadyInUse;
        if (port < 0 || port > 65535 || maxClients < 1 || maxClients > 4095 || maxChannels < 0 || maxChannels > 255 - SyschMax) return Error.InvalidParameter;
        string ip = _bindIp;
        if (ip != "*" && !NetUtil.Address(_bindIp, out ip)) return Error.InvalidParameter;
        var host = new ENetConnection();
        Error e = host.CreateHostBound(ip, port, maxClients, maxChannels > 0 ? maxChannels + SyschMax : 0, inBandwidth, outBandwidth);
        if (e != Error.Ok) return e;
        _host = host;
        _mode = Mode.Server;
        _uniqueId = TargetPeerServer;
        _status = ConnectionStatus.Connected;
        return Error.Ok;
    }

    public Error CreateClient(string address, int port, int channelCount = 0, int inBandwidth = 0, int outBandwidth = 0, int localPort = 0)
    {
        if (_mode != Mode.None) return Error.AlreadyInUse;
        if (port < 1 || port > 65535 || localPort < 0 || localPort > 65535 || channelCount < 0 || channelCount > 255 - SyschMax) return Error.InvalidParameter;
        if (!NetUtil.Address(address, out string ip)) return Error.CantResolve;
        var host = new ENetConnection();
        Error e;
        if (localPort > 0) e = host.CreateHostBound(NetUtil.IsLoopback(ip) ? NetUtil.LoopbackFor(ip) : _bindIp, localPort, 1, channelCount > 0 ? channelCount + SyschMax : 0, inBandwidth, outBandwidth);
        else e = host.CreateHost(1, channelCount > 0 ? channelCount + SyschMax : 0, inBandwidth, outBandwidth);
        if (e != Error.Ok) return e;
        int id = (int)GenerateUniqueId();
        ENetPacketPeer? server = host.ConnectToHost(ip, port, channelCount > 0 ? channelCount + SyschMax : 0, id);
        if (server == null) { host.Destroy(); return Error.CantConnect; }
        _host = host;
        _mode = Mode.Client;
        _uniqueId = id;
        _status = ConnectionStatus.Connecting;
        _clientServerPeer = server;
        return Error.Ok;
    }

    private ENetPacketPeer? _clientServerPeer;

    /// <summary>Mesh mode (every peer connected to every other) is not implemented.</summary>
    public Error CreateMesh(int uniqueId) => Error.Unavailable;
    public Error AddMeshPeer(int peerId, ENetConnection host) => Error.Unavailable;

    public ENetPacketPeer? GetPeer(int id) => _peers.TryGetValue(id, out ENetPacketPeer? p) ? p : null;

    public override bool RefuseNewConnections
    {
        get => base.RefuseNewConnections;
        set { base.RefuseNewConnections = value; _host?.RefuseNewConnections(value); }
    }

    public override void Poll()
    {
        if (_host == null) return;
        for (int guard = 0; guard < 100000; guard++)
        {
            if (_host == null || !_host.ServiceRaw(0, out ENetConnection.RawEvent ev)) break;
            switch (ev.Type)
            {
                case NetEvent.Connect: OnConnect(ev); break;
                case NetEvent.Disconnect: OnDisconnect(ev); break;
                case NetEvent.Receive: OnReceive(ev); break;
            }
        }
    }

    private void OnConnect(ENetConnection.RawEvent ev)
    {
        if (_mode == Mode.Client)
        {
            _status = ConnectionStatus.Connected;
            Register(TargetPeerServer, ev.Peer!);
            EmitPeerConnected(TargetPeerServer);
            return;
        }
        int id = (int)ev.Data;
        if (id <= TargetPeerServer || _peers.ContainsKey(id)) { ev.Peer!.PeerDisconnectNow(); return; }      // not a client id, or taken
        Register(id, ev.Peer!);
        EmitPeerConnected(id);
    }

    private void Register(int id, ENetPacketPeer peer)
    {
        _peers[id] = peer;
        _ids[peer.Key] = id;
    }

    private void OnDisconnect(ENetConnection.RawEvent ev)
    {
        if (!_ids.TryGetValue(ev.Peer!.Key, out int id))
        {
            // a client whose connection never completed
            if (_mode == Mode.Client && ReferenceEquals(ev.Peer, _clientServerPeer)) { _status = ConnectionStatus.Disconnected; _clientServerPeer = null; }
            return;
        }
        _ids.Remove(ev.Peer.Key);
        _peers.Remove(id);
        if (_mode == Mode.Client) { _status = ConnectionStatus.Disconnected; _clientServerPeer = null; }
        EmitPeerDisconnected(id);
    }

    private void OnReceive(ENetConnection.RawEvent ev)
    {
        if (!_ids.TryGetValue(ev.Peer!.Key, out int id)) { ev.Peer.GetPacket(); return; }
        byte[] data = ev.Peer.GetPacket();
        int ch = ev.Peer.GetPacketChannel();
        int flags = ev.Peer.GetPacketFlags();
        TransferModeEnum mode = (flags & ENetPacketPeer.FlagReliable) != 0 ? TransferModeEnum.Reliable
            : (flags & ENetPacketPeer.FlagUnsequenced) != 0 ? TransferModeEnum.Unreliable : TransferModeEnum.UnreliableOrdered;
        _incoming.Enqueue(new Packet(id, data, ch >= SyschMax ? ch - SyschMax + 1 : 0, mode));
    }

    public override void SetTargetPeer(int id) => _target = id;

    public override int GetAvailablePacketCount() => _incoming.Count;

    internal override Error PopPacket(out byte[] data)
    {
        if (_incoming.Count == 0) { _current = null; data = System.Array.Empty<byte>(); return Error.Unavailable; }
        _current = _incoming.Dequeue();
        data = _current.Data;
        return Error.Ok;
    }

    public override int GetPacketPeer() => _current?.From ?? 0;
    public override int GetPacketChannel() => _current?.Channel ?? 0;
    public override TransferModeEnum GetPacketMode() => _current?.Mode ?? TransferModeEnum.Reliable;

    public override Error PutPacket(byte[] buffer)
    {
        if (_host == null || _status != ConnectionStatus.Connected) return Error.Unconfigured;
        if (buffer.Length > GetMaxPacketSize()) return Error.InvalidParameter;
        int flags, channel;
        switch (TransferMode)
        {
            case TransferModeEnum.Unreliable: flags = ENetPacketPeer.FlagUnsequenced; channel = SyschUnreliable; break;
            case TransferModeEnum.UnreliableOrdered: flags = 0; channel = SyschReliable; break;
            default: flags = ENetPacketPeer.FlagReliable; channel = SyschReliable; break;
        }
        if (TransferChannel > 0) channel = SyschMax + TransferChannel - 1;
        if (_target == TargetPeerBroadcast)
        {
            Error first = Error.Ok;
            foreach (ENetPacketPeer p in _peers.Values)
            {
                Error e = p.Send(channel, buffer, flags);
                if (e != Error.Ok && first == Error.Ok) first = e;
            }
            return first;
        }
        if (_target > 0)
        {
            if (!_peers.TryGetValue(_target, out ENetPacketPeer? peer)) return Error.InvalidParameter;
            return peer.Send(channel, buffer, flags);
        }
        Error result = Error.Ok;                       // a negative target: everyone but that peer
        foreach (var kv in _peers)
        {
            if (kv.Key == -_target) continue;
            Error e = kv.Value.Send(channel, buffer, flags);
            if (e != Error.Ok && result == Error.Ok) result = e;
        }
        return result;
    }

    public override void Close()
    {
        if (_host != null)
        {
            foreach (ENetPacketPeer p in _peers.Values) p.PeerDisconnectNow();
            _clientServerPeer?.PeerDisconnectNow();
            _host.Flush();
            _host.Destroy();
        }
        _host = null;
        _peers.Clear();
        _ids.Clear();
        _incoming.Clear();
        _current = null;
        _clientServerPeer = null;
        _mode = Mode.None;
        _status = ConnectionStatus.Disconnected;
        _uniqueId = 0;
    }

    public override void DisconnectPeer(int peer, bool force = false)
    {
        if (!_peers.TryGetValue(peer, out ENetPacketPeer? p)) return;
        if (!force) { p.PeerDisconnect(); return; }          // the Disconnect event arrives on a later Poll
        p.PeerDisconnectNow();
        _host?.Flush();
        _ids.Remove(p.Key);
        _peers.Remove(peer);
        if (_mode == Mode.Client) { _status = ConnectionStatus.Disconnected; _clientServerPeer = null; }
        EmitPeerDisconnected(peer);
    }

    public override ConnectionStatus GetConnectionStatus() => _status;
    public override int GetUniqueId() => _uniqueId;
    public override bool IsServer() => _mode == Mode.Server;
    public override bool IsServerRelaySupported() => _mode != Mode.None;

    public override void Free()
    {
        Close();
        base.Free();
    }
}
