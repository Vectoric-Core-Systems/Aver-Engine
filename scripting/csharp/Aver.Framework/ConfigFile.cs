// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// ConfigFile: sections of typed key/value pairs in an INI-style text format, for settings and small saves.

using System.Globalization;
using System.Text;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>Sections of key/value pairs saved as text in an INI-style format: <c>[section]</c> lines, then
/// <c>key=value</c> with strings quoted, numbers and <c>true</c>/<c>false</c> bare, and <c>Vector3(x, y, z)</c>.
/// Values are string, long, double, bool or Vec3; ints and floats are stored as long and double.</summary>
public sealed class ConfigFile
{
    private readonly List<(string Name, List<(string Key, object Value)> Keys)> _sections = new();

    /// <summary>Sets a value, adding the section and key if needed. A null value erases the key.</summary>
    public void SetValue(string section, string key, object? value)
    {
        if (value is null) { EraseSectionKey(section, key); return; }
        value = Normalise(value);
        var keys = Section(section, create: true)!;
        int i = keys.FindIndex(k => k.Key == key);
        if (i >= 0) keys[i] = (key, value); else keys.Add((key, value));
    }

    /// <summary>The value, or <paramref name="fallback"/> when the section or key is absent.</summary>
    public object? GetValue(string section, string key, object? fallback = null)
    {
        var keys = Section(section, create: false);
        if (keys == null) return fallback;
        foreach (var (k, v) in keys) if (k == key) return v;
        return fallback;
    }

    /// <summary>The value converted to <typeparamref name="T"/>, or <paramref name="fallback"/> when absent or of another kind.</summary>
    public T GetValue<T>(string section, string key, T fallback)
    {
        object? v = GetValue(section, key);
        if (v is T t) return t;
        try { if (v is IConvertible && typeof(IConvertible).IsAssignableFrom(typeof(T))) return (T)Convert.ChangeType(v, typeof(T), CultureInfo.InvariantCulture); }
        catch (Exception ex) when (ex is InvalidCastException or FormatException or OverflowException) { }
        return fallback;
    }

    public bool HasSection(string section) => Section(section, create: false) != null;
    public bool HasSectionKey(string section, string key) => Section(section, create: false)?.Exists(k => k.Key == key) ?? false;
    public string[] GetSections() => _sections.Select(s => s.Name).ToArray();
    public string[] GetSectionKeys(string section) => Section(section, create: false)?.Select(k => k.Key).ToArray() ?? Array.Empty<string>();
    public void EraseSection(string section) => _sections.RemoveAll(s => s.Name == section);
    public void EraseSectionKey(string section, string key) => Section(section, create: false)?.RemoveAll(k => k.Key == key);
    public void Clear() => _sections.Clear();

    /// <summary>Writes the file; <c>res://</c> and <c>user://</c> paths are resolved. False when it cannot be written.</summary>
    public bool Save(string path)
    {
        try
        {
            string full = Paths.Globalize(path);
            string? dir = Path.GetDirectoryName(full);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllText(full, ToText());
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>Replaces the contents with the file's; false (contents unchanged) when it is missing or unreadable.</summary>
    public bool Load(string path)
    {
        try { return Parse(File.ReadAllText(Paths.Globalize(path))); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>The text Save writes.</summary>
    public string ToText()
    {
        var sb = new StringBuilder();
        foreach (var (name, keys) in _sections)
        {
            if (sb.Length > 0) sb.Append('\n');
            sb.Append('[').Append(name).Append("]\n\n");
            foreach (var (k, v) in keys) sb.Append(k).Append('=').Append(Encode(v)).Append('\n');
        }
        return sb.ToString();
    }

    /// <summary>Replaces the contents with parsed text. False (contents unchanged) on a line it cannot read.</summary>
    public bool Parse(string text)
    {
        var parsed = new ConfigFile();
        string section = "";
        foreach (string raw in text.Split('\n'))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[' && line[^1] == ']') { section = line[1..^1]; parsed.Section(section, create: true); continue; }
            int eq = line.IndexOf('=');
            if (eq <= 0 || !TryDecode(line[(eq + 1)..].Trim(), out object? v)) return false;
            parsed.SetValue(section, line[..eq].Trim(), v);
        }
        _sections.Clear();
        _sections.AddRange(parsed._sections);
        return true;
    }

    private List<(string Key, object Value)>? Section(string name, bool create)
    {
        foreach (var s in _sections) if (s.Name == name) return s.Keys;
        if (!create) return null;
        var keys = new List<(string, object)>();
        _sections.Add((name, keys));
        return keys;
    }

    private static object Normalise(object v) => v switch
    {
        int i => (long)i,
        short s => (long)s,
        float f => (double)f,
        string or long or double or bool or Vec3 => v,
        _ => v.ToString() ?? "",
    };

    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    internal static string Encode(object v) => v switch
    {
        string s => "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", "\\n") + "\"",
        bool b => b ? "true" : "false",
        long l => l.ToString(Inv),
        double d => double.IsInteger(d) && Math.Abs(d) < 1e15 ? d.ToString("0.0", Inv) : d.ToString("R", Inv),
        Vec3 p => string.Create(Inv, $"Vector3({p.X:R}, {p.Y:R}, {p.Z:R})"),
        _ => "\"" + v + "\"",
    };

    internal static bool TryDecode(string s, out object? v)
    {
        v = null;
        if (s.Length >= 2 && s[0] == '"' && s[^1] == '"')
        {
            var sb = new StringBuilder();
            for (int i = 1; i < s.Length - 1; ++i)
            {
                char c = s[i];
                if (c == '\\' && i + 1 < s.Length - 1) { c = s[++i]; sb.Append(c == 'n' ? '\n' : c == 't' ? '\t' : c); }
                else sb.Append(c);
            }
            v = sb.ToString();
            return true;
        }
        if (s == "true" || s == "false") { v = s == "true"; return true; }
        if (s.StartsWith("Vector3(", StringComparison.Ordinal) && s.EndsWith(')'))
        {
            string[] p = s[8..^1].Split(',');
            if (p.Length == 3 && float.TryParse(p[0], NumberStyles.Float, Inv, out float x) &&
                float.TryParse(p[1], NumberStyles.Float, Inv, out float y) && float.TryParse(p[2], NumberStyles.Float, Inv, out float z))
            { v = new Vec3(x, y, z); return true; }
            return false;
        }
        if (long.TryParse(s, NumberStyles.Integer, Inv, out long l)) { v = l; return true; }
        if (double.TryParse(s, NumberStyles.Float, Inv, out double d)) { v = d; return true; }
        return false;
    }
}
