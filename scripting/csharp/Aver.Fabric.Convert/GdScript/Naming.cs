// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript names to C# names (snake_case to PascalCase and camelCase, keywords, constants).

using System.Text;

namespace Aver.Fabric.Convert.GdScript;

internal static class Naming
{
    private static readonly HashSet<string> s_keywords = new()
    {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const", "continue", "decimal",
        "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern", "false", "finally", "fixed", "float", "for",
        "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new", "null", "object",
        "operator", "out", "override", "params", "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
        "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked",
        "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
    };

    public static bool IsKeyword(string s) => s_keywords.Contains(s);

    /// <summary>"@event" for a C# keyword, the name itself otherwise.</summary>
    public static string Safe(string s) => s_keywords.Contains(s) ? "@" + s : s;

    private static string[] Words(string s) => s.Split('_', StringSplitOptions.RemoveEmptyEntries);

    /// <summary>move_speed to MoveSpeed; WALK_SPEED to WalkSpeed; _private_thing to PrivateThing; already-Pascal names stay.</summary>
    public static string Pascal(string s)
    {
        var sb = new StringBuilder();
        foreach (string w in Words(s))
        {
            bool allUpper = w.All(c => !char.IsLetter(c) || char.IsUpper(c));
            string t = allUpper && w.Length > 1 ? w.ToLowerInvariant() : w;
            sb.Append(char.ToUpperInvariant(t[0])).Append(t, 1, t.Length - 1);
        }
        string r = sb.ToString();
        if (r.Length == 0) return s;
        return char.IsDigit(r[0]) ? "_" + r : r;
    }

    /// <summary>move_speed to moveSpeed (locals, parameters).</summary>
    public static string Camel(string s)
    {
        string p = Pascal(s);
        if (p.Length == 0) return s;
        // Keep a leading acronym readable: URL stays url, HTTPRequest stays hTTPRequest only when the source was snake_case.
        return char.ToLowerInvariant(p[0]) + p[1..];
    }

    /// <summary>A private-looking member (_was_on_floor) keeps its underscore: _wasOnFloor.</summary>
    public static string FieldName(string s) => s.StartsWith('_') ? "_" + Camel(s.TrimStart('_')) : Pascal(s);

    /// <summary>A method name: _update_state to UpdateState (the scene converter's lookup drops leading underscores).</summary>
    public static string MethodName(string s) => Pascal(s.TrimStart('_'));

    /// <summary>The name of an engine virtual: _ready to _Ready, _physics_process to _PhysicsProcess.</summary>
    public static string VirtualName(string s) => "_" + Pascal(s.TrimStart('_'));

    /// <summary>"AudioStreamPlayer3D" stays; "my_class" gets Pascal.</summary>
    public static string TypeName(string s) => s.Length > 0 && char.IsUpper(s[0]) ? s : Pascal(s);

    public static string Quote(string s)
    {
        var sb = new StringBuilder("\"");
        foreach (char c in s)
        {
            switch (c)
            {
                case '\\': sb.Append("\\\\"); break;
                case '"': sb.Append("\\\""); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                case '\0': sb.Append("\\0"); break;
                default:
                    if (c < 0x20 || c == 0x7f) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        return sb.Append('"').ToString();
    }

    public static string Num(double d)
    {
        if (double.IsPositiveInfinity(d)) return "double.PositiveInfinity";
        if (double.IsNegativeInfinity(d)) return "double.NegativeInfinity";
        if (double.IsNaN(d)) return "double.NaN";
        string s = d.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
        return s.Contains('.') || s.Contains('E') || s.Contains('e') ? s : s + ".0";
    }
}
