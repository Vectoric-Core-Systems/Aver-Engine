// Reroute (knot) nodes and macros. Both are removed by GraphLowering before either compiler runs, so every test
// here parses a graph, compiles it and INVOKES it: a rewrite that dropped or crossed a link shows up as a wrong number.
using System;
using Aver.Graph;

static class RerouteMacroTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += Value("a data reroute passes its value through", @"
OCGRAPH 1
NODE k ConstFloat value=3
NODE r Reroute
PIN r in in float
PIN r out out float
NODE add Add
PIN add a in float 0
PIN add b in float 4
PIN add result out float
LINK k value r in
LINK r out add a
OUT add result
", 7.0f);
        failures += Value("a chain of reroutes, one feeding two consumers", @"
OCGRAPH 1
NODE k ConstFloat value=3
NODE r1 Reroute
PIN r1 in in float
PIN r1 out out float
NODE r2 Reroute
PIN r2 in in float
PIN r2 out out float
NODE r3 Reroute
PIN r3 in in float
PIN r3 out out float
NODE add Add
PIN add a in float 0
PIN add b in float 0
PIN add result out float
LINK k value r1 in
LINK r1 out r2 in
LINK r2 out r3 in
LINK r2 out add a
LINK r3 out add b
OUT add result
", 6.0f);
        failures += Value("an orphan reroute drops its links and the input keeps its own value", @"
OCGRAPH 1
NODE r Reroute
PIN r in in float
PIN r out out float
NODE add Add
PIN add a in float 0
PIN add b in float 4
PIN add result out float
LINK r out add a
OUT add result
", 4.0f);
        failures += Value("an orphan reroute's typed-in value reaches what it feeds", @"
OCGRAPH 1
NODE r Reroute
PIN r in in float 10
PIN r out out float
NODE add Add
PIN add a in float 0
PIN add b in float 4
PIN add result out float
LINK r out add a
OUT add result
", 14.0f);
        failures += Value("an int[] reroute keeps its type through to an array input", @"
OCGRAPH 1
NODE mk MakeArray of=int
PINVAL mk item0 1
PINVAL mk item1 2
NODE r Reroute
PIN r in in int[]
PIN r out out int[]
NODE ln ArrayLength of=int
LINK mk array r in
LINK r out ln array
OUT ln length
", 2);
        failures += EntryValue("an exec reroute carries control flow", @"
OCGRAPH 1
VAR n float
NODE s OnTick
NODE r Reroute
PIN r in in exec
PIN r out out exec
NODE sv SetVar var=n
PINVAL sv value 5
NODE gv GetVar var=n
LINK s exec r in
LINK r out sv exec
OUT gv value
ENTRY s OnTick
", 5.0f);
        failures += Refused("a reroute whose pin types disagree with the link is refused", @"
OCGRAPH 1
NODE k ConstFloat value=3
NODE r Reroute
PIN r in in bool
PIN r out out bool
LINK k value r in
", "mismatch");

        failures += EntryValue("a macro with two exec outputs, used twice in one graph", PickGraph, 3.0f);
        failures += Value("a pure-data macro with two outputs, used twice", @"
OCGRAPH 1
MACRO Calc
MACROIN Calc a float
MACROIN Calc b float
MACROOUT Calc sum float
MACROOUT Calc prod float
NODE mi MacroInput macro=Calc
NODE ad Add macro=Calc
NODE mu Multiply macro=Calc
NODE mo MacroOutput macro=Calc
LINK mi a ad a
LINK mi b ad b
LINK mi a mu a
LINK mi b mu b
LINK ad result mo sum
LINK mu result mo prod
NODE k1 ConstFloat value=3
NODE k2 ConstFloat value=4
NODE c1 MacroInstance call=Calc
NODE c2 MacroInstance call=Calc
PINVAL c2 b 10
LINK k1 value c1 a
LINK k2 value c1 b
LINK c1 sum c2 a
OUT c1 sum
OUT c1 prod
OUT c2 sum
", new object[] { 7.0f, 12.0f, 17.0f });
        failures += Value("a macro body keeps its own constants, per instance", @"
OCGRAPH 1
MACRO AddTen
MACROIN AddTen x float
MACROOUT AddTen r float
NODE mi MacroInput macro=AddTen
NODE ten ConstFloat value=10 macro=AddTen
NODE ad Add macro=AddTen
NODE mo MacroOutput macro=AddTen
LINK mi x ad a
LINK ten value ad b
LINK ad result mo r
NODE k ConstFloat value=3
NODE m1 MacroInstance call=AddTen
NODE m2 MacroInstance call=AddTen
LINK k value m1 x
LINK m1 r m2 x
OUT m2 r
", 23.0f);
        failures += Value("a macro wired straight from input to output, and one that uses another macro", @"
OCGRAPH 1
MACRO Same
MACROIN Same x float
MACROOUT Same y float
NODE si MacroInput macro=Same
NODE so MacroOutput macro=Same
LINK si x so y
MACRO Twice
MACROIN Twice x float
MACROOUT Twice y float
NODE ti MacroInput macro=Twice
NODE a1 MacroInstance call=Same macro=Twice
NODE a2 MacroInstance call=Same macro=Twice
NODE ad Add macro=Twice
NODE to MacroOutput macro=Twice
LINK ti x a1 x
LINK ti x a2 x
LINK a1 y ad a
LINK a2 y ad b
LINK ad result to y
NODE k ConstFloat value=4
NODE t MacroInstance call=Twice
LINK k value t x
OUT t y
", 8.0f);
        failures += Expect("compiling leaves the parsed graph untouched", () =>
        {
            if (!OcGraphParser.Parse(PickGraph, out var g, out var err)) return "parse: " + err;
            var fn = new GraphCompiler(g).CompileEntryPoint("OnTick", out var cerr);
            if (fn == null) return "compile: " + cerr;
            return g.Nodes.ContainsKey("i1") && g.Nodes.ContainsKey("br") && g.Links.Count == 9 ? null : "graph was rewritten in place";
        });

        failures += Refused("a macro containing itself is refused", @"
OCGRAPH 1
MACRO Loop
MACROIN Loop x float
MACROOUT Loop y float
NODE li MacroInput macro=Loop
NODE lo MacroOutput macro=Loop
NODE me MacroInstance call=Loop macro=Loop
LINK li x me x
LINK me y lo y
", "contains itself");
        failures += Refused("two macros containing each other are refused", @"
OCGRAPH 1
MACRO A
MACROIN A x float
MACROOUT A y float
NODE ai MacroInput macro=A
NODE ab MacroInstance call=B macro=A
MACRO B
MACROIN B x float
MACROOUT B y float
NODE bi MacroInput macro=B
NODE ba MacroInstance call=A macro=B
", "contains itself");
        failures += Refused("an instance of an undeclared macro is refused", @"
OCGRAPH 1
NODE m MacroInstance call=Nope
", "Nope");
        failures += Refused("a wire leaving a macro body is refused", @"
OCGRAPH 1
MACRO M
MACROIN M x float
NODE mi MacroInput macro=M
NODE inner Add macro=M
NODE k ConstFloat value=1
LINK k value inner a
", "macro");

        return failures;
    }

    // OnTick -> i1(cond=true): yes -> i2(cond=false): no -> n=3; i2.yes -> n=2; i1.no -> n=1. Two instances of one macro, two exec outputs each.
    private const string PickGraph = @"
OCGRAPH 1
VAR n float
MACRO Pick
MACROIN Pick go exec
MACROIN Pick cond bool
MACROOUT Pick yes exec
MACROOUT Pick no exec
NODE mi MacroInput macro=Pick
NODE br Branch macro=Pick
NODE mo MacroOutput macro=Pick
LINK mi go br exec
LINK mi cond br cond
LINK br true mo yes
LINK br false mo no
NODE s OnTick
NODE i1 MacroInstance call=Pick
PINVAL i1 cond true
NODE i2 MacroInstance call=Pick
PINVAL i2 cond false
NODE a SetVar var=n
PINVAL a value 1
NODE b SetVar var=n
PINVAL b value 2
NODE c SetVar var=n
PINVAL c value 3
NODE gv GetVar var=n
LINK s exec i1 go
LINK i1 yes i2 go
LINK i1 no a exec
LINK i2 yes b exec
LINK i2 no c exec
OUT gv value
ENTRY s OnTick
";

    private static bool Same(object? got, object want)
    {
        if (want is object[] ws)
        {
            if (got is not object[] gs || gs.Length != ws.Length) return false;
            for (int i = 0; i < ws.Length; i++) if (!Same(gs[i], ws[i])) return false;
            return true;
        }
        return want is float f ? got is float g && Math.Abs(f - g) < 1e-4f : Equals(got, want);
    }

    private static string Show(object? o) =>
        o is object[] a ? "[" + string.Join(", ", Array.ConvertAll(a, Show)) + "]" : o?.ToString() ?? "null";

    private static int Expect(string what, Func<string?> check)
    {
        Console.WriteLine($"Test: {what}");
        var why = check();
        if (why != null) { Console.WriteLine($"  FAIL: {why}"); return 1; }
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int Value(string what, string text, object want)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        object? got;
        try { got = fn.DynamicInvoke(g.Variables.Count > 0 ? new object[] { GraphVarStore.CreateFor(g) } : null); }
        catch (Exception e) { Console.WriteLine($"  FAIL: invoke: {e.InnerException?.Message ?? e.Message}"); return 1; }
        if (!Same(got, want)) { Console.WriteLine($"  FAIL: expected {Show(want)}, got {Show(got)}"); return 1; }
        Console.WriteLine($"  PASS: {Show(got)}");
        return 0;
    }

    private static int EntryValue(string what, string text, object want)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).CompileEntryPoint("OnTick", out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        object? got;
        try { got = fn.DynamicInvoke(GraphVarStore.CreateFor(g)); }
        catch (Exception e) { Console.WriteLine($"  FAIL: invoke: {e.InnerException?.Message ?? e.Message}"); return 1; }
        if (!Same(got, want)) { Console.WriteLine($"  FAIL: expected {Show(want)}, got {Show(got)}"); return 1; }
        Console.WriteLine($"  PASS: {Show(got)}");
        return 0;
    }

    private static int Refused(string what, string text, string mention)
    {
        Console.WriteLine($"Test: {what}");
        if (OcGraphParser.Parse(text, out _, out var err)) { Console.WriteLine("  FAIL: parsed without complaint"); return 1; }
        if (err == null || !err.Contains(mention, StringComparison.OrdinalIgnoreCase))
        { Console.WriteLine($"  FAIL: refused, but the message does not mention '{mention}': {err}"); return 1; }
        Console.WriteLine($"  PASS: refused: {err}");
        return 0;
    }
}
