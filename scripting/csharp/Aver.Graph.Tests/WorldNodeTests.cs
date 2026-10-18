// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the World family (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): the 3 rows in GraphWorldNodeTable.cs,
// every method on WorldGraph. Bare-process scope like MontageNodeTests: the wiring the table owns (pins, defaults, the
// method each row names, valid IL, the compiler split), the cloud defaults agreeing with Sky.SetClouds, and the argument
// checks that refuse before any native call. What the host does with a cloud request or a fluid volume is checked where a
// host exists.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class WorldNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_setskyclouds", GenericKind.Exec, new[] { "exec:exec:in", "seed:int:in", "coverage:float:in", "density:float:in", "bottomCm:float:in", "topCm:float:in", "featureScale:float:in", "windX:float:in", "windY:float:in", "then:exec:out", "success:bool:out" },
            new[] { "coverage=0.45", "density=1", "bottomCm=150000", "topCm=280000", "windX=900", "windY=260" }),
        new("an_clearskyclouds", GenericKind.Exec, new[] { "exec:exec:in", "then:exec:out", "success:bool:out" }),
        new("an_spawnfluidvolume", GenericKind.Exec, new[] { "exec:exec:in", "cx:float:in", "cy:float:in", "cz:float:in", "hx:float:in", "hy:float:in", "hz:float:in", "then:exec:out", "success:bool:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("world", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("world", Rows);
        failures += TestCloudDefaultsMatchSky();
        failures += NodeRowKit.CheckIlValid("world", types);
        failures += NodeRowKit.CheckCompilerSplit("world", types);
        failures += TestFluidNameIsOptional();
        failures += TestRefusalsBeforeAnyNativeCall();
        failures += TestWorldChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the world rows are exactly the designed set of 3, all exec");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "WorldGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 3 || Rows.Any(r => r.Kind != GenericKind.Exec)) return Fail("expected 3 exec rows");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The row defaults are copied from Sky.SetClouds; if the C# defaults move, this says so.
    private static int TestCloudDefaultsMatchSky()
    {
        Console.WriteLine("Test: setskyclouds pin defaults equal the default arguments of Sky.SetClouds");
        MethodInfo? m = typeof(Aver.Framework.Sky).GetMethods().FirstOrDefault(x => x.Name == "SetClouds" && x.GetParameters().Length == 8);
        if (m == null) return Fail("Sky.SetClouds with 8 parameters not found");
        var byName = m.GetParameters().ToDictionary(p => p.Name!, p => p.DefaultValue);
        const string text = "OCGRAPH 1\nNODE s an_setskyclouds\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        var c = new FamilyAChecks();
        (string Pin, string Param)[] map =
        {
            ("density", "density"), ("bottomCm", "bottomCm"), ("topCm", "topCm"), ("featureScale", "featureScale"),
            ("windX", "windXCmPerSec"), ("windY", "windYCmPerSec"),
        };
        foreach (var (pin, param) in map)
        {
            var pv = g.PinnedValues.FirstOrDefault(v => v.NodeId == "s" && v.PinName == pin);
            if (pv == null) { Console.WriteLine($"    no default on pin {pin}"); c.Bad++; continue; }
            c.Near($"{pin} default", Convert.ToDouble(pv.Value), Convert.ToDouble(byName[param]), 1e-9);
        }
        var cov = g.PinnedValues.FirstOrDefault(v => v.NodeId == "s" && v.PinName == "coverage");
        c.True("coverage defaults to 0.45", cov != null && Math.Abs(Convert.ToDouble(cov.Value) - 0.45) < 1e-6);
        return c.Done();
    }

    // Only the name is an attribute, and it is optional.
    private static int TestFluidNameIsOptional()
    {
        Console.WriteLine("Test: spawnfluidvolume compiles with no name= and with one");
        if (NodeRowKit.CheckOptionalAttribute("NODE n an_spawnfluidvolume", exec: true) != 0) return 1;
        if (NodeRowKit.CheckOptionalAttribute("NODE n an_spawnfluidvolume name=pool", exec: true) != 0) return 1;
        Console.WriteLine("  PASS");
        return 0;
    }

    // These refuse on the argument alone, so they hold with or without the native services.
    private static int TestRefusalsBeforeAnyNativeCall()
    {
        Console.WriteLine("Test: a NaN cloud argument and a non-positive fluid extent are refused without a native call");
        var c = new FamilyAChecks();
        c.True("setskyclouds with NaN coverage", !(bool)FamilyAEval.Call("an_setskyclouds", FamilyAEval.In("seed", 1, "coverage", float.NaN))["success"]);
        c.True("setskyclouds with NaN wind", !(bool)FamilyAEval.Call("an_setskyclouds", FamilyAEval.In("seed", 1, "windX", float.NaN))["success"]);
        c.True("spawnfluidvolume with no extents", !(bool)FamilyAEval.Call("an_spawnfluidvolume", FamilyAEval.In("cx", 0f))["success"]);
        c.True("spawnfluidvolume with a zero extent", !(bool)FamilyAEval.Call("an_spawnfluidvolume", FamilyAEval.In("hx", 100f, "hy", 0f, "hz", 100f))["success"]);
        c.True("spawnfluidvolume with a negative extent", !(bool)FamilyAEval.Call("an_spawnfluidvolume", FamilyAEval.In("hx", 100f, "hy", 100f, "hz", -5f))["success"]);
        c.True("spawnfluidvolume with a NaN extent", !(bool)FamilyAEval.Call("an_spawnfluidvolume", FamilyAEval.In("hx", float.NaN, "hy", 100f, "hz", 100f))["success"]);
        // and a well-formed call never throws, whatever the host answers
        try
        {
            FamilyAEval.Call("an_setskyclouds", FamilyAEval.In("seed", 3));
            FamilyAEval.Call("an_clearskyclouds");
            FamilyAEval.Call("an_spawnfluidvolume", FamilyAEval.In("hx", 100f, "hy", 100f, "hz", 50f), new Dictionary<string, string> { ["name"] = "pond" });
        }
        catch (Exception e) { return Fail($"a wrapper threw {e.GetType().Name}: {e.Message}"); }
        return c.Done();
    }

    // A cloud change, a clear and a fluid volume chain on an exec path; success pins feed a Branch.
    private static int TestWorldChain()
    {
        Console.WriteLine("Test: clouds, clear and fluid volume chain exec to exec and their success pins feed a Branch");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE sc an_setskyclouds\nPINVAL sc seed 5\nNODE fv an_spawnfluidvolume name=pond\nPINVAL fv hx 200\nPINVAL fv hy 200\nPINVAL fv hz 50\n" +
                            "NODE cl an_clearskyclouds\nNODE br Branch\nNODE pf Print\n" +
                            "LINK tick.exec sc.exec\nLINK sc.then fv.exec\nLINK fv.then cl.exec\nLINK cl.then br.exec\nLINK fv.success br.cond\nLINK br.true pf.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
