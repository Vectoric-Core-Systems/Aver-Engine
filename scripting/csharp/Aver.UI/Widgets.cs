// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The retained widget system as a game script sees it: widgets, layouts, events, input state.
using System.Collections;

namespace Aver.UI;

/// <summary>The kinds of widget. Mirrors the native <c>UiWidgetKind</c>.</summary>
public enum WidgetKind
{
    /// <summary>A container with an optional background; the building block of every layout.</summary>
    Panel = 0,
    /// <summary>A label.</summary>
    Text = 1,
    /// <summary>A texture, or a plain tinted rectangle when it has none.</summary>
    Image = 2,
    /// <summary>Raises <see cref="UiEventKind.Clicked"/> and its command.</summary>
    Button = 3,
    /// <summary>A check box with an optional label.</summary>
    Toggle = 4,
    /// <summary>A horizontal value slider.</summary>
    Slider = 5,
    /// <summary>Cycles through a short list of options with left/right.</summary>
    Choice = 6,
    /// <summary>A scrolling list of selectable rows.</summary>
    List = 7,
    /// <summary>A scrolling container for other widgets.</summary>
    Scroll = 8,
    /// <summary>A single-line text field.</summary>
    TextInput = 9,
    /// <summary>A filled bar.</summary>
    Progress = 10,
    /// <summary>A button that listens for a key press, for rebinding.</summary>
    KeyBind = 11,
}

/// <summary>What happened to a widget. Mirrors the native <c>UiEventType</c>.</summary>
public enum UiEventKind
{
    /// <summary>A button, key-bind or list row was activated.</summary>
    Clicked = 0,
    /// <summary>A toggle flipped; <see cref="UiEvent.Value"/> is 1 when checked.</summary>
    Toggled = 1,
    /// <summary>A slider moved or a choice changed (<see cref="UiEvent.Index"/>).</summary>
    ValueChanged = 2,
    /// <summary>A list row was selected (<see cref="UiEvent.Index"/>).</summary>
    SelectionChanged = 3,
    /// <summary>A text field's contents changed.</summary>
    TextChanged = 4,
    /// <summary>Enter was pressed in a text field.</summary>
    TextSubmitted = 5,
    /// <summary>A widget took the focus.</summary>
    FocusGained = 6,
    /// <summary>A widget lost the focus.</summary>
    FocusLost = 7,
    /// <summary>A key-bind heard a key; <see cref="UiEvent.Index"/> is its raw key slot.</summary>
    KeyCaptured = 8,
    /// <summary>Cancel reached a menu (Escape, gamepad B).</summary>
    Cancel = 9,
    /// <summary>A widget's command fired. <see cref="UiEvent.Text"/> is the command.</summary>
    Command = 10,
}

/// <summary>One thing that happened in the UI.</summary>
/// <param name="Kind">What happened.</param>
/// <param name="Widget">The widget it happened to.</param>
/// <param name="Value">The new value (slider, toggle).</param>
/// <param name="Index">The row, option or key slot involved.</param>
/// <param name="Text">The text or command involved.</param>
public readonly record struct UiEvent(UiEventKind Kind, Widget Widget, float Value, int Index, string Text);

/// <summary>A handle to one widget in the UI tree. Cheap to copy; an id of 0 is "no widget" and every
/// operation on a missing widget is a harmless no-op that returns a default.</summary>
public readonly struct Widget : IEquatable<Widget>
{
    /// <summary>The native widget id.</summary>
    public uint Id { get; }

    /// <summary>Wraps a native id.</summary>
    public Widget(uint id) => Id = id;

    /// <summary>The "no widget" handle.</summary>
    public static Widget None => default;

    /// <summary>Whether this handle names a live widget.</summary>
    public bool Exists => Id != 0 && Native.aver_ui_widget_exists(Id) != 0;

    /// <summary>Creates a widget. With no <paramref name="parent"/> it is a root, which stretches over the viewport.</summary>
    public static Widget Create(WidgetKind kind, Widget parent = default, string name = "")
        => new(Native.aver_ui_widget_create((int)kind, parent.Id, name ?? string.Empty));

    /// <summary>Creates a child of this widget.</summary>
    public Widget Add(WidgetKind kind, string name = "") => Create(kind, this, name);

    /// <summary>Finds a widget by name anywhere in the tree.</summary>
    public static Widget FindAnywhere(string name) => new(Native.aver_ui_widget_find(name, 0));

    /// <summary>Finds a descendant of this widget by name.</summary>
    public Widget Find(string name) => new(Native.aver_ui_widget_find(name, Id));

    /// <summary>Destroys this widget and everything under it.</summary>
    public bool Destroy() => Native.aver_ui_widget_destroy(Id) != 0;

    /// <summary>Moves under <paramref name="newParent"/> (none = make it a root).</summary>
    public bool Reparent(Widget newParent, int index = -1) => Native.aver_ui_widget_reparent(Id, newParent.Id, index) != 0;

    /// <summary>The kind, or Panel for a missing widget.</summary>
    public WidgetKind Kind { get { int k = Native.aver_ui_widget_kind(Id); return k < 0 ? WidgetKind.Panel : (WidgetKind)k; } }

    /// <summary>The parent, or none for a root.</summary>
    public Widget Parent => new(Native.aver_ui_widget_parent(Id));

    /// <summary>The root of this widget's tree.</summary>
    public Widget Root => new(Native.aver_ui_widget_root(Id));

    /// <summary>The direct children, in order.</summary>
    public IEnumerable<Widget> Children
    {
        get
        {
            int n = Native.aver_ui_widget_child_count(Id);
            for (int i = 0; i < n; i++) yield return new Widget(Native.aver_ui_widget_child_at(Id, i));
        }
    }

    /// <summary>Sets any authored property by name and text, as a layout file would
    /// (<c>w.Set("anchors", "0.5 0.5")</c>). False for an unknown name or a bad value.</summary>
    public bool Set(string property, string value) => Native.aver_ui_widget_set_prop(Id, property, value ?? string.Empty) != 0;

    /// <summary>Sets a numeric property (<c>w.Set("width", 200f)</c>).</summary>
    public bool Set(string property, float value) => Set(property, value.ToString("R", System.Globalization.CultureInfo.InvariantCulture));

    /// <summary>Reads any authored property as text, or null for an unknown name.</summary>
    public string? Get(string property)
    {
        IntPtr p = Native.aver_ui_widget_get_prop(Id, property);
        return p == IntPtr.Zero ? null : Native.Utf8(p);
    }

    /// <summary>The label or contents.</summary>
    public string Text
    {
        get => Native.Utf8(Native.aver_ui_widget_get_text(Id));
        set => Native.aver_ui_widget_set_text(Id, value ?? string.Empty);
    }

    /// <summary><see cref="Text"/> after localization: a "@Table:Key=Default" reference resolved for the active culture.
    /// <see cref="Text"/> stays the raw authored string.</summary>
    public string ShownText => Native.Utf8(Native.aver_ui_widget_get_shown_text(Id));

    // ---- text styling (typed wrappers over the property table; docs/GAME_UI_TEXT.md) ---------------

    private static readonly string[] s_alignNames = { "left", "center", "right", "start", "end" };
    private static readonly string[] s_dirNames = { "auto", "ltr", "rtl" };
    private static readonly float[] s_textSize = new float[3];

    private float GetFloat(string property, float fallback)
        => float.TryParse(Get(property), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float v)
            ? v : fallback;

    private bool GetBool(string property) => string.Equals(Get(property), "true", StringComparison.OrdinalIgnoreCase);

    private void SetBool(string property, bool value) => Set(property, value ? "true" : "false");

    /// <summary>A family of the loaded font set; empty uses the theme's, then the default family.</summary>
    public string FontFamily
    {
        get => Get("fontFamily") ?? string.Empty;
        set => Set("fontFamily", value ?? string.Empty);
    }

    /// <summary>The text size in design pixels; 0 uses the theme's.</summary>
    public float FontSize
    {
        get => GetFloat("fontSize", 0f);
        set => Set("fontSize", value);
    }

    /// <summary>Use the family's bold face.</summary>
    public bool Bold { get => GetBool("bold"); set => SetBool("bold", value); }

    /// <summary>Use the family's italic face (or shear the regular one).</summary>
    public bool Italic { get => GetBool("italic"); set => SetBool("italic", value); }

    /// <summary>Where lines sit in the widget. <see cref="TextAlignment.Start"/> and <see cref="TextAlignment.End"/> follow the text direction.</summary>
    public TextAlignment TextAlign
    {
        get { int i = Array.IndexOf(s_alignNames, Get("textAlign")); return i < 0 ? TextAlignment.Left : (TextAlignment)i; }
        set => Set("textAlign", s_alignNames[Math.Clamp((int)value, 0, s_alignNames.Length - 1)]);
    }

    /// <summary>The paragraph direction; Auto follows the first strong letter.</summary>
    public TextDirection TextDir
    {
        get { int i = Array.IndexOf(s_dirNames, Get("textDir")); return i < 0 ? TextDirection.Auto : (TextDirection)i; }
        set => Set("textDir", s_dirNames[Math.Clamp((int)value, 0, s_dirNames.Length - 1)]);
    }

    /// <summary>Wrap a Text widget's lines at its width.</summary>
    public bool Wrap { get => GetBool("wrap"); set => SetBool("wrap", value); }

    /// <summary>End a line that is clipped by the widget's width with an ellipsis.</summary>
    public bool Ellipsis { get => GetBool("ellipsis"); set => SetBool("ellipsis", value); }

    /// <summary>The most lines to show; 0 is unlimited.</summary>
    public int MaxLines
    {
        get => (int)GetFloat("maxLines", 0f);
        set => Set("maxLines", (float)value);
    }

    /// <summary>Line height multiplier (1 = the font's own).</summary>
    public float LineSpacing { get => GetFloat("lineSpacing", 1f); set => Set("lineSpacing", value); }

    /// <summary>Extra space after every character, in design pixels.</summary>
    public float LetterSpacing { get => GetFloat("letterSpacing", 0f); set => Set("letterSpacing", value); }

    /// <summary>Read &lt;b&gt; &lt;i&gt; &lt;color=#RRGGBB&gt; &lt;size=N&gt; &lt;font=Name&gt; &lt;br&gt; tags in the text. Off by default, so
    /// strings containing '&lt;' are unaffected.</summary>
    public bool RichText { get => GetBool("rich"); set => SetBool("rich", value); }

    /// <summary>The text colour (overrides the theme's).</summary>
    public Colour TextColour
    {
        set
        {
            uint c = value.Packed;
            Set("textColor", $"#{(c & 0xFF):X2}{((c >> 8) & 0xFF):X2}{((c >> 16) & 0xFF):X2}{(c >> 24):X2}");
        }
    }

    /// <summary>The size of the widget's text as laid out by the last frame (zero before the first frame, and for
    /// a widget with no text).</summary>
    public TextMetrics TextSize
    {
        get
        {
            if (Native.aver_ui_widget_text_size(Id, s_textSize) == 0) return default;
            return new TextMetrics(s_textSize[0], s_textSize[1], (int)s_textSize[2]);
        }
    }

    /// <summary>A slider's or progress bar's value, clamped to its range when set.</summary>
    public float Value
    {
        get => Native.aver_ui_widget_get_value(Id);
        set => Native.aver_ui_widget_set_value(Id, value);
    }

    /// <summary>A toggle's state.</summary>
    public bool Checked
    {
        get => Native.aver_ui_widget_get_checked(Id) != 0;
        set => Native.aver_ui_widget_set_checked(Id, value ? 1 : 0);
    }

    /// <summary>A choice's or list's selected index.</summary>
    public int Selected
    {
        get => Native.aver_ui_widget_get_selected(Id);
        set => Native.aver_ui_widget_set_selected(Id, value);
    }

    /// <summary>Replaces a choice's or list's options.</summary>
    public void SetItems(IEnumerable<string> items) => Native.aver_ui_widget_set_items(Id, string.Join('\n', items));

    /// <summary>Whether this widget and all its ancestors are visible. Setting changes only this widget.</summary>
    public bool Visible
    {
        get => Native.aver_ui_widget_get_visible(Id) != 0;
        set => Native.aver_ui_widget_set_visible(Id, value ? 1 : 0);
    }

    /// <summary>Whether the widget reacts to input.</summary>
    public bool Enabled { set => Native.aver_ui_widget_set_enabled(Id, value ? 1 : 0); }

    /// <summary>The texture an image draws, resolved by the host (see <see cref="UiSystem.ImageResolver"/>).</summary>
    public ulong Texture { set => Native.aver_ui_widget_set_texture(Id, value); }

    private static readonly float[] s_rect = new float[4];

    /// <summary>The widget's screen rectangle as of the last frame.</summary>
    public Rect Rect
    {
        get
        {
            Native.aver_ui_widget_rect(Id, s_rect);
            return new Rect(s_rect[0], s_rect[1], s_rect[2], s_rect[3]);
        }
    }

    /// <summary>Gives this widget the keyboard/gamepad focus. False if it cannot take it.</summary>
    public bool Focus() => Native.aver_ui_set_focus(Id) != 0;

    /// <summary>Runs <paramref name="handler"/> when this widget is clicked. Returns this widget so calls chain.</summary>
    public Widget OnClick(Action handler) { UiSystem.On(this, UiEventKind.Clicked, _ => handler()); return this; }

    /// <summary>Runs <paramref name="handler"/> when a slider or choice changes.</summary>
    public Widget OnChanged(Action<UiEvent> handler) { UiSystem.On(this, UiEventKind.ValueChanged, handler); return this; }

    /// <summary>Runs <paramref name="handler"/> when a toggle flips.</summary>
    public Widget OnToggled(Action<bool> handler) { UiSystem.On(this, UiEventKind.Toggled, e => handler(e.Value > 0.5f)); return this; }

    /// <inheritdoc/>
    public bool Equals(Widget other) => Id == other.Id;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is Widget w && Equals(w);
    /// <inheritdoc/>
    public override int GetHashCode() => (int)Id;
    /// <summary>Handle equality.</summary>
    public static bool operator ==(Widget a, Widget b) => a.Id == b.Id;
    /// <summary>Handle inequality.</summary>
    public static bool operator !=(Widget a, Widget b) => a.Id != b.Id;
    /// <inheritdoc/>
    public override string ToString() => Id == 0 ? "Widget(none)" : $"Widget({Id})";
}

/// <summary>The UI as a whole: layouts, menus, events, theme and scale, and what the UI wants of the
/// game's input.</summary>
/// <remarks>Call <see cref="Pump"/> once a frame (from a HUD's Draw or a game mode's tick) to deliver
/// events to the handlers registered here.</remarks>
public static class UiSystem
{
    private static readonly Dictionary<(uint, UiEventKind), List<Action<UiEvent>>> s_widgetHandlers = new();
    private static readonly Dictionary<string, List<Action<UiEvent>>> s_commandHandlers = new();
    private static readonly byte[] s_textBuf = new byte[1024];
    private static readonly HashSet<uint> s_clickedThisFrame = new();
    private static readonly HashSet<string> s_commandsThisFrame = new();

    /// <summary>Raised for every event, before the per-widget and per-command handlers.</summary>
    public static event Action<UiEvent>? Event;

    /// <summary>Why the last layout failed to open.</summary>
    public static string LastError { get; private set; } = string.Empty;

    /// <summary>Resolves an image path (a layout's <c>image</c> property) to a texture id. Layouts
    /// opened while this is set have their images resolved; assign it before opening.</summary>
    public static Func<string, ulong>? ImageResolver { get; set; }

    // ---- events ---------------------------------------------------------------------------------

    /// <summary>Registers a handler for one kind of event on one widget.</summary>
    public static void On(Widget widget, UiEventKind kind, Action<UiEvent> handler)
    {
        var key = (widget.Id, kind);
        if (!s_widgetHandlers.TryGetValue(key, out var list)) s_widgetHandlers[key] = list = new List<Action<UiEvent>>();
        list.Add(handler);
    }

    /// <summary>Registers a handler for a command string (a button's <c>command</c> property).</summary>
    public static void OnCommand(string command, Action<UiEvent> handler)
    {
        if (!s_commandHandlers.TryGetValue(command, out var list)) s_commandHandlers[command] = list = new List<Action<UiEvent>>();
        list.Add(handler);
    }

    /// <summary>Drops every widget and command handler (and <see cref="Event"/> subscribers are untouched).</summary>
    public static void ClearHandlers()
    {
        s_widgetHandlers.Clear();
        s_commandHandlers.Clear();
    }

    /// <summary>Takes the next event the UI raised, without dispatching it. False when there is none.</summary>
    public static bool TryPoll(out UiEvent e)
    {
        if (Native.aver_ui_event_poll(out int type, out uint widget, out float value, out int index, s_textBuf, s_textBuf.Length) == 0)
        {
            e = default;
            return false;
        }
        int n = Array.IndexOf(s_textBuf, (byte)0);
        string text = System.Text.Encoding.UTF8.GetString(s_textBuf, 0, n < 0 ? s_textBuf.Length : n);
        e = new UiEvent((UiEventKind)type, new Widget(widget), value, index, text);
        return true;
    }

    /// <summary>Delivers every pending event to <see cref="Event"/> and the registered handlers. A
    /// handler that throws is skipped, not allowed to starve the rest. Returns how many were delivered.</summary>
    public static int Pump()
    {
        s_clickedThisFrame.Clear();
        s_commandsThisFrame.Clear();
        int count = 0;
        while (count < 512 && TryPoll(out UiEvent e))
        {
            count++;
            Deliver(e);
        }
        return count;
    }

    private static void Deliver(UiEvent e)
    {
        if (e.Kind == UiEventKind.Clicked) s_clickedThisFrame.Add(e.Widget.Id);
        else if (e.Kind == UiEventKind.Command) s_commandsThisFrame.Add(e.Text);
        try { Event?.Invoke(e); } catch (Exception) { /* a subscriber's fault is not the UI's */ }
        if (s_widgetHandlers.TryGetValue((e.Widget.Id, e.Kind), out var list))
            foreach (var h in list.ToArray()) { try { h(e); } catch (Exception) { } }
        if (e.Kind == UiEventKind.Command && s_commandHandlers.TryGetValue(e.Text, out var cmds))
            foreach (var h in cmds.ToArray()) { try { h(e); } catch (Exception) { } }
    }

    /// <summary>Whether <paramref name="widget"/> was clicked in the events the last <see cref="Pump"/>
    /// delivered. For polling code such as graph nodes; handlers are better for everything else.</summary>
    public static bool WasClicked(Widget widget) => s_clickedThisFrame.Contains(widget.Id);

    /// <summary>Whether <paramref name="command"/> fired in the events the last <see cref="Pump"/> delivered.</summary>
    public static bool CommandFired(string command) => s_commandsThisFrame.Contains(command);

    // ---- what the UI wants of the game ------------------------------------------------------------

    /// <summary>The pointer is on UI or a menu is open: gameplay should ignore the mouse.</summary>
    public static bool WantsPointer => (Native.aver_ui_wants_input() & 1u) != 0;
    /// <summary>A menu or text field owns the keyboard and gamepad.</summary>
    public static bool WantsKeyboard => (Native.aver_ui_wants_input() & 2u) != 0;
    /// <summary>A menu is open: gameplay input should pause.</summary>
    public static bool PausesGame => (Native.aver_ui_wants_input() & 4u) != 0;
    /// <summary>The cursor should be shown and free.</summary>
    public static bool ShowCursor => (Native.aver_ui_wants_input() & 8u) != 0;

    // ---- environment ----------------------------------------------------------------------------------

    /// <summary>Selects a built-in theme: "dark", "light" or "contrast".</summary>
    public static void SetTheme(string name) => Native.aver_ui_set_theme(name ?? "dark");

    /// <summary>Sets how design pixels scale: mode 0 constant, 1 width, 2 height, 3 shortest side,
    /// 4 blend, against a reference resolution, times the player's <paramref name="userScale"/>.</summary>
    public static void SetScaling(int mode, float referenceWidth, float referenceHeight, float userScale = 1f)
        => Native.aver_ui_set_dpi(mode, referenceWidth, referenceHeight, userScale);

    /// <summary>Sets only the player's UI-scale multiplier.</summary>
    public static void SetUserScale(float userScale) => Native.aver_ui_set_dpi(-1, 0f, 0f, userScale);

    /// <summary>The scale applied to design pixels as of the last frame.</summary>
    public static float Scale => Native.aver_ui_scale();

    // ---- layouts ----------------------------------------------------------------------------------------

    /// <summary>Creates the widgets of an <c>.ocui</c> layout given as text. Returns the root, or none
    /// with <see cref="LastError"/> set.</summary>
    public static Widget OpenLayoutText(string ocui, bool applyEnvironment = true)
    {
        uint id = Native.aver_ui_layout_open(ocui ?? string.Empty, applyEnvironment ? 1 : 0);
        if (id == 0)
        {
            LastError = Native.Utf8(Native.aver_ui_layout_error());
            return Widget.None;
        }
        LastError = string.Empty;
        var root = new Widget(id);
        ResolveImages(root);
        return root;
    }

    /// <summary>Reads an <c>.ocui</c> file and opens it. Returns none with <see cref="LastError"/> set
    /// when the file cannot be read or parsed.</summary>
    public static Widget OpenLayout(string path, bool applyEnvironment = true)
    {
        string text;
        try { text = File.ReadAllText(path); }
        catch (Exception ex)
        {
            LastError = $"cannot read '{path}': {ex.Message}";
            return Widget.None;
        }
        return OpenLayoutText(text, applyEnvironment);
    }

    /// <summary>The widget subtree at <paramref name="root"/> as <c>.ocui</c> text, for saving a layout
    /// built in code. Null for a missing widget.</summary>
    public static string? CaptureLayout(Widget root)
    {
        IntPtr p = Native.aver_ui_layout_capture(root.Id);
        return p == IntPtr.Zero ? null : Native.Utf8(p);
    }

    /// <summary>Asks <see cref="ImageResolver"/> for a texture for every image widget under
    /// <paramref name="root"/> that names one.</summary>
    public static void ResolveImages(Widget root)
    {
        var resolver = ImageResolver;
        if (resolver == null) return;
        var stack = new Stack<Widget>();
        stack.Push(root);
        while (stack.Count > 0)
        {
            Widget w = stack.Pop();
            if (w.Kind == WidgetKind.Image)
            {
                string? path = w.Get("image");
                if (!string.IsNullOrEmpty(path)) w.Texture = resolver(path);
            }
            foreach (Widget c in w.Children) stack.Push(c);
        }
    }

    /// <summary>Opens or closes a layout's root. A closed menu returns the game its input.</summary>
    public static void Show(Widget root, bool visible = true) => root.Visible = visible;

    /// <summary>The widget with the keyboard/gamepad focus, or none.</summary>
    public static Widget Focused => new(Native.aver_ui_focused());

    /// <summary>The topmost widget under a point, or none.</summary>
    public static Widget HitWidget(float x, float y) => new(Native.aver_ui_hit_widget(x, y));

    // ---- menus --------------------------------------------------------------------------------------------

    /// <summary>One button in a <see cref="CreateMenu"/> menu.</summary>
    /// <param name="Label">The text.</param>
    /// <param name="Command">What it raises; handle with <see cref="OnCommand"/>.</param>
    /// <param name="Danger">Draw it as a destructive choice.</param>
    public readonly record struct MenuEntry(string Label, string Command, bool Danger = false);

    /// <summary>A centred titled menu of buttons. Its Cancel (Escape, gamepad B) raises
    /// <paramref name="cancelCommand"/> when given; "ui.close" simply closes it.</summary>
    public static Widget CreateMenu(string name, string title, IEnumerable<MenuEntry> entries,
                                    bool modal = true, string cancelCommand = "")
    {
        string list = string.Join('\n', entries.Select(e => (e.Danger ? "!" : "") + e.Label + "|" + e.Command));
        return new Widget(Native.aver_ui_menu_create(name, title ?? string.Empty, list, modal ? 1 : 0, cancelCommand ?? string.Empty));
    }
}
