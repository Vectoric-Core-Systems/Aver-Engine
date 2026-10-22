// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the rope nodes (docs/plans/wave11/SOFTBODY_HAIR.md 2.7): the 6 rows in GraphRopeNodeTable.cs, every method on RopeGraphInterop.
// Bare-process scope like WorldFxNodeTests: the wiring the table owns (a real method behind every row, pins, defaults, the optional asset
// attribute, valid IL, the compiler split) and the "never throw" rule when the native DLL is absent. What the soft shape system does with a
// call is tests/softbody.scene's business.
using System;
using System.Linq;
using System.Reflection;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class RopeNodeTests
{
    private static readonly Row[] Rows =
    {
        new("createrope", GenericKind.Exec, new[] { "exec:exec:in", "x1:float:in", "y1:float:in", "z1:float:in", "x2:float:in", "y2:float:in", "z2:float:in", "length:float:in", "radius:float:in", "then:exec:out", "entity:int:out", "success:bool:out" }, new[] { "length=0", "radius=0" }),
        new("setropeend", GenericKind.Exec, new[] { "exec:exec:in", "rope:int:in", "end:int:in", "anchor:int:in", "ox:float:in", "oy:float:in", "oz:float:in", "then:exec:out", "success:bool:out" }),
        new("freeropeend", GenericKind.Exec, new[] { "exec:exec:in", "rope:int:in", "end:int:in", "x:float:in", "y:float:in", "z:float:in", "pinned:bool:in", "then:exec:out", "success:bool:out" }),
        new("cutrope", GenericKind.Exec, new[] { "exec:exec:in", "rope:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "other:int:out", "success:bool:out" }),
        new("getrope", GenericKind.Pure, new[] { "rope:int:in", "found:bool:out", "length:float:out", "strain:float:out", "nodes:int:out", "cut:bool:out" }),
        new("destroyrope", GenericKind.Exec, new[] { "exec:exec:in", "rope:int:in", "then:exec:out", "success:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("rope", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("rope", Rows);
        failures += NodeRowKit.CheckIlValid("rope", types);
        failures += NodeRowKit.CheckCompilerSplit("rope", types);
        failures += TestAssetAttribute();
        failures += TestNothingThrows();
        failures += TestRopeChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the rope rows are exactly the designed set of 6, one pure read and five exec writes, in the Rope family");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "RopeGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 6) return Fail($"expected 6 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1) return Fail("only GetRope is a read");
        foreach (var t in want)
            if (!GameSystemNodes.TryGet(t, out var s) || s.Family != "Rope") return Fail($"{t} is not tagged with the Rope family");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The tube's material is the rope asset's: Create Rope takes an optional asset= and nothing else needs a name.
    private static int TestAssetAttribute()
    {
        Console.WriteLine("Test: Create Rope's asset= is optional, and the other rope nodes need no attribute");
        int f = 0;
        f += NodeRowKit.CheckOptionalAttribute("NODE n CreateRope", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n CreateRope asset=Ropes/Cable.ocsoft", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n CutRope", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n DestroyRope", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // Failure returns false; with no native DLL every call fails, and none may throw.
    private static int TestNothingThrows()
    {
        Console.WriteLine("Test: every RopeGraphInterop method returns instead of throwing, native DLL or not");
        var owner = typeof(SoftBodies).Assembly.GetType("Aver.Framework.RopeGraphInterop");
        if (owner == null) return Fail("Aver.Framework.RopeGraphInterop was not found by reflection");
        int called = 0;
        foreach (var m in owner.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public).Where(m => m.Name.EndsWith("ForGraph")))
        {
            var args = m.GetParameters().Select(p =>
                p.ParameterType == typeof(string) ? (object)"" :
                p.ParameterType == typeof(float) ? 1f :
                p.ParameterType.IsByRef ? Activator.CreateInstance(p.ParameterType.GetElementType()!)! :
                Activator.CreateInstance(p.ParameterType)!).ToArray();
            try { m.Invoke(null, args); ++called; }
            catch (TargetInvocationException e) { return Fail($"{m.Name} threw {e.InnerException?.GetType().Name}: {e.InnerException?.Message}"); }
        }
        if (called != Rows.Length) return Fail($"called {called} methods, the table has {Rows.Length} rows");
        Console.WriteLine($"  PASS: {called} methods");
        return 0;
    }

    // A rope's life in one chain: make it, pin an end to an entity, cut it, read both pieces, destroy the far one. Create's entity feeds the rest.
    private static int TestRopeChain()
    {
        Console.WriteLine("Test: Create, Set End, Free End, Cut, Get and Destroy chain; the new entity feeds the later nodes");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE cr CreateRope\nNODE se SetRopeEnd\nNODE fe FreeRopeEnd\nNODE ct CutRope\nNODE gr GetRope\n" +
                            "NODE br Branch\nNODE pf Print\nNODE de DestroyRope\nNODE pi PrintInt\n" +
                            "LINK tick.exec cr.exec\nLINK cr.then se.exec\nLINK cr.entity se.rope\nLINK se.then fe.exec\nLINK cr.entity fe.rope\nLINK fe.then ct.exec\n" +
                            "LINK cr.entity ct.rope\nLINK ct.then br.exec\nLINK cr.entity gr.rope\nLINK gr.found br.cond\nLINK br.true pf.exec\nLINK gr.length pf.value\n" +
                            "LINK pf.then de.exec\nLINK ct.other de.rope\nLINK de.then pi.exec\nLINK gr.nodes pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
