// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the light, camera and particle-emitter nodes (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2):
// the 14 rows in GraphRenderPropsNodeTable.cs, every method on RenderPropsGraph. Bare-process scope like MorphNodeTests: the
// wiring the table owns (a real method behind every row, pins, defaults, push-only versus pure, required attributes, valid
// IL) plus the pure maths in Entity. What a field write does to a live scene needs the scene DLL, so those checks treat
// "unavailable" as a skip, not a failure.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class RenderPropsNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_setlightintensity", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "lux:float:in", "then:exec:out", "success:bool:out" }),
        new("an_getlightintensity", GenericKind.Pure, new[] { "entity:int:in", "lux:float:out", "found:bool:out" }),
        new("an_setlightcolor", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "r:float:in", "g:float:in", "b:float:in", "then:exec:out", "success:bool:out" }),
        new("an_setlightrange", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "rangeCm:float:in", "then:exec:out", "success:bool:out" }),
        new("an_setlightcone", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "innerDeg:float:in", "outerDeg:float:in", "then:exec:out", "success:bool:out" }, new[] { "innerDeg=20", "outerDeg=35" }),
        new("an_setlightshadows", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "enabled:bool:in", "then:exec:out", "success:bool:out" }, new[] { "enabled=true" }),
        new("an_setcamerafov", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "fovDeg:float:in", "then:exec:out", "success:bool:out" }, new[] { "fovDeg=60" }),
        new("an_getcamerafov", GenericKind.Pure, new[] { "entity:int:in", "fovDeg:float:out", "found:bool:out" }),
        new("an_setcameraclip", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "nearCm:float:in", "farCm:float:in", "then:exec:out", "success:bool:out" }, new[] { "nearCm=5", "farCm=500000" }),
        new("an_setcamerapriority", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "priority:int:in", "then:exec:out", "success:bool:out" }),
        new("an_setemitterenabled", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "enabled:bool:in", "then:exec:out", "success:bool:out" }, new[] { "enabled=true" }),
        new("an_setemittereffect", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("an_restartemitter", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("an_spawnemitterat", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "lifetime:float:in", "then:exec:out", "entity:int:out" }, new[] { "lifetime=3" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("render props", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("render props", Rows);
        failures += NodeRowKit.CheckIlValid("render props", types);
        failures += NodeRowKit.CheckCompilerSplit("render props", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestLightCameraEmitterChain();
        failures += TestReadsFeedLaterNodes();
        failures += TestConeCosines();
        failures += TestUnavailableSceneIsNotAFailure();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the render-props rows are exactly the designed set of 14, 2 pure reads and 12 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "RenderPropsGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 14) return Fail($"expected 14 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!pure.SequenceEqual(new[] { "an_getcamerafov", "an_getlightintensity" })) return Fail("only the two reads may be pure");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The effect path is the only attribute; both nodes that take it refuse to compile without it.
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: Set Emitter Effect and Spawn Emitter At fail to compile without effect=, naming it");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetEmitterEffect", "effect=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SpawnEmitterAt", "effect=", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // A flicker-and-burst handler: every write chained exec to exec, ending in a spawn whose entity feeds a print.
    private static int TestLightCameraEmitterChain()
    {
        Console.WriteLine("Test: OnTick -> every light, camera and emitter write in one chain compiles and its IL is valid");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE a AN_SetLightIntensity\nNODE b AN_SetLightColor\nNODE c AN_SetLightRange\nNODE d AN_SetLightCone\n" +
                            "NODE e AN_SetLightShadows\nNODE f AN_SetCameraFov\nNODE g AN_SetCameraClip\nNODE h AN_SetCameraPriority\nNODE i AN_SetEmitterEnabled\n" +
                            "NODE j AN_SetEmitterEffect effect=fx/spark.ocparticle\nNODE k AN_RestartEmitter\nNODE l AN_SpawnEmitterAt effect=fx/burst.ocparticle\nNODE p PrintInt\n" +
                            "LINK tick.exec a.exec\nLINK a.then b.exec\nLINK b.then c.exec\nLINK c.then d.exec\nLINK d.then e.exec\nLINK e.then f.exec\nLINK f.then g.exec\n" +
                            "LINK g.then h.exec\nLINK h.then i.exec\nLINK i.then j.exec\nLINK j.then k.exec\nLINK k.then l.exec\nLINK l.then p.exec\nLINK l.entity p.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestReadsFeedLaterNodes()
    {
        Console.WriteLine("Test: Get Light Intensity and Get Camera Fov feed a Branch and prints without being in the exec chain");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE gl AN_GetLightIntensity\nNODE gc AN_GetCameraFov\nNODE br Branch\nNODE pa Print\nNODE pb Print\n" +
                            "LINK tick.exec br.exec\nLINK gl.found br.cond\nLINK br.true pa.exec\nLINK gl.lux pa.value\nLINK pa.then pb.exec\nLINK gc.fovDeg pb.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The light stores cosines of the half-angles; the inner cone may never exceed the outer.
    private static int TestConeCosines()
    {
        Console.WriteLine("Test: LightConeCosines converts degrees to cosines and keeps the inner cone inside the outer");
        static bool Near(float a, float b) => MathF.Abs(a - b) < 1e-5f;
        Entity.LightConeCosines(20f, 35f, out float ic, out float oc);
        if (!Near(ic, MathF.Cos(20f * MathF.PI / 180f)) || !Near(oc, MathF.Cos(35f * MathF.PI / 180f))) return Fail($"20/35 gave {ic}, {oc}");
        if (ic <= oc) return Fail("a narrower inner cone must have the larger cosine");
        Entity.LightConeCosines(50f, 30f, out ic, out oc);
        if (!Near(ic, oc)) return Fail($"inner 50 over outer 30 must clamp to the outer, got {ic} vs {oc}");
        Entity.LightConeCosines(-10f, 200f, out ic, out oc);
        if (!Near(ic, 1f)) return Fail($"a negative inner angle must clamp to 0, got cos {ic}");
        if (oc < 0f || oc > 0.02f) return Fail($"a 200 degree outer must clamp just under 90, got cos {oc}");
        Entity.LightConeCosines(float.NaN, float.NaN, out ic, out oc);
        if (!Near(ic, 1f) || !Near(oc, 1f)) return Fail("a NaN angle must read as 0");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A dead handle answers false from every member, never throws. Without the scene DLL there is nothing to ask: skip.
    private static int TestUnavailableSceneIsNotAFailure()
    {
        Console.WriteLine("Test: a dead entity answers false from every render-props member (skipped when the scene DLL is absent)");
        try
        {
            Entity dead = Entity.None;
            bool any = dead.SetLightIntensity(10f) || dead.SetLightColour(new Aver.Scene.Vec3(1, 1, 1)) || dead.SetLightRange(10f) ||
                       dead.SetLightCone(10f, 20f) || dead.SetLightShadows(true) || dead.SetCameraFov(60f) || dead.SetCameraClip(5f, 100f) ||
                       dead.SetCameraPriority(1) || dead.SetEmitterEnabled(true) || dead.SetEmitterEffect("fx/a.ocparticle") || dead.RestartEmitter() ||
                       dead.TryGetLightIntensity(out _) || dead.TryGetCameraFov(out _);
            if (any) return Fail("a member reported success on Entity.None");
            Console.WriteLine("  PASS");
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e))
        {
            Console.WriteLine("  SKIP: scene library not loadable in this process");
        }
        return 0;
    }
}
