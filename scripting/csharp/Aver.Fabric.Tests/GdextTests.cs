// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G3 F5: the GDExtension host seen from C#. The wire form of a Variant is checked on its own; the rest (load a plugin, ClassDB
// instantiates its class, calls, properties, signals, the lifecycle, Node3D transform sync, unload) needs the native host library
// (Aver.Fabric.GDExt.Abi) and the test plugin (FabricTestExt) from the C++ build, and is skipped with a message when either is missing.

using System.Runtime.InteropServices;
using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class GdextTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("GDExtension wire form: every Variant type round-trips", WireRoundTrip);
        T.Run("GDExtension wire form: malformed bytes are refused", WireMalformed);
        T.Run("GDExtension host: load, ClassDB, calls, properties, signals, lifecycle, transform, unload", Host);
    });

    private static void WireRoundTrip()
    {
        var rich = new Collections.Dictionary();
        rich[1] = new Collections.Array { "a", 2, 3.5 };
        rich["k"] = new Vector3(1, 2, 3);
        var cases = new (string Name, Variant Value)[]
        {
            ("nil", default), ("bool", true), ("int", -123456789012L), ("float", 2.5), ("string", "héllo 世界"),
            ("StringName", new StringName("sn")), ("NodePath", new NodePath("../Kid:pos")),
            ("Vector2", new Vector2(1, -2)), ("Vector2I", new Vector2I(3, -4)),
            ("Rect2", new Rect2(1, 2, 3, 4)), ("Rect2I", new Rect2I(1, 2, 3, 4)),
            ("Vector3", new Vector3(1, 2, 3)), ("Vector3I", new Vector3I(1, 2, 3)),
            ("Transform2D", new Transform2D(new Vector2(1, 2), new Vector2(3, 4), new Vector2(5, 6))),
            ("Vector4", new Vector4(1, 2, 3, 4)), ("Vector4I", new Vector4I(1, 2, 3, 4)),
            ("Plane", new Plane(new Vector3(0, 1, 0), 3)), ("Quaternion", new Quaternion(0.1f, 0.2f, 0.3f, 0.9f)),
            ("Aabb", new Aabb(new Vector3(1, 2, 3), new Vector3(4, 5, 6))),
            ("Basis", new Basis(new Vector3(1, 2, 3), new Vector3(4, 5, 6), new Vector3(7, 8, 9))),
            ("Transform3D", new Transform3D(new Basis(new Vector3(0, 1, 0), 0.5f), new Vector3(7, 8, 9))),
            ("Projection", new Projection(new Vector4(1, 0, 0, 0), new Vector4(0, 2, 0, 0), new Vector4(0, 0, 3, 0), new Vector4(0, 0, 0, 4))),
            ("Color", new Color(0.1f, 0.2f, 0.3f, 0.4f)), ("Rid", new Rid(77)),
            ("Array", new Collections.Array { 1, "two", new Vector2(3, 4), new Collections.Array { 5 } }),
            ("Dictionary", rich),
            ("PackedByteArray", new byte[] { 1, 2, 255 }), ("PackedInt32Array", new[] { 1, -2, 3 }), ("PackedInt64Array", new[] { 1L << 40, -2L }),
            ("PackedFloat32Array", new[] { 1.5f, 2.5f }), ("PackedFloat64Array", new[] { 1.5, 2.5 }),
            ("PackedStringArray", new[] { "a", "", "ü" }), ("PackedVector2Array", new[] { new Vector2(1, 2) }),
            ("PackedVector3Array", new[] { new Vector3(1, 2, 3), new Vector3(4, 5, 6) }), ("PackedColorArray", new[] { new Color(1, 0, 0, 1) }),
            ("PackedVector4Array", Variant.CreateFrom(new[] { new Vector4(1, 2, 3, 4) })),
        };
        foreach ((string name, Variant value) in cases)
        {
            byte[] a = VariantWire.Encode(value);
            Variant back = VariantWire.Decode(a);
            byte[] b = VariantWire.Encode(back);
            T.Check($"{name}: same type and bytes after a round trip", back.VariantType == value.VariantType && a.SequenceEqual(b),
                    $"{value.VariantType} -> {back.VariantType}");
        }
        T.Check("int value", VariantWire.Decode(VariantWire.Encode(-123456789012L)).AsInt64() == -123456789012L);
        T.Check("string value (UTF-8)", VariantWire.Decode(VariantWire.Encode("héllo")).AsString() == "héllo");
        T.Check("Vector3 value", VariantWire.Decode(VariantWire.Encode(new Vector3(1, 2, 3))).AsVector3() == new Vector3(1, 2, 3));
        Basis m = VariantWire.Decode(VariantWire.Encode(new Basis(new Vector3(1, 2, 3), new Vector3(4, 5, 6), new Vector3(7, 8, 9)))).AsBasis();
        T.Check("Basis keeps its rows and columns", m.Row0 == new Vector3(1, 4, 7) && m.X == new Vector3(1, 2, 3), $"{m.Row0} / {m.X}");
        Collections.Dictionary d = VariantWire.Decode(VariantWire.Encode(rich)).AsGodotDictionary();
        T.Check("Dictionary keeps keys of different types", d.Count == 2 && d[1].AsGodotArray().Count == 3 && d["k"].AsVector3() == new Vector3(1, 2, 3));
        Variant[] args = VariantWire.DecodeArguments(VariantWire.EncodeArguments(new Variant[] { 1, "x", default }));
        T.Check("an argument list is an Array", args.Length == 3 && args[0].AsInt32() == 1 && args[1].AsString() == "x" && args[2].IsNil);
        T.Check("an empty argument list decodes to nothing", VariantWire.DecodeArguments(System.Array.Empty<byte>()).Length == 0);
        // Callable and Signal have no wire form
        byte[] c = VariantWire.Encode(Callable.From(() => { }));
        T.Check("a Callable travels as nil", c.Length == 1 && c[0] == 0);
    }

    private static void WireMalformed()
    {
        static bool Throws(byte[] b) { try { VariantWire.Decode(b); return false; } catch (FormatException) { return true; } }
        T.Check("an int cut short", Throws(new byte[] { 2, 1, 2 }));
        T.Check("an unknown type byte", Throws(new byte[] { 200 }));
        T.Check("a string longer than the buffer", Throws(new byte[] { 4, 100, 0, 0, 0, 65 }));
        T.Check("a negative array count", Throws(new byte[] { 28, 0xFF, 0xFF, 0xFF, 0xFF }));
        T.Check("an empty buffer", Throws(System.Array.Empty<byte>()));
        var deep = new List<byte>();
        for (int i = 0; i < 100; i++) { deep.Add(28); deep.AddRange(new byte[] { 1, 0, 0, 0 }); }
        deep.Add(0);
        T.Check("nesting deeper than 64 levels", Throws(deep.ToArray()));
    }

    // ---- the native host ----

    private static string? FindBin()
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string bin = Path.Combine(d.FullName, "build", "bin");
            if (File.Exists(Path.Combine(bin, HostFile())) && File.Exists(Path.Combine(bin, "gdext_test", "FabricTest.gdextension"))) return bin;
        }
        return null;
    }

    private static string HostFile() =>
        OperatingSystem.IsWindows() ? "Aver.Fabric.GDExt.Abi.dll" : OperatingSystem.IsMacOS() ? "libAver.Fabric.GDExt.Abi.dylib" : "libAver.Fabric.GDExt.Abi.so";

    private static void Host()
    {
        string? bin = FindBin();
        if (bin == null) { Console.WriteLine("  SKIP: the native host (build/bin/Aver.Fabric.GDExt.Abi) or the test plugin (build/bin/gdext_test) is not built"); return; }
        // the host library goes beside this assembly, where the default DllImport probing looks
        string beside = Path.Combine(AppContext.BaseDirectory, HostFile());
        try { if (!File.Exists(beside) || File.GetLastWriteTimeUtc(beside) < File.GetLastWriteTimeUtc(Path.Combine(bin, HostFile()))) File.Copy(Path.Combine(bin, HostFile()), beside, true); }
        catch (IOException) { /* a copy from an earlier run is in use */ }
        if (!GDExtensionManager.IsNativeAvailable) { T.Check("the native host loads", false, GDExtensionManager.LastError); return; }
        T.Check("the native host loads and speaks ABI 1", true);
        string dir = Path.Combine(bin, "gdext_test");
        string ext = Path.Combine(dir, "FabricTest.gdextension");

        T.Check("a missing .gdextension is refused", !GDExtensionManager.Load(Path.Combine(dir, "Nope.gdextension")) && GDExtensionManager.LastError.Length > 0, GDExtensionManager.LastError);
        T.Check("a plugin for Godot 4.0 is refused with a message", !GDExtensionManager.Load(Path.Combine(dir, "FabricTestOld.gdextension")) && GDExtensionManager.LastError.Contains("older than 4.1"), GDExtensionManager.LastError);
        T.Check("ClassDB does not know the class before the load", !ClassDB.ClassExists("FabricTestNode") && ClassDB.Instantiate("FabricTestNode").IsNil);

        T.Check("extension_api.json is accepted", GDExtensionManager.SetApiPath(Path.Combine(bin, "extension_api.json")));
        T.Check("Load", GDExtensionManager.Load(ext, dir), GDExtensionManager.LastError);
        T.Check("IsLoaded", GDExtensionManager.IsLoaded(ext) && GDExtensionManager.GetLoadedExtensions().Length == 1);
        T.Check("Load twice is a no-op", GDExtensionManager.Load(ext, dir));

        string[] classes = GDExtensionManager.GetClassList();
        T.Check("the plugin's classes are listed", classes.Contains("FabricTestNode") && classes.Contains("FabricTestRef") && classes.Length == 4, string.Join(",", classes));
        T.Check("ClassDB knows them", ClassDB.ClassExists("FabricTestNode") && ClassDB.GetParentClass("FabricTestNode").ToString() == "Node" &&
                ClassDB.IsParentClass("FabricTestNode", "Node") && ClassDB.IsParentClass("FabricTestNode", "Object") && !ClassDB.IsParentClass("FabricTestNode", "Node3D"));
        T.Check("an abstract class cannot be instantiated", !ClassDB.CanInstantiate("FabricTestAbstract") && ClassDB.Instantiate("FabricTestAbstract").IsNil);
        T.Check("methods, properties, signals and constants are listed", GDExtensionManager.GetMethodList("FabricTestNode").Contains("add") &&
                GDExtensionManager.GetPropertyList("FabricTestNode").SequenceEqual(new[] { "speed" }) &&
                GDExtensionManager.GetSignalList("FabricTestNode").SequenceEqual(new[] { "pinged" }) &&
                GDExtensionManager.GetConstants("FabricTestNode")["MAX_SPEED"] == 42);

        // an instance through ClassDB
        Variant made = ClassDB.Instantiate("FabricTestNode");
        var node = made.AsGodotObject() as GDExtensionNode;
        T.Check("ClassDB.Instantiate makes a GDExtensionNode", node != null && node.NativeHandle != 0 && node.ExtensionClass == "FabricTestNode");
        if (node == null) return;
        T.Check("Call reaches the native method", node.Call("add", 2, 3).AsInt32() == 5);
        T.Check("Call with a String", node.Call("greet", "World").AsString() == "Hello, World!");
        T.Check("HasMethod", node.HasMethod("add") && node.HasMethod("get_class") && !node.HasMethod("nope"));
        T.Check("Get/Set go through the property's getter and setter", node.Get("speed").AsDouble() == 1.0 && Set(node, "speed", 4.5) && node.Get("speed").AsDouble() == 4.5);
        bool threw = false;
        try { node.Call("add", 1); } catch (InvalidOperationException ex) { threw = ex.Message.Contains("too few arguments"); }
        T.Check("a native call error becomes an exception with the host's message", threw);
        T.Check("self_test: the extension called the host's builtin methods, operators and utility functions", node.Call("self_test").AsInt32() == 255);

        // signals
        var got = new List<long>();
        node.Connect("pinged", Callable.From((long n) => got.Add(n)));
        node.Call("ping", 7);
        node.Call("ping", 8);
        T.Check("a native signal reaches a managed handler", got.SequenceEqual(new long[] { 7, 8 }), string.Join(",", got));
        T.Check("HasSignal sees the native signal", node.HasSignal("pinged"));

        // lifecycle in the managed tree: enter pre-order, ready post-order, process, physics process, exit
        var parent = new GDExtensionNode { ExtensionClass = "FabricTestNode", Name = "P" };
        var child = new GDExtensionNode { ExtensionClass = "FabricTestNode", Name = "C" };
        parent.AddChild(child);
        SceneTree.Instance.Root.AddChild(parent);
        T.Check("_enter_tree and _ready ran once for both", parent.Call("get_enter_count").AsInt32() == 1 && child.Call("get_enter_count").AsInt32() == 1 &&
                parent.Call("get_ready_count").AsInt32() == 1 && child.Call("get_ready_count").AsInt32() == 1);
        T.Frame(1.0 / 60.0, 0);
        T.Frame(1.0 / 60.0, 2);
        T.Frame(1.0 / 60.0, 0);
        T.Check("_process runs each frame", parent.Call("get_process_count").AsInt32() == 3 && child.Call("get_process_count").AsInt32() == 3,
                $"{parent.Call("get_process_count")} {child.Call("get_process_count")}");
        T.Check("_physics_process runs each fixed step", parent.Call("get_physics_count").AsInt32() == 2, parent.Call("get_physics_count").ToString());
        int nativeKids = VariantWire.Decode(GdextNative.Result(GdextNative.aver_gdext_call(parent.NativeHandle, "get_child_count", null, 0))).AsInt32();
        T.Check("the managed parent link reached the native tree", nativeKids == 1, nativeKids.ToString());
        T.Check("a native child of a managed node: the managed child's native name", GdextNative.NodeName(child.NativeHandle) == "C");

        // a child the extension creates natively appears in the managed tree
        T.Check("spawn_child returns the new child count", parent.Call("spawn_child", "kid").AsInt32() == 2);
        Node? kid = parent.GetNodeOrNull("kid");
        T.Check("and the managed tree has it", kid is GDExtensionNode && kid.GetParent() == parent && kid.IsInsideTree());

        // exit
        SceneTree.Instance.Root.RemoveChild(parent);
        T.Check("_exit_tree ran", parent.Call("get_exit_count").AsInt32() == 1 && child.Call("get_exit_count").AsInt32() == 1);
        int parentHandle = parent.NativeHandle, childHandle = child.NativeHandle;
        parent.Free();
        T.Check("freeing the node frees the native object and its children", GdextNative.aver_gdext_object_is_valid(parentHandle) == 0 &&
                GdextNative.aver_gdext_object_is_valid(childHandle) == 0);

        // a Node3D from the host's engine classes: the transform is shared both ways
        var n3 = new GDExtensionNode3D { ExtensionClass = "Node3D" };
        T.Check("a host Node3D can be made", n3.EnsureNative() && n3.NativeHandle != 0);
        n3.Call("set_position", new Vector3(1, 2, 3));
        T.Check("what the extension sets reaches the managed transform", n3.Position == new Vector3(1, 2, 3), n3.Position.ToString());
        n3.Position = new Vector3(5, 0, 0);
        T.Check("what a script sets reaches the extension", n3.Call("get_position").AsVector3() == new Vector3(5, 0, 0), n3.Call("get_position").ToString());
        n3.Free();

        // Resource-like objects
        Variant refObj = ClassDB.Instantiate("FabricTestRef");
        var r = refObj.AsGodotObject() as GDExtensionObject;
        T.Check("a RefCounted class makes a GDExtensionObject", r != null && r.NativeHandle != 0);
        if (r != null)
        {
            Set(r, "value", 99);
            T.Check("its property round-trips", r.Get("value").AsInt32() == 99 && r.Call("get_value").AsInt32() == 99);
            r.Free();
        }

        int nodeHandle = node.NativeHandle;
        node.Free();
        T.Check("the native object is gone with the node", GdextNative.aver_gdext_object_is_valid(nodeHandle) == 0);

        // unload
        T.Check("Unload", GDExtensionManager.Unload(ext), GDExtensionManager.LastError);
        T.Check("the classes are gone", !ClassDB.ClassExists("FabricTestNode") && ClassDB.Instantiate("FabricTestNode").IsNil && !GDExtensionManager.IsLoaded(ext));
        T.Check("unloading again is refused", !GDExtensionManager.Unload(ext));

        // the project's own plugins: a content folder holding the .gdextension and its library (what AverAssetC fabric copies there)
        string content = Path.Combine(Path.GetTempPath(), "fabric_gdext_content_" + Guid.NewGuid().ToString("N"));
        string addon = Path.Combine(content, "addons", "fabrictest");
        try
        {
            Directory.CreateDirectory(addon);
            foreach (string f in Directory.EnumerateFiles(dir, "*FabricTestExt*")) File.Copy(f, Path.Combine(addon, Path.GetFileName(f)));
            File.Copy(ext, Path.Combine(addon, "FabricTest.gdextension"));
            T.Check("LoadProjectExtensions finds a plugin under the content folder", GDExtensionManager.LoadProjectExtensions(content) == 1 &&
                    ClassDB.ClassExists("FabricTestNode"), GDExtensionManager.LastError);
            T.Check("and loading the project again adds nothing", GDExtensionManager.LoadProjectExtensions(content) == 0);
            T.Check("a node placed by the converter (extension_class) makes its native object", new GDExtensionNode { ExtensionClass = "FabricTestNode" }.EnsureNative());
            T.Check("a folder that does not exist loads nothing", GDExtensionManager.LoadProjectExtensions(Path.Combine(content, "nope")) == 0);
            GDExtensionManager.Unload(Path.Combine(addon, "FabricTest.gdextension"));
        }
        finally { try { Directory.Delete(content, true); } catch (IOException) { } }
    }

    private static bool Set(GodotObject o, string name, Variant v) { o.Set(name, v); return true; }
}
