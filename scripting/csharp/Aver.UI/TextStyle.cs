// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Text styling types shared by Hud.TextBlock, UiText.Measure and the widget text properties (docs/GAME_UI_TEXT.md).
namespace Aver.UI;

/// <summary>Where a line of text sits in its box. Mirrors the native <c>AVER_UI_TEXT_ALIGN_*</c> and the widget
/// property <c>textAlign</c>. <see cref="Start"/> and <see cref="End"/> follow the paragraph direction: in
/// right-to-left text Start is the right edge.</summary>
public enum TextAlignment
{
    /// <summary>The left edge, whatever the text direction.</summary>
    Left = 0,
    /// <summary>Centred.</summary>
    Center = 1,
    /// <summary>The right edge, whatever the text direction.</summary>
    Right = 2,
    /// <summary>Where the text begins: left for left-to-right text, right for right-to-left.</summary>
    Start = 3,
    /// <summary>Where the text ends: right for left-to-right text, left for right-to-left.</summary>
    End = 4,
}

/// <summary>The direction of a paragraph. Mirrors <c>AVER_UI_TEXT_DIR_*</c> and the widget property <c>textDir</c>.</summary>
public enum TextDirection
{
    /// <summary>The first strong letter decides (Hebrew or Arabic: right to left; none: left to right).</summary>
    Auto = 0,
    /// <summary>Always left to right.</summary>
    LeftToRight = 1,
    /// <summary>Always right to left.</summary>
    RightToLeft = 2,
}

/// <summary>Switches for a text call (no flags is <c>default</c>). Mirrors <c>AVER_UI_TEXT_FLAG_*</c> one for one, which
/// tests/abi/AbiEnumTest checks.</summary>
[Flags]
public enum TextFlags
{
    /// <summary>Use the family's bold face.</summary>
    Bold = 1,
    /// <summary>Use the family's italic face (or shear the regular one).</summary>
    Italic = 2,
    /// <summary>Read &lt;b&gt; &lt;i&gt; &lt;color=#RRGGBB&gt; &lt;size=N&gt; &lt;font=Name&gt; &lt;br&gt; tags.</summary>
    Rich = 4,
    /// <summary>End a clipped line with an ellipsis (needs a width).</summary>
    Ellipsis = 8,
    /// <summary>Wrap lines at the given width.</summary>
    Wrap = 16,
}

/// <summary>The look of a run of text: a family of the loaded font set (null or empty is the default family), a size
/// in pixels, and the style switches.</summary>
/// <param name="Family">A family name from the font set, or null for the default.</param>
/// <param name="Size">The em size in pixels.</param>
/// <param name="Bold">Use the bold face.</param>
/// <param name="Italic">Use the italic face.</param>
/// <param name="Rich">Interpret rich-text tags.</param>
/// <param name="Ellipsis">End a clipped line with an ellipsis.</param>
public readonly record struct TextStyle(string? Family = null, float Size = 16f, bool Bold = false, bool Italic = false,
                                        bool Rich = false, bool Ellipsis = false)
{
    /// <summary>The style as native flags (wrapping is added by the call that has a width).</summary>
    internal int ToFlags() =>
        (Bold ? (int)TextFlags.Bold : 0) | (Italic ? (int)TextFlags.Italic : 0) |
        (Rich ? (int)TextFlags.Rich : 0) | (Ellipsis ? (int)TextFlags.Ellipsis : 0);
}

/// <summary>The size of laid-out text.</summary>
/// <param name="Width">The widest line, in pixels.</param>
/// <param name="Height">The whole block, in pixels.</param>
/// <param name="Lines">The number of lines.</param>
public readonly record struct TextMetrics(float Width, float Height, int Lines);

/// <summary>The text engine's counters (<c>aver_ui_text_stats</c>).</summary>
/// <param name="Pages">Atlas pages in use.</param>
/// <param name="GlyphsResident">Glyphs currently in the atlas.</param>
/// <param name="RasterThisFrame">Glyphs rasterised this frame (the budget bounds it).</param>
/// <param name="RasterTotal">Glyphs rasterised since start.</param>
/// <param name="EvictedPages">Atlas pages recycled since start.</param>
/// <param name="Overflow">Glyphs that found no room in a frame.</param>
/// <param name="LayoutHits">Layouts served from the cache.</param>
/// <param name="LayoutMisses">Layouts built.</param>
/// <param name="Faces">Font faces open.</param>
public readonly record struct TextStats(int Pages, int GlyphsResident, int RasterThisFrame, long RasterTotal,
                                        int EvictedPages, int Overflow, int LayoutHits, int LayoutMisses, int Faces);
