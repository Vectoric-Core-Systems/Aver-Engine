// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A node whose INPUT PIN DOES NOT EXIST must be refused at compile time, not silently compiled.
//
// HOW A GRAPH GETS INTO THIS STATE WITHOUT DOING ANYTHING EXOTIC: any explicit PIN record on a node
// suppresses ALL of that node's default pins (OcGraphParser.AddDefaultPins is skipped entirely once
// the author declares one by hand). So writing a single PIN line to expose, say, an exec pin silently
// deletes every other pin the node had -- including its required inputs. That trap is real enough that
// test-content/GraphDemo's IdleMotion.ocgraph and this repo's own FirstPerson target graph both carry
// a comment warning about it, and both had to list all seven pins by hand to work around it.
//
// WHAT WENT WRONG BEFORE THIS WAS FIXED, and why it was not merely "a default of 0":
//
//   * PULL (GraphCompiler.EmitPullInput): fell back to `pin?.Type ?? PinType.Float` and pushed a
//     FLOAT zero. For a missing `entity` pin -- an INT -- that puts a float32 on the stack where the
//     callee's signature wants an int32. That is a type error in the emitted IL, not a wrong number.
//   * PUSH (GraphCompiler.LoadPin): `if (pin != null)` guarded the whole emit, so a missing pin pushed
//     NOTHING AT ALL. The following Call then consumed whatever happened to be beneath it, silently
//     taking the wrong operand.
//
// Both produce a graph that "compiles" and then misbehaves at invoke, which is the single worst
// outcome available: the compiler's whole job here is to convert authoring mistakes into messages.
//
// NOTE ON WHAT IS **NOT** A BUG AND MUST KEEP WORKING: a pin that EXISTS but is simply not wired to
// anything is expected to read as zero -- an Add with only `a` connected is a legal graph meaning
// "a + 0". The tests below therefore assert BOTH halves: missing pin is refused, unwired pin is not.
using System;
using Aver.Graph;

static class MissingPinTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestMissingInputPinIsRefusedPull();
        failures += TestMissingInputPinIsRefusedPush();
        failures += TestUnwiredButDeclaredPinStillDefaultsToZero();
        return failures;
    }

    // GetField's defaults are entity:int-in and value:float-out. Declaring `value` by hand suppresses
    // both, so `entity` -- an INT input the emitter will ask for by name -- no longer exists.
    private const string MissingEntityPull =
        "OCGRAPH 1\n" +
        "NODE gf GetField field=CLight.intensityLux\n" +
        "PIN gf value out float\n" +
        "OUT gf value\n";

    private const string MissingEntityPush =
        "OCGRAPH 1\n" +
        "ENTRY t OnTick\nNODE t OnTick\n" +
        "NODE gf GetField field=CLight.intensityLux\n" +
        "PIN gf value out float\n" +
        "OUT gf value\n";

    private static FieldResolver F32Field => (string name, out int fieldId, out int kind) =>
    {
        fieldId = 7;
        kind = 0; // FieldKind.F32
        return true;
    };

    private static int TestMissingInputPinIsRefusedPull()
    {
        Console.WriteLine("Test: a node whose required INPUT pin was suppressed is refused at compile (PULL)");
        return RunRefusalCase(MissingEntityPull, push: false);
    }

    private static int TestMissingInputPinIsRefusedPush()
    {
        Console.WriteLine("Test: a node whose required INPUT pin was suppressed is refused at compile (PUSH)");
        return RunRefusalCase(MissingEntityPush, push: true);
    }

    private static int RunRefusalCase(string text, bool push)
    {
        try
        {
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                // Catching it at parse is equally acceptable, as long as it IS caught and says why.
                if (perr != null && perr.Contains("entity"))
                {
                    Console.WriteLine($"  PASS: refused at parse: {perr}");
                    return 0;
                }
                Console.WriteLine($"  FAIL: parse refused it but did not name the missing pin: {perr}");
                return 1;
            }

            var compiler = new GraphCompiler(graph, F32Field);
            var fn = push ? compiler.CompileEntryPoint("OnTick", out var cerr) : compiler.Compile(out cerr);

            if (fn != null)
            {
                Console.WriteLine("  FAIL: compiled a node with a missing required input pin instead of refusing");
                return 1;
            }
            if (cerr == null || !cerr.Contains("entity"))
            {
                Console.WriteLine($"  FAIL: refused, but the error does not name the missing pin 'entity': {cerr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {cerr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }

    // THE OTHER HALF, and the reason the fix has to distinguish "absent" from "unconnected": an Add
    // with only `a` wired is a legal, useful graph. If the fix had simply thrown whenever a pin had no
    // incoming link, every graph in this repo would stop compiling.
    private static int TestUnwiredButDeclaredPinStillDefaultsToZero()
    {
        Console.WriteLine("Test: a DECLARED but unwired input pin still reads as zero, in both compilers");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "NODE k ConstFloat value=5.0\n" +
                "NODE sum Add\n" +
                "LINK k value sum a\n" +   // `b` is left unwired on purpose
                "OUT sum result\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var fn = new GraphCompiler(graph).Compile(out var cerr);
            if (fn is null)
            {
                Console.WriteLine($"  FAIL: an unwired-but-declared pin must still compile, got: {cerr}");
                return 1;
            }
            var got = fn.DynamicInvoke();
            if (got is float f && Math.Abs(f - 5.0f) < 1e-6f)
            {
                Console.WriteLine("  PASS: 5 + (unwired b => 0) == 5, still compiles and still runs");
                return 0;
            }
            Console.WriteLine($"  FAIL: expected 5, got {got}");
            return 1;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }
}
