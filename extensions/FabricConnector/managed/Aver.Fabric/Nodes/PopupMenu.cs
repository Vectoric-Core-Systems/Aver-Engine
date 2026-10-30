// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// PopupMenu, OptionButton and MenuButton (G5 U2). A menu is a list of items (text, icon, id, check / radio / multistate, separator, submenu, shortcut,
// disabled, metadata, tooltip) laid out in rows; a click or accept key on an item toggles a checkable one, emits id_pressed then index_pressed and hides the
// menu (and the menus it was opened from). The default theme has no icon images, so check marks, radio dots and the submenu arrow are drawn as flat shapes.

using System.Globalization;

namespace Aver.Fabric;

/// <summary>A menu that pops up with a list of items (Godot's PopupMenu).</summary>
public class PopupMenu : Popup
{
    public new class SignalName : Popup.SignalName
    {
        public static readonly StringName IdPressed = "id_pressed";
        public static readonly StringName IdFocused = "id_focused";
        public static readonly StringName IndexPressed = "index_pressed";
        public static readonly StringName MenuChanged = "menu_changed";
    }

    public event Action<long> IdPressed { add => Connect(SignalName.IdPressed, Callable.From(value)); remove => Disconnect(SignalName.IdPressed, Callable.From(value)); }
    public event Action<long> IdFocused { add => Connect(SignalName.IdFocused, Callable.From(value)); remove => Disconnect(SignalName.IdFocused, Callable.From(value)); }
    public event Action<long> IndexPressed { add => Connect(SignalName.IndexPressed, Callable.From(value)); remove => Disconnect(SignalName.IndexPressed, Callable.From(value)); }
    public event Action MenuChanged { add => Connect(SignalName.MenuChanged, Callable.From(value)); remove => Disconnect(SignalName.MenuChanged, Callable.From(value)); }

    private enum Check { None = 0, Check = 1, Radio = 2 }

    private sealed class Item
    {
        public string Text = "";
        public Texture2D? Icon;
        public long Id;
        public bool Disabled, Checked, Separator;
        public Check Kind;
        public int MaxStates, State;
        public Variant Metadata;
        public string Tooltip = "";
        public Shortcut? Shortcut;
        public bool GlobalShortcut;
        public Key Accel;
        public string Submenu = "";
        public PopupMenu? SubmenuNode;
    }

    private readonly List<Item> _items = new();
    private int _focused = -1;
    private PopupMenu? _openSub;

    public bool HideOnItemSelection { get; set; } = true;
    public bool HideOnCheckableItemSelection { get; set; } = true;
    public bool HideOnStateItemSelection { get; set; }
    public float SubmenuPopupDelay { get; set; } = 0.3f;
    public bool AllowSearch { get; set; } = true;
    public int SystemMenuId { get; set; }
    public bool PreferNativeMenu { get; set; }
    public bool ShrinkHeight { get; set; } = true;
    public bool ShrinkWidth { get; set; } = true;

    public PopupMenu() { FocusMode = FocusModeEnum.All; }

    internal override bool CloseOnOutsideClick => true;

    // ---- items ----

    public int ItemCount
    {
        get => _items.Count;
        set
        {
            while (_items.Count < value) _items.Add(new Item { Id = _items.Count });
            if (_items.Count > value) _items.RemoveRange(value, _items.Count - value);
            Changed();
        }
    }

    private void Changed()
    {
        QueueRedraw();
        EmitSignal(SignalName.MenuChanged);
    }

    private Item At(int index) => index >= 0 && index < _items.Count ? _items[index] : throw new ArgumentOutOfRangeException(nameof(index), $"Index {index} is out of range (count {_items.Count}).");

    private int Add(Item item, long id)
    {
        item.Id = id < 0 ? _items.Count : id;
        _items.Add(item);
        Changed();
        return _items.Count - 1;
    }

    public void AddItem(string label, int id = -1, Key accel = Key.None) => Add(new Item { Text = label, Accel = accel }, id);

    public void AddIconItem(Texture2D texture, string label, int id = -1, Key accel = Key.None) => Add(new Item { Text = label, Icon = texture, Accel = accel }, id);

    public void AddCheckItem(string label, int id = -1, Key accel = Key.None) => Add(new Item { Text = label, Kind = Check.Check, Accel = accel }, id);

    public void AddIconCheckItem(Texture2D texture, string label, int id = -1, Key accel = Key.None) => Add(new Item { Text = label, Icon = texture, Kind = Check.Check, Accel = accel }, id);

    public void AddRadioCheckItem(string label, int id = -1, Key accel = Key.None) => Add(new Item { Text = label, Kind = Check.Radio, Accel = accel }, id);

    public void AddIconRadioCheckItem(Texture2D texture, string label, int id = -1, Key accel = Key.None) => Add(new Item { Text = label, Icon = texture, Kind = Check.Radio, Accel = accel }, id);

    /// <summary>An item that cycles through <paramref name="maxStates"/> states when activated.</summary>
    public void AddMultistateItem(string label, int maxStates, int defaultState = 0, int id = -1, Key accel = Key.None)
        => Add(new Item { Text = label, MaxStates = Math.Max(2, maxStates), State = defaultState, Accel = accel }, id);

    public void AddShortcut(Shortcut shortcut, int id = -1, bool global = false, bool allowEcho = false)
    {
        Add(new Item { Text = shortcut.GetAsText(), Shortcut = shortcut, GlobalShortcut = global }, id);
        if (global) SetProcessUnhandledKeyInput(true);
    }

    public void AddIconShortcut(Texture2D texture, Shortcut shortcut, int id = -1, bool global = false, bool allowEcho = false)
    {
        AddShortcut(shortcut, id, global, allowEcho);
        _items[^1].Icon = texture;
    }

    public void AddCheckShortcut(Shortcut shortcut, int id = -1, bool global = false)
    {
        AddShortcut(shortcut, id, global);
        _items[^1].Kind = Check.Check;
    }

    public void AddRadioCheckShortcut(Shortcut shortcut, int id = -1, bool global = false)
    {
        AddShortcut(shortcut, id, global);
        _items[^1].Kind = Check.Radio;
    }

    public void AddSeparator(string label = "", int id = -1) => Add(new Item { Text = label, Separator = true, Disabled = true }, id);

    /// <summary>An item that opens the submenu named <paramref name="submenu"/> (a PopupMenu child of this menu).</summary>
    public void AddSubmenuItem(string label, string submenu, int id = -1) => Add(new Item { Text = label, Submenu = submenu }, id);

    public void AddSubmenuNodeItem(string label, PopupMenu submenu, int id = -1) => Add(new Item { Text = label, Submenu = submenu.Name, SubmenuNode = submenu }, id);

    public string GetItemText(int index) => At(index).Text;
    public void SetItemText(int index, string text) { At(index).Text = text; Changed(); }
    public Texture2D? GetItemIcon(int index) => At(index).Icon;
    public void SetItemIcon(int index, Texture2D? icon) { At(index).Icon = icon; Changed(); }
    public int GetItemId(int index) => (int)At(index).Id;
    public void SetItemId(int index, int id) { At(index).Id = id; }
    public bool IsItemChecked(int index) => At(index).Checked;
    public void SetItemChecked(int index, bool @checked) { At(index).Checked = @checked; QueueRedraw(); }
    public bool IsItemCheckable(int index) => At(index).Kind != Check.None;
    public bool IsItemRadioCheckable(int index) => At(index).Kind == Check.Radio;
    public void SetItemAsCheckable(int index, bool enable) { At(index).Kind = enable ? Check.Check : Check.None; QueueRedraw(); }
    public void SetItemAsRadioCheckable(int index, bool enable) { At(index).Kind = enable ? Check.Radio : Check.None; QueueRedraw(); }
    public void SetItemAsSeparator(int index, bool enable) { At(index).Separator = enable; QueueRedraw(); }
    public bool IsItemSeparator(int index) => At(index).Separator;
    public bool IsItemDisabled(int index) => At(index).Disabled;
    public void SetItemDisabled(int index, bool disabled) { At(index).Disabled = disabled; QueueRedraw(); }
    public Variant GetItemMetadata(int index) => At(index).Metadata;
    public void SetItemMetadata(int index, Variant metadata) { At(index).Metadata = metadata; }
    public string GetItemTooltip(int index) => At(index).Tooltip;
    public void SetItemTooltip(int index, string tooltip) { At(index).Tooltip = tooltip; }
    public Shortcut? GetItemShortcut(int index) => At(index).Shortcut;
    public void SetItemShortcut(int index, Shortcut? shortcut, bool global = false) { At(index).Shortcut = shortcut; At(index).GlobalShortcut = global; if (global) SetProcessUnhandledKeyInput(true); QueueRedraw(); }
    public Key GetItemAccelerator(int index) => At(index).Accel;
    public void SetItemAccelerator(int index, Key accel) { At(index).Accel = accel; QueueRedraw(); }
    public string GetItemSubmenu(int index) => At(index).Submenu;
    public void SetItemSubmenu(int index, string submenu) { At(index).Submenu = submenu; QueueRedraw(); }
    public PopupMenu? GetItemSubmenuNode(int index) => SubmenuOf(At(index));
    public void SetItemSubmenuNode(int index, PopupMenu? submenu) { At(index).SubmenuNode = submenu; At(index).Submenu = submenu?.Name ?? ""; QueueRedraw(); }
    public int GetItemMultistateMax(int index) => At(index).MaxStates;
    public int GetItemMultistate(int index) => At(index).State;
    public void SetItemMultistate(int index, int state) { At(index).State = state; QueueRedraw(); }
    public void ToggleItemChecked(int index) { At(index).Checked = !At(index).Checked; QueueRedraw(); }
    public void ToggleItemMultistate(int index) { Item i = At(index); if (i.MaxStates > 0) i.State = (i.State + 1) % i.MaxStates; QueueRedraw(); }

    public int GetItemIndex(int id)
    {
        for (int i = 0; i < _items.Count; i++) if (_items[i].Id == id) return i;
        return -1;
    }

    public int GetItemCount() => _items.Count;

    public void RemoveItem(int index) { At(index); _items.RemoveAt(index); if (_focused >= _items.Count) _focused = -1; Changed(); }

    public void Clear(bool freeSubmenus = false) { _items.Clear(); _focused = -1; Changed(); }

    public int GetFocusedItem() => _focused;

    public void SetFocusedItem(int index)
    {
        _focused = index;
        if (index >= 0 && index < _items.Count) EmitSignal(SignalName.IdFocused, _items[index].Id);
        QueueRedraw();
    }

    /// <summary>The scene stores items as <c>item_N/text</c>, <c>item_N/id</c>, <c>item_N/checked</c>, <c>item_N/checkable</c> (1 check, 2 radio), <c>item_N/separator</c>,
    /// <c>item_N/disabled</c>, <c>item_N/icon</c>, <c>item_N/submenu</c>, <c>item_N/shortcut</c>; the items grow to hold the index.</summary>
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name.StartsWith("item_", StringComparison.Ordinal) && name.IndexOf('/') is var slash and > 5 && int.TryParse(name.AsSpan(5, slash - 5), NumberStyles.Integer, CultureInfo.InvariantCulture, out int index))
        {
            while (_items.Count <= index) _items.Add(new Item { Id = _items.Count });
            Item item = _items[index];
            switch (name[(slash + 1)..])
            {
                case "text": item.Text = value.AsString(); break;
                case "id": item.Id = value.AsInt64(); break;
                case "checked": item.Checked = value.AsBool(); break;
                case "checkable": item.Kind = (Check)value.AsInt32(); break;
                case "separator": item.Separator = value.AsBool(); if (item.Separator) item.Disabled = true; break;
                case "disabled": item.Disabled = value.AsBool(); break;
                case "icon": item.Icon = value.AsGodotObject() as Texture2D; break;
                case "submenu": item.Submenu = value.AsString(); break;
                case "shortcut": item.Shortcut = value.AsGodotObject() as Shortcut; break;
                case "metadata": item.Metadata = value; break;
                default: return false;
            }
            QueueRedraw();
            return true;
        }
        return base.TryDynamicSet(name, value);
    }

    // ---- activation ----

    private PopupMenu? SubmenuOf(Item item) => item.SubmenuNode ?? (item.Submenu.Length > 0 ? GetNodeOrNull(item.Submenu) as PopupMenu : null);

    /// <summary>Activates the item as a click would: toggles a check item, cycles a multistate item, emits id_pressed and index_pressed and hides the menu.</summary>
    public void ActivateItem(int index)
    {
        Item item = At(index);
        if (item.Disabled || item.Separator) return;
        if (SubmenuOf(item) != null) { OpenSubmenu(index); return; }
        bool hide = HideOnItemSelection;
        if (item.Kind != Check.None) { item.Checked = !item.Checked; hide = HideOnCheckableItemSelection; }
        else if (item.MaxStates > 0) { item.State = (item.State + 1) % item.MaxStates; hide = HideOnStateItemSelection; }
        QueueRedraw();
        EmitSignal(SignalName.IdPressed, item.Id);
        EmitSignal(SignalName.IndexPressed, index);
        if (hide) CloseChain();
    }

    // Hides this menu and, when it was opened from another menu, that one too.
    private void CloseChain()
    {
        Hide();
        if (GetParent() is PopupMenu parent && parent.IsPopupShown) parent.CloseChain();
    }

    /// <summary>Triggers the item whose shortcut or accelerator matches the key event; false when none does.</summary>
    public bool ActivateItemByEvent(InputEvent @event, bool forGlobalOnly = false)
    {
        for (int i = 0; i < _items.Count; i++)
        {
            Item item = _items[i];
            if (item.Disabled || item.Separator || (forGlobalOnly && !item.GlobalShortcut)) continue;
            bool match = item.Shortcut != null && item.Shortcut.MatchesEvent(@event);
            if (!match && item.Accel != Key.None && @event is InputEventKey { Pressed: true } k) match = k.Keycode == item.Accel;
            if (match) { ActivateItem(i); return true; }
        }
        return false;
    }

    public override void _UnhandledKeyInput(InputEvent @event)
    {
        if (@event is InputEventKey { Pressed: true, Echo: false } && (IsPopupShown || _items.Exists(i => i.GlobalShortcut)) && ActivateItemByEvent(@event, !IsPopupShown))
            GetViewport()?.SetInputAsHandled();
    }

    private void OpenSubmenu(int index)
    {
        PopupMenu? sub = SubmenuOf(_items[index]);
        if (sub == null) return;
        if (_openSub != null && _openSub != sub) _openSub.Hide();
        _openSub = sub;
        (float top, float height) = RowOf(index);
        Vector2 origin = GetGlobalTransform().Origin;
        if (sub.GetParent() == null) AddChild(sub);
        sub.Popup(new Rect2I((int)(origin.X + Size.X), (int)(origin.Y + top), 0, 0));
    }

    // ---- geometry ----

    private float Hsep => GetThemeConstant("h_separation");
    private float Vsep => GetThemeConstant("v_separation");

    private (float[] Tops, float[] Heights, float Width, float Height) Layout()
    {
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float lh = UiText.LineHeight(font, px);
        StyleBox panel = GetThemeStylebox("panel");
        float startPad = GetThemeConstant("item_start_padding"), endPad = GetThemeConstant("item_end_padding");
        float hsep = Hsep, vsep = Vsep;
        float textW = 0f, iconW = 0f, accelW = 0f, checkW = 0f, subW = 0f;
        foreach (Item i in _items)
        {
            if (i.Separator) { textW = MathF.Max(textW, i.Text.Length > 0 ? UiText.Width(font, px, i.Text) : 0f); continue; }
            textW = MathF.Max(textW, UiText.Width(font, px, i.Text));
            if (i.Icon != null) iconW = MathF.Max(iconW, i.Icon.GetWidth());
            string accel = AccelText(i);
            if (accel.Length > 0) accelW = MathF.Max(accelW, UiText.Width(font, px, accel));
            if (i.Kind != Check.None || i.MaxStates > 0) checkW = 16f + hsep;
            if (SubmenuOf(i) != null || i.Submenu.Length > 0) subW = 12f + hsep;
        }
        float width = panel.GetMinimumSize().X + startPad + checkW + (iconW > 0 ? iconW + hsep : 0f) + textW + (accelW > 0 ? accelW + hsep * 2f : 0f) + subW + endPad;
        var tops = new float[_items.Count];
        var heights = new float[_items.Count];
        float y = panel.GetOffset().Y;
        for (int n = 0; n < _items.Count; n++)
        {
            Item i = _items[n];
            float h = i.Separator ? (i.Text.Length > 0 ? lh + vsep : MathF.Max(GetThemeStylebox("separator").GetMinimumSize().Y, 1f) + 2f) : MathF.Max(lh, i.Icon?.GetHeight() ?? 0) + vsep;
            tops[n] = y;
            heights[n] = h;
            y += h;
        }
        return (tops, heights, width, y + panel.GetMinimumSize().Y - panel.GetOffset().Y);
    }

    private static string AccelText(Item i)
    {
        if (i.Shortcut != null) return i.Shortcut.GetAsText();
        return i.Accel != Key.None ? i.Accel.ToString() : "";
    }

    private (float Top, float Height) RowOf(int index)
    {
        var l = Layout();
        return (l.Tops[index], l.Heights[index]);
    }

    internal override Vector2 ContentSize()
    {
        var l = Layout();
        return new Vector2(l.Width, l.Height);
    }

    /// <summary>The item under a point of the menu, or -1.</summary>
    public int GetItemAt(Vector2 position)
    {
        var l = Layout();
        StyleBox panel = GetThemeStylebox("panel");
        if (position.X < panel.GetOffset().X || position.X > Size.X - (panel.GetMinimumSize().X - panel.GetOffset().X)) return -1;
        for (int i = 0; i < _items.Count; i++)
            if (position.Y >= l.Tops[i] && position.Y < l.Tops[i] + l.Heights[i]) return i;
        return -1;
    }

    internal override void OnAboutToPopup()
    {
        _focused = -1;
    }

    internal override void OnShown()
    {
        FocusMode = FocusModeEnum.All;
        GrabFocus();
    }

    internal override void OnHidden()
    {
        _openSub?.Hide();
        _openSub = null;
        ReleaseFocus();
    }

    public override string _GetTooltip(Vector2 atPosition)
    {
        int i = GetItemAt(atPosition);
        return i >= 0 ? _items[i].Tooltip : "";
    }

    // ---- input ----

    private int Step(int from, int dir)
    {
        if (_items.Count == 0) return -1;
        int i = from;
        for (int n = 0; n < _items.Count; n++)
        {
            i = ((i + dir) % _items.Count + _items.Count) % _items.Count;
            if (!_items[i].Disabled && !_items[i].Separator) return i;
        }
        return -1;
    }

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseMotion mm:
            {
                int over = GetItemAt(mm.Position);
                if (over != _focused)
                {
                    SetFocusedItem(over >= 0 && !_items[over].Separator ? over : -1);
                    if (_openSub != null && (over < 0 || SubmenuOf(_items[over]) != _openSub)) { _openSub.Hide(); _openSub = null; }
                    if (over >= 0 && SubmenuOf(_items[over]) != null && !_items[over].Disabled) OpenSubmenu(over);
                }
                AcceptEvent();
                return;
            }
            case InputEventMouseButton mb when mb.ButtonIndex == MouseButton.Left && !mb.Pressed:
            {
                int at = GetItemAt(mb.Position);
                if (at >= 0) ActivateItem(at);
                AcceptEvent();
                return;
            }
            case InputEventMouseButton mb2 when mb2.ButtonIndex is MouseButton.WheelUp or MouseButton.WheelDown && mb2.Pressed:
                SetFocusedItem(Step(_focused < 0 ? (mb2.ButtonIndex == MouseButton.WheelDown ? -1 : 0) : _focused, mb2.ButtonIndex == MouseButton.WheelDown ? 1 : -1));
                AcceptEvent();
                return;
            case InputEventMouseButton:
                AcceptEvent();
                return;
            case InputEventKey { Pressed: true } k:
                if (Gui.IsUiAction(k, "ui_down")) { SetFocusedItem(Step(_focused, 1)); AcceptEvent(); return; }
                if (Gui.IsUiAction(k, "ui_up")) { SetFocusedItem(Step(_focused < 0 ? 0 : _focused, -1)); AcceptEvent(); return; }
                if (Gui.IsUiAction(k, "ui_accept") && _focused >= 0) { ActivateItem(_focused); AcceptEvent(); return; }
                if (Gui.IsUiAction(k, "ui_right") && _focused >= 0 && SubmenuOf(_items[_focused]) != null) { OpenSubmenu(_focused); AcceptEvent(); return; }
                if (Gui.IsUiAction(k, "ui_left") && GetParent() is PopupMenu) { Hide(); AcceptEvent(); return; }
                if (AllowSearch && k.Unicode > 32 && !k.CtrlPressed && !k.AltPressed)
                {
                    string c = char.ToLowerInvariant((char)k.Unicode).ToString();
                    for (int n = 1; n <= _items.Count; n++)
                    {
                        int idx = ((_focused < 0 ? -1 : _focused) + n) % _items.Count;
                        if (!_items[idx].Disabled && !_items[idx].Separator && _items[idx].Text.ToLowerInvariant().StartsWith(c, StringComparison.Ordinal)) { SetFocusedItem(idx); AcceptEvent(); return; }
                    }
                }
                break;
        }
        base._GuiInput(@event);
    }

    // ---- drawing ----

    public override void _Draw()
    {
        var l = Layout();
        StyleBox panel = GetThemeStylebox("panel");
        DrawStyleBox(panel, LocalRect);
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float lh = UiText.LineHeight(font, px);
        float left = panel.GetOffset().X, right = Size.X - (panel.GetMinimumSize().X - panel.GetOffset().X);
        float startPad = GetThemeConstant("item_start_padding"), endPad = GetThemeConstant("item_end_padding");
        float hsep = Hsep;
        bool anyCheck = _items.Exists(i => i.Kind != Check.None || i.MaxStates > 0);
        float iconW = 0f;
        foreach (Item i in _items) if (i.Icon != null) iconW = MathF.Max(iconW, i.Icon.GetWidth());
        CanvasItem.TextFx fx = ThemeTextFx();
        for (int n = 0; n < _items.Count; n++)
        {
            Item item = _items[n];
            float top = l.Tops[n], h = l.Heights[n];
            if (item.Separator)
            {
                if (item.Text.Length > 0)
                {
                    DrawTextLine(new Vector2(left + startPad + (right - left - UiText.Width(font, px, item.Text)) / 2f, top + (h - lh) / 2f), item.Text, font, px, GetThemeColor("font_separator_color"), fx);
                }
                else DrawStyleBox(GetThemeStylebox("separator"), new Rect2(left, top + (h - MathF.Max(1f, GetThemeStylebox("separator").GetMinimumSize().Y)) / 2f, right - left, MathF.Max(1f, GetThemeStylebox("separator").GetMinimumSize().Y)));
                continue;
            }
            if (n == _focused && !item.Disabled) DrawStyleBox(GetThemeStylebox("hover"), new Rect2(left, top, right - left, h));
            Color color = item.Disabled ? GetThemeColor("font_disabled_color") : n == _focused ? GetThemeColor("font_hover_color") : GetThemeColor("font_color");
            float x = left + startPad;
            float cy = top + h / 2f;
            if (anyCheck)
            {
                if (item.Kind != Check.None || item.MaxStates > 0) DrawMark(new Rect2(x, cy - 8f, 16f, 16f), item, color);
                x += 16f + hsep;
            }
            if (iconW > 0f)
            {
                if (item.Icon != null) DrawTextureRect(item.Icon, new Rect2(x, cy - item.Icon.GetHeight() / 2f, item.Icon.GetWidth(), item.Icon.GetHeight()), false, item.Disabled ? new Color(1, 1, 1, 0.5f) : null);
                x += iconW + hsep;
            }
            DrawTextLine(new Vector2(x, cy - lh / 2f), item.Text, font, px, color, fx);
            string accel = AccelText(item);
            if (accel.Length > 0)
                DrawTextLine(new Vector2(right - endPad - UiText.Width(font, px, accel), cy - lh / 2f), accel, font, px, item.Disabled ? color : GetThemeColor("font_accelerator_color"), fx);
            if (SubmenuOf(item) != null || item.Submenu.Length > 0)
            {
                var arrow = new StyleBoxFlat { BgColor = color };
                arrow.SetCornerRadiusAll(0);
                DrawStyleBox(arrow, new Rect2(right - endPad - 6f, cy - 3f, 3f, 6f));
            }
        }
    }

    private void DrawMark(Rect2 box, Item item, Color color)
    {
        bool on = item.MaxStates > 0 ? item.State > 0 : item.Checked;
        var frame = new StyleBoxFlat { BgColor = new Color(0.1f, 0.1f, 0.1f, 0.6f), BorderColor = color };
        frame.SetBorderWidthAll(1);
        frame.SetCornerRadiusAll(item.Kind == Check.Radio ? 8 : 2);
        DrawStyleBox(frame, box.Grow(-1f));
        if (on)
        {
            var dot = new StyleBoxFlat { BgColor = color };
            dot.SetCornerRadiusAll(item.Kind == Check.Radio ? 4 : 1);
            DrawStyleBox(dot, box.Grow(-5f));
        }
    }
}

/// <summary>A button that drops a list of choices (Godot's OptionButton): one item is selected and shown on the button.</summary>
public class OptionButton : Button
{
    public new class SignalName : Button.SignalName
    {
        public static readonly StringName ItemSelected = "item_selected";
        public static readonly StringName ItemFocused = "item_focused";
    }

    public event Action<long> ItemSelected { add => Connect(SignalName.ItemSelected, Callable.From(value)); remove => Disconnect(SignalName.ItemSelected, Callable.From(value)); }
    public event Action<long> ItemFocused { add => Connect(SignalName.ItemFocused, Callable.From(value)); remove => Disconnect(SignalName.ItemFocused, Callable.From(value)); }

    private readonly PopupMenu _popup = new() { Name = "PopupMenu", HideOnItemSelection = true };
    private int _selected = -1, _pendingSelected = -2;
    private readonly List<Variant> _metadata = new();

    public bool FitToLongestItem { get; set; } = true;
    public bool AllowReselect { get; set; }

    public OptionButton()
    {
        Alignment = HorizontalAlignment.Left;
        _popup.IndexPressed += OnIndexPressed;
        _popup.IdFocused += id => EmitSignal(SignalName.ItemFocused, _popup.GetItemIndex((int)id));
        Pressed += ShowPopup;
    }

    public PopupMenu GetPopup() => _popup;

    /// <summary>Shows the choices under the button.</summary>
    public void ShowPopup()
    {
        if (_popup.ItemCount == 0) return;
        if (_popup.GetParent() == null) SceneTree.Instance.Root.AddChild(_popup);
        Vector2 at = GetGlobalTransform().Origin + new Vector2(0, Size.Y);
        Vector2 size = _popup.ContentSize();
        _popup.Popup(new Rect2I((int)at.X, (int)at.Y, (int)MathF.Max(size.X, Size.X), (int)size.Y));
        if (_selected >= 0) _popup.SetFocusedItem(_selected);
    }

    public int ItemCount { get => _popup.ItemCount; set { _popup.ItemCount = value; SyncSelection(); } }

    public int Selected
    {
        get => _selected;
        set => Select(value);
    }

    private void SyncSelection()
    {
        if (_pendingSelected != -2 && _pendingSelected < _popup.ItemCount) { int p = _pendingSelected; _pendingSelected = -2; Select(p); }
        else Refresh();
    }

    public void Select(int index)
    {
        if (index >= _popup.ItemCount) { _pendingSelected = index; return; }
        _selected = index < 0 ? -1 : index;
        _pendingSelected = -2;
        Refresh();
    }

    private void Refresh()
    {
        if (_selected >= 0 && _selected < _popup.ItemCount) { Text = _popup.GetItemText(_selected); Icon = _popup.GetItemIcon(_selected); }
        else { Text = ""; Icon = null; }
    }

    private void OnIndexPressed(long index)
    {
        bool same = index == _selected;
        Select((int)index);
        if (!same || AllowReselect) EmitSignal(SignalName.ItemSelected, index);
    }

    public int GetSelectedId() => _selected >= 0 ? _popup.GetItemId(_selected) : -1;

    public Variant GetSelectedMetadata() => _selected >= 0 ? _popup.GetItemMetadata(_selected) : default;

    public void AddItem(string label, int id = -1)
    {
        _popup.AddItem(label, id);
        if (_selected < 0 && _popup.ItemCount == 1) Select(0);
        else SyncSelection();
        UpdateMinimumSize();
    }

    public void AddIconItem(Texture2D texture, string label, int id = -1)
    {
        _popup.AddIconItem(texture, label, id);
        if (_selected < 0 && _popup.ItemCount == 1) Select(0);
        else SyncSelection();
        UpdateMinimumSize();
    }

    public void AddSeparator(string text = "") => _popup.AddSeparator(text);

    public void SetItemText(int index, string text) { _popup.SetItemText(index, text); if (index == _selected) Refresh(); UpdateMinimumSize(); }
    public string GetItemText(int index) => _popup.GetItemText(index);
    public void SetItemIcon(int index, Texture2D? icon) { _popup.SetItemIcon(index, icon); if (index == _selected) Refresh(); }
    public Texture2D? GetItemIcon(int index) => _popup.GetItemIcon(index);
    public void SetItemId(int index, int id) => _popup.SetItemId(index, id);
    public int GetItemId(int index) => _popup.GetItemId(index);
    public int GetItemIndex(int id) => _popup.GetItemIndex(id);
    public void SetItemMetadata(int index, Variant metadata) => _popup.SetItemMetadata(index, metadata);
    public Variant GetItemMetadata(int index) => _popup.GetItemMetadata(index);
    public void SetItemDisabled(int index, bool disabled) => _popup.SetItemDisabled(index, disabled);
    public bool IsItemDisabled(int index) => _popup.IsItemDisabled(index);
    public bool IsItemSeparator(int index) => _popup.IsItemSeparator(index);
    public void SetItemTooltip(int index, string tooltip) => _popup.SetItemTooltip(index, tooltip);
    public string GetItemTooltip(int index) => _popup.GetItemTooltip(index);
    public int GetItemCount() => _popup.ItemCount;
    public bool HasSelectableItems() => Enumerable.Range(0, _popup.ItemCount).Any(i => !_popup.IsItemDisabled(i) && !_popup.IsItemSeparator(i));

    public int GetSelectableItem(bool fromLast = false)
    {
        int n = _popup.ItemCount;
        for (int k = 0; k < n; k++)
        {
            int i = fromLast ? n - 1 - k : k;
            if (!_popup.IsItemDisabled(i) && !_popup.IsItemSeparator(i)) return i;
        }
        return -1;
    }

    public void SetItemCount(int count) => ItemCount = count;

    public void RemoveItem(int index)
    {
        _popup.RemoveItem(index);
        if (_selected == index) { _selected = -1; Refresh(); }
        else if (_selected > index) _selected--;
        UpdateMinimumSize();
    }

    public void Clear()
    {
        _popup.Clear();
        _selected = -1;
        Refresh();
        UpdateMinimumSize();
    }

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 min = base.NativeMinimumSize();
        if (!FitToLongestItem) return min;
        Font? font = ThemeFont();
        int px = ThemeFontSize();
        float widest = 0f;
        for (int i = 0; i < _popup.ItemCount; i++) widest = MathF.Max(widest, UiText.Width(font, px, _popup.GetItemText(i)));
        float own = UiText.Width(font, px, Text);
        return new Vector2(min.X + MathF.Max(0f, widest - own) + 16f, min.Y);
    }

    internal override void DrawCheck(Rect2 box, DrawMode mode) { }

    public override void _Draw()
    {
        base._Draw();
        Texture2D? arrow = GetThemeIcon("arrow");
        Rect2 content = StyleFor(GetDrawMode()).GetContentRect(LocalRect);
        if (arrow != null && arrow.EngineHandle != 0)
            DrawTexture(arrow, new Vector2(content.End.X - arrow.GetWidth(), content.Position.Y + (content.Size.Y - arrow.GetHeight()) / 2f), GetThemeColor("font_color"));
        else
        {
            var tri = new StyleBoxFlat { BgColor = GetThemeColor("font_color") };
            DrawStyleBox(tri, new Rect2(content.End.X - 10f, content.Position.Y + content.Size.Y / 2f - 2f, 8f, 4f));
        }
    }

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name.StartsWith("popup/", StringComparison.Ordinal))
        {
            bool ok = _popup.TryDynamicSet(name["popup/".Length..], value);
            if (ok) { SyncSelection(); UpdateMinimumSize(); }
            return ok;
        }
        return base.TryDynamicSet(name, value);
    }
}

/// <summary>A button that opens a menu (Godot's MenuButton).</summary>
public class MenuButton : Button
{
    public new class SignalName : Button.SignalName
    {
        public static readonly StringName AboutToPopup = "about_to_popup";
    }

    public event Action AboutToPopup { add => Connect(SignalName.AboutToPopup, Callable.From(value)); remove => Disconnect(SignalName.AboutToPopup, Callable.From(value)); }

    private readonly PopupMenu _popup = new() { Name = "PopupMenu" };

    public bool SwitchOnHover { get; set; }
    public bool ItemCountSet { get; set; }

    public MenuButton()
    {
        ToggleMode = true;
        FocusMode = FocusModeEnum.None;
        Alignment = HorizontalAlignment.Center;
        Toggled += on => { if (on) ShowMenu(); };
        _popup.PopupHide += () => SetPressedNoSignal(false);
    }

    public PopupMenu GetPopup() => _popup;

    public int ItemCount { get => _popup.ItemCount; set => _popup.ItemCount = value; }

    /// <summary>Opens the menu under the button.</summary>
    public void ShowPopup()
    {
        if (Disabled) return;
        ShowMenu();
    }

    private void ShowMenu()
    {
        EmitSignal(SignalName.AboutToPopup);
        if (_popup.GetParent() == null) SceneTree.Instance.Root.AddChild(_popup);
        Vector2 at = GetGlobalTransform().Origin + new Vector2(0, Size.Y);
        Vector2 size = _popup.ContentSize();
        _popup.Popup(new Rect2I((int)at.X, (int)at.Y, (int)MathF.Max(size.X, Size.X), (int)size.Y));
    }

    internal override void DrawCheck(Rect2 box, DrawMode mode) { }

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name.StartsWith("popup/", StringComparison.Ordinal)) return _popup.TryDynamicSet(name["popup/".Length..], value);
        return base.TryDynamicSet(name, value);
    }
}
