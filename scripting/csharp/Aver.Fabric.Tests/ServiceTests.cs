// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The scene tree's services: frame signals, scene tree timers and `await ToSignal`, group calls, scene changes, autoloads at /root/<Name>,
// the Timer node, resources (ResourceLoader, preload, Duplicate, local-to-scene) and PackedScene.Instantiate over a converted level.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class ServiceTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("SceneTree: process_frame, physics_frame, node_added, node_removed, tree_changed", FrameSignals);
        T.Run("SceneTree timers: timeout order, pause, physics, ignore_time_scale", TimerBehaviour);
        T.Run("await ToSignal(timer) resumes in order, and `await ToSignal` on a node signal works", AwaitOrdering);
        T.Run("Timer node: wait_time, one_shot, autostart, paused, process_callback, events", TimerNode);
        T.Run("SceneTree groups: tree order, call_group flags, set_group, notify_group", Groups);
        T.Run("SceneTree: current_scene, change_scene_to_packed, reload, autoloads at /root/Name", Scenes);
        T.Run("SceneTree: pause notifications, quit", PauseAndQuit);
        T.Run("ResourceLoader: res:// mapping, exists, cache, threaded requests, preload", Loader);
        T.Run("Resource: duplicate, take_over_path, local_to_scene", ResourceObjects);
        T.Run("PackedScene: a converted level instantiates with names, values, groups, transforms and connections", PackedSceneInstance);
        T.Run("PackedScene: instances are separate, scene changes use them, pack stores a node", PackedSceneMore);
        T.Run("PackedScene: the converter's own output for the ConvertGame project instantiates", ConverterOutput);
    });

    // Runs `AverAssetC fabric` on the hand-written ConvertGame project and instantiates its levels: the contract between the scene
    // converter (what it writes) and this runtime (what it reads). Skipped when the tool has not been built.
    private static void ConverterOutput()
    {
        string? exe = null;
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null && exe == null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", OperatingSystem.IsWindows() ? "AverAssetC.exe" : "AverAssetC");
            if (File.Exists(p)) exe = p;
        }
        if (exe == null) { Console.WriteLine("  SKIP: AverAssetC is not built (build/bin/AverAssetC)"); return; }
        string input = Path.Combine(T.DataDir(), "ConvertGame");
        string output = Path.Combine(Path.GetTempPath(), "fabric_runtime_" + Guid.NewGuid().ToString("N"));
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(input); psi.ArgumentList.Add(output); psi.ArgumentList.Add("--scenes-only");   // scripts are not this test's business
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter ran", proc.ExitCode == 0, "exit " + proc.ExitCode);
            }
            ResourceLoader.ContentRootOverride = Path.Combine(output, "ConvertGame", "Content");
            T.Check("the converted main scene is found as res://main.tscn", ResourceLoader.Exists("res://main.tscn") && ResourceLoader.Exists("res://props/crate.tscn"));
            PackedScene scene = ResourceLoader.Load<PackedScene>("res://main.tscn")!;
            T.Check("it can be instantiated (the autoload is not part of it)", scene.CanInstantiate());
            var tree = SceneTree.Instance;
            Node main = scene.Instantiate();
            tree.Root.AddChild(main);
            T.Check("the root keeps the scene's name", main.Name == "Main" && main is Node3D);
            T.Check("no error was logged while it entered the tree", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

            var sun = main.GetNode<DirectionalLight3D>("Sun");
            T.Check("the sun's energy and shadow", T.Near(sun.LightEnergy, 1.5f) && sun.ShadowEnabled);
            var cam = main.GetNode<Camera3D>("Camera");
            T.Check("the camera is current with its field of view", cam.IsCurrent() && T.Near(cam.Fov, 60f) && tree.Root.GetCamera3D() == cam);
            var crate = main.GetNode<RigidBody3D>("Crate");
            T.Check("the instanced crate: mass 5, at (4, 0.5, -3) turned 90 degrees about Y", T.Near(crate.Mass, 5f) && T.Near(crate.Position, new Vector3(4, 0.5f, -3)) && T.Near(crate.Basis * new Vector3(1, 0, 0), new Vector3(0, 0, -1)), $"{crate.Position}");
            T.Check("its hidden lid", !main.GetNode<MeshInstance3D>("Crate/Lid").Visible);
            T.Check("the unique name inside the instance is reachable from the scene root", main.GetNode("%Extra") == main.GetNode("Crate/Body/Extra"));
            var floor = main.GetNode<StaticBody3D>("Floor");
            T.Check("the floor: layer 3, group, physics material", floor.CollisionLayer == 3 && floor.IsInGroup("ground") && floor.PhysicsMaterialOverride is { } pm && T.Near(pm.Friction, 0.1f) && T.Near(pm.Bounce, 0.4f) && pm.Rough);
            T.Check("its box shape is built from Object(BoxShape3D, ...)", floor.GetNode<CollisionShape3D>("Shape").Shape is BoxShape3D { Size: var fs } && fs == new Vector3(20, 1, 20));
            var ball = main.GetNode<RigidBody3D>("Ball");
            T.Check("the ball: mass 2, effective damping 0.75, axis lock, contact monitor, layer 4 mask 5", ball.Mass == 2f && T.Near(ball.LinearDamp, 0.75f) && ball.AxisLockAngularY && ball.ContactMonitor && ball.MaxContactsReported == 4 && ball.CollisionLayer == 4 && ball.CollisionMask == 5);
            var zone = main.GetNode<Area3D>("Zone");
            T.Check("the zone: gravity override 1 with 2.5, not monitorable", zone.GravitySpaceOverride == Area3D.SpaceOverride.Combine && T.Near(zone.Gravity, 2.5f) && !zone.Monitorable);
            var scaled = main.GetNode<MeshInstance3D>("Scaled");
            T.Check("a non-uniform scale (PLACEG) comes back as Godot's (2, 3, 4) at (1, 2, 3)", T.Near(scaled.Scale, new Vector3(2, 3, 4)) && T.Near(scaled.Position, new Vector3(1, 2, 3)), $"{scaled.Scale} {scaled.Position}");
            var spin = main.GetNode<Node3D>("Spin");
            T.Check("a rotated, scaled node: position (1, 0, 0), yaw 90 degrees about Y, scale 2", T.Near(spin.Position, new Vector3(1, 0, 0)) && T.Near(spin.Rotation.Y, 1.5707964f, 1e-3f) && T.Near(spin.Scale, new Vector3(2, 2, 2), 1e-3f), $"{spin.Rotation} {spin.Scale}");
            T.Check("metadata/x becomes meta x", main.GetNode("Waypoint").GetMeta("tag").AsString() == "spawn");
            T.Check("a node with a script class that is not here is a Node3D that says so", main.GetNode("Player").GetType() == typeof(Node3D) && T.Logs.Any(l => l.Message.Contains("'Player'") || l.Message.Contains("Player")));
            T.Check("groups from the header", main.GetNode("Player").IsInGroup("player") && main.GetNode("Player").IsInGroup("actors") && tree.GetNodesInGroup("actors").Count == 1);
            T.Check("the 2D and UI carriers keep their place in the tree", main.GetNode("Hud") is Control && main.GetNode("Icon") is Node && main.GetNode("Route") is Node3D);

            Node crateScene = ResourceLoader.Load<PackedScene>("res://props/crate.tscn")!.Instantiate();
            tree.Root.AddChild(crateScene);
            T.Check("the instanced scene is also a scene of its own", crateScene is RigidBody3D && crateScene.GetNode("Body") is MeshInstance3D && crateScene.SceneFilePath == "res://props/crate.tscn");
        }
        finally
        {
            try { Directory.Delete(output, true); } catch (IOException) { }
        }
    }

    private static void FrameSignals()
    {
        var tree = SceneTree.Instance;
        var log = new List<string>();
        tree.ProcessFrame += () => log.Add("process");
        tree.PhysicsFrame += () => log.Add("physics");
        tree.NodeAdded += n => log.Add("added " + n.Name);
        tree.NodeRemoved += n => log.Add("removed " + n.Name);
        tree.TreeChanged += () => log.Add("changed");
        var hero = new Hero { Name = "H" };
        tree.Root.AddChild(hero);
        T.CheckSeq("adding a node: node_added then tree_changed", new[] { "added H", "changed" }, log);
        log.Clear();
        T.Frame(0.016, physicsSteps: 2);
        T.CheckSeq("a frame: physics_frame for each step, then process_frame", new[] { "physics", "physics", "process" }, log);
        T.Check("_Process and _PhysicsProcess ran after the frame signals", hero.Frames.Count == 1 && hero.Steps.Count == 2);
        long f0 = tree.GetFrame();
        T.Frame(0.016);
        T.Check("get_frame counts process frames", tree.GetFrame() == f0 + 1);
        log.Clear();
        tree.Root.RemoveChild(hero);
        T.CheckSeq("removing: node_removed then tree_changed", new[] { "removed H", "changed" }, log);
        log.Clear();
        var n = new Node { Name = "A" };
        tree.Root.AddChild(n);
        log.Clear();
        int renamed = 0;
        tree.NodeRenamed += _ => renamed++;
        n.Name = "B";
        T.Check("node_renamed when a node in the tree is renamed", renamed == 1);
        T.Check("get_node_count counts the tree without the root", tree.GetNodeCount() == 1);
    }

    private static void TimerBehaviour()
    {
        var tree = SceneTree.Instance;
        var fired = new List<string>();
        SceneTreeTimer late = tree.CreateTimer(1.0), early = tree.CreateTimer(0.5);
        late.Timeout += () => fired.Add("late");
        early.Timeout += () => fired.Add("early");
        T.Frames(2, 0.25);
        T.CheckSeq("the shorter timer fires first, in its own frame", new[] { "early" }, fired);
        T.Check("time_left counts down", T.Near((float)late.TimeLeft, 0.5f));
        T.Frames(2, 0.25);
        T.CheckSeq("and the other when its time is up", new[] { "early", "late" }, fired);

        fired.Clear();
        SceneTreeTimer a = tree.CreateTimer(0.5), b = tree.CreateTimer(0.5);
        a.Timeout += () => fired.Add("a");
        b.Timeout += () => fired.Add("b");
        T.Frame(0.5);
        T.CheckSeq("timers that are due together fire in creation order", new[] { "a", "b" }, fired);

        fired.Clear();
        SceneTreeTimer wait = tree.CreateTimer(0.5, processAlways: false), always = tree.CreateTimer(0.5, processAlways: true);
        wait.Timeout += () => fired.Add("pausable");
        always.Timeout += () => fired.Add("always");
        tree.Paused = true;
        T.Frame(0.5);
        T.CheckSeq("when paused only a process_always timer counts", new[] { "always" }, fired);
        tree.Paused = false;
        T.Frame(0.5);
        T.CheckSeq("the other resumes with the tree", new[] { "always", "pausable" }, fired);

        fired.Clear();
        SceneTreeTimer phys = tree.CreateTimer(0.5, processInPhysics: true);
        phys.Timeout += () => fired.Add("physics");
        T.Frame(1.0);
        T.Check("a physics timer ignores process frames", fired.Count == 0);
        T.Frame(0.001, physicsSteps: 31);
        T.CheckSeq("and fires on the physics step that reaches its time", new[] { "physics" }, fired);

        fired.Clear();
        SceneTreeTimer tl = tree.CreateTimer(1.0);
        tl.Timeout += () => fired.Add("changed");
        tl.TimeLeft = 0.1;
        T.Frame(0.2);
        T.Check("time_left can be set", fired.Count == 1);
        T.Check("a fired timer is gone from the tree", tree.TimerCount == 0);
    }

    private static void AwaitOrdering()
    {
        var tree = SceneTree.Instance;
        var host = new Awaiter { Name = "Host" };
        tree.Root.AddChild(host);
        var log = new List<string>();
        host.Wait(0.5, log, "half");
        host.Wait(0.25, log, "quarter");
        host.Wait(1.0, log, "one");
        T.Check("nothing resumes before its timeout", log.Count == 0);
        T.Frames(4, 0.25);
        T.CheckSeq("resumptions follow the timers' due order, in the frame they fire", new[] { "quarter@0", "half@1", "one@3" }, log);

        log.Clear();
        host.Wait(0.1, log, "phys", physics: true);
        T.Frame(0.5);
        T.Check("a physics timer wakes the awaiter only on physics steps", log.Count == 0);
        T.Frame(0.016, physicsSteps: 8);
        T.Check("and it does", log.Count == 1 && log[0].StartsWith("phys@"));

        // await on a node signal, and on a signal that is emitted before the await is reached by the continuation.
        var emitter = new Emitter { Name = "E" };
        tree.Root.AddChild(emitter);
        string seen = "";
        async void Run()
        {
            Variant[] args = await host.ToSignal(emitter, "Hit");
            seen = "hit " + args[0].AsInt32();
        }
        Run();
        T.Check("awaiting a signal that has not fired waits", seen == "");
        emitter.EmitSignal("Hit", 7);
        T.Check("and resumes with the arguments when it does", seen == "hit 7");

        // The documented one-liner works from a script's async method, and a deferred call made in the continuation runs at the pass end.
        var order = new List<string>();
        async void Chain()
        {
            await host.ToSignal(tree.CreateTimer(0.1), SceneTreeTimer.SignalName.Timeout);
            order.Add("resumed");
            Callable.From(() => order.Add("deferred")).CallDeferred();
            order.Add("after-defer");
        }
        Chain();
        T.Frame(0.2);
        T.CheckSeq("deferred calls made after an await run at the end of the pass", new[] { "resumed", "after-defer", "deferred" }, order);
    }

    private static void TimerNode()
    {
        var tree = SceneTree.Instance;
        var parent = new Node { Name = "P" };
        tree.Root.AddChild(parent);
        int fired = 0;
        var t = new Aver.Fabric.Timer { WaitTime = 1.0, OneShot = false, Autostart = true };
        t.Timeout += () => fired++;
        parent.AddChild(t);
        T.Check("autostart runs it when it is ready", !t.IsStopped() && T.Near((float)t.TimeLeft, 1f));
        T.Frames(4, 0.25);
        T.Check("it times out when the wait time has passed", fired == 1);
        T.Frames(8, 0.25);
        T.Check("and again each wait time", fired == 3);
        t.Paused = true;
        T.Frames(4, 0.25);
        T.Check("paused does not count", fired == 3);
        t.Paused = false;
        t.OneShot = true;
        t.Start(0.5);
        T.Frames(4, 0.25);
        T.Check("a one-shot fires once and stops", fired == 4 && t.IsStopped());

        var phys = new Aver.Fabric.Timer { WaitTime = 0.1, OneShot = true, ProcessCallback = Aver.Fabric.Timer.TimerProcessCallback.Physics };
        int physFired = 0;
        phys.Timeout += () => physFired++;
        parent.AddChild(phys);
        phys.Start();
        T.Frame(1.0);
        T.Check("process_callback Physics ignores process frames", physFired == 0);
        T.Frame(0.001, physicsSteps: 7);
        T.Check("and counts physics steps", physFired == 1);

        var paused = new Aver.Fabric.Timer { WaitTime = 0.2, OneShot = true };
        int pausedFired = 0;
        paused.Timeout += () => pausedFired++;
        parent.AddChild(paused);
        paused.Start();
        tree.Paused = true;
        T.Frame(0.5);
        T.Check("the tree's pause stops a timer", pausedFired == 0);
        paused.ProcessMode = Node.ProcessModeEnum.Always;
        T.Frame(0.5);
        T.Check("unless its process mode is Always", pausedFired == 1);
        tree.Paused = false;

        var slow = new Aver.Fabric.Timer { WaitTime = 1.0, OneShot = true, IgnoreTimeScale = true };
        slow.Timeout += () => fired += 100;
        parent.AddChild(slow);
        slow.Start();
        T.Frames(4, 0.25);
        T.Check("ignore_time_scale counts real time (here equal to the frame time)", fired >= 100);
    }

    private static void Groups()
    {
        var tree = SceneTree.Instance;
        GroupMember.Trail.Clear();
        var root = new Node { Name = "Main" };
        tree.Root.AddChild(root);
        GroupMember a = new() { Name = "A" }, b = new() { Name = "B" }, c = new() { Name = "C" };
        var holder = new Node { Name = "Holder" };
        root.AddChild(a); root.AddChild(holder); holder.AddChild(b); root.AddChild(c);
        foreach (var m in new[] { a, b, c }) m.AddToGroup("team");
        c.AddToGroup("solo");
        var inTree = tree.GetNodesInGroup("team");
        T.Check("get_nodes_in_group is in tree order", inTree.Count == 3 && inTree[0] == a && inTree[1] == b && inTree[2] == c);
        T.Check("get_first_node_in_group and has_group", tree.GetFirstNodeInGroup("team") == a && tree.HasGroup("solo") && !tree.HasGroup("nope"));
        T.Check("get_node_count_in_group", tree.GetNodeCountInGroup("team") == 3);

        tree.CallGroup("team", "ping");
        T.CheckSeq("call_group calls every member in tree order", new[] { "A ping", "B ping", "C ping" }, GroupMember.Trail);
        GroupMember.Trail.Clear();
        tree.CallGroup("team", "bump", 5);
        T.Check("with arguments", a.Value == 5 && b.Value == 5 && c.Value == 5);
        GroupMember.Trail.Clear();
        tree.CallGroupFlags(SceneTree.GroupCallFlags.Reverse, "team", "ping");
        T.CheckSeq("the Reverse flag walks it last first", new[] { "C ping", "B ping", "A ping" }, GroupMember.Trail);
        GroupMember.Trail.Clear();
        tree.CallGroupFlags(SceneTree.GroupCallFlags.Deferred, "team", "ping");
        T.Check("Deferred waits for the end of the pass", GroupMember.Trail.Count == 0);
        T.Frame(0.016);
        T.Check("and then calls them", GroupMember.Trail.Count == 3);
        GroupMember.Trail.Clear();
        tree.CallGroupFlags(SceneTree.GroupCallFlags.Deferred | SceneTree.GroupCallFlags.Unique, "team", "ping");
        tree.CallGroupFlags(SceneTree.GroupCallFlags.Deferred | SceneTree.GroupCallFlags.Unique, "team", "ping");
        T.Frame(0.016);
        T.Check("Unique makes one deferred call per group and method", GroupMember.Trail.Count == 3);
        GroupMember.Trail.Clear();
        tree.CallGroup("team", "no_such_method");
        T.Check("a missing method is skipped quietly", T.ErrorCount() == 0 && GroupMember.Trail.Count == 0);
        tree.CallGroup("empty_group", "ping");

        tree.SetGroup("team", "Value", 9);
        T.Check("set_group sets a property on each", a.Value == 9 && b.Value == 9 && c.Value == 9);
        var noted = new NoteRecorder { Name = "Noted" };
        noted.AddToGroup("note");
        root.AddChild(noted);
        tree.NotifyGroup("note", Node.NotificationPaused);
        T.CheckSeq("notify_group sends a notification to each member", new[] { "Noted paused" }, noted.Notes);
        b.Free();
        T.Check("a freed member leaves the group", tree.GetNodesInGroup("team").Count == 2);
        root.RemoveChild(a);
        T.Check("a member outside the tree does not count", tree.GetNodesInGroup("team").Count == 1 && tree.GetFirstNodeInGroup("team") == c);
    }

    private static void Scenes()
    {
        var tree = SceneTree.Instance;
        var global = new Node { Name = "Global", Persistent = true };
        tree.Root.AddChild(global);
        var main = new Node3D { Name = "Main" };
        tree.Root.AddChild(main);
        T.Check("an autoload is reachable as /root/<Name> from anywhere", main.GetNode("/root/Global") == global && tree.Root.GetNode("Global") == global);
        T.Check("current_scene is the first node that is not an autoload", tree.CurrentScene == main);

        var packed = new PackedScene();
        var other = new Node3D { Name = "Level2" };
        other.AddChild(new Node { Name = "Child" });
        packed.Pack(other);
        packed.TakeOverPath("res://level2.tscn");
        T.Check("pack stores a scene to instantiate", packed.CanInstantiate());
        Error err = tree.ChangeSceneToPacked(packed);
        T.Check("change_scene_to_packed returns Ok and waits for the end of the frame", err == Error.Ok && tree.CurrentScene == main && main.IsInsideTree());
        T.Frame(0.016);
        T.Check("then the old scene is gone and the new one is current", !main.IsAlive && tree.CurrentScene is { } cs && cs.Name == "Level2" && cs.GetChildCount() == 1);
        T.Check("the autoload survived", global.IsInsideTree() && tree.Root.HasNode("Global"));
        T.Check("the new scene knows its file", tree.CurrentScene!.SceneFilePath == "res://level2.tscn");

        Node first = tree.CurrentScene!;
        T.Check("reload_current_scene loads the scene again", tree.ReloadCurrentScene() == Error.Ok);
        T.Frame(0.016);
        T.Check("as a fresh instance", tree.CurrentScene != first && tree.CurrentScene!.Name == "Level2" && !first.IsAlive);

        Error unload = tree.UnloadCurrentScene();
        T.Frame(0.016);
        T.Check("unload_current_scene frees it", unload == Error.Ok && tree.CurrentScene == null);
        T.Check("change_scene_to_file of a missing scene is an error", tree.ChangeSceneToFile("res://missing.tscn") == Error.FileNotFound);
    }

    private static void PauseAndQuit()
    {
        var tree = SceneTree.Instance;
        var notes = new List<string>();
        var a = new NoteRecorder { Name = "Rec1", Notes = notes };
        var b = new NoteRecorder { Name = "Rec2", Notes = notes, ProcessMode = Node.ProcessModeEnum.Always };
        var c = new NoteRecorder { Name = "Rec3", Notes = notes, ProcessMode = Node.ProcessModeEnum.WhenPaused };
        foreach (Node n in new Node[] { a, b, c }) tree.Root.AddChild(n);
        tree.Paused = true;
        T.CheckSeq("pausing tells a pausable node it is paused and a WhenPaused one that it is unpaused; an Always node hears nothing", new[] { "Rec1 paused", "Rec3 unpaused" }, notes);
        notes.Clear();
        tree.Paused = false;
        T.CheckSeq("and resuming reverses both", new[] { "Rec1 unpaused", "Rec3 paused" }, notes);
        T.Check("quit is a request", !tree.QuitRequested);
        tree.Quit();
        T.Check("that is remembered", tree.QuitRequested);
    }

    // Level and mesh files live under test-content/ (SeparationTest allows game content nowhere else); tests/fabric/data sits beside it.
    private static string Content() => Path.Combine(T.DataDir(), "..", "..", "..", "test-content", "FabricRuntime", "Content");

    private static void Loader()
    {
        ResourceLoader.ContentRootOverride = Content();
        T.Check("a scene exists when its converted level is in the content folder", ResourceLoader.Exists("res://arena.tscn") && ResourceLoader.Exists("res://props/crate.tscn"));
        T.Check("a file exists under its content-relative path", ResourceLoader.Exists("Meshes/box.ocmesh"));
        T.Check("a texture is found where the converter puts them (Textures/<path>)", ResourceLoader.Exists("res://art/a.png"));
        T.Check("a path with no file does not exist", !ResourceLoader.Exists("res://nope.tscn") && !ResourceLoader.Exists("res://art/missing.png"));

        PackedScene? scene = ResourceLoader.Load<PackedScene>("res://arena.tscn");
        T.Check("loading a scene gives a PackedScene that can be instantiated", scene != null && scene.CanInstantiate() && scene.ResourcePath == "res://arena.tscn");
        T.Check("two loads are one object", ReferenceEquals(scene, ResourceLoader.Load<PackedScene>("res://arena.tscn")) && ResourceLoader.HasCached("res://arena.tscn"));
        T.Check("GD.Load does the same", ReferenceEquals(scene, GD.Load<PackedScene>("res://arena.tscn")));
        Texture2D? tex = ResourceLoader.Load<Texture2D>("res://art/a.png");
        T.Check("a texture loads as Texture2D with its path", tex != null && tex.ResourcePath == "res://art/a.png");
        Mesh? mesh = ResourceLoader.Load<Mesh>("Meshes/box.ocmesh");
        T.Check("a mesh as Mesh", mesh != null && mesh.GetType() == typeof(Mesh));
        T.Check("the type the extension suggests is used when none is asked for", ResourceLoader.Load("Meshes/box.ocmesh", null, ResourceLoader.CacheMode.Ignore) is Mesh);
        T.Check("cache mode Ignore does not keep it, Replace does", ResourceLoader.Load<Mesh>("Meshes/box.ocmesh", ResourceLoader.CacheMode.Replace) is { } m2 && ReferenceEquals(m2, ResourceLoader.GetCachedRef("Meshes/box.ocmesh")));

        // Asynchronous requests: in progress until the pass ends, then loaded; get_status reports a path nobody asked for as invalid.
        ResourceLoader.ClearCache();
        T.Check("the status of an unrequested path is invalid", ResourceLoader.LoadThreadedGetStatus("res://props/crate.tscn") == ResourceLoader.ThreadLoadStatus.InvalidResource);
        T.Check("a request is accepted", ResourceLoader.LoadThreadedRequest("res://props/crate.tscn") == Error.Ok);
        var progress = new Collections.Array();
        T.Check("and is in progress", ResourceLoader.LoadThreadedGetStatus("res://props/crate.tscn", progress) == ResourceLoader.ThreadLoadStatus.InProgress && progress.Count == 1);
        T.Frame(0.016);
        T.Check("loaded after the pass", ResourceLoader.LoadThreadedGetStatus("res://props/crate.tscn") == ResourceLoader.ThreadLoadStatus.Loaded);
        Resource? got = ResourceLoader.LoadThreadedGet("res://props/crate.tscn");
        T.Check("and get returns it", got is PackedScene ps && ps.CanInstantiate());
        ResourceLoader.LoadThreadedRequest("res://arena.tscn");
        T.Check("get before the pass loads at once", ResourceLoader.LoadThreadedGet("res://arena.tscn") is PackedScene);

        T.Check("preload gives the resource, once", ReferenceEquals(GD.Preload<PackedScene>("res://arena.tscn"), GD.Preload<PackedScene>("res://arena.tscn")));
        T.Check("preload of a missing file is an error but still a resource", GD.Preload<Texture2D>("res://art/gone.png") != null && T.ErrorCount("preload") == 1);
    }

    private static void ResourceObjects()
    {
        var outer = new BoxShape3D { Size = new Vector3(1, 2, 3), ResourceName = "box" };
        var copy = (BoxShape3D)outer.Duplicate();
        T.Check("duplicate copies the values and drops the path", copy != outer && copy.Size == outer.Size && copy.ResourceName == "box" && copy.ResourcePath == "");
        copy.Size = new Vector3(9, 9, 9);
        T.Check("and is a separate object", outer.Size == new Vector3(1, 2, 3) && copy.GetInstanceId() != outer.GetInstanceId());

        var mi = new MeshInstance3D { Mesh = new BoxMesh { Size = new Vector3(2, 2, 2) } };
        var holder = new Holder { Shape = outer };
        var shallow = (Holder)holder.Duplicate();
        var deep = (Holder)holder.Duplicate(true);
        T.Check("a shallow duplicate shares the resources it holds", ReferenceEquals(shallow.Shape, outer));
        T.Check("a deep one copies them too", !ReferenceEquals(deep.Shape, outer) && deep.Shape is BoxShape3D b && b.Size == outer.Size);
        _ = mi;

        outer.ResourcePath = "res://x.tres";
        var other = new BoxShape3D();
        other.TakeOverPath("res://x.tres");
        T.Check("take_over_path makes it the loaded one", ReferenceEquals(ResourceLoader.GetCachedRef("res://x.tres"), other) && other.ResourcePath == "res://x.tres");

        int changed = 0;
        outer.Changed += () => changed++;
        outer.EmitChanged();
        T.Check("the changed signal and event", changed == 1 && outer.HasSignal("changed"));

        // local_to_scene: each node that gets the value gets its own copy.
        var shared = new BoxShape3D { Size = new Vector3(5, 5, 5), ResourceLocalToScene = true };
        ResourceLoader.Provider = (path, type) => path == "res://local.tres" ? shared : null;
        var one = new CollisionShape3D(); var two = new CollisionShape3D();
        one.PendingValues = new List<(string, string)> { ("shape", "Resource(\"res://local.tres\")") };
        two.PendingValues = new List<(string, string)> { ("shape", "Resource(\"res://local.tres\")") };
        SceneTree.Instance.Root.AddChild(one); SceneTree.Instance.Root.AddChild(two);
        T.Check("local_to_scene resources are copied per node", one.Shape is BoxShape3D s1 && two.Shape is BoxShape3D s2 && !ReferenceEquals(s1, s2) && !ReferenceEquals(s1, shared) && s1.Size == shared.Size);
        var plain = new BoxShape3D { Size = new Vector3(6, 6, 6) };
        ResourceLoader.Provider = (path, type) => path == "res://plain.tres" ? plain : null;
        var three = new CollisionShape3D(); var four = new CollisionShape3D();
        three.PendingValues = new List<(string, string)> { ("shape", "Resource(\"res://plain.tres\")") };
        four.PendingValues = new List<(string, string)> { ("shape", "Resource(\"res://plain.tres\")") };
        SceneTree.Instance.Root.AddChild(three); SceneTree.Instance.Root.AddChild(four);
        T.Check("others are shared", ReferenceEquals(three.Shape, four.Shape));
    }

    private static PackedScene Arena()
    {
        ResourceLoader.ContentRootOverride = Content();
        return ResourceLoader.Load<PackedScene>("res://arena.tscn")!;
    }

    private static void PackedSceneInstance()
    {
        var tree = SceneTree.Instance;
        PackedScene scene = Arena();
        Node root = scene.Instantiate();
        T.Check("the scene root has the class, name and file", root is ArenaNode && root.Name == "Arena" && root.SceneFilePath == "res://arena.tscn");
        T.Check("the autoload placement in the file is not part of the scene", root.GetParent() == null && scene.Instantiate().Name == "Arena");
        T.Check("before it is added, the tree around it is built", root.GetChildCount() == 4 && !root.IsInsideTree());
        T.Check("children are in file order with their classes", root.GetChild(0) is Marker3D && root.GetChild(1) is OmniLight3D && root.GetChild(2) is Emitter && root.GetChild(3) is StaticBody3D);
        var order = new List<string>();
        root.GetChild(2).TreeEntered += () => order.Add("gate entered");
        tree.Root.AddChild(root);
        var arena = (ArenaNode)root;
        T.Check("exported values arrive before _EnterTree", arena.SpeedAtEnter == 7.5f && arena.Speed == 7.5f && arena.Title == "Arena One" && arena.Lives == 4, $"{arena.SpeedAtEnter} {arena.Title} {arena.Lives}");
        T.Check("groups are joined", arena.IsInGroup("arenas") && arena.IsInGroup("level") && tree.GetFirstNodeInGroup("level") == arena && arena.GetNode("Floor").IsInGroup("ground"));
        T.Check("the parent's PLACE (100, 200, 300 cm) is a Godot position of (2, 3, -1) m", T.Near(arena.Position, new Vector3(2, 3, -1)), arena.Position.ToString());
        var crate = arena.GetNode<Marker3D>("Crate");
        T.Check("a child's PLACE with yaw -90 and (300, 400, 50) cm is (4, 0.5, -3) m turned +90 degrees about Y", T.Near(crate.Position, new Vector3(4, 0.5f, -3)) && T.Near(crate.Basis * new Vector3(1, 0, 0), new Vector3(0, 0, -1)),
                $"{crate.Position} {crate.Basis * new Vector3(1, 0, 0)}");
        var lamp = arena.GetNode<OmniLight3D>("Lamp");
        T.Check("class properties reach the node", T.Near(lamp.LightEnergy, 2f) && T.Near(lamp.OmniRange, 8f));
        T.Check("the owner and the %Unique name come from _owner and _unique", crate.Owner == arena && arena.GetNode("%Crate") == crate);
        var floor = arena.GetNode<StaticBody3D>("Floor");
        T.Check("a physics material written as a dictionary becomes a PhysicsMaterial", floor.PhysicsMaterialOverride is { } pm && T.Near(pm.Friction, 0.25f) && T.Near(pm.Bounce, 0.5f) && pm.Rough && !pm.Absorbent);
        var gate = arena.GetNode<Emitter>("Gate");
        gate.EmitSignal("Hit", 12);
        T.CheckSeq("the scene's [connection] reaches the root's method", new[] { "gate hit 12" }, arena.Calls);
        T.Check("the scene is current", tree.CurrentScene == arena);

        // The fixture's second scene: a nested child with an inline shape resource.
        Node crateScene = ResourceLoader.Load<PackedScene>("res://props/crate.tscn")!.Instantiate();
        tree.Root.AddChild(crateScene);
        var body = (RigidBody3D)crateScene;
        T.Check("a nested node has its inline resource built from Object(...)", body.Mass == 5f && body.CollisionLayer == 4 && body.GetNode<CollisionShape3D>("Shape").Shape is BoxShape3D { Size: var sz } && sz == new Vector3(1, 2, 3));
        T.Check("and the generic Instantiate<T> checks the type", ResourceLoader.Load<PackedScene>("res://props/crate.tscn")!.Instantiate<RigidBody3D>() != null);
    }

    private static void PackedSceneMore()
    {
        var tree = SceneTree.Instance;
        PackedScene scene = Arena();
        Node a = scene.Instantiate(), b = scene.Instantiate();
        T.Check("each instance is separate", a != b && a.GetChild(0) != b.GetChild(0));
        tree.Root.AddChild(a);
        tree.Root.AddChild(b);
        T.Check("two instances take distinct sibling names", a.Name != b.Name && a.Name == "Arena");
        T.Check("and both are in the group", tree.GetNodesInGroup("arenas").Count == 2);

        // A scene with a script class that is not known falls back to a Node3D and says so.
        string text = "OCWORLD 1\nPLACE none 0 0 0 0 0 0 1 class NoSuchClass name Odd\n  COMPONENT Props _name=%22Odd%22\n";
        PackedScene odd = PackedScene.FromText(text, "res://odd.tscn");
        Node oddNode = odd.Instantiate();
        T.Check("an unknown class is a Node3D with a warning", oddNode is Node3D && oddNode.GetType() == typeof(Node3D) && T.Logs.Any(l => l.Level == Aver.Scripting.Log.Level.Warn && l.Message.Contains("NoSuchClass")));
        Node plain = PackedScene.FromText("OCWORLD 1\nPLACE Meshes/box.ocmesh 0 0 0 0 0 0 1\n").Instantiate();
        tree.Root.AddChild(plain);
        T.Check("a plain mesh placement is a MeshInstance3D with that mesh", plain is MeshInstance3D { Mesh.ResourcePath: "Meshes/box.ocmesh" });

        PackedScene bad = new PackedScene();
        T.Check("an empty scene cannot be instantiated", !bad.CanInstantiate() && bad.Instantiate() == null && T.ErrorCount("cannot instantiate") == 1);
        bool threw = false;
        try { PackedScene.FromText("PLACE none 0 0 0 0 0 0 1 class Node\nBEGIN\n"); } catch (FormatException) { threw = true; }
        T.Check("a level with an unclosed BEGIN is refused", threw);
        threw = false;
        try { PackedScene.FromText("OCWORLD 1\nEND\n"); } catch (FormatException) { threw = true; }
        T.Check("and an END with no BEGIN", threw);
        T.Check("percent decoding keeps UTF-8 and stray percent signs", OcWorldScene.PercentDecode("a%20b%C3%A9%zz%") == "a bé%zz%");

        // pack stores a copy: later changes to the original do not reach new instances.
        var original = new Hero { Name = "Original", Speed = 3f };
        original.AddChild(new Node3D { Name = "Kid" });
        var packed = new PackedScene();
        packed.Pack(original);
        original.Speed = 99f;
        var instance = (Hero)packed.Instantiate();
        T.Check("pack stores a copy of the node tree", instance != original && instance.Speed == 3f && instance.GetChildCount() == 1 && instance.GetChild(0).Name == "Kid");
    }
}

class Holder : Resource
{
    public Resource? Shape { get; set; }
}

class NoteRecorder : Node
{
    public List<string> Notes = new();
    public override void _Notification(int what)
    {
        if (what == NotificationPaused) Notes.Add(Name + " paused");
        else if (what == NotificationUnpaused) Notes.Add(Name + " unpaused");
    }
}
