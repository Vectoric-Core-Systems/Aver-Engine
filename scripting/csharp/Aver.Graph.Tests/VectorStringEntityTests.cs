// The float3, string and entity pin types and the nodes built on them: vector maths, string building, literals
// in PIN defaults / PINVAL / VAR, and the Int <-> Entity link rule. Pure nodes are compiled and invoked; the
// nodes that call natives (entity location, raycast, print) are only compiled -- no native library is loaded here.
using System;
using System.Numerics;
using Aver.Graph;

static class VectorStringEntityTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += Value("MakeVector then BreakVector round trips a component", @"
OCGRAPH 1
NODE x ConstFloat value=1
NODE y ConstFloat value=2
NODE z ConstFloat value=3
NODE mk MakeVector
NODE br BreakVector
LINK x value mk x
LINK y value mk y
LINK z value mk z
LINK mk v br v
OUT br y
", 2.0f);
        failures += Value("MakeVector builds the vector", @"
OCGRAPH 1
NODE mk MakeVector
PINVAL mk x 1
PINVAL mk y 2
PINVAL mk z 3
OUT mk v
", new Vector3(1, 2, 3));
        failures += Value("VectorAdd", TwoVectors("VectorAdd", "1,2,3", "4,5,6"), new Vector3(5, 7, 9));
        failures += Value("VectorSubtract", TwoVectors("VectorSubtract", "4,5,6", "1,2,3"), new Vector3(3, 3, 3));
        failures += Value("VectorDot", TwoVectors("VectorDot", "1,2,3", "4,5,6"), 32.0f);
        failures += Value("VectorCross", TwoVectors("VectorCross", "1,0,0", "0,1,0"), new Vector3(0, 0, 1));
        failures += Value("VectorDistance", TwoVectors("VectorDistance", "1,1,1", "4,5,1"), 5.0f);
        failures += Value("VectorLength", OneVector("VectorLength", "3,4,0"), 5.0f);
        failures += Value("VectorNormalize", OneVector("VectorNormalize", "0,3,4"), new Vector3(0, 0.6f, 0.8f));
        failures += Value("VectorNormalize of zero is zero, not NaN", OneVector("VectorNormalize", "0,0,0"), Vector3.Zero);
        failures += Value("VectorScale", @"
OCGRAPH 1
NODE a ConstVector value=1,2,3
NODE s ConstFloat value=2
NODE sc VectorScale
LINK a value sc v
LINK s value sc s
OUT sc result
", new Vector3(2, 4, 6));
        failures += Value("VectorLerp", @"
OCGRAPH 1
NODE a ConstVector value=0,0,0
NODE b ConstVector value=10,20,30
NODE t ConstFloat value=0.5
NODE l VectorLerp
LINK a value l a
LINK b value l b
LINK t value l t
OUT l result
", new Vector3(5, 10, 15));

        failures += Value("StringAppend + IntToString with a space escaped as %20", @"
OCGRAPH 1
NODE s ConstString value=Hello%20World
NODE n ConstInt value=42
NODE i IntToString
NODE ap StringAppend
LINK n value i a
LINK s value ap a
LINK i result ap b
OUT ap result
", "Hello World42");
        failures += Value("StringLength counts the decoded text", @"
OCGRAPH 1
NODE s ConstString value=a%20b%25
NODE l StringLength
LINK s value l s
OUT l result
", 4);
        failures += Value("StringEquals true", @"
OCGRAPH 1
NODE a ConstString value=x%20y
NODE b ConstString value=x%20y
NODE e StringEquals
LINK a value e a
LINK b value e b
OUT e result
", true);
        failures += Value("StringEquals false", @"
OCGRAPH 1
NODE a ConstString value=x
NODE b ConstString value=y
NODE e StringEquals
LINK a value e a
LINK b value e b
OUT e result
", false);
        failures += Value("FloatToString", @"
OCGRAPH 1
NODE f ConstFloat value=2.5
NODE t FloatToString
LINK f value t a
OUT t result
", "2.5");
        failures += Value("BoolToString", @"
OCGRAPH 1
NODE f ConstBool value=true
NODE t BoolToString
LINK f value t a
OUT t result
", "true");
        failures += Value("VectorToString", @"
OCGRAPH 1
NODE v ConstVector value=1,2,3
NODE t VectorToString
LINK v value t v
OUT t result
", "X=1 Y=2 Z=3");

        failures += Value("a float3 PIN default is the value the node reads", @"
OCGRAPH 1
NODE vl VectorLength
PIN vl v in float3 3,4,0
PIN vl result out float
OUT vl result
", 5.0f);
        failures += Value("a string PIN default is decoded", @"
OCGRAPH 1
NODE sl StringLength
PIN sl s in string ab%20cd
PIN sl result out int
OUT sl result
", 5);
        failures += Value("a float3 PINVAL on a default-pinned node", @"
OCGRAPH 1
NODE vl VectorLength
PINVAL vl v 0,0,2
OUT vl result
", 2.0f);
        failures += Value("a string PINVAL on a default-pinned node", @"
OCGRAPH 1
NODE sl StringLength
PINVAL sl s x%20y
OUT sl result
", 3);
        failures += Value("a PINVAL replaces a float3 PIN default", @"
OCGRAPH 1
NODE vl VectorLength
PIN vl v in float3 3,4,0
PIN vl result out float
PINVAL vl v 0,0,7
OUT vl result
", 7.0f);
        failures += Value("a float3 literal with missing parts reads them as 0", @"
OCGRAPH 1
NODE vl VectorLength
PINVAL vl v 3,4
OUT vl result
", 5.0f);
        failures += Value("an unwired float3 input reads zero, an unwired string empty", @"
OCGRAPH 1
NODE vl VectorLength
OUT vl result
", 0.0f);

        failures += VarValue("a float3 VAR default", "VAR dir float3 1,2,2\nNODE g GetVar var=dir\nOUT g value\n", new Vector3(1, 2, 2));
        failures += VarValue("a string VAR default", "VAR msg string hi%20there\nNODE g GetVar var=msg\nOUT g value\n", "hi there");
        failures += VarValue("an entity VAR default", "VAR who entity 9\nNODE g GetVar var=who\nOUT g value\n", 9);
        failures += VarValue("a float3 VAR without a default is zero", "VAR dir float3\nNODE g GetVar var=dir\nOUT g value\n", Vector3.Zero);

        failures += Compiles("an Int output links into an Entity input", @"
OCGRAPH 1
NODE k ConstInt value=7
NODE v IsValidEntity
LINK k value v e
OUT v result
");
        failures += Value("an Entity output links into an Int input", @"
OCGRAPH 1
NODE c ConstInt
PIN c value out entity 5
NODE i IntToString
LINK c value i a
OUT i result
", "5");
        failures += Refused("a float3 output cannot feed a float input", @"
OCGRAPH 1
NODE v ConstVector value=1,2,3
NODE a Add
LINK v value a a
OUT a result
");
        failures += Refused("a float output cannot feed a float3 input", @"
OCGRAPH 1
NODE f ConstFloat value=1
NODE l VectorLength
LINK f value l v
OUT l result
");
        failures += Refused("a float3 PINVAL that is not numbers", @"
OCGRAPH 1
NODE vl VectorLength
PINVAL vl v a,b,c
OUT vl result
");

        failures += Compiles("IsValidEntity compiles", "OCGRAPH 1\nNODE v IsValidEntity\nPINVAL v e 3\nOUT v result\n");
        failures += Compiles("GetEntityLocation compiles", "OCGRAPH 1\nNODE v GetEntityLocation\nPINVAL v e 3\nOUT v location\n");
        failures += CompilesEntry("SetEntityLocation, RaycastVector and PrintText compile on an exec chain", @"
OCGRAPH 1
NODE s OnStart
NODE v ConstVector value=1,2,3
NODE set SetEntityLocation
NODE rc RaycastVector
NODE p PrintText
NODE bx BreakVector
PINVAL set e 3
PINVAL p text hit%20something
LINK s exec set exec
LINK v value set location
LINK set then rc exec
LINK v value rc origin
LINK rc then p exec
LINK rc point bx v
OUT bx y
ENTRY s OnStart
");
        failures += Refused("a side-effecting data node cannot be compiled by Compile()", "OCGRAPH 1\nNODE set SetEntityLocation\nNODE v ConstFloat value=1\nOUT v value\n", compileMustFail: true);
        return failures;
    }

    private static string TwoVectors(string type, string a, string b) =>
        $"OCGRAPH 1\nNODE a ConstVector value={a}\nNODE b ConstVector value={b}\nNODE n {type}\nLINK a value n a\nLINK b value n b\nOUT n result\n";

    private static string OneVector(string type, string v) =>
        $"OCGRAPH 1\nNODE a ConstVector value={v}\nNODE n {type}\nLINK a value n v\nOUT n result\n";

    private static bool Same(object? got, object want) => want switch
    {
        float f => got is float g && Math.Abs(f - g) < 1e-4f,
        Vector3 v => got is Vector3 w && (v - w).Length() < 1e-4f,
        _ => Equals(got, want),
    };

    private static int Value(string what, string text, object want)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        object? got;
        try { got = fn.DynamicInvoke(); }
        catch (Exception e) { Console.WriteLine($"  FAIL: invoke: {e.InnerException?.Message ?? e.Message}"); return 1; }
        if (!Same(got, want)) { Console.WriteLine($"  FAIL: expected {want}, got {got}"); return 1; }
        Console.WriteLine($"  PASS: {got}");
        return 0;
    }

    private static int VarValue(string what, string body, object want)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse("OCGRAPH 1\n" + body, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        object? got;
        try { got = fn.DynamicInvoke(GraphVarStore.CreateFor(g)); }
        catch (Exception e) { Console.WriteLine($"  FAIL: invoke: {e.InnerException?.Message ?? e.Message}"); return 1; }
        if (!Same(got, want)) { Console.WriteLine($"  FAIL: expected {want}, got {got}"); return 1; }
        Console.WriteLine($"  PASS: {got}");
        return 0;
    }

    private static int Compiles(string what, string text)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        Console.WriteLine("  PASS: compiled");
        return 0;
    }

    private static int CompilesEntry(string what, string text)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).CompileEntryPoint("OnStart", out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        // Invoking JIT-compiles the whole body, so malformed IL surfaces as InvalidProgramException; the first
        // native call then fails because no engine library is loaded, which is the expected outcome here.
        try { fn.DynamicInvoke(); }
        catch (Exception e) when (e.InnerException is EntryPointNotFoundException or DllNotFoundException) { }
        catch (Exception e) { Console.WriteLine($"  FAIL: invoke: {e.InnerException?.GetType().Name}: {e.InnerException?.Message ?? e.Message}"); return 1; }
        Console.WriteLine("  PASS: compiled and the IL runs up to the first native call");
        return 0;
    }

    private static int Refused(string what, string text, bool compileMustFail = false)
    {
        Console.WriteLine($"Test: {what}");
        bool parsed = OcGraphParser.Parse(text, out var g, out var err);
        if (!compileMustFail)
        {
            if (parsed) { Console.WriteLine("  FAIL: parsed without complaint"); return 1; }
            Console.WriteLine($"  PASS: refused: {err}");
            return 0;
        }
        if (!parsed) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn != null) { Console.WriteLine("  FAIL: compiled"); return 1; }
        Console.WriteLine($"  PASS: refused: {cerr}");
        return 0;
    }
}
