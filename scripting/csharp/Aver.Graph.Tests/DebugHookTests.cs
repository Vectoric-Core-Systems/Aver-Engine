// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The graph debugger's managed half (GraphDebug + the compiler's two hooks). The design rule these tests
// hold it to: ARMED IS READ AT COMPILE TIME. A graph compiled unarmed must be byte-for-byte the graph it
// was before the debugger existed, so the proof is behavioural -- run it with breakpoints set and values
// to record, and see that neither hook ever fired. An armed graph must still be valid IL (invoking it
// JIT-compiles it), latch a breakpoint exactly once, and record float, int and bool values on wires.
using System;
using System.IO;
using System.Linq;
using Aver.Graph;

[AutoRun]
static class DebugHookTests
{
    private const string Sep = "\x1f";

    public static int RunAll()
    {
        int failures = 0;
        bool before = GraphDebug.Armed;
        try
        {
            failures += UnarmedCompileNeverTouchesTheHooks();
            failures += ArmedBreakpointLatchesOnceAndIsTakenOnce();
            failures += BreakpointOnAnotherNodeStaysQuiet();
            failures += FloatIntAndBoolValuesAreCaptured();
            failures += DisarmingForgetsValues();
            failures += EveryFixtureCompilesArmedToValidIl();
            failures += BreakpointSetParsing();
        }
        finally
        {
            GraphDebug.Armed = before;
            GraphDebug.SetBreakpoints("");
            GraphDebug.Reset();
        }
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    // delta added to `score` each tick; named so the breakpoint key is predictable.
    private const string Accumulate = @"OCGRAPH 1
NAME DebugT
VAR score float 0
PARAM delta float
NODE tick OnTick
NODE pd Param param=delta
NODE gv GetVar var=score
NODE add Add
NODE sv SetVar var=score
LINK tick.exec sv.exec
LINK gv.value add.a
LINK pd.value add.b
LINK add.result sv.value
ENTRY tick OnTick
";

    // Compiles Accumulate's OnTick under the CURRENT armed state and runs it once with delta = 5.
    private static float RunAccumulateOnce(out string? err)
    {
        err = null;
        if (!OcGraphParser.Parse(Accumulate, out var graph, out err)) return float.NaN;
        var fn = new GraphCompiler(graph).CompileEntryPoint("OnTick", out err);
        if (fn == null) return float.NaN;
        var store = GraphVarStore.CreateFor(graph);
        fn.DynamicInvoke(5f, store);   // (delta, the graph's variable store): no OUT, so no return value
        return store.GetFloat("score");
    }

    private static int RunOnce(string text, string entry, out string? err)
    {
        err = null;
        if (!OcGraphParser.Parse(text, out var graph, out err)) return -1;
        var fn = new GraphCompiler(graph).CompileEntryPoint(entry, out err);
        if (fn == null) return -1;
        try { fn.DynamicInvoke(ArgsFor(graph)); }
        catch (System.Reflection.TargetInvocationException e) when (e.InnerException is not InvalidProgramException) { }
        return 0;
    }

    // What an entry point is called with: one value per PARAM, then the variable store when there are VARs.
    private static object[] ArgsFor(Aver.Graph.Graph graph)
    {
        var args = new System.Collections.Generic.List<object>();
        foreach (var p in graph.Parameters)
            args.Add(p.Type == PinType.Float ? (object)1f : p.Type == PinType.Int ? (object)1 : (object)false);
        if (graph.Variables.Count > 0) args.Add(GraphVarStore.CreateFor(graph));
        return args.ToArray();
    }

    // ------------------------------------------------------------------ unarmed

    private static int UnarmedCompileNeverTouchesTheHooks()
    {
        Console.WriteLine("Test: a graph compiled UNARMED never reaches the breakpoint or value hooks");
        GraphDebug.Armed = false;
        GraphDebug.SetBreakpoints("DebugT" + Sep + "sv\nDebugT" + Sep + "tick");
        GraphDebug.Reset();
        float score = RunAccumulateOnce(out var err);
        int f = Expect("the graph still runs and accumulates", Math.Abs(score - 5f) < 1e-6f, err ?? score.ToString());
        f += Expect("no breakpoint latched, though both nodes ran", GraphDebug.TakeBreak().Length == 0);
        f += Expect("no values recorded", GraphDebug.GetValues("DebugT").Length == 0);
        return f;
    }

    // ------------------------------------------------------------------ armed

    private static int ArmedBreakpointLatchesOnceAndIsTakenOnce()
    {
        Console.WriteLine("Test: armed, a set breakpoint latches when its node runs and TakeBreak returns it once");
        GraphDebug.Armed = true;
        GraphDebug.SetBreakpoints("DebugT" + Sep + "sv");
        GraphDebug.Reset();
        float score = RunAccumulateOnce(out var err);
        int f = Expect("the armed graph is valid IL and runs the same", Math.Abs(score - 5f) < 1e-6f, err ?? score.ToString());
        string first = GraphDebug.TakeBreak();
        f += Expect("the break is graph, separator, node", first == "DebugT" + Sep + "sv", first.Replace(Sep, "<US>"));
        f += Expect("and a second take is empty", GraphDebug.TakeBreak().Length == 0);

        // Two trips before anyone takes: still one latch, the first.
        GraphDebug.SetBreakpoints("DebugT" + Sep + "tick\nDebugT" + Sep + "sv");
        RunAccumulateOnce(out _);
        string latched = GraphDebug.TakeBreak();
        f += Expect("several breakpoints in one frame latch the first node reached", latched == "DebugT" + Sep + "tick", latched.Replace(Sep, "<US>"));
        f += Expect("and the rest of that frame left nothing behind", GraphDebug.TakeBreak().Length == 0);
        return f;
    }

    private static int BreakpointOnAnotherNodeStaysQuiet()
    {
        Console.WriteLine("Test: a breakpoint on a node that did not run, or in another graph, never latches");
        GraphDebug.Armed = true;
        GraphDebug.SetBreakpoints("DebugT" + Sep + "nowhere\nOtherGraph" + Sep + "sv");
        GraphDebug.Reset();
        RunAccumulateOnce(out _);
        return Expect("nothing latched", GraphDebug.TakeBreak().Length == 0);
    }

    private static int FloatIntAndBoolValuesAreCaptured()
    {
        Console.WriteLine("Test: float, int and bool values on wires are captured and read back");
        GraphDebug.Armed = true;
        GraphDebug.SetBreakpoints("");
        GraphDebug.Reset();
        RunAccumulateOnce(out _);
        string v = GraphDebug.GetValues("DebugT");
        int f = Expect("the float sum reached SetVar's value pin (0 + 5)", v.Contains("sv.value=5;"), v);
        f += Expect("and Add's two inputs were seen", v.Contains("add.a=0;") && v.Contains("add.b=5;"), v);

        // int on a wire, bool on a wire
        const string mixed = @"OCGRAPH 1
NAME DebugMixed
VAR n int 0
NODE tick OnTick
NODE k ConstInt value=7
NODE sv SetVar var=n
NODE cb ConstBool value=true
NODE br Branch
LINK tick.exec sv.exec
LINK k.value sv.value
LINK sv.then br.exec
LINK cb.value br.cond
ENTRY tick OnTick
";
        GraphDebug.Reset();
        RunOnce(mixed, "OnTick", out var err);
        string m = GraphDebug.GetValues("DebugMixed");
        f += Expect("an int wire is recorded as 7", m.Contains("sv.value=7;"), err ?? m);
        f += Expect("a bool wire is recorded as 1", m.Contains("br.cond=1;"), err ?? m);
        f += Expect("a graph that never ran has no values", GraphDebug.GetValues("NeverRan").Length == 0);
        return f;
    }

    private static int DisarmingForgetsValues()
    {
        Console.WriteLine("Test: disarming forgets what was recorded");
        GraphDebug.Armed = true;
        GraphDebug.Reset();
        RunAccumulateOnce(out _);
        int f = Expect("values exist while armed", GraphDebug.GetValues("DebugT").Length > 0);
        GraphDebug.Armed = false;
        f += Expect("and are gone after disarming", GraphDebug.GetValues("DebugT").Length == 0);
        return f;
    }

    // ------------------------------------------------------------------ every fixture

    private static int EveryFixtureCompilesArmedToValidIl()
    {
        Console.WriteLine("Test: every fixture graph compiled armed is valid IL for each of its entries");
        GraphDebug.Armed = true;
        GraphDebug.SetBreakpoints("");
        int bad = 0, graphs = 0;
        foreach (string file in Directory.EnumerateFiles(".", "*.ocgraph").OrderBy(x => x, StringComparer.Ordinal))
        {
            string text = File.ReadAllText(file);
            if (!OcGraphParser.Parse(text, out var graph, out _)) continue;   // some fixtures are negative cases
            if (graph.DomainKind != GraphDomain.Gameplay) continue;
            ++graphs;
            foreach (string entry in graph.EntryPoints.Select(e => e.EventName).Distinct())
            {
                try
                {
                    var fn = new GraphCompiler(graph).CompileEntryPoint(entry, out var err);
                    if (fn == null) { if (err != null && err.Contains("InvalidProgram")) { Console.WriteLine($"    {file} {entry}: {err}"); ++bad; } continue; }
                    // Invoking JIT-compiles the emitted IL, so an invalid program throws before any native call;
                    // anything else (a missing native DLL, a null host) means the IL itself was fine.
                    try { fn.DynamicInvoke(ArgsFor(graph)); }
                    catch (System.Reflection.TargetInvocationException e) when (e.InnerException is InvalidProgramException)
                    { Console.WriteLine($"    {file} {entry}: invalid program"); ++bad; }
                    catch (Exception) { }
                }
                catch (Exception e)
                {
                    if (e is InvalidProgramException) { Console.WriteLine($"    {file} {entry}: invalid program"); ++bad; }
                }
            }
        }
        GraphDebug.Armed = false;
        if (bad > 0) return Expect($"{graphs} fixture graphs", false, $"{bad} entries produced invalid IL");
        return Expect($"{graphs} fixture graphs, no invalid IL", graphs > 0);
    }

    private static int BreakpointSetParsing()
    {
        Console.WriteLine("Test: the breakpoint set takes graph<US>node lines and skips junk");
        GraphDebug.Armed = true;
        GraphDebug.SetBreakpoints("DebugT" + Sep + "sv\r\n\r\nno separator\n" + Sep + "x\nG" + Sep + "\n");
        GraphDebug.Reset();
        RunAccumulateOnce(out _);
        int f = Expect("the one good line (with a CRLF ending) works", GraphDebug.TakeBreak() == "DebugT" + Sep + "sv");
        GraphDebug.SetBreakpoints(null);
        RunAccumulateOnce(out _);
        f += Expect("a null set clears them all", GraphDebug.TakeBreak().Length == 0);
        GraphDebug.Armed = false;
        return f;
    }
}
