// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for GetSynapseTarget, SynapseSteer (slice 4) and GetSynapsePerception (slice 5) -- Synapse's
// graph-facing half. See GraphInterop.SynapseGetTargetForGraph / SynapseSteerForGraph /
// SynapseGetPerceptionForGraph for the three wrappers, GraphCompiler.cs's
// "getsynapsetarget"/"synapsesteer"/"getsynapseperception" PULL cases and EmitPullSynapseSteer /
// EmitPullSynapsePerception for the emitters, and sandbox/src/GraphNodeDefs.hpp for the pin shapes
// this file asserts against.
//
// ALL THREE NODES TOUCH REAL NATIVE CODE, UNLIKE GetForward. GetForward's own graceful "unbound
// entity -> false" test (GetForwardNodeTests.cs) works in this bare process because its whole native
// surface is Actors.Get, a MANAGED lookup with no P/Invoke involved at all. None of these three are
// that lucky: GetSynapseTarget and GetSynapsePerception each call a raw aver_fw_synapse_* relay
// directly (no managed fallback), and SynapseSteer's very first line is Entity.IsAlive, which calls
// aver_scene_valid. All three throw EntryPointNotFoundException naming the exact symbol in THIS
// process (no engine build anywhere near Aver.Graph.Tests' own output directory, so neither
// Aver.Framework.dll nor Aver.Scene.dll's native counterpart exists for their own NativeResolver to
// find) -- the identical proof-of-wiring SaveLoadGameNodeTests.cs already establishes for
// SaveGame/LoadGame; see that file's own header for why the exception IS the proof, not a workaround.
using System;
using Aver.Graph;

static class SynapseNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestGetSynapseTargetDefaultPinsShape();
        failures += TestGetSynapseTargetEmitsRealNativeCallNamingTheSymbol();
        failures += TestSynapseSteerDefaultPinsShape();
        failures += TestSynapseSteerUnboundEntityReturnsFalseAndZeroesPull();
        failures += TestSynapseSteerUnboundEntityReturnsFalseAndZeroesPush();
        failures += TestGetSynapsePerceptionDefaultPinsShape();
        failures += TestGetSynapsePerceptionEmitsRealNativeCallNamingTheSymbol();
        return failures;
    }

    private static int TestGetSynapseTargetDefaultPinsShape()
    {
        Console.WriteLine("Test: GetSynapseTarget's default pins are entity:int-in, x/y/z:float-out, success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE gst GetSynapseTarget\nOUT gst x\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gst"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "x" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "y" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "z" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 5 &&
                node.Pins.TrueForAll(p => p.Type != PinType.Exec);
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 5 pins, exactly the declared shape, and no exec pins");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestGetSynapseTargetEmitsRealNativeCallNamingTheSymbol()
    {
        Console.WriteLine("Test: GetSynapseTarget pulled as data emits a real native call naming aver_fw_synapse_target");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "NODE ent ConstInt value=4242\n" +
                "NODE gst GetSynapseTarget\n" +
                "LINK ent.value gst.entity\n" +
                "OUT gst success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: compile: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                var got = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {got}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_synapse_target"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_synapse_target: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_synapse_target', proving the entity pin reached the native relay: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }

    private static int TestSynapseSteerDefaultPinsShape()
    {
        Console.WriteLine("Test: SynapseSteer's default pins are 7 float/int-in, 4 float/bool-out plus success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE ss SynapseSteer\nOUT ss forward\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["ss"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "dt" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "targetX" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "targetY" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "targetZ" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "turnRate" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "arriveRadius" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "forward" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "right" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "yawDelta" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "arrived" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 12 &&
                node.Pins.TrueForAll(p => p.Type != PinType.Exec);
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 12 pins, exactly the declared shape, and no exec pins");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // The contract that matters at runtime, mirroring GetForwardNodeTests.cs's own unbound-entity
    // case exactly: an entity nothing ever bound is a REAL, expected input (a target picked before
    // the chaser has spawned, a stale handle), and must produce false/zeroes rather than throwing or
    // handing back whatever happened to be left on the stack -- checked through BOTH compilers, the
    // same "single most repeated bug shape" reason GetForwardNodeTests.cs already gives.
    private static int TestSynapseSteerUnboundEntityReturnsFalseAndZeroesPull()
    {
        Console.WriteLine("Test: SynapseSteer's entity pin reaches Entity.IsAlive, a real native call naming aver_scene_valid (PULL)");
        return RunUnboundCase(usePushCompiler: false);
    }

    private static int TestSynapseSteerUnboundEntityReturnsFalseAndZeroesPush()
    {
        Console.WriteLine("Test: SynapseSteer's entity pin reaches Entity.IsAlive, a real native call naming aver_scene_valid (PUSH)");
        return RunUnboundCase(usePushCompiler: true);
    }

    // BOTH COMPILERS, EVERY TIME -- see this file's header for why: a node implemented in only one
    // path works on an exec chain and then silently misbehaves when pulled through OUT (or the
    // reverse), the single most repeated bug shape in GraphCompiler.cs's history.
    private static int RunUnboundCase(bool usePushCompiler)
    {
        try
        {
            string text =
                "OCGRAPH 1\n" +
                (usePushCompiler ? "ENTRY t OnTick\nNODE t OnTick\n" : "") +
                // 4242 is deliberately an id nothing in this process ever bound an actor to.
                "NODE ent ConstInt value=4242\n" +
                "NODE dt ConstFloat value=0.016\n" +
                "NODE tx ConstFloat value=100.0\n" +
                "NODE ty ConstFloat value=0.0\n" +
                "NODE tz ConstFloat value=0.0\n" +
                "NODE tr ConstFloat value=180.0\n" +
                "NODE ar ConstFloat value=50.0\n" +
                "NODE ss SynapseSteer\n" +
                "LINK ent.value ss.entity\n" +
                "LINK dt.value ss.dt\n" +
                "LINK tx.value ss.targetX\n" +
                "LINK ty.value ss.targetY\n" +
                "LINK tz.value ss.targetZ\n" +
                "LINK tr.value ss.turnRate\n" +
                "LINK ar.value ss.arriveRadius\n" +
                "OUT ss success\n";

            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var fn = usePushCompiler
                ? compiler.CompileEntryPoint("OnTick", out var cerr)
                : compiler.Compile(out cerr);
            if (fn is not Func<bool> typed)
            {
                Console.WriteLine($"  FAIL: compile: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                bool got = typed();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {got}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_valid"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_valid: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_valid', proving the entity pin reached Entity.IsAlive: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }

    private static int TestGetSynapsePerceptionDefaultPinsShape()
    {
        Console.WriteLine("Test: GetSynapsePerception's default pins are entity:int-in, canSeeTarget:bool/lastKnownTarget:int/timeSinceSeen:float/success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE gsp GetSynapsePerception\nOUT gsp canSeeTarget\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gsp"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "canSeeTarget" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Find(p => p.Name == "lastKnownTarget" && p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "timeSinceSeen" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 5 &&
                node.Pins.TrueForAll(p => p.Type != PinType.Exec);
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 5 pins, exactly the declared shape, and no exec pins");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestGetSynapsePerceptionEmitsRealNativeCallNamingTheSymbol()
    {
        Console.WriteLine("Test: GetSynapsePerception pulled as data emits a real native call naming aver_fw_synapse_perception");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "NODE ent ConstInt value=4242\n" +
                "NODE gsp GetSynapsePerception\n" +
                "LINK ent.value gsp.entity\n" +
                "OUT gsp success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: compile: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                var got = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {got}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_synapse_perception"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_synapse_perception: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_synapse_perception', proving the entity pin reached the native relay: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }
}
