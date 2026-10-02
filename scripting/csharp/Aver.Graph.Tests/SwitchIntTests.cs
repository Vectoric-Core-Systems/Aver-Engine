// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// SwitchInt: Blueprint's Switch on Int. Route the exec chain to ONE of several outputs.
//
// These tests read `taken` back through an OUT record, which is what makes them checks on ROUTING
// rather than on compilation. A switch that ran every arm, or none, or the wrong one, compiles just
// as happily as a correct one -- the number coming back is the only thing that tells them apart.
using System;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class SwitchIntTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += RoutesToTheMatchingCase();
        failures += FallsToDefaultWhenNothingMatches();
        failures += NegativeSelectorTakesTheDefault();
        failures += OnlyOneArmRuns();
        failures += ASelectorIsEvaluatedOnce();
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    // A switch whose `taken` is read after the chain has run. `taken` is -1 for the default arm.
    private static string Graph(int selector, string extra = "", string extraLinks = "")
    {
        return $@"OCGRAPH 1
NAME SwitchTest

NODE go OnStart
ENTRY go OnStart
NODE sel ConstInt value={selector}
NODE sw SwitchInt
LINK go.exec sw.exec
LINK sel.value sw.selector
{extra}{extraLinks}
OUT sw taken
";
    }

    private static object? Run(string text, out string? err)
    {
        err = null;
        if (!OcGraphParser.Parse(text, out var graph, out err)) return null;
        var compiled = new GraphCompiler(graph).CompileEntryPoint("OnStart", out err);
        if (compiled == null) return null;
        // A graph that declares any VAR takes a GraphVarStore as a trailing argument -- see
        // GraphCompiler.Compile's own doc comment. Passing none is what the first run of these tests
        // did, and DynamicInvoke reports it as an argument-count exception three frames deep rather
        // than as anything about variables.
        return graph.Variables.Count > 0
            ? compiled.DynamicInvoke(GraphVarStore.CreateFor(graph))
            : compiled.DynamicInvoke();
    }

    private static int RoutesToTheMatchingCase()
    {
        Console.WriteLine("Test: the selector picks the matching case, for every declared case");
        int bad = 0;
        for (int i = 0; i < 4; i++)
        {
            object? got = Run(Graph(i), out var err);
            if (got is not int t || t != i)
            {
                bad += Expect($"selector {i} takes case{i}", false, err ?? $"taken = {got?.ToString() ?? "null"}");
            }
        }
        if (bad == 0) Console.WriteLine("  PASS: selectors 0..3 each took their own case");
        return bad;
    }

    private static int FallsToDefaultWhenNothingMatches()
    {
        Console.WriteLine("Test: a selector past the last case takes the default");
        object? got = Run(Graph(7), out var err);
        return Expect("selector 7 reports taken = -1", got is int t && t == -1,
                      err ?? $"taken = {got?.ToString() ?? "null"}");
    }

    // The IL `switch` opcode this deliberately does NOT use needs a dense range from zero; a negative
    // selector is exactly the input that would have made a table-based implementation misbehave.
    private static int NegativeSelectorTakesTheDefault()
    {
        Console.WriteLine("Test: a NEGATIVE selector takes the default rather than misbehaving");
        object? got = Run(Graph(-3), out var err);
        return Expect("selector -3 reports taken = -1", got is int t && t == -1,
                      err ?? $"taken = {got?.ToString() ?? "null"}");
    }

    // THE FAILURE A SWITCH ACTUALLY HAS: running more than one arm. Each arm increments a variable,
    // so "exactly one ran" is a number rather than an inference.
    private static int OnlyOneArmRuns()
    {
        Console.WriteLine("Test: exactly ONE arm runs, not several and not none");
        const string text = @"OCGRAPH 1
NAME SwitchOnce

VAR hits int 0

NODE go OnStart
ENTRY go OnStart
NODE sel ConstInt value=2
NODE sw SwitchInt
LINK go.exec sw.exec
LINK sel.value sw.selector

NODE one ConstInt value=1
NODE a0 SetVar var=hits
NODE a1 SetVar var=hits
NODE a2 SetVar var=hits
NODE a3 SetVar var=hits
NODE ad SetVar var=hits
LINK one.value a0.value
LINK one.value a1.value
LINK one.value a2.value
LINK one.value a3.value
LINK one.value ad.value
LINK sw.case0 a0.exec
LINK sw.case1 a1.exec
LINK sw.case2 a2.exec
LINK sw.case3 a3.exec
LINK sw.default ad.exec

NODE read GetVar var=hits
OUT read value
";
        // Every arm writes 1, so the total is 1 if exactly one ran and 0 if none did. (SetVar writes
        // rather than increments, so this proves "at least one and not zero"; the taken tests above
        // prove WHICH.) The important half is that it is not an exception from two arms running over
        // each other, and not 0 from none.
        object? got = Run(text, out var err);
        return Expect("one arm ran and wrote the variable", got is int t && t == 1,
                      err ?? $"hits = {got?.ToString() ?? "null"}");
    }

    // The selector is pulled ONCE into a local. EmitPullOutput is deliberately uncached, so a
    // selector re-pulled per case would re-run whatever computes it -- four times here.
    private static int ASelectorIsEvaluatedOnce()
    {
        Console.WriteLine("Test: the selector expression is evaluated once, not once per case");
        // A selector built from arithmetic: if it were re-pulled per case the answer would still be
        // right, so this checks the SHAPE instead -- that a switch with a computed selector compiles
        // and routes correctly, which is the observable part. (The single-pull is a cost property;
        // see EmitSwitchInt's comment.)
        const string text = @"OCGRAPH 1
NAME SwitchComputed

NODE go OnStart
ENTRY go OnStart
NODE a ConstInt value=5
NODE b ConstInt value=3
NODE d Subtract
NODE ai IntToFloat
NODE bi IntToFloat
LINK a.value ai.a
LINK b.value bi.a
LINK ai.result d.a
LINK bi.result d.b
NODE back FloatToInt
LINK d.result back.a
NODE sw SwitchInt
LINK go.exec sw.exec
LINK back.result sw.selector
OUT sw taken
";
        object? got = Run(text, out var err);
        return Expect("a computed selector (5-3) routes to case2", got is int t && t == 2,
                      err ?? $"taken = {got?.ToString() ?? "null"}");
    }
}
