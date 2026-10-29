// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: MultiplayerSpawner, MultiplayerSynchronizer and SceneReplicationConfig between peers that are branches of the one tree (NetTestSupport.cs), over ENet on
// 127.0.0.1, over the lossy UDP proxy and over the in-memory link (whose clock is the frame's delta, so intervals can be counted exactly); and a GDScript with @rpc
// converted, compiled and called across peers.

using System.Reflection;
using Aver.Fabric;
using Aver.Fabric.Convert;
using GArray = Aver.Fabric.Collections.Array;

/// <summary>A player: its name is its owner's peer id, and it takes that authority when it enters the tree (the usual Godot pattern).</summary>
class NetPlayer : NetNode
{
    public readonly List<string> Seen = new();

    public override void _EnterTree()
    {
        Seen.Add($"enter {Name} tag={Tag} score={Score} pos={Pos}");
        if (int.TryParse(Name.ToString(), out int id)) SetMultiplayerAuthority(id);
    }
}

static class ReplicationTests
{
    public static int RunAll()
    {
        bool native = NetworkTests.EnsureNative();
        return T.Suite(() =>
        {
            T.Run("replication: SceneReplicationConfig and MultiplayerSynchronizer API", ConfigApi);
            T.Run("replication on the in-memory link: sync intervals counted in frames, stale packets dropped, deltas only on change", MemSync);
            T.Run("replication on the in-memory link: visibility_update_mode physics looks at the filters on physics steps only", MemVisibilityModes);
            T.Run("scripts: a converted GDScript with @rpc, rpc, rpc_id, Callable.rpc and multiplayer.* is compiled and called across peers", ConvertedScript);
            if (!native)
            {
                Console.WriteLine("Test: replication over sockets (SKIP: build/bin/Aver.Net.Abi was not found; build the Aver.Net.Abi target)");
                return;
            }
            T.Run("spawner over ENet: auto-spawn with spawn state, names, signals, despawn, limit, the non-authority", SpawnerBasics);
            T.Run("spawner over ENet: late joiners get what exists, custom spawn with data, visibility spawns and despawns", SpawnerMore);
            T.Run("synchronizer over ENet: always / on change / spawn modes, authority, authority changes, late joiner state", SyncModes);
            T.Run("synchronizer over ENet: visibility (public, per peer, filters, update modes), size limit, signals", SyncVisibility);
            T.Run("replication over a lossy UDP link: spawn, sync, delta and despawn converge", LossyReplication);
        });
    }

    // ---- the world every peer builds ----

    private static SceneReplicationConfig PlayerConfig()
    {
        var c = new SceneReplicationConfig();
        c.AddProperty(new NodePath(".:Tag"));
        c.PropertySetReplicationMode(new NodePath(".:Tag"), SceneReplicationConfig.ReplicationMode.Never);   // spawn only
        c.AddProperty(new NodePath(".:Pos"));                                                                  // spawn + every sync
        c.AddProperty(new NodePath(".:Hp"));
        c.PropertySetSpawn(new NodePath(".:Hp"), false);                                                       // every sync
        c.AddProperty(new NodePath(".:Score"));
        c.PropertySetSpawn(new NodePath(".:Score"), false);
        c.PropertySetReplicationMode(new NodePath(".:Score"), SceneReplicationConfig.ReplicationMode.OnChange);
        return c;
    }

    private static PackedScene PlayerScene()
    {
        var player = new NetPlayer { Name = "Player" };
        player.AddChild(new MultiplayerSynchronizer { Name = "Sync", ReplicationConfig = PlayerConfig() });
        var scene = new PackedScene();
        scene.Pack(player);
        return scene;
    }

    private static void World(NetPeer p)
    {
        ResourceLoader.Provider ??= (path, type) => path == "res://player.tscn" ? s_scene ??= PlayerScene() : null;
        var game = new NetNode { Name = "Game" };
        p.Root.AddChild(game);
        p.Root.AddChild(new Node { Name = "Players" });
        var sp = new MultiplayerSpawner { Name = "Spawner", SpawnPath = new NodePath("../Players") };
        sp.AddSpawnableScene("res://player.tscn");
        sp.SpawnFunction = Callable.From<Variant, Node>(d => new NetPlayer { Name = "custom", Tag = d.AsString() });
        p.Root.AddChild(sp);
        var cfg = new SceneReplicationConfig();
        cfg.AddProperty(new NodePath(".:Hp"));
        cfg.PropertySetSpawn(new NodePath(".:Hp"), false);
        cfg.AddProperty(new NodePath(".:Score"));
        cfg.PropertySetSpawn(new NodePath(".:Score"), false);
        cfg.PropertySetReplicationMode(new NodePath(".:Score"), SceneReplicationConfig.ReplicationMode.OnChange);
        cfg.AddProperty(new NodePath(".:Tag"));
        cfg.PropertySetSpawn(new NodePath(".:Tag"), false);
        p.Root.AddChild(new MultiplayerSynchronizer { Name = "GameSync", RootPath = new NodePath("../Game"), ReplicationConfig = cfg });
        sp.Spawned += n => p.Events.Add("spawned " + n.Name);
        sp.Despawned += n => p.Events.Add("despawned " + n.Name);
    }

    private static PackedScene? s_scene;

    private static NetPlayer? PlayerOf(NetPeer p, string name) => p.Root.GetNodeOrNull<NetPlayer>("Players/" + name);

    private static NetPlayer SpawnPlayer(NetPeer on, int owner, string tag = "", Vector3 pos = default, bool addNow = true)
    {
        var scene = (PackedScene)ResourceLoader.Load("res://player.tscn", typeof(PackedScene))!;
        var pl = (NetPlayer)scene.Instantiate();
        pl.Name = owner.ToString();
        pl.Tag = tag;
        pl.Pos = pos;
        if (addNow) on.Root.GetNode("Players").AddChild(pl);
        return pl;
    }

    // ---- API ----

    private static void ConfigApi()
    {
        var c = new SceneReplicationConfig();
        c.AddProperty(new NodePath(".:position"));
        c.AddProperty(new NodePath("Model:visible"));
        c.AddProperty(new NodePath(".:position"));
        T.Check("add_property refuses a duplicate", c.GetProperties().Count == 2 && T.ErrorCount("already in the list") == 1);
        c.AddProperty(new NodePath("Model"));
        T.Check("and a path that names no property", c.GetProperties().Count == 2 && T.ErrorCount("names no property") + T.ErrorCount("does not name a property") == 1);
        c.AddProperty(new NodePath(".:hp"), 0);
        T.Check("add_property takes an index; property_get_index", c.PropertyGetIndex(new NodePath(".:hp")) == 0 && c.PropertyGetIndex(new NodePath("Model:visible")) == 2 && c.HasProperty(new NodePath(".:hp")));
        T.Check("defaults: spawn on, replicated every sync", c.PropertyGetSpawn(new NodePath(".:hp")) && c.PropertyGetReplicationMode(new NodePath(".:hp")) == SceneReplicationConfig.ReplicationMode.Always && c.PropertyGetSync(new NodePath(".:hp")) && !c.PropertyGetWatch(new NodePath(".:hp")));
        c.PropertySetWatch(new NodePath(".:hp"), true);
        T.Check("property_set_watch is mode on-change", c.PropertyGetReplicationMode(new NodePath(".:hp")) == SceneReplicationConfig.ReplicationMode.OnChange && c.PropertyGetWatch(new NodePath(".:hp")) && !c.PropertyGetSync(new NodePath(".:hp")));
        c.PropertySetSync(new NodePath(".:hp"), false);
        c.PropertySetSpawn(new NodePath(".:hp"), false);
        T.Check("set_watch / set_sync leave a different mode alone; spawn is its own flag", c.PropertyGetWatch(new NodePath(".:hp")) && !c.PropertyGetSpawn(new NodePath(".:hp")));
        c.RemoveProperty(new NodePath(".:hp"));
        T.Check("remove_property", !c.HasProperty(new NodePath(".:hp")) && c.GetProperties().Count == 2);

        // the form the converter writes
        var scene = new SceneReplicationConfig();
        scene.Properties = new GArray
        {
            new Aver.Fabric.Collections.Dictionary { ["path"] = new NodePath(".:a"), ["spawn"] = false, ["replication_mode"] = 2 },
            new Aver.Fabric.Collections.Dictionary { ["path"] = new NodePath(".:b"), ["sync"] = true, ["watch"] = false },
            new Aver.Fabric.Collections.Dictionary { ["path"] = new NodePath(".") },
        };
        T.Check("the scene form (an array of dictionaries) fills the list; an entry with no property is skipped", scene.GetProperties().Count == 2 && !scene.PropertyGetSpawn(new NodePath(".:a"))
            && scene.PropertyGetWatch(new NodePath(".:a")) && scene.PropertyGetSync(new NodePath(".:b")));
        var round = new SceneReplicationConfig { Properties = scene.Properties };
        T.Check("and it reads back the same", round.GetProperties().Count == 2 && round.PropertyGetReplicationMode(new NodePath(".:a")) == SceneReplicationConfig.ReplicationMode.OnChange);

        var viaText = ClassDB.Build("SceneReplicationConfig", new[] { ("properties", (Variant)scene.Properties) }).AsGodotObject() as SceneReplicationConfig;
        T.Check("Object(SceneReplicationConfig, \"properties\": ...) builds through ClassDB", viaText != null && viaText.GetProperties().Count == 2);

        var sync = new MultiplayerSynchronizer();
        T.Check("synchronizer defaults: root_path .., public, idle updates, no intervals", sync.RootPath.ToString() == ".." && sync.PublicVisibility && sync.VisibilityUpdateMode == MultiplayerSynchronizer.VisibilityUpdateModeEnum.Idle
            && sync.ReplicationInterval == 0 && sync.DeltaInterval == 0 && sync.ReplicationConfig == null);
        sync.SetVisibilityFor(5, false);
        T.Check("get_visibility_for: an explicit value, else public_visibility", !sync.GetVisibilityFor(5) && sync.GetVisibilityFor(6));
        sync.SetVisibilityFor(0, false);
        T.Check("set_visibility_for(0) changes public_visibility", !sync.PublicVisibility && !sync.GetVisibilityFor(6));
        var spawner = new MultiplayerSpawner();
        spawner.AddSpawnableScene("res://a.tscn");
        spawner.AddSpawnableScene("res://a.tscn");
        T.Check("the spawnable list: add (no duplicates), count, get, clear", spawner.GetSpawnableSceneCount() == 1 && spawner.GetSpawnableScene(0) == "res://a.tscn" && spawner.GetSpawnableScene(3) == "" && T.ErrorCount("already a spawnable") == 1);
        spawner.SpawnableScenes = new[] { "Maps/x.ocworld" };
        T.Check("the scene-file form is a string array", spawner.GetSpawnableSceneCount() == 1 && spawner.SceneIndexOf("res://x.tscn") == 0 && spawner.SceneIndexOf("Maps/x.ocworld") == 0 && spawner.SceneIndexOf("res://y.tscn") == -1);
        spawner.ClearSpawnableScenes();
        T.Check("clear", spawner.GetSpawnableSceneCount() == 0);
        T.Check("a spawn outside the tree is refused", spawner.Spawn("x") == null && T.ErrorCount("not in the tree") == 1);

        // a duplicated scene keeps the nodes' settings
        var original = new MultiplayerSynchronizer { Name = "S", ReplicationInterval = 0.5, DeltaInterval = 0.25, PublicVisibility = false, RootPath = new NodePath("../X"), ReplicationConfig = c };
        var copy = (MultiplayerSynchronizer)original.Duplicate();
        T.Check("duplicate copies the synchronizer's settings", copy.ReplicationInterval == 0.5 && copy.DeltaInterval == 0.25 && !copy.PublicVisibility && copy.RootPath.ToString() == "../X" && copy.ReplicationConfig == c);
        var sp2 = new MultiplayerSpawner { Name = "P", SpawnLimit = 3, SpawnPath = new NodePath("../Q"), SpawnableScenes = new[] { "res://z.tscn" } };
        var spCopy = (MultiplayerSpawner)sp2.Duplicate();
        T.Check("and the spawner's", spCopy.SpawnLimit == 3 && spCopy.SpawnPath.ToString() == "../Q" && spCopy.GetSpawnableScene(0) == "res://z.tscn");
    }

    // ---- in-memory ----

    private static void MemSync()
    {
        var hub = new MemHub(9);
        NetCluster c = NetCluster.StartMem(hub, 2, World);
        c.FullyConnected(2000);
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        var gs = s.Root.GetNode<MultiplayerSynchronizer>("GameSync");
        var gsA = a.Root.GetNode<MultiplayerSynchronizer>("GameSync");
        var gsB = b.Root.GetNode<MultiplayerSynchronizer>("GameSync");
        int syncs = 0, deltas = 0;
        gsA.Synchronized += () => syncs++;
        gsA.DeltaSynchronized += () => deltas++;
        c.Settle(50);
        for (int i = 0; i < 5; i++) c.Step();
        syncs = deltas = 0;

        s.Game.Hp = 1;
        gs.ReplicationInterval = 0.1;
        gs.DeltaInterval = 0.25;
        for (int i = 0; i < 60; i++) { s.Game.Hp++; s.Game.Score++; c.Step(); }
        T.Check("replication_interval 0.1 s over 60 frames of 1/60 s is exactly 10 syncs", syncs == 10, "syncs " + syncs);
        T.Check("delta_interval 0.25 s with a value that changes every frame is exactly 4 deltas", deltas == 4, "deltas " + deltas);
        T.Check("the receivers hold values the authority had", a.Game.Hp > 1 && a.Game.Hp <= s.Game.Hp && b.Game.Hp == a.Game.Hp && a.Game.Score > 0 && a.Game.Score <= s.Game.Score);

        gs.ReplicationInterval = 0;
        gs.DeltaInterval = 0;
        c.Step();
        syncs = deltas = 0;
        for (int i = 0; i < 30; i++) { s.Game.Hp++; c.Step(); }
        T.Check("interval 0 syncs every frame", syncs == 30, "syncs " + syncs);
        T.Check("and an on-change property that did not change sends no delta", deltas == 0, "deltas " + deltas);
        s.Game.Score += 5;
        c.Step();
        T.Check("one change, one delta", deltas == 1 && a.Game.Score == s.Game.Score && b.Game.Score == s.Game.Score);
        c.Step(); c.Step();
        T.Check("and none after it", deltas == 1);

        // a link that reorders: an old Always sync must never overwrite a newer one
        hub.MaxDelayTicks = 5;
        var seen = new List<int>();
        gsA.Synchronized += () => seen.Add(a.Game.Hp);
        int start = s.Game.Hp;
        for (int i = 0; i < 80; i++) { s.Game.Hp = start + 1 + i; c.Step(); }
        for (int i = 0; i < 12; i++) c.Step();
        bool monotonic = true;
        for (int i = 1; i < seen.Count; i++) if (seen[i] < seen[i - 1]) monotonic = false;
        T.Check("on a link that delays packets by up to 5 ticks the values a receiver sees never go backwards", seen.Count > 40 && monotonic, $"{seen.Count} syncs, monotonic {monotonic}");
        T.Check("and it ends on the last value", a.Game.Hp == s.Game.Hp);
        hub.MaxDelayTicks = 0;

        // non-authority writes go nowhere; a client authority sends to the others
        a.Game.Hp = 4242;
        c.Step(); c.Step();
        T.Check("a peer that is not the authority never sends", s.Game.Hp != 4242 && b.Game.Hp != 4242);
        c.SetAuthority("Game", a.Id);
        c.SetAuthority("GameSync", a.Id);
        a.Game.Hp = 5151;
        c.Until(() => s.Game.Hp == 5151 && b.Game.Hp == 5151, 2000);
        T.Check("after set_multiplayer_authority a client's synchronizer sends to the server and (relayed) to the other client", s.Game.Hp == 5151 && b.Game.Hp == 5151);
        s.Game.Hp = 1;
        c.Step(); c.Step();
        T.Check("and the old authority no longer does", a.Game.Hp == 5151 && b.Game.Hp == 5151);

        // a sync from a peer that is not the authority is refused by the receiver
        int errs = T.ErrorCount("is not the authority of the synchronizer");
        c.SetAuthority("GameSync", b.Id);
        a.Root.GetNode("GameSync").SetMultiplayerAuthority(a.Id);   // a still thinks it owns it; b and the server think b does
        a.Game.Hp = 6000;
        c.Step(); c.Step(); c.Step();
        T.Check("a receiver that disagrees about the authority reports and drops the state", T.ErrorCount("is not the authority of the synchronizer") > errs && s.Game.Hp != 6000);
        c.Close();
    }

    private static void MemVisibilityModes()
    {
        var hub = new MemHub(2);
        NetCluster c = NetCluster.StartMem(hub, 2, World);
        c.FullyConnected(2000);
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        var gs = s.Root.GetNode<MultiplayerSynchronizer>("GameSync");
        bool allowB = true;
        gs.AddVisibilityFilter(Callable.From<long, bool>(id => id != b.Id || allowB));
        c.Settle(40);
        s.Game.Hp = 1;
        c.Step(); c.Step();
        T.Check("the filter lets everyone in", a.Game.Hp == 1 && b.Game.Hp == 1);

        gs.VisibilityUpdateMode = MultiplayerSynchronizer.VisibilityUpdateModeEnum.Physics;
        allowB = false;
        s.Game.Hp = 2;
        c.Step(); c.Step(); c.Step();
        T.Check("visibility_update_mode physics: process frames do not look at the filter", a.Game.Hp == 2 && b.Game.Hp == 2);
        T.Frame(1.0 / 60.0, 1);
        s.Game.Hp = 3;
        c.Step(); c.Step();
        T.Check("a physics step does: b is hidden, a is not", a.Game.Hp == 3 && b.Game.Hp == 2);

        allowB = true;
        c.Step(); c.Step();
        T.Check("and nothing changes until the next physics step", b.Game.Hp == 2);
        T.Frame(1.0 / 60.0, 1);
        c.Step();
        T.Check("which shows b again and sends it the current state", b.Game.Hp == 3);
        c.Close();
    }

    // ---- converted GDScript ----

    private static void ConvertedScript()
    {
        string dir = Path.Combine(T.DataDir(), "NetGame");
        var sources = new List<ScriptSource>();
        foreach (string f in Directory.EnumerateFiles(dir, "*.gd").OrderBy(x => x, StringComparer.Ordinal))
        {
            string cls = Path.GetFileNameWithoutExtension(f);
            cls = char.ToUpperInvariant(cls[0]) + cls[1..];
            sources.Add(new ScriptSource { ResPath = "res://" + Path.GetFileName(f), Text = File.ReadAllText(f), Kind = "gdscript", ClassName = cls, Output = "Content/Scripts/" + cls + ".cs" });
        }
        ConversionResult r = ScriptConverter.Convert(sources, new ConvertSettings { RefsDir = AppContext.BaseDirectory, KeepAssembly = true, ExtraSources = System.Array.Empty<(string, string)>() });
        T.Check("the NetGame scripts (player, main, lobby) compile with no errors", r.Compiled && r.Errors.Count == 0, string.Join("; ", r.Errors.Take(3).Select(e => e.ToString())) + string.Join("; ", r.GeneralErrors));
        string player = r.Scripts.First(x => x.ClassName == "Player").Text;
        T.Check("@rpc(\"any_peer\", \"call_local\", \"reliable\") is written out in full",
            player.Contains("[Rpc(MultiplayerApi.RpcMode.AnyPeer, CallLocal = true)]"));
        T.Check("a bare @rpc is authority, call remote, and GDScript's default transfer mode, unreliable (GodotSharp's default is reliable, so it is written)",
            player.Contains("[Rpc(MultiplayerApi.RpcMode.Authority, TransferMode = MultiplayerPeer.TransferModeEnum.Unreliable)]"));
        T.Check("unreliable_ordered and a channel", player.Contains("TransferMode = MultiplayerPeer.TransferModeEnum.UnreliableOrdered, TransferChannel = 2"));
        T.Check("the arguments may come in any order, and \"disabled\" is a mode", player.Contains("[Rpc(MultiplayerApi.RpcMode.Authority, CallLocal = true)]") && player.Contains("RpcMode.Disabled"));
        T.Check("rpc(\"name\", args) and rpc_id(id, \"name\", args) become Rpc / RpcId", player.Contains("Rpc(\"authority_ping\", 1)") && player.Contains("RpcId(1, \"unreliable_default\", 2.0)"));
        T.Check("func.rpc() and func.rpc_id() become Callable.Rpc / RpcId on the method's callable", player.Contains("Callable.From<string>(SetPlayerName).Rpc(\"host\")") && player.Contains("Callable.From<Vector3>(Shoot).RpcId(1, Vector3.Zero)"));
        T.Check("multiplayer.* is Multiplayer.*", player.Contains("Multiplayer.GetUniqueId()") && player.Contains("Multiplayer.IsServer()"));
        string lobby = r.Scripts.First(x => x.ClassName == "Lobby").Text;
        T.Check("multiplayer.peer_connected.connect(f) connects to the API's signal and multiplayer_peer is assigned", lobby.Contains("Multiplayer.MultiplayerPeer = Peer;") && lobby.Contains("MultiplayerApi.SignalName.PeerConnected"));
        T.Check("every @rpc is reported converted", r.Items.Count(i => i.Property == "Rpc" && i.Rating == Rating.Converted) == 9 && !r.Items.Any(i => i.Property == "Rpc" && i.Rating != Rating.Converted));

        Assembly asm = Assembly.Load(r.Assembly!);
        Type playerType = asm.GetType("Player")!;
        var probe = (Node)Activator.CreateInstance(playerType)!;
        var cfg = probe.GetRpcConfig().AsGodotDictionary()!;
        var state = cfg["State"].AsGodotDictionary()!;
        var shoot = cfg["Shoot"].AsGodotDictionary()!;
        var never = cfg["Never"].AsGodotDictionary()!;
        T.Check("the converted class's RPC table: state() is authority, remote, unreliable-ordered, channel 2",
            state["rpc_mode"].AsInt32() == 2 && state["transfer_mode"].AsInt32() == 1 && state["channel"].AsInt32() == 2 && !state["call_local"].AsBool());
        T.Check("shoot() is any_peer, call_local, unreliable; never() is disabled", shoot["rpc_mode"].AsInt32() == 1 && shoot["call_local"].AsBool() && shoot["transfer_mode"].AsInt32() == 0 && never["rpc_mode"].AsInt32() == 0);
        T.Check("a bare @rpc and a reordered @rpc(\"reliable\", \"call_local\")", cfg["AuthorityPing"].AsGodotDictionary()!["transfer_mode"].AsInt32() == 0 && cfg["Reorder"].AsGodotDictionary()!["transfer_mode"].AsInt32() == 2 && cfg["Reorder"].AsGodotDictionary()!["call_local"].AsBool());

        // across peers, over the in-memory link, with the converted class as the "Game" node (named "1": the script takes the authority of its name)
        var hub = new MemHub(4);
        NetCluster c = NetCluster.StartMem(hub, 2, p =>
        {
            var n = (Node)Activator.CreateInstance(playerType)!;
            n.Name = "1";
            p.Root.AddChild(n);
        });
        c.FullyConnected(2000);
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        Node G(NetPeer p) => p.Root.GetNode("1");
        long Shots(NetPeer p) => G(p).Get("shots").AsInt64();
        foreach (NetPeer p in c.Peers) G(p).Set("shots", 0);
        T.Check("the script's _enter_tree took the authority named by the node", G(s).GetMultiplayerAuthority() == 1 && G(s).IsMultiplayerAuthority() && !G(a).IsMultiplayerAuthority());

        G(s).Rpc("authority_ping", 3);
        c.Settle(40);
        T.Check("rpc(\"authority_ping\", 3) from the authority runs the converted function on the clients, not on the caller", Shots(a) == 3 && Shots(b) == 3 && Shots(s) == 0);
        G(a).Rpc("authority_ping", 1);
        c.Settle(40);
        T.Check("the same call from a client is refused (authority mode) by the server and, through the relay, by the other client", Shots(s) == 0 && Shots(b) == 3 && T.ErrorCount("not allowed") == 2);
        G(a).RpcId(1, "unreliable_default", 4.0);
        c.Settle(40);
        T.Check("rpc_id(1, \"unreliable_default\", 4.0) from a client reaches the server (any_peer)", Shots(s) == 4 && Shots(b) == 3);
        hub.SentLog.Clear();
        new Callable(G(s), "Shoot").Rpc(Vector3.Zero);
        c.Settle(40);
        T.Check("shoot.rpc(): call_local runs it here too, and it travels unreliably", Shots(s) == 5 && Shots(a) == 4 && Shots(b) == 4 && hub.SentLog[0].Mode == MultiplayerPeer.TransferModeEnum.Unreliable);
        G(s).Rpc("set_player_name", "zed");
        c.Settle(40);
        T.Check("set_player_name(\"zed\") any_peer, call_local, reliable: every peer has the name", new[] { s, a, b }.All(p => G(p).Get("player_name").AsString() == "zed"));
        hub.SentLog.Clear();
        G(s).Rpc("state", new Vector3(1, 2, 3));
        c.Settle(40);
        T.Check("state(pos) goes out on channel 2 as unreliable-ordered and moves the clients' node", hub.SentLog.Any(e => e.Channel == 2 && e.Mode == MultiplayerPeer.TransferModeEnum.UnreliableOrdered)
            && ((Node3D)G(a)).Position == new Vector3(1, 2, 3));
        G(s).Rpc("never");
        c.Settle(40);
        T.Check("a disabled RPC is refused on the receiving side", T.ErrorCount("'Never' is not allowed") == 2);

        // the lobby script drives the real API
        var lobbyType = asm.GetType("Lobby")!;
        var lobbyNode = (Node)Activator.CreateInstance(lobbyType)!;
        lobbyNode.Name = "Lobby";
        s.Root.AddChild(lobbyNode);
        lobbyNode.Call("OnPeerConnected", 9);
        lobbyNode.Call("OnPeerConnected", 12);
        lobbyNode.Call("OnPeerDisconnected", 9);
        T.Check("the converted lobby's handlers run (Array.append / erase)", lobbyNode.Get("players").AsGodotArray().Select(v => v.AsInt64()).SequenceEqual(new long[] { 12 }));
        c.Close();
    }

    // ---- spawner ----

    private static void SpawnerBasics()
    {
        NetCluster c = NetCluster.Start(2, World);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);

        NetPlayer pa = SpawnPlayer(s, a.Id, "alpha", new Vector3(1, 2, 3));
        T.Check("a spawnable scene added under spawn_path on the authority appears on every client, under the same name",
            c.Until(() => PlayerOf(a, a.Id.ToString()) != null && PlayerOf(b, a.Id.ToString()) != null));
        NetPlayer ra = PlayerOf(a, a.Id.ToString())!;
        T.Check("with the spawn state already set when the node entered the tree (spawn-mode properties)", ra.Seen[0] == $"enter {a.Id} tag=alpha score=0 pos=(1, 2, 3)", ra.Seen[0]);
        T.Check("a property that is not spawn-mode (Hp is) arrives with the sync instead; the script's _enter_tree ran: its authority is the peer in its name", ra.GetMultiplayerAuthority() == a.Id && pa.GetMultiplayerAuthority() == a.Id);
        T.Check("the scene's own children came with it", ra.GetNodeOrNull<MultiplayerSynchronizer>("Sync") != null && ra.GetParent() == a.Root.GetNode("Players"));
        T.Check("spawned is emitted on the receiving peers only", a.Events.Contains("spawned " + a.Id) && b.Events.Contains("spawned " + a.Id) && !s.Events.Any(e => e.StartsWith("spawned")));

        // not spawnable, and not the authority
        s.Root.GetNode("Players").AddChild(new NetNode { Name = "Plain" });
        NetPlayer local = SpawnPlayer(a, 777, addNow: true);
        c.Settle(200);
        T.Check("a node that is not a spawnable scene is not replicated", a.Root.GetNodeOrNull("Players/Plain") == null && b.Root.GetNodeOrNull("Players/Plain") == null);
        T.Check("a spawnable scene added by a peer that is not the spawner's authority stays local", PlayerOf(s, "777") == null && PlayerOf(b, "777") == null && PlayerOf(a, "777") == local);

        // the limit
        s.Root.GetNode<MultiplayerSpawner>("Spawner").SpawnLimit = 2;
        SpawnPlayer(s, 5001);
        SpawnPlayer(s, 5002);
        c.Until(() => PlayerOf(a, "5001") != null);
        c.Settle(100);
        T.Check("the spawn limit counts the nodes the spawner replicates: one more than it allows stays local", PlayerOf(a, "5001") != null && PlayerOf(a, "5002") == null && PlayerOf(s, "5002") != null && T.ErrorCount("spawn limit") == 1);
        s.Root.GetNode<MultiplayerSpawner>("Spawner").SpawnLimit = 0;

        // despawn
        a.Events.Clear();
        pa.Free();
        T.Check("freeing the node on the authority frees it on the clients, with despawned just before", c.Until(() => PlayerOf(a, a.Id.ToString()) == null && PlayerOf(b, a.Id.ToString()) == null));
        T.Check("despawned was emitted on the clients", a.Events.Contains("despawned " + a.Id) && b.Events.Contains("despawned " + a.Id) && !s.Events.Any(e => e.StartsWith("despawned")));
        NetPlayer p1 = PlayerOf(s, "5001")!;
        s.Root.GetNode("Players").RemoveChild(p1);
        T.Check("removing it from spawn_path despawns it too", c.Until(() => PlayerOf(a, "5001") == null));
        p1.Free();

        // the spawner is freed together with its parent: nodes that vanish without a signal are noticed
        NetPlayer p3 = SpawnPlayer(s, 6001);
        c.Until(() => PlayerOf(b, "6001") != null);
        s.Root.GetNode("Players").Free();
        T.Check("freeing the whole spawn_path node despawns what was in it", c.Until(() => b.Root.GetNodeOrNull("Players/6001") == null) && p3 != null);
        c.Close();
    }

    private static void SpawnerMore()
    {
        NetCluster c = NetCluster.Start(1, World);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1);
        SpawnPlayer(s, 100, "first", new Vector3(5, 0, 0));
        SpawnPlayer(s, 101, "second");
        c.Until(() => PlayerOf(a, "100") != null && PlayerOf(a, "101") != null);

        // a late joiner
        NetPeer late = c.AddLateClient(World);
        T.Check("a client that joins later gets every node that already exists, with its spawn state",
            c.Until(() => PlayerOf(late, "100") != null && PlayerOf(late, "101") != null));
        T.Check("spawn state: the tag and position of the first", PlayerOf(late, "100")!.Seen[0] == "enter 100 tag=first score=0 pos=(5, 0, 0)");
        T.Check("each exactly once", late.Events.Count(e => e.StartsWith("spawned ")) == 2 && late.Root.GetNode("Players").GetChildCount() == 2);
        // ... and the next spawn reaches it too
        SpawnPlayer(s, 102);
        T.Check("and it receives later spawns", c.Until(() => PlayerOf(late, "102") != null));

        // the typical lobby: the server spawns a player when a peer connects
        s.Mp.PeerConnected += id => SpawnPlayer(s, (int)id, "joined");
        NetPeer another = c.AddLateClient(World);
        T.Check("a player spawned from peer_connected reaches the joining peer itself (it was in the peer list already)",
            c.Until(() => PlayerOf(another, another.Id.ToString()) != null) && PlayerOf(another, another.Id.ToString())!.Seen[0].Contains("tag=joined"));
        T.Check("and the others", c.Until(() => PlayerOf(a, another.Id.ToString()) != null));

        // custom spawn
        MultiplayerSpawner sp = s.Root.GetNode<MultiplayerSpawner>("Spawner");
        Node? made = sp.Spawn("hello");
        T.Check("spawn(data) calls spawn_function and adds the node under spawn_path", made != null && made.GetParent() == s.Root.GetNode("Players") && ((NetPlayer)made).Tag == "hello");
        string madeName = made!.Name.ToString();
        T.Check("every other peer calls the same function with the same data and keeps the authority's name", c.Until(() => PlayerOf(a, madeName) != null && PlayerOf(late, madeName) != null) && PlayerOf(a, madeName)!.Tag == "hello");
        Node? second = sp.Spawn("again");
        T.Check("a second custom node gets its own name and travels too", second != null && second.Name.ToString() != madeName && c.Until(() => PlayerOf(a, second.Name.ToString()) is { } n && n.Tag == "again"));
        NetPeer later = c.AddLateClient(World);
        T.Check("a custom spawn reaches a peer that joins afterwards, built from the same data by its own spawn_function",
            c.Until(() => PlayerOf(later, madeName) is { Tag: "hello" } && PlayerOf(later, second!.Name.ToString()) is { Tag: "again" }));
        T.Check("spawn() on a peer that is not the authority is refused", a.Root.GetNode<MultiplayerSpawner>("Spawner").Spawn("x") == null && T.ErrorCount("only the multiplayer authority can spawn") == 1);
        sp.SpawnFunction = default;
        T.Check("and without a spawn_function", sp.Spawn("x") == null && T.ErrorCount("spawn_function is not set") == 1);
        sp.SpawnFunction = Callable.From<Variant, Node>(d => null!);
        T.Check("a spawn_function that returns no node is reported", sp.Spawn("x") == null && T.ErrorCount("must return a new node") == 1);

        // visibility decides who gets a spawned node
        NetPlayer hidden = SpawnPlayer(s, 300, addNow: false);
        var hs = hidden.GetNode<MultiplayerSynchronizer>("Sync");
        hs.PublicVisibility = false;
        s.Root.GetNode("Players").AddChild(hidden);
        c.Settle(150);
        T.Check("a spawned node whose synchronizer is not visible to a peer is not spawned there", PlayerOf(a, "300") == null && PlayerOf(late, "300") == null);
        var changed = new List<long>();
        hs.VisibilityChanged += id => changed.Add(id);
        hs.SetVisibilityFor(a.Id, true);
        T.Check("set_visibility_for(peer, true) spawns it on that peer only", c.Until(() => PlayerOf(a, "300") != null) && PlayerOf(late, "300") == null && changed.Contains(a.Id));
        hs.SetVisibilityFor(a.Id, false);
        T.Check("and false despawns it again", c.Until(() => PlayerOf(a, "300") == null) && a.Events.Contains("despawned 300"));
        hs.SetVisibilityFor(0, true);
        T.Check("making it public spawns it on the others; a peer with its own setting (a: false) keeps that",
            c.Until(() => PlayerOf(late, "300") != null && PlayerOf(another, "300") != null) && PlayerOf(a, "300") == null);
        hs.SetVisibilityFor(a.Id, true);
        T.Check("until it is given its own true", c.Until(() => PlayerOf(a, "300") != null));
        hs.AddVisibilityFilter(Callable.From<long, bool>(id => id != another.Id));
        T.Check("a filter that turns a peer down despawns it there", c.Until(() => PlayerOf(another, "300") == null) && PlayerOf(a, "300") != null && PlayerOf(late, "300") != null);
        c.Close();
    }

    // ---- synchronizer ----

    private static void SyncModes()
    {
        NetCluster c = NetCluster.Start(2, World);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        NetPlayer own = SpawnPlayer(s, a.Id, "tagged", new Vector3(1, 1, 1));
        c.Until(() => PlayerOf(a, a.Id.ToString()) != null && PlayerOf(b, a.Id.ToString()) != null);
        NetPlayer ofA = PlayerOf(a, a.Id.ToString())!, ofB = PlayerOf(b, a.Id.ToString())!;
        T.Check("the owner is the authority of its player on every peer", ofA.IsMultiplayerAuthority() && !ofB.IsMultiplayerAuthority() && !own.IsMultiplayerAuthority() && own.GetNode("Sync").GetMultiplayerAuthority() == a.Id);

        var syncA = ofA.GetNode<MultiplayerSynchronizer>("Sync");
        var syncS = own.GetNode<MultiplayerSynchronizer>("Sync");
        int serverSyncs = 0, serverDeltas = 0;
        syncS.Synchronized += () => serverSyncs++;
        syncS.DeltaSynchronized += () => serverDeltas++;
        c.Settle(150);
        serverSyncs = serverDeltas = 0;

        ofA.Hp = 50;
        ofA.Pos = new Vector3(9, 8, 7);
        T.Check("an always-mode property of the authority reaches the server and the other client", c.Until(() => own.Hp == 50 && ofB.Hp == 50 && ofB.Pos == new Vector3(9, 8, 7)));
        T.Check("and the synchronized signal fires on the receivers", serverSyncs > 0 && ofB.GetNode<MultiplayerSynchronizer>("Sync") != null);
        int before = serverDeltas;
        ofA.Score = 5;
        T.Check("an on-change property travels as a delta", c.Until(() => own.Score == 5 && ofB.Score == 5) && serverDeltas == before + 1);
        c.Settle(200);
        T.Check("and is not sent again while it does not change", serverDeltas == before + 1);
        ofA.Score = 6;
        c.Until(() => own.Score == 6);
        T.Check("a new value, a new delta", serverDeltas == before + 2);

        // only the authority sends
        own.Hp = 77;
        c.Settle(250);
        T.Check("a peer that is not the authority sends nothing: the server's own value changed and nobody else's did", ofA.Hp == 50 && ofB.Hp == 50);
        T.Check("and the authority's next sync puts the server's copy right again", c.Until(() => own.Hp == 50));

        // a late joiner while a owns the node
        NetPeer late = c.AddLateClient(World);
        T.Check("a late joiner gets the node and the state: spawn values at once, the rest with the first syncs",
            c.Until(() => PlayerOf(late, a.Id.ToString()) is { } n && n.Score == 6 && n.Hp == 50 && n.Tag == "tagged" && n.Pos == new Vector3(9, 8, 7)));
        T.Check("the on-change value (6) was sent to it though it did not change again", PlayerOf(late, a.Id.ToString())!.Score == 6);

        // the authority moves
        c.SetAuthority("Players/" + a.Id, b.Id);
        ofB.Hp = 33;
        T.Check("after set_multiplayer_authority the new authority's values flow to the others", c.Until(() => own.Hp == 33 && ofA.Hp == 33));
        ofA.Hp = 99;
        c.Settle(250);
        T.Check("and the old authority's no longer do", own.Hp == 33 && ofB.Hp == 33);
        ofB.Score = 40;
        T.Check("the new authority's on-change values too, to the late joiner as well", c.Until(() => own.Score == 40 && ofA.Score == 40 && PlayerOf(late, a.Id.ToString())!.Score == 40));
        c.Close();
    }

    private static void SyncVisibility()
    {
        NetCluster c = NetCluster.Start(2, World);
        c.FullyConnected();
        NetPeer s = c.Server, a = c.C(1), b = c.C(2);
        var gs = s.Root.GetNode<MultiplayerSynchronizer>("GameSync");
        var seenVis = new List<long>();
        gs.VisibilityChanged += id => seenVis.Add(id);
        NetPeer joiner = c.AddLateClient(World);
        T.Check("public by default: a peer that joins becomes visible and visibility_changed is emitted for it", c.Until(() => seenVis.Contains(joiner.Id)) && seenVis.Count == 1);
        s.Game.Hp = 11;
        T.Check("everyone gets the state", c.Until(() => a.Game.Hp == 11 && b.Game.Hp == 11 && joiner.Game.Hp == 11));

        bool allowB = false;
        gs.AddVisibilityFilter(Callable.From<long, bool>(id => id != b.Id || allowB));
        seenVis.Clear();
        s.Game.Hp = 12;
        T.Check("a filter that refuses b: a still gets the state, b does not", c.Until(() => a.Game.Hp == 12) && b.Game.Hp == 11);
        T.Check("b became invisible (visibility_changed for b only)", seenVis.SequenceEqual(new long[] { b.Id }));
        s.Game.Score = 3;
        c.Settle(150);
        T.Check("deltas skip it too", a.Game.Score == 3 && b.Game.Score == 0);
        allowB = true;
        gs.UpdateVisibility(b.Id);
        T.Check("update_visibility(b) after the filter's answer changed gives b the current state at once (watched values and the always-mode ones)", c.Until(() => b.Game.Score == 3 && b.Game.Hp == 12));

        // update modes
        gs.VisibilityUpdateMode = MultiplayerSynchronizer.VisibilityUpdateModeEnum.None;
        allowB = false;
        s.Game.Hp = 13;
        c.Settle(150);
        T.Check("with visibility_update_mode none a filter's new answer is not looked at on its own", b.Game.Hp == 13);
        gs.UpdateVisibility();
        s.Game.Hp = 14;
        T.Check("update_visibility() looks at all peers", c.Until(() => a.Game.Hp == 14) && b.Game.Hp == 13);
        gs.VisibilityUpdateMode = MultiplayerSynchronizer.VisibilityUpdateModeEnum.Idle;
        allowB = true;
        T.Check("idle mode looks every frame", c.Until(() => b.Game.Hp == 14));

        // public_visibility off: only the peers named
        gs.PublicVisibility = false;
        s.Game.Hp = 15;
        c.Settle(150);
        T.Check("public_visibility off hides everyone", a.Game.Hp == 14 && b.Game.Hp == 14);
        gs.SetVisibilityFor(a.Id, true);
        T.Check("and set_visibility_for(a, true) shows a alone", c.Until(() => a.Game.Hp == 15) && b.Game.Hp == 14);
        gs.PublicVisibility = true;
        T.Check("public_visibility back on shows b again", c.Until(() => b.Game.Hp == 15));

        // the size limits
        s.Mp.MaxSyncPacketSize = 60;
        s.Game.Tag = new string('x', 200);
        c.Settle(200);
        T.Check("a sync state over max_sync_packet_size is reported and not sent", T.ErrorCount("max_sync_packet_size") == 1 && a.Game.Tag.Length == 0);
        s.Game.Tag = "ok";
        T.Check("and small again, it flows", c.Until(() => a.Game.Tag == "ok"));
        s.Mp.MaxSyncPacketSize = 1350;
        c.Close();
    }

    // ---- lossy ----

    private static void LossyReplication()
    {
        NetCluster c = NetCluster.Start(1, World, lossy: true, loss: 0.12, dup: 0.08, minDelay: 0, maxDelay: 25, seed: 12);
        T.Check("connected through the lossy link", c.FullyConnected(40000));
        NetPeer s = c.Server, a = c.C(1);
        var pls = new List<NetPlayer>();
        for (int i = 0; i < 4; i++) pls.Add(SpawnPlayer(s, 900 + i, "t" + i, new Vector3(i, i, i)));
        T.Check("every spawn arrives, with its spawn state", c.Until(() => Enumerable.Range(0, 4).All(i => PlayerOf(a, (900 + i).ToString()) != null), 60000)
            && Enumerable.Range(0, 4).All(i => PlayerOf(a, (900 + i).ToString())!.Seen[0] == $"enter {900 + i} tag=t{i} score=0 pos=({i}, {i}, {i})"));
        // the players are named for owners that do not exist, so none of them syncs; the world's synchronizer (the server's) carries the values
        s.Game.Score = 21;
        s.Game.Hp = 5;
        T.Check("an on-change delta and an always sync arrive", c.Until(() => a.Game.Score == 21 && a.Game.Hp == 5, 40000));
        for (int i = 0; i < 30; i++) { s.Game.Hp = 100 + i; c.Step(); Thread.Sleep(2); }
        T.Check("a stream of unreliable syncs ends on the last value (the final state is re-sent every interval)", c.Until(() => a.Game.Hp == 129, 20000));
        s.Game.Score = 22;
        T.Check("a delta lost on the wire is re-sent by the reliable transport", c.Until(() => a.Game.Score == 22, 40000));
        foreach (NetPlayer p in pls) p.Free();
        T.Check("every despawn arrives", c.Until(() => a.Root.GetNode("Players").GetChildCount() == 0, 60000));
        T.Check("the proxy dropped and duplicated", c.Proxy!.Dropped > 5 && c.Proxy.Duplicated > 2, $"dropped {c.Proxy.Dropped}, duplicated {c.Proxy.Duplicated}");
        c.Close();
    }
}
