// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Range and the controls built on it (G5 U2): Range (min, max, step, page, exp_edit, rounded, allow_greater / lesser, value_changed), the sliders, the
// scroll bars, ProgressBar and TextureProgressBar. The arithmetic is Godot's (value snapping to the step, the page limit, the exponential ratio); the
// pointer maths follows Godot's slider (the grabber centres on the pointer within the width less one grabber).

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A control that holds a number between a minimum and a maximum (Godot's Range).</summary>
public class Range : Control
{
    public new class SignalName : Control.SignalName
    {
        public static readonly StringName ValueChanged = "value_changed";
        public static readonly StringName Changed = "changed";
    }

    public event Action<double> ValueChanged { add => Connect(SignalName.ValueChanged, Callable.From(value)); remove => Disconnect(SignalName.ValueChanged, Callable.From(value)); }

    private double _min, _max = 100, _step = 0.01, _page, _value;
    private bool _exp, _rounded, _greater, _lesser;

    public double MinValue { get => _min; set { _min = value; Revalidate(); } }
    public double MaxValue { get => _max; set { _max = Math.Max(value, _min); Revalidate(); } }
    public double Step { get => _step; set { _step = value; Revalidate(); } }
    public double Page { get => _page; set { _page = value; Revalidate(); } }
    public bool ExpEdit { get => _exp; set { _exp = value; QueueRedraw(); } }
    public bool Rounded { get => _rounded; set { _rounded = value; Revalidate(); } }
    public bool AllowGreater { get => _greater; set { _greater = value; Revalidate(); } }
    public bool AllowLesser { get => _lesser; set { _lesser = value; Revalidate(); } }

    public double Value { get => _value; set => SetValueInternal(value, true); }

    /// <summary>Sets the value without telling anyone (Godot's set_value_no_signal).</summary>
    public void SetValueNoSignal(double value) => SetValueInternal(value, false);

    private double Snap(double value)
    {
        if (_step > 0) value = Math.Round((value - _min) / _step) * _step + _min;
        if (_rounded) value = Math.Round(value);
        if (!_greater && value > _max - _page) value = _max - _page;
        if (!_lesser && value < _min) value = _min;
        return value;
    }

    // Applies the range rules to a candidate value; false when nothing changed.
    private bool SetValueInternal(double value, bool signal)
    {
        double v = Snap(value);
        if (v == _value) return false;
        _value = v;
        QueueRedraw();
        OnValueSet();
        if (signal)
        {
            EmitSignal(SignalName.ValueChanged, _value);
            OnValueChanged(_value);
        }
        return true;
    }

    private void Revalidate()
    {
        double v = Snap(_value);
        bool changed = v != _value;
        _value = v;
        EmitSignal(SignalName.Changed);
        QueueRedraw();
        UpdateMinimumSize();
        OnRangeChanged();
        if (changed)
        {
            OnValueSet();
            EmitSignal(SignalName.ValueChanged, _value);
            OnValueChanged(_value);
        }
    }

    /// <summary>The value changed (after the signal): a derived control updates what it shows.</summary>
    internal virtual void OnValueChanged(double value) { }

    internal virtual void OnValueSet() { }

    /// <summary>The minimum, maximum, step or page changed (a SpinBox formats its number to the step's precision again).</summary>
    internal virtual void OnRangeChanged() { }

    /// <summary>The value as a share of the range, 0 to 1 (exponential when exp_edit is on and the minimum is not negative).</summary>
    public double GetAsRatio()
    {
        if (_max == _min) return 1.0;
        double value = Math.Clamp(_value, _min, _max);
        if (_exp && _min >= 0)
        {
            double eMin = _min == 0 ? 0.0 : Math.Log2(_min), eMax = Math.Log2(_max);
            double v = Math.Log2(value);
            return Math.Clamp((v - eMin) / (eMax - eMin), 0, 1);
        }
        return Math.Clamp((value - _min) / (_max - _min), 0, 1);
    }

    /// <summary>Sets the value from a ratio, 0 to 1.</summary>
    public void SetAsRatio(double value)
    {
        value = Math.Clamp(value, 0, 1);
        double v;
        if (_exp && _min >= 0)
        {
            double eMin = _min == 0 ? 0.0 : Math.Log2(_min), eMax = Math.Log2(_max);
            v = Math.Pow(2, eMin + (eMax - eMin) * value);
        }
        else
        {
            double percent = (_max - _min) * value;
            v = _step > 0 ? Math.Round(percent / _step) * _step + _min : percent + _min;
        }
        Value = Math.Clamp(v, _min, _max);
    }

    /// <summary>Digits after the point the step needs (a step of 0.01 gives 2).</summary>
    internal int Decimals()
    {
        if (_step <= 0 || _step >= 1 && _step == Math.Floor(_step)) return 0;
        string s = _step.ToString("0.##########", CultureInfo.InvariantCulture);
        int dot = s.IndexOf('.');
        return dot < 0 ? 0 : s.Length - dot - 1;
    }

    /// <summary>The distance an arrow key or the wheel moves the value: the step, or a hundredth of the range without one.</summary>
    internal double KeyStep => _step > 0 ? _step : (_max - _min) / 100.0;
}

/// <summary>Base of the sliders (Godot's Slider).</summary>
public class Slider : Range
{
    public new class SignalName : Range.SignalName
    {
        public static readonly StringName DragStarted = "drag_started";
        public static readonly StringName DragEnded = "drag_ended";
    }

    public event Action DragStarted { add => Connect(SignalName.DragStarted, Callable.From(value)); remove => Disconnect(SignalName.DragStarted, Callable.From(value)); }
    public event Action<bool> DragEnded { add => Connect(SignalName.DragEnded, Callable.From(value)); remove => Disconnect(SignalName.DragEnded, Callable.From(value)); }

    private bool _dragging;
    private double _valueBeforeDrag;

    public bool Editable { get; set; } = true;
    public bool Scrollable { get; set; } = true;
    public int TickCount { get; set; }
    public bool TicksOnBorders { get; set; }

    internal virtual bool Vertical => false;

    public Slider() { FocusMode = FocusModeEnum.All; }

    internal float GrabberSize
    {
        get
        {
            int w = GetThemeIcon("grabber")?.GetWidth() ?? 0;
            return w > 0 ? w : 16;
        }
    }

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 style = GetThemeStylebox("slider").GetMinimumSize();
        float g = GrabberSize;
        return Vertical ? new Vector2(Math.Max(style.X, g), style.Y) : new Vector2(style.X, Math.Max(style.Y, g));
    }

    private double RatioAt(Vector2 pos)
    {
        float grab = GrabberSize;
        double max = (Vertical ? Size.Y : Size.X) - grab;
        if (max <= 0) return 0;
        double p = Vertical ? pos.Y : pos.X;
        return Vertical ? 1.0 - (p - grab / 2.0) / max : (p - grab / 2.0) / max;
    }

    public override void _GuiInput(InputEvent @event)
    {
        if (!Editable) return;
        switch (@event)
        {
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left:
                if (mb.Pressed)
                {
                    _dragging = true;
                    _valueBeforeDrag = Value;
                    EmitSignal(SignalName.DragStarted);
                    SetAsRatio(RatioAt(mb.Position));
                    QueueRedraw();
                }
                else if (_dragging)
                {
                    _dragging = false;
                    EmitSignal(SignalName.DragEnded, _valueBeforeDrag != Value);
                    QueueRedraw();
                }
                AcceptEvent();
                break;
            case InputEventMouseButton wheel when wheel.Pressed && Scrollable && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown or MouseButton.WheelLeft or MouseButton.WheelRight:
            {
                int dir = wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelRight ? 1 : -1;
                if (FocusMode != FocusModeEnum.None) GrabFocus();
                Value += dir * KeyStep;
                AcceptEvent();
                break;
            }
            case InputEventMouseMotion mm when _dragging:
                SetAsRatio(RatioAt(mm.Position));
                AcceptEvent();
                break;
            case InputEventKey { Pressed: true }:
                OnKey(@event);
                break;
        }
    }

    private void OnKey(InputEvent e)
    {
        string less = Vertical ? "ui_down" : "ui_left", more = Vertical ? "ui_up" : "ui_right";
        if (Gui.IsUiAction(e, less) || (Vertical ? false : Gui.IsUiAction(e, "ui_down"))) { Value -= KeyStep; AcceptEvent(); }
        else if (Gui.IsUiAction(e, more) || (Vertical ? false : Gui.IsUiAction(e, "ui_up"))) { Value += KeyStep; AcceptEvent(); }
        else if (Gui.IsUiAction(e, "ui_home")) { Value = MinValue; AcceptEvent(); }
        else if (Gui.IsUiAction(e, "ui_end")) { Value = MaxValue; AcceptEvent(); }
    }

    public override void _Draw()
    {
        StyleBox track = GetThemeStylebox("slider");
        float grab = GrabberSize;
        double ratio = GetAsRatio();
        if (Vertical)
        {
            float tw = Math.Max(1f, track.GetMinimumSize().X);
            DrawStyleBox(track, new Rect2((Size.X - tw) / 2f, 0, tw, Size.Y));
            float y = (float)((1.0 - ratio) * (Size.Y - grab));
            DrawGrabber(new Rect2((Size.X - grab) / 2f, y, grab, grab));
        }
        else
        {
            float th = Math.Max(1f, track.GetMinimumSize().Y);
            DrawStyleBox(track, new Rect2(0, (Size.Y - th) / 2f, Size.X, th));
            float x = (float)(ratio * (Size.X - grab));
            StyleBox area = GetThemeStylebox("grabber_area");
            if (x > 0f && area is not StyleBoxEmpty) DrawStyleBox(area, new Rect2(0, (Size.Y - th) / 2f, x + grab / 2f, th));
            DrawGrabber(new Rect2(x, (Size.Y - grab) / 2f, grab, grab));
        }
        if (TickCount > 1)
        {
            Texture2D? tick = GetThemeIcon("tick");
            for (int i = 0; i < TickCount && tick != null; i++)
            {
                float t = TickCount == 1 ? 0 : i / (float)(TickCount - 1);
                DrawTexture(tick, Vertical ? new Vector2(Size.X / 2f - tick.GetWidth() / 2f, t * (Size.Y - grab)) : new Vector2(t * (Size.X - grab), Size.Y / 2f - tick.GetHeight() / 2f));
            }
        }
    }

    private void DrawGrabber(Rect2 rect)
    {
        Texture2D? icon = GetThemeIcon(!Editable ? "grabber_disabled" : _dragging || HasFocus() ? "grabber_highlight" : "grabber");
        if (icon != null && icon.EngineHandle != 0) { DrawTexture(icon, rect.Position); return; }
        // the default theme carries no grabber image: a round flat knob in the font colour
        var knob = new StyleBoxFlat { BgColor = !Editable ? new Color(0.6f, 0.6f, 0.6f, 0.5f) : _dragging ? new Color(1, 1, 1, 1) : new Color(0.875f, 0.875f, 0.875f, 1) };
        knob.SetCornerRadiusAll((int)(rect.Size.X / 2f));
        DrawStyleBox(knob, rect);
    }
}

/// <summary>A horizontal slider (Godot's HSlider).</summary>
public class HSlider : Slider { }

/// <summary>A vertical slider (Godot's VSlider).</summary>
public class VSlider : Slider
{
    internal override bool Vertical => true;
}

/// <summary>Base of the scroll bars (Godot's ScrollBar): a range whose page is the visible part of the content.</summary>
public class ScrollBar : Range
{
    public new class SignalName : Range.SignalName
    {
        public static readonly StringName Scrolling = "scrolling";
    }

    public event Action Scrolling { add => Connect(SignalName.Scrolling, Callable.From(value)); remove => Disconnect(SignalName.Scrolling, Callable.From(value)); }

    private bool _grab;
    private float _grabOffset;

    public float CustomStep { get; set; } = -1f;

    internal virtual bool Vertical => false;

    public ScrollBar() { Step = 1; MaxValue = 100; Page = 0; }

    private float Along => Vertical ? Size.Y : Size.X;

    // Length and start of the grabber along the bar.
    internal (float Start, float Length) Grabber()
    {
        double range = MaxValue - MinValue;
        float along = Along;
        if (range <= 0) return (0, along);
        float length = Math.Max(10f, (float)(along * (Page / range)));
        length = Math.Min(length, along);
        double scrollable = range - Page;
        float start = scrollable <= 0 ? 0 : (float)((Value - MinValue) / scrollable * (along - length));
        return (start, length);
    }

    internal override Vector2 NativeMinimumSize()
    {
        int w = Math.Max(1, GetThemeConstant("scrollbar_size"));
        return Vertical ? new Vector2(w, 0) : new Vector2(0, w);
    }

    private double ValueAt(float p, float length)
    {
        double scrollable = MaxValue - MinValue - Page;
        float room = Along - length;
        if (scrollable <= 0 || room <= 0) return MinValue;
        return MinValue + Math.Clamp(p / room, 0, 1) * scrollable;
    }

    public override void _GuiInput(InputEvent @event)
    {
        (float start, float length) = Grabber();
        switch (@event)
        {
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left:
                if (mb.Pressed)
                {
                    float p = Vertical ? mb.Position.Y : mb.Position.X;
                    if (p >= start && p <= start + length) { _grab = true; _grabOffset = p - start; }
                    else
                    {
                        Value += p < start ? -Math.Max(Page, 1) : Math.Max(Page, 1);
                        EmitSignal(SignalName.Scrolling);
                    }
                }
                else _grab = false;
                QueueRedraw();
                AcceptEvent();
                break;
            case InputEventMouseButton wheel when wheel.Pressed && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown:
                Value += (wheel.ButtonIndex == MouseButton.WheelUp ? -1 : 1) * (CustomStep >= 0 ? CustomStep : Math.Max(Page / 8.0, 1));
                EmitSignal(SignalName.Scrolling);
                AcceptEvent();
                break;
            case InputEventMouseMotion mm when _grab:
                Value = ValueAt((Vertical ? mm.Position.Y : mm.Position.X) - _grabOffset, length);
                EmitSignal(SignalName.Scrolling);
                AcceptEvent();
                break;
        }
    }

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("scroll"), LocalRect);
        (float start, float length) = Grabber();
        StyleBox g = GetThemeStylebox(_grab ? "grabber_pressed" : "grabber");
        DrawStyleBox(g, Vertical ? new Rect2(0, start, Size.X, length) : new Rect2(start, 0, length, Size.Y));
    }
}

public class HScrollBar : ScrollBar { }

public class VScrollBar : ScrollBar
{
    internal override bool Vertical => true;
}

/// <summary>A bar that fills with the value (Godot's ProgressBar).</summary>
public class ProgressBar : Range
{
    public enum FillModeEnum { BeginToEnd = 0, EndToBegin = 1, TopToBottom = 2, BottomToTop = 3 }

    public bool ShowPercentage { get; set; } = true;
    public FillModeEnum FillMode { get; set; }
    public bool Indeterminate { get; set; }
    public bool EditorPreviewIndeterminate { get; set; } = true;

    public ProgressBar() { Step = 0.01; Value = 0; MouseFilter = MouseFilterEnum.Pass; }

    /// <summary>The text shown: the whole percentage of the way from the minimum to the maximum.</summary>
    internal string PercentText => ((int)(GetAsRatio() * 100)).ToString(CultureInfo.InvariantCulture) + "%";

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 bg = GetThemeStylebox("background").GetMinimumSize(), fill = GetThemeStylebox("fill").GetMinimumSize();
        var min = new Vector2(Math.Max(bg.X, fill.X), Math.Max(bg.Y, fill.Y));
        if (ShowPercentage)
            min.Y = Math.Max(min.Y, bg.Y + UiText.LineHeight(ThemeFont(), ThemeFontSize()));
        else
        {
            min.X = Math.Max(min.X, 1);
            min.Y = Math.Max(min.Y, 1);
        }
        return min;
    }

    /// <summary>The part of the bar the fill covers, in the control's space.</summary>
    internal Rect2 FillRect()
    {
        StyleBox bg = GetThemeStylebox("background");
        Rect2 inner = bg.GetContentRect(LocalRect);
        float r = (float)GetAsRatio();
        switch (FillMode)
        {
            case FillModeEnum.EndToBegin: return new Rect2(inner.End.X - inner.Size.X * r, inner.Position.Y, inner.Size.X * r, inner.Size.Y);
            case FillModeEnum.TopToBottom: return new Rect2(inner.Position.X, inner.Position.Y, inner.Size.X, inner.Size.Y * r);
            case FillModeEnum.BottomToTop: return new Rect2(inner.Position.X, inner.End.Y - inner.Size.Y * r, inner.Size.X, inner.Size.Y * r);
            default: return new Rect2(inner.Position.X, inner.Position.Y, inner.Size.X * r, inner.Size.Y);
        }
    }

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("background"), LocalRect);
        Rect2 fill = FillRect();
        StyleBox fillBox = GetThemeStylebox("fill");
        if (fill.Size.X > 0 && fill.Size.Y > 0) DrawStyleBox(fillBox, fill);
        if (!ShowPercentage) return;
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        string text = PercentText;
        float w = UiText.Width(font, px, text), h = UiText.LineHeight(font, px);
        DrawTextLine(new Vector2((Size.X - w) / 2f, (Size.Y - h) / 2f), text, font, px, GetThemeColor("font_color"), ThemeTextFx());
    }
}

/// <summary>A bar drawn from textures (Godot's TextureProgressBar): an under texture, a progress texture revealed by the value, and an over texture.</summary>
public class TextureProgressBar : Range
{
    public enum FillModeEnum { LeftToRight = 0, RightToLeft = 1, TopToBottom = 2, BottomToTop = 3, Clockwise = 4, CounterClockwise = 5, BilinearLeftAndRight = 6, BilinearTopAndBottom = 7, ClockwiseAndCounterClockwise = 8 }

    private FillModeEnum _mode;

    public FillModeEnum FillMode { get => _mode; set { _mode = value; QueueRedraw(); } }
    public Texture2D? TextureUnder { get; set; }
    public Texture2D? TextureOver { get; set; }
    public Texture2D? TextureProgress { get; set; }
    public Vector2 TextureProgressOffset { get; set; }
    public Color TintUnder { get; set; } = new(1, 1, 1, 1);
    public Color TintOver { get; set; } = new(1, 1, 1, 1);
    public Color TintProgress { get; set; } = new(1, 1, 1, 1);
    public float RadialInitialAngle { get; set; }
    public float RadialFillDegrees { get; set; } = 360f;
    public Vector2 RadialCenterOffset { get; set; }
    public bool NinePatchStretch { get; set; }
    public int StretchMarginLeft { get; set; }
    public int StretchMarginTop { get; set; }
    public int StretchMarginRight { get; set; }
    public int StretchMarginBottom { get; set; }

    public TextureProgressBar() { Step = 1; }

    internal override Vector2 NativeMinimumSize()
    {
        Texture2D? t = TextureUnder ?? TextureOver ?? TextureProgress;
        return NinePatchStretch || t == null ? Vector2.Zero : t.GetSize();
    }

    /// <summary>The part of the progress texture shown for linear fills, as a share of the texture (x, y, width, height from 0 to 1). A radial fill is
    /// approximated by the left-to-right fill with the same ratio.</summary>
    public Rect2 GetFillRegion()
    {
        float r = (float)GetAsRatio();
        switch (_mode)
        {
            case FillModeEnum.RightToLeft: return new Rect2(1f - r, 0, r, 1);
            case FillModeEnum.TopToBottom: return new Rect2(0, 0, 1, r);
            case FillModeEnum.BottomToTop: return new Rect2(0, 1f - r, 1, r);
            case FillModeEnum.BilinearLeftAndRight: return new Rect2(0.5f - r / 2f, 0, r, 1);
            case FillModeEnum.BilinearTopAndBottom: return new Rect2(0, 0.5f - r / 2f, 1, r);
            default: return new Rect2(0, 0, r, 1);
        }
    }

    /// <summary>The start and sweep of a radial fill in degrees (Godot measures clockwise from the top).</summary>
    public (float Start, float Sweep) GetRadialAngles()
    {
        float sweep = (float)GetAsRatio() * RadialFillDegrees;
        return _mode == FillModeEnum.CounterClockwise ? (RadialInitialAngle - sweep, sweep) : (RadialInitialAngle, sweep);
    }

    private bool Radial => _mode is FillModeEnum.Clockwise or FillModeEnum.CounterClockwise or FillModeEnum.ClockwiseAndCounterClockwise;

    public override void _Draw()
    {
        var all = new Rect2(Vector2.Zero, NinePatchStretch ? Size : (TextureUnder ?? TextureProgress)?.GetSize() ?? Size);
        if (TextureUnder != null) DrawTextureRect(TextureUnder, all, false, TintUnder);
        if (TextureProgress != null)
        {
            Vector2 ts = TextureProgress.GetSize();
            Rect2 region = Radial ? new Rect2(0, 0, (float)GetAsRatio(), 1) : GetFillRegion();
            var dest = new Rect2(TextureProgressOffset + new Vector2(ts.X * region.Position.X, ts.Y * region.Position.Y), new Vector2(ts.X * region.Size.X, ts.Y * region.Size.Y));
            if (NinePatchStretch) dest = new Rect2(all.Position + new Vector2(all.Size.X * region.Position.X, all.Size.Y * region.Position.Y), new Vector2(all.Size.X * region.Size.X, all.Size.Y * region.Size.Y));
            DrawTextureRectRegion(TextureProgress, dest, new Rect2(ts.X * region.Position.X, ts.Y * region.Position.Y, ts.X * region.Size.X, ts.Y * region.Size.Y), TintProgress);
        }
        if (TextureOver != null) DrawTextureRect(TextureOver, all, false, TintOver);
    }
}
