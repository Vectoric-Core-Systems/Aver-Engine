// Arrays and user structs: the array nodes on literal and variable lists, ForEachInArray, struct make/break/set, struct and
// array variables and function pins, and the refusals Validate and the link rule owe. Graphs are parsed, compiled
// and invoked; an ENTRY graph with a VAR is invoked with a store, twice, to prove the state persists.
using System;
using System.Collections.Generic;
using System.Numerics;
using Aver.Graph;

static class CollectionTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += Value("MakeArray of floats: Length and Get", @"
OCGRAPH 1
NODE mk MakeArray
PINVAL mk item0 1
PINVAL mk item1 2.5
PINVAL mk item2 3
NODE ln ArrayLength
NODE g ArrayGet
PINVAL g index 1
LINK mk array ln array
LINK mk array g array
OUT ln length
OUT g item
", new object[] { 3, 2.5f });
        failures += Value("MakeArray skips unwired items", @"
OCGRAPH 1
NODE mk MakeArray
PINVAL mk item1 7
NODE ln ArrayLength
LINK mk array ln array
OUT ln length
", 1);
        failures += Value("MakeArray of float3: Get", @"
OCGRAPH 1
NODE mk MakeArray of=float3
PINVAL mk item0 1,2,3
PINVAL mk item1 4,5,6
NODE g ArrayGet of=float3
PINVAL g index 1
LINK mk array g array
OUT g item
", new Vector3(4, 5, 6));
        failures += Value("MakeArray of strings: Get", @"
OCGRAPH 1
NODE mk MakeArray of=string
PINVAL mk item0 a%20b
PINVAL mk item1 c
NODE g ArrayGet of=string
PINVAL g index 0
LINK mk array g array
OUT g item
", "a b");
        failures += Value("an unwired array input reads as an empty list", "OCGRAPH 1\nNODE ln ArrayLength\nOUT ln length\n", 0);
        failures += Value("ArrayGet past the end returns the element default", @"
OCGRAPH 1
NODE mk MakeArray
PINVAL mk item0 4
NODE g ArrayGet
PINVAL g index 9
LINK mk array g array
OUT g item
", 0.0f);
        failures += Value("ArrayGet at a negative index returns the default, not an exception", @"
OCGRAPH 1
NODE mk MakeArray of=int
PINVAL mk item0 4
NODE g ArrayGet of=int
PINVAL g index -1
LINK mk array g array
OUT g item
", 0);
        failures += Value("ArrayGet past the end of a string array is empty", @"
OCGRAPH 1
NODE mk MakeArray of=string
NODE g ArrayGet of=string
PINVAL g index 3
LINK mk array g array
OUT g item
", "");
        failures += Value("ArrayContains and ArrayFind", @"
OCGRAPH 1
NODE mk MakeArray of=int
PINVAL mk item0 5
PINVAL mk item1 6
PINVAL mk item2 7
NODE c1 ArrayContains of=int
PINVAL c1 item 6
NODE c2 ArrayContains of=int
PINVAL c2 item 9
NODE f1 ArrayFind of=int
PINVAL f1 item 7
NODE f2 ArrayFind of=int
PINVAL f2 item 9
LINK mk array c1 array
LINK mk array c2 array
LINK mk array f1 array
LINK mk array f2 array
OUT c1 result
OUT c2 result
OUT f1 index
OUT f2 index
", new object[] { true, false, 2, -1 });

        // A VAR list persists across invocations of an entry point on one store, and a fresh store starts from the default.
        failures += EntryValue("ArrayAdd on a VAR float[] persists across calls", @"
OCGRAPH 1
VAR xs float[]
NODE s OnTick
NODE add ArrayAdd var=xs
PINVAL add item 4
NODE gv GetVar var=xs
NODE ln ArrayLength
LINK s exec add exec
LINK gv value ln array
OUT ln length
ENTRY s OnTick
", 1, 2, 3);
        failures += EntryValue("a VAR array default is the starting list", @"
OCGRAPH 1
VAR xs int[] 5,6,7
NODE s OnTick
NODE add ArrayAdd of=int var=xs
PINVAL add item 1
NODE gv GetVar var=xs
NODE ln ArrayLength of=int
LINK s exec add exec
LINK gv value ln array
OUT ln length
ENTRY s OnTick
", 4, 5, 6);
        failures += EntryValue("ArraySet, ArrayRemoveAt and ArrayClear on a VAR", @"
OCGRAPH 1
VAR xs float[] 1,2,3
NODE s OnTick
NODE st ArraySet var=xs
PINVAL st index 0
PINVAL st item 9
NODE rm ArrayRemoveAt var=xs
PINVAL rm index 1
NODE bad ArraySet var=xs
PINVAL bad index 50
PINVAL bad item 1
NODE gv GetVar var=xs
NODE g0 ArrayGet
PINVAL g0 index 0
NODE ln ArrayLength
LINK s exec st exec
LINK st then rm exec
LINK rm then bad exec
LINK gv value g0 array
LINK gv value ln array
OUT g0 item
ENTRY s OnTick
", 9.0f, 9.0f, 9.0f);
        failures += EntryValue("ArrayClear empties the list", @"
OCGRAPH 1
VAR xs float[] 1,2,3
NODE s OnTick
NODE cl ArrayClear var=xs
NODE gv GetVar var=xs
NODE ln ArrayLength
LINK s exec cl exec
LINK gv value ln array
OUT ln length
ENTRY s OnTick
", 0, 0, 0);

        failures += EntryValue("ForEachInArray sums the elements into a VAR and runs completed", @"
OCGRAPH 1
VAR xs float[] 1,2,3,4
VAR sum float
VAR done bool
NODE s OnTick
NODE fe ForEachInArray
NODE gx GetVar var=xs
NODE gs GetVar var=sum
NODE ad Add
NODE ss SetVar var=sum
NODE fl SetVar var=done
PINVAL fl value true
LINK s exec fe exec
LINK gx value fe array
LINK fe loop ss exec
LINK gs value ad a
LINK fe element ad b
LINK ad result ss value
LINK fe completed fl exec
NODE gd GetVar var=done
OUT gs value
OUT gd value
ENTRY s OnTick
", new object[] { 10.0f, true }, null, null);
        failures += EntryValue("ForEachInArray exposes the index", @"
OCGRAPH 1
VAR xs string[] a,b,c
VAR last int
NODE s OnTick
NODE fe ForEachInArray of=string
NODE gx GetVar var=xs
NODE ss SetVar var=last
LINK s exec fe exec
LINK gx value fe array
LINK fe loop ss exec
LINK fe index ss value
NODE gl GetVar var=last
OUT gl value
ENTRY s OnTick
", 2, 2, 2);

        failures += Value("a function takes an array and returns its length", @"
OCGRAPH 1
FUNC Len pure
FUNCIN Len a float[]
FUNCOUT Len n int
NODE fe FuncEntry func=Len
NODE fr FuncReturn func=Len
NODE al ArrayLength func=Len
LINK fe a al array
LINK al length fr n
NODE mk MakeArray
PINVAL mk item0 1
PINVAL mk item1 2
NODE c CallFunc call=Len
LINK mk array c a
OUT c n
", 2);
        failures += Value("an unset function output array reads as an empty list", @"
OCGRAPH 1
FUNC Mk pure
FUNCOUT Mk r float[]
NODE fe FuncEntry func=Mk
NODE fr FuncReturn func=Mk
NODE c CallFunc call=Mk
NODE ln ArrayLength
LINK c r ln array
OUT ln length
", 0);

        const string pt = "STRUCT Pt\nFIELD Pt x float\nFIELD Pt pos float3\nFIELD Pt name string\nFIELD Pt n int\nFIELD Pt on bool\n";
        failures += Value("MakeStruct then BreakStruct round trips float, float3 and string fields", "OCGRAPH 1\n" + pt + @"
NODE mk MakeStruct struct=Pt
PINVAL mk x 2.5
PINVAL mk pos 1,2,3
PINVAL mk name hi%20there
PINVAL mk n 7
PINVAL mk on true
NODE br BreakStruct struct=Pt
LINK mk value br value
OUT br x
OUT br pos
OUT br name
OUT br n
OUT br on
", new object[] { 2.5f, new Vector3(1, 2, 3), "hi there", 7, true });
        failures += Value("SetStructField replaces one field and keeps the rest", "OCGRAPH 1\n" + pt + @"
NODE mk MakeStruct struct=Pt
PINVAL mk x 2.5
PINVAL mk name keep
NODE st SetStructField struct=Pt field=x
PINVAL st x 9
NODE br BreakStruct struct=Pt
LINK mk value st value
LINK st result br value
OUT br x
OUT br name
", new object[] { 9.0f, "keep" });
        failures += Value("SetStructField leaves its input struct unchanged", "OCGRAPH 1\n" + pt + @"
NODE mk MakeStruct struct=Pt
PINVAL mk x 1
NODE st SetStructField struct=Pt field=x
PINVAL st x 9
NODE br BreakStruct struct=Pt
LINK mk value st value
LINK mk value br value
OUT br x
", 1.0f);
        failures += Value("an unwired struct input reads as the zero struct", "OCGRAPH 1\n" + pt + @"
NODE br BreakStruct struct=Pt
OUT br x
OUT br name
OUT br pos
", new object[] { 0.0f, "", Vector3.Zero });
        failures += EntryValue("a struct VAR holds a value across calls", "OCGRAPH 1\n" + pt + @"
VAR p struct:Pt
NODE s OnTick
NODE gp GetVar var=p
NODE br BreakStruct struct=Pt
NODE add Add
PINVAL add b 1
NODE st SetStructField struct=Pt field=x
NODE sv SetVar var=p
LINK gp value br value
LINK br x add a
LINK gp value st value
LINK add result st x
LINK st result sv value
LINK s exec sv exec
OUT br x
ENTRY s OnTick
", 1.0f, 2.0f, 3.0f);
        failures += Value("a function takes and returns a struct", "OCGRAPH 1\n" + pt + @"
FUNC Bump pure
FUNCIN Bump p struct:Pt
FUNCOUT Bump q struct:Pt
NODE fe FuncEntry func=Bump
NODE fr FuncReturn func=Bump
NODE st SetStructField struct=Pt field=n func=Bump
PINVAL st n 42
LINK fe p st value
LINK st result fr q
NODE mk MakeStruct struct=Pt
NODE c CallFunc call=Bump
NODE br BreakStruct struct=Pt
LINK mk value c p
LINK c q br value
OUT br n
", 42);
        failures += Value("MakeStruct pulled twice does not alias (each read is a fresh struct)", "OCGRAPH 1\n" + pt + @"
NODE mk MakeStruct struct=Pt
PINVAL mk n 3
NODE b1 BreakStruct struct=Pt
NODE b2 BreakStruct struct=Pt
LINK mk value b1 value
LINK mk value b2 value
OUT b1 n
OUT b2 n
", new object[] { 3, 3 });

        failures += Refused("a MakeStruct naming an unknown struct", "OCGRAPH 1\nNODE m MakeStruct struct=Nope\nOUT m value\n", "Nope");
        failures += Refused("a MakeStruct with no struct=", "OCGRAPH 1\nNODE m MakeStruct\nOUT m value\n", "struct=");
        failures += Refused("a VAR of an unknown struct", "OCGRAPH 1\nVAR p struct:Nope\n", "Nope");
        failures += Refused("a pin of an unknown struct", "OCGRAPH 1\nNODE a ConstFloat value=1\nPIN a x out struct:Nope\n", "Nope");
        failures += Refused("a duplicate STRUCT field", "OCGRAPH 1\nSTRUCT S\nFIELD S a float\nFIELD S a int\n", "twice");
        failures += Refused("a duplicate STRUCT", "OCGRAPH 1\nSTRUCT S\nSTRUCT S\n", "Duplicate STRUCT");
        failures += Refused("a FIELD of an undeclared STRUCT", "OCGRAPH 1\nFIELD S a float\n", "no STRUCT");
        failures += Refused("a FIELD that is not a plain type", "OCGRAPH 1\nSTRUCT S\nFIELD S a float[]\n", "FIELD");
        failures += Refused("a SetStructField naming a missing field", "OCGRAPH 1\nSTRUCT S\nFIELD S a float\nNODE m SetStructField struct=S field=zz\nOUT m result\n", "field");
        failures += Refused("an array node with an element type it does not have", "OCGRAPH 1\nNODE m MakeArray of=quaternion\nOUT m array\n", "of=");
        failures += Refused("an ArrayAdd with no var=", "OCGRAPH 1\nVAR xs float[]\nNODE a ArrayAdd\n", "var=");
        failures += Refused("an ArrayAdd on an undeclared variable", "OCGRAPH 1\nNODE a ArrayAdd var=nope\n", "nope");
        failures += Refused("an ArrayAdd whose element type disagrees with the variable", "OCGRAPH 1\nVAR xs int[]\nNODE a ArrayAdd of=float var=xs\n", "xs");
        failures += Refused("an ArrayAdd on a scalar variable", "OCGRAPH 1\nVAR x float\nNODE a ArrayAdd var=x\n", "x");
        failures += Refused("a float[] output cannot feed an int[] input", @"
OCGRAPH 1
NODE mk MakeArray
NODE ln ArrayLength of=int
LINK mk array ln array
OUT ln length
", "mismatch");
        failures += Refused("a float[] output cannot feed a float input", @"
OCGRAPH 1
NODE mk MakeArray
NODE a Add
LINK mk array a a
OUT a result
", "mismatch");
        failures += Refused("a struct output cannot feed a float input", "OCGRAPH 1\nSTRUCT S\nFIELD S a float\nNODE m MakeStruct struct=S\nNODE a Add\nLINK m value a a\nOUT a result\n", "mismatch");
        failures += Refused("a struct output cannot feed another struct's input", "OCGRAPH 1\nSTRUCT S\nFIELD S a float\nSTRUCT T\nFIELD T b float\nNODE m MakeStruct struct=S\nNODE br BreakStruct struct=T\nLINK m value br value\nOUT br b\n", "mismatch");
        failures += Refused("an array PARAM", "OCGRAPH 1\nPARAM xs float[]\n", "PARAM");
        failures += Refused("an array pin value typed in as a PINVAL", "OCGRAPH 1\nNODE ln ArrayLength\nPINVAL ln array 1,2\nOUT ln length\n", "");

        failures += Compiles("an int[] output links into an entity[] input", @"
OCGRAPH 1
NODE mk MakeArray of=int
NODE ln ArrayLength of=entity
LINK mk array ln array
OUT ln length
");
        failures += Compiles("a struct output links into the same struct's input", "OCGRAPH 1\nSTRUCT S\nFIELD S a float\nNODE m MakeStruct struct=S\nNODE br BreakStruct struct=S\nLINK m value br value\nOUT br a\n");

        // What the editor writes: every pin explicit (types as `float3[]`, `struct:Pt`), STRUCT before VAR, attributes on the NODE line.
        failures += EntryValue("a graph written the editor's way, with explicit array and struct pins", @"
OCGRAPH 1
NAME EditorStyle

STRUCT Pt
FIELD Pt x float
FIELD Pt pos float3

VAR dirs float3[] 1,0,0;0,2,0
VAR p struct:Pt

NODE s OnTick 0 0
NODE gd GetVar 0 0 var=dirs
NODE ag ArrayGet 0 0 of=float3
NODE mk MakeStruct 0 0 struct=Pt
NODE sv SetVar 0 0 var=p
NODE gp GetVar 0 0 var=p
NODE br BreakStruct 0 0 struct=Pt
PIN s exec out exec
PIN gd value out float3[]
PIN ag array in float3[]
PIN ag index in int 1
PIN ag item out float3
PIN mk x in float 4
PIN mk pos in float3
PIN mk value out struct:Pt
PIN sv exec in exec
PIN sv value in struct:Pt
PIN sv then out exec
PIN gp value out struct:Pt
PIN br value in struct:Pt
PIN br x out float
PIN br pos out float3
LINK s.exec sv.exec
LINK gd.value ag.array
LINK ag.item mk.pos
LINK mk.value sv.value
LINK gp.value br.value
OUT br x
OUT br pos
ENTRY s OnTick
", new object[] { 4.0f, new Vector3(0, 2, 0) });

        failures += TypeStrings();
        return failures;
    }

    private static int TypeStrings()
    {
        Console.WriteLine("Test: array and struct type strings parse and format");
        int bad = 0;
        foreach (var s in new[] { "float[]", "int[]", "bool[]", "float3[]", "string[]", "entity[]", "struct:Pt", "float", "exec" })
        {
            if (!PinTypeRules.TryParse(s, out var t, out var sn) || PinTypeRules.Format(t, sn) != s) { Console.WriteLine($"  FAIL: {s}"); ++bad; }
        }
        foreach (var s in new[] { "floatarray", "struct", "struct:", "exec[]", "quat[]", "3", "", "float[][]" })
        {
            if (PinTypeRules.TryParse(s, out _, out _)) { Console.WriteLine($"  FAIL: '{s}' parsed"); ++bad; }
        }
        if (bad == 0) Console.WriteLine("  PASS");
        return bad;
    }

    private static bool Same(object? got, object want)
    {
        if (want is object[] ws)
        {
            if (got is not object[] gs || gs.Length != ws.Length) return false;
            for (int i = 0; i < ws.Length; i++) if (!Same(gs[i], ws[i])) return false;
            return true;
        }
        return want switch
        {
            float f => got is float g && Math.Abs(f - g) < 1e-4f,
            Vector3 v => got is Vector3 w && (v - w).Length() < 1e-4f,
            _ => Equals(got, want),
        };
    }

    private static string Show(object? o) => o is object[] a ? "[" + string.Join(", ", Array.ConvertAll(a, Show)) + "]" : o?.ToString() ?? "null";

    private static int Value(string what, string text, object want)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).Compile(out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        object? got;
        try { got = fn.DynamicInvoke(g.Variables.Count > 0 ? new object[] { GraphVarStore.CreateFor(g) } : null); }
        catch (Exception e) { Console.WriteLine($"  FAIL: invoke: {e.InnerException?.Message ?? e.Message}"); return 1; }
        if (!Same(got, want)) { Console.WriteLine($"  FAIL: expected {Show(want)}, got {Show(got)}"); return 1; }
        Console.WriteLine($"  PASS: {Show(got)}");
        return 0;
    }

    /// Runs the OnTick entry point once per expected value (a null entry is not checked), on one store.
    private static int EntryValue(string what, string text, object first, object? second = null, object? third = null)
    {
        Console.WriteLine($"Test: {what}");
        if (!OcGraphParser.Parse(text, out var g, out var err)) { Console.WriteLine($"  FAIL: parse: {err}"); return 1; }
        var fn = new GraphCompiler(g).CompileEntryPoint("OnTick", out var cerr);
        if (fn == null) { Console.WriteLine($"  FAIL: compile: {cerr}"); return 1; }
        var store = GraphVarStore.CreateFor(g);
        int failures = 0;
        int call = 0;
        foreach (var want in new[] { first, second, third })
        {
            ++call;
            if (want == null) break;
            object? got;
            try { got = fn.DynamicInvoke(store); }
            catch (Exception e) { Console.WriteLine($"  FAIL: call {call}: {e.InnerException?.Message ?? e.Message}"); return 1; }
            if (!Same(got, want)) { Console.WriteLine($"  FAIL: call {call}: expected {Show(want)}, got {Show(got)}"); ++failures; }
        }
        if (failures == 0) Console.WriteLine("  PASS");
        return failures;
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

    private static int Refused(string what, string text, string mention)
    {
        Console.WriteLine($"Test: {what}");
        if (OcGraphParser.Parse(text, out _, out var err)) { Console.WriteLine("  FAIL: parsed without complaint"); return 1; }
        if (mention.Length > 0 && (err == null || !err.Contains(mention, StringComparison.OrdinalIgnoreCase)))
        { Console.WriteLine($"  FAIL: refused, but the message does not mention '{mention}': {err}"); return 1; }
        Console.WriteLine($"  PASS: refused: {err}");
        return 0;
    }
}
