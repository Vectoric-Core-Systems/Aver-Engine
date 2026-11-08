// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Replication: a server and clients in one process, over the in-memory network and (when Aver.Net.Abi loads) real UDP on 127.0.0.1.

using Aver.Framework;
using Aver.Framework.Net;
using Aver.Scene;

enum NetMode { Idle, Run, Dead }

[NetType("Test.Unit")]
sealed class TUnit : NetObject
{
    [Replicated(OnChanged = nameof(HealthChanged))] public int Health = 100;
    [Replicated] public string Label = "";
    [Replicated] public Vec3 Pos;
    [Replicated] public float[] Path = Array.Empty<float>();
    [Replicated] public NetMode Mode;
    [Replicated(OwnerOnly = true)] public int Ammo;
    [Replicated(Reliable = false)] public float Aim;
    public Vec3 Where;
    public int ChangeCalls, LastOld;
    public bool Spawned, Destroyed;
    public uint OwnerBefore;
    public readonly List<string> Calls = new();

    public override Vec3 NetPosition => Where;
    private void HealthChanged(int old) { ChangeCalls++; LastOld = old; }
    protected override void OnNetSpawn() => Spawned = true;
    protected override void OnNetDestroy() => Destroyed = true;
    protected override void OnOwnerChanged(uint previous) => OwnerBefore = previous;

    [Rpc(RpcTarget.Server)] public void Fire(int count, string weapon) => Calls.Add($"fire:{count}:{weapon}");
    [Rpc(RpcTarget.Client)] public void Hint(string text) => Calls.Add("hint:" + text);
    [Rpc(RpcTarget.Multicast)] public void Boom(Vec3 at) => Calls.Add($"boom:{at.X}");
    [Rpc(RpcTarget.Multicast, Reliable = false)] public void Flash() => Calls.Add("flash");
    [Rpc(RpcTarget.Server)] public void Throws() => throw new InvalidOperationException("boom");

    public void DoFire(int n, string w) => CallRpc(nameof(Fire), n, w);
    public void DoHint(string t) => CallRpc(nameof(Hint), t);
    public void DoBoom(float x) => CallRpc(nameof(Boom), new Vec3(x, 0, 0));
    public void DoFlash() => CallRpc(nameof(Flash));
    public void DoThrows() => CallRpc(nameof(Throws));
    public void BadCall() => CallRpc("NoSuchMethod");
}

static class NetReplicationTests
{
    private sealed class Rig : IDisposable
    {
        public readonly bool Native;
        public readonly LoopbackNetwork? Net;
        public readonly NetSession Server;
        public readonly List<NetSession> Clients = new();
        public readonly int Port;
        public long Now = 1000;

        public Rig(bool native, int maxClients = 4)
        {
            Native = native;
            if (native)
            {
                var t = NativeNetTransport.CreateServer("127.0.0.1", 0, maxClients, out Port) ?? throw new Exception("native server did not start");
                Server = new NetSession(t);
                Server.StartServer("127.0.0.1", Port, maxClients, out _);
            }
            else
            {
                Net = new LoopbackNetwork();
                Server = new NetSession(Net.CreateTransport());
                Server.StartServer("127.0.0.1", 0, maxClients, out Port);
            }
            Server.UnreliableResyncMs = 100;
        }

        public NetSession AddClient(string hello = "")
        {
            INetTransport t = Native ? NativeNetTransport.CreateClient()! : Net!.CreateTransport();
            var c = new NetSession(t) { UnreliableResyncMs = 100 };
            if (!c.StartClient("127.0.0.1", Port, hello)) throw new Exception("client did not start");
            Clients.Add(c);
            return c;
        }

        public void Step()
        {
            if (Native) { Thread.Sleep(1); Now = Environment.TickCount64; }
            else Now += 20;
            Server.Tick(Now);
            foreach (NetSession c in Clients.ToArray()) c.Tick(Now);
        }

        public bool Until(Func<bool> done, int limitMs = 5000)
        {
            long end = Environment.TickCount64 + limitMs;
            for (int i = 0; i < 600 && (!Native || Environment.TickCount64 < end); i++)
            {
                Step();
                if (done()) return true;
            }
            return done();
        }

        public void Settle(int steps = 40) { for (int i = 0; i < steps; i++) Step(); }

        public NetSession Connected(string hello = "")
        {
            NetSession c = AddClient(hello);
            if (!Until(() => c.State == NetSessionState.Connected)) throw new Exception("client did not connect");
            return c;
        }

        public void Dispose()
        {
            foreach (NetSession c in Clients) c.Dispose();
            Server.Dispose();
        }
    }

    private static TUnit? Proxy(NetSession c, uint id) => c.Find(id) as TUnit;

    public static int RunAll() => T.Suite(() =>
    {
        NetTypes.Register<TUnit>("Test.Unit");
        T.Run("net codec: every supported type round-trips, truncation is refused", Codec);
        foreach (bool native in new[] { false, true })
        {
            string mode = native ? "udp" : "memory";
            if (native && !NativeNetTransport.IsAvailable())
            {
                Console.WriteLine("  SKIP: Aver.Net.Abi did not load, real-socket replication tests not run");
                continue;
            }
            T.Run($"net [{mode}]: handshake, ids, approval and version refusal", () => Handshake(native));
            T.Run($"net [{mode}]: spawn carries state; changed properties converge with delta sends", () => Properties(native));
            T.Run($"net [{mode}]: OwnerOnly state, ownership change", () => OwnerState(native));
            T.Run($"net [{mode}]: unreliable properties converge", () => Unreliable(native));
            T.Run($"net [{mode}]: Server, Client and Multicast RPCs reach the right peers", () => Rpcs(native));
            T.Run($"net [{mode}]: authority: a non-owner's Server RPC is rejected", () => Authority(native));
            T.Run($"net [{mode}]: destroy, client disconnect and server stop clean up", () => Cleanup(native));
            T.Run($"net [{mode}]: distance relevance spawns and removes", () => Relevance(native));
        }
        T.Run("net [memory]: unreliable loss only delays convergence", UnreliableLoss);
        T.Run("net [memory]: garbage and strangers do not hurt the server", Garbage);
    });

    private static void Codec()
    {
        var w = new NetWriter();
        NetCodec.Write(w, typeof(int), -7); NetCodec.Write(w, typeof(ulong), ulong.MaxValue); NetCodec.Write(w, typeof(string), "héllo");
        NetCodec.Write(w, typeof(float), 1.5f); NetCodec.Write(w, typeof(bool), true); NetCodec.Write(w, typeof(Vec3), new Vec3(1, 2, 3));
        NetCodec.Write(w, typeof(Quat), new Quat(0, 0, 0, 1)); NetCodec.Write(w, typeof(NetMode), NetMode.Dead);
        NetCodec.Write(w, typeof(int[]), new[] { 1, 2, 3 }); NetCodec.Write(w, typeof(byte[]), new byte[] { 9, 8 });
        byte[] bytes = w.ToArray();
        var r = new NetReader(bytes);
        T.Check("int", (int)NetCodec.Read(r, typeof(int))! == -7);
        T.Check("ulong", (ulong)NetCodec.Read(r, typeof(ulong))! == ulong.MaxValue);
        T.Check("string", (string)NetCodec.Read(r, typeof(string))! == "héllo");
        T.Check("float and bool", (float)NetCodec.Read(r, typeof(float))! == 1.5f && (bool)NetCodec.Read(r, typeof(bool))!);
        T.Check("Vec3", (Vec3)NetCodec.Read(r, typeof(Vec3))! == new Vec3(1, 2, 3));
        T.Check("Quat", ((Quat)NetCodec.Read(r, typeof(Quat))!).W == 1);
        T.Check("enum", (NetMode)NetCodec.Read(r, typeof(NetMode))! == NetMode.Dead);
        T.Check("int[]", ((int[])NetCodec.Read(r, typeof(int[]))!).SequenceEqual(new[] { 1, 2, 3 }));
        T.Check("byte[]", ((byte[])NetCodec.Read(r, typeof(byte[]))!).SequenceEqual(new byte[] { 9, 8 }) && r.Remaining == 0);
        bool threw = false;
        try { NetCodec.Read(new NetReader(bytes[..3]), typeof(string)); NetCodec.Read(new NetReader(new byte[] { 1, 2 }), typeof(int)); } catch (NetFormatException) { threw = true; }
        T.Check("a truncated packet throws NetFormatException", threw);
        threw = false;
        try { NetCodec.Read(new NetReader(new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }), typeof(int[])); } catch (NetFormatException) { threw = true; }
        T.Check("a huge claimed array length is refused", threw);
        T.Check("unsupported types are reported", !NetCodec.IsSupported(typeof(object)) && !NetCodec.IsSupported(typeof(List<int>)));
    }

    private static void Handshake(bool native)
    {
        using var rig = new Rig(native);
        var connected = new List<string>();
        rig.Server.ClientConnected += c => connected.Add(c.UserData + "#" + c.Id);
        NetSession a = rig.Connected("alice");
        NetSession b = rig.Connected("bob");
        T.Check("ids are assigned in order", a.LocalConnectionId == 1 && b.LocalConnectionId == 2);
        T.Check("the server sees both, with their hello data", rig.Server.Connections.Count == 2 && connected.SequenceEqual(new[] { "alice#1", "bob#2" }));
        T.Check("roles", rig.Server.IsServer && a.IsClient && rig.Server.LocalConnectionId == 0);

        rig.Server.ConnectionApproval = c => c.UserData == "cheat" ? "banned" : null;
        NetSession bad = rig.AddClient("cheat");
        string why = "";
        bad.Disconnected += r => why = r;
        T.Check("a refused client learns why", rig.Until(() => why.Length > 0) && why.Contains("banned"));
        T.Check("...and is not counted", rig.Server.Connections.Count == 2 && bad.State == NetSessionState.Disconnected);
    }

    private static void Properties(bool native)
    {
        using var rig = new Rig(native);
        NetSession c = rig.Connected();
        var spawned = new List<TUnit>();
        c.ObjectSpawned += o => spawned.Add((TUnit)o);
        var u = rig.Server.Spawn<TUnit>();
        u.Health = 80; u.Label = "grunt"; u.Pos = new Vec3(1, 2, 3); u.Path = new[] { 1f, 2f }; u.Mode = NetMode.Run;
        T.Check("spawned on the server with an id", u.NetId != 0 && u.Spawned && u.HasAuthority);
        T.Check("the proxy arrives", rig.Until(() => spawned.Count == 1));
        TUnit p = spawned[0];
        T.Check("with the initial state", p.Health == 80 && p.Label == "grunt" && p.Pos == new Vec3(1, 2, 3) && p.Path.SequenceEqual(new[] { 1f, 2f }) && p.Mode == NetMode.Run);
        T.Check("OnNetSpawn ran and the client has no authority", p.Spawned && !p.HasAuthority && p.NetId == u.NetId);
        T.Check("RepNotify fired for the initial value", p.ChangeCalls == 1);

        rig.Settle(5);
        long before = rig.Server.Stats.PropMessagesSent;
        rig.Settle(10);
        T.Check("nothing changed, nothing sent", rig.Server.Stats.PropMessagesSent == before);

        u.Health = 55;
        T.Check("a change converges", rig.Until(() => p.Health == 55));
        T.Check("one property message carried it", rig.Server.Stats.PropMessagesSent == before + 1);
        T.Check("the other state is untouched", p.Label == "grunt" && p.Pos == new Vec3(1, 2, 3));
        T.Check("RepNotify got the old value", p.ChangeCalls == 2 && p.LastOld == 80);

        u.Label = "boss"; u.Path = new[] { 9f };
        T.Check("several at once", rig.Until(() => p.Label == "boss" && p.Path.Length == 1));
        u.Health = 55;
        rig.Settle(10);
        T.Check("writing the same value again is not a change", p.ChangeCalls == 2);

        // A client that joins late gets everything in its spawn.
        NetSession late = rig.Connected();
        TUnit? lp = null;
        T.Check("a late joiner gets the live state", rig.Until(() => (lp = late.Objects.OfType<TUnit>().FirstOrDefault()) != null) && lp!.Label == "boss" && lp.Health == 55);

        bool threw = false;
        try { c.Spawn("Test.Unit"); } catch (InvalidOperationException) { threw = true; }
        T.Check("a client cannot spawn", threw);
    }

    private static void OwnerState(bool native)
    {
        using var rig = new Rig(native);
        NetSession a = rig.Connected(), b = rig.Connected();
        var u = rig.Server.Spawn<TUnit>(a.LocalConnectionId);
        u.Ammo = 30; u.Health = 90;
        T.Check("both proxies arrive", rig.Until(() => Proxy(a, u.NetId) != null && Proxy(b, u.NetId) != null && Proxy(b, u.NetId)!.Health == 90));
        TUnit pa = Proxy(a, u.NetId)!, pb = Proxy(b, u.NetId)!;
        T.Check("the owner sees OwnerOnly state, the other does not", pa.Ammo == 30 && pb.Ammo == 0);
        T.Check("ownership is known on both ends", pa.IsOwner && !pb.IsOwner && pa.OwnerId == a.LocalConnectionId);
        u.Ammo = 29;
        T.Check("owner-only changes reach only the owner", rig.Until(() => pa.Ammo == 29) && pb.Ammo == 0);

        rig.Server.SetOwner(u, b.LocalConnectionId);
        T.Check("handing it over moves ownership and the state", rig.Until(() => pb.IsOwner && pb.Ammo == 29 && !pa.IsOwner));
        T.Check("OnOwnerChanged saw the previous owner", pa.OwnerBefore == a.LocalConnectionId && u.OwnerBefore == a.LocalConnectionId);
    }

    private static void Unreliable(bool native)
    {
        using var rig = new Rig(native);
        NetSession c = rig.Connected();
        var u = rig.Server.Spawn<TUnit>();
        T.Check("proxy", rig.Until(() => Proxy(c, u.NetId) != null));
        u.Aim = 42.5f;
        T.Check("the unreliable channel carries the value", rig.Until(() => Proxy(c, u.NetId)!.Aim == 42.5f));
    }

    private static void UnreliableLoss()
    {
        using var rig = new Rig(false);
        NetSession c = rig.Connected();
        var u = rig.Server.Spawn<TUnit>();
        rig.Until(() => Proxy(c, u.NetId) != null);
        rig.Net!.DropUnreliable = _ => true;
        u.Aim = 7f; u.Health = 61;
        T.Check("reliable state still arrives while every unreliable packet is lost", rig.Until(() => Proxy(c, u.NetId)!.Health == 61));
        T.Check("the lost value has not", Proxy(c, u.NetId)!.Aim == 0);
        rig.Net.DropUnreliable = null;
        T.Check("the periodic resync repairs it", rig.Until(() => Proxy(c, u.NetId)!.Aim == 7f));
    }

    private static void Rpcs(bool native)
    {
        using var rig = new Rig(native);
        NetSession a = rig.Connected(), b = rig.Connected();
        var u = rig.Server.Spawn<TUnit>(a.LocalConnectionId);
        T.Check("proxies", rig.Until(() => Proxy(a, u.NetId) != null && Proxy(b, u.NetId) != null));
        TUnit pa = Proxy(a, u.NetId)!, pb = Proxy(b, u.NetId)!;

        pa.DoFire(3, "rifle");
        T.Check("owner's Server RPC runs on the server with its arguments", rig.Until(() => u.Calls.Count == 1) && u.Calls[0] == "fire:3:rifle");
        T.Check("and nowhere else", pa.Calls.Count == 0 && pb.Calls.Count == 0);

        u.DoFire(1, "local");
        T.Check("the server calling a Server RPC just runs it", u.Calls.Contains("fire:1:local"));

        u.DoHint("reload");
        T.Check("a Client RPC runs on the owner", rig.Until(() => pa.Calls.Contains("hint:reload")));
        rig.Settle(10);
        T.Check("...only there", pb.Calls.Count == 0 && !u.Calls.Contains("hint:reload"));

        u.DoBoom(5);
        T.Check("a Multicast runs on the server and every client", rig.Until(() => pa.Calls.Contains("boom:5") && pb.Calls.Contains("boom:5")) && u.Calls.Contains("boom:5"));

        u.DoFlash();
        T.Check("an unreliable Multicast arrives", rig.Until(() => pa.Calls.Contains("flash") && pb.Calls.Contains("flash")));

        int before = u.Calls.Count;
        pa.DoBoom(1);
        pa.DoHint("nope");
        rig.Settle(15);
        T.Check("a client cannot call Multicast or Client RPCs", u.Calls.Count == before && pb.Calls.Count == 2 && pa.Calls.Count == 3);

        bool threw = false;
        try { u.BadCall(); } catch (ArgumentException) { threw = true; }
        T.Check("an unknown method name is an error at the call site", threw);

        pa.DoThrows();
        rig.Settle(15);
        T.Check("an RPC that throws is logged and the server carries on", rig.Server.Stats.RpcsExecuted > 0);
        u.Health = 12;
        T.Check("replication still works", rig.Until(() => pa.Health == 12));
    }

    private static void Authority(bool native)
    {
        using var rig = new Rig(native);
        NetSession a = rig.Connected(), b = rig.Connected();
        var u = rig.Server.Spawn<TUnit>(a.LocalConnectionId);
        rig.Until(() => Proxy(a, u.NetId) != null && Proxy(b, u.NetId) != null);
        var rejections = new List<NetRpcRejection>();
        rig.Server.RpcRejected += rejections.Add;

        Proxy(b, u.NetId)!.DoFire(99, "stolen");
        T.Check("the call is refused", rig.Until(() => rejections.Count == 1));
        T.Check("with the reason and the sender", rejections[0].Reason.Contains("owner") && rejections[0].From!.Id == b.LocalConnectionId && rejections[0].Method == "Fire");
        T.Check("and has no effect", u.Calls.Count == 0 && rig.Server.Stats.RpcsRejected == 1);

        Proxy(a, u.NetId)!.DoFire(1, "mine");
        T.Check("the owner's call still goes through", rig.Until(() => u.Calls.Count == 1));

        rig.Server.SetOwner(u, b.LocalConnectionId);
        rig.Until(() => Proxy(b, u.NetId)!.IsOwner);
        Proxy(a, u.NetId)!.DoFire(2, "old owner");
        Proxy(b, u.NetId)!.DoFire(3, "new owner");
        T.Check("after a handover the old owner is refused and the new one is not", rig.Until(() => u.Calls.Count == 2) && u.Calls[1] == "fire:3:new owner");
        rig.Settle(10);
        T.Check("the old owner's attempt was rejected", rejections.Count == 2 && rejections[1].From!.Id == a.LocalConnectionId);
    }

    private static void Cleanup(bool native)
    {
        using var rig = new Rig(native);
        NetSession a = rig.Connected(), b = rig.Connected();
        var mine = rig.Server.Spawn<TUnit>(a.LocalConnectionId);
        var shared = rig.Server.Spawn<TUnit>();
        T.Check("both clients have both", rig.Until(() => a.Objects.Count() == 2 && b.Objects.Count() == 2));
        TUnit onB = Proxy(b, shared.NetId)!;

        rig.Server.Destroy(shared);
        T.Check("destroy removes the proxies", rig.Until(() => a.Objects.Count() == 1 && b.Objects.Count() == 1));
        T.Check("OnNetDestroy ran on server and client", shared.Destroyed && onB.Destroyed && onB.Session == null);

        uint aId = a.LocalConnectionId;
        var left = new List<uint>();
        rig.Server.ClientDisconnected += c => left.Add(c.Id);
        var gone = new List<NetObject>();
        b.ObjectDestroyed += gone.Add;
        a.Stop();
        T.Check("a leaving client is noticed", rig.Until(() => left.Count == 1) && left[0] == aId);
        T.Check("its objects are destroyed everywhere", rig.Until(() => rig.Server.Objects.Count() == 0 && b.Objects.Count() == 0) && mine.Destroyed);
        T.Check("the server forgot the connection", rig.Server.Connections.Count == 1 && rig.Server.FindConnection(b.LocalConnectionId) != null);

        var keep = rig.Server.Spawn<TUnit>(b.LocalConnectionId);
        rig.Until(() => b.Objects.Count() == 1);
        string why = "";
        b.Disconnected += r => why = r;
        rig.Server.Stop();
        T.Check("a server stop ends the client", rig.Until(() => why.Length > 0) && b.State == NetSessionState.Disconnected);
        T.Check("and removes its proxies", b.Objects.Count() == 0 && keep.Destroyed);
    }

    private static void Relevance(bool native)
    {
        using var rig = new Rig(native);
        rig.Server.RelevanceFilter = NetRelevance.Distance(1000);
        NetSession c = rig.Connected();
        rig.Server.Connections.First().ViewPosition = Vec3.Zero;
        var near = rig.Server.Spawn<TUnit>(); near.Where = new Vec3(100, 0, 0);
        var far = rig.Server.Spawn<TUnit>(); far.Where = new Vec3(5000, 0, 0); far.Health = 33;
        bool nearUp = rig.Until(() => Proxy(c, near.NetId) != null);
        rig.Settle(10);
        T.Check("only the near object is replicated", nearUp && Proxy(c, far.NetId) == null);

        far.Where = new Vec3(200, 0, 0);
        T.Check("moving into range spawns it, with its current state", rig.Until(() => Proxy(c, far.NetId) != null && Proxy(c, far.NetId)!.Health == 33));
        near.Where = new Vec3(9000, 0, 0);
        T.Check("moving out of range removes the proxy but not the object", rig.Until(() => Proxy(c, near.NetId) == null) && !near.Destroyed);
        rig.Server.Connections.First().ViewPosition = new Vec3(9000, 0, 0);
        T.Check("the viewer moving brings it back", rig.Until(() => Proxy(c, near.NetId) != null));

        var owned = rig.Server.Spawn<TUnit>(c.LocalConnectionId); owned.Where = new Vec3(-99999, 0, 0);
        T.Check("an owner always gets its object", rig.Until(() => Proxy(c, owned.NetId) != null));
        var multi = rig.Server.Spawn<TUnit>(); multi.Where = new Vec3(-99999, 0, 0);
        multi.DoBoom(2);
        rig.Settle(10);
        T.Check("a Multicast skips clients the object is not relevant to", Proxy(c, multi.NetId) == null && multi.Calls.Contains("boom:2"));
    }

    private static void Garbage()
    {
        using var rig = new Rig(false);
        NetSession good = rig.Connected();
        var u = rig.Server.Spawn<TUnit>(good.LocalConnectionId);

        LoopbackTransport raw = rig.Net!.CreateTransport();
        long peer = raw.Connect("", rig.Port);
        raw.Send(peer, true, new byte[] { 7, 1, 0, 0, 0, 1, (byte)'x' });
        rig.Settle(5);
        T.Check("an RPC from an unauthenticated peer is ignored", rig.Server.Connections.Count == 1 && rig.Server.Stats.RpcsRejected == 0 && rig.Server.Stats.Malformed == 0);

        var hello = new NetWriter(); hello.U8(1); hello.U32(NetSession.ProtocolVersion); hello.Str("raw");
        raw.Send(peer, true, hello.ToArray());
        rig.Settle(5);
        T.Check("a hand-made hello is accepted like any other", rig.Server.Connections.Count == 2);
        raw.Send(peer, true, new byte[] { 7, (byte)u.NetId, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F });
        raw.Send(peer, true, new byte[] { 7 });
        raw.Send(peer, true, Array.Empty<byte>());
        rig.Settle(5);
        T.Check("truncated and oversized packets are counted, not fatal", rig.Server.Stats.Malformed >= 2);

        var wrongVersion = new NetWriter(); wrongVersion.U8(1); wrongVersion.U32(99); wrongVersion.Str("old");
        LoopbackTransport raw2 = rig.Net.CreateTransport();
        long p2 = raw2.Connect("", rig.Port);
        raw2.Send(p2, true, wrongVersion.ToArray());
        rig.Settle(5);
        T.Check("a wrong protocol version is turned away", rig.Server.Connections.Count == 2);
        u.Health = 5;
        T.Check("the server still replicates", rig.Until(() => Proxy(good, u.NetId)!.Health == 5));
        raw.Dispose(); raw2.Dispose();
    }
}
