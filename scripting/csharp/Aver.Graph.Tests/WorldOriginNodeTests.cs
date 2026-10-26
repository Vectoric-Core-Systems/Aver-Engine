// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the floating-origin nodes (docs/plans/wave11/LARGE_WORLD.md 2.5): the 3 rows in GraphWorldOriginNodeTable.cs, every method on
// WorldOriginGraph. Bare-process scope like TerrainNodeTests: the wiring the table owns (a real method behind every row, pins, defaults,
// valid IL), the region arithmetic the nodes are built on, and the "never throw, answer zero" rule with no host. What the engine reports as
// the origin is tests/framework_origin's and OriginParticipantsTest's business.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class WorldOriginNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_getworldorigin", GenericKind.Pure, new[] { "regionX:int:out", "regionY:int:out", "generation:int:out", "enabled:bool:out" }),
        new("an_getworldregion", GenericKind.Pure, new[] { "entity:int:in", "regionX:int:out", "regionY:int:out", "regionZ:int:out", "x:float:out", "y:float:out", "z:float:out" }),
        new("an_setworldregion", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "regionX:int:in", "regionY:int:in", "regionZ:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("world origin", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("world origin", Rows);
        failures += NodeRowKit.CheckIlValid("world origin", types);
        failures += NodeRowKit.CheckCompilerSplit("world origin", types);
        failures += TestNoAttributesNeeded();
        failures += TestRegionArithmetic();
        failures += TestNothingThrowsWithoutAHost();
        failures += TestChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the world origin rows are exactly the designed set of 3: two pure reads and one exec");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "WorldOriginGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 3) return Fail($"expected 3 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 2 || Rows.Count(r => r.Kind == GenericKind.Exec) != 1) return Fail("two pure reads and one exec setter");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestNoAttributesNeeded()
    {
        Console.WriteLine("Test: none of the three nodes needs an attribute");
        int f = 0;
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_GetWorldOrigin", exec: false);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_GetWorldRegion", exec: false);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AN_SetWorldRegion", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // The nodes carry a region and an offset from its centre. Region r is centred on r * 1,638,400 cm and covers 819,200 either side.
    private static int TestRegionArithmetic()
    {
        Console.WriteLine("Test: an absolute point splits into a region and a centre-relative offset, and back, exactly where a float can hold it");
        const long step = 1_638_400;
        (Double3 Abs, int Rx, int Ry, int Rz, float X, float Y, float Z)[] cases =
        {
            (new Double3(0, 0, 0), 0, 0, 0, 0f, 0f, 0f),
            (new Double3(10_000_000.0, -2_500_000.0, 120.5), 6, -2, 0, 10_000_000f - 6 * step, -2_500_000f + 2 * step, 120.5f),   // 100 km, -25 km
            (new Double3(819_199.75, -819_200.0, 0), 0, 0, 0, 819_199.75f, -819_200f, 0f),                                       // region 0's two edges
            (new Double3(819_200.0, 819_200.25, 0), 1, 1, 0, -819_200f, -819_199.75f, 0f),                                       // one step past
            (new Double3(123_456_789.123, 0, 0), 75, 0, 0, (float)(123_456_789.123 - 75.0 * step), 0f, 0f),
        };
        foreach (var c in cases)
        {
            WorldOriginGraph.RegionOf(c.Abs, out int rx, out int ry, out int rz, out float x, out float y, out float z);
            if (rx != c.Rx || ry != c.Ry || rz != c.Rz) return Fail($"{c.Abs}: region ({rx}, {ry}, {rz}) but expected ({c.Rx}, {c.Ry}, {c.Rz})");
            if (x != c.X || y != c.Y || z != c.Z) return Fail($"{c.Abs}: offset ({x:R}, {y:R}, {z:R}) but expected ({c.X:R}, {c.Y:R}, {c.Z:R})");
            Double3 back = WorldOriginGraph.AbsoluteOf(rx, ry, rz, x, y, z);
            // A float offset up to 819,200 cm holds 0.0625 cm.
            if (Math.Abs(back.X - c.Abs.X) > 0.0625 || Math.Abs(back.Y - c.Abs.Y) > 0.0625 || Math.Abs(back.Z - c.Abs.Z) > 0.0625)
                return Fail($"{c.Abs}: came back as {back}");
        }
        // The centre-relative offset stays inside the region for every point of a sweep across several seams.
        for (long cm = -5 * step; cm <= 5 * step; cm += 99_991)
        {
            WorldOriginGraph.RegionOf(new Double3(cm, cm, 0), out int rx, out _, out _, out float x, out _, out _);
            if (x < -819_200f || x > 819_200f) return Fail($"offset {x} at {cm} is outside the region");
            if (Math.Abs((double)rx * step + x - cm) > 0.0625) return Fail($"region {rx} and offset {x} do not rebuild {cm}");
        }
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestNothingThrowsWithoutAHost()
    {
        Console.WriteLine("Test: with no host the origin reads as (0, 0), generation 0, off, and the entity nodes answer zero or false");
        int rx = WorldOriginGraph.GetOriginForGraph(out int ry, out int gen, out bool enabled);
        if (rx != 0 || ry != 0 || gen != 0 || enabled) return Fail($"origin read ({rx}, {ry}) generation {gen} enabled {enabled}");
        int r = WorldOriginGraph.GetRegionForGraph(0, out int regionY, out int regionZ, out float x, out float y, out float z);
        if (r != 0 || regionY != 0 || regionZ != 0 || x != 0f || y != 0f || z != 0f) return Fail("region of entity 0 should be all zero");
        if (WorldOriginGraph.SetRegionForGraph(0, 1, 0, 0, 0f, 0f, 0f)) return Fail("setting entity 0 should be false");
        if (WorldOriginGraph.SetRegionForGraph(7, 1, 0, 0, float.NaN, 0f, 0f)) return Fail("a NaN offset should be refused");
        if (WorldOrigin.RequestShift(1, 0)) return Fail("a shift request with no host should be refused");
        // The managed conversions are plain arithmetic about a zero origin.
        var p = new Aver.Scene.Vec3(123.5f, -77.25f, 9f);
        Double3 a = WorldOrigin.ToAbsolute(p);
        Aver.Scene.Vec3 back = WorldOrigin.ToLocal(a);
        if (a.X != 123.5 || a.Y != -77.25 || a.Z != 9.0 || back != p) return Fail("ToAbsolute / ToLocal about a zero origin should be the identity");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A graph that reads the origin and an entity's region and feeds them onward compiles and runs without a host.
    private static int TestChain()
    {
        Console.WriteLine("Test: the origin and region reads feed a branch and a setter in one graph");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE wo AN_GetWorldOrigin\nNODE wr AN_GetWorldRegion\nNODE sr AN_SetWorldRegion\n" +
                            "NODE br Branch\nNODE pi PrintInt\nNODE pf Print\n" +
                            "LINK tick.exec br.exec\nLINK wo.enabled br.cond\nLINK br.true pi.exec\nLINK wo.regionX pi.value\n" +
                            "LINK pi.then sr.exec\nLINK wr.regionX sr.regionX\nLINK wr.regionY sr.regionY\nLINK wr.regionZ sr.regionZ\n" +
                            "LINK wr.x sr.x\nLINK wr.y sr.y\nLINK wr.z sr.z\nLINK sr.then pf.exec\nLINK wr.x pf.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
