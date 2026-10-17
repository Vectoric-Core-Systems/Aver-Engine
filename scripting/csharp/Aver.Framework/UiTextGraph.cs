// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// What the Game UI text nodes call (docs/GAME_UI_TEXT.md section 2.13): AN_SetUiFont, AN_SetUiTextStyle,
// AN_SetUiTextColor, AN_GetUiTextSize. The rows are in scripting/csharp/Aver.Graph/GraphUiTextNodeTable.cs;
// the palette mirror is sandbox/src/GraphNodeDefsUiText.hpp.
using Aver.UI;

namespace Aver.Framework;

/// <summary>The methods behind the Game UI text nodes. Widgets cross the graph as plain ints, like the other UI
/// nodes. Every method answers false for a widget that no longer exists, and in a build with no UI library (a
/// headless tool) it quietly does nothing.</summary>
internal static class UiTextGraphInterop
{
    private static bool TryWidget(int id, out Widget w)
    {
        w = id > 0 ? new Widget((uint)id) : Widget.None;
        return w.Exists;
    }

    /// <summary>Sets a widget's font: size in design pixels (0 = the theme's), bold, italic and, when
    /// <paramref name="family"/> is not empty, the family. An empty family keeps the current one.</summary>
    internal static bool SetUiFontForGraph(int widget, float size, bool bold, bool italic, string family)
    {
        try
        {
            if (!TryWidget(widget, out Widget w)) return false;
            w.FontSize = size < 0f ? 0f : size;
            w.Bold = bold;
            w.Italic = italic;
            if (!string.IsNullOrEmpty(family)) w.FontFamily = family;
            return true;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }

    /// <summary>Sets all nine text style properties at once: alignment (0 left, 1 centre, 2 right, 3 start, 4 end),
    /// direction (0 auto, 1 LTR, 2 RTL), wrap, ellipsis, line limit (0 = none), line spacing, letter spacing, rich text.</summary>
    internal static bool SetUiTextStyleForGraph(int widget, int align, int dir, bool wrap, bool ellipsis, int maxLines,
                                                float lineSpacing, float letterSpacing, bool rich)
    {
        try
        {
            if (!TryWidget(widget, out Widget w)) return false;
            w.TextAlign = (TextAlignment)Math.Clamp(align, 0, 4);
            w.TextDir = (TextDirection)Math.Clamp(dir, 0, 2);
            w.Wrap = wrap;
            w.Ellipsis = ellipsis;
            w.MaxLines = Math.Max(0, maxLines);
            w.LineSpacing = lineSpacing > 0f ? lineSpacing : 1f;
            w.LetterSpacing = letterSpacing;
            w.RichText = rich;
            return true;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }

    /// <summary>Sets a widget's text colour; each channel is 0..1.</summary>
    internal static bool SetUiTextColorForGraph(int widget, float r, float g, float b, float a)
    {
        try
        {
            if (!TryWidget(widget, out Widget w)) return false;
            static byte Channel(float v) => (byte)Math.Clamp((int)MathF.Round(v * 255f), 0, 255);
            w.TextColour = Colour.Rgb(Channel(r), Channel(g), Channel(b), Channel(a));
            return true;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }

    /// <summary>The size of the widget's text as laid out by the last frame. False (and zeroes) for a widget with no
    /// text, one not laid out yet, or a widget that no longer exists.</summary>
    internal static bool GetUiTextSizeForGraph(int widget, out float width, out float height, out int lines)
    {
        width = height = 0f;
        lines = 0;
        try
        {
            if (!TryWidget(widget, out Widget w)) return false;
            TextMetrics m = w.TextSize;
            if (m.Lines <= 0) return false;
            width = m.Width;
            height = m.Height;
            lines = m.Lines;
            return true;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }
}
