// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the gameplay camera nodes (docs/CAMERAS.md): the 23 rows in GraphCameraNodeTable.cs, every method on CameraGraph.
// Bare-process scope like RenderPropsNodeTests: the wiring the table owns (a real method behind every row, pins, defaults,
// push-only versus pure, valid IL), the enums against the C header they mirror, and the modifier mapping. What a call does
// to a live camera system needs the native libraries, so those checks treat "unavailable" as a skip, not a failure.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Aver.Framework;
using Aver.Graph;
using Row = NodeRowKit.Row;

[AutoRun]
static class CameraNodeTests
{
    private static readonly Row[] Rows =
    {
        new("an_cameraaddrig", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "preset:int:in", "target:int:in", "then:exec:out", "success:bool:out" }, new[] { "preset=0", "target=0" }),
        new("an_camerasetmode", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "mode:int:in", "then:exec:out", "success:bool:out" }, new[] { "mode=0" }),
        new("an_camerasettarget", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "target:int:in", "then:exec:out", "success:bool:out" }),
        new("an_camerasetviewtarget", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "blendSeconds:float:in", "curve:int:in", "then:exec:out", "success:bool:out" }, new[] { "blendSeconds=0.5", "curve=0" }),
        new("an_cameraclearviewtarget", GenericKind.Exec, new[] { "exec:exec:in", "blendSeconds:float:in", "curve:int:in", "then:exec:out", "success:bool:out" }, new[] { "blendSeconds=0.5", "curve=0" }),
        new("an_cameracut", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "then:exec:out", "success:bool:out" }, new[] { "entity=0" }),
        new("an_camerasetarm", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "lengthCm:float:in", "probeRadiusCm:float:in", "collide:bool:in", "then:exec:out", "success:bool:out" }, new[] { "lengthCm=450", "probeRadiusCm=12", "collide=true" }),
        new("an_camerasetsocket", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
        new("an_camerasetpivot", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "x:float:in", "y:float:in", "z:float:in", "then:exec:out", "success:bool:out" }),
        new("an_camerasetlag", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "posSpeed:float:in", "rotSpeed:float:in", "maxDistCm:float:in", "then:exec:out", "success:bool:out" }, new[] { "posSpeed=10", "rotSpeed=0", "maxDistCm=0" }),
        new("an_camerasetlookahead", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "seconds:float:in", "maxCm:float:in", "then:exec:out", "success:bool:out" }, new[] { "seconds=0.25", "maxCm=150" }),
        new("an_camerasetlimits", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "pitchMinDeg:float:in", "pitchMaxDeg:float:in", "then:exec:out", "success:bool:out" }, new[] { "pitchMinDeg=-80", "pitchMaxDeg=80" }),
        new("an_cameraaddlook", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "yawDeg:float:in", "pitchDeg:float:in", "then:exec:out", "success:bool:out" }),
        new("an_camerasetfade", GenericKind.Exec, new[] { "exec:exec:in", "entity:int:in", "startCm:float:in", "endCm:float:in", "then:exec:out", "success:bool:out" }, new[] { "startCm=140", "endCm=70" }),
        new("an_camerapushfovkick", GenericKind.Exec, new[] { "exec:exec:in", "deltaDeg:float:in", "inSeconds:float:in", "holdSeconds:float:in", "outSeconds:float:in", "then:exec:out", "id:int:out" }, new[] { "deltaDeg=8", "inSeconds=0.05", "holdSeconds=0.1", "outSeconds=0.4" }),
        new("an_camerapushzoom", GenericKind.Exec, new[] { "exec:exec:in", "fovScale:float:in", "blendSeconds:float:in", "then:exec:out", "id:int:out" }, new[] { "fovScale=0.6", "blendSeconds=0.2" }),
        new("an_camerapunch", GenericKind.Exec, new[] { "exec:exec:in", "pitchDeg:float:in", "yawDeg:float:in", "rollDeg:float:in", "recoverSeconds:float:in", "then:exec:out", "id:int:out" }, new[] { "recoverSeconds=0.3" }),
        new("an_cameraremovemodifier", GenericKind.Exec, new[] { "exec:exec:in", "id:int:in", "outSeconds:float:in", "then:exec:out", "success:bool:out" }, new[] { "outSeconds=-1" }),
        new("an_cameraclearmodifiers", GenericKind.Exec, new[] { "exec:exec:in", "tag:int:in", "then:exec:out", "count:int:out" }, new[] { "tag=0" }),
        new("an_camerashakeat", GenericKind.Exec, new[] { "exec:exec:in", "x:float:in", "y:float:in", "z:float:in", "radiusCm:float:in", "intensity:float:in", "then:exec:out", "success:bool:out" }, new[] { "radiusCm=1500", "intensity=0.6" }),
        new("an_cameragetpose", GenericKind.Pure, new[] { "x:float:out", "y:float:out", "z:float:out", "yawDeg:float:out", "pitchDeg:float:out", "rollDeg:float:out", "fovDeg:float:out", "found:bool:out" }),
        new("an_cameragetrigstate", GenericKind.Pure, new[] { "entity:int:in", "armCm:float:out", "colliding:bool:out", "active:bool:out", "blending:bool:out", "found:bool:out" }),
        new("an_cameragetviewtarget", GenericKind.Pure, new[] { "entity:int:out", "blending:bool:out", "blendAlpha:float:out" }),
    };

    public static int RunAll()
    {
        int failures = 0;
        string[] types = Rows.Select(r => r.Type).ToArray();
        failures += NodeRowKit.CheckRows("camera", Rows);
        failures += TestRowSetIsTheDesignSet();
        failures += NodeRowKit.CheckDefaultPins("camera", Rows);
        failures += NodeRowKit.CheckIlValid("camera", types);
        failures += NodeRowKit.CheckCompilerSplit("camera", types);
        failures += TestEnumsMirrorTheHeader();
        failures += TestCameraChain();
        failures += TestReadsFeedLaterNodes();
        failures += TestPresetModes();
        failures += TestUnavailableCameraIsNotAFailure();
        return failures;
    }

    private static int Fail(string msg) => NodeRowKit.Fail(msg);

    private static int TestRowSetIsTheDesignSet()
    {
        Console.WriteLine("Test: the camera rows are exactly the designed set of 23, 3 pure reads and 20 exec calls");
        var want = Rows.Select(r => r.Type).OrderBy(x => x).ToArray();
        var got = GameSystemNodes.All.Where(s => s.Owner.Name == "CameraGraph").Select(s => s.Type).OrderBy(x => x).ToArray();
        if (!got.SequenceEqual(want))
            return Fail($"table has [{string.Join(", ", got.Except(want))}] extra and lacks [{string.Join(", ", want.Except(got))}]");
        if (Rows.Length != 23) return Fail($"expected 23 rows, test lists {Rows.Length}");
        var pure = Rows.Where(r => r.Kind == GenericKind.Pure).Select(r => r.Type).OrderBy(x => x).ToArray();
        if (!pure.SequenceEqual(new[] { "an_cameragetpose", "an_cameragetrigstate", "an_cameragetviewtarget" })) return Fail("only the three reads may be pure");
        if (Rows.Any(r => !r.Type.StartsWith("an_camera"))) return Fail("every key starts with an_camera");
        Console.WriteLine("  PASS");
        return 0;
    }

    // The C# enums are copies of the AVER_FW_CAM_* defines in framework_camera_abi.h; the C++ AbiEnumTest pins the same copies.
    private static int TestEnumsMirrorTheHeader()
    {
        Console.WriteLine("Test: CameraMode, CameraRigPreset, CameraBlendCurve, CameraModifierKind and CameraRigFlags mirror framework_camera_abi.h");
        string? path = FindUp("modules/framework/include/aver/framework/framework_camera_abi.h");
        if (path == null) { Console.WriteLine("  SKIP: the header is not reachable from the test's directory"); return 0; }
        var defs = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(File.ReadAllText(path), @"^#define\s+(AVER_FW_CAM_[A-Z]+)_([A-Z0-9]+)\s+(0x[0-9A-Fa-f]+|\d+)", RegexOptions.Multiline))
            defs[m.Groups[1].Value + "_" + m.Groups[2].Value] = Convert.ToInt64(m.Groups[3].Value.TrimEnd('u', 'U'), m.Groups[3].Value.StartsWith("0x") ? 16 : 10);
        int bad = 0;
        bad += Mirror(defs, "AVER_FW_CAM_MODE", typeof(CameraMode));
        bad += Mirror(defs, "AVER_FW_CAM_PRESET", typeof(CameraRigPreset));
        bad += Mirror(defs, "AVER_FW_CAM_CURVE", typeof(CameraBlendCurve));
        bad += Mirror(defs, "AVER_FW_CAM_MOD", typeof(CameraModifierKind));
        foreach (CameraRigFlags f in Enum.GetValues<CameraRigFlags>())
        {
            string key = "AVER_FW_CAM_RIG_" + f.ToString().ToUpperInvariant();
            if (!defs.TryGetValue(key, out long v)) { Console.WriteLine($"    {key}: not in the header"); ++bad; }
            else if (v != (long)(int)f) { Console.WriteLine($"    {key}: header {v}, enum {(int)f}"); ++bad; }
        }
        if (bad > 0) return Fail($"{bad} problem(s)");
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int Mirror(Dictionary<string, long> defs, string prefix, Type enumType)
    {
        int bad = 0;
        int n = 0;
        foreach (var kv in defs.Where(d => d.Key.StartsWith(prefix + "_", StringComparison.Ordinal)))
        {
            ++n;
            string name = kv.Key[(prefix.Length + 1)..];
            string? member = Enum.GetNames(enumType).FirstOrDefault(x => string.Equals(x, name, StringComparison.OrdinalIgnoreCase));
            if (member == null) { Console.WriteLine($"    {kv.Key}: no enum member named like it"); ++bad; continue; }
            if (Convert.ToInt64(Enum.Parse(enumType, member)) != kv.Value) { Console.WriteLine($"    {kv.Key}: header {kv.Value}, enum {Enum.Parse(enumType, member):D}"); ++bad; }
        }
        if (n != Enum.GetNames(enumType).Length) { Console.WriteLine($"    {prefix}: header has {n} values, {enumType.Name} has {Enum.GetNames(enumType).Length}"); ++bad; }
        return bad;
    }

    private static string? FindUp(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(p)) return p;
        }
        return null;
    }

    // A pawn setup handler: every exec call in one chain, ending in a modifier id feeding a print.
    private static int TestCameraChain()
    {
        Console.WriteLine("Test: OnTick -> every camera call in one chain compiles and its IL is valid");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE a AN_CameraAddRig\nNODE b AN_CameraSetMode\nNODE c AN_CameraSetTarget\nNODE d AN_CameraSetViewTarget\n" +
                            "NODE e AN_CameraClearViewTarget\nNODE f AN_CameraCut\nNODE g AN_CameraSetArm\nNODE h AN_CameraSetSocket\nNODE i AN_CameraSetPivot\n" +
                            "NODE j AN_CameraSetLag\nNODE k AN_CameraSetLookAhead\nNODE l AN_CameraSetLimits\nNODE m AN_CameraAddLook\nNODE n AN_CameraSetFade\n" +
                            "NODE o AN_CameraPushFovKick\nNODE p AN_CameraPushZoom\nNODE q AN_CameraPunch\nNODE r AN_CameraRemoveModifier\nNODE s AN_CameraClearModifiers\n" +
                            "NODE t AN_CameraShakeAt\nNODE pr PrintInt\n" +
                            "LINK tick.exec a.exec\nLINK a.then b.exec\nLINK b.then c.exec\nLINK c.then d.exec\nLINK d.then e.exec\nLINK e.then f.exec\nLINK f.then g.exec\n" +
                            "LINK g.then h.exec\nLINK h.then i.exec\nLINK i.then j.exec\nLINK j.then k.exec\nLINK k.then l.exec\nLINK l.then m.exec\nLINK m.then n.exec\n" +
                            "LINK n.then o.exec\nLINK o.then p.exec\nLINK p.then q.exec\nLINK q.then r.exec\nLINK o.id r.id\nLINK r.then s.exec\nLINK s.then t.exec\n" +
                            "LINK t.then pr.exec\nLINK s.count pr.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    private static int TestReadsFeedLaterNodes()
    {
        Console.WriteLine("Test: Get Camera Pose, Get Camera Rig State and Get View Target feed a Branch and prints without being in the exec chain");
        const string text = "OCGRAPH 1\nNODE tick OnTick\nNODE gp AN_CameraGetPose\nNODE gs AN_CameraGetRigState\nNODE gv AN_CameraGetViewTarget\nNODE br Branch\nNODE pa Print\nNODE pb Print\nNODE pc PrintInt\n" +
                            "LINK tick.exec br.exec\nLINK gp.found br.cond\nLINK br.true pa.exec\nLINK gp.fovDeg pa.value\nLINK pa.then pb.exec\nLINK gs.armCm pb.value\nLINK pb.then pc.exec\nLINK gv.entity pc.value\nENTRY tick OnTick\n";
        string? bad = NodeRowKit.CompileAndRun(text, "OnTick");
        if (bad != null) return Fail(bad);
        Console.WriteLine("  PASS");
        return 0;
    }

    // VEHICLES attaches rigs by preset; the mode a preset starts in is the one the Character toggles back to.
    private static int TestPresetModes()
    {
        Console.WriteLine("Test: CameraRig.ModeOf maps every preset to the mode it starts in");
        var want = new (CameraRigPreset, CameraMode)[]
        {
            (CameraRigPreset.Chase, CameraMode.Chase), (CameraRigPreset.VehicleChase, CameraMode.Chase), (CameraRigPreset.Orbit, CameraMode.Orbit),
            (CameraRigPreset.FirstPerson, CameraMode.FirstPerson), (CameraRigPreset.Seat, CameraMode.Seat), (CameraRigPreset.TopDown, CameraMode.TopDown),
            (CameraRigPreset.Fixed, CameraMode.Fixed),
        };
        foreach (var (p, m) in want)
            if (CameraRig.ModeOf(p) != m) return Fail($"{p} should start in {m}, got {CameraRig.ModeOf(p)}");
        Console.WriteLine("  PASS");
        return 0;
    }

    // A dead handle or a missing native library answers false/0 from every member, never throws.
    private static int TestUnavailableCameraIsNotAFailure()
    {
        Console.WriteLine("Test: a dead entity and an absent camera system answer false/0 (skipped when the native libraries are absent)");
        try
        {
            Entity dead = Entity.None;
            bool any = dead.AddCameraRig() || dead.HasCameraRig || dead.RemoveCameraRig() || dead.SetCameraRigMode(CameraMode.Orbit) ||
                       dead.SetCameraRigTarget(dead) || dead.SetCameraRigArm(300f) || dead.SetCameraRigSocket(new Aver.Scene.Vec3(1, 2, 3)) ||
                       dead.SetCameraRigPivot(new Aver.Scene.Vec3(0, 0, 100)) || dead.SetCameraRigLag(5f) || dead.SetCameraRigLookAhead(0.2f) ||
                       dead.SetCameraRigPitchLimits(-60f, 60f) || dead.AddCameraRigLook(1f, 1f) || dead.SetCameraRigFade(100f, 50f) ||
                       dead.SetCameraRigBlend(0.4f) || dead.SetCameraRigIgnoreBody(1) || dead.SetCameraRigFlags(CameraRigFlags.NoFade) ||
                       dead.TryGetCameraRigState(out _) || CameraRig.SetViewTarget(dead) || CameraRig.TryGetPose(out _);
            if (any) return Fail("a member reported success on Entity.None");
            if (CameraRig.PushFovKick(5f) != 0 || CameraRig.PushFovScale(0.7f) != 0 || CameraRig.PushPunch(2f) != 0 || CameraRig.PushRoll(3f) != 0 ||
                CameraRig.PushArmScale(1.2f) != 0 || CameraRig.PushSocketOffset(new Aver.Scene.Vec3(0, 40, 0)) != 0)
                return Fail("a modifier push returned an id with no camera system running");
            if (CameraRig.ClearModifiers() != 0 || CameraRig.RemoveModifier(1)) return Fail("a clear or remove reported success with nothing running");
            Console.WriteLine("  PASS");
        }
        catch (Exception e) when (NodeRowKit.NativeMissing(e))
        {
            Console.WriteLine("  SKIP: native libraries not loadable in this process");
        }
        return 0;
    }
}
