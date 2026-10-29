// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Popups (G5 U2): PopupWindow (what Godot's Window is to a popup: popup(), popup_centered(), popup_hide, about_to_popup), Popup, PopupPanel, AcceptDialog and
// ConfirmationDialog. In the engine a popup is a top-level Control: the GUI hits it first, draws it on the overlay layer and closes it on a click
// outside (G5_PARITY.md U2 lists the differences from a real Window). PopupMenu is in PopupMenu.cs.

namespace Aver.Fabric;

/// <summary>The shared behaviour of the popup classes: a top-level control that is hidden until it is popped up (the part of Godot's Window a popup uses).</summary>
public class PopupWindow : Control
{
    public new class SignalName : Control.SignalName
    {
        public static readonly StringName PopupHide = "popup_hide";
        public static readonly StringName AboutToPopup = "about_to_popup";
        public static readonly StringName CloseRequested = "close_requested";
    }

    public event Action PopupHide { add => Connect(SignalName.PopupHide, Callable.From(value)); remove => Disconnect(SignalName.PopupHide, Callable.From(value)); }
    public event Action AboutToPopup { add => Connect(SignalName.AboutToPopup, Callable.From(value)); remove => Disconnect(SignalName.AboutToPopup, Callable.From(value)); }
    public event Action CloseRequested { add => Connect(SignalName.CloseRequested, Callable.From(value)); remove => Disconnect(SignalName.CloseRequested, Callable.From(value)); }

    public string Title { get; set; } = "";
    public bool Exclusive { get; set; }
    public bool Transient { get; set; }
    public bool Unresizable { get; set; }
    public bool Borderless { get; set; }
    public bool AlwaysOnTop { get; set; }
    public bool Transparent { get; set; }
    public bool PopupWindowFlag { get; set; } = true;
    public bool WrapControls { get; set; }
    public int InitialPosition { get; set; }
    public bool TransientToFocused { get; set; }
    public bool ForceNative { get; set; }
    public int Mode { get; set; }
    public Vector2I MinSize { get; set; }
    public Vector2I MaxSize { get; set; }

    public PopupWindow()
    {
        TopLevel = true;
        Visible = false;
        FocusMode = FocusModeEnum.None;
    }

    // Godot's `popup_window` cannot be a member (it would be named like this class).
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name == "popup_window") { PopupWindowFlag = value.AsBool(); return true; }
        return base.TryDynamicSet(name, value);
    }

    /// <summary>Whether a press outside the popup closes it (menus do; dialogs do not).</summary>
    internal virtual bool CloseOnOutsideClick => false;

    /// <summary>The size the popup takes when it is shown without being given one.</summary>
    internal virtual Vector2 ContentSize() => Size;

    /// <summary>A shown popup is in the GUI's popup list (hit first, drawn last).</summary>
    public bool IsPopupShown => Visible && Gui.Popups.Contains(this);

    internal override void OnShownChanged()
    {
        base.OnShownChanged();
        if (Visible && IsInsideTree()) Gui.PopupShown(this);
        else if (!Visible)
        {
            Gui.PopupHidden(this);
            OnHidden();
            EmitSignal(SignalName.PopupHide);
        }
    }

    internal virtual void OnHidden() { }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        if (Visible) Gui.PopupShown(this);
    }

    internal override void OnExitingTree()
    {
        Gui.PopupHidden(this);
        base.OnExitingTree();
    }

    private Rect2 Screen()
    {
        Viewport? vp = GetViewport() ?? SceneTree.Instance.Root;
        return vp.GetVisibleRect();
    }

    private void Place(Vector2 position, Vector2 size)
    {
        Vector2 min = new(MinSize.X, MinSize.Y);
        size = new Vector2(MathF.Max(size.X, min.X), MathF.Max(size.Y, min.Y));
        if (MaxSize.X > 0) size.X = MathF.Min(size.X, MaxSize.X);
        if (MaxSize.Y > 0) size.Y = MathF.Min(size.Y, MaxSize.Y);
        Rect2 screen = Screen();
        if (screen.Size.X > 0 && screen.Size.Y > 0)
        {
            position.X = MathF.Max(screen.Position.X, MathF.Min(position.X, screen.End.X - size.X));
            position.Y = MathF.Max(screen.Position.Y, MathF.Min(position.Y, screen.End.Y - size.Y));
        }
        SetSize(size);
        SetPosition(position);
    }

    private void Show_(Vector2 position, Vector2 size)
    {
        EmitSignal(SignalName.AboutToPopup);
        OnAboutToPopup();
        Place(position, size);
        Visible = true;
        if (IsInsideTree()) Gui.PopupShown(this);
        OnShown();
    }

    internal virtual void OnAboutToPopup() { }

    internal virtual void OnShown() { }

    /// <summary>Shows the popup at the given rectangle, or where it is at its content size (Godot's Window.popup).</summary>
    public void Popup(Rect2I? rect = null)
    {
        if (rect is { } r && r.Size.X > 0 && r.Size.Y > 0) Show_(new Vector2(r.Position.X, r.Position.Y), new Vector2(r.Size.X, r.Size.Y));
        else if (rect is { } p) Show_(new Vector2(p.Position.X, p.Position.Y), ContentSize());
        else Show_(Position, ContentSize());
    }

    /// <summary>Shows the popup centred in the screen, at least <paramref name="minsize"/> big.</summary>
    public void PopupCentered(Vector2I? minsize = null)
    {
        Vector2 size = ContentSize();
        if (minsize is { } m) size = new Vector2(MathF.Max(size.X, m.X), MathF.Max(size.Y, m.Y));
        Rect2 screen = Screen();
        Show_(screen.Position + (screen.Size - size) / 2f, size);
    }

    /// <summary>Shows the popup centred, <paramref name="ratio"/> of the screen's size.</summary>
    public void PopupCenteredRatio(float ratio = 0.8f)
    {
        Rect2 screen = Screen();
        Vector2 size = screen.Size * ratio;
        Show_(screen.Position + (screen.Size - size) / 2f, size);
    }

    /// <summary>Shows the popup centred at its content size or <paramref name="minsize"/>, shrunk to <paramref name="fallbackRatio"/> of the screen if it is bigger.</summary>
    public void PopupCenteredClamped(Vector2I? minsize = null, float fallbackRatio = 0.75f)
    {
        Vector2 size = ContentSize();
        if (minsize is { } m) size = new Vector2(MathF.Max(size.X, m.X), MathF.Max(size.Y, m.Y));
        Rect2 screen = Screen();
        size = new Vector2(MathF.Min(size.X, screen.Size.X * fallbackRatio), MathF.Min(size.Y, screen.Size.Y * fallbackRatio));
        Show_(screen.Position + (screen.Size - size) / 2f, size);
    }

    /// <summary>Shows the popup at a rectangle given in the parent's coordinates.</summary>
    public void PopupOnParent(Rect2I parentRect)
    {
        Vector2 origin = GetParent() is Control c ? c.GetGlobalTransform().Origin : Vector2.Zero;
        Show_(origin + new Vector2(parentRect.Position.X, parentRect.Position.Y), parentRect.Size.X > 0 ? new Vector2(parentRect.Size.X, parentRect.Size.Y) : ContentSize());
    }

    public void PopupCenteredExclusive(Vector2I? minsize = null) { Exclusive = true; PopupCentered(minsize); }

    public void PopupExclusive(Rect2I? rect = null) { Exclusive = true; Popup(rect); }

    public void SetVisible(bool visible)
    {
        if (visible && !Visible) Popup();
        else Visible = visible;
    }

    public override void _GuiInput(InputEvent @event)
    {
        if (@event is InputEventKey { Pressed: true } k && Gui.IsUiAction(k, "ui_cancel")) { RequestClose(); AcceptEvent(); }
    }

    /// <summary>The window's close request (Escape for a popup): hides it.</summary>
    internal virtual void RequestClose()
    {
        EmitSignal(SignalName.CloseRequested);
        Hide();
    }
}

/// <summary>A popup (Godot's Popup): it hides when the player clicks outside it.</summary>
public class Popup : PopupWindow
{
    internal override bool CloseOnOutsideClick => true;
}

/// <summary>A popup with a panel behind its contents (Godot's PopupPanel); the children fill the panel.</summary>
public class PopupPanel : Popup
{
    internal override Vector2 ContentSize()
    {
        Vector2 min = GetThemeStylebox("panel").GetMinimumSize();
        Vector2 inner = Vector2.Zero;
        foreach (Node n in ChildrenSnapshot())
            if (n is Control c && c.Visible) inner = new Vector2(MathF.Max(inner.X, c.GetCombinedMinimumSize().X), MathF.Max(inner.Y, c.GetCombinedMinimumSize().Y));
        return new Vector2(MathF.Max(Size.X, min.X + inner.X), MathF.Max(Size.Y, min.Y + inner.Y));
    }

    internal override void OnShown() => FitChildren();

    private void FitChildren()
    {
        StyleBox panel = GetThemeStylebox("panel");
        Rect2 content = panel.GetContentRect(LocalRect);
        foreach (Node n in ChildrenSnapshot())
            if (n is Control c && c.Visible && !c.TopLevel) { c.SetPosition(content.Position); c.SetSize(content.Size); }
    }

    public override void _Notification(int what)
    {
        if (what == NotificationResized) FitChildren();
    }

    public override void _Draw() => DrawStyleBox(GetThemeStylebox("panel"), LocalRect);
}

/// <summary>A dialog with a message and an OK button (Godot's AcceptDialog).</summary>
public class AcceptDialog : PopupWindow
{
    public new class SignalName : PopupWindow.SignalName
    {
        public static readonly StringName Confirmed = "confirmed";
        public static readonly StringName Canceled = "canceled";
        public static readonly StringName CustomAction = "custom_action";
    }

    public event Action Confirmed { add => Connect(SignalName.Confirmed, Callable.From(value)); remove => Disconnect(SignalName.Confirmed, Callable.From(value)); }
    public event Action Canceled { add => Connect(SignalName.Canceled, Callable.From(value)); remove => Disconnect(SignalName.Canceled, Callable.From(value)); }
    public event Action<StringName> CustomAction { add => Connect(SignalName.CustomAction, Callable.From(value)); remove => Disconnect(SignalName.CustomAction, Callable.From(value)); }

    private readonly Label _label = new() { Name = "Label" };
    private readonly Button _ok = new() { Name = "OkButton", Text = "OK" };
    private readonly List<Button> _buttons = new();
    private bool _hideOnOk = true, _closeOnEscape = true;

    public AcceptDialog()
    {
        Title = "Alert!";
        AddChild(_label);
        AddChild(_ok);
        _buttons.Add(_ok);
        _ok.Pressed += OnOk;
        Exclusive = true;
    }

    public string DialogText { get => _label.Text; set { _label.Text = value; Relayout(); } }
    public bool DialogHideOnOk { get => _hideOnOk; set => _hideOnOk = value; }
    public bool DialogCloseOnEscape { get => _closeOnEscape; set => _closeOnEscape = value; }
    public bool DialogAutowrap { get => _label.AutowrapMode != TextServer.AutowrapMode.Off; set { _label.AutowrapMode = value ? TextServer.AutowrapMode.WordSmart : TextServer.AutowrapMode.Off; Relayout(); } }
    public string OkButtonText { get => _ok.Text; set { _ok.Text = value; Relayout(); } }

    public Button GetOkButton() => _ok;

    public Label GetLabel() => _label;

    /// <summary>Adds a button to the dialog; pressing it emits custom_action with <paramref name="action"/> (and hides the dialog when the action is empty).</summary>
    public Button AddButton(string text, bool right = false, string action = "")
    {
        var b = new Button { Text = text, Name = "Button" + (_buttons.Count + 1) };
        string a = action;
        b.Pressed += () =>
        {
            if (a.Length > 0) EmitSignal(SignalName.CustomAction, new StringName(a));
            else Hide();
        };
        AddChild(b);
        if (right) _buttons.Add(b); else _buttons.Insert(0, b);
        Relayout();
        return b;
    }

    public Button AddCancelButton(string name)
    {
        var b = new Button { Text = name, Name = "CancelButton" };
        b.Pressed += OnCancel;
        AddChild(b);
        _buttons.Add(b);
        Relayout();
        return b;
    }

    public void RemoveButton(Control button)
    {
        if (button is Button b && _buttons.Remove(b)) { if (b.GetParent() == this) RemoveChild(b); Relayout(); }
    }

    public void RegisterTextEnter(LineEdit lineEdit) => lineEdit.TextSubmitted += _ => OnOk();

    private void OnOk()
    {
        if (_hideOnOk) Hide();
        EmitSignal(SignalName.Confirmed);
    }

    private void OnCancel()
    {
        Hide();
        EmitSignal(SignalName.Canceled);
    }

    internal override void RequestClose()
    {
        if (!_closeOnEscape) return;
        EmitSignal(SignalName.CloseRequested);
        OnCancel();
    }

    // ---- layout: title, message, a row of buttons ----

    private float TitleHeight => Borderless ? 0f : UiText.LineHeight(ThemeFont("title_font"), ThemeFontSize("title_font_size")) + GetThemeConstant("title_height", "Window") * 0f + 8f;

    private Vector2 ButtonRowSize()
    {
        float w = 0f, h = 0f;
        foreach (Button b in _buttons)
        {
            Vector2 m = b.GetCombinedMinimumSize();
            w += m.X;
            h = MathF.Max(h, m.Y);
        }
        return new Vector2(w + Math.Max(0, _buttons.Count - 1) * GetButtonSeparation(), h);
    }

    private float GetButtonSeparation() => 8f;

    internal override Vector2 ContentSize()
    {
        float margin = 8f;
        Vector2 label = _label.GetCombinedMinimumSize();
        Vector2 row = ButtonRowSize();
        float w = MathF.Max(label.X, row.X) + margin * 2f;
        float h = TitleHeight + label.Y + (label.Y > 0 ? margin : 0f) + row.Y + margin * 2f;
        return new Vector2(MathF.Max(w, Size.X), MathF.Max(h, Size.Y));
    }

    private void Relayout()
    {
        if (!IsInsideTree()) return;
        float margin = 8f;
        float top = TitleHeight + margin;
        Vector2 row = ButtonRowSize();
        float labelH = MathF.Max(0f, Size.Y - top - row.Y - margin * 2f);
        _label.SetPosition(new Vector2(margin, top));
        _label.SetSize(new Vector2(MathF.Max(0f, Size.X - margin * 2f), labelH));
        float x = (Size.X - row.X) / 2f;
        foreach (Button b in _buttons)
        {
            Vector2 m = b.GetCombinedMinimumSize();
            b.SetPosition(new Vector2(x, Size.Y - margin - row.Y));
            b.SetSize(m);
            x += m.X + GetButtonSeparation();
        }
    }

    internal override void OnShown() => Relayout();

    public override void _Notification(int what)
    {
        if (what == NotificationResized) Relayout();
        else if (what == NotificationReady || what == 13) Relayout();
    }

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("panel", "AcceptDialog"), LocalRect);
        if (!Borderless && Title.Length > 0)
            DrawTextLine(new Vector2(8f, 4f), Title, ThemeFont("title_font"), ThemeFontSize("title_font_size"), GetThemeColor("title_color", "Window"));
    }
}

/// <summary>An OK and Cancel dialog (Godot's ConfirmationDialog).</summary>
public class ConfirmationDialog : AcceptDialog
{
    private readonly Button _cancel;

    public ConfirmationDialog()
    {
        Title = "Please Confirm...";
        _cancel = AddCancelButton("Cancel");
    }

    public Button GetCancelButton() => _cancel;

    public string CancelButtonText { get => _cancel.Text; set => _cancel.Text = value; }
}
