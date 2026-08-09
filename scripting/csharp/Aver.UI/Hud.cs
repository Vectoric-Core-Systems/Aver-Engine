// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The game-facing UI: layers, colours, rectangles and the immediate-mode draw calls.
namespace Aver.UI;

/// <summary>Which band a widget composites in. Order within a layer is the order you draw in.</summary>
public enum Layer
{
    /// <summary>Backdrops and letterboxing — under everything.</summary>
    Background = 0,
    /// <summary>The HUD and ordinary widgets. The default.</summary>
    Content = 1,
    /// <summary>Menus and modal panels, over the HUD.</summary>
    Overlay = 2,
    /// <summary>Things that must sit above menus.</summary>
    Tooltip = 3,
    /// <summary>Never shipped, always on top.</summary>
    Debug = 4,
}

/// <summary>A colour written as ARGB. Stored as the 0xAABBGGRR the native vertex holds.</summary>
public readonly struct Colour
{
    private readonly uint _abgr;

    private Colour(uint abgr) => _abgr = abgr;

    /// <summary>Native packing, for the P/Invoke layer only.</summary>
    internal uint Packed => _abgr;

    /// <summary>From bytes: <c>Colour.Rgb(232, 228, 220)</c>, opaque unless an alpha is given.</summary>
    public static Colour Rgb(byte r, byte g, byte b, byte a = 255)
        => new((uint)(r | (g << 8) | (b << 16) | (a << 24)));

    /// <summary>From the familiar hex spelling: <c>Colour.Argb(0xFFE8E4DC)</c>.</summary>
    public static Colour Argb(uint argb)
        => Rgb((byte)(argb >> 16), (byte)(argb >> 8), (byte)argb, (byte)(argb >> 24));

    /// <summary>The same colour at a different opacity, 0..1. Clamped.</summary>
    public Colour WithAlpha(float alpha)
    {
        float a = alpha < 0f ? 0f : (alpha > 1f ? 1f : alpha);
        return new((_abgr & 0x00FFFFFFu) | ((uint)(a * 255f + 0.5f) << 24));
    }

    /// <summary>Linear blend between two colours, <paramref name="t"/> clamped to 0..1.</summary>
    public static Colour Lerp(Colour from, Colour to, float t)
    {
        float k = t < 0f ? 0f : (t > 1f ? 1f : t);
        static byte Mix(uint x, uint y, int shift, float k)
            => (byte)((((x >> shift) & 0xFF) * (1f - k)) + (((y >> shift) & 0xFF) * k) + 0.5f);
        return new((uint)(Mix(from._abgr, to._abgr, 0, k)
                        | (Mix(from._abgr, to._abgr, 8, k) << 8)
                        | (Mix(from._abgr, to._abgr, 16, k) << 16)
                        | (Mix(from._abgr, to._abgr, 24, k) << 24)));
    }
}

/// <summary>The game's UI: rectangles, in screen pixels, with a top-left origin. Draw from a tick;
/// the host clears and submits the list around the frame.</summary>
public static class Hud
{
    private static readonly float[] s_viewport = new float[4];

    /// <summary>The rectangle this frame's UI is laid out against, in backbuffer pixels.</summary>
    public static Rect Viewport
    {
        get
        {
            Native.aver_ui_viewport(s_viewport);
            return new Rect(s_viewport[0], s_viewport[1], s_viewport[2], s_viewport[3]);
        }
    }

    /// <summary>The band subsequent draws land in.</summary>
    public static Layer CurrentLayer
    {
        get => (Layer)Native.aver_ui_layer();
        set => Native.aver_ui_set_layer((int)value);
    }

    /// <summary>A solid rectangle.</summary>
    public static void Box(float x, float y, float w, float h, Colour colour)
        => Native.aver_ui_rect(x, y, w, h, colour.Packed);

    /// <summary>A solid rectangle.</summary>
    public static void Box(Rect r, Colour colour)
        => Native.aver_ui_rect(r.X, r.Y, r.Width, r.Height, colour.Packed);

    /// <summary>A rectangle with a border drawn inside its bounds.</summary>
    public static void Frame(Rect r, float thickness, Colour border, Colour fill)
    {
        Box(r, border);
        float t = thickness < 0f ? 0f : thickness;
        Box(new Rect(r.X + t, r.Y + t, r.Width - 2f * t, r.Height - 2f * t), fill);
    }

    /// <summary>A horizontal bar filled left to right by <paramref name="fraction"/> (0..1).</summary>
    public static void Bar(Rect r, float fraction, Colour border, Colour empty, Colour full)
    {
        Frame(r, 1f, border, empty);
        float k = fraction < 0f ? 0f : (fraction > 1f ? 1f : fraction);
        if (k <= 0f) return;
        Box(new Rect(r.X + 1f, r.Y + 1f, (r.Width - 2f) * k, r.Height - 2f), full);
    }

    /// <summary>A textured quad. <paramref name="texture"/> is 0 for the built-in white texel.</summary>
    public static void Image(Rect r, ulong texture, Colour tint,
                             float u0 = 0f, float v0 = 0f, float u1 = 1f, float v1 = 1f)
        => Native.aver_ui_textured_rect(r.X, r.Y, r.Width, r.Height, texture, u0, v0, u1, v1, tint.Packed);

    /// <summary>Confines subsequent draws to <paramref name="r"/> until <see cref="PopClip"/>. Nested
    /// clips INTERSECT, so a child can never escape its parent.</summary>
    public static void PushClip(Rect r)
        => Native.aver_ui_push_clip((int)r.X, (int)r.Y, (int)(r.X + r.Width), (int)(r.Y + r.Height));

    /// <summary>Restore the enclosing clip.</summary>
    public static void PopClip() => Native.aver_ui_pop_clip();

    /// <summary>How many draw calls this frame's UI will cost. For a debug readout, not for logic.</summary>
    public static int DrawCallCount => Native.aver_ui_command_count();
}

/// <summary>A rectangle in screen pixels, top-left origin, with +Y running down the screen.</summary>
public readonly record struct Rect(float X, float Y, float Width, float Height)
{
    /// <summary>Right edge.</summary>
    public float Right => X + Width;
    /// <summary>Bottom edge.</summary>
    public float Bottom => Y + Height;

    /// <summary>The same rectangle shrunk by <paramref name="by"/> on every side.</summary>
    public Rect Inset(float by) => new(X + by, Y + by, Width - 2f * by, Height - 2f * by);

    /// <summary>A rectangle of this size placed relative to a container, by fractional anchor.</summary>
    public static Rect Anchored(Rect container, float ax, float ay, float w, float h,
                                float offsetX = 0f, float offsetY = 0f)
        => new(container.X + container.Width * ax - w * ax + offsetX,
               container.Y + container.Height * ay - h * ay + offsetY,
               w, h);
}
