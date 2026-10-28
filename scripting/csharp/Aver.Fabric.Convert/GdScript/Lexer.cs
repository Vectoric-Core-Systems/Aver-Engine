// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript 2.0 tokenizer (G3 F4a): indentation blocks, brackets, every string and number form, $/% node sugar, annotations.

using System.Globalization;
using System.Text;

namespace Aver.Fabric.Convert.GdScript;

public enum TokKind { Ident, Keyword, Int, Float, String, Annotation, NodePath, Op, Newline, Indent, Dedent, Eof }
public enum StrKind { Normal, StringName, NodePath }

public sealed class Token
{
    public TokKind Kind;
    public string Text = "";   // identifier / keyword / operator text; the source slice of a literal; the name of an annotation (no @); the path of a node sugar
    public int Line, Col;      // first character
    public int EndLine, EndCol; // just after the last character
    public object? Value;      // Int: long; Float: double; String: the decoded string; NodePath: the path
    public StrKind StrKind;
    public bool Raw, Triple;
    public bool Unique;        // NodePath token written as %Name rather than $Path

    public bool IsOp(string s) => Kind == TokKind.Op && Text == s;
    public bool IsKw(string s) => Kind == TokKind.Keyword && Text == s;
    public override string ToString() => Kind switch
    {
        TokKind.Newline => "NEWLINE", TokKind.Indent => "INDENT", TokKind.Dedent => "DEDENT", TokKind.Eof => "EOF",
        _ => $"{Kind}({Text})",
    };
}

public sealed class Lexer
{
    public static readonly HashSet<string> Keywords = new()
    {
        "if", "elif", "else", "for", "while", "match", "break", "continue", "pass", "return", "class", "class_name", "extends",
        "is", "in", "as", "self", "signal", "func", "static", "const", "enum", "var", "breakpoint", "preload", "await", "assert",
        "void", "true", "false", "null", "and", "or", "not", "super",
    };

    private static readonly string[] s_ops3 = { "**=", "<<=", ">>=", "..." };
    private static readonly string[] s_ops2 =
    {
        "**", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "->", ":=", "..",
    };
    private const string Ops1 = "+-*/%=<>!&|^~.,:;()[]{}";

    public List<Token> Tokens { get; } = new();
    public List<ParseError> Errors { get; } = new();
    public List<Comment> Comments { get; } = new();

    // An open bracket, or a multi-line lambda body inside brackets (which has its own indentation, as in Godot).
    private sealed class Frame
    {
        public char Close;
        public bool Lambda;
        public int Base;       // indent-stack depth when the lambda body was opened
        public int Line, Col;  // where the bracket opened
    }

    // A "func" met inside brackets whose header has not reached its ':' yet.
    private sealed class LamTrack
    {
        public int Depth; // frame count at "func"
        public int Phase; // 0 before "(", 1 in the parameters, 2 after ")"
    }

    private readonly string _src;
    private int _pos, _line = 1, _lineStart;
    private readonly List<int> _indent = new() { 0 };
    private readonly List<Frame> _frames = new();
    private readonly List<LamTrack> _lam = new();
    private bool _pending;           // the current logical line has content, so a newline ends a statement
    private char _indentChar;        // ' ' or '\t' once the first indented line was seen

    public Lexer(string text)
    {
        if (text.Length > 0 && text[0] == '\uFEFF') text = text.Substring(1);
        _src = text.Replace("\r\n", "\n").Replace('\r', '\n');
    }

    public static Lexer Tokenize(string text)
    {
        var l = new Lexer(text);
        l.Run();
        return l;
    }

    private char Cur => _pos < _src.Length ? _src[_pos] : '\0';
    private char At(int o) => _pos + o < _src.Length ? _src[_pos + o] : '\0';
    private int Col => _pos - _lineStart + 1;
    private bool AtEnd => _pos >= _src.Length;

    private void Error(string msg, int line, int col) => Errors.Add(new ParseError(line, col, msg));
    private void Error(string msg) => Error(msg, _line, Col);

    private static bool IsIdentStart(char c) => c == '_' || char.IsLetter(c);
    private static bool IsIdentPart(char c) => c == '_' || char.IsLetterOrDigit(c);
    private static bool IsDigit(char c) => c >= '0' && c <= '9';
    private static bool IsQuote(char c) => c == '"' || c == '\'';

    private void NewLine() { _pos++; _line++; _lineStart = _pos; }

    public void Run()
    {
        bool lineStart = true;
        while (true)
        {
            if (lineStart)
            {
                lineStart = false;
                if (!BeginLine()) break;
            }
            while (Cur == ' ' || Cur == '\t') _pos++;
            if (AtEnd) break;
            char c = Cur;
            if (c == '\n')
            {
                bool inBracket = _frames.Count > 0 && !_frames[^1].Lambda;
                NewLine();
                if (inBracket) continue;
                EmitNewlineIfPending();
                lineStart = true;
                continue;
            }
            if (c == '\\' && ContinuationAhead())
            {
                while (Cur != '\n') _pos++;
                NewLine();
                continue;
            }
            if (c == '#') { ReadComment(); continue; }
            Scan();
        }
        Finish();
    }

    private bool ContinuationAhead()
    {
        int i = _pos + 1;
        while (i < _src.Length && (_src[i] == ' ' || _src[i] == '\t')) i++;
        return i < _src.Length && _src[i] == '\n';
    }

    // ---- lines and indentation ----

    // Moves to the first content character of the next logical line and settles its indentation. False at the end of the text.
    private bool BeginLine()
    {
        while (true)
        {
            int width = 0;
            bool tab = false, space = false;
            while (Cur == ' ' || Cur == '\t')
            {
                if (Cur == '\t') { width += 4; tab = true; } else { width++; space = true; }
                _pos++;
            }
            if (AtEnd) return false;
            if (Cur == '\n') { NewLine(); continue; }
            if (Cur == '#') { ReadComment(); if (Cur == '\n') NewLine(); continue; }
            if (tab || space)
            {
                char kind = tab ? '\t' : ' ';
                if (_indentChar == '\0') _indentChar = kind;
                if ((tab && space) || kind != _indentChar) Error("Mixed use of tabs and spaces for indentation", _line, 1);
            }
            ApplyIndent(width);
            return true;
        }
    }

    private void ApplyIndent(int width)
    {
        while (_frames.Count > 0 && _frames[^1].Lambda)
        {
            Frame f = _frames[^1];
            if (_indent.Count == f.Base) // the body has not started
            {
                if (width > _indent[^1]) { _indent.Add(width); Emit(TokKind.Indent, "", _line, Col, 0); return; }
                Error("Expected an indented block in the lambda");
                _frames.RemoveAt(_frames.Count - 1);
                continue;
            }
            while (_indent.Count > f.Base && width < _indent[^1]) { _indent.RemoveAt(_indent.Count - 1); Emit(TokKind.Dedent, "", _line, Col, 0); }
            if (_indent.Count == f.Base) { _frames.RemoveAt(_frames.Count - 1); continue; } // the lambda ended; its enclosing bracket goes on
            if (width > _indent[^1]) { _indent.Add(width); Emit(TokKind.Indent, "", _line, Col, 0); }
            else if (width != _indent[^1]) Error("Unindent does not match any outer indentation level");
            return;
        }
        if (_frames.Count > 0) return; // inside brackets the indentation means nothing
        if (width > _indent[^1]) { _indent.Add(width); Emit(TokKind.Indent, "", _line, Col, 0); return; }
        while (width < _indent[^1]) { _indent.RemoveAt(_indent.Count - 1); Emit(TokKind.Dedent, "", _line, Col, 0); }
        if (width != _indent[^1]) Error("Unindent does not match any outer indentation level");
    }

    private void EmitNewlineIfPending()
    {
        if (!_pending) return;
        _pending = false;
        Token last = Tokens[^1];
        Tokens.Add(new Token { Kind = TokKind.Newline, Line = last.EndLine, Col = last.EndCol, EndLine = last.EndLine, EndCol = last.EndCol });
    }

    private void PopLambdaFrame()
    {
        Frame f = _frames[^1];
        EmitNewlineIfPending();
        while (_indent.Count > f.Base) { _indent.RemoveAt(_indent.Count - 1); Emit(TokKind.Dedent, "", _line, Col, 0); }
        _frames.RemoveAt(_frames.Count - 1);
    }

    private void Finish()
    {
        while (_frames.Count > 0)
        {
            Frame f = _frames[^1];
            if (f.Lambda) PopLambdaFrame();
            else { Error($"Unclosed \"{OpenOf(f.Close)}\"", f.Line, f.Col); _frames.RemoveAt(_frames.Count - 1); }
        }
        EmitNewlineIfPending();
        while (_indent.Count > 1) { _indent.RemoveAt(_indent.Count - 1); Emit(TokKind.Dedent, "", _line, Col, 0); }
        Tokens.Add(new Token { Kind = TokKind.Eof, Line = _line, Col = Col, EndLine = _line, EndCol = Col });
    }

    private static char OpenOf(char close) => close == ')' ? '(' : close == ']' ? '[' : '{';

    private void ReadComment()
    {
        int col = Col;
        bool own = true;
        for (int i = _lineStart; i < _pos; i++) if (_src[i] != ' ' && _src[i] != '\t') { own = false; break; }
        int s = _pos;
        while (!AtEnd && Cur != '\n') _pos++;
        string text = _src.Substring(s, _pos - s);
        bool doc = text.StartsWith("##");
        string body = doc ? text.Substring(2) : text.Substring(1);
        if (body.StartsWith(' ')) body = body.Substring(1);
        Comments.Add(new Comment(_line, col, body.TrimEnd(), doc, own));
    }

    // ---- tokens ----

    private Token Emit(TokKind kind, string text, int line, int col, int _)
    {
        var t = new Token { Kind = kind, Text = text, Line = line, Col = col, EndLine = _line, EndCol = Col };
        Tokens.Add(t);
        if (kind != TokKind.Indent && kind != TokKind.Dedent) _pending = true;
        return t;
    }

    private bool PrevIsValue()
    {
        if (Tokens.Count == 0) return false;
        Token t = Tokens[^1];
        return t.Kind switch
        {
            TokKind.Ident or TokKind.Int or TokKind.Float or TokKind.String or TokKind.NodePath => true,
            TokKind.Keyword => t.Text is "self" or "true" or "false" or "null" or "super",
            TokKind.Op => t.Text is ")" or "]" or "}",
            _ => false,
        };
    }

    private void Scan()
    {
        char c = Cur;
        int sl = _line, sc = Col;

        if (IsIdentStart(c))
        {
            int s = _pos;
            while (IsIdentPart(Cur)) _pos++;
            string id = _src.Substring(s, _pos - s);
            if (id == "r" && IsQuote(Cur)) { ReadString(sl, sc, s, raw: true, StrKind.Normal); return; }
            Token t = Emit(Keywords.Contains(id) ? TokKind.Keyword : TokKind.Ident, id, sl, sc, 0);
            Track(t);
            return;
        }
        if (IsDigit(c) || (c == '.' && IsDigit(At(1)) && !PrevIsValue())) { ReadNumber(sl, sc); return; }
        if (IsQuote(c)) { ReadString(sl, sc, _pos, raw: false, StrKind.Normal); return; }
        if (c == '&' && IsQuote(At(1))) { int s = _pos; _pos++; ReadString(sl, sc, s, raw: false, StrKind.StringName); return; }
        if (c == '^' && IsQuote(At(1))) { int s = _pos; _pos++; ReadString(sl, sc, s, raw: false, StrKind.NodePath); return; }
        if (c == '@')
        {
            _pos++;
            if (!IsIdentStart(Cur)) { Error("Expected an annotation name after \"@\"", sl, sc); return; }
            int s = _pos;
            while (IsIdentPart(Cur)) _pos++;
            Emit(TokKind.Annotation, _src.Substring(s, _pos - s), sl, sc, 0);
            return;
        }
        if (c == '$') { ReadNodePath(sl, sc); return; }
        if (c == '%' && !PrevIsValue() && (IsIdentStart(At(1)) || IsQuote(At(1)))) { ReadNodePath(sl, sc); return; }
        ReadOperator(sl, sc);
    }

    private void ReadOperator(int sl, int sc)
    {
        string? op = null;
        foreach (string o in s_ops3) if (string.CompareOrdinal(_src, _pos, o, 0, 3) == 0) { op = o; break; }
        if (op == null) foreach (string o in s_ops2) if (string.CompareOrdinal(_src, _pos, o, 0, 2) == 0) { op = o; break; }
        if (op == null && Ops1.IndexOf(Cur) >= 0) op = Cur.ToString();
        if (op == null)
        {
            Error($"Unknown character \"{Cur}\"", sl, sc);
            _pos++;
            return;
        }
        char c0 = op[0];
        _pos += op.Length;
        if (op.Length == 1 && (c0 == '(' || c0 == '[' || c0 == '{'))
        {
            _frames.Add(new Frame { Close = c0 == '(' ? ')' : c0 == '[' ? ']' : '}', Line = sl, Col = sc });
        }
        else if (op.Length == 1 && (c0 == ')' || c0 == ']' || c0 == '}'))
        {
            while (_frames.Count > 0 && _frames[^1].Lambda) PopLambdaFrame(); // a lambda body ends where its bracket closes
            if (_frames.Count > 0 && _frames[^1].Close == c0) _frames.RemoveAt(_frames.Count - 1);
            else Error($"Unmatched \"{c0}\"", sl, sc);
        }
        Token t = Emit(TokKind.Op, op, sl, sc, 0);
        Track(t);
    }

    // Spots "func(...) -> T:" inside brackets and, when nothing but a comment follows the colon, opens an indented lambda body.
    private void Track(Token t)
    {
        if (t.Kind == TokKind.Keyword && t.Text == "func" && _frames.Count > 0) { _lam.Add(new LamTrack { Depth = _frames.Count }); return; }
        if (_lam.Count == 0) return;
        LamTrack tr = _lam[^1];
        if (t.Kind == TokKind.Op)
        {
            if (t.Text == "(" && tr.Phase == 0 && _frames.Count == tr.Depth + 1) tr.Phase = 1;
            else if (t.Text == ")" && tr.Phase == 1 && _frames.Count == tr.Depth) tr.Phase = 2;
            else if (t.Text == ":" && tr.Phase == 2 && _frames.Count == tr.Depth)
            {
                _lam.RemoveAt(_lam.Count - 1);
                if (RestOfLineEmpty()) _frames.Add(new Frame { Lambda = true, Base = _indent.Count, Line = t.Line, Col = t.Col });
                return;
            }
        }
        if (_frames.Count < tr.Depth) _lam.RemoveAt(_lam.Count - 1); // the bracket around the "func" closed first
    }

    private bool RestOfLineEmpty()
    {
        int i = _pos;
        while (i < _src.Length && (_src[i] == ' ' || _src[i] == '\t')) i++;
        return i >= _src.Length || _src[i] == '\n' || _src[i] == '#';
    }

    // ---- numbers ----

    private void ReadNumber(int sl, int sc)
    {
        int s = _pos;
        bool isFloat = false;
        object? value = null;
        if (Cur == '0' && (At(1) == 'x' || At(1) == 'X'))
        {
            _pos += 2;
            int ds = _pos;
            while (Uri.IsHexDigit(Cur) || Cur == '_') _pos++;
            string digits = _src.Substring(ds, _pos - ds).Replace("_", "");
            if (digits.Length == 0 || !ulong.TryParse(digits, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong u)) { Error("Invalid hexadecimal number", sl, sc); u = 0; }
            value = unchecked((long)u);
        }
        else if (Cur == '0' && (At(1) == 'b' || At(1) == 'B'))
        {
            _pos += 2;
            int ds = _pos;
            while (Cur == '0' || Cur == '1' || Cur == '_') _pos++;
            string digits = _src.Substring(ds, _pos - ds).Replace("_", "");
            ulong u = 0;
            if (digits.Length == 0 || digits.Length > 64) Error("Invalid binary number", sl, sc);
            else foreach (char ch in digits) u = (u << 1) | (ch == '1' ? 1UL : 0UL);
            value = unchecked((long)u);
        }
        else
        {
            while (IsDigit(Cur) || Cur == '_') _pos++;
            if (Cur == '.' && IsDigit(At(1))) { isFloat = true; _pos++; while (IsDigit(Cur) || Cur == '_') _pos++; }
            else if (Cur == '.' && At(1) != '.' && !IsIdentStart(At(1)) && _pos > s) { isFloat = true; _pos++; } // "1."
            if ((Cur == 'e' || Cur == 'E') && (IsDigit(At(1)) || ((At(1) == '+' || At(1) == '-') && IsDigit(At(2)))))
            {
                isFloat = true;
                _pos += 2;
                while (IsDigit(Cur)) _pos++;
            }
            string text = _src.Substring(s, _pos - s).Replace("_", "");
            if (isFloat)
            {
                if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double d)) { Error("Invalid number", sl, sc); d = 0; }
                value = d;
            }
            else
            {
                if (!long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out long l)) { Error("Integer constant is too large", sl, sc); l = 0; }
                value = l;
            }
        }
        Token t = Emit(isFloat ? TokKind.Float : TokKind.Int, _src.Substring(s, _pos - s), sl, sc, 0);
        t.Value = value;
    }

    // ---- strings ----

    // _pos is on the opening quote; `start` is where the token's source text begins (a prefix before the quote).
    private void ReadString(int sl, int sc, int start, bool raw, StrKind kind)
    {
        char q = Cur;
        bool triple = At(1) == q && At(2) == q;
        _pos += triple ? 3 : 1;
        var sb = new StringBuilder();
        bool closed = false;
        while (!AtEnd)
        {
            char c = Cur;
            if (c == q)
            {
                if (!triple) { _pos++; closed = true; break; }
                if (At(1) == q && At(2) == q) { _pos += 3; closed = true; break; }
                sb.Append(c); _pos++; continue;
            }
            if (c == '\n')
            {
                if (!triple) break;
                sb.Append('\n'); NewLine(); continue;
            }
            if (c == '\\')
            {
                if (raw)
                {
                    sb.Append(c); _pos++;
                    if (!AtEnd && Cur == q) { sb.Append(Cur); _pos++; }
                    continue;
                }
                ReadEscape(sb);
                continue;
            }
            sb.Append(c); _pos++;
        }
        if (!closed) Error("Unterminated string", sl, sc);
        Token t = Emit(TokKind.String, _src.Substring(start, _pos - start), sl, sc, 0);
        t.Value = sb.ToString();
        t.StrKind = kind;
        t.Raw = raw;
        t.Triple = triple;
    }

    private void ReadEscape(StringBuilder sb)
    {
        int line = _line, col = Col;
        _pos++; // backslash
        char e = Cur;
        if (AtEnd) return;
        _pos++;
        switch (e)
        {
            case 'n': sb.Append('\n'); break;
            case 't': sb.Append('\t'); break;
            case 'r': sb.Append('\r'); break;
            case 'a': sb.Append('\a'); break;
            case 'b': sb.Append('\b'); break;
            case 'f': sb.Append('\f'); break;
            case 'v': sb.Append('\v'); break;
            case '\\': sb.Append('\\'); break;
            case '"': sb.Append('"'); break;
            case '\'': sb.Append('\''); break;
            case '\n': _line++; _lineStart = _pos; break; // a backslash before the newline joins the lines
            case 'u':
            case 'U':
            {
                int n = e == 'u' ? 4 : 6;
                int s = _pos;
                while (_pos - s < n && Uri.IsHexDigit(Cur)) _pos++;
                if (_pos - s != n || !int.TryParse(_src.AsSpan(s, n), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out int cp) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                {
                    Error("Invalid Unicode escape", line, col);
                    break;
                }
                sb.Append(char.ConvertFromUtf32(cp));
                break;
            }
            default:
                Error($"Invalid escape sequence \"\\{e}\"", line, col);
                sb.Append(e);
                break;
        }
    }

    // ---- $Node/Path, $"quoted/path", $%Unique, %Unique ----

    private void ReadNodePath(int sl, int sc)
    {
        var sb = new StringBuilder();
        bool unique = Cur == '%';
        if (Cur == '$') _pos++;
        if (Cur == '%') { sb.Append('%'); _pos++; }
        if (!ReadPathElement(sb)) { Error("Expected a node name after \"" + _src[sc - 1 + _lineStart] + "\"", sl, sc); return; }
        while (Cur == '/')
        {
            sb.Append('/'); _pos++;
            if (Cur == '%') { sb.Append('%'); _pos++; }
            if (!ReadPathElement(sb)) { Error("Expected a node name after \"/\"", _line, Col); break; }
        }
        Token t = Emit(TokKind.NodePath, sb.ToString(), sl, sc, 0);
        t.Value = t.Text;
        t.Unique = unique;
    }

    private bool ReadPathElement(StringBuilder sb)
    {
        if (IsQuote(Cur))
        {
            char q = Cur;
            _pos++;
            while (!AtEnd && Cur != q && Cur != '\n') { sb.Append(Cur); _pos++; }
            if (Cur == q) _pos++; else Error("Unterminated string");
            return true;
        }
        if (!IsIdentStart(Cur)) return false;
        while (IsIdentPart(Cur)) { sb.Append(Cur); _pos++; }
        return true;
    }
}
