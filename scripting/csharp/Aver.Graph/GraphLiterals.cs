// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Text forms of the pin values that are not a bare number: float3 `x,y,z`, string with %XX escapes, entity int.
// The .ocgraph format is whitespace-tokenised, so a string's space, tab and newline travel as %20 %09 %0A.

using System;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Aver.Graph;

public static class GraphLiterals
{
    /// <summary>Encodes text for one .ocgraph token: % as %25, space %20, tab %09, newline %0A, return %0D.</summary>
    public static string EncodeString(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (char c in s)
        {
            switch (c)
            {
                case '%': sb.Append("%25"); break;
                case ' ': sb.Append("%20"); break;
                case '\t': sb.Append("%09"); break;
                case '\n': sb.Append("%0A"); break;
                case '\r': sb.Append("%0D"); break;
                default: sb.Append(c); break;
            }
        }
        return sb.ToString();
    }

    /// <summary>Reverses <see cref="EncodeString"/>; a % not followed by two hex digits stays as written.</summary>
    public static string DecodeString(string s)
    {
        if (s.IndexOf('%') < 0) return s;
        var sb = new StringBuilder(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] == '%' && i + 2 < s.Length && Uri.IsHexDigit(s[i + 1]) && Uri.IsHexDigit(s[i + 2]))
            {
                sb.Append((char)Convert.ToInt32(s.Substring(i + 1, 2), 16));
                i += 2;
            }
            else sb.Append(s[i]);
        }
        return sb.ToString();
    }

    /// <summary>Reads `x,y,z`; a missing or empty part is 0, a part that is not a number fails.</summary>
    public static bool TryParseFloat3(string s, out Vector3 v)
    {
        v = Vector3.Zero;
        string[] parts = s.Split(',');
        if (parts.Length > 3) return false;
        Span<float> c = stackalloc float[3];
        for (int i = 0; i < parts.Length; i++)
        {
            string p = parts[i].Trim();
            if (p.Length == 0) continue;
            if (!float.TryParse(p, NumberStyles.Float, CultureInfo.InvariantCulture, out c[i])) return false;
        }
        v = new Vector3(c[0], c[1], c[2]);
        return true;
    }

    public static string FormatFloat3(Vector3 v) =>
        string.Create(CultureInfo.InvariantCulture, $"{v.X:R},{v.Y:R},{v.Z:R}");

    /// <summary>A literal as the CLR value its pin type carries (Vector3, string, int), or false when it does not parse.
    /// Float, Int and Bool are included so one call types any pin default.</summary>
    public static bool TryParse(PinType type, string text, out object value)
    {
        value = 0;
        switch (type)
        {
            case PinType.Float3:
                if (!TryParseFloat3(text, out var v)) return false;
                value = v; return true;
            case PinType.String:
                value = DecodeString(text); return true;
            case PinType.Entity:
            case PinType.Int:
                if (!int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i)) return false;
                value = i; return true;
            case PinType.Float:
                if (!float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var f)) return false;
                value = f; return true;
            case PinType.Bool:
                if (bool.TryParse(text, out var b)) { value = b; return true; }
                if (text == "1") { value = true; return true; }
                if (text == "0") { value = false; return true; }
                return false;
            default:
                return false;
        }
    }

    /// <summary>The zero value of a data pin type: Vector3.Zero, "", 0.</summary>
    public static object DefaultFor(PinType type) => type switch
    {
        PinType.Float3 => Vector3.Zero,
        PinType.String => "",
        PinType.Float => 0f,
        PinType.Bool => false,
        _ => 0,
    };
}
