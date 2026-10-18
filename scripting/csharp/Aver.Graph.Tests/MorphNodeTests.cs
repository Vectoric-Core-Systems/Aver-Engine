// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the blend-shape and lip-sync nodes (docs/plans/wave9/MORPH_TARGETS.md section 2.9): the 9 rows in
// GraphMorphNodeTable.cs, every method on MorphGraphInterop. Bare-process scope like IkNodeTests: the wiring the
// table owns (a real method behind every row, pins, defaults, push-only versus pure, required attributes, valid IL),
// not what the face does with a weight (tests/anim.scene's MorphSystemTest does that).
using System;
using System.Linq;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

static class MorphNodeTests
{
    private static readonly Row[] Rows =
    {
        new("setmorphweight", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "weight:float:in", "blendSeconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "blendSeconds=0" }),
        new("setfacepose", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "weight:float:in", "blendSeconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "blendSeconds=0" }),
        new("clearmorphweights", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "blendSeconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "blendSeconds=0" }),
        new("getmorphweight", GenericKind.Pure, new[] { "entity:int:in", "found:bool:out", "weight:float:out" }),
        new("setfacerig", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }),
        new("playlipsync", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "rate:float:in", "startSeconds:float:in", "strength:float:in", "then:exec:out", "success:bool:out" }, new[] { "rate=1", "startSeconds=0", "strength=1" }),
        new("stoplipsync", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "blendSeconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "blendSeconds=0.1" }),
        new("queueviseme", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "startSeconds:float:in", "durationSeconds:float:in", "intensity:float:in", "then:exec:out", "success:bool:out" }, new[] { "startSeconds=0", "durationSeconds=0.1", "intensity=1" }),
        new("getlipsyncstate", GenericKind.Pure, new[] { "entity:int:in", "playing:bool:out", "time:float:out", "length:float:out", "strength:float:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("Morph targets", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("Morph targets", Rows);
        failures += TestDocumentedDefaults();
        failures += NodeRowKit.CheckIlValid("Morph targets", types);
        failures += NodeRowKit.CheckCompilerSplit("Morph targets", types);
        failures += TestMissingAttributeFailsByName();
        failures += TestWeightAndLipSyncChain();
        failures += TestReadChain();
        failures += TestNameTables();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the morph rows are exactly the designed set of 9, 2 pure state reads and exec writes");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "MorphGraphInterop").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 9) return Fail($"expected 9 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!pure.SequenceEqual(new[] { "getlipsyncstate", "getmorphweight" })) return Fail("only the two state reads may be pure");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The values the design names: blend 0 for set and clear, rate and strength 1, a 0.1 s viseme, a 0.1 s lip-sync fade.
    private static int TestDocumentedDefaults()
    {
        Console.WriteLine("Test: blendSeconds 0, rate 1, strength 1, durationSeconds 0.1, stop fade 0.1");
        const string text = "OCGRAPH 1\nNODE a SetMorphWeight target=jawOpen\nNODE b PlayLipSync track=a.ocvis\nNODE c StopLipSync\n" +
                            "NODE d QueueViseme viseme=aa\nNODE e ClearMorphWeights\n";
        if (!OcGraphParser.Parse(text, out var g, out var err)) return Fail("parse: " + err);
        object? V(string node, string pin) => g.PinnedValues.FirstOrDefault(v => v.NodeId == node && v.PinName == pin)?.Value;
        bool Near(object? v, float want) => v is float f && Math.Abs(f - want) < 1e-6f;
        bool ok = Near(V("a", "blendSeconds"), 0f) && Near(V("b", "rate"), 1f) && Near(V("b", "startSeconds"), 0f) && Near(V("b", "strength"), 1f) &&
                  Near(V("c", "blendSeconds"), 0.1f) && Near(V("d", "startSeconds"), 0f) && Near(V("d", "durationSeconds"), 0.1f) &&
                  Near(V("d", "intensity"), 1f) && Near(V("e", "blendSeconds"), 0f);
        if (!ok) return Fail("a default is missing or wrong");
        if (V("a", "weight") != null) return Fail("weight should have no default");
        if (g.Nodes["a"].Attrs["target"] != "jawOpen") return Fail("target= did not reach Node.Attrs");
        Console.WriteLine("  PASS");
        return 0;
    }

    // Target, pose, track and viseme names are required; the rig and the clear target are optional (empty = default / all).
    private static int TestMissingAttributeFailsByName()
    {
        Console.WriteLine("Test: morph nodes missing a required attribute fail to compile, naming it; the 'empty = default / all' ones compile bare");
        int f = 0;
        f += NodeRowKit.CheckMissingAttribute("NODE n SetMorphWeight", "target=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n SetFacePose", "pose=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n GetMorphWeight", "target=", exec: false);
        f += NodeRowKit.CheckMissingAttribute("NODE n PlayLipSync", "track=", exec: true);
        f += NodeRowKit.CheckMissingAttribute("NODE n QueueViseme", "viseme=", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n ClearMorphWeights", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n SetFaceRig", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n StopLipSync", exec: true);
        f += NodeRowKit.CheckOptionalAttribute("NODE n GetLipSyncState", exec: false);
        if (f == 0) Console.WriteLine("  PASS");
        return f == 0 ? 0 : 1;
    }

    // The chain the design names: OnTick -> Set Morph Weight -> Play Lip Sync, then a face pose, a rig, a viseme and a stop.
    private static int TestWeightAndLipSyncChain()
    {
        Console.WriteLine("Test: OnTick -> Set Morph Weight -> Play Lip Sync (-> Set Face Pose -> Set Face Rig -> Queue Viseme -> Stop Lip Sync) compiles and its IL is valid");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE mw SetMorphWeight target=jawOpen\nNODE pl PlayLipSync track=Speech/Hello.ocvis\nNODE fp SetFacePose pose=viseme_aa\n" +
                            "NODE fr SetFaceRig rig=Faces/Hero.ocface\nNODE qv QueueViseme viseme=HH\nNODE sl StopLipSync\nNODE cl ClearMorphWeights\n" +
                            "LINK tick.exec mw.exec\nLINK mw.then pl.exec\nLINK pl.then fp.exec\nLINK fp.then fr.exec\nLINK fr.then qv.exec\nLINK qv.then sl.exec\nLINK sl.then cl.exec\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The two reads feed a branch and a print.
    private static int TestReadChain()
    {
        Console.WriteLine("Test: Get Morph Weight and Get Lip Sync State feed a branch and prints");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE gw GetMorphWeight target=jawOpen\nNODE ls GetLipSyncState\nNODE br Branch\nNODE pa Print\nNODE pb Print\n" +
                            "LINK tick.exec br.exec\nLINK ls.playing br.cond\nLINK br.true pa.exec\nLINK gw.weight pa.value\nLINK pa.then pb.exec\nLINK ls.time pb.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // The name tables the scripts spell targets and visemes with.
    private static int TestNameTables()
    {
        Console.WriteLine("Test: FaceShapes has the 52 ARKit names and Visemes the 15, each distinct, and the spot checks match their constants");
        if (FaceShapes.All.Count != 52 || FaceShapes.All.Distinct().Count() != 52) return Fail($"FaceShapes.All holds {FaceShapes.All.Count} names");
        if (Visemes.All.Count != 15 || Visemes.All.Distinct().Count() != 15) return Fail($"Visemes.All holds {Visemes.All.Count} names");
        if (FaceShapes.AllShapes.Count != 52) return Fail("AllShapes");
        if (FaceShapes.JawOpen != "jawOpen" || FaceShapes.MouthSmileLeft != "mouthSmileLeft" || FaceShapes.TongueOut != "tongueOut" ||
            FaceShapes.EyeBlinkRight != "eyeBlinkRight") return Fail("an ARKit constant is misspelt");
        if (!Visemes.All.All(v => v.StartsWith("viseme_"))) return Fail("every viseme name starts with viseme_");
        if (Visemes.Aa != "viseme_aa" || Visemes.Sil != "viseme_sil" || Visemes.PP != "viseme_PP") return Fail("a viseme constant is misspelt");
        Console.WriteLine("  PASS");
        return 0;
    }
}
