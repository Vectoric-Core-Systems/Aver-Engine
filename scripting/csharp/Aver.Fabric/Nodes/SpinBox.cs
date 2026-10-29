// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// SpinBox (G5 U2): a Range with a LineEdit and up / down arrows. Typing a number and pressing Enter (or leaving the field) sets the value; the arrows, the wheel
// and the up / down keys step it by the step (or custom_arrow_step); the text shows prefix, the value at the step's precision, and suffix.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A number field with arrows (Godot's SpinBox).</summary>
public class SpinBox : Range
{
    private readonly LineEdit _line = new() { Name = "LineEdit" };
    private string _prefix = "", _suffix = "";
    private bool _updateOnTextChanged;
    private double _customStep;

    public SpinBox()
    {
        FocusMode = FocusModeEnum.None;
        _line.FocusMode = FocusModeEnum.All;
        _line.TextSubmitted += _ => Apply();
        _line.TextChanged += _ => { if (_updateOnTextChanged) Apply(); };
        _line.Connect(Control.SignalName.FocusExited, Callable.From(OnLineFocusLost));
        AddChild(_line);
        Refresh();
    }

    public string Prefix { get => _prefix; set { _prefix = value ?? ""; Refresh(); } }
    public string Suffix { get => _suffix; set { _suffix = value ?? ""; Refresh(); } }
    public bool Editable { get => _line.Editable; set => _line.Editable = value; }
    public bool UpdateOnTextChanged { get => _updateOnTextChanged; set => _updateOnTextChanged = value; }
    public double CustomArrowStep { get => _customStep; set => _customStep = value; }
    public bool SelectAllOnFocus { get => _line.SelectAllOnFocus; set => _line.SelectAllOnFocus = value; }
    public HorizontalAlignment Alignment { get => _line.Alignment; set => _line.Alignment = value; }

    public LineEdit GetLineEdit() => _line;

    private float ArrowWidth => 16f;

    /// <summary>The text of the field for the current value.</summary>
    internal string Format() => _prefix + (_prefix.Length > 0 ? " " : "") + Value.ToString("F" + Decimals(), CultureInfo.InvariantCulture) + (_suffix.Length > 0 ? " " : "") + _suffix;

    private void Refresh()
    {
        if (_line.Text != Format()) _line.Text = Format();
    }

    internal override void OnValueSet() => Refresh();

    internal override void OnRangeChanged() => Refresh();

    /// <summary>Reads the field's text as a number and sets the value (Godot's apply()).</summary>
    public void Apply()
    {
        string t = _line.Text.Trim();
        if (_prefix.Length > 0 && t.StartsWith(_prefix, StringComparison.Ordinal)) t = t[_prefix.Length..].Trim();
        if (_suffix.Length > 0 && t.EndsWith(_suffix, StringComparison.Ordinal)) t = t[..^_suffix.Length].Trim();
        if (double.TryParse(t, NumberStyles.Float, CultureInfo.InvariantCulture, out double v)) Value = v;
        Refresh();
    }

    private void OnLineFocusLost() => Apply();

    private void Arrow(int dir)
    {
        Apply();
        double step = _customStep > 0 ? _customStep : Step > 0 ? Step : 1;
        Value += dir * step;
        Refresh();
    }

    public override void _Notification(int what)
    {
        if (what == NotificationResized || what == NotificationReady) Layout();
    }

    private void Layout()
    {
        _line.SetPosition(Vector2.Zero);
        _line.SetSize(new Vector2(MathF.Max(0f, Size.X - ArrowWidth), Size.Y));
    }

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 m = _line.GetCombinedMinimumSize();
        return new Vector2(m.X + ArrowWidth, m.Y);
    }

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left && mb.Pressed && mb.Position.X >= Size.X - ArrowWidth:
                Arrow(mb.Position.Y < Size.Y / 2f ? 1 : -1);
                AcceptEvent();
                break;
            case InputEventMouseButton wheel when wheel.Pressed && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown && _line.HasFocus():
                Arrow(wheel.ButtonIndex == MouseButton.WheelUp ? 1 : -1);
                AcceptEvent();
                break;
        }
    }

    public override void _Draw()
    {
        Layout();
        Color c = GetThemeColor("font_color", "LineEdit");
        float x = Size.X - ArrowWidth;
        DrawStyleBox(new StyleBoxFlat { BgColor = new Color(0.2f, 0.2f, 0.2f, 0.6f) }, new Rect2(x, 0, ArrowWidth, Size.Y));
        DrawStyleBox(new StyleBoxFlat { BgColor = c }, new Rect2(x + 4f, Size.Y / 4f - 1f, ArrowWidth - 8f, 3f));
        DrawStyleBox(new StyleBoxFlat { BgColor = c }, new Rect2(x + 4f, Size.Y * 3f / 4f - 1f, ArrowWidth - 8f, 3f));
    }
}
