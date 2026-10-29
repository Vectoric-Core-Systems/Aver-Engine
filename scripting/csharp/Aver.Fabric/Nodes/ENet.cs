// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET1: ENetConnection and ENetPacketPeer over Aver.Net.Abi's reliable-UDP host. The protocol is Aver's own (modules/net), not ENet's wire format: a Godot
// project talks to another Aver process, not to a stock Godot server. Compression, DTLS and the packet-throttle controller are not implemented; see
// docs/plans/fabric/G6_NETWORK.md.

using GArray = Aver.Fabric.Collections.Array;

namespace Aver.Fabric;

/// <summary>One remote end of an <see cref="ENetConnection"/> (Godot's ENetPacketPeer). Received packets queue here; read them with GetPacket().</summary>
public class ENetPacketPeer : PacketPeer
{
    public enum PeerState
    {
        Disconnected = 0, Connecting = 1, AcknowledgingConnect = 2, ConnectionPending = 3, ConnectionSucceeded = 4, Connected = 5,
        DisconnectLater = 6, Disconnecting = 7, AcknowledgingDisconnect = 8, Zombie = 9,
    }

    public enum PeerStatistic
    {
        PacketLoss = 0, PacketLossVariance = 1, PacketThrottle = 2, RoundTripTime = 3, RoundTripTimeVariance = 4, LastRoundTripTime = 5,
        LastRoundTripTimeVariance = 6, PacketLossEpoch = 7, PacketThrottleEpoch = 8, PacketThrottleAcceleration = 9, PacketThrottleDeceleration = 10,
        PacketThrottleInterval = 11,
    }

    public const int PacketLossScale = 65536;
    public const int PacketThrottleScale = 32;
    public const int FlagReliable = 1;
    public const int FlagUnsequenced = 2;
    public const int FlagUnreliableFragment = 8;

    private readonly ENetConnection _owner;
    private readonly long _key;
    private readonly Queue<(byte[] Data, int Channel, int Flags)> _queue = new();
    private int _lastChannel, _lastFlags;
    private string _remoteIp = "";
    private int _remotePort;

    internal ENetPacketPeer(ENetConnection owner, long key)
    {
        _owner = owner;
        _key = key;
        var ip = new byte[64];
        if (NetNative.aver_net_peer_address(owner.HostHandle, key, ip, ip.Length, out int port) == 1) { _remoteIp = NetUtil.IpText(ip); _remotePort = port; }
    }

    internal long Key => _key;
    internal void Enqueue(byte[] data, int channel, int flags) => _queue.Enqueue((data, channel, flags));

    private int H => _owner.HostHandle;

    public PeerState GetState() => H == 0 ? PeerState.Disconnected : (PeerState)NetNative.aver_net_peer_state(H, _key);

    public bool IsActive() => GetState() == PeerState.Connected;

    public int GetChannels() => H == 0 ? 0 : (int)NetNative.aver_net_peer_stat(H, _key, (int)NetPeerMetric.Channels);

    /// <summary>Sends on a channel with ENet's flags (reliable, unsequenced; neither is unreliable and ordered).</summary>
    public Error Send(int channel, byte[] packet, int flags)
    {
        if (H == 0) return Error.Unconfigured;
        int send = ((flags & FlagReliable) != 0 ? (int)NetSendFlag.Reliable : 0) | ((flags & FlagUnsequenced) != 0 ? (int)NetSendFlag.Unsequenced : 0);
        int rc = NetNative.aver_net_peer_send(H, _key, channel, packet, packet.Length, send);
        return rc switch
        {
            0 => Error.Ok,
            -1 => Error.Unconfigured,        // not connected
            -2 => Error.InvalidParameter,    // no such channel
            -3 => Error.OutOfMemory,         // too big
            _ => Error.Unconfigured,
        };
    }

    public void Ping() { if (H != 0) NetNative.aver_net_peer_ping(H, _key); }
    public void PingInterval(int pingInterval) { if (H != 0) NetNative.aver_net_peer_set_ping_interval(H, _key, pingInterval); }
    public void Reset() { if (H != 0) NetNative.aver_net_peer_reset(H, _key); }
    public void PeerDisconnect(int data = 0) { if (H != 0) NetNative.aver_net_peer_disconnect(H, _key, (uint)data, (int)NetDisconnectMode.Request); }
    public void PeerDisconnectLater(int data = 0) { if (H != 0) NetNative.aver_net_peer_disconnect(H, _key, (uint)data, (int)NetDisconnectMode.Later); }
    public void PeerDisconnectNow(int data = 0) { if (H != 0) NetNative.aver_net_peer_disconnect(H, _key, (uint)data, (int)NetDisconnectMode.Now); }

    /// <summary>Stored and reported; Aver applies no RTT-driven throttle, so it changes nothing on the wire.</summary>
    public void ThrottleConfigure(int interval, int acceleration, int deceleration) { if (H != 0) NetNative.aver_net_peer_throttle(H, _key, interval, acceleration, deceleration); }

    public void SetTimeout(int timeout, int timeoutMin, int timeoutMax) { if (H != 0) NetNative.aver_net_peer_set_timeout(H, _key, timeout, timeoutMin, timeoutMax); }

    public double GetStatistic(PeerStatistic statistic) => H == 0 ? 0 : NetNative.aver_net_peer_stat(H, _key, (int)statistic);

    public string GetRemoteAddress() => _remoteIp;
    public int GetRemotePort() => _remotePort;

    /// <summary>The channel the last packet taken with GetPacket() arrived on.</summary>
    public int GetPacketChannel() => _lastChannel;

    /// <summary>The flags the last packet taken arrived with (FlagReliable, FlagUnsequenced, or 0).</summary>
    public int GetPacketFlags() => _lastFlags;

    public override int GetAvailablePacketCount() => _queue.Count;

    internal override Error PopPacket(out byte[] data)
    {
        if (_queue.Count == 0) { data = System.Array.Empty<byte>(); return Error.Unavailable; }
        var p = _queue.Dequeue();
        data = p.Data; _lastChannel = p.Channel; _lastFlags = p.Flags;
        return Error.Ok;
    }

    /// <summary>A reliable packet on channel 0.</summary>
    public override Error PutPacket(byte[] buffer) => Send(0, buffer, FlagReliable);
}

/// <summary>A reliable-UDP host: connects out, accepts in and moves packets (Godot's ENetConnection).</summary>
public class ENetConnection : RefCounted
{
    public enum CompressionMode { None = 0, RangeCoder = 1, Fastlz = 2, Zlib = 3, Zstd = 4 }

    public enum EventType { Error = -1, None = 0, Connect = 1, Disconnect = 2, Receive = 3 }

    public enum HostStatistic { SentData = 0, SentPackets = 1, ReceivedData = 2, ReceivedPackets = 3 }

    private int _host;
    private int _maxChannels;
    private readonly Dictionary<long, ENetPacketPeer> _peers = new();
    private readonly double[] _statBase = new double[4];
    private CompressionMode _compression;

    internal int HostHandle => _host;

    internal struct RawEvent { public NetEvent Type; public ENetPacketPeer? Peer; public uint Data; public int Channel; public int Reason; }

    public Error CreateHostBound(string bindAddress, int bindPort, int maxPeers = 32, int maxChannels = 0, int inBandwidth = 0, int outBandwidth = 0)
    {
        if (_host != 0) return Error.AlreadyInUse;
        if (!NetNative.Available) return Error.Unavailable;
        if (bindPort < 0 || bindPort > 65535 || maxPeers < 1 || maxPeers > 4095 || maxChannels < 0 || maxChannels > 255) return Error.InvalidParameter;
        string ip = bindAddress;
        if (ip != "*" && !NetUtil.Address(bindAddress, out ip)) return Error.InvalidParameter;
        _host = NetNative.aver_net_host_create(ip, bindPort, maxPeers, maxChannels, inBandwidth, outBandwidth);
        if (_host == 0) return Error.CantCreate;
        _maxChannels = maxChannels == 0 ? 255 : maxChannels;
        return Error.Ok;
    }

    /// <summary>A host with no address of its own (a client): its socket opens at the first ConnectToHost.</summary>
    public Error CreateHost(int maxPeers = 32, int maxChannels = 0, int inBandwidth = 0, int outBandwidth = 0)
    {
        if (_host != 0) return Error.AlreadyInUse;
        if (!NetNative.Available) return Error.Unavailable;
        if (maxPeers < 1 || maxPeers > 4095 || maxChannels < 0 || maxChannels > 255) return Error.InvalidParameter;
        _host = NetNative.aver_net_host_create(null, 0, maxPeers, maxChannels, inBandwidth, outBandwidth);
        if (_host == 0) return Error.CantCreate;
        _maxChannels = maxChannels == 0 ? 255 : maxChannels;
        return Error.Ok;
    }

    public new void Destroy()
    {
        if (_host != 0) NetNative.aver_net_host_destroy(_host);
        _host = 0;
        _peers.Clear();
    }

    public ENetPacketPeer? ConnectToHost(string address, int port, int channels = 0, int data = 0)
    {
        if (_host == 0 || port < 1 || port > 65535 || channels < 0 || channels > 255) return null;
        if (!NetUtil.Address(address, out string ip)) return null;
        long key = NetNative.aver_net_host_connect(_host, ip, port, channels, (uint)data);
        if (key == 0) return null;
        return Wrap(key);
    }

    private ENetPacketPeer Wrap(long key)
    {
        if (!_peers.TryGetValue(key, out ENetPacketPeer? p)) { p = new ENetPacketPeer(this, key); _peers[key] = p; }
        return p;
    }

    /// <summary>The next event, as a list of [EventType, peer, data, channel]; waits up to <paramref name="timeout"/> ms for one.</summary>
    public GArray Service(int timeout = 0)
    {
        var r = new GArray();
        if (!ServiceRaw(timeout, out RawEvent ev)) { r.Add((long)EventType.None); r.Add(default(Variant)); r.Add(0L); r.Add(0L); return r; }
        r.Add((long)(EventType)(int)ev.Type);
        r.Add(ev.Peer);
        r.Add((long)ev.Data);
        r.Add((long)ev.Channel);
        return r;
    }

    internal bool ServiceRaw(int timeout, out RawEvent ev)
    {
        ev = default;
        if (_host == 0) return false;
        int rc = NetNative.aver_net_host_service(_host, timeout, out int type, out long key, out int channel, out uint data, out int reason, out int length);
        if (rc != 1) return false;
        ev.Type = (NetEvent)type; ev.Channel = channel; ev.Reason = reason;
        switch (ev.Type)
        {
            case NetEvent.Connect:
                ev.Peer = Wrap(key);
                ev.Data = data;
                break;
            case NetEvent.Disconnect:
                ev.Peer = Wrap(key);
                ev.Data = data;
                _peers.Remove(key);
                break;
            case NetEvent.Receive:
            {
                ev.Peer = Wrap(key);
                var bytes = new byte[length];
                if (length > 0) NetNative.aver_net_host_event_data(_host, bytes, length);
                ev.Data = data;
                ev.Peer.Enqueue(bytes, channel, (int)data);
                ev.Data = 0;
                break;
            }
        }
        return true;
    }

    public void Flush() { if (_host != 0) NetNative.aver_net_host_flush(_host); }

    public void BandwidthLimit(int inBandwidth = 0, int outBandwidth = 0) { if (_host != 0) NetNative.aver_net_host_bandwidth_limit(_host, inBandwidth, outBandwidth); }

    public void ChannelLimit(int limit)
    {
        if (_host == 0) return;
        NetNative.aver_net_host_channel_limit(_host, limit);
        _maxChannels = limit <= 0 ? 255 : Math.Min(limit, 255);
    }

    /// <summary>Sends to every connected peer.</summary>
    public void Broadcast(int channel, byte[] packet, int flags)
    {
        if (_host == 0) return;
        int send = ((flags & ENetPacketPeer.FlagReliable) != 0 ? (int)NetSendFlag.Reliable : 0) | ((flags & ENetPacketPeer.FlagUnsequenced) != 0 ? (int)NetSendFlag.Unsequenced : 0);
        NetNative.aver_net_host_broadcast(_host, channel, packet, packet.Length, send);
    }

    /// <summary>Stored; Aver's protocol has no compression, so the wire is unchanged.</summary>
    public void Compress(CompressionMode mode) => _compression = mode;

    public Error DtlsServerSetup(object serverOptions) => Error.Unavailable;
    public Error DtlsClientSetup(string hostname, object? clientOptions = null) => Error.Unavailable;

    public void RefuseNewConnections(bool refuse) { if (_host != 0) NetNative.aver_net_host_refuse(_host, refuse ? 1 : 0); }

    /// <summary>The traffic since the last call for the same statistic.</summary>
    public double PopStatistic(HostStatistic statistic)
    {
        if (_host == 0) return 0;
        int which = statistic switch { HostStatistic.SentData => (int)NetHostStat.SentBytes, HostStatistic.SentPackets => (int)NetHostStat.SentPackets, HostStatistic.ReceivedData => (int)NetHostStat.ReceivedBytes, _ => (int)NetHostStat.ReceivedPackets };
        double total = NetNative.aver_net_host_stat(_host, which);
        double d = total - _statBase[(int)statistic];
        _statBase[(int)statistic] = total;
        return d;
    }

    public int GetMaxChannels() => _maxChannels;

    public int GetLocalPort() => _host == 0 ? 0 : NetNative.aver_net_host_local_port(_host);

    /// <summary>The peers that are connected or connecting.</summary>
    public GArray GetPeers()
    {
        var a = new GArray();
        foreach (var kv in _peers.ToList())
        {
            if (kv.Value.GetState() == ENetPacketPeer.PeerState.Disconnected) { _peers.Remove(kv.Key); continue; }
            a.Add(kv.Value);
        }
        return a;
    }

    /// <summary>Sends a raw datagram from the host's socket.</summary>
    public void SocketSend(string destinationAddress, int destinationPort, byte[] packet)
    {
        if (_host == 0 || !NetUtil.Address(destinationAddress, out string ip)) return;
        NetNative.aver_net_host_socket_send(_host, ip, destinationPort, packet, packet.Length);
    }

    public override void Free()
    {
        Destroy();
        base.Free();
    }
}
