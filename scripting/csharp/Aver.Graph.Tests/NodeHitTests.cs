// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// NODE-HIT RECORDING: every exec node reporting that it ran, so the editor can highlight live
// control flow. Visual scripting had no way to see which branch was taken -- you could print a value
// and nothing showed you the PATH.
//
// WHAT THESE TESTS ARE FOR, specifically:
//
//  - OFF MUST COST NOTHING AND RECORD NOTHING. The call sits on the hot path of every exec node of
//    every live instance. If a graph runs with recording off and hits still appear, the "static bool
//    and nothing else" claim is false.
//  - ONLY THE ARM THAT RAN. A Branch is the whole point: if both arms report, the highlight is a lie
//    that looks like a feature.
//  - A DIAMOND MUST NOT CONFUSE IT. EmitExecNode's own comment records that a node reachable from two
//    branch arms has its IL emitted TWICE at compile time. That is exactly why this records a
//    last-hit TIME and not a counter -- a counter would over-report by construction, and the test
//    that would have caught that is the one asserting a rejoin node appears once, not twice.
//  - GRAPHS MUST NOT BLEED. Two graphs each with a node called "n1" must not light each other's.
using System;
using Aver.Graph;

// No namespace, matching every other file in this suite.
static class NodeHitTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += RecordingOffRecordsNothing();
        failures += OnlyTheBranchArmThatRanIsRecorded();
        failures += ARejoinNodeIsReportedOnce();
        failures += HitsDoNotBleedBetweenGraphs();
        Interop.SetRecording(false);
        return failures;
    }

    private static int Expect(string what, bool ok, string detail = "")
    {
        if (ok) { Console.WriteLine($"  PASS: {what}"); return 0; }
        Console.WriteLine($"  FAIL: {what}{(detail.Length > 0 ? " -- " + detail : "")}");
        return 1;
    }

    // GraphInterop's recording members are internal to Aver.Framework, so this suite reaches them the
    // same way the compiler does: by reflection. Going through the real members rather than a test
    // double is the point -- a rename would break the emitted IL too, and this notices.
    private static class Interop
    {
        private static readonly Type T =
            typeof(Aver.Framework.Actors).Assembly.GetType("Aver.Framework.GraphInterop")
            ?? throw new InvalidOperationException("Aver.Framework.GraphInterop not found");

        public static void SetRecording(bool on) =>
            T.GetMethod("SetNodeHitRecording", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!
             .Invoke(null, new object[] { on });

        public static string Collect(string graph, double maxAge) =>
            (string)T.GetMethod("CollectNodeHits", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!
                     .Invoke(null, new object[] { graph, maxAge })!;
    }

    private static string[] HitIds(string graph)
    {
        string raw = Interop.Collect(graph, 60.0);
        if (raw.Length == 0) return Array.Empty<string>();
        string[] parts = raw.Split(';');
        var ids = new string[parts.Length];
        for (int i = 0; i < parts.Length; ++i) ids[i] = parts[i].Split(':')[0];
        return ids;
    }

    private static bool RunGraph(string text, string entry, out string err)
    {
        err = null!;
        if (!OcGraphParser.Parse(text, out var g, out err)) return false;
        var fn = new GraphCompiler(g).CompileEntryPoint(entry, out err);
        if (fn == null) return false;
        fn.DynamicInvoke();
        return true;
    }

    // THE COST CLAIM. With recording off the emitted call still happens -- it is in the IL
    // unconditionally -- so what must be true is that it stores nothing.
    private static int RecordingOffRecordsNothing()
    {
        Console.WriteLine("Test: with recording off, running a graph records no hits");
        Interop.SetRecording(false);
        const string text =
            "OCGRAPH 1\nNAME HitOff\nENTRY t OnTick\nNODE t OnTick\n" +
            "NODE p PrintString text=ran\nLINK t.exec p.exec\n";
        if (!RunGraph(text, "OnTick", out var err)) return Expect("the graph runs", false, err ?? "");
        return Expect("nothing was recorded while recording was off", HitIds("HitOff").Length == 0);
    }

    // A BRANCH IS THE WHOLE POINT. If both arms reported, the highlight would show a path that never
    // executed -- worse than no highlight, because it looks authoritative.
    private static int OnlyTheBranchArmThatRanIsRecorded()
    {
        Console.WriteLine("Test: only the branch arm that actually ran is recorded");
        Interop.SetRecording(false);
        Interop.SetRecording(true);   // clears, then arms
        const string text =
            "OCGRAPH 1\nNAME HitBranch\nENTRY t OnTick\nNODE t OnTick\n" +
            "NODE cond ConstBool value=true\n" +
            "NODE br Branch\n" +
            "NODE tookTrue PrintString text=T\n" +
            "NODE tookFalse PrintString text=F\n" +
            "LINK t.exec br.exec\nLINK cond.value br.cond\n" +
            "LINK br.true tookTrue.exec\nLINK br.false tookFalse.exec\n";
        if (!RunGraph(text, "OnTick", out var err)) return Expect("the branch graph runs", false, err ?? "");

        var ids = HitIds("HitBranch");
        int f = Expect("the branch node itself is recorded", Array.IndexOf(ids, "br") >= 0,
                       string.Join(",", ids));
        f += Expect("the TRUE arm is recorded", Array.IndexOf(ids, "tookTrue") >= 0, string.Join(",", ids));
        f += Expect("AND THE FALSE ARM IS NOT -- a highlight showing an untaken path would be a lie",
                    Array.IndexOf(ids, "tookFalse") < 0, string.Join(",", ids));
        return f;
    }

    // THE DIAMOND. EmitExecNode emits a rejoin node's IL once per arm reaching it, so at compile time
    // "after" below exists twice. Recording a TIME rather than a count is what makes that harmless;
    // this test is what would have caught a counter.
    private static int ARejoinNodeIsReportedOnce()
    {
        Console.WriteLine("Test: a node both branch arms rejoin is reported once, not twice");
        Interop.SetRecording(false);
        Interop.SetRecording(true);
        const string text =
            "OCGRAPH 1\nNAME HitDiamond\nENTRY t OnTick\nNODE t OnTick\n" +
            "NODE cond ConstBool value=true\n" +
            "NODE br Branch\n" +
            "NODE armT PrintString text=T\n" +
            "NODE armF PrintString text=F\n" +
            "NODE after PrintString text=joined\n" +
            "LINK t.exec br.exec\nLINK cond.value br.cond\n" +
            "LINK br.true armT.exec\nLINK br.false armF.exec\n" +
            "LINK armT.then after.exec\nLINK armF.then after.exec\n";
        if (!RunGraph(text, "OnTick", out var err)) return Expect("the diamond graph runs", false, err ?? "");

        var ids = HitIds("HitDiamond");
        int occurrences = 0;
        foreach (var id in ids) if (id == "after") ++occurrences;
        int f = Expect("the rejoin node is recorded", occurrences >= 1, string.Join(",", ids));
        f += Expect("EXACTLY ONCE, though its IL is emitted per arm -- which is why this is a "
                    + "last-hit time and not a counter", occurrences == 1, $"{occurrences} entries");
        return f;
    }

    // Two graphs, both with a node called "n1". Keying by node id alone would light the wrong canvas.
    private static int HitsDoNotBleedBetweenGraphs()
    {
        Console.WriteLine("Test: two graphs sharing a node id do not light each other's nodes");
        Interop.SetRecording(false);
        Interop.SetRecording(true);
        const string a =
            "OCGRAPH 1\nNAME HitAlpha\nENTRY t OnTick\nNODE t OnTick\n" +
            "NODE n1 PrintString text=a\nLINK t.exec n1.exec\n";
        const string b =
            "OCGRAPH 1\nNAME HitBeta\nENTRY t OnTick\nNODE t OnTick\n" +
            "NODE n1 PrintString text=b\nNODE n2 PrintString text=b2\n" +
            "LINK t.exec n1.exec\nLINK n1.then n2.exec\n";
        if (!RunGraph(a, "OnTick", out var e1)) return Expect("graph A runs", false, e1 ?? "");

        var alpha = HitIds("HitAlpha");
        int f = Expect("A recorded its own node", Array.IndexOf(alpha, "n1") >= 0, string.Join(",", alpha));
        f += Expect("and A has no node from B yet", Array.IndexOf(alpha, "n2") < 0, string.Join(",", alpha));

        if (!RunGraph(b, "OnTick", out var e2)) return f + Expect("graph B runs", false, e2 ?? "");
        var beta = HitIds("HitBeta");
        f += Expect("B recorded its own nodes", Array.IndexOf(beta, "n2") >= 0, string.Join(",", beta));
        // A's OnTick node is an exec node too, so A legitimately reports {t, n1}. What must NOT
        // appear is anything of B's -- the assertion is about bleed, not about a count.
        var alphaAfter = HitIds("HitAlpha");
        f += Expect("AND A STILL REPORTS ONLY ITS OWN, though both declare a node called n1",
                    Array.IndexOf(alphaAfter, "n2") < 0 && Array.IndexOf(alphaAfter, "n1") >= 0,
                    string.Join(",", alphaAfter));
        return f;
    }
}
