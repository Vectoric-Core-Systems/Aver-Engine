// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: SceneMultiplayer and RPC. Peers are branches of the one SceneTree, each with its own API and transport (see NetTestSupport.cs); the sockets are
// 127.0.0.1 only. Without build/bin/Aver.Net.Abi the socket tests are skipped; the in-memory ones always run.

using Aver.Fabric;
using GArray = Aver.Fabric.Collections.Array;
using GDict = Aver.Fabric.Collections.Dictionary;

static class MultiplayerTests
{
    public static int RunAll()
    {
        bool native = NetworkTests.EnsureNative();
        return T.Suite(() =>
        {
            T.Run("multiplayer: the default API offline, authority, rpc config, call_local, errors, per-branch APIs, Callable.rpc", Offline);
            T.Run("multiplayer: transfer modes and channels are the RPC's, on a link that drops unreliable packets", Modes);
            T.Run("multiplayer: a peer replaced, hostile packets, send_bytes, authentication on the in-memory link", MemMisc);
            if (!native)
            {
                Console.WriteLine("Test: multiplayer over sockets (SKIP: build/bin/Aver.Net.Abi was not found; build the Aver.Net.Abi target)");
                return;
            }
            T.Run("multiplayer over ENet: ids, peer list, signals, relayed peers, disconnects", Connect);
            T.Run("multiplayer over ENet: RPC delivery, call_local, rpc_id, negative targets, the remote sender, authority changes", Rpcs);
            T.Run("multiplayer over ENet: RPC rejections (authority, disabled, unknown, wrong node, arguments, throwing handler)", Rejections);
            T.Run("multiplayer over ENet: the node path cache and Variant arguments", PathCache);
            T.Run("multiplayer over ENet: server relay between clients, and with relay off", Relay);
            T.Run("multiplayer over ENet: send_bytes, authentication, refuse_new_connections", AuthAndRaw);
            T.Run("multiplayer over a lossy UDP link: reliable RPCs arrive once and in order, unreliable ones may not", Lossy);
        });
    }

    // ---- offline ----

    private static void Offline()
    {
        SceneTree tree = SceneTree.Instance;
        var n = new NetNode { Name = "N" };
        var kid = new NetNode { Name = "Kid" };
        n.AddChild(kid);
        T.Check("outside the tree there is no API", n.Multiplayer == null);
        T.Check("an RPC outside the tree is Unconfigured and reported", n.Rpc("Both", 1) == Error.Unconfigured && T.ErrorCount("not inside the tree") == 1);
        tree.Root.AddChild(n);
        MultiplayerApi mp = n.Multiplayer!;
        T.Check("the default API is a SceneMultiplayer over an OfflineMultiplayerPeer", mp is SceneMultiplayer && mp.MultiplayerPeer is OfflineMultiplayerPeer && mp.HasMultiplayerPeer());
        T.Check("offline: id 1, the server, no peers, no remote sender", mp.GetUniqueId() == 1 && mp.IsServer() && mp.GetPeers().Length == 0 && mp.GetRemoteSenderId() == 0);
        T.Check("every node is the server's by default", n.GetMultiplayerAuthority() == 1 && n.IsMultiplayerAuthority() && kid.GetMultiplayerAuthority() == 1);
        n.SetMultiplayerAuthority(7);
        T.Check("set_multiplayer_authority is recursive by default", n.GetMultiplayerAuthority() == 7 && kid.GetMultiplayerAuthority() == 7 && !n.IsMultiplayerAuthority());
        n.SetMultiplayerAuthority(1, false);
        T.Check("and can be applied to one node", n.GetMultiplayerAuthority() == 1 && kid.GetMultiplayerAuthority() == 7);
        kid.SetMultiplayerAuthority(1);

        GDict cfg = n.GetRpcConfig().AsGodotDictionary()!;
        T.Check("get_rpc_config lists the [Rpc] methods", cfg.ContainsKey("AuthCall") && cfg.ContainsKey("Both") && !cfg.ContainsKey("NotRpc"));
        GDict auth = cfg["AuthCall"].AsGodotDictionary()!, fast = cfg["Fast"].AsGodotDictionary()!, ordered = cfg["Ordered"].AsGodotDictionary()!, both = cfg["Both"].AsGodotDictionary()!;
        T.Check("defaults: authority, reliable, remote only, channel 0", auth["rpc_mode"].AsInt32() == 2 && auth["transfer_mode"].AsInt32() == 2 && !auth["call_local"].AsBool() && auth["channel"].AsInt32() == 0);
        T.Check("the attribute's options are the settings", fast["transfer_mode"].AsInt32() == 0 && ordered["transfer_mode"].AsInt32() == 1 && ordered["channel"].AsInt32() == 1
            && both["rpc_mode"].AsInt32() == 1 && both["call_local"].AsBool());

        T.Check("a call_local RPC runs here", n.Rpc("Both", 5) == Error.Ok && n.Log.SequenceEqual(new[] { "Both 5 <0>" }));
        T.Check("the name may be written the GDScript way", n.Rpc("both", 6) == Error.Ok && n.Log.Count == 2 && n.Log[1] == "Both 6 <0>");
        T.Check("a call_remote RPC offline does nothing (and is not an error)", n.Rpc("AuthCall", 1) == Error.Ok && n.Log.Count == 2);
        n.Log.Clear();
        T.Check("rpc_id to oneself runs a call_local method once", n.RpcId(1, "Both", 7) == Error.Ok && n.Log.SequenceEqual(new[] { "Both 7 <0>" }));
        T.Check("rpc_id to a peer that is not there is refused", n.RpcId(2, "Both", 8) == Error.InvalidParameter && T.ErrorCount("peer 2 is not connected") == 1);
        T.Check("a method with no [Rpc] cannot be called as one", n.Rpc("NotRpc") == Error.InvalidParameter && T.ErrorCount("RPC configuration") == 1);
        n.RpcConfig("NotRpc", new GDict { ["rpc_mode"] = 1, ["transfer_mode"] = 2, ["call_local"] = true, ["channel"] = 0 });
        n.Log.Clear();
        T.Check("rpc_config makes it one", n.Rpc("NotRpc") == Error.Ok && n.Log.SequenceEqual(new[] { "NotRpc" }) && n.GetRpcConfig().AsGodotDictionary()!.ContainsKey("NotRpc"));
        n.RpcConfig("NotRpc", default);
        T.Check("and nil takes it away again", !n.GetRpcConfig().AsGodotDictionary()!.ContainsKey("NotRpc") && n.Rpc("NotRpc") == Error.InvalidParameter);
        n.RpcConfig("not_rpc", new GDict { ["rpc_mode"] = 1, ["transfer_mode"] = 2, ["call_local"] = true, ["channel"] = 0 });
        T.Check("rpc_config with the GDScript spelling lands on the C# method", n.GetRpcConfig().AsGodotDictionary()!.ContainsKey("NotRpc") && !n.GetRpcConfig().AsGodotDictionary()!.ContainsKey("not_rpc"));
        n.RpcConfig("not_rpc", default);
        n.RpcConfig("Both", new GDict { ["rpc_mode"] = 1, ["transfer_mode"] = 2, ["call_local"] = false, ["channel"] = 0 });
        n.Log.Clear();
        n.Rpc("Both", 9);
        T.Check("an override replaces the attribute's settings", n.Log.Count == 0);
        n.RpcConfig("Both", new GDict { ["rpc_mode"] = 1, ["transfer_mode"] = 2, ["call_local"] = true, ["channel"] = 0 });

        n.Log.Clear();
        T.Check("Callable.rpc", new Callable(n, "Both").Rpc(11) == Error.Ok && n.Log.SequenceEqual(new[] { "Both 11 <0>" }));
        n.Log.Clear();
        T.Check("Callable.rpc_id", new Callable(n, "Both").RpcId(1, 12) == Error.Ok && n.Log.SequenceEqual(new[] { "Both 12 <0>" }));
        T.Check("a delegate cannot be an RPC", Callable.From(() => { }).Rpc() == Error.InvalidParameter);
        T.Check("MultiplayerAPI.rpc is the same call", n.Multiplayer!.Rpc(0, n, "Both", new GArray { 13 }) == Error.Ok && n.Log[^1] == "Both 13 <0>");

        // a branch with its own API
        var a = new Node { Name = "A" }; var b = new Node { Name = "B" };
        var ak = new NetNode { Name = "Deep" }; var bk = new NetNode { Name = "Deep" };
        a.AddChild(ak); b.AddChild(bk);
        tree.Root.AddChild(a); tree.Root.AddChild(b);
        var mpA = new SceneMultiplayer();
        tree.SetMultiplayer(mpA, a.GetPath());
        T.Check("SetMultiplayer with a path gives that branch its API", ak.Multiplayer == mpA && bk.Multiplayer == mp && a.Multiplayer == mpA && mpA.RootPath.ToString() == "/root/A");
        var inner = new SceneMultiplayer();
        ak.AddChild(new Node { Name = "In" });
        tree.SetMultiplayer(inner, ak.GetPath());
        T.Check("the deepest registered ancestor wins", ak.GetNode("In").Multiplayer == inner && ak.Multiplayer == inner && a.Multiplayer == mpA);
        T.Check("get_multiplayer for a path", tree.GetMultiplayer(new NodePath("/root/B/Deep")) == mp && tree.GetMultiplayer(new NodePath("/root/A")) == mpA && tree.GetMultiplayer() == mp);

        T.Check("multiplayer_poll is on by default", tree.MultiplayerPoll);
        MultiplayerApi created = MultiplayerApi.CreateDefaultInterface();
        T.Check("create_default_interface", created is SceneMultiplayer && MultiplayerApi.GetDefaultInterface().ToString() == "SceneMultiplayer");
        T.Check("assigning null puts the offline peer back", ((SceneMultiplayer)created).MultiplayerPeer is { } p0 && (created.MultiplayerPeer = null) == null && created.MultiplayerPeer is OfflineMultiplayerPeer && p0 != created.MultiplayerPeer);
    }

    // ---- the in-memory link ----

    private static void Modes()
    {
        var hub = new MemHub(5);
        NetCluster c = NetCluster.StartMem(hub, 2, NetCluster.GameOnly);
        T.Check("three peers see each other over the memory link", c.FullyConnected(2000));
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        hub.SentLog.Clear();
        s.Game.Rpc("AuthCall", 1);
        T.Check("a reliable RPC is sent reliably on channel 0 to everyone", hub.SentLog.Count == 1 && hub.SentLog[0].Mode == MultiplayerPeer.TransferModeEnum.Reliable && hub.SentLog[0].Channel == 0 && hub.SentLog[0].Target == 0);
        hub.SentLog.Clear();
        s.Game.Rpc("Fast", 2);
        T.Check("an unreliable RPC is sent unreliably", hub.SentLog.Count == 1 && hub.SentLog[0].Mode == MultiplayerPeer.TransferModeEnum.Unreliable && hub.SentLog[0].Channel == 0);
        hub.SentLog.Clear();
        s.Game.RpcId(a.Id, "Ordered", 3);
        T.Check("an unreliable-ordered RPC on channel 1 to one peer", hub.SentLog.Count == 1 && hub.SentLog[0].Mode == MultiplayerPeer.TransferModeEnum.UnreliableOrdered && hub.SentLog[0].Channel == 1 && hub.SentLog[0].Target == a.Id);
        c.Until(() => a.Game.Log.Count >= 3, 2000);
        T.Check("all three arrived over a clean link", a.Game.Log.Contains("AuthCall 1 <1>") && a.Game.Log.Contains("Fast 2") && a.Game.Log.Contains("Ordered 3") && b.Game.Log.Contains("Fast 2") && !b.Game.Log.Contains("Ordered 3"));
        a.Game.Log.Clear(); b.Game.Log.Clear();

        hub.Loss = 1.0;
        for (int i = 0; i < 5; i++) { s.Game.Rpc("Fast", i); s.Game.Rpc("AuthCall", i); }
        c.Settle(150);
        T.Check("a link that drops every unreliable packet still delivers the reliable RPCs, in order",
            a.Game.Log.SequenceEqual(Enumerable.Range(0, 5).Select(i => $"AuthCall {i} <1>")) && b.Game.Log.Count == 5, string.Join(",", a.Game.Log));
        T.Check("and the dropped ones never arrive", !a.Game.Log.Any(l => l.StartsWith("Fast")) && hub.Dropped >= 10);
        a.Game.Log.Clear();

        hub.Loss = 0; hub.MaxDelayTicks = 6;
        for (int i = 0; i < 40; i++) { s.Game.RpcId(a.Id, "Ordered", i); c.Step(); }
        c.Settle(200);
        var seen = a.Game.Log.Where(l => l.StartsWith("Ordered")).Select(l => int.Parse(l.Split(' ')[1])).ToList();
        T.Check("unreliable-ordered RPCs on a reordering link arrive in increasing order (a late one is dropped, not delivered late)", seen.Count > 20 && seen.SequenceEqual(seen.OrderBy(x => x)), string.Join(",", seen));
        c.Close();
    }

    private static void MemMisc()
    {
        var hub = new MemHub(11);
        NetCluster c = NetCluster.StartMem(hub, 1, NetCluster.GameOnly);
        c.FullyConnected(2000);
        NetPeer s = c.Server, a = c.C(1);

        // hostile packets: whatever arrives, the API drops it and keeps working
        var rng = new Random(2);
        int errorsBefore = T.ErrorCount();
        for (int i = 0; i < 200; i++)
        {
            var junk = new byte[rng.Next(1, 40)];
            rng.NextBytes(junk);
            junk[0] = (byte)(junk[0] % 14);
            a.Net.SetTargetPeer(1);
            a.Net.TransferMode = MultiplayerPeer.TransferModeEnum.Reliable;
            a.Net.PutPacket(junk);
        }
        c.Settle(100);
        s.Game.Log.Clear();
        a.Game.Rpc("Any", "still alive");
        c.Settle(100);
        T.Check("200 random packets are dropped and the next RPC still arrives", s.Game.Log.SequenceEqual(new[] { $"Any still alive <{a.Id}>" }), string.Join(",", s.Game.Log));
        T.Check("the junk was reported, not thrown", T.ErrorCount() > errorsBefore);

        // send_bytes / peer_packet
        var got = new List<string>();
        s.Mp.PeerPacket += (id, data) => got.Add($"{id}:{string.Join(",", data)}");
        T.Check("send_bytes to the server", a.Mp.SendBytes(new byte[] { 1, 2, 3 }, 1) == Error.Ok);
        c.Settle(60);
        T.Check("arrives as peer_packet with the sender's id", got.SequenceEqual(new[] { $"{a.Id}:1,2,3" }), string.Join(";", got));
        T.Check("an empty packet is refused", a.Mp.SendBytes(System.Array.Empty<byte>()) == Error.InvalidParameter);
        hub.SentLog.Clear();
        a.Mp.SendBytes(new byte[] { 9 }, 1, MultiplayerPeer.TransferModeEnum.Unreliable, 1);
        T.Check("send_bytes uses the mode and channel it is given", hub.SentLog.Count == 1 && hub.SentLog[0].Mode == MultiplayerPeer.TransferModeEnum.Unreliable && hub.SentLog[0].Channel == 1);

        // a peer replaced: the old peers are forgotten without signals
        var offline = new OfflineMultiplayerPeer();
        s.Events.Clear();
        s.Mp.MultiplayerPeer = offline;
        T.Check("replacing the peer clears the peer list quietly", s.Mp.GetPeers().Length == 0 && s.Events.Count == 0 && s.Mp.IsServer() && s.Mp.GetUniqueId() == 1);
        T.Check("a disconnected peer is refused", s.Mp.MultiplayerPeer == offline && ReplaceWithClosed(s.Mp) && T.ErrorCount("connecting or connected") == 1);
        c.Close();
    }

    private static bool ReplaceWithClosed(SceneMultiplayer mp)
    {
        var closed = new ENetMultiplayerPeer();   // never created: Disconnected
        mp.MultiplayerPeer = closed;
        return mp.MultiplayerPeer != closed;
    }

    // ---- over ENet ----

    private static void Connect()
    {
        NetCluster c = NetCluster.Start(2, NetCluster.GameOnly);
        T.Check("server, client 1 and client 2 all see two peers", c.FullyConnected());
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        T.Check("ids: the server is 1, clients have distinct ids >= 2", s.Id == 1 && a.Id >= 2 && b.Id >= 2 && a.Id != b.Id && s.Mp.IsServer() && !a.Mp.IsServer());
        T.Check("the server lists both clients, ascending", s.Mp.GetPeers().SequenceEqual(new[] { a.Id, b.Id }.OrderBy(x => x)));
        T.Check("a client lists the server and (relayed) the other client", a.Mp.GetPeers().SequenceEqual(new[] { 1, b.Id }.OrderBy(x => x)) && b.Mp.GetPeers().SequenceEqual(new[] { 1, a.Id }.OrderBy(x => x)));
        T.Check("the server saw one peer_connected per client", s.Events.Count(e => e.StartsWith("connected ")) == 2 && s.Events.Contains("connected " + a.Id) && s.Events.Contains("connected " + b.Id));
        T.Check("a client: peer_connected(1), then connected_to_server, then the other client", a.Events.Take(2).SequenceEqual(new[] { "connected 1", "connected_to_server" }) && a.Events.Contains("connected " + b.Id), string.Join(",", a.Events));
        T.Check("connected_to_server is emitted once and not on the server", a.Events.Count(e => e == "connected_to_server") == 1 && !s.Events.Contains("connected_to_server"));

        // a client leaves: the server and the other client hear of it
        s.Events.Clear(); a.Events.Clear(); b.Events.Clear();
        int bid = b.Id;
        b.Net.Close();
        T.Check("the server reports the departure", c.Until(() => s.Events.Contains("disconnected " + bid)));
        T.Check("the other client is told through the server", c.Until(() => a.Events.Contains("disconnected " + bid)));
        T.Check("the lists shrink", s.Mp.GetPeers().SequenceEqual(new[] { a.Id }) && a.Mp.GetPeers().SequenceEqual(new[] { 1 }));
        c.Step();
        T.Check("the leaver gets no server_disconnected for closing itself... status change is reported on its next poll", b.Events.Count(e => e == "server_disconnected") <= 1 && b.Mp.GetPeers().Length == 0);

        // the server goes away
        a.Events.Clear();
        s.Mp.DisconnectPeer(a.Id);
        T.Check("disconnect_peer on the server drops the client", c.Until(() => a.Events.Contains("disconnected 1")));
        T.Check("the client reports server_disconnected after the peer_disconnected", c.Until(() => a.Events.Contains("server_disconnected")) && a.Events.IndexOf("server_disconnected") > a.Events.IndexOf("disconnected 1"), string.Join(",", a.Events));
        c.Close();

        // connection_failed
        var mp = new SceneMultiplayer();
        var failed = new ENetMultiplayerPeer();
        failed.CreateClient("127.0.0.1", 9);
        ((ENetPacketPeer)failed.Host!.GetPeers()[0].AsGodotObject()!).SetTimeout(4, 600, 2000);
        var fails = 0;
        mp.ConnectionFailed += () => fails++;
        mp.MultiplayerPeer = failed;
        T.Check("a connect that never completes ends in connection_failed", NetworkTestsUntil(() => fails == 1, 9000, mp.Poll) && failed.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected);
        failed.Close();
    }

    private static bool NetworkTestsUntil(Func<bool> done, int ms, Action pump)
    {
        long end = System.Environment.TickCount64 + ms;
        while (System.Environment.TickCount64 < end) { pump(); if (done()) return true; Thread.Sleep(1); }
        pump();
        return done();
    }

    private static void Rpcs()
    {
        NetCluster c = NetCluster.Start(2, NetCluster.GameOnly);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);

        s.Game.Rpc("AuthCall", 1);
        T.Check("the authority's RPC reaches every client, with the server as the sender", c.Until(() => a.Game.Log.Count == 1 && b.Game.Log.Count == 1) && a.Game.Log[0] == "AuthCall 1 <1>" && b.Game.Log[0] == "AuthCall 1 <1>");
        T.Check("a call_remote RPC does not run on the caller", s.Game.Log.Count == 0);
        T.Check("outside an RPC the remote sender is 0", s.Mp.GetRemoteSenderId() == 0 && a.Mp.GetRemoteSenderId() == 0);

        a.Game.Rpc("Any", "hi");
        T.Check("an any_peer RPC from a client reaches the server and the other client", c.Until(() => s.Game.Log.Contains($"Any hi <{a.Id}>") && b.Game.Log.Contains($"Any hi <{a.Id}>")));
        T.Check("not the caller", !a.Game.Log.Any(l => l.StartsWith("Any")));

        s.Game.Rpc("Both", 4);
        T.Check("call_local also runs it on the caller, sender 0", s.Game.Log.Contains("Both 4 <0>") && c.Until(() => a.Game.Log.Contains("Both 4 <1>") && b.Game.Log.Contains("Both 4 <1>")));

        foreach (NetPeer p in c.Peers) p.Game.Log.Clear();
        s.Game.RpcId(a.Id, "AuthCall", 2);
        c.Until(() => a.Game.Log.Count == 1);
        c.Settle(60);
        T.Check("rpc_id reaches that peer only", a.Game.Log.SequenceEqual(new[] { "AuthCall 2 <1>" }) && b.Game.Log.Count == 0 && s.Game.Log.Count == 0);
        foreach (NetPeer p in c.Peers) p.Game.Log.Clear();
        s.Game.RpcId(-a.Id, "AuthCall", 3);
        c.Until(() => b.Game.Log.Count == 1);
        c.Settle(60);
        T.Check("a negative id reaches everyone but that peer", b.Game.Log.SequenceEqual(new[] { "AuthCall 3 <1>" }) && a.Game.Log.Count == 0);

        a.Game.RpcId(1, "Any", "only you");
        T.Check("a client's rpc_id(1) is the server alone", c.Until(() => s.Game.Log.Contains($"Any only you <{a.Id}>")) && !b.Game.Log.Any(l => l.Contains("only you")));

        // transfer modes over the real transport
        foreach (NetPeer p in c.Peers) p.Game.Log.Clear();
        s.Game.Rpc("Fast", 5);
        s.Game.Rpc("Ordered", 6);
        T.Check("an unreliable and an unreliable-ordered (channel 1) RPC arrive on loopback", c.Until(() => a.Game.Log.Contains("Fast 5") && a.Game.Log.Contains("Ordered 6") && b.Game.Log.Contains("Ordered 6")));

        // authority moves
        c.SetAuthority("Game", a.Id);
        T.Check("is_multiplayer_authority follows set_multiplayer_authority", a.Game.IsMultiplayerAuthority() && !s.Game.IsMultiplayerAuthority() && !b.Game.IsMultiplayerAuthority() && s.Game.GetMultiplayerAuthority() == a.Id);
        foreach (NetPeer p in c.Peers) p.Game.Log.Clear();
        a.Game.Rpc("AuthCall", 7);
        T.Check("the new authority may call authority RPCs", c.Until(() => s.Game.Log.Count == 1 && b.Game.Log.Count == 1) && s.Game.Log[0] == $"AuthCall 7 <{a.Id}>");
        int errs = T.ErrorCount("not allowed");
        s.Game.Rpc("AuthCall", 8);
        T.Check("and the old one may not", c.Until(() => T.ErrorCount("not allowed") == errs + 2) && a.Game.Log.Count == 0 && b.Game.Log.Count == 1);
        c.Close();
    }

    private static void Rejections()
    {
        NetCluster c = NetCluster.Start(1, NetCluster.GameOnly);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1);
        int err0 = T.ErrorCount();

        a.Game.Rpc("AuthCall", 1);
        T.Check("a client calling an authority-mode RPC is refused by the receiver, with the mode and authority in the message",
            c.Until(() => T.ErrorCount("not allowed on node '/root/S/Game' from peer " + a.Id) == 1) && s.Game.Log.Count == 0 && T.Logs.Any(l => l.Message.Contains("Mode is authority, authority is 1")));

        s.Game.RpcConfig("Off", new GDict { ["rpc_mode"] = 0, ["transfer_mode"] = 2, ["call_local"] = false, ["channel"] = 0 });
        a.Game.RpcConfig("Off", new GDict { ["rpc_mode"] = 1, ["transfer_mode"] = 2, ["call_local"] = false, ["channel"] = 0 });
        a.Game.Rpc("Off");
        T.Check("a disabled method refuses every call", c.Until(() => T.ErrorCount("'Off' is not allowed") == 1) && !s.Game.Log.Contains("Off"));

        a.Game.RpcConfig("Ghost", new GDict { ["rpc_mode"] = 1, ["transfer_mode"] = 2, ["call_local"] = false, ["channel"] = 0 });
        a.Game.Rpc("Ghost");
        T.Check("a method the receiver does not expose as an RPC is refused there", c.Until(() => T.ErrorCount("called 'Ghost' on '/root/S/Game', which is not an RPC method") == 1));

        var extra = new NetNode { Name = "OnlyHere" };
        a.Root.AddChild(extra);
        extra.Rpc("Any", "x");
        T.Check("a node the receiver does not have is reported there", c.Until(() => T.ErrorCount("'OnlyHere', which does not exist here") == 1));
        a.Game.Rpc("Any", "after");
        T.Check("the next RPC is delivered", c.Until(() => s.Game.Log.Contains($"Any after <{a.Id}>")));

        a.Game.Rpc("Throws");
        T.Check("a handler that throws is reported, not propagated", c.Until(() => T.ErrorCount("failed: InvalidOperationException: boom") == 1));
        a.Game.Rpc("Any", "after throw");
        T.Check("and the remote sender id is reset", c.Until(() => s.Game.Log.Contains($"Any after throw <{a.Id}>")) && s.Mp.GetRemoteSenderId() == 0);

        T.Check("an RPC to a method that does not exist on the sender is refused locally", a.Game.Rpc("Nope") == Error.InvalidParameter);
        T.Check("an RPC while connecting is refused", RpcWhileConnecting());
        T.Check("an RPC to a node outside the branch is refused", OutsideBranch(a));
        T.Check("the rejections were all errors in the log", T.ErrorCount() >= err0 + 6);
        c.Close();
    }

    private static bool RpcWhileConnecting()
    {
        var mp = new SceneMultiplayer();
        var net = new ENetMultiplayerPeer();
        net.CreateClient("127.0.0.1", 9);
        mp.MultiplayerPeer = net;
        var n = new NetNode { Name = "Lone" };
        SceneTree.Instance.Root.AddChild(n);
        SceneTree.Instance.SetMultiplayer(mp, n.GetPath());
        Error e = n.Rpc("Any", "x");
        net.Close();
        return e == Error.Unconfigured && T.ErrorCount("not connected yet") == 1;
    }

    private static bool OutsideBranch(NetPeer p)
    {
        var stray = new NetNode { Name = "Stray" };
        SceneTree.Instance.Root.AddChild(stray);
        Error e = p.Mp.Rpc(0, stray, "Any", new GArray { "x" });
        return e == Error.InvalidParameter && T.ErrorCount("not inside the branch") == 1;
    }

    private static void PathCache()
    {
        NetCluster c = NetCluster.Start(1, NetCluster.GameOnly);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1);
        int full0 = s.Mp.FullRefsSent;
        s.Game.Rpc("AuthCall", 1);
        s.Game.Rpc("AuthCall", 2);
        T.Check("the first packets to a node carry its path (nothing is confirmed yet)", s.Mp.FullRefsSent - full0 == 2 && s.Mp.CachedRefsSent == 0);
        c.Until(() => a.Game.Log.Count == 2);
        c.Settle(60);
        int cached0 = s.Mp.CachedRefsSent, full1 = s.Mp.FullRefsSent;
        s.Game.Rpc("AuthCall", 3);
        s.Game.Rpc("AuthCall", 4);
        T.Check("once the receiver confirmed the path, later packets carry only the id", s.Mp.CachedRefsSent - cached0 == 2 && s.Mp.FullRefsSent == full1);
        T.Check("and they are delivered", c.Until(() => a.Game.Log.Count == 4) && a.Game.Log[3] == "AuthCall 4 <1>");

        // every Variant kind an RPC argument may be
        s.Game.Rpc("Any", "x");
        a.Game.Log.Clear();
        var arr = new GArray { 1, "two", 3.5 };
        var dict = new GDict { ["k"] = new Vector3(1, 2, 3), ["j"] = true };
        a.Game.Rpc("Args", 42, "text", new Vector3(1, 2, 3), arr, dict, new byte[] { 1, 2, 3, 4, 5 });
        T.Check("int, string, Vector3, Array, Dictionary and bytes go through the Variant codec",
            c.Until(() => s.Game.Log.Any(l => l.StartsWith("Args"))) && s.Game.Log.Contains("Args 42 text (1, 2, 3) 3 2 5"), string.Join("|", s.Game.Log));

        // a node freed and a new one of the same name: the new node gets a new id
        var extra = new NetNode { Name = "Temp" }; var extra2 = new NetNode { Name = "Temp" };
        s.Root.AddChild(extra); a.Root.AddChild(extra2);
        extra.Rpc("Any", "t1");
        T.Check("a node added later is reachable", c.Until(() => extra2.Log.Count == 1));
        extra.Free(); extra2.Free();
        var extra3 = new NetNode { Name = "Temp" }; var extra4 = new NetNode { Name = "Temp" };
        s.Root.AddChild(extra3); a.Root.AddChild(extra4);
        extra3.Rpc("Any", "t2");
        T.Check("and so is its replacement", c.Until(() => extra4.Log.Count == 1) && extra4.Log[0].StartsWith("Any t2"));
        c.Close();
    }

    private static void Relay()
    {
        NetCluster c = NetCluster.Start(2, NetCluster.GameOnly);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        a.Game.RpcId(b.Id, "Any", "a to b");
        T.Check("a client's rpc_id to another client goes through the server and keeps the original sender",
            c.Until(() => b.Game.Log.Contains($"Any a to b <{a.Id}>")) && s.Game.Log.Count == 0 && a.Game.Log.Count == 0);
        b.Game.RpcId(a.Id, "Any", "b to a");
        T.Check("and the other way", c.Until(() => a.Game.Log.Contains($"Any b to a <{b.Id}>")));
        foreach (NetPeer p in c.Peers) p.Game.Log.Clear();
        a.Game.RpcId(-b.Id, "Any", "not b");
        T.Check("a negative target from a client is everyone but that peer (the server here)", c.Until(() => s.Game.Log.Contains($"Any not b <{a.Id}>")));
        c.Settle(80);
        T.Check("the excluded client got nothing, nor did the caller", b.Game.Log.Count == 0 && a.Game.Log.Count == 0);
        // path caches work through the relay: a second RPC to the same client carries only the id
        int cachedBefore = a.Mp.CachedRefsSent;
        a.Game.RpcId(b.Id, "Any", "again");
        c.Until(() => b.Game.Log.Contains($"Any again <{a.Id}>"));
        T.Check("the receiving client's path confirmation travelled back through the relay", a.Mp.CachedRefsSent > cachedBefore);
        c.Close();

        NetCluster n = NetCluster.Start(2, NetCluster.GameOnly, relay: false);
        n.Until(() => n.Server.Mp.GetPeers().Length == 2 && n.C(1).Mp.GetPeers().Length >= 1 && n.C(2).Mp.GetPeers().Length >= 1);
        n.Settle(100);
        T.Check("with server_relay off the clients are not told of each other", n.C(1).Mp.GetPeers().SequenceEqual(new[] { 1 }) && n.C(2).Mp.GetPeers().SequenceEqual(new[] { 1 }) && n.Server.Mp.GetPeers().Length == 2);
        int errs = T.ErrorCount();
        T.Check("and cannot send to each other", n.C(1).Game.RpcId(n.C(2).Id, "Any", "x") == Error.InvalidParameter && T.ErrorCount() > errs);
        n.C(1).Game.Rpc("Any", "to the server");
        T.Check("a broadcast from a client is the server alone", n.Until(() => n.Server.Game.Log.Count == 1) && n.C(2).Game.Log.Count == 0);
        n.Close();
    }

    private static void AuthAndRaw()
    {
        var trail = new List<string>();
        NetCluster auth = NetClusterWithAuth(trail, out NetPeer sv, out NetPeer cl);
        T.Check("both sides are told a peer is authenticating", auth.Until(() => sv.Events.Contains("authenticating " + cl.Id) && cl.Events.Contains("authenticating 1")));
        T.Check("it is not a peer yet", sv.Mp.GetPeers().Length == 0 && sv.Mp.GetAuthenticatingPeers().SequenceEqual(new[] { cl.Id }) && cl.Mp.GetAuthenticatingPeers().SequenceEqual(new[] { 1 }));
        T.Check("send_auth reaches the other side's auth_callback", auth.Until(() => trail.Contains($"S got {cl.Id}:hello server") && trail.Contains("C got 1:hello client")), string.Join("|", trail));
        T.Check("complete_auth on one side admits nobody", sv.Mp.CompleteAuth(cl.Id) == Error.Ok && sv.Mp.CompleteAuth(cl.Id) == Error.FileBadDrive && sv.Mp.GetPeers().Length == 0);
        auth.Settle(60);
        T.Check("until the other side completes too", sv.Mp.GetPeers().Length == 0);
        cl.Mp.CompleteAuth(1);
        T.Check("then both admit the peer", auth.Until(() => sv.Mp.GetPeers().SequenceEqual(new[] { cl.Id }) && cl.Mp.GetPeers().SequenceEqual(new[] { 1 })) && sv.Mp.GetAuthenticatingPeers().Length == 0);
        sv.Game.Rpc("AuthCall", 1);
        T.Check("and RPCs flow", auth.Until(() => cl.Game.Log.Count == 1));
        T.Check("send_auth for a peer that is not authenticating is refused", sv.Mp.SendAuth(cl.Id, new byte[] { 1 }) == Error.InvalidParameter && sv.Mp.CompleteAuth(999) == Error.InvalidParameter);
        auth.Close();

        // the timeout
        NetCluster slow = NetClusterWithAuth(new List<string>(), out NetPeer sv2, out NetPeer cl2);
        sv2.Mp.AuthTimeout = 0.3;
        sv2.Events.Clear();
        long t0 = System.Environment.TickCount64;
        T.Check("a peer that never completes is dropped after auth_timeout (the frame clock) and peer_authentication_failed is emitted",
            slow.Until(() => sv2.Events.Contains("auth_failed " + cl2.Id)) && sv2.Mp.GetPeers().Length == 0);
        T.Check("the client is disconnected from the server", slow.Until(() => cl2.Net.GetConnectionStatus() == MultiplayerPeer.ConnectionStatus.Disconnected, 8000));
        slow.Close();

        // refuse_new_connections goes to the peer
        var mp = new SceneMultiplayer { RefuseNewConnections = true };
        var net = new ENetMultiplayerPeer();
        net.SetBindIp("127.0.0.1");
        net.CreateServer(0, 4);
        mp.MultiplayerPeer = net;
        T.Check("refuse_new_connections is applied to the peer", mp.RefuseNewConnections && net.RefuseNewConnections);
        mp.RefuseNewConnections = false;
        T.Check("and cleared", !net.RefuseNewConnections);
        net.Close();
    }

    private static NetCluster NetClusterWithAuth(List<string> trail, out NetPeer server, out NetPeer client)
    {
        var cluster = new NetCluster();
        var net = new ENetMultiplayerPeer();
        net.SetBindIp("127.0.0.1");
        net.CreateServer(0, 4, 2);
        NetPeer sv = AddAuthPeer(cluster, "S", net, trail, "S");
        var cn = new ENetMultiplayerPeer();
        cn.CreateClient("127.0.0.1", net.Host!.GetLocalPort(), 2);
        NetPeer cl = AddAuthPeer(cluster, "C", cn, trail, "C");
        server = sv; client = cl;
        return cluster;
    }

    private static NetPeer AddAuthPeer(NetCluster cluster, string name, ENetMultiplayerPeer net, List<string> trail, string tag)
    {
        var p = new NetPeer { Name = name, Root = new Node { Name = name }, Mp = new SceneMultiplayer(), Net = net };
        SceneTree.Instance.Root.AddChild(p.Root);
        SceneTree.Instance.SetMultiplayer(p.Mp, p.Root.GetPath());
        p.Root.AddChild(new NetNode { Name = "Game" });
        p.Record();
        SceneMultiplayer mp = p.Mp;
        mp.AuthCallback = Callable.From<long, byte[]>((id, data) => trail.Add($"{tag} got {id}:{System.Text.Encoding.UTF8.GetString(data)}"));
        mp.PeerAuthenticating += id =>
        {
            p.Events.Add("authenticating " + id);
            mp.SendAuth(id, System.Text.Encoding.UTF8.GetBytes(tag == "S" ? "hello client" : "hello server"));
        };
        mp.PeerAuthenticationFailed += id => p.Events.Add("auth_failed " + id);
        mp.MultiplayerPeer = net;
        cluster.Peers.Add(p);
        return p;
    }

    // ---- the lossy link ----

    private static void Lossy()
    {
        NetCluster c = NetCluster.Start(1, NetCluster.GameOnly, lossy: true, loss: 0.15, dup: 0.1, minDelay: 0, maxDelay: 30, seed: 4);
        T.Check("the handshake completes through a link that drops 15% of the datagrams", c.FullyConnected(40000));
        NetPeer s = c.Server, a = c.C(1);
        const int count = 40;
        for (int i = 0; i < count; i++)
        {
            s.Game.Rpc("AuthCall", i);
            s.Game.Rpc("Fast", i);
            a.Game.Rpc("Any", i.ToString());
            c.Step();
        }
        bool done = c.Until(() => a.Game.Log.Count(l => l.StartsWith("AuthCall")) == count && s.Game.Log.Count(l => l.StartsWith("Any")) == count, 60000);
        T.Check("every reliable RPC arrives, in both directions, despite the loss", done, $"{a.Game.Log.Count(l => l.StartsWith("AuthCall"))}/{count}, {s.Game.Log.Count(l => l.StartsWith("Any"))}/{count}");
        var order = a.Game.Log.Where(l => l.StartsWith("AuthCall")).Select(l => int.Parse(l.Split(' ')[1])).ToList();
        T.Check("each exactly once and in order", order.SequenceEqual(Enumerable.Range(0, count)));
        var anyOrder = s.Game.Log.Where(l => l.StartsWith("Any")).Select(l => int.Parse(l.Split(' ')[1])).ToList();
        T.Check("the client's too", anyOrder.SequenceEqual(Enumerable.Range(0, count)));
        int fast = a.Game.Log.Count(l => l.StartsWith("Fast"));
        T.Check("unreliable RPCs may be lost, never invented more than once each", fast <= count + 4 && fast > 0, "arrived: " + fast);
        T.Check("the proxy really dropped and duplicated datagrams", c.Proxy!.Dropped > 10 && c.Proxy.Duplicated > 3, $"dropped {c.Proxy.Dropped}, duplicated {c.Proxy.Duplicated}");
        // the path is carried until it is confirmed; the confirmation is reliable, so it gets there
        c.Settle(300);
        int cached0 = s.Mp.CachedRefsSent;
        s.Game.Rpc("AuthCall", 99);
        T.Check("the path confirmation survived the loss: later packets carry only the id", s.Mp.CachedRefsSent > cached0);
        c.Close();
    }
}
