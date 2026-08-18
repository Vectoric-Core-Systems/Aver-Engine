// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Test suite for Aver.Graph: parsing and compilation.
// Comment explains WHY: this slice proves the property that a graph loaded from text,
// compiled to IL, and invoked returns the same value as hand-written C#.
// Tests verify that both C# format (integer node IDs as strings) and C++ format
// (string node IDs, dot notation links) are supported.

using System;
using System.Collections.Generic;
using Aver.Graph;

class Program
{
    // THE LITERAL'S SHAPE MUST NOT DECIDE THE CONSTANT'S TYPE. `NODE x ConstFloat value=100` -- no
// decimal point, which is a perfectly ordinary way to write an integer-valued float -- used to be
// boxed as an int by the NODE-line value= parser, and both compilers read a ConstFloat's constant
// with a strict `is float` and fell back to 0 with no warning. So it compiled to 0.0f, silently, in
// BOTH paths. The mirrors were just as bad: ConstInt value=7.0 became 0, ConstBool value=1 became
// false. It hid for so long because every sample in this tree writes float literals with a decimal
// point by convention, so the ambiguity never arose in a test.
//
// Asserted through BOTH compilers on purpose: the two constant-reading sites are separate pieces of
// code (EmitConstFloat and EmitPullOutput's "constfloat" case) and fixing one would not have fixed
// the other.
static int TestConstLiteralShapeDoesNotDecideType()
{
    int failures = 0;
    Console.WriteLine("Test: an integer-looking ConstFloat literal keeps its authored value");

    // PULL: a bare dataflow graph, no ENTRY, compiled by Compile().
    {
        const string text = "OCGRAPH 1\nNODE k ConstFloat value=100\nOUT k value\n";
        if (!OcGraphParser.Parse(text, out var g, out var perr))
        { Console.WriteLine($"  FAIL: parse: {perr}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn is null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        var got = fn.DynamicInvoke();
        if (got is float f && Math.Abs(f - 100f) < 1e-6f)
            Console.WriteLine("  PASS: PULL compiler reads value=100 as 100.0f, not 0");
        else { Console.WriteLine($"  FAIL: PULL gave {got}, expected 100"); ++failures; }
    }

    // PUSH: the same constant pulled through an ENTRY-driven exec graph.
    {
        const string text = "OCGRAPH 1\nENTRY t OnTick\nNODE t OnTick\n"
                          + "NODE k ConstFloat value=100\nOUT k value\n";
        if (!OcGraphParser.Parse(text, out var g, out var perr))
        { Console.WriteLine($"  FAIL: parse: {perr}"); return 1; }
        var fn = new GraphCompiler(g).CompileEntryPoint("OnTick", out var cerr);
        if (fn is null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        var got = fn.DynamicInvoke();
        if (got is float f && Math.Abs(f - 100f) < 1e-6f)
            Console.WriteLine("  PASS: PUSH compiler reads value=100 as 100.0f, not 0");
        else { Console.WriteLine($"  FAIL: PUSH gave {got}, expected 100"); ++failures; }
    }

    // The mirrors, which were equally broken in the other direction.
    {
        const string text = "OCGRAPH 1\nNODE i ConstInt value=7\nNODE b ConstBool value=true\n"
                          + "OUT i value\nOUT b value\n";
        if (!OcGraphParser.Parse(text, out var g, out var perr))
        { Console.WriteLine($"  FAIL: parse: {perr}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn is null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        var got = fn.DynamicInvoke() as object[];
        if (got is not null && got.Length == 2 && got[0] is int i7 && i7 == 7 && got[1] is bool bt && bt)
            Console.WriteLine("  PASS: ConstInt value=7 and ConstBool value=true survive");
        else { Console.WriteLine("  FAIL: ConstInt/ConstBool did not survive"); ++failures; }
    }

    // A literal that is not valid FOR ITS OWN TYPE is now refused by name at parse, rather than
    // compiling to zero. Silence was the bug; an error is the fix.
    {
        const string text = "OCGRAPH 1\nNODE k ConstInt value=notanumber\nOUT k value\n";
        if (OcGraphParser.Parse(text, out _, out var perr))
        { Console.WriteLine("  FAIL: a bad ConstInt literal parsed successfully"); ++failures; }
        else if (perr is not null && perr.Contains("value="))
            Console.WriteLine($"  PASS: refused by name: {perr}");
        else { Console.WriteLine($"  FAIL: refused, but unhelpfully: {perr}"); ++failures; }
    }

    return failures;
}

static int Main()
    {
        int failures = 0;

        failures += TestConstFloat();
        failures += TestConstInt();
        failures += TestConstBool();
        failures += TestAdd();
        failures += TestMultiply();
        failures += TestCompare();
        failures += TestAddThenMultiply();
        failures += TestCppFormat();
        failures += TestNameAndDescription();
        failures += TestCrossImplementationFixture();

        // PARAM (Compile() taking arguments) and the new math nodes.
        failures += TestParamEntityPassthrough();
        failures += TestParamTimeIntoSin();
        failures += TestParamOrderMatters();
        failures += TestCosNode();
        failures += TestDivideByZeroGuard();
        failures += TestDivideNormal();
        failures += TestSubtractNode();
        failures += TestParamMissingAttributeFailsToParse();
        failures += TestParamUndeclaredFailsToParse();
        failures += TestGraphWithoutParamsStillHasNoArguments();

        // getfield/setfield: real IL, checked at compile time. None of these boot a native scene
        // (nothing in this test process does -- see the class comment above these tests), so they
        // either exercise compile-time validation with a fake FieldResolver, or prove the emitted
        // IL performs a genuine P/Invoke call by observing that INVOKING it fails to load the
        // native library, rather than silently returning a stubbed constant.
        failures += TestGetFieldMissingFieldAttributeFailsCompile();
        failures += TestGetFieldUnknownFieldFailsCompile();
        failures += TestGetFieldWrongKindFailsCompile();
        failures += TestGetFieldEmitsRealNativeCall();
        failures += TestSetFieldEmitsRealNativeCall();

        // GraphHost: load-once/compile-once hosting, and the drone flight-path graph itself.
        failures += GraphHostTests.RunAll();

        // Exec/flow: branch, sequence, while, forEach, and event entry points (ENTRY/CompileEntryPoint).
        failures += GraphFlowTests.RunAll();

        // Select, InputKey, Raycast: the three node types this slice adds on top of the exec compiler.
        failures += TestConstLiteralShapeDoesNotDecideType();
        failures += NewNodeTests.RunAll();

        // GetFieldVec3/SetFieldVec3: the Vec3 half of GetField/SetField's own FieldKindF32 gap.
        failures += Vec3FieldTests.RunAll();

        // Spawn: the README's "Spawning/destroying an entity" row -- wraps Actors.Spawn, exec-only,
        // more strictly refused by the PULL compiler than any prior side-effecting node.
        failures += SpawnNodeTests.RunAll();

        // OnHit: the README's "Was I just hit" row -- a third, HOST-FIRED-ON-DEMAND entry point
        // (GraphHost.Fire), plus the widened PARAM contract that lets a hit's payload actually reach
        // the graph. See GraphHost.cs's own PHASE 3 comment.
        failures += OnHitEventTests.RunAll();

        // Independent adversarial pass over OnHit/Fire(), written separately from OnHitEventTests.cs
        // above: empty graph, missing node, duplicate ENTRY, missing attribute, a backwards LINK, a
        // duplicated node id, an exec cycle, event-name case sensitivity, and the Tick()/Fire()
        // disjointness boundary approached from the OnTick side instead of the OnHit side.
        failures += OnHitAdversarialTests.RunAll();

        // Gap B cross-language proof: reads attribute_edit_test.output.ocgraph, the file
        // tests/editor/src/GraphEditorLoadSaveTest.cpp's testAttributeEditRoundTrip() writes from a
        // REAL GraphEditor::setAttribute()/save() call -- not a fixture this suite authored itself. See
        // AttributeEditRoundTripTests.cs's own header comment; run GraphEditorLoadSaveTest.exe first.
        failures += AttributeEditRoundTripTests.RunAll();

        // Graph-local persistent variables (VAR/GetVar/SetVar/GraphVarStore): closes "nothing survives
        // between ticks" -- see GraphVarTests.cs's own header comment.
        failures += GraphVarTests.RunAll();

        // REVIEWER'S OWN independent adversarial pass, written separately from GraphVarTests.cs above,
        // targeting specifically the per-host-instance-not-per-graph-file sharing bug this design lives
        // or dies on -- see VarIsolationTests.cs's own header comment.
        failures += VarIsolationTests.RunAll();

        // CharacterMove: the last Blueprint-parity node -- one coarse, exec-only wrapper around
        // AverCharacter.Drive. See CharacterMoveNodeTests.cs's own header comment for why its two
        // failure paths (no actor / wrong actor type) are genuine end-to-end behavioural proofs,
        // unlike every prior GraphInterop wrapper's own tests.
        failures += CharacterMoveNodeTests.RunAll();

        // GRAPH-AS-CLASS: the CLASS record's format half (parse-level only -- registration and
        // per-instance binding live in Aver.Scripting.Bridge, unreachable from this bare process; see
        // GraphClassRecordTests.cs's own header comment).
        failures += GraphClassRecordTests.RunAll();
        // The COMPONENT TREE half of graph-as-class: what a spawned instance is MADE of, where
        // the CLASS record above says what it IS. See ComponentTreeTests.cs's own header for why
        // these are parse-level and deliberately do not re-test the structural rules the C++
        // reader owns.
        failures += ComponentTreeTests.RunAll();
        // InputKeyPressed / InputKeyReleased -- the EDGE of a key, where InputKey gives the
        // STATE. See EdgeInputNodeTests.cs for why the symbol name is the assertion.
        failures += EdgeInputNodeTests.RunAll();

        // GAP 3: FireEvent -- cross-entity events. The router (Aver.Graph.GraphEvents) lives in THIS
        // assembly, so unlike every prior GraphInterop wrapper's own tests, the reentrancy-guard tests
        // here are genuine end-to-end, value-asserting proofs with real compiled IL and real GraphHost
        // instances -- see FireEventNodeTests.cs's own header comment.
        failures += FireEventNodeTests.RunAll();

        // GetForward: CharacterMove's read-side counterpart, and the node a graph-only first-person
        // template could not exist without. Like CharacterMoveNodeTests these are genuine end-to-end
        // behavioural proofs rather than compile-only ones -- the unbound-entity path is a graceful,
        // documented return rather than a crash, so this process can invoke the emitted IL and assert
        // the real contract. See GetForwardNodeTests.cs's own header comment.
        failures += GetForwardNodeTests.RunAll();

        // A node whose required INPUT pin does not exist -- the state any explicit PIN record leaves a
        // node in, since one hand-written PIN suppresses every default. Both compilers mishandled it
        // silently rather than refusing; see MissingPinTests.cs's own header for the two different
        // ways they got it wrong, and for why "absent" has to stay distinct from "unconnected".
        failures += MissingPinTests.RunAll();

        if (failures == 0)
            Console.WriteLine("\nAll tests passed.");
        else
            Console.WriteLine($"\n{failures} test(s) failed.");

        return failures;
    }

    static int TestConstFloat()
    {
        Console.WriteLine("Test: ConstFloat");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=42.5
OUT 1 value
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 42.5f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestConstInt()
    {
        Console.WriteLine("Test: ConstInt");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstInt value=123
OUT 1 value
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            int result = fn();
            int expected = 123;
            if (result != expected)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestConstBool()
    {
        Console.WriteLine("Test: ConstBool");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstBool value=true
OUT 1 value
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            bool result = fn();
            bool expected = true;
            if (result != expected)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestAdd()
    {
        Console.WriteLine("Test: Add");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=10.0
NODE 2 ConstFloat value=32.0
NODE 3 Add
LINK 1 value 3 a
LINK 2 value 3 b
OUT 3 result
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 42.0f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestMultiply()
    {
        Console.WriteLine("Test: Multiply");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=6.0
NODE 2 ConstFloat value=7.0
NODE 3 Multiply
LINK 1 value 3 a
LINK 2 value 3 b
OUT 3 result
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 42.0f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestCompare()
    {
        Console.WriteLine("Test: Compare");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=50.0
NODE 2 ConstFloat value=40.0
NODE 3 Compare
LINK 1 value 3 a
LINK 2 value 3 b
OUT 3 result
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            bool result = fn();
            bool expected = true;  // 50 > 40
            if (result != expected)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestAddThenMultiply()
    {
        Console.WriteLine("Test: AddThenMultiply (complex graph)");
        try
        {
            // (10 + 2) * 3 = 36
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=10.0
NODE 2 ConstFloat value=2.0
NODE 3 ConstFloat value=3.0
NODE 4 Add
NODE 5 Multiply
LINK 1 value 4 a
LINK 2 value 4 b
LINK 4 result 5 a
LINK 3 value 5 b
OUT 5 result
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 36.0f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Test parsing graphs in C++ format (string node IDs, dot notation for links)
    static int TestCppFormat()
    {
        Console.WriteLine("Test: CppFormat (dot notation links)");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE const1 ConstFloat value=5.5
NODE const2 ConstFloat value=4.5
NODE adder Add
PIN const1 value out float
PIN const2 value out float
PIN adder a in float
PIN adder b in float
PIN adder result out float
LINK const1.value adder.a
LINK const2.value adder.b
OUT adder result
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 10.0f;  // 5.5 + 4.5
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Test that NAME and DESCRIPTION records are parsed and preserved
    static int TestNameAndDescription()
    {
        Console.WriteLine("Test: NameAndDescription");
        try
        {
            var graphText = @"
OCGRAPH 1
NAME TestGraph
DESCRIPTION This is a test graph for the cross-implementation test
NODE n1 ConstFloat value=15.0
OUT n1 value
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            // Verify NAME was stored
            if (graph.Name != "TestGraph")
            {
                Console.WriteLine($"  FAIL: Expected name 'TestGraph', got '{graph.Name}'");
                return 1;
            }

            // Verify DESCRIPTION was stored
            if (graph.Description != "This is a test graph for the cross-implementation test")
            {
                Console.WriteLine($"  FAIL: Expected description, got '{graph.Description}'");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 15.0f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: Name={graph.Name}, Description preserved");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Cross-implementation test: load a graph written by the C++ OcGraph writer,
    // parse it with the C# parser, compile it to IL, and verify the result.
    // This fixture (cross_impl_test.ocgraph) is in the exact format produced by OcGraph.cpp.
    static int TestCrossImplementationFixture()
    {
        Console.WriteLine("Test: CrossImplementationFixture");
        try
        {
            // Read the fixture file that was written by the C++ OcGraph writer.
            string filePath = "cross_impl_test.ocgraph";
            if (!File.Exists(filePath))
            {
                Console.WriteLine($"  FAIL: Fixture file not found: {filePath}");
                return 1;
            }

            string graphText = File.ReadAllText(filePath);

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            // Verify metadata was preserved from the C++ format.
            if (graph.Name != "CrossImplementationTest")
            {
                Console.WriteLine($"  FAIL: Expected name 'CrossImplementationTest', got '{graph.Name}'");
                return 1;
            }

            // Checked for the "C#" specifically, because that is what caught the bug this fixture
            // exists to prevent: the parser used to strip from the first '#' ANYWHERE in a line, so
            // this description arrived as "...executed by the C". A format about C# scripting meets
            // '#' inside values constantly.
            if (!graph.Description.Contains("C# runtime"))
            {
                Console.WriteLine($"  FAIL: description lost its '#' - got '{graph.Description}'");
                return 1;
            }

            // Compile the graph.
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(no error message)"}");
                Console.WriteLine($"  Nodes: {graph.Nodes.Count}, Links: {graph.Links.Count}, Outputs: {graph.Outputs.Count}");
                if (graph.Outputs.Count > 0)
                {
                    Console.WriteLine($"  Output: {graph.Outputs[0].NodeId}.{graph.Outputs[0].PinName}");
                }
                return 1;
            }

            // Execute the compiled graph and verify the result.
            // The fixture computes (5 + 7) * 3 = 36. CHAINED on purpose: a single constant would
            // pass on a runtime that ignored links entirely, which is the failure mode a
            // cross-implementation test most needs to exclude.
            float result = fn();
            float expected = 36.0f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- PARAM: Compile() producing a method that takes arguments ---------------------------------

    // A graph that just hands back the "entity" PARAM unchanged. Proves int-typed parameters flow
    // through, and that a graph with one PARAM compiles to Func<int,int> rather than Func<int>.
    static int TestParamEntityPassthrough()
    {
        Console.WriteLine("Test: Param (entity passthrough)");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
OUT e value
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<int, int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            int result = fn(77);
            if (result != 77)
            {
                Console.WriteLine($"  FAIL: Expected 77, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: fn(77) = {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A "time" PARAM feeding a Sin node. This is the shape a drone flight path actually needs:
    // a float parameter the caller supplies every tick, read by name, driving a trig node.
    static int TestParamTimeIntoSin()
    {
        Console.WriteLine("Test: Param (time) into Sin");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM time float
NODE t Param param=time
NODE s Sin
LINK t value s a
OUT s result
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float atZero = fn(0f);
            float atHalfPi = fn((float)(Math.PI / 2.0));
            if (Math.Abs(atZero - 0f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: sin(0) expected 0, got {atZero}");
                return 1;
            }
            if (Math.Abs(atHalfPi - 1f) > 1e-5)
            {
                Console.WriteLine($"  FAIL: sin(pi/2) expected 1, got {atHalfPi}");
                return 1;
            }

            Console.WriteLine($"  PASS: sin(0)={atZero}, sin(pi/2)={atHalfPi}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Two float PARAMs feeding a Subtract node, deliberately asymmetric (a - b, not a + b) so a bug
    // that swapped argument order or Ldarg indices would fail this test instead of passing by
    // coincidence the way it could with a commutative op.
    static int TestParamOrderMatters()
    {
        Console.WriteLine("Test: Param (two floats, order matters)");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM a float
PARAM b float
NODE pa Param param=a
NODE pb Param param=b
NODE sub Subtract
LINK pa value sub a
LINK pb value sub b
OUT sub result
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float, float, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float r1 = fn(10f, 3f);
            float r2 = fn(3f, 10f);
            if (Math.Abs(r1 - 7f) > 1e-6 || Math.Abs(r2 - (-7f)) > 1e-6)
            {
                Console.WriteLine($"  FAIL: fn(10,3) expected 7 got {r1}; fn(3,10) expected -7 got {r2}");
                return 1;
            }

            Console.WriteLine($"  PASS: fn(10,3)={r1}, fn(3,10)={r2}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestCosNode()
    {
        Console.WriteLine("Test: Cos");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM x float
NODE px Param param=x
NODE c Cos
LINK px value c a
OUT c result
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn(0f);
            if (Math.Abs(result - 1f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: cos(0) expected 1, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: cos(0)={result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestSubtractNode()
    {
        Console.WriteLine("Test: Subtract");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=10.0
NODE 2 ConstFloat value=3.0
NODE 3 Subtract
LINK 1 value 3 a
LINK 2 value 3 b
OUT 3 result
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            if (Math.Abs(result - 7.0f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected 7, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Divide's chosen zero-divisor convention: b == 0 yields 0.0, not IEEE754's Infinity/NaN.
    // See the comment on GraphCompiler.EmitDivide for why.
    static int TestDivideByZeroGuard()
    {
        Console.WriteLine("Test: Divide by zero guard (defined as 0.0, not NaN/Infinity)");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=10.0
NODE 2 ConstFloat value=0.0
NODE 3 Divide
LINK 1 value 3 a
LINK 2 value 3 b
OUT 3 result
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            if (float.IsNaN(result) || float.IsInfinity(result))
            {
                Console.WriteLine($"  FAIL: divide by zero produced {result} -- a NaN/Infinity reaching a transform is exactly what the guard exists to prevent");
                return 1;
            }
            if (result != 0f)
            {
                Console.WriteLine($"  FAIL: Expected the chosen convention (0.0) for divide-by-zero, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: 10/0 = {result} (guarded, not NaN/Infinity)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestDivideNormal()
    {
        Console.WriteLine("Test: Divide (non-zero divisor still divides normally)");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=21.0
NODE 2 ConstFloat value=4.0
NODE 3 Divide
LINK 1 value 3 a
LINK 2 value 3 b
OUT 3 result
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            if (compiler.Compile(out var compileErr) is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr}");
                return 1;
            }

            float result = fn();
            float expected = 21.0f / 4.0f;
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Expected {expected}, got {result}");
                return 1;
            }

            Console.WriteLine($"  PASS: {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A Param node with no param= attribute at all must fail to PARSE (Graph.Validate runs at the
    // end of OcGraphParser.Parse), with a message that says why -- not silently produce a node with
    // no output pin that fails somewhere unrelated later.
    static int TestParamMissingAttributeFailsToParse()
    {
        Console.WriteLine("Test: Param node with no param= attribute fails to parse");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param
OUT e value
";
            if (OcGraphParser.Parse(graphText, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (Param node has no param= attribute), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("param="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing param= attribute, got: {err}");
                return 1;
            }

            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A Param node naming a parameter that was never declared with PARAM must fail to parse, not
    // silently compile down to a wrong constant.
    static int TestParamUndeclaredFailsToParse()
    {
        Console.WriteLine("Test: Param node referencing an undeclared parameter fails to parse");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE e Param param=entity
OUT e value
";
            if (OcGraphParser.Parse(graphText, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail ('entity' was never declared with PARAM), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("undeclared parameter"))
            {
                Console.WriteLine($"  FAIL: expected an error about the undeclared parameter, got: {err}");
                return 1;
            }

            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Backward compatibility: a graph with no PARAM records at all -- i.e. every .ocgraph file that
    // existed before this change, including the cross-implementation fixture -- must still compile
    // to a zero-argument delegate. TestCrossImplementationFixture above already proves this
    // implicitly (it pattern-matches to Func<float>, which would fail if Compile() had grown a
    // parameter from nowhere); this test says so explicitly.
    static int TestGraphWithoutParamsStillHasNoArguments()
    {
        Console.WriteLine("Test: graph with no PARAM records compiles to zero arguments");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE 1 ConstFloat value=5.0
OUT 1 value
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            if (graph.Parameters.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero declared parameters, got {graph.Parameters.Count}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: expected a zero-argument Func<float>, got {compiled?.GetType().Name ?? "null"} ({compileErr})");
                return 1;
            }

            Console.WriteLine($"  PASS: {fn()}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- getfield/setfield: real IL, checked at compile time ---------------------------------------
    //
    // WHAT THESE TESTS DO AND DO NOT PROVE. Nothing in this test process boots a native scene (see
    // the scout notes this task was handed: GameApp.cpp never constructs a ScriptHost, and only the
    // editor -- sandbox/src, off limits here -- does). That means these tests cannot show a graph
    // setting a field and then read the field back changed; that round trip was not performed, and
    // this comment says so plainly rather than letting a passing test imply it.
    //
    // What CAN be shown without a native scene, and what these tests actually check:
    //   1. Compile-time field validation (unknown field, wrong field kind) works, using a fake
    //      FieldResolver that stands in for the native aver_scene_field/aver_scene_field_kind calls
    //      GraphCompiler's default resolver would otherwise make.
    //   2. The emitted IL performs a REAL P/Invoke call rather than the old hardcoded stub. The old
    //      EmitGetField/EmitSetField never touched anything outside the DynamicMethod -- invoking a
    //      compiled getfield graph just returned a constant 0.0f, no matter what, and could never
    //      throw. Invoking the new IL DOES throw here, and the specific exception is itself evidence
    //      of what got called: this test bin directory happens to hold the MANAGED Aver.Scene.dll
    //      (Aver.Graph.Tests references it directly) sitting exactly where NativeResolver.cs looks
    //      for the NATIVE one. NativeLibrary.TryLoad succeeds against it -- a managed assembly is
    //      still a loadable PE/COFF image -- so the P/Invoke gets past "find the DLL" and fails one
    //      step later, on "find the export", with System.EntryPointNotFoundException naming the exact
    //      P/Invoke symbol (aver_scene_get_f32 / aver_scene_set_f32). That is only reachable if the
    //      emitted IL actually issued that named call; this was verified by running the suite, not
    //      assumed -- the first version of this test asserted DllNotFoundException and failed with
    //      this EntryPointNotFoundException instead, which is what's asserted now.

    static int TestGetFieldMissingFieldAttributeFailsCompile()
    {
        Console.WriteLine("Test: GetField node with no field= attribute fails to compile");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gf GetField
LINK e value gf entity
OUT gf value
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (GetField has no field= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("field="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing field= attribute, got: {compileErr}");
                return 1;
            }

            Console.WriteLine($"  PASS: {compileErr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestGetFieldUnknownFieldFailsCompile()
    {
        Console.WriteLine("Test: GetField naming an unknown scene field fails at COMPILE time");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gf GetField field=Nonsense.DoesNotExist
LINK e value gf entity
OUT gf value
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            // A fake resolver standing in for the native scene: every field name is unknown. This is
            // what the OLD stub could never do -- it always "succeeded" and returned 0.0, regardless
            // of whether the named field existed. That silent-success failure mode is exactly what
            // this test guards against.
            FieldResolver alwaysUnknown = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 0;
                kind = 0;
                return false;
            };

            var compiler = new GraphCompiler(graph, alwaysUnknown);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (unknown field), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("unknown scene field"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the unknown field, got: {compileErr}");
                return 1;
            }

            Console.WriteLine($"  PASS: {compileErr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestGetFieldWrongKindFailsCompile()
    {
        Console.WriteLine("Test: GetField naming a non-F32 field fails at COMPILE time");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gf GetField field=CLocal.position
LINK e value gf entity
OUT gf value
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            // The field "exists" but is Vec3 (kind=1, aver::scene::FieldKind -- Fields.hpp), matching
            // real life: CLocal.position genuinely is a Vec3, not an F32, so getfield (which only
            // supports F32 today) must reject it rather than read three floats' worth of memory as
            // one float.
            FieldResolver vec3Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 1;
                kind = 1; // FieldKind.Vec3
                return true;
            };

            var compiler = new GraphCompiler(graph, vec3Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (field is Vec3, not F32), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("not an F32 field"))
            {
                Console.WriteLine($"  FAIL: expected an error about the field not being F32, got: {compileErr}");
                return 1;
            }

            Console.WriteLine($"  PASS: {compileErr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestGetFieldEmitsRealNativeCall()
    {
        Console.WriteLine("Test: GetField emits a real native call, not the old stub");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gf GetField field=Test.FakeF32Field
LINK e value gf entity
OUT gf value
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            // The field "exists" and is F32-kind, so compilation proceeds to actually emit the call.
            FieldResolver knownF32Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 42;
                kind = 0; // FieldKind.F32
                return true;
            };

            var compiler = new GraphCompiler(graph, knownF32Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<int, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                float unused = fn(1);
                Console.WriteLine($"  FAIL: expected invoking this to throw -- see the class comment above " +
                                   $"these tests for why -- but it returned {unused} without calling into anything");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_get_f32"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_get_f32: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: invoking made a real call looking for 'aver_scene_get_f32' (see the class comment above these tests for why this is EntryPointNotFoundException, not DllNotFoundException, in this test process): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    static int TestSetFieldEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetField emits a real native call, not the old stub");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
PARAM v float
NODE e Param param=entity
NODE pv Param param=v
NODE sf SetField field=Test.FakeF32Field
LINK e value sf entity
LINK pv value sf value
OUT sf success
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver knownF32Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 42;
                kind = 0; // FieldKind.F32
                return true;
            };

            var compiler = new GraphCompiler(graph, knownF32Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<int, float, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                bool unused = fn(1, 3.5f);
                Console.WriteLine($"  FAIL: expected invoking this to throw -- see the class comment above " +
                                   $"these tests for why -- but it returned {unused} without calling into anything");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_set_f32"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_set_f32: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: invoking made a real call looking for 'aver_scene_set_f32' (the old stub always reported success=true and never called anything): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
