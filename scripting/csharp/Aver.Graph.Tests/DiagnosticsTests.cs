// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// GraphDiagnostics: everything wrong with a graph, not only the first thing (the editor's Problems
// panel, docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md E1). The property that matters most is that
// the LIST agrees with the old single answer: for every graph Graph.Validate refused, the first
// diagnostic is that same sentence. The rest prove the checks only a list can afford, and that a
// compiler refusal arrives as an entry that names its node.
using System;
using System.Collections.Generic;
using System.Linq;
using Aver.Graph;

[AutoRun]
static class DiagnosticsTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += ThreeIndependentFaultsAreThreeDiagnostics();
        failures += FirstDiagnosticIsValidatesMessage();
        failures += ParseFailureIsOneParseError();
        failures += MissingRequiredAttributeIsReported();
        failures += UnreachableAndUnusedAreWarnings();
        failures += CompilerRefusalMapsToItsNode();
        failures += CleanGraphHasNoDiagnostics();
        failures += WireFormatAndCut();
        failures += ValidateStillReturnsTheFirst();
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    private static string Dump(IEnumerable<GraphDiagnostic> list) =>
        string.Join(" | ", list.Select(d => $"{d.Severity}:{d.Code}:{d.NodeId ?? "-"}"));

    // ------------------------------------------------------------------ the list

    private static int ThreeIndependentFaultsAreThreeDiagnostics()
    {
        Console.WriteLine("Test: three independent faults are three diagnostics, each naming its node");
        // bad: a table node without its required param=. gv: a variable nobody declared (the kind of fault
        // Validate stops at). lonely: an exec node nothing ever runs.
        const string text = @"OCGRAPH 1
NAME Faults
NODE tick OnTick
NODE bad TriggerAnim
NODE gv GetVar var=ghost
NODE lonely PrintString text=hi
LINK tick.exec bad.exec
ENTRY tick OnTick
";
        var list = GraphDiagnostics.Run(text);
        int f = Expect("exactly three diagnostics", list.Count == 3, Dump(list));
        var ids = list.Select(d => d.NodeId).OrderBy(x => x, StringComparer.Ordinal).ToArray();
        f += Expect("they name bad, gv and lonely", ids.SequenceEqual(new[] { "bad", "gv", "lonely" }), Dump(list));
        f += Expect("codes are attr.missing, var.undeclared and exec.unreachable",
                    list.Select(d => d.Code).OrderBy(x => x, StringComparer.Ordinal)
                        .SequenceEqual(new[] { "attr.missing", "exec.unreachable", "var.undeclared" }), Dump(list));
        f += Expect("the unreachable node is a warning, the other two are errors",
                    list.Single(d => d.NodeId == "lonely").Severity == DiagnosticSeverity.Warning &&
                    list.Count(d => d.Severity == DiagnosticSeverity.Error) == 2, Dump(list));
        return f;
    }

    // ------------------------------------------------------------------ agreement with Validate

    private static int FirstDiagnosticIsValidatesMessage()
    {
        Console.WriteLine("Test: for every graph Validate refuses, the first diagnostic is Validate's own message");
        var fixtures = new (string Name, string Text)[]
        {
            ("undeclared variable", "OCGRAPH 1\nNODE gv GetVar var=ghost\n"),
            ("GetVar without var=", "OCGRAPH 1\nVAR v float 0\nNODE gv GetVar\n"),
            ("variable type mismatch", "OCGRAPH 1\nVAR v int 0\nNODE gv GetVar var=v\nPIN gv value out float\n"),
            ("duplicate VAR", "OCGRAPH 1\nVAR v float 0\nVAR v float 1\n"),
            ("undeclared parameter", "OCGRAPH 1\nNODE p Param param=nope\n"),
            ("duplicate ENTRY", "OCGRAPH 1\nNODE a OnTick\nNODE b OnTick\nENTRY a OnTick\nENTRY b OnTick\n"),
            ("ENTRY names no node", "OCGRAPH 1\nENTRY ghost OnTick\n"),
            ("call to an undeclared function", "OCGRAPH 1\nNODE c CallFunc call=Missing\n"),
            ("function with no FuncEntry", "OCGRAPH 1\nFUNC F pure\nFUNCOUT F r float\n"),
            ("wire crossing a subgraph boundary",
             "OCGRAPH 1\nFUNC F pure\nFUNCIN F x float\nFUNCOUT F r float\nNODE fe FuncEntry func=F\nNODE inner Add func=F\n" +
             "NODE fr FuncReturn func=F\nNODE outer ConstFloat value=1\nLINK fe.x inner.a\nLINK inner.result fr.r\nLINK outer.value inner.b\n"),
            ("exec output wired twice",
             "OCGRAPH 1\nNODE t OnTick\nNODE a PrintString text=a\nNODE b PrintString text=b\nLINK t.exec a.exec\nLINK t.exec b.exec\nENTRY t OnTick\n"),
            ("link between mismatched types",
             "OCGRAPH 1\nNODE c ConstBool value=true\nNODE a Add\nLINK c.value a.a\n"),
            ("OUT names a missing pin", "OCGRAPH 1\nNODE c ConstFloat value=1\nOUT c nothing\n"),
        };
        int failures = 0;
        foreach (var (name, text) in fixtures)
        {
            bool refused = !OcGraphParser.Parse(text, out _, out var oldErr);
            if (!refused) { failures += Expect($"{name}: the fixture really is refused", false, "Parse accepted it"); continue; }
            var list = GraphDiagnostics.Run(text);
            bool ok = list.Count > 0 && list[0].Severity == DiagnosticSeverity.Error && list[0].Message == oldErr;
            failures += Expect($"{name}: first diagnostic equals Validate's message", ok,
                               $"validate said '{oldErr}', diagnostics said '{(list.Count > 0 ? list[0].Message : "(nothing)")}'");
        }
        return failures;
    }

    private static int ValidateStillReturnsTheFirst()
    {
        Console.WriteLine("Test: Validate still returns the first problem, and a clean graph validates");
        const string text = "OCGRAPH 1\nNODE gv GetVar var=ghost\nNODE p Param param=nope\n";
        OcGraphParser.Parse(text, out var g, out _, validate: false);
        var all = new List<GraphDiagnostic>();
        g.Diagnose(all);
        bool bad = g.Validate(out var first);
        int f = Expect("Validate fails", !bad);
        f += Expect("and says what Diagnose listed first", all.Count >= 2 && first == all[0].Message, first ?? "(null)");
        f += Expect("Diagnose lists both faults", all.Count == 2, Dump(all));

        OcGraphParser.Parse("OCGRAPH 1\nNODE c ConstFloat value=1\nOUT c value\n", out var ok, out _, validate: false);
        f += Expect("a clean graph validates with no message", ok.Validate(out var none) && none == null);
        return f;
    }

    private static int ParseFailureIsOneParseError()
    {
        Console.WriteLine("Test: text that does not parse is one error with code 'parse'");
        var list = GraphDiagnostics.Run("this is not a graph");
        int f = Expect("one diagnostic", list.Count == 1, Dump(list));
        if (list.Count == 1)
        {
            f += Expect("an error coded parse", list[0].Severity == DiagnosticSeverity.Error && list[0].Code == "parse");
            f += Expect("saying why", list[0].Message.Contains("OCGRAPH header"), list[0].Message);
        }
        var hard = GraphDiagnostics.Run("OCGRAPH 1\nVAR x exec\n");
        f += Expect("a hard record error (VAR declared exec) is a parse error too",
                    hard.Count == 1 && hard[0].Code == "parse", Dump(hard));
        return f;
    }

    // ------------------------------------------------------------------ the checks only a list affords

    private static int MissingRequiredAttributeIsReported()
    {
        Console.WriteLine("Test: a table node without its required attribute is attr.missing, naming the node");
        const string text = "OCGRAPH 1\nNODE t OnTick\nNODE n TriggerAnim\nLINK t.exec n.exec\nENTRY t OnTick\n";
        var list = GraphDiagnostics.Run(text);
        var d = list.FirstOrDefault(x => x.Code == "attr.missing");
        int f = Expect("reported", d != null, Dump(list));
        if (d != null)
        {
            f += Expect("an error on node n", d.Severity == DiagnosticSeverity.Error && d.NodeId == "n");
            f += Expect("naming the attribute", d.Message.Contains("param="), d.Message);
        }
        // Optional attributes (a trailing '?') and supplied ones are fine.
        var fine = GraphDiagnostics.Run("OCGRAPH 1\nNODE t OnTick\nNODE n TriggerAnim param=Fire\nLINK t.exec n.exec\nENTRY t OnTick\n");
        f += Expect("with the attribute present there is no attr.missing", !fine.Any(x => x.Code == "attr.missing"), Dump(fine));
        return f;
    }

    private static int UnreachableAndUnusedAreWarnings()
    {
        Console.WriteLine("Test: unreachable exec nodes and unused or write-only variables are warnings");
        const string text = @"OCGRAPH 1
VAR never float 0
VAR wo float 0
VAR ok float 0
NODE tick OnTick
NODE s1 SetVar var=wo
NODE s2 SetVar var=ok
NODE g2 GetVar var=ok
NODE orphanTick OnTick
LINK tick.exec s1.exec
LINK s1.then s2.exec
LINK g2.value s2.value
ENTRY tick OnTick
";
        var list = GraphDiagnostics.Run(text);
        int f = Expect("three warnings and nothing else", list.Count == 3 && list.All(d => d.Severity == DiagnosticSeverity.Warning), Dump(list));
        f += Expect("var.unused names 'never'", list.Any(d => d.Code == "var.unused" && d.Message.Contains("'never'")), Dump(list));
        f += Expect("var.writeonly names 'wo'", list.Any(d => d.Code == "var.writeonly" && d.Message.Contains("'wo'")), Dump(list));
        f += Expect("exec.unreachable names orphanTick (an event nothing binds)",
                    list.Any(d => d.Code == "exec.unreachable" && d.NodeId == "orphanTick"), Dump(list));
        f += Expect("the variable that is read and written is not mentioned", !list.Any(d => d.Message.Contains("'ok'")), Dump(list));
        return f;
    }

    private static int CompilerRefusalMapsToItsNode()
    {
        Console.WriteLine("Test: a graph that validates but will not compile is a 'compile' error naming its node");
        // Two exec nodes wired into a ring: every record is legal, the compiler refuses the cycle.
        const string text = @"OCGRAPH 1
NODE t OnTick
NODE a PrintString text=a
NODE b PrintString text=b
LINK t.exec a.exec
LINK a.then b.exec
LINK b.then a.exec
ENTRY t OnTick
";
        var list = GraphDiagnostics.Run(text);
        var d = list.FirstOrDefault(x => x.Code == "compile");
        int f = Expect("a compile diagnostic", d != null, Dump(list));
        if (d != null)
        {
            f += Expect("an error", d.Severity == DiagnosticSeverity.Error);
            f += Expect("naming node 'a', the first quoted node in the compiler's sentence", d.NodeId == "a", d.NodeId ?? "(none)");
            f += Expect("with the compiler's words", d.Message.Contains("exec cycle"), d.Message);
        }
        return f;
    }

    private static int CleanGraphHasNoDiagnostics()
    {
        Console.WriteLine("Test: a sound graph has no diagnostics");
        const string text = "OCGRAPH 1\nNODE t OnTick\nNODE p PrintString text=hello\nLINK t.exec p.exec\nENTRY t OnTick\n";
        var list = GraphDiagnostics.Run(text);
        return Expect("none", list.Count == 0, Dump(list));
    }

    // ------------------------------------------------------------------ the wire

    private static int WireFormatAndCut()
    {
        Console.WriteLine("Test: the wire form is E|W TAB node TAB pin TAB code TAB message, cut at a whole line");
        var items = new List<GraphDiagnostic>
        {
            new(DiagnosticSeverity.Error, "attr.missing", "first", "n1", "pin"),
            new(DiagnosticSeverity.Warning, "var.unused", "second\twith a tab"),
        };
        string wire = GraphDiagnostics.ToWire(items, 4096);
        var lines = wire.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        int f = Expect("two lines", lines.Length == 2, wire);
        if (lines.Length == 2)
        {
            f += Expect("error line", lines[0] == "E\tn1\tpin\tattr.missing\tfirst", lines[0]);
            f += Expect("warning line has empty node and pin and no stray tab",
                        lines[1] == "W\t\t\tvar.unused\tsecond with a tab", lines[1]);
        }

        var many = Enumerable.Range(0, 100)
            .Select(i => new GraphDiagnostic(DiagnosticSeverity.Error, "x", "message number " + i, "node" + i)).ToList();
        string cut = GraphDiagnostics.ToWire(many, 600);
        var cutLines = cut.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        f += Expect("a small buffer cuts the list and says how many were dropped",
                    cutLines.Length > 1 && cutLines[^1].StartsWith("...") && cutLines[^1].EndsWith(" more"), cutLines[^1]);
        int kept = cutLines.Length - 1;
        int more = int.Parse(cutLines[^1].Substring(3, cutLines[^1].Length - 3 - " more".Length));
        f += Expect("kept plus dropped is everything", kept + more == 100, $"{kept}+{more}");
        f += Expect("every kept line is whole", cutLines.Take(kept).All(l => l.Split('\t').Length == 5));
        f += Expect("and the text fits the budget", System.Text.Encoding.UTF8.GetByteCount(cut) <= 600, cut.Length.ToString());
        return f;
    }
}
