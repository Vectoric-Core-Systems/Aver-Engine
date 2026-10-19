// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the AI query nodes (docs/plans/wave3/EQS_SMARTOBJECTS_STATETREE.md section 9): 6 EQS rows and
// 9 smart-object rows (Aver.Synapse.QueryGraph) and 6 brain rows (BrainGraph), in GraphAiQueryNodeTable.cs.
// Bare-process scope like GasNodeTests: the wiring the table owns (a real method behind every row, pins that
// match its parameters, defaults, push-only versus pure, required attributes by name, valid IL, entries by
// event name) plus the argument guards that answer before any native call. What the queries and brains do
// is the native tests' job.
using System;
using System.Linq;
using Aver.Graph;
using Row = NodeRowKit.Row;

static class AiQueryNodeTests
{
    private static readonly string[] EqsInputs = { "querier:int:in", "target:int:in", "px:float:in", "py:float:in", "pz:float:in", "hasPoint:bool:in" };

    private static readonly Row[] Rows =
    {
        new("an_eqsrun", GenericKind.Exec, Concat(new[] { "exec:exec:in" }, EqsInputs,
            new[] { "then:exec:out", "found:bool:out", "count:int:out", "x:float:out", "y:float:out", "z:float:out", "entity:int:out", "score:float:out" })),
        new("an_eqsstart", GenericKind.Exec, Concat(new[] { "exec:exec:in" }, EqsInputs,
            new[] { "priority:int:in", "then:exec:out", "handle:int:out" })),
        new("an_eqsstatus", GenericKind.Pure, new[] { "handle:int:in", "status:int:out", "count:int:out" }),
        new("an_eqsgetitem", GenericKind.Pure, new[] { "handle:int:in", "index:int:in", "success:bool:out", "x:float:out", "y:float:out", "z:float:out", "entity:int:out", "score:float:out", "obj:int:out", "slot:int:out" }),
        new("an_eqscancel", GenericKind.Exec, new[] { "exec:exec:in", "handle:int:in", "then:exec:out", "success:bool:out" }),
        new("an_eqsrelease", GenericKind.Exec, new[] { "exec:exec:in", "handle:int:in", "then:exec:out", "success:bool:out" }),
        new("an_soattach", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("an_sofind", GenericKind.Pure, new[] { "user:int:in", "x:float:in", "y:float:in", "z:float:in", "radius:float:in", "matchAll:bool:in", "found:bool:out", "obj:int:out", "slot:int:out", "ex:float:out", "ey:float:out", "ez:float:out", "count:int:out" }, new[] { "radius=1500" }),
        new("an_soclaim", GenericKind.Exec, new[] { "exec:exec:in", "user:int:in", "obj:int:in", "slot:int:in", "ttl:float:in", "then:exec:out", "claim:int:out", "refusal:int:out" }, new[] { "ttl=-1" }),
        new("an_souse", GenericKind.Exec, new[] { "exec:exec:in", "claim:int:in", "then:exec:out", "success:bool:out" }),
        new("an_sorelease", GenericKind.Exec, new[] { "exec:exec:in", "claim:int:in", "reason:int:in", "then:exec:out", "success:bool:out" }),
        new("an_sogetclaim", GenericKind.Pure, new[] { "user:int:in", "index:int:in", "has:bool:out", "claim:int:out", "obj:int:out", "slot:int:out", "state:int:out", "x:float:out", "y:float:out", "z:float:out", "yaw:float:out", "ex:float:out", "ey:float:out", "ez:float:out" }, new[] { "index=0" }),
        new("an_soslotinfo", GenericKind.Pure, new[] { "entity:int:in", "slot:int:in", "success:bool:out", "state:int:out", "user:int:out", "x:float:out", "y:float:out", "z:float:out", "yaw:float:out" }),
        new("an_soslotcount", GenericKind.Pure, new[] { "entity:int:in", "count:int:out" }),
        new("an_sosetslotenabled", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "slot:int:in", "enabled:bool:in", "then:exec:out", "success:bool:out" }, new[] { "enabled=true" }),
        new("an_brainset", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("an_brainsendevent", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "payloadI:int:in", "payloadF:float:in", "then:exec:out", "success:bool:out" }),
        new("an_brainisinstate", GenericKind.Pure, new[] { "entity:int:in", "inState:bool:out" }),
        new("an_brainstatus", GenericKind.Pure, new[] { "entity:int:in", "status:int:out", "kind:int:out" }),
        new("an_brainrestart", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("an_brainstop", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
    };

    private static string[] Concat(params string[][] parts) => parts.SelectMany(p => p).ToArray();

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("AI query", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("AI query", Rows);
        failures += NodeRowKit.CheckIlValid("AI query", types);
        failures += NodeRowKit.CheckCompilerSplit("AI query", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestArgumentGuardsAnswerWithoutNative();
        failures += TestQueryChain();
        failures += TestSmartObjectChain();
        failures += TestBrainChain();
        failures += TestEventEntries();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static bool IsAiType(string t) =>
        t.StartsWith("an_eqs", StringComparison.OrdinalIgnoreCase) || (t.StartsWith("an_so", StringComparison.OrdinalIgnoreCase) && !t.StartsWith("an_soft", StringComparison.OrdinalIgnoreCase)) ||
        t.StartsWith("an_brain", StringComparison.OrdinalIgnoreCase);

    // No row may go missing or be added without this list (and the design) changing with it.
    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the AI query rows are exactly the designed set (6 EQS, 9 smart object, 6 brain)");
        var got = GameSystemNodes.All.Where(s => IsAiType(s.Type)).Select(s => s.Type).OrderBy(x => x).ToArray();
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 21) return Fail($"expected 21 rows, test lists {Rows.Length}");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Required: the query, asset, event and state names. Optional: the tag lists, which an empty value omits.
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: an AI node missing its required attribute fails to compile, naming it; tag lists stay optional");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_EqsRun", "query=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_EqsStart", "query=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SoAttach", "asset=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_BrainSet", "asset=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_BrainSendEvent", "event=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_BrainIsInState", "state=", exec: false);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_EqsRun query=AI/Cover.oceqs", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_EqsStart query=AI/Cover.oceqs", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SoFind", exec: false);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SoClaim", exec: true);
        // The rest take only pins.
        foreach (var t in new[] { "AN_EqsCancel", "AN_EqsRelease", "AN_SoUse", "AN_SoRelease", "AN_SoSetSlotEnabled", "AN_BrainRestart", "AN_BrainStop" })
            f += NodeRowKit.CheckOptionalAttribute("NODE n " + t, exec: true);
        foreach (var t in new[] { "AN_EqsStatus", "AN_EqsGetItem", "AN_SoGetClaim", "AN_SoSlotInfo", "AN_SoSlotCount", "AN_BrainStatus" })
            f += NodeRowKit.CheckOptionalAttribute("NODE n " + t, exec: false);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // Calls a row's own method by reflection. Null when the framework DLL is needed and absent.
    private static (object? Ret, object?[] Args)? Call(string type, params object?[] args)
    {
        if (!GameSystemNodes.TryGet(type, out var spec)) throw new InvalidOperationException("no row " + type);
        try { return (spec.Method.Invoke(null, args), args); }
        catch (System.Reflection.TargetInvocationException e) when (NodeRowKit.NativeMissing(e)) { return null; }
    }

    // Each of these answers from a managed guard before any native call, so the bare process can prove
    // the not-found shape a graph reads: false, 0, and the documented "none" values on the out pins.
    private static int TestArgumentGuardsAnswerWithoutNative()
    {
        Console.WriteLine("Test: empty names and zero handles answer false / 0 with the documented out values");
        var run = Call("an_eqsrun", 1, 0, 0f, 0f, 0f, false, "", "", 0, 0f, 0f, 0f, 0, 0f);
        if (run is { Ret: not false }) return Fail("EqsRun with no query should answer false");
        var start = Call("an_eqsstart", 1, 0, 0f, 0f, 0f, false, 0, "", "");
        if (start is { Ret: not 0 }) return Fail("EqsStart with no query should answer handle 0");
        var status = Call("an_eqsstatus", 0, 0);
        if (status is { Ret: not 0 } || status is { } s1 && (int)s1.Args[1]! != 0) return Fail("EqsStatus of handle 0 should be status 0, count 0");
        var item = Call("an_eqsgetitem", 0, 0, 0f, 0f, 0f, 0, 0f, 0, 0);
        if (item is { } i && (i.Ret is not false || (int)i.Args[8]! != -1)) return Fail("EqsGetItem of handle 0 should be false with slot -1");
        if (Call("an_eqscancel", 0) is { Ret: not false }) return Fail("EqsCancel of handle 0 should be false");
        if (Call("an_eqsrelease", 0) is { Ret: not false }) return Fail("EqsRelease of handle 0 should be false");
        if (Call("an_soattach", 1, "") is { Ret: not false }) return Fail("SoAttach with no asset should be false");
        if (Call("an_souse", 0) is { Ret: not false }) return Fail("SoUse of claim 0 should be false");
        if (Call("an_sorelease", 0, 0) is { Ret: not false }) return Fail("SoRelease of claim 0 should be false");
        var claim = Call("an_sogetclaim", 1, -1, 0, 0, 0, 0, 0f, 0f, 0f, 0f, 0f, 0f, 0f);
        if (claim is { } c && (c.Ret is not false || (int)c.Args[4]! != -1)) return Fail("SoGetClaim of index -1 should be false with slot -1");
        if (Call("an_brainset", 1, "") is { Ret: not false }) return Fail("BrainSet with no asset should be false");
        if (Call("an_brainsendevent", 1, 0, 0f, "") is { Ret: not false }) return Fail("BrainSendEvent with no event should be false");
        if (Call("an_brainisinstate", 1, "") is { Ret: not false }) return Fail("BrainIsInState with no state should be false");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Start a run, read its status and its first item, release it: the time-sliced path a graph takes.
    private static int TestQueryChain()
    {
        Console.WriteLine("Test: EQS Start feeds Status, Get Item and Release; Run reports the best item");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE s AN_EqsStart query=AI/FindCover.oceqs usertags=Team.Red\n" +
                            "NODE st AN_EqsStatus\nNODE gi AN_EqsGetItem\nNODE br Branch\nNODE pr Print\nNODE rl AN_EqsRelease\n" +
                            "NODE run AN_EqsRun query=AI/FindCover.oceqs\nNODE pi PrintInt\n" +
                            "LINK tick.exec s.exec\nLINK s.handle st.handle\nLINK s.handle gi.handle\nLINK s.handle rl.handle\n" +
                            "LINK s.then br.exec\nLINK gi.success br.cond\nLINK br.true pr.exec\nLINK gi.score pr.value\n" +
                            "LINK pr.then rl.exec\nLINK rl.then run.exec\nLINK run.then pi.exec\nLINK st.status pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Find a slot, claim it, use it, release it: the claim handle and the slot ride int pins between nodes.
    private static int TestSmartObjectChain()
    {
        Console.WriteLine("Test: Find Smart Object Slot feeds Claim, Use and Release");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE f AN_SoFind slottags=Seat.Sit\nNODE br Branch\nNODE c AN_SoClaim usertags=Species.Human\n" +
                            "NODE u AN_SoUse\nNODE rl AN_SoRelease\nNODE info AN_SoSlotInfo\nNODE cnt AN_SoSlotCount\nNODE pi PrintInt\n" +
                            "LINK tick.exec br.exec\nLINK f.found br.cond\nLINK br.true c.exec\nLINK f.obj c.obj\nLINK f.slot c.slot\n" +
                            "LINK c.then u.exec\nLINK c.claim u.claim\nLINK u.then rl.exec\nLINK c.claim rl.claim\n" +
                            "LINK rl.then pi.exec\nLINK f.obj info.entity\nLINK f.slot info.slot\nLINK info.state pi.value\nLINK f.obj cnt.entity\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Give an entity a state tree, send it an event, then read where it is.
    private static int TestBrainChain()
    {
        Console.WriteLine("Test: Set Brain, Send Brain Event, Brain Is In State and Brain Status chain");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE set AN_BrainSet asset=AI/Guard.ocst\nNODE ev AN_BrainSendEvent event=Alarm\n" +
                            "NODE q AN_BrainIsInState state=Combat.Chase\nNODE st AN_BrainStatus\nNODE br Branch\nNODE rs AN_BrainRestart\nNODE pi PrintInt\n" +
                            "LINK tick.exec set.exec\nLINK set.then ev.exec\nLINK ev.then br.exec\nLINK q.inState br.cond\n" +
                            "LINK br.true rs.exec\nLINK rs.then pi.exec\nLINK st.status pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Completion reaches a graph as a bus event by name, so the existing AN_OnEvent is the entry.
    private static int TestEventEntries()
    {
        Console.WriteLine("Test: AN_OnEvent entries EqsFinished, SmartObjectClaimed and SmartObjectReleased drive AI nodes");
        const string finished = "EqsFinished";
        const string eqs = "OCGRAPH 1\nNODE e AN_OnEvent\nNODE gi AN_EqsGetItem\nNODE br Branch\nNODE pr Print\n" +
                           "LINK e.exec br.exec\nLINK gi.success br.cond\nLINK br.true pr.exec\nLINK gi.score pr.value\nENTRY e " + finished + "\n";
        if (!OcGraphParser.Parse(eqs, out var g, out var err)) return Fail("parse: " + err);
        if (!NodeRowKit.HasEntry(g, finished)) return Fail("the ENTRY record was not kept");
        string? bad = NodeRowKit.CompileAndRun(eqs, finished);
        if (bad != null) return Fail("EQS graph: " + bad);

        const string claimed = "SmartObjectClaimed";
        const string so = "OCGRAPH 1\nNODE e AN_OnEvent\nNODE u AN_SoUse\nLINK e.exec u.exec\nENTRY e " + claimed + "\n";
        bad = NodeRowKit.CompileAndRun(so, claimed);
        if (bad != null) return Fail("smart object graph: " + bad);

        const string released = "SmartObjectReleased";
        const string rel = "OCGRAPH 1\nNODE e AN_OnEvent\nNODE b AN_BrainRestart\nLINK e.exec b.exec\nENTRY e " + released + "\n";
        bad = NodeRowKit.CompileAndRun(rel, released);
        if (bad != null) return Fail("release graph: " + bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
