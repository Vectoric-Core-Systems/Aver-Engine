// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Collapse to Function, proved across the language boundary. tests/graphtools' GraphCollapseTest takes
// collapse_test.ocgraph, collapses five of its nodes into a function through the C++ editor's real
// collapseSelectionToFunction()/save() path, and writes the result to collapse_test.output.ocgraph beside
// this file. This reads THAT file (not a hand-typed copy) and shows the promise the feature makes: the
// graph does what it did. Both graphs are compiled and run for two ticks, and their variables must be
// the same numbers -- not merely "the output parses".
//
// RUN GraphCollapseTest.exe BEFORE this suite when the fixture or the collapse rules change; the checked-in
// output is whatever that test last wrote.
using System;
using System.IO;
using System.Linq;
using Aver.Graph;

[AutoRun]
static class CollapseFunctionTests
{
    private const string Original = "collapse_test.ocgraph";
    private const string Collapsed = "collapse_test.output.ocgraph";

    public static int RunAll()
    {
        int failures = 0;
        failures += FilesArePresent();
        failures += TheOutputDeclaresTheFunctionAndCallsIt();
        failures += BothGraphsComputeTheSameVariables();
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    private static int FilesArePresent()
    {
        Console.WriteLine("Test: the collapse fixture and the C++ editor's output are present (run GraphCollapseTest.exe first)");
        int f = 0;
        foreach (string file in new[] { Original, Collapsed })
            f += Expect($"'{file}' is in {Directory.GetCurrentDirectory()}", File.Exists(file),
                        "Run build/bin/GraphCollapseTest.exe: it writes the output from a real collapse.");
        return f;
    }

    private static int TheOutputDeclaresTheFunctionAndCallsIt()
    {
        Console.WriteLine("Test: the collapsed graph parses, declares one impure function and calls it");
        if (!File.Exists(Collapsed)) return Expect("output file present", false);
        if (!OcGraphParser.Parse(File.ReadAllText(Collapsed), out var g, out var err))
            return Expect("the collapsed graph parses and validates", false, err ?? "");

        int f = Expect("one function", g.Functions.Count == 1, g.Functions.Count.ToString());
        if (g.Functions.Count != 1) return f;
        var fn = g.Functions[0];
        f += Expect("it is not pure (it carries the exec chain)", !fn.IsPure);
        f += Expect("one input (the value wired in from outside) and one output (the value read after)",
                    fn.Inputs.Count == 1 && fn.Outputs.Count == 1, $"{fn.Inputs.Count} in, {fn.Outputs.Count} out");
        f += Expect("it has its entry and its return", fn.EntryNodeId != null && fn.ReturnNodeId != null);

        var call = g.Nodes.Values.FirstOrDefault(n => n.Type.Equals("callfunc", StringComparison.OrdinalIgnoreCase));
        f += Expect("a CallFunc node calls it", call != null && call.CallTarget == fn.Name);
        var inside = g.Nodes.Values.Where(n => n.FuncOwner == fn.Name &&
                                               !n.Type.Equals("funcentry", StringComparison.OrdinalIgnoreCase) &&
                                               !n.Type.Equals("funcreturn", StringComparison.OrdinalIgnoreCase)).Select(n => n.Id)
                                   .OrderBy(x => x, StringComparer.Ordinal).ToArray();
        f += Expect("the five selected nodes moved inside", inside.SequenceEqual(new[] { "addn", "c2", "c3", "mul", "st" }),
                    string.Join(",", inside));
        f += Expect("the nodes left behind stayed in the event graph",
                    new[] { "tick", "gv", "c10", "add2", "sl" }.All(id => g.Nodes[id].FuncOwner == null));
        return f;
    }

    // Runs OnTick `ticks` times against one variable store, returning the two variables.
    private static (float Total, float Last)? Run(string file, int ticks, out string? err)
    {
        err = null;
        if (!OcGraphParser.Parse(File.ReadAllText(file), out var g, out err)) return null;
        var fn = new GraphCompiler(g).CompileEntryPoint("OnTick", out err);
        if (fn == null) return null;
        var store = GraphVarStore.CreateFor(g);
        try
        {
            for (int i = 0; i < ticks; i++) fn.DynamicInvoke(store);   // no PARAMs: the store is the only argument
        }
        catch (System.Reflection.TargetInvocationException e)
        {
            err = e.InnerException?.Message ?? e.Message;
            return null;
        }
        return (store.GetFloat("total"), store.GetFloat("last"));
    }

    private static int BothGraphsComputeTheSameVariables()
    {
        Console.WriteLine("Test: the original and the collapsed graph leave the same variables after two ticks");
        if (!File.Exists(Original) || !File.Exists(Collapsed)) return Expect("both files present", false);

        var before = Run(Original, 2, out var e1);
        var after = Run(Collapsed, 2, out var e2);
        int f = Expect("the original runs", before.HasValue, e1 ?? "");
        f += Expect("the collapsed graph runs", after.HasValue, e2 ?? "");
        if (!before.HasValue || !after.HasValue) return f;

        // tick 1: total = 2*3 + last(0) = 6, last = 6 + 10 = 16.  tick 2: total = 6 + 16 = 22, last = 22 + 10 = 32.
        f += Expect("the original gives total 22 and last 32 (hand calculation)",
                    Math.Abs(before.Value.Total - 22f) < 1e-4f && Math.Abs(before.Value.Last - 32f) < 1e-4f,
                    $"total {before.Value.Total}, last {before.Value.Last}");
        f += Expect("the collapsed graph gives the same total", Math.Abs(before.Value.Total - after.Value.Total) < 1e-4f,
                    $"{before.Value.Total} vs {after.Value.Total}");
        f += Expect("and the same last", Math.Abs(before.Value.Last - after.Value.Last) < 1e-4f,
                    $"{before.Value.Last} vs {after.Value.Last}");

        // One tick as well: a state-dependent difference (the function reading `last` at call time) would show here first.
        var b1 = Run(Original, 1, out _);
        var a1 = Run(Collapsed, 1, out _);
        f += Expect("after one tick they agree too (6 and 16)",
                    b1.HasValue && a1.HasValue && Math.Abs(b1.Value.Total - a1.Value.Total) < 1e-4f &&
                    Math.Abs(b1.Value.Last - a1.Value.Last) < 1e-4f && Math.Abs(a1.Value.Last - 16f) < 1e-4f);
        return f;
    }
}
