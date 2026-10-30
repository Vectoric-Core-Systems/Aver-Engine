// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Fonts (G5 U2): Font, FontFile, SystemFont and FontVariation, and UiText, the measuring the text controls share. A font is a FAMILY of the engine's font
// set (Content/UI/Fonts.ocfonts, docs/GAME_UI_TEXT.md): the converter writes one family per font file named after the file's stem, and a font here names that
// family; the engine rasterises TrueType at run time with fallback chains and kerning. Widths come from the engine (aver_ui_text_measure) when it has a face
// and from a fixed fallback (half an em per character, a line 1.2 em tall) when it has none, which is what headless runs and the tests see.

using System.Text;

namespace Aver.Fabric;

/// <summary>A font (Godot's Font): a family of the engine's font set plus a style.</summary>
public class Font : Resource
{
    /// <summary>The content path of the font file, empty for the project's default.</summary>
    public string FontPath { get => ResourcePath; set => ResourcePath = value; }

    public Font[] Fallbacks { get; set; } = System.Array.Empty<Font>();

    /// <summary>The family of the engine's font set this font draws with; empty is the set's default family. A font file's family is its file name without the
    /// extension (the name the converter gives the family in Fonts.ocfonts).</summary>
    internal virtual string Family => FamilyOfPath(ResourcePath);

    internal virtual bool IsBold => false;
    internal virtual bool IsItalic => false;

    internal static string FamilyOfPath(string path)
    {
        if (path.Length == 0) return "";
        string name = path.Replace('\\', '/');
        int slash = name.LastIndexOf('/');
        if (slash >= 0) name = name[(slash + 1)..];
        int dot = name.LastIndexOf('.');
        return dot > 0 ? name[..dot] : name;
    }

    /// <summary>The engine's text flags for this font's style (bold 1, italic 2).</summary>
    internal int StyleFlags => (IsBold ? 1 : 0) | (IsItalic ? 2 : 0);

    public virtual string GetFontName() => Family;

    public virtual string GetFontStyleName() => IsBold ? (IsItalic ? "Bold Italic" : "Bold") : IsItalic ? "Italic" : "Regular";

    /// <summary>Godot's TextServer.FontStyle flags: bold 1, italic 2, fixed width 4.</summary>
    public virtual long GetFontStyle() => (IsBold ? 1 : 0) | (IsItalic ? 2 : 0);

    public float GetHeight(int fontSize = 16) => UiText.LineHeight(this, fontSize);

    public float GetAscent(int fontSize = 16) => UiText.Ascent(this, fontSize);

    public float GetDescent(int fontSize = 16) => UiText.LineHeight(this, fontSize) - UiText.Ascent(this, fontSize);

    public float GetUnderlinePosition(int fontSize = 16) => UiText.Ascent(this, fontSize) + MathF.Max(1f, fontSize / 16f);

    public float GetUnderlineThickness(int fontSize = 16) => MathF.Max(1f, MathF.Round(fontSize / 16f));

    /// <summary>The size of one line of text. The alignment and width matter to justification only, which the engine does not do.</summary>
    public Vector2 GetStringSize(string text, HorizontalAlignment alignment = HorizontalAlignment.Left, float width = -1f, int fontSize = 16)
        => new(UiText.Width(this, fontSize, text), UiText.LineHeight(this, fontSize));

    /// <summary>The size of text broken at newlines and, when <paramref name="width"/> is positive, wrapped at words.</summary>
    public Vector2 GetMultilineStringSize(string text, HorizontalAlignment alignment = HorizontalAlignment.Left, float width = -1f, int fontSize = 16, int maxLines = -1)
    {
        TextLines lines = TextLines.Build(text, this, fontSize, width, TextLines.Wrap.Word);
        int n = maxLines > 0 ? Math.Min(maxLines, lines.Count) : lines.Count;
        float w = 0f;
        for (int i = 0; i < n; i++) w = MathF.Max(w, lines.Width(i));
        return new Vector2(w, n * lines.LineHeight);
    }

    public Vector2 GetCharSize(long @char, int fontSize) => new(UiText.Width(this, fontSize, char.ConvertFromUtf32((int)@char)), UiText.LineHeight(this, fontSize));

    public bool HasChar(long @char) => true;

    public string GetSupportedChars() => "";

    public bool IsLanguageSupported(string language) => true;

    public bool IsScriptSupported(string script) => true;
}

/// <summary>A font read from a file (Godot's FontFile). The file is registered as a family of the font set by the converter; the rendering options are stored
/// and, except where the docs say so, not acted on (the engine rasterises coverage glyphs: no hinting modes, no MSDF).</summary>
public class FontFile : Font
{
    public enum FontAntialiasing { None = 0, Gray = 1, Lcd = 2 }

    public enum FontHinting { None = 0, Light = 1, Normal = 2 }

    public string FontName { get; set; } = "";
    public string StyleName { get; set; } = "";
    public long FontStyle { get; set; }
    public int FixedSize { get; set; }
    public int FixedSizeScaleMode { get; set; }
    public FontAntialiasing Antialiasing { get; set; } = FontAntialiasing.Gray;
    public bool GenerateMipmaps { get; set; }
    public bool MultichannelSignedDistanceField { get; set; }
    public int MsdfPixelRange { get; set; } = 16;
    public int MsdfSize { get; set; } = 48;
    public bool AllowSystemFallback { get; set; } = true;
    public bool ForceAutohinter { get; set; }
    public FontHinting Hinting { get; set; } = FontHinting.Light;
    public int SubpixelPositioning { get; set; } = 1;
    public float Oversampling { get; set; }
    public Collections.Dictionary OpentypeFeatures { get; set; } = new();
    public byte[] Data { get; set; } = System.Array.Empty<byte>();

    internal override bool IsBold => (FontStyle & 1) != 0 || NameHas("bold");
    internal override bool IsItalic => (FontStyle & 2) != 0 || NameHas("italic") || NameHas("oblique");

    private bool NameHas(string word) => ResourcePath.Contains(word, StringComparison.OrdinalIgnoreCase);

    public override string GetFontName() => FontName.Length > 0 ? FontName : Family;

    /// <summary>Loads a font file as this font (Godot's load_dynamic_font): the path becomes the resource path.</summary>
    public Error LoadDynamicFont(string path)
    {
        ResourcePath = path;
        return Error.Ok;
    }
}

/// <summary>A font found by name among the operating system's fonts (Godot's SystemFont). The engine's font set reaches the OS fonts through its
/// <c>SYSTEM auto</c> entry, so the names are kept and the default family draws; a bold weight or italic asks that family for those faces.</summary>
public class SystemFont : Font
{
    public string[] FontNames { get; set; } = System.Array.Empty<string>();
    public bool FontItalic { get; set; }
    public int FontWeight { get; set; } = 400;
    public int FontStretch { get; set; } = 100;
    public FontFile.FontAntialiasing Antialiasing { get; set; } = FontFile.FontAntialiasing.Gray;
    public bool GenerateMipmaps { get; set; }
    public bool AllowSystemFallback { get; set; } = true;
    public bool ForceAutohinter { get; set; }
    public FontFile.FontHinting Hinting { get; set; } = FontFile.FontHinting.Light;
    public int SubpixelPositioning { get; set; } = 1;
    public float Oversampling { get; set; }
    public bool MultichannelSignedDistanceField { get; set; }
    public int MsdfPixelRange { get; set; } = 16;
    public int MsdfSize { get; set; } = 48;

    internal override string Family => "";
    internal override bool IsBold => FontWeight >= 600;
    internal override bool IsItalic => FontItalic;

    public override string GetFontName() => FontNames.Length > 0 ? FontNames[0] : "";
}

/// <summary>A font with variation settings on a base font (Godot's FontVariation). Embolden above zero asks for the base family's bold face (there is no
/// synthetic emboldening); the transform, OpenType variation axes and features are stored; glyph and space spacing are stored and not applied by the engine.</summary>
public class FontVariation : Font
{
    public Font? BaseFont { get; set; }
    public Collections.Dictionary VariationOpentype { get; set; } = new();
    public int VariationFaceIndex { get; set; }
    public float VariationEmbolden { get; set; }
    public Transform2D VariationTransform { get; set; } = Transform2D.Identity;
    public Collections.Dictionary OpentypeFeatures { get; set; } = new();
    public int SpacingGlyph { get; set; }
    public int SpacingSpace { get; set; }
    public int SpacingTop { get; set; }
    public int SpacingBottom { get; set; }
    public float BaselineOffset { get; set; }

    internal override string Family => BaseFont?.Family ?? "";
    internal override bool IsBold => VariationEmbolden > 0f || (BaseFont?.IsBold ?? false);
    internal override bool IsItalic => (BaseFont?.IsItalic ?? false) || MathF.Abs(VariationTransform.Y.X) > 0.05f;

    public override string GetFontName() => BaseFont?.GetFontName() ?? "";
}

/// <summary>Shared settings for a Label's font, size, colours, outline and shadow (Godot's LabelSettings).</summary>
public class LabelSettings : Resource
{
    public float LineSpacing { get; set; } = 3f;
    public float ParagraphSpacing { get; set; }
    public Font? Font { get; set; }
    public int FontSize { get; set; } = 16;
    public Color FontColor { get; set; } = new(1, 1, 1, 1);
    public int OutlineSize { get; set; }
    public Color OutlineColor { get; set; } = new(1, 1, 1, 1);
    public int ShadowSize { get; set; }
    public Color ShadowColor { get; set; } = new(0, 0, 0, 0);
    public Vector2 ShadowOffset { get; set; } = new(1, 1);
}

/// <summary>The measuring the text controls share: engine widths when the engine has a face, the fixed fallback otherwise.</summary>
internal static class UiText
{
    internal const float FallbackAdvance = 0.5f;
    internal const float FallbackLine = 1.2f;
    internal const float AscentShare = 0.8f;

    /// <summary>A test or host replacement for the width of a string: family, size, style flags, text. Null uses the engine.</summary>
    internal static Func<string, float, int, string, float>? WidthProvider;

    /// <summary>A replacement for the height of a line: family, size. Null uses the engine.</summary>
    internal static Func<string, float, float>? LineProvider;

    private static readonly Dictionary<(string, int, int, string), float> s_widths = new();
    private static readonly Dictionary<(string, int), float> s_lines = new();

    internal static void ResetForTests()
    {
        s_widths.Clear();
        s_lines.Clear();
        WidthProvider = null;
        LineProvider = null;
    }

    private static bool EngineMeasure(string text, string family, float px, int flags, out float w, out float h)
    {
        w = h = 0f;
        if (!UiNative.Available) return false;
        var whl = new float[3];
        try
        {
            if (UiNative.aver_ui_text_measure(text, family.Length > 0 ? family : null, px, flags, 0f, whl) == 0) return false;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { return false; }
        w = whl[0];
        h = whl[1];
        return true;
    }

    /// <summary>The width of one line of text.</summary>
    internal static float Width(Font? font, float px, string text)
    {
        if (text.Length == 0) return 0f;
        string family = font?.Family ?? "";
        int flags = font?.StyleFlags ?? 0;
        if (WidthProvider != null) return WidthProvider(family, px, flags, text);
        var key = (family, (int)MathF.Round(px * 4f), flags, text);
        if (s_widths.TryGetValue(key, out float known)) return known;
        float w = EngineMeasure(text, family, px, flags, out float ew, out _) ? ew : FallbackWidth(text, px);
        if (s_widths.Count > 8192) s_widths.Clear();
        s_widths[key] = w;
        return w;
    }

    /// <summary>Half an em per character, a tab as four.</summary>
    internal static float FallbackWidth(string text, float px)
    {
        float n = 0f;
        foreach (Rune r in text.EnumerateRunes()) n += r.Value == '\t' ? 4f : 1f;
        return n * px * FallbackAdvance;
    }

    internal static float LineHeight(Font? font, float px)
    {
        string family = font?.Family ?? "";
        if (LineProvider != null) return LineProvider(family, px);
        var key = (family, (int)MathF.Round(px * 4f));
        if (s_lines.TryGetValue(key, out float known)) return known;
        float h = EngineMeasure("A", family, px, 0, out _, out float eh) && eh > 0f ? eh : px * FallbackLine;
        s_lines[key] = h;
        return h;
    }

    internal static float Ascent(Font? font, float px) => LineHeight(font, px) * AscentShare;

    /// <summary>The Colors a Godot Color becomes in the UI draw list: straight 0xAABBGGRR.</summary>
    internal static uint Pack(Color c) => StyleBoxFlat.Pack(c);
}
