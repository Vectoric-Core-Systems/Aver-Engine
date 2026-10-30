// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// ItemList (G5 U2): a list of selectable items (text and icon), in one or more columns, with single / multi / toggle selection, keyboard movement,
// double click or accept to activate, and item_selected / multi_selected / item_clicked / item_activated / empty_clicked. The scroll bar is not drawn:
// get_v_scroll_bar() returns a detached bar whose value is the scroll position.

namespace Aver.Fabric;

/// <summary>A list of items to pick from (Godot's ItemList).</summary>
public class ItemList : Control
{
    public enum IconModeEnum { Top = 0, Left = 1 }

    public enum SelectModeEnum { Single = 0, Multi = 1, Toggle = 2 }

    public new class SignalName : Control.SignalName
    {
        public static readonly StringName ItemSelected = "item_selected";
        public static readonly StringName EmptyClicked = "empty_clicked";
        public static readonly StringName ItemClicked = "item_clicked";
        public static readonly StringName MultiSelected = "multi_selected";
        public static readonly StringName ItemActivated = "item_activated";
    }

    public event Action<long> ItemSelected { add => Connect(SignalName.ItemSelected, Callable.From(value)); remove => Disconnect(SignalName.ItemSelected, Callable.From(value)); }
    public event Action<Vector2, long> EmptyClicked { add => Connect(SignalName.EmptyClicked, Callable.From(value)); remove => Disconnect(SignalName.EmptyClicked, Callable.From(value)); }
    public event Action<long, Vector2, long> ItemClicked { add => Connect(SignalName.ItemClicked, Callable.From(value)); remove => Disconnect(SignalName.ItemClicked, Callable.From(value)); }
    public event Action<long, bool> MultiSelected { add => Connect(SignalName.MultiSelected, Callable.From(value)); remove => Disconnect(SignalName.MultiSelected, Callable.From(value)); }
    public event Action<long> ItemActivated { add => Connect(SignalName.ItemActivated, Callable.From(value)); remove => Disconnect(SignalName.ItemActivated, Callable.From(value)); }

    private sealed class Item
    {
        public string Text = "";
        public Texture2D? Icon;
        public bool Selectable = true, Selected, Disabled;
        public Variant Metadata;
        public string Tooltip = "";
        public Color? Fg, Bg;
    }

    private readonly List<Item> _items = new();
    private int _current = -1, _hover = -1, _shiftAnchor = -1;
    private readonly VScrollBar _scroll = new();
    private int _maxColumns = 1;

    public SelectModeEnum SelectMode { get; set; } = SelectModeEnum.Single;
    public bool AllowReselect { get; set; }
    public bool AllowRmbSelect { get; set; }
    public bool AllowSearch { get; set; } = true;
    private bool _autoHeight, _autoWidth;
    public bool AutoHeight { get => _autoHeight; set { _autoHeight = value; UpdateMinimumSize(); } }
    public bool AutoWidth { get => _autoWidth; set { _autoWidth = value; UpdateMinimumSize(); } }
    public int MaxTextLines { get; set; } = 1;
    public TextServer.OverrunBehavior TextOverrunBehavior { get; set; } = TextServer.OverrunBehavior.TrimEllipsis;
    public bool SameColumnWidth { get; set; }
    public int FixedColumnWidth { get; set; }
    public IconModeEnum IconMode { get; set; } = IconModeEnum.Left;
    public float IconScale { get; set; } = 1f;
    public Vector2I FixedIconSize { get; set; }
    public bool WraptextOnLines { get; set; }
    public int MaxColumns { get => _maxColumns; set { _maxColumns = Math.Max(0, value); QueueRedraw(); } }
    public int ItemCount { get => _items.Count; set { while (_items.Count < value) _items.Add(new Item()); if (_items.Count > value) _items.RemoveRange(value, _items.Count - value); QueueRedraw(); } }
    public bool WraparoundItems { get; set; } = true;
    public int ScrollHintMode { get; set; }
    public bool TileScrollHint { get; set; }

    public ItemList() { FocusMode = FocusModeEnum.All; }

    private Item At(int i) => i >= 0 && i < _items.Count ? _items[i] : throw new ArgumentOutOfRangeException(nameof(i), $"Index {i} is out of range (count {_items.Count}).");

    public int AddItem(string text, Texture2D? icon = null, bool selectable = true)
    {
        _items.Add(new Item { Text = text, Icon = icon, Selectable = selectable });
        QueueRedraw();
        UpdateMinimumSize();
        return _items.Count - 1;
    }

    public int AddIconItem(Texture2D icon, bool selectable = true) => AddItem("", icon, selectable);

    public void SetItemText(int idx, string text) { At(idx).Text = text; QueueRedraw(); }
    public string GetItemText(int idx) => At(idx).Text;
    public void SetItemIcon(int idx, Texture2D? icon) { At(idx).Icon = icon; QueueRedraw(); }
    public Texture2D? GetItemIcon(int idx) => At(idx).Icon;
    public void SetItemDisabled(int idx, bool disabled) { At(idx).Disabled = disabled; QueueRedraw(); }
    public bool IsItemDisabled(int idx) => At(idx).Disabled;
    public void SetItemSelectable(int idx, bool selectable) { At(idx).Selectable = selectable; }
    public bool IsItemSelectable(int idx) => At(idx).Selectable;
    public void SetItemMetadata(int idx, Variant metadata) { At(idx).Metadata = metadata; }
    public Variant GetItemMetadata(int idx) => At(idx).Metadata;
    public void SetItemTooltip(int idx, string tooltip) { At(idx).Tooltip = tooltip; }
    public string GetItemTooltip(int idx) => At(idx).Tooltip;
    public void SetItemCustomFgColor(int idx, Color color) { At(idx).Fg = color; QueueRedraw(); }
    public Color GetItemCustomFgColor(int idx) => At(idx).Fg ?? new Color(0, 0, 0, 0);
    public void SetItemCustomBgColor(int idx, Color color) { At(idx).Bg = color; QueueRedraw(); }
    public Color GetItemCustomBgColor(int idx) => At(idx).Bg ?? new Color(0, 0, 0, 0);
    public bool IsSelected(int idx) => idx >= 0 && idx < _items.Count && _items[idx].Selected;
    public int GetItemCount() => _items.Count;

    public void RemoveItem(int idx)
    {
        At(idx);
        _items.RemoveAt(idx);
        if (_current >= _items.Count) _current = _items.Count - 1;
        QueueRedraw();
        UpdateMinimumSize();
    }

    public void Clear()
    {
        _items.Clear();
        _current = -1;
        _scroll.Value = 0;
        QueueRedraw();
        UpdateMinimumSize();
    }

    public void MoveItem(int fromIdx, int toIdx)
    {
        Item i = At(fromIdx);
        _items.RemoveAt(fromIdx);
        _items.Insert(Math.Clamp(toIdx, 0, _items.Count), i);
        QueueRedraw();
    }

    public void SortItemsByText()
    {
        Item? cur = _current >= 0 ? _items[_current] : null;
        _items.Sort((a, b) => string.CompareOrdinal(a.Text, b.Text));
        _current = cur == null ? -1 : _items.IndexOf(cur);
        QueueRedraw();
    }

    /// <summary>Selects an item. With <paramref name="single"/> (or in single mode) the others are deselected.</summary>
    public void Select(int idx, bool single = true)
    {
        if (idx < 0 || idx >= _items.Count || !_items[idx].Selectable || _items[idx].Disabled) return;
        if (single || SelectMode == SelectModeEnum.Single)
            foreach (Item i in _items) i.Selected = false;
        _items[idx].Selected = SelectMode == SelectModeEnum.Toggle && !single ? !_items[idx].Selected : true;
        _current = idx;
        QueueRedraw();
    }

    public void Deselect(int idx) { if (idx >= 0 && idx < _items.Count) { _items[idx].Selected = false; QueueRedraw(); } }

    public void DeselectAll() { foreach (Item i in _items) i.Selected = false; QueueRedraw(); }

    public int[] GetSelectedItems()
    {
        var list = new List<int>();
        for (int i = 0; i < _items.Count; i++) if (_items[i].Selected) list.Add(i);
        return list.ToArray();
    }

    public int GetCurrent() => _current;

    public VScrollBar GetVScrollBar() => _scroll;

    public void EnsureCurrentIsVisible()
    {
        if (_current < 0) return;
        Rect2 r = GetItemRect(_current, false);
        Rect2 c = Content();
        if (r.Position.Y < c.Position.Y) _scroll.Value -= c.Position.Y - r.Position.Y;
        else if (r.End.Y > c.End.Y) _scroll.Value += r.End.Y - c.End.Y;
        QueueRedraw();
    }

    // ---- layout ----

    private Rect2 Content() => GetThemeStylebox("panel").GetContentRect(LocalRect);

    private int Columns() => Math.Max(1, _maxColumns == 0 ? Math.Max(1, (int)(Content().Size.X / Math.Max(1f, ColumnWidth()))) : _maxColumns);

    private float ColumnWidth()
    {
        Rect2 c = Content();
        if (FixedColumnWidth > 0) return FixedColumnWidth;
        int cols = _maxColumns == 0 ? 1 : Math.Max(1, _maxColumns);
        return c.Size.X / cols;
    }

    private float RowHeight()
    {
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float h = UiText.LineHeight(font, px) * Math.Max(1, MaxTextLines);
        foreach (Item i in _items)
            if (i.Icon != null) h = MathF.Max(h, (FixedIconSize.Y > 0 ? FixedIconSize.Y : i.Icon.GetHeight()) * IconScale);
        return h + GetThemeConstant("v_separation");
    }

    private int RowsTotal() => (_items.Count + Columns() - 1) / Columns();

    private float ContentHeight() => RowsTotal() * RowHeight();

    /// <summary>The rectangle of an item in the control's space (scrolled when <paramref name="expand"/> is false).</summary>
    public Rect2 GetItemRect(int idx, bool expand = true)
    {
        At(idx);
        int cols = Columns();
        Rect2 c = Content();
        float w = cols == 1 ? c.Size.X : ColumnWidth(), h = RowHeight();
        int row = idx / cols, col = idx % cols;
        return new Rect2(c.Position.X + col * w, c.Position.Y + row * h - (float)_scroll.Value, w, h);
    }

    /// <summary>The item under a point, or -1.</summary>
    public int GetItemAtPosition(Vector2 position, bool exact = false)
    {
        for (int i = 0; i < _items.Count; i++)
            if (GetItemRect(i, false).HasPoint(position)) return i;
        if (exact || _items.Count == 0) return -1;
        return -1;
    }

    private void SyncScroll()
    {
        double room = Math.Max(0, ContentHeight() - Content().Size.Y);
        _scroll.MaxValue = Math.Max(room + Content().Size.Y, 1);
        _scroll.Page = Content().Size.Y;
        if (_scroll.Value > room) _scroll.Value = room;
    }

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 style = GetThemeStylebox("panel").GetMinimumSize();
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float w = 0f, h = 0f;
        if (_autoWidth) foreach (Item i in _items) w = MathF.Max(w, UiText.Width(font, px, i.Text) + (i.Icon?.GetWidth() ?? 0));
        if (_autoHeight) h = ContentHeight();
        return new Vector2(w, h) + style;
    }

    // ---- input ----

    private void Clicked(int idx, MouseButtonMask _, MouseButton button, Vector2 pos, bool ctrl, bool shift)
    {
        if (idx < 0)
        {
            EmitSignal(SignalName.EmptyClicked, pos, (long)button);
            return;
        }
        Item it = _items[idx];
        if (it.Disabled) return;
        EmitSignal(SignalName.ItemClicked, idx, pos, (long)button);
        if (!it.Selectable || (button == MouseButton.Right && !AllowRmbSelect) || (button != MouseButton.Left && button != MouseButton.Right)) return;
        if (SelectMode == SelectModeEnum.Single)
        {
            bool already = it.Selected;
            if (!already || AllowReselect)
            {
                Select(idx);
                EmitSignal(SignalName.ItemSelected, idx);
            }
            _current = idx;
        }
        else if (SelectMode == SelectModeEnum.Multi && shift && _shiftAnchor >= 0)
        {
            int a = Math.Min(_shiftAnchor, idx), b = Math.Max(_shiftAnchor, idx);
            for (int i = a; i <= b; i++)
                if (_items[i].Selectable && !_items[i].Disabled && !_items[i].Selected) { _items[i].Selected = true; EmitSignal(SignalName.MultiSelected, i, true); }
            _current = idx;
        }
        else
        {
            bool toggle = SelectMode == SelectModeEnum.Toggle || ctrl;
            if (toggle)
            {
                it.Selected = !it.Selected;
                EmitSignal(SignalName.MultiSelected, idx, it.Selected);
            }
            else
            {
                foreach (Item i in _items) i.Selected = false;
                it.Selected = true;
                EmitSignal(SignalName.MultiSelected, idx, true);
            }
            _current = idx;
            _shiftAnchor = idx;
        }
        QueueRedraw();
    }

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseButton mb when mb.Pressed && mb.ButtonIndex is MouseButton.Left or MouseButton.Right or MouseButton.Middle:
            {
                GrabFocus();
                int at = GetItemAtPosition(mb.Position);
                Clicked(at, default, mb.ButtonIndex, mb.Position, mb.CtrlPressed || mb.MetaPressed, mb.ShiftPressed);
                if (mb.DoubleClick && at >= 0 && _items[at].Selectable && !_items[at].Disabled && mb.ButtonIndex == MouseButton.Left) EmitSignal(SignalName.ItemActivated, at);
                AcceptEvent();
                break;
            }
            case InputEventMouseButton wheel when wheel.Pressed && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown:
                SyncScroll();
                _scroll.Value += (wheel.ButtonIndex == MouseButton.WheelUp ? -1 : 1) * RowHeight() * 2;
                QueueRedraw();
                AcceptEvent();
                break;
            case InputEventMouseMotion mm:
            {
                int over = GetItemAtPosition(mm.Position);
                if (over != _hover) { _hover = over; QueueRedraw(); }
                break;
            }
            case InputEventKey { Pressed: true } k:
                OnKey(k);
                break;
        }
    }

    private void Move(int dir)
    {
        if (_items.Count == 0) return;
        int i = _current < 0 ? (dir > 0 ? -1 : _items.Count) : _current;
        for (int n = 0; n < _items.Count; n++)
        {
            i += dir;
            if (i < 0 || i >= _items.Count) return;
            if (_items[i].Selectable && !_items[i].Disabled) break;
        }
        if (i < 0 || i >= _items.Count) return;
        if (SelectMode == SelectModeEnum.Single) { Select(i); EmitSignal(SignalName.ItemSelected, i); }
        else _current = i;
        SyncScroll();
        EnsureCurrentIsVisible();
        QueueRedraw();
    }

    private void OnKey(InputEventKey k)
    {
        int cols = Columns();
        if (Gui.IsUiAction(k, "ui_down")) { Move(cols); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_up")) { Move(-cols); AcceptEvent(); }
        else if (cols > 1 && Gui.IsUiAction(k, "ui_right")) { Move(1); AcceptEvent(); }
        else if (cols > 1 && Gui.IsUiAction(k, "ui_left")) { Move(-1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_home")) { _current = -1; Move(1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_end")) { _current = _items.Count; Move(-1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_accept") && _current >= 0) { EmitSignal(SignalName.ItemActivated, _current); AcceptEvent(); }
        else if (SelectMode != SelectModeEnum.Single && Gui.IsUiAction(k, "ui_select") && _current >= 0)
        {
            _items[_current].Selected = !_items[_current].Selected;
            EmitSignal(SignalName.MultiSelected, _current, _items[_current].Selected);
            QueueRedraw();
            AcceptEvent();
        }
        else if (AllowSearch && k.Unicode > 32 && !k.CtrlPressed)
        {
            string c = char.ToLowerInvariant((char)k.Unicode).ToString();
            for (int n = 1; n <= _items.Count; n++)
            {
                int idx = ((_current < 0 ? -1 : _current) + n) % _items.Count;
                if (_items[idx].Selectable && _items[idx].Text.ToLowerInvariant().StartsWith(c, StringComparison.Ordinal))
                {
                    if (SelectMode == SelectModeEnum.Single) { Select(idx); EmitSignal(SignalName.ItemSelected, idx); }
                    else _current = idx;
                    EnsureCurrentIsVisible();
                    AcceptEvent();
                    break;
                }
            }
        }
    }

    public override string _GetTooltip(Vector2 atPosition)
    {
        int i = GetItemAtPosition(atPosition);
        return i >= 0 && _items[i].Tooltip.Length > 0 ? _items[i].Tooltip : TooltipText;
    }

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name.StartsWith("item_", StringComparison.Ordinal) && name.IndexOf('/') is var slash and > 5 && int.TryParse(name.AsSpan(5, slash - 5), out int index))
        {
            while (_items.Count <= index) _items.Add(new Item());
            Item item = _items[index];
            switch (name[(slash + 1)..])
            {
                case "text": item.Text = value.AsString(); break;
                case "icon": item.Icon = value.AsGodotObject() as Texture2D; break;
                case "selectable": item.Selectable = value.AsBool(); break;
                case "disabled": item.Disabled = value.AsBool(); break;
                default: return false;
            }
            QueueRedraw();
            return true;
        }
        return base.TryDynamicSet(name, value);
    }

    // ---- drawing ----

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("panel"), LocalRect);
        if (HasFocus()) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        SyncScroll();
        Rect2 c = Content();
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float lh = UiText.LineHeight(font, px);
        int hsep = GetThemeConstant("h_separation"), imargin = GetThemeConstant("icon_margin");
        CanvasItem.TextFx fx = ThemeTextFx();
        PushClip(c);
        for (int i = 0; i < _items.Count; i++)
        {
            Rect2 r = GetItemRect(i, false);
            if (r.End.Y < c.Position.Y || r.Position.Y > c.End.Y) continue;
            Item it = _items[i];
            if (it.Bg is { } bg) DrawRect(r, bg);
            if (it.Selected) DrawStyleBox(GetThemeStylebox(HasFocus() ? "selected_focus" : "selected"), r);
            else if (i == _hover && !it.Disabled) DrawStyleBox(GetThemeStylebox("hovered"), r);
            if (i == _current && HasFocus()) DrawStyleBox(GetThemeStylebox("cursor"), r);
            Color color = it.Disabled ? GetThemeColor("font_color").Darkened(0.5f) : it.Fg ?? (it.Selected ? GetThemeColor("font_selected_color") : i == _hover ? GetThemeColor("font_hovered_color") : GetThemeColor("font_color"));
            float x = r.Position.X + hsep;
            if (it.Icon != null)
            {
                Vector2 sz = FixedIconSize.X > 0 ? new Vector2(FixedIconSize.X, FixedIconSize.Y) : it.Icon.GetSize();
                sz *= IconScale;
                DrawTextureRect(it.Icon, new Rect2(x, r.Position.Y + (r.Size.Y - sz.Y) / 2f, sz.X, sz.Y), false, it.Disabled ? new Color(1, 1, 1, 0.5f) : null);
                x += sz.X + imargin;
            }
            string text = it.Text;
            if (text.Length > 0 && TextOverrunBehavior != TextServer.OverrunBehavior.NoTrimming)
                text = TextLines.Trim(text, font, px, MathF.Max(0f, r.End.X - x - hsep), TextOverrunBehavior is TextServer.OverrunBehavior.TrimWord or TextServer.OverrunBehavior.TrimWordEllipsis, TextOverrunBehavior >= TextServer.OverrunBehavior.TrimEllipsis ? "…" : "");
            DrawTextLine(new Vector2(x, r.Position.Y + (r.Size.Y - lh) / 2f), text, font, px, color, fx);
        }
        PopClip();
    }
}
