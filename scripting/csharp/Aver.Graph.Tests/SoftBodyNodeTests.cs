// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the cloth and destruction nodes (docs/SOFTBODY.md): the 8 rows in GraphSoftBodyNodeTable.cs, every method
// on SoftBodyGraphInterop. Bare-process scope like MotionMatchNodeTests: the wiring the table owns (pins, defaults,
// the method each row names, valid IL, the compiler split), not what the soft-body system does with a call
// (tests/softbody.scene does that).
using System;
using System.Linq;
using Aver.Graph;
using Row = NodeRowKit.Row;

static class SoftBodyNodeTests
{
    private static readonly Row[] Rows =
    {
        new("softbodydamage", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "radius:float:in", "energy:float:in", "dirX:float:in", "dirY:float:in", "dirZ:float:in", "then:exec:out", "success:bool:out" }, new[] { "radius=30", "energy=100" }),
        new("softbodyexplode", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "radius:float:in", "impulse:float:in", "energy:float:in", "then:exec:out", "touched:int:out", "success:bool:out" }, new[] { "radius=300", "impulse=600", "energy=500" }),
        new("softbodytear", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "radius:float:in", "then:exec:out", "success:bool:out" }, new[] { "radius=20" }),
        new("softbodyrepair", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("releasesoftbody", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("setsoftbodywind", GenericKind.Exec, new[] { "exec:exec:in", "vx:float:in", "vy:float:in", "vz:float:in", "gust:float:in", "gustHz:float:in", "turbulence:float:in", "then:exec:out", "success:bool:out" }, new[] { "gust=0", "gustHz=0.3", "turbulence=0" }),
        new("setsoftbodypreset", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "preset:int:in", "then:exec:out", "success:bool:out" }, new[] { "preset=0" }),
        new("getsoftbodystate", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "damage:float:out", "maxStrain:float:out", "pieces:int:out", "brokenEdges:int:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("soft body", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("soft body", Rows);
        failures += NodeRowKit.CheckIlValid("soft body", types);
        failures += NodeRowKit.CheckCompilerSplit("soft body", types);
        failures += TestDestructionChain();
        failures += TestStateFeedsLaterNodes();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the soft-body rows are exactly the designed set of 8, 1 pure read and 7 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "SoftBodyGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 8) return Fail($"expected 8 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1) return Fail("only GetSoftBodyState is a read");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The per-event loop of a shatter graph: wind up, hit a point, cut, release, repair; explode feeds its count on.
    private static int TestDestructionChain()
    {
        Console.WriteLine("Test: Wind, Damage, Tear, Release, Repair and Preset chain exec to exec; Explode's touched count feeds a later node");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE w SetSoftBodyWind\nNODE d SoftBodyDamage\nNODE t SoftBodyTear\nNODE r ReleaseSoftBody\nNODE p SetSoftBodyPreset\n" +
                            "NODE e SoftBodyExplode\nNODE rp SoftBodyRepair\nNODE pi PrintInt\n" +
                            "LINK tick.exec w.exec\nLINK w.then d.exec\nLINK d.then t.exec\nLINK t.then r.exec\nLINK r.then p.exec\nLINK p.then e.exec\nLINK e.then rp.exec\n" +
                            "LINK rp.then pi.exec\nLINK e.touched pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // Get Soft Body State is a pure read: its pins feed a branch and a print without being in the exec chain.
    private static int TestStateFeedsLaterNodes()
    {
        Console.WriteLine("Test: Get Soft Body State's found, damage and pieces feed a Branch and prints");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE gs GetSoftBodyState\nNODE br Branch\nNODE pf Print\nNODE pi PrintInt\n" +
                            "LINK tick.exec br.exec\nLINK gs.found br.cond\nLINK br.true pf.exec\nLINK gs.damage pf.value\nLINK pf.then pi.exec\nLINK gs.pieces pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
