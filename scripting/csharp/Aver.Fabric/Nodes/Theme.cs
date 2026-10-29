// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Theme, ThemeDB, Font and the StyleBox family (G5 U1). A Theme is Godot's: items of six kinds per type, type variations, a default font and
// size. The lookup order of Control.get_theme_* is in Control.cs (ThemeLookup); the file form is .octheme (docs/formats/FORMAT_SPECS.md), written
// by the converter from C++ and read here. A style box draws through the engine's tessellator (Aver.UI.Abi, aver/ui/UiStyleBox.hpp), so
// the geometry exists once.

using System.Globalization;
using System.Text;

namespace Aver.Fabric;

/// <summary>Base of every style box (Godot's StyleBox): the panel, button and frame backgrounds Controls draw.</summary>
public abstract class StyleBox : Resource
{
    private float _cl = -1, _ct = -1, _cr = -1, _cb = -1;

    public float ContentMarginLeft { get => _cl; set { _cl = value; EmitChanged(); } }
    public float ContentMarginTop { get => _ct; set { _ct = value; EmitChanged(); } }
    public float ContentMarginRight { get => _cr; set { _cr = value; EmitChanged(); } }
    public float ContentMarginBottom { get => _cb; set { _cb = value; EmitChanged(); } }

    public void SetContentMargin(Side margin, float offset)
    {
        switch (margin)
        {
            case Side.Left: ContentMarginLeft = offset; break;
            case Side.Top: ContentMarginTop = offset; break;
            case Side.Right: ContentMarginRight = offset; break;
            default: ContentMarginBottom = offset; break;
        }
    }

    public void SetContentMarginAll(float offset) { _cl = _ct = _cr = _cb = offset; EmitChanged(); }

    /// <summary>The explicit content margin, or -1 when it is unset.</summary>
    public float GetContentMargin(Side margin) => margin switch { Side.Left => _cl, Side.Top => _ct, Side.Right => _cr, _ => _cb };

    /// <summary>The margin in force: the explicit one, else the kind's own (a flat box's border, a texture box's texture margin).</summary>
    public float GetMargin(Side margin)
    {
        float c = GetContentMargin(margin);
        return c >= 0 ? c : DefaultMargin(margin);
    }

    internal virtual float DefaultMargin(Side margin) => 0;

    public Vector2 GetMinimumSize() => new(GetMargin(Side.Left) + GetMargin(Side.Right), GetMargin(Side.Top) + GetMargin(Side.Bottom));

    /// <summary>The offset of the content from the box's top-left corner.</summary>
    public Vector2 GetOffset() => new(GetMargin(Side.Left), GetMargin(Side.Top));

    /// <summary>The rectangle inside the content margins.</summary>
    public Rect2 GetContentRect(Rect2 rect) => new(rect.Position + GetOffset(), (rect.Size - GetMinimumSize()).Max(Vector2.Zero));

    /// <summary>The rectangle the box is drawn over: <paramref name="rect"/> grown by the expand margins.</summary>
    public virtual Rect2 GetDrawRect(Rect2 rect) => rect;

    /// <summary>Whether a point of the box is solid (Godot's test_mask). Without a mask, the rectangle.</summary>
    public virtual bool TestMask(Vector2 point, Rect2 rect) => GetDrawRect(rect).HasPoint(point);

    internal abstract UiStyleBoxStruct ToNative();

    /// <summary>The explicit content margins into a native box.</summary>
    internal void FillContent(ref UiStyleBoxStruct s)
    {
        s.ContentL = _cl; s.ContentT = _ct; s.ContentR = _cr; s.ContentB = _cb;
    }

    /// <summary>Draws the box over <paramref name="rect"/> into the UI draw list, on the current layer (Godot's StyleBox.draw with the
    /// canvas item implied).</summary>
    public void Draw(Rect2 rect)
    {
        UiStyleBoxStruct s = ToNative();
        UiNative.DrawStyleBox(s, rect.Position.X, rect.Position.Y, rect.Size.X, rect.Size.Y);
    }

    /// <summary>The triangles the engine would draw for this box over a rectangle; null when the engine library is absent.</summary>
    public StyleBoxMesh? Tessellate(Rect2 rect)
    {
        UiStyleBoxStruct s = ToNative();
        return UiNative.Mesh(s, rect.Position.X, rect.Position.Y, rect.Size.X, rect.Size.Y);
    }

    /// <summary>A copy that shares nothing with this one.</summary>
    public StyleBox DuplicateBox() => (StyleBox)Duplicate();
}

/// <summary>A style box that draws nothing and holds content margins only (Godot's StyleBoxEmpty).</summary>
public class StyleBoxEmpty : StyleBox
{
    internal override UiStyleBoxStruct ToNative()
    {
        UiStyleBoxStruct s = UiNative.Available ? UiNative.Defaults(UiSbKind.Empty) : default;
        s.Kind = (int)UiSbKind.Empty;
        FillContent(ref s);
        return s;
    }
}

/// <summary>A rectangle with a background, borders, rounded corners, a shadow and optional skew (Godot's StyleBoxFlat).</summary>
public class StyleBoxFlat : StyleBox
{
    private Color _bg = new(0.6f, 0.6f, 0.6f, 1f);
    private Color _bl = new(0.8f, 0.8f, 0.8f, 1f), _bt = new(0.8f, 0.8f, 0.8f, 1f), _br = new(0.8f, 0.8f, 0.8f, 1f), _bb = new(0.8f, 0.8f, 0.8f, 1f);
    private int _wl, _wt, _wr, _wb;
    private int _rtl, _rtr, _rbr, _rbl;
    private float _el, _et, _er, _eb;
    private Color _shadow = new(0, 0, 0, 0.6f);
    private int _shadowSize;
    private Vector2 _shadowOffset = Vector2.Zero, _skew = Vector2.Zero;
    private int _detail = 8;
    private float _aaSize = 1f;

    public Color BgColor { get => _bg; set { _bg = value; EmitChanged(); } }
    public bool DrawCenter { get; set; } = true;
    public bool BorderBlend { get; set; }
    public bool AntiAliasing { get; set; } = true;
    public float AntiAliasingSize { get => _aaSize; set { _aaSize = Math.Max(0.01f, value); EmitChanged(); } }
    public int CornerDetail { get => _detail; set { _detail = Math.Clamp(value, 1, 20); EmitChanged(); } }
    public Vector2 Skew { get => _skew; set { _skew = value; EmitChanged(); } }

    /// <summary>Godot's one border colour: reading gives the left side's, writing sets all four (<see cref="BorderColorLeft"/> and the others
    /// are an Aver extension for per-side colours).</summary>
    public Color BorderColor { get => _bl; set { _bl = _bt = _br = _bb = value; EmitChanged(); } }
    public Color BorderColorLeft { get => _bl; set { _bl = value; EmitChanged(); } }
    public Color BorderColorTop { get => _bt; set { _bt = value; EmitChanged(); } }
    public Color BorderColorRight { get => _br; set { _br = value; EmitChanged(); } }
    public Color BorderColorBottom { get => _bb; set { _bb = value; EmitChanged(); } }

    public int BorderWidthLeft { get => _wl; set { _wl = Math.Max(0, value); EmitChanged(); } }
    public int BorderWidthTop { get => _wt; set { _wt = Math.Max(0, value); EmitChanged(); } }
    public int BorderWidthRight { get => _wr; set { _wr = Math.Max(0, value); EmitChanged(); } }
    public int BorderWidthBottom { get => _wb; set { _wb = Math.Max(0, value); EmitChanged(); } }

    public int CornerRadiusTopLeft { get => _rtl; set { _rtl = Math.Max(0, value); EmitChanged(); } }
    public int CornerRadiusTopRight { get => _rtr; set { _rtr = Math.Max(0, value); EmitChanged(); } }
    public int CornerRadiusBottomRight { get => _rbr; set { _rbr = Math.Max(0, value); EmitChanged(); } }
    public int CornerRadiusBottomLeft { get => _rbl; set { _rbl = Math.Max(0, value); EmitChanged(); } }

    public float ExpandMarginLeft { get => _el; set { _el = value; EmitChanged(); } }
    public float ExpandMarginTop { get => _et; set { _et = value; EmitChanged(); } }
    public float ExpandMarginRight { get => _er; set { _er = value; EmitChanged(); } }
    public float ExpandMarginBottom { get => _eb; set { _eb = value; EmitChanged(); } }

    public Color ShadowColor { get => _shadow; set { _shadow = value; EmitChanged(); } }
    public int ShadowSize { get => _shadowSize; set { _shadowSize = Math.Max(0, value); EmitChanged(); } }
    public Vector2 ShadowOffset { get => _shadowOffset; set { _shadowOffset = value; EmitChanged(); } }

    public void SetBorderWidthAll(int width) { _wl = _wt = _wr = _wb = Math.Max(0, width); EmitChanged(); }
    public int GetBorderWidthMin() => Math.Min(Math.Min(_wl, _wt), Math.Min(_wr, _wb));
    public void SetBorderWidth(Side margin, int width)
    {
        switch (margin)
        {
            case Side.Left: BorderWidthLeft = width; break;
            case Side.Top: BorderWidthTop = width; break;
            case Side.Right: BorderWidthRight = width; break;
            default: BorderWidthBottom = width; break;
        }
    }
    public int GetBorderWidth(Side margin) => margin switch { Side.Left => _wl, Side.Top => _wt, Side.Right => _wr, _ => _wb };

    public void SetCornerRadiusAll(int radius) { _rtl = _rtr = _rbr = _rbl = Math.Max(0, radius); EmitChanged(); }
    public void SetCornerRadiusIndividual(int topLeft, int topRight, int bottomRight, int bottomLeft)
    {
        _rtl = Math.Max(0, topLeft); _rtr = Math.Max(0, topRight); _rbr = Math.Max(0, bottomRight); _rbl = Math.Max(0, bottomLeft);
        EmitChanged();
    }
    public void SetCornerRadius(Corner corner, int radius)
    {
        switch (corner)
        {
            case Corner.TopLeft: CornerRadiusTopLeft = radius; break;
            case Corner.TopRight: CornerRadiusTopRight = radius; break;
            case Corner.BottomRight: CornerRadiusBottomRight = radius; break;
            default: CornerRadiusBottomLeft = radius; break;
        }
    }
    public int GetCornerRadius(Corner corner) => corner switch { Corner.TopLeft => _rtl, Corner.TopRight => _rtr, Corner.BottomRight => _rbr, _ => _rbl };

    public void SetExpandMarginAll(float size) { _el = _et = _er = _eb = size; EmitChanged(); }
    public void SetExpandMargin(Side margin, float size)
    {
        switch (margin)
        {
            case Side.Left: ExpandMarginLeft = size; break;
            case Side.Top: ExpandMarginTop = size; break;
            case Side.Right: ExpandMarginRight = size; break;
            default: ExpandMarginBottom = size; break;
        }
    }
    public float GetExpandMargin(Side margin) => margin switch { Side.Left => _el, Side.Top => _et, Side.Right => _er, _ => _eb };

    internal override float DefaultMargin(Side margin) => GetBorderWidth(margin);

    public override Rect2 GetDrawRect(Rect2 rect) => new(rect.Position - new Vector2(_el, _et), rect.Size + new Vector2(_el + _er, _et + _eb));

    internal override UiStyleBoxStruct ToNative()
    {
        UiStyleBoxStruct s = UiNative.Available ? UiNative.Defaults(UiSbKind.Flat) : default;
        s.Kind = (int)UiSbKind.Flat;
        FillContent(ref s);
        s.ExpandL = _el; s.ExpandT = _et; s.ExpandR = _er; s.ExpandB = _eb;
        s.BgColor = Pack(_bg);
        s.BorderColorL = Pack(_bl); s.BorderColorT = Pack(_bt); s.BorderColorR = Pack(_br); s.BorderColorB = Pack(_bb);
        s.BorderL = _wl; s.BorderT = _wt; s.BorderR = _wr; s.BorderB = _wb;
        s.RadiusTL = _rtl; s.RadiusTR = _rtr; s.RadiusBR = _rbr; s.RadiusBL = _rbl;
        s.CornerDetail = _detail;
        s.AaSize = _aaSize;
        s.SkewX = _skew.X; s.SkewY = _skew.Y;
        s.ShadowColor = Pack(_shadow); s.ShadowSize = _shadowSize; s.ShadowOffX = _shadowOffset.X; s.ShadowOffY = _shadowOffset.Y;
        uint flags = 0;
        if (DrawCenter) flags |= (uint)UiSbFlags.DrawCenter;
        if (AntiAliasing) flags |= (uint)UiSbFlags.AntiAlias;
        if (BorderBlend) flags |= (uint)UiSbFlags.BorderBlend;
        s.Flags = flags;
        return s;
    }

    internal static uint Pack(Color c) =>
        (uint)Q(c.R) | ((uint)Q(c.G) << 8) | ((uint)Q(c.B) << 16) | ((uint)Q(c.A) << 24);

    private static int Q(float v) => (int)Math.Clamp(MathF.Round(v * 255f), 0f, 255f);

    internal static Color Unpack(uint c) => new((c & 0xFF) / 255f, ((c >> 8) & 0xFF) / 255f, ((c >> 16) & 0xFF) / 255f, ((c >> 24) & 0xFF) / 255f);
}

/// <summary>A 9-patch (Godot's StyleBoxTexture): the texture's corners keep their size, its edges and middle stretch or tile.</summary>
public class StyleBoxTexture : StyleBox
{
    public enum AxisStretchMode { Stretch = 0, Tile = 1, TileFit = 2 }

    private float _ml, _mt, _mr, _mb, _el, _et, _er, _eb;

    public Texture2D? Texture { get; set; }
    public Rect2 RegionRect { get; set; }
    public Color ModulateColor { get; set; } = new(1, 1, 1, 1);
    public bool DrawCenter { get; set; } = true;
    public AxisStretchMode AxisStretchHorizontal { get; set; }
    public AxisStretchMode AxisStretchVertical { get; set; }

    public float TextureMarginLeft { get => _ml; set { _ml = value; EmitChanged(); } }
    public float TextureMarginTop { get => _mt; set { _mt = value; EmitChanged(); } }
    public float TextureMarginRight { get => _mr; set { _mr = value; EmitChanged(); } }
    public float TextureMarginBottom { get => _mb; set { _mb = value; EmitChanged(); } }
    public float ExpandMarginLeft { get => _el; set { _el = value; EmitChanged(); } }
    public float ExpandMarginTop { get => _et; set { _et = value; EmitChanged(); } }
    public float ExpandMarginRight { get => _er; set { _er = value; EmitChanged(); } }
    public float ExpandMarginBottom { get => _eb; set { _eb = value; EmitChanged(); } }

    public void SetTextureMarginAll(float size) { _ml = _mt = _mr = _mb = size; EmitChanged(); }
    public void SetTextureMargin(Side margin, float size)
    {
        switch (margin)
        {
            case Side.Left: TextureMarginLeft = size; break;
            case Side.Top: TextureMarginTop = size; break;
            case Side.Right: TextureMarginRight = size; break;
            default: TextureMarginBottom = size; break;
        }
    }
    public float GetTextureMargin(Side margin) => margin switch { Side.Left => _ml, Side.Top => _mt, Side.Right => _mr, _ => _mb };
    public void SetExpandMarginAll(float size) { _el = _et = _er = _eb = size; EmitChanged(); }
    public void SetExpandMargin(Side margin, float size)
    {
        switch (margin)
        {
            case Side.Left: ExpandMarginLeft = size; break;
            case Side.Top: ExpandMarginTop = size; break;
            case Side.Right: ExpandMarginRight = size; break;
            default: ExpandMarginBottom = size; break;
        }
    }
    public float GetExpandMargin(Side margin) => margin switch { Side.Left => _el, Side.Top => _et, Side.Right => _er, _ => _eb };

    internal override float DefaultMargin(Side margin) => GetTextureMargin(margin);

    public override Rect2 GetDrawRect(Rect2 rect) => new(rect.Position - new Vector2(_el, _et), rect.Size + new Vector2(_el + _er, _et + _eb));

    internal override UiStyleBoxStruct ToNative()
    {
        UiStyleBoxStruct s = UiNative.Available ? UiNative.Defaults(UiSbKind.Texture) : default;
        s.Kind = (int)UiSbKind.Texture;
        FillContent(ref s);
        s.ExpandL = _el; s.ExpandT = _et; s.ExpandR = _er; s.ExpandB = _eb;
        s.PatchL = _ml; s.PatchT = _mt; s.PatchR = _mr; s.PatchB = _mb;
        s.AxisH = (int)AxisStretchHorizontal; s.AxisV = (int)AxisStretchVertical;
        s.Modulate = StyleBoxFlat.Pack(ModulateColor);
        s.Flags = DrawCenter ? (uint)UiSbFlags.DrawCenter : 0u;
        Vector2 size = Texture?.GetSize() ?? Vector2.Zero;
        s.TexW = size.X; s.TexH = size.Y;
        if (RegionRect.Size.X > 0 && RegionRect.Size.Y > 0)
        {
            s.RegionX = RegionRect.Position.X; s.RegionY = RegionRect.Position.Y; s.RegionW = RegionRect.Size.X; s.RegionH = RegionRect.Size.Y;
        }
        s.Texture = Texture?.EngineHandle ?? 0;
        return s;
    }
}

/// <summary>A horizontal or vertical line (Godot's StyleBoxLine).</summary>
public class StyleBoxLine : StyleBox
{
    public Color Color { get; set; } = new(0.6f, 0.6f, 0.6f, 1f);
    public int Thickness { get; set; } = 1;
    public float GrowBegin { get; set; } = 1f;
    public float GrowEnd { get; set; } = 1f;
    public bool Vertical { get; set; }

    internal override UiStyleBoxStruct ToNative()
    {
        UiStyleBoxStruct s = UiNative.Available ? UiNative.Defaults(UiSbKind.Line) : default;
        s.Kind = (int)UiSbKind.Line;
        FillContent(ref s);
        s.LineColor = StyleBoxFlat.Pack(Color);
        s.Thickness = Thickness;
        s.GrowBegin = GrowBegin; s.GrowEnd = GrowEnd;
        s.Flags = Vertical ? (uint)UiSbFlags.Vertical : 0u;
        return s;
    }
}

/// <summary>A collection of UI items per type (Godot's Theme): colours, constants, fonts, font sizes, icons and style boxes, type variations
/// and a default font. Files are .octheme (the converter writes one per Godot theme resource).</summary>
public class Theme : Resource
{
    public enum DataType { Color = 0, Constant = 1, Font = 2, FontSize = 3, Icon = 4, Stylebox = 5 }

    private readonly Dictionary<string, Dictionary<string, Color>> _colors = new();
    private readonly Dictionary<string, Dictionary<string, int>> _constants = new();
    private readonly Dictionary<string, Dictionary<string, Font>> _fonts = new();
    private readonly Dictionary<string, Dictionary<string, int>> _fontSizes = new();
    private readonly Dictionary<string, Dictionary<string, Texture2D>> _icons = new();
    private readonly Dictionary<string, Dictionary<string, StyleBox>> _styleBoxes = new();
    private readonly Dictionary<string, string> _variations = new();
    private float _baseScale;
    private int _defaultFontSize = -1;
    private Font? _defaultFont;

    public float DefaultBaseScale { get => _baseScale; set { _baseScale = value; EmitChanged(); } }
    public Font? DefaultFont { get => _defaultFont; set { _defaultFont = value; EmitChanged(); } }
    public int DefaultFontSize { get => _defaultFontSize; set { _defaultFontSize = value; EmitChanged(); } }

    public bool HasDefaultBaseScale() => _baseScale > 0;
    public bool HasDefaultFont() => _defaultFont != null;
    public bool HasDefaultFontSize() => _defaultFontSize >= 0;

    // ---- the six item kinds share their bookkeeping ----

    private static Dictionary<string, T> Of<T>(Dictionary<string, Dictionary<string, T>> all, string type)
    {
        if (!all.TryGetValue(type, out Dictionary<string, T>? d)) all[type] = d = new Dictionary<string, T>();
        return d;
    }

    private static bool Has<T>(Dictionary<string, Dictionary<string, T>> all, string name, string type) => all.TryGetValue(type, out var d) && d.ContainsKey(name);

    private static bool Clear<T>(Dictionary<string, Dictionary<string, T>> all, string name, string type) => all.TryGetValue(type, out var d) && d.Remove(name);

    private static bool Rename<T>(Dictionary<string, Dictionary<string, T>> all, string old, string @new, string type)
    {
        if (!all.TryGetValue(type, out var d) || !d.TryGetValue(old, out T? v) || d.ContainsKey(@new)) return false;
        d.Remove(old);
        d[@new] = v;
        return true;
    }

    private static string[] List<T>(Dictionary<string, Dictionary<string, T>> all, string type) => all.TryGetValue(type, out var d) ? d.Keys.ToArray() : System.Array.Empty<string>();

    private void Touch() => EmitChanged();

    // ---- colours ----
    public void SetColor(StringName name, StringName themeType, Color color) { Of(_colors, themeType.ToString())[name.ToString()] = color; Touch(); }
    public Color GetColor(StringName name, StringName themeType) => _colors.TryGetValue(themeType.ToString(), out var d) && d.TryGetValue(name.ToString(), out Color c) ? c : new Color(0, 0, 0, 1);
    public bool HasColor(StringName name, StringName themeType) => Has(_colors, name.ToString(), themeType.ToString());
    public void ClearColor(StringName name, StringName themeType) { if (Clear(_colors, name.ToString(), themeType.ToString())) Touch(); }
    public void RenameColor(StringName oldName, StringName name, StringName themeType) { if (Rename(_colors, oldName.ToString(), name.ToString(), themeType.ToString())) Touch(); }
    public string[] GetColorList(StringName themeType) => List(_colors, themeType.ToString());
    public string[] GetColorTypeList() => _colors.Keys.ToArray();

    // ---- constants ----
    public void SetConstant(StringName name, StringName themeType, int constant) { Of(_constants, themeType.ToString())[name.ToString()] = constant; Touch(); }
    public int GetConstant(StringName name, StringName themeType) => _constants.TryGetValue(themeType.ToString(), out var d) && d.TryGetValue(name.ToString(), out int c) ? c : 0;
    public bool HasConstant(StringName name, StringName themeType) => Has(_constants, name.ToString(), themeType.ToString());
    public void ClearConstant(StringName name, StringName themeType) { if (Clear(_constants, name.ToString(), themeType.ToString())) Touch(); }
    public void RenameConstant(StringName oldName, StringName name, StringName themeType) { if (Rename(_constants, oldName.ToString(), name.ToString(), themeType.ToString())) Touch(); }
    public string[] GetConstantList(StringName themeType) => List(_constants, themeType.ToString());
    public string[] GetConstantTypeList() => _constants.Keys.ToArray();

    // ---- fonts ----
    public void SetFont(StringName name, StringName themeType, Font font) { Of(_fonts, themeType.ToString())[name.ToString()] = font; Touch(); }
    public Font? GetFont(StringName name, StringName themeType) => _fonts.TryGetValue(themeType.ToString(), out var d) && d.TryGetValue(name.ToString(), out Font? f) ? f : _defaultFont;
    public bool HasFont(StringName name, StringName themeType) => Has(_fonts, name.ToString(), themeType.ToString());
    public void ClearFont(StringName name, StringName themeType) { if (Clear(_fonts, name.ToString(), themeType.ToString())) Touch(); }
    public void RenameFont(StringName oldName, StringName name, StringName themeType) { if (Rename(_fonts, oldName.ToString(), name.ToString(), themeType.ToString())) Touch(); }
    public string[] GetFontList(StringName themeType) => List(_fonts, themeType.ToString());
    public string[] GetFontTypeList() => _fonts.Keys.ToArray();

    // ---- font sizes ----
    public void SetFontSize(StringName name, StringName themeType, int fontSize) { Of(_fontSizes, themeType.ToString())[name.ToString()] = fontSize; Touch(); }
    public int GetFontSize(StringName name, StringName themeType) => _fontSizes.TryGetValue(themeType.ToString(), out var d) && d.TryGetValue(name.ToString(), out int s) ? s : _defaultFontSize;
    public bool HasFontSize(StringName name, StringName themeType) => Has(_fontSizes, name.ToString(), themeType.ToString());
    public void ClearFontSize(StringName name, StringName themeType) { if (Clear(_fontSizes, name.ToString(), themeType.ToString())) Touch(); }
    public void RenameFontSize(StringName oldName, StringName name, StringName themeType) { if (Rename(_fontSizes, oldName.ToString(), name.ToString(), themeType.ToString())) Touch(); }
    public string[] GetFontSizeList(StringName themeType) => List(_fontSizes, themeType.ToString());
    public string[] GetFontSizeTypeList() => _fontSizes.Keys.ToArray();

    // ---- icons ----
    public void SetIcon(StringName name, StringName themeType, Texture2D texture) { Of(_icons, themeType.ToString())[name.ToString()] = texture; Touch(); }
    public Texture2D? GetIcon(StringName name, StringName themeType) => _icons.TryGetValue(themeType.ToString(), out var d) && d.TryGetValue(name.ToString(), out Texture2D? t) ? t : null;
    public bool HasIcon(StringName name, StringName themeType) => Has(_icons, name.ToString(), themeType.ToString());
    public void ClearIcon(StringName name, StringName themeType) { if (Clear(_icons, name.ToString(), themeType.ToString())) Touch(); }
    public void RenameIcon(StringName oldName, StringName name, StringName themeType) { if (Rename(_icons, oldName.ToString(), name.ToString(), themeType.ToString())) Touch(); }
    public string[] GetIconList(StringName themeType) => List(_icons, themeType.ToString());
    public string[] GetIconTypeList() => _icons.Keys.ToArray();

    // ---- style boxes ----
    public void SetStylebox(StringName name, StringName themeType, StyleBox texture)
    {
        Dictionary<string, StyleBox> items = Of(_styleBoxes, themeType.ToString());
        items.TryGetValue(name.ToString(), out StyleBox? old);
        items[name.ToString()] = texture;
        if (old != null && old != texture && !_styleBoxes.Values.Any(d => d.ContainsValue(old)) && old.IsConnected(Resource.SignalName.Changed, Callable.From(Touch))) old.Changed -= Touch;
        if (!texture.IsConnected(Resource.SignalName.Changed, Callable.From(Touch))) texture.Changed += Touch;   // a style box that changes tells the theme, as in Godot
        Touch();
    }
    public StyleBox? GetStylebox(StringName name, StringName themeType) => _styleBoxes.TryGetValue(themeType.ToString(), out var d) && d.TryGetValue(name.ToString(), out StyleBox? s) ? s : null;
    public bool HasStylebox(StringName name, StringName themeType) => Has(_styleBoxes, name.ToString(), themeType.ToString());
    public void ClearStylebox(StringName name, StringName themeType) { if (Clear(_styleBoxes, name.ToString(), themeType.ToString())) Touch(); }
    public void RenameStylebox(StringName oldName, StringName name, StringName themeType) { if (Rename(_styleBoxes, oldName.ToString(), name.ToString(), themeType.ToString())) Touch(); }
    public string[] GetStyleboxList(StringName themeType) => List(_styleBoxes, themeType.ToString());
    public string[] GetStyleboxTypeList() => _styleBoxes.Keys.ToArray();

    // ---- by data type ----
    public void SetThemeItem(DataType dataType, StringName name, StringName themeType, Variant value)
    {
        switch (dataType)
        {
            case DataType.Color: SetColor(name, themeType, value.AsColor()); break;
            case DataType.Constant: SetConstant(name, themeType, (int)value.AsInt64()); break;
            case DataType.Font: if (value.AsGodotObject() is Font f) SetFont(name, themeType, f); break;
            case DataType.FontSize: SetFontSize(name, themeType, (int)value.AsInt64()); break;
            case DataType.Icon: if (value.AsGodotObject() is Texture2D t) SetIcon(name, themeType, t); break;
            default: if (value.AsGodotObject() is StyleBox s) SetStylebox(name, themeType, s); break;
        }
    }

    public Variant GetThemeItem(DataType dataType, StringName name, StringName themeType) => dataType switch
    {
        DataType.Color => GetColor(name, themeType),
        DataType.Constant => GetConstant(name, themeType),
        DataType.Font => Variant.CreateFrom((GodotObject?)GetFont(name, themeType)),
        DataType.FontSize => GetFontSize(name, themeType),
        DataType.Icon => Variant.CreateFrom((GodotObject?)GetIcon(name, themeType)),
        _ => Variant.CreateFrom((GodotObject?)GetStylebox(name, themeType)),
    };

    public bool HasThemeItem(DataType dataType, StringName name, StringName themeType) => dataType switch
    {
        DataType.Color => HasColor(name, themeType),
        DataType.Constant => HasConstant(name, themeType),
        DataType.Font => HasFont(name, themeType),
        DataType.FontSize => HasFontSize(name, themeType),
        DataType.Icon => HasIcon(name, themeType),
        _ => HasStylebox(name, themeType),
    };

    public void ClearThemeItem(DataType dataType, StringName name, StringName themeType)
    {
        switch (dataType)
        {
            case DataType.Color: ClearColor(name, themeType); break;
            case DataType.Constant: ClearConstant(name, themeType); break;
            case DataType.Font: ClearFont(name, themeType); break;
            case DataType.FontSize: ClearFontSize(name, themeType); break;
            case DataType.Icon: ClearIcon(name, themeType); break;
            default: ClearStylebox(name, themeType); break;
        }
    }

    public string[] GetThemeItemList(DataType dataType, StringName themeType) => dataType switch
    {
        DataType.Color => GetColorList(themeType),
        DataType.Constant => GetConstantList(themeType),
        DataType.Font => GetFontList(themeType),
        DataType.FontSize => GetFontSizeList(themeType),
        DataType.Icon => GetIconList(themeType),
        _ => GetStyleboxList(themeType),
    };

    // ---- types and variations ----
    public void AddType(StringName themeType)
    {
        string t = themeType.ToString();
        Of(_colors, t); Of(_constants, t); Of(_fonts, t); Of(_fontSizes, t); Of(_icons, t); Of(_styleBoxes, t);
    }

    public void RemoveType(StringName themeType)
    {
        string t = themeType.ToString();
        _colors.Remove(t); _constants.Remove(t); _fonts.Remove(t); _fontSizes.Remove(t); _icons.Remove(t); _styleBoxes.Remove(t); _variations.Remove(t);
        Touch();
    }

    public string[] GetTypeList()
    {
        var all = new HashSet<string>();
        foreach (string k in _colors.Keys) all.Add(k);
        foreach (string k in _constants.Keys) all.Add(k);
        foreach (string k in _fonts.Keys) all.Add(k);
        foreach (string k in _fontSizes.Keys) all.Add(k);
        foreach (string k in _icons.Keys) all.Add(k);
        foreach (string k in _styleBoxes.Keys) all.Add(k);
        return all.ToArray();
    }

    public void SetTypeVariation(StringName themeType, StringName baseType)
    {
        if (themeType.ToString().Length == 0 || baseType.ToString().Length == 0) return;
        _variations[themeType.ToString()] = baseType.ToString();
        Touch();
    }

    public bool IsTypeVariation(StringName themeType, StringName baseType) => _variations.TryGetValue(themeType.ToString(), out string? b) && b == baseType.ToString();

    public void ClearTypeVariation(StringName themeType) { if (_variations.Remove(themeType.ToString())) Touch(); }

    public StringName GetTypeVariationBase(StringName themeType) => _variations.TryGetValue(themeType.ToString(), out string? b) ? b : "";

    public string[] GetTypeVariationList(StringName baseType) => _variations.Where(kv => kv.Value == baseType.ToString()).Select(kv => kv.Key).ToArray();

    internal string? VariationBase(string type) => _variations.TryGetValue(type, out string? b) ? b : null;

    // ---- whole themes ----

    /// <summary>Copies another theme's items over this one's (its default font and size too when it has them).</summary>
    public void MergeWith(Theme other)
    {
        foreach (var (t, d) in other._colors) foreach (var (k, v) in d) Of(_colors, t)[k] = v;
        foreach (var (t, d) in other._constants) foreach (var (k, v) in d) Of(_constants, t)[k] = v;
        foreach (var (t, d) in other._fonts) foreach (var (k, v) in d) Of(_fonts, t)[k] = v;
        foreach (var (t, d) in other._fontSizes) foreach (var (k, v) in d) Of(_fontSizes, t)[k] = v;
        foreach (var (t, d) in other._icons) foreach (var (k, v) in d) Of(_icons, t)[k] = v;
        foreach (var (t, d) in other._styleBoxes) foreach (var (k, v) in d) Of(_styleBoxes, t)[k] = v;
        foreach (var (k, v) in other._variations) _variations[k] = v;
        if (other._baseScale > 0) _baseScale = other._baseScale;
        if (other._defaultFont != null) _defaultFont = other._defaultFont;
        if (other._defaultFontSize >= 0) _defaultFontSize = other._defaultFontSize;
        Touch();
    }

    public void Clear()
    {
        _colors.Clear(); _constants.Clear(); _fonts.Clear(); _fontSizes.Clear(); _icons.Clear(); _styleBoxes.Clear(); _variations.Clear();
        _baseScale = 0; _defaultFont = null; _defaultFontSize = -1;
        Touch();
    }

    // Lookups for the Control machinery: the item as stored, no fallbacks.
    internal bool TryColor(string type, string name, out Color v) { v = default; return _colors.TryGetValue(type, out var d) && d.TryGetValue(name, out v); }
    internal bool TryConstant(string type, string name, out int v) { v = 0; return _constants.TryGetValue(type, out var d) && d.TryGetValue(name, out v); }
    internal bool TryFont(string type, string name, out Font? v) { v = null; return _fonts.TryGetValue(type, out var d) && d.TryGetValue(name, out v); }
    internal bool TryFontSize(string type, string name, out int v) { v = 0; return _fontSizes.TryGetValue(type, out var d) && d.TryGetValue(name, out v); }
    internal bool TryIcon(string type, string name, out Texture2D? v) { v = null; return _icons.TryGetValue(type, out var d) && d.TryGetValue(name, out v); }
    internal bool TryStyleBox(string type, string name, out StyleBox? v) { v = null; return _styleBoxes.TryGetValue(type, out var d) && d.TryGetValue(name, out v); }

    // ---- the .octheme file ----

    /// <summary>Reads an .octheme document (aver/ui/UiThemeSheet.hpp). False with the reason when it is malformed; the theme is then unchanged.</summary>
    public bool LoadFromText(string text, out string error)
    {
        var parsed = new Theme();
        if (!OcTheme.Parse(text, parsed, out error)) return false;
        Clear();
        MergeWith(parsed);
        ResourceName = parsed.ResourceName;
        return true;
    }

    internal bool LoadFrom(string file)
    {
        try { return LoadFromText(File.ReadAllText(file), out string err) || Fail(file, err); }
        catch (IOException) { return false; }
    }

    private static bool Fail(string file, string err)
    {
        Aver.Scripting.Log.Warn($"[Fabric] cannot read the theme '{file}': {err}");
        return false;
    }
}

/// <summary>The engine-wide themes (Godot's ThemeDB): the project theme from the project's UI.THEME key and the built-in default.</summary>
public static class ThemeDB
{
    private static Theme? s_default;
    private static Theme? s_project;
    private static bool s_projectLoaded;

    /// <summary>The theme every lookup ends in: the metrics of Godot's built-in theme (from the engine's default text; empty and with default font
    /// size 16 when the engine library is absent).</summary>
    public static Theme GetDefaultTheme()
    {
        if (s_default != null) return s_default;
        var t = new Theme();
        string text = UiNative.DefaultThemeText();
        if (text.Length == 0 || !OcTheme.Parse(text, t, out _))
        {
            t.Clear();
            t.DefaultFontSize = 16;
            t.DefaultBaseScale = 1f;
        }
        return s_default = t;
    }

    /// <summary>The project's theme (the Godot setting gui/theme/custom, the project key UI.THEME), or null when it names none.</summary>
    public static Theme? GetProjectTheme()
    {
        if (s_projectLoaded) return s_project;
        s_projectLoaded = true;
        string path = ProjectSettings.GetSetting("gui/theme/custom", "").AsString();
        if (path.Length > 0) s_project = ResourceLoader.Load(path, typeof(Theme)) as Theme;
        return s_project;
    }

    /// <summary>Sets the project theme (a script or the host).</summary>
    public static void SetProjectTheme(Theme? theme) { s_project = theme; s_projectLoaded = true; Control.ThemeChangedEverywhere(); }

    /// <summary>The fallback font size when no theme sets one.</summary>
    public static int FallbackFontSize => 16;

    internal static void ResetForTests()
    {
        s_default = null;
        s_project = null;
        s_projectLoaded = false;
    }
}

/// <summary>Reads the .octheme text form (the reader of aver/ui/UiThemeSheet.hpp's writer).</summary>
internal static class OcTheme
{
    public static bool Parse(string text, Theme into, out string error)
    {
        error = "";
        bool header = false;
        int lineNo = 0;
        var variations = new List<(string Name, string Base)>();
        foreach (string raw in text.Split('\n'))
        {
            lineNo++;
            List<string> t = Tokens(raw);
            if (t.Count == 0 || (t[0].Length > 0 && t[0][0] == '#')) continue;
            string rec = t[0];
            string Fail(string why) => $"line {lineNo}: {why}";
            if (!header)
            {
                if (rec != "OCTHEME" || t.Count < 2 || !int.TryParse(t[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int ver) || ver < 1)
                {
                    error = Fail("expected 'OCTHEME <version>'");
                    return false;
                }
                header = true;
                continue;
            }
            switch (rec)
            {
                case "NAME": if (t.Count >= 2) into.ResourceName = t[1]; break;
                case "DEFAULTFONT":
                    if (t.Count >= 2 && t[1].Length > 0) into.DefaultFont = new Font { ResourcePath = t[1] };
                    break;
                case "DEFAULTFONTSIZE":
                    if (t.Count < 2 || !int.TryParse(t[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int dfs)) { error = Fail("bad DEFAULTFONTSIZE"); return false; }
                    into.DefaultFontSize = dfs;
                    break;
                case "BASESCALE":
                    if (t.Count < 2 || !Float(t[1], out float bs)) { error = Fail("bad BASESCALE"); return false; }
                    into.DefaultBaseScale = bs;
                    break;
                case "VARIATION":
                    if (t.Count < 3) { error = Fail("VARIATION needs a name and a base"); return false; }
                    variations.Add((t[1], t[2]));
                    break;
                case "COLOR":
                    if (t.Count < 4 || !HexColor(t[3], out Color c)) { error = Fail("bad COLOR"); return false; }
                    into.SetColor(t[2], t[1], c);
                    break;
                case "CONSTANT":
                    if (t.Count < 4 || !int.TryParse(t[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out int k)) { error = Fail("bad CONSTANT"); return false; }
                    into.SetConstant(t[2], t[1], k);
                    break;
                case "FONT":
                    if (t.Count < 4) { error = Fail("bad FONT"); return false; }
                    into.SetFont(t[2], t[1], new Font { ResourcePath = t[3] });
                    break;
                case "FONTSIZE":
                    if (t.Count < 4 || !int.TryParse(t[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out int fs)) { error = Fail("bad FONTSIZE"); return false; }
                    into.SetFontSize(t[2], t[1], fs);
                    break;
                case "ICON":
                    if (t.Count < 4) { error = Fail("bad ICON"); return false; }
                    into.SetIcon(t[2], t[1], t[3].Length == 0 ? new Texture2D() : (ResourceLoader.Load(t[3], typeof(Texture2D)) as Texture2D ?? new Texture2D()));
                    break;
                case "STYLEBOX":
                    if (t.Count < 4) { error = Fail("bad STYLEBOX"); return false; }
                    StyleBox? sb = StyleBoxFrom(t, out string why);
                    if (sb == null) { error = Fail(why); return false; }
                    into.SetStylebox(t[2], t[1], sb);
                    break;
                // unknown records are skipped: a file written by a newer build still loads
            }
        }
        if (!header) { error = "line 1: empty document"; return false; }
        foreach (var (name, @base) in variations) into.SetTypeVariation(name, @base);
        return true;
    }

    private static StyleBox? StyleBoxFrom(List<string> t, out string why)
    {
        why = "";
        string kind = t[3];
        var kv = new Dictionary<string, string>();
        for (int i = 4; i < t.Count; i++)
        {
            int eq = t[i].IndexOf('=');
            if (eq < 0) { why = $"expected key=value, got '{t[i]}'"; return null; }
            kv[t[i][..eq]] = t[i][(eq + 1)..];
        }
        float[] f;
        StyleBox box;
        switch (kind)
        {
            case "empty": box = new StyleBoxEmpty(); break;
            case "flat":
            {
                var s = new StyleBoxFlat { BorderBlend = false, AntiAliasing = false };
                uint flags = (uint)UiSbFlags.DrawCenter;
                if (kv.TryGetValue("flags", out string? fl) && uint.TryParse(fl, out uint fv)) flags = fv;
                s.DrawCenter = (flags & (uint)UiSbFlags.DrawCenter) != 0;
                s.AntiAliasing = (flags & (uint)UiSbFlags.AntiAlias) != 0;
                s.BorderBlend = (flags & (uint)UiSbFlags.BorderBlend) != 0;
                if (!Opt(kv, "bg", v => HexColor(v, out Color c) ? (Color?)c : null, c => s.BgColor = c, ref why)) return null;
                if (!Opt(kv, "border_color", v => HexColor(v, out Color c) ? (Color?)c : null, c => s.BorderColor = c, ref why)) return null;
                if (kv.TryGetValue("border_colors", out string? bcs))
                {
                    string[] p = bcs.Split(',');
                    if (p.Length != 4 || !HexColor(p[0], out Color a) || !HexColor(p[1], out Color b) || !HexColor(p[2], out Color c2) || !HexColor(p[3], out Color d)) { why = "bad value for 'border_colors'"; return null; }
                    s.BorderColorLeft = a; s.BorderColorTop = b; s.BorderColorRight = c2; s.BorderColorBottom = d;
                }
                if (kv.TryGetValue("border", out string? bw)) { if (!Floats(bw, 4, out f)) { why = "bad value for 'border'"; return null; } s.BorderWidthLeft = (int)f[0]; s.BorderWidthTop = (int)f[1]; s.BorderWidthRight = (int)f[2]; s.BorderWidthBottom = (int)f[3]; }
                if (kv.TryGetValue("radius", out string? rd)) { if (!Floats(rd, 4, out f)) { why = "bad value for 'radius'"; return null; } s.SetCornerRadiusIndividual((int)f[0], (int)f[1], (int)f[2], (int)f[3]); }
                if (kv.TryGetValue("detail", out string? dt) && int.TryParse(dt, out int det)) s.CornerDetail = det;
                if (kv.TryGetValue("aa_size", out string? aa) && Float(aa, out float aav)) s.AntiAliasingSize = aav;
                if (kv.TryGetValue("skew", out string? sk)) { if (!Floats(sk, 2, out f)) { why = "bad value for 'skew'"; return null; } s.Skew = new Vector2(f[0], f[1]); }
                if (kv.TryGetValue("shadow_color", out string? sc)) { if (!HexColor(sc, out Color shc)) { why = "bad value for 'shadow_color'"; return null; } s.ShadowColor = shc; }
                if (kv.TryGetValue("shadow_size", out string? ss) && Float(ss, out float ssv)) s.ShadowSize = (int)ssv;
                if (kv.TryGetValue("shadow_offset", out string? so)) { if (!Floats(so, 2, out f)) { why = "bad value for 'shadow_offset'"; return null; } s.ShadowOffset = new Vector2(f[0], f[1]); }
                if (kv.TryGetValue("expand", out string? ex)) { if (!Floats(ex, 4, out f)) { why = "bad value for 'expand'"; return null; } s.ExpandMarginLeft = f[0]; s.ExpandMarginTop = f[1]; s.ExpandMarginRight = f[2]; s.ExpandMarginBottom = f[3]; }
                box = s;
                break;
            }
            case "texture":
            {
                var s = new StyleBoxTexture();
                uint flags = (uint)UiSbFlags.DrawCenter;
                if (kv.TryGetValue("flags", out string? fl) && uint.TryParse(fl, out uint fv)) flags = fv;
                s.DrawCenter = (flags & (uint)UiSbFlags.DrawCenter) != 0;
                if (kv.TryGetValue("tex", out string? tex) && tex.Length > 0) s.Texture = ResourceLoader.Load(tex, typeof(Texture2D)) as Texture2D;
                if (kv.TryGetValue("region", out string? rg)) { if (!Floats(rg, 4, out f)) { why = "bad value for 'region'"; return null; } s.RegionRect = new Rect2(f[0], f[1], f[2], f[3]); }
                if (kv.TryGetValue("patch", out string? pt)) { if (!Floats(pt, 4, out f)) { why = "bad value for 'patch'"; return null; } s.TextureMarginLeft = f[0]; s.TextureMarginTop = f[1]; s.TextureMarginRight = f[2]; s.TextureMarginBottom = f[3]; }
                if (kv.TryGetValue("axis_h", out string? ah)) s.AxisStretchHorizontal = Axis(ah);
                if (kv.TryGetValue("axis_v", out string? av)) s.AxisStretchVertical = Axis(av);
                if (kv.TryGetValue("modulate", out string? md)) { if (!HexColor(md, out Color mc)) { why = "bad value for 'modulate'"; return null; } s.ModulateColor = mc; }
                if (kv.TryGetValue("expand", out string? ex)) { if (!Floats(ex, 4, out f)) { why = "bad value for 'expand'"; return null; } s.ExpandMarginLeft = f[0]; s.ExpandMarginTop = f[1]; s.ExpandMarginRight = f[2]; s.ExpandMarginBottom = f[3]; }
                box = s;
                break;
            }
            case "line":
            {
                var s = new StyleBoxLine();
                uint flags = 0;
                if (kv.TryGetValue("flags", out string? fl) && uint.TryParse(fl, out uint fv)) flags = fv;
                s.Vertical = (flags & (uint)UiSbFlags.Vertical) != 0;
                if (kv.TryGetValue("color", out string? lc)) { if (!HexColor(lc, out Color col)) { why = "bad value for 'color'"; return null; } s.Color = col; }
                if (kv.TryGetValue("thickness", out string? th) && Float(th, out float thv)) s.Thickness = (int)thv;
                if (kv.TryGetValue("grow", out string? gr)) { if (!Floats(gr, 2, out f)) { why = "bad value for 'grow'"; return null; } s.GrowBegin = f[0]; s.GrowEnd = f[1]; }
                box = s;
                break;
            }
            default: why = $"unknown style box kind '{kind}'"; return null;
        }
        if (kv.TryGetValue("content", out string? ct))
        {
            if (!Floats(ct, 4, out f)) { why = "bad value for 'content'"; return null; }
            box.ContentMarginLeft = f[0]; box.ContentMarginTop = f[1]; box.ContentMarginRight = f[2]; box.ContentMarginBottom = f[3];
        }
        return box;
    }

    private static bool Opt(Dictionary<string, string> kv, string key, Func<string, Color?> parse, Action<Color> set, ref string why)
    {
        if (!kv.TryGetValue(key, out string? v)) return true;
        Color? c = parse(v);
        if (c == null) { why = $"bad value for '{key}'"; return false; }
        set(c.Value);
        return true;
    }

    private static StyleBoxTexture.AxisStretchMode Axis(string s) => s == "tile" ? StyleBoxTexture.AxisStretchMode.Tile : s == "tile_fit" ? StyleBoxTexture.AxisStretchMode.TileFit : StyleBoxTexture.AxisStretchMode.Stretch;

    private static bool Float(string s, out float v) => float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v);

    private static bool Floats(string s, int n, out float[] v)
    {
        string[] p = s.Split(',');
        v = new float[n];
        if (p.Length != n) return false;
        for (int i = 0; i < n; i++) if (!Float(p[i], out v[i])) return false;
        return true;
    }

    // #RRGGBBAA
    internal static bool HexColor(string s, out Color c)
    {
        c = default;
        if (s.Length != 9 || s[0] != '#') return false;
        uint[] b = new uint[4];
        for (int i = 0; i < 4; i++) if (!uint.TryParse(s.AsSpan(1 + i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out b[i])) return false;
        c = new Color(b[0] / 255f, b[1] / 255f, b[2] / 255f, b[3] / 255f);
        return true;
    }

    // Whitespace-separated, quotes group (and are removed anywhere in a token), '#' at the start of a line is a comment.
    private static List<string> Tokens(string line)
    {
        var outList = new List<string>();
        var cur = new StringBuilder();
        bool any = false, quoted = false;
        foreach (char ch in line)
        {
            if (ch == '"') { quoted = !quoted; any = true; continue; }
            if (!quoted && (ch == ' ' || ch == '\t' || ch == '\r'))
            {
                if (any) { outList.Add(cur.ToString()); cur.Clear(); any = false; }
                continue;
            }
            cur.Append(ch);
            any = true;
        }
        if (any) outList.Add(cur.ToString());
        return outList;
    }
}
