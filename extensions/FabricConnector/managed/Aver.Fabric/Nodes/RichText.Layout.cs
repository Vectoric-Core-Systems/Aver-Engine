// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// RichTextLabel's layout (G5 U2): replays the item stream with a style stack and breaks it into lines. Words are atoms measured with UiText; a line is
// filled greedily and aligned left, centred, right or justified; indents, list markers and tables (cells laid out by column, widths from the cells' natural
// and shortest widths) are blocks; images sit on the baseline. The result is a list of draw operations, hit regions for meta and hint, and line records.

namespace Aver.Fabric;

internal sealed class RtStyle
{
    public Font? Font;
    public int Size;
    public Color Color;
    public bool Bold, Italic, Underline, Strike, Mono;
    public Color? Bg, Fg;
    public int OutlineSize;
    public Color? OutlineColor;
    public bool HasMeta;
    public Variant Meta;
    public int MetaUnderline;
    public string Hint = "", MetaTip = "";
    public List<RtItem> Effects = new();

    public RtStyle Clone()
    {
        var s = (RtStyle)MemberwiseClone();
        s.Effects = new List<RtItem>(Effects);
        return s;
    }
}

internal struct RtDraw
{
    public enum K { Text, Rect, Image }

    public K Kind;
    public Rect2 Rect;
    public string Text;
    public Color Color;
    public Font? Font;
    public int Size;
    public CanvasItem.TextFx Fx;
    public Texture2D? Texture;
    public int Ord;
    public List<RtItem>? Effects;
}

internal struct RtHit
{
    public Rect2 Rect;
    public bool IsMeta;
    public Variant Meta;
    public string Hint;
    public int Ord;
}

internal struct RtLine
{
    public float Top, Height;
    public int Paragraph, Ord;
}

internal sealed class RtBlock
{
    public readonly List<RtDraw> Draws = new();
    public readonly List<RtHit> Hits = new();
    public readonly List<RtLine> Lines = new();
    public float Height, Extent;

    public void Merge(RtBlock other, Vector2 offset)
    {
        foreach (RtDraw d in other.Draws) { RtDraw c = d; c.Rect = new Rect2(c.Rect.Position + offset, c.Rect.Size); Draws.Add(c); }
        foreach (RtHit h in other.Hits) { RtHit c = h; c.Rect = new Rect2(c.Rect.Position + offset, c.Rect.Size); Hits.Add(c); }
        foreach (RtLine l in other.Lines) { RtLine c = l; c.Top += offset.Y; Lines.Add(c); }
    }
}

public partial class RichTextLabel
{
    private sealed class Frame
    {
        public RtOp Op;
        public RtStyle Style = null!;
        public float Indent;
        public HorizontalAlignment Align;
        public RtItem? List;
        public int ListCounter;
        public bool ListStartPending;
    }

    private struct Atom
    {
        public string Text;
        public float W, H, Asc;
        public RtStyle Style;
        public bool Space;
        public int Ord;
        public RtItem? Image;
        public Font? Font;
    }

    private readonly Dictionary<(Font?, bool, bool), Font> _variants = new();

    private Font? BaseFont(RtStyle s)
    {
        if (s.Font != null) return s.Font;
        if (s.Mono && HasThemeFont("mono_font")) return GetThemeFont("mono_font");
        if (s.Bold && s.Italic && HasThemeFont("bold_italics_font")) return GetThemeFont("bold_italics_font");
        if (s.Bold && HasThemeFont("bold_font")) return GetThemeFont("bold_font");
        if (s.Italic && HasThemeFont("italics_font")) return GetThemeFont("italics_font");
        return GetThemeFont("normal_font");
    }

    internal Font? FontFor(RtStyle s)
    {
        Font? b = BaseFont(s);
        bool needBold = s.Bold && !(b?.IsBold ?? false) && !HasThemeFont("bold_font"), needItalic = s.Italic && !(b?.IsItalic ?? false) && !HasThemeFont("italics_font");
        if (b != null && (b == GetThemeFont("bold_font") && s.Bold)) needBold = false;
        if (!needBold && !needItalic) return b;
        if (_variants.TryGetValue((b, needBold, needItalic), out Font? known)) return known;
        var v = new FontVariation { BaseFont = b, VariationEmbolden = needBold ? 1f : 0f, VariationTransform = needItalic ? new Transform2D(new Vector2(1, 0), new Vector2(0.2f, 1), Vector2.Zero) : Transform2D.Identity };
        return _variants[(b, needBold, needItalic)] = v;
    }

    private RtStyle RootStyle()
    {
        int size = HasThemeFontSize("normal_font_size") ? GetThemeFontSize("normal_font_size") : ThemeFontSize("normal_font_size");
        return new RtStyle { Size = size, Color = GetThemeColor("default_color"), OutlineSize = GetThemeConstant("outline_size"), OutlineColor = HasThemeColor("font_outline_color") ? GetThemeColor("font_outline_color") : null };
    }

    private Frame Derive(Frame top, RtItem it)
    {
        var f = new Frame { Op = it.Op, Style = top.Style, Indent = top.Indent, Align = top.Align };
        RtStyle? s = null;
        RtStyle S() => s ??= top.Style.Clone();
        switch (it.Op)
        {
            case RtOp.Bold: S().Bold = true; break;
            case RtOp.Italic: S().Italic = true; break;
            case RtOp.BoldItalic: S().Bold = S().Italic = true; break;
            case RtOp.Normal: S().Bold = S().Italic = S().Mono = false; S().Font = null; break;
            case RtOp.Mono: S().Mono = true; break;
            case RtOp.Underline: S().Underline = true; break;
            case RtOp.Strike: S().Strike = true; break;
            case RtOp.Font: S().Font = it.Font; if (it.Int > 0) S().Size = it.Int; break;
            case RtOp.FontSize: S().Size = Math.Max(1, it.Int); break;
            case RtOp.Color: S().Color = it.Col; break;
            case RtOp.Bgcolor: S().Bg = it.Col; break;
            case RtOp.Fgcolor: S().Fg = it.Col; break;
            case RtOp.OutlineSize: S().OutlineSize = Math.Max(0, it.Int); break;
            case RtOp.OutlineColor: S().OutlineColor = it.Col; break;
            case RtOp.Meta: S().HasMeta = true; S().Meta = it.Var; S().MetaUnderline = it.Int; S().MetaTip = it.Tooltip; break;
            case RtOp.Hint: S().Hint = it.Str; break;
            case RtOp.Effect: S().Effects.Add(it); break;
            case RtOp.Paragraph: f.Align = it.Align; break;
            case RtOp.Indent: f.Indent += it.Int * IndentUnit(top.Style); break;
            case RtOp.List: f.Indent += it.Int * IndentUnit(top.Style); f.List = it; f.ListStartPending = true; break;
        }
        if (s != null) f.Style = s;
        return f;
    }

    private float IndentUnit(RtStyle s) => Math.Max(1, _tabSize) * UiText.Width(FontFor(s), s.Size, " ");

    // ---- text into atoms ----

    private IEnumerable<string> Words(string text)
    {
        var sb = new System.Text.StringBuilder();
        foreach (char c in text)
        {
            bool sp = c == ' ' || c == '\t';
            if (sb.Length > 0)
            {
                char prev = sb[^1];
                bool prevSp = prev == ' ' || prev == '\t';
                if (sp != prevSp || TextLines.IsCjk(c) || TextLines.IsCjk(prev) || (_wrap == TextServer.AutowrapMode.Arbitrary && !sp))
                {
                    yield return sb.ToString();
                    sb.Clear();
                }
            }
            sb.Append(c);
        }
        if (sb.Length > 0) yield return sb.ToString();
    }

    // ---- the layout proper ----

    private int FindClose(int open)
    {
        int depth = 0;
        for (int i = open; i < _items.Count; i++)
        {
            if (_items[i].Kind == RtItem.K.Open) depth++;
            else if (_items[i].Kind == RtItem.K.Close && --depth == 0) return i;
        }
        return _items.Count;
    }

    private RtBlock Run(int from, int to, Frame top, float width, ref int ord, ref int paragraph, bool measureOnly = false)
    {
        var block = new RtBlock();
        var stack = new List<Frame> { top };
        var line = new List<Atom>();
        float y = 0f;
        HorizontalAlignment lineAlign = top.Align;
        float lineIndent = top.Indent;
        bool paragraphFirst = true;
        float lineSep = GetThemeConstant("line_separation");
        int ordLocal = ord, paraLocal = paragraph;

        float LineWidth() { float w = 0; foreach (Atom a in line) w += a.W; return w; }

        void Flush(bool last)
        {
            // trim the trailing spaces
            while (line.Count > 0 && line[^1].Space) line.RemoveAt(line.Count - 1);
            Frame f = stack[^1];
            float avail = Math.Max(1f, width - lineIndent);
            float lw = LineWidth();
            float asc = 0f, desc = 0f;
            foreach (Atom a in line) { asc = MathF.Max(asc, a.Asc); desc = MathF.Max(desc, a.H - a.Asc); }
            if (line.Count == 0)
            {
                RtStyle s = f.Style;
                Font? font = FontFor(s);
                asc = UiText.Ascent(font, s.Size);
                desc = UiText.LineHeight(font, s.Size) - asc;
            }
            float h = asc + desc;
            float x = lineIndent;
            float extraSpace = 0f;
            switch (lineAlign)
            {
                case HorizontalAlignment.Center: x += (avail - lw) / 2f; break;
                case HorizontalAlignment.Right: x += avail - lw; break;
                case HorizontalAlignment.Fill:
                    if (!last) { int spaces = line.Count(a => a.Space); if (spaces > 0) extraSpace = Math.Max(0f, avail - lw) / spaces; }
                    break;
            }
            x = MathF.Max(x, lineIndent);
            // the list marker sits left of the first line of an item
            if (paragraphFirst && stack.Exists(fr => fr.List != null))
            {
                Frame lf = stack.Last(fr => fr.List != null);
                lf.ListCounter++;
                string marker = ListMarker(lf, lf.ListCounter);
                RtStyle ms = line.Count > 0 ? line[0].Style : lf.Style;
                Font? mf = FontFor(ms);
                float mw = UiText.Width(mf, ms.Size, marker + " ");
                block.Draws.Add(new RtDraw { Kind = RtDraw.K.Text, Rect = new Rect2(MathF.Max(0f, lineIndent - mw), y + asc - UiText.Ascent(mf, ms.Size), mw, UiText.LineHeight(mf, ms.Size)), Text = marker, Color = ms.Color, Font = mf, Size = ms.Size, Ord = ordLocal });
            }
            block.Lines.Add(new RtLine { Top = y, Height = h, Paragraph = paraLocal, Ord = line.Count > 0 ? line[0].Ord : ordLocal });
            // glyph runs: merge neighbouring atoms of one style
            for (int i = 0; i < line.Count; i++)
            {
                Atom a = line[i];
                float w = a.W + (a.Space ? extraSpace : 0f);
                float ay = y + asc - a.Asc;
                if (a.Image != null)
                {
                    block.Draws.Add(new RtDraw { Kind = RtDraw.K.Image, Rect = new Rect2(x, ay, a.W, a.H), Texture = a.Image.Texture, Color = a.Image.Tint, Ord = a.Ord });
                    if (a.Image.Tooltip.Length > 0) block.Hits.Add(new RtHit { Rect = new Rect2(x, ay, a.W, a.H), Hint = a.Image.Tooltip, Ord = a.Ord });
                    x += w;
                    continue;
                }
                RtStyle st = a.Style;
                var box = new Rect2(x, y, w, h);
                if (st.Bg is { } bg) block.Draws.Add(new RtDraw { Kind = RtDraw.K.Rect, Rect = box, Color = bg, Ord = a.Ord });
                if (!a.Space || st.Underline || st.Strike || st.Bg != null)
                {
                    if (!a.Space)
                    {
                        CanvasItem.TextFx fx = st.OutlineSize > 0 && st.OutlineColor is { } oc ? new CanvasItem.TextFx { OutlineSize = st.OutlineSize, OutlineColor = oc } : default;
                        block.Draws.Add(new RtDraw { Kind = RtDraw.K.Text, Rect = new Rect2(x, ay, w, a.H), Text = a.Text, Color = st.Fg ?? st.Color, Font = a.Font, Size = st.Size, Fx = fx, Ord = a.Ord, Effects = st.Effects.Count > 0 ? st.Effects : null });
                    }
                    float thick = MathF.Max(1f, MathF.Round(st.Size / 16f));
                    bool metaLine = st.HasMeta && st.MetaUnderline == (int)MetaUnderline.Always;
                    if (st.Underline || (metaLine && _metaUnderlined)) block.Draws.Add(new RtDraw { Kind = RtDraw.K.Rect, Rect = new Rect2(x, y + asc + MathF.Max(1f, st.Size / 12f), w, thick), Color = st.Fg ?? st.Color, Ord = a.Ord });
                    if (st.Strike) block.Draws.Add(new RtDraw { Kind = RtDraw.K.Rect, Rect = new Rect2(x, y + asc - a.Asc * 0.3f, w, thick), Color = st.Fg ?? st.Color, Ord = a.Ord });
                }
                if (st.HasMeta) block.Hits.Add(new RtHit { Rect = box, IsMeta = true, Meta = st.Meta, Hint = st.MetaTip, Ord = a.Ord });
                else if (st.Hint.Length > 0) block.Hits.Add(new RtHit { Rect = box, Hint = st.Hint, Ord = a.Ord });
                x += w;
            }
            block.Extent = MathF.Max(block.Extent, lineIndent + lw);
            y += h + lineSep;
            line.Clear();
            paragraphFirst = false;
            lineAlign = stack[^1].Align;
            lineIndent = stack[^1].Indent;
        }

        void Place(Atom atom)
        {
            float avail = Math.Max(1f, width - lineIndent);
            if (_wrap != TextServer.AutowrapMode.Off && width < 1e5f)
            {
                float lw = LineWidth();
                if (!atom.Space && line.Count > 0 && lw + atom.W > avail + 0.01f && line.Exists(a => !a.Space))
                {
                    Flush(false);
                }
                if (!atom.Space && atom.W > Math.Max(1f, width - lineIndent) && _wrap is TextServer.AutowrapMode.Arbitrary or TextServer.AutowrapMode.WordSmart && atom.Image == null)
                {
                    // a word wider than the line: break it by characters
                    string rest = atom.Text;
                    while (rest.Length > 0)
                    {
                        int k = 1;
                        while (k < rest.Length && UiText.Width(atom.Font, atom.Style.Size, rest[..(k + 1)]) <= Math.Max(1f, width - lineIndent) - LineWidth()) k++;
                        string head = rest[..k];
                        line.Add(atom with { Text = head, W = UiText.Width(atom.Font, atom.Style.Size, head), Ord = atom.Ord + (atom.Text.Length - rest.Length) });
                        rest = rest[k..];
                        if (rest.Length > 0) Flush(false);
                    }
                    return;
                }
                if (atom.Space && line.Count == 0 && block.Lines.Count > 0 && !paragraphFirst) return;   // no spaces at the start of a wrapped line
            }
            line.Add(atom);
        }

        for (int i = from; i < to; i++)
        {
            RtItem it = _items[i];
            Frame f = stack[^1];
            switch (it.Kind)
            {
                case RtItem.K.Text:
                {
                    RtStyle s = f.Style;
                    Font? font = FontFor(s);
                    float lh = UiText.LineHeight(font, s.Size), asc = UiText.Ascent(font, s.Size);
                    foreach (string w in Words(it.Text))
                    {
                        bool space = w[0] == ' ' || w[0] == '\t';
                        float ww = space && w.Contains('\t') ? w.Sum(c => c == '\t' ? IndentUnit(s) : UiText.Width(font, s.Size, " ")) : UiText.Width(font, s.Size, w);
                        Place(new Atom { Text = w, W = ww, H = lh, Asc = asc, Style = s, Space = space, Ord = ordLocal, Font = font });
                        ordLocal += w.Length;
                    }
                    break;
                }
                case RtItem.K.Image:
                {
                    RtStyle s = f.Style;
                    Font? font = FontFor(s);
                    float lh = UiText.LineHeight(font, s.Size), asc = UiText.Ascent(font, s.Size);
                    Vector2 sz = it.ImageSize;
                    if (it.Flag && width < 1e5f) sz = new Vector2(width * sz.X / 100f, width * sz.Y / 100f);
                    float imgAsc = (InlineAlignment)it.Int switch { InlineAlignment.Top => asc, InlineAlignment.Center => sz.Y / 2f + (asc - (lh - asc)) / 2f, _ => sz.Y };
                    Place(new Atom { W = sz.X, H = sz.Y, Asc = imgAsc, Style = s, Image = it, Ord = ordLocal, Text = "" });
                    ordLocal++;
                    break;
                }
                case RtItem.K.Newline:
                    Flush(true);
                    paraLocal++;
                    paragraphFirst = true;
                    break;
                case RtItem.K.Open when it.Op == RtOp.Table:
                {
                    if (line.Count > 0) Flush(true);
                    int close = FindClose(i);
                    RtBlock table = LayoutTable(i, close, f, Math.Max(1f, width - f.Indent), ref ordLocal, ref paraLocal);
                    block.Merge(table, new Vector2(f.Indent, y));
                    block.Extent = MathF.Max(block.Extent, f.Indent + table.Extent);
                    y += table.Height + lineSep;
                    paragraphFirst = true;
                    i = Math.Min(close, to - 1);
                    break;
                }
                case RtItem.K.Open:
                    if (it.Op is RtOp.Paragraph or RtOp.Indent or RtOp.List && line.Count > 0) Flush(true);
                    stack.Add(Derive(f, it));
                    if (it.Op is RtOp.Paragraph or RtOp.Indent or RtOp.List) { lineAlign = stack[^1].Align; lineIndent = stack[^1].Indent; paragraphFirst = true; }
                    break;
                case RtItem.K.Close:
                    if (stack.Count > 1)
                    {
                        RtOp op = stack[^1].Op;
                        if (op is RtOp.Paragraph or RtOp.Indent or RtOp.List && line.Count > 0) Flush(true);
                        stack.RemoveAt(stack.Count - 1);
                        if (op is RtOp.Paragraph or RtOp.Indent or RtOp.List) { lineAlign = stack[^1].Align; lineIndent = stack[^1].Indent; paragraphFirst = true; }
                    }
                    break;
            }
        }
        if (line.Count > 0 || block.Lines.Count == 0 || to > from && _items[to - 1].Kind == RtItem.K.Newline) Flush(true);
        block.Height = Math.Max(0f, y - lineSep);
        ord = ordLocal;
        paragraph = paraLocal;
        return block;
    }

    private static string ListMarker(Frame lf, int n)
    {
        var type = (ListType)(int)lf.List!.F;
        bool caps = lf.List.Flag;
        switch (type)
        {
            case ListType.Numbers: return n + ".";
            case ListType.Letters:
            {
                string s = "";
                for (int k = n; k > 0; k = (k - 1) / 26) s = (char)((caps ? 'A' : 'a') + (k - 1) % 26) + s;
                return s + ".";
            }
            case ListType.Roman:
            {
                int[] v = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
                string[] r = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
                var sb = new System.Text.StringBuilder();
                for (int k = 0, rest = n; k < v.Length; k++) while (rest >= v[k]) { sb.Append(r[k]); rest -= v[k]; }
                return (caps ? sb.ToString() : sb.ToString().ToLowerInvariant()) + ".";
            }
            default: return lf.List.Str;
        }
    }

    // A table: its cells in order, columns sized to the content, rows as tall as their tallest cell.
    private RtBlock LayoutTable(int open, int close, Frame outer, float width, ref int ord, ref int paragraph)
    {
        RtItem tableItem = _items[open];
        int cols = Math.Max(1, tableItem.Int);
        var cells = new List<(int From, int To)>();
        for (int i = open + 1; i < close; i++)
        {
            if (_items[i].Kind == RtItem.K.Open && _items[i].Op == RtOp.Cell)
            {
                int cc = FindClose(i);
                cells.Add((i + 1, cc));
                i = cc;
            }
        }
        int rows = (cells.Count + cols - 1) / cols;
        float hsep = GetThemeConstant("table_h_separation"), vsep = GetThemeConstant("table_v_separation");
        var natural = new float[cols];
        var shortest = new float[cols];
        var cellFrame = new Frame { Op = RtOp.Cell, Style = outer.Style, Indent = 0, Align = HorizontalAlignment.Left };
        int o0 = ord, p0 = paragraph;
        for (int c = 0; c < cells.Count; c++)
        {
            int col = c % cols;
            int ot = o0, pt = p0;
            RtBlock wide = Run(cells[c].From, cells[c].To, cellFrame, 1e6f, ref ot, ref pt, true);
            natural[col] = MathF.Max(natural[col], wide.Extent + _cellPadding.Position.X + _cellPadding.Size.X);
            WrapSwap(TextServer.AutowrapMode.Word, () =>
            {
                int os = o0, ps = p0;
                RtBlock narrow = Run(cells[c].From, cells[c].To, cellFrame, 1f, ref os, ref ps, true);
                shortest[col] = MathF.Max(shortest[col], narrow.Extent + _cellPadding.Position.X + _cellPadding.Size.X);
            });
        }
        float spacing = hsep * (cols - 1);
        var colW = (float[])natural.Clone();
        float sum = colW.Sum() + spacing;
        if (sum > width)
        {
            // shrink the columns that have room down towards their shortest word, in proportion to that room
            float over = sum - width, room = 0f;
            for (int c = 0; c < cols; c++) room += MathF.Max(0f, colW[c] - shortest[c]);
            if (room > 0f)
                for (int c = 0; c < cols; c++) colW[c] -= MathF.Min(MathF.Max(0f, colW[c] - shortest[c]), over * MathF.Max(0f, colW[c] - shortest[c]) / room);
        }
        else if (_columnExpand.Count > 0)
        {
            float ratioSum = 0f;
            foreach (var kv in _columnExpand) if (kv.Value.Expand && kv.Key < cols) ratioSum += kv.Value.Ratio;
            if (ratioSum > 0f)
            {
                float extra = width - sum;
                foreach (var kv in _columnExpand) if (kv.Value.Expand && kv.Key < cols) colW[kv.Key] += extra * kv.Value.Ratio / ratioSum;
            }
        }
        var table = new RtBlock();
        float y = 0f;
        int ordAcc = o0, paraAcc = p0;
        for (int r = 0; r < rows; r++)
        {
            float rowH = 0f;
            var rowBlocks = new List<(RtBlock Block, float X)>();
            float x = 0f;
            for (int c = r * cols; c < Math.Min(cells.Count, (r + 1) * cols); c++)
            {
                int col = c % cols;
                float cw = Math.Max(1f, colW[col] - _cellPadding.Position.X - _cellPadding.Size.X);
                RtBlock b = Run(cells[c].From, cells[c].To, cellFrame, cw, ref ordAcc, ref paraAcc);
                rowBlocks.Add((b, x));
                rowH = MathF.Max(rowH, b.Height + _cellPadding.Position.Y + _cellPadding.Size.Y);
                x += colW[col] + hsep;
            }
            Color? bg = (r % 2 == 0 ? _rowEven : _rowOdd);
            for (int k = 0; k < rowBlocks.Count; k++)
            {
                float cx = rowBlocks[k].X;
                int col = (r * cols + k) % cols;
                if (bg is { } bgc && bgc.A > 0f) table.Draws.Add(new RtDraw { Kind = RtDraw.K.Rect, Rect = new Rect2(cx, y, colW[col], rowH), Color = bgc, Ord = ordAcc });
                if (_cellBorder.A > 0f)
                {
                    foreach (Rect2 edge in new[] { new Rect2(cx, y, colW[col], 1), new Rect2(cx, y + rowH - 1, colW[col], 1), new Rect2(cx, y, 1, rowH), new Rect2(cx + colW[col] - 1, y, 1, rowH) })
                        table.Draws.Add(new RtDraw { Kind = RtDraw.K.Rect, Rect = edge, Color = _cellBorder, Ord = ordAcc });
                }
                table.Merge(rowBlocks[k].Block, new Vector2(cx + _cellPadding.Position.X, y + _cellPadding.Position.Y));
            }
            y += rowH + vsep;
        }
        table.Height = Math.Max(0f, y - vsep);
        table.Extent = colW.Sum() + spacing;
        ord = ordAcc;
        paragraph = paraAcc;
        return table;
    }

    private void WrapSwap(TextServer.AutowrapMode mode, Action action)
    {
        TextServer.AutowrapMode keep = _wrap;
        _wrap = mode;
        try { action(); }
        finally { _wrap = keep; }
    }

    // The whole document at a width.
    private RtBlock LayoutAll(float width)
    {
        var root = new Frame { Op = RtOp.Normal, Style = RootStyle(), Indent = 0, Align = HorizontalAlignment.Left };
        int ord = 0, paragraph = 0;
        return Run(0, _items.Count, root, width, ref ord, ref paragraph);
    }
}
