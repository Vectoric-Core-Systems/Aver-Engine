// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 U1: the Control runtime. Layout (anchors, offsets, grow, size flags, containers), theme lookup, style boxes over the engine's tessellator, the project
// stretch, and the GUI (mouse_filter propagation, focus, tooltips, drag and drop). The tessellator, the stretch arithmetic and the default theme live in
// C++ (UiStyleBoxTest, UiStretchTest, UiThemeSheetTest) and are reached through Aver.UI.Abi: the suite needs build/bin/Aver.UI.Abi and skips without it.

using System.Runtime.InteropServices;
using Aver.Fabric;

/// <summary>A control that records the events it is given and can accept them.</summary>
class Probe : Control
{
    public static readonly List<string> Trail = new();
    public static readonly List<string> Hover = new();   // mouse_entered / exited, kept apart from the events
    public bool Accepts;
    public bool LogMotion = true;
    public float Radius = -1;   // a round hit area when positive
    public string? Tip;
    public Func<string, Control?>? Custom;
    public Func<Vector2, Variant>? DragSource;
    public Func<Vector2, Variant, bool>? CanDrop;
    public Action<Vector2, Variant>? OnDrop;

    public Probe() { }
    public Probe(string name, float x, float y, float w, float h, MouseFilterEnum filter = MouseFilterEnum.Stop)
    {
        Name = name;
        SetPosition(new Vector2(x, y));
        SetSize(new Vector2(w, h));
        MouseFilter = filter;
    }

    public override void _GuiInput(InputEvent @event)
    {
        string d = UiTests.Describe(@event);
        if (@event is InputEventMouseMotion && !LogMotion) return;
        Trail.Add($"{Name}:{d}");
        if (Accepts) AcceptEvent();
    }

    public override bool _HasPoint(Vector2 point) => Radius > 0 ? (point - Size / 2f).Length() <= Radius : base._HasPoint(point);

    public override string _GetTooltip(Vector2 atPosition)
    {
        Trail.Add($"{Name}:tip {atPosition.X:0.#},{atPosition.Y:0.#}");
        return Tip ?? TooltipText;
    }

    public override GodotObject? _MakeCustomTooltip(string forText) => Custom?.Invoke(forText);

    public override Variant _GetDragData(Vector2 atPosition)
    {
        Trail.Add($"{Name}:getdrag {atPosition.X:0.#},{atPosition.Y:0.#}");
        return DragSource?.Invoke(atPosition) ?? default;
    }

    public override bool _CanDropData(Vector2 atPosition, Variant data)
    {
        bool ok = CanDrop?.Invoke(atPosition, data) ?? false;
        Trail.Add($"{Name}:candrop {ok}");
        return ok;
    }

    public override void _DropData(Vector2 atPosition, Variant data)
    {
        Trail.Add($"{Name}:drop {atPosition.X:0.#},{atPosition.Y:0.#} {data.AsString()}");
        OnDrop?.Invoke(atPosition, data);
    }

    public override void _Notification(int what)
    {
        switch (what)
        {
            case NotificationMouseEnter: Hover.Add($"{Name}:enter"); break;
            case NotificationMouseExit: Hover.Add($"{Name}:exit"); break;
            case NotificationFocusEnter: Trail.Add($"{Name}:focus+"); break;
            case NotificationFocusExit: Trail.Add($"{Name}:focus-"); break;
            case NotificationDragBegin: Trail.Add($"{Name}:dragbegin"); break;
            case NotificationDragEnd: Trail.Add($"{Name}:dragend"); break;
        }
    }
}

/// <summary>A plain node that sees what the GUI let through.</summary>
class UnhandledProbe : Node
{
    public static readonly List<string> Seen = new();
    public override void _UnhandledInput(InputEvent @event) => Seen.Add(UiTests.Describe(@event));
}

static partial class UiTests
{
    private static bool s_native;

    public static int RunAll()
    {
        s_native = EnsureNative();
        if (!s_native)
        {
            Console.WriteLine("Test: ui (SKIP: build/bin/Aver.UI.Abi was not found; build the Aver.UI.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            T.Run("Control: anchors, offsets and the rectangle they make", Anchors);
            T.Run("Control: the layout presets", Presets);
            T.Run("Control: grow directions, minimum size, position and size setters", GrowAndMinimum);
            T.Run("Control: a scene's stored layout values are applied in file order", SceneValues);
            T.Run("Containers: box layout, stretch ratios, alignment and size flags", BoxContainers);
            T.Run("Containers: grid, margin, centre and panel containers", OtherContainers);
            T.Run("Containers: scroll container", Scrolling);
            T.Run("Theme: the lookup order (overrides, node chain, project, default, variations)", ThemeOrder);
            T.Run("Theme: items, types, variations, merge and the .octheme file", ThemeResource);
            T.Run("StyleBox: classes over the engine's tessellator", StyleBoxes);
            T.Run("Stretch: the window's content scale, letterbox and pointer mapping", Stretch);
            T.Run("Drawing: Controls record their draw commands and the GUI replays them", Drawing);
            T.Run("GUI: hit testing, mouse_filter propagation and accept_event", MousePropagation);
            T.Run("GUI: a pressed control keeps the pointer; enter and exit", MouseCapture);
            T.Run("GUI: focus and focus navigation", Focus);
            T.Run("GUI: tooltips and their timing", Tooltips);
            T.Run("GUI: drag and drop", DragDrop);
            T.Run("the converter's output for the UiGame fixture instantiates and lays out (AverAssetC)", ConverterOutput);
        });
    }

    // ---- the native library ----

    private static string LibFile() =>
        OperatingSystem.IsWindows() ? "Aver.UI.Abi.dll" : OperatingSystem.IsMacOS() ? "libAver.UI.Abi.dylib" : "libAver.UI.Abi.so";

    private static string? FindBin(string file)
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string bin = Path.Combine(d.FullName, "build", "bin");
            if (File.Exists(Path.Combine(bin, file))) return bin;
        }
        return null;
    }

    internal static bool EnsureNative()
    {
        string? bin = FindBin(LibFile());
        if (bin != null)
        {
            string beside = Path.Combine(AppContext.BaseDirectory, LibFile());
            try { if (!File.Exists(beside) || File.GetLastWriteTimeUtc(beside) < File.GetLastWriteTimeUtc(Path.Combine(bin, LibFile()))) File.Copy(Path.Combine(bin, LibFile()), beside, true); }
            catch (IOException) { /* a copy from an earlier run is in use */ }
            UiNative.Reprobe();
        }
        return UiNative.Available;
    }

    // ---- helpers ----

    private static Window Root => SceneTree.Instance.Root;

    private static void Settle() => Gui.FlushLayout();

    internal static string Describe(InputEvent e) => e switch
    {
        InputEventMouseButton b => $"{(b.Pressed ? "down" : "up")} {b.ButtonIndex} {b.Position.X:0.#},{b.Position.Y:0.#}",
        InputEventMouseMotion m => $"move {m.Position.X:0.#},{m.Position.Y:0.#}",
        InputEventKey k => $"key {k.Keycode} {(k.Pressed ? "down" : "up")}",
        _ => e.GetType().Name,
    };

    private static void Move(float x, float y, float rx = 0, float ry = 0) =>
        Input.ParseInputEvent(new InputEventMouseMotion { Position = new Vector2(x, y), GlobalPosition = new Vector2(x, y), Relative = new Vector2(rx, ry) });

    private static void Click(float x, float y, bool down, MouseButton b = MouseButton.Left) =>
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(x, y), GlobalPosition = new Vector2(x, y), Pressed = down, ButtonIndex = b });

    private static void PressKey(Key k, bool down = true, bool shift = false) =>
        Input.ParseInputEvent(new InputEventKey { Keycode = k, PhysicalKeycode = k, Pressed = down, ShiftPressed = shift });

    private static bool Is(Vector2 v, float x, float y) => T.Near(v.X, x) && T.Near(v.Y, y);

    private static bool IsRect(Rect2 r, float x, float y, float w, float h) => Is(r.Position, x, y) && Is(r.Size, w, h);

    private static Control Sized(string name, float w, float h)
    {
        var c = new Control { Name = name };
        c.CustomMinimumSize = new Vector2(w, h);
        return c;
    }

    private static Theme Plain()
    {
        // A theme that sets nothing: the default theme answers.
        return new Theme();
    }

    // ---- layout ----

    private static void Anchors()
    {
        var full = new Control { Name = "Full" };
        full.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect);
        Root.AddChild(full);
        T.Check("full rect fills the window (1280 x 720)", IsRect(full.GetRect(), 0, 0, 1280, 720), $"{full.GetRect()}");

        full.SetOffsetsPreset(Control.LayoutPreset.FullRect, Control.LayoutPresetMode.Minsize, 10);
        T.Check("full rect with a margin of 10", IsRect(full.GetRect(), 10, 10, 1260, 700), $"{full.GetRect()}");

        var inner = new Control { Name = "Inner" };
        inner.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect);
        full.AddChild(inner);
        T.Check("a child's anchors are fractions of its parent Control's size", IsRect(inner.GetRect(), 0, 0, 1260, 700));
        full.Size = new Vector2(500, 300);
        T.Check("it follows the parent when the parent is resized", IsRect(inner.GetRect(), 0, 0, 500, 300), $"{inner.GetRect()}");

        var half = new Control { Name = "Half", AnchorLeft = 0.5f, AnchorRight = 1f, AnchorBottom = 1f, OffsetLeft = 20f, OffsetRight = -10f, OffsetBottom = -5f };
        full.AddChild(half);
        T.Check("anchor + offset: x = anchor * parent + offset", IsRect(half.GetRect(), 270, 0, 220, 295), $"{half.GetRect()}");

        var fixedBox = new Control { Name = "Fixed", Size = new Vector2(100, 40), Position = new Vector2(30, 20) };
        T.Check("a control outside the tree keeps the position and size it was given", IsRect(fixedBox.GetRect(), 30, 20, 100, 40));
        full.AddChild(fixedBox);
        T.Check("and keeps them when it enters", IsRect(fixedBox.GetRect(), 30, 20, 100, 40));
        T.Check("the offsets hold position and size when the anchors are the top-left corner", fixedBox.OffsetLeft == 30 && fixedBox.OffsetTop == 20 && fixedBox.OffsetRight == 130 && fixedBox.OffsetBottom == 60);

        var centered = new Control { Name = "Centered", CustomMinimumSize = new Vector2(200, 100) };
        Root.AddChild(centered);
        centered.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.Center);
        T.Check("centre preset at the minimum size is centred in the window", IsRect(centered.GetRect(), 540, 310, 200, 100), $"{centered.GetRect()}");

        centered.Position = new Vector2(700, 400);
        T.Check("setting the position rewrites the offsets, not the anchors", IsRect(centered.GetRect(), 700, 400, 200, 100) && centered.OffsetLeft == 60 && centered.AnchorLeft == 0.5f, $"{centered.OffsetLeft} {centered.AnchorLeft}");
        centered.SetPosition(new Vector2(10, 10), keepOffsets: true);
        T.Check("with keep_offsets the anchors move instead", centered.OffsetLeft == 60 && T.Near(centered.AnchorLeft, (10 - 60) / 1280f), $"{centered.AnchorLeft}");

        int resized = 0;
        var watched = new Control { Name = "Watched", Size = new Vector2(10, 10) };
        watched.Resized += () => resized++;
        Root.AddChild(watched);
        watched.Size = new Vector2(20, 20);
        watched.Size = new Vector2(20, 20);
        watched.Position = new Vector2(5, 5);
        T.Check("resized is emitted when the size changes, and only then", resized == 1, $"{resized}");

        var pushed = new Control { Name = "Pushed", AnchorLeft = 0.2f, AnchorRight = 0.5f };
        Root.AddChild(pushed);
        pushed.SetAnchor(Side.Left, 0.8f);
        T.Check("moving an anchor past its opposite pushes the opposite one", pushed.AnchorLeft == 0.8f && pushed.AnchorRight == 0.8f);
        pushed.SetAnchor(Side.Right, 0.3f, pushOppositeAnchor: false);
        T.Check("or, without push, stops at it", pushed.AnchorRight == 0.8f);
        pushed.SetAnchor(Side.Left, 0.1f, keepOffset: false);
        T.Check("keep_offset false keeps the edge where it was (offset changes)", T.Near(pushed.OffsetLeft, 0.8f * 1280 - 0.1f * 1280));
    }

    private static void Presets()
    {
        float[][] expect =
        {
            new[] { 0f, 0f, 0f, 0f }, new[] { 1f, 0f, 1f, 0f }, new[] { 0f, 1f, 0f, 1f }, new[] { 1f, 1f, 1f, 1f },
            new[] { 0f, .5f, 0f, .5f }, new[] { .5f, 0f, .5f, 0f }, new[] { 1f, .5f, 1f, .5f }, new[] { .5f, 1f, .5f, 1f }, new[] { .5f, .5f, .5f, .5f },
            new[] { 0f, 0f, 0f, 1f }, new[] { 0f, 0f, 1f, 0f }, new[] { 1f, 0f, 1f, 1f }, new[] { 0f, 1f, 1f, 1f },
            new[] { 0f, .5f, 1f, .5f }, new[] { .5f, 0f, .5f, 1f }, new[] { 0f, 0f, 1f, 1f },
        };
        bool ok = true;
        string bad = "";
        for (int p = 0; p < 16; p++)
        {
            var c = new Control { CustomMinimumSize = new Vector2(100, 40) };
            Root.AddChild(c);
            c.SetAnchorsPreset((Control.LayoutPreset)p);
            float[] got = { c.AnchorLeft, c.AnchorTop, c.AnchorRight, c.AnchorBottom };
            for (int i = 0; i < 4; i++) if (got[i] != expect[p][i]) { ok = false; bad += $" preset {p} side {i}"; }
            c.QueueFree();
        }
        T.Check("every preset's anchors are Godot's", ok, bad);

        var win = Root;
        var tl = new Control { CustomMinimumSize = new Vector2(100, 40) };
        Root.AddChild(tl);
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.TopLeft, Control.LayoutPresetMode.Minsize, 8);
        T.Check("top-left with a margin", IsRect(tl.GetRect(), 8, 8, 100, 40), $"{tl.GetRect()}");
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.BottomRight, Control.LayoutPresetMode.Minsize, 8);
        T.Check("bottom-right with a margin", IsRect(tl.GetRect(), 1280 - 108, 720 - 48, 100, 40), $"{tl.GetRect()}");
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.CenterTop);
        T.Check("centre-top", IsRect(tl.GetRect(), 590, 0, 100, 40), $"{tl.GetRect()}");
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.BottomWide, Control.LayoutPresetMode.Minsize, 4);
        T.Check("bottom-wide: full width less the margins, minimum height", IsRect(tl.GetRect(), 4, 720 - 44, 1272, 40), $"{tl.GetRect()}");
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.VcenterWide);
        T.Check("vcenter-wide", IsRect(tl.GetRect(), 0, 340, 1280, 40), $"{tl.GetRect()}");
        tl.Size = new Vector2(300, 300);
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.TopLeft, Control.LayoutPresetMode.KeepSize);
        T.Check("keep_size keeps the current size", Is(tl.Size, 300, 300));
        tl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.TopLeft, Control.LayoutPresetMode.KeepWidth);
        T.Check("keep_width keeps the width and takes the minimum height", Is(tl.Size, 300, 40), $"{tl.Size}");
    }

    private static void GrowAndMinimum()
    {
        Control Make(Control.GrowDirection h, Control.GrowDirection v)
        {
            var c = new Control { OffsetLeft = 100, OffsetTop = 100, OffsetRight = 110, OffsetBottom = 110, CustomMinimumSize = new Vector2(50, 50), GrowHorizontal = h, GrowVertical = v };
            Root.AddChild(c);
            return c;
        }
        var end = Make(Control.GrowDirection.End, Control.GrowDirection.End);
        T.Check("grow end: the minimum size is taken to the right and down", IsRect(end.GetRect(), 100, 100, 50, 50), $"{end.GetRect()}");
        var begin = Make(Control.GrowDirection.Begin, Control.GrowDirection.Begin);
        T.Check("grow begin: it grows to the left and up", IsRect(begin.GetRect(), 60, 60, 50, 50), $"{begin.GetRect()}");
        var both = Make(Control.GrowDirection.Both, Control.GrowDirection.Both);
        T.Check("grow both: half each side", IsRect(both.GetRect(), 80, 80, 50, 50), $"{both.GetRect()}");

        var c2 = new Control();
        Root.AddChild(c2);
        c2.CustomMinimumSize = new Vector2(50, 60);
        Settle();
        T.Check("a custom minimum size raises the size", Is(c2.Size, 50, 60));
        c2.SetSize(new Vector2(10, 10));
        T.Check("set_size cannot go below the minimum", Is(c2.Size, 50, 60));
        c2.Size = new Vector2(80, 90);
        T.Check("size above the minimum is kept", Is(c2.Size, 80, 90));
        c2.ResetSize();
        T.Check("reset_size goes back to the minimum", Is(c2.Size, 50, 60));

        int changed = 0;
        c2.MinimumSizeChanged += () => changed++;
        c2.CustomMinimumSize = new Vector2(70, 60);
        T.Check("the minimum size is up to date at once for a reader", Is(c2.GetCombinedMinimumSize(), 70, 60));
        T.Check("but the size and the signal wait for the end of the frame", changed == 0 && Is(c2.Size, 50, 60));
        Settle();
        T.Check("then the size grows and minimum_size_changed fires once", changed == 1 && Is(c2.Size, 70, 60), $"{changed} {c2.Size}");

        var scripted = new MinOverride();
        Root.AddChild(scripted);
        T.Check("a script's _get_minimum_size counts", Is(scripted.GetCombinedMinimumSize(), 33, 22));
        scripted.CustomMinimumSize = new Vector2(10, 100);
        T.Check("the larger per axis wins over the custom minimum size", Is(scripted.GetCombinedMinimumSize(), 33, 100));
    }

    class MinOverride : Control
    {
        public override Vector2 _GetMinimumSize() => new(33, 22);
    }

    private static void SceneValues()
    {
        var c = new Control { Name = "FromScene" };
        c.PendingValues = new List<(string Key, string Text)>
        {
            ("layout_mode", "1"), ("anchors_preset", "15"), ("anchor_right", "1.0"), ("anchor_bottom", "1.0"),
            ("offset_left", "8.0"), ("offset_top", "8.0"), ("offset_right", "-8.0"), ("offset_bottom", "-8.0"),
            ("grow_horizontal", "2"), ("grow_vertical", "2"), ("size_flags_horizontal", "3"), ("size_flags_vertical", "4"),
            ("size_flags_stretch_ratio", "2.5"), ("custom_minimum_size", "Vector2(30, 20)"), ("mouse_filter", "1"), ("focus_mode", "2"),
            ("tooltip_text", "\"Hello\""), ("clip_contents", "true"), ("pivot_offset", "Vector2(5, 6)"), ("rotation", "0.5"), ("theme_type_variation", "&\"Danger\""),
            ("theme_override_constants/margin_left", "12"), ("theme_override_colors/font_color", "Color(1, 0, 0, 1)"), ("focus_neighbor_right", "NodePath(\"../B\")"),
        };
        Root.AddChild(c);
        T.Check("anchors_preset then anchor_* then offsets_*: a full rectangle inset by 8", IsRect(c.GetRect(), 8, 8, 1264, 704), $"{c.GetRect()} {c.AnchorRight}");
        T.Check("layout_mode is kept", c.LayoutMode == 1);
        T.Check("grow, size flags, ratio", c.GrowHorizontal == Control.GrowDirection.Both && c.SizeFlagsHorizontal == Control.SizeFlags.ExpandFill && c.SizeFlagsVertical == Control.SizeFlags.ShrinkCenter && c.SizeFlagsStretchRatio == 2.5f);
        T.Check("minimum size, mouse filter, focus mode, tooltip, clipping, pivot, rotation",
                Is(c.CustomMinimumSize, 30, 20) && c.MouseFilter == Control.MouseFilterEnum.Pass && c.FocusMode == Control.FocusModeEnum.All && c.TooltipText == "Hello" && c.ClipContents && Is(c.PivotOffset, 5, 6) && T.Near(c.Rotation, 0.5f));
        T.Check("theme_type_variation", c.ThemeTypeVariation.ToString() == "Danger");
        T.Check("theme_override_* keys become overrides", c.GetThemeConstant("margin_left") == 12 && c.GetThemeColor("font_color").R == 1f && c.HasThemeColorOverride("font_color"));
        T.Check("a focus neighbour is a node path", c.FocusNeighborRight.ToString() == "../B");
    }

    // ---- containers ----

    private static HBoxContainer Row(float w, float h, params Control[] kids)
    {
        var hb = new HBoxContainer { Name = "Row" };
        foreach (Control k in kids) hb.AddChild(k);
        Root.AddChild(hb);
        hb.Size = new Vector2(w, h);
        Settle();
        return hb;
    }

    private static void BoxContainers()
    {
        var a = Sized("A", 50, 20);
        var b = Sized("B", 60, 30);
        var c = Sized("C", 40, 10);
        var hb = Row(400, 100, a, b, c);
        T.Check("the default theme's separation is 4", hb.GetThemeConstant("separation") == 4);
        T.Check("children sit in a row with the separation between them", Is(a.Position, 0, 0) && Is(b.Position, 54, 0) && Is(c.Position, 118, 0), $"{a.Position} {b.Position} {c.Position}");
        T.Check("they keep their minimum width and fill the height", Is(a.Size, 50, 100) && Is(b.Size, 60, 100) && Is(c.Size, 40, 100));
        T.Check("the box's minimum size is the sum and the tallest", Is(hb.GetCombinedMinimumSize(), 158, 30), $"{hb.GetCombinedMinimumSize()}");

        b.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        Settle();
        T.Check("one expanding child takes all the room that is left", Is(b.Size, 302, 100) && Is(c.Position, 360, 0) && T.Near(c.Position.X + c.Size.X, 400), $"{b.Size} {c.Position}");

        b.SizeFlagsHorizontal = Control.SizeFlags.Fill;
        a.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        c.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        c.SizeFlagsStretchRatio = 2f;
        Settle();
        T.Check("expanding children share by stretch ratio (1 : 2 of 332)", Is(a.Size, 110, 100) && Is(c.Size, 222, 100) && Is(b.Position, 114, 0) && Is(c.Position, 178, 0), $"{a.Size} {c.Size} {b.Position} {c.Position}");
        T.Check("and the last one ends exactly at the edge", T.Near(c.Position.X + c.Size.X, 400));

        var big = Sized("Big", 300, 20);
        var small = Sized("Small", 20, 20);
        big.SizeFlagsHorizontal = small.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        Row(400, 50, big, small);
        T.Check("a child whose share is below its minimum stops stretching and the others share the rest", Is(big.Size, 300, 50) && Is(small.Position, 304, 0) && Is(small.Size, 96, 50), $"{big.Size} {small.Position} {small.Size}");

        var v1 = Sized("V1", 30, 30);
        var v2 = Sized("V2", 40, 30);
        var vb = new VBoxContainer { Alignment = BoxContainer.AlignmentMode.Center };
        vb.AddChild(v1);
        vb.AddChild(v2);
        Root.AddChild(vb);
        vb.Size = new Vector2(100, 200);
        Settle();
        T.Check("a VBox stacks and centres when nothing expands", Is(v1.Position, 0, 68) && Is(v2.Position, 0, 102) && Is(v1.Size, 100, 30), $"{v1.Position} {v2.Position}");
        vb.Alignment = BoxContainer.AlignmentMode.End;
        Settle();
        T.Check("end alignment pushes it to the bottom", Is(v1.Position, 0, 136) && Is(v2.Position, 0, 170), $"{v1.Position} {v2.Position}");

        var s1 = Sized("S1", 20, 20);
        var s2 = Sized("S2", 20, 20);
        var s3 = Sized("S3", 20, 20);
        s1.SizeFlagsVertical = Control.SizeFlags.ShrinkCenter;
        s2.SizeFlagsVertical = Control.SizeFlags.ShrinkEnd;
        s3.SizeFlagsHorizontal = Control.SizeFlags.ShrinkCenter;
        Row(200, 100, s1, s2, s3);
        T.Check("shrink centre / end place a child that does not fill", Is(s1.Position, 0, 40) && Is(s1.Size, 20, 20) && Is(s2.Position, 24, 80), $"{s1.Position} {s2.Position}");

        var h1 = Sized("H1", 20, 20);
        var h2 = Sized("H2", 20, 20);
        var h3 = Sized("H3", 20, 20);
        Row(200, 100, h1, h2, h3);
        h2.Visible = false;
        Settle();
        T.Check("a hidden child is skipped", Is(h3.Position, 24, 0), $"{h3.Position}");
        h2.Visible = true;
        Settle();
        T.Check("and comes back", Is(h3.Position, 48, 0));

        var added = Sized("Late", 10, 10);
        var row = Row(200, 100, Sized("X", 20, 20));
        row.AddChild(added);
        T.Check("a child added later is laid out at the end of the frame, not before", Is(added.Position, 0, 0));
        Settle();
        T.Check("and then it is", Is(added.Position, 24, 0), $"{added.Position}");
    }

    private static void OtherContainers()
    {
        var g = new GridContainer { Columns = 2 };
        var c0 = Sized("c0", 10, 10); var c1 = Sized("c1", 30, 20); var c2 = Sized("c2", 20, 40); var c3 = Sized("c3", 10, 10);
        foreach (Control c in new[] { c0, c1, c2, c3 }) g.AddChild(c);
        Root.AddChild(g);
        Settle();
        T.Check("a grid's minimum size is the column widths and row heights plus separations", Is(g.GetCombinedMinimumSize(), 54, 64), $"{g.GetCombinedMinimumSize()}");
        T.Check("cells line up on the column and row", Is(c0.Position, 0, 0) && Is(c1.Position, 24, 0) && Is(c2.Position, 0, 24) && Is(c3.Position, 24, 24), $"{c1.Position} {c2.Position} {c3.Position}");
        T.Check("each fills its cell", Is(c0.Size, 20, 20) && Is(c1.Size, 30, 20) && Is(c2.Size, 20, 40) && Is(c3.Size, 30, 40), $"{c0.Size} {c3.Size}");
        c1.SizeFlagsHorizontal = Control.SizeFlags.ExpandFill;
        g.Size = new Vector2(200, 64);
        Settle();
        T.Check("an expanding column takes the rest of the width", Is(c1.Position, 24, 0) && Is(c1.Size, 176, 20), $"{c1.Position} {c1.Size}");

        var m = new MarginContainer();
        var inside = Sized("inside", 30, 30);
        m.AddChild(inside);
        m.AddThemeConstantOverride("margin_left", 10);
        m.AddThemeConstantOverride("margin_top", 5);
        m.AddThemeConstantOverride("margin_right", 15);
        m.AddThemeConstantOverride("margin_bottom", 20);
        Root.AddChild(m);
        m.Size = new Vector2(200, 100);
        Settle();
        T.Check("a margin container insets its child", IsRect(inside.GetRect(), 10, 5, 175, 75), $"{inside.GetRect()}");
        T.Check("and adds the margins to its minimum size", Is(m.GetCombinedMinimumSize(), 55, 55), $"{m.GetCombinedMinimumSize()}");

        var cc = new CenterContainer();
        var mid = Sized("mid", 40, 20);
        cc.AddChild(mid);
        Root.AddChild(cc);
        cc.Size = new Vector2(200, 100);
        Settle();
        T.Check("a centre container centres the child at its minimum size", IsRect(mid.GetRect(), 80, 40, 40, 20), $"{mid.GetRect()}");
        cc.UseTopLeft = true;
        Settle();
        T.Check("use_top_left centres it on the corner", IsRect(mid.GetRect(), -20, -10, 40, 20), $"{mid.GetRect()}");

        var pc = new PanelContainer();
        var content = Sized("content", 30, 30);
        pc.AddChild(content);
        var style = new StyleBoxFlat();
        style.SetContentMarginAll(6);
        pc.AddThemeStyleboxOverride("panel", style);
        Root.AddChild(pc);
        pc.Size = new Vector2(100, 80);
        Settle();
        T.Check("a panel container keeps its child inside the style box's margins", IsRect(content.GetRect(), 6, 6, 88, 68), $"{content.GetRect()}");
        T.Check("and its minimum size includes them", Is(pc.GetCombinedMinimumSize(), 42, 42), $"{pc.GetCombinedMinimumSize()}");
        pc.RunDraw();
        T.Check("it draws its panel style box", pc.Commands.Count == 1 && pc.Commands[0].Type == CanvasItem.DrawCommand.Kind.StyleBox && Is(pc.Commands[0].Rect.Size, 100, 80));

        var panel = new Panel { Size = new Vector2(64, 32) };
        Root.AddChild(panel);
        panel.RunDraw();
        T.Check("a panel draws the theme's 'panel' over its rectangle", panel.Commands.Count == 1 && IsRect(panel.Commands[0].Rect, 0, 0, 64, 32));
        panel.RunDraw();
        T.Check("and only again after queue_redraw", panel.Commands.Count == 1);
        panel.QueueRedraw();
        panel.RunDraw();
        T.Check("(one command, redrawn)", panel.Commands.Count == 1);

        int sorts = 0;
        var watched = new HBoxContainer();
        watched.SortChildrenSignal += () => sorts++;
        watched.AddChild(Sized("w", 10, 10));
        Root.AddChild(watched);
        Settle();
        int first = sorts;
        watched.Size = new Vector2(300, 50);
        Settle();
        T.Check("sort_children is emitted on entering and on a resize", first >= 1 && sorts > first, $"{first} {sorts}");
    }

    private static void Scrolling()
    {
        var sc = new ScrollContainer();
        var big = Sized("big", 300, 50);
        sc.AddChild(big);
        Root.AddChild(sc);
        sc.Size = new Vector2(100, 100);
        Settle();
        T.Check("a wide child shows the horizontal bar only", sc.HorizontalBarVisible && !sc.VerticalBarVisible);
        T.Check("and is laid out at its minimum size", IsRect(big.GetRect(), 0, 0, 300, 50), $"{big.GetRect()}");
        sc.ScrollHorizontal = 150;
        Settle();
        T.Check("scroll_horizontal moves the child left", Is(big.Position, -150, 0), $"{big.Position}");
        sc.ScrollHorizontal = 500;
        T.Check("and is clamped to the range", sc.ScrollHorizontal == 200, $"{sc.ScrollHorizontal}");

        var tall = Sized("tall", 300, 400);
        tall.MouseFilter = Control.MouseFilterEnum.Pass;   // a Stop child would keep the wheel for itself, as in Godot
        sc.RemoveChild(big);
        sc.AddChild(tall);
        sc.ScrollHorizontal = 0;
        Settle();
        T.Check("a child bigger on both axes shows both bars", sc.HorizontalBarVisible && sc.VerticalBarVisible);
        Move(40, 40);
        Click(40, 40, true, MouseButton.WheelDown);
        Click(40, 40, false, MouseButton.WheelDown);
        Settle();
        T.Check("the wheel scrolls by an eighth of the view", sc.ScrollVertical == 11, $"{sc.ScrollVertical}");
        sc.VerticalScrollMode = ScrollContainer.ScrollMode.Disabled;
        sc.HorizontalScrollMode = ScrollContainer.ScrollMode.Disabled;
        Settle();
        T.Check("with scrolling disabled the container's minimum size is the child's", Is(sc.GetCombinedMinimumSize(), 300, 400), $"{sc.GetCombinedMinimumSize()}");
        T.Check("and no bars show", !sc.HorizontalBarVisible && !sc.VerticalBarVisible);
        sc.VerticalScrollMode = ScrollContainer.ScrollMode.ShowAlways;
        sc.HorizontalScrollMode = ScrollContainer.ScrollMode.ShowNever;
        sc.Size = new Vector2(100, 100);
        Settle();
        T.Check("show_always shows the bar even when nothing needs it, show_never hides it", sc.VerticalBarVisible && !sc.HorizontalBarVisible);

        var content = Sized("content", 20, 20);
        var host = new ScrollContainer();
        var column = new VBoxContainer();
        var target = Sized("target", 20, 30);
        column.AddChild(Sized("filler", 20, 200));
        column.AddChild(target);
        host.AddChild(column);
        Root.AddChild(host);
        host.Size = new Vector2(100, 100);
        Settle();
        host.EnsureControlVisible(target);
        Settle();
        T.Check("ensure_control_visible scrolls the child into view", host.ScrollVertical > 0 && target.GlobalPosition.Y + target.Size.Y <= host.GlobalPosition.Y + host.Size.Y + 0.5f, $"{host.ScrollVertical} {target.GlobalPosition}");
    }
}
