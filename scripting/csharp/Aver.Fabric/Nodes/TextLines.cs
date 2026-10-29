// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// TextLines (G5 U2): the line breaking the text controls share (Label, Button, LineEdit, TextEdit, ProgressBar, the rich text). Text is broken at newlines
// and, when a width is given, at spaces, after hyphens and between CJK characters (arbitrary mode: anywhere); each line is measured with UiText so what a
// control reports as its minimum size is what it draws. Characters are counted the way visible_characters counts them: every character but the newline.

using System.Text;

namespace Aver.Fabric;

internal sealed class TextLines
{
    public enum Wrap { Off = 0, Arbitrary = 1, Word = 2, Smart = 3 }

    public struct Line
    {
        /// <summary>First character in the source text, how many characters the line shows, and how many non-newline characters precede it.</summary>
        public int Start, Length, Ord;
        public float Width;
    }

    public string Text = "";
    public readonly List<Line> Items = new();
    public float LineHeight;
    public Font? Font;
    public float Px;

    public int Count => Items.Count;

    public float Width(int i) => Items[i].Width;

    public string LineText(int i) => Text.Substring(Items[i].Start, Items[i].Length);

    public float MaxWidth()
    {
        float w = 0f;
        foreach (Line l in Items) w = MathF.Max(w, l.Width);
        return w;
    }

    internal static bool IsCjk(int c) =>
        (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3040 && c <= 0x30FF);

    /// <summary>Breaks <paramref name="text"/> into lines no wider than <paramref name="maxWidth"/> (a width of zero or less does not wrap).</summary>
    public static TextLines Build(string text, Font? font, float px, float maxWidth, Wrap wrap)
    {
        var r = new TextLines { Text = text, Font = font, Px = px, LineHeight = UiText.LineHeight(font, px) };
        int paraStart = 0, ord = 0;
        while (true)
        {
            int nl = text.IndexOf('\n', paraStart);
            int paraEnd = nl < 0 ? text.Length : nl;
            r.BreakParagraph(paraStart, paraEnd, ord, maxWidth, wrap);
            ord += paraEnd - paraStart;
            if (nl < 0) break;
            paraStart = nl + 1;
        }
        return r;
    }

    private void Emit(int start, int length, int ordBase, int paraStart)
    {
        string s = Text.Substring(start, length);
        Items.Add(new Line { Start = start, Length = length, Ord = ordBase + (start - paraStart), Width = UiText.Width(Font, Px, s) });
    }

    private float WidthOf(int start, int end) => UiText.Width(Font, Px, Text.Substring(start, end - start));

    private int TrimEnd(int start, int end)
    {
        while (end > start && (Text[end - 1] == ' ' || Text[end - 1] == '\t' || Text[end - 1] == '\r')) end--;
        return end;
    }

    private void BreakParagraph(int ps, int pe, int ordBase, float maxWidth, Wrap wrap)
    {
        if (wrap == Wrap.Off || maxWidth <= 0f || pe == ps || WidthOf(ps, pe) <= maxWidth)
        {
            Emit(ps, pe - ps, ordBase, ps);
            return;
        }
        // token ends: where a line may end
        var ends = new List<int>();
        for (int i = ps; i < pe; i++)
        {
            char c = Text[i];
            bool brk = wrap == Wrap.Arbitrary || c == ' ' || c == '\t' || IsCjk(c) || (c == '-' && i + 1 < pe && char.IsLetterOrDigit(Text[i + 1]));
            if (char.IsHighSurrogate(c) && i + 1 < pe) { ends.Add(i + 2); i++; continue; }
            if (brk || i == pe - 1) ends.Add(i + 1);
        }
        int lineStart = ps, accepted = ps, t = 0;
        while (t < ends.Count)
        {
            int te = ends[t];
            if (WidthOf(lineStart, TrimEnd(lineStart, te)) <= maxWidth || (accepted == lineStart && wrap == Wrap.Word))
            {
                accepted = te;
                t++;
                continue;
            }
            if (accepted > lineStart)
            {
                Emit(lineStart, TrimEnd(lineStart, accepted) - lineStart, ordBase, ps);
                lineStart = accepted;
                continue;
            }
            // one token wider than the line (smart mode): fill the line with as many characters as fit
            int k = 1;
            while (lineStart + k < te && WidthOf(lineStart, lineStart + k + 1) <= maxWidth) k++;
            Emit(lineStart, k, ordBase, ps);
            lineStart += k;
            accepted = lineStart;
        }
        if (lineStart < pe) Emit(lineStart, pe - lineStart, ordBase, ps);
    }

    /// <summary>The longest prefix of <paramref name="s"/> that fits <paramref name="maxWidth"/> with the ellipsis appended (or without it when
    /// <paramref name="ellipsis"/> is empty). At word granularity the cut moves back to a space.</summary>
    public static string Trim(string s, Font? font, float px, float maxWidth, bool word, string ellipsis)
    {
        if (UiText.Width(font, px, s) <= maxWidth) return s;
        float room = maxWidth - (ellipsis.Length > 0 ? UiText.Width(font, px, ellipsis) : 0f);
        int n = 0;
        while (n < s.Length && UiText.Width(font, px, s.Substring(0, n + 1)) <= room) n++;
        if (word && n < s.Length)
        {
            int sp = s.LastIndexOf(' ', Math.Max(0, n - 1), Math.Max(0, n));
            n = sp > 0 ? sp : 0;
        }
        return s.Substring(0, n).TrimEnd() + ellipsis;
    }
}
