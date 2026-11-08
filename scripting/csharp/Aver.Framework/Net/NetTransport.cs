// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// What replication needs from a transport: listen, connect, poll for events, send on one of two channels. NativeNetTransport drives modules/net
// (Aver.Net.Abi); LoopbackNetwork is the in-process one the tests and a single-process listen server use.

using System.Runtime.InteropServices;

namespace Aver.Framework.Net;

public enum NetTransportEventType { Connect = 1, Disconnect = 2, Data = 3 }

/// <summary>One thing the transport saw. Peers are named by an opaque non-zero id.</summary>
public struct NetTransportEvent
{
    public NetTransportEventType Type;
    public long Peer;
    public int Channel;
    public byte[]? Data;
}

/// <summary>Channel 0 is for reliable traffic (acknowledged, in order); channel 1 for unreliable-ordered traffic (newest wins).</summary>
public interface INetTransport : IDisposable
{
    public const int ReliableChannel = 0;
    public const int UnreliableChannel = 1;

    /// <summary>Starts accepting peers; <paramref name="boundPort"/> is the port in use (0 asks for a free one).</summary>
    bool Listen(string ip, int port, int maxPeers, out int boundPort);
    /// <summary>Starts a connection and returns the peer id (0 on failure). The Connect event comes from <see cref="Poll"/>.</summary>
    long Connect(string ip, int port);
    /// <summary>Moves the network once and hands out the next event; false when none is pending.</summary>
    bool Poll(out NetTransportEvent ev);
    bool Send(long peer, bool reliable, byte[] data);
    /// <summary>A graceful close: data already queued is delivered first. A Disconnect event follows on both sides.</summary>
    void Disconnect(long peer);
}

/// <summary>An in-memory network: transports made from one <see cref="LoopbackNetwork"/> reach each other by port. Optionally drops
/// unreliable packets (a rule the caller supplies) so tests can exercise loss.</summary>
public sealed class LoopbackNetwork
{
    internal readonly Dictionary<int, LoopbackTransport> Listeners = new();
    private int _nextPort = 40000;
    private long _nextPeer = 1;

    /// <summary>Called for each unreliable packet; true drops it.</summary>
    public Func<byte[], bool>? DropUnreliable;

    public LoopbackTransport CreateTransport() => new(this);
    internal int AllocPort() => _nextPort++;
    internal long AllocPeer() => _nextPeer++;
}

public sealed class LoopbackTransport : INetTransport
{
    private readonly LoopbackNetwork _net;
    private readonly Queue<NetTransportEvent> _events = new();
    private readonly Dictionary<long, (LoopbackTransport Other, long OtherPeer)> _peers = new();
    private int _port;
    private int _maxPeers;

    internal LoopbackTransport(LoopbackNetwork net) => _net = net;

    public bool Listen(string ip, int port, int maxPeers, out int boundPort)
    {
        boundPort = 0;
        if (_port != 0) return false;
        int p = port == 0 ? _net.AllocPort() : port;
        if (_net.Listeners.ContainsKey(p)) return false;
        _net.Listeners[p] = this;
        _port = boundPort = p;
        _maxPeers = maxPeers;
        return true;
    }

    public long Connect(string ip, int port)
    {
        if (!_net.Listeners.TryGetValue(port, out LoopbackTransport? server) || server._peers.Count >= server._maxPeers) return 0;
        long mine = _net.AllocPeer(), theirs = _net.AllocPeer();
        _peers[mine] = (server, theirs);
        server._peers[theirs] = (this, mine);
        _events.Enqueue(new NetTransportEvent { Type = NetTransportEventType.Connect, Peer = mine });
        server._events.Enqueue(new NetTransportEvent { Type = NetTransportEventType.Connect, Peer = theirs });
        return mine;
    }

    public bool Poll(out NetTransportEvent ev)
    {
        if (_events.Count == 0) { ev = default; return false; }
        ev = _events.Dequeue();
        return true;
    }

    public bool Send(long peer, bool reliable, byte[] data)
    {
        if (!_peers.TryGetValue(peer, out var to)) return false;
        if (!reliable && _net.DropUnreliable is { } drop && drop(data)) return true;
        to.Other._events.Enqueue(new NetTransportEvent
        {
            Type = NetTransportEventType.Data, Peer = to.OtherPeer, Data = (byte[])data.Clone(),
            Channel = reliable ? INetTransport.ReliableChannel : INetTransport.UnreliableChannel,
        });
        return true;
    }

    public void Disconnect(long peer)
    {
        if (!_peers.Remove(peer, out var to)) return;
        to.Other._peers.Remove(to.OtherPeer);
        _events.Enqueue(new NetTransportEvent { Type = NetTransportEventType.Disconnect, Peer = peer });
        to.Other._events.Enqueue(new NetTransportEvent { Type = NetTransportEventType.Disconnect, Peer = to.OtherPeer });
    }

    public void Dispose()
    {
        foreach (long p in _peers.Keys.ToArray()) Disconnect(p);
        if (_port != 0) _net.Listeners.Remove(_port);
        _port = 0;
    }
}

/// <summary>The reliable-UDP transport of <c>modules/net</c>, through the <c>Aver.Net.Abi</c> library. Needs that library beside the executable.</summary>
public sealed class NativeNetTransport : INetTransport
{
    private const string Lib = "Aver.Net.Abi";

    [DllImport(Lib)] private static extern int aver_net_host_create(string? bindIp, int port, int maxPeers, int channelLimit, int inBandwidth, int outBandwidth);
    [DllImport(Lib)] private static extern int aver_net_host_destroy(int host);
    [DllImport(Lib)] private static extern int aver_net_host_local_port(int host);
    [DllImport(Lib)] private static extern long aver_net_host_connect(int host, string ip, int port, int channels, uint data);
    [DllImport(Lib)] private static extern int aver_net_host_service(int host, int timeoutMs, out int type, out long peer, out int channel, out uint data, out int reason, out int length);
    [DllImport(Lib)] private static extern int aver_net_host_event_data(int host, byte[] buffer, int capacity);
    [DllImport(Lib)] private static extern int aver_net_host_flush(int host);
    [DllImport(Lib)] private static extern int aver_net_peer_send(int host, long peer, int channel, byte[] data, int length, int flags);
    [DllImport(Lib)] private static extern int aver_net_peer_disconnect(int host, long peer, uint data, int mode);

    private int _host;
    private readonly bool _listen;

    private NativeNetTransport(int host, bool listen) { _host = host; _listen = listen; }

    /// <summary>True when the native library loads.</summary>
    public static bool IsAvailable()
    {
        try { int h = aver_net_host_create(null, 0, 1, 2, 0, 0); if (h == 0) return false; aver_net_host_destroy(h); return true; }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { return false; }
    }

    /// <summary>A host that accepts connections on <paramref name="ip"/>:<paramref name="port"/> (loopback unless the caller means to listen wider).</summary>
    public static NativeNetTransport? CreateServer(string ip, int port, int maxPeers, out int boundPort)
    {
        boundPort = 0;
        int h = aver_net_host_create(ip, port, maxPeers, 2, 0, 0);
        if (h == 0) return null;
        boundPort = aver_net_host_local_port(h);
        return new NativeNetTransport(h, true);
    }

    public static NativeNetTransport? CreateClient()
    {
        int h = aver_net_host_create(null, 0, 1, 2, 0, 0);
        return h == 0 ? null : new NativeNetTransport(h, false);
    }

    public bool Listen(string ip, int port, int maxPeers, out int boundPort)
    {
        boundPort = _listen && _host != 0 ? aver_net_host_local_port(_host) : 0;
        return boundPort != 0;
    }

    public long Connect(string ip, int port) => _host == 0 ? 0 : aver_net_host_connect(_host, ip, port, 2, 0);

    public bool Poll(out NetTransportEvent ev)
    {
        ev = default;
        if (_host == 0) return false;
        if (aver_net_host_service(_host, 0, out int type, out long peer, out int channel, out _, out _, out int length) != 1) return false;
        ev.Type = (NetTransportEventType)type;
        ev.Peer = peer;
        ev.Channel = channel;
        if (type == 3)
        {
            ev.Data = new byte[length];
            aver_net_host_event_data(_host, ev.Data, length);
        }
        return true;
    }

    public bool Send(long peer, bool reliable, byte[] data) =>
        _host != 0 && aver_net_peer_send(_host, peer, reliable ? INetTransport.ReliableChannel : INetTransport.UnreliableChannel, data, data.Length, reliable ? 1 : 0) == 0;

    public void Disconnect(long peer) { if (_host != 0) aver_net_peer_disconnect(_host, peer, 0, 1); }

    public void Dispose()
    {
        if (_host == 0) return;
        aver_net_host_flush(_host);
        aver_net_host_destroy(_host);
        _host = 0;
    }
}
