// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The pin type model beyond the scalars: type strings (`float3`, `float[]`, `struct:Name`), which pins may link, and the
// CLR types. Arrays are a CLR List of the element type; a struct is a GraphStruct, a name plus a typed object[] of fields.

using System;
using System.Collections;
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

    /// True for the types an array element may have.
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

/// The calls the compiled IL makes for struct values; null-safe so a struct left unset reads as its zero value. A field
/// may be an array (a fresh empty list in the zero struct) or another struct (null in the zero struct, read as its zero).
public static class GraphStructOps
{
    private static readonly Dictionary<string, Func<object>[]> s_layouts = new();

    private static Func<object> ZeroMaker(string type)
    {
        if (PinTypeRules.TryParse(type, out var t, out _) && PinTypeRules.IsArray(t))
        {
            Type list = PinTypeRules.ClrType(t);
            return () => Activator.CreateInstance(list)!;
        }
        object zero = type switch
        {
            "float" => 0f,
            "bool" => false,
            "float3" => Vector3.Zero,
            "string" => "",
            "int" or "entity" => 0,
            _ => null!,
        };
        return () => zero;
    }

    /// The zero struct for a field-type signature such as `float,float3[],struct:Inner`.
    public static GraphStruct Default(string name, string signature)
    {
        Func<object>[]? makers;
        lock (s_layouts)
        {
            if (!s_layouts.TryGetValue(signature, out makers))
                s_layouts[signature] = makers = signature.Length == 0
                    ? Array.Empty<Func<object>>() : signature.Split(',').Select(ZeroMaker).ToArray();
        }
        var fields = new object[makers.Length];
        for (int i = 0; i < makers.Length; i++) fields[i] = makers[i]();
        return new GraphStruct(name, fields);
    }

    // A list stored into a struct is copied, so a struct never shares a list with an array variable a node mutates.
    private static object Own(object v) =>
        v is IList && v.GetType().IsGenericType ? Activator.CreateInstance(v.GetType(), v)! : v;

    public static GraphStruct Make(string name, object[] fields)
    {
        for (int i = 0; i < fields.Length; i++) fields[i] = Own(fields[i]);
        return new GraphStruct(name, fields);
    }

    /// A copy of `s` with one field replaced.
    public static GraphStruct With(GraphStruct? s, string name, string signature, int index, object value)
    {
        var copy = s == null ? Default(name, signature) : new GraphStruct(s.Name, (object[])s.Fields.Clone());
        if ((uint)index < (uint)copy.Fields.Length) copy.Fields[index] = Own(value);
        return copy;
    }

    private static object? Field(GraphStruct? s, int i) => s != null && (uint)i < (uint)s.Fields.Length ? s.Fields[i] : null;

    public static float GetFloat(GraphStruct? s, int i) => Field(s, i) is float v ? v : 0f;
    public static int GetInt(GraphStruct? s, int i) => Field(s, i) is int v ? v : 0;
    public static bool GetBool(GraphStruct? s, int i) => Field(s, i) is bool v && v;
    public static Vector3 GetVector(GraphStruct? s, int i) => Field(s, i) is Vector3 v ? v : Vector3.Zero;
    public static string GetString(GraphStruct? s, int i) => Field(s, i) as string ?? "";
    public static List<T> GetArray<T>(GraphStruct? s, int i) => Field(s, i) as List<T> ?? new List<T>();
    public static GraphStruct? GetStruct(GraphStruct? s, int i) => Field(s, i) as GraphStruct;
}

/// Struct literals, the text a struct VAR default or a struct pin's typed-in value is written as: `{field=value|...}`.
/// Fields are named, in any order; one left out keeps its zero. A value is its field type's own literal (float3 `x,y,z`,
/// an array's list, a nested struct's braces). A string value escapes | { } as %7C %7B %7D besides EncodeString's set.
public static class GraphStructLiterals
{
    private const int MaxDepth = 32;

    public static bool TryParse(IReadOnlyList<GraphStructDef> defs, string structName, string text, out GraphStruct value, out string err) =>
        TryParse(defs, structName, text, 0, out value, out err);

    private static bool TryParse(IReadOnlyList<GraphStructDef> defs, string structName, string text, int depth,
                                 out GraphStruct value, out string err)
    {
        value = null!;
        err = "";
        var def = defs.FirstOrDefault(d => d.Name == structName);
        if (def == null) { err = $"struct '{structName}' is not declared"; return false; }
        if (depth > MaxDepth) { err = $"a {structName} literal nests more than {MaxDepth} structs deep"; return false; }
        text = text.Trim();
        if (text.Length < 2 || text[0] != '{' || text[^1] != '}') { err = $"'{text}' is not a {structName} literal {{field=value|...}}"; return false; }

        var result = GraphStructOps.Default(def.Name, def.Signature);
        var seen = new HashSet<string>();
        foreach (string part in SplitTopLevel(text.Substring(1, text.Length - 2)))
        {
            if (part.Length == 0) continue;
            int eq = part.IndexOf('=');
            if (eq <= 0) { err = $"'{part}' in a {structName} literal is not field=value"; return false; }
            string name = part.Substring(0, eq).Trim(), lit = part.Substring(eq + 1);
            int idx = def.Fields.FindIndex(f => f.Name == name);
            if (idx < 0) { err = $"struct '{structName}' has no field '{name}'"; return false; }
            if (!seen.Add(name)) { err = $"a {structName} literal sets field '{name}' twice"; return false; }
            var field = def.Fields[idx];
            object v;
            if (field.Type == PinType.Struct)
            {
                if (!TryParse(defs, field.StructName ?? "", lit, depth + 1, out var nested, out err)) return false;
                v = nested;
            }
            else if (PinTypeRules.IsArray(field.Type) ? !GraphLiterals.TryParseArray(field.Type, lit.Trim(), out v)
                                                       : !GraphLiterals.TryParse(field.Type, field.Type == PinType.String ? lit : lit.Trim(), out v))
            {
                err = $"field '{name}' of {structName}: '{lit}' is not a {PinTypeRules.Format(field.Type)}";
                return false;
            }
            result.Fields[idx] = v;
        }
        value = result;
        return true;
    }

    // Splits on the | that are not inside a nested literal's braces.
    private static List<string> SplitTopLevel(string s)
    {
        var parts = new List<string>();
        int depth = 0, start = 0;
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            else if (s[i] == '|' && depth == 0) { parts.Add(s.Substring(start, i - start)); start = i + 1; }
        }
        parts.Add(s.Substring(start));
        return parts;
    }
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
