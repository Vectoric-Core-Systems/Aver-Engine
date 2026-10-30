// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The GUI (G5 U1): what Godot's Viewport does for Controls. Hit testing, the pointer event chain with mouse_filter, a pressed control keeping the
// pointer, mouse_entered / exited, keyboard focus and focus navigation, tooltips with their delay, drag and drop, the deferred layout queue and the
// replay of every control's recorded drawing into the engine's UI draw list. One instance of the state for the process (like the SceneTree), reset with
// the tree. The tests drive it through InputEvents and T.Frame(); the engine feeds it the same way (G5_PARITY.md U1 lists what only a headed run can show).

namespace Aver.Fabric;

internal static class Gui
{
    // ---- registry and the deferred layout queue ----

    private static readonly HashSet<Control> s_controls = new();
    private static readonly Queue<Action> s_deferred = new();
    private static GuiDriver? s_driver;

    internal static IEnumerable<Control> AllControls() => s_controls.ToArray();

    internal static void Defer(Action a) => s_deferred.Enqueue(a);

    /// <summary>Runs the queued layout work (container sorts, minimum size updates) until nothing is left. Called at the end of every idle frame and
    /// before drawing and input; a test calls it to see a layout settle.</summary>
    internal static void FlushLayout()
    {
        for (int guard = 0; s_deferred.Count > 0 && guard < 100000; guard++) s_deferred.Dequeue()();
    }

    internal static void Enter(Control c)
    {
        s_controls.Add(c);
        if (NodeHost.Current.IsEngine && (s_driver == null || !s_driver.IsAlive))
        {
            s_driver = new GuiDriver();
            Defer(() => { if (s_driver is { IsAlive: true, } d && !d.IsInsideTree()) SceneTree.Instance.Root.AddChild(d); });
        }
    }

    internal static void Exit(Control c)
    {
        s_controls.Remove(c);
        if (s_focus != null && (s_focus == c || c.IsAncestorOf(s_focus))) ReleaseFocusQuiet();
        if (s_mouseFocus != null && (s_mouseFocus == c || c.IsAncestorOf(s_mouseFocus))) { s_mouseFocus = null; s_buttonMask = 0; }
        if (s_over.Count > 0) s_over.RemoveAll(o => o == c || c.IsAncestorOf(o));
        if (s_tooltipSource != null && (s_tooltipSource == c || c.IsAncestorOf(s_tooltipSource))) CancelTooltip();
        if (s_dragOver != null && (s_dragOver == c || c.IsAncestorOf(s_dragOver))) s_dragOver = null;
        if (s_dragging && s_dragSource != null && (s_dragSource == c || c.IsAncestorOf(s_dragSource))) s_dragSource = null;
    }

    internal static void ControlHidden(Control c)
    {
        if (s_focus != null && (s_focus == c || c.IsAncestorOf(s_focus))) ReleaseFocusQuiet();
        if (s_mouseFocus != null && (s_mouseFocus == c || c.IsAncestorOf(s_mouseFocus))) { s_mouseFocus = null; s_buttonMask = 0; }
        s_over.RemoveAll(o => o == c || c.IsAncestorOf(o));
        if (s_tooltipSource != null && (s_tooltipSource == c || c.IsAncestorOf(s_tooltipSource))) CancelTooltip();
    }

    // ---- popups (G5 U2): shown above everything, hit first, closed by a click outside ----

    private static readonly List<PopupWindow> s_popups = new();

    internal static IReadOnlyList<PopupWindow> Popups => s_popups;

    internal static void PopupShown(PopupWindow p)
    {
        s_popups.Remove(p);
        s_popups.Add(p);
    }

    internal static void PopupHidden(PopupWindow p) => s_popups.Remove(p);

    private static bool InAnyPopup(Vector2 global)
    {
        foreach (PopupWindow p in s_popups) if (FindAt(p, global) != null) return true;
        return false;
    }

    // A press outside every shown popup closes those that close that way; an exclusive popup also swallows it. True when the press is used up.
    private static bool ClosePopupsForPress(Vector2 global)
    {
        if (s_popups.Count == 0 || InAnyPopup(global)) return false;
        bool swallow = false;
        foreach (PopupWindow p in s_popups.ToArray())
        {
            if (p.CloseOnOutsideClick) { p.Hide(); swallow = true; }
            else if (p.Exclusive) swallow = true;
        }
        return swallow;
    }

    internal static void ResetForTests()
    {
        s_popups.Clear();
        s_controls.Clear();
        s_deferred.Clear();
        s_driver = null;
        s_focus = null;
        s_mouseFocus = null;
        s_buttonMask = 0;
        s_over.Clear();
        s_dragging = false;
        s_dragData = default;
        s_dragPreview = null;
        s_pendingPreview = null;
        s_dragSource = null;
        s_dragOver = null;
        s_dragSuccess = false;
        s_dragAttempted = false;
        s_dragAccum = default;
        s_dragCanDrop = false;
        s_accepted = false;
        s_tooltipSource = null;
        s_tooltipVisible = false;
        s_tooltipTimer = -1;
        s_tooltipText = "";
        s_customTooltip = null;
        s_mousePos = default;
        s_lastEnginePointer = default;
        s_engineButtons = 0;
        TooltipDelaySeconds = -1;
    }

    // ---- keyboard focus ----

    private static Control? s_focus;

    internal static Control? FocusOwner => s_focus;

    internal static void GrabFocus(Control c)
    {
        if (!c.IsInsideTree() || !c.IsVisibleInTree()) return;
        if (c.FocusMode == Control.FocusModeEnum.None)
        {
            Aver.Scripting.Log.Error($"[Fabric] Control '{c.Name}': can't grab focus with focus_mode = None");
            return;
        }
        if (s_focus == c) return;
        Control? old = s_focus;
        s_focus = c;
        if (old != null)
        {
            old.Notification(Control.NotificationFocusExit);
            old.EmitSignal(Control.SignalName.FocusExited);
            old.QueueRedraw();
        }
        c.Notification(Control.NotificationFocusEnter);
        c.EmitSignal(Control.SignalName.FocusEntered);
        c.QueueRedraw();
    }

    internal static void ReleaseFocus(Control c)
    {
        if (s_focus != c) return;
        ReleaseFocusQuiet();
    }

    internal static void ReleaseFocusQuiet()
    {
        Control? old = s_focus;
        s_focus = null;
        if (old == null || !old.IsAlive) return;
        old.Notification(Control.NotificationFocusExit);
        old.EmitSignal(Control.SignalName.FocusExited);
        old.QueueRedraw();
    }

    // ---- hit testing ----

    /// <summary>The Control a pointer at <paramref name="global"/> (UI space) would reach: the topmost visible Control whose rectangle holds it,
    /// ignoring the ones with mouse_filter Ignore and everything outside a clipping ancestor. Later siblings are above earlier ones, children above
    /// their parent.</summary>
    internal static Control? FindControl(Vector2 global)
    {
        for (int i = s_popups.Count - 1; i >= 0; i--)
            if (FindAt(s_popups[i], global) is { } over) return over;
        if (s_popups.Exists(p => p.Exclusive)) return null;
        Node root = SceneTree.Instance.Root;
        List<Node> kids = root.ChildrenSnapshot();
        for (int i = kids.Count - 1; i >= 0; i--)
            if (FindAt(kids[i], global) is { } hit) return hit;
        return null;
    }

    private static Control? FindAt(Node node, Vector2 global)
    {
        if (node is SubViewport) return null;
        Control? control = node as Control;
        if (node is CanvasItem ci)
        {
            if (!ci.Visible) return null;
            if (control != null && control.ClipContents && !control.HasPointGlobal(global)) return null;
        }
        List<Node> kids = node.ChildrenSnapshot();
        for (int i = kids.Count - 1; i >= 0; i--)
            if (FindAt(kids[i], global) is { } hit) return hit;
        if (control != null && control.MouseFilter != Control.MouseFilterEnum.Ignore && control.CanProcess() && control.HasPointGlobal(global)) return control;
        return null;
    }

    // ---- the event chain ----

    private static bool s_accepted;
    private static Vector2 s_mousePos;
    private static Control? s_mouseFocus;
    private static int s_buttonMask;
    private static readonly List<Control> s_over = new();   // the control under the pointer and the controls above it, deepest first

    internal static void Accept()
    {
        s_accepted = true;
        InputEvent.Current?.MarkHandled();
    }

    internal static bool IsAccepted => s_accepted;

    internal static Vector2 MousePosition => s_mousePos;

    internal static Control? HoveredControl => s_over.Count > 0 ? s_over[0] : null;

    internal static IReadOnlyList<Control> HoverChain => s_over;

    private static InputEvent AtPosition(InputEvent e, Vector2 local, Vector2 global)
    {
        switch (e)
        {
            case InputEventMouseButton b:
                return new InputEventMouseButton
                {
                    ButtonIndex = b.ButtonIndex, Pressed = b.Pressed, DoubleClick = b.DoubleClick, Factor = b.Factor, Canceled = b.Canceled, ButtonMask = b.ButtonMask,
                    ShiftPressed = b.ShiftPressed, CtrlPressed = b.CtrlPressed, AltPressed = b.AltPressed, MetaPressed = b.MetaPressed, Device = b.Device,
                    Position = local, GlobalPosition = global, Source = b.Source,
                };
            case InputEventMouseMotion m:
                return new InputEventMouseMotion
                {
                    Relative = m.Relative, ScreenRelative = m.ScreenRelative, Velocity = m.Velocity, ScreenVelocity = m.ScreenVelocity, Pressure = m.Pressure,
                    ButtonMask = m.ButtonMask, ShiftPressed = m.ShiftPressed, CtrlPressed = m.CtrlPressed, AltPressed = m.AltPressed, MetaPressed = m.MetaPressed,
                    Device = m.Device, Position = local, GlobalPosition = global, Source = m.Source,
                };
        }
        return e;
    }

    private static Vector2 ToLocal(Control c, Vector2 global)
    {
        Transform2D t = c.GetGlobalTransform();
        return t.Determinant() == 0f ? global : t.AffineInverse() * global;
    }

    /// <summary>Gives a mouse event to <paramref name="start"/> and then, while nothing accepted it and no control stops it, to each Control parent.
    /// A control with mouse_filter Stop ends the walk; Pass hands it to the parent; Ignore is skipped. Returns whether the walk stopped the event.</summary>
    private static bool CallInput(Control start, InputEvent e, Vector2 global)
    {
        bool stopped = false;
        s_accepted = false;
        for (Control? c = start; c != null; c = c.TopLevel ? null : c.GetParent() as Control)
        {
            if (c.MouseFilter != Control.MouseFilterEnum.Ignore && c.CanProcess())
                c.CallGuiInput(AtPosition(e, ToLocal(c, global), global));
            if (!c.IsAlive || !c.IsInsideTree() || c.TopLevel) break;
            if (s_accepted) break;
            if (c.MouseFilter == Control.MouseFilterEnum.Stop) { stopped = true; break; }
        }
        return stopped || s_accepted;
    }

    /// <summary>The GUI's turn on an input event, after every node's _Input and before _UnhandledInput. Returns whether it was handled (the event is
    /// marked handled too, so _UnhandledInput does not see it).</summary>
    internal static bool HandleInput(InputEvent e)
    {
        FlushLayout();
        InputEvent? outer = InputEvent.Current;
        InputEvent.Current = e;
        try
        {
            bool handled = e switch
            {
                InputEventMouseMotion m => OnMotion(m),
                InputEventMouseButton b => OnButton(b),
                _ => OnOther(e),
            };
            if (handled) e.MarkHandled();
            return handled;
        }
        finally { InputEvent.Current = outer; }
    }

    // ---- pointer ----

    private static bool OnMotion(InputEventMouseMotion m)
    {
        s_mousePos = m.Position;
        if (s_dragging)
        {
            UpdateDrag(m.Position);
            return true;
        }
        if (s_mouseFocus != null && s_buttonMask != 0)
        {
            Control target = s_mouseFocus;
            if ((s_buttonMask & 1) != 0 && !s_dragAttempted)
            {
                s_dragAccum += m.Relative;
                if (s_dragAccum.Length() > 10f)
                {
                    s_dragAttempted = true;
                    Vector2 at = ToLocal(target, m.Position - s_dragAccum);
                    Variant data = target.CallGetDragData(at);
                    if (!data.IsNil)
                    {
                        BeginDrag(target, data);
                        UpdateDrag(m.Position);
                        return true;
                    }
                    DropPendingPreview();
                }
            }
            bool wasStopped = CallInput(target, m, m.Position);
            return wasStopped;
        }
        Control? over = FindControl(m.Position);
        UpdateHover(over);
        RestartTooltip(over, m.Position);
        if (over == null) return false;
        return CallInput(over, m, m.Position);
    }

    private static bool OnButton(InputEventMouseButton b)
    {
        s_mousePos = b.Position;
        int bit = 1 << Math.Max(0, (int)b.ButtonIndex - 1);
        if (b.Pressed)
        {
            CancelTooltip();
            if (s_dragging) return true;   // a click while dragging does nothing but keep the drag
            if (s_buttonMask == 0 && ClosePopupsForPress(b.Position)) return true;
            if (s_buttonMask == 0)
            {
                s_mouseFocus = FindControl(b.Position);
                UpdateHover(s_mouseFocus);
            }
            if (s_mouseFocus == null) return false;
            s_buttonMask |= bit;
            if (b.ButtonIndex == MouseButton.Left)
            {
                s_dragAccum = Vector2.Zero;
                s_dragAttempted = false;
                GrabFocusFromClick(s_mouseFocus);
            }
            return CallInput(s_mouseFocus, b, b.Position);
        }

        // a release
        if (s_dragging)
        {
            if (b.ButtonIndex == MouseButton.Left) EndDrag(b.Position);
            return true;
        }
        if (s_mouseFocus == null) return false;
        Control target = s_mouseFocus;
        s_buttonMask &= ~bit;
        bool handled = CallInput(target, b, b.Position);
        if (s_buttonMask == 0)
        {
            s_mouseFocus = null;
            UpdateHover(FindControl(b.Position));
        }
        return handled;
    }

    // A left press takes keyboard focus for the first control from the one hit upwards that accepts it by clicking, as far as a Pass chain reaches.
    private static void GrabFocusFromClick(Control hit)
    {
        for (Control? c = hit; c != null; c = c.TopLevel ? null : c.GetParent() as Control)
        {
            if (c.FocusMode != Control.FocusModeEnum.None && c.MouseFilter != Control.MouseFilterEnum.Ignore) { GrabFocus(c); return; }
            if (c.MouseFilter == Control.MouseFilterEnum.Stop) return;
        }
    }

    private static void UpdateHover(Control? over)
    {
        var next = new List<Control>();
        for (Control? c = over; c != null; c = c.TopLevel ? null : c.GetParent() as Control)
            if (c.MouseFilter != Control.MouseFilterEnum.Ignore) next.Add(c);
        Control? oldTop = s_over.Count > 0 ? s_over[0] : null;
        foreach (Control c in s_over.ToArray())
            if (!next.Contains(c) && c.IsAlive)
            {
                c.Notification(Control.NotificationMouseExit);
                c.EmitSignal(Control.SignalName.MouseExited);
            }
        Control? newTop = next.Count > 0 ? next[0] : null;
        if (oldTop != newTop && oldTop != null && oldTop.IsAlive) oldTop.Notification(Control.NotificationMouseExitSelf);
        for (int i = next.Count - 1; i >= 0; i--)
            if (!s_over.Contains(next[i]))
            {
                next[i].Notification(Control.NotificationMouseEnter);
                next[i].EmitSignal(Control.SignalName.MouseEntered);
            }
        if (oldTop != newTop && newTop != null) newTop.Notification(Control.NotificationMouseEnterSelf);
        s_over.Clear();
        s_over.AddRange(next);
    }

    // ---- keys, actions and focus navigation ----

    private static bool OnOther(InputEvent e)
    {
        if (s_focus != null && !s_focus.IsVisibleInTree()) ReleaseFocusQuiet();
        if (e.IsPressed()) CancelTooltip();
        if (s_dragging && IsUiAction(e, "ui_cancel"))
        {
            CancelDrag();
            return true;
        }
        if (s_focus != null && s_focus.CanProcess())
        {
            s_accepted = false;
            s_focus.CallGuiInput(e);
            if (s_accepted) return true;
        }
        if (e.IsPressed() && s_popups.Count > 0 && IsUiAction(e, "ui_cancel"))
        {
            s_popups[^1].RequestClose();   // Escape closes the topmost popup when nothing else took it
            return true;
        }
        if (!e.IsPressed() || e.IsEcho() && e is not InputEventKey) return false;
        return Navigate(e);
    }

    /// <summary>Whether <paramref name="e"/> triggers a ui_* action: the project's InputMap entry when it has events, else Godot's built-in keys.</summary>
    internal static bool IsUiAction(InputEvent e, string action)
    {
        if (InputMap.HasAction(action) && e.IsAction(action)) return true;
        if (e is not InputEventKey { Pressed: true } k) return false;
        if (InputMap.HasAction(action) && InputMap.ActionGetEvents(action).Count > 0) return false;   // the project rebound it
        Key key = k.GetKeycodeWithModifiers();
        return action switch
        {
            "ui_accept" => key is Key.Enter or Key.KpEnter or Key.Space,
            "ui_select" => key == Key.Space,
            "ui_cancel" => key == Key.Escape,
            "ui_focus_next" => key == Key.Tab && !k.ShiftPressed,
            "ui_focus_prev" => (key == Key.Tab && k.ShiftPressed) || key == Key.Backtab,
            "ui_left" => key == Key.Left,
            "ui_right" => key == Key.Right,
            "ui_up" => key == Key.Up,
            "ui_down" => key == Key.Down,
            "ui_home" => key == Key.Home,
            "ui_end" => key == Key.End,
            "ui_page_up" => key == Key.Pageup,
            "ui_page_down" => key == Key.Pagedown,
            _ => false,
        };
    }

    private static bool Navigate(InputEvent e)
    {
        Control? from = s_focus;
        Control? next = null;
        if (IsUiAction(e, "ui_focus_next")) next = from?.FindNextValidFocus() ?? FirstFocusable(forward: true);
        else if (IsUiAction(e, "ui_focus_prev")) next = from?.FindPrevValidFocus() ?? FirstFocusable(forward: false);
        else if (from != null && IsUiAction(e, "ui_up")) next = from.FindValidFocusNeighbor(Side.Top);
        else if (from != null && IsUiAction(e, "ui_down")) next = from.FindValidFocusNeighbor(Side.Bottom);
        else if (from != null && IsUiAction(e, "ui_left")) next = from.FindValidFocusNeighbor(Side.Left);
        else if (from != null && IsUiAction(e, "ui_right")) next = from.FindValidFocusNeighbor(Side.Right);
        else return false;
        if (next != null && next != from) GrabFocus(next);
        return true;
    }

    private static Control? FirstFocusable(bool forward)
    {
        List<Control> all = FocusableInTreeOrder();
        if (all.Count == 0) return null;
        return forward ? all[0] : all[^1];
    }

    /// <summary>Every visible Control that can take focus, in tree order.</summary>
    internal static List<Control> FocusableInTreeOrder()
    {
        var list = new List<Control>();
        foreach (Node n in NodeHost.PreOrder(SceneTree.Instance.Root))
            if (n is Control c && c.FocusMode != Control.FocusModeEnum.None && c.IsVisibleInTree() && c.CanProcess()) list.Add(c);
        return list;
    }

    // ---- tooltips ----

    private static Control? s_tooltipSource;
    private static Control? s_tooltipOver;
    private static bool s_tooltipVisible;
    private static double s_tooltipTimer = -1;
    private static string s_tooltipText = "";
    private static Control? s_customTooltip;
    private static Vector2 s_tooltipPos;
    private static Vector2 s_tooltipAnchor;

    /// <summary>gui/timers/tooltip_delay_sec (0.5 by default); a test may set it. Negative reads the project setting.</summary>
    internal static double TooltipDelaySeconds { get; set; } = -1;

    private static double TooltipDelay()
    {
        if (TooltipDelaySeconds >= 0) return TooltipDelaySeconds;
        Variant v = ProjectSettings.GetSetting("gui/timers/tooltip_delay_sec", 0.5);
        return v.AsDouble() >= 0 ? v.AsDouble() : 0.5;
    }

    internal static bool TooltipVisible => s_tooltipVisible;
    internal static string TooltipText => s_tooltipText;
    internal static Control? TooltipSource => s_tooltipSource;
    internal static Control? CustomTooltipControl => s_customTooltip;
    internal static Vector2 TooltipPosition => s_tooltipPos;

    // The pointer moved over a control: a shown tooltip goes away, the timer for the control under the pointer starts again.
    private static void RestartTooltip(Control? over, Vector2 pos)
    {
        bool moved = s_tooltipVisible || over != s_tooltipOver || (pos - s_tooltipAnchor).Length() > 0f;
        if (s_tooltipVisible) HideTooltip();
        s_tooltipOver = over;
        s_tooltipAnchor = pos;
        s_tooltipTimer = over != null && moved ? TooltipDelay() : -1;
        if (over == null) s_tooltipTimer = -1;
    }

    internal static void CancelTooltip()
    {
        HideTooltip();
        s_tooltipTimer = -1;
        s_tooltipOver = null;
    }

    private static void HideTooltip()
    {
        if (!s_tooltipVisible && s_customTooltip == null) { s_tooltipSource = null; return; }
        s_tooltipVisible = false;
        if (s_customTooltip != null)
        {
            Control custom = s_customTooltip;
            s_customTooltip = null;
            if (custom.IsAlive) { if (custom.GetParent() != null) custom.GetParent()!.RemoveChild(custom); custom.Free(); }
        }
        s_tooltipSource = null;
        s_tooltipText = "";
    }

    // The tooltip's owner is the first control from the one under the pointer upwards that has text, as far as a Pass chain reaches.
    private static void ShowTooltip()
    {
        if (s_tooltipOver == null) return;
        Vector2 global = s_tooltipAnchor;
        for (Control? c = s_tooltipOver; c != null; c = c.TopLevel ? null : c.GetParent() as Control)
        {
            string text = c.GetTooltip(ToLocal(c, global));
            if (text.Length > 0)
            {
                s_tooltipSource = c;
                s_tooltipText = text;
                s_customTooltip = null;
                if (c.CallMakeCustomTooltip(text) is { } custom)
                {
                    s_customTooltip = custom;
                    custom.MouseFilter = Control.MouseFilterEnum.Ignore;
                    custom.TopLevel = true;
                    SceneTree.Instance.Root.AddChild(custom);
                    FlushLayout();
                    custom.ResetSize();
                    s_tooltipPos = ClampToScreen(global + new Vector2(10, 10), custom.Size);
                    custom.Position = s_tooltipPos;
                }
                else s_tooltipPos = ClampToScreen(global + new Vector2(10, 10), DefaultTooltipSize(text));
                s_tooltipVisible = true;
                return;
            }
            if (c.MouseFilter == Control.MouseFilterEnum.Stop) return;
        }
    }

    private static Vector2 ClampToScreen(Vector2 pos, Vector2 size)
    {
        Vector2 area = SceneTree.Instance.Root.GetVisibleRect().Size;
        return new Vector2(Math.Max(0f, Math.Min(pos.X, area.X - size.X)), Math.Max(0f, Math.Min(pos.Y, area.Y - size.Y)));
    }

    /// <summary>The default tooltip's size: the text laid out at the TooltipLabel size plus the TooltipPanel margins.</summary>
    internal static Vector2 DefaultTooltipSize(string text)
    {
        Control probe = SceneTree.Instance.Root.GetChildCount() > 0 && SceneTree.Instance.Root.GetChild(0) is Control rc ? rc : TooltipProbe;
        StyleBox panel = probe.GetThemeStylebox("panel", "TooltipPanel");
        int px = Math.Max(1, probe.GetThemeFontSize("font_size", "TooltipLabel"));
        Vector2 t = MeasureText(text, px, 0f);
        return t + panel.GetMinimumSize();
    }

    private static Control? s_probe;
    private static Control TooltipProbe => s_probe ??= new Control();

    internal static Vector2 MeasureText(string text, float px, float maxWidth)
    {
        if (UiNative.Available)
        {
            var whl = new float[3];
            try
            {
                if (UiNative.aver_ui_text_measure(text, null, px, 0, maxWidth, whl) != 0) return new Vector2(whl[0], whl[1]);
            }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        }
        // no font to measure with (a headless run): an average glyph is half the em wide, a line 1.2 em tall
        string[] lines = text.Split('\n');
        return new Vector2(lines.Max(l => l.Length) * px * 0.5f, lines.Length * px * 1.2f);
    }

    // ---- drag and drop ----

    private static bool s_dragging, s_dragSuccess, s_dragAttempted, s_dragCanDrop;
    private static Variant s_dragData;
    private static Control? s_dragPreview, s_dragSource, s_dragOver;
    private static Vector2 s_dragAccum;

    internal static bool IsDragging => s_dragging;
    internal static Variant DragData => s_dragData;
    internal static bool DragSuccessful => s_dragSuccess;
    internal static bool DragCanDrop => s_dragCanDrop;
    internal static Control? DragPreview => s_dragPreview;
    internal static Control? DragSource => s_dragSource;

    internal static void BeginDrag(Control source, Variant data)
    {
        s_dragging = true;
        s_dragData = data;
        s_dragSource = source;
        s_dragSuccess = false;
        s_dragCanDrop = false;
        s_dragOver = null;
        s_mouseFocus = null;
        s_buttonMask = 0;
        CancelTooltip();
        if (s_pendingPreview != null) { Control p = s_pendingPreview; s_pendingPreview = null; if (p.IsAlive) AttachPreview(p); }
        BroadcastToControls(Control.NotificationDragBegin);
    }

    private static Control? s_pendingPreview;   // set_drag_preview called inside _get_drag_data, before the drag exists

    internal static void SetDragPreview(Control preview)
    {
        if (preview.IsInsideTree() || preview.GetParent() != null)
        {
            Aver.Scripting.Log.Error("[Fabric] set_drag_preview: the preview control must not be in the tree");
            return;
        }
        if (!s_dragging)
        {
            if (s_pendingPreview is { IsAlive: true } old) old.Free();
            s_pendingPreview = preview;
            return;
        }
        AttachPreview(preview);
    }

    private static void AttachPreview(Control preview)
    {
        if (s_dragPreview != null && s_dragPreview.IsAlive) { if (s_dragPreview.GetParent() != null) s_dragPreview.GetParent()!.RemoveChild(s_dragPreview); s_dragPreview.Free(); }
        s_dragPreview = preview;
        MakeIgnore(preview);
        preview.TopLevel = true;
        SceneTree.Instance.Root.AddChild(preview);
        preview.Position = s_mousePos;
    }

    private static void DropPendingPreview()
    {
        if (s_pendingPreview is { IsAlive: true } p) p.Free();
        s_pendingPreview = null;
    }

    private static void MakeIgnore(Control c)
    {
        c.MouseFilter = Control.MouseFilterEnum.Ignore;
        foreach (Node n in c.ChildrenSnapshot()) if (n is Control cc) MakeIgnore(cc);
    }

    private static void UpdateDrag(Vector2 pos)
    {
        if (s_dragPreview is { IsAlive: true } p) p.Position = pos;
        Control? over = FindControl(pos);
        s_dragOver = over;
        UpdateHover(over);
        s_dragCanDrop = over != null && Drop(over, pos, justCheck: true);
    }

    /// <summary>Offers the data to <paramref name="at"/> and then to each Control parent until one takes it (or a control with mouse_filter Stop
    /// refuses). With <paramref name="justCheck"/> nothing is dropped.</summary>
    private static bool Drop(Control at, Vector2 global, bool justCheck)
    {
        for (Control? c = at; c != null; c = c.TopLevel ? null : c.GetParent() as Control)
        {
            if (c.MouseFilter != Control.MouseFilterEnum.Ignore && c.CallCanDropData(ToLocal(c, global), s_dragData))
            {
                if (!justCheck) c.CallDropData(ToLocal(c, global), s_dragData);
                return true;
            }
            if (c.MouseFilter == Control.MouseFilterEnum.Stop) break;
        }
        return false;
    }

    private static void EndDrag(Vector2 pos)
    {
        Control? over = FindControl(pos);
        bool ok = over != null && Drop(over, pos, justCheck: false);
        FinishDrag(ok);
        UpdateHover(FindControl(pos));
    }

    internal static void CancelDrag()
    {
        if (!s_dragging) return;
        FinishDrag(false);
    }

    private static void FinishDrag(bool success)
    {
        s_dragging = false;
        s_dragSuccess = success;
        s_dragOver = null;
        s_dragCanDrop = false;
        if (s_dragPreview is { IsAlive: true } p)
        {
            if (p.GetParent() != null) p.GetParent()!.RemoveChild(p);
            p.Free();
        }
        s_dragPreview = null;
        BroadcastToControls(Control.NotificationDragEnd);
        s_dragData = default;
        s_dragSource = null;
    }

    private static void BroadcastToControls(int what)
    {
        foreach (Control c in s_controls.ToArray()) if (c.IsAlive) c.Notification(what);
    }

    // ---- per frame ----

    private static Vector2 s_lastEnginePointer;
    private static int s_engineButtons;

    /// <summary>The idle frame's GUI work: the engine's pointer (when there is one), layout, tooltips, and drawing.</summary>
    internal static void FrameUpdate(double delta)
    {
        if (s_controls.Count == 0 && s_deferred.Count == 0) return;
        if (NodeHost.Current.IsEngine && UiNative.Available) PollEnginePointer();
        FlushLayout();
        if (s_tooltipTimer >= 0)
        {
            s_tooltipTimer -= delta;
            if (s_tooltipTimer <= 0)
            {
                s_tooltipTimer = -1;
                ShowTooltip();
            }
        }
        if (NodeHost.Current.IsEngine && UiNative.Available) DrawAll();
    }

    /// <summary>The engine gives scripts no absolute pointer, so the GUI reads the one the host set for the UI (window pixels), maps it through the
    /// stretch into UI space and feeds Controls the same events a window would.</summary>
    private static void PollEnginePointer()
    {
        var xy = new float[2];
        try
        {
            UiNative.aver_ui_pointer(xy);
            var window = SceneTree.Instance.Root;
            Vector2 pos = window.WindowToContent(new Vector2(xy[0], xy[1]));
            Vector2 prev = s_lastEnginePointer;
            int down = 0;
            for (int b = 0; b < 3; b++) if (UiNative.aver_ui_pointer_down(b) != 0) down |= 1 << b;
            if (pos != prev)
            {
                s_lastEnginePointer = pos;
                var move = new InputEventMouseMotion { Position = pos, GlobalPosition = pos, Relative = pos - prev, ButtonMask = (MouseButtonMask)(down & 7) };
                Input.Absorb(move);
                HandleInput(move);
            }
            int changed = down ^ s_engineButtons;
            for (int b = 0; b < 3; b++)
            {
                if ((changed & (1 << b)) == 0) continue;
                var ev = new InputEventMouseButton
                {
                    Position = pos, GlobalPosition = pos, Pressed = (down & (1 << b)) != 0, ButtonIndex = (MouseButton)(b + 1), ButtonMask = (MouseButtonMask)(down & 7),
                };
                Input.Absorb(ev);
                HandleInput(ev);
            }
            s_engineButtons = down;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
    }

    // ---- drawing ----

    /// <summary>Replays every visible Control's recorded drawing into the engine's UI draw list: runs the draw pass of the ones that asked, applies
    /// the stretch, modulate and clipping, then draws the tooltip and the drag preview on top.</summary>
    internal static void DrawAll()
    {
        FlushLayout();
        Window window = SceneTree.Instance.Root;
        Transform2D toWindow = window.ContentToWindowTransform();
        if (window.IsLetterboxed()) DrawLetterbox(window.WindowPixels, window.GetContentScreenRect());
        foreach (Node n in window.ChildrenSnapshot()) DrawNode(n, toWindow);
        if (s_popups.Count > 0)
        {
            int previous = UiNative.aver_ui_layer();
            UiNative.aver_ui_set_layer(2);   // AVER_UI_LAYER_OVERLAY
            foreach (PopupWindow p in s_popups.ToArray()) DrawNode(p, toWindow, p);
            UiNative.aver_ui_set_layer(previous);
        }
        if (s_tooltipVisible && s_customTooltip == null) DrawDefaultTooltip(window, toWindow);
    }

    // The bars of a letterboxed window are black, over whatever the scene drew there (Godot clips the whole picture to the content rectangle).
    private static void DrawLetterbox(Vector2 window, Rect2 content)
    {
        const uint black = 0xFF000000;
        if (content.Position.X > 0f) UiNative.aver_ui_rect(0, 0, content.Position.X, window.Y, black);
        if (window.X - content.End.X > 0f) UiNative.aver_ui_rect(content.End.X, 0, window.X - content.End.X, window.Y, black);
        if (content.Position.Y > 0f) UiNative.aver_ui_rect(content.Position.X, 0, content.Size.X, content.Position.Y, black);
        if (window.Y - content.End.Y > 0f) UiNative.aver_ui_rect(content.Position.X, content.End.Y, content.Size.X, window.Y - content.End.Y, black);
    }

    private static void DrawNode(Node node, Transform2D toWindow, Node? popupRoot = null)
    {
        if (node is SubViewport) return;
        if (node is PopupWindow shown && node != popupRoot && s_popups.Contains(shown)) return;   // drawn last, on the overlay layer, each once
        int clips = 0;
        if (node is CanvasItem ci)
        {
            if (!ci.Visible) return;
            ci.RunDraw();
            Transform2D xf = toWindow * ci.GlobalCanvasTransform;
            Color mod = ci.GetCombinedModulate();
            foreach (CanvasItem.DrawCommand cmd in ci.Commands) Replay(cmd, xf, mod);
            if (ci.ClipsChildren)
            {
                Rect2 r = toWindow * ci.ClipRectGlobal;
                UiNative.aver_ui_push_clip((int)MathF.Floor(r.Position.X), (int)MathF.Floor(r.Position.Y), (int)MathF.Ceiling(r.End.X), (int)MathF.Ceiling(r.End.Y));
                clips++;
            }
        }
        foreach (Node c in node.ChildrenSnapshot()) DrawNode(c, toWindow, popupRoot);
        for (; clips > 0; clips--) UiNative.aver_ui_pop_clip();
    }

    private static uint Tint(uint rgba, Color m)
    {
        if (m.R == 1f && m.G == 1f && m.B == 1f && m.A == 1f) return rgba;
        uint r = (uint)MathF.Round((rgba & 0xFF) * m.R), g = (uint)MathF.Round(((rgba >> 8) & 0xFF) * m.G);
        uint b = (uint)MathF.Round(((rgba >> 16) & 0xFF) * m.B), a = (uint)MathF.Round(((rgba >> 24) & 0xFF) * m.A);
        return Math.Min(r, 255u) | (Math.Min(g, 255u) << 8) | (Math.Min(b, 255u) << 16) | (Math.Min(a, 255u) << 24);
    }

    private static void Replay(CanvasItem.DrawCommand cmd, Transform2D xf, Color mod)
    {
        Rect2 r = xf * cmd.Rect;
        switch (cmd.Type)
        {
            case CanvasItem.DrawCommand.Kind.Rect:
                UiNative.aver_ui_rect(r.Position.X, r.Position.Y, r.Size.X, r.Size.Y, Tint(StyleBoxFlat.Pack(cmd.Color), mod));
                break;
            case CanvasItem.DrawCommand.Kind.StyleBox:
            {
                UiStyleBoxStruct s = cmd.Box;
                s.BgColor = Tint(s.BgColor, mod);
                s.BorderColorL = Tint(s.BorderColorL, mod); s.BorderColorT = Tint(s.BorderColorT, mod);
                s.BorderColorR = Tint(s.BorderColorR, mod); s.BorderColorB = Tint(s.BorderColorB, mod);
                s.ShadowColor = Tint(s.ShadowColor, mod);
                s.Modulate = Tint(s.Modulate, mod);
                s.LineColor = Tint(s.LineColor, mod);
                UiNative.DrawStyleBox(s, r.Position.X, r.Position.Y, r.Size.X, r.Size.Y);
                break;
            }
            case CanvasItem.DrawCommand.Kind.Text:
            {
                int align = cmd.Align switch { HorizontalAlignment.Center => 1, HorizontalAlignment.Right => 2, _ => 0 };
                float scale = Math.Max(Math.Abs(xf.X.X), 0.0001f);
                string? family = cmd.Family.Length > 0 ? cmd.Family : null;
                float width = cmd.Width > 0 ? cmd.Width * scale : 0f;
                if (cmd.Fx.Any)
                {
                    CanvasItem.TextFx fx = cmd.Fx;
                    UiNative.aver_ui_text_block_fx(r.Position.X, r.Position.Y, cmd.Text, Tint(StyleBoxFlat.Pack(cmd.Color), mod), family, cmd.FontSize * scale, cmd.Flags, width, align,
                                                   fx.OutlineSize * scale, Tint(StyleBoxFlat.Pack(fx.OutlineColor), mod), Tint(StyleBoxFlat.Pack(fx.ShadowColor), mod),
                                                   fx.ShadowOffset.X * scale, fx.ShadowOffset.Y * scale, fx.ShadowOutlineSize * scale, null);
                }
                else
                    UiNative.aver_ui_text_block(r.Position.X, r.Position.Y, cmd.Text, Tint(StyleBoxFlat.Pack(cmd.Color), mod), family, cmd.FontSize * scale, cmd.Flags, width, align, null);
                break;
            }
            case CanvasItem.DrawCommand.Kind.PushClip:
                UiNative.aver_ui_push_clip((int)MathF.Floor(r.Position.X), (int)MathF.Floor(r.Position.Y), (int)MathF.Ceiling(r.End.X), (int)MathF.Ceiling(r.End.Y));
                break;
            case CanvasItem.DrawCommand.Kind.PopClip:
                UiNative.aver_ui_pop_clip();
                break;
            case CanvasItem.DrawCommand.Kind.Texture:
                if (cmd.Handle != 0)
                    UiNative.aver_ui_textured_rect(r.Position.X, r.Position.Y, r.Size.X, r.Size.Y, cmd.Handle, cmd.Uv.Position.X, cmd.Uv.Position.Y, cmd.Uv.End.X, cmd.Uv.End.Y, Tint(StyleBoxFlat.Pack(cmd.Color), mod));
                break;
        }
    }

    private static void DrawDefaultTooltip(Window window, Transform2D toWindow)
    {
        Control probe = TooltipProbe;
        StyleBox panel = probe.GetThemeStylebox("panel", "TooltipPanel");
        int px = Math.Max(1, probe.GetThemeFontSize("font_size", "TooltipLabel"));
        Color text = probe.GetThemeColor("font_color", "TooltipLabel");
        Vector2 size = DefaultTooltipSize(s_tooltipText);
        var rect = toWindow * new Rect2(s_tooltipPos, size);
        int previous = UiNative.aver_ui_layer();
        UiNative.aver_ui_set_layer(3);   // AVER_UI_LAYER_TOOLTIP
        panel.Draw(rect);
        Vector2 inner = panel.GetOffset();
        UiNative.aver_ui_text_block(rect.Position.X + inner.X, rect.Position.Y + inner.Y, s_tooltipText, StyleBoxFlat.Pack(text), null, px, 0, 0f, 0, null);
        UiNative.aver_ui_set_layer(previous);
    }
}

/// <summary>The hidden node that gives the GUI its turn in the engine's input chain (the managed dispatch calls <see cref="Gui.HandleInput"/> itself).</summary>
internal sealed class GuiDriver : Node
{
    public GuiDriver()
    {
        ProcessMode = ProcessModeEnum.Always;
        Persistent = true;
        SetProcessUnhandledInput(true);
    }

    public override void OnUnhandledInput(Aver.Framework.InputEvent e)
    {
        InputEvent ge = InputBridge.FromFramework(e);
        if (Gui.HandleInput(ge)) Aver.Framework.Hosting.ExtensionHost.MarkHandled(e);
    }
}
