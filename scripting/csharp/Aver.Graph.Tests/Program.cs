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
}
