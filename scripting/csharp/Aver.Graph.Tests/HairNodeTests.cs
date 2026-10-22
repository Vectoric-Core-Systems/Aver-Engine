// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the strand hair nodes (docs/plans/wave11/SOFTBODY_HAIR.md 2.20): the 8 rows in GraphHairNodeTable.cs, every method on HairGraphInterop.
// Bare-process scope like RopeNodeTests: the wiring the table owns (a real method behind every row, pins, defaults, the required attributes, valid
// IL, the compiler split) and the "never throw" rule when the native DLL is absent. What the hair system does with a call is
// tests/hair and tests/framework_hair's business.
using System;
using System.Linq;
using System.Reflection;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class HairNodeTests
{
    private static readonly Row[] Rows =
    {
        new("addhair", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("sethairparams", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "width:float:in", "density:float:in", "stiffness:float:in", "wind:float:in", "then:exec:out", "success:bool:out" }, new[] { "width=1", "density=1", "stiffness=1", "wind=1" }),
        new("sethairlook", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("sethairenabled", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "on:bool:in", "then:exec:out", "success:bool:out" }, new[] { "on=true" }),
        new("teleporthair", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("addhaircollider", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "shape:int:in", "sx:float:in", "sy:float:in", "sz:float:in", "then:exec:out", "success:bool:out" }, new[] { "shape=0", "sx=10", "sy=0", "sz=0" }),
        new("gethair", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "simulating:bool:out", "lod:int:out", "renderStrands:int:out", "simStrands:int:out" }),
        new("sethairwind", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "gust:float:in", "then:exec:out", "success:bool:out" }, new[] { "gust=0" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("hair", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("hair", Rows);
        failures += NodeRowKit.CheckIlValid("hair", types);
        failures += NodeRowKit.CheckCompilerSplit("hair", types);
        failures += TestAttributes();
        failures += TestNothingThrows();
        failures += TestParamsSentinel();
        failures += TestHairChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the hair rows are exactly the designed set of 8, one pure read and seven exec writes, in the Hair family");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "HairGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 8) return Fail($"expected 8 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1) return Fail("only GetHair is a read");
        foreach (var t in want)
            if (!GameSystemNodes.TryGet(t, out var s) || s.Family != "Hair") return Fail($"{t} is not tagged with the Hair family");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The groom, the look material and the collider bone are names: required attributes. The other five nodes take none.
    private static int TestAttributes()
    {
        Console.WriteLine("Test: Add Hair needs asset=, Set Hair Look needs material=, Add Hair Collider needs bone=; the rest need no attribute");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n AddHair", "asset=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n SetHairLook", "material=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n AddHairCollider", "bone=", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n AddHair asset=Hair/Ponytail.ochair", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetHairParams", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetHairEnabled", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n TeleportHair", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetHairWind", exec: true);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // Failure returns false; with no native DLL every call fails, and none may throw.
    private static int TestNothingThrows()
    {
        Console.WriteLine("Test: every HairGraphInterop method returns instead of throwing, native DLL or not");
        var owner = typeof(SoftBodies).Assembly.GetType("Aver.Framework.HairGraphInterop");
        if (owner == null) return Fail("Aver.Framework.HairGraphInterop was not found by reflection");
        int called = 0;
        foreach (var m in owner.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public).Where(m => m.Name.EndsWith("ForGraph")))
        {
            var args = m.GetParameters().Select(p =>
                p.ParameterType == typeof(string) ? (object)"head" :
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

    // Set Hair Params with all four scales negative touches nothing and so succeeds even with no native DLL; a refused shape never reaches the relay.
    private static int TestParamsSentinel()
    {
        Console.WriteLine("Test: Set Hair Params with every scale negative leaves everything alone, and a collider shape outside 0..2 is refused");
        var owner = typeof(SoftBodies).Assembly.GetType("Aver.Framework.HairGraphInterop");
        if (owner == null) return Fail("Aver.Framework.HairGraphInterop was not found by reflection");
        var set = owner.GetMethod("SetHairParamsForGraph", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        var add = owner.GetMethod("AddHairColliderForGraph", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        if (set == null || add == null) return Fail("SetHairParamsForGraph or AddHairColliderForGraph was not found");
        if (!(bool)set.Invoke(null, new object[] { 1, -1f, -1f, -1f, -1f })!) return Fail("a no-op parameter set should report success");
        if ((bool)add.Invoke(null, new object[] { 1, 3, 1f, 1f, 1f, "head" })!) return Fail("shape 3 should be refused");
        if ((bool)add.Invoke(null, new object[] { 1, -1, 1f, 1f, 1f, "head" })!) return Fail("shape -1 should be refused");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A groom's life in one chain: add it, set the scales and the look, add a collider, teleport, read it back and print the level.
    private static int TestHairChain()
    {
        Console.WriteLine("Test: Add, Set Params, Set Look, Add Collider, Teleport, Get and Set Wind chain compiles and runs");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE ah AddHair asset=Hair/Ponytail.ochair\nNODE sp SetHairParams\nNODE sl SetHairLook material=Materials/HairBrown.ocmat\n" +
                            "NODE ac AddHairCollider bone=head\nNODE th TeleportHair\nNODE sw SetHairWind\nNODE gh GetHair\nNODE br Branch\nNODE pi PrintInt\n" +
                            "LINK tick.exec ah.exec\nLINK ah.then sp.exec\nLINK sp.then sl.exec\nLINK sl.then ac.exec\nLINK ac.then th.exec\nLINK th.then sw.exec\n" +
                            "LINK sw.then br.exec\nLINK gh.found br.cond\nLINK br.true pi.exec\nLINK gh.lod pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
