// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Color (non-premultiplied, float channels) and the named colours scripts use most.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>An RGBA colour with float channels in 0..1 (Godot's Color).</summary>
public struct Color : IEquatable<Color>
{
    public float R;
    public float G;
    public float B;
    public float A;

    public int R8 { readonly get => To8(R); set => R = value / 255f; }
    public int G8 { readonly get => To8(G); set => G = value / 255f; }
    public int B8 { readonly get => To8(B); set => B = value / 255f; }
    public int A8 { readonly get => To8(A); set => A = value / 255f; }

    public float H
    {
        readonly get
        {
            float min = Math.Min(R, Math.Min(G, B)), max = Math.Max(R, Math.Max(G, B)), delta = max - min;
            if (delta == 0f) return 0f;
            float h;
            if (R == max) h = (G - B) / delta;
            else if (G == max) h = 2f + (B - R) / delta;
            else h = 4f + (R - G) / delta;
            h /= 6f;
            if (h < 0f) h += 1f;
            return h;
        }
        set => this = FromHsv(value, S, V, A);
    }

    public float S
    {
        readonly get
        {
            float max = Math.Max(R, Math.Max(G, B)), min = Math.Min(R, Math.Min(G, B));
            return max == 0f ? 0f : (max - min) / max;
        }
        set => this = FromHsv(H, value, V, A);
    }

    public float V
    {
        readonly get => Math.Max(R, Math.Max(G, B));
        set => this = FromHsv(H, S, value, A);
    }

    public float this[int index]
    {
        readonly get => index switch { 0 => R, 1 => G, 2 => B, 3 => A, _ => throw new ArgumentOutOfRangeException(nameof(index)) };
        set
        {
            switch (index)
            {
                case 0: R = value; break;
                case 1: G = value; break;
                case 2: B = value; break;
                case 3: A = value; break;
                default: throw new ArgumentOutOfRangeException(nameof(index));
            }
        }
    }

    private static int To8(float v) => (int)(Math.Clamp(v, 0f, 1f) * 255f + 0.5f);

    public Color(float r, float g, float b, float a = 1f) { R = r; G = g; B = b; A = a; }
    public Color(Color c, float a = 1f) { R = c.R; G = c.G; B = c.B; A = a; }

    /// <summary>From a name ("red") or an HTML hex code ("#ff8800", "ff880080", "#f80").</summary>
    public Color(string code)
    {
        if (!TryParse(code, out Color c)) throw new ArgumentException($"Invalid color code: '{code}'.", nameof(code));
        this = c;
    }

    /// <summary>From a name or hex code, or <paramref name="fallback"/> when it is neither.</summary>
    public Color(string code, Color fallback)
    {
        this = TryParse(code, out Color c) ? c : fallback;
    }

    /// <summary>From 0..255 channels.</summary>
    public static Color Color8(byte r8, byte g8, byte b8, byte a8 = 255) => new(r8 / 255f, g8 / 255f, b8 / 255f, a8 / 255f);

    public static Color FromHtml(string html) => TryParse(html, out Color c) ? c : throw new ArgumentException($"Invalid color code: '{html}'.", nameof(html));
    public static bool HtmlIsValid(string color) => TryParse(color, out _);

    /// <summary>From a 32-bit 0xRRGGBBAA value.</summary>
    public static Color FromRgba32(uint rgba) => Color8((byte)(rgba >> 24), (byte)(rgba >> 16), (byte)(rgba >> 8), (byte)rgba);

    public static Color FromHsv(float hue, float saturation, float value, float alpha = 1f)
    {
        if (saturation == 0f) return new Color(value, value, value, alpha);
        hue = (hue * 6f) % 6f;
        if (hue < 0f) hue += 6f;
        int i = (int)MathF.Floor(hue);
        float f = hue - i, p = value * (1f - saturation), q = value * (1f - saturation * f), t = value * (1f - saturation * (1f - f));
        return i switch
        {
            0 => new Color(value, t, p, alpha),
            1 => new Color(q, value, p, alpha),
            2 => new Color(p, value, t, alpha),
            3 => new Color(p, q, value, alpha),
            4 => new Color(t, p, value, alpha),
            _ => new Color(value, p, q, alpha),
        };
    }

    private static bool TryParse(string? code, out Color c)
    {
        c = default;
        if (string.IsNullOrEmpty(code)) return false;
        string s = code.Trim();
        if (s.StartsWith('#')) s = s[1..];
        else if (Colors.TryGetNamed(s, out c)) return true;
        if (s.Length != 3 && s.Length != 4 && s.Length != 6 && s.Length != 8) return false;
        int[] v = new int[s.Length];
        for (int i = 0; i < s.Length; i++)
        {
            if (!Uri.IsHexDigit(s[i])) return false;
            v[i] = Convert.ToInt32(s[i].ToString(), 16);
        }
        if (s.Length <= 4)
        {
            c = new Color(v[0] * 17 / 255f, v[1] * 17 / 255f, v[2] * 17 / 255f, s.Length == 4 ? v[3] * 17 / 255f : 1f);
        }
        else
        {
            c = new Color((v[0] * 16 + v[1]) / 255f, (v[2] * 16 + v[3]) / 255f, (v[4] * 16 + v[5]) / 255f, s.Length == 8 ? (v[6] * 16 + v[7]) / 255f : 1f);
        }
        return true;
    }

    public readonly Color Blend(Color over)
    {
        float sa = 1f - over.A, a = A * sa + over.A;
        if (a == 0f) return new Color(0, 0, 0, 0);
        return new Color((R * A * sa + over.R * over.A) / a, (G * A * sa + over.G * over.A) / a, (B * A * sa + over.B * over.A) / a, a);
    }

    public readonly Color Clamp(Color? min = null, Color? max = null)
    {
        Color lo = min ?? new Color(0, 0, 0, 0), hi = max ?? new Color(1, 1, 1, 1);
        return new Color(Math.Clamp(R, lo.R, hi.R), Math.Clamp(G, lo.G, hi.G), Math.Clamp(B, lo.B, hi.B), Math.Clamp(A, lo.A, hi.A));
    }

    public readonly Color Darkened(float amount) => new(R * (1f - amount), G * (1f - amount), B * (1f - amount), A);
    public readonly Color Lightened(float amount) => new(R + (1f - R) * amount, G + (1f - G) * amount, B + (1f - B) * amount, A);
    public readonly Color Inverted() => new(1f - R, 1f - G, 1f - B, A);
    public readonly float Luminance() => 0.2126f * R + 0.7152f * G + 0.0722f * B;
    public readonly Color Lerp(Color to, float weight) =>
        new(Mathf.Lerp(R, to.R, weight), Mathf.Lerp(G, to.G, weight), Mathf.Lerp(B, to.B, weight), Mathf.Lerp(A, to.A, weight));
    public readonly bool IsEqualApprox(Color other) =>
        Mathf.IsEqualApprox(R, other.R) && Mathf.IsEqualApprox(G, other.G) && Mathf.IsEqualApprox(B, other.B) && Mathf.IsEqualApprox(A, other.A);

    public readonly Color SrgbToLinear() => new(Lin(R), Lin(G), Lin(B), A);
    public readonly Color LinearToSrgb() => new(Srgb(R), Srgb(G), Srgb(B), A);
    private static float Lin(float c) => c < 0.04045f ? c * (1f / 12.92f) : MathF.Pow((c + 0.055f) * (1f / 1.055f), 2.4f);
    private static float Srgb(float c) => c < 0.0031308f ? 12.92f * c : 1.055f * MathF.Pow(c, 1f / 2.4f) - 0.055f;

    public readonly uint ToRgba32() => ((uint)R8 << 24) | ((uint)G8 << 16) | ((uint)B8 << 8) | (uint)A8;
    public readonly uint ToArgb32() => ((uint)A8 << 24) | ((uint)R8 << 16) | ((uint)G8 << 8) | (uint)B8;
    public readonly uint ToAbgr32() => ((uint)A8 << 24) | ((uint)B8 << 16) | ((uint)G8 << 8) | (uint)R8;
    public readonly ulong ToRgba64() => ((ulong)To16(R) << 48) | ((ulong)To16(G) << 32) | ((ulong)To16(B) << 16) | To16(A);
    private static ushort To16(float v) => (ushort)(Math.Clamp(v, 0f, 1f) * 65535f + 0.5f);

    /// <summary>The colour as lowercase hex, "rrggbbaa" or "rrggbb".</summary>
    public readonly string ToHtml(bool includeAlpha = true)
    {
        string s = $"{R8:x2}{G8:x2}{B8:x2}";
        return includeAlpha ? s + $"{A8:x2}" : s;
    }

    public static Color operator +(Color l, Color r) => new(l.R + r.R, l.G + r.G, l.B + r.B, l.A + r.A);
    public static Color operator -(Color l, Color r) => new(l.R - r.R, l.G - r.G, l.B - r.B, l.A - r.A);
    public static Color operator -(Color c) => new(1f - c.R, 1f - c.G, 1f - c.B, 1f - c.A);
    public static Color operator *(Color c, float s) => new(c.R * s, c.G * s, c.B * s, c.A * s);
    public static Color operator *(float s, Color c) => new(c.R * s, c.G * s, c.B * s, c.A * s);
    public static Color operator *(Color l, Color r) => new(l.R * r.R, l.G * r.G, l.B * r.B, l.A * r.A);
    public static Color operator /(Color c, float s) => new(c.R / s, c.G / s, c.B / s, c.A / s);
    public static bool operator ==(Color l, Color r) => l.Equals(r);
    public static bool operator !=(Color l, Color r) => !l.Equals(r);

    public readonly bool Equals(Color other) => R == other.R && G == other.G && B == other.B && A == other.A;
    public override readonly bool Equals(object? obj) => obj is Color c && Equals(c);
    public override readonly int GetHashCode() => HashCode.Combine(R, G, B, A);
    public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({R}, {G}, {B}, {A})");
}

/// <summary>The named colours of Godot's palette that scripts use most (a subset of its full list).</summary>
public static class Colors
{
    private static readonly Dictionary<string, Color> s_named = new(StringComparer.OrdinalIgnoreCase)
    {
        ["aliceblue"] = new(0.94f, 0.97f, 1f), ["aqua"] = new(0f, 1f, 1f), ["azure"] = new(0.94f, 1f, 1f), ["beige"] = new(0.96f, 0.96f, 0.86f),
        ["black"] = new(0f, 0f, 0f), ["blue"] = new(0f, 0f, 1f), ["brown"] = new(0.65f, 0.16f, 0.16f), ["chocolate"] = new(0.82f, 0.41f, 0.12f),
        ["coral"] = new(1f, 0.5f, 0.31f), ["crimson"] = new(0.86f, 0.08f, 0.24f), ["cyan"] = new(0f, 1f, 1f), ["darkblue"] = new(0f, 0f, 0.55f),
        ["darkgray"] = new(0.66f, 0.66f, 0.66f), ["darkgreen"] = new(0f, 0.39f, 0f), ["darkorange"] = new(1f, 0.55f, 0f), ["darkred"] = new(0.55f, 0f, 0f),
        ["deeppink"] = new(1f, 0.08f, 0.58f), ["dimgray"] = new(0.41f, 0.41f, 0.41f), ["dodgerblue"] = new(0.12f, 0.56f, 1f),
        ["forestgreen"] = new(0.13f, 0.55f, 0.13f), ["fuchsia"] = new(1f, 0f, 1f), ["gold"] = new(1f, 0.84f, 0f), ["gray"] = new(0.75f, 0.75f, 0.75f),
        ["green"] = new(0f, 1f, 0f), ["hotpink"] = new(1f, 0.41f, 0.71f), ["indigo"] = new(0.29f, 0f, 0.51f), ["ivory"] = new(1f, 1f, 0.94f),
        ["khaki"] = new(0.94f, 0.9f, 0.55f), ["lavender"] = new(0.9f, 0.9f, 0.98f), ["lightblue"] = new(0.68f, 0.85f, 0.9f), ["lightgray"] = new(0.83f, 0.83f, 0.83f),
        ["lightgreen"] = new(0.56f, 0.93f, 0.56f), ["lime"] = new(0f, 1f, 0f), ["magenta"] = new(1f, 0f, 1f), ["maroon"] = new(0.69f, 0.19f, 0.38f),
        ["navyblue"] = new(0f, 0f, 0.5f), ["olive"] = new(0.5f, 0.5f, 0f), ["orange"] = new(1f, 0.65f, 0f), ["orchid"] = new(0.85f, 0.44f, 0.84f),
        ["pink"] = new(1f, 0.75f, 0.8f), ["purple"] = new(0.63f, 0.13f, 0.94f), ["red"] = new(1f, 0f, 0f), ["salmon"] = new(0.98f, 0.5f, 0.45f),
        ["silver"] = new(0.75f, 0.75f, 0.75f), ["skyblue"] = new(0.53f, 0.81f, 0.92f), ["tan"] = new(0.82f, 0.71f, 0.55f), ["teal"] = new(0f, 0.5f, 0.5f),
        ["tomato"] = new(1f, 0.39f, 0.28f), ["transparent"] = new(1f, 1f, 1f, 0f), ["turquoise"] = new(0.25f, 0.88f, 0.82f), ["violet"] = new(0.93f, 0.51f, 0.93f),
        ["webgray"] = new(0.5f, 0.5f, 0.5f), ["webgreen"] = new(0f, 0.5f, 0f), ["webmaroon"] = new(0.5f, 0f, 0f), ["webpurple"] = new(0.5f, 0f, 0.5f),
        ["white"] = new(1f, 1f, 1f), ["yellow"] = new(1f, 1f, 0f),
    };

    internal static bool TryGetNamed(string name, out Color color) =>
        s_named.TryGetValue(name.Replace("_", "").Replace(" ", ""), out color);

    public static Color AliceBlue => s_named["aliceblue"];
    public static Color Aqua => s_named["aqua"];
    public static Color Azure => s_named["azure"];
    public static Color Beige => s_named["beige"];
    public static Color Black => s_named["black"];
    public static Color Blue => s_named["blue"];
    public static Color Brown => s_named["brown"];
    public static Color Chocolate => s_named["chocolate"];
    public static Color Coral => s_named["coral"];
    public static Color Crimson => s_named["crimson"];
    public static Color Cyan => s_named["cyan"];
    public static Color DarkBlue => s_named["darkblue"];
    public static Color DarkGray => s_named["darkgray"];
    public static Color DarkGreen => s_named["darkgreen"];
    public static Color DarkOrange => s_named["darkorange"];
    public static Color DarkRed => s_named["darkred"];
    public static Color DeepPink => s_named["deeppink"];
    public static Color DimGray => s_named["dimgray"];
    public static Color DodgerBlue => s_named["dodgerblue"];
    public static Color ForestGreen => s_named["forestgreen"];
    public static Color Fuchsia => s_named["fuchsia"];
    public static Color Gold => s_named["gold"];
    public static Color Gray => s_named["gray"];
    public static Color Green => s_named["green"];
    public static Color HotPink => s_named["hotpink"];
    public static Color Indigo => s_named["indigo"];
    public static Color Ivory => s_named["ivory"];
    public static Color Khaki => s_named["khaki"];
    public static Color Lavender => s_named["lavender"];
    public static Color LightBlue => s_named["lightblue"];
    public static Color LightGray => s_named["lightgray"];
    public static Color LightGreen => s_named["lightgreen"];
    public static Color Lime => s_named["lime"];
    public static Color Magenta => s_named["magenta"];
    public static Color Maroon => s_named["maroon"];
    public static Color NavyBlue => s_named["navyblue"];
    public static Color Olive => s_named["olive"];
    public static Color Orange => s_named["orange"];
    public static Color Orchid => s_named["orchid"];
    public static Color Pink => s_named["pink"];
    public static Color Purple => s_named["purple"];
    public static Color Red => s_named["red"];
    public static Color Salmon => s_named["salmon"];
    public static Color Silver => s_named["silver"];
    public static Color SkyBlue => s_named["skyblue"];
    public static Color Tan => s_named["tan"];
    public static Color Teal => s_named["teal"];
    public static Color Tomato => s_named["tomato"];
    public static Color Transparent => s_named["transparent"];
    public static Color Turquoise => s_named["turquoise"];
    public static Color Violet => s_named["violet"];
    public static Color WebGray => s_named["webgray"];
    public static Color WebGreen => s_named["webgreen"];
    public static Color WebMaroon => s_named["webmaroon"];
    public static Color WebPurple => s_named["webpurple"];
    public static Color White => s_named["white"];
    public static Color Yellow => s_named["yellow"];
}
