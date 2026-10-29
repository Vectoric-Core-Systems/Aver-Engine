// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Variant: Godot's dynamic value (nil, bool, 64-bit int, double, strings, math types, objects, collections) with Godot's
// conversion, equality and truthiness rules.

using System.Globalization;
using System.Text;
using Aver.Fabric.Collections;

namespace Aver.Fabric;

/// <summary>A value of any Godot type. Integers are 64-bit and floats are doubles, as in Godot.</summary>
public readonly struct Variant : IEquatable<Variant>
{
    /// <summary>The type a Variant holds (Godot's Variant.Type, with its numbering).</summary>
    public enum Type
    {
        Nil = 0, Bool = 1, Int = 2, Float = 3, String = 4, Vector2 = 5, Vector2I = 6, Rect2 = 7, Rect2I = 8, Vector3 = 9, Vector3I = 10,
        Transform2D = 11, Vector4 = 12, Vector4I = 13, Plane = 14, Quaternion = 15, Aabb = 16, Basis = 17, Transform3D = 18, Projection = 19,
        Color = 20, StringName = 21, NodePath = 22, Rid = 23, Object = 24, Callable = 25, Signal = 26, Dictionary = 27, Array = 28,
        PackedByteArray = 29, PackedInt32Array = 30, PackedInt64Array = 31, PackedFloat32Array = 32, PackedFloat64Array = 33,
        PackedStringArray = 34, PackedVector2Array = 35, PackedVector3Array = 36, PackedColorArray = 37, PackedVector4Array = 38,
        Max = 39,
    }

    private readonly Type _type;
    private readonly long _i;
    private readonly double _d;
    private readonly object? _o;

    private Variant(Type type, long i = 0, double d = 0, object? o = null) { _type = type; _i = i; _d = d; _o = o; }

    /// <summary>The type of the held value.</summary>
    public Type VariantType => _type;

    public bool IsNil => _type == Type.Nil;
    internal bool IsNumeric => _type is Type.Int or Type.Float or Type.Bool;

    // ---- creation ----

    public static Variant Nil => default;
    public static Variant CreateFrom(bool v) => new(Type.Bool, v ? 1 : 0);
    public static Variant CreateFrom(long v) => new(Type.Int, v);
    public static Variant CreateFrom(int v) => new(Type.Int, v);
    public static Variant CreateFrom(double v) => new(Type.Float, d: v);
    public static Variant CreateFrom(float v) => new(Type.Float, d: v);
    public static Variant CreateFrom(string? v) => v is null ? default : new(Type.String, o: v);
    public static Variant CreateFrom(StringName? v) => v is null ? default : new(Type.StringName, o: v);
    public static Variant CreateFrom(NodePath? v) => v is null ? default : new(Type.NodePath, o: v);
    public static Variant CreateFrom(Vector2 v) => new(Type.Vector2, o: v);
    public static Variant CreateFrom(Vector2I v) => new(Type.Vector2I, o: v);
    public static Variant CreateFrom(Rect2 v) => new(Type.Rect2, o: v);
    public static Variant CreateFrom(Rect2I v) => new(Type.Rect2I, o: v);
    public static Variant CreateFrom(Vector3 v) => new(Type.Vector3, o: v);
    public static Variant CreateFrom(Vector3I v) => new(Type.Vector3I, o: v);
    public static Variant CreateFrom(Transform2D v) => new(Type.Transform2D, o: v);
    public static Variant CreateFrom(Vector4 v) => new(Type.Vector4, o: v);
    public static Variant CreateFrom(Vector4I v) => new(Type.Vector4I, o: v);
    public static Variant CreateFrom(Plane v) => new(Type.Plane, o: v);
    public static Variant CreateFrom(Quaternion v) => new(Type.Quaternion, o: v);
    public static Variant CreateFrom(Aabb v) => new(Type.Aabb, o: v);
    public static Variant CreateFrom(Basis v) => new(Type.Basis, o: v);
    public static Variant CreateFrom(Transform3D v) => new(Type.Transform3D, o: v);
    public static Variant CreateFrom(Projection v) => new(Type.Projection, o: v);
    public static Variant CreateFrom(Color v) => new(Type.Color, o: v);
    public static Variant CreateFrom(Rid v) => new(Type.Rid, i: unchecked((long)v.Id));
    public static Variant CreateFrom(GodotObject? v) => v is null ? default : new(Type.Object, o: v);
    public static Variant CreateFrom(Callable v) => new(Type.Callable, o: v);
    public static Variant CreateFrom(Signal v) => new(Type.Signal, o: v);
    public static Variant CreateFrom(Collections.Dictionary? v) => v is null ? default : new(Type.Dictionary, o: v);
    public static Variant CreateFrom(Collections.Array? v) => v is null ? default : new(Type.Array, o: v);
    public static Variant CreateFrom(byte[]? v) => v is null ? default : new(Type.PackedByteArray, o: v);
    public static Variant CreateFrom(int[]? v) => v is null ? default : new(Type.PackedInt32Array, o: v);
    public static Variant CreateFrom(long[]? v) => v is null ? default : new(Type.PackedInt64Array, o: v);
    public static Variant CreateFrom(float[]? v) => v is null ? default : new(Type.PackedFloat32Array, o: v);
    public static Variant CreateFrom(double[]? v) => v is null ? default : new(Type.PackedFloat64Array, o: v);
    public static Variant CreateFrom(string[]? v) => v is null ? default : new(Type.PackedStringArray, o: v);
    public static Variant CreateFrom(Vector2[]? v) => v is null ? default : new(Type.PackedVector2Array, o: v);
    public static Variant CreateFrom(Vector3[]? v) => v is null ? default : new(Type.PackedVector3Array, o: v);
    public static Variant CreateFrom(Color[]? v) => v is null ? default : new(Type.PackedColorArray, o: v);
    public static Variant CreateFrom(Vector4[]? v) => v is null ? default : new(Type.PackedVector4Array, o: v);

    /// <summary>Wraps any .NET value: primitives, Godot types, enums (as ints), collections, objects and Variants.</summary>
    public static Variant From<T>(T from) => FromObject(from);

    internal static Variant FromObject(object? o) => o switch
    {
        null => default,
        Variant v => v,
        bool b => CreateFrom(b),
        sbyte x => CreateFrom((long)x),
        byte x => CreateFrom((long)x),
        short x => CreateFrom((long)x),
        ushort x => CreateFrom((long)x),
        int x => CreateFrom((long)x),
        uint x => CreateFrom((long)x),
        long x => CreateFrom(x),
        ulong x => CreateFrom(unchecked((long)x)),
        char c => CreateFrom((long)c),
        float x => CreateFrom(x),
        double x => CreateFrom(x),
        decimal x => CreateFrom((double)x),
        string s => CreateFrom(s),
        StringName sn => CreateFrom(sn),
        NodePath np => CreateFrom(np),
        Vector2 x => CreateFrom(x),
        Vector2I x => CreateFrom(x),
        Rect2 x => CreateFrom(x),
        Rect2I x => CreateFrom(x),
        Vector3 x => CreateFrom(x),
        Vector3I x => CreateFrom(x),
        Transform2D x => CreateFrom(x),
        Vector4 x => CreateFrom(x),
        Vector4I x => CreateFrom(x),
        Plane x => CreateFrom(x),
        Quaternion x => CreateFrom(x),
        Aabb x => CreateFrom(x),
        Basis x => CreateFrom(x),
        Transform3D x => CreateFrom(x),
        Projection x => CreateFrom(x),
        Color x => CreateFrom(x),
        Rid x => CreateFrom(x),
        GodotObject x => CreateFrom(x),
        Callable x => CreateFrom(x),
        Signal x => CreateFrom(x),
        Collections.Dictionary x => CreateFrom(x),
        Collections.Array x => CreateFrom(x),
        byte[] x => CreateFrom(x),
        int[] x => CreateFrom(x),
        long[] x => CreateFrom(x),
        float[] x => CreateFrom(x),
        double[] x => CreateFrom(x),
        string[] x => CreateFrom(x),
        Vector2[] x => CreateFrom(x),
        Vector3[] x => CreateFrom(x),
        Color[] x => CreateFrom(x),
        Vector4[] x => CreateFrom(x),
        Enum e => CreateFrom(Convert.ToInt64(e, CultureInfo.InvariantCulture)),
        _ => FromWrapper(o),
    };

    internal static bool TryFrom(object? o, out Variant v)
    {
        try { v = FromObject(o); return true; }
        catch (ArgumentException) { v = default; return false; }
    }

    // Array<T> and Dictionary<K,V> hold their untyped twin; reach it through their explicit conversion.
    private static Variant FromWrapper(object o)
    {
        System.Type t = o.GetType();
        if (t.IsGenericType)
        {
            System.Type def = t.GetGenericTypeDefinition();
            if (def == typeof(Collections.Array<>))
                return CreateFrom((Collections.Array)t.GetMethod("op_Explicit", new[] { t })!.Invoke(null, new[] { o })!);
            if (def == typeof(Collections.Dictionary<,>))
                return CreateFrom((Collections.Dictionary)t.GetMethods().First(m => m.Name == "op_Explicit" && m.ReturnType == typeof(Collections.Dictionary)).Invoke(null, new[] { o })!);
        }
        throw new ArgumentException($"{t.FullName} cannot be held by a Variant.");
    }

    public static implicit operator Variant(bool v) => CreateFrom(v);
    public static implicit operator Variant(sbyte v) => CreateFrom((long)v);
    public static implicit operator Variant(byte v) => CreateFrom((long)v);
    public static implicit operator Variant(short v) => CreateFrom((long)v);
    public static implicit operator Variant(ushort v) => CreateFrom((long)v);
    public static implicit operator Variant(int v) => CreateFrom((long)v);
    public static implicit operator Variant(uint v) => CreateFrom((long)v);
    public static implicit operator Variant(long v) => CreateFrom(v);
    public static implicit operator Variant(ulong v) => CreateFrom(unchecked((long)v));
    public static implicit operator Variant(float v) => CreateFrom(v);
    public static implicit operator Variant(double v) => CreateFrom(v);
    public static implicit operator Variant(string? v) => CreateFrom(v);
    public static implicit operator Variant(StringName? v) => CreateFrom(v);
    public static implicit operator Variant(NodePath? v) => CreateFrom(v);
    public static implicit operator Variant(Vector2 v) => CreateFrom(v);
    public static implicit operator Variant(Vector2I v) => CreateFrom(v);
    public static implicit operator Variant(Rect2 v) => CreateFrom(v);
    public static implicit operator Variant(Rect2I v) => CreateFrom(v);
    public static implicit operator Variant(Vector3 v) => CreateFrom(v);
    public static implicit operator Variant(Vector3I v) => CreateFrom(v);
    public static implicit operator Variant(Transform2D v) => CreateFrom(v);
    public static implicit operator Variant(Vector4 v) => CreateFrom(v);
    public static implicit operator Variant(Vector4I v) => CreateFrom(v);
    public static implicit operator Variant(Plane v) => CreateFrom(v);
    public static implicit operator Variant(Quaternion v) => CreateFrom(v);
    public static implicit operator Variant(Aabb v) => CreateFrom(v);
    public static implicit operator Variant(Basis v) => CreateFrom(v);
    public static implicit operator Variant(Transform3D v) => CreateFrom(v);
    public static implicit operator Variant(Projection v) => CreateFrom(v);
    public static implicit operator Variant(Color v) => CreateFrom(v);
    public static implicit operator Variant(Rid v) => CreateFrom(v);
    public static implicit operator Variant(GodotObject? v) => CreateFrom(v);
    public static implicit operator Variant(Callable v) => CreateFrom(v);
    public static implicit operator Variant(Signal v) => CreateFrom(v);
    public static implicit operator Variant(Collections.Dictionary? v) => CreateFrom(v);
    public static implicit operator Variant(Collections.Array? v) => CreateFrom(v);
    public static implicit operator Variant(byte[]? v) => CreateFrom(v);
    public static implicit operator Variant(int[]? v) => CreateFrom(v);
    public static implicit operator Variant(long[]? v) => CreateFrom(v);
    public static implicit operator Variant(float[]? v) => CreateFrom(v);
    public static implicit operator Variant(double[]? v) => CreateFrom(v);
    public static implicit operator Variant(string[]? v) => CreateFrom(v);
    public static implicit operator Variant(Vector2[]? v) => CreateFrom(v);
    public static implicit operator Variant(Vector3[]? v) => CreateFrom(v);
    public static implicit operator Variant(Color[]? v) => CreateFrom(v);
    public static implicit operator Variant(Vector4[]? v) => CreateFrom(v);

    public static explicit operator bool(Variant v) => v.AsBool();
    public static explicit operator int(Variant v) => v.AsInt32();
    public static explicit operator long(Variant v) => v.AsInt64();
    public static explicit operator float(Variant v) => v.AsSingle();
    public static explicit operator double(Variant v) => v.AsDouble();
    public static explicit operator string(Variant v) => v.AsString();
    public static explicit operator Vector2(Variant v) => v.AsVector2();
    public static explicit operator Vector3(Variant v) => v.AsVector3();
    public static explicit operator Color(Variant v) => v.AsColor();
    public static explicit operator Quaternion(Variant v) => v.AsQuaternion();
    public static explicit operator Transform3D(Variant v) => v.AsTransform3D();
    public static explicit operator GodotObject?(Variant v) => v.AsGodotObject();

    // ---- conversion out ----

    /// <summary>The value as <typeparamref name="T"/>, converting as Godot does (int to float, float to int by truncation, string parsing).</summary>
    public T As<T>() => (T)ToType(typeof(T))!;

    public bool AsBool() => Booleanize();

    public long AsInt64() => _type switch
    {
        Type.Bool or Type.Int or Type.Rid => _i,
        Type.Float => double.IsNaN(_d) ? 0 : (long)Math.Clamp(_d, long.MinValue, long.MaxValue),
        Type.String or Type.StringName => ParseInt(AsString()),
        _ => 0,
    };

    public int AsInt32() => unchecked((int)AsInt64());
    public uint AsUInt32() => unchecked((uint)AsInt64());
    public ulong AsUInt64() => unchecked((ulong)AsInt64());
    public short AsInt16() => unchecked((short)AsInt64());
    public ushort AsUInt16() => unchecked((ushort)AsInt64());
    public sbyte AsSByte() => unchecked((sbyte)AsInt64());
    public byte AsByte() => unchecked((byte)AsInt64());

    public double AsDouble() => _type switch
    {
        Type.Bool or Type.Int => _i,
        Type.Float => _d,
        Type.String or Type.StringName => double.TryParse(LeadingNumber(AsString()), NumberStyles.Float, CultureInfo.InvariantCulture, out double d) ? d : 0.0,
        _ => 0.0,
    };

    public float AsSingle() => (float)AsDouble();

    private static long ParseInt(string s)
    {
        string t = LeadingNumber(s);
        if (long.TryParse(t, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long l)) return l;
        return double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out double d) ? (long)d : 0;
    }

    // Godot's to_int / to_float read the leading number and ignore the rest ("12abc" is 12).
    private static string LeadingNumber(string s)
    {
        s = s.Trim();
        int i = 0;
        if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
        int digits = 0;
        while (i < s.Length && char.IsAsciiDigit(s[i])) { i++; digits++; }
        if (i < s.Length && s[i] == '.') { i++; while (i < s.Length && char.IsAsciiDigit(s[i])) { i++; digits++; } }
        if (digits > 0 && i < s.Length && (s[i] == 'e' || s[i] == 'E'))
        {
            int save = i++;
            if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
            int ed = 0;
            while (i < s.Length && char.IsAsciiDigit(s[i])) { i++; ed++; }
            if (ed == 0) i = save;
        }
        return digits == 0 ? "0" : s[..i];
    }

    /// <summary>The value as text, the way Godot's <c>str()</c> prints it.</summary>
    public string AsString() => _type switch
    {
        Type.Nil => "<null>",
        Type.Bool => _i != 0 ? "true" : "false",
        Type.Int => _i.ToString(CultureInfo.InvariantCulture),
        Type.Float => FormatFloat(_d),
        Type.String => (string)_o!,
        Type.Rid => $"RID({(ulong)_i})",
        Type.Object => _o is GodotObject go && go.IsAlive ? go.ToString() ?? "" : "<Freed Object>",
        Type.PackedByteArray => "[" + string.Join(", ", ((byte[])_o!).Select(b => b.ToString(CultureInfo.InvariantCulture))) + "]",
        Type.PackedInt32Array => "[" + string.Join(", ", (int[])_o!) + "]",
        Type.PackedInt64Array => "[" + string.Join(", ", (long[])_o!) + "]",
        Type.PackedFloat32Array => "[" + string.Join(", ", ((float[])_o!).Select(f => FormatFloat(f))) + "]",
        Type.PackedFloat64Array => "[" + string.Join(", ", ((double[])_o!).Select(FormatFloat)) + "]",
        Type.PackedStringArray => "[" + string.Join(", ", (string[])_o!) + "]",
        Type.PackedVector2Array => "[" + string.Join(", ", (Vector2[])_o!) + "]",
        Type.PackedVector3Array => "[" + string.Join(", ", (Vector3[])_o!) + "]",
        Type.PackedColorArray => "[" + string.Join(", ", (Color[])_o!) + "]",
        _ => _o?.ToString() ?? "",
    };

    /// <summary>Godot prints a float with up to 15 significant digits and always shows a decimal point for whole numbers.</summary>
    internal static string FormatFloat(double d)
    {
        if (double.IsNaN(d)) return "nan";
        if (double.IsPositiveInfinity(d)) return "inf";
        if (double.IsNegativeInfinity(d)) return "-inf";
        string s = d.ToString("G15", CultureInfo.InvariantCulture);
        if (s.Contains('E'))
        {
            // 1E+20 becomes 1e+20, the form Godot prints.
            s = s.Replace("E+", "e+").Replace("E-", "e-");
            return s;
        }
        return s.Contains('.') ? s : s + ".0";
    }

    public Vector2 AsVector2() => _type switch { Type.Vector2 => (Vector2)_o!, Type.Vector2I => (Vector2)(Vector2I)_o!, _ => default };
    public Vector2I AsVector2I() => _type switch { Type.Vector2I => (Vector2I)_o!, Type.Vector2 => (Vector2I)(Vector2)_o!, _ => default };
    public Rect2 AsRect2() => _type switch { Type.Rect2 => (Rect2)_o!, Type.Rect2I => (Rect2)(Rect2I)_o!, _ => default };
    public Rect2I AsRect2I() => _type switch { Type.Rect2I => (Rect2I)_o!, Type.Rect2 => (Rect2I)(Rect2)_o!, _ => default };
    public Vector3 AsVector3() => _type switch { Type.Vector3 => (Vector3)_o!, Type.Vector3I => (Vector3)(Vector3I)_o!, _ => default };
    public Vector3I AsVector3I() => _type switch { Type.Vector3I => (Vector3I)_o!, Type.Vector3 => (Vector3I)(Vector3)_o!, _ => default };
    public Transform2D AsTransform2D() => _type == Type.Transform2D ? (Transform2D)_o! : Transform2D.Identity;
    public Vector4 AsVector4() => _type switch { Type.Vector4 => (Vector4)_o!, Type.Vector4I => (Vector4)(Vector4I)_o!, _ => default };
    public Vector4I AsVector4I() => _type switch { Type.Vector4I => (Vector4I)_o!, Type.Vector4 => (Vector4I)(Vector4)_o!, _ => default };
    public Plane AsPlane() => _type == Type.Plane ? (Plane)_o! : default;
    public Quaternion AsQuaternion() => _type switch { Type.Quaternion => (Quaternion)_o!, Type.Basis => new Quaternion((Basis)_o!), _ => Quaternion.Identity };
    public Aabb AsAabb() => _type == Type.Aabb ? (Aabb)_o! : default;
    public Basis AsBasis() => _type switch { Type.Basis => (Basis)_o!, Type.Quaternion => new Basis((Quaternion)_o!), Type.Transform3D => ((Transform3D)_o!).Basis, _ => Basis.Identity };
    public Transform3D AsTransform3D() => _type switch
    {
        Type.Transform3D => (Transform3D)_o!,
        Type.Basis => new Transform3D((Basis)_o!, Vector3.Zero),
        Type.Quaternion => new Transform3D(new Basis((Quaternion)_o!), Vector3.Zero),
        _ => Transform3D.Identity,
    };
    public Projection AsProjection() => _type == Type.Projection ? (Projection)_o! : Projection.Identity;
    public Color AsColor() => _type switch
    {
        Type.Color => (Color)_o!,
        Type.String or Type.StringName => new Color(AsString(), default),
        _ => default,
    };
    public StringName AsStringName() => _type switch { Type.StringName => (StringName)_o!, Type.String => new StringName((string)_o!), _ => StringName.Empty };
    public NodePath AsNodePath() => _type switch { Type.NodePath => (NodePath)_o!, Type.String => new NodePath((string)_o!), Type.StringName => new NodePath(_o!.ToString()!), _ => new NodePath() };
    public Rid AsRid() => _type == Type.Rid ? new Rid(unchecked((ulong)_i)) : default;
    public Callable AsCallable() => _type == Type.Callable ? (Callable)_o! : default;
    public Signal AsSignal() => _type == Type.Signal ? (Signal)_o! : default;
    public Collections.Dictionary AsGodotDictionary() => _type == Type.Dictionary ? (Collections.Dictionary)_o! : new Collections.Dictionary();
    public Collections.Array AsGodotArray() => _type switch
    {
        Type.Array => (Collections.Array)_o!,
        Type.PackedByteArray => new Collections.Array(((byte[])_o!).Select(x => (Variant)(long)x)),
        Type.PackedInt32Array => new Collections.Array(((int[])_o!).Select(x => (Variant)(long)x)),
        Type.PackedInt64Array => new Collections.Array(((long[])_o!).Select(x => (Variant)x)),
        Type.PackedFloat32Array => new Collections.Array(((float[])_o!).Select(x => (Variant)x)),
        Type.PackedFloat64Array => new Collections.Array(((double[])_o!).Select(x => (Variant)x)),
        Type.PackedStringArray => new Collections.Array(((string[])_o!).Select(x => (Variant)x)),
        Type.PackedVector2Array => new Collections.Array(((Vector2[])_o!).Select(x => (Variant)x)),
        Type.PackedVector3Array => new Collections.Array(((Vector3[])_o!).Select(x => (Variant)x)),
        Type.PackedColorArray => new Collections.Array(((Color[])_o!).Select(x => (Variant)x)),
        _ => new Collections.Array(),
    };
    public GodotObject? AsGodotObject() => _type == Type.Object && _o is GodotObject go && go.IsAlive ? go : null;

    public byte[] AsByteArray() => _type == Type.PackedByteArray ? (byte[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsByte()).ToArray() : System.Array.Empty<byte>();
    public int[] AsInt32Array() => _type == Type.PackedInt32Array ? (int[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsInt32()).ToArray() : System.Array.Empty<int>();
    public long[] AsInt64Array() => _type == Type.PackedInt64Array ? (long[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsInt64()).ToArray() : System.Array.Empty<long>();
    public float[] AsFloat32Array() => _type == Type.PackedFloat32Array ? (float[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsSingle()).ToArray() : System.Array.Empty<float>();
    public double[] AsFloat64Array() => _type == Type.PackedFloat64Array ? (double[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsDouble()).ToArray() : System.Array.Empty<double>();
    public string[] AsStringArray() => _type == Type.PackedStringArray ? (string[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsString()).ToArray() : System.Array.Empty<string>();
    public Vector2[] AsVector2Array() => _type == Type.PackedVector2Array ? (Vector2[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsVector2()).ToArray() : System.Array.Empty<Vector2>();
    public Vector3[] AsVector3Array() => _type == Type.PackedVector3Array ? (Vector3[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsVector3()).ToArray() : System.Array.Empty<Vector3>();
    public Vector4[] AsVector4Array() => _type == Type.PackedVector4Array ? (Vector4[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsVector4()).ToArray() : System.Array.Empty<Vector4>();
    public Color[] AsColorArray() => _type == Type.PackedColorArray ? (Color[])_o! : _type == Type.Array ? ((Collections.Array)_o!).Select(v => v.AsColor()).ToArray() : System.Array.Empty<Color>();

    // The value as a .NET type, for As<T> and for calls through reflection.
    internal object? ToType(System.Type t)
    {
        if (t == typeof(Variant)) return this;
        System.Type? nullable = Nullable.GetUnderlyingType(t);
        if (nullable != null) return IsNil ? null : ToType(nullable);
        if (t == typeof(bool)) return AsBool();
        if (t == typeof(int)) return AsInt32();
        if (t == typeof(long)) return AsInt64();
        if (t == typeof(float)) return AsSingle();
        if (t == typeof(double)) return AsDouble();
        if (t == typeof(string)) return IsNil ? null : AsString();
        if (t == typeof(uint)) return AsUInt32();
        if (t == typeof(ulong)) return AsUInt64();
        if (t == typeof(short)) return AsInt16();
        if (t == typeof(ushort)) return AsUInt16();
        if (t == typeof(sbyte)) return AsSByte();
        if (t == typeof(byte)) return AsByte();
        if (t == typeof(char)) return (char)AsInt64();
        if (t == typeof(decimal)) return (decimal)AsDouble();
        if (t.IsEnum) return Enum.ToObject(t, AsInt64());
        if (t == typeof(Vector2)) return AsVector2();
        if (t == typeof(Vector2I)) return AsVector2I();
        if (t == typeof(Rect2)) return AsRect2();
        if (t == typeof(Rect2I)) return AsRect2I();
        if (t == typeof(Vector3)) return AsVector3();
        if (t == typeof(Vector3I)) return AsVector3I();
        if (t == typeof(Transform2D)) return AsTransform2D();
        if (t == typeof(Vector4)) return AsVector4();
        if (t == typeof(Vector4I)) return AsVector4I();
        if (t == typeof(Plane)) return AsPlane();
        if (t == typeof(Quaternion)) return AsQuaternion();
        if (t == typeof(Aabb)) return AsAabb();
        if (t == typeof(Basis)) return AsBasis();
        if (t == typeof(Transform3D)) return AsTransform3D();
        if (t == typeof(Projection)) return AsProjection();
        if (t == typeof(Color)) return AsColor();
        if (t == typeof(StringName)) return IsNil ? null : AsStringName();
        if (t == typeof(NodePath)) return IsNil ? null : AsNodePath();
        if (t == typeof(Rid)) return AsRid();
        if (t == typeof(Callable)) return AsCallable();
        if (t == typeof(Signal)) return AsSignal();
        if (t == typeof(Collections.Dictionary)) return _type == Type.Dictionary ? _o : null;
        if (t == typeof(Collections.Array)) return _type == Type.Nil ? null : AsGodotArray();
        if (t == typeof(byte[])) return AsByteArray();
        if (t == typeof(int[])) return AsInt32Array();
        if (t == typeof(long[])) return AsInt64Array();
        if (t == typeof(float[])) return AsFloat32Array();
        if (t == typeof(double[])) return AsFloat64Array();
        if (t == typeof(string[])) return AsStringArray();
        if (t == typeof(Vector2[])) return AsVector2Array();
        if (t == typeof(Vector3[])) return AsVector3Array();
        if (t == typeof(Color[])) return AsColorArray();
        if (t.IsGenericType)
        {
            System.Type def = t.GetGenericTypeDefinition();
            if (def == typeof(Collections.Array<>)) return Activator.CreateInstance(t, AsGodotArray());
            if (def == typeof(Collections.Dictionary<,>)) return Activator.CreateInstance(t, AsGodotDictionary());
        }
        if (typeof(GodotObject).IsAssignableFrom(t) || t == typeof(object))
        {
            GodotObject? go = AsGodotObject();
            if (t == typeof(object)) return go ?? (IsNil ? null : _o ?? (object)_i);
            if (go != null && t.IsInstanceOfType(go)) return go;
            if (_type == Type.Dictionary && AsGodotDictionary() is { } dict && ClassDB.FromDictionary(dict, t) is { } built) return built;
            // A scene's Resource("path") is untyped; the property it is assigned to says what to load.
            if (go is Resource { ResourcePath.Length: > 0 } stub && go.GetType() == typeof(Resource) && typeof(Resource).IsAssignableFrom(t))
                return ResourceLoader.Load(stub.ResourcePath, t);
            return null;
        }
        return t.IsValueType ? Activator.CreateInstance(t) : null;
    }

    // ---- Godot's rules ----

    /// <summary>Godot's truthiness: nil, false, 0, 0.0, "", a zero vector, an identity transform/quaternion/basis, a null or freed object and an empty collection are false.</summary>
    public bool Booleanize() => _type switch
    {
        Type.Nil => false,
        Type.Bool or Type.Int or Type.Rid => _i != 0,
        Type.Float => _d != 0.0,
        Type.String => ((string)_o!).Length != 0,
        Type.StringName => !((StringName)_o!).IsEmpty,
        Type.NodePath => !((NodePath)_o!).IsEmpty,
        Type.Vector2 => (Vector2)_o! != Vector2.Zero,
        Type.Vector2I => (Vector2I)_o! != Vector2I.Zero,
        Type.Vector3 => (Vector3)_o! != Vector3.Zero,
        Type.Vector3I => (Vector3I)_o! != Vector3I.Zero,
        Type.Vector4 => (Vector4)_o! != Vector4.Zero,
        Type.Vector4I => (Vector4I)_o! != Vector4I.Zero,
        Type.Rect2 => (Rect2)_o! != default,
        Type.Rect2I => (Rect2I)_o! != default,
        Type.Aabb => (Aabb)_o! != default,
        Type.Plane => (Plane)_o! != default,
        Type.Color => (Color)_o! != new Color(0, 0, 0, 1),
        Type.Quaternion => (Quaternion)_o! != Quaternion.Identity,
        Type.Basis => (Basis)_o! != Basis.Identity,
        Type.Transform2D => (Transform2D)_o! != Transform2D.Identity,
        Type.Transform3D => (Transform3D)_o! != Transform3D.Identity,
        Type.Projection => (Projection)_o! != Projection.Identity,
        Type.Object => _o is GodotObject go && go.IsAlive,
        Type.Callable => !((Callable)_o!).IsNull,
        Type.Signal => !((Signal)_o!).IsNull,
        Type.Dictionary => ((Collections.Dictionary)_o!).Count != 0,
        Type.Array => ((Collections.Array)_o!).Count != 0,
        Type.PackedByteArray => ((byte[])_o!).Length != 0,
        Type.PackedInt32Array => ((int[])_o!).Length != 0,
        Type.PackedInt64Array => ((long[])_o!).Length != 0,
        Type.PackedFloat32Array => ((float[])_o!).Length != 0,
        Type.PackedFloat64Array => ((double[])_o!).Length != 0,
        Type.PackedStringArray => ((string[])_o!).Length != 0,
        Type.PackedVector2Array => ((Vector2[])_o!).Length != 0,
        Type.PackedVector3Array => ((Vector3[])_o!).Length != 0,
        Type.PackedColorArray => ((Color[])_o!).Length != 0,
        Type.PackedVector4Array => ((Vector4[])_o!).Length != 0,
        _ => false,
    };

    /// <summary>A copy that does not share collections with this one; <paramref name="deep"/> also copies the collections inside.</summary>
    public Variant Duplicate(bool deep = false) => _type switch
    {
        Type.Array => CreateFrom(((Collections.Array)_o!).Duplicate(deep)),
        Type.Dictionary => CreateFrom(((Collections.Dictionary)_o!).Duplicate(deep)),
        Type.PackedByteArray => CreateFrom((byte[])((byte[])_o!).Clone()),
        Type.PackedInt32Array => CreateFrom((int[])((int[])_o!).Clone()),
        Type.PackedInt64Array => CreateFrom((long[])((long[])_o!).Clone()),
        Type.PackedFloat32Array => CreateFrom((float[])((float[])_o!).Clone()),
        Type.PackedFloat64Array => CreateFrom((double[])((double[])_o!).Clone()),
        Type.PackedStringArray => CreateFrom((string[])((string[])_o!).Clone()),
        Type.PackedVector2Array => CreateFrom((Vector2[])((Vector2[])_o!).Clone()),
        Type.PackedVector3Array => CreateFrom((Vector3[])((Vector3[])_o!).Clone()),
        Type.PackedColorArray => CreateFrom((Color[])((Color[])_o!).Clone()),
        _ => this,
    };

    /// <summary>Godot's <c>==</c>: ints and floats compare as numbers, a String equals a StringName, nil equals a null or freed object, collections compare by content.</summary>
    public bool Equals(Variant other)
    {
        if (_type == other._type) return SameTypeEquals(other);
        bool aNum = _type is Type.Int or Type.Float or Type.Bool, bNum = other._type is Type.Int or Type.Float or Type.Bool;
        if (aNum && bNum) return AsDouble() == other.AsDouble();
        if ((_type, other._type) is (Type.String, Type.StringName) or (Type.StringName, Type.String)) return AsString() == other.AsString();
        if ((_type, other._type) is (Type.String, Type.NodePath) or (Type.NodePath, Type.String)) return AsString() == other.AsString();
        if (_type == Type.Nil) return other._type == Type.Object && !other.Booleanize();
        if (other._type == Type.Nil) return _type == Type.Object && !Booleanize();
        if ((_type, other._type) is (Type.Vector2, Type.Vector2I) or (Type.Vector2I, Type.Vector2)) return AsVector2() == other.AsVector2();
        if ((_type, other._type) is (Type.Vector3, Type.Vector3I) or (Type.Vector3I, Type.Vector3)) return AsVector3() == other.AsVector3();
        if ((_type, other._type) is (Type.Vector4, Type.Vector4I) or (Type.Vector4I, Type.Vector4)) return AsVector4() == other.AsVector4();
        return false;
    }

    private bool SameTypeEquals(Variant o)
    {
        switch (_type)
        {
            case Type.Nil: return true;
            case Type.Bool: case Type.Int: case Type.Rid: return _i == o._i;
            case Type.Float: return _d == o._d;
            case Type.String: return (string)_o! == (string)o._o!;
            case Type.Object: return ReferenceEquals(_o, o._o) || (!Booleanize() && !o.Booleanize());
            case Type.Array:
            {
                var a = (Collections.Array)_o!; var b = (Collections.Array)o._o!;
                if (ReferenceEquals(a, b)) return true;
                if (a.Count != b.Count) return false;
                for (int i = 0; i < a.Count; i++) if (a[i] != b[i]) return false;
                return true;
            }
            case Type.Dictionary:
            {
                var a = (Collections.Dictionary)_o!; var b = (Collections.Dictionary)o._o!;
                if (ReferenceEquals(a, b)) return true;
                if (a.Count != b.Count) return false;
                foreach (var kv in a) if (!b.TryGetValue(kv.Key, out Variant bv) || kv.Value != bv) return false;
                return true;
            }
            case Type.PackedByteArray: return ((byte[])_o!).AsSpan().SequenceEqual((byte[])o._o!);
            case Type.PackedInt32Array: return ((int[])_o!).AsSpan().SequenceEqual((int[])o._o!);
            case Type.PackedInt64Array: return ((long[])_o!).AsSpan().SequenceEqual((long[])o._o!);
            case Type.PackedFloat32Array: return ((float[])_o!).AsSpan().SequenceEqual((float[])o._o!);
            case Type.PackedFloat64Array: return ((double[])_o!).AsSpan().SequenceEqual((double[])o._o!);
            case Type.PackedStringArray: return ((string[])_o!).AsSpan().SequenceEqual((string[])o._o!);
            case Type.PackedVector2Array: return ((Vector2[])_o!).AsSpan().SequenceEqual((Vector2[])o._o!);
            case Type.PackedVector3Array: return ((Vector3[])_o!).AsSpan().SequenceEqual((Vector3[])o._o!);
            case Type.PackedColorArray: return ((Color[])_o!).AsSpan().SequenceEqual((Color[])o._o!);
            default: return Equals(_o, o._o);
        }
    }

    public static bool operator ==(Variant l, Variant r) => l.Equals(r);
    public static bool operator !=(Variant l, Variant r) => !l.Equals(r);
    public override bool Equals(object? obj) => obj is Variant v && Equals(v);

    public override int GetHashCode() => _type switch
    {
        Type.Nil => 0,
        Type.Bool or Type.Int => ((double)_i).GetHashCode(),   // equal to a float of the same value, as Equals says
        Type.Float => _d.GetHashCode(),
        Type.String or Type.StringName => AsString().GetHashCode(),
        Type.Object => _o is GodotObject go && go.IsAlive ? System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(go) : 0,
        Type.Array or Type.Dictionary => ((Type)_type).GetHashCode(),
        _ => _o?.GetHashCode() ?? 0,
    };

    public override string ToString() => AsString();
}
