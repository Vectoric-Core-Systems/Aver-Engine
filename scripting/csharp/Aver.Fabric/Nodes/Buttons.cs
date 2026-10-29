// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Buttons (G5 U2): BaseButton with Godot's press logic (button_down, pressed on release or on press by action_mode, toggle_mode, button groups, keyboard
// accept, shortcuts, the draw mode), Button, CheckBox, CheckButton, LinkButton, TextureButton and ButtonGroup. A button's look comes from the theme under its own
// class name (Button/styles/normal, hover, pressed, ...). The default theme carries no icon images, so a check mark or radio dot is drawn as a flat box.

namespace Aver.Fabric;

/// <summary>A set of buttons of which one at a time is pressed (Godot's ButtonGroup).</summary>
public class ButtonGroup : Resource
{
    public new class SignalName : Resource.SignalName
    {
        public static readonly StringName Pressed = "pressed";
    }

    public event Action<BaseButton> Pressed { add => Connect(SignalName.Pressed, Callable.From(value)); remove => Disconnect(SignalName.Pressed, Callable.From(value)); }

    internal readonly List<BaseButton> Members = new();

    /// <summary>Whether the pressed button of the group can be pressed again to leave none pressed.</summary>
    public bool AllowUnpress { get; set; }

    public BaseButton? GetPressedButton()
    {
        foreach (BaseButton b in Members) if (b.ButtonPressed) return b;
        return null;
    }

    public Collections.Array<BaseButton> GetButtons()
    {
        var a = new Collections.Array<BaseButton>();
        foreach (BaseButton b in Members) a.Add(b);
        return a;
    }
}

/// <summary>Base of every button (Godot's BaseButton).</summary>
public class BaseButton : Control
{
    public enum DrawMode { Normal = 0, Pressed = 1, Hover = 2, Disabled = 3, HoverPressed = 4 }

    public enum ActionModeEnum { ButtonPress = 0, ButtonRelease = 1 }

    public new class SignalName : Control.SignalName
    {
        public static readonly StringName Pressed = "pressed";
        public static readonly StringName ButtonDown = "button_down";
        public static readonly StringName ButtonUp = "button_up";
        public static readonly StringName Toggled = "toggled";
    }

    public event Action Pressed { add => Connect(SignalName.Pressed, Callable.From(value)); remove => Disconnect(SignalName.Pressed, Callable.From(value)); }
    public event Action ButtonDown { add => Connect(SignalName.ButtonDown, Callable.From(value)); remove => Disconnect(SignalName.ButtonDown, Callable.From(value)); }
    public event Action ButtonUp { add => Connect(SignalName.ButtonUp, Callable.From(value)); remove => Disconnect(SignalName.ButtonUp, Callable.From(value)); }
    public event Action<bool> Toggled { add => Connect(SignalName.Toggled, Callable.From(value)); remove => Disconnect(SignalName.Toggled, Callable.From(value)); }

    private bool _disabled, _toggle, _pressed, _hovering, _pressAttempt, _pressingInside;
    private ButtonGroup? _group;
    private Shortcut? _shortcut;

    public BaseButton()
    {
        FocusMode = FocusModeEnum.All;
        SetProcessUnhandledKeyInput(false);
    }

    public ActionModeEnum ActionMode { get; set; } = ActionModeEnum.ButtonRelease;
    public MouseButtonMask ButtonMask { get; set; } = MouseButtonMask.Left;
    public bool KeepPressedOutside { get; set; }
    public bool ShortcutFeedback { get; set; } = true;
    public bool ShortcutInTooltip { get; set; } = true;

    public bool Disabled
    {
        get => _disabled;
        set
        {
            if (_disabled == value) return;
            _disabled = value;
            if (value) { _pressAttempt = false; _pressingInside = false; }
            QueueRedraw();
        }
    }

    public bool ToggleMode { get => _toggle; set { _toggle = value; QueueRedraw(); } }

    /// <summary>Whether a toggle button is pressed. Setting it emits toggled when it changes (Godot's set_pressed).</summary>
    public bool ButtonPressed
    {
        get => _pressed;
        set
        {
            if (!_toggle || _pressed == value) return;
            _pressed = value;
            if (value)
            {
                UnpressGroup();
                _group?.EmitSignal(ButtonGroup.SignalName.Pressed, this);
            }
            EmitSignal(SignalName.Toggled, _pressed);
            QueueRedraw();
        }
    }

    /// <summary>Sets the pressed state without any signal.</summary>
    public void SetPressedNoSignal(bool pressed)
    {
        if (!_toggle || _pressed == pressed) return;
        _pressed = pressed;
        QueueRedraw();
    }

    public ButtonGroup? ButtonGroup
    {
        get => _group;
        set
        {
            if (_group == value) return;
            _group?.Members.Remove(this);
            _group = value;
            _group?.Members.Add(this);
            QueueRedraw();
        }
    }

    public Shortcut? Shortcut
    {
        get => _shortcut;
        set
        {
            _shortcut = value;
            SetProcessUnhandledKeyInput(value != null);
        }
    }

    public bool IsPressed() => _pressed;
    public bool IsHovered() => _hovering;

    public override void _UnhandledKeyInput(InputEvent @event)
    {
        if (_disabled || _shortcut == null || @event is not InputEventKey { Pressed: true, Echo: false } || !IsVisibleInTree()) return;
        if (_shortcut.MatchesEvent(@event)) { Trigger(true); Trigger(false); GetViewport()?.SetInputAsHandled(); }
    }

    /// <summary>A script's reaction to a press (Godot's _pressed).</summary>
    public virtual void _Pressed() { }

    /// <summary>A script's reaction to a toggle (Godot's _toggled).</summary>
    public virtual void _Toggled(bool toggledOn) { }

    private void UnpressGroup()
    {
        if (_group == null) return;
        if (_toggle && !_group.AllowUnpress) _pressed = true;
        foreach (BaseButton other in _group.Members.ToArray())
            if (other != this) other.ButtonPressed = false;
    }

    private void FirePressed()
    {
        _Pressed();
        EmitSignal(SignalName.Pressed);
    }

    // Godot's on_action_event: a press or release of the button (mouse or accept key).
    private void Trigger(bool pressed)
    {
        if (pressed)
        {
            _pressAttempt = true;
            _pressingInside = true;
            EmitSignal(SignalName.ButtonDown);
        }
        if (_pressAttempt && _pressingInside)
        {
            if (_toggle)
            {
                if ((pressed && ActionMode == ActionModeEnum.ButtonPress) || (!pressed && ActionMode == ActionModeEnum.ButtonRelease))
                {
                    if (ActionMode == ActionModeEnum.ButtonPress) { _pressAttempt = false; _pressingInside = false; }
                    _pressed = !_pressed;
                    UnpressGroup();
                    _group?.EmitSignal(ButtonGroup.SignalName.Pressed, this);
                    _Toggled(_pressed);
                    EmitSignal(SignalName.Toggled, _pressed);
                    FirePressed();
                }
            }
            else if ((pressed && ActionMode == ActionModeEnum.ButtonPress) || (!pressed && ActionMode == ActionModeEnum.ButtonRelease))
                FirePressed();
        }
        if (!pressed)
        {
            _pressAttempt = false;
            _pressingInside = false;
            EmitSignal(SignalName.ButtonUp);
        }
        QueueRedraw();
    }

    public override void _GuiInput(InputEvent @event)
    {
        if (_disabled) return;
        switch (@event)
        {
            case InputEventMouseButton mb:
            {
                int bit = mb.ButtonIndex switch { MouseButton.Left => 1, MouseButton.Right => 2, MouseButton.Middle => 4, _ => 0 };
                if (bit == 0 || ((int)ButtonMask & bit) == 0) return;
                if (mb.Pressed) Trigger(true);
                else
                {
                    if (!_pressAttempt) return;
                    bool inside = HasPoint(mb.Position);
                    if (!inside) _hovering = false;
                    _pressingInside = inside;
                    Trigger(false);
                }
                AcceptEvent();
                break;
            }
            case InputEventMouseMotion mm:
                if (_pressAttempt)
                {
                    bool last = _pressingInside;
                    _pressingInside = HasPoint(mm.Position);
                    if (last != _pressingInside) QueueRedraw();
                }
                break;
            case InputEventKey k:
                if (k.Pressed && !k.Echo && Gui.IsUiAction(k, "ui_accept")) { Trigger(true); AcceptEvent(); }
                else if (!k.Pressed && _pressAttempt && k.Keycode is Key.Enter or Key.KpEnter or Key.Space) { Trigger(false); AcceptEvent(); }
                break;
        }
    }

    public override void _Notification(int what)
    {
        switch (what)
        {
            case NotificationMouseEnter: _hovering = true; QueueRedraw(); break;
            case NotificationMouseExit: _hovering = false; QueueRedraw(); break;
            case NotificationFocusExit:
                if (_pressAttempt) { _pressAttempt = false; _pressingInside = false; QueueRedraw(); }
                break;
        }
    }

    /// <summary>Which of the looks the button has now.</summary>
    public DrawMode GetDrawMode()
    {
        if (_disabled) return DrawMode.Disabled;
        if (!_pressAttempt && _hovering) return _pressed ? DrawMode.HoverPressed : DrawMode.Hover;
        bool pressing;
        if (_pressAttempt)
        {
            pressing = _pressingInside || KeepPressedOutside;
            if (_pressed) pressing = !pressing;
        }
        else pressing = _pressed;
        return pressing ? DrawMode.Pressed : DrawMode.Normal;
    }

    /// <summary>The style box name for a draw mode; hover_pressed falls back to pressed when the theme has none.</summary>
    internal StyleBox StyleFor(DrawMode mode, string normal = "normal")
    {
        string n = mode switch
        {
            DrawMode.Pressed => "pressed", DrawMode.Hover => "hover", DrawMode.Disabled => "disabled", DrawMode.HoverPressed => "hover_pressed", _ => normal,
        };
        if (mode == DrawMode.HoverPressed && !HasThemeStylebox(n)) n = "pressed";
        return GetThemeStylebox(n);
    }
}

/// <summary>A text and icon button (Godot's Button).</summary>
public class Button : BaseButton
{
    private string _text = "";
    private Texture2D? _icon;
    private bool _flat, _clip, _expandIcon;
    private HorizontalAlignment _align = HorizontalAlignment.Center, _iconAlign = HorizontalAlignment.Left;
    private VerticalAlignment _vIconAlign = VerticalAlignment.Center;
    private TextServer.AutowrapMode _wrap = TextServer.AutowrapMode.Off;
    private TextServer.OverrunBehavior _overrun = TextServer.OverrunBehavior.NoTrimming;

    public string Text { get => _text; set { _text = value ?? ""; UpdateMinimumSize(); QueueRedraw(); } }
    public Texture2D? Icon { get => _icon; set { _icon = value; UpdateMinimumSize(); QueueRedraw(); } }
    public bool Flat { get => _flat; set { _flat = value; QueueRedraw(); } }
    public HorizontalAlignment Alignment { get => _align; set { _align = value; QueueRedraw(); } }
    public HorizontalAlignment IconAlignment { get => _iconAlign; set { _iconAlign = value; QueueRedraw(); } }
    public VerticalAlignment VerticalIconAlignment { get => _vIconAlign; set { _vIconAlign = value; QueueRedraw(); } }
    public bool ExpandIcon { get => _expandIcon; set { _expandIcon = value; UpdateMinimumSize(); QueueRedraw(); } }
    public bool ClipText { get => _clip; set { _clip = value; UpdateMinimumSize(); QueueRedraw(); } }
    public TextServer.OverrunBehavior TextOverrunBehavior { get => _overrun; set { _overrun = value; UpdateMinimumSize(); QueueRedraw(); } }
    public TextServer.AutowrapMode AutowrapMode { get => _wrap; set { _wrap = value; UpdateMinimumSize(); QueueRedraw(); } }
    public string Language { get; set; } = "";
    public new TextServer.Direction TextDirection { get; set; } = TextServer.Direction.Inherited;
    public int AutowrapTrimFlags { get; set; } = 192;

    /// <summary>The theme class the button's items are found under (a derived button asks the same names of its base).</summary>
    internal virtual bool HasCheck => false;

    internal virtual Vector2 CheckSize => Vector2.Zero;

    internal virtual bool CheckOnRight => false;

    private float IconMaxWidth => GetThemeConstant("icon_max_width");

    private Vector2 IconSize()
    {
        if (_icon == null) return Vector2.Zero;
        Vector2 s = _icon.GetSize();
        float max = IconMaxWidth;
        if (max > 0 && s.X > max) s *= max / s.X;
        return s;
    }

    internal override Vector2 NativeMinimumSize()
    {
        StyleBox style = GetThemeStylebox("normal");
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        TextLines lines = TextLines.Build(_text, font, px, 0, TextLines.Wrap.Off);
        float sep = GetThemeConstant("h_separation");
        float w = _clip || _overrun != TextServer.OverrunBehavior.NoTrimming || _wrap != TextServer.AutowrapMode.Off ? 0f : lines.MaxWidth();
        float h = _text.Length == 0 && _icon != null ? 0f : lines.Count * lines.LineHeight;
        Vector2 icon = IconSize();
        if (_icon != null)
        {
            w += icon.X + (_text.Length > 0 && w > 0 ? sep : 0f);
            h = Math.Max(h, icon.Y);
        }
        if (HasCheck)
        {
            Vector2 cs = CheckSize;
            w += cs.X + (w > 0 ? sep : 0f);
            h = Math.Max(h, cs.Y);
        }
        return new Vector2(w, h) + style.GetMinimumSize();
    }

    private Color FontColorFor(DrawMode mode)
    {
        return mode switch
        {
            DrawMode.Disabled => GetThemeColor("font_disabled_color"),
            DrawMode.HoverPressed => HasThemeColor("font_hover_pressed_color") ? GetThemeColor("font_hover_pressed_color") : GetThemeColor("font_pressed_color"),
            DrawMode.Pressed => GetThemeColor("font_pressed_color"),
            DrawMode.Hover => GetThemeColor("font_hover_color"),
            _ => HasFocus() && HasThemeColor("font_focus_color") ? GetThemeColor("font_focus_color") : GetThemeColor("font_color"),
        };
    }

    private Color IconColorFor(DrawMode mode)
    {
        string n = mode switch { DrawMode.Disabled => "icon_disabled_color", DrawMode.Pressed => "icon_pressed_color", DrawMode.HoverPressed => "icon_hover_pressed_color", DrawMode.Hover => "icon_hover_color", _ => "icon_normal_color" };
        return HasThemeColor(n) ? GetThemeColor(n) : new Color(1, 1, 1, 1);
    }

    internal virtual void DrawCheck(Rect2 box, DrawMode mode) { }

    internal virtual StyleBox BackgroundFor(DrawMode mode) => StyleFor(mode);

    public override void _Draw()
    {
        DrawMode mode = GetDrawMode();
        StyleBox style = BackgroundFor(mode);
        if (!_flat || mode is DrawMode.Pressed or DrawMode.HoverPressed) DrawStyleBox(style, LocalRect);
        if (HasFocus() && !Disabled) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        Rect2 content = style.GetContentRect(LocalRect);
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float sep = GetThemeConstant("h_separation");
        float wrapW = _wrap != TextServer.AutowrapMode.Off ? content.Size.X : 0f;
        TextLines lines = TextLines.Build(_text, font, px, wrapW, TextServer.WrapOf(_wrap));
        Vector2 icon = IconSize();
        float checkW = HasCheck ? CheckSize.X : 0f;
        float textW = lines.MaxWidth();
        float lineH = lines.LineHeight;
        float textH = lines.Count * lineH;
        float gap = _text.Length > 0 ? sep : 0f;
        float room = content.Size.X - icon.X - (icon.X > 0 ? gap : 0f) - checkW - (checkW > 0 ? sep : 0f);
        bool overrun = _clip || _overrun != TextServer.OverrunBehavior.NoTrimming;
        if (overrun) textW = Math.Min(textW, Math.Max(0f, room));
        float groupW = icon.X + (icon.X > 0 && textW > 0 ? gap : 0f) + textW + checkW + (checkW > 0 && textW > 0 ? sep : 0f);
        float x = content.Position.X;
        if (_align == HorizontalAlignment.Center) x += (content.Size.X - groupW) / 2f;
        else if (_align == HorizontalAlignment.Right) x += content.Size.X - groupW;
        x = Math.Max(content.Position.X, x);
        float cy = content.Position.Y + content.Size.Y / 2f;
        if (HasCheck && !CheckOnRight)
        {
            DrawCheck(new Rect2(x, cy - CheckSize.Y / 2f, CheckSize.X, CheckSize.Y), mode);
            x += checkW + sep;
        }
        bool iconFirst = _iconAlign != HorizontalAlignment.Right;
        if (_icon != null && iconFirst)
        {
            DrawTextureRect(_icon, new Rect2(x, cy - icon.Y / 2f, icon.X, icon.Y), false, IconColorFor(mode));
            x += icon.X + gap;
        }
        if (_text.Length > 0)
        {
            Color color = FontColorFor(mode);
            CanvasItem.TextFx fx = ThemeTextFx();
            float y = cy - textH / 2f;
            if (overrun) PushClip(content);
            for (int i = 0; i < lines.Count; i++)
            {
                string seg = lines.LineText(i);
                float w = lines.Width(i);
                if (overrun && w > textW)
                {
                    bool ell = _overrun >= TextServer.OverrunBehavior.TrimEllipsis;
                    seg = TextLines.Trim(seg, font, px, textW, _overrun is TextServer.OverrunBehavior.TrimWord or TextServer.OverrunBehavior.TrimWordEllipsis, ell ? "…" : "");
                    w = UiText.Width(font, px, seg);
                }
                float lx = x;
                if (_align == HorizontalAlignment.Center) lx += (textW - w) / 2f;
                else if (_align == HorizontalAlignment.Right) lx += textW - w;
                DrawTextLine(new Vector2(lx, y + i * lineH), seg, font, px, color, fx);
            }
            if (overrun) PopClip();
            x += textW;
        }
        if (_icon != null && !iconFirst)
        {
            x += gap;
            DrawTextureRect(_icon, new Rect2(x, cy - icon.Y / 2f, icon.X, icon.Y), false, IconColorFor(mode));
            x += icon.X;
        }
        if (HasCheck && CheckOnRight)
            DrawCheck(new Rect2(content.End.X - CheckSize.X - (_align == HorizontalAlignment.Right ? 0f : 0f), cy - CheckSize.Y / 2f, CheckSize.X, CheckSize.Y), mode);
    }
}

/// <summary>A button with a check box (Godot's CheckBox): a toggle whose box shows the state; in a button group it is a radio button.</summary>
public class CheckBox : Button
{
    public CheckBox()
    {
        ToggleMode = true;
        Alignment = HorizontalAlignment.Left;
    }

    internal override bool HasCheck => true;

    internal override Vector2 CheckSize
    {
        get
        {
            Texture2D? t = CheckIcon();
            return t != null && t.GetWidth() > 0 ? t.GetSize() : new Vector2(16, 16);
        }
    }

    private Texture2D? CheckIcon()
    {
        bool radio = ButtonGroup != null;
        string name = Disabled ? (radio ? (IsPressed() ? "radio_checked_disabled" : "radio_unchecked_disabled") : (IsPressed() ? "checked_disabled" : "unchecked_disabled"))
                               : (radio ? (IsPressed() ? "radio_checked" : "radio_unchecked") : (IsPressed() ? "checked" : "unchecked"));
        return GetThemeIcon(name);
    }

    internal override StyleBox BackgroundFor(DrawMode mode) => StyleFor(mode == DrawMode.Pressed ? DrawMode.Normal : mode == DrawMode.HoverPressed ? DrawMode.Hover : mode);

    internal override void DrawCheck(Rect2 box, DrawMode mode)
    {
        Texture2D? t = CheckIcon();
        if (t != null && t.EngineHandle != 0) { DrawTextureRect(t, box, false); return; }
        bool radio = ButtonGroup != null;
        var frame = new StyleBoxFlat { BgColor = new Color(0.1f, 0.1f, 0.1f, 0.6f), BorderColor = new Color(0.875f, 0.875f, 0.875f, Disabled ? 0.5f : 1f) };
        frame.SetBorderWidthAll(1);
        frame.SetCornerRadiusAll(radio ? (int)(box.Size.X / 2f) : 2);
        DrawStyleBox(frame, box);
        if (IsPressed())
        {
            var mark = new StyleBoxFlat { BgColor = new Color(0.95f, 0.95f, 0.95f, Disabled ? 0.5f : 1f) };
            mark.SetCornerRadiusAll(radio ? (int)(box.Size.X / 4f) : 1);
            DrawStyleBox(mark, box.Grow(-4f));
        }
    }
}

/// <summary>A toggle drawn as a switch (Godot's CheckButton): the text on the left, the switch on the right.</summary>
public class CheckButton : Button
{
    public CheckButton()
    {
        ToggleMode = true;
        Alignment = HorizontalAlignment.Left;
    }

    internal override bool HasCheck => true;
    internal override bool CheckOnRight => true;

    internal override Vector2 CheckSize
    {
        get
        {
            Texture2D? t = GetThemeIcon(IsPressed() ? "checked" : "unchecked");
            return t != null && t.GetWidth() > 0 ? t.GetSize() : new Vector2(38, 18);
        }
    }

    internal override StyleBox BackgroundFor(DrawMode mode) => StyleFor(mode == DrawMode.Pressed ? DrawMode.Normal : mode == DrawMode.HoverPressed ? DrawMode.Hover : mode);

    internal override void DrawCheck(Rect2 box, DrawMode mode)
    {
        Texture2D? t = GetThemeIcon(IsPressed() ? "checked" : "unchecked");
        if (t != null && t.EngineHandle != 0) { DrawTextureRect(t, box, false); return; }
        var track = new StyleBoxFlat { BgColor = IsPressed() ? new Color(0.3f, 0.6f, 0.9f, Disabled ? 0.5f : 1f) : new Color(0.3f, 0.3f, 0.3f, Disabled ? 0.5f : 1f) };
        track.SetCornerRadiusAll((int)(box.Size.Y / 2f));
        DrawStyleBox(track, box);
        float k = box.Size.Y - 4f;
        var knob = new StyleBoxFlat { BgColor = new Color(0.95f, 0.95f, 0.95f, 1f) };
        knob.SetCornerRadiusAll((int)(k / 2f));
        DrawStyleBox(knob, new Rect2(IsPressed() ? box.End.X - k - 2f : box.Position.X + 2f, box.Position.Y + 2f, k, k));
    }
}

/// <summary>A button that looks like a hyperlink (Godot's LinkButton).</summary>
public class LinkButton : BaseButton
{
    public enum UnderlineModeEnum { Always = 0, OnHover = 1, Never = 2 }

    private string _text = "";

    public string Text { get => _text; set { _text = value ?? ""; UpdateMinimumSize(); QueueRedraw(); } }
    public UnderlineModeEnum Underline { get; set; } = UnderlineModeEnum.Always;
    public string Uri { get; set; } = "";
    public string Language { get; set; } = "";
    public new TextServer.Direction TextDirection { get; set; } = TextServer.Direction.Inherited;

    public LinkButton() { MouseDefaultCursorShape = CursorShape.PointingHand; }

    internal override Vector2 NativeMinimumSize()
    {
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        return new Vector2(UiText.Width(font, px, _text), UiText.LineHeight(font, px));
    }

    public override void _Draw()
    {
        DrawMode mode = GetDrawMode();
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        Color color = mode switch
        {
            DrawMode.Disabled => GetThemeColor("font_disabled_color"),
            DrawMode.Pressed or DrawMode.HoverPressed => GetThemeColor("font_pressed_color"),
            DrawMode.Hover => GetThemeColor("font_hover_color"),
            _ => HasFocus() ? GetThemeColor("font_focus_color") : GetThemeColor("font_color"),
        };
        if (HasFocus()) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        DrawTextLine(Vector2.Zero, _text, font, px, color, ThemeTextFx());
        bool under = Underline == UnderlineModeEnum.Always || (Underline == UnderlineModeEnum.OnHover && IsHovered());
        if (under)
        {
            float y = UiText.LineHeight(font, px) + GetThemeConstant("underline_spacing") - 1f;
            DrawRect(new Rect2(0, y, UiText.Width(font, px, _text), Math.Max(1f, px / 16f)), color);
        }
    }
}

/// <summary>A button drawn from textures (Godot's TextureButton).</summary>
public class TextureButton : BaseButton
{
    public enum StretchModeEnum { Scale = 0, Tile = 1, Keep = 2, KeepCentered = 3, KeepAspect = 4, KeepAspectCentered = 5, KeepAspectCovered = 6 }

    private Texture2D? _normal, _pressed, _hover, _disabled, _focused;
    private bool _ignoreSize;

    public Texture2D? TextureNormal { get => _normal; set { _normal = value; UpdateMinimumSize(); QueueRedraw(); } }
    public Texture2D? TexturePressed { get => _pressed; set { _pressed = value; UpdateMinimumSize(); QueueRedraw(); } }
    public Texture2D? TextureHover { get => _hover; set { _hover = value; UpdateMinimumSize(); QueueRedraw(); } }
    public Texture2D? TextureDisabled { get => _disabled; set { _disabled = value; UpdateMinimumSize(); QueueRedraw(); } }
    public Texture2D? TextureFocused { get => _focused; set { _focused = value; QueueRedraw(); } }
    public Resource? TextureClickMask { get; set; }
    public bool IgnoreTextureSize { get => _ignoreSize; set { _ignoreSize = value; UpdateMinimumSize(); } }
    public StretchModeEnum StretchMode { get; set; } = StretchModeEnum.Scale;
    public bool FlipH { get; set; }
    public bool FlipV { get; set; }

    internal override Vector2 NativeMinimumSize()
    {
        if (_ignoreSize) return Vector2.Zero;
        Texture2D? t = TextureNormal ?? TexturePressed ?? TextureHover ?? TextureDisabled;
        return t?.GetSize() ?? Vector2.Zero;
    }

    private Texture2D? Current()
    {
        DrawMode mode = GetDrawMode();
        Texture2D? t = mode switch
        {
            DrawMode.Disabled => TextureDisabled,
            DrawMode.Pressed or DrawMode.HoverPressed => TexturePressed ?? (mode == DrawMode.HoverPressed ? TextureHover : null),
            DrawMode.Hover => TextureHover,
            _ => null,
        };
        t ??= mode == DrawMode.Normal && HasFocus() && TextureFocused != null ? TextureFocused : TextureNormal;
        if (mode != DrawMode.Normal && t == null) t = TextureNormal;
        return t;
    }

    /// <summary>The rectangle the texture is drawn into for the stretch mode.</summary>
    internal Rect2 TextureRect(Vector2 texture)
    {
        Rect2 whole = LocalRect;
        switch (StretchMode)
        {
            case StretchModeEnum.Keep: return new Rect2(Vector2.Zero, texture);
            case StretchModeEnum.KeepCentered: return new Rect2((Size - texture) / 2f, texture);
            case StretchModeEnum.KeepAspect:
            case StretchModeEnum.KeepAspectCentered:
            case StretchModeEnum.KeepAspectCovered:
            {
                if (texture.X <= 0 || texture.Y <= 0) return whole;
                float s = StretchMode == StretchModeEnum.KeepAspectCovered ? MathF.Max(Size.X / texture.X, Size.Y / texture.Y) : MathF.Min(Size.X / texture.X, Size.Y / texture.Y);
                Vector2 d = texture * s;
                return StretchMode == StretchModeEnum.KeepAspect ? new Rect2(Vector2.Zero, d) : new Rect2((Size - d) / 2f, d);
            }
            default: return whole;
        }
    }

    public override void _Draw()
    {
        Texture2D? t = Current();
        if (t != null)
        {
            Rect2 r = TextureRect(t.GetSize());
            DrawTextureRect(t, r, StretchMode == StretchModeEnum.Tile);
        }
    }
}
