// Test suite for Aver.Graph: parsing and compilation.
// Comment explains WHY: this slice proves the property that a graph loaded from text,
// compiled to IL, and invoked returns the same value as hand-written C#.

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
}
