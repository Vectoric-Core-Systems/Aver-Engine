// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tree and TreeItem (G5 U2): columns with titles, items with text / check / range / icon cells, folding, single / row / multi selection, keyboard movement,
// and Godot's signals (item_selected, cell_selected, multi_selected, item_edited, item_collapsed, item_activated, item_mouse_selected, empty_clicked,
// column_title_clicked, button_clicked). Text cells are not edited in place (edit_selected reports false); check cells toggle on a click.

namespace Aver.Fabric;

/// <summary>One item of a <see cref="Tree"/> (Godot's TreeItem).</summary>
public class TreeItem : GodotObject
{
    public enum TreeCellMode { String = 0, Check = 1, Range = 2, Icon = 3, Custom = 4 }

    internal sealed class Cell
    {
        public TreeCellMode Mode;
        public string Text = "", Suffix = "", Tooltip = "";
        public Texture2D? Icon;
        public bool Checked, Indeterminate, Editable, Selectable = true, Selected;
        public Variant Metadata;
        public Color? Custom, Bg;
        public HorizontalAlignment Align = HorizontalAlignment.Left;
        public double Value, Min, Max = 100, Step = 1;
        public bool Expression;
        public readonly List<(Texture2D Icon, int Id, bool Disabled, string Tooltip)> Buttons = new();
    }

    internal Tree? Owner;
    internal TreeItem? Parent;
    internal readonly List<TreeItem> Kids = new();
    internal Cell[] Cells;
    private bool _collapsed, _visible = true;

    internal TreeItem(Tree owner, int columns)
    {
        Owner = owner;
        Cells = new Cell[columns];
        for (int i = 0; i < columns; i++) Cells[i] = new Cell();
    }

    internal void Resize(int columns)
    {
        var next = new Cell[columns];
        for (int i = 0; i < columns; i++) next[i] = i < Cells.Length ? Cells[i] : new Cell();
        Cells = next;
        foreach (TreeItem k in Kids) k.Resize(columns);
    }

    private Cell C(int column) => column >= 0 && column < Cells.Length ? Cells[column] : throw new ArgumentOutOfRangeException(nameof(column), $"Column {column} is out of range (columns {Cells.Length}).");

    private void Dirty() => Owner?.ItemChanged();

    public bool Collapsed
    {
        get => _collapsed;
        set
        {
            if (_collapsed == value || DisableFolding) return;
            _collapsed = value;
            Dirty();
            Owner?.EmitSignal(Tree.SignalName.ItemCollapsed, this);
        }
    }

    public bool DisableFolding { get; set; }
    public bool IsCollapsed() => _collapsed;
    public void SetCollapsed(bool enable) => Collapsed = enable;
    public bool IsFoldingDisabled() => DisableFolding;
    public void SetCollapsedRecursive(bool enable) { Collapsed = enable; foreach (TreeItem k in Kids) k.SetCollapsedRecursive(enable); }
    public bool IsAnyCollapsed(bool onlyVisible = false) => Kids.Any(k => k._collapsed && k.Kids.Count > 0) || Kids.Any(k => k.IsAnyCollapsed(onlyVisible));

    public bool Visible { get => _visible; set { _visible = value; Dirty(); } }
    public bool IsVisible() => _visible;
    public void SetVisible(bool visible) => Visible = visible;
    public bool IsVisibleInTree() { for (TreeItem? i = this; i != null; i = i.Parent) if (!i._visible) return false; return true; }

    public int CustomMinimumHeight { get; set; }

    // ---- structure ----

    public TreeItem? GetParent() => Parent;
    public int GetChildCount() => Kids.Count;
    public TreeItem? GetChild(int index) => index >= 0 && index < Kids.Count ? Kids[index] : index < 0 && -index <= Kids.Count ? Kids[^(-index)] : null;
    public TreeItem? GetFirstChild() => Kids.Count > 0 ? Kids[0] : null;

    public Collections.Array<TreeItem> GetChildren()
    {
        var a = new Collections.Array<TreeItem>();
        foreach (TreeItem k in Kids) a.Add(k);
        return a;
    }

    public int GetIndex() => Parent?.Kids.IndexOf(this) ?? 0;

    public TreeItem? GetNext() => Parent != null && GetIndex() + 1 < Parent.Kids.Count ? Parent.Kids[GetIndex() + 1] : null;

    public TreeItem? GetPrev() => Parent != null && GetIndex() > 0 ? Parent.Kids[GetIndex() - 1] : null;

    public TreeItem? GetNextInTree(bool wrap = false)
    {
        if (Kids.Count > 0) return Kids[0];
        for (TreeItem? i = this; i != null; i = i.Parent)
            if (i.GetNext() is { } n) return n;
        return wrap ? Owner?.GetRoot() : null;
    }

    public TreeItem? GetPrevInTree(bool wrap = false)
    {
        if (GetPrev() is { } p)
        {
            TreeItem last = p;
            while (last.Kids.Count > 0) last = last.Kids[^1];
            return last;
        }
        if (Parent != null) return Parent;
        if (!wrap || Owner?.GetRoot() == null) return null;
        TreeItem end = Owner.GetRoot()!;
        while (end.Kids.Count > 0) end = end.Kids[^1];
        return end;
    }

    public TreeItem? GetNextVisible(bool wrap = false)
    {
        TreeItem? n = this;
        do
        {
            n = n!.Collapsed || n.Kids.Count == 0 ? NextSkipChildren(n) : n.Kids[0];
        } while (n != null && !n.IsVisibleInTree());
        return n ?? (wrap ? Owner?.FirstVisible() : null);
    }

    private static TreeItem? NextSkipChildren(TreeItem i)
    {
        for (TreeItem? x = i; x != null; x = x.Parent)
            if (x.GetNext() is { } n) return n;
        return null;
    }

    public TreeItem? GetPrevVisible(bool wrap = false)
    {
        List<TreeItem> rows = Owner?.VisibleItems() ?? new List<TreeItem>();
        int at = rows.IndexOf(this);
        if (at > 0) return rows[at - 1];
        return wrap && rows.Count > 0 ? rows[^1] : null;
    }

    public TreeItem? CreateChild(int index = -1) => Owner?.CreateItem(this, index);

    public void AddChild(TreeItem child)
    {
        child.Parent?.Kids.Remove(child);
        child.Parent = this;
        Kids.Add(child);
        Dirty();
    }

    public void RemoveChild(TreeItem child)
    {
        if (Kids.Remove(child)) { child.Parent = null; Dirty(); }
    }

    public void MoveBefore(TreeItem item)
    {
        if (Parent == null || item.Parent == null) return;
        Parent.Kids.Remove(this);
        Parent = item.Parent;
        Parent.Kids.Insert(Parent.Kids.IndexOf(item), this);
        Dirty();
    }

    public void MoveAfter(TreeItem item)
    {
        if (Parent == null || item.Parent == null) return;
        Parent.Kids.Remove(this);
        Parent = item.Parent;
        Parent.Kids.Insert(Parent.Kids.IndexOf(item) + 1, this);
        Dirty();
    }

    public override void Free()
    {
        if (Parent != null) { Parent.Kids.Remove(this); Parent = null; }
        else if (Owner != null && Owner.GetRoot() == this) Owner.ClearRoot();
        foreach (TreeItem k in Kids.ToArray()) k.Free();
        Owner?.ItemChanged();
        Owner = null;
        base.Free();
    }

    public Tree? GetTree() => Owner;

    // ---- cells ----

    public void SetCellMode(int column, TreeCellMode mode) { C(column).Mode = mode; Dirty(); }
    public TreeCellMode GetCellMode(int column) => C(column).Mode;
    public void SetText(int column, string text) { C(column).Text = text; Dirty(); }
    public string GetText(int column) => C(column).Text;
    public void SetTextAlignment(int column, HorizontalAlignment alignment) { C(column).Align = alignment; Dirty(); }
    public HorizontalAlignment GetTextAlignment(int column) => C(column).Align;
    public void SetSuffix(int column, string text) { C(column).Suffix = text; Dirty(); }
    public string GetSuffix(int column) => C(column).Suffix;
    public void SetIcon(int column, Texture2D? texture) { C(column).Icon = texture; Dirty(); }
    public Texture2D? GetIcon(int column) => C(column).Icon;
    public void SetChecked(int column, bool @checked) { C(column).Checked = @checked; C(column).Indeterminate = false; Dirty(); }
    public bool IsChecked(int column) => C(column).Checked;
    public void SetIndeterminate(int column, bool indeterminate) { C(column).Indeterminate = indeterminate; Dirty(); }
    public bool IsIndeterminate(int column) => C(column).Indeterminate;
    public void SetEditable(int column, bool enabled) { C(column).Editable = enabled; }
    public bool IsEditable(int column) => C(column).Editable;
    public void SetSelectable(int column, bool selectable) { C(column).Selectable = selectable; }
    public bool IsSelectable(int column) => C(column).Selectable;
    public void SetMetadata(int column, Variant meta) { C(column).Metadata = meta; }
    public Variant GetMetadata(int column) => C(column).Metadata;
    public void SetTooltipText(int column, string tooltip) { C(column).Tooltip = tooltip; }
    public string GetTooltipText(int column) => C(column).Tooltip;
    public void SetCustomColor(int column, Color color) { C(column).Custom = color; Dirty(); }
    public Color GetCustomColor(int column) => C(column).Custom ?? new Color(0, 0, 0, 1);
    public void ClearCustomColor(int column) { C(column).Custom = null; Dirty(); }
    public void SetCustomBgColor(int column, Color color, bool justOutline = false) { C(column).Bg = color; Dirty(); }
    public Color GetCustomBgColor(int column) => C(column).Bg ?? new Color(0, 0, 0, 1);
    public void ClearCustomBgColor(int column) { C(column).Bg = null; Dirty(); }

    public void SetRange(int column, double value)
    {
        Cell c = C(column);
        c.Value = Math.Clamp(c.Step > 0 ? Math.Round(value / c.Step) * c.Step : value, c.Min, c.Max);
        Dirty();
    }

    public double GetRange(int column) => C(column).Value;
    public void SetRangeConfig(int column, double min, double max, double step, bool expr = false) { Cell c = C(column); c.Min = min; c.Max = max; c.Step = step; c.Expression = expr; SetRange(column, c.Value); }
    public Collections.Dictionary GetRangeConfig(int column) { Cell c = C(column); return new Collections.Dictionary { ["min"] = c.Min, ["max"] = c.Max, ["step"] = c.Step, ["expr"] = c.Expression }; }

    public void AddButton(int column, Texture2D button, int id = -1, bool disabled = false, string tooltipText = "")
    {
        Cell c = C(column);
        c.Buttons.Add((button, id < 0 ? c.Buttons.Count : id, disabled, tooltipText));
        Dirty();
    }

    public int GetButtonCount(int column) => C(column).Buttons.Count;
    public int GetButtonId(int column, int index) => C(column).Buttons[index].Id;
    public int GetButtonById(int column, int id) => C(column).Buttons.FindIndex(b => b.Id == id);
    public Texture2D GetButton(int column, int index) => C(column).Buttons[index].Icon;
    public bool IsButtonDisabled(int column, int index) => C(column).Buttons[index].Disabled;
    public void EraseButton(int column, int index) { C(column).Buttons.RemoveAt(index); Dirty(); }

    public void Select(int column) => Owner?.SelectCell(this, column, false, false);
    public void Deselect(int column) { C(column).Selected = false; Dirty(); }
    public bool IsSelected(int column) => C(column).Selected;
    public void UncollapseTree() { for (TreeItem? i = this; i != null; i = i.Parent) i._collapsed = false; Dirty(); }
    public void CallRecursive(StringName method, params Variant[] args) { Call(method, args); foreach (TreeItem k in Kids) k.CallRecursive(method, args); }
}

/// <summary>A tree or table of items (Godot's Tree).</summary>
public class Tree : Control
{
    public enum SelectModeEnum { Single = 0, Row = 1, Multi = 2 }

    public enum DropModeFlagsEnum { Disabled = 0, OnItem = 1, Inbetween = 2 }

    public new class SignalName : Control.SignalName
    {
        public static readonly StringName ItemSelected = "item_selected";
        public static readonly StringName CellSelected = "cell_selected";
        public static readonly StringName MultiSelected = "multi_selected";
        public static readonly StringName ItemMouseSelected = "item_mouse_selected";
        public static readonly StringName EmptyClicked = "empty_clicked";
        public static readonly StringName ItemEdited = "item_edited";
        public static readonly StringName ItemCollapsed = "item_collapsed";
        public static readonly StringName ItemActivated = "item_activated";
        public static readonly StringName ColumnTitleClicked = "column_title_clicked";
        public static readonly StringName ButtonClicked = "button_clicked";
        public static readonly StringName CheckPropagatedToItem = "check_propagated_to_item";
    }

    public event Action ItemSelected { add => Connect(SignalName.ItemSelected, Callable.From(value)); remove => Disconnect(SignalName.ItemSelected, Callable.From(value)); }
    public event Action CellSelected { add => Connect(SignalName.CellSelected, Callable.From(value)); remove => Disconnect(SignalName.CellSelected, Callable.From(value)); }
    public event Action<TreeItem, long, bool> MultiSelected { add => Connect(SignalName.MultiSelected, Callable.From(value)); remove => Disconnect(SignalName.MultiSelected, Callable.From(value)); }
    public event Action<Vector2, long> ItemMouseSelected { add => Connect(SignalName.ItemMouseSelected, Callable.From(value)); remove => Disconnect(SignalName.ItemMouseSelected, Callable.From(value)); }
    public event Action<Vector2, long> EmptyClicked { add => Connect(SignalName.EmptyClicked, Callable.From(value)); remove => Disconnect(SignalName.EmptyClicked, Callable.From(value)); }
    public event Action ItemEdited { add => Connect(SignalName.ItemEdited, Callable.From(value)); remove => Disconnect(SignalName.ItemEdited, Callable.From(value)); }
    public event Action<TreeItem> ItemCollapsed { add => Connect(SignalName.ItemCollapsed, Callable.From(value)); remove => Disconnect(SignalName.ItemCollapsed, Callable.From(value)); }
    public event Action ItemActivated { add => Connect(SignalName.ItemActivated, Callable.From(value)); remove => Disconnect(SignalName.ItemActivated, Callable.From(value)); }
    public event Action<long, long> ColumnTitleClicked { add => Connect(SignalName.ColumnTitleClicked, Callable.From(value)); remove => Disconnect(SignalName.ColumnTitleClicked, Callable.From(value)); }
    public event Action<TreeItem, long, long, long> ButtonClicked { add => Connect(SignalName.ButtonClicked, Callable.From(value)); remove => Disconnect(SignalName.ButtonClicked, Callable.From(value)); }

    private TreeItem? _root, _selected;
    private int _columns = 1, _selectedCol;
    private bool _hideRoot, _hideFolding, _titles;
    private double _scrollY;
    private readonly List<string> _titleText = new() { "" };
    private readonly List<bool> _expand = new() { true };
    private readonly List<float> _ratio = new() { 1f };
    private readonly List<float> _minWidth = new() { 0f };

    public SelectModeEnum SelectMode { get; set; } = SelectModeEnum.Single;
    public bool AllowReselect { get; set; }
    public bool AllowRmbSelect { get; set; }
    public bool AllowSearch { get; set; } = true;
    public bool EnableRecursiveFolding { get; set; } = true;
    public bool ScrollHorizontalEnabled { get; set; } = true;
    public bool ScrollVerticalEnabled { get; set; } = true;
    public DropModeFlagsEnum DropModeFlags { get; set; }
    public int ScrollHintMode { get; set; }
    public bool TileScrollHint { get; set; }
    public bool ColumnTitlesVisible { get => _titles; set { _titles = value; QueueRedraw(); } }
    public bool HideRoot { get => _hideRoot; set { _hideRoot = value; QueueRedraw(); } }
    public bool HideFolding { get => _hideFolding; set { _hideFolding = value; QueueRedraw(); } }

    public int Columns
    {
        get => _columns;
        set
        {
            value = Math.Clamp(value, 1, 64);
            _columns = value;
            while (_titleText.Count < value) { _titleText.Add(""); _expand.Add(true); _ratio.Add(1f); _minWidth.Add(0f); }
            _root?.Resize(value);
            QueueRedraw();
        }
    }

    public Tree() { FocusMode = FocusModeEnum.All; }

    internal void ItemChanged() { QueueRedraw(); UpdateMinimumSize(); }

    internal void ClearRoot() { _root = null; _selected = null; }

    // ---- items ----

    /// <summary>Makes an item: the root when there is none, otherwise a child of <paramref name="parent"/> (the root by default) at <paramref name="index"/>.</summary>
    public TreeItem? CreateItem(TreeItem? parent = null, int index = -1)
    {
        if (_root == null)
        {
            _root = new TreeItem(this, _columns);
            ItemChanged();
            return _root;
        }
        TreeItem p = parent ?? _root;
        var item = new TreeItem(this, _columns) { Parent = p };
        if (index < 0 || index >= p.Kids.Count) p.Kids.Add(item); else p.Kids.Insert(index, item);
        ItemChanged();
        return item;
    }

    public TreeItem? GetRoot() => _root;

    public void Clear()
    {
        _root?.Free();
        _root = null;
        _selected = null;
        _scrollY = 0;
        ItemChanged();
    }

    public void SetColumnTitle(int column, string title) { if (column >= 0 && column < _columns) { _titleText[column] = title; QueueRedraw(); } }
    public string GetColumnTitle(int column) => column >= 0 && column < _columns ? _titleText[column] : "";
    public void SetColumnExpand(int column, bool expand) { _expand[column] = expand; QueueRedraw(); }
    public bool IsColumnExpanding(int column) => _expand[column];
    public void SetColumnExpandRatio(int column, int ratio) { _ratio[column] = ratio; QueueRedraw(); }
    public int GetColumnExpandRatio(int column) => (int)_ratio[column];
    public void SetColumnCustomMinimumWidth(int column, int minWidth) { _minWidth[column] = minWidth; QueueRedraw(); }
    public int GetColumnCustomMinimumWidth(int column) => (int)_minWidth[column];
    public void SetColumnClipContent(int column, bool enable) { }
    public void SetColumnTitleAlignment(int column, HorizontalAlignment alignment) { }

    private float TitleHeight => _titles ? UiText.LineHeight(ThemeFont("title_button_font"), ThemeFontSize("title_button_font_size")) + 4f : 0f;

    private float RowHeight()
    {
        float h = UiText.LineHeight(ThemeFont(), ThemeFontSize());
        return h + GetThemeConstant("v_separation");
    }

    // The rows in view order, with their depth.
    private List<(TreeItem Item, int Depth)> Rows()
    {
        var rows = new List<(TreeItem, int)>();
        void Walk(TreeItem i, int depth)
        {
            if (!i.IsVisible()) return;
            rows.Add((i, depth));
            if (!i.IsCollapsed()) foreach (TreeItem k in i.Kids) Walk(k, depth + 1);
        }
        if (_root != null)
        {
            if (_hideRoot) foreach (TreeItem k in _root.Kids) Walk(k, 0);
            else Walk(_root, 0);
        }
        return rows;
    }

    internal List<TreeItem> VisibleItems() => Rows().Select(r => r.Item).ToList();

    internal TreeItem? FirstVisible() => Rows().FirstOrDefault().Item;

    private Rect2 Content() => GetThemeStylebox("panel").GetContentRect(LocalRect);

    private float[] ColumnWidths()
    {
        float total = Content().Size.X;
        var w = new float[_columns];
        float fixedSum = 0f, ratioSum = 0f;
        for (int i = 0; i < _columns; i++)
        {
            w[i] = _minWidth[i];
            if (_expand[i]) ratioSum += _ratio[i]; else fixedSum += w[i];
        }
        float room = Math.Max(0f, total - fixedSum);
        for (int i = 0; i < _columns; i++)
            if (_expand[i]) w[i] = Math.Max(_minWidth[i], ratioSum > 0 ? room * _ratio[i] / ratioSum : 0f);
        return w;
    }

    /// <summary>The rectangle of an item's cell (or the whole row for column -1), in the control's space.</summary>
    public Rect2 GetItemAreaRect(TreeItem item, int column = -1, int button = -1)
    {
        var rows = Rows();
        int at = rows.FindIndex(r => r.Item == item);
        if (at < 0) return default;
        Rect2 c = Content();
        float rh = RowHeight();
        float y = c.Position.Y + TitleHeight + at * rh - (float)_scrollY;
        float[] w = ColumnWidths();
        if (column < 0) return new Rect2(c.Position.X, y, c.Size.X, rh);
        float x = c.Position.X;
        for (int i = 0; i < column; i++) x += w[i];
        return new Rect2(x, y, w[column], rh);
    }

    public TreeItem? GetItemAtPosition(Vector2 position)
    {
        var rows = Rows();
        Rect2 c = Content();
        if (!c.HasPoint(position) || position.Y < c.Position.Y + TitleHeight) return null;
        int at = (int)MathF.Floor((position.Y - c.Position.Y - TitleHeight + (float)_scrollY) / RowHeight());
        return at >= 0 && at < rows.Count ? rows[at].Item : null;
    }

    public int GetColumnAtPosition(Vector2 position)
    {
        Rect2 c = Content();
        float x = c.Position.X;
        float[] w = ColumnWidths();
        for (int i = 0; i < _columns; i++)
        {
            if (position.X < x + w[i]) return position.X >= x ? i : -1;
            x += w[i];
        }
        return -1;
    }

    public int GetColumnWidth(int column) => (int)ColumnWidths()[column];

    public void ScrollToItem(TreeItem item, bool centerOnItem = false)
    {
        Rect2 r = GetItemAreaRect(item);
        Rect2 c = Content();
        float top = c.Position.Y + TitleHeight;
        if (centerOnItem) _scrollY += r.Position.Y - (top + (c.End.Y - top - r.Size.Y) / 2f);
        else if (r.Position.Y < top) _scrollY -= top - r.Position.Y;
        else if (r.End.Y > c.End.Y) _scrollY += r.End.Y - c.End.Y;
        _scrollY = Math.Max(0, _scrollY);
        QueueRedraw();
    }

    public void EnsureCursorIsVisible() { if (_selected != null) ScrollToItem(_selected); }

    // ---- selection ----

    public TreeItem? GetSelected() => _selected;

    public int GetSelectedColumn() => _selected != null ? _selectedCol : -1;

    public void SetSelected(TreeItem item, int column) => SelectCell(item, column, false, false);

    public TreeItem? GetNextSelected(TreeItem? from)
    {
        var rows = Rows();
        int start = from == null ? 0 : rows.FindIndex(r => r.Item == from) + 1;
        for (int i = Math.Max(0, start); i < rows.Count; i++)
            if (rows[i].Item.Cells.Any(c => c.Selected)) return rows[i].Item;
        return null;
    }

    public void DeselectAll()
    {
        foreach ((TreeItem i, int _) in Rows()) foreach (TreeItem.Cell c in i.Cells) c.Selected = false;
        _selected = null;
        QueueRedraw();
    }

    public TreeItem? GetEdited() => null;
    public int GetEditedColumn() => -1;
    public bool EditSelected(bool forceEdit = false) => false;

    internal void SelectCell(TreeItem item, int column, bool toggle, bool range)
    {
        if (column < 0 || column >= _columns || !item.Cells[column].Selectable) return;
        if (SelectMode == SelectModeEnum.Multi)
        {
            TreeItem.Cell cell = item.Cells[column];
            if (toggle) cell.Selected = !cell.Selected;
            else
            {
                DeselectAll();
                cell.Selected = true;
            }
            _selected = item;
            _selectedCol = column;
            EmitSignal(SignalName.MultiSelected, item, column, cell.Selected);
        }
        else
        {
            bool same = _selected == item && (SelectMode == SelectModeEnum.Row || _selectedCol == column);
            DeselectAll();
            if (SelectMode == SelectModeEnum.Row) foreach (TreeItem.Cell c in item.Cells) c.Selected = c.Selectable;
            else item.Cells[column].Selected = true;
            _selected = item;
            _selectedCol = column;
            if (!same || AllowReselect)
            {
                EmitSignal(SignalName.CellSelected);
                EmitSignal(SignalName.ItemSelected);
            }
        }
        QueueRedraw();
    }

    // ---- input ----

    private const float Arrow = 16f;

    private float IndentOf(int depth) => (_hideFolding ? 0f : Arrow) + depth * Math.Max(0, GetThemeConstant("item_margin") > 0 ? GetThemeConstant("item_margin") : 16);

    private void Toggle(TreeItem item)
    {
        bool target = !item.Collapsed;
        item.Collapsed = target;
        if (EnableRecursiveFolding && Input.IsKeyPressed(Key.Shift)) item.SetCollapsedRecursive(target);
    }

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseButton mb when mb.Pressed && mb.ButtonIndex is MouseButton.Left or MouseButton.Right or MouseButton.Middle:
                OnPress(mb);
                break;
            case InputEventMouseButton wheel when wheel.Pressed && wheel.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown:
            {
                int total = Rows().Count;
                double max = Math.Max(0, total * RowHeight() - (Content().Size.Y - TitleHeight));
                _scrollY = Math.Clamp(_scrollY + (wheel.ButtonIndex == MouseButton.WheelUp ? -1 : 1) * RowHeight() * 3, 0, max);
                QueueRedraw();
                AcceptEvent();
                break;
            }
            case InputEventKey { Pressed: true } k:
                OnKey(k);
                break;
        }
    }

    private void OnPress(InputEventMouseButton mb)
    {
        GrabFocus();
        AcceptEvent();
        Rect2 c = Content();
        if (_titles && mb.Position.Y >= c.Position.Y && mb.Position.Y < c.Position.Y + TitleHeight)
        {
            int col = GetColumnAtPosition(mb.Position);
            if (col >= 0) EmitSignal(SignalName.ColumnTitleClicked, col, (long)mb.ButtonIndex);
            return;
        }
        TreeItem? item = GetItemAtPosition(mb.Position);
        if (item == null)
        {
            EmitSignal(SignalName.EmptyClicked, mb.Position, (long)mb.ButtonIndex);
            return;
        }
        int column = GetColumnAtPosition(mb.Position);
        if (column < 0) return;
        var rows = Rows();
        int depth = rows.First(r => r.Item == item).Depth;
        TreeItem.Cell cell = item.Cells[column];
        Rect2 cellRect = GetItemAreaRect(item, column);
        float x0 = cellRect.Position.X + (column == 0 ? IndentOf(depth) : 0f);
        // the fold arrow
        if (column == 0 && !_hideFolding && item.Kids.Count > 0 && !item.DisableFolding && mb.Position.X >= x0 - Arrow && mb.Position.X < x0 && mb.ButtonIndex == MouseButton.Left)
        {
            Toggle(item);
            return;
        }
        foreach (TreeItem.Cell cc in new[] { cell })
        {
            // buttons at the right edge of the cell
            float bx = cellRect.End.X;
            for (int b = cc.Buttons.Count - 1; b >= 0; b--)
            {
                float bw = Math.Max(16f, cc.Buttons[b].Icon.GetWidth());
                bx -= bw;
                if (mb.Position.X >= bx && !cc.Buttons[b].Disabled) { EmitSignal(SignalName.ButtonClicked, item, column, cc.Buttons[b].Id, (long)mb.ButtonIndex); return; }
            }
        }
        if (cell.Mode == TreeItem.TreeCellMode.Check && cell.Editable && mb.ButtonIndex == MouseButton.Left && mb.Position.X >= x0 && mb.Position.X < x0 + 20f)
        {
            cell.Checked = !cell.Checked;
            cell.Indeterminate = false;
            EmitSignal(SignalName.ItemEdited);
            QueueRedraw();
            return;
        }
        if (mb.ButtonIndex == MouseButton.Right && !AllowRmbSelect) { EmitSignal(SignalName.ItemMouseSelected, mb.Position, (long)mb.ButtonIndex); return; }
        SelectCell(item, column, SelectMode == SelectModeEnum.Multi && (mb.CtrlPressed || mb.MetaPressed), mb.ShiftPressed);
        EmitSignal(SignalName.ItemMouseSelected, mb.Position, (long)mb.ButtonIndex);
        if (mb.DoubleClick && mb.ButtonIndex == MouseButton.Left) EmitSignal(SignalName.ItemActivated);
    }

    private void Move(int dir)
    {
        var rows = Rows();
        if (rows.Count == 0) return;
        int at = _selected == null ? (dir > 0 ? -1 : rows.Count) : rows.FindIndex(r => r.Item == _selected);
        int to = Math.Clamp(at + dir, 0, rows.Count - 1);
        if (to == at) return;
        SelectCell(rows[to].Item, _selected == null ? 0 : _selectedCol, false, false);
        EnsureCursorIsVisible();
    }

    private void OnKey(InputEventKey k)
    {
        if (Gui.IsUiAction(k, "ui_down")) { Move(1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_up")) { Move(-1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_home")) { _selected = null; Move(1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_end")) { _selected = null; Move(-1); AcceptEvent(); }
        else if (Gui.IsUiAction(k, "ui_right") && _selected != null)
        {
            if (_selected.Kids.Count > 0 && _selected.Collapsed) _selected.Collapsed = false;
            else if (_selectedCol < _columns - 1 && SelectMode != SelectModeEnum.Row) SelectCell(_selected, _selectedCol + 1, false, false);
            AcceptEvent();
        }
        else if (Gui.IsUiAction(k, "ui_left") && _selected != null)
        {
            if (_selected.Kids.Count > 0 && !_selected.Collapsed) _selected.Collapsed = true;
            else if (_selectedCol > 0 && SelectMode != SelectModeEnum.Row) SelectCell(_selected, _selectedCol - 1, false, false);
            else if (_selected.Parent != null && !(_hideRoot && _selected.Parent == _root)) SelectCell(_selected.Parent, _selectedCol, false, false);
            AcceptEvent();
        }
        else if (Gui.IsUiAction(k, "ui_accept") && _selected != null) { EmitSignal(SignalName.ItemActivated); AcceptEvent(); }
        else if (k.Keycode == Key.Space && _selected != null && _selected.Cells[_selectedCol].Mode == TreeItem.TreeCellMode.Check && _selected.Cells[_selectedCol].Editable)
        {
            TreeItem.Cell cell = _selected.Cells[_selectedCol];
            cell.Checked = !cell.Checked;
            EmitSignal(SignalName.ItemEdited);
            QueueRedraw();
            AcceptEvent();
        }
    }

    internal override Vector2 NativeMinimumSize() => GetThemeStylebox("panel").GetMinimumSize();

    // ---- drawing ----

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("panel"), LocalRect);
        if (HasFocus()) DrawStyleBox(GetThemeStylebox("focus"), LocalRect);
        Rect2 c = Content();
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float lh = UiText.LineHeight(font, px), rh = RowHeight();
        float[] widths = ColumnWidths();
        CanvasItem.TextFx fx = ThemeTextFx();
        PushClip(c);
        if (_titles)
        {
            float x = c.Position.X;
            for (int i = 0; i < _columns; i++)
            {
                DrawStyleBox(GetThemeStylebox("title_button_normal"), new Rect2(x, c.Position.Y, widths[i], TitleHeight));
                DrawTextLine(new Vector2(x + 4f, c.Position.Y + 2f), _titleText[i], ThemeFont("title_button_font"), ThemeFontSize("title_button_font_size"), GetThemeColor("title_button_color"), fx);
                x += widths[i];
            }
        }
        float top = c.Position.Y + TitleHeight;
        var rows = Rows();
        for (int r = 0; r < rows.Count; r++)
        {
            (TreeItem item, int depth) = rows[r];
            float y = top + r * rh - (float)_scrollY;
            if (y + rh < top || y > c.End.Y) continue;
            float x = c.Position.X;
            for (int col = 0; col < _columns; col++)
            {
                TreeItem.Cell cell = item.Cells[col];
                var area = new Rect2(x, y, widths[col], rh);
                if (cell.Bg is { } bg) DrawRect(area, bg);
                if (cell.Selected) DrawStyleBox(GetThemeStylebox(HasFocus() ? "selected_focus" : "selected"), area);
                else if (_selected == item && col == _selectedCol && HasFocus()) DrawStyleBox(GetThemeStylebox("cursor"), area);
                float tx = x + 2f;
                if (col == 0)
                {
                    tx = x + IndentOf(depth);
                    if (!_hideFolding && item.Kids.Count > 0 && !item.DisableFolding)
                        DrawTextLine(new Vector2(tx - Arrow + 3f, y + (rh - lh) / 2f), item.Collapsed ? "▸" : "▾", font, px, GetThemeColor("font_color"), fx);
                }
                if (cell.Mode == TreeItem.TreeCellMode.Check)
                {
                    var box = new Rect2(tx, y + (rh - 14f) / 2f, 14f, 14f);
                    var frame = new StyleBoxFlat { BgColor = new Color(0.1f, 0.1f, 0.1f, 0.6f), BorderColor = new Color(0.875f, 0.875f, 0.875f, 1f) };
                    frame.SetBorderWidthAll(1);
                    DrawStyleBox(frame, box);
                    if (cell.Checked || cell.Indeterminate) DrawStyleBox(new StyleBoxFlat { BgColor = new Color(0.95f, 0.95f, 0.95f, cell.Indeterminate ? 0.5f : 1f) }, box.Grow(-4f));
                    tx += 20f;
                }
                if (cell.Icon != null)
                {
                    Vector2 sz = cell.Icon.GetSize();
                    DrawTextureRect(cell.Icon, new Rect2(tx, y + (rh - sz.Y) / 2f, sz.X, sz.Y), false);
                    tx += sz.X + 2f;
                }
                string text = cell.Mode == TreeItem.TreeCellMode.Range ? cell.Value.ToString(System.Globalization.CultureInfo.InvariantCulture) : cell.Text;
                text += cell.Suffix;
                Color color = cell.Custom ?? (cell.Selected ? GetThemeColor("font_selected_color") : GetThemeColor("font_color"));
                float avail = x + widths[col] - tx - cell.Buttons.Count * 16f - 2f;
                float tw = UiText.Width(font, px, text);
                if (tw > avail) text = TextLines.Trim(text, font, px, MathF.Max(0f, avail), false, "…");
                float ox = cell.Align == HorizontalAlignment.Center ? (avail - UiText.Width(font, px, text)) / 2f : cell.Align == HorizontalAlignment.Right ? avail - UiText.Width(font, px, text) : 0f;
                DrawTextLine(new Vector2(tx + Math.Max(0f, ox), y + (rh - lh) / 2f), text, font, px, color, fx);
                float bx = x + widths[col];
                for (int b = cell.Buttons.Count - 1; b >= 0; b--)
                {
                    Texture2D bi = cell.Buttons[b].Icon;
                    float bw = Math.Max(16f, bi.GetWidth());
                    bx -= bw;
                    DrawTextureRect(bi, new Rect2(bx, y + (rh - bi.GetHeight()) / 2f, bi.GetWidth(), bi.GetHeight()), false, cell.Buttons[b].Disabled ? new Color(1, 1, 1, 0.5f) : null);
                }
                x += widths[col];
            }
        }
        PopClip();
    }
}
