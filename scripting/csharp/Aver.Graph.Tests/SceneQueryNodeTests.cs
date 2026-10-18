// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the SceneQuery family (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): the 12 rows in
// GraphSceneQueryNodeTable.cs, every method on GraphSceneQueries. Bare-process scope like MontageNodeTests: the wiring the
// table owns (pins, the method each row names, required attributes, valid IL, the compiler split) and the rule that an
// unavailable scene answers 0 or false instead of throwing. What a query returns over a live world is checked where a
// world exists (tests/scene, tests/framework); "unavailable" is not a failure here, as in GasPersistTest.
using System;
using System.Collections.Generic;
using System.Linq;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class SceneQueryNodeTests
{
    private static string[] Pins(bool exec, string ins, string outs)
    {
        var l = new List<string>();
        if (exec) l.Add("exec:exec:in");
        foreach (var p in ins.Split(' ', StringSplitOptions.RemoveEmptyEntries)) l.Add(p + ":in");
        if (exec) l.Add("then:exec:out");
        foreach (var p in outs.Split(' ', StringSplitOptions.RemoveEmptyEntries)) l.Add(p + ":out");
        return l.ToArray();
    }

    private static Row P(string type, string ins, string outs) => new(type, GenericKind.Pure, Pins(false, ins, outs));
    private static Row X(string type, string ins, string outs) => new(type, GenericKind.Exec, Pins(true, ins, outs));

    private static readonly Row[] Rows =
    {
        P("an_countactorswithtag", "mask:int", "count:int"),
        P("an_getactorwithtagat", "mask:int index:int", "entity:int"),
        P("an_countactorsofclass", "", "count:int"),
        P("an_getactorofclassat", "index:int", "entity:int"),
        P("an_getchildcount", "entity:int", "count:int"),
        P("an_getchildat", "entity:int index:int", "child:int"),
        P("an_getparent", "entity:int", "parent:int"),
        P("an_hascomponent", "entity:int", "has:bool"),
        X("an_addcomponent", "entity:int", "success:bool"),
        P("an_getdistance", "a:int b:int", "distance:float success:bool"),
        P("an_getragdollstate", "entity:int", "mode:int has:bool"),
        P("an_getactorcount", "", "count:int"),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("scene query", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("scene query", Rows);
        failures += NodeRowKit.CheckIlValid("scene query", types);
        failures += NodeRowKit.CheckCompilerSplit("scene query", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestNoSceneAnswersNeutrally();
        failures += TestPublicSurface();
        failures += TestIterationChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the scene-query rows are exactly the designed set of 12, 11 pure reads and one exec write");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "GraphSceneQueries").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 12) return Fail($"expected 12 rows, test lists {Rows.Length}");
        var exec = Rows.Where(r => r.Kind == GenericKind.Exec).Select(r => r.Type).ToArray();
        if (!exec.SequenceEqual(new[] { "an_addcomponent" })) return Fail("only AddComponent may be exec");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Class and component names are attributes; without one the query could never match anything.
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: a class or component query with no class= / component= fails to compile, naming it");
        if (NodeRowKit.CheckMissingAttribute("NODE n an_countactorsofclass", "class", exec: false) != 0) return 1;
        if (NodeRowKit.CheckMissingAttribute("NODE n an_getactorofclassat", "class", exec: false) != 0) return 1;
        if (NodeRowKit.CheckMissingAttribute("NODE n an_hascomponent", "component", exec: false) != 0) return 1;
        if (NodeRowKit.CheckMissingAttribute("NODE n an_addcomponent", "component", exec: true) != 0) return 1;
        Console.WriteLine("  PASS");
        return 0;
    }

    // Dead handles, out-of-range indices and an empty class name give 0 or false and never throw, with or without a scene.
    private static int TestNoSceneAnswersNeutrally()
    {
        Console.WriteLine("Test: dead handles, bad indices and empty names answer 0 or false, never throw");
        var c = new FamilyAChecks();
        var cls = new Dictionary<string, string> { ["class"] = "" };
        var comp = new Dictionary<string, string> { ["component"] = "CLight" };
        try
        {
            c.Eq("countactorswithtag(0) is 0", FamilyAEval.I("an_countactorswithtag", "count", "mask", 0), 0);
            c.Eq("getactorwithtagat with a negative index", FamilyAEval.I("an_getactorwithtagat", "entity", "mask", 1, "index", -1), 0);
            c.Eq("countactorsofclass of an empty name", (int)FamilyAEval.Call("an_countactorsofclass", null, cls)["count"], 0);
            c.Eq("getactorofclassat of an empty name", (int)FamilyAEval.Call("an_getactorofclassat", FamilyAEval.In("index", 0), cls)["entity"], 0);
            c.Eq("getactorofclassat with a negative index", (int)FamilyAEval.Call("an_getactorofclassat", FamilyAEval.In("index", -2), cls)["entity"], 0);
            c.Eq("getchildcount of a dead handle", FamilyAEval.I("an_getchildcount", "count", "entity", 0), 0);
            c.Eq("getchildat of a dead handle", FamilyAEval.I("an_getchildat", "child", "entity", 0, "index", 0), 0);
            c.Eq("getchildat with a negative index", FamilyAEval.I("an_getchildat", "child", "entity", 7, "index", -1), 0);
            c.Eq("getparent of a dead handle", FamilyAEval.I("an_getparent", "parent", "entity", 0), 0);
            c.True("hascomponent of a dead handle", !(bool)FamilyAEval.Call("an_hascomponent", FamilyAEval.In("entity", 0), comp)["has"]);
            c.True("addcomponent to a dead handle", !(bool)FamilyAEval.Call("an_addcomponent", FamilyAEval.In("entity", 0), comp)["success"]);
            c.True("addcomponent with an empty name", !(bool)FamilyAEval.Call("an_addcomponent", FamilyAEval.In("entity", 0), null)["success"]);

            var d = FamilyAEval.Call("an_getdistance", FamilyAEval.In("a", 0, "b", 0));
            c.True("getdistance of two dead handles fails", !(bool)d["success"]);
            c.Near("getdistance leaves 0", (float)d["distance"], 0.0);

            var rag = FamilyAEval.Call("an_getragdollstate", FamilyAEval.In("entity", 0));
            c.True("getragdollstate of a dead handle has none", !(bool)rag["has"]);
            c.Eq("getragdollstate mode is -1", (int)rag["mode"], -1);

            c.True("getactorcount is not negative", FamilyAEval.I("an_getactorcount", "count") >= 0);
        }
        catch (Exception e) { return Fail($"a wrapper threw {e.GetType().Name}: {e.Message}"); }
        return c.Done();
    }

    // The public C# face of the same queries.
    private static int TestPublicSurface()
    {
        Console.WriteLine("Test: SceneQueries and Game.ActorsOfClass guard their arguments before touching the scene");
        var c = new FamilyAChecks();
        c.Eq("WithTagAt(-1) is none", Aver.Framework.SceneQueries.WithTagAt(1u, -1).Handle, 0);
        c.Eq("ActorOfClassAt(-1) is none", Aver.Framework.SceneQueries.ActorOfClassAt("Anything", -1).Handle, 0);
        c.Eq("ChildAt(-1) is none", Aver.Framework.SceneQueries.ChildAt(Aver.Framework.Entity.None, -1).Handle, 0);
        c.Eq("CountActorsOfClass of an empty name", Aver.Framework.SceneQueries.CountActorsOfClass(""), 0);
        c.True("ActorsOfClass of an empty name is empty", !Aver.Framework.Game.ActorsOfClass("").Any());
        c.True("ActorsOfClass of null is empty", !Aver.Framework.Game.ActorsOfClass(null!).Any());
        try
        {
            c.True("TryGetDistance of two invalid handles", !Aver.Framework.SceneQueries.TryGetDistance(Aver.Framework.Entity.None, Aver.Framework.Entity.None, out float dist) && dist == 0f);
            c.Eq("RagdollModeOf an invalid handle", Aver.Framework.SceneQueries.RagdollModeOf(Aver.Framework.Entity.None), -1);
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e)) { }   // no scene DLL in a bare process
        return c.Done();
    }

    // The counted pattern the design names: a tag count drives a ForEach whose index picks each entity in turn.
    private static int TestIterationChain()
    {
        Console.WriteLine("Test: a count drives a ForEach whose index reads each item; queries feed Branch, AddComponent and prints");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE cnt an_countactorswithtag\nPINVAL cnt mask 3\nNODE fe ForEach\nNODE at an_getactorwithtagat\nPINVAL at mask 3\n" +
                            "NODE pi PrintInt\nNODE done PrintInt\nNODE hc an_hascomponent component=CLight\nNODE br Branch\nNODE ac an_addcomponent component=CLight\n" +
                            "NODE d an_getdistance\nNODE pf Print\nNODE rd an_getragdollstate\nNODE cc an_getchildcount\nNODE gp an_getparent\nNODE na an_getactorcount\n" +
                            "LINK tick.exec fe.exec\nLINK cnt.count fe.count\nLINK fe.index at.index\nLINK fe.loop br.exec\nLINK at.entity hc.entity\nLINK hc.has br.cond\n" +
                            "LINK br.false ac.exec\nLINK at.entity ac.entity\nLINK ac.then pi.exec\nLINK at.entity pi.value\nLINK pi.then pf.exec\nLINK at.entity d.a\nLINK gp.parent d.b\n" +
                            "LINK at.entity gp.entity\nLINK d.distance pf.value\nLINK fe.done done.exec\nLINK na.count done.value\n" +
                            "NODE ra PrintInt\nLINK pf.then ra.exec\nLINK rd.mode ra.value\nLINK at.entity rd.entity\nLINK at.entity cc.entity\n" +
                            "ENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
