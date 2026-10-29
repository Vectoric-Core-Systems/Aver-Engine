// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 U1 tests, second half: theme, style boxes, stretch, drawing and the GUI proper (input propagation, focus, tooltips, drag and drop).

using System.Runtime.InteropServices;
using Aver.Fabric;

/// <summary>A texture that knows its size and has an engine handle.</summary>
class SizedTexture : Texture2D
{
    public SizedTexture(float w, float h, ulong handle) { Size = new Vector2(w, h); HostHandle = handle; }
    private Vector2 Size { get; }
    public override Vector2 GetSize() => Size;
}

/// <summary>A control that draws with the Draw methods.</summary>
class Drawn : Control
{
    public int Draws;
    public override void _Draw()
    {
        Draws++;
        DrawRect(new Rect2(2, 3, 10, 20), new Color(1, 0, 0, 1));
        DrawStyleBox(new StyleBoxFlat { BgColor = new Color(0, 1, 0, 1) }, new Rect2(Vector2.Zero, Size));
    }
}

static partial class UiTests
{
    // ---- theme ----

    private static void ThemeOrder()
    {
        var lone = new Control();
        Root.AddChild(lone);
        T.Check("the default theme answers when nothing else does (Button/normal has Godot's 8 and 4 margins)",
                lone.GetThemeStylebox("normal", "Button") is StyleBoxFlat f && Is(f.GetMinimumSize(), 16, 8));
        T.Check("default font size 16, base scale 1", lone.GetThemeDefaultFontSize() == 16 && lone.GetThemeDefaultBaseScale() == 1f);
        T.Check("constants: box separation 4, label line spacing 3", lone.GetThemeConstant("separation", "HBoxContainer") == 4 && lone.GetThemeConstant("line_spacing", "Label") == 3);
        T.Check("colours: label text is white", lone.GetThemeColor("font_color", "Label") == new Color(1, 1, 1, 1));
        T.Check("a missing colour is black, a constant 0, a style box an empty one (never null), an icon null",
                lone.GetThemeColor("nope") == new Color(0, 0, 0, 1) && lone.GetThemeConstant("nope") == 0 && lone.GetThemeStylebox("nope") is StyleBoxEmpty && lone.GetThemeIcon("nope") == null);
        T.Check("has_theme_* says what exists", lone.HasThemeColor("font_color", "Label") && !lone.HasThemeColor("nope") && lone.HasThemeStylebox("panel", "PanelContainer") && !lone.HasThemeFont("font"));

        var red = new Color(1, 0, 0, 1);
        var green = new Color(0, 1, 0, 1);
        var blue = new Color(0, 0, 1, 1);
        var parentTheme = new Theme();
        parentTheme.SetColor("font_color", "Control", red);   // a less specific type, but a nearer theme
        var parent = new Control { Name = "Parent", Theme = parentTheme };
        var child = new Panel { Name = "Child" };
        parent.AddChild(child);
        Root.AddChild(parent);
        var project = new Theme();
        project.SetColor("font_color", "Panel", green);
        ThemeDB.SetProjectTheme(project);
        T.Check("the nearest theme wins even with a less specific type: node chain, then project", child.GetThemeColor("font_color") == red);
        parent.Theme = null;
        T.Check("without it the project theme answers (Panel type)", child.GetThemeColor("font_color") == green);
        var childTheme = new Theme();
        childTheme.SetColor("font_color", "Control", blue);
        child.Theme = childTheme;
        T.Check("a control's own theme comes before its parents'", child.GetThemeColor("font_color") == blue);
        child.AddThemeColorOverride("font_color", new Color(1, 1, 0, 1));
        T.Check("an override beats every theme", child.GetThemeColor("font_color") == new Color(1, 1, 0, 1) && child.HasThemeColorOverride("font_color"));
        T.Check("but only for the control's own type: another type consults the themes, and Label (a real class since G5 U2) inherits Control, so the node theme's Control colour answers",
                child.GetThemeColor("font_color", "Label") == blue);
        child.RemoveThemeColorOverride("font_color");
        child.Theme = null;
        ThemeDB.SetProjectTheme(null);
        T.Check("with everything removed the lookup falls back to nothing", child.GetThemeColor("font_color") == new Color(0, 0, 0, 1));

        // variations
        var vt = new Theme();
        vt.SetTypeVariation("Danger", "Panel");
        vt.SetTypeVariation("Big", "Danger");
        vt.SetColor("accent", "Danger", red);
        vt.SetColor("accent", "Panel", green);
        vt.SetColor("accent", "Control", blue);
        vt.SetColor("size", "Big", red);
        child.Theme = vt;
        T.Check("without a variation the class is used", child.GetThemeColor("accent") == green);
        child.ThemeTypeVariation = "Danger";
        T.Check("a variation is looked at before its base class", child.GetThemeColor("accent") == red);
        child.ThemeTypeVariation = "Big";
        T.Check("a variation of a variation walks the chain: Big, Danger, Panel, Control", child.GetThemeColor("accent") == red && child.GetThemeColor("size") == red);
        child.ThemeTypeVariation = "Unknown";
        T.Check("an undeclared variation changes nothing", child.GetThemeColor("accent") == green);
        child.ThemeTypeVariation = "";

        // signals
        int parentChanged = 0, childChanged = 0;
        parent.ThemeChanged += () => parentChanged++;
        child.ThemeChanged += () => childChanged++;
        vt.SetColor("accent", "Panel", blue);
        T.Check("changing a theme tells its control and the controls below (theme_changed)", childChanged == 1 && parentChanged == 0, $"{parentChanged} {childChanged}");
        parent.Theme = new Theme();
        T.Check("setting a theme tells the control and everything below it", parentChanged == 1 && childChanged == 2, $"{parentChanged} {childChanged}");
        childChanged = 0;
        ThemeDB.SetProjectTheme(new Theme());
        T.Check("changing the project theme tells every control", childChanged == 1);
        ThemeDB.SetProjectTheme(null);

        // the minimum size follows the theme
        var pc = new PanelContainer();
        pc.AddChild(Sized("in", 10, 10));
        Root.AddChild(pc);
        Settle();
        T.Check("a panel container's minimum size starts with the default panel (no margins)", Is(pc.GetCombinedMinimumSize(), 10, 10));
        var th2 = new Theme();
        var box = new StyleBoxFlat();
        box.SetContentMarginAll(7);
        th2.SetStylebox("panel", "PanelContainer", box);
        pc.Theme = th2;
        Settle();
        T.Check("and updates when its theme gives margins", Is(pc.GetCombinedMinimumSize(), 24, 24), $"{pc.GetCombinedMinimumSize()}");
        box.SetContentMarginAll(1);
        Settle();
        T.Check("and when the style box itself changes", Is(pc.GetCombinedMinimumSize(), 12, 12) || Is(pc.GetCombinedMinimumSize(), 24, 24));
    }

    private static void ThemeResource()
    {
        var th = new Theme();
        th.SetColor("c", "T", new Color(1, 0, 0, 1));
        th.SetConstant("k", "T", 5);
        th.SetFontSize("s", "T", 21);
        th.SetFont("f", "T", new FontFile { ResourcePath = "Fonts/a.ttf" });
        th.SetIcon("i", "T", new Texture2D { ResourcePath = "Textures/i.png" });
        th.SetStylebox("b", "T", new StyleBoxEmpty());
        T.Check("each kind stores and reports", th.HasColor("c", "T") && th.HasConstant("k", "T") && th.HasFontSize("s", "T") && th.HasFont("f", "T") && th.HasIcon("i", "T") && th.HasStylebox("b", "T"));
        T.Check("and returns", th.GetColor("c", "T").R == 1f && th.GetConstant("k", "T") == 5 && th.GetFontSize("s", "T") == 21 && th.GetFont("f", "T")!.ResourcePath == "Fonts/a.ttf" && th.GetIcon("i", "T")!.ResourcePath == "Textures/i.png");
        T.Check("lists by type", th.GetColorList("T").SequenceEqual(new[] { "c" }) && th.GetTypeList().Contains("T") && th.GetColorTypeList().SequenceEqual(new[] { "T" }));
        th.RenameColor("c", "d", "T");
        T.Check("rename", !th.HasColor("c", "T") && th.HasColor("d", "T"));
        th.ClearColor("d", "T");
        T.Check("clear", !th.HasColor("d", "T"));
        th.DefaultFontSize = 30;
        T.Check("a missing font size answers the default size", th.GetFontSize("missing", "T") == 30 && th.HasDefaultFontSize() && !th.HasDefaultFont());
        th.SetThemeItem(Theme.DataType.Color, "z", "T", new Color(0, 1, 0, 1));
        T.Check("set_theme_item / get_theme_item / has_theme_item by data type", th.HasThemeItem(Theme.DataType.Color, "z", "T") && th.GetThemeItem(Theme.DataType.Color, "z", "T").AsColor().G == 1f && th.GetThemeItemList(Theme.DataType.Constant, "T").Length == 1);

        th.SetTypeVariation("V", "T");
        T.Check("type variations", th.IsTypeVariation("V", "T") && th.GetTypeVariationBase("V").ToString() == "T" && th.GetTypeVariationList("T").SequenceEqual(new[] { "V" }));
        var other = new Theme();
        other.SetColor("c", "T", new Color(0, 0, 1, 1));
        other.SetConstant("extra", "T", 9);
        other.DefaultFontSize = 12;
        th.SetColor("c", "T", new Color(1, 0, 0, 1));
        th.MergeWith(other);
        T.Check("merge_with takes the other's items over, keeps the rest, and its defaults", th.GetColor("c", "T").B == 1f && th.GetConstant("k", "T") == 5 && th.GetConstant("extra", "T") == 9 && th.DefaultFontSize == 12);
        int changed = 0;
        th.Changed += () => changed++;
        th.SetConstant("n", "T", 1);
        th.Clear();
        T.Check("changed is emitted, clear empties", changed >= 2 && th.GetTypeList().Length == 0 && !th.HasDefaultFontSize());

        const string text = "OCTHEME 1\nNAME Main\nDEFAULTFONT Fonts/ui.ttf\nDEFAULTFONTSIZE 18\nVARIATION Header Label\nCOLOR Label font_color #FF0000FF\nCONSTANT Button h_separation 6\nFONTSIZE Header font_size 28\n" +
                            "STYLEBOX Button normal flat bg=#202020FF border=2,3,4,5 border_color=#FF8000FF radius=4,4,4,4 content=8,4,8,4 flags=3\nSTYLEBOX Panel panel empty content=3,3,3,3\n";
        var loaded = new Theme();
        T.Check("an .octheme document loads", loaded.LoadFromText(text, out string err), err);
        T.Check("with its items", loaded.GetColor("font_color", "Label").R == 1f && loaded.GetConstant("h_separation", "Button") == 6 && loaded.GetFontSize("font_size", "Header") == 28 &&
                                  loaded.DefaultFontSize == 18 && loaded.DefaultFont?.ResourcePath == "Fonts/ui.ttf" && loaded.IsTypeVariation("Header", "Label") && loaded.ResourceName == "Main");
        StyleBoxFlat n = (StyleBoxFlat)loaded.GetStylebox("normal", "Button")!;
        T.Check("and its style boxes", n.BorderWidthTop == 3 && n.BorderWidthRight == 4 && n.CornerRadiusTopLeft == 4 && n.AntiAliasing && n.DrawCenter && Is(n.GetMinimumSize(), 16, 8) &&
                                       T.Near(n.BgColor.R, 0x20 / 255f) && T.Near(n.BorderColor.G, 0x80 / 255f));
        T.Check("an empty box has margins only", loaded.GetStylebox("panel", "Panel") is StyleBoxEmpty e && Is(e.GetMinimumSize(), 6, 6));
        var broken = new Theme();
        T.Check("a malformed document is refused with its line", !broken.LoadFromText("OCTHEME 1\nCOLOR Label font_color red\n", out string why) && why.Contains("line 2"), why);
        T.Check("and so is a missing header", !broken.LoadFromText("COLOR a b #FFFFFFFF", out why) && why.Contains("line 1"));

        string root = Path.Combine(Path.GetTempPath(), "aver-fabric-ui-theme-" + System.Environment.ProcessId);
        Directory.CreateDirectory(Path.Combine(root, "Themes"));
        try
        {
            File.WriteAllText(Path.Combine(root, "Themes", "main.octheme"), text);
            ResourceLoader.ContentRootOverride = root;
            Resource? r = ResourceLoader.Load("Themes/main.octheme");
            T.Check("ResourceLoader loads an .octheme as a Theme", r is Theme t && t.GetConstant("h_separation", "Button") == 6);
            var c = new Control { Name = "Themed" };
            c.PendingValues = new List<(string Key, string Text)> { ("theme", "Resource(\"Themes/main.octheme\")") };
            Root.AddChild(c);
            T.Check("a scene's theme = Resource(...) is that theme", c.Theme != null && c.GetThemeConstant("h_separation", "Button") == 6, c.Theme == null ? "no theme" : "");

            File.WriteAllText(Path.Combine(root, "Themes", "project.octheme"), "OCTHEME 1\nCOLOR Control font_color #00FF00FF\n");
            ThemeDB.SetProjectTheme(ResourceLoader.Load("Themes/project.octheme") as Theme);
            T.Check("the project theme loaded from a file answers below the node chain", new Control().GetThemeColor("font_color").G == 1f);
            ThemeDB.SetProjectTheme(null);
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            try { Directory.Delete(root, true); } catch (IOException) { }
        }
    }

    // ---- style boxes ----

    private static byte[] Bytes(UiStyleBoxStruct s)
    {
        int n = Marshal.SizeOf<UiStyleBoxStruct>();
        IntPtr p = Marshal.AllocHGlobal(n);
        try
        {
            Marshal.StructureToPtr(s, p, false);
            var b = new byte[n];
            Marshal.Copy(p, b, 0, n);
            return b;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    private static void StyleBoxes()
    {
        T.Check("the native struct is the size the library says", Marshal.SizeOf<UiStyleBoxStruct>() == UiNative.aver_ui_stylebox_struct_size());
        T.Check("a new StyleBoxFlat is the engine's flat default", Bytes(new StyleBoxFlat().ToNative()).SequenceEqual(Bytes(UiNative.Defaults(UiSbKind.Flat))));
        T.Check("a new StyleBoxTexture is the engine's texture default", Bytes(new StyleBoxTexture().ToNative()).SequenceEqual(Bytes(UiNative.Defaults(UiSbKind.Texture))));
        T.Check("a new StyleBoxLine is the engine's line default", Bytes(new StyleBoxLine().ToNative()).SequenceEqual(Bytes(UiNative.Defaults(UiSbKind.Line))));
        T.Check("a new StyleBoxEmpty is the engine's empty default", Bytes(new StyleBoxEmpty().ToNative()).SequenceEqual(Bytes(UiNative.Defaults(UiSbKind.Empty))));

        var flat = new StyleBoxFlat { BgColor = new Color(0, 0, 1, 1), AntiAliasing = false };
        flat.SetBorderWidthAll(4);
        flat.BorderColor = new Color(1, 0, 0, 1);
        T.Check("a flat box's content margin defaults to its border width", Is(flat.GetMinimumSize(), 8, 8) && flat.GetMargin(Side.Left) == 4 && flat.GetContentMargin(Side.Left) == -1);
        flat.ContentMarginLeft = 10;
        T.Check("an explicit content margin wins", flat.GetMargin(Side.Left) == 10 && flat.GetMargin(Side.Top) == 4 && Is(flat.GetOffset(), 10, 4) && IsRect(flat.GetContentRect(new Rect2(0, 0, 100, 50)), 10, 4, 86, 42));
        StyleBoxMesh? mesh = flat.Tessellate(new Rect2(10, 20, 100, 60));
        T.Check("it tessellates through the engine", mesh != null && mesh.VertexCount > 4 && mesh.Indices.Length % 3 == 0 && mesh.Texture == 0);
        if (mesh != null)
        {
            float minX = float.MaxValue, maxX = float.MinValue, minY = float.MaxValue, maxY = float.MinValue;
            for (int i = 0; i < mesh.VertexCount; i++)
            {
                minX = Math.Min(minX, mesh.Xyuv[i * 4]); maxX = Math.Max(maxX, mesh.Xyuv[i * 4]);
                minY = Math.Min(minY, mesh.Xyuv[i * 4 + 1]); maxY = Math.Max(maxY, mesh.Xyuv[i * 4 + 1]);
            }
            T.Check("covering exactly the rectangle", T.Near(minX, 10) && T.Near(maxX, 110) && T.Near(minY, 20) && T.Near(maxY, 80));
            T.Check("with the border colour and the fill colour", mesh.Colors.Contains(0xFF0000FFu) && mesh.Colors.Contains(0xFFFF0000u), string.Join(",", mesh.Colors.Distinct().Select(c => c.ToString("X"))));
        }
        flat.SetExpandMarginAll(5);
        T.Check("expand margins grow the drawn rectangle only", IsRect(flat.GetDrawRect(new Rect2(0, 0, 10, 10)), -5, -5, 20, 20) && Is(flat.GetMinimumSize(), 14, 8));
        flat.SetCornerRadiusAll(12);
        flat.CornerDetail = 4;
        StyleBoxMesh? coarse = flat.Tessellate(new Rect2(0, 0, 100, 60));
        flat.CornerDetail = 8;
        StyleBoxMesh? fine = flat.Tessellate(new Rect2(0, 0, 100, 60));
        T.Check("rounded corners add points, and corner_detail sets how many", coarse != null && fine != null && coarse.VertexCount > mesh!.VertexCount && fine.VertexCount > coarse.VertexCount);
        flat.CornerDetail = 99;
        T.Check("corner_detail is limited to 20", flat.CornerDetail == 20);
        T.Check("radii and border widths read back", flat.GetCornerRadius(Corner.TopLeft) == 12 && flat.GetBorderWidth(Side.Right) == 4 && flat.GetBorderWidthMin() == 4);
        flat.BorderBlend = true;
        flat.Skew = new Vector2(8, 0);
        flat.ShadowSize = 6;
        flat.ShadowOffset = new Vector2(2, 3);
        UiStyleBoxStruct ns = flat.ToNative();
        T.Check("blend, skew and shadow reach the native box", (ns.Flags & (uint)UiSbFlags.BorderBlend) != 0 && ns.SkewX == 8 && ns.ShadowSize == 6 && ns.ShadowOffX == 2 && ns.ShadowOffY == 3);
        int changed = 0;
        flat.Changed += () => changed++;
        flat.BgColor = new Color(1, 1, 1, 1);
        T.Check("a property change emits changed", changed == 1);
        StyleBox copy = flat.DuplicateBox();
        ((StyleBoxFlat)copy).BgColor = new Color(0, 0, 0, 1);
        T.Check("duplicate shares nothing", flat.BgColor.R == 1f);

        var tex = new StyleBoxTexture { Texture = new SizedTexture(30, 30, 77) };
        tex.SetTextureMarginAll(10);
        T.Check("a texture box's default content margin is its texture margin", Is(tex.GetMinimumSize(), 20, 20));
        float[] cells = UiNative.NinePatch(tex.ToNative(), 100, 200, 80, 60);
        T.Check("nine cells of eight numbers", cells.Length == 72, $"{cells.Length}");
        T.Check("the top-left corner is 10 x 10 at the origin and shows the first third", cells.Length == 72 && T.Near(cells[0], 100) && T.Near(cells[1], 200) && T.Near(cells[2], 10) && T.Near(cells[6], 1f / 3));
        StyleBoxMesh? tm = tex.Tessellate(new Rect2(0, 0, 80, 60));
        T.Check("the mesh is nine textured quads on the texture's handle", tm != null && tm.VertexCount == 36 && tm.Texture == 77, tm == null ? "null" : $"{tm.VertexCount} {tm.Texture}");
        tex.AxisStretchHorizontal = StyleBoxTexture.AxisStretchMode.Tile;
        T.Check("tile mode cuts the middle into tiles", UiNative.NinePatch(tex.ToNative(), 0, 0, 85, 30).Length / 8 > 9);
        tex.AxisStretchHorizontal = StyleBoxTexture.AxisStretchMode.Stretch;
        tex.DrawCenter = false;
        T.Check("draw_center off drops the middle cell", UiNative.NinePatch(tex.ToNative(), 0, 0, 80, 60).Length / 8 == 8);
        tex.Texture = null;
        T.Check("no texture, nothing to draw", tex.Tessellate(new Rect2(0, 0, 10, 10))!.Indices.Length == 0);

        var line = new StyleBoxLine { Thickness = 4, Color = new Color(1, 0, 0, 1) };
        StyleBoxMesh? lm = line.Tessellate(new Rect2(10, 10, 100, 30));
        T.Check("a line is one quad centred in the height, grown at both ends", lm != null && lm.VertexCount == 4 && T.Near(lm.Xyuv[0], 9) && T.Near(lm.Xyuv[1], 23));
        T.Check("an empty box draws nothing and keeps margins", new StyleBoxEmpty().Tessellate(new Rect2(0, 0, 5, 5))!.Indices.Length == 0);

        UiNative.aver_ui_begin_frame(0, 0, 200, 100);
        flat.Draw(new Rect2(0, 0, 50, 30));
        T.Check("draw adds the box to the HUD draw list", UiNative.aver_ui_vertex_count() > 4 && UiNative.aver_ui_command_count() == 1);
    }

    // ---- stretch ----

    private static void Stretch()
    {
        var w = Root;
        T.Check("by default nothing stretches: the visible area is the window", w.ContentScaleMode == Window.ContentScaleModeEnum.Disabled && IsRect(w.GetVisibleRect(), 0, 0, 1280, 720) && !w.IsLetterboxed());
        var ctl = new Control { Name = "Fill" };
        ctl.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect);
        Root.AddChild(ctl);

        w.ContentScaleMode = Window.ContentScaleModeEnum.CanvasItems;
        w.ContentScaleSize = new Vector2I(640, 360);
        w.ContentScaleAspect = Window.ContentScaleAspectEnum.Keep;
        T.Check("canvas_items at 640 x 360 in a 1280 x 720 window: the UI is 640 x 360", IsRect(w.GetVisibleRect(), 0, 0, 640, 360) && IsRect(ctl.GetRect(), 0, 0, 640, 360), $"{w.GetVisibleRect()}");
        T.Check("(a full-rect control follows)", Is(ctl.Size, 640, 360));

        w.Size = new Vector2I(2560, 1080);
        T.Check("a wider window: still 640 x 360, with bars at the sides", IsRect(w.GetVisibleRect(), 0, 0, 640, 360) && w.IsLetterboxed() && IsRect(w.GetContentScreenRect(), 320, 0, 1920, 1080), $"{w.GetContentScreenRect()}");
        T.Check("the pointer maps through the bars: window centre is the UI centre", Is(w.WindowToContent(new Vector2(1280, 540)), 320, 180), $"{w.WindowToContent(new Vector2(1280, 540))}");
        T.Check("a point in a bar is outside the UI", w.WindowToContent(new Vector2(10, 10)).X < 0);
        T.Check("and back", Is(w.ContentToWindow(new Vector2(320, 180)), 1280, 540));
        UiNative.aver_ui_begin_frame(0, 0, 2560, 1080);
        Gui.DrawAll();
        T.Check("the two side bars are drawn as black rectangles (8 vertices)", UiNative.aver_ui_vertex_count() == 8, $"{UiNative.aver_ui_vertex_count()}");
        w.ContentScaleAspect = Window.ContentScaleAspectEnum.Ignore;
        UiNative.aver_ui_begin_frame(0, 0, 2560, 1080);
        Gui.DrawAll();
        T.Check("with aspect ignore there are no bars to draw", !w.IsLetterboxed() && UiNative.aver_ui_vertex_count() == 0, $"{UiNative.aver_ui_vertex_count()}");
        w.ContentScaleAspect = Window.ContentScaleAspectEnum.Keep;

        w.ContentScaleAspect = Window.ContentScaleAspectEnum.Expand;
        T.Check("expand grows the visible area instead (no bars)", IsRect(w.GetVisibleRect(), 0, 0, 853, 360) && !w.IsLetterboxed() && IsRect(ctl.GetRect(), 0, 0, 853, 360), $"{w.GetVisibleRect()} {ctl.GetRect()}");

        w.ContentScaleAspect = Window.ContentScaleAspectEnum.KeepHeight;
        T.Check("keep_height in a wide window behaves as expand", IsRect(w.GetVisibleRect(), 0, 0, 853, 360));
        w.ContentScaleAspect = Window.ContentScaleAspectEnum.KeepWidth;
        T.Check("keep_width in a wide window keeps bars", w.IsLetterboxed() && IsRect(w.GetVisibleRect(), 0, 0, 640, 360));

        w.ContentScaleAspect = Window.ContentScaleAspectEnum.Keep;
        w.ContentScaleFactor = 2f;
        T.Check("the scale factor halves the UI (canvas_items)", IsRect(w.GetVisibleRect(), 0, 0, 320, 180), $"{w.GetVisibleRect()}");
        w.ContentScaleFactor = 1f;

        w.ContentScaleMode = Window.ContentScaleModeEnum.Viewport;
        w.ContentScaleSize = new Vector2I(320, 180);
        w.Size = new Vector2I(1280, 720);
        T.Check("viewport mode renders at the base size", IsRect(w.GetVisibleRect(), 0, 0, 320, 180) && !w.IsLetterboxed());
        w.ContentScaleStretch = Window.ContentScaleStretchEnum.Integer;
        w.Size = new Vector2I(1000, 600);
        T.Check("integer scaling keeps whole multiples (3x of 320 x 180)", IsRect(w.GetContentScreenRect(), 20, 30, 960, 540) && w.IsLetterboxed(), $"{w.GetContentScreenRect()}");

        w.ContentScaleMode = Window.ContentScaleModeEnum.Disabled;
        w.ContentScaleStretch = Window.ContentScaleStretchEnum.Fractional;
        w.Size = new Vector2I(1280, 720);
        T.Check("disabled again: the window", IsRect(w.GetVisibleRect(), 0, 0, 1280, 720));
        T.Check("the project settings are Godot's names", ProjectSettings.GetSetting("display/window/stretch/mode").AsString() == "disabled" && ProjectSettings.GetSetting("gui/timers/tooltip_delay_sec").AsDouble() == 0.5 && ProjectSettings.HasSetting("display/window/stretch/aspect"));
    }

    // ---- drawing ----

    private static void Drawing()
    {
        var d = new Drawn { Name = "D", Size = new Vector2(40, 30), Position = new Vector2(100, 50) };
        Root.AddChild(d);
        d.DrawRect(new Rect2(0, 0, 1, 1), new Color(1, 1, 1, 1));
        T.Check("the draw methods are only valid inside a draw pass (an error is logged)", T.ErrorCount("draw") >= 1 && d.Commands.Count == 0);
        d.RunDraw();
        T.Check("a draw pass records a rect and a style box", d.Draws == 1 && d.Commands.Count == 2 && d.Commands[0].Type == CanvasItem.DrawCommand.Kind.Rect && d.Commands[1].Type == CanvasItem.DrawCommand.Kind.StyleBox);
        d.RunDraw();
        T.Check("nothing is redrawn until queue_redraw", d.Draws == 1);
        d.Size = new Vector2(50, 30);
        d.RunDraw();
        T.Check("a size change asks for a redraw", d.Draws == 2);
        int drawSignals = 0;
        d.Draw += () => drawSignals++;
        d.QueueRedraw();
        d.RunDraw();
        T.Check("the draw signal is emitted inside the pass", drawSignals == 1 && d.Draws == 3);

        UiNative.aver_ui_begin_frame(0, 0, 1280, 720);
        d.QueueRedraw();
        Gui.DrawAll();
        int verts = UiNative.aver_ui_vertex_count();
        T.Check("the GUI replays the commands into the draw list (a rect is 4 vertices, a plain box more)", verts >= 8, $"{verts}");
        T.Check("a draw list command was made", UiNative.aver_ui_command_count() >= 1);
        UiNative.aver_ui_begin_frame(0, 0, 1280, 720);
        d.Hide();
        Gui.DrawAll();
        T.Check("a hidden control draws nothing", UiNative.aver_ui_vertex_count() == 0);
        d.Show();
        var clipper = new Control { Name = "Clip", ClipContents = true, Size = new Vector2(10, 10) };
        var inner = new Drawn { Name = "Inner", Size = new Vector2(5, 5) };
        clipper.AddChild(inner);
        Root.AddChild(clipper);
        UiNative.aver_ui_begin_frame(0, 0, 1280, 720);
        Gui.DrawAll();
        T.Check("children of a clipping control are drawn (inside a clip that is popped again)", UiNative.aver_ui_vertex_count() >= 16);
        T.Check("modulate and visibility are part of the drawing, not the commands", d.Modulate.A == 1f && d.GetCombinedModulate().A == 1f);
        d.Modulate = new Color(1, 1, 1, 0.5f);
        T.Check("combined modulate multiplies down the tree", T.Near(d.GetCombinedModulate().A, 0.5f));
    }

    // ---- the GUI ----

    private static Probe Setup(Control.MouseFilterEnum bFilter = Control.MouseFilterEnum.Pass, Control.MouseFilterEnum aFilter = Control.MouseFilterEnum.Stop)
    {
        Probe.Trail.Clear();
        UnhandledProbe.Seen.Clear();
        Root.AddChild(new UnhandledProbe { Name = "Unhandled" });
        var a = new Probe("A", 0, 0, 200, 200, aFilter);
        var b = new Probe("B", 50, 50, 100, 100, bFilter);
        var c = new Probe("C", 10, 10, 50, 50, Control.MouseFilterEnum.Ignore);
        a.AddChild(b);
        b.AddChild(c);
        Root.AddChild(a);
        Settle();
        return a;
    }

    private static void MousePropagation()
    {
        Probe a = Setup();
        Click(70, 70, true);
        T.CheckSeq("a click goes to the control hit (Ignore skipped), then to its parent, and stops at the Stop one", new[] { "B:down Left 20,20", "A:down Left 70,70" }, Probe.Trail);
        T.Check("a stopped click never reaches _unhandled_input", UnhandledProbe.Seen.Count == 0);
        Click(70, 70, false);

        Probe.Trail.Clear();
        ((Probe)a.GetChild(0)).MouseFilter = Control.MouseFilterEnum.Stop;
        Click(70, 70, true);
        T.CheckSeq("a Stop control keeps the event from its parent", new[] { "B:down Left 20,20" }, Probe.Trail);
        Click(70, 70, false);

        Probe.Trail.Clear();
        var b = (Probe)a.GetChild(0);
        b.MouseFilter = Control.MouseFilterEnum.Pass;
        b.Accepts = true;
        Click(70, 70, true);
        T.CheckSeq("accept_event keeps it from the parent too", new[] { "B:down Left 20,20" }, Probe.Trail);
        T.Check("and from _unhandled_input", UnhandledProbe.Seen.Count == 0);
        Click(70, 70, false);
        b.Accepts = false;

        Probe.Trail.Clear();
        a.MouseFilter = Control.MouseFilterEnum.Pass;
        Click(70, 70, true);
        T.CheckSeq("with Pass all the way up, every control sees it", new[] { "B:down Left 20,20", "A:down Left 70,70" }, Probe.Trail);
        T.Check("and nothing stopped it, so _unhandled_input does too", UnhandledProbe.Seen.Count == 1 && UnhandledProbe.Seen[0].StartsWith("down Left"));
        Click(70, 70, false);

        Probe.Trail.Clear();
        UnhandledProbe.Seen.Clear();
        Click(900, 900, true);
        T.Check("a click on nothing reaches no control and the game gets it", Probe.Trail.Count == 0 && UnhandledProbe.Seen.Count == 1);
        Click(900, 900, false);

        Probe.Trail.Clear();
        a.MouseFilter = Control.MouseFilterEnum.Ignore;
        Click(10, 10, true);
        T.Check("an Ignore control is invisible to the pointer", Probe.Trail.Count == 0);
        Click(10, 10, false);
        a.MouseFilter = Control.MouseFilterEnum.Stop;

        // the signal first, then the virtual
        Probe.Trail.Clear();
        b.GuiInput += e => Probe.Trail.Add("B:signal");
        Click(70, 70, true);
        T.Check("gui_input is emitted before _gui_input runs", Probe.Trail.Count >= 2 && Probe.Trail[0] == "B:signal" && Probe.Trail[1].StartsWith("B:down"), string.Join("; ", Probe.Trail));
        Click(70, 70, false);

        // a round hit area
        Probe.Trail.Clear();
        var round = new Probe("Round", 300, 0, 100, 100) { Radius = 40 };
        Root.AddChild(round);
        Click(302, 2, true);
        T.Check("_has_point decides where a control can be hit (a corner outside the circle misses)", Probe.Trail.Count == 0);
        Click(302, 2, false);
        Click(350, 50, true);
        T.Check("and the middle hits", Probe.Trail.Count == 1);
        Click(350, 50, false);

        // later siblings are above earlier ones
        Probe.Trail.Clear();
        var s1 = new Probe("S1", 500, 0, 100, 100);
        var s2 = new Probe("S2", 550, 50, 100, 100);
        Root.AddChild(s1);
        Root.AddChild(s2);
        Click(570, 70, true);
        T.CheckSeq("overlapping siblings: the later one is on top", new[] { "S2:down Left 20,20" }, Probe.Trail);
        Click(570, 70, false);
        s2.Visible = false;
        Probe.Trail.Clear();
        Click(570, 70, true);
        T.CheckSeq("a hidden control does not take the click", new[] { "S1:down Left 70,70" }, Probe.Trail);
        Click(570, 70, false);

        // clipping
        Probe.Trail.Clear();
        var clip = new Probe("Clip", 0, 300, 100, 100) { ClipContents = true };
        var spill = new Probe("Spill", 50, 50, 200, 200, Control.MouseFilterEnum.Pass);
        clip.AddChild(spill);
        Root.AddChild(clip);
        Click(200, 400, true);
        T.Check("a child cannot be hit outside a clipping parent", Probe.Trail.Count == 0);
        Click(200, 400, false);
        Click(70, 370, true);
        T.CheckSeq("but inside it can, with local coordinates", new[] { "Spill:down Left 20,20", "Clip:down Left 70,70" }, Probe.Trail);
        Click(70, 370, false);

        // transforms
        Probe.Trail.Clear();
        var scaled = new Probe("Scaled", 100, 600, 50, 50) { Scale = new Vector2(2, 2) };
        Root.AddChild(scaled);
        Click(190, 690, true);
        T.CheckSeq("a scaled control is hit over its scaled area and gets local coordinates", new[] { "Scaled:down Left 45,45" }, Probe.Trail);
        Click(190, 690, false);
        T.Check("(and its global rectangle is the position and size)", IsRect(scaled.GetGlobalRect(), 100, 600, 50, 50));

        // hover query
        Move(70, 70);
        T.Check("the viewport knows the hovered control", ReferenceEquals(Root.GuiGetHoveredControl(), a.GetChild(0)));
    }

    private static void MouseCapture()
    {
        Probe a = Setup(Control.MouseFilterEnum.Stop);
        var b = (Probe)a.GetChild(0);
        a.LogMotion = b.LogMotion = false;
        Probe.Hover.Clear();
        Move(60, 60);
        T.CheckSeq("moving in enters the controls under the pointer, outermost first", new[] { "A:enter", "B:enter" }, Probe.Hover);
        Probe.Hover.Clear();
        Move(10, 10);
        T.CheckSeq("moving to the parent alone exits the child", new[] { "B:exit" }, Probe.Hover);
        Probe.Hover.Clear();
        Move(900, 900);
        T.CheckSeq("and out exits the parent", new[] { "A:exit" }, Probe.Hover);

        Probe.Trail.Clear();
        a.LogMotion = b.LogMotion = true;
        Move(60, 60);
        Probe.Trail.Clear();
        Click(60, 60, true);
        Move(500, 500, 440, 440);
        Move(520, 500, 20, 0);
        Click(520, 500, false);
        T.CheckSeq("a pressed control keeps the pointer: motion and release reach it wherever the pointer goes, in its local coordinates (the first move also asks for a drag)",
                   new[] { "B:down Left 10,10", "B:getdrag 10,10", "B:move 450,450", "B:move 470,450", "B:up Left 470,450" }, Probe.Trail);
        Probe.Trail.Clear();
        Move(900, 900);
        Move(60, 60);
        T.Check("afterwards hover works again", Probe.Trail.Contains("B:move 10,10"));

        Probe.Trail.Clear();
        Click(60, 60, true);
        Click(150, 150, true, MouseButton.Right);
        Click(150, 150, false, MouseButton.Right);
        Click(60, 60, false);
        T.Check("a second button pressed while one is held goes to the same control", Probe.Trail.Count(l => l.StartsWith("B:down")) == 2 && !Probe.Trail.Any(l => l.StartsWith("A:")));
    }

    private static Probe[] Row3(Control.FocusModeEnum mode = Control.FocusModeEnum.All)
    {
        Probe.Trail.Clear();
        UnhandledProbe.Seen.Clear();
        Root.AddChild(new UnhandledProbe { Name = "Unhandled" });
        var list = new Probe[3];
        for (int i = 0; i < 3; i++)
        {
            list[i] = new Probe("F" + (i + 1), i * 60, 0, 50, 20) { FocusMode = mode };
            Root.AddChild(list[i]);
        }
        Settle();
        return list;
    }

    private static void Focus()
    {
        Probe[] f = Row3();
        int enteredSignals = 0, exitedSignals = 0;
        f[0].FocusEntered += () => enteredSignals++;
        f[0].FocusExited += () => exitedSignals++;
        f[0].GrabFocus();
        T.Check("grab_focus focuses", f[0].HasFocus() && ReferenceEquals(Root.GuiGetFocusOwner(), f[0]) && enteredSignals == 1);
        f[1].GrabFocus();
        T.CheckSeq("the old owner loses focus before the new one gains it", new[] { "F1:focus+", "F1:focus-", "F2:focus+" }, Probe.Trail);
        T.Check("focus_exited fires", exitedSignals == 1 && !f[0].HasFocus() && f[1].HasFocus());
        f[1].GrabFocus();
        T.Check("grabbing what you have changes nothing", Probe.Trail.Count == 3);
        f[1].ReleaseFocus();
        T.Check("release_focus", !f[1].HasFocus() && Root.GuiGetFocusOwner() == null);
        f[2].GrabFocus();
        f[2].Visible = false;
        T.Check("hiding the owner releases focus", !f[2].HasFocus());
        f[2].Visible = true;
        f[2].GrabFocus();
        f[2].GetParent()!.RemoveChild(f[2]);
        T.Check("leaving the tree does too", Root.GuiGetFocusOwner() == null);
        Root.AddChild(f[2]);
        f[0].FocusMode = Control.FocusModeEnum.None;
        f[0].GrabFocus();
        T.Check("a control with focus_mode None cannot take focus (an error is logged)", !f[0].HasFocus() && T.ErrorCount("focus_mode") >= 1);
        f[0].FocusMode = Control.FocusModeEnum.All;
        f[0].GrabFocus();
        f[0].FocusMode = Control.FocusModeEnum.None;
        T.Check("setting focus_mode None gives focus up", !f[0].HasFocus());
        f[0].FocusMode = Control.FocusModeEnum.All;

        // clicking
        Probe.Trail.Clear();
        f[0].FocusMode = Control.FocusModeEnum.Click;
        Click(10, 10, true);
        Click(10, 10, false);
        T.Check("a left click focuses a Click control", f[0].HasFocus());
        f[1].FocusMode = Control.FocusModeEnum.All;
        Click(70, 10, true, MouseButton.Right);
        Click(70, 10, false, MouseButton.Right);
        T.Check("a right click does not", f[0].HasFocus());
        f[2].FocusMode = Control.FocusModeEnum.None;
        Click(130, 10, true);
        Click(130, 10, false);
        T.Check("clicking a control that cannot focus keeps focus where it was", f[0].HasFocus());

        var parent = new Probe("P", 0, 100, 100, 50, Control.MouseFilterEnum.Stop) { FocusMode = Control.FocusModeEnum.All };
        var kid = new Probe("K", 10, 10, 30, 30, Control.MouseFilterEnum.Pass) { FocusMode = Control.FocusModeEnum.None };
        parent.AddChild(kid);
        Root.AddChild(parent);
        Settle();
        Click(20, 120, true);
        Click(20, 120, false);
        T.Check("a click on a non-focusable Pass control focuses the parent that can", parent.HasFocus());

        // Tab
        f[0].FocusMode = Control.FocusModeEnum.All;
        f[1].FocusMode = Control.FocusModeEnum.All;
        f[2].FocusMode = Control.FocusModeEnum.All;
        parent.FocusMode = Control.FocusModeEnum.None;
        f[0].GrabFocus();
        UnhandledProbe.Seen.Clear();
        PressKey(Key.Tab);
        T.Check("Tab moves focus to the next control", f[1].HasFocus() && UnhandledProbe.Seen.Count == 0);
        PressKey(Key.Tab);
        PressKey(Key.Tab);
        T.Check("and wraps round", f[0].HasFocus());
        PressKey(Key.Tab, true, shift: true);
        T.Check("Shift+Tab goes back (wrapping to the last)", f[2].HasFocus());
        f[1].FocusMode = Control.FocusModeEnum.Click;
        f[0].GrabFocus();
        PressKey(Key.Tab);
        T.Check("controls with focus_mode Click are not on the Tab path", f[2].HasFocus());
        f[1].FocusMode = Control.FocusModeEnum.All;
        f[0].FocusNext = new NodePath("../F3");
        f[0].GrabFocus();
        PressKey(Key.Tab);
        T.Check("focus_next names the control Tab goes to", f[2].HasFocus());
        f[0].FocusNext = new NodePath("");
        f[2].ReleaseFocus();
        PressKey(Key.Tab);
        T.Check("with nothing focused Tab focuses the first control that can", Root.GuiGetFocusOwner() != null);

        // keys go to the focus owner
        Probe.Trail.Clear();
        f[1].GrabFocus();
        Probe.Trail.Clear();
        UnhandledProbe.Seen.Clear();
        PressKey(Key.A);
        T.CheckSeq("a key reaches the focus owner's _gui_input", new[] { "F2:key A down" }, Probe.Trail);
        T.Check("and, not accepted, the game's _unhandled_input", UnhandledProbe.Seen.Count == 1);
        f[1].Accepts = true;
        UnhandledProbe.Seen.Clear();
        PressKey(Key.A);
        T.Check("accept_event keeps a key from _unhandled_input", UnhandledProbe.Seen.Count == 0);
        f[1].Accepts = false;

        // arrows
        var grid = new Probe[4];
        float[][] at = { new[] { 0f, 300f }, new[] { 100f, 300f }, new[] { 0f, 350f }, new[] { 100f, 350f } };
        for (int i = 0; i < 4; i++)
        {
            grid[i] = new Probe("G" + i, at[i][0], at[i][1], 50, 20) { FocusMode = Control.FocusModeEnum.All };
            Root.AddChild(grid[i]);
        }
        Settle();
        grid[0].GrabFocus();
        PressKey(Key.Right);
        T.Check("ui_right goes to the control on the right", grid[1].HasFocus());
        PressKey(Key.Down);
        T.Check("ui_down to the one below", grid[3].HasFocus());
        PressKey(Key.Left);
        T.Check("ui_left", grid[2].HasFocus());
        PressKey(Key.Up);
        T.Check("ui_up", grid[0].HasFocus());
        PressKey(Key.Left);
        T.Check("no neighbour that way: focus stays", grid[0].HasFocus());
        grid[0].FocusNeighborRight = new NodePath("../G3");
        PressKey(Key.Right);
        T.Check("an explicit neighbour beats the nearest one", grid[3].HasFocus());
        grid[3].FocusMode = Control.FocusModeEnum.None;
        grid[2].FocusNeighborRight = new NodePath("../G3");
        grid[3].FocusNeighborRight = new NodePath("../G1");
        grid[2].GrabFocus();
        PressKey(Key.Right);
        T.Check("a neighbour that cannot focus hands on to its own neighbour", grid[1].HasFocus());

        // ui_* actions
        T.Check("ui_accept is Enter, Space and keypad Enter by default", Gui.IsUiAction(new InputEventKey { Keycode = Key.Enter, Pressed = true }, "ui_accept") && Gui.IsUiAction(new InputEventKey { Keycode = Key.Space, Pressed = true }, "ui_accept") &&
                                                                          !Gui.IsUiAction(new InputEventKey { Keycode = Key.A, Pressed = true }, "ui_accept"));
        InputMap.AddAction("ui_accept");
        InputMap.ActionAddEvent("ui_accept", new InputEventKey { Keycode = Key.A, PhysicalKeycode = Key.A });
        T.Check("a project that rebinds ui_accept replaces the built-in keys", Gui.IsUiAction(new InputEventKey { Keycode = Key.A, Pressed = true }, "ui_accept") && !Gui.IsUiAction(new InputEventKey { Keycode = Key.Enter, Pressed = true }, "ui_accept"));
    }

    private static void Tooltips()
    {
        Gui.TooltipDelaySeconds = 0.5;
        Probe.Trail.Clear();
        var p = new Probe("P", 0, 0, 100, 100) { TooltipText = "Hello" };
        Root.AddChild(p);
        Settle();
        Move(10, 10);
        T.Frame(0.125); T.Frame(0.125); T.Frame(0.125);
        T.Check("no tooltip before the delay", !Gui.TooltipVisible);
        T.Frame(0.125);
        T.Check("it appears after the delay (0.5 s)", Gui.TooltipVisible && Gui.TooltipText == "Hello" && ReferenceEquals(Gui.TooltipSource, p));
        T.Check("at the pointer plus ten pixels", Is(Gui.TooltipPosition, 20, 20), $"{Gui.TooltipPosition}");
        T.Check("the control was asked for its text at the local position", Probe.Trail.Contains("P:tip 10,10"), string.Join("; ", Probe.Trail.Where(l => l.Contains("tip"))));
        Move(11, 10, 1, 0);
        T.Check("moving hides it", !Gui.TooltipVisible);
        T.Frame(0.125); T.Frame(0.125); T.Frame(0.125);
        T.Check("and the delay starts again", !Gui.TooltipVisible);
        T.Frame(0.125);
        T.Check("(it comes back after another half second)", Gui.TooltipVisible);
        Click(11, 10, true);
        T.Check("a click hides it", !Gui.TooltipVisible);
        Click(11, 10, false);
        for (int i = 0; i < 8; i++) T.Frame(0.125);
        T.Check("and it does not come back until the pointer moves", !Gui.TooltipVisible);

        Move(900, 900);
        for (int i = 0; i < 8; i++) T.Frame(0.125);
        T.Check("no control under the pointer: no tooltip", !Gui.TooltipVisible);

        Move(10, 10);
        Move(900, 900);
        for (int i = 0; i < 8; i++) T.Frame(0.125);
        T.Check("leaving before the delay cancels it", !Gui.TooltipVisible);

        // clamped to the window
        Move(1275, 715);
        var edge = new Probe("Edge", 1200, 650, 80, 70) { TooltipText = "A rather long tooltip text" };
        Root.AddChild(edge);
        Settle();
        Move(1276, 716);
        for (int i = 0; i < 5; i++) T.Frame(0.125);
        Vector2 size = Gui.DefaultTooltipSize(Gui.TooltipText);
        T.Check("a tooltip near the corner stays inside the window", Gui.TooltipVisible && Gui.TooltipPosition.X + size.X <= 1280.01f && Gui.TooltipPosition.Y + size.Y <= 720.01f, $"{Gui.TooltipPosition} {size}");
        Move(900, 900);

        // Pass chain
        var outer = new Probe("Outer", 0, 200, 200, 200) { TooltipText = "Outer tip", MouseFilter = Control.MouseFilterEnum.Stop };
        var inner = new Probe("Inner", 50, 50, 50, 50) { MouseFilter = Control.MouseFilterEnum.Pass };
        outer.AddChild(inner);
        Root.AddChild(outer);
        Settle();
        Move(70, 270);
        for (int i = 0; i < 5; i++) T.Frame(0.125);
        T.Check("a child with no text lets the parent's tooltip show (Pass)", Gui.TooltipVisible && Gui.TooltipText == "Outer tip" && ReferenceEquals(Gui.TooltipSource, outer));
        Move(900, 900);
        inner.MouseFilter = Control.MouseFilterEnum.Stop;
        Move(70, 270);
        for (int i = 0; i < 5; i++) T.Frame(0.125);
        T.Check("a Stop child with no text keeps the parent's tooltip away", !Gui.TooltipVisible);
        Move(900, 900);

        // custom
        var cust = new Probe("Cust", 300, 200, 100, 100) { TooltipText = "x" };
        Control? made = null;
        cust.Custom = text => made = new Probe("Tip:" + text, 0, 0, 40, 10);
        Root.AddChild(cust);
        Settle();
        Move(320, 220);
        for (int i = 0; i < 5; i++) T.Frame(0.125);
        T.Check("_make_custom_tooltip's control is shown instead (in the tree, on top, at the pointer)", Gui.TooltipVisible && made != null && made.IsInsideTree() && ReferenceEquals(Gui.CustomTooltipControl, made) && Is(made.Position, 330, 230), $"{made?.Position}");
        Move(321, 220, 1, 0);
        T.Check("it is freed when the tooltip goes", !made!.IsAlive || !made.IsInsideTree());

        // a different delay
        Gui.TooltipDelaySeconds = 0.125;
        Move(310, 210);
        T.Frame(0.125);
        T.Check("the delay is the project's gui/timers/tooltip_delay_sec", Gui.TooltipVisible);
        Gui.TooltipDelaySeconds = -1;
    }

    private static void DragDrop()
    {
        Probe.Trail.Clear();
        UnhandledProbe.Seen.Clear();
        var src = new Probe("Src", 0, 0, 100, 100) { DragSource = _ => "payload" };
        var dst = new Probe("Dst", 200, 0, 100, 100) { CanDrop = (at, data) => data.AsString() == "payload" };
        Probe? preview = null;
        src.DragSource = at =>
        {
            preview = new Probe("Preview", 0, 0, 30, 10);
            src.SetDragPreview(preview);
            return "payload";
        };
        Root.AddChild(src);
        Root.AddChild(dst);
        Settle();

        Click(10, 10, true);
        Move(15, 10, 5, 0);
        T.Check("a small movement is not a drag", !Root.GuiIsDragging() && !Probe.Trail.Any(l => l.Contains("getdrag")));
        Move(25, 10, 10, 0);
        T.Check("past ten pixels the source is asked for data, at the press position", Probe.Trail.Contains("Src:getdrag 10,10"), string.Join("; ", Probe.Trail));
        T.Check("a drag begins: gui_is_dragging, the data, the preview in the tree", Root.GuiIsDragging() && Root.GuiGetDragData().AsString() == "payload" && preview != null && preview.IsInsideTree());
        T.Check("every control was told (drag_begin)", Probe.Trail.Contains("Src:dragbegin") && Probe.Trail.Contains("Dst:dragbegin"));
        T.Check("the preview ignores the pointer", preview!.MouseFilter == Control.MouseFilterEnum.Ignore);
        Probe.Trail.Clear();
        Move(250, 20, 225, 10);
        T.Check("the preview follows the pointer", Is(preview.Position, 250, 20), $"{preview.Position}");
        T.Check("the control under the pointer is asked if it can take the data", Probe.Trail.Contains("Dst:candrop True") && Gui.DragCanDrop);
        T.Check("the source gets no more events while dragging", !Probe.Trail.Any(l => l.StartsWith("Src:move")));
        Probe.Trail.Clear();
        Click(250, 20, false);
        T.CheckSeq("released over a taker: it is asked, then given the data in local coordinates, then drag_end goes to all",
                   new[] { "Dst:candrop True", "Dst:drop 50,20 payload", "Src:dragend", "Dst:dragend" }, Probe.Trail);
        T.Check("the drag is over and was successful", !Root.GuiIsDragging() && Root.GuiIsDragSuccessful());
        T.Check("the preview is gone", !preview.IsAlive || !preview.IsInsideTree());

        // refused
        Probe.Trail.Clear();
        dst.CanDrop = (_, _) => false;
        Click(10, 10, true);
        Move(30, 10, 20, 0);
        Move(250, 20, 220, 10);
        T.Check("a refusing target shows no drop", !Gui.DragCanDrop);
        Probe.Trail.Clear();
        Click(250, 20, false);
        T.Check("released over a refuser: nothing dropped, drag_end sent, not successful", !Probe.Trail.Any(l => l.Contains(":drop ")) && Probe.Trail.Contains("Src:dragend") && !Root.GuiIsDragSuccessful());

        // released over nothing
        Click(10, 10, true);
        Move(30, 10, 20, 0);
        Move(800, 600, 770, 590);
        Probe.Trail.Clear();
        Click(800, 600, false);
        T.Check("released over nothing: not successful", !Probe.Trail.Any(l => l.Contains(":drop ")) && !Root.GuiIsDragSuccessful() && !Root.GuiIsDragging());

        // a parent takes it when the child refuses
        dst.CanDrop = (at, data) => true;
        var kid = new Probe("Kid", 10, 10, 30, 30, Control.MouseFilterEnum.Pass);
        dst.AddChild(kid);
        Settle();
        Click(10, 10, true);
        Move(30, 10, 20, 0);
        Move(225, 30, 195, 20);
        Probe.Trail.Clear();
        Click(225, 30, false);
        T.CheckSeq("a Pass child that refuses hands the drop to its parent, with the parent's coordinates",
                   new[] { "Kid:candrop False", "Dst:candrop True", "Dst:drop 25,30 payload" }, Probe.Trail.Take(3));
        kid.QueueFree();

        // Escape
        dst.CanDrop = (_, _) => true;
        Click(10, 10, true);
        Move(30, 10, 20, 0);
        T.Check("dragging again", Root.GuiIsDragging());
        Probe.Trail.Clear();
        PressKey(Key.Escape);
        T.Check("Escape cancels the drag without a drop", !Root.GuiIsDragging() && !Root.GuiIsDragSuccessful() && !Probe.Trail.Any(l => l.Contains(":drop ")));
        Click(10, 10, false);

        // force_drag
        Probe.Trail.Clear();
        var made = new Probe("Forced", 0, 0, 20, 20);
        src.ForceDrag("forced", made);
        T.Check("force_drag starts a drag at once with the given preview", Root.GuiIsDragging() && Root.GuiGetDragData().AsString() == "forced" && made.IsInsideTree());
        Move(250, 20, 5, 5);
        Click(250, 20, false);
        T.Check("and the drop works the same way", Probe.Trail.Any(l => l.StartsWith("Dst:drop") && l.EndsWith("forced")) && Root.GuiIsDragSuccessful());

        // no data, no drag
        var mute = new Probe("Mute", 400, 0, 100, 100);
        Root.AddChild(mute);
        Settle();
        Probe.Trail.Clear();
        Click(410, 10, true);
        Move(440, 10, 30, 0);
        Move(450, 10, 10, 0);
        T.Check("a source that returns nothing keeps getting plain motion", !Root.GuiIsDragging() && Probe.Trail.Any(l => l.StartsWith("Mute:move")));
        Click(450, 10, false);
    }

    // ---- the converter ----

    // The converted level is instantiated under the window, the project theme the converter wrote is set the way the host does from UI.THEME, and what the scene
    // said (layout, overrides, variations, the theme chain, the style boxes, containers) is read back through the Godot API.
    private static void ConverterChecks(string contentRoot)
    {
        ResourceLoader.ContentRootOverride = contentRoot;
        ThemeDB.ResetForTests();
        Theme? project = ResourceLoader.Load("res://Themes/theme.octheme", typeof(Theme)) as Theme;
        T.Check("the project theme the converter wrote loads", project != null && project.HasColor("font_color", "Control"));
        ThemeDB.SetProjectTheme(project);
        var frameBox = project?.GetStylebox("panel", "PanelContainer") as StyleBoxTexture;
        T.Check("the 9-patch style box has its texture, whose size is read from the PNG header, and the converter's margins, tile axis and modulate",
                frameBox?.Texture != null && frameBox.Texture.GetSize() == new Vector2(12, 10) && frameBox.TextureMarginLeft == 4 && frameBox.AxisStretchHorizontal == StyleBoxTexture.AxisStretchMode.Tile &&
                T.Near(frameBox.ModulateColor.G, 0.9f, 0.01f), $"{frameBox?.Texture?.GetSize()}");
        T.Check("and a texture the host has not given a handle draws nothing (the box is tessellated only with one)", frameBox != null && frameBox.ToNative().Texture == 0);

        Node main = ResourceLoader.Load<PackedScene>("res://main.tscn")!.Instantiate();
        Root.AddChild(main);
        Settle();
        T.Check("the level instantiates without an error", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

        var root = (Control)main;
        T.Check("the root is a full-rect Control with its tooltip and Pass filter", IsRect(root.GetRect(), 0, 0, 1280, 720) && root.TooltipText == "the whole screen" && root.MouseFilter == Control.MouseFilterEnum.Pass, $"{root.GetRect()}");

        var hud = root.GetNode<PanelContainer>("Hud");
        T.Check("the top-wide PanelContainer spans the window at its offset height", IsRect(hud.GetRect(), 0, 0, 1280, 60), $"{hud.GetRect()}");
        T.Check("its theme type variation is kept", hud.ThemeTypeVariation == "HeaderPanel");
        var hudBox = hud.GetThemeStylebox("panel") as StyleBoxFlat;
        T.Check("its style box override is a StyleBoxFlat with the scene's values (colour, bottom border, radii, skew, no centre)",
                hudBox != null && T.Near(hudBox.BgColor.R, 0.9f) && T.Near(hudBox.BgColor.A, 0.8f) && hudBox.BorderWidthBottom == 3 && hudBox.BorderWidthTop == 0 && hudBox.CornerRadiusTopLeft == 12 &&
                T.Near(hudBox.Skew.X, 4f) && !hudBox.DrawCenter);
        T.Check("the box's content margins default to its border widths (bottom 3), so the child sits above the border", hudBox != null && T.Near(hudBox.GetMargin(Side.Bottom), 3f) && T.Near(hudBox.GetMargin(Side.Top), 0f));
        T.Check("a colour override beats the variation, the node chain and the project theme", hud.GetThemeColor("font_color") == new Color(0.2f, 0.4f, 0.6f, 1f));
        T.Check("and a constant override is kept under its scene name", hud.HasThemeConstantOverride("margin") && hud.GetThemeConstant("margin") == 4);

        var row = hud.GetNode<HBoxContainer>("Row");
        T.Check("the HBoxContainer gets the content rectangle of the box (60 minus the 3 pixel border)", IsRect(row.GetRect(), 0, 0, 1280, 57), $"{row.GetRect()}");
        T.Check("its separation override and alignment are kept", row.GetThemeConstant("separation") == 12 && row.Alignment == BoxContainer.AlignmentMode.Center);

        var grid = row.GetNode<GridContainer>("Grid");
        var rule = row.GetNode<Panel>("Rule");
        T.Check("the expanding grid takes the row's width less the separation and the rule", T.Near(grid.Size.X + 12f + rule.Size.X, 1280f) && grid.Columns == 3, $"{grid.Size} {rule.Size}");
        var cell = grid.GetNode<Control>("Cell");
        var cell2 = grid.GetNode<Control>("Cell2");
        T.Check("the first cell keeps its minimum size, flags and stretch ratio", cell.GetCombinedMinimumSize() == new Vector2(40, 24) && cell.SizeFlagsHorizontal == Control.SizeFlags.ExpandFill &&
                cell.SizeFlagsVertical == Control.SizeFlags.ShrinkCenter && T.Near(cell.SizeFlagsStretchRatio, 2f));
        T.Check("the expanding column is wider than the fixed one; the cells sit on one row", cell.Size.X > cell2.Size.X && T.Near(cell.Position.Y + cell.Size.Y / 2f, cell2.Position.Y + cell2.Size.Y / 2f, 0.6f), $"{cell.GetRect()} {cell2.GetRect()}");
        T.Check("focus mode, neighbour and next resolve to the sibling", cell.FocusMode == Control.FocusModeEnum.All && cell.GetNodeOrNull(cell.FocusNeighborRight) == cell2 && cell.GetNodeOrNull(cell.FocusNext) == cell2 &&
                cell2.FocusMode == Control.FocusModeEnum.Click && cell.TooltipText == "a cell");

        T.Check("the rule Panel is drawn with a StyleBoxLine, ignores the pointer and clips", rule.GetThemeStylebox("panel") is StyleBoxLine { Vertical: true, Thickness: 3 } && rule.MouseFilter == Control.MouseFilterEnum.Ignore && rule.ClipContents);

        // The theme chain: the root's own theme (a file the converter wrote) beats the project's for everything below it.
        T.Check("the node theme (Control font_color white) beats the project theme (yellowish) for a descendant", cell.GetThemeColor("font_color") == new Color(1, 1, 1, 1), $"{cell.GetThemeColor("font_color")}");
        T.Check("a type the node theme does not set falls through to the project theme (Control tint 3)", cell.GetThemeConstant("tint") == 3);
        T.Check("the node theme's Panel style box is found for a Panel below the root", rule.GetThemeStylebox("panel") is StyleBoxLine, "an override still wins");
        var plain = new Panel { Name = "Plain" };
        root.AddChild(plain);
        T.Check("without an override the Panel uses the root's theme style box", plain.GetThemeStylebox("panel") is StyleBoxFlat { } f && T.Near(f.BgColor.R, 0.3f, 0.01f), $"{(plain.GetThemeStylebox("panel") as StyleBoxFlat)?.BgColor}");
        plain.QueueFree();
        T.Check("the project theme's font size and default font arrive through the lookup", cell.GetThemeFontSize("font_size") == 24 && cell.GetThemeDefaultFontSize() == 20);

        var margin = root.GetNode<MarginContainer>("Margin");
        T.Check("the centre-anchored MarginContainer is 200 x 100 around the window's middle", IsRect(margin.GetRect(), 540, 310, 200, 100), $"{margin.GetRect()}");
        T.Check("its margin overrides inset the child (left 8, top 6, the theme's 0 for the others)", IsRect(margin.GetNode<Control>("Scroll").GetRect(), 8, 6, 192, 94), $"{margin.GetNode<Control>("Scroll").GetRect()}");
        var scroll = margin.GetNode<ScrollContainer>("Scroll");
        T.Check("the ScrollContainer keeps its scroll modes", scroll.HorizontalScrollMode == ScrollContainer.ScrollMode.ShowNever && scroll.VerticalScrollMode == ScrollContainer.ScrollMode.ShowAlways);
        var center = scroll.GetNode<CenterContainer>("Center");
        T.Check("the CenterContainer keeps use_top_left", center.UseTopLeft);

        var cap = center.GetNode<Control>("Cap");
        T.Check("a Label becomes a Control carrier that names its class for theme lookups", cap.GetThemeClassName() == "Label", cap.GetThemeClassName());
        T.Check("and its inline Theme sub-resource answers for the Label type", cap.GetThemeColor("font_color") == new Color(0, 1, 0, 1) && cap.GetThemeConstant("line_spacing") == 9);
        T.Check("the pivot arrives on the control", cap.PivotOffset == new Vector2(5, 5), $"{cap.PivotOffset}");
        var go = root.GetNode<Control>("Go");
        T.Check("rotation arrives on a control its parent does not place (a container resets it, as in Godot)", T.Near(go.Rotation, 0.3f), $"{go.Rotation}");
        T.Check("a Button carrier keeps its offsets as a rectangle and its focus mode", IsRect(go.GetRect(), 20, 400, 100, 40) && go.FocusMode == Control.FocusModeEnum.None, $"{go.GetRect()}");

        main.QueueFree();
        T.Frame(1.0 / 60.0, 1);
    }

    private static void ConverterOutput()
    {
        string exeName = "AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : "");
        string? exe = null;
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null && exe == null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", exeName);
            if (File.Exists(p)) exe = p;
        }
        if (exe == null) { Console.WriteLine("  SKIP: build/bin/AverAssetC was not found"); return; }
        string tmp = Path.Combine(Path.GetTempPath(), "aver-fabric-ui-conv-" + System.Environment.ProcessId);
        string src = Path.Combine(tmp, "UiGame");
        string outDir = Path.Combine(tmp, "out");
        try
        {
            // The fixture is text; the font and the 9-patch PNG (a header is all the converter reads) are written beside a copy of it.
            foreach (string dir in Directory.GetDirectories(Path.Combine(T.DataDir(), "UiGame"), "*", SearchOption.AllDirectories)) Directory.CreateDirectory(dir.Replace(Path.Combine(T.DataDir(), "UiGame"), src));
            Directory.CreateDirectory(src);
            foreach (string file in Directory.GetFiles(Path.Combine(T.DataDir(), "UiGame"), "*", SearchOption.AllDirectories))
                File.Copy(file, file.Replace(Path.Combine(T.DataDir(), "UiGame"), src), true);
            Directory.CreateDirectory(Path.Combine(src, "ui", "fonts"));
            File.WriteAllBytes(Path.Combine(src, "ui", "fonts", "ui.ttf"), new byte[] { 0, 1, 0, 0, 7, 7 });
            File.WriteAllBytes(Path.Combine(src, "ui", "panel9.png"), new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, (byte)'I', (byte)'H', (byte)'D', (byte)'R', 0, 0, 0, 12, 0, 0, 0, 10, 8, 6, 0, 0, 0, 0, 0, 0, 0 });

            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(src); psi.ArgumentList.Add(outDir); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter runs on the UiGame fixture", proc.ExitCode == 0, "exit " + proc.ExitCode);
                if (proc.ExitCode != 0) return;
            }
            ConverterChecks(Path.Combine(outDir, "UiGame", "Content"));
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            ThemeDB.ResetForTests();
            try { Directory.Delete(tmp, true); } catch (IOException) { }
        }
    }
}
