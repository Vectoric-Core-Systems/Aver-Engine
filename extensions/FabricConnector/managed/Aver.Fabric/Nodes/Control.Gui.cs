// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Control's input side (G5 U1): _gui_input and mouse_filter, keyboard focus and its neighbours, tooltips, drag and drop. The machinery is Gui.cs; this is
// the Godot surface on a Control.

namespace Aver.Fabric;

public partial class Control
{
    private FocusModeEnum _focusMode = FocusModeEnum.None;
    private readonly NodePath?[] _neighbor = new NodePath?[4];   // left, top, right, bottom
    private NodePath? _focusNext, _focusPrev;

    public MouseFilterEnum MouseFilter { get; set; } = MouseFilterEnum.Stop;
    public string TooltipText { get; set; } = "";

    public FocusModeEnum FocusMode
    {
        get => _focusMode;
        set
        {
            _focusMode = value;
            if (value == FocusModeEnum.None) ReleaseFocus();
        }
    }

    public NodePath FocusNeighborLeft { get => _neighbor[0] ?? new NodePath(""); set => _neighbor[0] = value; }
    public NodePath FocusNeighborTop { get => _neighbor[1] ?? new NodePath(""); set => _neighbor[1] = value; }
    public NodePath FocusNeighborRight { get => _neighbor[2] ?? new NodePath(""); set => _neighbor[2] = value; }
    public NodePath FocusNeighborBottom { get => _neighbor[3] ?? new NodePath(""); set => _neighbor[3] = value; }
    public NodePath FocusNext { get => _focusNext ?? new NodePath(""); set => _focusNext = value; }
    public NodePath FocusPrevious { get => _focusPrev ?? new NodePath(""); set => _focusPrev = value; }

    public void SetFocusNeighbor(Side side, NodePath neighbor) => _neighbor[(int)side] = neighbor;
    public NodePath GetFocusNeighbor(Side side) => _neighbor[(int)side] ?? new NodePath("");

    // ---- input virtuals ----

    /// <summary>Called with every input event that reaches the control (mouse events at the control's local position). Call <see cref="AcceptEvent"/> to stop it
    /// reaching the parents and the rest of the game.</summary>
    public virtual void _GuiInput(InputEvent @event) { }

    /// <summary>Whether the point (in the control's own space) is on the control: the rectangle by default. Mouse events only reach a control where this is true.</summary>
    public virtual bool _HasPoint(Vector2 point) => new Rect2(Vector2.Zero, _size).HasPoint(point);

    public bool HasPoint(Vector2 point) => _HasPoint(point);

    internal bool HasPointGlobal(Vector2 global)
    {
        Transform2D t = GetGlobalTransform();
        if (t.Determinant() == 0f) return false;
        return _HasPoint(t.AffineInverse() * global);
    }

    internal void CallGuiInput(InputEvent e)
    {
        EmitSignal(SignalName.GuiInput, e);   // the signal first, so a connected handler can accept the event before the virtual sees it
        _GuiInput(e);
    }

    /// <summary>Stops the event being passed on: no parent control receives it and the game's unhandled input never sees it.</summary>
    public void AcceptEvent() => Gui.Accept();

    // ---- focus ----

    /// <summary>Takes keyboard focus (the control needs a focus mode and to be visible in the tree).</summary>
    public void GrabFocus() => Gui.GrabFocus(this);

    public void ReleaseFocus() => Gui.ReleaseFocus(this);

    public bool HasFocus() => Gui.FocusOwner == this;

    public void GrabClickFocus() { if (FocusMode != FocusModeEnum.None) Gui.GrabFocus(this); }

    private Control? NeighborFrom(NodePath? path)
    {
        if (path == null || path.IsEmpty) return null;
        return GetNodeOrNull(path) as Control;
    }

    /// <summary>The next control Tab reaches: <see cref="FocusNext"/> if set, else the next control in tree order that accepts keyboard focus (focus mode All),
    /// wrapping round.</summary>
    public Control? FindNextValidFocus()
    {
        if (NeighborFrom(_focusNext) is { } explicitNext && explicitNext.FocusMode != FocusModeEnum.None) return explicitNext;
        return ScanFocus(forward: true);
    }

    public Control? FindPrevValidFocus()
    {
        if (NeighborFrom(_focusPrev) is { } explicitPrev && explicitPrev.FocusMode != FocusModeEnum.None) return explicitPrev;
        return ScanFocus(forward: false);
    }

    private Control? ScanFocus(bool forward)
    {
        var order = new List<Control>();
        foreach (Node n in NodeHost.PreOrder(GetViewport() ?? SceneTree.Instance.Root))
            if (n is Control c && (c == this || (c.Visible && c.IsVisibleInTree()))) order.Add(c);
        int at = order.IndexOf(this);
        if (at < 0) return null;
        for (int i = 1; i <= order.Count; i++)
        {
            Control c = order[((forward ? at + i : at - i) % order.Count + order.Count) % order.Count];
            if (c.FocusMode == FocusModeEnum.All && c.CanProcess()) return c;
        }
        return null;
    }

    /// <summary>The control that arrow keys in <paramref name="side"/> move focus to: the neighbour the scene set, else the nearest focusable control in that
    /// direction (nearest by the gap between the rectangles, with a penalty for sliding sideways). Godot's search is a segment-distance search over the same
    /// candidates; the choice agrees on grids and rows and can differ on irregular layouts.</summary>
    public Control? FindValidFocusNeighbor(Side side)
    {
        NodePath? path = _neighbor[(int)side];
        if (path == null || path.IsEmpty) return GeometricNeighbor(side);
        Control current = this;
        for (int hops = 0; hops < 16; hops++)
        {
            Control? next = current.NeighborFrom(current._neighbor[(int)side]);
            if (next == null) return null;
            if (next.FocusMode != FocusModeEnum.None) return next;
            current = next;   // a neighbour that cannot take focus hands on to its own neighbour in the same direction
        }
        return null;
    }

    private static Rect2 GlobalBounds(Control c) => c.GetGlobalTransform() * new Rect2(Vector2.Zero, c.Size);

    private Control? GeometricNeighbor(Side side)
    {
        Rect2 from = GlobalBounds(this);
        Vector2 dir = side switch { Side.Left => new Vector2(-1, 0), Side.Top => new Vector2(0, -1), Side.Right => new Vector2(1, 0), _ => new Vector2(0, 1) };
        Control? best = null;
        float bestScore = float.MaxValue;
        foreach (Control c in Gui.FocusableInTreeOrder())
        {
            if (c == this || c.IsAncestorOf(this) || IsAncestorOf(c)) continue;
            Rect2 to = GlobalBounds(c);
            // the candidate has to be beyond this control's edge in the direction asked
            float along = dir.X != 0 ? (dir.X > 0 ? to.Position.X - from.End.X : from.Position.X - to.End.X)
                                     : (dir.Y > 0 ? to.Position.Y - from.End.Y : from.Position.Y - to.End.Y);
            float centreAlong = dir.X != 0 ? (to.GetCenter().X - from.GetCenter().X) * dir.X : (to.GetCenter().Y - from.GetCenter().Y) * dir.Y;
            if (centreAlong <= 0f) continue;
            along = Math.Max(0f, along);
            float perp = dir.X != 0 ? Gap(from.Position.Y, from.End.Y, to.Position.Y, to.End.Y) : Gap(from.Position.X, from.End.X, to.Position.X, to.End.X);
            float offCentre = dir.X != 0 ? Math.Abs(to.GetCenter().Y - from.GetCenter().Y) : Math.Abs(to.GetCenter().X - from.GetCenter().X);
            float score = along + 3f * perp + 0.01f * offCentre;
            if (score < bestScore) { bestScore = score; best = c; }
        }
        return best;
    }

    private static float Gap(float a0, float a1, float b0, float b1) => Math.Max(0f, Math.Max(b0 - a1, a0 - b1));

    // ---- tooltips ----

    /// <summary>Called to find the tooltip text for the point (in the control's own space): <see cref="TooltipText"/> by default.</summary>
    public virtual string _GetTooltip(Vector2 atPosition) => TooltipText;

    public string GetTooltip(Vector2 atPosition = default) => _GetTooltip(atPosition);

    /// <summary>A script's own tooltip control for <paramref name="forText"/> (null for the default text tooltip). The control is shown for the tooltip's life
    /// and freed after it.</summary>
    public virtual GodotObject? _MakeCustomTooltip(string forText) => null;

    internal Control? CallMakeCustomTooltip(string text) => _MakeCustomTooltip(text) as Control;

    // ---- drag and drop ----

    /// <summary>Called when a drag starts on the control (the left button held and moved past a few pixels). Return the data to start a drag, nil for none;
    /// call <see cref="SetDragPreview"/> inside to show something under the pointer.</summary>
    public virtual Variant _GetDragData(Vector2 atPosition) => default;

    /// <summary>Called as a drag moves over the control and when it is released: whether the data can be dropped here.</summary>
    public virtual bool _CanDropData(Vector2 atPosition, Variant data) => false;

    /// <summary>Called when the data is dropped on a control that said it can take it.</summary>
    public virtual void _DropData(Vector2 atPosition, Variant data) { }

    internal Variant CallGetDragData(Vector2 at) => _GetDragData(at);
    internal bool CallCanDropData(Vector2 at, Variant data) => _CanDropData(at, data);
    internal void CallDropData(Vector2 at, Variant data) => _DropData(at, data);

    /// <summary>Shows <paramref name="control"/> under the pointer for the rest of the drag (the control is freed when the drag ends).</summary>
    public void SetDragPreview(Control control) => Gui.SetDragPreview(control);

    /// <summary>Starts a drag now with the given data and an optional preview.</summary>
    public void ForceDrag(Variant data, Control? preview)
    {
        if (data.IsNil) return;
        Gui.BeginDrag(this, data);
        if (preview != null) Gui.SetDragPreview(preview);
    }

    /// <summary>Moves the pointer (the engine has no pointer warp; the GUI's position is set).</summary>
    public void WarpMouse(Vector2 position) { }
}
