// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GDScript 2.0 parser (G3 F4a): tokens in, typed syntax tree out. A syntax error is recorded with its line:column and the parser
// goes on with the next statement or member.

namespace Aver.Fabric.Convert.GdScript;

public sealed class ParseResult
{
    public ClassDecl Script = null!;
    public List<ParseError> Errors = new();
    public List<Comment> Comments = new();
    public List<Token> Tokens = new();
    public bool Ok => Errors.Count == 0;
}

public static class GdScriptParser
{
    /// <summary>Parses a whole script. Never throws on bad source: the problems are in <see cref="ParseResult.Errors"/>.</summary>
    public static ParseResult Parse(string text)
    {
        Lexer lx = Lexer.Tokenize(text);
        var parser = new Parser(lx);
        ClassDecl script = parser.ParseScript();
        return parser.Finish(script);
    }

    /// <summary>Parses one expression (an assignment is accepted too). Null when it could not be parsed.</summary>
    public static Expr? ParseExpression(string text, out List<ParseError> errors)
    {
        Lexer lx = Lexer.Tokenize(text);
        var parser = new Parser(lx);
        Expr? e = parser.ParseSingleExpression();
        errors = parser.Finish(null).Errors;
        return e;
    }
}

internal sealed class ParseException : Exception
{
    public readonly Token At;
    public ParseException(Token at, string message) : base(message) { At = at; }
}

internal sealed class Parser
{
    // Binding powers, lowest first. The order follows Godot's own table.
    private const int PAssign = 1, PCast = 2, PTernary = 3, POr = 4, PAnd = 5, PNot = 6, PIn = 7, PCompare = 8, PBitOr = 9, PBitXor = 10,
        PBitAnd = 11, PShift = 12, PAdd = 13, PFactor = 14, PSign = 15, PBitNot = 16, PPower = 17, PTypeTest = 18, PAwait = 19,
        PCall = 20, PAttr = 21, PSubscript = 22;

    private static readonly HashSet<string> ScriptAnnotations = new() { "tool", "icon", "static_unload" };
    private static readonly HashSet<string> StandaloneAnnotations =
        new() { "export_group", "export_subgroup", "export_category", "warning_ignore_start", "warning_ignore_restore" };

    private readonly Lexer _lexer;
    private readonly List<Token> _toks;
    private readonly List<ParseError> _errors = new();
    private readonly Dictionary<int, string> _docs = new();
    private int _p;
    private Pos _lastEnd;
    private HashSet<int>? _codeLines;

    public Parser(Lexer lexer)
    {
        _lexer = lexer;
        _toks = lexer.Tokens;
        foreach (Comment c in lexer.Comments) if (c.IsDoc && c.OwnLine) _docs[c.Line] = c.Text;
    }

    public ParseResult Finish(ClassDecl? script)
    {
        var all = new List<ParseError>(_lexer.Errors);
        all.AddRange(_errors);
        var seen = new HashSet<(int, int, string)>();
        var result = new ParseResult
        {
            Script = script!,
            Comments = _lexer.Comments,
            Tokens = _toks,
            Errors = all.Where(e => seen.Add((e.Line, e.Col, e.Message))).OrderBy(e => e.Line).ThenBy(e => e.Col).ToList(),
        };
        return result;
    }

    // ---- token helpers ----

    private Token Cur => _toks[_p];
    private Token Prev => _toks[Math.Max(_p - 1, 0)];
    private Token Peek(int n = 1) => _toks[Math.Min(_p + n, _toks.Count - 1)];

    private Token Next()
    {
        Token t = _toks[_p];
        if (t.Kind != TokKind.Eof)
        {
            _p++;
            if (t.Kind is not (TokKind.Newline or TokKind.Indent or TokKind.Dedent)) _lastEnd = new Pos(t.EndLine, t.EndCol);
        }
        return t;
    }

    private bool IsOp(string s) => Cur.Kind == TokKind.Op && Cur.Text == s;
    private bool IsKw(string s) => Cur.Kind == TokKind.Keyword && Cur.Text == s;
    private bool MatchOp(string s) { if (!IsOp(s)) return false; Next(); return true; }
    private bool MatchKw(string s) { if (!IsKw(s)) return false; Next(); return true; }

    private static string Describe(Token t) => t.Kind switch
    {
        TokKind.Newline => "end of line",
        TokKind.Indent => "indentation",
        TokKind.Dedent => "unindent",
        TokKind.Eof => "end of file",
        TokKind.Annotation => "\"@" + t.Text + "\"",
        TokKind.String or TokKind.NodePath => "a string",
        _ => "\"" + t.Text + "\"",
    };

    private static ParseException Err(Token at, string msg) => new(at, msg);

    private Token ExpectOp(string s, string? ctx = null)
    {
        if (!IsOp(s)) throw Err(Cur, $"Expected \"{s}\"{(ctx != null ? " " + ctx : "")} but found {Describe(Cur)}");
        return Next();
    }

    private Token ExpectKw(string s)
    {
        if (!IsKw(s)) throw Err(Cur, $"Expected \"{s}\" but found {Describe(Cur)}");
        return Next();
    }

    private Token ExpectIdent(string what)
    {
        if (Cur.Kind != TokKind.Ident) throw Err(Cur, $"Expected {what} but found {Describe(Cur)}");
        return Next();
    }

    private void Report(ParseException e) => _errors.Add(new ParseError(e.At.Line, e.At.Col, e.Message));

    private static Pos P(Token t) => new(t.Line, t.Col);

    private T Fin<T>(T node, Token start) where T : Node
    {
        node.Start = P(start);
        node.End = _lastEnd;
        return node;
    }

    private T FinFrom<T>(T node, Pos start) where T : Node
    {
        node.Start = start;
        node.End = _lastEnd;
        return node;
    }

    // A statement ends at a newline. A lambda or property body that closed with an unindent has ended it already.
    private void EndLine()
    {
        if (Cur.Kind == TokKind.Newline) { Next(); return; }
        if (Cur.Kind is TokKind.Eof or TokKind.Dedent) return;
        if (Prev.Kind is TokKind.Dedent or TokKind.Newline) return;
        throw Err(Cur, $"Expected end of statement but found {Describe(Cur)}");
    }

    private string? DocFor(Token first)
    {
        var lines = new List<string>();
        for (int l = first.Line - 1; _docs.TryGetValue(l, out string? d); l--) lines.Insert(0, d);
        return lines.Count == 0 ? null : string.Join("\n", lines);
    }

    // Godot also reads the script's own doc comment from the block right under extends / class_name, unless code follows it
    // directly (then the block documents that member).
    private string? ClassDocBelow()
    {
        int line = _lastEnd.Line + 1;
        var lines = new List<string>();
        while (_docs.TryGetValue(line, out string? d)) { lines.Add(d); line++; }
        if (lines.Count == 0) return null;
        _codeLines ??= new HashSet<int>(_toks.Where(t => t.Kind is not (TokKind.Newline or TokKind.Indent or TokKind.Dedent or TokKind.Eof)).Select(t => t.Line));
        return _codeLines.Contains(line) ? null : string.Join("\n", lines);
    }

    // ---- error recovery ----

    private void SkipLine()
    {
        while (Cur.Kind is not (TokKind.Eof or TokKind.Dedent))
        {
            bool nl = Cur.Kind == TokKind.Newline;
            Next();
            if (nl) return;
        }
    }

    private void SkipBlock()
    {
        if (Cur.Kind != TokKind.Indent) return;
        int depth = 0;
        while (Cur.Kind != TokKind.Eof)
        {
            if (Cur.Kind == TokKind.Indent) depth++;
            else if (Cur.Kind == TokKind.Dedent) depth--;
            Next();
            if (depth == 0) return;
        }
    }

    // After an error: drop the rest of the line, the block under it, and any elif/else that belonged to a dropped if.
    private void Sync()
    {
        SkipLine();
        SkipBlock();
        while (IsKw("elif") || IsKw("else"))
        {
            SkipLine();
            SkipBlock();
        }
    }

    // ---- script and class bodies ----

    public ClassDecl ParseScript()
    {
        var root = new ClassDecl { Start = new Pos(1, 1) };
        ParseClassBody(root, inner: false);
        root.End = _lastEnd.Line == 0 ? new Pos(1, 1) : _lastEnd;
        return root;
    }

    private void ParseClassBody(ClassDecl c, bool inner)
    {
        while (true)
        {
            while (Cur.Kind == TokKind.Newline) Next();
            if (Cur.Kind == TokKind.Eof) return;
            if (Cur.Kind == TokKind.Dedent) { Next(); if (inner) return; continue; }
            if (Cur.Kind == TokKind.Indent)
            {
                _errors.Add(new ParseError(Cur.Line, Cur.Col, "Unexpected indentation"));
                SkipBlock();
                continue;
            }
            int before = _p;
            Token at = Cur;
            try { ParseMember(c); }
            catch (ParseException e)
            {
                Report(e);
                Sync();
                c.Members.Add(Fin(new ErrorStmt(), at));
                if (_p == before && Cur.Kind is not (TokKind.Eof or TokKind.Dedent)) Next();
            }
        }
    }

    private void ParseMember(ClassDecl c)
    {
        Token first = Cur;
        var anns = new List<Annotation>();
        while (Cur.Kind == TokKind.Annotation)
        {
            Annotation a = ParseAnnotation();
            if (ScriptAnnotations.Contains(a.Name)) c.Annotations.Add(a);
            else if (StandaloneAnnotations.Contains(a.Name))
            {
                var sa = new StandaloneAnnotation { Annotation = a, Start = a.Start, End = a.End };
                c.Members.Add(sa);
            }
            else anns.Add(a);
            if (Cur.Kind == TokKind.Newline) Next();
        }
        if (Cur.Kind is TokKind.Eof or TokKind.Dedent)
        {
            if (anns.Count > 0) throw Err(Cur, $"Expected a declaration after \"@{anns[^1].Name}\"");
            return;
        }
        if (Cur.Kind == TokKind.Newline) { Next(); return; }

        Token t = Cur;
        Member? m = null;
        if (t.Kind == TokKind.Keyword)
        {
            switch (t.Text)
            {
                case "extends":
                    if (c.Extends != null) throw Err(t, "\"extends\" can only be used once");
                    c.DocComment ??= DocFor(t);
                    c.Extends = ParseExtends();
                    EndLine();
                    c.DocComment ??= ClassDocBelow();
                    break;
                case "class_name":
                    ParseClassName(c);
                    EndLine();
                    c.DocComment ??= ClassDocBelow();
                    break;
                case "signal": m = ParseSignal(); EndLine(); break;
                case "enum": m = ParseEnum(); EndLine(); break;
                case "const": m = ParseConst(); EndLine(); break;
                case "var": m = ParseVar(false, true, t); EndLine(); break;
                case "func": m = ParseFunc(false, t); break;
                case "class": m = ParseInnerClass(); break;
                case "static":
                    Next();
                    if (IsKw("var")) { m = ParseVar(true, true, t); EndLine(); }
                    else if (IsKw("func")) m = ParseFunc(true, t);
                    else throw Err(Cur, $"Expected \"var\" or \"func\" after \"static\" but found {Describe(Cur)}");
                    break;
                case "pass": Next(); EndLine(); break;
                default: throw Err(t, $"Expected a declaration but found {Describe(t)}");
            }
        }
        else if (IsOp(";")) Next();
        else throw Err(t, $"Expected a declaration but found {Describe(t)}");

        if (m == null)
        {
            if (anns.Count > 0) throw Err(t, $"\"@{anns[^1].Name}\" cannot be used on this");
            return;
        }
        m.Annotations.InsertRange(0, anns);
        m.DocComment = DocFor(first);
        c.Members.Add(m);
    }

    private Annotation ParseAnnotation()
    {
        Token t = Next();
        var a = new Annotation { Name = t.Text };
        if (IsOp("(") && Cur.Line == t.Line)
        {
            Next();
            a.Args = ParseArgs(")");
        }
        return Fin(a, t);
    }

    private ExtendsClause ParseExtends()
    {
        Token start = Next(); // extends
        var e = new ExtendsClause();
        if (Cur.Kind == TokKind.String)
        {
            Token s = Next();
            e.ResPath = (string)s.Value!;
            while (MatchOp(".")) e.Path.Add(ExpectIdent("an inner class name").Text);
        }
        else
        {
            e.Path.Add(ExpectIdent("a class name").Text);
            while (MatchOp(".")) e.Path.Add(ExpectIdent("a class name").Text);
        }
        return Fin(e, start);
    }

    private void ParseClassName(ClassDecl c)
    {
        c.DocComment ??= DocFor(Next()); // class_name
        Token name = ExpectIdent("a class name");
        if (c.Name != null) throw Err(name, "\"class_name\" can only be used once");
        c.Name = name.Text;
        if (MatchOp(",")) // Godot 3 form: class_name Foo, "res://icon.png"
        {
            Token icon = Cur;
            if (Cur.Kind != TokKind.String) throw Err(Cur, "Expected an icon path string");
            Next();
            c.Annotations.Add(new Annotation
            {
                Name = "icon",
                Args = { new Literal { Kind = LitKind.String, Value = icon.Value, Text = icon.Text, Start = P(icon), End = new Pos(icon.EndLine, icon.EndCol) } },
                Start = P(icon), End = new Pos(icon.EndLine, icon.EndCol),
            });
        }
        if (IsKw("extends"))
        {
            if (c.Extends != null) throw Err(Cur, "\"extends\" can only be used once");
            c.Extends = ParseExtends();
        }
    }

    private ClassDecl ParseInnerClass()
    {
        Token start = Next(); // class
        var c = new ClassDecl { IsInner = true };
        c.Name = ExpectIdent("a class name").Text;
        if (IsKw("extends")) c.Extends = ParseExtends();
        ExpectOp(":");
        if (Cur.Kind != TokKind.Newline) throw Err(Cur, "Expected an indented class body");
        Next();
        if (Cur.Kind != TokKind.Indent) throw Err(Cur, "Expected an indented class body");
        Next();
        ParseClassBody(c, inner: true);
        return Fin(c, start);
    }

    private SignalDecl ParseSignal()
    {
        Token start = Next(); // signal
        var s = new SignalDecl { Name = ExpectIdent("a signal name").Text };
        if (MatchOp("("))
        {
            s.Params = ParseParams(")");
            ExpectOp(")");
        }
        return Fin(s, start);
    }

    private EnumDecl ParseEnum()
    {
        Token start = Next(); // enum
        var e = new EnumDecl();
        if (Cur.Kind == TokKind.Ident) e.Name = Next().Text;
        ExpectOp("{", "after \"enum\"");
        while (!IsOp("}"))
        {
            Token n = ExpectIdent("an enum value name");
            var v = new EnumValue { Name = n.Text };
            if (MatchOp("=")) v.Value = ParseExpr();
            e.Values.Add(Fin(v, n));
            if (!MatchOp(",")) break;
        }
        ExpectOp("}");
        return Fin(e, start);
    }

    private ConstDecl ParseConst()
    {
        Token start = Next(); // const
        var c = new ConstDecl { Name = ExpectIdent("a constant name").Text };
        if (MatchOp(":=")) { c.Inferred = true; c.Value = ParseExpr(); }
        else
        {
            if (MatchOp(":")) c.Type = ParseType();
            ExpectOp("=", "in a constant declaration");
            c.Value = ParseExpr();
        }
        return Fin(c, start);
    }

    private VarDecl ParseVar(bool isStatic, bool allowProperty, Token start)
    {
        Next(); // var
        var v = new VarDecl { IsStatic = isStatic, Name = ExpectIdent("a variable name").Text };
        if (MatchOp(":="))
        {
            v.Inferred = true;
            v.Init = ParseExpr();
        }
        else
        {
            if (IsOp(":"))
            {
                Next();
                if (allowProperty && ((Cur.Kind == TokKind.Newline && Peek().Kind == TokKind.Indent) || PropertyClauseAhead()))
                {
                    ParsePropertyBlock(v);
                    return Fin(v, start);
                }
                v.Type = ParseType();
            }
            if (MatchOp("=")) v.Init = ParseExpr();
        }
        if (allowProperty && IsOp(":"))
        {
            Next();
            ParsePropertyBlock(v);
        }
        return Fin(v, start);
    }

    private bool PropertyClauseAhead() =>
        Cur.Kind == TokKind.Ident && Cur.Text is "set" or "get" && Peek().Kind == TokKind.Op && Peek().Text is "(" or "=" or ":" or ",";

    private void ParsePropertyBlock(VarDecl v)
    {
        if (Cur.Kind == TokKind.Newline)
        {
            Next();
            if (Cur.Kind != TokKind.Indent) throw Err(Cur, "Expected an indented property block");
            Next();
            while (true)
            {
                while (Cur.Kind == TokKind.Newline) Next();
                if (Cur.Kind == TokKind.Dedent) { Next(); break; }
                if (Cur.Kind == TokKind.Eof) break;
                ParseAccessor(v);
                while (MatchOp(",")) ParseAccessor(v);
            }
            return;
        }
        ParseAccessor(v);
        while (MatchOp(",")) ParseAccessor(v);
    }

    private void ParseAccessor(VarDecl v)
    {
        Token t = Cur;
        if (t.Kind != TokKind.Ident || t.Text is not ("set" or "get")) throw Err(t, $"Expected \"set\" or \"get\" but found {Describe(t)}");
        Next();
        var acc = new PropertyAccessor { IsSetter = t.Text == "set" };
        if (MatchOp("=")) acc.Delegate = ExpectIdent("a function name").Text;
        else
        {
            if (MatchOp("("))
            {
                if (!IsOp(")")) acc.ParamName = ExpectIdent("a parameter name").Text;
                ExpectOp(")");
            }
            ExpectOp(":");
            acc.Body = ParseSuite();
        }
        Fin(acc, t);
        if (acc.IsSetter) { if (v.Setter != null) throw Err(t, "Setter is already defined"); v.Setter = acc; }
        else { if (v.Getter != null) throw Err(t, "Getter is already defined"); v.Getter = acc; }
    }

    private FuncDecl ParseFunc(bool isStatic, Token start)
    {
        Next(); // func
        var f = new FuncDecl { IsStatic = isStatic, Name = ExpectIdent("a function name").Text };
        ExpectOp("(", "after the function name");
        f.Params = ParseParams(")");
        ExpectOp(")");
        if (MatchOp("->")) f.ReturnType = ParseType();
        ExpectOp(":", "after the function signature");
        f.Body = ParseSuite();
        return Fin(f, start);
    }

    private List<Param> ParseParams(string close)
    {
        var list = new List<Param>();
        while (!IsOp(close))
        {
            Token start = Cur;
            var p = new Param();
            if (MatchOp("...")) p.IsRest = true;
            p.Name = ExpectIdent("a parameter name").Text;
            if (MatchOp(":="))
            {
                p.Inferred = true;
                p.Default = ParseExpr();
            }
            else
            {
                if (MatchOp(":")) p.Type = ParseType();
                if (MatchOp("=")) p.Default = ParseExpr();
            }
            list.Add(Fin(p, start));
            if (!MatchOp(",")) break;
        }
        return list;
    }

    private TypeRef ParseType()
    {
        Token start = Cur;
        var t = new TypeRef();
        if (IsKw("void")) { Next(); t.Parts.Add("void"); return Fin(t, start); }
        t.Parts.Add(ExpectIdent("a type name").Text);
        while (IsOp(".") && Peek().Kind == TokKind.Ident) { Next(); t.Parts.Add(Next().Text); }
        if (IsOp("["))
        {
            Next();
            do t.Args.Add(ParseType()); while (MatchOp(","));
            ExpectOp("]", "to close the type arguments");
        }
        return Fin(t, start);
    }

    // ---- blocks and statements ----

    // The body after a ':': an indented block, or the statements on the same line.
    private Block ParseSuite()
    {
        Token start = Cur;
        var b = new Block();
        if (Cur.Kind == TokKind.Newline)
        {
            Next();
            if (Cur.Kind != TokKind.Indent) throw Err(Cur, "Expected an indented block");
            Next();
            ParseStatements(b.Statements);
        }
        else ParseSimpleLine(b.Statements);
        return FinBlock(b, start);
    }

    private Block FinBlock(Block b, Token start)
    {
        b.Start = b.Statements.Count > 0 ? b.Statements[0].Start : P(start);
        b.End = b.Statements.Count > 0 ? b.Statements[^1].End : P(start);
        return b;
    }

    private void ParseStatements(List<Stmt> into)
    {
        while (true)
        {
            while (Cur.Kind == TokKind.Newline) Next();
            if (Cur.Kind == TokKind.Dedent) { Next(); return; }
            if (Cur.Kind == TokKind.Eof) return;
            if (Cur.Kind == TokKind.Indent)
            {
                _errors.Add(new ParseError(Cur.Line, Cur.Col, "Unexpected indentation"));
                SkipBlock();
                continue;
            }
            int before = _p;
            Token at = Cur;
            try { ParseStatement(into); }
            catch (ParseException e)
            {
                Report(e);
                Sync();
                into.Add(Fin(new ErrorStmt(), at));
                if (_p == before && Cur.Kind is not (TokKind.Eof or TokKind.Dedent)) Next();
            }
        }
    }

    private void ParseStatement(List<Stmt> into)
    {
        List<Annotation>? anns = null;
        while (Cur.Kind == TokKind.Annotation)
        {
            (anns ??= new()).Add(ParseAnnotation());
            if (Cur.Kind == TokKind.Newline) Next();
        }
        int before = into.Count;
        if (Cur.Kind == TokKind.Keyword && Cur.Text is "if" or "while" or "for" or "match")
        {
            into.Add(Cur.Text switch { "if" => ParseIf(), "while" => ParseWhile(), "for" => ParseFor(), _ => ParseMatch() });
        }
        else ParseSimpleLine(into);
        if (anns != null)
        {
            if (into.Count == before) throw Err(Cur, $"Expected a statement after \"@{anns[^1].Name}\"");
            into[before].Annotations.InsertRange(0, anns);
        }
    }

    // One or more simple statements separated by ';', then the end of the line.
    private void ParseSimpleLine(List<Stmt> into)
    {
        do
        {
            if (Cur.Kind is TokKind.Newline or TokKind.Eof or TokKind.Dedent) break;
            into.Add(ParseSimple());
        } while (MatchOp(";"));
        EndLine();
    }

    private Stmt ParseSimple()
    {
        Token t = Cur;
        if (t.Kind == TokKind.Keyword)
        {
            switch (t.Text)
            {
                case "var": return ParseVar(false, false, t);
                case "const": return ParseConst();
                case "pass": Next(); return Fin(new PassStmt(), t);
                case "break": Next(); return Fin(new BreakStmt(), t);
                case "continue": Next(); return Fin(new ContinueStmt(), t);
                case "breakpoint": Next(); return Fin(new BreakpointStmt(), t);
                case "return":
                {
                    Next();
                    var r = new ReturnStmt();
                    bool none = Cur.Kind is TokKind.Newline or TokKind.Eof or TokKind.Dedent ||
                                (Cur.Kind == TokKind.Op && Cur.Text is ";" or ")" or "]" or "}" or ",");
                    if (!none) r.Value = ParseExpr();
                    return Fin(r, t);
                }
                case "assert":
                {
                    Next();
                    ExpectOp("(", "after \"assert\"");
                    List<Expr> args = ParseArgs(")");
                    if (args.Count is 0 or > 2) throw Err(t, "\"assert\" takes a condition and an optional message");
                    return Fin(new AssertStmt { Cond = args[0], Message = args.Count > 1 ? args[1] : null }, t);
                }
            }
        }
        Expr e = ParsePrec(PAssign);
        return Fin(new ExprStmt { Expr = e }, t);
    }

    private IfStmt ParseIf()
    {
        Token start = Next(); // if
        var s = new IfStmt();
        Token bs = start;
        while (true)
        {
            var br = new IfBranch { Cond = ParseExpr() };
            ExpectOp(":", "after the condition");
            br.Body = ParseSuite();
            s.Branches.Add(Fin(br, bs));
            if (IsKw("elif")) { bs = Next(); continue; }
            break;
        }
        if (IsKw("else"))
        {
            Next();
            ExpectOp(":", "after \"else\"");
            s.Else = ParseSuite();
        }
        return Fin(s, start);
    }

    private WhileStmt ParseWhile()
    {
        Token start = Next();
        var s = new WhileStmt { Cond = ParseExpr() };
        ExpectOp(":", "after the condition");
        s.Body = ParseSuite();
        return Fin(s, start);
    }

    private ForStmt ParseFor()
    {
        Token start = Next();
        var s = new ForStmt { VarName = ExpectIdent("a loop variable name").Text };
        if (MatchOp(":")) s.VarType = ParseType();
        ExpectKw("in");
        s.Iterable = ParseExpr();
        ExpectOp(":", "after the loop header");
        s.Body = ParseSuite();
        return Fin(s, start);
    }

    private MatchStmt ParseMatch()
    {
        Token start = Next();
        var m = new MatchStmt { Subject = ParseExpr() };
        ExpectOp(":", "after the match value");
        if (Cur.Kind != TokKind.Newline) throw Err(Cur, "Expected an indented block of match branches");
        Next();
        if (Cur.Kind != TokKind.Indent) throw Err(Cur, "Expected an indented block of match branches");
        Next();
        while (true)
        {
            while (Cur.Kind == TokKind.Newline) Next();
            if (Cur.Kind == TokKind.Dedent) { Next(); break; }
            if (Cur.Kind == TokKind.Eof) break;
            int before = _p;
            Token at = Cur;
            try { m.Branches.Add(ParseMatchBranch()); }
            catch (ParseException e)
            {
                Report(e);
                Sync();
                if (_p == before && Cur.Kind is not (TokKind.Eof or TokKind.Dedent)) Next();
            }
        }
        if (m.Branches.Count == 0 && !_errors.Any()) throw Err(start, "A match needs at least one branch");
        return Fin(m, start);
    }

    private MatchBranch ParseMatchBranch()
    {
        Token start = Cur;
        var b = new MatchBranch();
        do b.Patterns.Add(ParsePattern()); while (MatchOp(","));
        if (Cur.Kind == TokKind.Ident && Cur.Text == "when") { Next(); b.Guard = ParseExpr(); }
        ExpectOp(":", "after the match pattern");
        b.Body = ParseSuite();
        return Fin(b, start);
    }

    private Pattern ParsePattern()
    {
        Token t = Cur;
        if (IsKw("var"))
        {
            Next();
            return Fin(new PatBind { Name = ExpectIdent("a binding name").Text }, t);
        }
        if (IsOp(".."))
        {
            Next();
            return Fin(new PatRest(), t);
        }
        if (IsOp("["))
        {
            Next();
            var a = new PatArray();
            while (!IsOp("]"))
            {
                a.Items.Add(ParsePattern());
                if (!MatchOp(",")) break;
            }
            ExpectOp("]", "to close the array pattern");
            return Fin(a, t);
        }
        if (IsOp("{"))
        {
            Next();
            var d = new PatDict();
            while (!IsOp("}"))
            {
                if (IsOp("..")) { Next(); d.HasRest = true; }
                else
                {
                    Token ks = Cur;
                    var entry = new PatDictEntry { Key = ParseExpr() };
                    if (MatchOp(":")) entry.Value = ParsePattern();
                    d.Entries.Add(Fin(entry, ks));
                }
                if (!MatchOp(",")) break;
            }
            ExpectOp("}", "to close the dictionary pattern");
            return Fin(d, t);
        }
        if (t.Kind == TokKind.Ident && t.Text == "_" && !(Peek().Kind == TokKind.Op && Peek().Text is "." or "(" or "["))
        {
            Next();
            return Fin(new PatWildcard(), t);
        }
        return Fin(new PatValue { Expr = ParseExpr() }, t);
    }

    // ---- expressions ----

    public Expr? ParseSingleExpression()
    {
        try
        {
            while (Cur.Kind == TokKind.Newline) Next();
            Expr e = ParsePrec(PAssign);
            while (Cur.Kind == TokKind.Newline) Next();
            if (Cur.Kind != TokKind.Eof) throw Err(Cur, $"Expected the end of the expression but found {Describe(Cur)}");
            return e;
        }
        catch (ParseException e)
        {
            Report(e);
            return null;
        }
    }

    private Expr ParseExpr() => ParsePrec(PCast);

    private List<Expr> ParseArgs(string close)
    {
        var args = new List<Expr>();
        while (!IsOp(close))
        {
            args.Add(ParseExpr());
            if (!MatchOp(",")) break;
        }
        ExpectOp(close, close == ")" ? "to close the call" : "to close the list");
        return args;
    }

    private int InfixPrec(Token t)
    {
        if (t.Kind == TokKind.Op)
        {
            switch (t.Text)
            {
                case "=": case "+=": case "-=": case "*=": case "/=": case "%=": case "**=": case "&=": case "|=": case "^=": case "<<=": case ">>=":
                    return PAssign;
                case "||": return POr;
                case "&&": return PAnd;
                case "==": case "!=": case "<": case ">": case "<=": case ">=": return PCompare;
                case "|": return PBitOr;
                case "^": return PBitXor;
                case "&": return PBitAnd;
                case "<<": case ">>": return PShift;
                case "+": case "-": return PAdd;
                case "*": case "/": case "%": return PFactor;
                case "**": return PPower;
                case "(": return PCall;
                case ".": return PAttr;
                case "[": return PSubscript;
            }
            return 0;
        }
        if (t.Kind == TokKind.Keyword)
        {
            switch (t.Text)
            {
                case "if": return PTernary;
                case "as": return PCast;
                case "or": return POr;
                case "and": return PAnd;
                case "in": return PIn;
                case "is": return PTypeTest;
                case "not": return Peek().IsKw("in") ? PIn : 0;
            }
        }
        return 0;
    }

    private Expr ParsePrec(int min)
    {
        Expr left = ParsePrefix();
        while (true)
        {
            int prec = InfixPrec(Cur);
            if (prec == 0 || prec < min) break;
            left = ParseInfix(left, prec);
        }
        return left;
    }

    private static string Canon(string op) => op switch { "&&" => "and", "||" => "or", "!" => "not", _ => op };

    private Expr ParseInfix(Expr left, int prec)
    {
        Token op = Cur;
        Pos start = left.Start;
        switch (prec)
        {
            case PAssign when op.Kind == TokKind.Op:
            {
                Next();
                Expr value = ParsePrec(PAssign);
                return FinFrom(new Assign { Op = op.Text, Target = left, Value = value }, start);
            }
            case PTernary:
            {
                Next();
                Expr cond = ParsePrec(POr);
                ExpectKw("else");
                Expr other = ParsePrec(PTernary);
                return FinFrom(new Ternary { Then = left, Cond = cond, Else = other }, start);
            }
            case PCast when op.IsKw("as"):
            {
                Next();
                return FinFrom(new Cast { Value = left, Type = ParseType() }, start);
            }
            case PTypeTest:
            {
                Next();
                return FinFrom(new TypeTest { Value = left, Type = ParseType() }, start);
            }
            case PCall:
            {
                Next();
                var call = new Call { Callee = left, Args = ParseArgs(")") };
                return FinFrom(call, start);
            }
            case PAttr:
            {
                Next();
                Token n = Cur;
                if (n.Kind != TokKind.Ident && n.Kind != TokKind.Keyword) throw Err(n, $"Expected a member name after \".\" but found {Describe(n)}");
                Next();
                return FinFrom(new MemberAccess { Target = left, Name = n.Text }, start);
            }
            case PSubscript:
            {
                Next();
                Expr idx = ParseExpr();
                ExpectOp("]", "to close the subscript");
                return FinFrom(new Subscript { Target = left, Index = idx }, start);
            }
        }
        // left-associative binary operators, "in" and "not in" among them
        string text = op.Text;
        Next();
        if (op.IsKw("not")) { Next(); text = "not in"; }
        Expr right = ParsePrec(prec + 1);
        return FinFrom(new Binary { Op = Canon(text), Left = left, Right = right }, start);
    }

    private Expr ParsePrefix()
    {
        Token t = Cur;
        switch (t.Kind)
        {
            case TokKind.Int:
                Next();
                return Fin(new Literal { Kind = LitKind.Int, Value = t.Value, Text = t.Text }, t);
            case TokKind.Float:
                Next();
                return Fin(new Literal { Kind = LitKind.Float, Value = t.Value, Text = t.Text }, t);
            case TokKind.String:
                Next();
                return Fin(new Literal
                {
                    Kind = t.StrKind == StrKind.StringName ? LitKind.StringName : t.StrKind == StrKind.NodePath ? LitKind.NodePath : LitKind.String,
                    Value = t.Value, Text = t.Text, Raw = t.Raw, Triple = t.Triple,
                }, t);
            case TokKind.Ident:
                Next();
                return Fin(new Identifier { Name = t.Text }, t);
            case TokKind.NodePath:
                Next();
                return Fin(new GetNodeExpr { Path = t.Text }, t);
            case TokKind.Keyword:
                switch (t.Text)
                {
                    case "true": case "false":
                        Next();
                        return Fin(new Literal { Kind = LitKind.Bool, Value = t.Text == "true", Text = t.Text }, t);
                    case "null":
                        Next();
                        return Fin(new Literal { Kind = LitKind.Null, Text = "null" }, t);
                    case "self": Next(); return Fin(new SelfExpr(), t);
                    case "super": Next(); return Fin(new SuperExpr(), t);
                    case "func": return ParseLambda();
                    case "preload":
                    {
                        Next();
                        ExpectOp("(", "after \"preload\"");
                        Expr path = ParseExpr();
                        ExpectOp(")", "to close \"preload\"");
                        return Fin(new PreloadExpr { Path = path }, t);
                    }
                    case "await":
                    {
                        Next();
                        Expr operand = ParsePrec(PAwait);
                        return Fin(new AwaitExpr { Operand = operand }, t);
                    }
                    case "not":
                    {
                        Next();
                        Expr operand = ParsePrec(PNot);
                        return Fin(new Unary { Op = "not", Operand = operand }, t);
                    }
                }
                break;
            case TokKind.Op:
                switch (t.Text)
                {
                    case "-": case "+":
                    {
                        Next();
                        Expr operand = ParsePrec(PSign);
                        return Fin(new Unary { Op = t.Text, Operand = operand }, t);
                    }
                    case "~":
                    {
                        Next();
                        Expr operand = ParsePrec(PBitNot);
                        return Fin(new Unary { Op = "~", Operand = operand }, t);
                    }
                    case "!":
                    {
                        Next();
                        Expr operand = ParsePrec(PNot);
                        return Fin(new Unary { Op = "not", Operand = operand }, t);
                    }
                    case "(":
                    {
                        Next();
                        Expr inner = ParseExpr();
                        ExpectOp(")", "to close the parenthesis");
                        inner.Parenthesized = true;
                        inner.Start = P(t);
                        inner.End = _lastEnd;
                        return inner;
                    }
                    case "[":
                    {
                        Next();
                        return Fin(new ArrayLit { Items = ParseArgs("]") }, t);
                    }
                    case "{": return ParseDict();
                }
                break;
        }
        throw Err(t, $"Expected an expression but found {Describe(t)}");
    }

    private Expr ParseDict()
    {
        Token start = Next(); // {
        var d = new DictLit();
        while (!IsOp("}"))
        {
            Token ks = Cur;
            var entry = new DictEntry();
            if (Cur.Kind == TokKind.Ident && Peek().IsOp("="))
            {
                Next();
                entry.Key = new Literal
                {
                    Kind = LitKind.String, Value = ks.Text, Text = ks.Text,
                    Start = P(ks), End = new Pos(ks.EndLine, ks.EndCol),
                };
                Next();
                d.LuaStyle = true;
            }
            else
            {
                entry.Key = ParseExpr();
                ExpectOp(":", "after the dictionary key");
            }
            entry.Value = ParseExpr();
            d.Entries.Add(Fin(entry, ks));
            if (!MatchOp(",")) break;
        }
        ExpectOp("}", "to close the dictionary");
        return Fin(d, start);
    }

    private Expr ParseLambda()
    {
        Token start = Next(); // func
        var l = new LambdaExpr();
        if (Cur.Kind == TokKind.Ident) l.Name = Next().Text;
        ExpectOp("(", "after \"func\"");
        l.Params = ParseParams(")");
        ExpectOp(")");
        if (MatchOp("->")) l.ReturnType = ParseType();
        ExpectOp(":", "after the lambda signature");
        if (Cur.Kind == TokKind.Newline) l.Body = ParseSuite();
        else
        {
            Token bs = Cur;
            var b = new Block();
            b.Statements.Add(ParseSimple());
            l.Body = FinBlock(b, bs);
            l.IsInline = true;
        }
        return Fin(l, start);
    }
}
