// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 U2: the widgets. Input events go in, signals and values come out, over the real GUI of U1 and the engine's default theme (Aver.UI.Abi: the suite skips without
// it). Text is measured with the fixed fallback (half an em a character, a line 1.2 em tall) because no font is loaded, so every width below is a count of
// characters times 8 at 16 px. This file: text support, Label, Range and the sliders, and the buttons; WidgetTests.Lists.cs has the menus and fields.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;
using Range = Aver.Fabric.Range;

static partial class WidgetTests
{
    public static int RunAll()
    {
        if (!UiTests.EnsureNative())
        {
            Console.WriteLine("Test: widgets (SKIP: build/bin/Aver.UI.Abi was not found; build the Aver.UI.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            T.Run("Fonts: families, styles and the metrics the text controls share", FontsAndMetrics);
            T.Run("TextLines: word, smart and arbitrary breaking, newlines, CJK, ellipsis", Lines);
            T.Run("Label: minimum size, autowrap, overrun, visible characters, lines, alignment, outline and shadow", LabelBehaviour);
            T.Run("Range: snapping, page, ratio, exp_edit, rounded, signals", RangeBehaviour);
            T.Run("Sliders, scroll bars and progress bars: pointer maths, keys and signals", SlidersAndBars);
            T.Run("SpinBox: typing, arrows, prefix and suffix", SpinBoxBehaviour);
            T.Run("BaseButton and Button: press logic, signals in Godot's order, toggle, groups, keyboard, shortcut", ButtonBehaviour);
            T.Run("CheckBox, CheckButton, LinkButton, TextureButton, ColorRect, TextureRect: size and drawing", ButtonKinds);
            T.Run("PopupMenu: items, check and radio items, separators, submenus, shortcuts, id_pressed", PopupMenuBehaviour);
            T.Run("OptionButton and MenuButton: items, selection and the popup", OptionAndMenuButtons);
            T.Run("Popups and dialogs: popup(), centred, outside click, Escape, AcceptDialog and ConfirmationDialog", PopupsAndDialogs);
            T.Run("LineEdit: typing, caret, selection, clipboard, undo, max_length, secret, signals", LineEditBehaviour);
            T.Run("TextEdit: lines, caret, selection, undo, search, wrapping", TextEditBehaviour);
            T.Run("ItemList: selection modes, keys, double click, signals", ItemListBehaviour);
            T.Run("Tree and TreeItem: items, columns, folding, selection, keys, signals", TreeBehaviour);
            T.Run("TabBar and TabContainer: tabs, signals' order, disabled tabs, child visibility", TabBehaviour);
            T.Run("SplitContainer, FlowContainer, AspectRatioContainer, TextureRect, separators", LayoutBehaviour);
            T.Run("ColorPicker and ColorPickerButton", ColorPickerBehaviour);
            T.Run("ScrollContainer: bars drawn, dragged and mirrored", ScrollBars);
            T.Run("The classes a converted GDScript names are real, with Godot's members", RealClasses);
            T.Run("Text outline and shadow reach the draw commands", OutlineAndShadow);
            T.Run("Properties arrive by their scene names (items, tabs, enums, resources)", SceneNames);
            T.Run("The converter's output for the WidgetGame fixture instantiates and answers through the Godot API (AverAssetC)", ConverterOutput);
        });
    }

    // ---- helpers ----

    private static Window Root => SceneTree.Instance.Root;

    private static void Settle() => Gui.FlushLayout();

    private static void Move(float x, float y, float rx = 0, float ry = 0) =>
        Input.ParseInputEvent(new InputEventMouseMotion { Position = new Vector2(x, y), GlobalPosition = new Vector2(x, y), Relative = new Vector2(rx, ry) });

    private static void Click(float x, float y, bool down, MouseButton b = MouseButton.Left, bool ctrl = false, bool shift = false, bool dbl = false) =>
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(x, y), GlobalPosition = new Vector2(x, y), Pressed = down, ButtonIndex = b, CtrlPressed = ctrl, ShiftPressed = shift, DoubleClick = dbl });

    private static void Tap(float x, float y, MouseButton b = MouseButton.Left, bool ctrl = false, bool shift = false) { Click(x, y, true, b, ctrl, shift); Click(x, y, false, b, ctrl, shift); }

    // A wheel notch is a press and a release, as the engine sends it.
    private static void Wheel(float x, float y, MouseButton b) { Click(x, y, true, b); Click(x, y, false, b); }

    private static void Key_(Key k, bool down = true, bool shift = false, bool ctrl = false) =>
        Input.ParseInputEvent(new InputEventKey { Keycode = k, PhysicalKeycode = k, Pressed = down, ShiftPressed = shift, CtrlPressed = ctrl });

    private static void Press(Key k, bool shift = false, bool ctrl = false) { Key_(k, true, shift, ctrl); Key_(k, false, shift, ctrl); }

    private static void Type(string text)
    {
        foreach (char c in text)
        {
            Key key = c is >= 'a' and <= 'z' ? (Key)(c - 'a' + 'A') : c is >= 'A' and <= 'Z' ? (Key)c : c == ' ' ? Key.Space : Key.None;
            Input.ParseInputEvent(new InputEventKey { Keycode = key, PhysicalKeycode = key, Unicode = c, Pressed = true });
            Input.ParseInputEvent(new InputEventKey { Keycode = key, PhysicalKeycode = key, Unicode = c, Pressed = false });
        }
    }

    private static bool Is(Vector2 v, float x, float y, float eps = 0.01f) => T.Near(v.X, x, eps) && T.Near(v.Y, y, eps);

    private static bool IsRect(Rect2 r, float x, float y, float w, float h, float eps = 0.01f) => Is(r.Position, x, y, eps) && Is(r.Size, w, h, eps);

    /// <summary>Runs the draw pass and returns the recorded commands.</summary>
    private static List<CanvasItem.DrawCommand> Draw(CanvasItem c)
    {
        c.QueueRedraw();
        c.RunDraw();
        return c.Commands.ToList();
    }

    private static List<CanvasItem.DrawCommand> Texts(CanvasItem c) => Draw(c).Where(d => d.Type == CanvasItem.DrawCommand.Kind.Text).ToList();

    private static List<string> Log(params (Action<Action<string>> hook, string name)[] _) => new();

    private sealed class Sizeable : Texture2D
    {
        private readonly Vector2 _size;
        public Sizeable(float w, float h, ulong handle = 0) { _size = new Vector2(w, h); HostHandle = handle; }
        public override Vector2 GetSize() => _size;
    }

    // ---- fonts and metrics ----

    private static void FontsAndMetrics()
    {
        var file = new FontFile { ResourcePath = "Fonts/ui/Inter-Bold.ttf" };
        T.Check("a font file's family is its file name without the extension (the name the converter gives the family)", file.GetFontName() == "Inter-Bold" && Font.FamilyOfPath("res://a/b/c.otf") == "c" && Font.FamilyOfPath("") == "");
        T.Check("and 'bold' / 'italic' in the name set the style flags", file.GetFontStyleName() == "Bold" && new FontFile { ResourcePath = "x/Mono-Italic.ttf" }.GetFontStyleName() == "Italic");
        T.Check("the style flags are Godot's TextServer.FontStyle bits", file.GetFontStyle() == 1 && new FontFile { FontStyle = 3 }.GetFontStyle() == 3);
        var variation = new FontVariation { BaseFont = new FontFile { ResourcePath = "Fonts/Base.ttf" }, VariationEmbolden = 1.2f };
        T.Check("a FontVariation names its base family and asks for bold when emboldened", variation.GetFontName() == "Base" && variation.GetFontStyleName() == "Bold");
        var slanted = new FontVariation { BaseFont = new FontFile { ResourcePath = "Fonts/Base.ttf" }, VariationTransform = new Transform2D(new Vector2(1, 0), new Vector2(0.2f, 1), Vector2.Zero) };
        T.Check("an x skew asks for italic", slanted.GetFontStyleName() == "Italic");
        var system = new SystemFont { FontNames = new[] { "Arial", "Helvetica" }, FontWeight = 700 };
        T.Check("a SystemFont keeps its names, draws with the default family and is bold from weight 600", system.GetFontName() == "Arial" && system.GetFontStyleName() == "Bold");

        var font = new FontFile { ResourcePath = "Fonts/Mono.ttf" };
        T.Check("the fallback metrics: half an em per character, a line 1.2 em tall, the baseline at 0.8 of the line",
                T.Near(font.GetStringSize("hello", fontSize: 16).X, 40f) && T.Near(font.GetHeight(16), 19.2f) && T.Near(font.GetAscent(16), 15.36f) && T.Near(font.GetDescent(16), 3.84f));
        T.Check("it scales with the size", T.Near(font.GetStringSize("hello", fontSize: 32).X, 80f) && T.Near(font.GetHeight(32), 38.4f));
        T.Check("a multiline string is as wide as its widest line and as tall as its lines", Is(font.GetMultilineStringSize("ab\nabcd", fontSize: 16), 32, 38.4f));
        T.Check("a width wraps the multiline size at spaces: 'aaa bbb' fills 56 px, 'ccc' goes below", Is(font.GetMultilineStringSize("aaa bbb ccc", HorizontalAlignment.Left, 56, 16), 56, 38.4f), $"{font.GetMultilineStringSize("aaa bbb ccc", HorizontalAlignment.Left, 56, 16)}");
        T.Check("a character's size is one advance by the line height", Is(font.GetCharSize('A', 16), 8, 19.2f));
        T.Check("a tab is four advances", T.Near(UiText.FallbackWidth("\t", 16), 32f));

        UiText.WidthProvider = (family, px, flags, text) => text.Length * 10f + flags;
        T.Check("a host provider replaces the measuring (and gets the family and the style bits)", T.Near(new FontFile { ResourcePath = "a/B-Bold.ttf" }.GetStringSize("abc").X, 31f));
        UiText.ResetForTests();
        T.Check("and clearing it restores the fallback", T.Near(font.GetStringSize("abc").X, 24f));
    }

    // ---- line breaking ----

    private static void Lines()
    {
        Font? f = null;
        var l = TextLines.Build("hello world foo", f, 16, 88, TextLines.Wrap.Word);
        T.Check("words that fit stay on a line (88 px holds 'hello world')", l.Count == 2 && l.LineText(0) == "hello world" && l.LineText(1) == "foo", $"{l.Count}");
        T.Check("the second line starts at character 12 (the space that was dropped counts)", l.Items[1].Ord == 12 && l.Items[1].Start == 12);
        l = TextLines.Build("hello world foo", f, 16, 80, TextLines.Wrap.Word);
        T.Check("at 80 px the line breaks before 'world'", l.Count == 2 && l.LineText(0) == "hello" && l.LineText(1) == "world foo");
        T.Check("no width, no wrapping", TextLines.Build("hello world foo", f, 16, 0, TextLines.Wrap.Word).Count == 1);
        T.Check("wrap off never breaks", TextLines.Build("hello world foo", f, 16, 40, TextLines.Wrap.Off).Count == 1);
        l = TextLines.Build("abcdefghijkl", f, 16, 40, TextLines.Wrap.Word);
        T.Check("word mode lets a word too long for the line overflow", l.Count == 1 && l.Width(0) == 96);
        l = TextLines.Build("abcdefghijkl", f, 16, 40, TextLines.Wrap.Smart);
        T.Check("smart mode breaks it by characters: abcde / fghij / kl", l.Count == 3 && l.LineText(0) == "abcde" && l.LineText(1) == "fghij" && l.LineText(2) == "kl");
        l = TextLines.Build("ab cdefg hi", f, 16, 40, TextLines.Wrap.Arbitrary);
        T.Check("arbitrary mode fills each line to the width", l.Count >= 3 && l.Items.All(i => i.Width <= 40.01f), string.Join("|", Enumerable.Range(0, l.Count).Select(l.LineText)));
        l = TextLines.Build("a\nb\n\nc", f, 16, 0, TextLines.Wrap.Word);
        T.Check("a newline always breaks; an empty paragraph is an empty line", l.Count == 4 && l.LineText(2) == "" && l.Items[3].Ord == 2, string.Join("|", Enumerable.Range(0, l.Count).Select(l.LineText)));
        l = TextLines.Build("你好世界", f, 16, 16, TextLines.Wrap.Word);
        T.Check("CJK text may break between any two characters", l.Count == 2 && l.LineText(0) == "你好");
        l = TextLines.Build("one two", f, 16, 30, TextLines.Wrap.Word);
        T.Check("a line's width is measured without its trailing space", l.Count == 2 && T.Near(l.Width(0), 24f));
        T.Check("trim to a width with an ellipsis: room for the dots first", TextLines.Trim("hello world", f, 16, 56, false, "…") == "hello…");
        T.Check("trimming at a word keeps whole words", TextLines.Trim("hello world", f, 16, 70, true, "…") == "hello…", TextLines.Trim("hello world", f, 16, 70, true, "…"));
        T.Check("text that fits is untouched", TextLines.Trim("hi", f, 16, 56, false, "…") == "hi");
        T.Check("no ellipsis, just a cut", TextLines.Trim("hello world", f, 16, 40, false, "") == "hello");
    }

    // ---- Label ----

    private static void LabelBehaviour()
    {
        var label = new Label { Text = "Hello" };
        Root.AddChild(label);
        Settle();
        T.Check("a label is as big as its text: 5 characters, one line (nothing in the default style)", Is(label.GetCombinedMinimumSize(), 40, 19.2f), $"{label.GetCombinedMinimumSize()}");
        T.Check("it ignores the pointer, as Godot's does", label.MouseFilter == Control.MouseFilterEnum.Ignore);
        label.Text = "Hello World";
        Settle();
        T.Check("changing the text updates the minimum size", T.Near(label.GetCombinedMinimumSize().X, 88f));
        label.Text = "a\nbb\nccc";
        Settle();
        T.Check("newlines make lines: the widest and 3 x 19.2 + 2 x the line spacing of 3", Is(label.GetCombinedMinimumSize(), 24, 63.6f), $"{label.GetCombinedMinimumSize()}");
        T.Check("line and character counts", label.GetLineCount() == 3 && label.GetTotalCharacterCount() == 6 && label.GetLineHeight() == 20);
        label.Uppercase = true;
        label.Text = "abc";
        T.Check("uppercase draws capitals but keeps the text", label.Text == "abc" && Texts(label)[0].Text == "ABC");
        label.Uppercase = false;

        var wrap = new Label { Text = "aaa bbb ccc", AutowrapMode = TextServer.AutowrapMode.Word };
        Root.AddChild(wrap);
        wrap.Size = new Vector2(50, 10);
        Settle();
        Settle();
        T.Check("autowrap: a width of 50 holds six characters, so three lines", wrap.GetLineCount() == 3, $"{wrap.GetLineCount()}");
        T.Check("and the control grows to the wrapped height (3 lines and 2 gaps)", T.Near(wrap.GetCombinedMinimumSize().Y, 63.6f) && T.Near(wrap.Size.Y, 63.6f), $"{wrap.GetCombinedMinimumSize()} {wrap.Size}");
        T.Check("its minimum width is one pixel, so it can shrink", T.Near(wrap.GetCombinedMinimumSize().X, 1f));
        wrap.MaxLinesVisible = 2;
        Settle();
        T.Check("max_lines_visible draws two lines and sizes for two", wrap.GetVisibleLineCount() == 2 && T.Near(wrap.GetCombinedMinimumSize().Y, 41.4f), $"{wrap.GetCombinedMinimumSize()}");
        T.Check("the texts drawn are the first two lines", Texts(wrap).Select(t => t.Text).SequenceEqual(new[] { "aaa", "bbb" }));
        wrap.MaxLinesVisible = -1;
        wrap.LinesSkipped = 1;
        Settle();
        T.Check("lines_skipped starts at the second line", Texts(wrap).Select(t => t.Text).SequenceEqual(new[] { "bbb", "ccc" }));
        wrap.LinesSkipped = 0;

        var over = new Label { Text = "Hello World", TextOverrunBehavior = TextServer.OverrunBehavior.TrimEllipsis };
        Root.AddChild(over);
        over.Size = new Vector2(50, 20);
        Settle();
        T.Check("trim with ellipsis: 'Hello' and the dots fit 50 px, the control may shrink below its text", Texts(over)[0].Text == "Hello…" && over.GetCombinedMinimumSize().X <= 1.01f, Texts(over)[0].Text);
        over.TextOverrunBehavior = TextServer.OverrunBehavior.TrimChar;
        T.Check("trim char just cuts (and drops the space it ends on)", Texts(over)[0].Text == "Hello", Texts(over)[0].Text);
        over.TextOverrunBehavior = TextServer.OverrunBehavior.TrimWordEllipsis;
        T.Check("trim word with ellipsis keeps no partial word", Texts(over)[0].Text == "…", Texts(over)[0].Text);
        over.Size = new Vector2(100, 20);
        over.TextOverrunBehavior = TextServer.OverrunBehavior.TrimEllipsis;
        T.Check("text that fits is not trimmed", Texts(over)[0].Text == "Hello World");

        var vis = new Label { Text = "Hello World" };
        Root.AddChild(vis);
        vis.VisibleCharacters = 5;
        T.Check("visible_characters shows the first five", Texts(vis)[0].Text == "Hello" && T.Near(vis.VisibleRatio, 5f / 11f, 0.001f));
        vis.VisibleRatio = 0.5f;
        T.Check("visible_ratio 0.5 of 11 characters is 5", vis.VisibleCharacters == 5);
        vis.VisibleRatio = 1f;
        T.Check("ratio 1 shows all and resets the count to -1", vis.VisibleCharacters == -1 && Texts(vis)[0].Text == "Hello World");
        vis.VisibleCharacters = 0;
        T.Check("zero shows nothing", Texts(vis).Count == 0 || Texts(vis)[0].Text.Length == 0);
        vis.Text = "ab\ncd";
        vis.VisibleCharacters = 3;
        T.Check("newlines are not characters: three characters are 'ab' and 'c'", Texts(vis).Select(t => t.Text).SequenceEqual(new[] { "ab", "c" }), string.Join("|", Texts(vis).Select(t => t.Text)));

        var al = new Label { Text = "Hello" };
        Root.AddChild(al);
        al.Size = new Vector2(100, 40);
        al.HorizontalAlignment = HorizontalAlignment.Center;
        T.Check("centred in 100: (100 - 40) / 2 = 30", T.Near(Texts(al)[0].Rect.Position.X, 30f));
        al.HorizontalAlignment = HorizontalAlignment.Right;
        T.Check("right aligned: 60", T.Near(Texts(al)[0].Rect.Position.X, 60f));
        al.VerticalAlignment = VerticalAlignment.Center;
        T.Check("vertically centred in 40: (40 - 19.2) / 2", T.Near(Texts(al)[0].Rect.Position.Y, 10.4f), $"{Texts(al)[0].Rect.Position.Y}");
        al.VerticalAlignment = VerticalAlignment.Bottom;
        T.Check("bottom aligned", T.Near(Texts(al)[0].Rect.Position.Y, 20.8f));
        T.Check("the colour is the theme's label white", Texts(al)[0].Color == new Color(1, 1, 1, 1));
        al.AddThemeColorOverride("font_color", new Color(1, 0, 0, 1));
        T.Check("and a colour override wins", Texts(al)[0].Color == new Color(1, 0, 0, 1));
        al.AddThemeFontSizeOverride("font_size", 32);
        Settle();
        T.Check("a font size override doubles the width", T.Near(al.GetCombinedMinimumSize().X, 80f) && T.Near(Texts(al)[0].FontSize, 32f));
        al.AddThemeFontOverride("font", new FontFile { ResourcePath = "Fonts/Mono-Bold.ttf" });
        T.Check("a font override names its family and style in the command", Texts(al)[0].Family == "Mono-Bold" && Texts(al)[0].Flags == 1, $"{Texts(al)[0].Family} {Texts(al)[0].Flags}");
        T.Check("a clipped label draws inside a clip", Draw(new Label { Text = "x", ClipText = true }).Any(d => d.Type == CanvasItem.DrawCommand.Kind.PushClip));
    }

    private static void OutlineAndShadow()
    {
        var label = new Label { Text = "Fx" };
        Root.AddChild(label);
        T.Check("no outline or shadow by default: the command carries none", !Texts(label)[0].Fx.Any);
        label.AddThemeConstantOverride("outline_size", 3);
        label.AddThemeColorOverride("font_outline_color", new Color(0, 0, 1, 1));
        CanvasItem.TextFx fx = Texts(label)[0].Fx;
        T.Check("outline_size and font_outline_color arrive in the command", fx.OutlineSize == 3 && fx.OutlineColor == new Color(0, 0, 1, 1) && fx.Any);
        label.AddThemeColorOverride("font_shadow_color", new Color(0, 0, 0, 0.5f));
        label.AddThemeConstantOverride("shadow_offset_x", 2);
        label.AddThemeConstantOverride("shadow_offset_y", 3);
        label.AddThemeConstantOverride("shadow_outline_size", 4);
        fx = Texts(label)[0].Fx;
        T.Check("the shadow colour, offset and outline arrive too", fx.ShadowColor.A == 0.5f && fx.ShadowOffset == new Vector2(2, 3) && fx.ShadowOutlineSize == 4);
        var settings = new LabelSettings { OutlineSize = 2, OutlineColor = new Color(1, 1, 0, 1), ShadowSize = 1, ShadowColor = new Color(0, 0, 0, 1), ShadowOffset = new Vector2(5, 6), FontSize = 24, FontColor = new Color(0, 1, 0, 1), LineSpacing = 7 };
        var withSettings = new Label { Text = "S", LabelSettings = settings };
        Root.AddChild(withSettings);
        CanvasItem.DrawCommand cmd = Texts(withSettings)[0];
        T.Check("LabelSettings win over the theme: size, colour, outline, shadow", cmd.FontSize == 24 && cmd.Color == new Color(0, 1, 0, 1) && cmd.Fx.OutlineSize == 2 && cmd.Fx.ShadowOffset == new Vector2(5, 6) && cmd.Fx.ShadowOutlineSize == 1);
        withSettings.Text = "a\nb";
        Settle();
        T.Check("and the settings' line spacing separates the lines", T.Near(withSettings.GetCombinedMinimumSize().Y, 2 * 28.8f + 7f), $"{withSettings.GetCombinedMinimumSize()}");

        // the CPU geometry of the outline and shadow passes is aver::ui::uiTextFxPasses (UiTextEffectTest); here, the stroke of draw_string_outline
        var drawer = new OutlineDrawer();
        Root.AddChild(drawer);
        List<CanvasItem.DrawCommand> t = Texts(drawer);
        T.Check("draw_string_outline records an outlined string with a clear fill; draw_string the plain one over it", t.Count == 2 && t[0].Fx.OutlineSize == 2 && t[0].Color.A == 0f && !t[1].Fx.Any && t[1].Color.A == 1f);
        T.Check("draw_string puts the baseline at the position (the top is an ascent above: 0.8 of the 19.2 line)", T.Near(t[1].Rect.Position.Y, 40f - 15.36f), $"{t[1].Rect.Position.Y}");
        T.Check("and text is replayed through aver_ui_text_block_fx without a device (nothing drawn, no crash)", ReplayText(label));
    }

    private sealed class OutlineDrawer : Control
    {
        public override void _Draw()
        {
            DrawStringOutline(null, new Vector2(10, 40), "Out", HorizontalAlignment.Left, -1, 16, 2, new Color(0, 0, 0, 1));
            DrawString(null, new Vector2(10, 40), "Out", HorizontalAlignment.Left, -1, 16, new Color(1, 1, 1, 1));
        }
    }

    private static bool ReplayText(Control label)
    {
        UiNative.aver_ui_begin_frame(0, 0, 1280, 720);
        Gui.DrawAll();
        return UiNative.aver_ui_vertex_count() >= 0;
    }

    // ---- Range ----

    private static void RangeBehaviour()
    {
        var r = new Range { MinValue = 0, MaxValue = 10, Step = 1 };
        var seen = new List<double>();
        r.ValueChanged += v => seen.Add(v);
        r.Value = 3.4;
        T.Check("the value snaps to the step", r.Value == 3);
        r.Value = 3.4;
        T.Check("setting the same value again emits nothing", seen.Count == 1 && seen[0] == 3);
        r.Value = 99;
        T.Check("the maximum caps it", r.Value == 10);
        r.Value = -5;
        T.Check("the minimum floors it", r.Value == 0);
        r.SetValueNoSignal(7);
        T.Check("set_value_no_signal changes it without a signal", r.Value == 7 && seen.Count == 3);
        r.Page = 2;
        T.Check("a page shortens the top: the value cannot pass max - page", r.Value == 7 && r.GetAsRatio() > 0);
        r.Value = 10;
        T.Check("so 10 becomes 8", r.Value == 8);
        r.AllowGreater = true;
        r.Value = 12;
        T.Check("allow_greater lifts the cap", r.Value == 12);
        r.AllowLesser = true;
        r.Value = -3;
        T.Check("allow_lesser lifts the floor", r.Value == -3);
        r.MaxValue = 5;
        T.Check("changing the range re-validates the value and tells the world", r.Value == -3);

        var ratio = new Range { MinValue = 10, MaxValue = 20, Step = 0 };
        ratio.Value = 15;
        T.Check("get_as_ratio is the share of the range", T.Near((float)ratio.GetAsRatio(), 0.5f));
        ratio.SetAsRatio(0.25);
        T.Check("set_as_ratio sets from a share (no step: exact)", T.Near((float)ratio.Value, 12.5f));
        var snapped = new Range { MinValue = 0, MaxValue = 10, Step = 2 };
        snapped.SetAsRatio(0.33);
        T.Check("with a step it snaps to it: 0.33 of 10 is 3.3, the nearest step is 4", snapped.Value == 4, $"{snapped.Value}");
        var expo = new Range { MinValue = 1, MaxValue = 100, Step = 0.01, ExpEdit = true };
        expo.SetAsRatio(0.5);
        T.Check("exp_edit: half the ratio of 1..100 is 10", Math.Abs(expo.Value - 10) < 0.02, $"{expo.Value}");
        expo.Value = 100;
        T.Check("and the ratio of 100 is 1", T.Near((float)expo.GetAsRatio(), 1f));
        var rounded = new Range { MinValue = 0, MaxValue = 10, Step = 0.1, Rounded = true };
        rounded.Value = 4.6;
        T.Check("rounded takes whole numbers", rounded.Value == 5);
        var empty = new Range { MinValue = 5, MaxValue = 5 };
        T.Check("an empty range has the ratio 1", empty.GetAsRatio() == 1.0);
        var defaults = new Range();
        T.Check("Godot's defaults: 0..100, step 0.01", defaults.MinValue == 0 && defaults.MaxValue == 100 && defaults.Step == 0.01 && defaults.Page == 0 && defaults.Value == 0);
    }

    // ---- sliders, scroll bars, progress ----

    private static void SlidersAndBars()
    {
        var slider = new HSlider { MinValue = 0, MaxValue = 100, Step = 1 };
        Root.AddChild(slider);
        slider.Position = new Vector2(0, 0);
        slider.Size = new Vector2(116, 16);
        Settle();
        T.Check("a slider is a grabber high (16) and fills the width it is given", Is(slider.GetCombinedMinimumSize(), 0, 16) && slider.FocusMode == Control.FocusModeEnum.All);
        var log = new List<string>();
        slider.ValueChanged += v => log.Add("v" + v);
        slider.DragStarted += () => log.Add("start");
        slider.DragEnded += changed => log.Add("end:" + changed);
        Click(58, 8, true);
        T.Check("a press moves the grabber's centre to the pointer: (58 - 8) / (116 - 16) = half", slider.Value == 50 && log.SequenceEqual(new[] { "start", "v50" }), string.Join(",", log));
        Move(108, 8, 50, 0);
        T.Check("dragging follows the pointer", slider.Value == 100);
        Move(300, 400, 192, 392);
        T.Check("even far outside the control (it holds the pointer) and clamps at the ends", slider.Value == 100);
        Move(-50, 8, -350, -392);
        T.Check("and at the start", slider.Value == 0);
        Click(-50, 8, false);
        T.Check("release emits drag_ended with whether the value differs from the start of the drag (it ended where it began)", log[^1] == "end:False", string.Join(",", log));
        Click(30, 8, true);
        Move(80, 8, 50, 0);
        Click(80, 8, false);
        T.Check("and true when it does", log[^1] == "end:True" && slider.Value > 0, string.Join(",", log));
        slider.Value = 0;
        slider.Editable = false;
        Click(58, 8, true);
        Click(58, 8, false);
        T.Check("an uneditable slider ignores the pointer", slider.Value == 0);
        slider.Editable = true;

        slider.GrabFocus();
        Press(Key.Right);
        T.Check("the right key adds the step", slider.Value == 1);
        Press(Key.End);
        T.Check("End goes to the maximum", slider.Value == 100);
        Press(Key.Left);
        Press(Key.Left);
        T.Check("left steps back", slider.Value == 98);
        Press(Key.Home);
        T.Check("Home goes to the minimum", slider.Value == 0);
        Wheel(10, 8, MouseButton.WheelUp);
        T.Check("the wheel steps by the step", slider.Value == 1);
        slider.Scrollable = false;
        Wheel(10, 8, MouseButton.WheelUp);
        T.Check("unless scrollable is off", slider.Value == 1);

        var v = new VSlider { MinValue = 0, MaxValue = 100, Step = 1 };
        Root.AddChild(v);
        v.Position = new Vector2(300, 0);
        v.Size = new Vector2(16, 116);
        Settle();
        Tap(308, 8);
        T.Check("a vertical slider reads the maximum at the top", v.Value == 100);
        Tap(308, 108);
        T.Check("and the minimum at the bottom", v.Value == 0);
        v.GrabFocus();
        Press(Key.Up);
        T.Check("up adds the step", v.Value == 1);
        T.Check("it draws a track and a grabber", Draw(v).Count(d => d.Type == CanvasItem.DrawCommand.Kind.StyleBox) >= 2);

        // scroll bars
        var bar = new HScrollBar { MinValue = 0, MaxValue = 100, Page = 25 };
        Root.AddChild(bar);
        bar.Position = new Vector2(0, 200);
        bar.Size = new Vector2(100, 12);
        Settle();
        (float start, float len) = bar.Grabber();
        T.Check("the grabber is the page's share of the track: 25 of 100", T.Near(len, 25f) && T.Near(start, 0f), $"{start} {len}");
        bar.Value = 75;
        (start, len) = bar.Grabber();
        T.Check("at the end it sits flush: the value tops out at max - page", bar.Value == 75 && T.Near(start, 75f), $"{start}");
        T.Check("the bar is 12 high (scrollbar_size)", T.Near(bar.GetCombinedMinimumSize().Y, 12f));
        bar.Value = 0;
        Tap(60, 206);
        T.Check("a press on the track beyond the grabber pages by the page", T.Near((float)bar.Value, 25f), $"{bar.Value}");
        bar.Value = 0;
        Click(10, 206, true);
        Move(40, 206, 30, 0);
        T.Check("dragging the grabber (grabbed 10 px in) moves the value in proportion to the track: 30 px of 75", T.Near((float)bar.Value, 30f), $"{bar.Value}");
        Click(40, 206, false);

        // progress bars
        var progress = new ProgressBar { MinValue = 0, MaxValue = 200, Step = 1 };
        Root.AddChild(progress);
        progress.Size = new Vector2(100, 30);
        progress.Value = 50;
        Settle();
        T.Check("the text is the share of the way as a whole percent: 50 of 200 is 25%", progress.PercentText == "25%");
        T.Check("the fill is that share of the space inside the background (2 px margins): 96 x 25%", IsRect(progress.FillRect(), 2, 2, 24, 26), $"{progress.FillRect()}");
        progress.FillMode = ProgressBar.FillModeEnum.EndToBegin;
        T.Check("end to begin fills from the right", IsRect(progress.FillRect(), 74, 2, 24, 26));
        progress.FillMode = ProgressBar.FillModeEnum.BottomToTop;
        T.Check("bottom to top fills from the bottom", IsRect(progress.FillRect(), 2, 21.5f, 96, 6.5f), $"{progress.FillRect()}");
        progress.FillMode = ProgressBar.FillModeEnum.BeginToEnd;
        T.Check("it is as tall as a line of text plus the background's margins", T.Near(progress.GetCombinedMinimumSize().Y, 4f + 19.2f), $"{progress.GetCombinedMinimumSize()}");
        T.Check("it draws the background, the fill and the percentage", Texts(progress).Count == 1 && Texts(progress)[0].Text == "25%");
        progress.ShowPercentage = false;
        T.Check("without the percentage there is no text", Texts(progress).Count == 0 && progress.GetCombinedMinimumSize().X >= 1f);
        progress.Value = 200;
        T.Check("full is 100%", progress.PercentText == "100%");

        // texture progress
        var tex = new TextureProgressBar { MinValue = 0, MaxValue = 100, Value = 25, TextureProgress = new Sizeable(100, 20, 7) };
        T.Check("left to right reveals the left quarter", IsRect(tex.GetFillRegion(), 0, 0, 0.25f, 1));
        tex.FillMode = TextureProgressBar.FillModeEnum.RightToLeft;
        T.Check("right to left, the right quarter", IsRect(tex.GetFillRegion(), 0.75f, 0, 0.25f, 1));
        tex.FillMode = TextureProgressBar.FillModeEnum.TopToBottom;
        T.Check("top to bottom, the top quarter", IsRect(tex.GetFillRegion(), 0, 0, 1, 0.25f));
        tex.FillMode = TextureProgressBar.FillModeEnum.BottomToTop;
        T.Check("bottom to top, the bottom quarter", IsRect(tex.GetFillRegion(), 0, 0.75f, 1, 0.25f));
        tex.FillMode = TextureProgressBar.FillModeEnum.BilinearLeftAndRight;
        T.Check("bilinear grows from the middle", IsRect(tex.GetFillRegion(), 0.375f, 0, 0.25f, 1));
        tex.FillMode = TextureProgressBar.FillModeEnum.Clockwise;
        tex.RadialInitialAngle = 90;
        tex.RadialFillDegrees = 180;
        (float a0, float sweep) = tex.GetRadialAngles();
        T.Check("a radial fill reports its angles: from 90 degrees, a quarter of 180 is 45", T.Near(a0, 90f) && T.Near(sweep, 45f));
        tex.FillMode = TextureProgressBar.FillModeEnum.CounterClockwise;
        (a0, sweep) = tex.GetRadialAngles();
        T.Check("counter-clockwise starts the sweep behind the initial angle", T.Near(a0, 45f) && T.Near(sweep, 45f));
        tex.FillMode = TextureProgressBar.FillModeEnum.LeftToRight;
        Root.AddChild(tex);
        List<CanvasItem.DrawCommand> cmds = Draw(tex);
        T.Check("it draws the progress texture's revealed part with its engine handle", cmds.Count == 1 && cmds[0].Type == CanvasItem.DrawCommand.Kind.Texture && cmds[0].Handle == 7 && T.Near(cmds[0].Rect.Size.X, 25f) && T.Near(cmds[0].Uv.Size.X, 0.25f), $"{cmds.Count}");
    }

    private static void SpinBoxBehaviour()
    {
        var spin = new SpinBox { MinValue = 0, MaxValue = 10, Step = 1, Value = 3 };
        Root.AddChild(spin);
        spin.Size = new Vector2(100, 30);
        Settle();
        var seen = new List<double>();
        spin.ValueChanged += v => seen.Add(v);
        T.Check("the field shows the value at the step's precision", spin.GetLineEdit().Text == "3");
        spin.Step = 0.25;
        T.Check("a step of 0.25 shows two decimals", spin.Value == 3 && spin.GetLineEdit().Text == "3.00");
        spin.Step = 1;
        spin.GetLineEdit().Text = "7";
        spin.GetLineEdit().EmitSignal(LineEdit.SignalName.TextSubmitted, "7");
        T.Check("Enter in the field sets the value", spin.Value == 7 && seen.Contains(7));
        spin.GetLineEdit().Text = "oops";
        spin.Apply();
        T.Check("text that is not a number is put back", spin.GetLineEdit().Text == "7" && spin.Value == 7);
        spin.GetLineEdit().Text = "99";
        spin.Apply();
        T.Check("a number past the range is capped", spin.Value == 10);
        Tap(95, 5);
        T.Check("the up arrow (right edge, top half) adds the step but not past the maximum", spin.Value == 10);
        Tap(95, 25);
        T.Check("the down arrow subtracts it", spin.Value == 9);
        spin.CustomArrowStep = 5;
        Tap(95, 25);
        T.Check("custom_arrow_step replaces the step for the arrows", spin.Value == 4);
        spin.Prefix = "$";
        spin.Suffix = "kg";
        T.Check("prefix and suffix surround the number", spin.GetLineEdit().Text == "$ 4 kg");
        spin.GetLineEdit().Text = "$ 6 kg";
        spin.Apply();
        T.Check("and are stripped when it is read", spin.Value == 6);
        T.Check("it is wider than the field by the arrows and as tall as the field", T.Near(spin.GetCombinedMinimumSize().X, spin.GetLineEdit().GetCombinedMinimumSize().X + 16f));
        bool wasEditable = spin.Editable;
        spin.Editable = false;
        T.Check("the editable flag is the field's", wasEditable && !spin.GetLineEdit().Editable);
    }

    // ---- buttons ----

    private static Button MakeButton(string text = "OK", float x = 10, float y = 10, float w = 80, float h = 30)
    {
        var b = new Button { Text = text };
        Root.AddChild(b);
        b.Position = new Vector2(x, y);
        b.Size = new Vector2(w, h);
        Settle();
        return b;
    }

    private static List<string> Watch(BaseButton b, string name = "")
    {
        var log = new List<string>();
        string p = name.Length > 0 ? name + ":" : "";
        b.ButtonDown += () => log.Add(p + "down");
        b.ButtonUp += () => log.Add(p + "up");
        b.Pressed += () => log.Add(p + "pressed");
        b.Toggled += on => log.Add(p + "toggled:" + on);
        return log;
    }

    private static void ButtonBehaviour()
    {
        Button b = MakeButton();
        T.Check("a button is its text and the style's margins: 2 x 8 + 16 wide, 19.2 + 8 high", Is(b.GetCombinedMinimumSize(), 32, 27.2f), $"{b.GetCombinedMinimumSize()}");
        List<string> log = Watch(b);
        Tap(20, 20);
        T.Check("a click is button_down, pressed, button_up - pressed on the release", log.SequenceEqual(new[] { "down", "pressed", "up" }), string.Join(",", log));
        log.Clear();
        Click(20, 20, true);
        Click(300, 300, false);
        T.Check("released outside: no pressed", log.SequenceEqual(new[] { "down", "up" }), string.Join(",", log));
        log.Clear();
        b.ActionMode = BaseButton.ActionModeEnum.ButtonPress;
        Click(20, 20, true);
        T.Check("action_mode press: pressed at once, on the press", log.SequenceEqual(new[] { "down", "pressed" }), string.Join(",", log));
        Click(20, 20, false);
        T.Check("and the release adds only the up", log[^1] == "up" && log.Count(s => s == "pressed") == 1);
        b.ActionMode = BaseButton.ActionModeEnum.ButtonRelease;
        log.Clear();
        b.Disabled = true;
        Tap(20, 20);
        T.Check("a disabled button does nothing", log.Count == 0 && b.GetDrawMode() == BaseButton.DrawMode.Disabled);
        b.Disabled = false;

        log.Clear();
        Click(20, 20, true, MouseButton.Right);
        Click(20, 20, false, MouseButton.Right);
        T.Check("only the left button by default (button_mask)", log.Count == 0);
        b.ButtonMask = MouseButtonMask.Left | MouseButtonMask.Right;
        Tap(20, 20, MouseButton.Right);
        T.Check("button_mask adds the right one", log.Contains("pressed"));
        b.ButtonMask = MouseButtonMask.Left;

        Move(20, 20);
        T.Check("hovering changes the draw mode", b.GetDrawMode() == BaseButton.DrawMode.Hover && b.IsHovered());
        Click(20, 20, true);
        T.Check("and holding it pressed", b.GetDrawMode() == BaseButton.DrawMode.Pressed);
        Move(500, 500, 480, 480);
        T.Check("moving off while held shows the normal look", b.GetDrawMode() == BaseButton.DrawMode.Normal);
        b.KeepPressedOutside = true;
        T.Check("unless keep_pressed_outside", b.GetDrawMode() == BaseButton.DrawMode.Pressed);
        Click(500, 500, false);
        b.KeepPressedOutside = false;

        // keyboard
        log.Clear();
        b.GrabFocus();
        Key_(Key.Enter, true);
        T.Check("accept (Enter) presses like the mouse: button_down on the key down", log.SequenceEqual(new[] { "down" }), string.Join(",", log));
        Key_(Key.Enter, false);
        T.Check("pressed and button_up on the release", log.SequenceEqual(new[] { "down", "pressed", "up" }), string.Join(",", log));
        log.Clear();
        Press(Key.Space);
        T.Check("so does Space", log.Contains("pressed"));

        // toggle
        var t = MakeButton("T", 200, 10);
        t.ToggleMode = true;
        List<string> tl = Watch(t);
        Tap(210, 20);
        T.Check("a toggle button: toggled(true) then pressed", tl.SequenceEqual(new[] { "down", "toggled:True", "pressed", "up" }) && t.ButtonPressed, string.Join(",", tl));
        T.Check("it draws pressed", t.GetDrawMode() == BaseButton.DrawMode.Pressed || t.GetDrawMode() == BaseButton.DrawMode.HoverPressed);
        tl.Clear();
        Tap(210, 20);
        T.Check("and back", tl.Contains("toggled:False") && !t.ButtonPressed);
        tl.Clear();
        t.ButtonPressed = true;
        T.Check("setting button_pressed from code emits toggled but not pressed", tl.SequenceEqual(new[] { "toggled:True" }), string.Join(",", tl));
        t.SetPressedNoSignal(false);
        T.Check("set_pressed_no_signal emits nothing", tl.Count == 1 && !t.ButtonPressed);
        b.ButtonPressed = true;
        T.Check("button_pressed does nothing on a plain button", !b.ButtonPressed);

        // groups
        var group = new ButtonGroup();
        var ga = MakeButton("A", 400, 10);
        var gb = MakeButton("B", 500, 10);
        ga.ToggleMode = gb.ToggleMode = true;
        ga.ButtonGroup = gb.ButtonGroup = group;
        var groupLog = new List<string>();
        ga.Toggled += on => groupLog.Add("a:" + on);
        gb.Toggled += on => groupLog.Add("b:" + on);
        group.Pressed += p => groupLog.Add("group:" + ((Button)p).Text);
        Tap(410, 20);
        T.Check("in a group the pressed button reports to the group", ga.ButtonPressed && groupLog.SequenceEqual(new[] { "group:A", "a:True" }), string.Join(",", groupLog));
        groupLog.Clear();
        Tap(510, 20);
        T.Check("pressing another one releases the first (its toggled first)", !ga.ButtonPressed && gb.ButtonPressed && groupLog.SequenceEqual(new[] { "a:False", "group:B", "b:True" }), string.Join(",", groupLog));
        T.Check("the group knows the pressed button and its members", group.GetPressedButton() == gb && group.GetButtons().Count == 2);
        groupLog.Clear();
        Tap(510, 20);
        T.Check("pressing the pressed one again keeps it pressed (allow_unpress is off) and still reports", gb.ButtonPressed && groupLog.Contains("b:True"), string.Join(",", groupLog));
        group.AllowUnpress = true;
        Tap(510, 20);
        T.Check("with allow_unpress it lets go", !gb.ButtonPressed && group.GetPressedButton() == null);

        // shortcut
        var sc = MakeButton("S", 600, 10);
        List<string> sl = Watch(sc);
        sc.Shortcut = new Shortcut { Events = new Collections.Array { (Variant)new InputEventKey { Keycode = Key.F5 } } };
        Press(Key.F5);
        T.Check("a shortcut presses the button when its key is pressed", sl.Contains("pressed"), string.Join(",", sl));
        sl.Clear();
        sc.Disabled = true;
        Press(Key.F5);
        T.Check("not while disabled", sl.Count == 0);
        T.Check("the shortcut matches by key", sc.Shortcut.MatchesEvent(new InputEventKey { Keycode = Key.F5 }) && !sc.Shortcut.MatchesEvent(new InputEventKey { Keycode = Key.F6 }) && sc.Shortcut.GetAsText().Length > 0);

        // virtuals
        var scripted = new ScriptedButton { Text = "x" };
        Root.AddChild(scripted);
        scripted.Position = new Vector2(700, 10);
        scripted.Size = new Vector2(50, 30);
        Settle();
        Tap(710, 20);
        T.Check("_pressed and _toggled run for a script's button", scripted.Presses == 1);
    }

    private sealed class ScriptedButton : Button
    {
        public int Presses;
        public override void _Pressed() => Presses++;
    }

    private static void ButtonKinds()
    {
        var cb = new CheckBox { Text = "Check" };
        Root.AddChild(cb);
        Settle();
        T.Check("a check box is the text, a 16 px box, the 4 px gap and the margins: 40 + 16 + 4 + 16 = 76", Is(cb.GetCombinedMinimumSize(), 76, 27.2f), $"{cb.GetCombinedMinimumSize()}");
        T.Check("it is a toggle left-aligned", cb.ToggleMode && cb.Alignment == HorizontalAlignment.Left);
        cb.Position = new Vector2(10, 10);
        cb.Size = new Vector2(100, 30);
        List<string> log = Watch(cb);
        Tap(20, 20);
        T.Check("it toggles on a click", cb.ButtonPressed && log.Contains("toggled:True"));
        List<CanvasItem.DrawCommand> cmds = Draw(cb);
        T.Check("the box and the mark are drawn before the text", cmds.Count(c => c.Type == CanvasItem.DrawCommand.Kind.StyleBox) >= 3 && Texts(cb)[0].Text == "Check");
        T.Check("the text starts after the box and the gap: 8 margin + 16 + 4", T.Near(Texts(cb)[0].Rect.Position.X, 28f), $"{Texts(cb)[0].Rect.Position.X}");
        var radioGroup = new ButtonGroup();
        var r1 = new CheckBox { Text = "One", ButtonGroup = radioGroup };
        var r2 = new CheckBox { Text = "Two", ButtonGroup = radioGroup };
        r1.ButtonPressed = true;
        r2.ButtonPressed = true;
        T.Check("check boxes in a group are radio buttons: one at a time", !r1.ButtonPressed && r2.ButtonPressed);

        var cbtn = new CheckButton { Text = "Switch" };
        Root.AddChild(cbtn);
        cbtn.Position = new Vector2(10, 100);
        cbtn.Size = new Vector2(200, 30);
        Settle();
        T.Check("a check button is the text, the 4 px gap, a 38 px switch and the margins: 48 + 4 + 38 + 16", Is(cbtn.GetCombinedMinimumSize(), 106, 27.2f) && cbtn.ToggleMode, $"{cbtn.GetCombinedMinimumSize()}");
        cbtn.ButtonPressed = true;
        T.Check("pressed or not, it draws", Draw(cbtn).Count > 3);

        var link = new LinkButton { Text = "link" };
        Root.AddChild(link);
        Settle();
        T.Check("a link is just its text", Is(link.GetCombinedMinimumSize(), 32, 19.2f));
        List<CanvasItem.DrawCommand> lc = Draw(link);
        T.Check("it is underlined (a rectangle under the text)", lc.Count(c => c.Type == CanvasItem.DrawCommand.Kind.Rect) == 1);
        link.Underline = LinkButton.UnderlineModeEnum.Never;
        T.Check("never underlined on request", Draw(link).Count(c => c.Type == CanvasItem.DrawCommand.Kind.Rect) == 0);
        T.Check("and the hand cursor", link.MouseDefaultCursorShape == Control.CursorShape.PointingHand);

        var tb = new TextureButton { TextureNormal = new Sizeable(32, 24, 5), TexturePressed = new Sizeable(32, 24, 6), StretchMode = TextureButton.StretchModeEnum.KeepAspectCentered };
        Root.AddChild(tb);
        Settle();
        T.Check("a texture button is its texture's size", Is(tb.GetCombinedMinimumSize(), 32, 24));
        tb.IgnoreTextureSize = true;
        T.Check("or nothing when the size is ignored", Is(tb.GetCombinedMinimumSize(), 0, 0));
        tb.Position = new Vector2(10, 200);
        tb.Size = new Vector2(100, 24);
        T.Check("keep aspect centred in 100 x 24 is the texture's own 32 x 24, centred", IsRect(tb.TextureRect(new Vector2(32, 24)), 34, 0, 32, 24), $"{tb.TextureRect(new Vector2(32, 24))}");
        tb.StretchMode = TextureButton.StretchModeEnum.Scale;
        T.Check("scale fills the control", IsRect(tb.TextureRect(new Vector2(32, 24)), 0, 0, 100, 24));
        List<CanvasItem.DrawCommand> td = Draw(tb);
        T.Check("it draws the normal texture's handle", td.Count == 1 && td[0].Handle == 5);
        Click(20, 210, true);
        td = Draw(tb);
        T.Check("and the pressed one while it is held", td.Count == 1 && td[0].Handle == 6, $"{td.Count}");
        Click(20, 210, false);

        var rect = new ColorRect { Color = new Color(0.2f, 0.4f, 0.6f, 1) };
        Root.AddChild(rect);
        rect.Size = new Vector2(30, 20);
        List<CanvasItem.DrawCommand> rc = Draw(rect);
        T.Check("a colour rectangle fills its rectangle with its colour", rc.Count == 1 && rc[0].Color == new Color(0.2f, 0.4f, 0.6f, 1) && Is(rc[0].Rect.Size, 30, 20));

        var tr = new TextureRect { Texture = new Sizeable(40, 20, 9), StretchMode = TextureRect.StretchModeEnum.KeepAspectCovered };
        Root.AddChild(tr);
        Settle();
        T.Check("a texture rect is its texture's size and passes the pointer on", Is(tr.GetCombinedMinimumSize(), 40, 20) && tr.MouseFilter == Control.MouseFilterEnum.Pass);
        tr.ExpandMode = TextureRect.ExpandModeEnum.IgnoreSize;
        T.Check("ignore size makes it free", Is(tr.GetCombinedMinimumSize(), 0, 0));
        tr.Size = new Vector2(100, 100);
        T.Check("keep aspect covered scales the texture to cover: 200 x 100, centred", IsRect(tr.DrawRectFor(new Vector2(40, 20)), -50, 0, 200, 100), $"{tr.DrawRectFor(new Vector2(40, 20))}");
        tr.StretchMode = TextureRect.StretchModeEnum.KeepAspect;
        T.Check("keep aspect fits at the corner: 100 x 50", IsRect(tr.DrawRectFor(new Vector2(40, 20)), 0, 0, 100, 50));
        tr.StretchMode = TextureRect.StretchModeEnum.KeepCentered;
        T.Check("keep centred leaves the size", IsRect(tr.DrawRectFor(new Vector2(40, 20)), 30, 40, 40, 20));
        tr.StretchMode = TextureRect.StretchModeEnum.Tile;
        List<CanvasItem.DrawCommand> tc = Draw(tr);
        T.Check("tile asks for the texture repeated: 2.5 x 5 times", tc.Count == 1 && T.Near(tc[0].Uv.Size.X, 2.5f) && T.Near(tc[0].Uv.Size.Y, 5f), $"{tc.Count}");
        T.Check("a texture with no handle records a command that draws nothing (host rule of U1)", Draw(new TextureRect { Texture = new Sizeable(8, 8, 0) }).All(c => c.Handle == 0));
        var nine = new NinePatchRect { Texture = new Sizeable(30, 30, 3), PatchMarginLeft = 5, PatchMarginTop = 5, PatchMarginRight = 5, PatchMarginBottom = 5 };
        Root.AddChild(nine);
        nine.Size = new Vector2(100, 60);
        T.Check("a nine-patch rect draws through the engine's 9-patch box", Draw(nine).Count(c => c.Type == CanvasItem.DrawCommand.Kind.StyleBox) == 1 && nine.AsStyleBox().TextureMarginLeft == 5);
        T.Check("it ignores the pointer", nine.MouseFilter == Control.MouseFilterEnum.Ignore);
    }
}
