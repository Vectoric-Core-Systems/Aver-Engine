// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The picture and layout widgets of G5 U2: SplitContainer (H / V, draggable), FlowContainer (H / V), AspectRatioContainer, ColorRect, TextureRect,
// NinePatchRect and the separators. Layouts follow Godot's rules on the minimum sizes the containers of U1 already use.

namespace Aver.Fabric;

/// <summary>Two children with a draggable divider (Godot's SplitContainer).</summary>
public class SplitContainer : Container
{
    public enum DraggerVisibilityEnum { Visible = 0, Hidden = 1, HiddenCollapsed = 2 }

    public new class SignalName : Container.SignalName
    {
        public static readonly StringName Dragged = "dragged";
    }

    public event Action<long> Dragged { add => Connect(SignalName.Dragged, Callable.From(value)); remove => Disconnect(SignalName.Dragged, Callable.From(value)); }

    private bool _vertical, _collapsed, _drag;
    private int _offset;
    private float _dragFrom;
    private int _dragOffset;

    public bool Vertical { get => _vertical; set { _vertical = value; UpdateMinimumSize(); QueueSort(); QueueRedraw(); } }
    public int SplitOffset { get => _offset; set { _offset = value; QueueSort(); QueueRedraw(); } }
    public bool Collapsed { get => _collapsed; set { _collapsed = value; QueueSort(); QueueRedraw(); } }
    public DraggerVisibilityEnum DraggerVisibility { get; set; }
    public bool DraggingEnabled { get; set; } = true;
    public int[] SplitOffsets { get => new[] { _offset }; set { if (value.Length > 0) SplitOffset = value[0]; } }

    public SplitContainer() { MouseFilter = MouseFilterEnum.Pass; }

    private float Sep => GetThemeConstant("separation");

    private (Control? A, Control? B) Pair()
    {
        Control? a = null, b = null;
        foreach (Control c in LaidOutChildren()) { if (a == null) a = c; else if (b == null) b = c; }
        return (a, b);
    }

    private bool Hidden(Control? b) => _collapsed || DraggerVisibility == DraggerVisibilityEnum.Hidden || b == null;

    internal override Vector2 NativeMinimumSize()
    {
        (Control? a, Control? b) = Pair();
        float sep = b == null ? 0f : Sep;
        Vector2 ma = a?.GetCombinedMinimumSize() ?? Vector2.Zero, mb = b?.GetCombinedMinimumSize() ?? Vector2.Zero;
        return _vertical ? new Vector2(MathF.Max(ma.X, mb.X), ma.Y + sep + mb.Y) : new Vector2(ma.X + sep + mb.X, MathF.Max(ma.Y, mb.Y));
    }

    /// <summary>The size of the first child along the split axis: the share its expansion gives it, moved by split_offset, kept between the minimums.</summary>
    internal float FirstSize()
    {
        (Control? a, Control? b) = Pair();
        if (a == null || b == null) return 0f;
        float axis = _vertical ? Size.Y : Size.X, sep = Sep;
        float minA = _vertical ? a.GetCombinedMinimumSize().Y : a.GetCombinedMinimumSize().X;
        float minB = _vertical ? b.GetCombinedMinimumSize().Y : b.GetCombinedMinimumSize().X;
        var flagsA = _vertical ? a.SizeFlagsVertical : a.SizeFlagsHorizontal;
        var flagsB = _vertical ? b.SizeFlagsVertical : b.SizeFlagsHorizontal;
        bool ea = (flagsA & SizeFlags.Expand) != 0, eb = (flagsB & SizeFlags.Expand) != 0;
        float room = axis - sep;
        float first;
        if (ea && eb) first = room * a.SizeFlagsStretchRatio / (a.SizeFlagsStretchRatio + b.SizeFlagsStretchRatio);
        else if (ea) first = room - minB;
        else first = minA;
        if (_collapsed) return first;
        first += _offset;
        return MathF.Max(minA, MathF.Min(room - minB, first));
    }

    internal override void SortChildren()
    {
        (Control? a, Control? b) = Pair();
        if (a == null) return;
        if (b == null) { FitChildInRect(a, LocalRect); return; }
        float first = _collapsed ? FirstSize() : FirstSize();
        float sep = Sep;
        if (_vertical)
        {
            FitChildInRect(a, new Rect2(0, 0, Size.X, first));
            FitChildInRect(b, new Rect2(0, first + sep, Size.X, Size.Y - first - sep));
        }
        else
        {
            FitChildInRect(a, new Rect2(0, 0, first, Size.Y));
            FitChildInRect(b, new Rect2(first + sep, 0, Size.X - first - sep, Size.Y));
        }
    }

    private Rect2 DraggerRect()
    {
        float first = FirstSize(), sep = Sep;
        return _vertical ? new Rect2(0, first, Size.X, sep) : new Rect2(first, 0, sep, Size.Y);
    }

    public override bool _HasPoint(Vector2 point) => LocalRect.HasPoint(point);

    public override void _GuiInput(InputEvent @event)
    {
        (Control? _, Control? b) = Pair();
        if (b == null || _collapsed || DraggerVisibility == DraggerVisibilityEnum.Hidden || !DraggingEnabled) return;
        switch (@event)
        {
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left:
                if (mb.Pressed && DraggerRect().HasPoint(mb.Position))
                {
                    _drag = true;
                    _dragFrom = _vertical ? mb.Position.Y : mb.Position.X;
                    _dragOffset = _offset;
                    AcceptEvent();
                }
                else if (!mb.Pressed && _drag) { _drag = false; AcceptEvent(); }
                break;
            case InputEventMouseMotion mm when _drag:
            {
                float now = _vertical ? mm.Position.Y : mm.Position.X;
                _offset = _dragOffset + (int)(now - _dragFrom);
                QueueSort();
                QueueRedraw();
                EmitSignal(SignalName.Dragged, FirstSize() - (FirstSizeNoOffset()));
                AcceptEvent();
                break;
            }
        }
    }

    private float FirstSizeNoOffset()
    {
        int keep = _offset;
        _offset = 0;
        float f = FirstSize();
        _offset = keep;
        return f;
    }

    public override void _Draw()
    {
        (Control? a, Control? b) = Pair();
        if (a == null || b == null || Hidden(b) && DraggerVisibility != DraggerVisibilityEnum.Visible) return;
        Rect2 r = DraggerRect();
        DrawStyleBox(GetThemeStylebox("split_bar_background"), r);
        float grab = 24f;
        Rect2 g = _vertical ? new Rect2(r.Position.X + (r.Size.X - grab) / 2f, r.Position.Y + (r.Size.Y - 2f) / 2f, grab, 2f) : new Rect2(r.Position.X + (r.Size.X - 2f) / 2f, r.Position.Y + (r.Size.Y - grab) / 2f, 2f, grab);
        if (DraggerVisibility != DraggerVisibilityEnum.Hidden) DrawRect(g, new Color(0.875f, 0.875f, 0.875f, 0.5f));
    }
}

public class HSplitContainer : SplitContainer { }

public class VSplitContainer : SplitContainer
{
    public VSplitContainer() { Vertical = true; }
}

/// <summary>Children laid out in lines that wrap (Godot's FlowContainer).</summary>
public class FlowContainer : Container
{
    public enum AlignmentMode { Begin = 0, Center = 1, End = 2 }

    public enum LastWrapAlignmentMode { Inherit = 0, Begin = 1, Center = 2, End = 3 }

    private bool _vertical, _reverse;
    private AlignmentMode _alignment = AlignmentMode.Begin;

    public bool Vertical { get => _vertical; set { _vertical = value; UpdateMinimumSize(); QueueSort(); } }
    public AlignmentMode Alignment { get => _alignment; set { _alignment = value; QueueSort(); } }
    public LastWrapAlignmentMode LastWrapAlignment { get; set; }
    public bool ReverseFill { get => _reverse; set { _reverse = value; QueueSort(); } }

    private float HSep => GetThemeConstant("h_separation");
    private float VSep => GetThemeConstant("v_separation");

    // The height of a horizontal flow (the width of a vertical one) depends on the length it is given.
    public override void _Notification(int what)
    {
        if (what == NotificationResized) UpdateMinimumSize();
    }

    private struct Run
    {
        public List<Control> Items;
        public float Main, Cross;
    }

    // Splits the children into runs no longer than `limit` along the main axis.
    private List<Run> Runs(float limit)
    {
        float sepMain = _vertical ? VSep : HSep;
        var runs = new List<Run>();
        var cur = new Run { Items = new List<Control>() };
        foreach (Control c in LaidOutChildren())
        {
            Vector2 m = c.GetCombinedMinimumSize();
            float main = _vertical ? m.Y : m.X, cross = _vertical ? m.X : m.Y;
            float next = cur.Items.Count == 0 ? main : cur.Main + sepMain + main;
            if (cur.Items.Count > 0 && limit > 0 && next > limit)
            {
                runs.Add(cur);
                cur = new Run { Items = new List<Control>() };
                next = main;
            }
            cur.Items.Add(c);
            cur.Main = next;
            cur.Cross = MathF.Max(cur.Cross, cross);
        }
        if (cur.Items.Count > 0) runs.Add(cur);
        return runs;
    }

    internal override Vector2 NativeMinimumSize()
    {
        float limit = _vertical ? Size.Y : Size.X;
        List<Run> runs = Runs(limit);
        float cross = 0f, widest = 0f;
        foreach (Run r in runs) { cross += r.Cross; widest = MathF.Max(widest, r.Main); }
        float sepCross = _vertical ? HSep : VSep;
        cross += Math.Max(0, runs.Count - 1) * sepCross;
        float minMain = 0f;
        foreach (Control c in LaidOutChildren()) minMain = MathF.Max(minMain, _vertical ? c.GetCombinedMinimumSize().Y : c.GetCombinedMinimumSize().X);
        return _vertical ? new Vector2(cross, minMain) : new Vector2(minMain, cross);
    }

    internal override void SortChildren()
    {
        float limit = _vertical ? Size.Y : Size.X;
        List<Run> runs = Runs(limit);
        float sepMain = _vertical ? VSep : HSep, sepCross = _vertical ? HSep : VSep;
        float cross = 0f;
        for (int ri = 0; ri < runs.Count; ri++)
        {
            Run r = runs[ri];
            float extra = MathF.Max(0f, limit - r.Main);
            int expanders = 0;
            foreach (Control c in r.Items) if (((_vertical ? c.SizeFlagsVertical : c.SizeFlagsHorizontal) & SizeFlags.Expand) != 0) expanders++;
            float start = 0f;
            if (expanders == 0) start = _alignment == AlignmentMode.Center ? extra / 2f : _alignment == AlignmentMode.End ? extra : 0f;
            float pos = start;
            foreach (Control c in r.Items)
            {
                Vector2 m = c.GetCombinedMinimumSize();
                float main = _vertical ? m.Y : m.X;
                if (expanders > 0 && ((_vertical ? c.SizeFlagsVertical : c.SizeFlagsHorizontal) & SizeFlags.Expand) != 0) main += extra / expanders;
                Rect2 slot = _vertical ? new Rect2(cross, pos, r.Cross, main) : new Rect2(pos, cross, main, r.Cross);
                FitChildInRect(c, slot);
                pos += main + sepMain;
            }
            cross += r.Cross + sepCross;
        }
    }
}

public class HFlowContainer : FlowContainer { }

public class VFlowContainer : FlowContainer
{
    public VFlowContainer() { Vertical = true; }
}

/// <summary>Keeps its children at a fixed aspect ratio (Godot's AspectRatioContainer).</summary>
public class AspectRatioContainer : Container
{
    public enum StretchModeEnum { WidthControlsHeight = 0, HeightControlsWidth = 1, Fit = 2, Cover = 3 }

    public enum AlignmentMode { Begin = 0, Center = 1, End = 2 }

    private float _ratio = 1f;
    private StretchModeEnum _mode = StretchModeEnum.Fit;

    public float Ratio { get => _ratio; set { _ratio = MathF.Max(0.0001f, value); QueueSort(); } }
    public StretchModeEnum StretchMode { get => _mode; set { _mode = value; QueueSort(); } }
    public AlignmentMode AlignmentHorizontal { get; set; } = AlignmentMode.Center;
    public AlignmentMode AlignmentVertical { get; set; } = AlignmentMode.Center;

    /// <summary>The rectangle (within a container of the given size) the children are given.</summary>
    internal Rect2 ChildRect(Vector2 size)
    {
        Vector2 s = size;
        switch (_mode)
        {
            case StretchModeEnum.WidthControlsHeight: s = new Vector2(size.X, size.X / _ratio); break;
            case StretchModeEnum.HeightControlsWidth: s = new Vector2(size.Y * _ratio, size.Y); break;
            case StretchModeEnum.Fit: s = size.X / MathF.Max(size.Y, 0.0001f) > _ratio ? new Vector2(size.Y * _ratio, size.Y) : new Vector2(size.X, size.X / _ratio); break;
            case StretchModeEnum.Cover: s = size.X / MathF.Max(size.Y, 0.0001f) > _ratio ? new Vector2(size.X, size.X / _ratio) : new Vector2(size.Y * _ratio, size.Y); break;
        }
        float x = AlignmentHorizontal == AlignmentMode.Begin ? 0f : AlignmentHorizontal == AlignmentMode.End ? size.X - s.X : (size.X - s.X) / 2f;
        float y = AlignmentVertical == AlignmentMode.Begin ? 0f : AlignmentVertical == AlignmentMode.End ? size.Y - s.Y : (size.Y - s.Y) / 2f;
        return new Rect2(x, y, s.X, s.Y);
    }

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 m = Vector2.Zero;
        foreach (Control c in LaidOutChildren()) { Vector2 cm = c.GetCombinedMinimumSize(); m = new Vector2(MathF.Max(m.X, cm.X), MathF.Max(m.Y, cm.Y)); }
        return m;
    }

    internal override void SortChildren()
    {
        Rect2 r = ChildRect(Size);
        foreach (Control c in LaidOutChildren()) FitChildInRect(c, r);
    }
}

/// <summary>A filled rectangle (Godot's ColorRect).</summary>
public class ColorRect : Control
{
    private Color _color = new(1, 1, 1, 1);

    public Color Color { get => _color; set { _color = value; QueueRedraw(); } }

    public override void _Draw() => DrawRect(LocalRect, _color);
}

/// <summary>A texture on the screen (Godot's TextureRect).</summary>
public class TextureRect : Control
{
    public enum ExpandModeEnum { KeepSize = 0, IgnoreSize = 1, FitWidth = 2, FitWidthProportional = 3, FitHeight = 4, FitHeightProportional = 5 }

    public enum StretchModeEnum { Scale = 0, Tile = 1, Keep = 2, KeepCentered = 3, KeepAspect = 4, KeepAspectCentered = 5, KeepAspectCovered = 6 }

    private Texture2D? _texture;
    private ExpandModeEnum _expand;
    private StretchModeEnum _stretch = StretchModeEnum.Scale;
    private bool _flipH, _flipV;

    public TextureRect() { MouseFilter = MouseFilterEnum.Pass; }

    public Texture2D? Texture { get => _texture; set { _texture = value; UpdateMinimumSize(); QueueRedraw(); } }
    public ExpandModeEnum ExpandMode { get => _expand; set { _expand = value; UpdateMinimumSize(); QueueRedraw(); } }
    public StretchModeEnum StretchMode { get => _stretch; set { _stretch = value; QueueRedraw(); } }
    public bool FlipH { get => _flipH; set { _flipH = value; QueueRedraw(); } }
    public bool FlipV { get => _flipV; set { _flipV = value; QueueRedraw(); } }

    internal override Vector2 NativeMinimumSize()
    {
        if (_texture == null) return Vector2.Zero;
        Vector2 t = _texture.GetSize();
        switch (_expand)
        {
            case ExpandModeEnum.IgnoreSize: return Vector2.Zero;
            case ExpandModeEnum.FitWidth: return new Vector2(t.X, 0);
            case ExpandModeEnum.FitWidthProportional: return new Vector2(0, t.X > 0 ? Size.X * t.Y / t.X : 0f);
            case ExpandModeEnum.FitHeight: return new Vector2(0, t.Y);
            case ExpandModeEnum.FitHeightProportional: return new Vector2(t.Y > 0 ? Size.Y * t.X / t.Y : 0f, 0);
            default: return t;
        }
    }

    /// <summary>The rectangle the texture is drawn into for the stretch mode.</summary>
    internal Rect2 DrawRectFor(Vector2 texture)
    {
        switch (_stretch)
        {
            case StretchModeEnum.Keep: return new Rect2(Vector2.Zero, texture);
            case StretchModeEnum.KeepCentered: return new Rect2((Size - texture) / 2f, texture);
            case StretchModeEnum.KeepAspect:
            case StretchModeEnum.KeepAspectCentered:
            case StretchModeEnum.KeepAspectCovered:
            {
                if (texture.X <= 0 || texture.Y <= 0) return LocalRect;
                float s = _stretch == StretchModeEnum.KeepAspectCovered ? MathF.Max(Size.X / texture.X, Size.Y / texture.Y) : MathF.Min(Size.X / texture.X, Size.Y / texture.Y);
                Vector2 d = texture * s;
                return _stretch == StretchModeEnum.KeepAspect ? new Rect2(Vector2.Zero, d) : new Rect2((Size - d) / 2f, d);
            }
            default: return LocalRect;
        }
    }

    public override void _Draw()
    {
        if (_texture == null) return;
        Vector2 t = _texture.GetSize();
        Rect2 r = DrawRectFor(t);
        if (_stretch == StretchModeEnum.KeepAspectCovered) PushClip(LocalRect);
        DrawTextureRect(_texture, r, _stretch == StretchModeEnum.Tile);
        if (_stretch == StretchModeEnum.KeepAspectCovered) PopClip();
    }
}

/// <summary>A texture drawn as a nine-patch (Godot's NinePatchRect). The picture appears once the host gives the texture an engine handle (U1's StyleBoxTexture rule).</summary>
public class NinePatchRect : Control
{
    public enum AxisStretchMode { Stretch = 0, Tile = 1, TileFit = 2 }

    public Texture2D? Texture { get; set; }
    public bool DrawCenter { get; set; } = true;
    public Rect2 RegionRect { get; set; }
    public int PatchMarginLeft { get; set; }
    public int PatchMarginTop { get; set; }
    public int PatchMarginRight { get; set; }
    public int PatchMarginBottom { get; set; }
    public AxisStretchMode AxisStretchHorizontal { get; set; }
    public AxisStretchMode AxisStretchVertical { get; set; }

    public NinePatchRect() { MouseFilter = MouseFilterEnum.Ignore; }

    public void SetPatchMargin(Side margin, int value)
    {
        switch (margin)
        {
            case Side.Left: PatchMarginLeft = value; break;
            case Side.Top: PatchMarginTop = value; break;
            case Side.Right: PatchMarginRight = value; break;
            default: PatchMarginBottom = value; break;
        }
        QueueRedraw();
    }

    public int GetPatchMargin(Side margin) => margin switch { Side.Left => PatchMarginLeft, Side.Top => PatchMarginTop, Side.Right => PatchMarginRight, _ => PatchMarginBottom };

    /// <summary>The nine-patch as a style box the engine draws.</summary>
    internal StyleBoxTexture AsStyleBox()
    {
        var box = new StyleBoxTexture
        {
            Texture = Texture, DrawCenter = DrawCenter, RegionRect = RegionRect,
            TextureMarginLeft = PatchMarginLeft, TextureMarginTop = PatchMarginTop, TextureMarginRight = PatchMarginRight, TextureMarginBottom = PatchMarginBottom,
            AxisStretchHorizontal = (StyleBoxTexture.AxisStretchMode)(int)AxisStretchHorizontal, AxisStretchVertical = (StyleBoxTexture.AxisStretchMode)(int)AxisStretchVertical,
        };
        return box;
    }

    public override void _Draw()
    {
        if (Texture != null) DrawStyleBox(AsStyleBox(), LocalRect);
    }
}

/// <summary>Base of the separators (Godot's Separator).</summary>
public class Separator : Control
{
    public Separator() { MouseFilter = MouseFilterEnum.Pass; }
}

/// <summary>A horizontal line (Godot's HSeparator).</summary>
public class HSeparator : Separator
{
    internal override Vector2 NativeMinimumSize() => new(0, Math.Max(GetThemeConstant("separation"), GetThemeStylebox("separator").GetMinimumSize().Y));

    public override void _Draw()
    {
        StyleBox s = GetThemeStylebox("separator");
        float h = Math.Max(1f, s.GetMinimumSize().Y);
        DrawStyleBox(s, new Rect2(0, (Size.Y - h) / 2f, Size.X, h));
    }
}

/// <summary>A vertical line (Godot's VSeparator).</summary>
public class VSeparator : Separator
{
    internal override Vector2 NativeMinimumSize() => new(Math.Max(GetThemeConstant("separation"), GetThemeStylebox("separator").GetMinimumSize().X), 0);

    public override void _Draw()
    {
        StyleBox s = GetThemeStylebox("separator");
        float w = Math.Max(1f, s.GetMinimumSize().X);
        DrawStyleBox(s, new Rect2((Size.X - w) / 2f, 0, w, Size.Y));
    }
}
