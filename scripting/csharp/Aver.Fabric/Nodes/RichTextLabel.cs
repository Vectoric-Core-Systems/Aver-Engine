// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// RichTextLabel (G5 U2): Godot's rich text control. The document (RichText.Doc.cs) comes from BBCode (BBCode.cs) or the push_* API, the layout
// (RichText.Layout.cs) is cached per width, and this file draws it, scrolls it, finds meta and hint under the pointer, selects text, and runs the effects
// (wave, tornado, shake, fade, rainbow and custom RichTextEffect classes) per glyph with the label's own clock.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>How an inline image or table sits on its line (Godot's InlineAlignment).</summary>
public enum InlineAlignment
{
    TopTo = 0, CenterTo = 1, BaselineTo = 3, BottomTo = 2, ToTop = 0, ToCenter = 4, ToBaseline = 8, ToBottom = 12, Top = 0, Center = 5, Bottom = 14,
}

/// <summary>Text with styles, images, lists, tables and effects (Godot's RichTextLabel).</summary>
public partial class RichTextLabel : Control
{
    public enum ListType { Numbers = 0, Letters = 1, Roman = 2, Dots = 3 }

    public enum MetaUnderline { Never = 0, Always = 1, OnHover = 2 }

    public enum ImageUpdateMask { Texture = 1, Size = 2, Color = 4, Alignment = 8, Region = 16, Pad = 32, Tooltip = 64, WidthInPercent = 128, HeightInPercent = 256 }

    public new class SignalName : Control.SignalName
    {
        public static readonly StringName MetaClicked = "meta_clicked";
        public static readonly StringName MetaHoverStarted = "meta_hover_started";
        public static readonly StringName MetaHoverEnded = "meta_hover_ended";
        public static readonly StringName Finished = "finished";
    }

    public event Action<Variant> MetaClicked { add => Connect(SignalName.MetaClicked, Callable.From(value)); remove => Disconnect(SignalName.MetaClicked, Callable.From(value)); }
    public event Action<Variant> MetaHoverStarted { add => Connect(SignalName.MetaHoverStarted, Callable.From(value)); remove => Disconnect(SignalName.MetaHoverStarted, Callable.From(value)); }
    public event Action<Variant> MetaHoverEnded { add => Connect(SignalName.MetaHoverEnded, Callable.From(value)); remove => Disconnect(SignalName.MetaHoverEnded, Callable.From(value)); }

    private string _text = "";
    private bool _bbcode, _fitContent, _scrollActive = true, _scrollFollowing, _metaUnderlined = true, _selectionEnabled, _stick;
    private TextServer.AutowrapMode _wrap = TextServer.AutowrapMode.WordSmart;
    private int _tabSize = 4, _visibleChars = -1;
    private float _visibleRatio = 1f;
    private readonly VScrollBar _scrollV = new();
    private readonly Dictionary<int, (bool Expand, int Ratio)> _columnExpand = new();
    private Color? _rowOdd, _rowEven;
    private Color _cellBorder = new(0, 0, 0, 0);
    private Rect2 _cellPadding = default;
    private RtBlock? _layout;
    private float _layoutWidth = -1f;
    private int _selFrom = -1, _selTo = -1, _hover = -1;
    private bool _drag;
    private double _effectTime;
    private Collections.Array<RichTextEffect> _customEffects = new();

    public RichTextLabel()
    {
        MouseFilter = MouseFilterEnum.Pass;
        FocusMode = FocusModeEnum.None;
        SetProcess(false);
    }

    // ---- properties ----

    public string Text
    {
        get => _text;
        set
        {
            _text = value ?? "";
            ClearDocument();
            if (_bbcode) BBCodeParser.Append(this, _text);
            else AddText(_text);
        }
    }

    public bool BbcodeEnabled
    {
        get => _bbcode;
        set
        {
            if (_bbcode == value) return;
            _bbcode = value;
            if (_text.Length > 0) Text = _text;
        }
    }

    public TextServer.AutowrapMode AutowrapMode { get => _wrap; set { _wrap = value; Dirty(); } }
    public int TabSize { get => _tabSize; set { _tabSize = Math.Max(1, value); Dirty(); } }
    public bool FitContent { get => _fitContent; set { _fitContent = value; Dirty(); } }
    public bool ScrollActive { get => _scrollActive; set { _scrollActive = value; Dirty(); } }
    public bool ScrollFollowing { get => _scrollFollowing; set { _scrollFollowing = value; if (value) _stick = true; QueueRedraw(); } }
    public bool MetaUnderlined { get => _metaUnderlined; set { _metaUnderlined = value; Dirty(); } }
    public bool HintUnderlined { get; set; } = true;
    public bool SelectionEnabled { get => _selectionEnabled; set { _selectionEnabled = value; if (value) FocusMode = FocusModeEnum.Click; } }
    public bool DeselectOnFocusLossEnabled { get; set; }
    public bool ContextMenuEnabled { get; set; }
    public bool ShortcutKeysEnabled { get; set; } = true;
    public bool Threaded { get; set; }
    public float ProgressBarDelay { get; set; } = 1000f;
    public string Language { get; set; } = "";
    public new TextServer.Direction TextDirection { get; set; } = TextServer.Direction.Inherited;
    public TextServer.VisibleCharactersBehavior VisibleCharactersBehavior { get; set; } = TextServer.VisibleCharactersBehavior.CharsBeforeShaping;
    public TextServer.OverrunBehavior TextOverrunBehavior { get; set; }
    public Collections.Array<RichTextEffect> CustomEffects { get => _customEffects; set { _customEffects = value; Dirty(); } }

    public string GetText() => _text;

    /// <summary>Parses BBCode and adds it to the end of the document (Godot's append_text).</summary>
    public void AppendText(string bbcode)
    {
        BBCodeParser.Append(this, bbcode);
        FollowScroll();
    }

    /// <summary>Replaces the document with the BBCode text (Godot's parse_bbcode).</summary>
    public Error ParseBbcode(string bbcode)
    {
        ClearDocument();
        BBCodeParser.Append(this, bbcode);
        return Error.Ok;
    }

    private void ClearDocument()
    {
        string keep = _text;
        Clear();
        _text = keep;
    }

    public int VisibleCharacters
    {
        get => _visibleChars;
        set
        {
            _visibleChars = value;
            int total = GetTotalCharacterCount();
            _visibleRatio = value < 0 || total == 0 ? 1f : Math.Min(1f, value / (float)total);
            QueueRedraw();
        }
    }

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

    public bool HasCustomEffect(string tag)
    {
        foreach (RichTextEffect e in _customEffects) if (e.Bbcode == tag) return true;
        return false;
    }

    public void InstallEffect(Variant effect)
    {
        if (effect.AsGodotObject() is RichTextEffect e) { _customEffects.Add(e); Dirty(); }
    }

    // ---- layout, scroll ----

    private void Dirty()
    {
        _layout = null;
        UpdateMinimumSize();
        QueueRedraw();
        SetProcess(HasEffects);
    }

    private StyleBox Normal => GetThemeStylebox("normal");

    private Rect2 Content() => Normal.GetContentRect(LocalRect);

    private RtBlock Layout()
    {
        float width = Content().Size.X;
        if (_layout != null && _layoutWidth == width) return _layout;
        RtBlock b = LayoutAll(width > 0f ? width : 1e6f);
        float view = Content().Size.Y;
        if (_scrollActive && b.Height > view && width > 12f && view > 0f)
        {
            width -= 12f;
            b = LayoutAll(width);
        }
        _layout = b;
        _lastHeight = b.Height;
        _layoutWidth = Content().Size.X;
        double max = Math.Max(0, b.Height - view);
        _scrollV.MaxValue = Math.Max(1, b.Height);
        _scrollV.Page = view;
        if (_stick && _scrollFollowing) _scrollV.Value = max;
        if (_scrollV.Value > max) _scrollV.Value = max;
        return b;
    }

    private float _lastHeight;

    private void FollowScroll()
    {
        if (!_scrollFollowing) return;
        // stay at the bottom when we were there
        double max = Math.Max(0, _lastHeight - Content().Size.Y);
        _stick = _scrollV.Value >= max - 1.0;
        QueueRedraw();
    }

    /// <summary>The effect tags of the document, in order (for tests of the formulas).</summary>
    internal List<RtItem> EffectItems() => _items.Where(i => i.Kind == RtItem.K.Open && i.Op == RtOp.Effect).ToList();

    /// <summary>The seconds the effects have run.</summary>
    internal double EffectClock => _effectTime;

    public VScrollBar GetVScrollBar() => _scrollV;

    public int GetContentHeight() => (int)MathF.Ceiling(Layout().Height);

    public int GetContentWidth() => (int)MathF.Ceiling(Layout().Extent);

    public int GetLineCount() => Layout().Lines.Count;

    public int GetVisibleLineCount()
    {
        RtBlock b = Layout();
        float top = (float)_scrollV.Value, bottom = top + Content().Size.Y;
        int n = 0;
        foreach (RtLine l in b.Lines) if (l.Top + l.Height > top && l.Top < bottom) n++;
        return n;
    }

    public int GetLineOffset(int line)
    {
        RtBlock b = Layout();
        return line >= 0 && line < b.Lines.Count ? (int)b.Lines[line].Top : 0;
    }

    public int GetParagraphOffset(int paragraph)
    {
        foreach (RtLine l in Layout().Lines) if (l.Paragraph == paragraph) return (int)l.Top;
        return 0;
    }

    public void ScrollToLine(int line) => _scrollV.Value = GetLineOffset(line);

    public void ScrollToParagraph(int paragraph) => _scrollV.Value = GetParagraphOffset(paragraph);

    public void ScrollToSelection() { if (_selFrom >= 0) ScrollToCharacter(_selFrom); }

    private void ScrollToCharacter(int ord)
    {
        foreach (RtLine l in Layout().Lines) if (l.Ord >= ord) { _scrollV.Value = l.Top; return; }
    }

    internal override Vector2 NativeMinimumSize()
    {
        if (!_fitContent) return Vector2.Zero;
        RtBlock b = Layout();
        Vector2 style = Normal.GetMinimumSize();
        return new Vector2(_wrap == TextServer.AutowrapMode.Off ? b.Extent : 1f, b.Height) + style;
    }

    public override void _Notification(int what)
    {
        if (what == NotificationResized) { if (Content().Size.X != _layoutWidth) { _layout = null; if (_fitContent) UpdateMinimumSize(); } QueueRedraw(); }
        else if (what == NotificationThemeChanged) { _layout = null; _variants.Clear(); }
    }

    // ---- effects ----

    public override void _Process(double delta)
    {
        _effectTime += delta;
        QueueRedraw();
    }

    /// <summary>Sets the clock the effects run on (seconds). The label advances it itself every frame while it has an effect.</summary>
    internal void SetEffectTime(double seconds) { _effectTime = seconds; QueueRedraw(); }

    private static float Num(RtItem e, string key, float fallback) =>
        e.Args != null && e.Args.TryGetValue(key, out string? s) && float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float v) ? v : fallback;

    private static uint Hash(int a, int b)
    {
        uint h = (uint)a * 2654435761u ^ (uint)b * 40503u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        return h;
    }

    /// <summary>One character's offset, colour and visibility under the effects of its run. Godot's formulas for the built-in effects; amplitude and strength
    /// scaling are read from its documented defaults.</summary>
    internal bool ApplyEffects(List<RtItem> effects, int index, int relative, double time, ref Vector2 offset, ref Color color, int rangeStart)
    {
        foreach (RtItem e in effects)
        {
            switch (e.Tag)
            {
                case "wave":
                {
                    float amp = Num(e, "amp", 20f), freq = Num(e, "freq", 5f);
                    offset.Y -= (float)(Math.Sin(freq * time + (rangeStart + relative) / 4.0) * (amp / 10.0));
                    break;
                }
                case "tornado":
                {
                    float radius = Num(e, "radius", 10f), freq = Num(e, "freq", 1f);
                    double a = freq * time + (rangeStart + relative) / 4.0;
                    offset += new Vector2((float)Math.Sin(a), (float)Math.Cos(a)) * radius;
                    break;
                }
                case "shake":
                {
                    float rate = Num(e, "rate", 20f), level = Num(e, "level", 5f);
                    int step = (int)Math.Floor(time * rate);
                    double a = Hash(index, step) / (double)uint.MaxValue * Math.PI * 2.0;
                    offset += new Vector2((float)Math.Sin(a), (float)Math.Cos(a)) * level;
                    break;
                }
                case "fade":
                {
                    float start = Num(e, "start", 4f), length = Num(e, "length", 14f);
                    if (relative + rangeStart >= start) color.A *= Math.Clamp(1f - (relative + rangeStart - start) / Math.Max(1f, length), 0f, 1f);
                    break;
                }
                case "rainbow":
                {
                    float freq = Num(e, "freq", 1f), sat = Num(e, "sat", 0.8f), val = Num(e, "val", 0.8f);
                    float hue = (float)(((-freq * time + (rangeStart + relative) / 20.0) % 1.0 + 1.0) % 1.0);
                    color = Color.FromHsv(hue, sat, val, color.A);
                    break;
                }
                default:
                    foreach (RichTextEffect custom in _customEffects)
                    {
                        if (custom.Bbcode != e.Tag) continue;
                        var fx = new CharFXTransform
                        {
                            Range = new Vector2I(rangeStart, rangeStart + relative + 1), ElapsedTime = time, RelativeIndex = relative, AbsoluteIndex = index,
                            Offset = offset, Color = color, Visible = true,
                        };
                        if (e.Args != null) foreach (KeyValuePair<string, string> kv in e.Args) fx.Env[kv.Key] = float.TryParse(kv.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float f) ? f : kv.Value;
                        bool visible = custom._ProcessCustomFx(fx) && fx.Visible;
                        offset = fx.Offset;
                        color = fx.Color;
                        if (!visible) return false;
                    }
                    break;
            }
        }
        return true;
    }

    // ---- drawing ----

    public override void _Draw()
    {
        DrawStyleBox(Normal, LocalRect);
        if (HasFocus()) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        RtBlock b = Layout();
        Rect2 c = Content();
        Vector2 origin = c.Position - new Vector2(0, (float)_scrollV.Value);
        CanvasItem.TextFx themeFx = ThemeTextFx();
        PushClip(c);
        int visible = _visibleChars;
        foreach (RtDraw d in b.Draws)
        {
            if (visible >= 0 && d.Ord >= visible && d.Kind != RtDraw.K.Rect) continue;
            if (visible >= 0 && d.Ord >= visible) continue;
            var r = new Rect2(d.Rect.Position + origin, d.Rect.Size);
            if (r.End.Y < c.Position.Y || r.Position.Y > c.End.Y) continue;
            switch (d.Kind)
            {
                case RtDraw.K.Rect: DrawRect(r, d.Color); break;
                case RtDraw.K.Image: if (d.Texture != null) DrawTextureRect(d.Texture, r, false, d.Color); break;
                case RtDraw.K.Text: DrawRun(d, r, visible, themeFx); break;
            }
        }
        if (_selFrom >= 0 && _selTo > _selFrom) DrawSelection(b, origin);
        PopClip();
    }

    private void DrawRun(RtDraw d, Rect2 r, int visible, CanvasItem.TextFx themeFx)
    {
        string text = d.Text;
        if (visible >= 0) text = text[..Math.Clamp(visible - d.Ord, 0, text.Length)];
        if (text.Length == 0) return;
        CanvasItem.TextFx fx = d.Fx.Any ? d.Fx : themeFx.OutlineSize >= 0.5f || themeFx.ShadowColor.A > 0f ? themeFx : default;
        if (d.Effects == null)
        {
            DrawTextLine(r.Position, text, d.Font, d.Size, d.Color, fx);
            return;
        }
        for (int i = 0; i < text.Length; i++)
        {
            Vector2 off = Vector2.Zero;
            Color col = d.Color;
            if (!ApplyEffects(d.Effects, d.Ord + i, i, _effectTime, ref off, ref col, d.Ord)) continue;
            float x = UiText.Width(d.Font, d.Size, text[..i]);
            DrawTextLine(r.Position + new Vector2(x, 0) + off, text[i].ToString(), d.Font, d.Size, col, fx);
        }
    }

    private void DrawSelection(RtBlock b, Vector2 origin)
    {
        Color sel = GetThemeColor("selection_color");
        foreach (RtDraw d in b.Draws)
        {
            if (d.Kind != RtDraw.K.Text) continue;
            int a = Math.Max(_selFrom, d.Ord), z = Math.Min(_selTo, d.Ord + d.Text.Length);
            if (z <= a) continue;
            float x0 = UiText.Width(d.Font, d.Size, d.Text[..(a - d.Ord)]), x1 = UiText.Width(d.Font, d.Size, d.Text[..(z - d.Ord)]);
            DrawRect(new Rect2(d.Rect.Position + origin + new Vector2(x0, 0), new Vector2(x1 - x0, d.Rect.Size.Y)), sel);
        }
    }

    // ---- selection and meta ----

    private int OrdAt(Vector2 p)
    {
        RtBlock b = Layout();
        Vector2 q = p - Content().Position + new Vector2(0, (float)_scrollV.Value);
        int best = -1;
        float bestDist = float.MaxValue;
        foreach (RtDraw d in b.Draws)
        {
            if (d.Kind != RtDraw.K.Text) continue;
            float dy = q.Y < d.Rect.Position.Y ? d.Rect.Position.Y - q.Y : q.Y > d.Rect.End.Y ? q.Y - d.Rect.End.Y : 0f;
            float dx = q.X < d.Rect.Position.X ? d.Rect.Position.X - q.X : q.X > d.Rect.End.X ? q.X - d.Rect.End.X : 0f;
            float dist = dy * 1000f + dx;
            if (dist >= bestDist) continue;
            bestDist = dist;
            int k = 0;
            while (k < d.Text.Length && UiText.Width(d.Font, d.Size, d.Text[..(k + 1)]) < q.X - d.Rect.Position.X) k++;
            best = d.Ord + k;
        }
        return best;
    }

    private RtHit? HitAt(Vector2 p)
    {
        RtBlock b = Layout();
        Vector2 q = p - Content().Position + new Vector2(0, (float)_scrollV.Value);
        for (int i = b.Hits.Count - 1; i >= 0; i--) if (b.Hits[i].Rect.HasPoint(q)) return b.Hits[i];
        return null;
    }

    public bool SelectAll()
    {
        if (!_selectionEnabled) return false;
        _selFrom = 0;
        _selTo = GetTotalCharacterCount();
        QueueRedraw();
        return true;
    }

    public void Deselect() { _selFrom = _selTo = -1; QueueRedraw(); }

    public int GetSelectionFrom() => _selFrom;

    public int GetSelectionTo() => _selTo;

    public string GetSelectedText()
    {
        if (_selFrom < 0 || _selTo <= _selFrom) return "";
        var sb = new System.Text.StringBuilder();
        int n = 0;
        foreach (char c in GetParsedText())
        {
            if (c == '\n') { if (n > _selFrom && n < _selTo) sb.Append(c); continue; }
            if (n >= _selFrom && n < _selTo) sb.Append(c);
            n++;
        }
        return sb.ToString();
    }

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseMotion mm:
            {
                RtHit? hit = HitAt(mm.Position);
                int now = hit is { IsMeta: true } ? hit.Value.Ord : -1;
                if (now != _hover)
                {
                    if (_hover >= 0 && _lastMeta is { } old) EmitSignal(SignalName.MetaHoverEnded, old);
                    _hover = now;
                    _lastMeta = hit is { IsMeta: true } ? hit.Value.Meta : (Variant?)null;
                    if (now >= 0) EmitSignal(SignalName.MetaHoverStarted, hit!.Value.Meta);
                }
                if (_drag && _selectionEnabled) { int o = OrdAt(mm.Position); if (o >= 0) { _selFrom = Math.Min(_dragFrom, o); _selTo = Math.Max(_dragFrom, o); QueueRedraw(); } }
                break;
            }
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left:
                if (mb.Pressed)
                {
                    RtHit? hit = HitAt(mb.Position);
                    if (hit is { IsMeta: true }) { EmitSignal(SignalName.MetaClicked, hit.Value.Meta); AcceptEvent(); }
                    else if (_selectionEnabled)
                    {
                        GrabFocus();
                        _drag = true;
                        _dragFrom = Math.Max(0, OrdAt(mb.Position));
                        _selFrom = _selTo = -1;
                        QueueRedraw();
                    }
                }
                else _drag = false;
                break;
            case InputEventMouseButton wheel when wheel.Pressed && _scrollActive && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown:
            {
                double max = Math.Max(0, Layout().Height - Content().Size.Y);
                if (max <= 0) break;
                _scrollV.Value = Math.Clamp(_scrollV.Value + (wheel.ButtonIndex == MouseButton.WheelUp ? -1 : 1) * UiText.LineHeight(ThemeFont("normal_font"), ThemeFontSize("normal_font_size")) * 3, 0, max);
                QueueRedraw();
                AcceptEvent();
                break;
            }
            case InputEventKey { Pressed: true } k when _selectionEnabled && ShortcutKeysEnabled && (k.CtrlPressed || k.MetaPressed):
                if (k.Keycode == Key.A) { SelectAll(); AcceptEvent(); }
                else if (k.Keycode == Key.C) { UiClipboard.Text = GetSelectedText(); AcceptEvent(); }
                break;
        }
    }

    private Variant? _lastMeta;
    private int _dragFrom;

    public override string _GetTooltip(Vector2 atPosition)
    {
        RtHit? hit = HitAt(atPosition);
        return hit is { } h && h.Hint.Length > 0 ? h.Hint : TooltipText;
    }
}
