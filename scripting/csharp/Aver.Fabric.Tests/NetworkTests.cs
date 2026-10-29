// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET1: Godot's networking classes over Aver.Net.Abi. The Variant binary encoding and StreamPeerBuffer need no library; the socket classes, ENetConnection
// and the multiplayer peers run two ends in one process over LOOPBACK ONLY (every bind is 127.0.0.1, no wildcard, no name but "localhost"). The protocol itself is
// tested on a simulated lossy network by the C++ suite NetProtocolTest; what is checked here is the Godot surface on top: byte-exact Variant encodings, endianness,
// method behaviour, signals, transfer modes, target peers and the multiplayer state machine. Without build/bin/Aver.Net.Abi the native tests are skipped.

using Aver.Fabric;
using GArray = Aver.Fabric.Collections.Array;
using GDict = Aver.Fabric.Collections.Dictionary;
using IO = System.IO;

static class NetworkTests
{
    private static bool s_native;

    public static int RunAll()
    {
        s_native = EnsureNative();
        return T.Suite(() =>
        {
            T.Run("Variant binary encoding: byte-exact against Godot's layout", EncodingBytes);
            T.Run("Variant binary encoding: every type round-trips", EncodingRoundTrip);
            T.Run("Variant binary encoding: objects, callables and hostile bytes", EncodingEdges);
            T.Run("StreamPeerBuffer: integers, floats, strings and Variants in both byte orders", BufferPrimitives);
            T.Run("StreamPeerBuffer: cursor, resize, short reads, duplicate", BufferCursor);
            if (!s_native)
            {
                Console.WriteLine("Test: networking (SKIP: build/bin/Aver.Net.Abi was not found; build the Aver.Net.Abi target)");
                return;
            }
            T.Run("IP: numeric addresses, localhost, the resolver queue, local addresses", IpBasics);
            T.Run("PacketPeerUDP: bind, send, receive, put_var and get_var, connected filtering", UdpPeers);
            T.Run("PacketPeerUDP: errors and state", UdpErrors);
            T.Run("UDPServer: pending connections, routing by address, replies, the pending limit", UdpServerRouting);
            T.Run("StreamPeerTCP and TCPServer: connect, integers in both orders, strings, Variants", TcpBasics);
            T.Run("StreamPeerTCP: a megabyte in partial writes, close detection, connect failure", TcpBulk);
            T.Run("ENetConnection and ENetPacketPeer: connect, events, channels, flags, statistics, disconnect", EnetBasics);
            T.Run("ENetConnection: broadcast, refusing connections, bandwidth call, peer list", EnetMore);
            T.Run("ENetMultiplayerPeer: server and client connect, ids, signals, status", MultiplayerConnect);
            T.Run("ENetMultiplayerPeer: transfer modes, channels, target peers, packet sender", MultiplayerPackets);
            T.Run("ENetMultiplayerPeer: put_var / get_var, big packets, disconnect_peer, close", MultiplayerVarsAndClose);
            T.Run("ENetMultiplayerPeer: limits, refusing, failed connect, argument errors", MultiplayerLimits);
            T.Run("OfflineMultiplayerPeer", Offline);
        });
    }

    // ---- the native library ----

    private static string LibFile() =>
        OperatingSystem.IsWindows() ? "Aver.Net.Abi.dll" : OperatingSystem.IsMacOS() ? "libAver.Net.Abi.dylib" : "libAver.Net.Abi.so";

    private static string? FindBin(string file)
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string bin = IO.Path.Combine(d.FullName, "build", "bin");
            if (File.Exists(IO.Path.Combine(bin, file))) return bin;
        }
        return null;
    }

    internal static bool EnsureNative()
    {
        string? bin = FindBin(LibFile());
        if (bin != null)
        {
            string beside = IO.Path.Combine(AppContext.BaseDirectory, LibFile());
            try { if (!File.Exists(beside) || File.GetLastWriteTimeUtc(beside) < File.GetLastWriteTimeUtc(IO.Path.Combine(bin, LibFile()))) File.Copy(IO.Path.Combine(bin, LibFile()), beside, true); }
            catch (IOException) { /* a copy from an earlier run is in use */ }
            NetNative.Reprobe();
        }
        return NetNative.Available;
    }

    private static bool Until(Func<bool> done, int ms = 6000, Action? pump = null)
    {
        long end = System.Environment.TickCount64 + ms;
        while (System.Environment.TickCount64 < end)
        {
            pump?.Invoke();
            if (done()) return true;
            Thread.Sleep(1);
        }
        pump?.Invoke();
        return done();
    }

    private static byte[] Bytes(params int[] b) => b.Select(x => (byte)x).ToArray();

    private static byte[] Enc(Variant v)
    {
        var o = new List<byte>();
        return VariantBinary.Encode(v, o) == Error.Ok ? o.ToArray() : System.Array.Empty<byte>();
    }

    // ---- Variant encoding ----

    private static void EncodingBytes()
    {
        T.CheckSeq("nil", Bytes(0, 0, 0, 0), Enc(default));
        T.CheckSeq("true", Bytes(1, 0, 0, 0, 1, 0, 0, 0), Enc(true));
        T.CheckSeq("int 5", Bytes(2, 0, 0, 0, 5, 0, 0, 0), Enc(5));
        T.CheckSeq("int -1 is 32 bits", Bytes(2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF), Enc(-1));
        T.CheckSeq("int 2^40 carries the 64-bit flag", Bytes(2, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0), Enc(1L << 40));
        T.CheckSeq("float 1.5 fits 32 bits", Bytes(3, 0, 0, 0, 0, 0, 0xC0, 0x3F), Enc(1.5));
        T.CheckSeq("float 0.1 does not, so it is 64 bits", Bytes(3, 0, 1, 0, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F), Enc(0.1));
        T.CheckSeq("string \"hi\" is padded to 4", Bytes(4, 0, 0, 0, 2, 0, 0, 0, 0x68, 0x69, 0, 0), Enc("hi"));
        T.CheckSeq("string \"abcd\" needs no padding", Bytes(4, 0, 0, 0, 4, 0, 0, 0, 0x61, 0x62, 0x63, 0x64), Enc("abcd"));
        T.CheckSeq("empty string", Bytes(4, 0, 0, 0, 0, 0, 0, 0), Enc(""));
        T.CheckSeq("Vector2(1, 2)", Bytes(5, 0, 0, 0, 0, 0, 0x80, 0x3F, 0, 0, 0, 0x40), Enc(new Vector2(1, 2)));
        T.CheckSeq("Vector2i(1, -2)", Bytes(6, 0, 0, 0, 1, 0, 0, 0, 0xFE, 0xFF, 0xFF, 0xFF), Enc(new Vector2I(1, -2)));
        T.CheckSeq("Array [1, \"a\"]", Bytes(0x1C, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0x61, 0, 0, 0), Enc(new GArray { 1, "a" }));
        T.CheckSeq("Dictionary {\"k\": 7}", Bytes(0x1B, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0x6B, 0, 0, 0, 2, 0, 0, 0, 7, 0, 0, 0), Enc(new GDict { ["k"] = 7 }));
        T.CheckSeq("PackedByteArray [1, 2, 3]", Bytes(0x1D, 0, 0, 0, 3, 0, 0, 0, 1, 2, 3, 0), Enc(new byte[] { 1, 2, 3 }));
        T.CheckSeq("PackedInt32Array [1, -1]", Bytes(0x1E, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF), Enc(new[] { 1, -1 }));
        T.CheckSeq("Color(1, 0, 0, 1)", Bytes(0x14, 0, 0, 0, 0, 0, 0x80, 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x3F), Enc(new Color(1, 0, 0, 1)));
        T.CheckSeq("NodePath \"/a/b:c\" in the new format", Bytes(0x16, 0, 0, 0, 2, 0, 0, 0x80, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x61, 0, 0, 0, 1, 0, 0, 0, 0x62, 0, 0, 0, 1, 0, 0, 0, 0x63, 0, 0, 0),
            Enc(new NodePath("/a/b:c")));
        T.CheckSeq("StringName", Bytes(0x15, 0, 0, 0, 1, 0, 0, 0, 0x78, 0, 0, 0), Enc(new StringName("x")));
        T.CheckSeq("Rid", Bytes(0x17, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0), Enc(new Rid(9)));
        T.CheckSeq("GD.VarToBytes is the same bytes", Bytes(2, 0, 0, 0, 5, 0, 0, 0), GD.VarToBytes(5));
    }

    private static Variant RoundTrip(Variant v)
    {
        byte[] b = Enc(v);
        Error e = VariantBinary.Decode(b, false, out Variant r, out int used);
        T.Check($"  {v.VariantType} decodes cleanly and consumes every byte", e == Error.Ok && used == b.Length, $"error {e}, used {used} of {b.Length}");
        return r;
    }

    private static void EncodingRoundTrip()
    {
        T.Check("nil", RoundTrip(default).IsNil);
        T.Check("bool", RoundTrip(true).AsBool() && !RoundTrip(false).AsBool());
        T.Check("ints, 32 and 64 bit", RoundTrip(123456).AsInt64() == 123456 && RoundTrip(-9876543210L).AsInt64() == -9876543210L && RoundTrip(long.MinValue).AsInt64() == long.MinValue);
        T.Check("floats, exact and not", RoundTrip(0.25).AsDouble() == 0.25 && RoundTrip(Math.PI).AsDouble() == Math.PI && double.IsNaN(RoundTrip(double.NaN).AsDouble()));
        T.Check("strings with non-ASCII text", RoundTrip("héllo wörld ✓").AsString() == "héllo wörld ✓");
        T.Check("string name", RoundTrip(new StringName("sn")).AsString() == "sn");
        T.Check("vector2 / vector2i", RoundTrip(new Vector2(1.5f, -2)).AsVector2() == new Vector2(1.5f, -2) && RoundTrip(new Vector2I(3, -4)).AsVector2I() == new Vector2I(3, -4));
        T.Check("rect2 / rect2i", RoundTrip(new Rect2(1, 2, 3, 4)).AsRect2() == new Rect2(1, 2, 3, 4) && RoundTrip(new Rect2I(1, 2, 3, 4)).AsRect2I() == new Rect2I(1, 2, 3, 4));
        T.Check("vector3 / vector3i", RoundTrip(new Vector3(1, 2, 3)).AsVector3() == new Vector3(1, 2, 3) && RoundTrip(new Vector3I(1, 2, 3)).AsVector3I() == new Vector3I(1, 2, 3));
        T.Check("vector4 / vector4i", RoundTrip(new Vector4(1, 2, 3, 4)).AsVector4() == new Vector4(1, 2, 3, 4) && RoundTrip(new Vector4I(1, 2, 3, 4)).AsVector4I() == new Vector4I(1, 2, 3, 4));
        var t2 = new Transform2D(new Vector2(1, 2), new Vector2(3, 4), new Vector2(5, 6));
        T.Check("transform2d", RoundTrip(t2).AsTransform2D() == t2);
        T.Check("plane / quaternion", RoundTrip(new Plane(0, 1, 0, 3)).AsPlane() == new Plane(0, 1, 0, 3) && RoundTrip(new Quaternion(0.1f, 0.2f, 0.3f, 0.9f)).AsQuaternion() == new Quaternion(0.1f, 0.2f, 0.3f, 0.9f));
        T.Check("aabb", RoundTrip(new Aabb(new Vector3(1, 2, 3), new Vector3(4, 5, 6))).AsAabb() == new Aabb(new Vector3(1, 2, 3), new Vector3(4, 5, 6)));
        var basis = new Basis { Row0 = new Vector3(1, 2, 3), Row1 = new Vector3(4, 5, 6), Row2 = new Vector3(7, 8, 9) };
        T.Check("basis keeps its rows", RoundTrip(basis).AsBasis() == basis);
        var xf = new Transform3D(basis, new Vector3(10, 11, 12));
        T.Check("transform3d", RoundTrip(xf).AsTransform3D() == xf);
        var proj = new Projection(new Vector4(1, 2, 3, 4), new Vector4(5, 6, 7, 8), new Vector4(9, 10, 11, 12), new Vector4(13, 14, 15, 16));
        T.Check("projection", RoundTrip(proj).AsProjection() == proj);
        T.Check("color", RoundTrip(new Color(0.1f, 0.2f, 0.3f, 0.4f)).AsColor() == new Color(0.1f, 0.2f, 0.3f, 0.4f));
        NodePath np = RoundTrip(new NodePath("/root/Main/Player:position:x")).AsNodePath();
        T.Check("node path: names, subnames and absoluteness", np.IsAbsolute() && np.GetNameCount() == 3 && np.GetSubnameCount() == 2 && np.ToString() == "/root/Main/Player:position:x");
        T.Check("relative node path", !RoundTrip(new NodePath("A/B")).AsNodePath().IsAbsolute() && RoundTrip(new NodePath("A/B")).AsNodePath().ToString() == "A/B");
        T.Check("rid", RoundTrip(new Rid(123456789012UL)).AsRid().Id == 123456789012UL);

        var arr = new GArray { 1, "two", 3.5, new Vector2(1, 1), new GArray { 7, 8 }, default(Variant) };
        GArray ra = RoundTrip(arr).AsGodotArray();
        T.Check("array, nested", ra.Count == 6 && ra[0].AsInt64() == 1 && ra[1].AsString() == "two" && ra[2].AsDouble() == 3.5 && ra[3].AsVector2() == new Vector2(1, 1)
            && ra[4].AsGodotArray().Count == 2 && ra[4].AsGodotArray()[1].AsInt64() == 8 && ra[5].IsNil);
        var dict = new GDict { ["name"] = "x", [3] = new GDict { ["deep"] = true }, [new Vector2I(1, 2)] = 9 };
        GDict rd = RoundTrip(dict).AsGodotDictionary();
        T.Check("dictionary with mixed keys, nested", rd.Count == 3 && rd["name"].AsString() == "x" && rd[3].AsGodotDictionary()["deep"].AsBool() && rd[new Vector2I(1, 2)].AsInt64() == 9);

        T.Check("packed byte / int32 / int64", RoundTrip(new byte[] { 1, 2, 3, 4, 5 }).AsByteArray().SequenceEqual(new byte[] { 1, 2, 3, 4, 5 })
            && RoundTrip(new[] { 1, -2 }).AsInt32Array().SequenceEqual(new[] { 1, -2 }) && RoundTrip(new[] { 1L << 40, -5L }).AsInt64Array().SequenceEqual(new[] { 1L << 40, -5L }));
        T.Check("packed float32 / float64", RoundTrip(new[] { 1.5f, -2f }).AsFloat32Array().SequenceEqual(new[] { 1.5f, -2f }) && RoundTrip(new[] { 0.1, 2.0 }).AsFloat64Array().SequenceEqual(new[] { 0.1, 2.0 }));
        T.Check("packed strings", RoundTrip(new[] { "a", "", "ccc" }).AsStringArray().SequenceEqual(new[] { "a", "", "ccc" }));
        T.Check("packed vector2 / vector3 / color / vector4",
            RoundTrip(new[] { new Vector2(1, 2) }).AsVector2Array()[0] == new Vector2(1, 2) && RoundTrip(new[] { new Vector3(1, 2, 3) }).AsVector3Array()[0] == new Vector3(1, 2, 3)
            && RoundTrip(new[] { new Color(1, 0, 0, 1) }).AsColorArray()[0] == new Color(1, 0, 0, 1) && RoundTrip(new[] { new Vector4(1, 2, 3, 4) }).AsVector4Array()[0] == new Vector4(1, 2, 3, 4));
        T.Check("empty packed arrays", RoundTrip(System.Array.Empty<byte>()).AsByteArray().Length == 0 && RoundTrip(System.Array.Empty<string>()).AsStringArray().Length == 0);
        T.Check("GD.BytesToVar", GD.BytesToVar(GD.VarToBytes(new GArray { 1, 2 })).AsGodotArray().Count == 2 && GD.BytesToVar(new byte[] { 1, 2 }).IsNil);
    }

    private static void EncodingEdges()
    {
        var obj = new RandomNumberGenerator();
        byte[] b = Enc(obj);
        T.Check("an object travels as its instance id (flag 1<<16, 8 bytes)", b.Length == 12 && b[0] == 24 && b[2] == 1);
        Error e = VariantBinary.Decode(b, false, out Variant r, out _);
        T.Check("and decodes to an EncodedObjectAsId carrying that id", e == Error.Ok && r.AsGodotObject() is EncodedObjectAsId eo && eo.ObjectId == obj.GetInstanceId());
        var o = new List<byte>();
        T.Check("full objects cannot be encoded", VariantBinary.Encode(obj, o, true) == Error.Unavailable);
        T.Check("GD.VarToBytesWithObjects answers empty for an object", GD.VarToBytesWithObjects(obj).Length == 0);
        // a full-object header (no as-id flag) is refused, with or without permission
        byte[] full = Bytes(24, 0, 0, 0, 0, 0, 0, 0);
        T.Check("a full object is refused without allow_objects", VariantBinary.Decode(full, false, out _, out _) == Error.Unauthorized);
        T.Check("and not available with it", VariantBinary.Decode(full, true, out _, out _) == Error.Unavailable);
        T.Check("callable and signal encode as headers and decode empty", Enc(new Callable()).Length == 4 && VariantBinary.Decode(Enc(new Callable()), false, out Variant c, out int cu) == Error.Ok && cu == 4 && c.VariantType == Variant.Type.Callable);

        T.Check("empty input", VariantBinary.Decode(System.Array.Empty<byte>(), false, out _, out _) == Error.InvalidData);
        T.Check("a type past the table", VariantBinary.Decode(Bytes(0x63, 0, 0, 0), false, out _, out _) == Error.InvalidData);
        byte[] good = Enc(new GArray { "hello world", new Vector3(1, 2, 3) });
        bool allSafe = true;
        for (int cut = 0; cut < good.Length; cut++) allSafe &= VariantBinary.Decode(good.AsSpan(0, cut), false, out _, out _) != Error.Ok;
        T.Check("every truncation of a valid encoding is an error, never an exception", allSafe);
        T.Check("a string claiming more bytes than exist", VariantBinary.Decode(Bytes(4, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F), false, out _, out _) == Error.InvalidData);
        T.Check("an array claiming 2 billion elements", VariantBinary.Decode(Bytes(0x1C, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F), false, out _, out _) == Error.InvalidData);
        T.Check("a packed array claiming more than its bytes", VariantBinary.Decode(Bytes(0x20, 0, 0, 0, 0x10, 0, 0, 0, 1, 2, 3, 4), false, out _, out _) == Error.InvalidData);
        // nesting deeper than the limit
        var deep = new List<byte>();
        for (int i = 0; i < 400; i++) { deep.AddRange(Bytes(0x1C, 0, 0, 0, 1, 0, 0, 0)); }
        deep.AddRange(Bytes(0, 0, 0, 0));
        T.Check("400 nested arrays are refused as too deep", VariantBinary.Decode(deep.ToArray(), false, out _, out _) == Error.InvalidData);
        var rng = new Random(5);
        bool noThrow = true;
        for (int i = 0; i < 3000 && noThrow; i++)
        {
            var junk = new byte[rng.Next(0, 80)];
            rng.NextBytes(junk);
            if (junk.Length >= 4) { junk[0] = (byte)rng.Next(0, 40); junk[1] = 0; junk[3] = 0; }
            try { VariantBinary.Decode(junk, false, out _, out _); } catch (Exception) { noThrow = false; }
        }
        T.Check("3000 random byte strings never throw", noThrow);
        // typed containers (Godot 4.4) are read and their type header skipped
        byte[] typed = Bytes(0x1C, 0, 1, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 7, 0, 0, 0);
        Error te = VariantBinary.Decode(typed, false, out Variant tv, out _);
        T.Check("a typed array (builtin element type int) decodes as an array", te == Error.Ok && tv.AsGodotArray().Count == 1 && tv.AsGodotArray()[0].AsInt64() == 7);
    }

    // ---- StreamPeerBuffer ----

    private static void BufferPrimitives()
    {
        foreach (bool big in new[] { false, true })
        {
            var b = new StreamPeerBuffer { BigEndian = big };
            b.Put8(-5); b.PutU8(200); b.Put16(-1234); b.PutU16(54321); b.Put32(-123456789); b.PutU32(4000000000u); b.Put64(-1234567890123L); b.PutU64(18000000000000000000UL);
            b.PutHalf(1.5f); b.PutFloat(3.25f); b.PutDouble(Math.PI); b.PutString("ascii"); b.PutUtf8String("üñí"); b.PutVar(new GArray { 1, "x" });
            b.Seek(0);
            string tag = big ? "big-endian" : "little-endian";
            T.Check($"{tag}: integers", b.Get8() == -5 && b.GetU8() == 200 && b.Get16() == -1234 && b.GetU16() == 54321 && b.Get32() == -123456789 && b.GetU32() == 4000000000u
                && b.Get64() == -1234567890123L && b.GetU64() == 18000000000000000000UL);
            T.Check($"{tag}: half, float, double", b.GetHalf() == 1.5f && b.GetFloat() == 3.25f && b.GetDouble() == Math.PI);
            T.Check($"{tag}: strings and a length-prefixed Variant", b.GetString() == "ascii" && b.GetUtf8String() == "üñí" && b.GetVar().AsGodotArray().Count == 2);
            T.Check($"{tag}: the cursor is at the end", b.GetAvailableBytes() == 0);
        }
        var le = new StreamPeerBuffer();
        le.Put32(0x01020304);
        T.CheckSeq("little-endian bytes", Bytes(4, 3, 2, 1), le.Data);
        var be = new StreamPeerBuffer { BigEndian = true };
        be.Put32(0x01020304);
        T.CheckSeq("big-endian bytes", Bytes(1, 2, 3, 4), be.Data);
        var s = new StreamPeerBuffer();
        s.PutString("ab");
        T.CheckSeq("a string is a 32-bit length then the bytes", Bytes(2, 0, 0, 0, 0x61, 0x62), s.Data);
        var v = new StreamPeerBuffer();
        v.PutVar(5);
        T.CheckSeq("put_var on a stream is length-prefixed", Bytes(8, 0, 0, 0, 2, 0, 0, 0, 5, 0, 0, 0), v.Data);
        var half = new StreamPeerBuffer();
        half.PutHalf(1f);
        T.CheckSeq("half float 1.0 is 0x3C00", Bytes(0x00, 0x3C), half.Data);
    }

    private static void BufferCursor()
    {
        var b = new StreamPeerBuffer();
        b.PutData(new byte[] { 1, 2, 3, 4, 5 });
        T.Check("size and position after a write", b.GetSize() == 5 && b.GetPosition() == 5);
        b.Seek(1);
        b.PutData(new byte[] { 9, 9 });
        T.CheckSeq("a write at the cursor overwrites", new byte[] { 1, 9, 9, 4, 5 }, b.Data);
        b.Seek(4);
        b.PutData(new byte[] { 7, 7, 7 });
        T.CheckSeq("a write past the end grows the buffer", new byte[] { 1, 9, 9, 4, 7, 7, 7 }, b.Data);
        b.Seek(0);
        GArray r = b.GetData(3);
        T.Check("get_data returns [Error, bytes]", r[0].AsInt64() == 0 && r[1].AsByteArray().SequenceEqual(new byte[] { 1, 9, 9 }));
        GArray short1 = b.GetData(100);
        T.Check("asking for more than is there is an error with what was there", short1[0].AsInt64() == (long)Error.InvalidParameter && short1[1].AsByteArray().Length == 4);
        b.Seek(2);
        GArray part = b.GetPartialData(100);
        T.Check("get_partial_data takes what is there and reports no error", part[0].AsInt64() == 0 && part[1].AsByteArray().Length == 5);
        b.Resize(3);
        T.Check("resize cuts", b.GetSize() == 3 && b.GetPosition() == 3);
        b.Seek(99);
        T.Check("seek out of range is ignored", b.GetPosition() == 3);
        StreamPeerBuffer copy = b.Duplicate();
        copy.Seek(0); copy.PutU8(0xAA);
        T.Check("duplicate is independent", b.Data[0] == 1 && copy.Data[0] == 0xAA);
        b.Data = new byte[] { 5, 6 };
        T.Check("assigning Data resets the cursor", b.GetPosition() == 0 && b.GetU8() == 5);
        b.Clear();
        T.Check("clear", b.GetSize() == 0 && b.GetPosition() == 0 && b.GetU8() == 0);
        GArray p = new StreamPeerBuffer().PutPartialData(new byte[] { 1, 2 });
        T.Check("put_partial_data returns [Error, sent]", p[0].AsInt64() == 0 && p[1].AsInt64() == 2);
    }

    // ---- IP ----

    private static void IpBasics()
    {
        T.Check("a numeric IPv4 address resolves to itself", IP.ResolveHostname("127.0.0.1") == "127.0.0.1");
        T.Check("a numeric IPv6 address too", IP.ResolveHostname("::1") == "::1");
        T.Check("an IPv4 address is not IPv6", IP.ResolveHostname("127.0.0.1", IP.Type.Ipv6) == "");
        T.Check("localhost resolves to loopback IPv4", IP.ResolveHostname("localhost", IP.Type.Ipv4).StartsWith("127."));
        T.Check("a malformed name or empty text does not resolve", IP.ResolveHostname("") == "" && IP.ResolveHostname("no spaces allowed here") == "");
        T.Check("addresses list", IP.ResolveHostnameAddresses("localhost", IP.Type.Ipv4).Length >= 1);
        int id = IP.ResolveHostnameQueueItem("127.0.0.1");
        T.Check("the resolver queue answers at once", id != IP.ResolverInvalidId && IP.GetResolveItemStatus(id) == IP.ResolverStatus.Done && IP.GetResolveItemAddress(id) == "127.0.0.1" && IP.GetResolveItemAddresses(id).Count == 1);
        IP.EraseResolveItem(id);
        T.Check("an erased item is gone", IP.GetResolveItemStatus(id) == IP.ResolverStatus.None);
        int bad = IP.ResolveHostnameQueueItem("");
        T.Check("a failed item reports Error", IP.GetResolveItemStatus(bad) == IP.ResolverStatus.Error);
        IP.EraseResolveItem(bad);
        T.Check("local addresses include loopback", IP.GetLocalAddresses().Contains("127.0.0.1"));
        T.Check("local interfaces list the addresses", IP.GetLocalInterfaces().Count == 1);
    }

    // ---- UDP ----

    private static void UdpPeers()
    {
        var a = new PacketPeerUDP();
        var b = new PacketPeerUDP();
        T.Check("bind a", a.Bind(0, "127.0.0.1") == Error.Ok && a.IsBound());
        T.Check("bind b", b.Bind(0, "127.0.0.1") == Error.Ok);
        T.Check("each has a port", a.GetLocalPort() > 0 && b.GetLocalPort() > 0 && a.GetLocalPort() != b.GetLocalPort());
        T.Check("nothing queued yet", b.GetAvailablePacketCount() == 0);
        T.Check("put_packet without a destination is Unconfigured", a.PutPacket(new byte[] { 1 }) == Error.Unconfigured);
        T.Check("set_dest_address", a.SetDestAddress("127.0.0.1", b.GetLocalPort()) == Error.Ok);
        T.Check("put_packet", a.PutPacket(new byte[] { 1, 2, 3 }) == Error.Ok);
        T.Check("wait returns when a packet arrives", b.Wait() == Error.Ok);
        T.Check("one packet queued", b.GetAvailablePacketCount() == 1);
        byte[] p = b.GetPacket();
        T.Check("get_packet: bytes, sender address and port", p.SequenceEqual(new byte[] { 1, 2, 3 }) && b.GetPacketIP() == "127.0.0.1" && b.GetPacketPort() == a.GetLocalPort() && b.GetPacketError() == Error.Ok);
        T.Check("an empty queue gives an empty packet and an error", b.GetPacket().Length == 0 && b.GetPacketError() == Error.Unavailable);

        var payload = new GDict { ["hp"] = 90, ["pos"] = new Vector3(1, 2, 3), ["tags"] = new[] { "a", "b" } };
        T.Check("put_var", a.PutVar(payload) == Error.Ok);
        Until(() => b.GetAvailablePacketCount() > 0);
        GDict got = b.GetVar().AsGodotDictionary();
        T.Check("get_var round-trips a dictionary", got.Count == 3 && got["hp"].AsInt64() == 90 && got["pos"].AsVector3() == new Vector3(1, 2, 3) && got["tags"].AsStringArray().Length == 2);
        T.Check("put_var has no length prefix: the packet is the encoding", a.PutVar(5) == Error.Ok && Until(() => b.GetAvailablePacketCount() > 0) && b.GetPacket().SequenceEqual(Bytes(2, 0, 0, 0, 5, 0, 0, 0)));
        a.EncodeBufferMaxSize = 8;
        T.Check("a Variant over EncodeBufferMaxSize is refused", a.PutVar(new GArray { 1, 2, 3 }) == Error.OutOfMemory);
        a.EncodeBufferMaxSize = 8388608;
        a.PutPacket(new byte[] { 0xFF, 0xFF });
        Until(() => b.GetAvailablePacketCount() > 0);
        T.Check("get_var on bytes that are not a Variant gives nil and an error", b.GetVar().IsNil && b.GetPacketError() != Error.Ok);

        // a connected peer keeps only its peer's packets
        var c = new PacketPeerUDP();
        T.Check("connect_to_host", c.ConnectToHost("127.0.0.1", b.GetLocalPort()) == Error.Ok && c.IsSocketConnected());
        var stranger = new PacketPeerUDP();
        stranger.Bind(0, "127.0.0.1");
        stranger.SetDestAddress("127.0.0.1", c.GetLocalPort());
        stranger.PutPacket(new byte[] { 66 });
        b.SetDestAddress("127.0.0.1", c.GetLocalPort());
        b.PutPacket(new byte[] { 77 });
        Until(() => c.GetAvailablePacketCount() > 0);
        T.Check("a connected UDP peer drops packets from other addresses", c.GetAvailablePacketCount() == 1 && c.GetPacket()[0] == 77);
        c.Close();
        T.Check("close unbinds", !c.IsSocketConnected() && c.GetLocalPort() == 0);

        // an unbound peer can still send: its socket opens on loopback for a loopback destination
        var sender = new PacketPeerUDP();
        sender.SetDestAddress("127.0.0.1", b.GetLocalPort());
        T.Check("sending without ever binding", sender.PutPacket(new byte[] { 5 }) == Error.Ok && Until(() => b.GetAvailablePacketCount() > 0) && b.GetPacket()[0] == 5);
        sender.SetBroadcastEnabled(false);
        T.Check("multicast is not available", sender.JoinMulticastGroup("239.0.0.1", "") == Error.Unavailable);
        a.Close(); b.Close(); sender.Close(); stranger.Close();
    }

    private static void UdpErrors()
    {
        var a = new PacketPeerUDP();
        T.Check("a port out of range", a.Bind(70000, "127.0.0.1") == Error.InvalidParameter);
        T.Check("an unparseable bind address", a.Bind(0, "not an address") == Error.InvalidParameter);
        T.Check("bind", a.Bind(0, "127.0.0.1") == Error.Ok);
        T.Check("bind twice", a.Bind(0, "127.0.0.1") == Error.AlreadyInUse);
        var b = new PacketPeerUDP();
        T.Check("bind to a taken port", b.Bind(a.GetLocalPort(), "127.0.0.1") == Error.AlreadyInUse);
        T.Check("a bad destination", b.SetDestAddress("", 5) == Error.InvalidParameter && b.SetDestAddress("127.0.0.1", 0) == Error.InvalidParameter);
        T.Check("wait with no socket", new PacketPeerUDP().Wait() == Error.Unconfigured);
        T.Check("a packet over the UDP limit", ((Func<Error>)(() => { var p = new PacketPeerUDP(); p.SetDestAddress("127.0.0.1", 9); return p.PutPacket(new byte[70000]); }))() == Error.InvalidParameter);
        var v6 = new PacketPeerUDP();
        Error e6 = v6.Bind(0, "::1");
        if (e6 == Error.Ok)
        {
            var w6 = new PacketPeerUDP();
            w6.SetDestAddress("::1", v6.GetLocalPort());
            T.Check("IPv6 loopback datagram", w6.PutPacket(new byte[] { 6 }) == Error.Ok && Until(() => v6.GetAvailablePacketCount() > 0) && v6.GetPacketIP() == "" && v6.GetPacket()[0] == 6 && v6.GetPacketIP() == "::1");
        }
        else Console.WriteLine("  (no IPv6 loopback: the IPv6 check was skipped)");
        a.Close(); b.Close(); v6.Close();
    }

    private static void UdpServerRouting()
    {
        var server = new UDPServer();
        T.Check("not listening yet", !server.IsListening() && server.Poll() == Error.Unconfigured);
        T.Check("listen", server.Listen(0, "127.0.0.1") == Error.Ok && server.IsListening() && server.GetLocalPort() > 0);
        T.Check("listen twice", server.Listen(0, "127.0.0.1") == Error.AlreadyInUse);
        int port = server.GetLocalPort();

        var ca = new PacketPeerUDP(); ca.ConnectToHost("127.0.0.1", port);
        var cb = new PacketPeerUDP(); cb.ConnectToHost("127.0.0.1", port);
        ca.PutPacket(new byte[] { 1 });
        cb.PutPacket(new byte[] { 2 });
        T.Check("a new address makes a pending connection", Until(() => server.IsConnectionAvailable()));
        Until(() => { server.Poll(); return false; }, 100);
        PacketPeerUDP? pa = server.TakeConnection();
        PacketPeerUDP? pb = server.TakeConnection();
        T.Check("two distinct peers", pa != null && pb != null && !ReferenceEquals(pa, pb) && server.TakeConnection() == null);
        if (pa == null || pb == null) return;
        // each peer got the datagram of its own source
        bool aIsA = pa.GetAvailablePacketCount() == 1 && pa.GetPacket()[0] == 1;
        bool bIsB = pb.GetAvailablePacketCount() == 1 && pb.GetPacket()[0] == 2;
        T.Check("each peer holds the first packet of its own source", aIsA && bIsB && pa.GetPacketPort() == ca.GetLocalPort() && pb.GetPacketPort() == cb.GetLocalPort());
        ca.PutPacket(new byte[] { 11 });
        ca.PutPacket(new byte[] { 12 });
        Until(() => pa.GetAvailablePacketCount() >= 2);
        T.Check("later datagrams are routed to the peer, not to a new connection", pa.GetAvailablePacketCount() == 2 && !server.IsConnectionAvailable() && pb.GetAvailablePacketCount() == 0);
        T.Check("peer packets arrive in order", pa.GetPacket()[0] == 11 && pa.GetPacket()[0] == 12);
        T.Check("a server-made peer replies to its source", pa.PutPacket(new byte[] { 99 }) == Error.Ok && Until(() => ca.GetAvailablePacketCount() > 0) && ca.GetPacket()[0] == 99);
        T.Check("and the reply came from the server's port", ca.GetPacketPort() == port);
        T.Check("a server-made peer is bound", pa.IsBound() && pa.GetLocalPort() == port);

        pa.Close();
        ca.PutPacket(new byte[] { 21 });
        T.Check("after the peer closes, its address makes a new pending connection", Until(() => server.IsConnectionAvailable()));
        server.TakeConnection();

        server.MaxPendingConnections = 1;
        var c1 = new PacketPeerUDP(); c1.ConnectToHost("127.0.0.1", port);
        var c2 = new PacketPeerUDP(); c2.ConnectToHost("127.0.0.1", port);
        var c3 = new PacketPeerUDP(); c3.ConnectToHost("127.0.0.1", port);
        c1.PutPacket(new byte[] { 1 }); c2.PutPacket(new byte[] { 2 }); c3.PutPacket(new byte[] { 3 });
        Until(() => { server.Poll(); return false; }, 150);
        int taken = 0;
        while (server.TakeConnection() != null) taken++;
        T.Check("past MaxPendingConnections new sources are dropped", taken == 1);
        server.Stop();
        T.Check("stop", !server.IsListening() && server.TakeConnection() == null);
        foreach (var p in new[] { ca, cb, c1, c2, c3 }) p.Close();
    }

    // ---- TCP ----

    private static (TCPServer Server, StreamPeerTCP Client, StreamPeerTCP Accepted) TcpPair()
    {
        var server = new TCPServer();
        server.Listen(0, "127.0.0.1");
        var client = new StreamPeerTCP();
        client.ConnectToHost("127.0.0.1", server.GetLocalPort());
        Until(() => { client.Poll(); return client.GetStatus() == StreamPeerTCP.Status.Connected; });
        StreamPeerTCP? accepted = null;
        Until(() => (accepted ??= server.TakeConnection()) != null);
        return (server, client, accepted!);
    }

    private static void TcpBasics()
    {
        var server = new TCPServer();
        T.Check("not listening", !server.IsListening() && !server.IsConnectionAvailable() && server.TakeConnection() == null);
        T.Check("listen", server.Listen(0, "127.0.0.1") == Error.Ok && server.IsListening() && server.GetLocalPort() > 0);
        T.Check("listen twice", server.Listen(0, "127.0.0.1") == Error.AlreadyInUse);
        T.Check("no connection pending", !server.IsConnectionAvailable());
        var client = new StreamPeerTCP();
        T.Check("status before connecting", client.GetStatus() == StreamPeerTCP.Status.None);
        T.Check("a bad port", client.ConnectToHost("127.0.0.1", 0) == Error.InvalidParameter);
        T.Check("connect_to_host", client.ConnectToHost("127.0.0.1", server.GetLocalPort()) == Error.Ok);
        T.Check("connecting twice", client.ConnectToHost("127.0.0.1", server.GetLocalPort()) == Error.AlreadyInUse);
        T.Check("poll moves it to connected", Until(() => { client.Poll(); return client.GetStatus() == StreamPeerTCP.Status.Connected; }));
        T.Check("the server sees a connection", Until(() => server.IsConnectionAvailable()));
        StreamPeerTCP? acc = server.TakeConnection();
        T.Check("take_connection", acc != null && acc.GetStatus() == StreamPeerTCP.Status.Connected && acc.GetConnectedHost() == "127.0.0.1" && acc.GetConnectedPort() == client.GetLocalPort());
        if (acc == null) return;
        T.Check("the client knows where it connected", client.GetConnectedHost() == "127.0.0.1" && client.GetConnectedPort() == server.GetLocalPort() && client.GetLocalPort() > 0);

        client.PutU8(7); client.Put16(-2); client.Put32(123456); client.PutU64(1UL << 60); client.PutFloat(2.5f); client.PutString("tcp"); client.PutVar(new GDict { ["a"] = 1 });
        T.Check("integers, floats, strings and a Variant cross the socket",
            acc.GetU8() == 7 && acc.Get16() == -2 && acc.Get32() == 123456 && acc.GetU64() == 1UL << 60 && acc.GetFloat() == 2.5f && acc.GetString() == "tcp" && acc.GetVar().AsGodotDictionary()["a"].AsInt64() == 1);
        client.BigEndian = true; acc.BigEndian = true;
        client.Put32(0x0A0B0C0D);
        T.Check("big-endian on both ends", acc.Get32() == 0x0A0B0C0D);
        client.BigEndian = false; acc.BigEndian = false; acc.Put32(0x0A0B0C0D);
        T.CheckSeq("and little-endian bytes are the reverse", Bytes(0x0D, 0x0C, 0x0B, 0x0A), client.GetData(4)[1].AsByteArray());

        client.PutData(new byte[] { 1, 2, 3, 4, 5, 6 });
        Until(() => acc.GetAvailableBytes() >= 6);
        T.Check("get_available_bytes", acc.GetAvailableBytes() == 6);
        GArray part = acc.GetPartialData(4);
        T.Check("get_partial_data takes what it is asked for", part[0].AsInt64() == 0 && part[1].AsByteArray().Length == 4 && acc.GetAvailableBytes() == 2);
        GArray none = new StreamPeerTCP().GetPartialData(4);
        T.Check("reading an unconnected peer is an error", none[0].AsInt64() != 0);
        T.Check("put_partial_data reports how much it sent", client.PutPartialData(new byte[] { 9, 9 })[1].AsInt64() == 2);
        server.Stop();
        T.Check("stop", !server.IsListening());
    }

    private static void TcpBulk()
    {
        var (server, client, acc) = TcpPair();
        T.Check("pair connected", client.GetStatus() == StreamPeerTCP.Status.Connected && acc != null);
        var big = new byte[1 << 20];
        new Random(3).NextBytes(big);
        int sent = 0;
        var got = new List<byte>();
        Until(() =>
        {
            if (sent < big.Length)
            {
                var chunk = new byte[Math.Min(65536, big.Length - sent)];
                System.Array.Copy(big, sent, chunk, 0, chunk.Length);
                GArray r = client.PutPartialData(chunk);
                sent += (int)r[1].AsInt64();
            }
            GArray g = acc.GetPartialData(1 << 20);
            got.AddRange(g[1].AsByteArray());
            return got.Count >= big.Length;
        }, 20000);
        T.Check("a megabyte arrives in partial writes and reads, intact", got.Count == big.Length && got.SequenceEqual(big));

        client.PutData(new byte[100]);
        T.Check("a blocking get_data waits for all of it", acc.GetData(100)[1].AsByteArray().Length == 100);
        client.DisconnectFromHost();
        T.Check("disconnect_from_host", client.GetStatus() == StreamPeerTCP.Status.None && client.GetConnectedHost() == "");
        T.Check("the other end notices the close on poll", Until(() => { acc.Poll(); return acc.GetStatus() == StreamPeerTCP.Status.None; }));
        T.Check("reading a closed peer is an error", acc.GetData(4)[0].AsInt64() != 0);
        server.Stop();

        // a connect to a port nobody listens on
        var dead = new TCPServer();
        dead.Listen(0, "127.0.0.1");
        int deadPort = dead.GetLocalPort();
        dead.Stop();
        var c = new StreamPeerTCP();
        c.ConnectToHost("127.0.0.1", deadPort);
        T.Check("connecting to a closed port ends in Error", Until(() => { c.Poll(); return c.GetStatus() == StreamPeerTCP.Status.Error; }));
        c.DisconnectFromHost();
        T.Check("and the peer can try again after disconnect_from_host", c.GetStatus() == StreamPeerTCP.Status.None);
        T.Check("binding a client socket is not available", c.Bind(0) == Error.Unavailable);
    }

    // ---- ENet ----

    private static (ENetConnection Server, ENetConnection Client) HostPair(int clientChannels = 3)
    {
        var server = new ENetConnection();
        server.CreateHostBound("127.0.0.1", 0, 8, 4);
        var client = new ENetConnection();
        client.CreateHost(1);
        return (server, client);
    }

    private static GArray Next(ENetConnection c, int ms = 0) => c.Service(ms);

    private static void EnetBasics()
    {
        var (server, client) = HostPair();
        T.Check("the server has a port and the channel limit it asked for", server.GetLocalPort() > 0 && server.GetMaxChannels() == 4);
        T.Check("a second create is refused", server.CreateHost() == Error.AlreadyInUse);
        T.Check("a client host has no port until it connects", client.GetLocalPort() == 0);
        ENetPacketPeer? cp = client.ConnectToHost("127.0.0.1", server.GetLocalPort(), 3, 4242);
        T.Check("connect_to_host returns the peer, Connecting", cp != null && cp.GetState() == ENetPacketPeer.PeerState.Connecting);
        if (cp == null) return;
        ENetPacketPeer? sp = null;
        long sdata = 0, cdata = -1;
        bool cok = false, sok = false;
        T.Check("both sides see Connect", Until(() =>
        {
            GArray s = Next(server), c = Next(client);
            if (s[0].AsInt64() == (long)ENetConnection.EventType.Connect) { sok = true; sp = s[1].AsGodotObject() as ENetPacketPeer; sdata = s[2].AsInt64(); }
            if (c[0].AsInt64() == (long)ENetConnection.EventType.Connect) { cok = true; cdata = c[2].AsInt64(); }
            return sok && cok;
        }));
        T.Check("the connect-data word reaches the server", sdata == 4242 && cdata == 0 && sp != null);
        if (sp == null) return;
        T.Check("peers are Connected with the negotiated channels", cp.IsActive() && sp.IsActive() && cp.GetChannels() == 3 && sp.GetChannels() == 3);
        T.Check("the remote address is loopback", sp.GetRemoteAddress() == "127.0.0.1" && sp.GetRemotePort() > 0 && cp.GetRemotePort() == server.GetLocalPort());
        T.Check("the client now has a socket", client.GetLocalPort() > 0);

        T.Check("send reliable on channel 1", cp.Send(1, new byte[] { 1, 2, 3 }, ENetPacketPeer.FlagReliable) == Error.Ok);
        T.Check("send unsequenced on channel 2", cp.Send(2, new byte[] { 4 }, ENetPacketPeer.FlagUnsequenced) == Error.Ok);
        T.Check("send unreliable ordered on channel 0", cp.Send(0, new byte[] { 5, 5 }, 0) == Error.Ok);
        T.Check("a channel past the count is refused", cp.Send(3, new byte[] { 1 }, 0) == Error.InvalidParameter);
        var rx = new List<(byte[] Data, int Channel, int Flags)>();
        Until(() =>
        {
            GArray s = Next(server);
            if (s[0].AsInt64() == (long)ENetConnection.EventType.Receive)
            {
                var peer = (ENetPacketPeer)s[1].AsGodotObject()!;
                byte[] d = peer.GetPacket();
                rx.Add((d, peer.GetPacketChannel(), peer.GetPacketFlags()));
            }
            Next(client);
            return rx.Count >= 3;
        });
        rx.Sort((x, y) => x.Channel.CompareTo(y.Channel));
        T.Check("three packets with their channels", rx.Count == 3 && rx[0].Channel == 0 && rx[1].Channel == 1 && rx[2].Channel == 2);
        T.Check("and their flags", rx.Count == 3 && rx[0].Flags == 0 && rx[1].Flags == ENetPacketPeer.FlagReliable && rx[2].Flags == ENetPacketPeer.FlagUnsequenced);
        T.Check("and their bytes", rx.Count == 3 && rx[1].Data.SequenceEqual(new byte[] { 1, 2, 3 }) && rx[2].Data[0] == 4 && rx[0].Data.Length == 2);

        T.Check("put_packet is a reliable send on channel 0", sp.PutPacket(new byte[] { 8 }) == Error.Ok);
        byte[]? back = null;
        Until(() =>
        {
            Next(server);
            GArray c = Next(client);
            if (c[0].AsInt64() == (long)ENetConnection.EventType.Receive) { var p = (ENetPacketPeer)c[1].AsGodotObject()!; back = p.GetPacket(); T.Check("get_packet_channel is 0 and the flag reliable", p.GetPacketChannel() == 0 && p.GetPacketFlags() == 1); }
            return back != null;
        });
        T.Check("the server's put_packet arrives", back != null && back[0] == 8);

        T.Check("a Variant through the peer's PacketPeer API", sp.PutVar(new GArray { "hello", 5 }) == Error.Ok);
        GArray? var0 = null;
        Until(() => { Next(server); GArray c = Next(client); if (c[0].AsInt64() == 3) var0 = ((ENetPacketPeer)c[1].AsGodotObject()!).GetVar().AsGodotArray(); return var0 != null; });
        T.Check("and back out", var0 != null && var0[0].AsString() == "hello" && var0[1].AsInt64() == 5);

        cp.Ping();
        cp.PingInterval(100);
        cp.SetTimeout(16, 2000, 8000);
        cp.ThrottleConfigure(3000, 4, 5);
        T.Check("throttle settings read back", cp.GetStatistic(ENetPacketPeer.PeerStatistic.PacketThrottleAcceleration) == 4 && cp.GetStatistic(ENetPacketPeer.PeerStatistic.PacketThrottleDeceleration) == 5
            && cp.GetStatistic(ENetPacketPeer.PeerStatistic.PacketThrottleInterval) == 3000);
        Until(() => { Next(server); Next(client); return cp.GetStatistic(ENetPacketPeer.PeerStatistic.RoundTripTime) > 0; }, 3000);
        T.Check("the round trip time is measured (loopback: a few ms)", cp.GetStatistic(ENetPacketPeer.PeerStatistic.RoundTripTime) >= 1 && cp.GetStatistic(ENetPacketPeer.PeerStatistic.RoundTripTime) < 500);
        T.Check("no packet loss on loopback", cp.GetStatistic(ENetPacketPeer.PeerStatistic.PacketLoss) <= ENetPacketPeer.PacketLossScale / 10);
        T.Check("host statistics count traffic and reset", client.PopStatistic(ENetConnection.HostStatistic.SentData) > 0 && client.PopStatistic(ENetConnection.HostStatistic.SentData) < 100);

        cp.PeerDisconnect(7);
        long sreason = -1, creason = -1;
        bool sd = false, cd = false;
        T.Check("a disconnect gives both sides a Disconnect event", Until(() =>
        {
            GArray s = Next(server), c = Next(client);
            if (s[0].AsInt64() == (long)ENetConnection.EventType.Disconnect) { sd = true; sreason = s[2].AsInt64(); }
            if (c[0].AsInt64() == (long)ENetConnection.EventType.Disconnect) cd = true;
            return sd && cd;
        }));
        T.Check("the data word reaches the other side", sreason == 7);
        T.Check("the peer is Disconnected and no longer active", cp.GetState() == ENetPacketPeer.PeerState.Disconnected && !cp.IsActive() && cp.Send(0, new byte[] { 1 }, 0) == Error.Unconfigured);
        _ = creason;
        server.Destroy();
        T.Check("destroy", server.GetLocalPort() == 0 && server.Service()[0].AsInt64() == (long)ENetConnection.EventType.None);
        client.Destroy();
    }

    private static void EnetMore()
    {
        var server = new ENetConnection();
        server.CreateHostBound("127.0.0.1", 0, 8);
        var c1 = new ENetConnection(); c1.CreateHost(1);
        var c2 = new ENetConnection(); c2.CreateHost(1);
        c1.ConnectToHost("127.0.0.1", server.GetLocalPort(), 2, 1);
        c2.ConnectToHost("127.0.0.1", server.GetLocalPort(), 2, 2);
        int connects = 0;
        Until(() => { if (Next(server)[0].AsInt64() == 1) connects++; Next(c1); Next(c2); return connects == 2; });
        T.Check("two clients connect", connects == 2 && server.GetPeers().Count == 2);
        server.Broadcast(0, new byte[] { 42 }, ENetPacketPeer.FlagReliable);
        int hit = 0;
        Until(() => { Next(server); foreach (ENetConnection c in new[] { c1, c2 }) if (Next(c)[0].AsInt64() == 3) hit++; return hit == 2; });
        T.Check("broadcast reaches every peer", hit == 2);
        server.BandwidthLimit(100000, 100000);
        server.ChannelLimit(3);
        T.Check("channel limit", server.GetMaxChannels() == 3);
        T.Check("a client's peer list", c1.GetPeers().Count == 1);

        server.RefuseNewConnections(true);
        var c3 = new ENetConnection(); c3.CreateHost(1);
        ENetPacketPeer? p3 = c3.ConnectToHost("127.0.0.1", server.GetLocalPort(), 2, 3);
        p3?.SetTimeout(4, 500, 1500);
        bool failed = false;
        Until(() => { Next(server); if (Next(c3)[0].AsInt64() == 2) failed = true; return failed; }, 6000);
        T.Check("a refusing server turns the client away; the client gets a Disconnect, never a Connect", failed && server.GetPeers().Count == 2);

        var junk = new byte[20];
        System.Array.Fill(junk, (byte)0xAB);
        server.SocketSend("127.0.0.1", c1.GetLocalPort(), junk);
        T.Check("socket_send puts a raw datagram on the wire; the receiver's checksum rejects it", Until(() => { Next(c1); return NetNative.aver_net_host_stat(c1.HostHandle, (int)NetHostStat.BadChecksum) > 0; }, 2000));
        T.Check("DTLS is not available", server.DtlsServerSetup(new object()) == Error.Unavailable && c1.DtlsClientSetup("localhost") == Error.Unavailable);
        T.Check("argument checks", new ENetConnection().CreateHost(0) == Error.InvalidParameter && new ENetConnection().CreateHostBound("not an address", 0) == Error.InvalidParameter);
        T.Check("connecting from an uncreated host", new ENetConnection().ConnectToHost("127.0.0.1", 5000) == null);
        c1.Compress(ENetConnection.CompressionMode.Zlib);
        foreach (var c in new[] { server, c1, c2, c3 }) c.Destroy();
    }

    // ---- multiplayer ----

    private sealed class Net
    {
        public ENetMultiplayerPeer Server = new();
        public List<ENetMultiplayerPeer> Clients = new();
        public void Poll() { Server.Poll(); foreach (var c in Clients) c.Poll(); }
        public ENetMultiplayerPeer AddClient()
        {
            var c = new ENetMultiplayerPeer();
            Clients.Add(c);
            return c;
        }
    }

    private static Net StartNet(int clients, int maxClients = 8)
    {
        var n = new Net();
        n.Server.SetBindIp("127.0.0.1");
        if (n.Server.CreateServer(0, maxClients) != Error.Ok) throw new InvalidOperationException("server");
        int port = n.Server.Host!.GetLocalPort();
        for (int i = 0; i < clients; i++)
        {
            var c = n.AddClient();
            if (c.CreateClient("127.0.0.1", port) != Error.Ok) throw new InvalidOperationException("client");
        }
        Until(() => n.Clients.All(c => c.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected) && n.Server.Host!.GetPeers().Count == clients, 8000, n.Poll);
        return n;
    }

    private static void MultiplayerConnect()
    {
        var server = new ENetMultiplayerPeer();
        var client = new ENetMultiplayerPeer();
        T.Check("before creating: disconnected, id 0", server.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected && server.GetUniqueId() == 0 && server.Host == null);
        T.Check("set_bind_ip checks the address", server.SetBindIp("nonsense address") == Error.InvalidParameter && server.SetBindIp("127.0.0.1") == Error.Ok);
        var sConnected = new List<long>(); var sGone = new List<long>(); var cConnected = new List<long>(); var cGone = new List<long>();
        server.PeerConnected += id => sConnected.Add(id);
        server.PeerDisconnected += id => sGone.Add(id);
        client.PeerConnected += id => cConnected.Add(id);
        client.PeerDisconnected += id => cGone.Add(id);
        T.Check("create_server", server.CreateServer(0, 4) == Error.Ok);
        T.Check("the server is connected at once, id 1", server.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected && server.GetUniqueId() == 1 && server.IsServer());
        T.Check("a second create_server is refused", server.CreateServer(0) == Error.AlreadyInUse && server.CreateClient("127.0.0.1", 5000) == Error.AlreadyInUse);
        int port = server.Host!.GetLocalPort();
        T.Check("create_client", client.CreateClient("127.0.0.1", port) == Error.Ok);
        T.Check("the client is Connecting with a random id", client.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connecting && client.GetUniqueId() >= 2 && !client.IsServer());
        T.Check("both sides connect", Until(() => client.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected && sConnected.Count == 1, 8000, () => { server.Poll(); client.Poll(); }));
        T.Check("the server emits peer_connected with the client's id", sConnected.Count == 1 && sConnected[0] == client.GetUniqueId());
        T.Check("the client emits peer_connected(1)", cConnected.Count == 1 && cConnected[0] == 1);
        T.Check("get_peer finds the ENet peers", server.GetPeer((int)sConnected[0]) != null && client.GetPeer(1) != null && server.GetPeer(999) == null);
        T.Check("server relay is supported", server.IsServerRelaySupported() && client.IsServerRelaySupported());
        uint gen = server.GenerateUniqueId();
        T.Check("generate_unique_id is a client id", gen >= 2 && gen <= 0x7FFFFFFF);
        T.Check("max packet size", server.GetMaxPacketSize() == 1 << 24);
        client.Close();
        T.Check("close: disconnected, id 0, host gone", client.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected && client.GetUniqueId() == 0 && client.Host == null);
        T.Check("the server emits peer_disconnected", Until(() => sGone.Count == 1, 8000, server.Poll) && sGone[0] == sConnected[0]);
        server.Close();
        T.Check("the server closes too", server.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected && server.Host == null);
        T.Check("a closed peer can be created again", server.CreateServer(0, 2) == Error.Ok);
        server.Close();
    }

    private static void MultiplayerPackets()
    {
        var n = StartNet(2);
        ENetMultiplayerPeer a = n.Clients[0], b = n.Clients[1];
        T.Check("two clients and the server are connected", a.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected && b.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected);
        T.Check("with distinct ids", a.GetUniqueId() != b.GetUniqueId());

        a.TransferMode = MultiplayerPeer.TransferModeEnum.Reliable;
        a.SetTargetPeer(MultiplayerPeer.TargetPeerServer);
        T.Check("a client sends reliably to the server", a.PutPacket(new byte[] { 1, 1 }) == Error.Ok);
        Until(() => n.Server.GetAvailablePacketCount() > 0, 3000, n.Poll);
        byte[] p = n.Server.GetPacket();
        T.Check("the server reads it and knows who sent it", p.SequenceEqual(new byte[] { 1, 1 }) && n.Server.GetPacketPeer() == a.GetUniqueId());
        T.Check("with its mode and channel", n.Server.GetPacketMode() == MultiplayerPeer.TransferModeEnum.Reliable && n.Server.GetPacketChannel() == 0);

        foreach (var mode in new[] { MultiplayerPeer.TransferModeEnum.Unreliable, MultiplayerPeer.TransferModeEnum.UnreliableOrdered })
        {
            a.TransferMode = mode;
            a.PutPacket(new byte[] { 2 });
            Until(() => n.Server.GetAvailablePacketCount() > 0, 3000, n.Poll);
            n.Server.GetPacket();
            T.Check($"{mode}: the receiver reports the same mode", n.Server.GetPacketMode() == mode);
        }

        a.TransferMode = MultiplayerPeer.TransferModeEnum.Reliable;
        a.TransferChannel = 2;
        a.PutPacket(new byte[] { 3 });
        Until(() => n.Server.GetAvailablePacketCount() > 0, 3000, n.Poll);
        n.Server.GetPacket();
        T.Check("transfer_channel 2 arrives as channel 2", n.Server.GetPacketChannel() == 2);
        a.TransferChannel = 0;

        // server to a given client, to everyone, to everyone but one
        n.Server.TransferMode = MultiplayerPeer.TransferModeEnum.Reliable;
        n.Server.SetTargetPeer(b.GetUniqueId());
        T.Check("the server targets one client", n.Server.PutPacket(new byte[] { 10 }) == Error.Ok);
        Until(() => b.GetAvailablePacketCount() > 0, 3000, n.Poll);
        T.Check("only that client receives it, from peer 1", b.GetAvailablePacketCount() == 1 && a.GetAvailablePacketCount() == 0 && b.GetPacket()[0] == 10 && b.GetPacketPeer() == 1);
        n.Server.SetTargetPeer(MultiplayerPeer.TargetPeerBroadcast);
        n.Server.PutPacket(new byte[] { 11 });
        Until(() => a.GetAvailablePacketCount() > 0 && b.GetAvailablePacketCount() > 0, 3000, n.Poll);
        T.Check("broadcast reaches both", a.GetPacket()[0] == 11 && b.GetPacket()[0] == 11);
        n.Server.SetTargetPeer(-a.GetUniqueId());
        n.Server.PutPacket(new byte[] { 12 });
        Until(() => b.GetAvailablePacketCount() > 0, 3000, n.Poll);
        Until(() => false, 150, n.Poll);
        T.Check("a negative target is everyone but that peer", b.GetAvailablePacketCount() == 1 && a.GetAvailablePacketCount() == 0);
        b.GetPacket();
        n.Server.SetTargetPeer(123456);
        T.Check("an unknown target is refused", n.Server.PutPacket(new byte[] { 1 }) == Error.InvalidParameter);

        T.Check("an empty queue", a.GetPacket().Length == 0 && a.GetPacketError() == Error.Unavailable && a.GetAvailablePacketCount() == 0);
        var lone = new ENetMultiplayerPeer();
        T.Check("sending while not connected is Unconfigured", lone.PutPacket(new byte[] { 1 }) == Error.Unconfigured);
        foreach (var c in n.Clients) c.Close();
        n.Server.Close();
    }

    private static void MultiplayerVarsAndClose()
    {
        var n = StartNet(2);
        ENetMultiplayerPeer a = n.Clients[0], b = n.Clients[1];
        a.SetTargetPeer(1);
        var state = new GDict { ["pos"] = new Vector3(1, 2, 3), ["name"] = "p1", ["inv"] = new GArray { 1, 2, 3 } };
        T.Check("put_var", a.PutVar(state) == Error.Ok);
        Until(() => n.Server.GetAvailablePacketCount() > 0, 3000, n.Poll);
        GDict got = n.Server.GetVar().AsGodotDictionary();
        T.Check("get_var on the server", got["name"].AsString() == "p1" && got["pos"].AsVector3() == new Vector3(1, 2, 3) && got["inv"].AsGodotArray().Count == 3);

        var big = new byte[500000];
        new Random(8).NextBytes(big);
        a.TransferMode = MultiplayerPeer.TransferModeEnum.Reliable;
        T.Check("a 500 kB packet", a.PutPacket(big) == Error.Ok);
        Until(() => n.Server.GetAvailablePacketCount() > 0, 10000, n.Poll);
        T.Check("arrives whole", n.Server.GetAvailablePacketCount() == 1 && n.Server.GetPacket().SequenceEqual(big));

        var sGone = new List<long>(); var aGone = new List<long>();
        n.Server.PeerDisconnected += id => sGone.Add(id);
        a.PeerDisconnected += id => aGone.Add(id);
        int aid = a.GetUniqueId();
        n.Server.DisconnectPeer(aid);
        T.Check("disconnect_peer (graceful) removes the peer on a later poll", Until(() => sGone.Count == 1 && aGone.Count == 1, 8000, n.Poll));
        T.Check("both emit peer_disconnected, the client with the server's id", sGone[0] == aid && aGone[0] == 1);
        T.Check("the client is Disconnected", a.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected);

        int bid = b.GetUniqueId();
        n.Server.DisconnectPeer(bid, true);
        T.Check("a forced disconnect emits at once, without a poll", sGone.Count == 2 && sGone[1] == bid && n.Server.GetPeer(bid) == null);
        T.Check("the forced peer learns of it", Until(() => b.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected, 8000, n.Poll));
        n.Server.DisconnectPeer(99999);
        n.Server.Close();
        foreach (var c in n.Clients) c.Close();
    }

    private static void MultiplayerLimits()
    {
        var n = StartNet(1, maxClients: 1);
        T.Check("one client fills a one-slot server", n.Clients[0].GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected);
        int port = n.Server.Host!.GetLocalPort();
        var extra = new ENetMultiplayerPeer();
        extra.CreateClient("127.0.0.1", port);
        ((ENetPacketPeer)extra.Host!.GetPeers()[0].AsGodotObject()!).SetTimeout(4, 600, 2000);
        T.Check("a second client is never connected and ends Disconnected", Until(() => extra.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected, 8000, () => { n.Poll(); extra.Poll(); }));
        T.Check("the server never announced it", n.Server.Host!.GetPeers().Count == 1);
        extra.Close();

        var s2 = new ENetMultiplayerPeer();
        s2.SetBindIp("127.0.0.1");
        s2.CreateServer(0, 4);
        s2.RefuseNewConnections = true;
        var late = new ENetMultiplayerPeer();
        late.CreateClient("127.0.0.1", s2.Host!.GetLocalPort());
        ((ENetPacketPeer)late.Host!.GetPeers()[0].AsGodotObject()!).SetTimeout(4, 600, 2000);
        T.Check("refuse_new_connections turns clients away", Until(() => late.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected, 8000, () => { s2.Poll(); late.Poll(); }) && s2.Host!.GetPeers().Count == 0);

        T.Check("argument errors", new ENetMultiplayerPeer().CreateServer(70000) == Error.InvalidParameter && new ENetMultiplayerPeer().CreateServer(0, 0) == Error.InvalidParameter
            && new ENetMultiplayerPeer().CreateClient("127.0.0.1", 0) == Error.InvalidParameter && new ENetMultiplayerPeer().CreateClient("", 5000) == Error.CantResolve);
        T.Check("mesh mode is not available", new ENetMultiplayerPeer().CreateMesh(5) == Error.Unavailable);
        var dup = new ENetMultiplayerPeer();
        dup.SetBindIp("127.0.0.1");
        T.Check("two servers on one port", dup.CreateServer(n.Server.Host!.GetLocalPort()) != Error.Ok);
        foreach (var c in n.Clients) c.Close();
        n.Server.Close(); s2.Close(); late.Close();
    }

    private static void Offline()
    {
        var o = new OfflineMultiplayerPeer();
        T.Check("always connected, the server, id 1", o.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Connected && o.GetUniqueId() == 1 && o.IsServer());
        T.Check("no packets", o.GetAvailablePacketCount() == 0 && o.GetPacket().Length == 0 && o.GetPacketError() == Error.Unavailable);
        T.Check("sending is accepted and goes nowhere", o.PutPacket(new byte[] { 1 }) == Error.Ok);
        o.Poll(); o.Close(); o.DisconnectPeer(5); o.SetTargetPeer(3);
        T.Check("the remaining calls are no-ops", o.GetPacketPeer() == 1 && o.GetMaxPacketSize() == 1 && !o.IsServerRelaySupported());
    }
}
