// A value typed into an unwired input pin in the editor is written as that pin's PIN record default
// (`PIN node pin in float 2.5`). These tests prove the compiler reads it, that an explicit PINVAL still
// wins whichever record comes first, and that a wire wins over both.
using System;
using Aver.Graph;

static class PinDefaultValueTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += Expect("an input pin's PIN default is the value the node reads", @"
OCGRAPH 1
NODE add Add
PIN add a in float 2.5
PIN add b in float 4
PIN add result out float
OUT add result
", 6.5f);
        failures += Expect("a PINVAL after the PIN default replaces it", @"
OCGRAPH 1
NODE add Add
PIN add a in float 2.5
PIN add b in float 4
PIN add result out float
PINVAL add a 10
OUT add result
", 14.0f);
        failures += Expect("a PINVAL before the PIN default still wins", @"
OCGRAPH 1
NODE add Add
PINVAL add a 10
PIN add a in float 2.5
PIN add b in float 4
PIN add result out float
OUT add result
", 14.0f);
        failures += Expect("a wire wins over the pin's default", @"
OCGRAPH 1
NODE k ConstFloat value=3
NODE add Add
PIN add a in float 100
PIN add b in float 4
PIN add result out float
LINK k.value add.a
OUT add result
", 7.0f);
        return failures;
    }

    private static int Expect(string what, string text, float want)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        var got = fn.DynamicInvoke();
        if (!(got is float f) || Math.Abs(f - want) > 1e-5f) { Console.WriteLine($"  FAIL: expected {want}, got {got}"); return 1; }
        Console.WriteLine($"  PASS: {want}");
        return 0;
    }
}
