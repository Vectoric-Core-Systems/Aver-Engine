// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Containers (G5 U1): Container, BoxContainer (H/V), GridContainer, MarginContainer, CenterContainer, PanelContainer, ScrollContainer and Panel. A
// container sorts its children at the end of the frame (or when Gui.FlushLayout runs) with Godot's algorithms: minimum sizes first, then the space left
// over shared by the children that expand (stretch ratios), each child fitted in its slot by its size flags. Scroll bars are not nodes yet (U2): a
// ScrollContainer reserves their room and scrolls through scroll_horizontal / scroll_vertical and the wheel.

namespace Aver.Fabric;

/// <summary>Base of the nodes that lay their Control children out (Godot's Container).</summary>
public class Container : Control
{
    public new class SignalName : Control.SignalName
    {
        public static readonly StringName PreSortChildren = "pre_sort_children";
        public static readonly StringName SortChildren = "sort_children";
    }

    public const int NotificationPreSortChildren = 50;
    public const int NotificationSortChildren = 51;

    public event Action SortChildrenSignal { add => Connect(SignalName.SortChildren, Callable.From(value)); remove => Disconnect(SignalName.SortChildren, Callable.From(value)); }

    private bool _sortQueued;

    public Container()
    {
        Connect(Node.SignalName.ChildOrderChanged, Callable.From(OnChildrenChanged));
    }

    private void OnChildrenChanged()
    {
        UpdateMinimumSize();
        QueueSort();
    }

    /// <summary>Asks for the children to be laid out again at the end of the frame.</summary>
    public void QueueSort()
    {
        if (!IsInsideTree() || _sortQueued) return;
        _sortQueued = true;
        Gui.Defer(RunSort);
    }

    private void RunSort()
    {
        _sortQueued = false;
        if (!IsAlive || !IsInsideTree()) return;
        Notification(NotificationPreSortChildren);
        EmitSignal(SignalName.PreSortChildren);
        Notification(NotificationSortChildren);
        SortChildren();
        EmitSignal(SignalName.SortChildren);
    }

    /// <summary>A built-in container's layout. Scripts sort in <c>_Notification</c> on <see cref="NotificationSortChildren"/> instead.</summary>
    internal virtual void SortChildren() { }

    internal void OnChildMinimumSizeChanged()
    {
        UpdateMinimumSize();
        QueueSort();
    }

    /// <summary>Places a child inside a rectangle the way its size flags say: filling it, or at its minimum size at the start, centre or end of it.</summary>
    public void FitChildInRect(Control child, Rect2 rect)
    {
        Vector2 min = child.GetCombinedMinimumSize();
        Rect2 r = rect;
        if ((child.SizeFlagsHorizontal & SizeFlags.Fill) == 0)
        {
            r.Size = new Vector2(min.X, r.Size.Y);
            if ((child.SizeFlagsHorizontal & SizeFlags.ShrinkEnd) != 0) r.Position += new Vector2(rect.Size.X - min.X, 0);
            else if ((child.SizeFlagsHorizontal & SizeFlags.ShrinkCenter) != 0) r.Position += new Vector2(MathF.Floor((rect.Size.X - min.X) / 2f), 0);
        }
        if ((child.SizeFlagsVertical & SizeFlags.Fill) == 0)
        {
            r.Size = new Vector2(r.Size.X, min.Y);
            if ((child.SizeFlagsVertical & SizeFlags.ShrinkEnd) != 0) r.Position += new Vector2(0, rect.Size.Y - min.Y);
            else if ((child.SizeFlagsVertical & SizeFlags.ShrinkCenter) != 0) r.Position += new Vector2(0, MathF.Floor((rect.Size.Y - min.Y) / 2f));
        }
        child.SetPosition(r.Position);
        child.SetSize(r.Size);
        child.Rotation = 0f;
        child.Scale = Vector2.One;
    }

    /// <summary>The visible Control children that take part in the layout.</summary>
    internal List<Control> LaidOutChildren()
    {
        var list = new List<Control>();
        foreach (Node n in ChildrenSnapshot())
            if (n is Control c && c.Visible && !c.TopLevel) list.Add(c);
        return list;
    }
}

/// <summary>Lays children out in a row or a column (Godot's BoxContainer).</summary>
public class BoxContainer : Container
{
    public enum AlignmentMode { Begin = 0, Center = 1, End = 2 }

    private bool _vertical;
    private AlignmentMode _alignment = AlignmentMode.Begin;

    public bool Vertical { get => _vertical; set { _vertical = value; UpdateMinimumSize(); QueueSort(); } }

    public AlignmentMode Alignment { get => _alignment; set { _alignment = value; QueueSort(); } }

    public BoxContainer() { }

    internal int Separation => GetThemeConstant("separation");

    internal override Vector2 NativeMinimumSize()
    {
        int sep = Separation;
        int w = 0, h = 0;
        bool first = true;
        foreach (Control c in LaidOutChildren())
        {
            Vector2 ms = c.GetCombinedMinimumSize();
            int mw = (int)ms.X, mh = (int)ms.Y;
            if (_vertical) { h += mh + (first ? 0 : sep); w = Math.Max(w, mw); }
            else { w += mw + (first ? 0 : sep); h = Math.Max(h, mh); }
            first = false;
        }
        return new Vector2(w, h);
    }

    private sealed class Slot
    {
        public Control Child = null!;
        public int Min, Final;
        public bool Stretch;
    }

    internal override void SortChildren()
    {
        List<Control> kids = LaidOutChildren();
        int sep = Separation;
        var slots = new List<Slot>();
        int stretchMin = 0, stretchAvail = 0;
        float ratioTotal = 0f;
        foreach (Control c in kids)
        {
            Vector2 ms = c.GetCombinedMinimumSize();
            int min = _vertical ? (int)ms.Y : (int)ms.X;
            bool stretch = ((_vertical ? c.SizeFlagsVertical : c.SizeFlagsHorizontal) & SizeFlags.Expand) != 0;
            stretchMin += min;
            if (stretch) { stretchAvail += min; ratioTotal += c.SizeFlagsStretchRatio; }
            slots.Add(new Slot { Child = c, Min = min, Final = min, Stretch = stretch });
        }
        if (slots.Count == 0) return;

        Vector2 size = Size;
        int axis = (int)(_vertical ? size.Y : size.X);
        int stretchMax = axis - (slots.Count - 1) * sep;
        int diff = Math.Max(0, stretchMax - stretchMin);
        stretchAvail += diff;

        // Children whose share would be below their minimum stop stretching and the rest share what is left, until every share fits.
        bool stretched = false;
        while (ratioTotal > 0f)
        {
            stretched = true;
            bool fit = true;
            float error = 0f;
            foreach (Slot s in slots)
            {
                if (!s.Stretch) continue;
                float share = stretchAvail * s.Child.SizeFlagsStretchRatio / ratioTotal;
                if (share < s.Min)
                {
                    s.Stretch = false;
                    ratioTotal -= s.Child.SizeFlagsStretchRatio;
                    stretchAvail -= s.Min;
                    s.Final = s.Min;
                    fit = false;
                    break;
                }
                int whole = (int)share;
                error += share - whole;
                if (error >= 1f - 1e-4f) { whole++; error -= 1f; }
                s.Final = whole;
            }
            if (fit) break;
        }

        int ofs = 0;
        if (!stretched)
        {
            if (_alignment == AlignmentMode.Center) ofs = diff / 2;
            else if (_alignment == AlignmentMode.End) ofs = diff;
        }
        bool first = true;
        int idx = 0;
        foreach (Slot s in slots)
        {
            if (first) first = false;
            else ofs += sep;
            int from = ofs, to = ofs + s.Final;
            if (s.Stretch && idx == slots.Count - 1) to = axis;   // the last one absorbs the rounding
            int length = to - from;
            Rect2 rect = _vertical ? new Rect2(0, from, size.X, length) : new Rect2(from, 0, length, size.Y);
            FitChildInRect(s.Child, rect);
            ofs = to;
            idx++;
        }
    }
}

/// <summary>A row (Godot's HBoxContainer).</summary>
public class HBoxContainer : BoxContainer { }

/// <summary>A column (Godot's VBoxContainer).</summary>
public class VBoxContainer : BoxContainer
{
    public VBoxContainer() { Vertical = true; }
}

/// <summary>A grid with a fixed number of columns (Godot's GridContainer).</summary>
public class GridContainer : Container
{
    private int _columns = 1;

    public int Columns { get => _columns; set { _columns = Math.Max(1, value); UpdateMinimumSize(); QueueSort(); } }

    internal int HSeparation => GetThemeConstant("h_separation");
    internal int VSeparation => GetThemeConstant("v_separation");

    internal override Vector2 NativeMinimumSize()
    {
        var colMin = new Dictionary<int, int>();
        var rowMin = new Dictionary<int, int>();
        int index = 0;
        foreach (Control c in LaidOutChildren())
        {
            int row = index / _columns, col = index % _columns;
            index++;
            Vector2 ms = c.GetCombinedMinimumSize();
            colMin[col] = Math.Max(colMin.GetValueOrDefault(col), (int)ms.X);
            rowMin[row] = Math.Max(rowMin.GetValueOrDefault(row), (int)ms.Y);
        }
        int maxCol = Math.Min(index, _columns), maxRow = index == 0 ? 0 : (index + _columns - 1) / _columns;
        int w = colMin.Values.Sum() + HSeparation * Math.Max(0, maxCol - 1);
        int h = rowMin.Values.Sum() + VSeparation * Math.Max(0, maxRow - 1);
        return new Vector2(w, h);
    }

    internal override void SortChildren()
    {
        List<Control> kids = LaidOutChildren();
        var colMin = new Dictionary<int, int>();
        var rowMin = new Dictionary<int, int>();
        var colExpanded = new SortedSet<int>();
        var rowExpanded = new SortedSet<int>();
        int hsep = HSeparation, vsep = VSeparation;
        int index = 0;
        foreach (Control c in kids)
        {
            int row = index / _columns, col = index % _columns;
            index++;
            Vector2 ms = c.GetCombinedMinimumSize();
            colMin[col] = Math.Max(colMin.GetValueOrDefault(col), (int)ms.X);
            rowMin[row] = Math.Max(rowMin.GetValueOrDefault(row), (int)ms.Y);
            if ((c.SizeFlagsHorizontal & SizeFlags.Expand) != 0) colExpanded.Add(col);
            if ((c.SizeFlagsVertical & SizeFlags.Expand) != 0) rowExpanded.Add(row);
        }
        if (index == 0) return;
        for (int i = index; i < _columns; i++) colExpanded.Add(i);   // columns nothing fills count as expanded

        int maxCol = Math.Min(index, _columns), maxRow = (index + _columns - 1) / _columns;
        Vector2 remaining = Size;
        foreach (var (k, v) in colMin) if (!colExpanded.Contains(k)) remaining.X -= v;
        foreach (var (k, v) in rowMin) if (!rowExpanded.Contains(k)) remaining.Y -= v;
        remaining.X -= hsep * (maxCol - 1);
        remaining.Y -= vsep * (maxRow - 1);

        bool fits = false;
        while (!fits && colExpanded.Count > 0)
        {
            fits = true;
            int widest = colExpanded.Min;
            foreach (int e in colExpanded)
            {
                if (colMin.GetValueOrDefault(e) > (int)(remaining.X / colExpanded.Count))
                {
                    if (colMin.GetValueOrDefault(e) > colMin.GetValueOrDefault(widest)) widest = e;
                    fits = false;
                }
            }
            if (!fits)
            {
                colExpanded.Remove(widest);
                remaining.X -= colMin.GetValueOrDefault(widest);
            }
        }
        fits = false;
        while (!fits && rowExpanded.Count > 0)
        {
            fits = true;
            int tallest = rowExpanded.Min;
            foreach (int e in rowExpanded)
            {
                if (rowMin.GetValueOrDefault(e) > (int)(remaining.Y / rowExpanded.Count))
                {
                    if (rowMin.GetValueOrDefault(e) > rowMin.GetValueOrDefault(tallest)) tallest = e;
                    fits = false;
                }
            }
            if (!fits)
            {
                rowExpanded.Remove(tallest);
                remaining.Y -= rowMin.GetValueOrDefault(tallest);
            }
        }
        int colWidth = colExpanded.Count > 0 ? (int)(remaining.X / colExpanded.Count) : 0;
        int rowHeight = rowExpanded.Count > 0 ? (int)(remaining.Y / rowExpanded.Count) : 0;

        int colOfs = 0, rowOfs = 0;
        index = 0;
        foreach (Control c in kids)
        {
            int row = index / _columns, col = index % _columns;
            index++;
            if (col == 0)
            {
                colOfs = 0;
                if (row > 0) rowOfs += (rowExpanded.Contains(row - 1) ? rowHeight : rowMin.GetValueOrDefault(row - 1)) + vsep;
            }
            var s = new Vector2(colExpanded.Contains(col) ? colWidth : colMin.GetValueOrDefault(col), rowExpanded.Contains(row) ? rowHeight : rowMin.GetValueOrDefault(row));
            FitChildInRect(c, new Rect2(colOfs, rowOfs, s.X, s.Y));
            colOfs += (int)s.X + hsep;
        }
    }
}

/// <summary>Insets its children by four margins (Godot's MarginContainer; the margins are theme constants margin_left, margin_top, ...).</summary>
public class MarginContainer : Container
{
    private int Margin(string name) => GetThemeConstant(name);

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 max = Vector2.Zero;
        foreach (Control c in LaidOutChildren())
        {
            Vector2 s = c.GetCombinedMinimumSize();
            max = new Vector2(Math.Max(max.X, s.X), Math.Max(max.Y, s.Y));
        }
        return max + new Vector2(Margin("margin_left") + Margin("margin_right"), Margin("margin_top") + Margin("margin_bottom"));
    }

    internal override void SortChildren()
    {
        int l = Margin("margin_left"), t = Margin("margin_top"), r = Margin("margin_right"), b = Margin("margin_bottom");
        Vector2 size = Size;
        foreach (Control c in LaidOutChildren())
            FitChildInRect(c, new Rect2(l, t, size.X - l - r, size.Y - t - b));
    }
}

/// <summary>Centres each child at its minimum size (Godot's CenterContainer).</summary>
public class CenterContainer : Container
{
    private bool _useTopLeft;

    /// <summary>Centre on the container's top-left corner instead of its middle.</summary>
    public bool UseTopLeft { get => _useTopLeft; set { _useTopLeft = value; UpdateMinimumSize(); QueueSort(); } }

    internal override Vector2 NativeMinimumSize()
    {
        if (_useTopLeft) return Vector2.Zero;
        Vector2 ms = Vector2.Zero;
        foreach (Control c in LaidOutChildren())
        {
            Vector2 s = c.GetCombinedMinimumSize();
            ms = new Vector2(Math.Max(ms.X, s.X), Math.Max(ms.Y, s.Y));
        }
        return ms;
    }

    internal override void SortChildren()
    {
        Vector2 size = Size;
        foreach (Control c in LaidOutChildren())
        {
            Vector2 min = c.GetCombinedMinimumSize();
            Vector2 ofs = ((_useTopLeft ? Vector2.Zero : size) - min) / 2f;
            FitChildInRect(c, new Rect2(ofs, min));
        }
    }
}

/// <summary>A container that draws the theme's "panel" style box and keeps its children inside its margins (Godot's PanelContainer).</summary>
public class PanelContainer : Container
{
    internal StyleBox PanelStyle => GetThemeStylebox("panel");

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 ms = Vector2.Zero;
        foreach (Control c in LaidOutChildren())
        {
            Vector2 s = c.GetCombinedMinimumSize();
            ms = new Vector2(Math.Max(ms.X, s.X), Math.Max(ms.Y, s.Y));
        }
        return ms + PanelStyle.GetMinimumSize();
    }

    internal override void SortChildren()
    {
        StyleBox style = PanelStyle;
        Vector2 size = Size - style.GetMinimumSize();
        Vector2 ofs = style.GetOffset();
        foreach (Control c in LaidOutChildren()) FitChildInRect(c, new Rect2(ofs, size));
    }

    public override void _Draw() => DrawStyleBox(PanelStyle, new Rect2(Vector2.Zero, Size));
}

/// <summary>A rectangle that draws the theme's "panel" style box (Godot's Panel).</summary>
public class Panel : Control
{
    public override void _Draw() => DrawStyleBox(GetThemeStylebox("panel"), new Rect2(Vector2.Zero, Size));
}

/// <summary>Scrolls one big child inside its own rectangle (Godot's ScrollContainer). The scroll bars are not nodes yet: their room is reserved
/// and the position is scroll_horizontal / scroll_vertical (and the wheel).</summary>
public class ScrollContainer : Container
{
    public enum ScrollMode { Disabled = 0, Auto = 1, ShowAlways = 2, ShowNever = 3, Reserve = 4 }

    public new class SignalName : Container.SignalName
    {
        public static readonly StringName ScrollStarted = "scroll_started";
        public static readonly StringName ScrollEnded = "scroll_ended";
    }

    private int _scrollH, _scrollV;
    private ScrollMode _hMode = ScrollMode.Auto, _vMode = ScrollMode.Auto;
    private Vector2 _childMin;
    private bool _hBar, _vBar;

    public ScrollContainer() { ClipContents = true; }

    public ScrollMode HorizontalScrollMode { get => _hMode; set { _hMode = value; UpdateMinimumSize(); QueueSort(); } }
    public ScrollMode VerticalScrollMode { get => _vMode; set { _vMode = value; UpdateMinimumSize(); QueueSort(); } }
    public bool FollowFocus { get; set; }
    public int ScrollDeadzone { get; set; }

    public int ScrollHorizontal { get => _scrollH; set { _scrollH = ClampScroll(value, true); QueueSort(); QueueRedraw(); if (_barsHooked) SyncBars(); } }
    public int ScrollVertical { get => _scrollV; set { _scrollV = ClampScroll(value, false); QueueSort(); QueueRedraw(); if (_barsHooked) SyncBars(); } }

    /// <summary>Whether the scroll bars are showing (they are not drawn here; a layout decision).</summary>
    public bool HorizontalBarVisible => _hBar;
    public bool VerticalBarVisible => _vBar;

    private int BarSize => GetThemeConstant("scrollbar_size", "VScrollBar");

    private int ClampScroll(int v, bool horizontal)
    {
        Vector2 inner = ViewSize();
        float max = horizontal ? _childMin.X - inner.X : _childMin.Y - inner.Y;
        return (int)Math.Clamp(v, 0, Math.Max(0, (int)MathF.Ceiling(max)));
    }

    private Vector2 ViewSize()
    {
        StyleBox style = GetThemeStylebox("panel");
        Vector2 s = Size - style.GetMinimumSize();
        if (_vBar) s.X -= BarSize;
        if (_hBar) s.Y -= BarSize;
        return new Vector2(Math.Max(0, s.X), Math.Max(0, s.Y));
    }

    private Vector2 LargestChildMinimum()
    {
        Vector2 m = Vector2.Zero;
        foreach (Control c in LaidOutChildren())
        {
            Vector2 s = c.GetCombinedMinimumSize();
            m = new Vector2(Math.Max(m.X, s.X), Math.Max(m.Y, s.Y));
        }
        return m;
    }

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 min = Vector2.Zero;
        Vector2 child = LargestChildMinimum();
        if (_hMode == ScrollMode.Disabled) min.X = child.X;
        if (_vMode == ScrollMode.Disabled) min.Y = child.Y;
        if (_vMode == ScrollMode.ShowAlways || _vMode == ScrollMode.Reserve) min.X += BarSize;
        if (_hMode == ScrollMode.ShowAlways || _hMode == ScrollMode.Reserve) min.Y += BarSize;
        return min + GetThemeStylebox("panel").GetMinimumSize();
    }

    internal override void SortChildren()
    {
        StyleBox style = GetThemeStylebox("panel");
        Vector2 outer = Size - style.GetMinimumSize();
        Vector2 ofs = style.GetOffset();
        _childMin = LargestChildMinimum();

        bool Show(ScrollMode mode, float content, float room) => mode switch
        {
            ScrollMode.Disabled or ScrollMode.ShowNever => false,
            ScrollMode.ShowAlways or ScrollMode.Reserve => true,
            _ => content > room,
        };
        // A bar takes room from the other axis, which can make that one need a bar too: settle in two passes.
        _hBar = _vBar = false;
        for (int pass = 0; pass < 2; pass++)
        {
            _vBar = Show(_vMode, _childMin.Y, outer.Y - (_hBar ? BarSize : 0));
            _hBar = Show(_hMode, _childMin.X, outer.X - (_vBar ? BarSize : 0));
        }
        Vector2 view = new(Math.Max(0, outer.X - (_vBar ? BarSize : 0)), Math.Max(0, outer.Y - (_hBar ? BarSize : 0)));
        _scrollH = (int)Math.Clamp(_scrollH, 0, Math.Max(0, (int)MathF.Ceiling(_childMin.X - view.X)));
        _scrollV = (int)Math.Clamp(_scrollV, 0, Math.Max(0, (int)MathF.Ceiling(_childMin.Y - view.Y)));

        foreach (Control c in LaidOutChildren())
        {
            Vector2 min = c.GetCombinedMinimumSize();
            var r = new Rect2(-new Vector2(_scrollH, _scrollV), min);
            if ((c.SizeFlagsHorizontal & SizeFlags.Expand) != 0) r.Size = new Vector2(Math.Max(view.X, min.X), r.Size.Y);
            if ((c.SizeFlagsVertical & SizeFlags.Expand) != 0) r.Size = new Vector2(r.Size.X, Math.Max(view.Y, min.Y));
            r.Position += ofs;
            FitChildInRect(c, r);
        }
    }

    /// <summary>Scrolls so that <paramref name="control"/> is inside the view (Godot's ensure_control_visible).</summary>
    public void EnsureControlVisible(Control control)
    {
        if (!IsAncestorOf(control)) return;
        Vector2 view = ViewSize();
        Vector2 topLeft = control.GlobalPosition - GlobalPosition + new Vector2(_scrollH, _scrollV) - GetThemeStylebox("panel").GetOffset();
        Vector2 size = control.Size;
        if (topLeft.X < _scrollH) ScrollHorizontal = (int)topLeft.X;
        else if (topLeft.X + size.X > _scrollH + view.X) ScrollHorizontal = (int)(topLeft.X + size.X - view.X);
        if (topLeft.Y < _scrollV) ScrollVertical = (int)topLeft.Y;
        else if (topLeft.Y + size.Y > _scrollV + view.Y) ScrollVertical = (int)(topLeft.Y + size.Y - view.Y);
    }

    // ---- the scroll bars (G5 U2): drawn and dragged here; HScrollBar / VScrollBar objects mirror them for scripts ----

    private readonly HScrollBar _hBarNode = new();
    private readonly VScrollBar _vBarNode = new();
    private int _barDrag;   // 1 vertical, 2 horizontal
    private float _barGrab;
    private bool _barsHooked;

    private void HookBars()
    {
        if (_barsHooked) return;
        _barsHooked = true;
        _vBarNode.ValueChanged += v => { if (!_syncing && (int)v != _scrollV) ScrollVertical = (int)v; };
        _hBarNode.ValueChanged += v => { if (!_syncing && (int)v != _scrollH) ScrollHorizontal = (int)v; };
    }

    private bool _syncing;

    public HScrollBar GetHScrollBar() { HookBars(); SyncBars(); return _hBarNode; }

    public VScrollBar GetVScrollBar() { HookBars(); SyncBars(); return _vBarNode; }

    private void SyncBars()
    {
        Vector2 view = ViewSize();
        _syncing = true;
        _vBarNode.MaxValue = Math.Max(_childMin.Y, 1);
        _vBarNode.Page = view.Y;
        _vBarNode.SetValueNoSignal(_scrollV);
        _hBarNode.MaxValue = Math.Max(_childMin.X, 1);
        _hBarNode.Page = view.X;
        _hBarNode.SetValueNoSignal(_scrollH);
        _syncing = false;
    }

    // The bar's track and grabber rectangles, in the container's space.
    private (Rect2 Track, Rect2 Grabber) BarRects(bool vertical)
    {
        StyleBox style = GetThemeStylebox("panel");
        Vector2 view = ViewSize();
        Vector2 ofs = style.GetOffset();
        float content = vertical ? _childMin.Y : _childMin.X, room = vertical ? view.Y : view.X, pos = vertical ? _scrollV : _scrollH;
        Rect2 track = vertical ? new Rect2(ofs.X + view.X, ofs.Y, BarSize, view.Y) : new Rect2(ofs.X, ofs.Y + view.Y, view.X, BarSize);
        float length = content <= 0 ? room : Math.Min(room, Math.Max(10f, room * room / Math.Max(content, room)));
        float travel = room - length, scrollable = Math.Max(1f, content - room);
        float start = travel * Math.Clamp(pos / scrollable, 0f, 1f);
        Rect2 grabber = vertical ? new Rect2(track.Position.X, track.Position.Y + start, track.Size.X, length) : new Rect2(track.Position.X + start, track.Position.Y, length, track.Size.Y);
        return (track, grabber);
    }

    public override void _GuiInput(InputEvent @event)
    {
        if (@event is InputEventMouseButton { ButtonIndex: MouseButton.Left } press)
        {
            if (!press.Pressed) { _barDrag = 0; return; }
            if (_vBar && BarRects(true).Track.HasPoint(press.Position)) { (Rect2 t, Rect2 g) = BarRects(true); if (g.HasPoint(press.Position)) { _barDrag = 1; _barGrab = press.Position.Y - g.Position.Y; } else ScrollVertical = _scrollV + (press.Position.Y < g.Position.Y ? -1 : 1) * (int)ViewSize().Y; AcceptEvent(); return; }
            if (_hBar && BarRects(false).Track.HasPoint(press.Position)) { (Rect2 t, Rect2 g) = BarRects(false); if (g.HasPoint(press.Position)) { _barDrag = 2; _barGrab = press.Position.X - g.Position.X; } else ScrollHorizontal = _scrollH + (press.Position.X < g.Position.X ? -1 : 1) * (int)ViewSize().X; AcceptEvent(); return; }
        }
        if (@event is InputEventMouseMotion motion && _barDrag != 0)
        {
            bool vertical = _barDrag == 1;
            (Rect2 track, Rect2 grab) = BarRects(vertical);
            float travel = (vertical ? track.Size.Y - grab.Size.Y : track.Size.X - grab.Size.X);
            float content = vertical ? _childMin.Y : _childMin.X, room = vertical ? ViewSize().Y : ViewSize().X;
            float p = (vertical ? motion.Position.Y - track.Position.Y : motion.Position.X - track.Position.X) - _barGrab;
            int v = travel <= 0 ? 0 : (int)(Math.Clamp(p / travel, 0f, 1f) * Math.Max(0f, content - room));
            if (vertical) ScrollVertical = v; else ScrollHorizontal = v;
            AcceptEvent();
            return;
        }
        if (@event is not InputEventMouseButton { Pressed: true } b) return;
        Vector2 view = ViewSize();
        float stepV = view.Y / 8f * b.Factor, stepH = view.X / 8f * b.Factor;
        switch (b.ButtonIndex)
        {
            case MouseButton.WheelUp when _vBar: ScrollVertical = _scrollV - (int)stepV; AcceptEvent(); break;
            case MouseButton.WheelDown when _vBar: ScrollVertical = _scrollV + (int)stepV; AcceptEvent(); break;
            case MouseButton.WheelLeft when _hBar: ScrollHorizontal = _scrollH - (int)stepH; AcceptEvent(); break;
            case MouseButton.WheelRight when _hBar: ScrollHorizontal = _scrollH + (int)stepH; AcceptEvent(); break;
        }
    }

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("panel"), new Rect2(Vector2.Zero, Size));
        foreach (bool vertical in new[] { true, false })
        {
            if (vertical ? !_vBar : !_hBar) continue;
            (Rect2 track, Rect2 grab) = BarRects(vertical);
            string type = vertical ? "VScrollBar" : "HScrollBar";
            DrawStyleBox(GetThemeStylebox("scroll", type), track);
            DrawStyleBox(GetThemeStylebox(_barDrag == (vertical ? 1 : 2) ? "grabber_pressed" : "grabber", type), grab);
        }
    }
}
