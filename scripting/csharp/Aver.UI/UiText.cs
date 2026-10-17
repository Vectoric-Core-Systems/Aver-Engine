// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The game UI's text engine from a script: measuring, glyph queries, prewarming and counters.
namespace Aver.UI;

/// <summary>The runtime text engine: TrueType fonts with fallback chains, kerning, Arabic shaping, bidirectional
/// text, line breaking and styling (docs/GAME_UI_TEXT.md). The HOST loads the font set and follows the
/// culture; a game normally only measures and draws. Drawing is <see cref="Hud.TextBlock"/> or a widget's text
/// properties.</summary>
public static class UiText
{
    private static readonly float[] s_whl = new float[3];
    private static readonly float[] s_stats = new float[12];

    /// <summary>Loads an .ocfonts document, replacing the font set. The host normally does this at startup
    /// (from <c>Content/UI/Fonts.ocfonts</c> or the built-in default). False with <see cref="LastError"/> set
    /// when it does not parse.</summary>
    public static bool LoadFontSet(string ocfontsText) => Native.aver_ui_text_load_fontset(ocfontsText ?? string.Empty) != 0;

    /// <summary>Why the last <see cref="LoadFontSet"/> failed.</summary>
    public static string LastError => Native.Utf8(Native.aver_ui_text_error());

    /// <summary>Atlas page size (256..2048), page cap (1..64) and the per-frame glyph budget. A value of 0 keeps the
    /// current one. Changing the page size or cap drops every cached glyph.</summary>
    public static void Configure(int pageSize = 0, int maxPages = 0, int glyphBudget = 0)
        => Native.aver_ui_text_configure(pageSize, maxPages, glyphBudget);

    /// <summary>The UI language (BCP-47, e.g. "ja" or "zh-Hans"). It picks the face for Han characters and applies
    /// the font set's OVERRIDE rows. The host normally follows the active culture, so a game does not set this.</summary>
    public static string Language { set => Native.aver_ui_text_set_language(value ?? string.Empty); }

    /// <summary>Whether some face of <paramref name="family"/> (null = the default) has the code point.</summary>
    public static bool HasGlyph(string? family, int codePoint)
        => Native.aver_ui_text_has_glyph(family, (uint)codePoint) != 0;

    /// <summary>The size <paramref name="text"/> would occupy. A <paramref name="maxWidth"/> above 0 wraps it (or ends
    /// a line with an ellipsis when the style asks). A localization reference ("@Table:Key=Default") is resolved
    /// first. A zero result with no lines means no font is available.</summary>
    public static TextMetrics Measure(string text, TextStyle style, float maxWidth = 0f)
    {
        int flags = style.ToFlags() | (maxWidth > 0f ? (int)TextFlags.Wrap : 0);
        if (Native.aver_ui_text_measure(text ?? string.Empty, style.Family, style.Size, flags, maxWidth, s_whl) == 0)
            return default;
        return new TextMetrics(s_whl[0], s_whl[1], (int)s_whl[2]);
    }

    /// <summary>Rasterises the glyphs of <paramref name="text"/> now, ignoring the per-frame budget: call it while a
    /// loading screen is up so the first frame of gameplay does not spread a big string over several frames.</summary>
    public static void Prewarm(string text, TextStyle style)
        => Native.aver_ui_text_prewarm(text ?? string.Empty, style.Family, style.Size, style.ToFlags());

    /// <summary>The engine's counters, for a debug readout.</summary>
    public static TextStats Stats
    {
        get
        {
            Native.aver_ui_text_stats(s_stats);
            return new TextStats((int)s_stats[0], (int)s_stats[1], (int)s_stats[2], (long)s_stats[3], (int)s_stats[4],
                                 (int)s_stats[5], (int)s_stats[6], (int)s_stats[7], (int)s_stats[10]);
        }
    }
}
