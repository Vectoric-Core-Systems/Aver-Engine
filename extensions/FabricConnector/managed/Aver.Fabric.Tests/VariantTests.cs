// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// VariantTests: Variant's conversions, equality and truthiness, Godot's text form, the collections, StringName/NodePath and GD.

using Aver.Fabric;
using Aver.Fabric.Collections;

static class VariantTests
{
    private enum Mood { Calm = 1, Angry = 2 }

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("Variant: types and implicit conversions", Types);
        T.Run("Variant: conversions as Godot does them", Conversions);
        T.Run("Variant: equality", Equality);
        T.Run("Variant: truthiness", Truthiness);
        T.Run("Variant: printing", Printing);
        T.Run("Variant: collections and duplicate", CollectionsAndDuplicate);
        T.Run("Variant text: every literal kind", TextLiterals);
        T.Run("Variant text: bad input is refused", TextErrors);
        T.Run("StringName and NodePath", Names);
        T.Run("Array, Dictionary and their typed views", Collections);
        T.Run("GD: print, rich print, random, instance ids", GdFunctions);
        T.Run("Object: get, set, call and meta by name", ReflectionByName);
    });

    private static void Types()
    {
        T.Check("nil", default(Variant).VariantType == Variant.Type.Nil && Variant.Nil.IsNil);
        T.Check("bool", ((Variant)true).VariantType == Variant.Type.Bool);
        T.Check("int is 64-bit", ((Variant)int.MaxValue).VariantType == Variant.Type.Int && ((Variant)(long.MaxValue)).AsInt64() == long.MaxValue);
        T.Check("float is a double", ((Variant)1.5f).VariantType == Variant.Type.Float && ((Variant)1.5).AsDouble() == 1.5);
        T.Check("string and names", ((Variant)"a").VariantType == Variant.Type.String && ((Variant)new StringName("a")).VariantType == Variant.Type.StringName &&
                                   ((Variant)new NodePath("a")).VariantType == Variant.Type.NodePath);
        T.Check("vectors", ((Variant)Vector3.One).VariantType == Variant.Type.Vector3 && ((Variant)Vector2I.One).VariantType == Variant.Type.Vector2I);
        T.Check("math types", ((Variant)Basis.Identity).VariantType == Variant.Type.Basis && ((Variant)Transform3D.Identity).VariantType == Variant.Type.Transform3D &&
                              ((Variant)Quaternion.Identity).VariantType == Variant.Type.Quaternion && ((Variant)Colors.Red).VariantType == Variant.Type.Color &&
                              ((Variant)new Aabb()).VariantType == Variant.Type.Aabb && ((Variant)new Plane()).VariantType == Variant.Type.Plane);
        T.Check("packed arrays", ((Variant)new byte[] { 1 }).VariantType == Variant.Type.PackedByteArray && ((Variant)new[] { 1, 2 }).VariantType == Variant.Type.PackedInt32Array &&
                                 ((Variant)new[] { "a" }).VariantType == Variant.Type.PackedStringArray && ((Variant)new[] { Vector3.One }).VariantType == Variant.Type.PackedVector3Array);
        T.Check("collections", ((Variant)new Aver.Fabric.Collections.Array()).VariantType == Variant.Type.Array && ((Variant)new Aver.Fabric.Collections.Dictionary()).VariantType == Variant.Type.Dictionary);
        T.Check("enum becomes an int", Variant.From(Mood.Angry).VariantType == Variant.Type.Int && Variant.From(Mood.Angry).AsInt32() == 2);
        T.Check("type numbering is Godot's", (int)Variant.Type.Transform3D == 18 && (int)Variant.Type.Color == 20 && (int)Variant.Type.Object == 24 && (int)Variant.Type.Array == 28);
        T.Check("a null object is nil", ((Variant)(GodotObject?)null).IsNil);
    }

    private static void Conversions()
    {
        T.Check("float to int truncates toward zero", ((Variant)3.9).AsInt32() == 3 && ((Variant)(-3.9)).AsInt32() == -3);
        T.Check("int to float", ((Variant)7).AsDouble() == 7.0);
        T.Check("bool to int", ((Variant)true).AsInt32() == 1 && ((Variant)false).AsInt32() == 0);
        T.Check("string to int reads the leading number", ((Variant)"12abc").AsInt32() == 12 && ((Variant)"abc").AsInt32() == 0 && ((Variant)" -7 ").AsInt32() == -7);
        T.Check("string to float", ((Variant)"2.5e1").AsDouble() == 25.0 && ((Variant)"x").AsDouble() == 0.0);
        T.Check("As<T> for the primitives", ((Variant)5).As<float>() == 5f && ((Variant)5.5).As<long>() == 5L && ((Variant)"5").As<int>() == 5);
        T.Check("As<enum>", ((Variant)2).As<Mood>() == Mood.Angry);
        T.Check("As<Nullable>", ((Variant)3).As<int?>() == 3 && default(Variant).As<int?>() is null);
        T.Check("Vector2 and Vector2I convert", ((Variant)new Vector2I(1, 2)).AsVector2() == new Vector2(1, 2) && ((Variant)new Vector2(1.9f, 2.9f)).AsVector2I() == new Vector2I(1, 2));
        T.Check("Vector3 and Vector3I convert", ((Variant)new Vector3I(1, 2, 3)).As<Vector3>() == new Vector3(1, 2, 3));
        T.Check("a wrong type gives the zero value", ((Variant)"x").AsVector3() == Vector3.Zero && ((Variant)3).AsColor() == default);
        T.Check("identity defaults for transforms", ((Variant)3).AsBasis() == Basis.Identity && ((Variant)3).AsTransform3D() == Transform3D.Identity);
        T.Check("String to StringName and NodePath", ((Variant)"a/b").AsNodePath().GetNameCount() == 2 && ((Variant)"x").AsStringName() == "x");
        T.Check("colour from a hex string", ((Variant)"#ff0000").AsColor() == new Color(1, 0, 0));
        T.Check("Quaternion from a Basis", ((Variant)new Basis(Vector3.Up, 1f)).AsQuaternion().IsNormalized());
        T.Check("packed to Array", ((Variant)new[] { 1, 2, 3 }).AsGodotArray().Count == 3);
        T.Check("Array to packed", ((Variant)new Aver.Fabric.Collections.Array { 1, 2 }).AsInt32Array().SequenceEqual(new[] { 1, 2 }));
        var obj = new Marker3D();
        T.Check("an object round trips", ((Variant)obj).AsGodotObject() == obj && ((Variant)obj).As<Node3D>() == obj && ((Variant)obj).As<Timer2>() == null);
        obj.Free();
        T.Check("a freed object reads as null", ((Variant)obj).AsGodotObject() == null);
    }

    // A node type the Variant must refuse for a Marker3D.
    private sealed class Timer2 : Node { }

    private static void Equality()
    {
        T.Check("1 == 1.0", (Variant)1 == (Variant)1.0);
        T.Check("1 != 2", (Variant)1 != (Variant)2);
        T.Check("true == 1", (Variant)true == (Variant)1);
        T.Check("a string is not an int", (Variant)"1" != (Variant)1);
        T.Check("String == StringName", (Variant)"a" == (Variant)new StringName("a"));
        T.Check("equal arrays", (Variant)new Aver.Fabric.Collections.Array { 1, "a" } == (Variant)new Aver.Fabric.Collections.Array { 1.0, "a" });
        T.Check("unequal arrays", (Variant)new Aver.Fabric.Collections.Array { 1 } != (Variant)new Aver.Fabric.Collections.Array { 1, 2 });
        var d1 = new Aver.Fabric.Collections.Dictionary { ["a"] = 1 };
        var d2 = new Aver.Fabric.Collections.Dictionary { ["a"] = 1 };
        T.Check("equal dictionaries", (Variant)d1 == (Variant)d2);
        d2["b"] = 2;
        T.Check("unequal dictionaries", (Variant)d1 != (Variant)d2);
        T.Check("vectors by value", (Variant)new Vector3(1, 2, 3) == (Variant)new Vector3(1, 2, 3) && (Variant)new Vector3(1, 2, 3) != (Variant)new Vector3(1, 2, 4));
        T.Check("different vector types", (Variant)Vector3.One != (Variant)Vector2.One);
        T.Check("nil equals nil but not 0", default(Variant) == default(Variant) && default(Variant) != (Variant)0);
        var gone = new Marker3D(); Variant v = gone; gone.Free();
        T.Check("a freed object equals nil", v == default(Variant));
        T.Check("packed arrays by content", (Variant)new[] { 1, 2 } == (Variant)new[] { 1, 2 } && (Variant)new[] { 1, 2 } != (Variant)new[] { 2, 1 });
        T.Check("equal values hash alike (int and float)", ((Variant)1).GetHashCode() == ((Variant)1.0).GetHashCode());
        var map = new Aver.Fabric.Collections.Dictionary();
        map[1] = "one";
        T.Check("a float key finds an int key", map[1.0].AsString() == "one");
    }

    private static void Truthiness()
    {
        T.Check("false values", !default(Variant).Booleanize() && !((Variant)false).Booleanize() && !((Variant)0).Booleanize() && !((Variant)0.0).Booleanize() &&
                                !((Variant)"").Booleanize() && !((Variant)Vector3.Zero).Booleanize() && !((Variant)new Aver.Fabric.Collections.Array()).Booleanize() &&
                                !((Variant)new Aver.Fabric.Collections.Dictionary()).Booleanize() && !((Variant)new byte[0]).Booleanize());
        T.Check("true values", ((Variant)true).Booleanize() && ((Variant)1).Booleanize() && ((Variant)(-0.5)).Booleanize() && ((Variant)"a").Booleanize() &&
                               ((Variant)new Vector3(0, 0, 1)).Booleanize() && ((Variant)new Aver.Fabric.Collections.Array { 0 }).Booleanize());
        T.Check("the identity transform, basis and quaternion count as zero (Godot's rule)",
            !((Variant)Transform3D.Identity).Booleanize() && !((Variant)Basis.Identity).Booleanize() && !((Variant)Quaternion.Identity).Booleanize());
        T.Check("a rotated transform is true", ((Variant)new Transform3D(new Basis(Vector3.Up, 1f), Vector3.Zero)).Booleanize());
        T.Check("opaque black is the zero colour", !((Variant)new Color(0, 0, 0, 1)).Booleanize() && ((Variant)new Color(0, 0, 0, 0)).Booleanize());
        var n = new Marker3D(); Variant v = n;
        T.Check("a live object is true, a freed one false", v.Booleanize() && ((Func<bool>)(() => { n.Free(); return !v.Booleanize(); }))());
    }

    private static void Printing()
    {
        T.Check("whole floats show .0", ((Variant)1.0).AsString() == "1.0" && ((Variant)(-2.0)).AsString() == "-2.0");
        T.Check("fractions", ((Variant)0.5).AsString() == "0.5");
        T.Check("0.1 + 0.2 prints as 0.3", ((Variant)(0.1 + 0.2)).AsString() == "0.3");
        T.Check("large exponents", ((Variant)1e20).AsString() == "1e+20");
        T.Check("nan and inf", ((Variant)double.NaN).AsString() == "nan" && ((Variant)double.PositiveInfinity).AsString() == "inf");
        T.Check("bool and nil", ((Variant)true).AsString() == "true" && default(Variant).AsString() == "<null>");
        T.Check("vector", ((Variant)new Vector3(1, 2.5f, 3)).AsString() == "(1, 2.5, 3)");
        T.Check("arrays and dictionaries", ((Variant)new Aver.Fabric.Collections.Array { 1, "a" }).AsString() == "[1, a]" &&
                                          ((Variant)new Aver.Fabric.Collections.Dictionary { ["k"] = 2 }).AsString() == "{ k: 2 }");
    }

    private static void CollectionsAndDuplicate()
    {
        var inner = new Aver.Fabric.Collections.Array { 1, 2 };
        var outer = new Aver.Fabric.Collections.Array { inner };
        Variant shallow = ((Variant)outer).Duplicate(false);
        Variant deep = ((Variant)outer).Duplicate(true);
        inner.Add(3);
        T.Check("a shallow copy shares the inner array", shallow.AsGodotArray()[0].AsGodotArray().Count == 3);
        T.Check("a deep copy does not", deep.AsGodotArray()[0].AsGodotArray().Count == 2);
        T.Check("the outer array is a new one", !ReferenceEquals(shallow.AsGodotArray(), outer));
        var bytes = new byte[] { 1, 2 };
        Variant copy = ((Variant)bytes).Duplicate();
        bytes[0] = 9;
        T.Check("packed arrays are copied", copy.AsByteArray()[0] == 1);
    }

    private static void TextLiterals()
    {
        T.Check("null, true, false", VariantText.Parse("null").IsNil && VariantText.Parse("true").AsBool() && !VariantText.Parse("false").AsBool());
        T.Check("integers", VariantText.Parse("-42").VariantType == Variant.Type.Int && VariantText.Parse("-42").AsInt64() == -42 && VariantText.Parse("0xFF").AsInt64() == 255);
        T.Check("floats", VariantText.Parse("1.5").VariantType == Variant.Type.Float && VariantText.Parse("1e3").AsDouble() == 1000.0 && VariantText.Parse("-.5").AsDouble() == -0.5);
        T.Check("inf and nan", double.IsPositiveInfinity(VariantText.Parse("inf").AsDouble()) && double.IsNegativeInfinity(VariantText.Parse("inf_neg").AsDouble()) && double.IsNaN(VariantText.Parse("nan").AsDouble()));
        T.Check("strings with escapes", VariantText.Parse("\"a\\nb\\t\\\"q\\\" \\u00e9\"").AsString() == "a\nb\t\"q\" é");
        T.Check("StringName and NodePath", VariantText.Parse("&\"idle\"").VariantType == Variant.Type.StringName && VariantText.Parse("NodePath(\"A/B:x\")").AsNodePath().GetSubname(0) == "x");
        T.Check("Vector2/3/4 and the I forms", VariantText.Parse("Vector2(1, 2)").AsVector2() == new Vector2(1, 2) && VariantText.Parse("Vector3(1, 2, 3)").AsVector3() == new Vector3(1, 2, 3) &&
                                              VariantText.Parse("Vector4(1, 2, 3, 4)").AsVector4() == new Vector4(1, 2, 3, 4) && VariantText.Parse("Vector3i(1, 2, 3)").VariantType == Variant.Type.Vector3I &&
                                              VariantText.Parse("Vector2i(4, 5)").AsVector2I() == new Vector2I(4, 5));
        T.Check("Color", VariantText.Parse("Color(1, 0.5, 0, 1)").AsColor() == new Color(1, 0.5f, 0, 1) && VariantText.Parse("Color(1, 0, 0)").AsColor().A == 1f);
        T.Check("Quaternion, Plane, Rect2, AABB", VariantText.Parse("Quaternion(0, 0, 0, 1)").AsQuaternion() == Quaternion.Identity && VariantText.Parse("Plane(0, 1, 0, 2)").AsPlane().D == 2f &&
                                                  VariantText.Parse("Rect2(1, 2, 3, 4)").AsRect2() == new Rect2(1, 2, 3, 4) &&
                                                  VariantText.Parse("AABB(1, 2, 3, 4, 5, 6)").AsAabb() == new Aabb(new Vector3(1, 2, 3), new Vector3(4, 5, 6)));
        Basis rotY = VariantText.Parse("Basis(0, 0, 1, 0, 1, 0, -1, 0, 0)").AsBasis();
        T.CheckVec("Basis is read row by row (its X axis is the first column)", new Vector3(0, 0, -1), rotY * new Vector3(1, 0, 0));
        Transform3D xf = VariantText.Parse("Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 4, 5, 6)").AsTransform3D();
        T.Check("Transform3D: basis rows then origin", xf.Basis == Basis.Identity && xf.Origin == new Vector3(4, 5, 6));
        Transform2D x2 = VariantText.Parse("Transform2D(1, 0, 0, 1, 7, 8)").AsTransform2D();
        T.Check("Transform2D: x, y, origin", x2.Origin == new Vector2(7, 8) && x2.X == new Vector2(1, 0));
        T.Check("Projection", VariantText.Parse("Projection(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)").AsProjection() == Projection.Identity);
        var arr = VariantText.Parse("[1, \"two\", Vector2(3, 4), [5]]").AsGodotArray();
        T.Check("arrays nest", arr.Count == 4 && arr[1].AsString() == "two" && arr[3].AsGodotArray()[0].AsInt32() == 5);
        T.Check("typed arrays drop the type", VariantText.Parse("Array[int]([1, 2, 3])").AsGodotArray().Count == 3);
        var dict = VariantText.Parse("{ \"a\": 1, \"b\": [2], \"c\": { \"d\": null } }").AsGodotDictionary();
        T.Check("dictionaries nest and keep their order", dict.Count == 3 && dict.KeysArray()[0].AsString() == "a" && dict["b"].AsGodotArray().Count == 1 && dict["c"].AsGodotDictionary()["d"].IsNil);
        T.Check("packed arrays", VariantText.Parse("PackedInt32Array(1, 2, 3)").AsInt32Array().SequenceEqual(new[] { 1, 2, 3 }) &&
                                 VariantText.Parse("PackedStringArray(\"a\", \"b\")").AsStringArray().SequenceEqual(new[] { "a", "b" }) &&
                                 VariantText.Parse("PackedVector3Array(1, 2, 3, 4, 5, 6)").AsVector3Array().Length == 2 &&
                                 VariantText.Parse("PackedFloat32Array(0.5, 1.5)").AsFloat32Array()[1] == 1.5f &&
                                 VariantText.Parse("PackedColorArray(1, 0, 0, 1)").AsColorArray()[0] == new Color(1, 0, 0, 1));
        T.Check("packed bytes as base64 or numbers", VariantText.Parse("PackedByteArray(\"AQID\")").AsByteArray().SequenceEqual(new byte[] { 1, 2, 3 }) &&
                                                     VariantText.Parse("PackedByteArray(1, 2, 3)").AsByteArray().SequenceEqual(new byte[] { 1, 2, 3 }));
        ResourceLoader.ClearCache();
        Resource? res = VariantText.Parse("Resource(\"res://x/y.tres\")").AsGodotObject() as Resource;
        T.Check("Resource(path) loads a resource that knows its path", res != null && res.ResourcePath == "res://x/y.tres");
        T.Check("and the same path gives the same resource", ReferenceEquals(res, VariantText.Parse("Resource(\"res://x/y.tres\")").AsGodotObject()));
        T.Check("whitespace is free", VariantText.Parse("  Vector2 ( 1 ,\n 2 )  ").AsVector2() == new Vector2(1, 2));
    }

    private static void TextErrors()
    {
        foreach (string bad in new[] { "", "Vector3(1, 2", "Vector3(1, 2)", "Bogus(1)", "\"unterminated", "[1, 2", "{ \"a\" 1 }", "1 2", "Vector3(1, x, 3)" })
        {
            bool threw = false;
            try { VariantText.Parse(bad); } catch (FormatException) { threw = true; }
            T.Check($"'{bad}' is refused", threw);
        }
        T.Check("TryParse reports instead of throwing", !VariantText.TryParse("nope(", out _));
    }

    private static void Names()
    {
        T.Check("StringName compares by text and with strings", new StringName("a") == new StringName("a") && new StringName("a") == "a" && "a" == new StringName("a") && new StringName("a") != "b");
        T.Check("StringName null handling", (StringName?)null == null && new StringName("a") != null);
        T.Check("StringName converts to string", ((string)new StringName("hi")) == "hi" && new StringName("").IsEmpty);
        var p = new NodePath("/root/Main/Player:position:x");
        T.Check("absolute path with a property part", p.IsAbsolute() && p.GetNameCount() == 3 && p.GetName(1) == "Main" && p.GetSubnameCount() == 2 && p.GetSubname(1) == "x");
        T.Check("concatenated names and subnames", p.GetConcatenatedNames() == "root/Main/Player" && p.GetConcatenatedSubnames() == "position:x");
        T.Check("ToString round trips", p.ToString() == "/root/Main/Player:position:x" && new NodePath("../A/B").ToString() == "../A/B");
        T.Check("relative path", !new NodePath("A/B").IsAbsolute() && new NodePath("A/B").GetNameCount() == 2);
        T.Check("empty", new NodePath("").IsEmpty && !new NodePath(".").IsEmpty);
        T.Check("equality by text", new NodePath("A/B") == new NodePath("A/B") && new NodePath("A") != new NodePath("B"));
        T.Check("a string converts to a path", ((NodePath)"x/y").GetName(1) == "y");
    }

    private static void Collections()
    {
        var a = new Aver.Fabric.Collections.Array { 3, 1, 2 };
        a.Sort();
        T.Check("sort", a.Select(v => v.AsInt32()).SequenceEqual(new[] { 1, 2, 3 }));
        a.Reverse();
        T.Check("reverse, pop, pop_front", a.Pop().AsInt32() == 1 && a.PopFront().AsInt32() == 3 && a.Count == 1);
        T.Check("resize fills with nil", a.Resize(3) == Error.Ok && a.Count == 3 && a[2].IsNil && a.Resize(-1) == Error.InvalidParameter);
        var big = new Aver.Fabric.Collections.Array { 0, 1, 2, 3, 4 };
        T.Check("slice with negative indices", big.Slice(1, -1).Select(v => v.AsInt32()).SequenceEqual(new[] { 1, 2, 3 }));

        var typed = new Aver.Fabric.Collections.Array<int> { 5, 6 };
        typed.Add(7);
        T.Check("Array<int> holds ints", typed.Count == 3 && typed[2] == 7 && typed.Contains(6) && typed.IndexOf(6) == 1);
        var untyped = (Aver.Fabric.Collections.Array)typed;
        T.Check("its untyped twin sees the same data", untyped.Count == 3 && untyped[0].AsInt32() == 5);
        var strs = new Aver.Fabric.Collections.Array<string>(new[] { "x", "y" });
        T.Check("Array<string> from a sequence", strs[1] == "y");

        var d = new Aver.Fabric.Collections.Dictionary { ["z"] = 1, ["a"] = 2, ["m"] = 3 };
        T.Check("insertion order is kept", d.KeysArray().Select(k => k.AsString()).SequenceEqual(new[] { "z", "a", "m" }));
        d.Remove("a");
        d["q"] = 4;
        T.Check("order after remove and add", d.KeysArray().Select(k => k.AsString()).SequenceEqual(new[] { "z", "m", "q" }) && d["m"].AsInt32() == 3);
        T.Check("overwrite keeps the position", ((Func<bool>)(() => { d["z"] = 9; return d.KeysArray()[0].AsString() == "z" && d["z"].AsInt32() == 9; }))());
        T.Check("get with default and has", d.Get("nope", 7).AsInt32() == 7 && d.Has("m") && !d.Has("a"));
        var merged = new Aver.Fabric.Collections.Dictionary { ["z"] = 0, ["new"] = 1 };
        merged.Merge(d, false);
        T.Check("merge without overwrite", merged["z"].AsInt32() == 0 && merged.ContainsKey("m"));
        var td = new Aver.Fabric.Collections.Dictionary<string, int>();
        td["one"] = 1; td.Add("two", 2);
        T.Check("Dictionary<string,int>", td.Count == 2 && td["two"] == 2 && td.ContainsKey("one") && td.TryGetValue("one", out int one) && one == 1);
        bool dupThrew = false;
        try { td.Add("one", 5); } catch (ArgumentException) { dupThrew = true; }
        T.Check("adding a duplicate key throws", dupThrew);
        bool missThrew = false;
        try { _ = d["missing"]; } catch (KeyNotFoundException) { missThrew = true; }
        T.Check("a missing key throws on read", missThrew);
    }

    private static void GdFunctions()
    {
        T.Reset();
        var lines = new List<string>();
        GD.PrintSink = lines.Add;
        GD.Print("a", 1, 2.5, true, null, Vector2.One);
        T.Check("Print joins with nothing and prints Godot forms", lines.Count == 1 && lines[0] == "a12.5true<null>(1, 1)", lines.Count > 0 ? lines[0] : "");
        lines.Clear();
        GD.PrintRich("[color=red]hi[/color] [b]there[/b] [0]");
        T.Check("PrintRich strips tags but not brackets that are not tags", lines.Count == 1 && lines[0] == "hi there [0]", lines.Count > 0 ? lines[0] : "");
        lines.Clear();
        GD.PrintS("a", "b"); GD.PrintT("c", "d");
        T.Check("PrintS and PrintT", lines[0] == "a b" && lines[1] == "c\td");
        var errors = new List<string>();
        GD.ErrorSink = errors.Add;
        GD.PushError("bad ", 7);
        T.Check("PushError", errors.Count == 1 && errors[0] == "bad 7");
        T.Check("Str", GD.Str("x", 1, 1.0) == "x11.0");
        GD.Seed(12345);
        float r1 = GD.Randf(); uint i1 = GD.Randi();
        GD.Seed(12345);
        T.Check("the same seed repeats the sequence", GD.Randf() == r1 && GD.Randi() == i1);
        GD.Seed(1);
        bool inRange = true;
        for (int i = 0; i < 200; i++) { double v = GD.RandRange(2.0, 3.0); int w = GD.RandRange(5, 7); if (v < 2 || v >= 3 || w < 5 || w > 7) inRange = false; }
        T.Check("RandRange stays in range (ints include both ends)", inRange);
        var rng = new Aver.Fabric.RandomNumberGenerator { Seed = 99 };
        uint first = rng.Randi();
        rng.Seed = 99;
        T.Check("RandomNumberGenerator instances are seedable", rng.Randi() == first);
        var node = new Marker3D();
        ulong id = node.GetInstanceId();
        T.Check("instance id finds the object", GD.InstanceFromId(id) == node && node.GetInstanceId() == id && GodotObject.IsInstanceValid(node));
        node.Free();
        T.Check("a freed object is gone from the id table and invalid", GD.InstanceFromId(id) == null && !GodotObject.IsInstanceValid(node));
        T.Check("ids are unique", new Marker3D().GetInstanceId() != new Marker3D().GetInstanceId());
    }

    private sealed class Reflected : RefCounted
    {
        [Export] public float MoveSpeed = 3f;
        public int Counter { get; set; }
        private string _secret = "s";
        public Vector3 Pos { get; set; }
        public int Add(int a, int b) => a + b;
        public string Greet(string who, string greeting = "hi") => greeting + " " + who;
        public void SetPos(Vector3 v) => Pos = v;
        public void Boom() => throw new InvalidOperationException("boom");
    }

    private static void ReflectionByName()
    {
        T.Reset();
        var o = new Reflected();
        o.Set("move_speed", 9.5);
        T.Check("set by snake_case name reaches the C# field", o.MoveSpeed == 9.5f && o.Get("move_speed").AsSingle() == 9.5f);
        o.Set("counter", 4.0);
        T.Check("set converts the value to the member type", o.Counter == 4 && o.Get("Counter").AsInt32() == 4);
        o.Set("pos:y", 2.0);
        T.Check("set a component with a:b", o.Pos == new Vector3(0, 2, 0) && o.Get("pos:y").AsSingle() == 2f);
        T.Check("call by name with conversion", o.Call("add", 2, 3.9).AsInt32() == 5);
        T.Check("call fills optional arguments", o.Call("greet", "Ann").AsString() == "hi Ann" && o.Call("greet", "Bo", "yo").AsString() == "yo Bo");
        o.Call("set_pos", new Vector3(1, 2, 3));
        T.Check("call a setter-style method", o.Pos == new Vector3(1, 2, 3));
        T.Check("has_method", o.HasMethod("add") && o.HasMethod("Greet") && !o.HasMethod("nothing"));
        bool missing = false;
        try { o.Call("nothing"); } catch (MissingMethodException) { missing = true; }
        T.Check("calling a missing method throws", missing);
        bool inner = false;
        try { o.Call("boom"); } catch (InvalidOperationException) { inner = true; }
        T.Check("an exception inside the method arrives unwrapped", inner);
        o.Set("not_there", 1);
        T.Check("setting an unknown property only warns", T.Logs.Any(l => l.Message.Contains("not_there")));
        o.SetMeta("tag", "x");
        T.Check("meta", o.HasMeta("tag") && o.GetMeta("tag").AsString() == "x" && o.GetMetaList().Length == 1);
        o.SetMeta("tag", default(Variant));
        T.Check("setting nil removes meta", !o.HasMeta("tag"));
        T.Check("exported names are listed", o.GetExportedPropertyNames().SequenceEqual(new[] { "MoveSpeed" }));
        o.CallDeferred("set_pos", new Vector3(7, 7, 7));
        T.Check("call_deferred waits for the flush", o.Pos == new Vector3(1, 2, 3));
        Aver.Framework.Hosting.ExtensionHost.FlushDeferred();
        T.Check("and runs at it", o.Pos == new Vector3(7, 7, 7));
        o.SetDeferred("counter", 11);
        Aver.Framework.Hosting.ExtensionHost.FlushDeferred();
        T.Check("set_deferred", o.Counter == 11);
        T.Check("class names", o.GetClass() == "RefCounted" && o.IsClass("Reflected") && o.IsClass("RefCounted"));
    }
}
