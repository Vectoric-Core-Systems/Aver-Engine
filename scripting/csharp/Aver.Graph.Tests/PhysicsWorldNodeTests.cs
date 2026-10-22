// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the physics-world nodes (docs/plans/wave11/WORLD_STATE_DEBUG.md section 2.12): the 7 rows in
// GraphPhysicsWorldNodeTable.cs and every method on PhysicsWorldGraph. Bare-process scope like VehicleNodeTests: the wiring the
// table owns (a real method behind every row, pins, defaults, push-only versus pure, required attributes, valid IL) and the
// no-throw rule, not what Jolt does with a snapshot (tests/physics' PhysicsStateTest does that).
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;
using AP = Aver.Physics;

[AutoRun]
static class PhysicsWorldNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_physicssetsetting", GenericKind.Exec, new[] { "exec:exec:in", "value:float:in", "then:exec:out", "success:bool:out" }),
        new("an_physicsgetsetting", GenericKind.Pure, new[] { "found:bool:out", "value:float:out" }),
        new("an_physicssnapshot", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "snapshot:int:out", "success:bool:out" }),
        new("an_physicsrestore", GenericKind.Exec, new[] { "exec:exec:in", "snapshot:int:in", "then:exec:out", "success:bool:out" }),
        new("an_physicsreleasesnapshot", GenericKind.Exec, new[] { "exec:exec:in", "snapshot:int:in", "then:exec:out", "success:bool:out" }),
        new("an_physicsstatehash", GenericKind.Pure, new[] { "low:int:out", "high:int:out" }),
        new("an_physicsstats", GenericKind.Pure, new[] { "bodies:int:out", "activeDynamic:int:out", "steps:int:out", "stepMs:float:out", "updateErrors:int:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("physics world", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("physics world", Rows);
        failures += NodeRowKit.CheckIlValid("physics world", types);
        failures += NodeRowKit.CheckCompilerSplit("physics world", types);
        failures += TestAttributes();
        failures += TestPartsParse();
        failures += TestFailuresReturnFalseRatherThanThrow();
        failures += TestSnapshotChainCompiles();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the physics-world rows are exactly the designed set of 7, 3 pure reads and 4 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "PhysicsWorldGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 7) return Fail($"expected 7 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!pure.SequenceEqual(new[] { "an_physicsgetsetting", "an_physicsstatehash", "an_physicsstats" })) return Fail("only the three reads may be pure");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The setting name is required; the snapshot's parts are optional (empty means all).
    private static int TestAttributes()
    {
        Console.WriteLine("Test: the setting attribute is required, the snapshot parts attribute is optional");
        int bad = 0;
        bad += NodeRowKit.CheckMissingAttribute("NODE n AN_PhysicsSetSetting", "setting", exec: true);
        bad += NodeRowKit.CheckMissingAttribute("NODE n AN_PhysicsGetSetting", "setting", exec: false);
        bad += NodeRowKit.CheckOptionalAttribute("NODE n AN_PhysicsSnapshot", exec: true);
        if (bad == 0) Console.WriteLine("  PASS");
        return bad;
    }

    private static int TestPartsParse()
    {
        Console.WriteLine("Test: the parts attribute reads names separated by commas, plus signs, bars or spaces; empty means all");
        bool ok = PhysicsWorldGraph.TryParseParts(null, out var p) && p == AP.SnapshotParts.All &&
                  PhysicsWorldGraph.TryParseParts("  ", out p) && p == AP.SnapshotParts.All &&
                  PhysicsWorldGraph.TryParseParts("all", out p) && p == AP.SnapshotParts.All &&
                  PhysicsWorldGraph.TryParseParts("bodies,contacts", out p) && p == (AP.SnapshotParts.Bodies | AP.SnapshotParts.Contacts) &&
                  PhysicsWorldGraph.TryParseParts("Bodies + Engine", out p) && p == (AP.SnapshotParts.Bodies | AP.SnapshotParts.Engine) &&
                  PhysicsWorldGraph.TryParseParts("characters|statics", out p) && p == (AP.SnapshotParts.Characters | AP.SnapshotParts.Statics);
        if (!ok) return Fail("a valid parts string was read wrongly");
        if (PhysicsWorldGraph.TryParseParts("bodies,wheels", out p) || p != 0) return Fail("an unknown part must be refused, and leave no parts");
        Console.WriteLine("  PASS");
        return 0;
    }

    // With no physics library or no world every method answers false/0 and the table stays empty; nothing throws.
    private static int TestFailuresReturnFalseRatherThanThrow()
    {
        Console.WriteLine("Test: every method answers false/0 rather than throw for a bad name, a dead snapshot or no world");
        PhysicsWorldGraph.ResetForTests();
        try
        {
            if (PhysicsWorldGraph.SetSettingForGraph("noSuchSetting", 1f)) return Fail("an unknown setting must be refused");
            if (PhysicsWorldGraph.SetSettingForGraph("", 1f)) return Fail("an empty setting name must be refused");
            if (PhysicsWorldGraph.GetSettingForGraph("noSuchSetting", out float v) || v != 0f) return Fail("an unknown setting reads as not found, value 0");
            if (PhysicsWorldGraph.SnapshotForGraph("wheels", out int snap) || snap != 0) return Fail("an unknown part must be refused, snapshot 0");
            if (PhysicsWorldGraph.RestoreForGraph(999)) return Fail("restoring a snapshot that was never taken must be false");
            if (PhysicsWorldGraph.ReleaseSnapshotForGraph(999)) return Fail("releasing a snapshot that was never taken must be false");
            if (PhysicsWorldGraph.HeldForTests != 0) return Fail("a refused call must not put anything in the table");
            // These two touch the native library when it is present; without it they answer 0.
            PhysicsWorldGraph.StateHashForGraph(out _);
            PhysicsWorldGraph.StatsForGraph(out _, out _, out _, out _);
            // A valid part with no world (or no library) is false and leaves the table empty.
            if (PhysicsWorldGraph.SnapshotForGraph("bodies", out snap))
            {
                Console.WriteLine("  SKIP: a physics world is live in this process");
                PhysicsWorldGraph.ReleaseSnapshotForGraph(snap);
                return 0;
            }
            if (snap != 0 || PhysicsWorldGraph.HeldForTests != 0) return Fail("a failed snapshot must be 0 and leave the table empty");
        }
        catch (Exception e)
        {
            return Fail($"a physics-world call must answer false, not throw, without a world: {e.GetType().Name}: {e.Message}");
        }
        Console.WriteLine("  PASS");
        return 0;
    }

    // A snapshot's id feeds Restore and Release in one exec chain; the pure hash and stats compile beside it.
    private static int TestSnapshotChainCompiles()
    {
        Console.WriteLine("Test: Snapshot's id feeds Restore and Release in one exec chain, and the pure reads feed prints, and it compiles");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE ss AN_PhysicsSetSetting setting=velocitySteps\nNODE s AN_PhysicsSnapshot parts=bodies\nNODE r AN_PhysicsRestore\nNODE d AN_PhysicsReleaseSnapshot\n" +
                            "NODE h AN_PhysicsStateHash\nNODE st AN_PhysicsStats\nNODE pe PrintInt\nNODE pf PrintInt\n" +
                            "LINK tick.exec ss.exec\nLINK ss.then s.exec\nLINK s.then r.exec\nLINK s.snapshot r.snapshot\nLINK r.then d.exec\nLINK s.snapshot d.snapshot\n" +
                            "LINK d.then pe.exec\nLINK h.low pe.value\nLINK pe.then pf.exec\nLINK st.bodies pf.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
