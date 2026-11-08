// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the physical animation nodes (docs/physics/PHYSICAL_ANIMATION.md): the 10 rows in GraphPhysAnimNodeTable.cs, every method on
// PhysAnimGraphInterop. Bare-process scope like HairNodeTests: the wiring the table owns (a real method behind every row, pins, defaults,
// required and optional attributes, valid IL, the compiler split) and the "never throw" rule when the native DLL is absent. What the
// ragdoll system does with a call is tests/ragdoll's PhysicalAnimationTest and tests/framework_physanim's business.
using System;
using System.Linq;
using System.Reflection;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class PhysAnimNodeTests
{
    private static readonly Row[] Rows =
    {
        new("applyhitreaction", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "px:float:in", "py:float:in", "pz:float:in", "damage:float:in", "radius:float:in", "then:exec:out", "success:bool:out" }),
        new("setphysicalanimationprofile", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("blendphysicalanimation", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "weight:float:in", "seconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "seconds=0.2" }),
        new("simulatebonesubtree", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("setbonestrength", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "strength:float:in", "then:exec:out", "success:bool:out" }),
        new("setboneblend", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "blend:float:in", "then:exec:out", "success:bool:out" }),
        new("pinbone", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "pinned:bool:in", "then:exec:out", "success:bool:out" }, new[] { "pinned=true" }),
        new("dieasragdoll", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("getupfromragdoll", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("getphysicalanimationstate", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "life:int:out", "facing:int:out", "reacting:int:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("physical animation", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("physical animation", Rows);
        failures += NodeRowKit.CheckIlValid("physical animation", types);
        failures += NodeRowKit.CheckCompilerSplit("physical animation", types);
        failures += TestAttributes();
        failures += TestNothingThrows();
        failures += TestDefaultsAreWhatTheDocsSay();
        failures += TestApiValues();
        failures += TestReactionChain();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the physical animation rows are exactly the designed set of 10, one pure read and nine exec writes, in the PhysAnim family");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "PhysAnimGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 10) return Fail($"expected 10 rows, test lists {Rows.Length}");
        if (Rows.Count(r => r.Kind == GenericKind.Pure) != 1) return Fail("only Get Physical Animation State is a read");
        foreach (var t in want)
            if (!GameSystemNodes.TryGet(t, out var s) || s.Family != "PhysAnim") return Fail($"{t} is not tagged with the PhysAnim family");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The per-bone nodes need a bone; the hit, the subtree and the profile take "empty" (root part, whole body, defaults).
    private static int TestAttributes()
    {
        Console.WriteLine("Test: Set Bone Strength, Set Bone Blend and Pin Bone need bone=; the hit, subtree and profile nodes take it empty");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n SetBoneStrength", "bone=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n SetBoneBlend", "bone=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n PinBone", "bone=", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n ApplyHitReaction", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SimulateBoneSubtree", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetPhysicalAnimationProfile", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n BlendPhysicalAnimation", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n DieAsRagdoll", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n GetUpFromRagdoll", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n GetPhysicalAnimationState", exec: false);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // With no native DLL (or no provider) every call fails, and none may throw.
    private static int TestNothingThrows()
    {
        Console.WriteLine("Test: every PhysAnimGraphInterop method returns instead of throwing, native DLL or not");
        var owner = typeof(Entity).Assembly.GetType("Aver.Framework.PhysAnimGraphInterop");
        if (owner == null) return Fail("Aver.Framework.PhysAnimGraphInterop was not found by reflection");
        int called = 0;
        foreach (var m in owner.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public).Where(m => m.Name.EndsWith("ForGraph")))
        {
            var args = m.GetParameters().Select(p =>
                p.ParameterType == typeof(string) ? (object)"spine" :
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

    private static int TestDefaultsAreWhatTheDocsSay()
    {
        Console.WriteLine("Test: Blend Physical Animation defaults to 0.2 s and Pin Bone to pinned");
        const string text = "OCGRAPH 1\nNODE b BlendPhysicalAnimation\nNODE p PinBone bone=hand\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        object? V(string node, string pin) => g.PinnedValues.FirstOrDefault(v => v.NodeId == node && v.PinName == pin)?.Value;
        if (!(V("b", "seconds") is float s && Math.Abs(s - 0.2f) < 1e-6f)) return Fail("seconds default is missing or wrong");
        if (!(V("p", "pinned") is bool pinned && pinned)) return Fail("pinned default is missing or wrong");
        if (V("b", "weight") != null) return Fail("weight should have no default");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The public C# surface: the enums are the numbers the ABI documents, a hit event carries what it was given, and no call throws without a host.
    private static int TestApiValues()
    {
        Console.WriteLine("Test: LifeState and BodyFacing carry the documented numbers; the Entity API answers false without a host");
        if ((int)LifeState.Alive != 0 || (int)LifeState.Dying != 1 || (int)LifeState.Dead != 2 || (int)LifeState.GettingUp != 3) return Fail("LifeState numbers");
        if ((int)BodyFacing.Upright != 0 || (int)BodyFacing.Supine != 1 || (int)BodyFacing.Prone != 2 || (int)BodyFacing.Side != 3) return Fail("BodyFacing numbers");
        var e = default(Entity);
        var hit = new HitEvent(e, "head", new Aver.Scene.Vec3(1, 2, 3), new Aver.Scene.Vec3(4, 5, 6), 7f, 8f);
        if (hit.Bone != "head" || hit.Impulse.Z != 3 || hit.Point.X != 4 || hit.Damage != 7f || hit.Radius != 8f || !hit.Target.Equals(e)) return Fail("a HitEvent does not carry what it was given");
        try
        {
            e.ApplyHit(hit);
            e.ApplyHitReaction("head", new Aver.Scene.Vec3(1, 0, 0));
            if (e.SetBoneStrength("", 1f) || e.PinBone(null!, true)) return Fail("an empty bone name must be refused");
            if (e.TryGetPhysicalAnimationState(out var life, out var facing, out var reacting) || life != LifeState.Alive || facing != BodyFacing.Upright || reacting != 0)
                return Fail("no ragdoll should read Alive, Upright, 0");
        }
        catch (Exception ex) { return Fail($"the Entity API threw {ex.GetType().Name}: {ex.Message}"); }
        Console.WriteLine("  PASS");
        return 0;
    }

    // A character's day in one chain: take a hit, simulate an arm, weaken it, blend it out, pin the head, blend the body, die, get up, read the state.
    private static int TestReactionChain()
    {
        Console.WriteLine("Test: Apply Hit Reaction through Get Up chains into Get Physical Animation State, which gates a branch");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE ph SetPhysicalAnimationProfile profile=Anim/Brawler.ocphysanim\nNODE hr ApplyHitReaction bone=head\n" +
                            "NODE sb SimulateBoneSubtree bone=upperarm_l\nNODE ss SetBoneStrength bone=upperarm_l\nNODE sl SetBoneBlend bone=hand_l\nNODE pb PinBone bone=head\n" +
                            "NODE bp BlendPhysicalAnimation\nNODE dr DieAsRagdoll\nNODE gu GetUpFromRagdoll\nNODE gs GetPhysicalAnimationState\nNODE br Branch\nNODE pi PrintInt\n" +
                            "LINK tick.exec ph.exec\nLINK ph.then hr.exec\nLINK hr.then sb.exec\nLINK sb.then ss.exec\nLINK ss.then sl.exec\nLINK sl.then pb.exec\n" +
                            "LINK pb.then bp.exec\nLINK bp.then dr.exec\nLINK dr.then gu.exec\nLINK gu.then br.exec\nLINK gs.found br.cond\nLINK br.true pi.exec\n" +
                            "LINK gs.life pi.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }
}
