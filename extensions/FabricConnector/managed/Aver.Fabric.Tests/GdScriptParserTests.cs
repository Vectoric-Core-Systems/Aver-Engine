// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Fabric.Convert (G3 F4a): the GDScript tokenizer and parser. Small sources for tokens and precedence, the hand-written
// fixtures in tests/fabric/data/gdscript for every construct, two realistic scripts, and error recovery.

using System.Globalization;
using Aver.Fabric.Convert.GdScript;

static class GdScriptParserTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("gdscript tokens: indentation with tabs and spaces, blank and comment lines, nesting", TokIndent);
        T.Run("gdscript tokens: mixed tabs and spaces, continuation lines, brackets swallow newlines", TokLines);
        T.Run("gdscript tokens: number forms (hex, binary, underscores, exponents, 1. and .5)", TokNumbers);
        T.Run("gdscript tokens: string forms, escapes, raw, triple, StringName and NodePath, unterminated", TokStrings);
        T.Run("gdscript tokens: every operator, keywords against identifiers", TokOperators);
        T.Run("gdscript tokens: $Path, $\"quoted\", $%Unique and %Unique against the % operator", TokNodeSugar);
        T.Run("gdscript tokens: annotations, comments and doc comments, positions", TokMisc);
        T.Run("gdscript tokens: a multi-line lambda inside brackets gets its own block", TokLambdaBlocks);
        T.Run("gdscript expr: precedence and associativity follow Godot's table", ExprPrecedence);
        T.Run("gdscript expr: calls, members, subscripts, node sugar, await, super, preload, literals", ExprPostfixAndAtoms);
        T.Run("gdscript expr: assignments, lambdas, ternary, as/is/in, positions and parentheses", ExprMisc);
        T.Run("gdscript stmt: var, const, if/elif/else, while, for, return on small sources", StmtSmall);
        T.Run("gdscript fixture 02: extends, class_name, signals, enums, consts, vars, annotations, inner classes", FxDeclarations);
        T.Run("gdscript fixture 03: setget in every form", FxProperties);
        T.Run("gdscript fixture 04: functions, parameters, types, await, super, locals", FxFunctions);
        T.Run("gdscript fixture 05: control flow", FxControlFlow);
        T.Run("gdscript fixture 06: match with every pattern kind", FxMatch);
        T.Run("gdscript fixture 07: expressions of every kind", FxExpressions);
        T.Run("gdscript fixture 08: lambdas, inline and multi-line, as arguments and values", FxLambdas);
        T.Run("gdscript fixture 09: string literals and line tracking after multi-line strings", FxStrings);
        T.Run("gdscript fixture: player controller", FxPlayer);
        T.Run("gdscript fixture: UI menu", FxMenu);
        T.Run("gdscript header: class_name, extends in every form, @tool on its own line or not, doc comments", HeaderForms);
        T.Run("gdscript layout: CRLF, a BOM and space indentation give the same tree as the tabbed file", LayoutsAgree);
        T.Run("gdscript tree: every node's range holds its children's", PositionsNest);
        T.Run("gdscript errors: parsing goes on after a bad statement, member or block", ErrorRecovery);
        T.Run("gdscript errors: indentation, unterminated strings, unclosed brackets, bad escapes", ErrorKinds);
        T.Run("gdscript errors: damaged copies of every fixture never throw or hang", Damaged);
    });

    // ---- helpers ----

    private static string Fx(string name) => File.ReadAllText(Path.Combine(T.DataDir(), "gdscript", name));

    private static ParseResult ParseFx(string name)
    {
        ParseResult r = GdScriptParser.Parse(Fx(name));
        T.Check($"{name} parses without errors", r.Ok, string.Join("; ", r.Errors));
        return r;
    }

    // 1-based line of the first fixture line that starts with `prefix`.
    private static int LineOf(string file, string prefix) =>
        Array.FindIndex(Fx(file).Replace("\r\n", "\n").Split('\n'), l => l.StartsWith(prefix)) + 1;

    private static void Eq(string what, string expected, string actual) => T.Check(what, expected == actual, $"expected {expected}, got {actual}");

    private static string Toks(string src) => string.Join(" ", Lexer.Tokenize(src).Tokens.Select(t => t.ToString()));

    private static string Ex(string src)
    {
        Expr? e = GdScriptParser.ParseExpression(src, out List<ParseError> errs);
        return errs.Count > 0 || e == null ? "ERR:" + string.Join("|", errs) : AstDump.Dump(e);
    }

    private static void ExEq(string src, string expected) => Eq($"expr  {src.Replace("\n", "\\n")}", expected, Ex(src));

    // Statements (relative indentation kept) wrapped in a function; returns the body.
    private static Block Body(string stmts, List<ParseError>? errors = null)
    {
        string src = "func f():\n" + string.Join("\n", stmts.Split('\n').Select(l => "\t" + l)) + "\n";
        ParseResult r = GdScriptParser.Parse(src);
        errors?.AddRange(r.Errors);
        if (r.Errors.Count > 0) throw new Exception("parse errors in '" + stmts + "': " + string.Join("; ", r.Errors));
        return ((FuncDecl)r.Script.Members[0]).Body;
    }

    private static void StEq(string stmts, string expected) => Eq("stmt  " + stmts.Replace("\n", "\\n"), expected, AstDump.Dump(Body(stmts)));

    private static T1? Member<T1>(ClassDecl c, string name) where T1 : Member => c.Members.OfType<T1>().FirstOrDefault(m => m switch
    {
        VarDecl v => v.Name == name, FuncDecl f => f.Name == name, ConstDecl k => k.Name == name, SignalDecl s => s.Name == name,
        EnumDecl e => e.Name == name, ClassDecl k => k.Name == name, _ => false,
    });

    private static VarDecl V(ClassDecl c, string n) => Member<VarDecl>(c, n) ?? throw new Exception("no var " + n);
    private static FuncDecl F(ClassDecl c, string n) => Member<FuncDecl>(c, n) ?? throw new Exception("no func " + n);
    private static string D(Node? n) => n == null ? "<null>" : AstDump.Dump(n);

    private static VarDecl Local(FuncDecl f, string name) =>
        f.Body.Statements.OfType<VarDecl>().First(v => v.Name == name);

    private static IEnumerable<T1> All<T1>(Node n) where T1 : Node => n.Descendants().OfType<T1>();

    // ---- tokenizer ----

    private static void TokIndent()
    {
        Eq("tabs", "Keyword(if) Ident(x) Op(:) NEWLINE INDENT Keyword(pass) NEWLINE DEDENT EOF", Toks("if x:\n\tpass\n"));
        Eq("spaces", "Keyword(if) Ident(x) Op(:) NEWLINE INDENT Keyword(pass) NEWLINE DEDENT EOF", Toks("if x:\n    pass\n"));
        Eq("two levels closing together", "Ident(a) Op(:) NEWLINE INDENT Ident(b) Op(:) NEWLINE INDENT Ident(c) NEWLINE DEDENT DEDENT Ident(d) NEWLINE EOF",
           Toks("a:\n\tb:\n\t\tc\nd\n"));
        Eq("blank and comment-only lines carry no tokens", "Ident(a) NEWLINE Ident(b) NEWLINE EOF", Toks("a\n\n# c\n   # d\n\t\nb\n"));
        Eq("no newline at the end", "Ident(a) NEWLINE EOF", Toks("a"));
        Eq("dedents at the end", "Ident(a) Op(:) NEWLINE INDENT Ident(b) NEWLINE DEDENT EOF", Toks("a:\n\tb"));
        Eq("CRLF", "Ident(a) Op(:) NEWLINE INDENT Ident(b) NEWLINE DEDENT EOF", Toks("a:\r\n\tb\r\n"));
        Eq("a dedent to a level in between", "Ident(a) Op(:) NEWLINE INDENT Ident(b) NEWLINE DEDENT Ident(c) NEWLINE EOF", Toks("a:\n\tb\nc\n"));
        var lx = Lexer.Tokenize("a:\n        b\n    c\n");
        T.Check("an unindent to no outer level is reported", lx.Errors.Any(e => e.Message.Contains("Unindent") && e.Line == 3), string.Join("; ", lx.Errors));
    }

    private static void TokLines()
    {
        Lexer lx = Lexer.Tokenize("if a:\n\tx\n  y\n");
        T.Check("tabs then spaces is reported", lx.Errors.Any(e => e.Message.Contains("Mixed") && e.Line == 3), string.Join("; ", lx.Errors));
        lx = Lexer.Tokenize("if a:\n\t  x\n");
        T.Check("tab and space in one indent is reported", lx.Errors.Any(e => e.Message.Contains("Mixed")), string.Join("; ", lx.Errors));
        Eq("backslash continuation", "Ident(a) Op(=) Int(1) Op(+) Int(2) NEWLINE EOF", Toks("a = 1 + \\\n2\n"));
        Eq("continuation with trailing blanks", "Ident(a) Op(=) Int(1) Op(+) Int(2) NEWLINE EOF", Toks("a = 1 + \\  \n2\n"));
        Eq("parentheses", "Ident(f) Op(() Int(1) Op(,) Int(2) Op()) NEWLINE EOF", Toks("f(1,\n\n  2\n)\n"));
        Eq("brackets and braces", "Op([) Int(1) Op(,) Int(2) Op(]) Op(,) NEWLINE Op({) Ident(a) Op(:) Int(1) Op(}) NEWLINE EOF", Toks("[1,\n2],\n{a:\n1}"));
        Eq("a comment inside brackets", "Ident(f) Op(() Int(1) Op()) NEWLINE EOF", Toks("f(\n# note\n1)\n"));
        Eq("indentation inside brackets means nothing", "Ident(a) Op(=) Op([) Int(1) Op(]) NEWLINE Ident(b) NEWLINE EOF", Toks("a = [\n\t\t\t1\n]\nb\n"));
    }

    private static void TokNumbers()
    {
        (string src, TokKind kind, string val)[] cases =
        {
            ("1", TokKind.Int, "1"), ("1_000_000", TokKind.Int, "1000000"), ("0xFF", TokKind.Int, "255"), ("0xff_ff", TokKind.Int, "65535"),
            ("0b1010_0101", TokKind.Int, "165"), ("0x0", TokKind.Int, "0"), ("0xFFFFFFFFFFFFFFFF", TokKind.Int, "-1"),
            ("3.14", TokKind.Float, "3.14"), ("1e3", TokKind.Float, "1000"), ("1.5e-3", TokKind.Float, "0.0015"), ("2E4", TokKind.Float, "20000"),
            ("1e+2", TokKind.Float, "100"), (".5", TokKind.Float, "0.5"), ("1.", TokKind.Float, "1"), ("1_0.2_5", TokKind.Float, "10.25"),
        };
        foreach (var (src, kind, val) in cases)
        {
            Token t = Lexer.Tokenize(src).Tokens[0];
            string v = t.Value is long l ? l.ToString(CultureInfo.InvariantCulture) : ((double)t.Value!).ToString("R", CultureInfo.InvariantCulture);
            Eq($"number {src}", kind + ":" + val, t.Kind + ":" + v);
        }
        Eq("a method on an int literal keeps the dot", "Int(1) Op(.) Ident(foo) NEWLINE EOF", Toks("1.foo"));
        Eq("a range-like pair stays two tokens", "Int(1) Op(..) Int(2) NEWLINE EOF", Toks("1..2"));
        Eq("a member of a variable is not a float", "Ident(a) Op(.) Ident(b) NEWLINE EOF", Toks("a.b"));
        T.Check("an integer too large is reported", Lexer.Tokenize("99999999999999999999").Errors.Count == 1);
        T.Check("0x with no digits is reported", Lexer.Tokenize("0x").Errors.Count == 1);
    }

    private static Token Str(string src) => Lexer.Tokenize(src).Tokens[0];

    private static void TokStrings()
    {
        Eq("double quotes", "a\tb", (string)Str("\"a\\tb\"").Value!);
        Eq("single quotes", "x", (string)Str("'x'").Value!);
        Eq("quote inside", "it's", (string)Str("\"it's\"").Value!);
        Eq("escaped quotes and backslash", "\"'\\", (string)Str("\"\\\"\\'\\\\\"").Value!);
        Eq("newline escape", "a\nb", (string)Str("\"a\\nb\"").Value!);
        Eq("\\u escape", "A", (string)Str("\"\\u0041\"").Value!);
        Eq("\\U escape", "\U0001F600", (string)Str("\"\\U01F600\"").Value!);
        Token raw = Str("r\"a\\n\\\"b\"");
        Eq("raw keeps backslashes", "a\\n\\\"b", (string)raw.Value!);
        T.Check("raw flag", raw.Raw);
        Token tri = Str("\"\"\"a\nb \"c\" d\"\"\"");
        Eq("triple quoted spans lines", "a\nb \"c\" d", (string)tri.Value!);
        T.Check("triple flag", tri.Triple);
        Token sn = Str("&\"move\"");
        T.Check("StringName literal", sn.Kind == TokKind.String && sn.StrKind == StrKind.StringName && (string)sn.Value! == "move");
        Token np = Str("^\"A/B:c\"");
        T.Check("NodePath literal", np.Kind == TokKind.String && np.StrKind == StrKind.NodePath && (string)np.Value! == "A/B:c");
        Eq("backslash before a newline joins the lines", "abcdef", (string)Str("\"abc\\\ndef\"").Value!);
        T.Check("the token after a multi-line string is on the right line", Lexer.Tokenize("\"\"\"a\nb\"\"\" x").Tokens[1].Line == 2);
        T.Check("unterminated", Lexer.Tokenize("\"abc").Errors.Any(e => e.Message.Contains("Unterminated")));
        T.Check("bad escape", Lexer.Tokenize("\"\\q\"").Errors.Any(e => e.Message.Contains("escape")));
        T.Check("bad \\u escape", Lexer.Tokenize("\"\\u12\"").Errors.Any(e => e.Message.Contains("Unicode")));
        Eq("an r that is not a string prefix", "Ident(r) Op(+) Int(1) NEWLINE EOF", Toks("r + 1"));
    }

    private static void TokOperators()
    {
        string ops = "** **= + - * / % = += -= *= /= %= &= |= ^= <<= >>= << >> == != < > <= >= && || ! & | ^ ~ . .. ... , : ; -> :=";
        var tokens = Lexer.Tokenize(ops).Tokens.Where(t => t.Kind == TokKind.Op).Select(t => t.Text);
        Eq("all operators lex as themselves", ops, string.Join(" ", tokens));
        Eq("longest match", "Ident(a) Op(**=) Ident(b) NEWLINE EOF", Toks("a**=b"));
        Eq("minus then greater", "Ident(a) Op(-) Op(>) Ident(b) NEWLINE EOF", Toks("a - > b"));
        Eq("keywords", "Keyword(and) Keyword(or) Keyword(not) Keyword(in) Keyword(is) Keyword(as) Keyword(if) Keyword(else) NEWLINE EOF", Toks("and or not in is as if else"));
        Eq("keyword prefixes are identifiers", "Ident(android) Ident(order) Ident(nothing) Ident(inner) Ident(isolate) Ident(assert_ok) NEWLINE EOF",
           Toks("android order nothing inner isolate assert_ok"));
        Eq("soft words and builtins are identifiers", "Ident(when) Ident(set) Ident(get) Ident(_) Ident(PI) Ident(INF) NEWLINE EOF", Toks("when set get _ PI INF"));
        Eq("literals and self", "Keyword(true) Keyword(false) Keyword(null) Keyword(self) Keyword(super) Keyword(void) NEWLINE EOF", Toks("true false null self super void"));
        Eq("declaration keywords", "Keyword(func) Keyword(class) Keyword(class_name) Keyword(extends) Keyword(signal) Keyword(enum) Keyword(const) Keyword(var) Keyword(static) NEWLINE EOF",
           Toks("func class class_name extends signal enum const var static"));
        T.Check("an unknown character is reported", Lexer.Tokenize("a ? b").Errors.Any(e => e.Message.Contains("Unknown")));
    }

    private static void TokNodeSugar()
    {
        Eq("$Path", "NodePath(Player/Camera) NEWLINE EOF", Toks("$Player/Camera"));
        Eq("$ then a member", "NodePath(Label) Op(.) Ident(text) NEWLINE EOF", Toks("$Label.text"));
        Eq("$\"quoted\"", "NodePath(../a b/c) NEWLINE EOF", Toks("$\"../a b/c\""));
        Eq("$%Unique", "NodePath(%Unique) NEWLINE EOF", Toks("$%Unique"));
        Eq("$A/%B", "NodePath(A/%B) NEWLINE EOF", Toks("$A/%B"));
        Eq("%Unique at the start", "NodePath(%Unique) NEWLINE EOF", Toks("%Unique"));
        Eq("%Unique after =", "Ident(x) Op(=) NodePath(%Name) NEWLINE EOF", Toks("x = %Name"));
        Eq("%\"Quoted Name\"", "NodePath(%Quoted Name) NEWLINE EOF", Toks("%\"Quoted Name\""));
        Eq("inside a list", "Op([) NodePath(%A) Op(,) NodePath(%B) Op(]) NEWLINE EOF", Toks("[%A, %B]"));
        Eq("modulo after a name", "Ident(a) Op(%) Ident(b) NEWLINE EOF", Toks("a % b"));
        Eq("modulo without spaces", "Ident(a) Op(%) Ident(b) NEWLINE EOF", Toks("a%b"));
        Eq("string format", "String(\"%d\") Op(%) Ident(n) NEWLINE EOF", Toks("\"%d\" % n"));
        Eq("modulo after a call", "Ident(f) Op(() Op()) Op(%) Int(2) NEWLINE EOF", Toks("f() % 2"));
        Eq("modulo after a bracket", "Ident(a) Op([) Int(0) Op(]) Op(%) Ident(b) NEWLINE EOF", Toks("a[0] % b"));
        Token t = Lexer.Tokenize("%Name").Tokens[0];
        T.Check("unique flag", t.Unique);
        T.Check("a missing name is reported", Lexer.Tokenize("$ ").Errors.Count == 1);
    }

    private static void TokMisc()
    {
        Eq("annotations", "Annotation(export) Keyword(var) Ident(x) NEWLINE EOF", Toks("@export var x"));
        Eq("annotation with arguments", "Annotation(export_range) Op(() Int(0) Op(,) Float(1.5) Op()) Keyword(var) Ident(x) NEWLINE EOF", Toks("@export_range(0, 1.5) var x"));
        Eq("annotation names", "Annotation(tool) Annotation(onready) Annotation(rpc) Annotation(warning_ignore) Annotation(static_unload) Annotation(icon) NEWLINE EOF",
           Toks("@tool @onready @rpc @warning_ignore @static_unload @icon"));
        Lexer lx = Lexer.Tokenize("# c\nx = 1 # tail\n## doc\n\t## indented doc\n");
        T.Check("four comments", lx.Comments.Count == 4, lx.Comments.Count.ToString());
        Comment c0 = lx.Comments[0], c1 = lx.Comments[1], c2 = lx.Comments[2], c3 = lx.Comments[3];
        T.Check("plain comment on its own line", c0.Line == 1 && c0.Text == "c" && !c0.IsDoc && c0.OwnLine);
        T.Check("trailing comment", c1.Line == 2 && c1.Text == "tail" && !c1.IsDoc && !c1.OwnLine && c1.Col == 7);
        T.Check("doc comment", c2.Line == 3 && c2.Text == "doc" && c2.IsDoc && c2.OwnLine);
        T.Check("indented doc comment", c3.Line == 4 && c3.Text == "indented doc" && c3.IsDoc && c3.OwnLine);
        Eq("comments are not tokens", "Ident(x) Op(=) Int(1) NEWLINE EOF", Toks("# c\nx = 1 # tail\n## doc\n\t## indented doc\n"));
        List<Token> p = Lexer.Tokenize("var x = 10\n  ").Tokens;
        T.Check("var at 1:1", p[0].Line == 1 && p[0].Col == 1 && p[0].EndCol == 4);
        T.Check("x at 1:5", p[1].Col == 5 && p[1].EndCol == 6);
        T.Check("= at 1:7", p[2].Col == 7);
        T.Check("10 at 1:9..1:11", p[3].Col == 9 && p[3].EndCol == 11 && p[3].EndLine == 1);
        Eq("BOM is skipped", "Ident(a) NEWLINE EOF", Toks("\uFEFFa"));
    }

    private static void TokLambdaBlocks()
    {
        const string multi = "Ident(foo) Op(() Keyword(func) Op(() Ident(x) Op()) Op(:) NEWLINE INDENT Keyword(return) Ident(x) NEWLINE DEDENT Op()) NEWLINE EOF";
        Eq("closing bracket on its own line", multi, Toks("foo(func(x):\n\treturn x\n)\n"));
        Eq("closing bracket on the last body line", multi, Toks("foo(func(x):\n\treturn x)\n"));
        Eq("inline lambda has no block", "Ident(foo) Op(() Keyword(func) Op(() Ident(x) Op()) Op(:) Keyword(return) Ident(x) Op()) NEWLINE EOF", Toks("foo(func(x): return x)\n"));
        Eq("arguments after the block", "Ident(foo) Op(() Keyword(func) Op(() Op()) Op(:) NEWLINE INDENT Keyword(pass) NEWLINE DEDENT Op(,) Int(5) Op()) NEWLINE EOF",
           Toks("foo(func():\n\tpass\n, 5)\n"));
        Eq("a lambda body with its own block", "Ident(f) Op(() Keyword(func) Op(() Op()) Op(:) NEWLINE INDENT Keyword(if) Ident(a) Op(:) NEWLINE INDENT Keyword(pass) NEWLINE DEDENT DEDENT Op()) NEWLINE EOF",
           Toks("f(func():\n\tif a:\n\t\tpass\n)\n"));
        Eq("a statement-level lambda is plain indentation", "Keyword(var) Ident(f) Op(=) Keyword(func) Op(() Op()) Op(:) NEWLINE INDENT Keyword(pass) NEWLINE DEDENT Ident(x) NEWLINE EOF",
           Toks("var f = func():\n\tpass\nx\n"));
        Eq("return type before the colon", "Ident(f) Op(() Keyword(func) Op(() Op()) Op(->) Ident(int) Op(:) NEWLINE INDENT Keyword(return) Int(1) NEWLINE DEDENT Op()) NEWLINE EOF",
           Toks("f(func() -> int:\n\treturn 1\n)\n"));
        Eq("a comment after the colon", "Ident(f) Op(() Keyword(func) Op(() Op()) Op(:) NEWLINE INDENT Keyword(pass) NEWLINE DEDENT Op()) NEWLINE EOF",
           Toks("f(func(): # note\n\tpass\n)\n"));
        Lexer lx = Lexer.Tokenize("f(\n1,\n");
        T.Check("unclosed bracket", lx.Errors.Any(e => e.Message.Contains("Unclosed") && e.Line == 1 && e.Col == 2), string.Join("; ", lx.Errors));
        T.Check("unmatched bracket", Lexer.Tokenize("a)").Errors.Any(e => e.Message.Contains("Unmatched")));
    }

    // ---- expressions ----

    private static void ExprPrecedence()
    {
        ExEq("1 + 2 * 3", "(+ 1 (* 2 3))");
        ExEq("(1 + 2) * 3", "(* (+ 1 2) 3)");
        ExEq("a - b - c", "(- (- a b) c)");
        ExEq("a / b * c", "(* (/ a b) c)");
        ExEq("a ** b ** c", "(** (** a b) c)");
        ExEq("-a ** 2", "(- (** a 2))");
        ExEq("2 ** -1", "(** 2 (- 1))");
        ExEq("- -a", "(- (- a))");
        ExEq("~a + 1", "(+ (~ a) 1)");
        ExEq("+a", "(+ a)");
        ExEq("a << 2 | b & c ^ 1", "(| (<< a 2) (^ (& b c) 1))");
        ExEq("a | b ^ c & d", "(| a (^ b (& c d)))");
        ExEq("a < b == c", "(== (< a b) c)");
        ExEq("a + b < c - d", "(< (+ a b) (- c d))");
        ExEq("a == b and b != c", "(and (== a b) (!= b c))");
        ExEq("a and b or c and d", "(or (and a b) (and c d))");
        ExEq("a && b || !c", "(or (and a b) (not c))");
        ExEq("not a and b", "(and (not a) b)");
        ExEq("not a == b", "(not (== a b))");
        ExEq("not a in b", "(not (in a b))");
        ExEq("a in b", "(in a b)");
        ExEq("a not in b", "(not in a b)");
        ExEq("a in b and c", "(and (in a b) c)");
        ExEq("a < b in c", "(in (< a b) c)");
        ExEq("a if b else c", "(?: a b c)");
        ExEq("a if b else c if d else e", "(?: a b (?: c d e))");
        ExEq("a or b if c else d", "(?: (or a b) c d)");
        ExEq("a + 1 if b else c * 2", "(?: (+ a 1) b (* c 2))");
        ExEq("x as Node3D", "(as x Node3D)");
        ExEq("x as Array[int]", "(as x Array[int])");
        ExEq("x is Node3D", "(is x Node3D)");
        ExEq("x is Array[int]", "(is x Array[int])");
        ExEq("x is Outer.Inner", "(is x Outer.Inner)");
        ExEq("a is Foo and b", "(and (is a Foo) b)");
        ExEq("a == b is Foo", "(== a (is b Foo))");
        ExEq("a + b as int", "(as (+ a b) int)");
        ExEq("a if b else c as int", "(as (?: a b c) int)");
        ExEq("(a as Node3D).position", "(. (as a Node3D) position)");
        ExEq("\"%d\" % 3", "(% \"%d\" 3)");
        ExEq("\"%d %d\" % [1, 2]", "(% \"%d %d\" (array 1 2))");
        ExEq("a % b * c", "(* (% a b) c)");
    }

    private static void ExprPostfixAndAtoms()
    {
        ExEq("a.b.c", "(. (. a b) c)");
        ExEq("a.b(1, 2)[0].c", "(. ([] (call (. a b) 1 2) 0) c)");
        ExEq("f(1,)", "(call f 1)");
        ExEq("f()", "(call f)");
        ExEq("f(1)(2)", "(call (call f 1) 2)");
        ExEq("a[1][2]", "([] ([] a 1) 2)");
        ExEq("a[b + 1]", "([] a (+ b 1))");
        ExEq("-a.b", "(- (. a b))");
        ExEq("-a[0]", "(- ([] a 0))");
        ExEq("a.is_empty()", "(call (. a is_empty))");
        ExEq("$A/B.visible", "(. ($ A/B) visible)");
        ExEq("$\"../x y\"", "($ ../x y)");
        ExEq("%Name.text", "(. ($ %Name) text)");
        ExEq("$%Name", "($ %Name)");
        ExEq("$A.get_child(0)", "(call (. ($ A) get_child) 0)");
        ExEq("await a.b()", "(await (call (. a b)))");
        ExEq("await get_tree().create_timer(1).timeout", "(await (. (call (. (call get_tree) create_timer) 1) timeout))");
        ExEq("await x is Foo", "(is (await x) Foo)");
        ExEq("await a + 1", "(+ (await a) 1)");
        ExEq("preload(\"res://a.tscn\")", "(preload \"res://a.tscn\")");
        ExEq("preload(\"res://a.tscn\").instantiate()", "(call (. (preload \"res://a.tscn\") instantiate))");
        ExEq("load(\"res://a\")", "(call load \"res://a\")");
        ExEq("super.foo(1)", "(call (. super foo) 1)");
        ExEq("super(1, 2)", "(call super 1 2)");
        ExEq("self.x", "(. self x)");
        ExEq("[1, [2], {\"a\": 3}]", "(array 1 (array 2) (dict (\"a\" 3)))");
        ExEq("[]", "(array)");
        ExEq("[1,\n 2,\n]", "(array 1 2)");
        ExEq("{}", "(dict)");
        ExEq("{a: 1, \"b\": 2, 3: 4}", "(dict (a 1) (\"b\" 2) (3 4))");
        ExEq("{a = 1, b = 2}", "(dict-lua (\"a\" 1) (\"b\" 2))");
        ExEq("{\n\t\"k\": [1],\n\t\"j\": {},\n}", "(dict (\"k\" (array 1)) (\"j\" (dict)))");
        ExEq("Array[int]()", "(call ([] Array int))");
        ExEq("Vector3(1, 2.5, -3)", "(call Vector3 1 2.5 (- 3))");
        ExEq("&\"n\"", "&\"n\"");
        ExEq("^\"p/q\"", "^\"p/q\"");
        ExEq("'it''s'", "ERR:1:5: Expected the end of the expression but found a string"); // a juxtaposed string is not a thing
        ExEq("1.5", "1.5");
        ExEq("0xff", "255");
        ExEq("1e3", "1000.0");
        ExEq("true", "true");
        ExEq("null", "null");
        ExEq("PI", "PI");
        ExEq("1_000", "1000");
        ExEq("\"x\" + 'y'", "(+ \"x\" \"y\")");
        ExEq("a.b.c\n", "(. (. a b) c)");
    }

    private static void ExprMisc()
    {
        ExEq("a = 1", "(= a 1)");
        ExEq("a = b = c", "(= a (= b c))");
        ExEq("a += 1 + 2", "(+= a (+ 1 2))");
        ExEq("a -= 1", "(-= a 1)");
        ExEq("a *= 2", "(*= a 2)");
        ExEq("a /= 2", "(/= a 2)");
        ExEq("a %= 2", "(%= a 2)");
        ExEq("a **= 2", "(**= a 2)");
        ExEq("a &= 1", "(&= a 1)");
        ExEq("a |= 1", "(|= a 1)");
        ExEq("a ^= 1", "(^= a 1)");
        ExEq("a <<= 1", "(<<= a 1)");
        ExEq("a >>= 1", "(>>= a 1)");
        ExEq("a[0] = 1", "(= ([] a 0) 1)");
        ExEq("a.b.c = d if e else f", "(= (. (. a b) c) (?: d e f))");
        ExEq("func(x): return x", "(lambda (x) (block (return x)))");
        ExEq("func(a: int, b := 2) -> int: return a + b", "(lambda (a:int b:=2) -> int (block (return (+ a b))))");
        ExEq("func named(x): pass", "(lambda named (x) (block (pass)))");
        ExEq("f(func(): pass, 2)", "(call f (lambda () (block (pass))) 2)");
        ExEq("f(func(): return 1, func(): return 2)", "(call f (lambda () (block (return 1))) (lambda () (block (return 2))))");
        ExEq("[func(): return 1, 2]", "(array (lambda () (block (return 1))) 2)");

        Expr e = GdScriptParser.ParseExpression("a + bb", out _)!;
        T.Check("binary range", e.Start == new Pos(1, 1) && e.End == new Pos(1, 7), $"{e.Start}-{e.End}");
        e = GdScriptParser.ParseExpression("(a)", out _)!;
        T.Check("parenthesised: flag and range include the parentheses", e.Parenthesized && e.Start == new Pos(1, 1) && e.End == new Pos(1, 4), $"{e.Parenthesized} {e.Start}-{e.End}");
        e = GdScriptParser.ParseExpression("(a + b) * c", out _)!;
        T.Check("left operand starts the binary", e.Start == new Pos(1, 1) && ((Binary)e).Left.Parenthesized);
        T.Check("Parenthesized is false by default", !GdScriptParser.ParseExpression("a", out _)!.Parenthesized);
        var call = (Call)GdScriptParser.ParseExpression("foo(1, 22)", out _)!;
        T.Check("call range", call.Start == new Pos(1, 1) && call.End == new Pos(1, 11) && call.Args[1].Start == new Pos(1, 8) && call.Args[1].End == new Pos(1, 10));

        T.Check("missing operand", Ex("1 +").StartsWith("ERR:1:4"), Ex("1 +"));
        T.Check("trailing junk", Ex("1 2").StartsWith("ERR:1:3"), Ex("1 2"));
        T.Check("unclosed paren", Ex("(1").StartsWith("ERR:"), Ex("(1"));
        T.Check("empty is an error", Ex("").StartsWith("ERR:"), Ex(""));
        T.Check("ternary without else", Ex("a if b").StartsWith("ERR:"), Ex("a if b"));
        T.Check("lua dict with a mixed key still parses", Ex("{a = 1, \"b\": 2}").StartsWith("ERR:") == false, Ex("{a = 1, \"b\": 2}"));
        T.Check("keyword as expression", Ex("else").StartsWith("ERR:1:1"), Ex("else"));
        T.Check("'is not' is not GDScript 4.0-4.3 syntax and is rejected", Ex("a is not B").StartsWith("ERR:1:6"), Ex("a is not B"));
    }

    // ---- statements ----

    private static void StmtSmall()
    {
        StEq("var a = 1", "(block (var a = 1))");
        StEq("var a: int = 1\nvar b := 2\nvar c\nvar d: Node", "(block (var a :int = 1) (var b := 2) (var c) (var d :Node))");
        StEq("var a = 1; var b = 2", "(block (var a = 1) (var b = 2))");
        StEq("const C = 3\nconst D: float = 1.5\nconst E := 2", "(block (const C = 3) (const D :float = 1.5) (const E := 2))");
        StEq("pass", "(block (pass))");
        StEq("return", "(block (return))");
        StEq("return a if b else c", "(block (return (?: a b c)))");
        StEq("break\ncontinue\nbreakpoint", "(block (break) (continue) (breakpoint))");
        StEq("assert(x)\nassert(x, \"msg\")", "(block (assert x) (assert x \"msg\"))");
        StEq("if a:\n\tx\nelif b:\n\ty\nelse:\n\tz", "(block (if (a (block x)) (b (block y)) (else (block z))))");
        StEq("if a: x\nelse: y", "(block (if (a (block x)) (else (block y))))");
        StEq("if a:\n\tif b:\n\t\tx\n\telse:\n\t\ty", "(block (if (a (block (if (b (block x)) (else (block y)))))))");
        StEq("while a < 3:\n\ta += 1", "(block (while (< a 3) (block (+= a 1))))");
        StEq("for i in range(3):\n\tf(i)", "(block (for i in (call range 3) (block (call f i))))");
        StEq("for i: int in x:\n\tpass", "(block (for i :int in x (block (pass))))");
        StEq("for i in x: pass", "(block (for i in x (block (pass))))");
        StEq("match x:\n\t1:\n\t\ta\n\t_:\n\t\tb", "(block (match x (branch (1) (block a)) (branch (_) (block b))))");
        StEq("var f = func(x):\n\treturn x\nvar y = 1", "(block (var f = (lambda (x) (block (return x)))) (var y = 1))");
        StEq("a = 1\nb += 2\nc.d()", "(block (= a 1) (+= b 2) (call (. c d)))");
        StEq("# only a comment\nx\n\n# another\ny", "(block x y)");
        StEq("x\n\n\n\ny", "(block x y)");

        Block b = Body("if a:\n\tx\nelif b:\n\ty\nelif c:\n\tz\nelse:\n\tw");
        var s = (IfStmt)b.Statements[0];
        T.Check("if has three branches and an else", s.Branches.Count == 3 && s.Else != null);
        T.Check("the if line and branch bounds", s.Branches[1].Start.Line == 4 && s.Branches[2].Start.Line == 6 && s.Start.Line == 2 && s.End.Line == 9, $"{s.Start}-{s.End}");
    }

    // ---- fixtures ----

    private static void FxDeclarations()
    {
        ClassDecl c = ParseFx("02_declarations.gd").Script;
        Eq("class_name", "Declarations", c.Name ?? "<null>");
        Eq("extends a script path", "res://base/base_actor.gd", c.Extends?.ResPath ?? "<null>");
        T.Check("@tool", c.IsTool);
        Eq("@icon", "res://icon.svg", c.Icon ?? "<null>");
        T.Check("script annotations stay on the class", c.Annotations.Count == 2 && !c.Members.OfType<StandaloneAnnotation>().Any(a => a.Annotation.Name is "tool" or "icon"));
        Eq("class doc comment", "A script that declares everything a class body can hold.", c.DocComment ?? "<null>");

        T.Check("three signals", c.Members.OfType<SignalDecl>().Count() == 3);
        Eq("signal without parameters", "(signal died)", D(Member<SignalDecl>(c, "died")));
        Eq("signal with typed parameters", "(signal hit damage:int source:Node3D)", D(Member<SignalDecl>(c, "hit")));
        Eq("signal with an inferred default", "(signal moved from to:=(. Vector3 ZERO))", D(Member<SignalDecl>(c, "moved")));

        Eq("named enum", "(enum State IDLE RUN=5 JUMP FALL=(<< 1 3))", D(Member<EnumDecl>(c, "State")));
        EnumDecl anon = c.Members.OfType<EnumDecl>().First(e => e.Name == null);
        Eq("anonymous enum", "(enum <anon> ANON_A ANON_B=2)", D(anon));

        Eq("const untyped", "(const MAX_HEALTH = 100)", D(Member<ConstDecl>(c, "MAX_HEALTH")));
        Eq("const typed", "(const SPEED :float = 4.5)", D(Member<ConstDecl>(c, "SPEED")));
        Eq("const inferred", "(const GRAVITY := 9.8)", D(Member<ConstDecl>(c, "GRAVITY")));
        Eq("const with a generic type", "(const NAMES :Array[String] = (array \"a\" \"b\"))", D(Member<ConstDecl>(c, "NAMES")));
        Eq("const dictionary", "(const LOOKUP = (dict (\"one\" 1) (\"two\" 2)))", D(Member<ConstDecl>(c, "LOOKUP")));

        VarDecl health = V(c, "health");
        Eq("@export var", "@export (var health :int = MAX_HEALTH)", D(health));
        Eq("var doc comment", "The hit points.", health.DocComment ?? "<null>");
        Eq("annotation with arguments", "@export_range(0.0 10.0 0.1 \"or_greater\") (var speed := SPEED)", D(V(c, "speed")));
        Eq("export_enum", "@export_enum(\"Warrior\" \"Mage\" \"Thief\") (var job :int)", D(V(c, "job")));
        StandaloneAnnotation grp = c.Members.OfType<StandaloneAnnotation>().Single();
        Eq("a standalone annotation is its own member", "@export_group(\"Visual\")", D(grp));
        T.Check("it comes before the vars it groups", c.Members.IndexOf(grp) < c.Members.IndexOf(V(c, "tint")) && c.Members.IndexOf(grp) > c.Members.IndexOf(V(c, "job")));
        Eq("typed export", "@export (var tint :Color = (. Color WHITE))", D(V(c, "tint")));
        Eq("@onready with $", "@onready (var sprite = ($ Sprite))", D(V(c, "sprite")));
        Eq("@onready with % and a type", "@onready (var timer :Timer = ($ %Cooldown))", D(V(c, "timer")));
        Eq("untyped var", "(var plain)", D(V(c, "plain")));
        Eq("typed var", "(var typed :Vector3)", D(V(c, "typed")));
        Eq("inferred var", "(var inferred := (call Vector3 1 2 3))", D(V(c, "inferred")));
        Eq("typed array", "(var items :Array[int] = (array 1 2 3))", D(V(c, "items")));
        Eq("typed dictionary", "(var table :Dictionary[String, int] = (dict))", D(V(c, "table")));
        Eq("nested generic", "(var nested :Array[Array] = (array))", D(V(c, "nested")));
        T.Check("dotted type", V(c, "other").Type!.Parts.SequenceEqual(new[] { "Outer", "Inner" }) && V(c, "other").Type!.Name == "Outer.Inner");
        T.Check("static var", V(c, "counter").IsStatic && V(c, "counter").Inferred && V(c, "shared").IsStatic && V(c, "shared").Type!.Name == "int");
        T.Check("the next vars are not annotated by the group", V(c, "a").Annotations.Count == 1 && V(c, "b").Annotations.Count == 1);
        FuncDecl rh = F(c, "remote_hit");
        Eq("@rpc on a func with an inline body", "@rpc(\"any_peer\" \"call_local\" \"reliable\") (func remote_hit (amount) (block (pass)))", D(rh));

        ClassDecl weapon = Member<ClassDecl>(c, "Weapon")!;
        T.Check("inner class with extends", weapon.IsInner && weapon.Extends!.ToString() == "Node");
        Eq("inner class members", "(var damage := 3)|(func fire () -> void (block (pass)))|(class Muzzle (var flash = true))",
           string.Join("|", weapon.Members.Select(D)));
        Eq("nested inner class", "Muzzle", Member<ClassDecl>(weapon, "Muzzle")!.Name!);
        ClassDecl plain = Member<ClassDecl>(c, "Plain")!;
        Eq("inner class without extends", "(class Plain (signal ready_to_go) (const LIMIT = 2))", D(plain));
        T.Check("positions of an inner class", weapon.Start.Line == LineOf("02_declarations.gd", "class Weapon") && weapon.End.Line == LineOf("02_declarations.gd", "\t\tvar flash") &&
                plain.Start.Line == LineOf("02_declarations.gd", "class Plain"), $"{weapon.Start}-{weapon.End} {plain.Start}");
    }

    private static void FxProperties()
    {
        ClassDecl c = ParseFx("03_properties.gd").Script;
        VarDecl h = V(c, "health");
        T.Check("health: setter with a parameter and a getter", h.Setter!.IsSetter && h.Setter.ParamName == "value" && h.Getter != null && !h.Getter.IsSetter);
        Eq("health setter body", "(block (= health (call clampi value 0 100)) (call (. health_changed emit) health))", D(h.Setter.Body));
        Eq("health getter body", "(block (return health))", D(h.Getter!.Body));
        Eq("health type and init", "int/10", h.Type + "/" + D(h.Init));

        VarDecl lives = V(c, "lives");
        T.Check("delegates: set = fn, get = fn", lives.Setter!.Delegate == "set_lives" && lives.Getter!.Delegate == "get_lives" && lives.Setter.Body == null && lives.Getter.Body == null);
        VarDecl mana = V(c, "mana");
        T.Check("getter only, inferred", mana.Setter == null && mana.Getter!.Body != null && mana.Inferred && D(mana.Init) == "5");
        Eq("mana getter", "(block (return (* mana 2)))", D(mana.Getter.Body));
        VarDecl armor = V(c, "armor");
        T.Check("setter only", armor.Getter == null && armor.Setter!.ParamName == "v");
        VarDecl un = V(c, "untyped");
        T.Check("no type, get() with empty parentheses", un.Type == null && un.Init == null && un.Setter!.ParamName == "v" && un.Getter!.ParamName == null && un.Getter.Body != null);
        VarDecl sh = V(c, "shield");
        T.Check("indented block with a single delegate", sh.Getter!.Delegate == "get_shield" && sh.Setter == null);
        VarDecl one = V(c, "one_line");
        T.Check("both delegates on one line", one.Setter!.Delegate == "set_one_line" && one.Getter!.Delegate == "get_one_line");
        VarDecl tight = V(c, "tight");
        T.Check("inline getter body", tight.Getter!.Body!.Statements.Count == 1 && D(tight.Getter.Body) == "(block (return 7))" && tight.Inferred);
        Eq("whole property in the dump", "(var armor :float = 1.0 (set v (block (= armor v))))", D(armor));
        T.Check("the declaration after a property block parses", Member<SignalDecl>(c, "health_changed") != null && F(c, "get_one_line") != null);
        T.Check("accessor ranges are inside the var", h.Setter.Start.Line == 4 && h.Getter.Start.Line == 7 && h.End.Line == 8, $"{h.Setter.Start} {h.Getter.Start} {h.End}");
    }

    private static void FxFunctions()
    {
        ClassDecl c = ParseFx("04_functions.gd").Script;
        Eq("no params", "(func no_args () (block (pass)))", D(F(c, "no_args")));
        Eq("typed params with defaults", "(func typed (a:int b:float=2.0 c:=\"x\" d=null) -> bool (block (return (> a 0))))", D(F(c, "typed")));
        Eq("-> void", "void", F(c, "returns_void").ReturnType!.Name);
        T.Check("IsVoid", F(c, "returns_void").ReturnType!.IsVoid);
        Eq("generic return type", "Array[int]", F(c, "returns_array").ReturnType!.ToString());
        Eq("typed dictionary return type", "Dictionary[String, Node3D]", F(c, "returns_dict").ReturnType!.ToString());
        Eq("dotted return type", "Outer.Inner", F(c, "returns_inner").ReturnType!.Name);
        FuncDecl make = F(c, "make");
        T.Check("static func", make.IsStatic && make.Params[0].Type!.Name == "StringName" && D(make.Body) == "(block (return (call (. Functions new))))");
        FuncDecl variadic = F(c, "variadic");
        T.Check("rest parameter", variadic.Params.Count == 2 && !variadic.Params[0].IsRest && variadic.Params[1].IsRest && variadic.Params[1].Name == "rest" && variadic.Params[1].Type!.Name == "Array");
        Eq("parameter list over several lines with a trailing comma", "a|b|c=3", string.Join("|", F(c, "trailing").Params.Select(p => p.Name + (p.Default != null ? "=" + D(p.Default) : ""))));

        FuncDecl ready = F(c, "_ready");
        var st = ready.Body.Statements;
        Eq("super()", "(call super)", D(st[0]));
        Eq("super.method()", "(call (. super _ready))", D(st[1]));
        Eq("var with await", "(var x := (await (. (call (. (call get_tree) create_timer) 0.5) timeout)))", D(st[2]));
        Eq("await a node signal", "(await (. ($ Anim) animation_finished))", D(st[3]));
        Eq("await a bare signal", "(var y = (await some_signal))", D(st[4]));
        Eq("_init calls super with args", "(func _init (value=1) (block (call super value)))", D(F(c, "_init")));
        Eq("one-liner", "(func one_liner () (block (return 42)))", D(F(c, "one_liner")));
        Eq("semicolons", "(block (var a = 1) (var b = 2) (return (+ a b)))", D(F(c, "multi_statement").Body));

        FuncDecl wl = F(c, "with_locals");
        var ws = wl.Body.Statements;
        Eq("local const", "(const LIMIT = 10)", D(ws[0]));
        Eq("local inferred", "(var total := 0)", D(ws[1]));
        Eq("local typed", "(var typed_local :int = 5)", D(ws[2]));
        Eq("compound assignment statement", "(+= total typed_local)", D(ws[3]));
        Eq("assert", "(assert (> total 0))", D(ws[4]));
        Eq("assert with message", "(assert (< total LIMIT) \"too big\")", D(ws[5]));
        Eq("annotation on a local", "@warning_ignore(\"unused_variable\") (var ignored = 1)", D(ws[6]));
        T.Check("function range", wl.Start.Line == LineOf("04_functions.gd", "func with_locals") && wl.End.Line == LineOf("04_functions.gd", "\treturn total"), $"{wl.Start}-{wl.End}");
        T.Check("every function is a member", c.Members.OfType<FuncDecl>().Count() == 14, c.Members.OfType<FuncDecl>().Count().ToString());
    }

    private static void FxControlFlow()
    {
        ClassDecl c = ParseFx("05_control_flow.gd").Script;
        Eq("if / elif / else with inline elif", "(block (if ((< x 0) (block (return \"neg\"))) ((== x 0) (block (return \"zero\"))) ((< x 10) (block (return \"small\"))) (else (block (return \"big\")))))",
           D(F(c, "branches").Body));
        var ls = F(c, "loops").Body.Statements;
        T.Check("seven statements in loops", ls.Count == 7, ls.Count.ToString());
        var w = (WhileStmt)ls[1];
        Eq("while body", "(block (+= n 1) (if ((== n 3) (block (continue)))) (if ((> n 7) (block (break)))))", D(w.Body));
        Eq("for over range", "(for i in (call range 5) (block (call print i)))", D(ls[2]));
        Eq("for with a typed variable", "(for i :int in (array 1 2 3) (block (call print i)))", D(ls[3]));
        Eq("for over a dictionary", "(for k in (dict (\"a\" 1)) (block (pass)))", D(ls[4]));
        Eq("for with an inline body", "(for child in (call get_children) (block (call (. child queue_free))))", D(ls[5]));
        Eq("while true: break", "(while true (block (break)))", D(ls[6]));
        Eq("nested loops", "(block (for i in 3 (block (for j in 3 (block (if ((== i j) (block (pass))) (else (block (call print i j)))))))))", D(F(c, "nested").Body));
        var ts = F(c, "ternaries").Body.Statements;
        Eq("ternary", "(var m = (?: a (> a b) b))", D(ts[0]));
        Eq("chained ternary", "(var s = (?: \"x\" a (?: \"y\" b \"z\")))", D(ts[1]));
        Eq("ternary in return", "(return (?: (- 1) (< a 0) 1))", D(ts[2]));
        var ii = F(c, "inline_if").Body.Statements;
        T.Check("inline if", ii.Count == 2 && ((IfStmt)ii[0]).Branches[0].Body.Statements.Count == 1 && ((IfStmt)ii[0]).Else == null);
        var second = (IfStmt)ii[1];
        T.Check("inline if with ; and else", second.Branches[0].Body.Statements.Count == 2 && second.Else!.Statements.Count == 1);
        var ms = F(c, "misc").Body.Statements;
        Eq("misc", "(pass)|(pass)|(breakpoint)|(return)", string.Join("|", ms.Select(D)));
    }

    private static void FxMatch()
    {
        ClassDecl c = ParseFx("06_match.gd").Script;
        var lit = (MatchStmt)F(c, "literals").Body.Statements[0];
        Eq("subject", "x", D(lit.Subject));
        Eq("literal patterns", "1|2 3|\"hello\"|(- 1)|1.5|true|null|(. Dir UP)|LIMIT|_", string.Join("|", lit.Branches.Select(b => string.Join(" ", b.Patterns.Select(D)))));
        T.Check("a literal pattern is a PatValue, _ a PatWildcard", lit.Branches[0].Patterns[0] is PatValue && lit.Branches[9].Patterns[0] is PatWildcard && lit.Branches[1].Patterns.Count == 2);
        Eq("branch body", "(block (call print \"one\"))", D(lit.Branches[0].Body));

        var bind = (MatchStmt)F(c, "binding").Body.Statements[0];
        Eq("binding with a guard", "(var v) when (> v 10)", D(bind.Branches[0].Patterns[0]) + " when " + D(bind.Branches[0].Guard));
        T.Check("binding without a guard", bind.Branches[1].Patterns[0] is PatBind { Name: "w" } && bind.Branches[1].Guard == null);

        var guards = (MatchStmt)F(c, "guards").Body.Statements[0];
        Eq("guards", "1 when (> y 0)|2 3 when (< y 0)|_ when (== y 0)",
           string.Join("|", guards.Branches.Select(b => string.Join(" ", b.Patterns.Select(D)) + " when " + D(b.Guard))));

        var arrays = (MatchStmt)F(c, "arrays").Body.Statements[0];
        Eq("array patterns", "(parr)|(parr 1)|(parr 1 2)|(parr (var a) (var b))|(parr 1 ..)|(parr (var first) _ ..)|(parr (parr 1 2) (parr (var c) ..))",
           string.Join("|", arrays.Branches.Select(b => D(b.Patterns[0]))));
        T.Check("HasRest", ((PatArray)arrays.Branches[4].Patterns[0]).HasRest && !((PatArray)arrays.Branches[3].Patterns[0]).HasRest);

        var dicts = (MatchStmt)F(c, "dictionaries").Body.Statements[0];
        Eq("dictionary patterns",
           "(pdict)|(pdict (\"name\" \"Dennis\"))|(pdict (\"name\" (var n)) (\"age\" 18))|(pdict (\"name\" \"Dennis\") ..)|(pdict (\"k\" (parr (var a) ..)) (\"z\" (pdict (\"y\" (var q)))))",
           string.Join("|", dicts.Branches.Select(b => D(b.Patterns[0]))));
        T.Check("dictionary HasRest", ((PatDict)dicts.Branches[3].Patterns[0]).HasRest && !((PatDict)dicts.Branches[2].Patterns[0]).HasRest);

        var inl = (MatchStmt)F(c, "inline_bodies").Body.Statements[0];
        Eq("inline bodies", "(block (call print \"one\"))|(block (return 2))|(block (pass))", string.Join("|", inl.Branches.Select(b => D(b.Body))));
    }

    private static void FxExpressions()
    {
        ClassDecl c = ParseFx("07_expressions.gd").Script;
        Eq("preload const", "(const Bullet = (preload \"res://bullet.tscn\"))", D(Member<ConstDecl>(c, "Bullet")));
        Eq("$ path", "@onready (var label = ($ UI/Label))", D(V(c, "label")));
        Eq("quoted $ path", "@onready (var camera :Camera3D = ($ ../Camera Rig/Camera3D))", D(V(c, "camera")));
        Eq("% unique", "@onready (var bar = ($ %HealthBar))", D(V(c, "bar")));
        Eq("$ path with a unique segment", "@onready (var deep = ($ Pivot/%Marker))", D(V(c, "deep")));
        T.Check("IsUnique", ((GetNodeExpr)V(c, "bar").Init!).IsUnique && !((GetNodeExpr)V(c, "label").Init!).IsUnique);
        Eq("dictionary literal", "(var table := (dict (\"a\" 1) (\"b\" (array 1 2)) (\"c\" (dict (\"d\" 2)))))", D(V(c, "table")));
        Eq("lua dictionary", "(var lua := (dict-lua (\"name\" \"bob\") (\"age\" 3)))", D(V(c, "lua")));
        Eq("mixed array", "(var arr := (array 1 2.5 \"three\" (array 4) (dict (\"five\" 5)) null true))", D(V(c, "arr")));
        Eq("typed array var", "(var typed_arr :Array[Node3D] = (array))", D(V(c, "typed_arr")));
        Eq("typed dictionary var", "(var typed_dict :Dictionary[String, Array] = (dict))", D(V(c, "typed_dict")));
        Eq("typed array constructor", "(var made = (call ([] Array int)))", D(V(c, "made")));

        FuncDecl ar = F(c, "arithmetic");
        string[] expect =
        {
            "(+ a (* b c))", "(* (+ a b) c)", "(- (- a b) c)", "(** (** a b) c)", "(- (** a 2))", "(** 2 (- 1))", "(% a b)", "(* (/ a b) c)",
            "(| (<< a 2) (^ (& b c) 1))", "(+ (~ a) 1)", "(- (- a))", "(or (and (== a b) (!= b c)) (not a))", "(or (and a b) (not c))",
            "(and (and (and (< a b) (<= b c)) (> c a)) (>= c a))", "(in a b)", "(not in a b)", "(not (in a b))", "(is a Node3D)", "(as a Node3D)",
            "(. (as a Node3D) position)", "(?: a b c)", "(% \"x %s y %d\" (array a b))", "(% \"single %s\" a)", "(is a Array[int])",
        };
        for (int i = 0; i < expect.Length; i++) Eq($"arithmetic r{i + 1}", expect[i], D(Local(ar, "r" + (i + 1)).Init));

        Eq("assignments", "(= a 1)|(+= a 1)|(-= a 1)|(*= a 2)|(/= a 2)|(%= a 3)|(**= a 2)|(&= a 1)|(|= a 2)|(^= a 3)|(<<= a 1)|(>>= a 1)|(= (. (. a b) c) 4)|(= ([] a 0) 5)|(+= ([] ([] a 0) 1) 6)|(= (. self value) 7)|(= (. ($ Node) visible) false)",
           string.Join("|", F(c, "assignments").Body.Statements.Select(D)));
        Eq("calls", "(call print)|(call print 1 2)|(call (call (. a method) 1) 2)|(call (. ([] (. (call (. (. a b) c)) d) 0) e))|(call (. (call get_node \"Path\") queue_free))|(= (. (call (. ($ Mesh) get_surface_override_material) 0) albedo_color) (. Color RED))|(call (. (call load \"res://x.tscn\") instantiate))|(call (. Bullet instantiate))|(call (. super foo) 1)|(* (call (. (call Vector3 1 2 3) normalized)) 5.0)|(return (await (. (call get_tree) process_frame)))",
           string.Join("|", F(c, "calls").Body.Statements.Select(D)));
        var atoms = F(c, "atoms");
        Eq("atoms", "self|null|(array true false)|(+ (+ PI TAU) INF)|^\"Foo/Bar\"|&\"action\"|(+ (call (. a size)) 2)|(+ a 1)",
           string.Join("|", atoms.Body.Statements.OfType<VarDecl>().Select(v => D(v.Init))));
        int ln = LineOf("07_expressions.gd", "\tvar lines");
        T.Check("a continued line keeps the right line numbers", Local(atoms, "lines").Start.Line == ln && Local(atoms, "lines").End.Line == ln + 1 && Local(atoms, "multi").Start.Line == ln + 2,
                $"{Local(atoms, "lines").Start}-{Local(atoms, "lines").End} {Local(atoms, "multi").Start}");
        Eq("the parenthesised value is flagged", "True", ((Binary)Local(atoms, "multi").Init!).Parenthesized.ToString());
    }

    private static void FxLambdas()
    {
        ClassDecl c = ParseFx("08_lambdas.gd").Script;
        var il = (LambdaExpr)V(c, "inline_lambda").Init!;
        T.Check("inline lambda", il.IsInline && il.Name == null && D(il) == "(lambda (x) (block (return (* x 2))))", D(il));
        Eq("typed lambda", "(lambda (a:int b:=2) -> int (block (return (+ a b))))", D(V(c, "typed_lambda").Init));
        var named = (LambdaExpr)V(c, "named_lambda").Init!;
        T.Check("named lambda", named.Name == "fact" && D(named.Body) == "(block (return (?: 1 (<= n 1) (* n (call fact (- n 1))))))", D(named));
        var bl = (LambdaExpr)V(c, "block_lambda").Init!;
        T.Check("block lambda at statement level", !bl.IsInline && bl.Body.Statements.Count == 2, D(bl));
        Eq("the declaration after a block lambda", "(var after_lambda = 5)", D(V(c, "after_lambda")));

        FuncDecl f = F(c, "connect_things");
        var lambdas = All<LambdaExpr>(f).ToList();
        T.Check("sixteen lambdas", lambdas.Count == 16, lambdas.Count.ToString());
        T.Check("seven inline and nine blocks", lambdas.Count(l => l.IsInline) == 7 && lambdas.Count(l => !l.IsInline) == 9);
        var st = f.Body.Statements;
        T.Check("fourteen statements", st.Count == 14, st.Count.ToString());
        Eq("inline lambda argument", "(call (. (. button pressed) connect) (lambda () (block (call print \"pressed\"))))", D(st[0]));
        Eq("block lambda argument, ) on its own line", "(call (. (. button pressed) connect) (lambda () (block (call print \"multi\") (call queue_free))))", D(st[1]));
        Eq("block with an if", "(call (. (. button toggled) connect) (lambda (on:bool) (block (if (on (block (call print \"on\"))) (else (block (call print \"off\")))))))", D(st[2]));
        Eq("map with an inline lambda", "(call (. items map) (lambda (i) (block (return (* i 2)))))", D(st[3]));
        Eq("a block lambda followed by .map", "(call (. (call (. items filter) (lambda (i) (block (return (> i 1))))) map) (lambda (i) (block (return (call str i)))))", D(st[4]));
        Eq("closing parenthesis on the last body line", "(call (. items sort_custom) (lambda (a b) (block (return (< a b)))))", D(st[5]));
        Eq("reduce with an extra argument", "(var total = (call (. items reduce) (lambda (acc i) (block (return (+ acc i)))) 0))", D(st[6]));
        Eq("lambdas in an array", "(var pair = (array (lambda () (block (return 1))) (lambda () (block (return 2)))))", D(st[7]));
        Eq("lambdas in a dictionary", "(var d = (dict (\"f\" (lambda (x) (block (return x)))) (\"g\" 3)))", D(st[8]));
        var cd = (Call)st[9].Descendants().OfType<Call>().First();
        T.Check("arguments after a block lambda", cd.Args.Count == 3 && cd.Args[1] is LambdaExpr { IsInline: false } && D(cd.Args[2]) == "5" && D(cd.Args[0]) == "\"foo\"", D(cd));
        var outer = (LambdaExpr)((VarDecl)st[10]).Init!;
        T.Check("nested lambdas", outer.Body.Statements.Count == 2 && outer.Body.Statements[0] is VarDecl { Init: LambdaExpr { IsInline: false } }, D(outer));
        Eq("a lambda block inside a lambda block", "(call (. (. t timeout) connect) (lambda () (block (var nested = (lambda () (block (pass)))) (call (. nested call)))))", D(st[12]));
        Eq("last statement", "(return outer)", D(st[13]));
    }

    private static void FxStrings()
    {
        string text = Fx("09_strings.gd");
        ClassDecl c = ParseFx("09_strings.gd").Script;
        string S(string name) => (string)((Literal)V(c, name).Init!).Value!;
        Eq("double", "double", S("plain"));
        Eq("single", "single", S("single"));
        Eq("quotes", "it's \"quoted\"", S("quote_in"));
        Eq("escapes", "tab\there\nnewline \\ backslash A \U0001F600", S("escapes"));
        var raw = (Literal)V(c, "raw").Init!;
        T.Check("raw", raw.Raw && (string)raw.Value! == "dir\\path\\n");
        Eq("raw with an escaped quote", "a\\'b", S("raw_single"));
        var rt = (Literal)V(c, "raw_triple").Init!;
        T.Check("raw triple", rt.Raw && rt.Triple && (string)rt.Value! == "raw \\n\nsecond");
        var tri = (Literal)V(c, "triple").Init!;
        T.Check("triple", tri.Triple && (string)tri.Value! == "line one\nline \"two\"\nline three");
        Eq("triple single quotes", "it's\nfine", S("triple_single"));
        T.Check("StringName", ((Literal)V(c, "string_name").Init!).Kind == LitKind.StringName && S("string_name") == "move_left");
        T.Check("NodePath", ((Literal)V(c, "node_path").Init!).Kind == LitKind.NodePath && S("node_path") == "Player/Camera:fov");
        Eq("concatenation", "(var joined = (+ \"a\" \"b\"))", D(V(c, "joined")));
        Eq("format operator", "(var formatted = (% \"%d items, %s\" (array 3 \"ok\")))", D(V(c, "formatted")));
        Eq("backslash-newline in a string", "abcdef", S("cont"));
        string[] lines = text.Replace("\r\n", "\n").Split('\n');
        foreach (string name in new[] { "raw_triple", "triple", "triple_single", "joined", "formatted", "cont" })
        {
            int expected = Array.FindIndex(lines, l => l.StartsWith("var " + name + " ")) + 1;
            T.Check($"{name} starts on line {expected}", V(c, name).Start.Line == expected, V(c, name).Start.ToString());
        }
        T.Check("a multi-line string ends on its last line", V(c, "triple").End.Line == V(c, "triple").Start.Line + 2);
    }

    private static void FxPlayer()
    {
        ClassDecl c = ParseFx("player_controller.gd").Script;
        Eq("name and base", "PlayerController extends CharacterBody3D", c.Name + " extends " + c.Extends);
        T.Check("signals", c.Members.OfType<SignalDecl>().Select(s => s.Name).SequenceEqual(new[] { "jumped", "landed", "health_changed", "died" }));
        Eq("enum", "(enum State IDLE WALK SPRINT AIR DEAD)", D(Member<EnumDecl>(c, "State")));
        T.Check("five consts", c.Members.OfType<ConstDecl>().Count() == 5 && Member<ConstDecl>(c, "WALK_SPEED")!.Inferred);
        T.Check("two export groups", c.Members.OfType<StandaloneAnnotation>().Select(a => (string)((Literal)a.Annotation.Args[0]).Value!).SequenceEqual(new[] { "Movement", "Combat" }));
        Eq("export_flags", "@export_flags(\"Fire\" \"Ice\" \"Poison\") (var resistances :int = 0)", D(V(c, "resistances")));
        Eq("onready node paths", "($ Head)|($ Head/Camera3D)|($ Head/Camera3D/RayCast3D)|($ %AnimationPlayer)|($ DashTimer)",
           string.Join("|", c.Members.OfType<VarDecl>().Where(v => v.HasAnnotation("onready")).Select(v => D(v.Init))));
        Eq("enum-typed var with an enum default", "State/(. State IDLE)", V(c, "state").Type + "/" + D(V(c, "state").Init));
        Eq("emit inside the setter", "(call (. health_changed emit) old health)", D(V(c, "health").Setter!.Body!.Statements[2]));
        T.Check("the setter has a local and an if", V(c, "health").Setter!.Body!.Statements.Count == 4 && V(c, "health").Setter.Body.Statements[3] is IfStmt, D(V(c, "health").Setter.Body));
        Eq("typed dictionary and StringName array", "Dictionary[String, int]|Array[StringName]", V(c, "inventory").Type + "|" + V(c, "tags").Type);
        T.Check("nine functions", c.Members.OfType<FuncDecl>().Count() == 9);
        T.Check("two awaits", All<AwaitExpr>(c).Count() == 2);
        T.Check("two ternaries", All<Ternary>(c).Count() == 2);
        T.Check("one match, with a wildcard branch", All<MatchStmt>(c).Single().Branches.Last().Patterns[0] is PatWildcard);
        var input = (IfStmt)F(c, "_unhandled_input").Body.Statements[0];
        Eq("event is X and Y", "(and (is event InputEventMouseMotion) (== (. Input mouse_mode) (. Input MOUSE_MODE_CAPTURED)))", D(input.Branches[0].Cond));
        T.Check("elif branch", input.Branches.Count == 2 && input.Else == null);
        var pp = F(c, "_physics_process");
        T.Check("physics process statements", pp.Body.Statements.Count == 12, pp.Body.Statements.Count.ToString());
        Eq("typed locals with :=", "(var direction := (call (. (* (. transform basis) (call Vector3 (. input_dir x) 0 (. input_dir y))) normalized)))", D(Local(pp, "direction")));
        Eq("ternary in a local", "(var speed := (?: SPRINT_SPEED (call (. Input is_action_pressed) \"sprint\") WALK_SPEED))", D(Local(pp, "speed")));
        Eq("func with a default argument", "(func pick_up (item:StringName count:=1) -> bool (block (= ([] inventory item) (+ (call (. inventory get) item 0) count)) (return true)))", D(F(c, "pick_up")));
        T.Check("every node has a plausible range", All<Node>(c).All(n => n.End.Line >= n.Start.Line));
    }

    private static void FxMenu()
    {
        ClassDecl c = ParseFx("ui_menu.gd").Script;
        T.Check("@tool", c.IsTool);
        Eq("name and base", "MainMenu extends Control", c.Name + " extends " + c.Extends);
        Eq("doc comment", "The title screen: buttons, a settings panel and a fading logo.", c.DocComment ?? "<null>");
        var levels = (DictLit)Member<ConstDecl>(c, "LEVELS")!.Value!;
        T.Check("multi-line dictionary constant with a trailing comma", levels.Entries.Count == 3 && D(levels.Entries[2].Key) == "\"arena\"", D(levels));
        Eq("preload const", "(const CLICK_SOUND = (preload \"res://audio/click.ogg\"))", D(Member<ConstDecl>(c, "CLICK_SOUND")));
        VarDecl title = V(c, "title");
        T.Check("export with a setter that uses %", title.HasAnnotation("export") && title.Setter!.ParamName == "value" && D(title.Setter.Body).Contains("($ %Title)"), D(title));
        Eq("export_multiline", "@export_multiline (var credits_text :String = \"\")", D(V(c, "credits_text")));
        Eq("export_file", "@export_file(\"*.tscn\") (var first_level :String)", D(V(c, "first_level")));
        Eq("typed array of nodes", "@onready (var buttons :Array[Button] = (array ($ Panels/Main/Start) ($ Panels/Main/Settings) ($ Panels/Main/Quit)))", D(V(c, "buttons")));
        T.Check("three lambdas in _ready, one a block", All<LambdaExpr>(F(c, "_ready")).Count() == 3 && All<LambdaExpr>(F(c, "_ready")).Count(l => !l.IsInline) == 1);
        Eq("bind", "(call (. (. b pressed) connect) (call (. _on_button_pressed bind) (. b name)))", D(All<ExprStmt>(F(c, "_ready")).First(s => D(s).Contains("bind"))));
        var matches = All<MatchStmt>(c).ToList();
        T.Check("two matches", matches.Count == 2);
        Eq("StringName patterns", "&\"Start\"|&\"Settings\"|&\"Quit\"|_", string.Join("|", matches[1].Branches.Select(b => D(b.Patterns[0]))));
        Eq("match on an enum constant", "(. Panel MAIN)|(. Panel SETTINGS)|(. Panel CREDITS)", string.Join("|", matches[0].Branches.Select(b => D(b.Patterns[0]))));
        Eq("$ path statement", "(block (call (. ($ Panels/Credits/Back) grab_focus)))", D(matches[0].Branches[2].Body));
        Eq("string format with %s", "(call push_warning (% \"Unknown button: %s\" button_name))", D(All<ExprStmt>(matches[1]).Last()));
        var sg = F(c, "start_game");
        T.Check("typed local from a call", D(sg.Body.Statements[0]) == "(var path :String = (call (. LEVELS get) level_key first_level))", D(sg.Body.Statements[0]));
        T.Check("await inside", All<AwaitExpr>(sg).Single().Operand is MemberAccess { Name: "finished" });
        T.Check("every function is found", new[] { "_ready", "show_panel", "_on_button_pressed", "start_game", "play_click", "_unhandled_input" }.All(n => Member<FuncDecl>(c, n) != null));
    }

    private static ClassDecl Ok(string src)
    {
        ParseResult r = GdScriptParser.Parse(src);
        T.Check("parses: " + src.Split('\n')[0], r.Ok, string.Join("; ", r.Errors));
        return r.Script;
    }

    private static void HeaderForms()
    {
        ClassDecl c = Ok("class_name A extends B\n");
        T.Check("class_name and extends on one line", c.Name == "A" && c.Extends!.ToString() == "B");
        c = Ok("extends Outer.Inner\n");
        T.Check("dotted extends", c.Extends!.Path.SequenceEqual(new[] { "Outer", "Inner" }) && c.Extends.ResPath == null);
        c = Ok("extends \"res://x.gd\".Inner\n");
        T.Check("path with an inner class", c.Extends!.ResPath == "res://x.gd" && c.Extends.Path.SequenceEqual(new[] { "Inner" }) && c.Extends.ToString() == "\"res://x.gd\".Inner");
        c = Ok("extends \"res://x.gd\"\n");
        T.Check("path only", c.Extends!.ResPath == "res://x.gd" && c.Extends.Path.Count == 0);
        c = Ok("class_name A, \"res://i.svg\"\n");
        Eq("Godot 3 icon form", "res://i.svg", c.Icon ?? "<null>");
        c = Ok("@tool\nextends Node\n");
        T.Check("@tool on its own line", c.IsTool && c.Extends!.ToString() == "Node");
        c = Ok("@tool extends Node\n");
        T.Check("@tool on the extends line", c.IsTool && c.Extends!.ToString() == "Node");
        c = Ok("@static_unload\nextends Node\nstatic var n = 1\n");
        T.Check("@static_unload", c.IsStaticUnload && V(c, "n").IsStatic);
        c = Ok("extends Node\n## Under extends\n## second line\n\nvar x\n");
        Eq("class doc under extends", "Under extends\nsecond line", c.DocComment ?? "<null>");
        T.Check("and the next member has none", V(c, "x").DocComment == null);
        c = Ok("extends Node\n## about x\nvar x\n");
        T.Check("a doc block touching a member documents the member", c.DocComment == null && V(c, "x").DocComment == "about x");
        c = Ok("## Above extends\nextends Node\n");
        Eq("class doc above extends", "Above extends", c.DocComment ?? "<null>");
        c = Ok("var a\n\n## about b\n## more\nvar b\n## stray\n\nvar c\n");
        T.Check("doc comments only count when they touch the declaration", V(c, "a").DocComment == null && V(c, "b").DocComment == "about b\nmore" && V(c, "c").DocComment == null);
        c = Ok("signal s # trailing\nsignal t()\nenum E { A, }\nconst K = 1 # c\n");
        T.Check("signal forms, trailing comma in an enum, trailing comments", c.Members.Count == 4 && Member<EnumDecl>(c, "E")!.Values.Count == 1);
        c = Ok("class A extends \"res://b.gd\":\n\tvar x\nvar y\n");
        T.Check("inner class extending a script path", Member<ClassDecl>(c, "A")!.Extends!.ResPath == "res://b.gd" && V(c, "y") != null);
        c = Ok("@export var a := 1\n@export\n@onready\nvar b\n@onready var c = 2\n");
        T.Check("annotations on the same line, or on lines of their own", V(c, "a").Annotations.Count == 1 && V(c, "b").Annotations.Select(a => a.Name).SequenceEqual(new[] { "export", "onready" }) && V(c, "c").HasAnnotation("onready"));
        c = Ok("func f():\n\tprint(\"a\" if x else \"b\")\n\tawait (g())\n\tvar n = -x ** 2\n");
        Eq("assorted expression forms", "(block (call print (?: \"a\" x \"b\")) (await (call g)) (var n = (- (** x 2))))", D(F(c, "f").Body));
    }

    private static void LayoutsAgree()
    {
        foreach (string file in new[] { "player_controller.gd", "ui_menu.gd", "08_lambdas.gd", "06_match.gd", "03_properties.gd" })
        {
            string text = Fx(file).Replace("\r\n", "\n");
            string baseline = D(GdScriptParser.Parse(text).Script);
            string crlf = D(GdScriptParser.Parse(text.Replace("\n", "\r\n")).Script);
            string bom = D(GdScriptParser.Parse("﻿" + text).Script);
            string spaces = D(GdScriptParser.Parse(System.Text.RegularExpressions.Regex.Replace(text, @"(?m)^\t+", m => new string(' ', 4 * m.Length))).Script);
            string noFinalNewline = D(GdScriptParser.Parse(text.TrimEnd('\n')).Script);
            T.Check($"{file}: CRLF", crlf == baseline);
            T.Check($"{file}: BOM", bom == baseline);
            T.Check($"{file}: four-space indentation", spaces == baseline);
            T.Check($"{file}: no newline at the end", noFinalNewline == baseline);
        }
        ParseResult a = GdScriptParser.Parse("var x = 1\r\nvar y = 2\r\n"), b = GdScriptParser.Parse("var x = 1\nvar y = 2\n");
        T.Check("CRLF keeps line and column numbers", D(a.Script) == D(b.Script) && a.Script.Members[1].Start == b.Script.Members[1].Start && a.Script.Members[1].Start == new Pos(2, 1));
    }

    private static void PositionsNest()
    {
        int nodes = 0;
        foreach (string name in Directory.GetFiles(Path.Combine(T.DataDir(), "gdscript"), "*.gd").Where(n => !n.EndsWith("10_errors.gd")).OrderBy(n => n))
        {
            ClassDecl root = GdScriptParser.Parse(File.ReadAllText(name)).Script;
            var bad = new List<string>();
            void Walk(Node n)
            {
                nodes++;
                if (n is not ClassDecl { IsInner: false } && (n.Start.Line == 0 || n.End.Line < n.Start.Line || (n.End.Line == n.Start.Line && n.End.Col < n.Start.Col))) bad.Add($"{n.GetType().Name} {n.Start}-{n.End}");
                foreach (Node ch in n.Children())
                {
                    if (ch is not Annotation && (ch.Start.Line < n.Start.Line || (ch.Start.Line == n.Start.Line && ch.Start.Col < n.Start.Col) ||
                        ch.End.Line > n.End.Line || (ch.End.Line == n.End.Line && ch.End.Col > n.End.Col)))
                        bad.Add($"{ch.GetType().Name} {ch.Start}-{ch.End} not inside {n.GetType().Name} {n.Start}-{n.End}");
                    Walk(ch);
                }
            }
            Walk(root);
            T.Check($"{Path.GetFileName(name)}: ranges nest", bad.Count == 0, string.Join("; ", bad.Take(5)));
        }
        T.Check("the walk saw many nodes", nodes > 1500, nodes.ToString());
        ClassDecl c = GdScriptParser.Parse("class_name A\nvar x = 1\nfunc f():\n\tpass\n").Script;
        T.Check("Children is in source order", c.Children().Select(n => n.Start.Line).SequenceEqual(new[] { 2, 3 }));
    }

    // ---- errors ----

    private static void ErrorRecovery()
    {
        ParseResult r = GdScriptParser.Parse(Fx("10_errors.gd"));
        string errs = string.Join("; ", r.Errors);
        T.Check("six errors, one per bad line", r.Errors.Select(e => e.Line).SequenceEqual(new[] { 4, 9, 10, 14, 20, 26 }), errs);
        T.Check("line:column of the first", r.Errors[0].Line == 4 && r.Errors[0].Col == 14 && r.Errors[0].Message.Contains("expression"), r.Errors[0].ToString());
        T.Check("the second", r.Errors[1].Col == 10, r.Errors[1].ToString());
        T.Check("the missing operand is reported at the end of the line", r.Errors[2].Col == 13 && r.Errors[2].Message.Contains("end of line"), r.Errors[2].ToString());
        T.Check("the empty if condition", r.Errors[3].Col == 5, r.Errors[3].ToString());
        T.Check("the broken parameter list", r.Errors[4].Col == 10 && r.Errors[4].Message.Contains("\")\""), r.Errors[4].ToString());
        T.Check("the stray token", r.Errors[5].Col == 12 && r.Errors[5].Message.Contains("end of statement"), r.Errors[5].ToString());
        ClassDecl c = r.Script;
        T.Check("the good members around the errors are all there", V(c, "ok_before") != null && V(c, "ok_after") != null && Member<SignalDecl>(c, "ok_signal") != null);
        T.Check("the broken members are placeholders", c.Members.OfType<ErrorStmt>().Count() == 2, c.Members.OfType<ErrorStmt>().Count().ToString());
        T.Check("last_good", F(c, "last_good").ReturnType!.Name == "int" && D(F(c, "last_good").Body) == "(block (return 9))");
        Eq("f keeps the statements around its bad lines", "(var a = 1)|(error)|(error)|(return a)", string.Join("|", F(c, "f").Body.Statements.Select(D)));
        Eq("g drops the bad if with its elif, keeps the next statement", "(error)|(call print \"kept\")", string.Join("|", F(c, "g").Body.Statements.Select(D)));
        T.Check("after_all still returns", F(c, "after_all").Body.Statements.OfType<ReturnStmt>().Any());
        T.Check("the second run gives the same errors", string.Join("; ", GdScriptParser.Parse(Fx("10_errors.gd")).Errors) == errs);

        r = GdScriptParser.Parse("func a():\n\tvar x = (1 +\nfunc b():\n\tpass\n");
        T.Check("an unclosed bracket is an error and the file still ends", !r.Ok && r.Script != null);
        r = GdScriptParser.Parse("func a():\n\tmatch x:\n\t\t1 pass\n\t\t2:\n\t\t\tpass\n\tstill_here()\n");
        T.Check("a bad match branch is skipped, the next one and the following statement survive",
                r.Errors.Count == 1 && r.Errors[0].Line == 3 && ((FuncDecl)r.Script.Members[0]).Body.Statements.Count == 2 &&
                ((MatchStmt)((FuncDecl)r.Script.Members[0]).Body.Statements[0]).Branches.Count == 1, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("class A:\n\tvar x = = 1\n\tvar y = 2\nvar z = 3\n");
        T.Check("an error inside an inner class stays inside it", r.Errors.Count == 1 && r.Errors[0].Line == 2 &&
                Member<ClassDecl>(r.Script, "A")!.Members.OfType<VarDecl>().Single().Name == "y" && V(r.Script, "z") != null, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("@export\nfunc f(): pass\n@export\n");
        T.Check("a dangling annotation at the end", r.Errors.Count >= 1 && r.Errors.Any(e => e.Line == 3 || e.Message.Contains("@export")), string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("var x: int: set(v): x = v\nvar y: = 1\nvar z = 3\n");
        T.Check("var with ':' and no type: error, then the next var", r.Errors.Count >= 1 && V(r.Script, "z") != null, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("extends A\nextends B\nclass_name X\nclass_name Y\n");
        T.Check("duplicate extends and class_name", r.Errors.Count == 2 && r.Errors[0].Line == 2 && r.Errors[1].Line == 4, string.Join("; ", r.Errors));
    }

    private static void ErrorKinds()
    {
        ParseResult r = GdScriptParser.Parse("func f():\n\tx = 1\n\t\ty = 2\n\tz = 3\n");
        T.Check("unexpected indent: one error, the next statement parses", r.Errors.Count == 1 && r.Errors[0].Line == 3 && r.Errors[0].Message.Contains("indentation") &&
                ((FuncDecl)r.Script.Members[0]).Body.Statements.Count == 2, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("var a = \"abc\nvar b = 2\n");
        T.Check("unterminated string: reported at the quote, the next line still parses", r.Errors.Any(e => e.Line == 1 && e.Col == 9 && e.Message.Contains("Unterminated")) && V(r.Script, "b") != null, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("var a = [1, 2\nvar b = 2\n");
        T.Check("unclosed bracket carries its position", r.Errors.Any(e => e.Message.Contains("Unclosed") && e.Line == 1 && e.Col == 9), string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("var a = \"\\q\"\n");
        T.Check("bad escape", r.Errors.Count == 1 && r.Errors[0].Message.Contains("escape") && r.Errors[0].Col == 10, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("func f(:\n\tpass\n");
        T.Check("garbage in a signature never throws", r.Errors.Count >= 1);
        r = GdScriptParser.Parse("func f():\npass\n");
        T.Check("missing indentation", r.Errors.Any(e => e.Line == 2 && e.Message.Contains("indented")), string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("func f():\n\tif x\n\t\tpass\n");
        T.Check("missing colon", r.Errors.Count == 1 && r.Errors[0].Line == 2 && r.Errors[0].Message.Contains("\":\""), string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("func f():\n\tvar x = 1 +\n");
        T.Check("expression cut at the end of the file", r.Errors.Count == 1, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("static pass\nvar y = 1\n");
        T.Check("static needs var or func", r.Errors.Count == 1 && V(r.Script, "y") != null, string.Join("; ", r.Errors));
        r = GdScriptParser.Parse("");
        T.Check("empty script", r.Ok && r.Script.Members.Count == 0);
        r = GdScriptParser.Parse("# nothing\n\n## but comments\n");
        T.Check("comments only", r.Ok && r.Script.Members.Count == 0 && r.Comments.Count == 2);
        r = GdScriptParser.Parse("signal s(a, b\n)\nvar x\n");
        T.Check("a signal over two lines", r.Ok && Member<SignalDecl>(r.Script, "s")!.Params.Count == 2, string.Join("; ", r.Errors));
    }

    private static void Damaged()
    {
        var rng = new Random(20261028);
        int runs = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        foreach (string file in Directory.GetFiles(Path.Combine(T.DataDir(), "gdscript"), "*.gd").OrderBy(f => f))
        {
            string text = File.ReadAllText(file);
            for (int i = 0; i < 25; i++)
            {
                string mutated = text;
                int edits = 1 + rng.Next(4);
                for (int k = 0; k < edits && mutated.Length > 10; k++)
                {
                    int at = rng.Next(mutated.Length - 5);
                    switch (rng.Next(4))
                    {
                        case 0: mutated = mutated.Remove(at, 1 + rng.Next(5)); break;
                        case 1: { const string junk = ":([{\"'\\\t\n)]}#@$%"; mutated = mutated.Insert(at, junk[rng.Next(junk.Length)].ToString()); break; }
                        case 2: mutated = mutated.Insert(at, "\n\t"); break;
                        default: mutated = mutated.Substring(0, at); break;
                    }
                }
                try
                {
                    ParseResult r = GdScriptParser.Parse(mutated);
                    if (r.Script == null) throw new Exception("no script");
                    runs++;
                }
                catch (Exception ex)
                {
                    T.Check($"{Path.GetFileName(file)} damaged variant {i} parses without throwing", false, ex.GetType().Name + ": " + ex.Message);
                    return;
                }
            }
        }
        T.Check($"{runs} damaged scripts parsed without an exception", runs >= 250);
        T.Check("and in good time", sw.Elapsed.TotalSeconds < 60, sw.Elapsed.ToString());
    }
}
