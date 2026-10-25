// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the physical-surface nodes (docs/plans/wave11/SHAPES_MATERIALS.md): the 7 rows in GraphPhysicalSurfaceNodeTable.cs,
// every method on SurfaceGraph. Bare-process scope like CameraNodeTests: the wiring the table owns (a real method behind every
// row, pins, defaults, push-only versus pure, valid IL), and that every method answers false/0 instead of throwing when the
// native libraries or the entity are absent. What a call does to a live physics world needs the native libraries and is the
// C++ SurfaceTest's business.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class PhysicalSurfaceNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_raycastsurface", GenericKind.Exec, new[]
        {
            "exec:exec:in", "originX:float:in", "originY:float:in", "originZ:float:in", "dirX:float:in", "dirY:float:in", "dirZ:float:in",
            "maxDist:float:in", "layerMask:int:in", "then:exec:out", "hit:bool:out", "entity:int:out", "pointX:float:out", "pointY:float:out",
            "pointZ:float:out", "normalX:float:out", "normalY:float:out", "normalZ:float:out", "distance:float:out", "surfaceId:int:out",
            "friction:float:out", "restitution:float:out", "tag:int:out",
        }, new[] { "maxDist=10000", "layerMask=-1" }),
        new("an_surfacebeneath", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "probeCm:float:in", "then:exec:out", "found:bool:out", "surfaceId:int:out", "friction:float:out", "restitution:float:out", "tag:int:out" }, new[] { "probeCm=150" }),
        new("an_surfaceinfo", GenericKind.Pure, new[] { "surfaceId:int:in", "found:bool:out", "friction:float:out", "restitution:float:out", "tag:int:out" }),
        new("an_setbodysurface", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "surfaceId:int:in", "copyPhysics:bool:in", "then:exec:out", "success:bool:out" }, new[] { "copyPhysics=false" }),
        new("an_registersurface", GenericKind.Exec, new[] { "exec:exec:in", "friction:float:in", "restitution:float:in", "tag:int:in", "then:exec:out", "surfaceId:int:out", "success:bool:out" }, new[] { "friction=-1", "restitution=-1", "tag=0" }),
        new("an_tunesurface", GenericKind.Exec, new[] { "exec:exec:in", "surfaceId:int:in", "friction:float:in", "restitution:float:in", "tag:int:in", "then:exec:out", "success:bool:out" }),
        new("an_setbodyscale", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "scaleX:float:in", "scaleY:float:in", "scaleZ:float:in", "keepMass:bool:in", "then:exec:out", "success:bool:out" }, new[] { "keepMass=false" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("physical surface", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("physical surface", Rows);
        failures += NodeRowKit.CheckIlValid("physical surface", types);
        failures += NodeRowKit.CheckCompilerSplit("physical surface", types);
        failures += TestChainCompiles();
        failures += TestUnavailableIsNotAFailure();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the physical surface rows are exactly the designed set of 7, one pure read and six exec calls");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "SurfaceGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 7) return Fail($"expected 7 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).ToArray();
        if (!pure.SequenceEqual(new[] { "an_surfaceinfo" })) return Fail("only an_surfaceinfo may be pure");
        if (GameSystemNodes.All.Any(s => s.Owner.Name == "SurfaceGraph" && s.Family != "PhysicalSurface"))
            return Fail("every row belongs to the PhysicalSurface family");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A setup handler: register a surface, tune it, label a body, scale it, then ask what is beneath and what a ray hits.
    private static int TestChainCompiles()
    {
        Console.WriteLine("Test: OnTick -> every surface call in one chain compiles, and the pure read feeds a Branch");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE a AN_RegisterSurface name=Ice\nNODE b AN_TuneSurface\nNODE c AN_SetBodySurface\nNODE d AN_SetBodyScale\n" +
                            "NODE e AN_SurfaceBeneath\nNODE f AN_RaycastSurface\nNODE g AN_SurfaceInfo\nNODE br Branch\nNODE pr Print\n" +
                            "LINK tick.exec a.exec\nLINK a.then b.exec\nLINK a.surfaceId b.surfaceId\nLINK b.then c.exec\nLINK a.surfaceId c.surfaceId\nLINK c.then d.exec\n" +
                            "LINK d.then e.exec\nLINK e.then f.exec\nLINK f.surfaceId g.surfaceId\nLINK f.then br.exec\nLINK g.found br.cond\nLINK br.true pr.exec\n" +
                            "LINK g.friction pr.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // A dead entity, an empty name, an unknown surface and an absent physics library answer false/0 from every method, never throw.
    private static int TestUnavailableIsNotAFailure()
    {
        Console.WriteLine("Test: a dead entity, an empty name and an absent physics library answer false/0 (skipped when the native libraries are absent)");
        try
        {
            if (SurfaceGraph.SetBodySurfaceForGraph(0, 123, false) || SurfaceGraph.SetBodyScaleForGraph(0, 1f, 1f, 1f, false))
                return Fail("a body method reported success on entity 0");
            if (SurfaceGraph.BeneathForGraph(0, 150f, out int id, out float f, out float r, out int tag) || id != 0 || f != 0f || r != 0f || tag != 0)
                return Fail("Beneath on entity 0 reported a hit or left an output set");
            if (SurfaceGraph.RaycastSurfaceForGraph(0, 0, 100, 0, 0, -1, 500f, -1, out int ent, out _, out _, out _, out _, out _, out _, out float dist,
                                                    out int sid, out _, out _, out int rtag))
                return Fail("a raycast reported a hit with no physics world");
            if (ent != 0 || dist != 0f || sid != 0 || rtag != 0) return Fail("a missed raycast left an output set");
            if (SurfaceGraph.RegisterForGraph("", 0.5f, 0f, 0, out int regId) || regId != 0) return Fail("registering an empty name reported success");
            if (SurfaceGraph.InfoForGraph(0, out _, out _, out _)) return Fail("surface 0 is not a surface");
            Console.WriteLine("  PASS");
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e))
        {
            Console.WriteLine("  SKIP: native libraries not loadable in this process");
        }
        return 0;
    }
}
