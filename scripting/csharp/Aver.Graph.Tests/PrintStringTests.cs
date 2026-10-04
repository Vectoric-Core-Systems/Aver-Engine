// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// PrintString: the node that answers "did control flow reach here, and in what order".
//
// Print and PrintInt cannot answer it. Both need a VALUE wired before they say anything, so proving
// a branch was taken meant inventing a number to route through it -- and both label their line with
// the node's auto-generated id, so the log reads "print3 = 1" and the author works out which node
// that was. Here the author writes the label and nothing but exec is wired.
//
// WHAT THESE TESTS ACTUALLY PROVE, and it is more than "it compiled": PrintStringMethod is resolved
// by reflection in a static field initialiser, so a missing or renamed PrintStringForGraph throws at
// type-init the first time anything is compiled at all. And INVOKING is what proves the emitted IL
// is well-formed -- the failure this node's shape invites is an unbalanced stack, because it is the
// only print with no value pin and therefore the only one that must NOT call EmitPullInput.
using System;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class PrintStringTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += PrintStringCompilesAndRunsWithNothingWiredButExec();
        failures += PrintStringKeepsItsAuthoredText();
        failures += PrintStringWithNoTextIsStillValid();
        failures += PrintStringHasNoValuePin();
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    private const string TickPrints =
        "OCGRAPH 1\n" +
        "ENTRY t OnTick\n" +
        "NODE t OnTick\n" +
        "NODE hello PrintString text=reached_the_tick\n" +
        "LINK t.exec hello.exec\n";

    // THE ONE THAT MATTERS. Compiling proves the reflection target exists; invoking proves the IL
    // this node emits is valid, which is the risk its no-value-pin shape creates.
    private static int PrintStringCompilesAndRunsWithNothingWiredButExec()
    {
        Console.WriteLine("Test: a PrintString reached from OnTick compiles and runs");
        int f = 0;
        if (!OcGraphParser.Parse(TickPrints, out var g, out var err))
            return Expect("parses", false, err ?? "");

        var compiled = new GraphCompiler(g).CompileEntryPoint("OnTick", out err);
        if (compiled == null) return Expect("compiles", false, err ?? "");

        try
        {
            compiled.DynamicInvoke();
            f += Expect("and RUNS -- the emitted IL is well-formed with no value pin pulled", true);
        }
        catch (Exception ex)
        {
            f += Expect("and runs", false, ex.InnerException?.Message ?? ex.Message);
        }
        return f;
    }

    private static int PrintStringKeepsItsAuthoredText()
    {
        Console.WriteLine("Test: text= survives the parse onto the node");
        if (!OcGraphParser.Parse(TickPrints, out var g, out var err))
            return Expect("parses", false, err ?? "");
        return Expect("the node carries its authored message",
                      g.Nodes["hello"].PrintText == "reached_the_tick",
                      g.Nodes["hello"].PrintText ?? "null");
    }

    // A node dropped from the palette carries no text= until the author types one, and it must not
    // refuse to compile in the meantime -- an editor whose fresh node breaks the graph is unusable.
    private static int PrintStringWithNoTextIsStillValid()
    {
        Console.WriteLine("Test: a PrintString with no text= yet still compiles and runs");
        const string text =
            "OCGRAPH 1\n" +
            "ENTRY t OnTick\n" +
            "NODE t OnTick\n" +
            "NODE hello PrintString\n" +
            "LINK t.exec hello.exec\n";
        int f = 0;
        if (!OcGraphParser.Parse(text, out var g, out var err))
            return Expect("parses", false, err ?? "");
        f += Expect("PrintText is null, not empty-string-by-accident", g.Nodes["hello"].PrintText is null);
        var compiled = new GraphCompiler(g).CompileEntryPoint("OnTick", out err);
        if (compiled == null) return f + Expect("compiles", false, err ?? "");
        try { compiled.DynamicInvoke(); f += Expect("and runs", true); }
        catch (Exception ex) { f += Expect("and runs", false, ex.InnerException?.Message ?? ex.Message); }
        return f;
    }

    // EXEC IN, EXEC OUT, AND NOTHING ELSE -- checked because a value pin appearing here would
    // reintroduce exactly the thing that made Print unusable for "did this run", and because the
    // parser's default pins and the editor's catalog row have to agree with each other pin for pin.
    private static int PrintStringHasNoValuePin()
    {
        Console.WriteLine("Test: PrintString declares exec in, exec out, and no value pin");
        if (!OcGraphParser.Parse(TickPrints, out var g, out var err))
            return Expect("parses", false, err ?? "");
        var pins = g.Nodes["hello"].Pins;
        int f = Expect("exactly two pins", pins.Count == 2, $"got {pins.Count}");
        f += Expect("an exec input and an exec output, both Exec-typed",
                    pins.Exists(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) &&
                    pins.Exists(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec));
        f += Expect("and no 'value' pin at all", !pins.Exists(p => p.Name == "value"));
        return f;
    }
}
