// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke into Aver.Net.Abi (modules/net.abi, net_abi.h): OS sockets and the reliable-UDP host/peer protocol. The enums restate the header's constants and are
// checked against it by AbiEnumTest; the declarations are checked against the header's parameter names and types.

using System.Runtime.InteropServices;
using System.Text;

namespace Aver.Fabric;

/// <summary>AVER_NET_EVENT_*: what aver_net_host_service reports.</summary>
internal enum NetEvent { None = 0, Connect = 1, Disconnect = 2, Receive = 3 }

/// <summary>AVER_NET_REASON_*: why a Disconnect event happened.</summary>
internal enum NetReason { None = 0, Remote = 1, Local = 2, Timeout = 3, ConnectFailed = 4 }

/// <summary>AVER_NET_PEER_STATE_*: Godot's ENetPacketPeer.PeerState numbering.</summary>
internal enum NetPeerState
{
    Disconnected = 0, Connecting = 1, AcknowledgingConnect = 2, ConnectionPending = 3, ConnectionSucceeded = 4, Connected = 5,
    DisconnectLater = 6, Disconnecting = 7, AcknowledgingDisconnect = 8, Zombie = 9,
}

/// <summary>AVER_NET_SEND_FLAG_*.</summary>
internal enum NetSendFlag { Reliable = 1, Unsequenced = 2 }

/// <summary>AVER_NET_DISCONNECT_MODE_*.</summary>
internal enum NetDisconnectMode { Request = 0, Later = 1, Now = 2 }

/// <summary>AVER_NET_PEERSTAT_*: the first twelve are Godot's ENetPacketPeer.PeerStatistic.</summary>
internal enum NetPeerMetric
{
    PacketLoss = 0, PacketLossVariance = 1, PacketThrottle = 2, RoundTripTime = 3, RoundTripTimeVariance = 4, LastRoundTripTime = 5, LastRoundTripTimeVariance = 6,
    PacketLossEpoch = 7, PacketThrottleEpoch = 8, PacketThrottleAcceleration = 9, PacketThrottleDeceleration = 10, PacketThrottleInterval = 11,
    BytesSent = 16, BytesReceived = 17, PacketsSent = 18, PacketsLost = 19, PacketsDropped = 20, Mtu = 21, LowestRoundTripTime = 22, ReliableInTransit = 23,
    PingInterval = 24, Channels = 25,
}

/// <summary>AVER_NET_HOSTSTAT_*.</summary>
internal enum NetHostStat { SentBytes = 0, SentPackets = 1, ReceivedBytes = 2, ReceivedPackets = 3, BadChecksum = 4 }

/// <summary>The exports of Aver.Net.Abi. The library is looked for beside the executable (NativeResolver); a host without it makes
/// <see cref="Available"/> false and the networking classes answer Error.Unavailable.</summary>
internal static class NetNative
{
    private const string Lib = "Aver.Net.Abi";

    internal const int WouldBlock = -1, Failed = -2, Closed = -3;
    internal const int SendBadHandle = -4;

    private static int s_state; // 0 unknown, 1 loaded, 2 missing

    internal static bool Available
    {
        get
        {
            if (s_state == 0)
            {
                try { s_state = aver_net_abi_version() >= 1 ? 1 : 2; }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { s_state = 2; }
            }
            return s_state == 1;
        }
    }

    /// <summary>Forgets a failed probe (a test that has just copied the library beside itself).</summary>
    internal static void Reprobe() { if (s_state == 2) s_state = 0; }

    /// <summary>The UTF-8 text in the first <paramref name="length"/> bytes of a buffer.</summary>
    internal static string Text(byte[] buffer, int length) => length <= 0 ? "" : Encoding.UTF8.GetString(buffer, 0, Math.Min(length, buffer.Length));

    /// <summary>Numeric addresses joined by newlines, as the resolver returns them.</summary>
    internal static List<string> Resolve(string host, int ipType)
    {
        var buf = new byte[1024];
        int n = aver_net_resolve(host, ipType, buf, buf.Length);
        if (n < 0) { buf = new byte[-n + 1]; n = aver_net_resolve(host, ipType, buf, buf.Length); }
        return n <= 0 ? new List<string>() : new List<string>(Text(buf, n).Split('\n', StringSplitOptions.RemoveEmptyEntries));
    }

    internal static List<string> LocalAddresses()
    {
        var buf = new byte[4096];
        int n = aver_net_local_addresses(buf, buf.Length);
        if (n < 0) { buf = new byte[-n + 1]; n = aver_net_local_addresses(buf, buf.Length); }
        return n <= 0 ? new List<string>() : new List<string>(Text(buf, n).Split('\n', StringSplitOptions.RemoveEmptyEntries));
    }

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_abi_version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_net_now_ms();

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_resolve([MarshalAs(UnmanagedType.LPUTF8Str)] string hostName, int ipType, byte[] buffer, int capacity);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_local_addresses(byte[] buffer, int capacity);

    // ---- sockets ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_open(int family);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_bind(int sock, [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_local_port(int sock);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_send(int sock, byte[] data, int length, [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_recv(int sock, byte[] buffer, int capacity, byte[] ipOut, int ipCapacity, out int portOut);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_wait(int sock, int timeoutMs);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_udp_set_broadcast(int sock, int enable);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_connect([MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_status(int sock);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_send(int sock, byte[] data, int length);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_recv(int sock, byte[] buffer, int capacity);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_wait(int sock, int forWrite, int timeoutMs);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_set_nodelay(int sock, int enable);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_local_port(int sock);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_remote(int sock, byte[] ipOut, int ipCapacity, out int portOut);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_listen([MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port, int backlog);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_accept(int listener);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_listener_port(int listener);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_tcp_listener_wait(int listener, int timeoutMs);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_close(int handle);

    // ---- hosts and peers ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_create([MarshalAs(UnmanagedType.LPUTF8Str)] string? bindIp, int port, int maxPeers, int channelLimit, int inBandwidth, int outBandwidth);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_destroy(int host);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_local_port(int host);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_max_peers(int host);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_channel_limit(int host, int limit);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_bandwidth_limit(int host, int inBandwidth, int outBandwidth);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_refuse(int host, int refuse);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_net_host_connect(int host, [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port, int channels, uint data);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_service(int host, int timeoutMs, out int type, out long peer, out int channel, out uint data, out int reason, out int length);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_event_data(int host, byte[] buffer, int capacity);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_flush(int host);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_broadcast(int host, int channel, byte[] data, int length, int flags);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_socket_send(int host, [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, int port, byte[] data, int length);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_host_peers(int host, long[]? keys, int capacity);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_net_host_stat(int host, int which);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_send(int host, long peer, int channel, byte[] data, int length, int flags);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_state(int host, long peer);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_disconnect(int host, long peer, uint data, int mode);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_reset(int host, long peer);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_ping(int host, long peer);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_set_ping_interval(int host, long peer, int intervalMs);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_set_timeout(int host, long peer, int limit, int minimumMs, int maximumMs);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_throttle(int host, long peer, int intervalMs, int acceleration, int deceleration);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_net_peer_address(int host, long peer, byte[] ipOut, int ipCapacity, out int portOut);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern uint aver_net_peer_connect_data(int host, long peer);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern double aver_net_peer_stat(int host, long peer, int which);
}
