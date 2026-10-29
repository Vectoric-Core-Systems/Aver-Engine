// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Label (G5 U2): Godot's text label. Lines are broken here (TextLines) and drawn one by one, so what the minimum size counts is what is drawn: autowrap,
// the overrun behaviours (trim to a character or a word, with or without an ellipsis), max_lines_visible, lines_skipped, visible_characters /
// visible_ratio, uppercase, horizontal and vertical alignment, LabelSettings, and the theme's outline and shadow.

namespace Aver.Fabric;

/// <summary>Text on the screen (Godot's Label).</summary>
public class Label : Control
{
    private string _text = "";
    private LabelSettings? _settings;
    private HorizontalAlignment _halign = HorizontalAlignment.Left;
    private VerticalAlignment _valign = VerticalAlignment.Top;
    private TextServer.AutowrapMode _wrap = TextServer.AutowrapMode.Off;
    private TextServer.OverrunBehavior _overrun = TextServer.OverrunBehavior.NoTrimming;
    private string _ellipsisChar = "…";
    private bool _clip, _upper;
    private int _linesSkipped, _maxLines = -1, _visibleChars = -1;
    private float _visibleRatio = 1f;
    private float _shapedWidth = -1f;
    private TextLines? _lines;

    public Label() { MouseFilter = MouseFilterEnum.Ignore; }

    public string Text
    {
        get => _text;
        set
        {
            value ??= "";
            if (_text == value) return;
            _text = value;
            Invalidate();
        }
    }

    public LabelSettings? LabelSettings { get => _settings; set { _settings = value; Invalidate(); } }
    public HorizontalAlignment HorizontalAlignment { get => _halign; set { _halign = value; QueueRedraw(); } }
    public VerticalAlignment VerticalAlignment { get => _valign; set { _valign = value; QueueRedraw(); } }
    public TextServer.AutowrapMode AutowrapMode { get => _wrap; set { _wrap = value; Invalidate(); } }
    public TextServer.JustificationFlag JustificationFlags { get; set; } = TextServer.JustificationFlag.Kashida | TextServer.JustificationFlag.WordBound | TextServer.JustificationFlag.SkipLastLine | TextServer.JustificationFlag.DoNotSkipSingleLine;
    public bool ClipText { get => _clip; set { _clip = value; Invalidate(); } }
    public TextServer.OverrunBehavior TextOverrunBehavior { get => _overrun; set { _overrun = value; Invalidate(); } }
    public string EllipsisChar { get => _ellipsisChar; set { _ellipsisChar = value; Invalidate(); } }
    public bool Uppercase { get => _upper; set { _upper = value; Invalidate(); } }
    public int LinesSkipped { get => _linesSkipped; set { _linesSkipped = Math.Max(0, value); Invalidate(); } }
    public int MaxLinesVisible { get => _maxLines; set { _maxLines = value; Invalidate(); } }
    public string Language { get; set; } = "";
    public string ParagraphSeparator { get; set; } = "\\n";
    public new TextServer.Direction TextDirection { get; set; } = TextServer.Direction.Inherited;
    public float[] TabStops { get; set; } = System.Array.Empty<float>();
    public TextServer.VisibleCharactersBehavior VisibleCharactersBehavior { get; set; } = TextServer.VisibleCharactersBehavior.CharsBeforeShaping;

    /// <summary>How many characters are shown; -1 shows all. Newlines are not counted.</summary>
    public int VisibleCharacters
    {
        get => _visibleChars;
        set
        {
            _visibleChars = value;
            _visibleRatio = value < 0 ? 1f : (GetTotalCharacterCount() == 0 ? 1f : Math.Min(1f, value / (float)GetTotalCharacterCount()));
            QueueRedraw();
        }
    }

    /// <summary>The shown share of the characters, 0 to 1.</summary>
    public float VisibleRatio
    {
        get => _visibleRatio;
        set
        {
            if (value >= 1f) { _visibleChars = -1; _visibleRatio = 1f; }
            else
            {
                _visibleRatio = Math.Max(0f, value);
                _visibleChars = (int)(GetTotalCharacterCount() * _visibleRatio);
            }
            QueueRedraw();
        }
    }

    private void Invalidate()
    {
        _lines = null;
        UpdateMinimumSize();
        QueueRedraw();
    }

    // ---- the look ----

    private Font? FontNow => _settings?.Font ?? ThemeFont();
    private int SizeNow => _settings?.FontSize ?? ThemeFontSize();
    private Color ColorNow => _settings?.FontColor ?? GetThemeColor("font_color");
    private float SpacingNow => _settings?.LineSpacing ?? GetThemeConstant("line_spacing");
    private string Shown => _upper ? _text.ToUpperInvariant() : _text;

    private CanvasItem.TextFx FxNow()
    {
        if (_settings != null)
            return new CanvasItem.TextFx
            {
                OutlineSize = _settings.OutlineSize, OutlineColor = _settings.OutlineColor, ShadowColor = _settings.ShadowColor,
                ShadowOffset = _settings.ShadowOffset, ShadowOutlineSize = _settings.ShadowSize,
            };
        return ThemeTextFx();
    }

    private bool Overruns => _overrun != TextServer.OverrunBehavior.NoTrimming;

    // The width the text is laid out in: the control's width less the style, when it wraps and has a width.
    private float WrapWidth()
    {
        if (_wrap == TextServer.AutowrapMode.Off) return 0f;
        float w = Size.X - GetThemeStylebox("normal").GetMinimumSize().X;
        return w > 1f ? w : 0f;
    }

    private TextLines Lines()
    {
        float w = WrapWidth();
        if (_lines == null || _shapedWidth != w)
        {
            _shapedWidth = w;
            _lines = TextLines.Build(Shown, FontNow, SizeNow, w, TextServer.WrapOf(_wrap));
        }
        return _lines;
    }

    private (int First, int Count) VisibleLines(TextLines l)
    {
        int first = Math.Min(_linesSkipped, Math.Max(0, l.Count - 1));
        int count = l.Count - first;
        if (_maxLines > 0) count = Math.Min(count, _maxLines);
        return (first, count);
    }

    /// <summary>The number of lines the text is broken into.</summary>
    public int GetLineCount() => Lines().Count;

    /// <summary>The number of lines drawn (after lines_skipped and max_lines_visible).</summary>
    public int GetVisibleLineCount() => VisibleLines(Lines()).Count;

    public int GetLineHeight(int line = -1) => (int)MathF.Ceiling(UiText.LineHeight(FontNow, SizeNow));

    /// <summary>The characters in the text, newlines not counted.</summary>
    public int GetTotalCharacterCount()
    {
        int n = 0;
        foreach (char c in Shown) if (c != '\n') n++;
        return n;
    }

    public Rect2 GetCharacterBounds(int pos)
    {
        TextLines l = Lines();
        for (int i = 0; i < l.Count; i++)
        {
            TextLines.Line li = l.Items[i];
            if (pos < li.Ord || pos > li.Ord + li.Length) continue;
            string s = l.Text.Substring(li.Start, li.Length);
            int k = Math.Clamp(pos - li.Ord, 0, li.Length);
            float x0 = UiText.Width(FontNow, SizeNow, s[..k]);
            float x1 = k < li.Length ? UiText.Width(FontNow, SizeNow, s[..(k + 1)]) : x0;
            float lh = UiText.LineHeight(FontNow, SizeNow);
            return new Rect2(x0, i * (lh + SpacingNow), x1 - x0, lh);
        }
        return default;
    }

    // ---- size ----

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 style = GetThemeStylebox("normal").GetMinimumSize();
        TextLines l = Lines();
        (int _, int count) = VisibleLines(l);
        float lh = l.LineHeight;
        float h = count * lh + Math.Max(0, count - 1) * SpacingNow;
        if (_wrap != TextServer.AutowrapMode.Off) return new Vector2(1f + style.X, (_clip || Overruns ? 1f : h) + style.Y);
        float w = l.MaxWidth();
        if (_clip || Overruns) w = 1f;
        return new Vector2(w + style.X, h + style.Y);
    }

    public override void _Notification(int what)
    {
        if (what == NotificationResized && _wrap != TextServer.AutowrapMode.Off && WrapWidth() != _shapedWidth) Invalidate();
        else if (what == NotificationThemeChanged) { _lines = null; }
    }

    // ---- drawing ----

    public override void _Draw()
    {
        StyleBox normal = GetThemeStylebox("normal");
        DrawStyleBox(normal, LocalRect);
        TextLines l = Lines();
        (int first, int count) = VisibleLines(l);
        Font? font = FontNow;
        int px = SizeNow;
        float lh = l.LineHeight, spacing = SpacingNow;
        Rect2 content = normal.GetContentRect(LocalRect);
        float textH = count * lh + Math.Max(0, count - 1) * spacing;
        float y = content.Position.Y;
        if (_valign == VerticalAlignment.Center) y += (content.Size.Y - textH) / 2f;
        else if (_valign == VerticalAlignment.Bottom) y += content.Size.Y - textH;
        Color color = ColorNow;
        CanvasItem.TextFx fx = FxNow();
        if (_clip || Overruns) PushClip(content.Grow(Math.Max(0f, fx.OutlineSize)));
        for (int i = 0; i < count; i++)
        {
            int li = first + i;
            TextLines.Line line = l.Items[li];
            string seg = l.LineText(li);
            float width = line.Width;
            bool lastShown = i == count - 1 && li < l.Count - 1;
            if (Overruns && (width > content.Size.X || (lastShown && _maxLines > 0 && _wrap != TextServer.AutowrapMode.Off)) && content.Size.X > 0f)
            {
                bool word = _overrun is TextServer.OverrunBehavior.TrimWord or TextServer.OverrunBehavior.TrimWordEllipsis or TextServer.OverrunBehavior.TrimWordEllipsisForce;
                bool ell = _overrun >= TextServer.OverrunBehavior.TrimEllipsis;
                if (width > content.Size.X) seg = TextLines.Trim(seg, font, px, content.Size.X, word, ell ? _ellipsisChar : "");
                else if (ell) seg = TextLines.Trim(seg + _ellipsisChar + _ellipsisChar, font, px, MathF.Max(0f, content.Size.X), word, "");
                width = UiText.Width(font, px, seg);
            }
            float x = content.Position.X;
            if (_halign == HorizontalAlignment.Center) x += (content.Size.X - width) / 2f;
            else if (_halign == HorizontalAlignment.Right) x += content.Size.X - width;
            if (_visibleChars >= 0) seg = seg[..Math.Clamp(_visibleChars - line.Ord, 0, seg.Length)];
            DrawTextLine(new Vector2(x, y + i * (lh + spacing)), seg, font, px, color, fx);
        }
        if (_clip || Overruns) PopClip();
    }
}
