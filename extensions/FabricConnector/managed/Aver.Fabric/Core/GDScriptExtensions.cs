// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GodotSharp's StringExtensions plus the GDScript Array and Dictionary methods C#'s collections lack, as extension methods. Integers are
// long (GDScript's int); a C# int converts.

using System.Globalization;
using System.Text;
using Aver.Fabric.Collections;

namespace Aver.Fabric;

/// <summary>The String methods of GDScript (and GodotSharp's StringExtensions) on <see cref="string"/>.</summary>
public static class StringExtensions
{
    public static string Substr(this string s, long from, long len = -1)
    {
        if (from < 0) from = 0;
        if (from >= s.Length) return "";
        long n = len < 0 || from + len > s.Length ? s.Length - from : len;
        return s.Substring((int)from, (int)n);
    }

    public static string Left(this string s, long length) => length <= 0 ? "" : length >= s.Length ? s : s[..(int)length];
    public static string Right(this string s, long length) => length <= 0 ? "" : length >= s.Length ? s : s[^(int)length..];

    public static string StripEdges(this string s, bool left = true, bool right = true) =>
        left && right ? s.Trim() : left ? s.TrimStart() : right ? s.TrimEnd() : s;

    /// <summary>The index of the first occurrence at or after <paramref name="from"/>, or -1.</summary>
    public static long Find(this string s, string what, long from = 0) =>
        from < 0 || from > s.Length ? -1 : s.IndexOf(what, (int)from, StringComparison.Ordinal);

    public static long Rfind(this string s, string what, long from = -1) =>
        from < 0 ? s.LastIndexOf(what, StringComparison.Ordinal) : s.LastIndexOf(what, (int)Math.Min(from + what.Length - 1, s.Length - 1), StringComparison.Ordinal);

    public static long Count(this string s, string what, long from = 0, long to = 0)
    {
        if (what.Length == 0) return 0;
        string area = s.Substr(from, to <= 0 ? -1 : to - from);
        long n = 0;
        for (int i = area.IndexOf(what, StringComparison.Ordinal); i >= 0; i = area.IndexOf(what, i + what.Length, StringComparison.Ordinal)) n++;
        return n;
    }

    /// <summary>Splits at a delimiter; an empty delimiter splits into characters.</summary>
    public static string[] Split(this string s, string delimiter, bool allowEmpty, long maxsplit = 0)
    {
        if (delimiter.Length == 0) return s.Select(c => c.ToString()).ToArray();
        var parts = new List<string>();
        int at = 0;
        while (maxsplit <= 0 || parts.Count < maxsplit)
        {
            int i = s.IndexOf(delimiter, at, StringComparison.Ordinal);
            if (i < 0) break;
            parts.Add(s[at..i]);
            at = i + delimiter.Length;
        }
        parts.Add(s[at..]);
        return allowEmpty ? parts.ToArray() : parts.Where(p => p.Length > 0).ToArray();
    }

    public static long ToInt(this string s) => Variant.From(s).AsInt64();
    public static double ToFloat(this string s) => Variant.From(s).AsDouble();

    public static bool IsValidInt(this string s)
    {
        s = s.Trim();
        return s.Length > 0 && long.TryParse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _);
    }

    public static bool IsValidFloat(this string s)
    {
        s = s.Trim();
        return s.Length > 0 && double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out _);
    }

    public static string Capitalize(this string s)
    {
        var sb = new StringBuilder();
        bool word = true;
        foreach (char c in s)
        {
            if (c == '_' || c == ' ' || c == '-') { if (sb.Length > 0 && sb[^1] != ' ') sb.Append(' '); word = true; }
            else { sb.Append(word ? char.ToUpperInvariant(c) : c); word = false; }
        }
        return sb.ToString().Trim();
    }

    public static string PadZeros(this string s, long digits) => s.PadLeft((int)digits, '0');
    public static string PadDecimals(this string s, long digits)
    {
        int dot = s.IndexOf('.');
        if (dot < 0) return digits > 0 ? s + "." + new string('0', (int)digits) : s;
        return s.PadRight(dot + 1 + (int)digits, '0');
    }

    public static string Repeat(this string s, long count) => count <= 0 ? "" : string.Concat(Enumerable.Repeat(s, (int)count));
    public static string Insert(this string s, long position, string what) => s.Insert((int)Math.Clamp(position, 0, s.Length), what);
    public static string Erase(this string s, long position, long chars = 1) =>
        position < 0 || position >= s.Length ? s : s.Remove((int)position, (int)Math.Min(chars, s.Length - position));

    public static bool IsEmpty(this string s) => s.Length == 0;
    public static bool BeginsWith(this string s, string text) => s.StartsWith(text, StringComparison.Ordinal);
    public static string Join(this string separator, IEnumerable<string> parts) => string.Join(separator, parts);
    public static string Join(this string separator, Collections.Array parts) => string.Join(separator, parts.Select(p => p.AsString()));

    public static string GetFile(this string s) { int i = s.LastIndexOfAny(new[] { '/', '\\' }); return i < 0 ? s : s[(i + 1)..]; }
    public static string GetBaseDir(this string s) { int i = s.LastIndexOfAny(new[] { '/', '\\' }); return i < 0 ? "" : s[..i]; }
    public static string GetExtension(this string s) { int i = s.LastIndexOf('.'); return i < 0 || i < s.LastIndexOfAny(new[] { '/', '\\' }) ? "" : s[(i + 1)..]; }

    public static string ToSnakeCase(this string s)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < s.Length; i++)
        {
            if (char.IsUpper(s[i]) && i > 0 && (char.IsLower(s[i - 1]) || char.IsDigit(s[i - 1]))) sb.Append('_');
            sb.Append(char.ToLowerInvariant(s[i]));
        }
        return sb.ToString();
    }

    public static string ToPascalCase(this string s) =>
        string.Concat(s.Split('_', ' ', '-').Where(p => p.Length > 0).Select(p => char.ToUpperInvariant(p[0]) + p[1..]));

    /// <summary>String.format: "{name}" placeholders from a Dictionary, "{0}" from an Array.</summary>
    public static string Format(this string s, Variant values)
    {
        if (values.VariantType == Variant.Type.Dictionary)
        {
            foreach (var kv in values.AsGodotDictionary()) s = s.Replace("{" + kv.Key.AsString() + "}", kv.Value.AsString(), StringComparison.Ordinal);
        }
        else if (values.VariantType == Variant.Type.Array)
        {
            var a = values.AsGodotArray();
            for (int i = 0; i < a.Count; i++)
            {
                if (a[i].VariantType == Variant.Type.Array && a[i].AsGodotArray().Count == 2)
                    s = s.Replace("{" + a[i].AsGodotArray()[0].AsString() + "}", a[i].AsGodotArray()[1].AsString(), StringComparison.Ordinal);
                else s = s.Replace("{" + i.ToString(CultureInfo.InvariantCulture) + "}", a[i].AsString(), StringComparison.Ordinal);
            }
        }
        return s;
    }
}

/// <summary>GDScript's Array methods for the typed and the untyped Array, and C#'s lists.</summary>
public static class ArrayExtensions
{
    public static T PopBack<T>(this IList<T> a)
    {
        if (a.Count == 0) return default!;
        T v = a[^1];
        a.RemoveAt(a.Count - 1);
        return v;
    }

    public static T PopFront<T>(this IList<T> a)
    {
        if (a.Count == 0) return default!;
        T v = a[0];
        a.RemoveAt(0);
        return v;
    }

    public static void PushFront<T>(this IList<T> a, T v) => a.Insert(0, v);
    public static T Front<T>(this IList<T> a) => a.Count == 0 ? default! : a[0];
    public static T Back<T>(this IList<T> a) => a.Count == 0 ? default! : a[a.Count - 1];

    /// <summary>The index of the first equal element at or after <paramref name="from"/>, or -1.</summary>
    public static long Find<T>(this IList<T> a, T what, long from = 0)
    {
        for (int i = (int)Math.Max(from, 0); i < a.Count; i++)
            if (Equals(a[i], what)) return i;
        return -1;
    }

    public static long Count<T>(this IList<T> a, T what)
    {
        long n = 0;
        for (int i = 0; i < a.Count; i++) if (Equals(a[i], what)) n++;
        return n;
    }

    public static void AppendArray<T>(this IList<T> a, IEnumerable<T> other)
    {
        foreach (T v in other.ToList()) a.Add(v);
    }

    public static void Fill<T>(this IList<T> a, T value)
    {
        for (int i = 0; i < a.Count; i++) a[i] = value;
    }

    public static void Resize<T>(this IList<T> a, long size)
    {
        while (a.Count > size) a.RemoveAt(a.Count - 1);
        while (a.Count < size) a.Add(default!);
    }

    public static T Min<T>(this IList<T> a) where T : IComparable<T> => a.Count == 0 ? default! : a.Aggregate((x, y) => y.CompareTo(x) < 0 ? y : x);
    public static T Max<T>(this IList<T> a) where T : IComparable<T> => a.Count == 0 ? default! : a.Aggregate((x, y) => y.CompareTo(x) > 0 ? y : x);

    /// <summary>sort_custom: <paramref name="less"/>(a, b) is true when a sorts before b. Stable.</summary>
    public static void SortCustom<T>(this IList<T> a, Func<T, T, bool> less)
    {
        var sorted = a.ToList();
        for (int i = 1; i < sorted.Count; i++)
        {
            T key = sorted[i];
            int j = i - 1;
            while (j >= 0 && less(key, sorted[j])) { sorted[j + 1] = sorted[j]; j--; }
            sorted[j + 1] = key;
        }
        for (int i = 0; i < sorted.Count; i++) a[i] = sorted[i];
    }

    public static Collections.Array<TResult> Map<T, TResult>(this IEnumerable<T> a, Func<T, TResult> f) => new(a.Select(f).ToList());
    public static Collections.Array<T> Filter<T>(this IEnumerable<T> a, Func<T, bool> f) => new(a.Where(f).ToList());
    public static TAcc Reduce<T, TAcc>(this IEnumerable<T> a, Func<TAcc, T, TAcc> f, TAcc initial) => a.Aggregate(initial, f);
    public static string Join<T>(this IEnumerable<T> a, string separator) => string.Join(separator, a.Select(x => GD.Str(x)));

    /// <summary>The untyped Array's map/filter/reduce taking Callables (the GDScript form).</summary>
    public static Collections.Array Map(this Collections.Array a, Callable f) => a.Map(v => f.Call(v));
    public static Collections.Array Filter(this Collections.Array a, Callable f) => a.Filter(v => f.Call(v).AsBool());
    public static Variant Reduce(this Collections.Array a, Callable f, Variant initial = default)
    {
        Variant acc = initial;
        foreach (Variant v in a) acc = f.Call(acc, v);
        return acc;
    }
    public static bool Any(this Collections.Array a, Callable f) => a.Any(v => f.Call(v).AsBool());
    public static bool All(this Collections.Array a, Callable f) => a.All(v => f.Call(v).AsBool());
    public static void SortCustom(this Collections.Array a, Callable less) => a.SortCustom((x, y) => less.Call(x, y).AsBool());
    public static Variant Min(this Collections.Array a) => a.Count == 0 ? default : a.Aggregate((x, y) => GDScript.Lt(y, x) ? y : x);
    public static Variant Max(this Collections.Array a) => a.Count == 0 ? default : a.Aggregate((x, y) => GDScript.Gt(y, x) ? y : x);
    public static Collections.Array Slice(this Collections.Array a, long begin, long end = int.MaxValue) => a.Slice((int)begin, (int)Math.Min(end, int.MaxValue));
}

/// <summary>GDScript's Dictionary methods for the typed Dictionary.</summary>
public static class DictionaryExtensions
{
    public static TValue Get<TKey, TValue>(this Collections.Dictionary<TKey, TValue> d, TKey key, TValue @default = default!) where TKey : notnull =>
        d.TryGetValue(key, out TValue? v) ? v : @default;

    public static bool Has<TKey, TValue>(this Collections.Dictionary<TKey, TValue> d, TKey key) where TKey : notnull => d.ContainsKey(key);

    public static void Merge<TKey, TValue>(this Collections.Dictionary<TKey, TValue> d, Collections.Dictionary<TKey, TValue> other, bool overwrite = false) where TKey : notnull
    {
        foreach (var kv in other.ToList()) if (overwrite || !d.ContainsKey(kv.Key)) d[kv.Key] = kv.Value;
    }

    public static Collections.Array<TKey> KeysArray<TKey, TValue>(this Collections.Dictionary<TKey, TValue> d) where TKey : notnull => new(d.Keys.ToList());
    public static Collections.Array<TValue> ValuesArray<TKey, TValue>(this Collections.Dictionary<TKey, TValue> d) where TKey : notnull => new(d.Values.ToList());
}
