// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// SceneValueTests: what a converted level writes into a node's values and the runtime reads back: classes by name (ClassDB), inline
// resources (Object(...)), resource paths, node-typed exports, and the class of a plain placement.

using Aver.Fabric;

class Follower : Node3D
{
    [Export] public Node3D? Target;
    [Export] public Node? Other { get; set; }
    [Export] public Marker3D? Wrong;
}

class ScriptShape : Shape3D { }

/// <summary>GodotSharp exports keep their PascalCase names; the level's keys arrive lower-cased.</summary>
class PascalExports : Node
{
    [Export] public float MoveSpeed = 1f;
    [Export] public int MaxHP { get; set; }
    [Export] public string _title = "";
}

static class SceneValueTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("ClassDB: names, instances, parents", ClassDb);
        T.Run("Object(...): an inline resource by class name", InlineObjects);
        T.Run("resources in node properties: shape, mesh and Resource(path)", ResourceProperties);
        T.Run("node-typed exports are NodePaths resolved against the scene", NodeReferences);
        T.Run("a plain placement takes the class its Props name", PlainPlacementClass);
        T.Run("signal names: GDScript snake_case and GodotSharp PascalCase are one signal", SignalSpellings);
        T.Run("lower-cased level keys reach PascalCase exports", LoweredKeys);
    });

    private static void LoweredKeys()
    {
        var main = new Rec("Main");
        SceneTree.Instance.Root.AddChild(main);
        var p = new PascalExports();
        p.PendingValues = new List<(string, string)> { ("movespeed", "4.5"), ("maxhp", "7"), ("title", "\"x\""), ("position", "Vector3(1, 0, 0)") };
        main.AddChild(p);
        T.Check("a field, a property and an underscore field", p.MoveSpeed == 4.5f && p.MaxHP == 7 && p._title == "x");
        T.Check("a key the class has no member for is still kept as metadata", p.GetMeta("position").AsVector3() == new Vector3(1, 0, 0));
    }

    private static void SignalSpellings()
    {
        var src = new Emitter { Name = "Src" };
        var dst = new Emitter { Name = "Dst" };
        T.Check("connect with the snake_case spelling", src.Connect("hit", new Callable(dst, "on_hit")) == Error.Ok);
        T.Check("it is the declared signal", src.HasSignal("hit") && src.HasSignal("Hit") && src.HasSignal("HIT"));
        src.EmitSignal("Hit", 5);
        T.Check("emitted by the declared spelling", dst.Last == 5);
        T.Check("is connected and listed under any spelling", src.IsConnected("Hit", new Callable(dst, "on_hit")) && src.HasConnections("hit") && src.GetSignalConnections("HIT").Length == 1);
        src.Disconnect("hit", new Callable(dst, "on_hit"));
        T.Check("disconnect with another spelling", !src.HasConnections("Hit"));
        T.Check("built-in signals keep their snake_case", src.HasSignal("tree_entered") && src.HasSignal("TreeEntered"));
        T.Check("an unknown signal is still unknown", !src.HasSignal("nope") && src.EmitSignal("nope") == Error.Unavailable);
        src.AddUserSignal("my_event");
        T.Check("user signals match the same way", src.HasSignal("MyEvent"));
    }

    private static void ClassDb()
    {
        T.Check("known and unknown", ClassDB.ClassExists("Node3D") && ClassDB.ClassExists("BoxShape3D") && !ClassDB.ClassExists("Nope"));
        T.Check("can instantiate", ClassDB.CanInstantiate("Marker3D") && !ClassDB.CanInstantiate("Nope"));
        Variant v = ClassDB.Instantiate("Marker3D");
        T.Check("instantiate gives the object", v.AsGodotObject() is Marker3D);
        T.Check("instantiate of nothing is nil", ClassDB.Instantiate("Nope").IsNil);
        T.Check("parent class", ClassDB.GetParentClass("BoxShape3D") == "Shape3D" && ClassDB.GetParentClass("Node3D") == "Node");
        T.Check("is parent class", ClassDB.IsParentClass("RigidBody3D", "Node3D") && ClassDB.IsParentClass("Node3D", "Node3D") && !ClassDB.IsParentClass("Node3D", "RigidBody3D"));
        T.Check("inheriters", ClassDB.GetInheritersFromClass("Shape3D").Contains("SphereShape3D") && !ClassDB.GetInheritersFromClass("Shape3D").Contains("Shape3D"));
        ClassDB.Register(typeof(ScriptShape));
        T.Check("a script class can join", ClassDB.ClassExists("ScriptShape") && ClassDB.Instantiate("ScriptShape").AsGodotObject() is ScriptShape);
        bool refused = false;
        try { ClassDB.Register(typeof(string)); } catch (ArgumentException) { refused = true; }
        T.Check("only GodotObjects can join", refused);
    }

    private static void InlineObjects()
    {
        Variant v = VariantText.Parse("Object(BoxShape3D, \"size\": Vector3(2, 3, 4), \"margin\": 0.1)");
        var box = v.AsGodotObject() as BoxShape3D;
        T.Check("a box shape with its size and margin", box != null && box.Size == new Vector3(2, 3, 4) && T.Near(box.Margin, 0.1f));
        T.Check("no properties", VariantText.Parse("Object(SphereShape3D)").AsGodotObject() is SphereShape3D { Radius: 0.5f });
        T.Check("spaces and a trailing comma", VariantText.Parse("Object( CapsuleShape3D , \"radius\" : 0.25 , )").AsGodotObject() is CapsuleShape3D { Radius: 0.25f });
        Variant nested = VariantText.Parse("[Object(SphereShape3D, \"radius\": 2), Object(SphereShape3D, \"radius\": 3)]");
        T.Check("inside an array", nested.AsGodotArray().Count == 2 && ((SphereShape3D)nested.AsGodotArray()[1].AsGodotObject()!).Radius == 3f);
        T.Check("an unknown class is bad text", !VariantText.TryParse("Object(Nope, \"a\": 1)", out _));
        T.Check("so is a missing close", !VariantText.TryParse("Object(SphereShape3D, \"radius\": 1", out _));
        T.Logs.Clear();
        Variant odd = VariantText.Parse("Object(SphereShape3D, \"nope\": 1, \"radius\": 4)");
        T.Check("an unknown property warns and the rest apply", T.Logs.Any(l => l.Message.Contains("nope")) && ((SphereShape3D)odd.AsGodotObject()!).Radius == 4f);
    }

    private static void ResourceProperties()
    {
        var main = new Rec("Main");
        SceneTree.Instance.Root.AddChild(main);
        var cs = new CollisionShape3D { Name = "Shape" };
        cs.PendingValues = new List<(string, string)> { ("shape", "Object(BoxShape3D, \"size\": Vector3(1, 2, 1))"), ("disabled", "true") };
        main.AddChild(cs);
        T.Check("a shape resource reaches the node", cs.Shape is BoxShape3D { Size.Y: 2f } && cs.Disabled);

        var mi = new MeshInstance3D { Name = "Mesh" };
        mi.PendingValues = new List<(string, string)> { ("mesh", "Resource(\"res://Meshes/door.ocmesh\")") };
        main.AddChild(mi);
        T.Check("Resource(path) becomes the property's own type", mi.Mesh is { } m && m.GetType() == typeof(Mesh) && m.ResourcePath == "res://Meshes/door.ocmesh");
        T.Check("the asset path drops res://", MeshInstance3D.AssetPath("res://Meshes/door.ocmesh") == "Meshes/door.ocmesh" && MeshInstance3D.AssetPath("Meshes/door.ocmesh") == "Meshes/door.ocmesh");

        var mi2 = new MeshInstance3D { Name = "Mesh2" };
        mi2.PendingValues = new List<(string, string)> { ("mesh", "Object(BoxMesh, \"size\": Vector3(1, 1, 3))") };
        main.AddChild(mi2);
        T.Check("an inline mesh", mi2.Mesh is BoxMesh { Size.Z: 3f });
    }

    private static void NodeReferences()
    {
        var main = new Rec("Main");
        SceneTree.Instance.Root.AddChild(main);
        // A subtree built off-tree, as a scene instantiation does: every node is linked before any enters.
        var scene = new Rec3D("Scene");
        var follower = new Follower { Name = "F" };
        follower.PendingValues = new List<(string, string)>
        {
            ("target", "NodePath(\"../Pal\")"), ("other", "NodePath(\"../Late/Deep\")"), ("wrong", "NodePath(\"../Pal\")"), ("_name", "F"),
        };
        var pal = new Rec3D("Pal");
        var late = new Rec("Late");
        late.AddChild(new Rec("Deep"));
        scene.AddChild(follower); scene.AddChild(pal); scene.AddChild(late);
        main.AddChild(scene);
        T.Check("a node-typed field gets the node", follower.Target == pal);
        T.Check("also when the target comes after it in the tree", follower.Other == late.GetNode("Deep"));
        T.Check("a node of the wrong type is not assigned", follower.Wrong == null);

        var lone = new Follower { Name = "Lone" };
        lone.PendingValues = new List<(string, string)> { ("target", "NodePath(\"../Missing\")"), ("other", "NodePath(\"\")") };
        T.Logs.Clear();
        main.AddChild(lone);
        T.Check("a missing target warns when the node is ready", lone.Target == null && T.Logs.Any(l => l.Message.Contains("../Missing")));
        T.Check("an empty path clears the field", lone.Other == null);
    }

    private static void PlainPlacementClass()
    {
        T.Check("a Node3D class named by _class", Node.WrapperFor(new[] { ("_class", "Marker3D") }) is Marker3D);
        T.Check("a light class", Node.WrapperFor(new[] { ("x", "1"), ("_class", "OmniLight3D") }) is OmniLight3D);
        T.Check("no class means Node3D", Node.WrapperFor(new (string, string)[0]).GetType() == typeof(Node3D));
        T.Check("an unknown class means Node3D", Node.WrapperFor(new[] { ("_class", "Nope") }).GetType() == typeof(Node3D));
        T.Check("a class that is not a Node3D means Node3D", Node.WrapperFor(new[] { ("_class", "Timer") }).GetType() == typeof(Node3D));
    }
}
