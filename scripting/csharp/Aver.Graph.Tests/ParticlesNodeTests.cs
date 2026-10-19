// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the Particles v2 nodes (docs/plans/wave10/GPU_PARTICLES.md section 2.13): the 9 rows in
// GraphParticlesNodeTable.cs, every method on ParticlesGraph. Bare-process scope like MorphNodeTests: the wiring the
// table owns (a real method behind every row, pins, defaults, push-only versus pure, required attributes, valid IL),
// not what the particle system does with a request (tests/particles does that). The native provider being absent is not
// a failure: every call answers false / 0 there.
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class ParticlesNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_spawnparticlesat", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "dirX:float:in", "dirY:float:in", "dirZ:float:in", "count:int:in", "then:exec:out", "spawned:int:out" }, new[] { "dirX=0", "dirY=0", "dirZ=1", "count=1" }),
        new("an_setparticlefloat", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "value:float:in", "then:exec:out", "success:bool:out" }),
        new("an_setparticlevector", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
        new("an_setparticlecolor", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "r:float:in", "g:float:in", "b:float:in", "a:float:in", "then:exec:out", "success:bool:out" }, new[] { "a=1" }),
        new("an_burstemitter", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "count:int:in", "then:exec:out", "success:bool:out" }, new[] { "count=10" }),
        new("an_stopemitter", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "killParticles:bool:in", "then:exec:out", "success:bool:out" }, new[] { "killParticles=false" }),
        new("an_setemittermodifiers", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "rateScale:float:in", "sizeScale:float:in", "speedScale:float:in", "then:exec:out", "success:bool:out" }, new[] { "rateScale=1", "sizeScale=1", "speedScale=1" }),
        new("an_getemitterstate", GenericKind.Pure, new[] { "entity:int:in", "alive:int:out", "onGpu:bool:out", "found:bool:out" }),
        new("an_getparticlestats", GenericKind.Pure, new[] { "gpuEmitters:int:out", "cpuEmitters:int:out", "gpuParticles:int:out", "cpuParticles:int:out", "ok:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("Particles", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("Particles", Rows);
        failures += NodeRowKit.CheckIlValid("Particles", types);
        failures += NodeRowKit.CheckCompilerSplit("Particles", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestOlderEmitterKeysStayWhereTheyAre();
        failures += TestNoNativeProviderIsNotAFailure();
        failures += TestChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the particle rows are exactly the designed set of 9, two pure reads and exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "ParticlesGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 9) return Fail($"expected 9 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!pure.SequenceEqual(new[] { "an_getemitterstate", "an_getparticlestats" })) return Fail("only the two reads may be pure");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The effect path and the parameter name are attributes, required by name.
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: nodes missing effect= or param= fail to compile, naming it");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SpawnParticlesAt", "effect=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetParticleFloat", "param=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetParticleVector", "param=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AN_SetParticleColor", "param=", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // This family adds no duplicate of the four 0.7 emitter nodes: each key exists once, in the RenderProps family.
    private static int TestOlderEmitterKeysStayWhereTheyAre()
    {
        Console.WriteLine("Test: the four older emitter keys are still present exactly once, in RenderProps");
        foreach (var key in new[] { "an_setemitterenabled", "an_setemittereffect", "an_restartemitter", "an_spawnemitterat" })
        {
            var rows = GameSystemNodes.All.Where(s => string.Equals(s.Type, key, StringComparison.OrdinalIgnoreCase)).ToArray();
            if (rows.Length != 1) return Fail($"{key}: {rows.Length} rows");
            if (rows[0].Family != "RenderProps") return Fail($"{key} lives in family {rows[0].Family}, expected RenderProps");
        }
        if (GameSystemNodes.Duplicates.Any(d => d.Contains("emitter", StringComparison.OrdinalIgnoreCase)))
            return Fail("an emitter key is defined twice");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Without the native DLL every method answers false / 0 and none throws.
    private static int TestNoNativeProviderIsNotAFailure()
    {
        Console.WriteLine("Test: the ForGraph methods never throw without a native provider");
        try
        {
            ParticlesGraph.SpawnParticlesAtForGraph(0, 0, 0, 0, 0, 1, 3, "Particles/Sparks.ocparticle");
            ParticlesGraph.SpawnParticlesAtForGraph(0, 0, 0, 0, 0, 1, 0, "");
            ParticlesGraph.SetParticleFloatForGraph(1, "Intensity", 2f);
            ParticlesGraph.SetParticleFloatForGraph(1, "", 2f);
            ParticlesGraph.SetParticleVectorForGraph(1, "Wind", 1, 2, 3);
            ParticlesGraph.SetParticleColorForGraph(1, "Tint", 1, 0.5f, 0, 1);
            ParticlesGraph.BurstEmitterForGraph(1, 5);
            ParticlesGraph.BurstEmitterForGraph(1, -5);
            ParticlesGraph.StopEmitterForGraph(1, true);
            ParticlesGraph.SetEmitterModifiersForGraph(1, 1f, float.NaN, 1f);
            ParticlesGraph.GetEmitterStateForGraph(1, out _, out _);
            ParticlesGraph.GetParticleStatsForGraph(out _, out _, out _, out _);
        }
        catch (Exception e) when (!NodeRowKit.NativeMissing(e))
        {
            return Fail("a ForGraph method threw " + e.GetType().Name + ": " + e.Message);
        }
        // Argument guards answer without ever reaching the native side.
        if (ParticlesGraph.SpawnParticlesAtForGraph(0, 0, 0, 0, 0, 1, 0, "x.ocparticle") != 0) return Fail("count 0 must spawn 0");
        if (ParticlesGraph.SpawnParticlesAtForGraph(0, 0, 0, 0, 0, 1, 3, "") != 0) return Fail("an empty effect must spawn 0");
        if (ParticlesGraph.SetParticleFloatForGraph(1, "", 1f)) return Fail("an empty parameter name must answer false");
        if (ParticlesGraph.BurstEmitterForGraph(1, 0)) return Fail("a zero burst must answer false");
        if (ParticlesGraph.SetEmitterModifiersForGraph(1, float.NaN, 1f, 1f)) return Fail("a non-finite modifier must answer false");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The chain the design names: OnTick -> Spawn Particles At -> Set Particle Float -> Burst -> Modifiers -> Stop, with a state read.
    private static int TestChain()
    {
        Console.WriteLine("Test: OnTick -> Spawn Particles At -> Set Particle Float -> Burst -> Modifiers -> Stop compiles and its IL is valid");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE sp AN_SpawnParticlesAt effect=Particles/Sparks.ocparticle\nNODE sf AN_SetParticleFloat param=Intensity\n" +
                            "NODE bu AN_BurstEmitter\nNODE mo AN_SetEmitterModifiers\nNODE st AN_StopEmitter\nNODE gs AN_GetEmitterState\nNODE pr PrintInt\n" +
                            "LINK tick.exec sp.exec\nLINK sp.then sf.exec\nLINK sf.then bu.exec\nLINK bu.then mo.exec\nLINK mo.then st.exec\nLINK st.then pr.exec\nLINK gs.alive pr.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
