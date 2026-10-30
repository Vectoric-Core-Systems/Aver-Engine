// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// TabBar and TabContainer (G5 U2). One strip implementation (TabStrip) lays out, draws and hit-tests the tabs for both: the TabBar is the strip alone, the
// TabContainer is a strip over a panel that shows one Control child at a time (the tab titles are the children's names, or their `_tab_name` metadata).
// Signals follow Godot: tab_clicked on a press, then tab_selected, then tab_changed when the tab is a new one.

namespace Aver.Fabric;

internal sealed class TabStrip
{
    internal sealed class Tab
    {
        public string Title = "";
        public Texture2D? Icon;
        public bool Disabled, Hidden;
        public string Tooltip = "";
        public Variant Metadata;
    }

    internal readonly Control Host;
    internal readonly List<Tab> Tabs = new();
    internal int Hover = -1;
    internal HorizontalAlignment Align = HorizontalAlignment.Left;
    internal int CloseMode;       // 0 never, 1 active only, 2 always
    internal int MaxWidth;
    internal int Current;

    public TabStrip(Control host) { Host = host; }

    private StyleBox Style(string name) => Host.GetThemeStylebox(name);

    internal float Height()
    {
        float lh = UiText.LineHeight(Host.ThemeFont(), Host.ThemeFontSize());
        float h = 0f;
        foreach (string s in new[] { "tab_selected", "tab_unselected", "tab_disabled" }) h = MathF.Max(h, Style(s).GetMinimumSize().Y);
        float icon = 0f;
        foreach (Tab t in Tabs) if (t.Icon != null) icon = MathF.Max(icon, t.Icon.GetHeight());
        return h + MathF.Max(lh, icon);
    }

    private float TabWidth(int i)
    {
        Tab t = Tabs[i];
        StyleBox style = Style(i == Current ? "tab_selected" : t.Disabled ? "tab_disabled" : "tab_unselected");
        float w = style.GetMinimumSize().X + UiText.Width(Host.ThemeFont(), Host.ThemeFontSize(), t.Title);
        if (t.Icon != null) w += t.Icon.GetWidth() + (t.Title.Length > 0 ? Host.GetThemeConstant("h_separation") : 0);
        if (CloseMode == 2 || (CloseMode == 1 && i == Current)) w += 16f + Host.GetThemeConstant("h_separation");
        if (MaxWidth > 0) w = MathF.Min(w, MaxWidth);
        return w;
    }

    internal float TotalWidth()
    {
        float w = 0f;
        for (int i = 0; i < Tabs.Count; i++) if (!Tabs[i].Hidden) w += TabWidth(i);
        return w;
    }

    /// <summary>The rectangles of the tabs along a strip of the given width (a hidden tab has an empty one).</summary>
    internal Rect2[] Layout(float width)
    {
        var rects = new Rect2[Tabs.Count];
        float h = Height();
        float total = TotalWidth();
        float x = Align == HorizontalAlignment.Center ? (width - total) / 2f : Align == HorizontalAlignment.Right ? width - total : 0f;
        x = MathF.Max(0f, x);
        for (int i = 0; i < Tabs.Count; i++)
        {
            if (Tabs[i].Hidden) { rects[i] = default; continue; }
            float w = TabWidth(i);
            rects[i] = new Rect2(x, 0, w, h);
            x += w;
        }
        return rects;
    }

    internal int TabAt(Vector2 p, float width)
    {
        Rect2[] r = Layout(width);
        for (int i = 0; i < r.Length; i++) if (!Tabs[i].Hidden && r[i].HasPoint(p)) return i;
        return -1;
    }

    internal bool OnClose(int i, Vector2 p, float width)
    {
        if (!(CloseMode == 2 || (CloseMode == 1 && i == Current))) return false;
        Rect2 r = Layout(width)[i];
        return p.X >= r.End.X - Style("tab_selected").GetMargin(Side.Right) - 16f;
    }

    internal void Draw(CanvasItem canvas, Vector2 origin, float width)
    {
        Font? font = Host.ThemeFont();
        int px = Host.ThemeFontSize();
        float lh = UiText.LineHeight(font, px);
        int sep = Host.GetThemeConstant("h_separation");
        Rect2[] rects = Layout(width);
        for (int i = 0; i < Tabs.Count; i++)
        {
            if (Tabs[i].Hidden) continue;
            Tab t = Tabs[i];
            Rect2 r = new(rects[i].Position + origin, rects[i].Size);
            string style = i == Current ? "tab_selected" : t.Disabled ? "tab_disabled" : i == Hover ? "tab_hovered" : "tab_unselected";
            if (i != Current && !Host.HasThemeStylebox(style)) style = "tab_unselected";
            StyleBox box = Style(style);
            canvas.DrawStyleBox(box, r);
            Rect2 content = box.GetContentRect(r);
            Color color = Host.GetThemeColor(i == Current ? "font_selected_color" : t.Disabled ? "font_disabled_color" : i == Hover ? "font_hovered_color" : "font_unselected_color");
            float x = content.Position.X;
            if (t.Icon != null)
            {
                canvas.DrawTextureRect(t.Icon, new Rect2(x, content.Position.Y + (content.Size.Y - t.Icon.GetHeight()) / 2f, t.Icon.GetWidth(), t.Icon.GetHeight()), false);
                x += t.Icon.GetWidth() + sep;
            }
            string title = t.Title;
            float avail = content.End.X - x - (CloseMode == 2 || (CloseMode == 1 && i == Current) ? 16f + sep : 0f);
            if (UiText.Width(font, px, title) > avail) title = TextLines.Trim(title, font, px, MathF.Max(0f, avail), false, "…");
            canvas.DrawTextLine(new Vector2(x, content.Position.Y + (content.Size.Y - lh) / 2f), title, font, px, color, Host.ThemeTextFx());
            if (CloseMode == 2 || (CloseMode == 1 && i == Current))
                canvas.DrawTextLine(new Vector2(r.End.X - box.GetMargin(Side.Right) - 14f, content.Position.Y + (content.Size.Y - lh) / 2f), "×", font, px, color);
        }
    }
}

/// <summary>A strip of tabs (Godot's TabBar).</summary>
public class TabBar : Control
{
    public enum AlignmentMode { Left = 0, Center = 1, Right = 2 }

    public enum CloseButtonDisplayPolicy { ShowNever = 0, ShowActiveOnly = 1, ShowAlways = 2 }

    public new class SignalName : Control.SignalName
    {
        public static readonly StringName TabChanged = "tab_changed";
        public static readonly StringName TabSelected = "tab_selected";
        public static readonly StringName TabClicked = "tab_clicked";
        public static readonly StringName TabClosePressed = "tab_close_pressed";
        public static readonly StringName TabHovered = "tab_hovered";
        public static readonly StringName TabRmbClicked = "tab_rmb_clicked";
        public static readonly StringName ActiveTabRearranged = "active_tab_rearranged";
    }

    public event Action<long> TabChanged { add => Connect(SignalName.TabChanged, Callable.From(value)); remove => Disconnect(SignalName.TabChanged, Callable.From(value)); }
    public event Action<long> TabSelected { add => Connect(SignalName.TabSelected, Callable.From(value)); remove => Disconnect(SignalName.TabSelected, Callable.From(value)); }
    public event Action<long> TabClicked { add => Connect(SignalName.TabClicked, Callable.From(value)); remove => Disconnect(SignalName.TabClicked, Callable.From(value)); }
    public event Action<long> TabClosePressed { add => Connect(SignalName.TabClosePressed, Callable.From(value)); remove => Disconnect(SignalName.TabClosePressed, Callable.From(value)); }
    public event Action<long> TabHovered { add => Connect(SignalName.TabHovered, Callable.From(value)); remove => Disconnect(SignalName.TabHovered, Callable.From(value)); }
    public event Action<long> TabRmbClicked { add => Connect(SignalName.TabRmbClicked, Callable.From(value)); remove => Disconnect(SignalName.TabRmbClicked, Callable.From(value)); }

    internal readonly TabStrip Strip;
    private int _current;

    public bool ClipTabs { get; set; } = true;
    public bool ScrollingEnabled { get; set; } = true;
    public bool DragToRearrangeEnabled { get; set; }
    public bool SelectWithRmb { get; set; }
    public int TabsRearrangeGroup { get; set; } = -1;
    public bool SwitchOnDragHover { get; set; } = true;
    public bool DeselectEnabled { get; set; }
    public bool ScrollToSelected { get; set; } = true;

    public TabBar()
    {
        Strip = new TabStrip(this);
        FocusMode = FocusModeEnum.All;
    }

    public int TabCount { get => Strip.Tabs.Count; set { while (Strip.Tabs.Count < value) Strip.Tabs.Add(new TabStrip.Tab()); if (Strip.Tabs.Count > value) Strip.Tabs.RemoveRange(value, Strip.Tabs.Count - value); Validate(); } }

    public AlignmentMode TabAlignment { get => (AlignmentMode)(int)Strip.Align; set { Strip.Align = (HorizontalAlignment)(int)value; QueueRedraw(); } }

    public CloseButtonDisplayPolicy TabCloseDisplayPolicy { get => (CloseButtonDisplayPolicy)Strip.CloseMode; set { Strip.CloseMode = (int)value; UpdateMinimumSize(); QueueRedraw(); } }

    public int MaxTabWidth { get => Strip.MaxWidth; set { Strip.MaxWidth = value; QueueRedraw(); } }

    public int CurrentTab
    {
        get => _current;
        set => SetCurrent(value, true);
    }

    private void SetCurrent(int tab, bool signals)
    {
        tab = Math.Clamp(tab, Strip.Tabs.Count == 0 ? -1 : 0, Strip.Tabs.Count - 1);
        if (Strip.Tabs.Count == 0) tab = 0;
        int previous = _current;
        _current = tab;
        Strip.Current = tab;
        QueueRedraw();
        if (!signals || Strip.Tabs.Count == 0) return;
        EmitSignal(SignalName.TabSelected, tab);
        if (previous != tab) EmitSignal(SignalName.TabChanged, tab);
    }

    private void Validate()
    {
        if (_current >= Strip.Tabs.Count) { _current = Math.Max(0, Strip.Tabs.Count - 1); Strip.Current = _current; }
        UpdateMinimumSize();
        QueueRedraw();
    }

    public void AddTab(string title = "", Texture2D? icon = null)
    {
        Strip.Tabs.Add(new TabStrip.Tab { Title = title, Icon = icon });
        Validate();
    }

    public void RemoveTab(int tabIdx)
    {
        Strip.Tabs.RemoveAt(tabIdx);
        if (tabIdx < _current) _current--;
        Validate();
    }

    public void Clear() { Strip.Tabs.Clear(); _current = 0; Strip.Current = 0; Validate(); }

    public void MoveTab(int from, int to)
    {
        TabStrip.Tab t = Strip.Tabs[from];
        Strip.Tabs.RemoveAt(from);
        Strip.Tabs.Insert(Math.Clamp(to, 0, Strip.Tabs.Count), t);
        QueueRedraw();
    }

    public void SetTabTitle(int tabIdx, string title) { Strip.Tabs[tabIdx].Title = title; Validate(); }
    public string GetTabTitle(int tabIdx) => Strip.Tabs[tabIdx].Title;
    public void SetTabIcon(int tabIdx, Texture2D? icon) { Strip.Tabs[tabIdx].Icon = icon; Validate(); }
    public Texture2D? GetTabIcon(int tabIdx) => Strip.Tabs[tabIdx].Icon;
    public void SetTabDisabled(int tabIdx, bool disabled) { Strip.Tabs[tabIdx].Disabled = disabled; QueueRedraw(); }
    public bool IsTabDisabled(int tabIdx) => Strip.Tabs[tabIdx].Disabled;
    public void SetTabHidden(int tabIdx, bool hidden) { Strip.Tabs[tabIdx].Hidden = hidden; Validate(); }
    public bool IsTabHidden(int tabIdx) => Strip.Tabs[tabIdx].Hidden;
    public void SetTabTooltip(int tabIdx, string tooltip) { Strip.Tabs[tabIdx].Tooltip = tooltip; }
    public string GetTabTooltip(int tabIdx) => Strip.Tabs[tabIdx].Tooltip;
    public void SetTabMetadata(int tabIdx, Variant metadata) { Strip.Tabs[tabIdx].Metadata = metadata; }
    public Variant GetTabMetadata(int tabIdx) => Strip.Tabs[tabIdx].Metadata;
    public int GetTabIdxAtPoint(Vector2 point) => Strip.TabAt(point, Size.X);
    public Rect2 GetTabRect(int tabIdx) => Strip.Layout(Size.X)[tabIdx];
    public int GetPreviousTab() => PrevAvailable(-1);

    private int PrevAvailable(int dir)
    {
        int n = Strip.Tabs.Count;
        for (int i = 1; i <= n; i++)
        {
            int k = ((_current + dir * i) % n + n) % n;
            if (!Strip.Tabs[k].Disabled && !Strip.Tabs[k].Hidden) return k;
        }
        return -1;
    }

    public bool SelectPreviousAvailable() { int k = PrevAvailable(-1); if (k < 0 || k == _current) return false; CurrentTab = k; return true; }

    public bool SelectNextAvailable() { int k = PrevAvailable(1); if (k < 0 || k == _current) return false; CurrentTab = k; return true; }

    internal override Vector2 NativeMinimumSize() => new(ClipTabs ? 0f : Strip.TotalWidth(), Strip.Height());

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (name.StartsWith("tab_", StringComparison.Ordinal) && name.IndexOf('/') is var slash and > 4 && int.TryParse(name.AsSpan(4, slash - 4), out int index))
        {
            while (Strip.Tabs.Count <= index) Strip.Tabs.Add(new TabStrip.Tab());
            TabStrip.Tab t = Strip.Tabs[index];
            switch (name[(slash + 1)..])
            {
                case "title": t.Title = value.AsString(); break;
                case "icon": t.Icon = value.AsGodotObject() as Texture2D; break;
                case "disabled": t.Disabled = value.AsBool(); break;
                case "hidden": t.Hidden = value.AsBool(); break;
                default: return false;
            }
            Validate();
            return true;
        }
        return base.TryDynamicSet(name, value);
    }

    public override void _GuiInput(InputEvent @event)
    {
        switch (@event)
        {
            case InputEventMouseMotion mm:
            {
                int over = Strip.TabAt(mm.Position, Size.X);
                if (over != Strip.Hover) { Strip.Hover = over; QueueRedraw(); if (over >= 0) EmitSignal(SignalName.TabHovered, over); }
                break;
            }
            case InputEventMouseButton mb when mb.Pressed && mb.ButtonIndex is MouseButton.Left or MouseButton.Right:
            {
                int at = Strip.TabAt(mb.Position, Size.X);
                if (at < 0) break;
                AcceptEvent();
                if (mb.ButtonIndex == MouseButton.Right)
                {
                    EmitSignal(SignalName.TabRmbClicked, at);
                    if (!SelectWithRmb) break;
                }
                else if (Strip.OnClose(at, mb.Position, Size.X)) { EmitSignal(SignalName.TabClosePressed, at); break; }
                EmitSignal(SignalName.TabClicked, at);
                if (!Strip.Tabs[at].Disabled) SetCurrent(at, true);
                break;
            }
            case InputEventKey { Pressed: true } k:
                if (Gui.IsUiAction(k, "ui_left")) { if (SelectPreviousAvailable()) AcceptEvent(); }
                else if (Gui.IsUiAction(k, "ui_right")) { if (SelectNextAvailable()) AcceptEvent(); }
                break;
        }
    }

    public override void _Notification(int what)
    {
        if (what == NotificationMouseExit) { Strip.Hover = -1; QueueRedraw(); }
    }

    public override string _GetTooltip(Vector2 atPosition)
    {
        int i = Strip.TabAt(atPosition, Size.X);
        return i >= 0 && Strip.Tabs[i].Tooltip.Length > 0 ? Strip.Tabs[i].Tooltip : TooltipText;
    }

    public override void _Draw()
    {
        DrawStyleBox(GetThemeStylebox("tabbar_background"), LocalRect);
        PushClip(LocalRect);
        Strip.Draw(this, Vector2.Zero, Size.X);
        PopClip();
    }
}

/// <summary>Shows one of its Control children at a time under a row of tabs (Godot's TabContainer).</summary>
public class TabContainer : Container
{
    public enum TabPositionEnum { Top = 0, Bottom = 1, Max = 2 }

    public new class SignalName : Container.SignalName
    {
        public static readonly StringName TabChanged = "tab_changed";
        public static readonly StringName TabSelected = "tab_selected";
        public static readonly StringName TabClicked = "tab_clicked";
        public static readonly StringName TabHovered = "tab_hovered";
        public static readonly StringName TabButtonPressed = "tab_button_pressed";
        public static readonly StringName PrePopupPressed = "pre_popup_pressed";
    }

    public event Action<long> TabChanged { add => Connect(SignalName.TabChanged, Callable.From(value)); remove => Disconnect(SignalName.TabChanged, Callable.From(value)); }
    public event Action<long> TabSelected { add => Connect(SignalName.TabSelected, Callable.From(value)); remove => Disconnect(SignalName.TabSelected, Callable.From(value)); }
    public event Action<long> TabClicked { add => Connect(SignalName.TabClicked, Callable.From(value)); remove => Disconnect(SignalName.TabClicked, Callable.From(value)); }
    public event Action<long> TabHovered { add => Connect(SignalName.TabHovered, Callable.From(value)); remove => Disconnect(SignalName.TabHovered, Callable.From(value)); }

    private readonly TabStrip _strip;
    private int _current;
    private bool _tabsVisible = true;
    private TabPositionEnum _position = TabPositionEnum.Top;

    public bool ClipTabs { get; set; } = true;
    public bool UseHiddenTabsForMinSize { get; set; }
    public bool DragToRearrangeEnabled { get; set; }
    public bool DeselectEnabled { get; set; }
    public int TabsRearrangeGroup { get; set; } = -1;
    public FocusModeEnum TabFocusMode { get; set; } = FocusModeEnum.Click;
    public bool SwitchOnDragHover { get; set; } = true;
    public bool AllTabsInFront { get; set; }

    public TabContainer()
    {
        _strip = new TabStrip(this);
        MouseFilter = MouseFilterEnum.Stop;
        Connect(Node.SignalName.ChildOrderChanged, Callable.From(Sync));
    }

    public TabBar.AlignmentMode TabAlignment { get => (TabBar.AlignmentMode)(int)_strip.Align; set { _strip.Align = (HorizontalAlignment)(int)value; QueueRedraw(); } }
    public TabPositionEnum TabsPosition { get => _position; set { _position = value; QueueSort(); QueueRedraw(); } }
    public bool TabsVisible { get => _tabsVisible; set { _tabsVisible = value; UpdateMinimumSize(); QueueSort(); QueueRedraw(); } }
    public int TabsRearrangeGroupId { get => TabsRearrangeGroup; set => TabsRearrangeGroup = value; }

    private List<Control> Pages()
    {
        var list = new List<Control>();
        foreach (Node n in ChildrenSnapshot()) if (n is Control c && !c.TopLevel) list.Add(c);
        return list;
    }

    private void Sync()
    {
        List<Control> pages = Pages();
        var tabs = new List<TabStrip.Tab>();
        foreach (Control p in pages)
            tabs.Add(new TabStrip.Tab
            {
                Title = p.HasMeta("_tab_name") ? p.GetMeta("_tab_name").AsString() : p.Name.ToString(),
                Icon = p.HasMeta("_tab_icon") ? p.GetMeta("_tab_icon").AsGodotObject() as Texture2D : null,
                Disabled = p.HasMeta("_tab_disabled") && p.GetMeta("_tab_disabled").AsBool(),
                Hidden = p.HasMeta("_tab_hidden") && p.GetMeta("_tab_hidden").AsBool(),
            });
        _strip.Tabs.Clear();
        _strip.Tabs.AddRange(tabs);
        _current = Math.Clamp(_current, 0, Math.Max(0, pages.Count - 1));
        _strip.Current = _current;
        ShowCurrent(pages);
        UpdateMinimumSize();
        QueueSort();
        QueueRedraw();
    }

    private void ShowCurrent(List<Control> pages)
    {
        for (int i = 0; i < pages.Count; i++)
            if (pages[i].Visible != (i == _current)) pages[i].Visible = i == _current;
    }

    public int CurrentTab
    {
        get => _current;
        set => SetCurrent(value, true);
    }

    private void SetCurrent(int tab, bool signals)
    {
        List<Control> pages = Pages();
        if (pages.Count == 0) { _current = Math.Max(0, tab); return; }
        tab = Math.Clamp(tab, 0, pages.Count - 1);
        int previous = _current;
        _current = tab;
        _strip.Current = tab;
        ShowCurrent(pages);
        UpdateMinimumSize();
        QueueSort();
        QueueRedraw();
        if (!signals) return;
        EmitSignal(SignalName.TabSelected, tab);
        if (previous != tab) EmitSignal(SignalName.TabChanged, tab);
    }

    public int GetTabCount() => Pages().Count;

    public Control? GetCurrentTabControl() => GetTabControl(_current);

    public Control? GetTabControl(int tabIdx)
    {
        List<Control> pages = Pages();
        return tabIdx >= 0 && tabIdx < pages.Count ? pages[tabIdx] : null;
    }

    public int GetTabIdxFromControl(Control control) => Pages().IndexOf(control);

    public string GetTabTitle(int tabIdx) => tabIdx >= 0 && tabIdx < _strip.Tabs.Count ? _strip.Tabs[tabIdx].Title : "";

    public void SetTabTitle(int tabIdx, string title)
    {
        if (GetTabControl(tabIdx) is { } c) c.SetMeta("_tab_name", title);
        Sync();
    }

    public Texture2D? GetTabIcon(int tabIdx) => tabIdx >= 0 && tabIdx < _strip.Tabs.Count ? _strip.Tabs[tabIdx].Icon : null;

    public void SetTabIcon(int tabIdx, Texture2D? icon)
    {
        if (GetTabControl(tabIdx) is { } c) c.SetMeta("_tab_icon", Variant.CreateFrom((GodotObject?)icon));
        Sync();
    }

    public bool IsTabDisabled(int tabIdx) => tabIdx >= 0 && tabIdx < _strip.Tabs.Count && _strip.Tabs[tabIdx].Disabled;

    public void SetTabDisabled(int tabIdx, bool disabled)
    {
        if (GetTabControl(tabIdx) is { } c) c.SetMeta("_tab_disabled", disabled);
        Sync();
    }

    public bool IsTabHidden(int tabIdx) => tabIdx >= 0 && tabIdx < _strip.Tabs.Count && _strip.Tabs[tabIdx].Hidden;

    public void SetTabHidden(int tabIdx, bool hidden)
    {
        if (GetTabControl(tabIdx) is { } c) c.SetMeta("_tab_hidden", hidden);
        Sync();
    }

    public int GetPreviousTab()
    {
        int n = _strip.Tabs.Count;
        return n == 0 ? -1 : (_current + n - 1) % n;
    }

    public bool SelectPreviousAvailable() => Step(-1);

    public bool SelectNextAvailable() => Step(1);

    private bool Step(int dir)
    {
        int n = _strip.Tabs.Count;
        for (int i = 1; i < n; i++)
        {
            int k = ((_current + dir * i) % n + n) % n;
            if (!_strip.Tabs[k].Disabled && !_strip.Tabs[k].Hidden) { CurrentTab = k; return true; }
        }
        return false;
    }

    public int GetTabIdxAtPoint(Vector2 point) => _strip.TabAt(point - BarOrigin(), Size.X);

    // ---- layout ----

    private float BarHeight => _tabsVisible ? _strip.Height() : 0f;

    private Vector2 BarOrigin() => _position == TabPositionEnum.Bottom ? new Vector2(0, Size.Y - BarHeight) : Vector2.Zero;

    internal override Vector2 NativeMinimumSize()
    {
        Vector2 panel = GetThemeStylebox("panel").GetMinimumSize();
        Vector2 child = Vector2.Zero;
        List<Control> pages = Pages();
        for (int i = 0; i < pages.Count; i++)
        {
            if (i != _current && !UseHiddenTabsForMinSize) continue;
            Vector2 m = pages[i].GetCombinedMinimumSize();
            child = new Vector2(MathF.Max(child.X, m.X), MathF.Max(child.Y, m.Y));
        }
        float tabsW = ClipTabs || !_tabsVisible ? 0f : _strip.TotalWidth();
        return new Vector2(MathF.Max(child.X + panel.X, tabsW), child.Y + panel.Y + BarHeight);
    }

    internal override void SortChildren()
    {
        Rect2 area = new(0, _position == TabPositionEnum.Top ? BarHeight : 0f, Size.X, MathF.Max(0f, Size.Y - BarHeight));
        Rect2 content = GetThemeStylebox("panel").GetContentRect(area);
        foreach (Control c in Pages()) FitChildInRect(c, content);
    }

    // ---- input and drawing ----

    public override void _GuiInput(InputEvent @event)
    {
        if (!_tabsVisible) return;
        Vector2 origin = BarOrigin();
        float h = BarHeight;
        switch (@event)
        {
            case InputEventMouseMotion mm:
            {
                int over = mm.Position.Y >= origin.Y && mm.Position.Y < origin.Y + h ? _strip.TabAt(mm.Position - origin, Size.X) : -1;
                if (over != _strip.Hover) { _strip.Hover = over; QueueRedraw(); if (over >= 0) EmitSignal(SignalName.TabHovered, over); }
                break;
            }
            case InputEventMouseButton mb when mb.Pressed && mb.ButtonIndex == MouseButton.Left && mb.Position.Y >= origin.Y && mb.Position.Y < origin.Y + h:
            {
                int at = _strip.TabAt(mb.Position - origin, Size.X);
                if (at < 0) break;
                AcceptEvent();
                EmitSignal(SignalName.TabClicked, at);
                if (!_strip.Tabs[at].Disabled) SetCurrent(at, true);
                break;
            }
            case InputEventKey { Pressed: true } k when HasFocus():
                if (Gui.IsUiAction(k, "ui_left")) { if (SelectPreviousAvailable()) AcceptEvent(); }
                else if (Gui.IsUiAction(k, "ui_right")) { if (SelectNextAvailable()) AcceptEvent(); }
                break;
        }
    }

    public override void _Notification(int what)
    {
        if (what == NotificationMouseExit) { _strip.Hover = -1; QueueRedraw(); }
        else if (what == NotificationResized) QueueSort();
    }

    public override string _GetTooltip(Vector2 atPosition)
    {
        int i = _strip.TabAt(atPosition - BarOrigin(), Size.X);
        return i >= 0 && _strip.Tabs[i].Tooltip.Length > 0 ? _strip.Tabs[i].Tooltip : TooltipText;
    }

    public override void _Draw()
    {
        Vector2 origin = BarOrigin();
        float h = BarHeight;
        DrawStyleBox(GetThemeStylebox("panel"), new Rect2(0, _position == TabPositionEnum.Top ? h : 0f, Size.X, MathF.Max(0f, Size.Y - h)));
        if (!_tabsVisible) return;
        DrawStyleBox(GetThemeStylebox("tabbar_background"), new Rect2(origin, new Vector2(Size.X, h)));
        PushClip(new Rect2(origin, new Vector2(Size.X, h)));
        _strip.Draw(this, origin, Size.X);
        PopClip();
    }
}
