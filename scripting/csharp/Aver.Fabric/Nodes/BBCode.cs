// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The BBCode reader of RichTextLabel (G5 U2). It turns [tag] text into calls of the label's push_* / add_text / pop API, so the document is the same however it
// was written. Rules, as in Godot: a closing tag must match the innermost open tag (otherwise it is shown as text); a tag nobody knows is shown as text;
// tags still open at the end of the text are closed there; [lb] and [rb] are the brackets; a '[' with another '[' before its ']' is a literal.

using System.Globalization;
using System.Text;

namespace Aver.Fabric;

internal static class BBCodeParser
{
    private static readonly HashSet<string> s_effects = new() { "wave", "tornado", "shake", "fade", "rainbow", "pulse" };

    /// <summary>Parses <paramref name="text"/> into <paramref name="label"/> (appending to its document).</summary>
    internal static void Append(RichTextLabel label, string text)
    {
        var open = new Stack<string>();
        var plain = new StringBuilder();
        void FlushText()
        {
            if (plain.Length > 0) { label.AddText(plain.ToString()); plain.Clear(); }
        }
        int pos = 0;
        while (pos < text.Length)
        {
            int lb = text.IndexOf('[', pos);
            if (lb < 0) { plain.Append(text, pos, text.Length - pos); break; }
            plain.Append(text, pos, lb - pos);
            int rb = text.IndexOf(']', lb + 1);
            if (rb < 0) { plain.Append(text, lb, text.Length - lb); break; }
            int inner = text.IndexOf('[', lb + 1);
            if (inner >= 0 && inner < rb) { plain.Append('['); pos = lb + 1; continue; }
            string tag = text.Substring(lb + 1, rb - lb - 1);
            int next = rb + 1;
            if (!Handle(label, tag, text, ref next, open, plain, FlushText)) plain.Append(text, lb, rb - lb + 1);
            pos = next;
        }
        FlushText();
        while (open.Count > 0) { open.Pop(); label.Pop(); }
    }

    private static bool Handle(RichTextLabel l, string tag, string text, ref int next, Stack<string> open, StringBuilder plain, Action flush)
    {
        if (tag.Length == 0 || char.IsWhiteSpace(tag[0])) return false;
        if (tag == "lb") { plain.Append('['); return true; }
        if (tag == "rb") { plain.Append(']'); return true; }
        if (tag[0] == '/')
        {
            string close = tag[1..];
            if (open.Count == 0 || open.Peek() != close) return false;
            flush();
            open.Pop();
            l.Pop();
            return true;
        }
        int cut = 0;
        while (cut < tag.Length && tag[cut] != '=' && !char.IsWhiteSpace(tag[cut])) cut++;
        string name = tag[..cut];
        string rest = tag[cut..].TrimStart();
        string value = "";
        Dictionary<string, string> args = new(StringComparer.Ordinal);
        bool rawValue = name is "url" or "hint" or "lang" or "font";
        if (rest.StartsWith('='))
        {
            string after = rest[1..];
            if (rawValue) { value = Unquote(after.Trim()); if (name == "font") SplitFontArgs(ref value, args); }
            else
            {
                (value, string tail) = TakeToken(after);
                args = ParseArgs(tail);
            }
        }
        else if (rest.Length > 0) args = ParseArgs(rest);
        string Arg(string key, string fallback = "") => args.TryGetValue(key, out string? v) ? v : fallback;

        switch (name)
        {
            case "b": flush(); l.PushBold(); open.Push(name); return true;
            case "i": flush(); l.PushItalics(); open.Push(name); return true;
            case "u": flush(); l.PushUnderline(); open.Push(name); return true;
            case "s": flush(); l.PushStrikethrough(); open.Push(name); return true;
            case "code": flush(); l.PushMono(); open.Push(name); return true;
            case "center": flush(); l.PushParagraph(HorizontalAlignment.Center); open.Push(name); return true;
            case "left": flush(); l.PushParagraph(HorizontalAlignment.Left); open.Push(name); return true;
            case "right": flush(); l.PushParagraph(HorizontalAlignment.Right); open.Push(name); return true;
            case "fill": flush(); l.PushParagraph(HorizontalAlignment.Fill); open.Push(name); return true;
            case "p":
            {
                string a = value.Length > 0 ? value : Arg("align");
                flush();
                l.PushParagraph(a switch { "center" => HorizontalAlignment.Center, "right" => HorizontalAlignment.Right, "fill" => HorizontalAlignment.Fill, _ => HorizontalAlignment.Left });
                open.Push(name);
                return true;
            }
            case "indent": flush(); l.PushIndent(1); open.Push(name); return true;
            case "ul":
                flush();
                l.PushList(1, RichTextLabel.ListType.Dots, false, args.TryGetValue("bullet", out string? b) ? b : value.Length > 0 ? value : "•");
                open.Push(name);
                return true;
            case "ol":
            {
                string type = value.Length > 0 ? value : Arg("type", "1");
                flush();
                l.PushList(1, type switch { "a" or "A" => RichTextLabel.ListType.Letters, "i" or "I" => RichTextLabel.ListType.Roman, _ => RichTextLabel.ListType.Numbers }, type is "A" or "I");
                open.Push(name);
                return true;
            }
            case "table":
            {
                int cols = int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int c) ? c : 1;
                flush();
                l.PushTable(cols);
                open.Push(name);
                return true;
            }
            case "cell": flush(); l.PushCell(); open.Push(name); return true;
            case "color": flush(); l.PushColor(ParseColor(value, Colors.White)); open.Push(name); return true;
            case "bgcolor": flush(); l.PushBgcolor(ParseColor(value, Colors.Black)); open.Push(name); return true;
            case "fgcolor": flush(); l.PushFgcolor(ParseColor(value, Colors.White)); open.Push(name); return true;
            case "outline_color": flush(); l.PushOutlineColor(ParseColor(value, Colors.Black)); open.Push(name); return true;
            case "outline_size": flush(); l.PushOutlineSize(ParseInt(value, 0)); open.Push(name); return true;
            case "font_size": flush(); l.PushFontSize(ParseInt(value, 16)); open.Push(name); return true;
            case "lang": flush(); l.PushLanguage(value); open.Push(name); return true;
            case "hint": flush(); l.PushHint(value); open.Push(name); return true;
            case "font":
            {
                Font f = LoadFont(value);
                flush();
                l.PushFont(f, args.TryGetValue("size", out string? sz) ? ParseInt(sz, 0) : 0);
                open.Push(name);
                return true;
            }
            case "url":
                if (value.Length > 0) { flush(); l.PushMeta(value); open.Push(name); return true; }
                else
                {
                    int end = text.IndexOf("[/url]", next, StringComparison.Ordinal);
                    if (end < 0) return false;
                    flush();
                    string link = text[next..end];
                    l.PushMeta(link);
                    l.AddText(link);
                    l.Pop();
                    next = end + "[/url]".Length;
                    return true;
                }
            case "img":
            {
                int end = text.IndexOf("[/img]", next, StringComparison.Ordinal);
                if (end < 0) return false;
                string path = text[next..end].Trim();
                int w = 0, h = 0;
                if (value.Length > 0)
                {
                    string[] wh = value.Split('x', 'X');
                    w = ParseInt(wh[0], 0);
                    if (wh.Length > 1) h = ParseInt(wh[1], 0);
                }
                if (args.TryGetValue("width", out string? aw)) w = ParseInt(aw, 0);
                if (args.TryGetValue("height", out string? ah)) h = ParseInt(ah, 0);
                Color tint = args.TryGetValue("color", out string? ac) ? ParseColor(ac, Colors.White) : Colors.White;
                Texture2D tex = ResourceLoader.Load(path, typeof(Texture2D)) as Texture2D ?? new Texture2D { ResourcePath = path };
                flush();
                l.AddImage(tex, w, h, tint, InlineAlignment.Center, null, default, false, Arg("tooltip"), Arg("width").EndsWith('%'));
                next = end + "[/img]".Length;
                return true;
            }
        }
        if (s_effects.Contains(name) || l.HasCustomEffect(name))
        {
            flush();
            var ea = new Dictionary<string, string>(args);
            if (value.Length > 0) ea["value"] = value;
            l.PushEffect(name, ea);
            open.Push(name);
            return true;
        }
        return false;
    }

    private static string Unquote(string v) => v.Length >= 2 && (v[0] == '"' || v[0] == '\'') && v[^1] == v[0] ? v[1..^1] : v;

    // The first token of a tag value (up to a space, quotes respected) and what follows it.
    private static (string Token, string Tail) TakeToken(string s)
    {
        s = s.TrimStart();
        if (s.Length > 0 && (s[0] == '"' || s[0] == '\''))
        {
            int q = s.IndexOf(s[0], 1);
            if (q > 0) return (s[1..q], s[(q + 1)..]);
        }
        int i = 0;
        while (i < s.Length && !char.IsWhiteSpace(s[i])) i++;
        return (s[..i], s[i..]);
    }

    private static Dictionary<string, string> ParseArgs(string s)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        int i = 0;
        while (i < s.Length)
        {
            while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
            int k = i;
            while (i < s.Length && s[i] != '=' && !char.IsWhiteSpace(s[i])) i++;
            string key = s[k..i];
            if (i < s.Length && s[i] == '=')
            {
                i++;
                (string v, string tail) = TakeToken(s[i..]);
                d[key] = v;
                i = s.Length - tail.Length;
            }
            else if (key.Length > 0) d[key] = "";
        }
        return d;
    }

    private static void SplitFontArgs(ref string value, Dictionary<string, string> args)
    {
        // [font=path size=24] : the path runs to the first " key=" pair
        int sp = value.IndexOf(' ');
        if (sp > 0 && value[(sp + 1)..].Contains('='))
        {
            foreach (KeyValuePair<string, string> kv in ParseArgs(value[(sp + 1)..])) args[kv.Key] = kv.Value;
            value = value[..sp];
        }
    }

    private static Font LoadFont(string path)
    {
        if (path.Length > 0 && ResourceLoader.Load(path, typeof(Font)) is Font f) return f;
        return new FontFile { ResourcePath = path };
    }

    private static int ParseInt(string s, int fallback) => int.TryParse(s.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? v : fallback;

    internal static Color ParseColor(string code, Color fallback)
    {
        code = code.Trim();
        if (code.Length == 0) return fallback;
        return new Color(code, fallback);
    }
}
