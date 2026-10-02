// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for SetControlRig -- the node that makes a control rig reachable from a LEVEL.
//
// WHY THIS NODE MATTERS MORE THAN ITS SIZE SUGGESTS. Everything under it was already built and
// tested -- anim::twoBoneIk and anim::aimAt (IkTest), the .ocrig format (OcRigTest), CControlRig
// applied through AnimSystem's pose-modifier seam (ControlRigTest) -- but a rig could only ever be
// attached from C++. An .ocworld PLACE record has no skeleton field, so a placed mesh is a static
// mesh with no pose to modify, and a skinned character reaches a level through a graph CLASS
// instead. That made this node the missing link rather than a new level-format attribute.
//
// THE PROOF IS AN EXCEPTION, exactly as in SynapseNodeTests.cs and SaveLoadGameNodeTests.cs -- see
// SynapseNodeTests.cs's own header for the full argument. There is no engine build near this test
// assembly's output, so the first native call SetControlRig makes throws
// EntryPointNotFoundException naming the symbol it wanted. Here that symbol is aver_scene_component,
// which is a sharper result than usual: CControlRig is registered at RUNTIME rather than being a
// built-in, so it is the ONE component in the animation family that cannot be attached by a
// Component enum value. Landing on aver_scene_component proves the node reached the by-name path
// (scene ABI 1.4) and not some built-in id that happened to compile.
//
// It also proves the IL is well-formed all the way to a THREE-argument call: a stack-shape mistake
// surfaces as InvalidProgramException or a verification failure at invoke time, not as
// EntryPointNotFoundException, so getting this exception means entity, rig and weight were all
// pushed correctly.
//
// WHAT THIS FILE CANNOT PROVE, STATED PLAINLY. It shows that a float reaches the call; it cannot
// show WHICH float. Both compilers fall back to a literal 0 for an input pin with no LINK and no
// PINVAL, and for this one node that zero is actively harmful -- a rig bound at weight 0 loads,
// validates, resolves its bone names and then scales every op to nothing, indistinguishable from a
// rig authored for the wrong skeleton. GraphCompiler.IsPinUnwired exists to make that default 1
// instead. Asserting the constant would mean reading the emitted body, and these delegates are
// DynamicMethods whose GetMethodBody() throws, so a test written that way would SKIP every run and
// report green while checking nothing. The value is asserted instead in C++, in
// tests/editor/src/GraphEditorLoadSaveTest.cpp, on the path a person actually takes: the editor
// writing weight's default into a file when it creates the node.
using System;
using Aver.Graph;

static class ControlRigNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestDefaultPinsShape();
        failures += TestRigAttributeParses();
        failures += TestMissingRigAttributeIsRefused(usePushCompiler: false);
        failures += TestMissingRigAttributeIsRefused(usePushCompiler: true);
        failures += TestReachesTheByNameComponentLookup(usePushCompiler: false);
        failures += TestReachesTheByNameComponentLookup(usePushCompiler: true);
        failures += TestExplicitWeightCompilesAndReachesTheCall();
        return failures;
    }

    private static int TestDefaultPinsShape()
    {
        Console.WriteLine("Test: SetControlRig's default pins are entity:int-in, weight:float-in, success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE r SetControlRig rig=Content/Rigs/ArmReach.ocrig\nOUT r success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["r"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "weight" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 3 &&
                node.Pins.TrueForAll(p => p.Type != PinType.Exec);
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 3 pins, exactly the declared shape, and no exec pins");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestRigAttributeParses()
    {
        Console.WriteLine("Test: rig= lands in Node.RigPath verbatim, like skeleton= and clip= before it");
        try
        {
            const string path = "Content/Rigs/ArmReach.ocrig";
            var text = $"OCGRAPH 1\nNODE r SetControlRig rig={path}\nOUT r success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.Nodes["r"].RigPath != path)
            {
                Console.WriteLine($"  FAIL: RigPath is '{graph.Nodes["r"].RigPath ?? "(null)"}', expected '{path}'");
                return 1;
            }
            Console.WriteLine($"  PASS: RigPath == '{path}'");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // A rig= that is missing must fail at COMPILE time and say so. Without this the node would emit a
    // call with an empty asset path, which hashes to a perfectly valid-looking id that resolves to no
    // file -- a rig that silently never loads, reported only as a runtime warning.
    private static int TestMissingRigAttributeIsRefused(bool usePushCompiler)
    {
        string which = usePushCompiler ? "PUSH" : "PULL";
        Console.WriteLine($"Test: a SetControlRig with no rig= is refused by the {which} compiler, naming the node");
        try
        {
            var text = usePushCompiler
                ? @"
OCGRAPH 1
NODE tick OnTick
NODE r SetControlRig
PIN r exec in exec
PIN r entity in int
PIN r weight in float
PIN r then out exec
PIN r success out bool
LINK tick.exec r.exec
ENTRY tick OnTick
OUT r success
"
                : "OCGRAPH 1\nNODE r SetControlRig\nOUT r success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            try
            {
                var fn = usePushCompiler
                    ? compiler.CompileEntryPoint("OnTick", out var cerr)
                    : compiler.Compile(out cerr);
                // A compiler that reports the problem as an error string rather than throwing is
                // equally acceptable -- what must NOT happen is a usable delegate.
                if (fn == null)
                {
                    Console.WriteLine($"  PASS: refused with an error rather than compiling: {cerr ?? "(no message)"}");
                    return 0;
                }
                Console.WriteLine("  FAIL: compiled a SetControlRig with no rig= attribute");
                return 1;
            }
            catch (InvalidOperationException ex) when (ex.Message.Contains("rig="))
            {
                Console.WriteLine($"  PASS: refused, naming the missing attribute: {ex.Message}");
                return 0;
            }
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}"); return 1; }
    }

    private static int TestReachesTheByNameComponentLookup(bool usePushCompiler)
    {
        string which = usePushCompiler ? "PUSH" : "PULL";
        Console.WriteLine($"Test: SetControlRig through the {which} compiler reaches aver_scene_component, the by-NAME attach path");
        try
        {
            var text = usePushCompiler
                ? @"
OCGRAPH 1
NODE tick OnTick
NODE ent ConstInt value=4242
NODE r SetControlRig rig=Content/Rigs/ArmReach.ocrig
PIN r exec in exec
PIN r entity in int
PIN r weight in float
PIN r then out exec
PIN r success out bool
LINK tick.exec r.exec
LINK ent.value r.entity
ENTRY tick OnTick
OUT r success
"
                : "OCGRAPH 1\n" +
                  "NODE ent ConstInt value=4242\n" +
                  "NODE r SetControlRig rig=Content/Rigs/ArmReach.ocrig\n" +
                  "LINK ent.value r.entity\n" +
                  "OUT r success\n";
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
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header) but it returned {got}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_component"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_component -- " +
                                      $"which would mean the node attached by a BUILT-IN id instead of by name: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_component', proving the dynamic by-name attach path: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}"); return 1; }
    }

    // A weight the author DID set must reach the call -- 0 included, since "0 turns the whole rig
    // off without detaching it" is the documented meaning of the pin. PINVAL, not PIN: a PIN record
    // is a pin DECLARATION and declaring any pin by hand suppresses every default pin on that node.
    private static int TestExplicitWeightCompilesAndReachesTheCall()
    {
        Console.WriteLine("Test: an explicitly set weight of 0 compiles and still reaches the attach call");
        try
        {
            const string text = @"
OCGRAPH 1
NODE ent ConstInt value=4242
NODE r SetControlRig rig=Content/Rigs/ArmReach.ocrig
LINK ent.value r.entity
PINVAL r weight 0
OUT r success
";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            if (graph.PinnedValues.Find(p => p.NodeId == "r" && p.PinName == "weight") == null)
            {
                Console.WriteLine("  FAIL: the PINVAL did not land on the weight pin");
                return 1;
            }
            var fn = new GraphCompiler(graph).Compile(out var cerr);
            if (fn is not Func<bool> typed)
            {
                Console.WriteLine($"  FAIL: compile: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool got = typed();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {got}");
                return 1;
            }
            catch (EntryPointNotFoundException)
            {
                Console.WriteLine("  PASS: an author-set weight compiles and the call is still reached");
                return 0;
            }
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}"); return 1; }
    }
}
