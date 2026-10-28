// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// StringName and NodePath: Godot's two string-like key types.

using System.Text;

namespace Aver.Fabric;

/// <summary>An interned-style name (Godot's StringName). A class, as in GodotSharp, so it can be null; it compares by text.</summary>
public sealed class StringName : IEquatable<StringName>, IComparable<StringName>
{
    private readonly string _s;

    public StringName() => _s = "";
    public StringName(string name) => _s = name ?? "";

    public static StringName Empty { get; } = new("");

    public bool IsEmpty => _s.Length == 0;

    public static implicit operator StringName(string from) => new(from);
    public static implicit operator string(StringName from) => from?._s ?? "";

    public static bool operator ==(StringName? l, StringName? r)
    {
        if (ReferenceEquals(l, r)) return true;
        if (l is null || r is null) return false;
        return l._s == r._s;
    }
    public static bool operator !=(StringName? l, StringName? r) => !(l == r);

    public bool Equals(StringName? other) => other is not null && other._s == _s;
    public override bool Equals(object? obj) => obj switch { StringName n => Equals(n), string s => s == _s, _ => false };
    public override int GetHashCode() => _s.GetHashCode();
    public int CompareTo(StringName? other) => string.CompareOrdinal(_s, other?._s);
    public override string ToString() => _s;
}

/// <summary>A path of node names, optionally ending in property names after colons (Godot's NodePath).
/// "Player/Gun", "../Enemy", "/root/Main", "%Hud/Score", "Mesh:position:x".</summary>
public sealed class NodePath : IEquatable<NodePath>
{
    private readonly string[] _names;
    private readonly string[] _subnames;
    private readonly bool _absolute;

    public NodePath() : this("") { }

    public NodePath(string path)
    {
        path ??= "";
        int colon = path.IndexOf(':');
        string namePart = colon < 0 ? path : path[..colon];
        _subnames = colon < 0 ? System.Array.Empty<string>() : path[(colon + 1)..].Split(':', StringSplitOptions.RemoveEmptyEntries);
        _absolute = namePart.StartsWith('/');
        _names = namePart.Split('/', StringSplitOptions.RemoveEmptyEntries);
    }

    public static implicit operator NodePath(string from) => new(from);
    public static implicit operator string(NodePath from) => from?.ToString() ?? "";

    /// <summary>True when the path has no names and no property part.</summary>
    public bool IsEmpty => _names.Length == 0 && _subnames.Length == 0 && !_absolute;

    public bool IsAbsolute() => _absolute;
    public int GetNameCount() => _names.Length;
    public string GetName(int idx) => idx >= 0 && idx < _names.Length ? _names[idx] : "";
    public int GetSubnameCount() => _subnames.Length;
    public string GetSubname(int idx) => idx >= 0 && idx < _subnames.Length ? _subnames[idx] : "";
    public string GetConcatenatedNames() => string.Join('/', _names);
    public string GetConcatenatedSubnames() => string.Join(':', _subnames);

    /// <summary>The path read as a property path: ":position:x" for "position:x" style, names kept.</summary>
    public NodePath GetAsPropertyPath() => new(":" + (_names.Length > 0 ? GetConcatenatedNames() + (_subnames.Length > 0 ? ":" : "") : "") + GetConcatenatedSubnames());

    /// <summary>The node-name part alone, without any property part.</summary>
    internal NodePath WithoutSubnames()
    {
        if (_subnames.Length == 0) return this;
        return new NodePath((_absolute ? "/" : "") + GetConcatenatedNames());
    }

    public static bool operator ==(NodePath? l, NodePath? r)
    {
        if (ReferenceEquals(l, r)) return true;
        if (l is null || r is null) return false;
        return l.Equals(r);
    }
    public static bool operator !=(NodePath? l, NodePath? r) => !(l == r);

    public bool Equals(NodePath? other) => other is not null && ToString() == other.ToString();
    public override bool Equals(object? obj) => obj is NodePath n && Equals(n);
    public override int GetHashCode() => ToString().GetHashCode();

    public override string ToString()
    {
        var sb = new StringBuilder();
        if (_absolute) sb.Append('/');
        sb.Append(string.Join('/', _names));
        foreach (string s in _subnames) sb.Append(':').Append(s);
        return sb.ToString();
    }
}
