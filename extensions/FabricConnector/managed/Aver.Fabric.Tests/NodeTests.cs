// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// NodeTests: the tree API (children, names, paths, unique names, owner, groups, free) and the lifecycle order, run by the managed
// host, which does what the framework's spawn batch does in the engine.

using Aver.Fabric;

static class NodeTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("tree: add, remove, move, reparent, indices", TreeEditing);
        T.Run("tree: refused edits", RefusedEdits);
        T.Run("names: clashes, auto names, rename signal", Names);
        T.Run("paths: relative, absolute, '..', '.', unique names", Paths);
        T.Run("paths: GetPath, GetPathTo, FindChild, FindChildren", PathQueries);
        T.Run("groups: tree order, call_group, leaving", Groups);
        T.Run("lifecycle: enter is pre-order, ready is post-order", EnterReadyOrder);
        T.Run("lifecycle: signals and notifications around enter and ready", LifecycleSignals);
        T.Run("lifecycle: adding a subtree to a tree that is running", LateAdd);
        T.Run("lifecycle: exit order, re-adding does not repeat _Ready", ExitOrder);
        T.Run("lifecycle: Free and QueueFree", FreeAndQueueFree);
        T.Run("processing: which virtuals tick, switches, priorities, modes", ProcessingFlags);
        T.Run("processing: _Process, _PhysicsProcess, input routing", ProcessCalls);
        T.Run("exported values: applied before _EnterTree, by name, with owner and unique name", ExportedValues);
        T.Run("scene connections: wired when the node is ready, with binds, flags and missing targets", SceneConnections);
        T.Run("duplicate", DuplicateNodes);
        T.Run("SceneTree: groups, pause, timers", SceneTreeApi);
        T.Run("the scripting bridge can declare and bind every node class", BridgeContract);
    });

    // The bridge declares each concrete Node type of Aver.Fabric as a registry class named after the type and binds it with
    // Activator.CreateInstance: so each needs a public parameterless constructor and a name nothing else uses.
    private static void BridgeContract()
    {
        var types = typeof(Node).Assembly.GetTypes().Where(t => typeof(Node).IsAssignableFrom(t) && !t.IsAbstract).ToList();
        T.Check("there are node classes", types.Count >= 24, $"{types.Count}");
        T.Check("each has a public parameterless constructor", types.All(t => t.GetConstructor(Type.EmptyTypes) != null),
                string.Join(", ", types.Where(t => t.GetConstructor(Type.EmptyTypes) == null).Select(t => t.Name)));
        var names = types.Select(Aver.Framework.ClassNames.Of).ToList();
        T.Check("their registry names are distinct", names.Distinct().Count() == names.Count);
        var reserved = new[] { "Actor", "Pawn", "Character", "PlayerController", "GameMode", "GameInstance" };
        T.Check("and none is a framework base class", !names.Intersect(reserved).Any());
        foreach (string godot in new[] { "Node", "Node3D", "MeshInstance3D", "Camera3D", "OmniLight3D", "SpotLight3D", "DirectionalLight3D", "StaticBody3D", "RigidBody3D",
                                        "CharacterBody3D", "Area3D", "CollisionShape3D", "Marker3D", "Timer", "Control", "CanvasItem" })
            T.Check($"{godot} is a class a level can place", names.Contains(godot));
        T.Check("non-node types are not declared as classes", !names.Contains("Resource") && !names.Contains("SceneTreeTimer"));
    }

    // Main
    //  |- Player
    //  |   `- Gun
    //  |- Enemy
    //  `- Hud
    //      `- Score
    private static Rec BuildMain(out Rec player, out Rec gun, out Rec enemy, out Rec hud, out Rec score)
    {
        var main = new Rec("Main");
        player = new Rec("Player"); gun = new Rec("Gun"); enemy = new Rec("Enemy"); hud = new Rec("Hud"); score = new Rec("Score");
        main.AddChild(player); player.AddChild(gun); main.AddChild(enemy); main.AddChild(hud); hud.AddChild(score);
        foreach (Node n in new Node[] { player, gun, enemy, hud, score }) n.Owner = main;
        hud.UniqueNameInOwner = true; score.UniqueNameInOwner = true;
        return main;
    }

    private static void TreeEditing()
    {
        var a = new Node(); var b = new Node(); var c = new Node();
        a.Name = "A"; b.Name = "B"; c.Name = "C";
        var p = new Node { Name = "P" };
        p.AddChild(a); p.AddChild(b); p.AddChild(c);
        T.Check("children keep the order they were added in", p.GetChildCount() == 3 && p.GetChild(0) == a && p.GetChild(2) == c && p.GetChild(-1) == c);
        T.Check("GetChildren is a snapshot array", p.GetChildren().Select(n => n.Name.ToString()).SequenceEqual(new[] { "A", "B", "C" }));
        T.Check("parent and index", a.GetParent() == p && b.GetIndex() == 1 && p.GetParent() == null);
        p.MoveChild(c, 0);
        T.Check("move_child", p.GetChildren().Select(n => n.Name.ToString()).SequenceEqual(new[] { "C", "A", "B" }));
        p.MoveChild(c, -1);
        T.Check("move_child to the end with -1", p.GetChild(2) == c);
        p.RemoveChild(b);
        T.Check("remove_child detaches without freeing", b.GetParent() == null && p.GetChildCount() == 2 && GodotObject.IsInstanceValid(b));
        var q = new Node { Name = "Q" };
        q.AddChild(b);
        T.Check("a removed node can be added elsewhere", b.GetParent() == q);
        a.Reparent(q);
        T.Check("reparent", a.GetParent() == q && p.GetChildCount() == 1 && q.GetChildren().Select(n => n.Name.ToString()).SequenceEqual(new[] { "B", "A" }));
        T.Check("is_ancestor_of", p.IsAncestorOf(c) && !p.IsAncestorOf(a) && !c.IsAncestorOf(p));
        var sib = new Node { Name = "S" };
        b.AddSibling(sib);
        T.Check("add_sibling places it right after", q.GetChildren().Select(n => n.Name.ToString()).SequenceEqual(new[] { "B", "S", "A" }));
        var repl = new Node { Name = "R" };
        var kid = new Node { Name = "K" };
        sib.AddChild(kid);
        sib.ReplaceBy(repl);
        T.Check("replace_by takes the place and the children", q.GetChild(1) == repl && repl.GetChild(0) == kid && sib.GetParent() == null);
    }

    private static void RefusedEdits()
    {
        var p = new Node { Name = "P" }; var c = new Node { Name = "C" };
        p.AddChild(c);
        bool twice = false, self = false, cycle = false, strangerRemove = false;
        try { new Node().AddChild(c); } catch (InvalidOperationException) { twice = true; }
        try { p.AddChild(p); } catch (InvalidOperationException) { self = true; }
        try { c.AddChild(p); } catch (InvalidOperationException) { cycle = true; }
        try { new Node().RemoveChild(c); } catch (InvalidOperationException) { strangerRemove = true; }
        T.Check("a node with a parent cannot be added again", twice);
        T.Check("a node cannot be its own child", self);
        T.Check("an ancestor cannot become a child", cycle);
        T.Check("remove_child of a non-child is refused", strangerRemove);
        bool range = false;
        try { p.GetChild(5); } catch (ArgumentOutOfRangeException) { range = true; }
        T.Check("get_child out of range throws", range);
    }

    private static void Names()
    {
        var p = new Node { Name = "P" };
        var a = new Node { Name = "Foo" }; var b = new Node { Name = "Foo" }; var c = new Node { Name = "Foo" };
        p.AddChild(a); p.AddChild(b); p.AddChild(c);
        T.Check("a clashing name becomes @Foo@N", a.Name == "Foo" && b.Name == "@Foo@2" && c.Name == "@Foo@3");
        var d = new Node(); var e = new Node();
        p.AddChild(d); p.AddChild(e);
        T.Check("an unnamed node is named after its class: @Node@N", d.Name == "@Node@2" && e.Name == "@Node@3");
        var f = new Node { Name = "Enemy" }; var g = new Node { Name = "Enemy" };
        p.AddChild(f); p.AddChild(g, forceReadableName: true);
        T.Check("force_readable_name gives Enemy2", g.Name == "Enemy2");
        var h = new Node();
        p.AddChild(h, true);
        T.Check("and a readable default name", h.Name == "Node");
        int renamed = 0;
        a.Connect("renamed", Callable.From(() => renamed++));
        a.Name = "Bar";
        T.Check("renaming emits 'renamed' and changes the name", renamed == 1 && a.Name == "Bar");
        a.Name = "Bar";
        T.Check("setting the same name is quiet", renamed == 1);
        b.Name = "Bar";
        T.Check("renaming into a clash gets a unique name", b.Name == "@Bar@2");
        a.Name = "wi/th:ba@d.chars";
        T.Check("illegal characters are removed", a.Name == "withbadchars");
        T.Check("the node can be found by its new name", p.GetNode("withbadchars") == a);
    }

    private static void Paths()
    {
        Rec main = BuildMain(out var player, out var gun, out var enemy, out var hud, out var score);
        T.Check("relative path", main.GetNode("Player/Gun") == gun && main.GetNode<Rec>("Enemy") == enemy);
        T.Check("parent with '..'", gun.GetNode("../../Enemy") == enemy && player.GetNode("..") == main);
        T.Check("'.' is itself", player.GetNode(".") == player && player.GetNode("./Gun") == gun);
        T.Check("a missing node is null with GetNodeOrNull", main.GetNodeOrNull("Nope") == null && main.GetNodeOrNull("Player/Nope") == null && !main.HasNode("Nope"));
        bool threw = false;
        try { main.GetNode("Nope"); } catch (InvalidOperationException ex) { threw = ex.Message.Contains("Nope"); }
        T.Check("GetNode throws with the path in the message", threw);
        bool typeThrew = false;
        try { main.GetNode<Aver.Fabric.Timer>("Player"); } catch (InvalidOperationException) { typeThrew = true; }
        T.Check("GetNode<T> of the wrong type throws", typeThrew);
        T.Check("GetNodeOrNull<T> of the wrong type is null", main.GetNodeOrNull<Aver.Fabric.Timer>("Player") == null && main.GetNodeOrNull<Rec>("Player") == player);
        T.Check("empty path is null", main.GetNodeOrNull("") == null);
        T.Check("'..' above the top is null", main.GetNodeOrNull("..") == null);

        T.Check("unique name from inside the scene", gun.GetNode("%Hud") == hud && gun.GetNode("%Hud/Score") == score && enemy.GetNode("%Score") == score);
        T.Check("unique name from the owner", main.GetNode("%Score") == score);
        score.UniqueNameInOwner = false;
        T.Check("no longer unique", main.GetNodeOrNull("%Score") == null && hud.GetNodeOrNull("Score") == score);

        // Absolute paths need the tree: the first name is the root window's, "root".
        SceneTree.Instance.Root.AddChild(main);
        T.Check("absolute path from anywhere", gun.GetNode("/root/Main/Player/Gun") == gun && enemy.GetNode("/root/Main") == main);
        T.Check("the window's own name is root", SceneTree.Instance.Root.Name == "root" && main.GetParent() == SceneTree.Instance.Root);
        T.Check("absolute path with a wrong first name is null", gun.GetNodeOrNull("/wrong/Main") == null);
    }

    private static void PathQueries()
    {
        Rec main = BuildMain(out var player, out var gun, out var enemy, out var hud, out var score);
        T.Check("get_path_to", gun.GetPathTo(enemy).ToString() == "../../Enemy" && main.GetPathTo(gun).ToString() == "Player/Gun" && gun.GetPathTo(gun).ToString() == ".");
        bool noTree = false;
        try { _ = gun.GetPath(); } catch (InvalidOperationException) { noTree = true; }
        T.Check("get_path needs the tree", noTree);
        SceneTree.Instance.Root.AddChild(main);
        T.Check("get_path", gun.GetPath().ToString() == "/root/Main/Player/Gun" && main.GetPath().ToString() == "/root/Main");
        T.Check("find_child by wildcard", main.FindChild("Sc*") == score && main.FindChild("?un") == gun && main.FindChild("*", true) == player);
        T.Check("find_child without recursion", main.FindChild("Gun", false) == null && main.FindChild("Player", false) == player);
        T.Check("find_child owned-only skips unowned nodes", ((Func<bool>)(() => { var x = new Rec("Loose"); main.AddChild(x); return main.FindChild("Loose") == null && main.FindChild("Loose", true, false) == x; }))());
        T.Check("find_children", main.FindChildren("*u*", "", true, true).Select(n => n.Name.ToString()).SequenceEqual(new[] { "Gun", "Hud" }));
        T.Check("find_parent", gun.FindParent("Ma*") == main && gun.FindParent("Zzz") == null);
    }

    private static void Groups()
    {
        Rec main = BuildMain(out var player, out var gun, out var enemy, out var hud, out var score);
        SceneTree.Instance.Root.AddChild(main);
        // Joined in an order that is not the tree's.
        score.AddToGroup("g_units"); enemy.AddToGroup("g_units"); gun.AddToGroup("g_units"); main.AddToGroup("g_units");
        T.Check("is_in_group and get_groups", gun.IsInGroup("g_units") && !hud.IsInGroup("g_units") && gun.GetGroups().Count == 1);
        T.Check("get_nodes_in_group is in tree order", SceneTree.Instance.GetNodesInGroup("g_units").Select(n => n.Name.ToString()).SequenceEqual(new[] { "Main", "Gun", "Enemy", "Score" }));
        T.Check("first node in group and has_group", SceneTree.Instance.GetFirstNodeInGroup("g_units") == main && SceneTree.Instance.HasGroup("g_units") && !SceneTree.Instance.HasGroup("g_none"));
        gun.AddToGroup("g_units");
        T.Check("joining twice is harmless", SceneTree.Instance.GetNodesInGroup("g_units").Count == 4);
        enemy.RemoveFromGroup("g_units");
        T.Check("remove_from_group", SceneTree.Instance.GetNodesInGroup("g_units").Count == 3 && !enemy.IsInGroup("g_units"));
        var calls = new List<string>();
        foreach (Node n in SceneTree.Instance.GetNodesInGroup("g_units")) n.SetMeta("hits", 0);
        SceneTree.Instance.CallGroup("g_units", "set_meta", "hits", 5);
        T.Check("call_group calls a method on each member", SceneTree.Instance.GetNodesInGroup("g_units").All(n => n.GetMeta("hits").AsInt32() == 5));
        SceneTree.Instance.SetGroup("g_units", "name", "X");
        T.Check("set_group sets a property", main.Name == "X");
        main.Name = "Main";
        player.RemoveChild(gun);
        T.Check("a node outside the tree is not in the group's list", !SceneTree.Instance.GetNodesInGroup("g_units").Contains(gun));
        score.Free();
        T.Check("a freed node leaves its groups", !SceneTree.Instance.GetNodesInGroup("g_units").Contains(score));
        _ = calls;
    }

    private static void EnterReadyOrder()
    {
        // A( B( C, D ), E )
        var a = new Rec("A"); var b = new Rec("B"); var c = new Rec("C"); var d = new Rec("D"); var e = new Rec("E");
        a.AddChild(b); b.AddChild(c); b.AddChild(d); a.AddChild(e);
        T.Check("nothing but 'parented' runs while the tree is not in the scene tree", Rec.Trail.All(s => s.StartsWith("parented")) && !a.IsInsideTree());
        Rec.Trail.Clear();
        SceneTree.Instance.Root.AddChild(a);
        T.CheckSeq("_EnterTree parents first, then _Ready children first",
            new[] { "parented A", "enter A", "enter B", "enter C", "enter D", "enter E", "ready C", "ready D", "ready B", "ready E", "ready A" }, Rec.Trail);
        T.Check("every node knows it is in the tree and ready", new Node[] { a, b, c, d, e }.All(n => n.IsInsideTree() && n.IsNodeReady()));
    }

    private static void LifecycleSignals()
    {
        var parent = new Rec("P"); var child = new Rec("C");
        var order = new List<string>();
        child.Connect("tree_entered", Callable.From(() => order.Add("C.tree_entered")));
        child.Connect("ready", Callable.From(() => order.Add("C.ready")));
        child.Connect("tree_exiting", Callable.From(() => order.Add("C.tree_exiting")));
        child.Connect("tree_exited", Callable.From(() => order.Add("C.tree_exited")));
        parent.Connect("child_entered_tree", Callable.From((Node n) => order.Add("P.child_entered " + n.Name)));
        parent.Connect("child_exiting_tree", Callable.From((Node n) => order.Add("P.child_exiting " + n.Name)));
        parent.Connect("child_order_changed", Callable.From(() => order.Add("P.order_changed")));
        parent.AddChild(child);
        SceneTree.Instance.Root.AddChild(parent);
        parent.RemoveChild(child);
        T.CheckSeq("signals interleave with the virtuals in Godot's order",
            new[] { "P.order_changed", "C.tree_entered", "P.child_entered C", "C.ready", "P.child_exiting C", "C.tree_exiting", "C.tree_exited", "P.order_changed" }, order);
        int nodeAdded = 0, nodeRemoved = 0;
        SceneTree.Instance.Connect("node_added", Callable.From((Node n) => nodeAdded++));
        SceneTree.Instance.Connect("node_removed", Callable.From((Node n) => nodeRemoved++));
        parent.AddChild(child);
        parent.RemoveChild(child);
        T.Check("the tree announces added and removed nodes", nodeAdded == 1 && nodeRemoved == 1);
        Rec.Trail.Clear();
        var lone = new Rec("Lone");
        parent.AddChild(lone);
        T.Check("parented comes before enter", Rec.Trail.IndexOf("parented Lone") < Rec.Trail.IndexOf("enter Lone"));
        parent.RemoveChild(lone);
        T.Check("unparented after removal", Rec.Trail.Contains("unparented Lone"));
    }

    private static void LateAdd()
    {
        var root = new Rec("Root");
        SceneTree.Instance.Root.AddChild(root);
        var branch = new Rec("Branch"); var leaf1 = new Rec("Leaf1"); var leaf2 = new Rec("Leaf2");
        branch.AddChild(leaf1); branch.AddChild(leaf2);
        Rec.Trail.Clear();
        root.AddChild(branch);
        T.CheckSeq("a subtree enters in order and is ready before add_child returns",
            new[] { "parented Branch", "enter Branch", "enter Leaf1", "enter Leaf2", "ready Leaf1", "ready Leaf2", "ready Branch" }, Rec.Trail);
        var late = new Rec("Late");
        Rec.Trail.Clear();
        leaf1.AddChild(late);
        T.CheckSeq("a single node under a running parent", new[] { "parented Late", "enter Late", "ready Late" }, Rec.Trail);
    }

    private static void ExitOrder()
    {
        var a = new Rec("A"); var b = new Rec("B"); var c = new Rec("C"); var d = new Rec("D"); var e = new Rec("E");
        a.AddChild(b); b.AddChild(c); b.AddChild(d); a.AddChild(e);
        SceneTree.Instance.Root.AddChild(a);
        Rec.Trail.Clear();
        a.RemoveChild(b);
        T.CheckSeq("a subtree leaves last child first, the node after its children", new[] { "exit D", "exit C", "exit B", "unparented B" }, Rec.Trail);
        T.Check("it is out of the tree but still ready and alive", !b.IsInsideTree() && !c.IsInsideTree() && b.IsNodeReady() && GodotObject.IsInstanceValid(b));
        Rec.Trail.Clear();
        a.AddChild(b);
        T.CheckSeq("adding it back enters again but _Ready is not repeated", new[] { "parented B", "enter B", "enter C", "enter D" }, Rec.Trail);
        Rec.Trail.Clear();
        SceneTree.Instance.Root.RemoveChild(a);
        // B was added back after E, so A's children are [E, B]; the last child leaves first.
        T.CheckSeq("the whole tree leaves, last child first", new[] { "exit D", "exit C", "exit B", "exit E", "exit A", "unparented A" }, Rec.Trail);
    }

    private static void FreeAndQueueFree()
    {
        var a = new Rec("A"); var b = new Rec("B"); var c = new Rec("C");
        a.AddChild(b); b.AddChild(c);
        SceneTree.Instance.Root.AddChild(a);
        Rec.Trail.Clear();
        b.Free();
        T.CheckSeq("free exits the subtree children first", new[] { "exit C", "exit B" }, Rec.Trail);
        T.Check("the node and its children are invalid and gone from the parent", !GodotObject.IsInstanceValid(b) && !GodotObject.IsInstanceValid(c) && a.GetChildCount() == 0);
        Rec.Trail.Clear();
        var d = new Rec("D"); var e = new Rec("E");
        a.AddChild(d); d.AddChild(e);
        Rec.Trail.Clear();
        d.QueueFree();
        T.Check("queue_free marks it but changes nothing yet", d.IsQueuedForDeletion() && GodotObject.IsInstanceValid(d) && d.IsInsideTree() && Rec.Trail.Count == 0);
        d.QueueFree();
        Aver.Framework.Hosting.ExtensionHost.FlushDeferred();
        T.CheckSeq("at the flush it is freed (once)", new[] { "exit E", "exit D" }, Rec.Trail);
        T.Check("and children go with it", !GodotObject.IsInstanceValid(d) && !GodotObject.IsInstanceValid(e) && a.GetChildCount() == 0);
        var lone = new Rec("Lone");
        lone.Free();
        T.Check("a node that never entered the tree can be freed", !GodotObject.IsInstanceValid(lone) && Rec.Trail.Count == 2);
        int exited = 0;
        var f = new Rec("F");
        a.AddChild(f);
        f.Connect("tree_exited", Callable.From(() => exited++));
        f.Free();
        T.Check("tree_exited is emitted on free", exited == 1);
    }

    private static void ProcessingFlags()
    {
        var hero = new Hero(); var idle = new Idle(); var input = new InputNode();
        T.Check("a class that overrides _Process and _PhysicsProcess ticks", hero.TickEnabled && hero.PhysicsTickEnabled && hero.IsProcessing() && hero.IsPhysicsProcessing());
        T.Check("a class that overrides neither does not", !idle.TickEnabled && !idle.PhysicsTickEnabled && !idle.InputEnabled);
        T.Check("input virtuals switch input on", input.InputEnabled && input.IsProcessingInput() && input.IsProcessingUnhandledInput() && input.IsProcessingUnhandledKeyInput());
        hero.SetProcess(false);
        T.Check("set_process(false) turns the frame tick off", !hero.TickEnabled && !hero.IsProcessing() && hero.PhysicsTickEnabled);
        hero.SetPhysicsProcess(false);
        T.Check("set_physics_process(false) the other", !hero.PhysicsTickEnabled);
        hero.SetProcess(true);
        T.Check("and back on", hero.TickEnabled);
        idle.SetProcess(true);
        T.Check("a node can opt in without an override", idle.TickEnabled);
        hero.ProcessPriority = 7; hero.ProcessPhysicsPriority = -3;
        T.Check("process priorities map to tick priorities", hero.TickPriority == 7 && hero.PhysicsTickPriority == -3 && hero.ProcessPriority == 7);
        hero.ProcessMode = Node.ProcessModeEnum.WhenPaused;
        T.Check("process_mode maps to the framework's pause mode", hero.PauseMode == Aver.Framework.PauseMode.WhenPaused && hero.ProcessMode == Node.ProcessModeEnum.WhenPaused);
        foreach (Node.ProcessModeEnum m in Enum.GetValues<Node.ProcessModeEnum>())
        {
            hero.ProcessMode = m;
            if ((int)hero.PauseMode != (int)m) { T.Check("process mode numbering matches", false); break; }
        }
        T.Check("the numbering of Inherit, Pausable, WhenPaused, Always, Disabled is shared", (int)Node.ProcessModeEnum.Always == (int)Aver.Framework.PauseMode.Always && (int)Node.ProcessModeEnum.Disabled == (int)Aver.Framework.PauseMode.Disabled);

        // can_process follows the nearest explicit mode up the tree and the tree's pause.
        var top = new Rec("Top"); var mid = new Rec("Mid"); var low = new Rec("Low");
        top.AddChild(mid); mid.AddChild(low);
        SceneTree.Instance.Root.AddChild(top);
        T.Check("a running tree lets nodes process", low.CanProcess());
        SceneTree.Instance.Paused = true;
        T.Check("a paused tree stops Inherit nodes", !low.CanProcess());
        mid.ProcessMode = Node.ProcessModeEnum.Always;
        T.Check("an Always ancestor lets descendants that inherit keep going", low.CanProcess() && mid.CanProcess() && !top.CanProcess());
        low.ProcessMode = Node.ProcessModeEnum.Disabled;
        T.Check("Disabled never processes", !low.CanProcess());
        low.ProcessMode = Node.ProcessModeEnum.WhenPaused;
        T.Check("WhenPaused processes only while paused", low.CanProcess());
        SceneTree.Instance.Paused = false;
        T.Check("and not while running", !low.CanProcess());
    }

    private static void ProcessCalls()
    {
        var hero = new Hero();
        hero.OnTick(0.016f);
        hero.OnPhysicsTick(1f / 60f);
        T.Check("nothing ticks outside the tree", hero.Frames.Count == 0 && hero.Steps.Count == 0);
        SceneTree.Instance.Root.AddChild(hero);
        hero.OnTick(0.25f); hero.OnTick(0.5f);
        hero.OnPhysicsTick(1f / 60f);
        T.Check("_Process gets the frame delta as a double", hero.Frames.SequenceEqual(new[] { 0.25, 0.5 }));
        T.Check("_PhysicsProcess gets the fixed step", hero.Steps.Count == 1 && Math.Abs(hero.Steps[0] - 1.0 / 60.0) < 1e-6);
        hero.SetProcess(false);
        hero.OnTick(1f);
        T.Check("after set_process(false) _Process is not called", !hero.TickEnabled && hero.Frames.Count == 2);

        var inp = new InputNode();
        SceneTree.Instance.Root.AddChild(inp);
        var key = new Aver.Framework.InputEventKey(Aver.Framework.Key.A, true);
        var click = new Aver.Framework.InputEventMouseButton(Aver.Framework.Key.MouseLeft, true);
        inp.OnInput(key); inp.OnUnhandledInput(key); inp.OnUnhandledInput(click);
        T.Check("_Input, then _UnhandledInput and _UnhandledKeyInput for a key", inp.Seen.Count == 4 && inp.Seen[0] == "input A" && inp.Seen[1] == "unhandled A" &&
                                                                                inp.Seen[2] == "unhandled key A" && inp.Seen[3] == "unhandled Mouse Button Left", string.Join(" | ", inp.Seen));
        inp.SetProcessInput(false);
        inp.Seen.Clear();
        inp.OnInput(key);
        T.Check("set_process_input(false) stops _Input", inp.Seen.Count == 0 && inp.InputEnabled);
        inp.SetProcessUnhandledInput(false); inp.SetProcessUnhandledKeyInput(false);
        T.Check("with all three off the node takes no input", !inp.InputEnabled);
    }

    private static void ExportedValues()
    {
        var main = new Rec("Main");
        SceneTree.Instance.Root.AddChild(main);
        var hero = new Hero { Name = "Hero" };
        hero.PendingValues = new List<(string, string)>
        {
            ("_name", "Renamed"), ("speed", "12.5"), ("start_pos", "Vector3(1, 2, 3)"), ("title", "\"The Hero\""), ("lives", "9"),
            ("tint", "Color(1, 0, 0, 1)"), ("items", "[1, 2, \"x\"]"), ("not_a_property", "42"), ("_owner", ".."), ("_unique", "1"),
        };
        main.AddChild(hero);
        T.Check("values are in before _EnterTree runs", hero.Entered == 1 && hero.SpeedAtEnter == 12.5f);
        T.Check("floats, vectors, strings, a property, colours and arrays", hero.Speed == 12.5f && hero.StartPos == new Vector3(1, 2, 3) && hero.Title == "The Hero" && hero.Lives == 9 &&
                                                                                hero.Tint == new Color(1, 0, 0, 1) && hero.Items.Count == 3 && hero.Items[2].AsString() == "x");
        T.Check("_name renames the node", hero.Name == "Renamed" && main.GetNode("Renamed") == hero);
        T.Check("_owner and _unique make %Renamed work", hero.Owner == main && main.GetNode("%Renamed") == hero);
        T.Check("an unknown property is kept as metadata and warned about", hero.GetMeta("not_a_property").AsInt32() == 42 && T.Logs.Any(l => l.Message.Contains("not_a_property")));
        var bad = new Hero();
        bad.PendingValues = new List<(string, string)> { ("speed", "Vector3(oops"), ("lives", "7") };
        main.AddChild(bad);
        T.Check("an unreadable value is skipped with a warning and the rest still apply", bad.Speed == 5f && bad.Lives == 7 && T.Logs.Any(l => l.Message.Contains("'speed'")));
        var nodeSettings = new Rec("Cfg");
        nodeSettings.PendingValues = new List<(string, string)> { ("process_mode", "3"), ("process_priority", "4") };
        main.AddChild(nodeSettings);
        T.Check("built-in Node properties by Godot name", nodeSettings.ProcessMode == Node.ProcessModeEnum.Always && nodeSettings.ProcessPriority == 4);
    }

    private static void SceneConnections()
    {
        var root = new Rec("Root");
        var src = new Emitter { Name = "Src" };
        var dst = new Emitter { Name = "Dst" };
        root.AddChild(src); root.AddChild(dst);
        src.PendingValues = new List<(string, string)>
        {
            ("_connections",
             "[{ \"signal\": \"Hit\", \"to\": \"../Dst\", \"method\": \"on_hit\", \"flags\": 0, \"binds\": [], \"unbinds\": 0 }," +
             " { \"signal\": \"Named\", \"to\": \"../Dst\", \"method\": \"on_named\", \"flags\": 0, \"binds\": [3.5] }," +
             " { \"signal\": \"Hit\", \"to\": \"../Dst\", \"method\": \"second\", \"flags\": 4 }," +
             " { \"signal\": \"Died\", \"to\": \"../Nope\", \"method\": \"on_died\" }]"),
        };
        T.Check("nothing is connected before the node is in the tree", !src.HasConnections("Hit"));
        SceneTree.Instance.Root.AddChild(root);
        T.Check("the connections exist once it is", src.GetSignalConnections("Hit").Length == 2 && src.HasConnections("Named"));
        src.EmitSignal("Hit", 9);
        T.Check("a signal reaches the target's method, by its GDScript-style name", dst.Last == 9 && dst.Got.Contains("hit 9"));
        T.Check("flag 4 (one-shot) from the scene is honoured", dst.Got.Contains("second 9") && src.GetSignalConnections("Hit").Length == 1);
        src.EmitSignal("Hit", 10);
        T.Check("and the one-shot does not run again", dst.Got.Count(g => g.StartsWith("second")) == 1);
        src.EmitSignal("Named", "Zed");
        T.Check("binds follow the emitted arguments", dst.Got.Contains("named Zed 3.5"));
        T.Check("a target that is not in the scene is a warning, not a failure", T.Logs.Any(l => l.Level == Aver.Scripting.Log.Level.Warn && l.Message.Contains("../Nope")) && !src.HasConnections("Died"));
    }

    private static void DuplicateNodes()
    {
        var hero = new Hero { Name = "H", Speed = 3.5f, Title = "t" };
        hero.AddToGroup("g_dup");
        var child = new Rec("Kid");
        hero.AddChild(child);
        var copy = (Hero)hero.Duplicate();
        T.Check("a duplicate has the same class, name and field values", copy != hero && copy.Name == "H" && copy.Speed == 3.5f && copy.Title == "t");
        T.Check("with copies of the children", copy.GetChildCount() == 1 && copy.GetChild(0) != child && copy.GetChild(0).Name == "Kid");
        T.Check("and the groups", copy.IsInGroup("g_dup"));
        var noGroups = (Hero)hero.Duplicate((int)Node.DuplicateFlags.Signals);
        T.Check("groups are copied only with the flag", !noGroups.IsInGroup("g_dup"));
    }

    private static void SceneTreeApi()
    {
        var tree = SceneTree.Instance;
        T.Check("paused is a switch", !tree.Paused && ((Func<bool>)(() => { tree.Paused = true; bool on = tree.Paused; tree.Paused = false; return on && !tree.Paused; }))());
        var timer = tree.CreateTimer(1.5);
        int fired = 0;
        timer.Connect("timeout", Callable.From(() => fired++));
        SceneTree.AdvanceTimers(1.0);
        T.Check("a scene tree timer waits", fired == 0 && timer.TimeLeft > 0.4);
        SceneTree.AdvanceTimers(1.0);
        T.Check("and fires once", fired == 1);
        SceneTree.AdvanceTimers(5.0);
        T.Check("never again", fired == 1);
        T.Check("the root window", tree.Root.Name == "root" && tree.Root.IsInsideTree() && tree.Root.GetClass() == "Window");
        T.Check("scene paths map to the converter's level files", SceneTree.ScenePathMapper("res://levels/main.tscn") == "Maps/levels/main.ocworld");
        T.Check("change_scene_to_file of a scene with no file behind it fails", tree.ChangeSceneToFile("res://a.tscn") == Error.FileNotFound);
        var n = new Rec("N");
        T.Check("get_tree is null outside the tree and the tree inside it", n.GetTree() == null && ((Func<bool>)(() => { tree.Root.AddChild(n); return n.GetTree() == tree && n.GetViewport() == tree.Root; }))());
    }
}
