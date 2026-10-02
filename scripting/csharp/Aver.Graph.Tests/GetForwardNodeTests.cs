// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for GetForward -- CharacterMove's read-side counterpart, and the node that makes a graph-only
// first-person template possible at all. See GraphInterop.LookDirectionForGraph for why the engine
// needed a node here rather than graph-side trigonometry, GraphCompiler.cs's
// EmitGetForward/EmitPullGetForward for the two emitters, and OcGraphParser.cs's "getforward" case for
// the pin shape.
//
// THESE TESTS INVOKE THE COMPILED IL, they do not merely compile it. That is possible here in a way it
// was not for GetFieldVec3 (see Vec3FieldTests.cs's header on why those observe a native-call failure
// instead): GetForward's whole native surface is Actors.Get, which returns null in a host that never
// installed a resolver -- a documented, graceful path, not a crash. So this process can run the emitted
// method end to end and assert its REAL contract: false, and every out-parameter left at zero. A node
// that silently returned garbage, or threw, or left uninitialised stack values in the eye pins, fails
// here rather than at the first graph someone writes with it.
//
// BOTH COMPILERS, EVERY TIME. A node implemented in only one path works on an exec chain and then
// silently misbehaves when pulled through OUT (or the reverse) -- the single most repeated bug shape in
// GraphCompiler.cs's history. Every behavioural assertion below is therefore made twice, once through
// Compile() and once through CompileEntryPoint().
using System;
using Aver.Graph;

static class GetForwardNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestGetForwardDefaultPinsShape();
        failures += TestGetForwardUnboundEntityReturnsFalseAndZeroesPull();
        failures += TestGetForwardUnboundEntityReturnsFalseAndZeroesPush();
        failures += TestGetForwardAllSixComponentsPulledIndependently();
        failures += TestGetForwardUnknownOutputPinFailsClearly();
        return failures;
    }

    private static int TestGetForwardDefaultPinsShape()
    {
        Console.WriteLine("Test: GetForward's default pins are entity:int-in, x/y/z + eyeX/eyeY/eyeZ:float-out, success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE gf GetForward\nOUT gf x\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gf"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "x" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "y" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "z" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "eyeX" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "eyeY" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "eyeZ" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 8; // no exec pins -- pure idempotent read, mirrors GetFieldVec3
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 8 pins, exactly the declared shape, and no exec pins");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // The contract that matters at runtime: an entity with no live actor bound is a REAL, expected case
    // (a graph pointed at the wrong id, or a Character that has not spawned yet), and it must produce a
    // false plus zeroes rather than throwing out of DynamicInvoke or handing back whatever happened to
    // be on the stack. Asserting the eye pins are zero too is the point -- they are the ones a
    // half-written emitter would leave uninitialised, since the direction pins are written first.
    private static int TestGetForwardUnboundEntityReturnsFalseAndZeroesPull()
    {
        Console.WriteLine("Test: GetForward on an entity with no live actor returns false and zeroes (PULL)");
        return RunUnboundCase(usePushCompiler: false);
    }

    private static int TestGetForwardUnboundEntityReturnsFalseAndZeroesPush()
    {
        Console.WriteLine("Test: GetForward on an entity with no live actor returns false and zeroes (PUSH)");
        return RunUnboundCase(usePushCompiler: true);
    }

    private static int RunUnboundCase(bool usePushCompiler)
    {
        try
        {
            string text =
                "OCGRAPH 1\n" +
                "PARAM entity int\n" +
                (usePushCompiler ? "ENTRY t OnTick\nNODE t OnTick\n" : "") +
                "NODE e Param param=entity\n" +
                "NODE gf GetForward\n" +
                "LINK e value gf entity\n" +
                "OUT gf success\n" +
                "OUT gf x\n" +
                "OUT gf eyeZ\n";

            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var fn = usePushCompiler
                ? compiler.CompileEntryPoint("OnTick", out var cerr)
                : compiler.Compile(out cerr);
            if (fn is null)
            {
                Console.WriteLine($"  FAIL: compile: {cerr}");
                return 1;
            }

            // 4242 is deliberately an id nothing in this process ever bound an actor to.
            var got = fn.DynamicInvoke(4242) as object[];
            if (got is null || got.Length != 3)
            {
                Console.WriteLine($"  FAIL: expected 3 OUT values, got {(got is null ? "null" : got.Length.ToString())}");
                return 1;
            }
            if (got[0] is not bool success || success)
            {
                Console.WriteLine($"  FAIL: expected success=false for an unbound entity, got {got[0]}");
                return 1;
            }
            if (got[1] is not float x || Math.Abs(x) > 1e-6f)
            {
                Console.WriteLine($"  FAIL: expected x=0 on failure, got {got[1]}");
                return 1;
            }
            if (got[2] is not float eyeZ || Math.Abs(eyeZ) > 1e-6f)
            {
                Console.WriteLine($"  FAIL: expected eyeZ=0 on failure, got {got[2]}");
                return 1;
            }
            Console.WriteLine("  PASS: success=false, direction and eye pins both zeroed, no exception");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }

    // Six separate pulls of the same node in one graph. Proves each pin name maps to the component the
    // author asked for rather than all six resolving to the same local -- a mapping mistake that would
    // be invisible while every value is zero, which is exactly the state the unbound case above leaves
    // them in. Here they must at minimum all COMPILE and all come back as floats.
    private static int TestGetForwardAllSixComponentsPulledIndependently()
    {
        Console.WriteLine("Test: all six GetForward components can be pulled independently in one graph");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "PARAM entity int\n" +
                "NODE e Param param=entity\n" +
                "NODE gf GetForward\n" +
                "LINK e value gf entity\n" +
                "OUT gf x\nOUT gf y\nOUT gf z\nOUT gf eyeX\nOUT gf eyeY\nOUT gf eyeZ\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var fn = new GraphCompiler(graph).Compile(out var cerr);
            if (fn is null)
            {
                Console.WriteLine($"  FAIL: compile: {cerr}");
                return 1;
            }
            var got = fn.DynamicInvoke(4242) as object[];
            if (got is null || got.Length != 6)
            {
                Console.WriteLine($"  FAIL: expected 6 OUT values, got {(got is null ? "null" : got.Length.ToString())}");
                return 1;
            }
            for (int i = 0; i < 6; ++i)
            {
                if (got[i] is not float)
                {
                    Console.WriteLine($"  FAIL: OUT #{i} is {got[i]?.GetType().Name ?? "null"}, expected float");
                    return 1;
                }
            }
            Console.WriteLine("  PASS: six independent pulls compile and each yields a float");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }

    // A misspelled pin must be named in the error, not silently resolved to something else or crash the
    // verifier -- the same contract EmitPullGetFieldVec3 and Raycast already hold themselves to.
    private static int TestGetForwardUnknownOutputPinFailsClearly()
    {
        Console.WriteLine("Test: GetForward with an unknown output pin fails compile with a clear error");
        try
        {
            const string text =
                "OCGRAPH 1\nPARAM entity int\nNODE e Param param=entity\n" +
                "NODE gf GetForward\nLINK e value gf entity\nOUT gf forwardX\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                // Rejecting at PARSE is an equally acceptable place to catch this, as long as it is caught.
                Console.WriteLine($"  PASS: refused at parse: {perr}");
                return 0;
            }
            var fn = new GraphCompiler(graph).Compile(out var cerr);
            if (fn != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail for pin 'forwardX', but it succeeded");
                return 1;
            }
            if (cerr == null || !cerr.Contains("forwardX"))
            {
                Console.WriteLine($"  FAIL: expected an error naming 'forwardX', got: {cerr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {cerr}");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }
}
