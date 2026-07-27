namespace Aver.UI;

/// <summary>Which band a widget composites in. Coarse and named on purpose.</summary>
/// <remarks>
/// A HUD, a pause menu and a tooltip are separate things that must layer predictably without any of
/// them knowing the others exist. Sorting by a per-widget depth number is the alternative and it is
/// worse: it makes every z decision global, so adding a tooltip means auditing every other widget's
/// number. Order WITHIN a layer is the order you draw in.
/// </remarks>
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

/// <summary>A colour, in the spelling a person writes rather than the one a vertex holds.</summary>
/// <remarks>
/// The native vertex stores 0xAABBGGRR, which is what a R8G8B8A8_UNORM attribute reads on a
/// little-endian machine and is not how anybody writes a colour. Everything here takes the familiar
/// ARGB form and swizzles once, at the boundary, so a game never carries the packing around.
/// </remarks>
public readonly struct Colour
{
    private readonly uint _abgr;

    private Colour(uint abgr) => _abgr = abgr;

    /// <summary>Native packing. Internal because the whole point is that a caller never sees it.</summary>
    internal uint Packed => _abgr;

    /// <summary>From bytes: <c>Colour.Rgb(232, 228, 220)</c>, opaque unless an alpha is given.</summary>
    public static Colour Rgb(byte r, byte g, byte b, byte a = 255)
        => new((uint)(r | (g << 8) | (b << 16) | (a << 24)));

    /// <summary>From the familiar hex spelling: <c>Colour.Argb(0xFFE8E4DC)</c>.</summary>
    public static Colour Argb(uint argb)
        => Rgb((byte)(argb >> 16), (byte)(argb >> 8), (byte)argb, (byte)(argb >> 24));

    /// <summary>The same colour at a different opacity, 0..1. Clamped, because a HUD fades things.</summary>
    public Colour WithAlpha(float alpha)
    {
        float a = alpha < 0f ? 0f : (alpha > 1f ? 1f : alpha);
        return new((_abgr & 0x00FFFFFFu) | ((uint)(a * 255f + 0.5f) << 24));
    }

    /// <summary>Linear blend, for a bar that changes colour as it empties.</summary>
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

/// <summary>The game's UI: rectangles, in screen pixels, with a top-left origin.</summary>
/// <remarks>
/// <para>
/// Draw from a tick. The HOST clears the list once per frame before anything ticks and submits it
/// afterwards, so a game only ever draws — there is no Begin for a game to call, and that is
/// deliberate: a game that cleared the list would erase whatever another system had contributed, and
/// the last one to run would win with nothing anywhere to say so.
/// </para>
/// <para>
/// Anchor to <see cref="Viewport"/>, never to the window. In the editor the game is drawn into a
/// dockspace panel and the two differ; a HUD laid out against the window would sit partly under the
/// editor's own chrome. In a shipped build they are the same and nothing changes.
/// </para>
/// <para>
/// There is no text yet. That is not an omission here — nothing in the engine can rasterise a glyph.
/// </para>
/// </remarks>
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

    /// <summary>A rectangle with a border drawn inside its bounds, the way a HUD frame is drawn.</summary>
    /// <remarks>
    /// Inside, not outside: a frame that grew its own bounds would not fit the layout that positioned
    /// it, and every caller would have to subtract the thickness back off by hand.
    /// </remarks>
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

    /// <summary>
    /// Confine subsequent draws to <paramref name="r"/> until <see cref="PopClip"/>. Nested clips
    /// INTERSECT, so a child can never escape its parent by pushing a larger rectangle.
    /// </summary>
    public static void PushClip(Rect r)
        => Native.aver_ui_push_clip((int)r.X, (int)r.Y, (int)(r.X + r.Width), (int)(r.Y + r.Height));

    /// <summary>Restore the enclosing clip.</summary>
    public static void PopClip() => Native.aver_ui_pop_clip();

    /// <summary>How many draw calls this frame's UI will cost. For a debug readout, not for logic.</summary>
    public static int DrawCallCount => Native.aver_ui_command_count();
}

/// <summary>A rectangle in screen pixels, top-left origin.</summary>
/// <param name="X">Left edge.</param>
/// <param name="Y">Top edge.</param>
/// <param name="Width">Extent along +X.</param>
/// <param name="Height">Extent along +Y, which runs DOWN the screen.</param>
public readonly record struct Rect(float X, float Y, float Width, float Height)
{
    /// <summary>Right edge.</summary>
    public float Right => X + Width;
    /// <summary>Bottom edge.</summary>
    public float Bottom => Y + Height;

    /// <summary>The same rectangle shrunk by <paramref name="by"/> on every side.</summary>
    public Rect Inset(float by) => new(X + by, Y + by, Width - 2f * by, Height - 2f * by);

    /// <summary>A rectangle of this size placed relative to a container, by fractional anchor.</summary>
    /// <remarks>
    /// Anchoring rather than absolute placement, because a HUD outlives the resolution it was
    /// authored at: an element pinned to the bottom-right by subtraction moves off-screen the moment
    /// the window is smaller than the number that was subtracted.
    /// </remarks>
    public static Rect Anchored(Rect container, float ax, float ay, float w, float h,
                                float offsetX = 0f, float offsetY = 0f)
        => new(container.X + container.Width * ax - w * ax + offsetX,
               container.Y + container.Height * ay - h * ay + offsetY,
               w, h);
}
