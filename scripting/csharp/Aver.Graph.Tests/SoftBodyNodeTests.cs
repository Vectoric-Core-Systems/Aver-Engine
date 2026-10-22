// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the cloth and destruction nodes (docs/SOFTBODY.md, docs/plans/wave10/SOFTBODY_DEPTH.md 2.13, docs/plans/wave11/SOFTBODY_HAIR.md 2.7): the 25 rows in GraphSoftBodyNodeTable.cs, every method
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
        // version 2 (wave 10)
        new("popsoftbodyevent", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "found:bool:out", "kind:int:out", "entity:int:out", "piece:int:out", "x:float:out", "y:float:out", "z:float:out", "value:float:out", "success:bool:out" }),
        new("setsoftbodybudget", GenericKind.Exec, new[] { "exec:exec:in", "maxPieces:int:in", "maxParticles:int:in", "maxRebuilds:int:in", "lifetime:float:in", "then:exec:out", "success:bool:out" }, new[] { "maxPieces=48", "maxParticles=20000", "maxRebuilds=2", "lifetime=12" }),
        new("clearsoftbodydebris", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("setsoftbodyasleep", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "asleep:bool:in", "then:exec:out", "success:bool:out" }),
        new("setsoftbodyeffects", GenericKind.Exec, new[] { "exec:exec:in", "enabled:bool:in", "then:exec:out", "success:bool:out" }, new[] { "enabled=true" }),
        new("repairallsoftbodies", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "repaired:int:out", "success:bool:out" }),
        new("getsoftbodydents", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "dentedEdges:int:out", "maxSetCm:float:out", "debrisPieces:int:out" }),
        // version 3 (wave 11)
        new("setsoftpressure", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "kPa:float:in", "then:exec:out", "success:bool:out" }),
        new("getsoftvolume", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "restCm3:float:out", "nowCm3:float:out", "ratio:float:out" }),
        new("grabsoftbody", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "reach:float:in", "targetX:float:in", "targetY:float:in", "targetZ:float:in", "then:exec:out", "grab:int:out", "success:bool:out" }, new[] { "reach=30" }),
        new("movesoftgrab", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "grab:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
        new("releasesoftgrab", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "grab:int:in", "then:exec:out", "success:bool:out" }),
        new("setsoftbuoyancy", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "density:float:in", "drag:float:in", "then:exec:out", "success:bool:out" }, new[] { "density=1000", "drag=1" }),
        new("getsoftsubmerged", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "fraction:float:out", "volumeCm3:float:out" }),
        new("setsoftcontact", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "softScale:float:in", "otherScale:float:in", "then:exec:out", "success:bool:out" }, new[] { "softScale=1", "otherScale=1" }),
        new("setsoftpairmode", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "other:int:in", "mode:int:in", "then:exec:out", "success:bool:out" }),
        new("addsoftjelly", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "preset:int:in", "then:exec:out", "success:bool:out" }, new[] { "preset=5" }),
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
        failures += TestV2Chain();
        failures += TestV3Chain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the soft-body rows are exactly the designed set of 25, 4 pure reads and 21 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "SoftBodyGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 25) return Fail($"expected 25 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 4) return Fail("only GetSoftBodyState, GetSoftBodyDents, GetSoftVolume and GetSoftSubmerged are reads");
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

    // The v2 rows in a chain: pop an event, hold and release, switch effects, repair all; Pop's value feeds a print and Dents feeds a branch.
    private static int TestV2Chain()
    {
        Console.WriteLine("Test: Pop Event, Budget, Asleep, Effects, Clear Debris and Repair All chain; Get Dents feeds a Branch");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE pe PopSoftBodyEvent\nNODE bu SetSoftBodyBudget\nNODE sa SetSoftBodyAsleep\nNODE ef SetSoftBodyEffects\n" +
                            "NODE cd ClearSoftBodyDebris\nNODE ra RepairAllSoftBodies\nNODE gd GetSoftBodyDents\nNODE br Branch\nNODE pf Print\nNODE pi PrintInt\n" +
                            "LINK tick.exec pe.exec\nLINK pe.then bu.exec\nLINK bu.then sa.exec\nLINK sa.then ef.exec\nLINK ef.then cd.exec\nLINK cd.then ra.exec\n" +
                            "LINK ra.then br.exec\nLINK gd.found br.cond\nLINK br.true pf.exec\nLINK pe.value pf.value\nLINK pf.then pi.exec\nLINK ra.repaired pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The v3 rows in a chain: make a jelly, inflate it, grab a vertex and move and release the grab with the id the grab returned, float it,
    // set its contact and a pair mode; Get Volume's found feeds a Branch and Get Submerged's fraction a print.
    private static int TestV3Chain()
    {
        Console.WriteLine("Test: Add Jelly, Pressure, Grab, Move, Release, Buoyancy, Contact and Pair Mode chain; the grab id feeds Move and Release; the reads feed a Branch and prints");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE aj AddSoftJelly\nNODE sp SetSoftPressure\nNODE gb GrabSoftBody\nNODE mg MoveSoftGrab\nNODE rg ReleaseSoftGrab\n" +
                            "NODE sb SetSoftBuoyancy\nNODE sc SetSoftContact\nNODE pm SetSoftPairMode\nNODE gv GetSoftVolume\nNODE gs GetSoftSubmerged\nNODE br Branch\n" +
                            "NODE pf Print\nNODE pi PrintInt\n" +
                            "LINK tick.exec aj.exec\nLINK aj.then sp.exec\nLINK sp.then gb.exec\nLINK gb.then mg.exec\nLINK gb.grab mg.grab\nLINK mg.then rg.exec\nLINK gb.grab rg.grab\n" +
                            "LINK rg.then sb.exec\nLINK sb.then sc.exec\nLINK sc.then pm.exec\nLINK pm.then br.exec\nLINK gv.found br.cond\nLINK br.true pf.exec\nLINK gs.fraction pf.value\n" +
                            "LINK pf.then pi.exec\nLINK gb.grab pi.value\nENTRY tick OnTick\n";
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
