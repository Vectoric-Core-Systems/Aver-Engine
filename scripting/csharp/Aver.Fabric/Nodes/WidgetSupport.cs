// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// What the widgets of G5 U2 share: TextServer's enums (the names scripts reach as TextServer.AutowrapMode ...), Shortcut, the in-process clipboard, and the
// Control members the widgets read their look with (font, size, colours, a text line drawn with the theme's outline and shadow).

namespace Aver.Fabric;

/// <summary>Godot's TextServer, reduced to the enums the text controls name.</summary>
public static class TextServer
{
    public enum AutowrapMode { Off = 0, Arbitrary = 1, Word = 2, WordSmart = 3 }

    public enum OverrunBehavior { NoTrimming = 0, TrimChar = 1, TrimWord = 2, TrimEllipsis = 3, TrimWordEllipsis = 4, TrimEllipsisForce = 5, TrimWordEllipsisForce = 6 }

    public enum VisibleCharactersBehavior { CharsBeforeShaping = 0, CharsAfterShaping = 1, GlyphsAuto = 2, GlyphsLtr = 3, GlyphsRtl = 4 }

    [Flags]
    public enum JustificationFlag { None = 0, Kashida = 1, WordBound = 2, TrimEdgeSpaces = 4, AfterLastTab = 8, ConstrainEllipsis = 16, SkipLastLine = 32, SkipLastLineWithVisibleChars = 64, DoNotSkipSingleLine = 128 }

    public enum Direction { Auto = 0, Ltr = 1, Rtl = 2, Inherited = 3 }

    public enum Orientation { Horizontal = 0, Vertical = 1 }

    [Flags]
    public enum FontStyle { Bold = 1, Italic = 2, FixedWidth = 4 }

    internal static TextLines.Wrap WrapOf(AutowrapMode m) => m switch
    {
        AutowrapMode.Arbitrary => TextLines.Wrap.Arbitrary,
        AutowrapMode.Word => TextLines.Wrap.Word,
        AutowrapMode.WordSmart => TextLines.Wrap.Smart,
        _ => TextLines.Wrap.Off,
    };
}

/// <summary>A list of input events that trigger something (Godot's Shortcut).</summary>
public class Shortcut : Resource
{
    public Collections.Array Events { get; set; } = new();

    /// <summary>Whether the event is one of the shortcut's.</summary>
    public bool MatchesEvent(InputEvent @event)
    {
        foreach (Variant v in Events)
            if (v.AsGodotObject() is InputEvent e && e.IsMatch(@event, true)) return true;
        return false;
    }

    public bool HasValidEvent()
    {
        foreach (Variant v in Events) if (v.AsGodotObject() is InputEvent) return true;
        return false;
    }

    public string GetAsText()
    {
        foreach (Variant v in Events)
            if (v.AsGodotObject() is InputEvent e) return e.AsText();
        return "None";
    }
}

/// <summary>The text the editing controls copy and paste. The engine host has no clipboard seam yet, so it is a buffer shared by the controls of this process.</summary>
internal static class UiClipboard
{
    internal static string Text = "";
}

public partial class Control
{
    /// <summary>The control's rectangle in its own space.</summary>
    internal Rect2 LocalRect => new(Vector2.Zero, Size);

    internal Font? ThemeFont(string name = "font", string type = "") => GetThemeFont(name, type);

    internal int ThemeFontSize(string name = "font_size", string type = "") => Math.Max(1, GetThemeFontSize(name, type));

    /// <summary>The outline and shadow a text item of this control's class is drawn with, from the theme items every text class shares
    /// (font_outline_color, outline_size, font_shadow_color, shadow_offset_x / y, shadow_outline_size).</summary>
    internal CanvasItem.TextFx ThemeTextFx(string type = "")
    {
        CanvasItem.TextFx fx = new()
        {
            OutlineSize = HasThemeConstant("outline_size", type) ? GetThemeConstant("outline_size", type) : 0,
            OutlineColor = HasThemeColor("font_outline_color", type) ? GetThemeColor("font_outline_color", type) : new Color(0, 0, 0, 1),
            ShadowColor = HasThemeColor("font_shadow_color", type) ? GetThemeColor("font_shadow_color", type) : new Color(0, 0, 0, 0),
            ShadowOffset = new Vector2(GetThemeConstant("shadow_offset_x", type), GetThemeConstant("shadow_offset_y", type)),
            ShadowOutlineSize = GetThemeConstant("shadow_outline_size", type),
        };
        return fx;
    }

    /// <summary>The scene stores enum and flag values as integers.</summary>
    internal static T EnumOf<T>(int v) where T : struct, Enum => (T)Enum.ToObject(typeof(T), v);
}
