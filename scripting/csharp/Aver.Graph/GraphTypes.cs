// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The pin type model beyond the scalars: type strings (`float3`, `float[]`, `struct:Name`), which pins may link, and the
// CLR types. Arrays are a CLR List of the element type; a struct is a GraphStruct, a name plus a typed object[] of fields.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;

namespace Aver.Graph;

/// Which pins may be linked, how a pin type is spelled in a file, and what CLR type carries it.
public static class PinTypeRules
{
    public static bool IsArray(PinType t) => t >= PinType.FloatArray && t <= PinType.EntityArray;

    /// The element of an array type; any other type is returned unchanged.
    public static PinType ElementOf(PinType t) => t switch
    {
        PinType.FloatArray => PinType.Float,
        PinType.IntArray => PinType.Int,
        PinType.BoolArray => PinType.Bool,
        PinType.Float3Array => PinType.Float3,
        PinType.StringArray => PinType.String,
        PinType.EntityArray => PinType.Entity,
        _ => t,
    };

    /// The array of a scalar element type, or false when the type has no array form.
    public static bool TryArrayOf(PinType elem, out PinType array)
    {
        array = elem switch
        {
            PinType.Float => PinType.FloatArray,
            PinType.Int => PinType.IntArray,
            PinType.Bool => PinType.BoolArray,
            PinType.Float3 => PinType.Float3Array,
            PinType.String => PinType.StringArray,
            PinType.Entity => PinType.EntityArray,
            _ => elem,
        };
        return array != elem;
    }

    /// True for the types a struct field or an array element may have.
    public static bool IsValueScalar(PinType t) =>
        t is PinType.Float or PinType.Int or PinType.Bool or PinType.Float3 or PinType.String or PinType.Entity;

    /// Float, int, bool and entity pins, whose value the debugger can widen to a double.
    public static bool IsNumeric(PinType t) => t is PinType.Float or PinType.Int or PinType.Bool or PinType.Entity;

    /// Same type, Int with Entity (one IL type) or IntArray with EntityArray, in either direction.
    public static bool Compatible(PinType a, PinType b) =>
        a == b ||
        (a == PinType.Int && b == PinType.Entity) || (a == PinType.Entity && b == PinType.Int) ||
        (a == PinType.IntArray && b == PinType.EntityArray) || (a == PinType.EntityArray && b == PinType.IntArray);

    /// Compatible, and for two struct pins the same struct.
    public static bool Compatible(PinType a, string? aStruct, PinType b, string? bStruct) =>
        a == PinType.Struct ? b == PinType.Struct && string.Equals(aStruct, bStruct, StringComparison.Ordinal) : Compatible(a, b);

    public static bool Compatible(Pin a, Pin b) => Compatible(a.Type, a.StructName, b.Type, b.StructName);

    /// Reads a type string: a scalar name (any case), `<scalar>[]`, `struct:<Name>` or `exec`.
    public static bool TryParse(string text, out PinType type, out string? structName)
    {
        type = PinType.Float;
        structName = null;
        if (text.StartsWith("struct:", StringComparison.OrdinalIgnoreCase))
        {
            if (text.Length == 7) return false;
            type = PinType.Struct;
            structName = text.Substring(7);
            return true;
        }
        if (text.EndsWith("[]", StringComparison.Ordinal))
        {
            if (!TryParse(text.Substring(0, text.Length - 2), out var elem, out _) || !TryArrayOf(elem, out type)) return false;
            return true;
        }
        if (text.Length == 0 || !char.IsLetter(text[0]) || !Enum.TryParse<PinType>(text, true, out type)) return false;
        return type <= PinType.Entity;
    }

    /// The file spelling of a type, the inverse of TryParse.
    public static string Format(PinType type, string? structName = null) => type switch
    {
        PinType.Struct => "struct:" + structName,
        PinType.Float => "float",
        PinType.Int => "int",
        PinType.Bool => "bool",
        PinType.Exec => "exec",
        PinType.Float3 => "float3",
        PinType.String => "string",
        PinType.Entity => "entity",
        _ => Format(ElementOf(type)) + "[]",
    };

    /// The CLR type carrying a pin type. Exec is typeof(void), which no local may have.
    public static Type ClrType(PinType t) => t switch
    {
        PinType.Float => typeof(float),
        PinType.Int or PinType.Entity => typeof(int),
        PinType.Bool => typeof(bool),
        PinType.Float3 => typeof(Vector3),
        PinType.String => typeof(string),
        PinType.Struct => typeof(GraphStruct),
        _ when IsArray(t) => typeof(List<>).MakeGenericType(ClrType(ElementOf(t))),
        _ => typeof(void),
    };
}

/// A user struct's value: its STRUCT name and one boxed value per field, in declaration order. Treated as immutable
/// (SetStructField copies), so a pin or variable holding one never aliases another.
public sealed class GraphStruct
{
    public GraphStruct(string name, object[] fields) { Name = name; Fields = fields; }
    public string Name { get; }
    public object[] Fields { get; }
}

/// The calls the compiled IL makes for struct values; null-safe so a struct left unset reads as its zero value.
public static class GraphStructOps
{
    private static readonly Dictionary<string, object[]> s_templates = new();

    private static object ZeroOf(string type) => type switch
    {
        "float" => 0f,
        "bool" => false,
        "float3" => Vector3.Zero,
        "string" => "",
        _ => 0,
    };

    /// The zero struct for a field-type signature such as `float,float3,string`.
    public static GraphStruct Default(string name, string signature)
    {
        object[]? template;
        lock (s_templates)
        {
            if (!s_templates.TryGetValue(signature, out template))
                s_templates[signature] = template = signature.Length == 0
                    ? Array.Empty<object>() : signature.Split(',').Select(ZeroOf).ToArray();
        }
        return new GraphStruct(name, (object[])template.Clone());
    }

    public static GraphStruct Make(string name, object[] fields) => new(name, fields);

    /// A copy of `s` with one field replaced.
    public static GraphStruct With(GraphStruct? s, string name, string signature, int index, object value)
    {
        var copy = s == null ? Default(name, signature) : new GraphStruct(s.Name, (object[])s.Fields.Clone());
        if ((uint)index < (uint)copy.Fields.Length) copy.Fields[index] = value;
        return copy;
    }

    private static object? Field(GraphStruct? s, int i) => s != null && (uint)i < (uint)s.Fields.Length ? s.Fields[i] : null;

    public static float GetFloat(GraphStruct? s, int i) => Field(s, i) is float v ? v : 0f;
    public static int GetInt(GraphStruct? s, int i) => Field(s, i) is int v ? v : 0;
    public static bool GetBool(GraphStruct? s, int i) => Field(s, i) is bool v && v;
    public static Vector3 GetVector(GraphStruct? s, int i) => Field(s, i) is Vector3 v ? v : Vector3.Zero;
    public static string GetString(GraphStruct? s, int i) => Field(s, i) as string ?? "";
}

/// The calls the compiled IL makes for array nodes; null-safe, and an out-of-range index reads or writes nothing.
public static class GraphArrayOps
{
    public static int Length<T>(List<T>? a) => a?.Count ?? 0;
    public static T? Get<T>(List<T>? a, int i) => a != null && (uint)i < (uint)a.Count ? a[i] : default;
    public static string GetString(List<string>? a, int i) => a != null && (uint)i < (uint)a.Count ? a[i] ?? "" : "";
    public static bool Contains<T>(List<T>? a, T item) => a != null && a.Contains(item);
    public static int Find<T>(List<T>? a, T item) => a?.IndexOf(item) ?? -1;
    public static void Add<T>(List<T>? a, T item) => a?.Add(item);
    public static void Set<T>(List<T>? a, int i, T item) { if (a != null && (uint)i < (uint)a.Count) a[i] = item; }
    public static void RemoveAt<T>(List<T>? a, int i) { if (a != null && (uint)i < (uint)a.Count) a.RemoveAt(i); }
    public static void Clear<T>(List<T>? a) => a?.Clear();
}
