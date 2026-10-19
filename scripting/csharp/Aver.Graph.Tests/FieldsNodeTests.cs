// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the Fields nodes (docs/plans/wave10/SOFTBODY_DEPTH.md 2.13): the 8 rows in GraphFieldsNodeTable.cs, every method
// on SoftFieldGraph. Bare-process scope like SoftBodyNodeTests: the wiring the table owns (pins, defaults, the method each
// row names, valid IL, the compiler split), the enum values against the C header they mirror, and the spec encoding. What the
// soft-body system does with a field is tests/softbody.scene.
using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Aver.Framework;
using Aver.Graph;
using Aver.Scene;
using Row = NodeRowKit.Row;

[AutoRun]
static class FieldsNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_softfieldpulse", GenericKind.Exec, new[] { "exec:exec:in", "kind:int:in", "shape:int:in", "x:float:in", "y:float:in", "z:float:in", "ex:float:in", "ey:float:in", "ez:float:in", "dirX:float:in", "dirY:float:in", "dirZ:float:in", "strength:float:in", "energy:float:in", "falloff:float:in", "targets:int:in", "then:exec:out", "reached:int:out", "success:bool:out" }, new[] { "falloff=1", "targets=7" }),
        new("an_softfieldimpact", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "dirX:float:in", "dirY:float:in", "dirZ:float:in", "radius:float:in", "energy:float:in", "kick:float:in", "then:exec:out", "reached:int:out", "success:bool:out" }, new[] { "radius=40", "energy=500", "kick=0" }),
        new("an_softfieldstart", GenericKind.Exec, new[] { "exec:exec:in", "kind:int:in", "shape:int:in", "x:float:in", "y:float:in", "z:float:in", "ex:float:in", "ey:float:in", "ez:float:in", "dirX:float:in", "dirY:float:in", "dirZ:float:in", "strength:float:in", "energy:float:in", "falloff:float:in", "targets:int:in", "lifetime:float:in", "then:exec:out", "handle:int:out", "success:bool:out" }, new[] { "falloff=1", "targets=7", "lifetime=5" }),
        new("an_softfieldstop", GenericKind.Exec, new[] { "exec:exec:in", "handle:int:in", "then:exec:out", "success:bool:out" }),
        new("an_spawnsoftfield", GenericKind.Exec, new[] { "exec:exec:in", "kind:int:in", "shape:int:in", "x:float:in", "y:float:in", "z:float:in", "ex:float:in", "ey:float:in", "ez:float:in", "strength:float:in", "energy:float:in", "falloff:float:in", "lifetime:float:in", "then:exec:out", "entity:int:out", "success:bool:out" }, new[] { "falloff=1", "lifetime=5" }),
        new("an_softfieldsetenabled", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "enabled:bool:in", "then:exec:out", "success:bool:out" }, new[] { "enabled=true" }),
        new("an_softfieldsetstrength", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "strength:float:in", "energy:float:in", "then:exec:out", "success:bool:out" }),
        new("an_getsoftfieldtouched", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "touched:int:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("fields", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("fields", Rows);
        failures += NodeRowKit.CheckIlValid("fields", types);
        failures += NodeRowKit.CheckCompilerSplit("fields", types);
        failures += TestFieldChain();
        failures += TestTouchedFeedsLaterNodes();
        failures += TestEnumsMatchTheHeader();
        failures += TestWireEncoding();
        failures += TestBadKindIsRefused();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the Fields rows are exactly the designed set of 8, 1 pure read and 7 exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "SoftFieldGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 8) return Fail($"expected 8 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1) return Fail("only Get Soft Field Touched is a read");
        if (Rows.Any(r => !r.Type.StartsWith("an_", StringComparison.Ordinal))) return Fail("every Fields key carries the an_ prefix");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Spawn -> SetStrength -> Pulse -> Start -> Stop, with Impact and a touched read feeding a branch and a print.
    private static int TestFieldChain()
    {
        Console.WriteLine("Test: Spawn, Set Strength, Pulse, Start, Stop and Impact chain exec to exec");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE sp AN_SpawnSoftField\nNODE ss AN_SoftFieldSetStrength\nNODE se AN_SoftFieldSetEnabled\n" +
                            "NODE pu AN_SoftFieldPulse\nNODE st AN_SoftFieldStart\nNODE sx AN_SoftFieldStop\nNODE im AN_SoftFieldImpact\nNODE pi PrintInt\n" +
                            "LINK tick.exec sp.exec\nLINK sp.then ss.exec\nLINK sp.entity ss.entity\nLINK ss.then se.exec\nLINK sp.entity se.entity\n" +
                            "LINK se.then pu.exec\nLINK pu.then st.exec\nLINK st.then sx.exec\nLINK st.handle sx.handle\nLINK sx.then im.exec\n" +
                            "LINK im.then pi.exec\nLINK im.reached pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestTouchedFeedsLaterNodes()
    {
        Console.WriteLine("Test: Get Soft Field Touched feeds a Branch and a print");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE gt AN_GetSoftFieldTouched\nNODE br Branch\nNODE pi PrintInt\n" +
                            "LINK tick.exec br.exec\nLINK gt.found br.cond\nLINK br.true pi.exec\nLINK gt.touched pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The enums and the spec layout against framework_softfield_abi.h and Fields.hpp, found by walking up from the build to the repo.
    private static string? FindRepoFile(string rel)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, rel);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    private static int TestEnumsMatchTheHeader()
    {
        Console.WriteLine("Test: SoftFieldKind, SoftFieldShape and SoftFieldTargets match softbody::Fields.hpp");
        string? path = FindRepoFile("modules/softbody/include/aver/softbody/Fields.hpp");
        if (path == null) { Console.WriteLine("  SKIP: Fields.hpp not found from here"); return 0; }
        string text = File.ReadAllText(path);
        var kind = Regex.Match(text, @"enum class FieldKind\s*:\s*u8\s*\{([^}]*)\}");
        var shape = Regex.Match(text, @"enum class FieldShape\s*:\s*u8\s*\{([^}]*)\}");
        if (!kind.Success || !shape.Success) return Fail("could not read the C++ enums");
        string[] ck = kind.Groups[1].Value.Split(',').Select(x => x.Trim()).Where(x => x.Length > 0).ToArray();
        string[] cs = shape.Groups[1].Value.Split(',').Select(x => x.Trim()).Where(x => x.Length > 0).ToArray();
        var csKinds = Enum.GetNames<SoftFieldKind>();
        var csShapes = Enum.GetNames<SoftFieldShape>();
        if (ck.Length != csKinds.Length) return Fail($"FieldKind has {ck.Length} members in C++ and {csKinds.Length} in C#");
        for (int i = 0; i < ck.Length; i++)
        {
            string name = ck[i].Split('=')[0].Trim();
            if (!string.Equals(name, csKinds[i], StringComparison.Ordinal) || (int)Enum.Parse<SoftFieldKind>(name) != i) return Fail($"FieldKind member {i} is {name} in C++, {csKinds[i]} in C#");
        }
        if (cs.Length != csShapes.Length) return Fail($"FieldShape has {cs.Length} members in C++ and {csShapes.Length} in C#");
        for (int i = 0; i < cs.Length; i++)
        {
            string name = cs[i].Split('=')[0].Trim();
            if (!string.Equals(name, csShapes[i], StringComparison.Ordinal) || (int)Enum.Parse<SoftFieldShape>(name) != i) return Fail($"FieldShape member {i} is {name} in C++, {csShapes[i]} in C#");
        }
        if ((int)SoftFieldTargets.Cloth != 1 || (int)SoftFieldTargets.Destructible != 2 || (int)SoftFieldTargets.Debris != 4 || (int)SoftFieldTargets.All != 7)
            return Fail("SoftFieldTargets must be 1, 2, 4 and 7 (kFieldTarget*)");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Aver.Framework's internals are not visible to this project, so the wire form and the graph wrappers are reached by reflection.
    private static Type? FrameworkType(string name) => typeof(SoftFields).Assembly.GetType("Aver.Framework." + name);

    private static int TestWireEncoding()
    {
        Console.WriteLine("Test: a SoftFieldSpec encodes to the 24 floats of framework_softfield_abi.h");
        var spec = new SoftFieldSpec(SoftFieldKind.Damage, SoftFieldShape.Capsule, new Vec3(1, 2, 3), new Vec3(4, 5, 6),
                                     new Vec3(10, 20, 30), new Vec3(0, 1, 0), Strength: 7f, EnergyJoules: 8f, Falloff: 2f, Turbulence: 0.5f,
                                     Targets: SoftFieldTargets.Cloth | SoftFieldTargets.Debris, PerSecond: true);
        var wire = FrameworkType("SoftFieldWire");
        var encode = wire?.GetMethod("Encode", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public);
        if (encode == null) return Fail("SoftFieldWire.Encode was not found");
        var f = (float[]?)encode.Invoke(null, new object[] { spec, 9f });
        if (f == null || f.Length != 24) return Fail("the spec is 24 floats");
        if (f[0] != 2 || f[1] != 2 || f[2] != 5 || f[3] != 1) return Fail("kind, shape, targets and perSecond are slots 0 to 3");
        if (f[4] != 1 || f[5] != 2 || f[6] != 3 || f[7] != 4 || f[8] != 5 || f[9] != 6) return Fail("centre is 4-6 and extents 7-9");
        if (f[10] != 30 || f[11] != 20 || f[12] != 10) return Fail("slots 10-12 are yaw, pitch, roll: Z, Y, X of the rotation");
        if (f[13] != 0 || f[14] != 1 || f[15] != 0) return Fail("direction is 13-15");
        if (f[16] != 7 || f[17] != 8 || f[18] != 2 || f[19] != 0.5f || f[20] != 5 || f[21] != 9 || f[22] != 0 || f[23] != 0)
            return Fail("strength, energy, falloff, turbulence, wake, lifetime and the two reserved zeros are 16-23");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A kind or shape out of range is refused before any native call, so this runs in a bare process.
    private static int TestBadKindIsRefused()
    {
        Console.WriteLine("Test: a bad kind or shape is refused by the graph wrappers without a native call");
        var graph = FrameworkType("SoftFieldGraph");
        if (graph == null) return Fail("SoftFieldGraph was not found");
        const System.Reflection.BindingFlags flags = System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public;

        var pulse = graph.GetMethod("SoftFieldPulseForGraph", flags);
        if (pulse == null) return Fail("SoftFieldPulseForGraph was not found");
        object?[] a = { 9, 0, 0f, 0f, 0f, 1f, 1f, 1f, 0f, 0f, 0f, 0f, 0f, 1f, 7, 0 };
        if ((bool)pulse.Invoke(null, a)! || (int)a[15]! != 0) return Fail("kind 9 must be refused");

        var start = graph.GetMethod("SoftFieldStartForGraph", flags);
        if (start == null) return Fail("SoftFieldStartForGraph was not found");
        object?[] b = { 0, 7, 0f, 0f, 0f, 1f, 1f, 1f, 0f, 0f, 0f, 0f, 0f, 1f, 7, 5f, 0 };
        if ((bool)start.Invoke(null, b)! || (int)b[16]! != 0) return Fail("shape 7 must be refused");

        var spawn = graph.GetMethod("SpawnSoftFieldForGraph", flags);
        if (spawn == null) return Fail("SpawnSoftFieldForGraph was not found");
        object?[] c = { -1, 0, 0f, 0f, 0f, 1f, 1f, 1f, 0f, 0f, 1f, 5f, 0 };
        if ((bool)spawn.Invoke(null, c)! || (int)c[12]! != 0) return Fail("kind -1 must be refused");
        Console.WriteLine("  PASS");
        return 0;
    }
}
