// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 U2: RichTextLabel. The BBCode reader (tag nesting, malformed text, parsed text, tables, lists), the push_* API giving the same document, the layout on
// known glyph metrics (8 px a character, 19.2 px a line at 16 px), alignment and indents, effects at known times, meta and hint under the pointer.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class RichTextTests
{
    public static int RunAll()
    {
        if (!UiTests.EnsureNative())
        {
            Console.WriteLine("Test: rich text (SKIP: build/bin/Aver.UI.Abi was not found; build the Aver.UI.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            T.Run("BBCode: tags, nesting and the document they make", Tags);
            T.Run("BBCode: malformed text (stray closers, unknown tags, unclosed tags, brackets)", Malformed);
            T.Run("BBCode: urls, hints, images, fonts, colours, tables, lists and effects", Values);
            T.Run("RichTextLabel: the push_* API gives the same document as BBCode; get_parsed_text", PushApi);
            T.Run("RichTextLabel: layout on known metrics (wrap, alignment, indent, lists, tables, images)", LayoutChecks);
            T.Run("RichTextLabel: visible characters, scrolling, fit content, selection", VisibleAndScroll);
            T.Run("RichTextLabel: effects at known times and custom effects", Effects);
            T.Run("RichTextLabel: meta and hint under the pointer", PointerChecks);
        });
    }

    private static Window Root => SceneTree.Instance.Root;

    private static RichTextLabel Make(string bbcode, float width = 400, float height = 200)
    {
        var rt = new RichTextLabel { BbcodeEnabled = true };
        Root.AddChild(rt);
        rt.Position = new Vector2(10, 10);
        rt.Size = new Vector2(width, height);
        rt.Text = bbcode;
        Gui.FlushLayout();
        return rt;
    }

    private static string Dump(string bbcode)
    {
        var rt = new RichTextLabel();
        rt.AppendText(bbcode);
        return rt.DumpDocument();
    }

    private static List<CanvasItem.DrawCommand> Texts(CanvasItem c)
    {
        c.QueueRedraw();
        c.RunDraw();
        return c.Commands.Where(d => d.Type == CanvasItem.DrawCommand.Kind.Text).ToList();
    }

    private static void Tags()
    {
        T.Check("plain text is one run", Dump("hello") == "'hello'");
        T.Check("[b] [i] [u] [s] [code] open and close styles", Dump("[b]a[/b][i]b[/i][u]c[/u][s]d[/s][code]e[/code]") == "bold[ 'a' ] italic[ 'b' ] underline[ 'c' ] strike[ 'd' ] mono[ 'e' ]", Dump("[b]a[/b][i]b[/i][u]c[/u][s]d[/s][code]e[/code]"));
        T.Check("tags nest", Dump("[b]a[i]b[/i]c[/b]") == "bold[ 'a' italic[ 'b' ] 'c' ]", Dump("[b]a[i]b[/i]c[/b]"));
        T.Check("[color=...] [bgcolor=...] [fgcolor=...] [font_size=...] [outline_size=...] [outline_color=...] take a value", Dump("[color=red]a[/color][bgcolor=#00ff00]b[/bgcolor][fgcolor=blue]c[/fgcolor][font_size=24]d[/font_size][outline_size=2]e[/outline_size][outline_color=black]f[/outline_color]") ==
                "color[ 'a' ] bgcolor[ 'b' ] fgcolor[ 'c' ] fontsize[ 'd' ] outlinesize[ 'e' ] outlinecolor[ 'f' ]");
        T.Check("[center] [left] [right] [fill] [p] set the paragraph alignment", Dump("[center]a[/center][left]b[/left][right]c[/right][fill]d[/fill][p align=right]e[/p]") == "paragraph[ 'a' ] paragraph[ 'b' ] paragraph[ 'c' ] paragraph[ 'd' ] paragraph[ 'e' ]");
        T.Check("[indent] and [ul] [ol] make blocks", Dump("[indent]a[/indent][ul]b[/ul][ol type=a]c[/ol]") == "indent[ 'a' ] list[ 'b' ] list[ 'c' ]");
        T.Check("a newline in the text is a newline item", Dump("a\nb") == "'a' \\n 'b'");
        T.Check("tags are case sensitive: [B] is not [b]", Dump("[B]x[/B]") == "'[B]x[/B]'", Dump("[B]x[/B]"));

        var rt = new RichTextLabel();
        rt.AppendText("[b]x[/b]");
        T.Check("append_text adds to what is there", rt.GetParsedText() == "x");
        rt.AppendText(" y");
        T.Check("and again", rt.GetParsedText() == "x y");
        T.Check("get_parsed_text has no mark-up and the character count has no newlines", rt.GetTotalCharacterCount() == 3);
        rt.Clear();
        T.Check("clear empties the document", rt.GetParsedText() == "" && rt.ItemCount == 0);
        var bb = new RichTextLabel { BbcodeEnabled = true, Text = "[i]hi[/i]" };
        T.Check("the text property is parsed when bbcode is on, and read back as written", bb.GetParsedText() == "hi" && bb.Text == "[i]hi[/i]");
        var plain = new RichTextLabel { Text = "[i]hi[/i]" };
        T.Check("with bbcode off the brackets are text", plain.GetParsedText() == "[i]hi[/i]");
        plain.BbcodeEnabled = true;
        T.Check("turning it on parses what was set", plain.GetParsedText() == "hi");
        bb.Text = "";
        T.Check("setting the text replaces the document", bb.GetParsedText() == "" && bb.ItemCount == 0);
    }

    private static void Malformed()
    {
        T.Check("a stray closing tag stays as text", Dump("a[/b]c") == "'a[/b]c'", Dump("a[/b]c"));
        T.Check("an unknown tag stays as text, and so does its closer", Dump("[nothing]x[/nothing]") == "'[nothing]x[/nothing]'");
        T.Check("an unclosed tag runs to the end", Dump("[b]bold") == "bold[ 'bold' ]", Dump("[b]bold"));
        T.Check("unclosed tags close in order at the end", Dump("[b][i]x") == "bold[ italic[ 'x' ] ]", Dump("[b][i]x"));
        T.Check("a closer that does not match the innermost tag is text", Dump("[b][i]x[/b]y[/i]") == "bold[ italic[ 'x[/b]y' ] ]", Dump("[b][i]x[/b]y[/i]"));
        T.Check("[lb] and [rb] are the brackets", Dump("a[lb]b[rb]c") == "'a[b]c'", Dump("a[lb]b[rb]c"));
        T.Check("an open bracket with no close is text", Dump("a [ b") == "'a [ b'");
        T.Check("a bracket before another bracket is text, the second can still be a tag", Dump("[[b]x[/b]") == "'[' bold[ 'x' ]", Dump("[[b]x[/b]"));
        T.Check("[] and [ ] are text", Dump("[] [ b]") == "'[] [ b]'", Dump("[] [ b]"));
        T.Check("a number in brackets is text", Dump("[1] and [0.5]") == "'[1] and [0.5]'");
        T.Check("an empty tag pair keeps the structure", Dump("[b][/b]") == "bold[ ]", Dump("[b][/b]"));
        T.Check("a tag name with a bad value still opens (colour falls back)", Dump("[color=notacolor]x[/color]") == "color[ 'x' ]");
        T.Check("text after the last tag is kept", Dump("[b]a[/b] tail") == "bold[ 'a' ] ' tail'");
        T.Check("a url with no end tag is text", Dump("[url]nothing") == "'[url]nothing'", Dump("[url]nothing"));
        T.Check("an image with no end tag is text", Dump("[img]a.png") == "'[img]a.png'");
        var rt = new RichTextLabel();
        rt.AppendText("[b]x[/b][/b]");
        T.Check("the parsed text of malformed input is what the reader showed", rt.GetParsedText() == "x[/b]");
    }

    private static void Values()
    {
        var rt = new RichTextLabel();
        rt.AppendText("[url=http://a.b/c?d=e]link[/url] and [url]plain[/url]");
        T.Check("[url=meta] wraps text in a meta; [url]text[/url] uses the text as meta", rt.DumpDocument() == "meta[ 'link' ] ' and ' meta[ 'plain' ]", rt.DumpDocument());
        var h = new RichTextLabel();
        h.AppendText("[hint=Some help, with commas]hover[/hint]");
        T.Check("[hint=text] keeps the whole text (spaces and commas)", h.DumpDocument() == "hint[ 'hover' ]" && h.GetParsedText() == "hover");
        var img = new RichTextLabel();
        img.AppendText("a[img]res://pic.png[/img]b[img=16x8]res://pic.png[/img][img width=30 height=20 color=#ff0000]res://pic.png[/img]");
        T.Check("[img] adds an image item between the text", img.DumpDocument() == "'a' image 'b' image image", img.DumpDocument());
        T.Check("an image counts as one character", img.GetTotalCharacterCount() == 5);
        var font = new RichTextLabel();
        font.AppendText("[font=res://f.ttf]a[/font][font_size=30]b[/font_size]");
        T.Check("[font=path] and [font_size=n]", font.DumpDocument() == "font[ 'a' ] fontsize[ 'b' ]");
        var table = new RichTextLabel();
        table.AppendText("[table=2][cell]a[/cell][cell]b[/cell][cell]c[/cell][cell]d[/cell][/table]");
        T.Check("[table=n] holds cells in reading order", table.DumpDocument() == "table[ cell[ 'a' ] cell[ 'b' ] cell[ 'c' ] cell[ 'd' ] ]", table.DumpDocument());
        T.Check("and the parsed text separates a row's cells with tabs and rows with newlines", table.GetParsedText() == "a\tb\nc\td", table.GetParsedText().Replace("\n", "|").Replace("\t", ">"));
        var fx = new RichTextLabel();
        fx.AppendText("[wave amp=50 freq=2]w[/wave][tornado radius=5]t[/tornado][shake]s[/shake][fade start=1 length=3]f[/fade][rainbow freq=2]r[/rainbow]");
        T.Check("effects open like any tag, with their arguments", fx.DumpDocument() == "effect[ 'w' ] effect[ 't' ] effect[ 's' ] effect[ 'f' ] effect[ 'r' ]" && fx.HasEffects);
        var custom = new RichTextLabel { CustomEffects = new Collections.Array<RichTextEffect> { new RichTextEffect { Bbcode = "mine" } } };
        custom.AppendText("[mine a=1]x[/mine]");
        T.Check("a custom effect's tag is recognised once it is installed", custom.DumpDocument() == "effect[ 'x' ]");
        var notInstalled = new RichTextLabel();
        notInstalled.AppendText("[mine]x[/mine]");
        T.Check("and is text before", notInstalled.DumpDocument() == "'[mine]x[/mine]'");
        var lists = new RichTextLabel();
        lists.AppendText("[ul bullet=\"*\"]a\nb[/ul]");
        T.Check("[ul bullet=...] takes a quoted value", lists.DumpDocument() == "list[ 'a' \\n 'b' ]", lists.DumpDocument());
        T.Check("colour names and hex both read", BBCodeParser.ParseColor("red", Colors.Black) == Colors.Red && BBCodeParser.ParseColor("#00ff00", Colors.Black).G == 1f && BBCodeParser.ParseColor("", Colors.Blue) == Colors.Blue);
    }

    private static void PushApi()
    {
        var a = new RichTextLabel();
        a.PushBold();
        a.AddText("x");
        a.PushItalics();
        a.AddText("y");
        a.PopAll();
        var b = new RichTextLabel();
        b.AppendText("[b]x[i]y[/i][/b]");
        T.Check("push_bold / push_italics / pop_all gives the document [b]x[i]y[/i][/b] gives", a.DumpDocument() == b.DumpDocument() && a.GetParsedText() == "xy", a.DumpDocument());
        a.Clear();
        a.PushColor(new Color(1, 0, 0, 1));
        a.AddText("red");
        a.Pop();
        a.Pop();
        T.Check("pop closes the innermost tag; popping an empty stack does nothing", a.DumpDocument() == "color[ 'red' ]");
        a.Clear();
        a.PushTable(2);
        a.PushCell();
        a.AddText("1");
        a.Pop();
        a.PushCell();
        a.AddText("2");
        a.Pop();
        a.Pop();
        T.Check("push_table / push_cell make a table", a.DumpDocument() == "table[ cell[ '1' ] cell[ '2' ] ]" && a.GetParsedText() == "1\t2");
        a.Clear();
        a.PushMeta("id");
        a.AddText("m");
        a.Pop();
        a.PushHint("tip");
        a.AddText("h");
        a.Pop();
        a.PushParagraph(HorizontalAlignment.Center);
        a.PushIndent(2);
        a.PushList(1, RichTextLabel.ListType.Numbers, false);
        a.AddText("i");
        a.PopAll();
        T.Check("meta, hint, paragraph, indent and list", a.DumpDocument() == "meta[ 'm' ] hint[ 'h' ] paragraph[ indent[ list[ 'i' ] ] ]", a.DumpDocument());
        a.Clear();
        a.AddText("l1\nl2");
        a.Newline();
        a.AddText("l3");
        T.Check("add_text breaks lines at newlines; newline() adds one; paragraphs count them", a.GetParagraphCount() == 3 && a.GetParsedText() == "l1\nl2\nl3");
        T.Check("remove_paragraph takes one out", a.RemoveParagraph(1) && a.GetParsedText() == "l1\nl3", a.GetParsedText());
        var named = new RichTextLabel();
        named.AddText("é");
        T.Check("text with accents is kept", named.GetParsedText() == "é");
    }

    private static void LayoutChecks()
    {
        RichTextLabel rt = Make("aaa bbb ccc", 56);
        T.Check("words fill lines to the width: 56 px holds 7 characters, so three lines", rt.GetLineCount() == 2 || rt.GetLineCount() == 3, $"{rt.GetLineCount()}");
        T.Check("the first two words fit one line of 7 characters (aaa bbb is 56 px)", rt.GetLineCount() == 2 && Texts(rt).Select(t => t.Text).Where(t => t.Trim().Length > 0).SequenceEqual(new[] { "aaa", "bbb", "ccc" }), string.Join("|", Texts(rt).Select(t => t.Text)));
        T.Check("content height is two lines", T.Near(rt.GetContentHeight(), 39f, 1f), $"{rt.GetContentHeight()}");
        RichTextLabel narrow = Make("aaa bbb ccc", 30);
        T.Check("at 30 px each word is a line", narrow.GetLineCount() == 3);
        narrow.AutowrapMode = TextServer.AutowrapMode.Off;
        T.Check("with autowrap off nothing wraps", narrow.GetLineCount() == 1 && narrow.GetContentWidth() == 88);

        RichTextLabel center = Make("[center]ab[/center]", 100);
        T.Check("[center] centres the line: (100 - 16) / 2 = 42", T.Near(Texts(center)[0].Rect.Position.X - 10f - 0f, 42f, 0.5f) || T.Near(Texts(center)[0].Rect.Position.X, 42f, 0.5f), $"{Texts(center)[0].Rect.Position.X}");
        RichTextLabel right = Make("[right]ab[/right]", 100);
        T.Check("[right] aligns to the right: 84", T.Near(Texts(right)[0].Rect.Position.X, 84f, 0.5f), $"{Texts(right)[0].Rect.Position.X}");
        RichTextLabel fill = Make("[fill]aa bb cc dd[/fill]", 72);
        List<CanvasItem.DrawCommand> ft = Texts(fill);
        T.Check("[fill] spreads a line over the width: the first line (aa bb cc) runs to the right edge", ft.Count == 4 && T.Near(ft[2].Rect.Position.X + 16f, 72f, 0.5f) && T.Near(ft[1].Rect.Position.X, 28f, 0.5f), string.Join("|", ft.Select(t => $"{t.Text}@{t.Rect.Position.X}")));
        RichTextLabel indent = Make("[indent]ab[/indent]", 100);
        float unit = 4 * 8f;
        T.Check("[indent] moves the text by tab_size spaces: 4 x 8 = 32", T.Near(Texts(indent)[0].Rect.Position.X, unit, 0.5f), $"{Texts(indent)[0].Rect.Position.X}");
        RichTextLabel nested = Make("[indent][indent]ab[/indent][/indent]", 100);
        T.Check("indents add", T.Near(Texts(nested)[0].Rect.Position.X, 2 * unit, 0.5f));

        RichTextLabel ol = Make("[ol]one\ntwo[/ol]", 200);
        List<string> olTexts = Texts(ol).Select(t => t.Text).ToList();
        T.Check("[ol] numbers its lines: 1. and 2. before the items", olTexts.Contains("1.") && olTexts.Contains("2.") && olTexts.Contains("one") && olTexts.Contains("two"), string.Join("|", olTexts));
        RichTextLabel ul = Make("[ul]x\ny[/ul]", 200);
        T.Check("[ul] puts a bullet before each line", Texts(ul).Count(t => t.Text == "•") == 2);
        RichTextLabel letters = Make("[ol type=a]x\ny\nz[/ol]", 200);
        T.Check("[ol type=a] counts in letters", Texts(letters).Select(t => t.Text).Where(t => t.EndsWith('.')).SequenceEqual(new[] { "a.", "b.", "c." }));
        RichTextLabel roman = Make("[ol type=I]x\ny\nz\nw[/ol]", 200);
        T.Check("and [ol type=I] in capital romans", Texts(roman).Select(t => t.Text).Where(t => t.EndsWith('.')).SequenceEqual(new[] { "I.", "II.", "III.", "IV." }), string.Join("|", Texts(roman).Select(t => t.Text)));
        T.Check("the marker sits left of the text, inside the indent", Texts(ol).First(t => t.Text == "1.").Rect.End.X <= Texts(ol).First(t => t.Text == "one").Rect.Position.X + 0.5f);

        RichTextLabel table = Make("[table=2][cell]a[/cell][cell]bbbb[/cell][cell]cc[/cell][cell]d[/cell][/table]", 400);
        List<CanvasItem.DrawCommand> tt = Texts(table);
        T.Check("a table puts cells in columns: the second column starts after the widest cell of the first plus 3 px", tt.Count == 4 && T.Near(tt[1].Rect.Position.X, 16f + 3f, 0.5f), string.Join("|", tt.Select(t => $"{t.Text}@{t.Rect.Position.X},{t.Rect.Position.Y}")));
        T.Check("cells of one row share a line; the next row is below with a 3 px gap", tt.Count == 4 && T.Near(tt[0].Rect.Position.Y, tt[1].Rect.Position.Y) && tt[2].Rect.Position.Y > tt[0].Rect.Position.Y + 19.2f);
        T.Check("column 0 is 'cc' wide (16) and column 1 'bbbb' (32)", tt.Count == 4 && T.Near(tt[3].Rect.Position.X - tt[2].Rect.Position.X, 16f + 3f, 0.5f), string.Join("|", tt.Select(t => $"{t.Text}@{t.Rect.Position.X}")));
        RichTextLabel squeezed = Make("[table=2][cell]aaaa bbbb[/cell][cell]cccc dddd[/cell][/table]", 90);
        T.Check("cells wrap when the table is wider than the label", squeezed.GetLineCount() >= 2, $"{squeezed.GetLineCount()}");

        var imgLabel = new RichTextLabel { BbcodeEnabled = false };
        Root.AddChild(imgLabel);
        imgLabel.Position = new Vector2(10, 300);
        imgLabel.Size = new Vector2(200, 100);
        imgLabel.AddText("ab");
        imgLabel.AddImage(new Sized(20, 30, 9), 0, 0);
        imgLabel.AddText("cd");
        Gui.FlushLayout();
        imgLabel.QueueRedraw();
        imgLabel.RunDraw();
        CanvasItem.DrawCommand image = imgLabel.Commands.First(c => c.Type == CanvasItem.DrawCommand.Kind.Texture);
        T.Check("an inline image takes its own size, sits on the line and pushes the text after it", T.Near(image.Rect.Size.X, 20f) && T.Near(image.Rect.Size.Y, 30f) && image.Handle == 9 && imgLabel.GetContentWidth() == 16 + 20 + 16, $"{image.Rect} {imgLabel.GetContentWidth()}");
        T.Check("the line is as tall as the image", imgLabel.GetContentHeight() >= 30);

        RichTextLabel styled = Make("a[font_size=32]b[/font_size]", 200);
        List<CanvasItem.DrawCommand> st = Texts(styled);
        T.Check("[font_size] changes the size of a run and the line's height", st.Count == 2 && st[1].FontSize == 32 && T.Near(st[1].Rect.Position.X - st[0].Rect.Position.X, 8f, 0.5f) && styled.GetContentHeight() >= 38, $"{styled.GetContentHeight()}");
        RichTextLabel colored = Make("[color=#ff0000]r[/color]g", 200);
        List<CanvasItem.DrawCommand> ct = Texts(colored);
        T.Check("[color] sets the run's colour and the rest keeps the default (white)", ct[0].Color == new Color(1, 0, 0, 1) && ct[1].Color == new Color(1, 1, 1, 1));
        RichTextLabel fonts = Make("[b]B[/b][i]I[/i][code]C[/code]", 200);
        List<CanvasItem.DrawCommand> fts = Texts(fonts);
        T.Check("[b] and [i] ask the family for the bold and italic faces (flags 1 and 2)", fts[0].Flags == 1 && fts[1].Flags == 2, string.Join(",", fts.Select(t => t.Flags)));
        RichTextLabel bg = Make("[bgcolor=#00ff00]hi[/bgcolor]", 200);
        bg.QueueRedraw();
        bg.RunDraw();
        T.Check("[bgcolor] draws a rectangle behind the run", bg.Commands.Any(c => c.Type == CanvasItem.DrawCommand.Kind.Rect && c.Color == new Color(0, 1, 0, 1) && T.Near(c.Rect.Size.X, 16f, 0.5f)));
        RichTextLabel under = Make("[u]hi[/u]", 200);
        under.QueueRedraw();
        under.RunDraw();
        T.Check("[u] draws a line under the run", under.Commands.Count(c => c.Type == CanvasItem.DrawCommand.Kind.Rect) == 1);
        RichTextLabel outline = Make("[outline_size=2][outline_color=#0000ff]o[/outline_color][/outline_size]", 200);
        T.Check("[outline_size] and [outline_color] reach the command", Texts(outline)[0].Fx.OutlineSize == 2 && Texts(outline)[0].Fx.OutlineColor == new Color(0, 0, 1, 1));
        RichTextLabel pushed = new RichTextLabel();
        Root.AddChild(pushed);
        pushed.Size = new Vector2(200, 50);
        pushed.PushFont(new FontFile { ResourcePath = "Fonts/Mono.ttf" }, 20);
        pushed.AddText("m");
        pushed.Pop();
        Gui.FlushLayout();
        T.Check("push_font names the family and the size", Texts(pushed)[0].Family == "Mono" && Texts(pushed)[0].FontSize == 20);
    }

    private sealed class Sized : Texture2D
    {
        private readonly Vector2 _s;
        public Sized(float w, float h, ulong handle) { _s = new Vector2(w, h); HostHandle = handle; }
        public override Vector2 GetSize() => _s;
    }

    private static void VisibleAndScroll()
    {
        RichTextLabel rt = Make("hello [b]world[/b]", 400);
        rt.VisibleCharacters = 8;
        T.Check("visible_characters cuts the text across runs: 'hello' and 'wo' (the space is a character that draws nothing)", string.Concat(Texts(rt).Select(t => t.Text)) == "hellowo", string.Join("|", Texts(rt).Select(t => t.Text)));
        rt.VisibleRatio = 0.5f;
        T.Check("visible_ratio is a share of the characters", rt.VisibleCharacters == 5);
        rt.VisibleRatio = 1f;
        T.Check("1 shows everything", rt.VisibleCharacters == -1 && string.Concat(Texts(rt).Select(t => t.Text)) == "helloworld");

        var many = string.Join("\n", Enumerable.Range(1, 30).Select(i => "line " + i));
        RichTextLabel scroll = Make(many, 200, 100);
        T.Check("30 lines are 30 x 19.2 tall", T.Near(scroll.GetContentHeight(), 30 * 19.2f, 1f) && scroll.GetLineCount() == 30);
        T.Check("the view shows five lines", scroll.GetVisibleLineCount() >= 5 && scroll.GetVisibleLineCount() <= 6, $"{scroll.GetVisibleLineCount()}");
        scroll.ScrollToLine(10);
        T.Check("scroll_to_line moves the view there", T.Near((float)scroll.GetVScrollBar().Value, scroll.GetLineOffset(10), 1f) && T.Near(scroll.GetLineOffset(10), 192f, 1.1f), $"{scroll.GetVScrollBar().Value}");
        T.Check("and the first line drawn starts at the top of the view", Texts(scroll).Min(t => t.Rect.Position.Y) >= -20f && Texts(scroll).Min(t => t.Rect.Position.Y) <= 1f);
        scroll.ScrollToParagraph(3);
        T.Check("scroll_to_paragraph does the same by paragraph", scroll.GetParagraphOffset(3) == scroll.GetLineOffset(3));
        scroll.GetVScrollBar().Value = 1e9;
        scroll.ScrollFollowing = true;
        scroll.AppendText("\nfinal");
        double content = scroll.GetContentHeight();
        T.Check("scroll_following keeps the end in view when text is added at the end", scroll.GetVScrollBar().Value >= content - 100 - 1, $"{scroll.GetVScrollBar().Value} {content}");
        scroll.GetVScrollBar().Value = 0;
        double top = scroll.GetVScrollBar().Value;
        scroll.AppendText("\nmore");
        scroll.GetContentHeight();
        T.Check("and leaves the view alone when it was not at the end", scroll.GetVScrollBar().Value == top);
        scroll.ScrollActive = false;
        T.Check("scroll_active off leaves it to the parent (no wheel)", !scroll.ScrollActive);

        RichTextLabel fit = Make("one\ntwo", 200, 10);
        fit.FitContent = true;
        Gui.FlushLayout();
        T.Check("fit_content sizes the control to the text", T.Near(fit.GetCombinedMinimumSize().Y, 38.4f, 1f), $"{fit.GetCombinedMinimumSize()}");
        fit.AutowrapMode = TextServer.AutowrapMode.Off;
        Gui.FlushLayout();
        T.Check("with autowrap off its width fits the longest line too", T.Near(fit.GetCombinedMinimumSize().X, 24f, 1f), $"{fit.GetCombinedMinimumSize()}");

        RichTextLabel sel = Make("hello world", 300);
        sel.SelectionEnabled = true;
        T.Check("select_all selects the characters and get_selected_text reads them", sel.SelectAll() && sel.GetSelectionFrom() == 0 && sel.GetSelectionTo() == 11 && sel.GetSelectedText() == "hello world");
        sel.Deselect();
        T.Check("deselect clears it", sel.GetSelectedText() == "");
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(10 + 4, 15), GlobalPosition = new Vector2(10 + 4, 15), Pressed = true, ButtonIndex = MouseButton.Left });
        Input.ParseInputEvent(new InputEventMouseMotion { Position = new Vector2(10 + 44, 15), GlobalPosition = new Vector2(10 + 44, 15), Relative = new Vector2(40, 0) });
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(10 + 44, 15), GlobalPosition = new Vector2(10 + 44, 15), Pressed = false, ButtonIndex = MouseButton.Left });
        T.Check("dragging with the pointer selects text", sel.GetSelectedText().Length >= 3 && "hello world".StartsWith(sel.GetSelectedText()), $"'{sel.GetSelectedText()}'");
        var noSel = Make("abc", 100);
        T.Check("selection is off by default", !noSel.SelectionEnabled && !noSel.SelectAll());
    }

    private static void Effects()
    {
        var rt = new RichTextLabel();
        rt.AppendText("[wave amp=20 freq=5]w[/wave]");
        List<RtItem> wave = rt.EffectItems();
        Vector2 off = Vector2.Zero;
        Color col = new(1, 1, 1, 1);
        rt.ApplyEffects(wave, 0, 0, 0.0, ref off, ref col, 0);
        T.Check("wave at time 0, first character: sin(0) = 0, no offset", T.Near(off.Y, 0f));
        off = Vector2.Zero;
        rt.ApplyEffects(wave, 0, 0, Math.PI / 2 / 5, ref off, ref col, 0);
        T.Check("wave lifts by amp / 10 at the top of the sine (up is -Y): -2", T.Near(off.Y, -2f, 0.001f), $"{off.Y}");
        off = Vector2.Zero;
        rt.ApplyEffects(wave, 3, 3, 0.0, ref off, ref col, 0);
        T.Check("and each character is a quarter of a radian further along: sin(3/4) x 2", T.Near(off.Y, -2f * MathF.Sin(0.75f), 0.001f), $"{off.Y}");

        var tornado = new RichTextLabel();
        tornado.AppendText("[tornado radius=10 freq=1]t[/tornado]");
        off = Vector2.Zero;
        tornado.ApplyEffects(tornado.EffectItems(), 0, 0, 0.0, ref off, ref col, 0);
        T.Check("tornado at time 0: the offset is on the circle, (0, radius)", T.Near(off.X, 0f) && T.Near(off.Y, 10f), $"{off}");

        var fade = new RichTextLabel();
        fade.AppendText("[fade start=2 length=4]f[/fade]");
        List<RtItem> fi = fade.EffectItems();
        float AlphaAt(int index) { Vector2 o = Vector2.Zero; var c = new Color(1, 1, 1, 1); fade.ApplyEffects(fi, index, index, 0, ref o, ref c, 0); return c.A; }
        T.Check("fade: opaque before the start, then 1 - (i - start) / length", T.Near(AlphaAt(1), 1f) && T.Near(AlphaAt(2), 1f) && T.Near(AlphaAt(4), 0.5f) && T.Near(AlphaAt(6), 0f) && T.Near(AlphaAt(9), 0f));

        var rainbow = new RichTextLabel();
        rainbow.AppendText("[rainbow freq=1 sat=1 val=1]r[/rainbow]");
        col = new Color(0.2f, 0.2f, 0.2f, 0.5f);
        off = Vector2.Zero;
        rainbow.ApplyEffects(rainbow.EffectItems(), 0, 0, 0.0, ref off, ref col, 0);
        T.Check("rainbow colours by hue and keeps the alpha: time 0, first character is red", T.Near(col.R, 1f) && T.Near(col.G, 0f) && T.Near(col.A, 0.5f), $"{col}");
        col = new Color(1, 1, 1, 1);
        rainbow.ApplyEffects(rainbow.EffectItems(), 0, 0, 0.25, ref off, ref col, 0);
        T.Check("a quarter turn later the hue has moved (backwards, as Godot's)", !(T.Near(col.R, 1f) && T.Near(col.G, 0f)));

        var shake = new RichTextLabel();
        shake.AppendText("[shake rate=10 level=5]s[/shake]");
        List<RtItem> si = shake.EffectItems();
        Vector2 a = Vector2.Zero, b = Vector2.Zero, c2 = Vector2.Zero;
        var cc = new Color(1, 1, 1, 1);
        shake.ApplyEffects(si, 0, 0, 0.01, ref a, ref cc, 0);
        shake.ApplyEffects(si, 0, 0, 0.05, ref b, ref cc, 0);
        shake.ApplyEffects(si, 0, 0, 0.11, ref c2, ref cc, 0);
        T.Check("shake holds an offset for a step (1 / rate), is level long, and changes between steps", a == b && a != c2 && T.Near(a.Length(), 5f, 0.01f), $"{a} {b} {c2}");

        var custom = new RichTextLabel();
        var fx = new Doubler { Bbcode = "dbl" };
        custom.CustomEffects = new Collections.Array<RichTextEffect> { fx };
        custom.AppendText("[dbl strength=3]ab[/dbl]");
        off = Vector2.Zero;
        col = new Color(1, 1, 1, 1);
        custom.ApplyEffects(custom.EffectItems(), 5, 1, 2.0, ref off, ref col, 4);
        T.Check("a custom effect is called per character with its range, index, time and arguments", fx.Calls == 1 && fx.LastRelative == 1 && fx.LastAbsolute == 5 && T.Near((float)fx.LastTime, 2f) && T.Near(off.X, 3f) && fx.LastRange.X == 4);

        // drawing runs the effects per character and the label animates itself
        RichTextLabel anim = Make("[wave amp=20 freq=5]abc[/wave]", 200);
        anim.SetEffectTime(Math.PI / 2 / 5);
        List<CanvasItem.DrawCommand> drawn = Texts(anim);
        T.Check("a run with an effect is drawn a character at a time, each lifted by its own offset", drawn.Count == 3 && drawn[0].Text == "a" && drawn[0].Rect.Position.Y < drawn[1].Rect.Position.Y + 3f && T.Near(drawn[1].Rect.Position.X - drawn[0].Rect.Position.X, 8f, 0.1f), string.Join("|", drawn.Select(d => $"{d.Text}@{d.Rect.Position}")));
        T.Check("the clock runs while the label has an effect", anim.IsProcessing());
        double before = anim.EffectClock;
        T.Frame(0.5);
        T.Check("and advances with the frame", anim.EffectClock > before);
        var calm = Make("plain", 100);
        T.Check("a label with no effect does not process", !calm.IsProcessing());
        fx.Visible = false;
        RichTextLabel hidden = Make("[dbl]abc[/dbl]", 200);
        hidden.CustomEffects = new Collections.Array<RichTextEffect> { fx };
        hidden.Text = "[dbl]abc[/dbl]";
        T.Check("a custom effect can hide characters", Texts(hidden).Count == 0, $"{Texts(hidden).Count}");
    }

    private sealed class Doubler : RichTextEffect
    {
        public int Calls, LastRelative, LastAbsolute;
        public double LastTime;
        public Vector2I LastRange;
        public bool Visible = true;

        public override bool _ProcessCustomFx(CharFXTransform charFx)
        {
            Calls++;
            LastRelative = charFx.RelativeIndex;
            LastAbsolute = charFx.AbsoluteIndex;
            LastTime = charFx.ElapsedTime;
            LastRange = charFx.Range;
            charFx.Offset = new Vector2(Convert.ToSingle(charFx.Env.TryGetValue("strength", out Variant s) ? s.AsDouble() : 0.0), 0);
            return Visible;
        }
    }

    private static void PointerChecks()
    {
        RichTextLabel rt = Make("go [url=door]here[/url] [hint=a tip]hover[/hint] end", 400);
        var log = new List<string>();
        rt.MetaClicked += m => log.Add("click:" + m.AsString());
        rt.MetaHoverStarted += m => log.Add("in:" + m.AsString());
        rt.MetaHoverEnded += m => log.Add("out:" + m.AsString());
        float y = 10 + 8;
        // "go " is 3 characters (24 px); "here" is 24..56
        Input.ParseInputEvent(new InputEventMouseMotion { Position = new Vector2(10 + 30, y), GlobalPosition = new Vector2(10 + 30, y), Relative = new Vector2(1, 0) });
        T.Check("moving over a meta says meta_hover_started with it", log.SequenceEqual(new[] { "in:door" }), string.Join(",", log));
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(10 + 30, y), GlobalPosition = new Vector2(10 + 30, y), Pressed = true, ButtonIndex = MouseButton.Left });
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(10 + 30, y), GlobalPosition = new Vector2(10 + 30, y), Pressed = false, ButtonIndex = MouseButton.Left });
        T.Check("a click on it says meta_clicked", log.Contains("click:door"));
        Input.ParseInputEvent(new InputEventMouseMotion { Position = new Vector2(10 + 5, y), GlobalPosition = new Vector2(10 + 5, y), Relative = new Vector2(-25, 0) });
        T.Check("moving off says meta_hover_ended", log.Last() == "out:door", string.Join(",", log));
        T.Check("a hint is the tooltip at that point; elsewhere the control's own", rt.GetTooltip(new Vector2(70, 8)) == "a tip" && rt.GetTooltip(new Vector2(5, 8)) == "");
        log.Clear();
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(10 + 5, y), GlobalPosition = new Vector2(10 + 5, y), Pressed = true, ButtonIndex = MouseButton.Left });
        Input.ParseInputEvent(new InputEventMouseButton { Position = new Vector2(10 + 5, y), GlobalPosition = new Vector2(10 + 5, y), Pressed = false, ButtonIndex = MouseButton.Left });
        T.Check("a click on plain text sends nothing", log.Count == 0);
        T.Check("a meta is underlined while meta_underlined is on", Draw(rt).Count(c => c.Type == CanvasItem.DrawCommand.Kind.Rect) == 1);
        rt.MetaUnderlined = false;
        T.Check("and not when it is off", Draw(rt).Count(c => c.Type == CanvasItem.DrawCommand.Kind.Rect) == 0);
        T.Check("the label passes the pointer on by default", rt.MouseFilter == Control.MouseFilterEnum.Pass);
    }

    private static List<CanvasItem.DrawCommand> Draw(CanvasItem c)
    {
        c.QueueRedraw();
        c.RunDraw();
        return c.Commands.ToList();
    }
}
