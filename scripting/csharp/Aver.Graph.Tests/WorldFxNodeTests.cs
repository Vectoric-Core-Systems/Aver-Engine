// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the sun, fog, sky-light and post-layer nodes (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.4): the
// 12 rows in GraphWorldFxNodeTable.cs, every method on WorldFxGraph. Bare-process scope like IkNodeTests: the wiring the table
// owns (a real method behind every row, pins, defaults, push-only, required attributes, valid IL), the key-name map, and the
// "never throw" rule when the native DLL is absent. What the ABI does with a value is tests/framework_worldfx's business.
using System;
using System.Linq;
using System.Reflection;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class WorldFxNodeTests
{
    private static readonly Row[] Rows =
    {
        new("setsundirection", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
        new("setsunintensity", GenericKind.Exec, new[] { "exec:exec:in", "lux:float:in", "then:exec:out", "success:bool:out" }),
        new("setsuncolor", GenericKind.Exec, new[] { "exec:exec:in", "r:float:in", "g:float:in", "b:float:in", "temperatureK:float:in", "then:exec:out", "success:bool:out" }, new[] { "temperatureK=0" }),
        new("setfogdensity", GenericKind.Exec, new[] { "exec:exec:in", "density:float:in", "then:exec:out", "success:bool:out" }),
        new("setfogcolor", GenericKind.Exec, new[] { "exec:exec:in", "r:float:in", "g:float:in", "b:float:in", "then:exec:out", "success:bool:out" }),
        new("setfogshape", GenericKind.Exec, new[] { "exec:exec:in", "falloff:float:in", "height:float:in", "start:float:in", "maxOpacity:float:in", "then:exec:out", "success:bool:out" }, new[] { "maxOpacity=1" }),
        new("setskylight", GenericKind.Exec, new[] { "exec:exec:in", "multiplier:float:in", "then:exec:out", "success:bool:out" }, new[] { "multiplier=1" }),
        new("clearenvironment", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("setpostvalue", GenericKind.Exec, new[] { "exec:exec:in", "v0:float:in", "v1:float:in", "v2:float:in", "then:exec:out", "success:bool:out" }),
        new("setpostlayerweight", GenericKind.Exec, new[] { "exec:exec:in", "weight:float:in", "blendSeconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "weight=1", "blendSeconds=0" }),
        new("clearpostlayer", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("setpostvolumeweight", GenericKind.Exec, new[] { "exec:exec:in", "weight:float:in", "blendSeconds:float:in", "then:exec:out", "success:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("world effects", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("world effects", Rows);
        failures += NodeRowKit.CheckIlValid("world effects", types);
        failures += NodeRowKit.CheckCompilerSplit("world effects", types);
        failures += TestAttributes();
        failures += TestEnvironmentKeyNames();
        failures += TestNothingThrows();
        failures += TestEffectsChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static Type? Internal(string name) => typeof(Sky).Assembly.GetType("Aver.Framework." + name);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the world effects rows are exactly the designed set of 12, all exec");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "WorldFxGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 12) return Fail($"expected 12 rows, test lists {Rows.Length}");
        if (Rows.Any(r => r.Kind != GenericKind.Exec)) return Fail("every world effects node is an exec node");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Layer and key are the ones a call means nothing without; layer, environment key and the other optional names default.
    private static int TestAttributes()
    {
        Console.WriteLine("Test: Set Post Value needs key=, Set Post Volume Weight needs volume=; every layer= and the environment key= are optional");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n SetPostValue", "key=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n SetPostVolumeWeight", "volume=", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetPostValue key=saturation", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetPostLayerWeight", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n ClearPostLayer", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n ClearEnvironment", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetSunDirection", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // The names a clearenvironment node takes map onto the ABI keys 1..11 (the numbers the C header pins), case-blind.
    private static int TestEnvironmentKeyNames()
    {
        Console.WriteLine("Test: the dotted environment names map to the eleven ABI keys, with aliases, and nonsense is refused without a native call");
        var names = Internal("EnvKeyNames");
        var parse = names?.GetMethod("TryParse", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        if (parse == null) return Fail("Aver.Framework.EnvKeyNames.TryParse was not found by reflection");
        (string Name, int Key)[] cases =
        {
            ("sun.dir", 1), ("sun.direction", 1), ("sun.lux", 2), ("sun.intensity", 2), ("sun.color", 3), ("sun.colour", 3),
            ("sun.temp", 4), ("sun.temperature", 4), ("fog.density", 5), ("fog.color", 6), ("fog.falloff", 7), ("fog.height", 8),
            ("fog.start", 9), ("fog.maxopacity", 10), ("fog.maxopac", 10), ("sky.light", 11), ("SUN.DIR", 1), ("  Fog.Density ", 5),
        };
        foreach (var (name, key) in cases)
        {
            var args = new object?[] { name, null };
            bool ok = (bool)parse.Invoke(null, args)!;
            if (!ok || Convert.ToInt32(args[1]) != key) return Fail($"'{name}' should map to key {key}, got ok={ok} key={args[1]}");
        }
        foreach (var bad in new[] { "", "sun", "fog", "sun.dir.x", "moon.dir", "weather" })
        {
            var args = new object?[] { bad, null };
            if ((bool)parse.Invoke(null, args)!) return Fail($"'{bad}' should not be an environment key");
        }
        // The public surface refuses before it reaches the DLL, so these answer in a bare process too.
        if (Sky.ClearOverride("nonsense")) return Fail("Sky.ClearOverride accepted a name that is not a key");
        if (PostFx.SetValue("layer", "  ", 1f)) return Fail("PostFx.SetValue accepted an empty key");
        if (PostFx.SetVolumeWeight("", 1f)) return Fail("PostFx.SetVolumeWeight accepted an empty volume");
        if (PostFx.SetVolumeWeight("v", -1f)) return Fail("PostFx.SetVolumeWeight accepted a negative weight, which the ABI reads as 'remove'");
        if (PostFx.ClearVolumeWeight("")) return Fail("PostFx.ClearVolumeWeight accepted an empty volume");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Failure returns false and logs once; with no native DLL every call fails, and none may throw.
    private static int TestNothingThrows()
    {
        Console.WriteLine("Test: every WorldFxGraph method returns instead of throwing, native DLL or not");
        var owner = Internal("WorldFxGraph");
        if (owner == null) return Fail("Aver.Framework.WorldFxGraph was not found by reflection");
        int called = 0;
        foreach (var m in owner.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public).Where(m => m.Name.EndsWith("ForGraph")))
        {
            var args = m.GetParameters().Select(p => p.ParameterType == typeof(string) ? (object)"" : p.ParameterType == typeof(float) ? 1f : Activator.CreateInstance(p.ParameterType)!).ToArray();
            try { m.Invoke(null, args); ++called; }
            catch (TargetInvocationException e) { return Fail($"{m.Name} threw {e.InnerException?.GetType().Name}: {e.InnerException?.Message}"); }
        }
        if (called != Rows.Length) return Fail($"called {called} methods, the table has {Rows.Length} rows");
        Console.WriteLine($"  PASS: {called} methods");
        return 0;
    }

    // A sunset in one chain: sun, fog, sky light, then a post layer that fades in, gated on its own success, then cleared.
    private static int TestEffectsChain()
    {
        Console.WriteLine("Test: sun, fog, sky light and post nodes chain exec to exec; a success pin gates a branch");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE sd SetSunDirection\nNODE sc SetSunColor\nNODE si SetSunIntensity\nNODE fd SetFogDensity\nNODE fc SetFogColor\n" +
                            "NODE fs SetFogShape\nNODE sl SetSkyLight\nNODE pv SetPostValue layer=dusk key=saturation\nNODE pw SetPostLayerWeight layer=dusk\nNODE vw SetPostVolumeWeight volume=Interior\n" +
                            "NODE br Branch\nNODE pr Print\nNODE cp ClearPostLayer layer=dusk\nNODE ce ClearEnvironment key=sun.dir\n" +
                            "LINK tick.exec sd.exec\nLINK sd.then sc.exec\nLINK sc.then si.exec\nLINK si.then fd.exec\nLINK fd.then fc.exec\nLINK fc.then fs.exec\nLINK fs.then sl.exec\n" +
                            "LINK sl.then pv.exec\nLINK pv.then pw.exec\nLINK pw.then vw.exec\nLINK vw.then br.exec\nLINK pv.success br.cond\nLINK br.true cp.exec\nLINK br.false pr.exec\n" +
                            "LINK cp.then ce.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
