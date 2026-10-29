// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// RichTextLabel's document (G5 U2): the item stream every way of filling the label produces. BBCode (BBCode.cs) is parsed into calls of the push_* / pop /
// add_text API, and that API appends items here, so a script that builds text with push_bold() and add_text() and one that writes [b]..[/b] get the same
// document. An item is a text run, a newline, an image, or the opening or closing of a style or a block (paragraph, indent, list, table, cell).

namespace Aver.Fabric;

/// <summary>What an opening item changes.</summary>
internal enum RtOp
{
    Bold, Italic, BoldItalic, Normal, Mono, Underline, Strike, Font, FontSize, Color, Bgcolor, Fgcolor, OutlineSize, OutlineColor, Meta, Hint,
    Paragraph, Indent, List, Table, Cell, Effect, Lang,
}

internal sealed class RtItem
{
    public enum K { Text, Newline, Image, Open, Close }

    public K Kind;
    public string Text = "";
    public RtOp Op;
    public Font? Font;
    public int Int;
    public float F;
    public Color Col;
    public Variant Var;
    public string Str = "";
    public HorizontalAlignment Align;
    public bool Flag;
    public Texture2D? Texture;
    public Vector2 ImageSize;
    public Color Tint = new(1, 1, 1, 1);
    public string Tooltip = "";
    public Dictionary<string, string>? Args;
    public string Tag = "";

    public override string ToString() => Kind switch
    {
        K.Text => "'" + Text + "'",
        K.Newline => "\\n",
        K.Image => "image",
        K.Open => Op.ToString().ToLowerInvariant() + "[",
        _ => "]",
    };
}

/// <summary>A custom effect for RichTextLabel (Godot's RichTextEffect): <c>[bbcode]</c> in the text runs it over the characters inside.</summary>
public class RichTextEffect : Resource
{
    /// <summary>The tag the effect answers to.</summary>
    public string Bbcode { get; set; } = "";

    /// <summary>Called for every character in the effect's range; return false to hide the character.</summary>
    public virtual bool _ProcessCustomFx(CharFXTransform charFx) => true;
}

/// <summary>What a custom effect sees and changes for one character (Godot's CharFXTransform).</summary>
public class CharFXTransform : RefCounted
{
    public Vector2I Range { get; set; }
    public double ElapsedTime { get; set; }
    public bool Visible { get; set; } = true;
    public bool Outline { get; set; }
    public Vector2 Offset { get; set; }
    public Color Color { get; set; } = new(1, 1, 1, 1);
    public Collections.Dictionary Env { get; set; } = new();
    public int RelativeIndex { get; set; }
    public int AbsoluteIndex { get; set; }
    public long Glyph { get; set; }
    public long GlyphFlags { get; set; }
    public long GlyphIndex { get; set; }
    public int GlyphCount { get; set; }
    public long FontCache { get; set; }
}

public partial class RichTextLabel
{
    private readonly List<RtItem> _items = new();
    private readonly Stack<RtOp> _open = new();
    private int _tableDepth;

    private RtItem Add(RtItem item)
    {
        _items.Add(item);
        Dirty();
        return item;
    }

    private void Push(RtOp op, Action<RtItem>? fill = null)
    {
        var it = new RtItem { Kind = RtItem.K.Open, Op = op };
        fill?.Invoke(it);
        _open.Push(op);
        if (op == RtOp.Table) _tableDepth++;
        Add(it);
    }

    // ---- the push / pop API ----

    /// <summary>Adds plain text (no BBCode) to the end.</summary>
    public void AddText(string text)
    {
        if (text.Length == 0) return;
        // a newline in the text ends the line, as in Godot
        int at = 0;
        while (true)
        {
            int nl = text.IndexOf('\n', at);
            string part = nl < 0 ? text[at..] : text[at..nl];
            if (part.Length > 0) Add(new RtItem { Kind = RtItem.K.Text, Text = part });
            if (nl < 0) break;
            Add(new RtItem { Kind = RtItem.K.Newline });
            at = nl + 1;
        }
        FollowScroll();
    }

    /// <summary>Ends the current line.</summary>
    public void Newline() { Add(new RtItem { Kind = RtItem.K.Newline }); FollowScroll(); }

    public void PushBold() => Push(RtOp.Bold);
    public void PushItalics() => Push(RtOp.Italic);
    public void PushBoldItalics() => Push(RtOp.BoldItalic);
    public void PushNormal() => Push(RtOp.Normal);
    public void PushMono() => Push(RtOp.Mono);
    public void PushUnderline() => Push(RtOp.Underline);
    public void PushStrikethrough() => Push(RtOp.Strike);
    public void PushFont(Font font, int fontSize = 0) => Push(RtOp.Font, i => { i.Font = font; i.Int = fontSize; });
    public void PushFontSize(int fontSize) => Push(RtOp.FontSize, i => i.Int = fontSize);
    public void PushColor(Color color) => Push(RtOp.Color, i => i.Col = color);
    public void PushBgcolor(Color bgcolor) => Push(RtOp.Bgcolor, i => i.Col = bgcolor);
    public void PushFgcolor(Color fgcolor) => Push(RtOp.Fgcolor, i => i.Col = fgcolor);
    public void PushOutlineSize(int outlineSize) => Push(RtOp.OutlineSize, i => i.Int = outlineSize);
    public void PushOutlineColor(Color color) => Push(RtOp.OutlineColor, i => i.Col = color);
    public void PushMeta(Variant data, MetaUnderline underlineMode = MetaUnderline.Always, string tooltip = "") => Push(RtOp.Meta, i => { i.Var = data; i.Int = (int)underlineMode; i.Tooltip = tooltip; });
    public void PushHint(string description) => Push(RtOp.Hint, i => i.Str = description);
    public void PushParagraph(HorizontalAlignment alignment, TextServer.Direction baseDirection = TextServer.Direction.Auto, string language = "") => Push(RtOp.Paragraph, i => i.Align = alignment);
    public void PushIndent(int level) => Push(RtOp.Indent, i => i.Int = Math.Max(1, level));
    public void PushLanguage(string language) => Push(RtOp.Lang, i => i.Str = language);

    /// <summary>Starts a list: each line until the matching pop is an item.</summary>
    public void PushList(int level, ListType type, bool capitalize, string bullet = "•")
        => Push(RtOp.List, i => { i.Int = Math.Max(1, level); i.F = (float)type; i.Flag = capitalize; i.Str = bullet; });

    /// <summary>Starts a table of <paramref name="columns"/> columns; push_cell() starts each cell in turn.</summary>
    public void PushTable(int columns, InlineAlignment inlineAlign = InlineAlignment.Top, int alignToRow = -1)
        => Push(RtOp.Table, i => { i.Int = Math.Max(1, columns); i.F = (float)inlineAlign; });

    public void PushCell() => Push(RtOp.Cell);

    public void SetTableColumnExpand(int column, bool expand, int ratio = 1, bool shrink = true)
    {
        _columnExpand[column] = (expand, Math.Max(1, ratio));
        Dirty();
    }

    public void SetCellRowBackgroundColor(Color oddRowBg, Color evenRowBg) { _rowOdd = oddRowBg; _rowEven = evenRowBg; Dirty(); }
    public void SetCellBorderColor(Color color) { _cellBorder = color; Dirty(); }
    public void SetCellSizeOverride(Vector2 minSize, Vector2 maxSize) { }
    public void SetCellPadding(Rect2 padding) { _cellPadding = padding; Dirty(); }

    public void PushEffect(string tag, Dictionary<string, string>? args = null) => Push(RtOp.Effect, i => { i.Tag = tag; i.Args = args; });

    /// <summary>Closes the innermost open tag.</summary>
    public void Pop()
    {
        if (_open.Count == 0) return;
        RtOp op = _open.Pop();
        if (op == RtOp.Table) _tableDepth--;
        Add(new RtItem { Kind = RtItem.K.Close, Op = op });
    }

    /// <summary>Closes every open tag.</summary>
    public void PopAll() { while (_open.Count > 0) Pop(); }

    /// <summary>Pops down to a context pushed with push_context() (here the whole stack is one context).</summary>
    public int PushContext() => _open.Count;

    public void PopContext() { }

    /// <summary>Adds an image (Godot's add_image). A width or height of 0 uses the texture's; <paramref name="sizeInPercent"/> reads them as percent of the label's width.</summary>
    public void AddImage(Texture2D image, int width = 0, int height = 0, Color? color = null, InlineAlignment inlineAlign = InlineAlignment.Center, Rect2? region = null, Variant key = default, bool pad = false, string tooltip = "", bool sizeInPercent = false)
    {
        Vector2 ts = image.GetSize();
        float w = width > 0 ? width : ts.X, h = height > 0 ? height : ts.Y;
        if (width > 0 && height <= 0 && ts.X > 0) h = ts.Y * width / ts.X;
        else if (height > 0 && width <= 0 && ts.Y > 0) w = ts.X * height / ts.Y;
        Add(new RtItem { Kind = RtItem.K.Image, Texture = image, ImageSize = new Vector2(w, h), Tint = color ?? new Color(1, 1, 1, 1), Tooltip = tooltip, Flag = sizeInPercent, Int = (int)inlineAlign });
        FollowScroll();
    }

    /// <summary>Removes everything and returns the label to an empty document.</summary>
    public void Clear()
    {
        _items.Clear();
        _open.Clear();
        _tableDepth = 0;
        _text = "";
        _scrollV.Value = 0;
        _selFrom = _selTo = -1;
        Dirty();
    }

    /// <summary>Removes the paragraph at the index (a paragraph ends at a newline).</summary>
    public bool RemoveParagraph(int paragraph, bool noInvalidate = false)
    {
        int p = 0, start = 0;
        for (int i = 0; i <= _items.Count; i++)
        {
            bool end = i == _items.Count || _items[i].Kind == RtItem.K.Newline;
            if (!end) continue;
            if (p == paragraph)
            {
                int to = Math.Min(_items.Count, i + 1);
                _items.RemoveRange(start, to - start);
                Dirty();
                return true;
            }
            p++;
            start = i + 1;
        }
        return false;
    }

    // ---- reading the document back ----

    /// <summary>The text without the mark-up (Godot's get_parsed_text): text runs and newlines; table cells of a row are separated by a tab and rows by a newline.</summary>
    public string GetParsedText()
    {
        var sb = new System.Text.StringBuilder();
        var tables = new Stack<(int Columns, int Cells)>();
        foreach (RtItem it in _items)
        {
            switch (it.Kind)
            {
                case RtItem.K.Text: sb.Append(it.Text); break;
                case RtItem.K.Newline: sb.Append('\n'); break;
                case RtItem.K.Open when it.Op == RtOp.Table: tables.Push((Math.Max(1, it.Int), 0)); break;
                case RtItem.K.Open when it.Op == RtOp.Cell && tables.Count > 0:
                {
                    (int cols, int cells) = tables.Pop();
                    if (cells > 0) sb.Append(cells % cols == 0 ? '\n' : '\t');
                    tables.Push((cols, cells + 1));
                    break;
                }
                case RtItem.K.Close when it.Op == RtOp.Table && tables.Count > 0: tables.Pop(); break;
            }
        }
        return sb.ToString();
    }

    /// <summary>The characters in the document: text characters, and one for each image; newlines are not counted.</summary>
    public int GetTotalCharacterCount()
    {
        int n = 0;
        foreach (RtItem it in _items)
        {
            if (it.Kind == RtItem.K.Text) n += it.Text.Length;
            else if (it.Kind == RtItem.K.Image) n++;
        }
        return n;
    }

    /// <summary>The paragraphs: lines of the source separated by a newline.</summary>
    public int GetParagraphCount()
    {
        int n = 1;
        foreach (RtItem it in _items) if (it.Kind == RtItem.K.Newline) n++;
        return n;
    }

    /// <summary>The tag stack and text as a flat string, for tests: <c>bold[ 'x' ]</c>.</summary>
    internal string DumpDocument() => string.Join(" ", _items.Select(i => i.ToString()));

    internal int ItemCount => _items.Count;

    internal bool HasEffects => _items.Any(i => i.Kind == RtItem.K.Open && i.Op == RtOp.Effect);
}
