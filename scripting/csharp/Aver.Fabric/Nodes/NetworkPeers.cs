// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET1: Godot's low-level networking classes over Aver.Net.Abi (OS sockets): IP, PacketPeer, PacketPeerUDP, UDPServer, StreamPeer, StreamPeerBuffer,
// StreamPeerTCP and TCPServer. Names, enums and error codes are Godot's. Differences are listed in docs/plans/fabric/G6_NETWORK.md: everything is polled from
// the game thread (no worker threads), a name is resolved synchronously (the resolver queue answers at once), multicast is unavailable, and the default
// bind address "*" is honoured only when a script passes it -- code that wants loopback passes "127.0.0.1" or "::1".

using System.Buffers.Binary;
using System.Text;
using GArray = Aver.Fabric.Collections.Array;
using GDict = Aver.Fabric.Collections.Dictionary;

namespace Aver.Fabric;

/// <summary>Name resolution and the local interface list (Godot's IP singleton).</summary>
public static class IP
{
    public enum ResolverStatus { None = 0, Waiting = 1, Done = 2, Error = 3 }

    public enum Type { None = 0, Ipv4 = 1, Ipv6 = 2, Any = 3 }

    public const int ResolverMaxQueries = 256;
    public const int ResolverInvalidId = -1;

    private sealed class Item { public ResolverStatus Status; public List<string> Addresses = new(); }

    private static readonly Dictionary<int, Item> s_items = new();
    private static int s_nextId = 1;

    private static int Family(Type t) => t == Type.Ipv4 ? 4 : t == Type.Ipv6 ? 6 : 0;

    /// <summary>The first address of the name, or an empty string. A numeric address resolves to itself.</summary>
    public static string ResolveHostname(string host, Type ipType = Type.Any)
    {
        var all = ResolveHostnameAddresses(host, ipType);
        return all.Length > 0 ? all[0] : "";
    }

    /// <summary>Every address of the name (IPv4 first when both exist).</summary>
    public static string[] ResolveHostnameAddresses(string host, Type ipType = Type.Any)
    {
        if (string.IsNullOrEmpty(host) || ipType == Type.None || !NetNative.Available) return System.Array.Empty<string>();
        List<string> r = NetNative.Resolve(host, Family(ipType));
        r.Sort((a, b) => a.Contains(':').CompareTo(b.Contains(':')));
        return r.ToArray();
    }

    /// <summary>Resolves at once (there is no worker thread) and keeps the answer under an id.</summary>
    public static int ResolveHostnameQueueItem(string host, Type ipType = Type.Any)
    {
        if (s_items.Count >= ResolverMaxQueries) return ResolverInvalidId;
        int id = s_nextId;
        while (s_items.ContainsKey(id)) id = id % ResolverMaxQueries + 1;
        s_nextId = id % ResolverMaxQueries + 1;
        string[] a = ResolveHostnameAddresses(host, ipType);
        s_items[id] = new Item { Status = a.Length > 0 ? ResolverStatus.Done : ResolverStatus.Error, Addresses = new List<string>(a) };
        return id;
    }

    public static ResolverStatus GetResolveItemStatus(int id) => s_items.TryGetValue(id, out Item? it) ? it.Status : ResolverStatus.None;
    public static string GetResolveItemAddress(int id) => s_items.TryGetValue(id, out Item? it) && it.Addresses.Count > 0 ? it.Addresses[0] : "";
    public static GArray GetResolveItemAddresses(int id)
    {
        var a = new GArray();
        if (s_items.TryGetValue(id, out Item? it)) foreach (string s in it.Addresses) a.Add(s);
        return a;
    }
    public static void EraseResolveItem(int id) => s_items.Remove(id);

    /// <summary>The addresses of every interface that is up, as numeric text.</summary>
    public static string[] GetLocalAddresses() => NetNative.Available ? NetNative.LocalAddresses().ToArray() : System.Array.Empty<string>();

    /// <summary>One entry holding all the local addresses: the engine reports addresses, not adapter names.</summary>
    public static GArray GetLocalInterfaces()
    {
        var list = new GArray();
        string[] addrs = GetLocalAddresses();
        if (addrs.Length == 0) return list;
        var d = new GDict { ["index"] = "0", ["name"] = "local", ["friendly"] = "Local addresses", ["addresses"] = new GArray(addrs.Select(a => (Variant)a)) };
        list.Add(d);
        return list;
    }

    public static void ClearCache(string hostname = "") { }
}

/// <summary>The transport helpers the classes below share.</summary>
internal static class NetUtil
{
    /// <summary>A numeric address for a host name or address; false when it does not resolve or the bytes are not an address.</summary>
    internal static bool Address(string host, out string ip, IP.Type type = IP.Type.Any)
    {
        ip = IP.ResolveHostname(host, type);
        return ip.Length > 0;
    }

    internal static int Family(string ip) => ip.Contains(':') ? 6 : 4;

    internal static bool IsLoopback(string ip) => ip == "::1" || ip.StartsWith("127.", StringComparison.Ordinal);

    internal static string IpText(byte[] buf) { int n = System.Array.IndexOf(buf, (byte)0); return Encoding.UTF8.GetString(buf, 0, n < 0 ? buf.Length : n); }

    /// <summary>The loopback address in the family of <paramref name="ip"/>: where a socket that only talks to loopback binds, so it never opens a wildcard port.</summary>
    internal static string LoopbackFor(string ip) => Family(ip) == 6 ? "::1" : "127.0.0.1";

    internal static readonly System.Diagnostics.Stopwatch Clock = System.Diagnostics.Stopwatch.StartNew();
}

/// <summary>The packet side of Godot's networking: whole messages, one at a time (PacketPeer).</summary>
public abstract class PacketPeer : RefCounted
{
    /// <summary>The largest Variant PutVar will encode (Godot's default, 8 MiB).</summary>
    public int EncodeBufferMaxSize { get; set; } = 8388608;

    private Error _packetError = Error.Ok;

    public abstract int GetAvailablePacketCount();

    /// <summary>Takes the next packet; Error.Unavailable when there is none.</summary>
    internal abstract Error PopPacket(out byte[] data);

    public virtual byte[] GetPacket()
    {
        _packetError = PopPacket(out byte[] d);
        return _packetError == Error.Ok ? d : System.Array.Empty<byte>();
    }

    public Error GetPacketError() => _packetError;

    public abstract Error PutPacket(byte[] buffer);

    public virtual Variant GetVar(bool allowObjects = false)
    {
        byte[] p = GetPacket();
        if (_packetError != Error.Ok) return default;
        _packetError = VariantBinary.Decode(p, allowObjects, out Variant v, out _);
        return _packetError == Error.Ok ? v : default;
    }

    public virtual Error PutVar(Variant var, bool fullObjects = false)
    {
        var o = new List<byte>();
        Error e = VariantBinary.Encode(var, o, fullObjects);
        if (e != Error.Ok) return e;
        if (o.Count > EncodeBufferMaxSize) return Error.OutOfMemory;
        return PutPacket(o.ToArray());
    }
}

/// <summary>Datagrams over UDP (Godot's PacketPeerUDP). Bind to receive; set a destination (or connect) to send.</summary>
public class PacketPeerUDP : PacketPeer
{
    private int _sock;
    private int _family;
    private bool _bound;
    private string _destIp = "";
    private int _destPort;
    private bool _connected;
    private string _packetIp = "";
    private int _packetPort;
    private int _recvBytes = 65536;
    private readonly Queue<(byte[] Data, string Ip, int Port)> _queue = new();
    private UDPServer? _server;     // set for a peer a UDPServer hands out: it shares the server's socket

    private const int MaxQueued = 65536;

    internal static PacketPeerUDP ForServer(UDPServer server, string ip, int port)
    {
        var p = new PacketPeerUDP { _server = server, _destIp = ip, _destPort = port, _connected = true };
        return p;
    }

    internal void Enqueue(byte[] data, string ip, int port) { if (_queue.Count < MaxQueued) _queue.Enqueue((data, ip, port)); }

    private bool Open(int family)
    {
        if (_sock != 0 && _family == family) return true;
        if (_sock != 0) NetNative.aver_net_close(_sock);
        _sock = NetNative.Available ? NetNative.aver_net_udp_open(family) : 0;
        _family = _sock != 0 ? family : 0;
        _bound = false;
        return _sock != 0;
    }

    /// <summary>Binds a local port; "*" is the wildcard (every interface), anything else a numeric address or a name.</summary>
    public Error Bind(int port, string bindAddress = "*", int recvBufSize = 65536)
    {
        if (_server != null || _bound) return Error.AlreadyInUse;
        if (port < 0 || port > 65535) return Error.InvalidParameter;
        if (!NetNative.Available) return Error.Unavailable;
        string ip = bindAddress;
        if (ip != "*" && !NetUtil.Address(bindAddress, out ip)) return Error.InvalidParameter;
        if (!Open(ip == "*" ? 4 : NetUtil.Family(ip))) return Error.CantCreate;
        if (NetNative.aver_net_udp_bind(_sock, ip, port) != 1) { Close(); return Error.AlreadyInUse; }
        _recvBytes = Math.Max(recvBufSize, 1024);
        _bound = true;
        return Error.Ok;
    }

    public void Close()
    {
        if (_server != null) { _server.Release(this); _server = null; _queue.Clear(); _connected = false; return; }
        if (_sock != 0) NetNative.aver_net_close(_sock);
        _sock = 0; _family = 0; _bound = false; _connected = false;
        _queue.Clear();
    }

    public bool IsBound() => _bound || _server != null;

    public int GetLocalPort() => _server != null ? _server.GetLocalPort() : _sock != 0 ? NetNative.aver_net_udp_local_port(_sock) : 0;

    /// <summary>Waits until a packet is queued (up to a minute); Error.Unconfigured when there is no socket.</summary>
    public Error Wait()
    {
        if (_server != null) return Error.Unconfigured;
        if (_sock == 0) return Error.Unconfigured;
        long end = NetUtil.Clock.ElapsedMilliseconds + 60000;
        while (_queue.Count == 0)
        {
            Pump();
            if (_queue.Count > 0) break;
            if (NetUtil.Clock.ElapsedMilliseconds > end) return Error.Timeout;
            NetNative.aver_net_udp_wait(_sock, 10);
        }
        return Error.Ok;
    }

    /// <summary>Gives the peer a default destination; it also keeps only packets from that address (a connected UDP socket).</summary>
    public Error ConnectToHost(string host, int port)
    {
        if (_server != null) return Error.AlreadyInUse;
        Error e = SetDestAddress(host, port);
        if (e != Error.Ok) return e;
        if (!EnsureSocket()) return Error.CantCreate;
        _connected = true;
        return Error.Ok;
    }

    public bool IsSocketConnected() => _connected;

    public Error SetDestAddress(string host, int port)
    {
        if (port < 1 || port > 65535) return Error.InvalidParameter;
        if (!NetUtil.Address(host, out string ip)) return Error.InvalidParameter;
        _destIp = ip;
        _destPort = port;
        return Error.Ok;
    }

    public void SetBroadcastEnabled(bool enabled)
    {
        if (_sock != 0 || EnsureSocket()) NetNative.aver_net_udp_set_broadcast(_sock, enabled ? 1 : 0);
    }

    public Error JoinMulticastGroup(string multicastAddress, string interfaceName) => Error.Unavailable;
    public Error LeaveMulticastGroup(string multicastAddress, string interfaceName) => Error.Unavailable;

    public string GetPacketIP() => _packetIp;
    public int GetPacketPort() => _packetPort;

    // A socket for sending when the script never bound one: loopback destinations go out from the loopback address.
    private bool EnsureSocket()
    {
        if (_server != null) return true;
        if (_sock != 0) return true;
        if (_destIp.Length == 0) return false;
        if (!Open(NetUtil.Family(_destIp))) return false;
        if (NetUtil.IsLoopback(_destIp)) { NetNative.aver_net_udp_bind(_sock, NetUtil.LoopbackFor(_destIp), 0); _bound = true; }
        return true;
    }

    /// <summary>Reads everything the socket holds into the queue.</summary>
    internal void Pump()
    {
        if (_server != null) { _server.Poll(); return; }
        if (_sock == 0) return;
        var buf = new byte[Math.Max(_recvBytes, 2048)];
        var ipBuf = new byte[64];
        while (_queue.Count < MaxQueued)
        {
            int n = NetNative.aver_net_udp_recv(_sock, buf, buf.Length, ipBuf, ipBuf.Length, out int port);
            if (n < 0) break;
            string ip = NetUtil.IpText(ipBuf);
            if (_connected && (ip != _destIp || port != _destPort)) continue;
            var d = new byte[n];
            System.Array.Copy(buf, d, n);
            _queue.Enqueue((d, ip, port));
        }
    }

    public override int GetAvailablePacketCount()
    {
        Pump();
        return _queue.Count;
    }

    internal override Error PopPacket(out byte[] data)
    {
        Pump();
        if (_queue.Count == 0) { data = System.Array.Empty<byte>(); return Error.Unavailable; }
        var p = _queue.Dequeue();
        data = p.Data; _packetIp = p.Ip; _packetPort = p.Port;
        return Error.Ok;
    }

    public override Error PutPacket(byte[] buffer)
    {
        if (_destIp.Length == 0) return Error.Unconfigured;
        if (buffer.Length > 65507) return Error.InvalidParameter;
        if (_server != null) return _server.SendTo(buffer, _destIp, _destPort);
        if (!NetNative.Available) return Error.Unavailable;
        if (!EnsureSocket()) return Error.CantCreate;
        int n = NetNative.aver_net_udp_send(_sock, buffer, buffer.Length, _destIp, _destPort);
        return n == buffer.Length ? Error.Ok : n == NetNative.WouldBlock ? Error.Busy : Error.Failed;
    }

    public override void Free()
    {
        Close();
        base.Free();
    }
}

/// <summary>Accepts UDP "connections": the first datagram from an unknown address makes a PacketPeerUDP, and later datagrams from it go to that peer (Godot's UDPServer).</summary>
public class UDPServer : RefCounted
{
    private int _sock;
    private readonly Dictionary<string, PacketPeerUDP> _peers = new();
    private readonly Queue<PacketPeerUDP> _pending = new();

    public int MaxPendingConnections { get; set; } = 16;

    public Error Listen(int port, string bindAddress = "*")
    {
        if (_sock != 0) return Error.AlreadyInUse;
        if (port < 0 || port > 65535) return Error.InvalidParameter;
        if (!NetNative.Available) return Error.Unavailable;
        string ip = bindAddress;
        if (ip != "*" && !NetUtil.Address(bindAddress, out ip)) return Error.InvalidParameter;
        int sock = NetNative.aver_net_udp_open(ip == "*" ? 4 : NetUtil.Family(ip));
        if (sock == 0) return Error.CantCreate;
        if (NetNative.aver_net_udp_bind(sock, ip, port) != 1) { NetNative.aver_net_close(sock); return Error.AlreadyInUse; }
        _sock = sock;
        return Error.Ok;
    }

    public bool IsListening() => _sock != 0;

    public int GetLocalPort() => _sock != 0 ? NetNative.aver_net_udp_local_port(_sock) : 0;

    /// <summary>Reads the socket: a datagram from a known address joins that peer's queue, from a new one makes a pending connection (dropped past MaxPendingConnections).</summary>
    public Error Poll()
    {
        if (_sock == 0) return Error.Unconfigured;
        var buf = new byte[65536];
        var ipBuf = new byte[64];
        for (int guard = 0; guard < 4096; guard++)
        {
            int n = NetNative.aver_net_udp_recv(_sock, buf, buf.Length, ipBuf, ipBuf.Length, out int port);
            if (n < 0) break;
            string ip = NetUtil.IpText(ipBuf);
            string key = ip + "|" + port;
            var d = new byte[n];
            System.Array.Copy(buf, d, n);
            if (_peers.TryGetValue(key, out PacketPeerUDP? peer)) { peer.Enqueue(d, ip, port); continue; }
            if (_pending.Count >= MaxPendingConnections) continue;
            peer = PacketPeerUDP.ForServer(this, ip, port);
            peer.Enqueue(d, ip, port);
            _peers[key] = peer;
            _pending.Enqueue(peer);
        }
        return Error.Ok;
    }

    public bool IsConnectionAvailable()
    {
        Poll();
        return _pending.Count > 0;
    }

    public PacketPeerUDP? TakeConnection()
    {
        Poll();
        return _pending.Count > 0 ? _pending.Dequeue() : null;
    }

    public void Stop()
    {
        if (_sock != 0) NetNative.aver_net_close(_sock);
        _sock = 0;
        _peers.Clear();
        _pending.Clear();
    }

    internal Error SendTo(byte[] data, string ip, int port)
    {
        if (_sock == 0) return Error.Unconfigured;
        int n = NetNative.aver_net_udp_send(_sock, data, data.Length, ip, port);
        return n == data.Length ? Error.Ok : n == NetNative.WouldBlock ? Error.Busy : Error.Failed;
    }

    internal void Release(PacketPeerUDP peer)
    {
        foreach (var kv in _peers.ToList()) if (ReferenceEquals(kv.Value, peer)) _peers.Remove(kv.Key);
    }

    public override void Free()
    {
        Stop();
        base.Free();
    }
}

/// <summary>A byte stream (Godot's StreamPeer): sized integers, floats, strings and Variants in either byte order over some transport.</summary>
public abstract class StreamPeer : RefCounted
{
    /// <summary>Big-endian integers and floats when true (default little-endian).</summary>
    public bool BigEndian { get; set; }

    /// <summary>One write attempt: how many bytes the transport took (0 when it is full).</summary>
    internal abstract Error WritePartial(byte[] data, int offset, int count, out int written);

    /// <summary>One read attempt: whatever is there now, possibly nothing.</summary>
    internal abstract Error ReadPartial(byte[] buffer, int offset, int count, out int read);

    /// <summary>True for a transport that can deliver more later (a socket); a buffer is complete or not.</summary>
    internal virtual bool CanWait => false;

    internal virtual void Wait(bool forWrite) => Thread.Sleep(1);

    public abstract int GetAvailableBytes();

    public Error PutData(byte[] data)
    {
        int off = 0;
        long end = NetUtil.Clock.ElapsedMilliseconds + 30000;
        while (off < data.Length)
        {
            Error e = WritePartial(data, off, data.Length - off, out int n);
            if (e != Error.Ok) return e;
            off += n;
            if (n == 0)
            {
                if (!CanWait) return Error.Failed;
                if (NetUtil.Clock.ElapsedMilliseconds > end) return Error.Timeout;
                Wait(true);
            }
        }
        return Error.Ok;
    }

    /// <summary>[Error, number of bytes sent]; one attempt.</summary>
    public GArray PutPartialData(byte[] data)
    {
        Error e = WritePartial(data, 0, data.Length, out int n);
        return new GArray { (long)(int)e, (long)n };
    }

    /// <summary>[Error, bytes]: blocks (a socket, up to 30 s) until all of them arrived.</summary>
    public GArray GetData(int bytes)
    {
        var buf = new byte[Math.Max(bytes, 0)];
        Error e = ReadExact(buf, buf.Length, out int got);
        if (got < buf.Length) System.Array.Resize(ref buf, got);
        return new GArray { (long)(int)e, buf };
    }

    /// <summary>[Error, bytes]: what is available now, at most <paramref name="bytes"/>.</summary>
    public GArray GetPartialData(int bytes)
    {
        var buf = new byte[Math.Max(bytes, 0)];
        Error e = ReadPartial(buf, 0, buf.Length, out int got);
        if (got < buf.Length) System.Array.Resize(ref buf, got);
        return new GArray { (long)(int)e, buf };
    }

    internal Error ReadExact(byte[] buf, int count, out int got)
    {
        got = 0;
        long end = NetUtil.Clock.ElapsedMilliseconds + 30000;
        while (got < count)
        {
            Error e = ReadPartial(buf, got, count - got, out int n);
            got += n;
            if (e != Error.Ok) return e;
            if (got >= count) break;
            if (n == 0)
            {
                if (!CanWait) return Error.InvalidParameter;
                if (NetUtil.Clock.ElapsedMilliseconds > end) return Error.Timeout;
                Wait(false);
            }
        }
        return Error.Ok;
    }

    private void Put(Span<byte> le)
    {
        if (BigEndian) le.Reverse();
        PutData(le.ToArray());
    }

    private bool Take(int n, out byte[] b)
    {
        b = new byte[n];
        if (ReadExact(b, n, out _) != Error.Ok) return false;
        if (BigEndian) System.Array.Reverse(b);
        return true;
    }

    public void Put8(sbyte value) => PutData(new[] { (byte)value });
    public void PutU8(byte value) => PutData(new[] { value });
    public void Put16(short value) { Span<byte> b = stackalloc byte[2]; BinaryPrimitives.WriteInt16LittleEndian(b, value); Put(b); }
    public void PutU16(ushort value) { Span<byte> b = stackalloc byte[2]; BinaryPrimitives.WriteUInt16LittleEndian(b, value); Put(b); }
    public void Put32(int value) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteInt32LittleEndian(b, value); Put(b); }
    public void PutU32(uint value) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, value); Put(b); }
    public void Put64(long value) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteInt64LittleEndian(b, value); Put(b); }
    public void PutU64(ulong value) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(b, value); Put(b); }
    public void PutHalf(float value) { Span<byte> b = stackalloc byte[2]; BinaryPrimitives.WriteHalfLittleEndian(b, (Half)value); Put(b); }
    public void PutFloat(float value) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteSingleLittleEndian(b, value); Put(b); }
    public void PutDouble(double value) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteDoubleLittleEndian(b, value); Put(b); }

    /// <summary>A 32-bit length, then the ASCII bytes (anything outside ASCII becomes '?').</summary>
    public void PutString(string value)
    {
        byte[] b = Encoding.ASCII.GetBytes(value ?? "");
        PutU32((uint)b.Length);
        PutData(b);
    }

    public void PutUtf8String(string value)
    {
        byte[] b = Encoding.UTF8.GetBytes(value ?? "");
        PutU32((uint)b.Length);
        PutData(b);
    }

    /// <summary>A 32-bit length, then the encoded Variant (Godot's StreamPeer prefixes it; PacketPeer does not).</summary>
    public Error PutVar(Variant var, bool fullObjects = false)
    {
        var o = new List<byte>();
        Error e = VariantBinary.Encode(var, o, fullObjects);
        if (e != Error.Ok) return e;
        Put32(o.Count);
        return PutData(o.ToArray());
    }

    public sbyte Get8() => Take(1, out byte[] b) ? (sbyte)b[0] : (sbyte)0;
    public byte GetU8() => Take(1, out byte[] b) ? b[0] : (byte)0;
    public short Get16() => Take(2, out byte[] b) ? BinaryPrimitives.ReadInt16LittleEndian(b) : (short)0;
    public ushort GetU16() => Take(2, out byte[] b) ? BinaryPrimitives.ReadUInt16LittleEndian(b) : (ushort)0;
    public int Get32() => Take(4, out byte[] b) ? BinaryPrimitives.ReadInt32LittleEndian(b) : 0;
    public uint GetU32() => Take(4, out byte[] b) ? BinaryPrimitives.ReadUInt32LittleEndian(b) : 0u;
    public long Get64() => Take(8, out byte[] b) ? BinaryPrimitives.ReadInt64LittleEndian(b) : 0L;
    public ulong GetU64() => Take(8, out byte[] b) ? BinaryPrimitives.ReadUInt64LittleEndian(b) : 0UL;
    public float GetHalf() => Take(2, out byte[] b) ? (float)BinaryPrimitives.ReadHalfLittleEndian(b) : 0f;
    public float GetFloat() => Take(4, out byte[] b) ? BinaryPrimitives.ReadSingleLittleEndian(b) : 0f;
    public double GetDouble() => Take(8, out byte[] b) ? BinaryPrimitives.ReadDoubleLittleEndian(b) : 0d;

    /// <summary>With bytes = -1 the length comes first from the stream.</summary>
    public string GetString(int bytes = -1) => ReadText(bytes, Encoding.ASCII);

    public string GetUtf8String(int bytes = -1) => ReadText(bytes, Encoding.UTF8);

    private string ReadText(int bytes, Encoding enc)
    {
        if (bytes < 0) bytes = (int)GetU32();
        if (bytes <= 0 || bytes > (1 << 28)) return "";
        var b = new byte[bytes];
        return ReadExact(b, bytes, out _) == Error.Ok ? enc.GetString(b) : "";
    }

    /// <summary>Reads a length-prefixed Variant; nil when the bytes are not one.</summary>
    public Variant GetVar(bool allowObjects = false)
    {
        int len = Get32();
        if (len <= 0 || len > (1 << 28)) return default;
        var b = new byte[len];
        if (ReadExact(b, len, out _) != Error.Ok) return default;
        return VariantBinary.Decode(b, allowObjects, out Variant v, out _) == Error.Ok ? v : default;
    }
}

/// <summary>A byte buffer with a cursor that is also a StreamPeer (Godot's StreamPeerBuffer): writes land at the cursor and grow the buffer.</summary>
public class StreamPeerBuffer : StreamPeer
{
    private byte[] _data = System.Array.Empty<byte>();
    private int _pos;

    /// <summary>A copy of the buffer.</summary>
    public byte[] Data
    {
        get => (byte[])_data.Clone();
        set { _data = (byte[])(value ?? System.Array.Empty<byte>()).Clone(); _pos = 0; }
    }

    public void Resize(int size)
    {
        if (size < 0) return;
        System.Array.Resize(ref _data, size);
        if (_pos > size) _pos = size;
    }

    public void Clear() { _data = System.Array.Empty<byte>(); _pos = 0; }
    public void Seek(int position) { if (position >= 0 && position <= _data.Length) _pos = position; }
    public int GetSize() => _data.Length;
    public int GetPosition() => _pos;

    public StreamPeerBuffer Duplicate() => new StreamPeerBuffer { _data = (byte[])_data.Clone(), _pos = _pos, BigEndian = BigEndian };

    public override int GetAvailableBytes() => _data.Length - _pos;

    internal override Error WritePartial(byte[] data, int offset, int count, out int written)
    {
        if (_pos + count > _data.Length) System.Array.Resize(ref _data, _pos + count);
        System.Array.Copy(data, offset, _data, _pos, count);
        _pos += count;
        written = count;
        return Error.Ok;
    }

    internal override Error ReadPartial(byte[] buffer, int offset, int count, out int read)
    {
        read = Math.Max(0, Math.Min(count, _data.Length - _pos));
        System.Array.Copy(_data, _pos, buffer, offset, read);
        _pos += read;
        return Error.Ok;
    }
}

/// <summary>A TCP connection (Godot's StreamPeerTCP). Call Poll() to move a connect along and to notice a close.</summary>
public class StreamPeerTCP : StreamPeer
{
    public enum Status { None = 0, Connecting = 1, Connected = 2, Error = 3 }

    private int _sock;
    private Status _status = Status.None;
    private string _host = "";
    private int _port;
    private byte[] _in = new byte[4096];
    private int _inStart, _inEnd;
    private bool _remoteClosed;

    internal static StreamPeerTCP Accepted(int sock, string ip, int port) => new() { _sock = sock, _status = Status.Connected, _host = ip, _port = port };

    public Error ConnectToHost(string host, int port)
    {
        if (_status != Status.None) return Error.AlreadyInUse;
        if (port < 1 || port > 65535) return Error.InvalidParameter;
        if (!NetNative.Available) return Error.Unavailable;
        if (!NetUtil.Address(host, out string ip)) return Error.CantResolve;
        _sock = NetNative.aver_net_tcp_connect(ip, port);
        if (_sock == 0) { _status = Status.Error; return Error.CantConnect; }
        _host = ip; _port = port;
        _status = Status.Connecting;
        _remoteClosed = false;
        _inStart = _inEnd = 0;
        return Error.Ok;
    }

    /// <summary>Binding the local side of a TCP client is not available.</summary>
    public Error Bind(int port, string host = "*") => Error.Unavailable;

    public Error Poll()
    {
        if (_sock == 0) return Error.Ok;
        if (_status == Status.Connecting)
        {
            int s = NetNative.aver_net_tcp_status(_sock);
            if (s == 1) _status = Status.Connected;
            else if (s < 0) { Drop(); _status = Status.Error; return Error.Failed; }
        }
        if (_status == Status.Connected) Fill();
        return Error.Ok;
    }

    public Status GetStatus() => _status;

    public string GetConnectedHost() => _status == Status.None ? "" : _host;
    public int GetConnectedPort() => _status == Status.None ? 0 : _port;
    public int GetLocalPort() => _sock != 0 ? NetNative.aver_net_tcp_local_port(_sock) : 0;

    public void SetNoDelay(bool enabled) { if (_sock != 0) NetNative.aver_net_tcp_set_nodelay(_sock, enabled ? 1 : 0); }

    public void DisconnectFromHost()
    {
        Drop();
        _status = Status.None;
        _host = "";
        _port = 0;
    }

    private void Drop()
    {
        if (_sock != 0) NetNative.aver_net_close(_sock);
        _sock = 0;
    }

    // Moves what the socket holds into the read-ahead buffer; notes an orderly close. Buffered bytes stay readable after the close.
    private void Fill()
    {
        if (_sock == 0 || _remoteClosed) return;
        var tmp = new byte[16384];
        for (int guard = 0; guard < 64; guard++)
        {
            int n = NetNative.aver_net_tcp_recv(_sock, tmp, tmp.Length);
            if (n > 0)
            {
                if (_inEnd + n > _in.Length)
                {
                    if (_inStart > 0) { System.Array.Copy(_in, _inStart, _in, 0, _inEnd - _inStart); _inEnd -= _inStart; _inStart = 0; }
                    if (_inEnd + n > _in.Length) System.Array.Resize(ref _in, Math.Max(_in.Length * 2, _inEnd + n));
                }
                System.Array.Copy(tmp, 0, _in, _inEnd, n);
                _inEnd += n;
                continue;
            }
            if (n == NetNative.WouldBlock) return;
            _remoteClosed = true;                       // closed in order, or the socket failed
            Drop();
            if (n == NetNative.Failed) _status = Status.Error;
            else if (_inEnd == _inStart) _status = Status.None;      // with bytes still buffered the status follows once they are read
            return;
        }
    }

    public override int GetAvailableBytes()
    {
        if (_status == Status.Connecting) Poll(); else if (_status == Status.Connected) Fill();
        return _inEnd - _inStart;
    }

    internal override bool CanWait => true;

    internal override void Wait(bool forWrite)
    {
        if (_sock != 0) NetNative.aver_net_tcp_wait(_sock, forWrite ? 1 : 0, 5);
        else Thread.Sleep(1);
    }

    internal override Error WritePartial(byte[] data, int offset, int count, out int written)
    {
        written = 0;
        if (_status == Status.Connecting) Poll();
        if (_status != Status.Connected || _sock == 0) return Error.Unconfigured;
        byte[] chunk = data;
        if (offset != 0) { chunk = new byte[count]; System.Array.Copy(data, offset, chunk, 0, count); }
        int n = NetNative.aver_net_tcp_send(_sock, chunk, Math.Min(count, 1 << 20));
        if (n >= 0) { written = n; return Error.Ok; }
        if (n == NetNative.WouldBlock) return Error.Ok;
        _status = Status.Error;
        Drop();
        return Error.ConnectionError;
    }

    internal override Error ReadPartial(byte[] buffer, int offset, int count, out int read)
    {
        read = 0;
        if (_status == Status.Connecting) Poll();
        Fill();
        int have = _inEnd - _inStart;
        if (have == 0)
        {
            return _remoteClosed || _status == Status.Error || _status == Status.None && _sock == 0 ? Error.ConnectionError : Error.Ok;
        }
        read = Math.Min(count, have);
        System.Array.Copy(_in, _inStart, buffer, offset, read);
        _inStart += read;
        if (_inStart == _inEnd) { _inStart = _inEnd = 0; if (_remoteClosed && _status == Status.Connected) _status = Status.None; }
        return Error.Ok;
    }

    public override void Free()
    {
        DisconnectFromHost();
        base.Free();
    }
}

/// <summary>Listens for TCP connections (Godot's TCPServer).</summary>
public class TCPServer : RefCounted
{
    private int _sock;

    public Error Listen(int port, string bindAddress = "*")
    {
        if (_sock != 0) return Error.AlreadyInUse;
        if (port < 0 || port > 65535) return Error.InvalidParameter;
        if (!NetNative.Available) return Error.Unavailable;
        string ip = bindAddress;
        if (ip != "*" && !NetUtil.Address(bindAddress, out ip)) return Error.InvalidParameter;
        int s = NetNative.aver_net_tcp_listen(ip, port, 16);
        if (s == 0) return Error.AlreadyInUse;
        _sock = s;
        return Error.Ok;
    }

    public bool IsListening() => _sock != 0;
    public int GetLocalPort() => _sock != 0 ? NetNative.aver_net_tcp_listener_port(_sock) : 0;
    public bool IsConnectionAvailable() => _sock != 0 && NetNative.aver_net_tcp_listener_wait(_sock, 0) == 1;

    public StreamPeerTCP? TakeConnection()
    {
        if (_sock == 0) return null;
        int c = NetNative.aver_net_tcp_accept(_sock);
        if (c == 0) return null;
        var ip = new byte[64];
        NetNative.aver_net_tcp_remote(c, ip, ip.Length, out int port);
        return StreamPeerTCP.Accepted(c, NetUtil.IpText(ip), port);
    }

    public void Stop()
    {
        if (_sock != 0) NetNative.aver_net_close(_sock);
        _sock = 0;
    }

    public override void Free()
    {
        Stop();
        base.Free();
    }
}
