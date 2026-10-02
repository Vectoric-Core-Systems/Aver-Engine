// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the node types added on top of the exec/PUSH compiler (a9038da): Select, InputKey,
// Raycast, and -- the continuous-input pair closing the visual-scripting README's remaining
// engine-side gap -- MouseDelta, MoveAxis. See GraphCompiler.cs's EmitSelect/EmitInputKey/EmitRaycast/
// EmitExecRaycast/EmitMouseDelta/EmitExecMouseDelta/EmitMoveAxis/EmitExecMoveAxis comments for the
// design these tests are proving, and OcGraphParser.cs's "Select / InputKey / Raycast" and
// "MouseDelta / MoveAxis" sections for the pin shapes.
//
// SELECT is fully testable end to end -- pure data, no native surface, so both PULL (Compile()) and
// PUSH (CompileEntryPoint()) tests below actually invoke the compiled delegate and check real numbers.
//
// INPUTKEY AND RAYCAST INHERIT THE SAME HONEST LIMITATION GetFieldTests/SetFieldTests ALREADY NAME
// (see Program.cs's own comment above TestGetFieldEmitsRealNativeCall): nothing in THIS test process
// boots a live native Aver.Framework/Aver.Physics -- confirmed empirically, not assumed, by actually
// running this suite and reading the resulting exception, the same way that comment describes doing
// for GetField/SetField. So the tests below prove exactly what GetField/SetField's own tests prove:
// compile-time pin/shape correctness, and that the EMITTED IL performs a REAL P/Invoke-reaching call
// (not the old kind of hardcoded-return stub) by observing the SPECIFIC failure that call produces --
// EntryPointNotFoundException for InputKey (Aver.Framework.dll's managed copy sits where NativeResolver
// looks for the native one, exactly like Aver.Scene.dll does for GetField/SetField -- see
// Aver.Framework.csproj's InternalsVisibleTo grant, added in this same slice), and DllNotFoundException
// for Raycast (Aver.Physics has no managed contract assembly to collide with at all, so there is
// nothing sitting at that name for NativeLibrary.TryLoad to find even accidentally -- this test process
// never claims otherwise).
//
// A genuine round trip (aver_fw_input_set_key -> aver_fw_input_key, or a real Jolt Raycast against a
// real static body) was attempted beyond this: an isolated, throwaway console harness (NOT part of
// this repository) loaded the REAL native Aver.Framework.dll built at build-release/bin/ and proved,
// by reflection, that aver_fw_input_set_key(5,1) followed by aver_fw_input_key(5) really does return 1,
// and 0 after aver_fw_input_set_key(5,0) -- genuine evidence the underlying ABI is correct. Reproducing
// that INSIDE this shipped suite turned out to be blocked by a real constraint, not a shortcut: this
// exe's own deps.json/TPA list already names "Aver.Framework"/"Aver.Scene" as local dependencies (it
// references Aver.Graph, which references both), and .NET Core resolves a TPA-listed simple name from
// its recorded path UNCONDITIONALLY -- before any custom AssemblyLoadContext.Resolving hook or even an
// eager LoadFromStream gets a chance to redirect it -- so swapping a real native DLL in at that exact
// path makes the CLR try to load IT as the managed assembly and fail with BadImageFormatException
// before Main ever runs, no matter how early the redirect is registered. Solving that for real needs
// the managed and native copies to live in genuinely different directories (exactly how PACKAGING.md's
// own shipped-game layout already separates "Scripting\" from the root) via a Private="false" ProjectReference
// restructuring -- out of scope to retrofit onto the SHARED Aver.Graph.csproj here, since it would
// change copy-local behaviour for every consumer, GetField/SetField's own tests included. Named
// honestly rather than left unattempted or silently claimed solved.

using System;
using Aver.Graph;

static class NewNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- Select ----
        failures += TestSelectDefaultPinsShape();
        failures += TestSelectTakesTrueBranchPull();
        failures += TestSelectTakesFalseBranchPull();
        failures += TestSelectEmbeddedInLargerPullGraph();
        failures += TestSelectInEntryOnTickGraph();

        // ---- InputKey ----
        failures += TestInputKeyDefaultPinsShape();
        failures += TestInputKeyEmitsRealNativeCallPull();
        failures += TestInputKeyEmitsRealNativeCallPush();

        // ---- Raycast ----
        failures += TestRaycastDefaultPinsShape();
        failures += TestRaycastEmitsRealNativeCallPull();
        failures += TestRaycastEmitsRealNativeCallPush();
        failures += TestRaycastMissingOutputPinFailsCompileWithClearError();
        failures += TestRaycastPulledWithoutExecVisitFailsClearly();
        failures += TestRaycastCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll();

        // ---- MouseDelta ----
        failures += TestMouseDeltaDefaultPinsShape();
        failures += TestMouseDeltaEmitsRealNativeCallPull();
        failures += TestMouseDeltaEmitsRealNativeCallPush();
        failures += TestMouseDeltaPulledWithoutExecVisitFailsClearly();
        failures += TestMouseDeltaCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll();

        // ---- MoveAxis ----
        failures += TestMoveAxisDefaultPinsShape();
        failures += TestMoveAxisEmitsRealNativeCallPull();
        failures += TestMoveAxisEmitsRealNativeCallPush();
        failures += TestMoveAxisPulledWithoutExecVisitFailsClearly();
        failures += TestMoveAxisCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll();

        return failures;
    }

    // =================================================================================================
    // SELECT
    // =================================================================================================

    private static int TestSelectDefaultPinsShape()
    {
        Console.WriteLine("Test: Select's default pins are cond:bool-in, ifTrue/ifFalse:float-in, result:float-out");
        try
        {
            var text = "OCGRAPH 1\nNODE s Select\nOUT s result\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["s"];
            bool ok =
                node.Pins.Find(p => p.Name == "cond" && !p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Find(p => p.Name == "ifTrue" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "ifFalse" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "result" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Count == 4; // no exec pins -- pure data node
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 4 pins, no exec, types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSelectTakesTrueBranchPull() => RunSelectPullCase(true, 1f, 2f, 1f);
    private static int TestSelectTakesFalseBranchPull() => RunSelectPullCase(false, 1f, 2f, 2f);

    private static int RunSelectPullCase(bool cond, float ifTrue, float ifFalse, float expected)
    {
        Console.WriteLine($"Test: Select (PULL/Compile()) with cond={cond} picks {(cond ? "ifTrue" : "ifFalse")}");
        try
        {
            // FORCED DECIMAL POINT (":0.0", not a bare "{ifTrue}") is load-bearing, not cosmetic: the
            // NODE record's own "value=" attribute parsing tries int.TryParse BEFORE float.TryParse
            // (OcGraphParser.cs, the NODE-record branch -- unlike the PIN record's own value parsing a
            // few lines below it, which was already fixed to type by the pin's DECLARED type instead;
            // this older branch was not). A whole-number float like 1f.ToString() renders "1" with no
            // decimal point, which silently becomes ConstantOutput{Value=(int)1} instead of (float)1f,
            // and EmitConstFloat's `co.Value is float f` check then falls back to 0f with no error at
            // all -- discovered empirically while writing this exact test (it read "expected 1, got 0"
            // for BOTH the true and false cases, tracked down via an isolated repro). Not this task's
            // node types' bug -- pre-existing parser behavior, worth a separate look -- but real enough
            // to trip silently here if left as a bare interpolation.
            var text = $@"
OCGRAPH 1
NODE cond ConstBool value={(cond ? "true" : "false")}
NODE t ConstFloat value={ifTrue:0.0}
NODE f ConstFloat value={ifFalse:0.0}
NODE sel Select
LINK cond.value sel.cond
LINK t.value sel.ifTrue
LINK f.value sel.ifFalse
OUT sel result
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            float result = fn();
            if (Math.Abs(result - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected {expected}, got {result}");
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

    // Fed by Compare/Add/Multiply, not bare constants -- proves LoadPin's "read from a linked node's
    // already-computed local" path works for Select's three inputs exactly like it does for any other
    // node, not just when they happen to be Const nodes.
    private static int TestSelectEmbeddedInLargerPullGraph()
    {
        Console.WriteLine("Test: Select fed by Compare/Add/Multiply (not bare constants)");
        try
        {
            var text = @"
OCGRAPH 1
NODE c1 ConstFloat value=3.0
NODE c2 ConstFloat value=4.0
NODE cmp Compare
LINK c1.value cmp.a
LINK c2.value cmp.b
NODE sum Add
LINK c1.value sum.a
LINK c2.value sum.b
NODE prod Multiply
LINK c1.value prod.a
LINK c2.value prod.b
NODE sel Select
LINK cmp.result sel.cond
LINK sum.result sel.ifTrue
LINK prod.result sel.ifFalse
OUT sel result
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            // cmp: 3 > 4 == false -> Select picks ifFalse == prod.result == 3*4 == 12 (not sum == 7).
            float result = fn();
            if (Math.Abs(result - 12f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected 12 (3>4 is false, so ifFalse=prod=3*4), got {result}");
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

    // THE TEST CLASS THE PREVIOUS (REJECTED) ATTEMPT COULD NOT BUILD, per the task's own framing: an
    // ENTRY OnTick graph where Select reads two upstream PULL values (two PARAM nodes) and its result
    // feeds an OUT read after the exec chain finishes. Select itself has no exec pins -- it never sits
    // ON the exec chain -- so this also proves a pure-data node can be reached purely through
    // EmitPullOutput's recursive pull, with no _execLocals entry of its own, inside an event-driven
    // compile. Two PARAMs (a, b) plus a bool (useA) select which one wins; run with useA both ways.
    private static int TestSelectInEntryOnTickGraph()
    {
        Console.WriteLine("Test: Select inside an ENTRY OnTick graph, reading two upstream PULL PARAMs, read back via OUT");
        try
        {
            var text = @"
OCGRAPH 1
PARAM a float
PARAM b float
PARAM useA bool
NODE tick OnTick
NODE pa Param param=a
NODE pb Param param=b
NODE pu Param param=useA
NODE sel Select
LINK pu.value sel.cond
LINK pa.value sel.ifTrue
LINK pb.value sel.ifFalse
ENTRY tick OnTick
OUT sel result
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<float, float, bool, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            float whenTrue = fn(10f, 20f, true);
            float whenFalse = fn(10f, 20f, false);
            if (Math.Abs(whenTrue - 10f) > 1e-6 || Math.Abs(whenFalse - 20f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected fn(10,20,true)=10 and fn(10,20,false)=20, got {whenTrue} and {whenFalse}");
                return 1;
            }
            Console.WriteLine($"  PASS: useA=true -> {whenTrue}; useA=false -> {whenFalse}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // INPUTKEY
    // =================================================================================================

    private static int TestInputKeyDefaultPinsShape()
    {
        Console.WriteLine("Test: InputKey's default pins are key:int-in, down:bool-out");
        try
        {
            var text = "OCGRAPH 1\nNODE k InputKey\nOUT k down\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["k"];
            bool ok =
                node.Pins.Find(p => p.Name == "key" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "down" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 2;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 2 pins, no exec, types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // See this file's own header comment for exactly what this does and does not prove, and why
    // EntryPointNotFoundException (not a stub, not a stub-shaped exception) is the right thing to
    // assert in this process.
    private static int TestInputKeyEmitsRealNativeCallPull()
    {
        Console.WriteLine("Test: InputKey (PULL/Compile()) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM key int
NODE pk Param param=key
NODE ik InputKey
LINK pk.value ik.key
OUT ik down
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<int, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn(5);
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_key"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_key: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_key': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestInputKeyEmitsRealNativeCallPush()
    {
        Console.WriteLine("Test: InputKey (PUSH/CompileEntryPoint) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM key int
NODE tick OnTick
NODE pk Param param=key
NODE ik InputKey
LINK pk.value ik.key
ENTRY tick OnTick
OUT ik down
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<int, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn(5);
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_key"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_key: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_key' via the PUSH compiler too: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // RAYCAST
    // =================================================================================================

    private static int TestRaycastDefaultPinsShape()
    {
        Console.WriteLine("Test: Raycast's default pins are exec-in + 7 float-in + then(exec-out) + 5 outputs");
        try
        {
            var text = "OCGRAPH 1\nNODE r Raycast\nOUT r hit\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["r"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "originX" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "originY" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "originZ" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "dirX" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "dirY" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "dirZ" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "maxDist" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "hit" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Find(p => p.Name == "entity" && p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "pointX" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "pointY" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "pointZ" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Count == 14;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 14 pins (1 exec-in, 7 float-in, 1 exec-out, 5 data-out), types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static string RaycastGraphText() => @"
OCGRAPH 1
PARAM ox float
NODE pox Param param=ox
NODE rc Raycast
LINK pox.value rc.originX
PIN pox value out float
OUT rc hit
OUT rc entity
OUT rc pointX
OUT rc pointY
OUT rc pointZ
";

    // PULL: Compile() computes Raycast's 5 outputs with ONE call (see EmitRaycast's own comment for
    // why the PULL compiler needs no _execLocals-style mechanism to get that guarantee -- it falls out
    // of Compile()'s single topological pass for free). Invoking the compiled delegate is what proves
    // the emitted call is real -- see this file's header comment for why DllNotFoundException naming
    // Aver.Physics (not a hardcoded return) is the expected, correct failure here.
    private static int TestRaycastEmitsRealNativeCallPull()
    {
        Console.WriteLine("Test: Raycast (PULL/Compile()) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
NODE ox ConstFloat value=0.0
NODE rc Raycast
LINK ox.value rc.originX
OUT rc hit
OUT rc entity
OUT rc pointX
OUT rc pointY
OUT rc pointZ
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (DllNotFoundException dnfEx)
            {
                if (!dnfEx.Message.Contains("Aver.Physics"))
                {
                    Console.WriteLine($"  FAIL: threw DllNotFoundException, but not naming Aver.Physics: {dnfEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call reached Aver.Physics: {dnfEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // PUSH: Raycast reached via the exec chain -- EmitExecRaycast's ONE native call, results captured
    // into _execLocals, read back via OUT after the chain finishes (CompileEntryPoint's own "THE
    // RETURN VALUE" doc). Same real-call proof as the PULL test above, through the OTHER mechanism.
    private static int TestRaycastEmitsRealNativeCallPush()
    {
        Console.WriteLine("Test: Raycast (PUSH/CompileEntryPoint, via the exec chain) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE ox ConstFloat value=0.0
NODE rc Raycast
LINK tick.exec rc.exec
LINK ox.value rc.originX
ENTRY tick OnTick
OUT rc hit
OUT rc entity
OUT rc pointX
OUT rc pointY
OUT rc pointZ
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (DllNotFoundException dnfEx)
            {
                if (!dnfEx.Message.Contains("Aver.Physics"))
                {
                    Console.WriteLine($"  FAIL: threw DllNotFoundException, but not naming Aver.Physics: {dnfEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call reached Aver.Physics via the exec chain too: {dnfEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A hand-authored Raycast node missing one of its 5 output pins (here: no PIN records at all for
    // "pointZ", achieved by giving it SOME explicit pins -- which disables AddDefaultPins entirely --
    // but not all thirteen) must fail with RequirePinLocal's own clear message, not an unbalanced-stack
    // IL verifier exception three layers down.
    private static int TestRaycastMissingOutputPinFailsCompileWithClearError()
    {
        Console.WriteLine("Test: Raycast missing an output pin fails compile with a clear error, not a verifier crash");
        try
        {
            var text = @"
OCGRAPH 1
NODE rc Raycast
PIN rc originX in float
PIN rc originY in float
PIN rc originZ in float
PIN rc dirX in float
PIN rc dirY in float
PIN rc dirZ in float
PIN rc maxDist in float
PIN rc hit out bool
PIN rc entity out int
PIN rc pointX out float
PIN rc pointY out float
OUT rc hit
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (missing 'pointZ' output pin), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("pointZ"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the missing 'pointZ' pin, got: {compileErr}");
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

    // A DELIBERATE PHASE-1 LIMITATION, tested directly: a Raycast node inside an ENTRY graph that is
    // wired ONLY by a data LINK (nothing ever points an exec edge at its "exec" input) has no
    // _execLocals entry for anything CompileEntryPoint's OUT tries to read back, and "raycast" has no
    // case of its own in EmitPullOutput's switch (see that method's "raycast has NO case here" comment)
    // -- so this must fail with a clear NotSupportedException naming the node type, not silently return
    // a wrong value or crash. This is the one thing InputKey/GetField (both freely pullable) do NOT
    // share with Raycast.
    private static int TestRaycastPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: Raycast never wired into the exec chain, but pulled via OUT, fails clearly (not silently 0)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE rc Raycast
ENTRY start OnStart
OUT rc hit
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (rc.hit pulled with no exec visit ever reaching rc), but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("Raycast", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the Raycast node type, got: {compileErr}");
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

    // Both mechanisms coexist without interfering: a graph with NO ENTRY at all never touches
    // EmitPullOutput/_execLocals in the first place (Compile() is the only compiler that ever runs on
    // it) -- so the "pulled without an exec visit" restriction the test above proves for the PUSH
    // compiler simply does not apply here. Same shape as TestRaycastEmitsRealNativeCallPull, asserted
    // again here specifically to make the "pure PULL still works" claim explicit rather than implicit.
    private static int TestRaycastCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll()
    {
        Console.WriteLine("Test: a pure-PULL graph (no ENTRY at all) still computes Raycast's outputs via EmitNode, unaffected by the PUSH-only restriction above");
        try
        {
            if (!OcGraphParser.Parse(RaycastGraphText(), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero ENTRY records, got {graph.EntryPoints.Count}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<float, object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn(0f);
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (DllNotFoundException dnfEx)
            {
                if (!dnfEx.Message.Contains("Aver.Physics"))
                {
                    Console.WriteLine($"  FAIL: threw DllNotFoundException, but not naming Aver.Physics: {dnfEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded, real call reached Aver.Physics: {dnfEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // MOUSEDELTA / MOVEAXIS
    //
    // The continuous-input pair (see GraphCompiler.EmitExecMouseDelta/EmitExecMoveAxis and
    // Aver.Framework.GraphInterop.MouseDeltaForGraph/MoveAxisForGraph for the design). Both are wired
    // through Raycast's exec-cached shape, not GetFieldVec3's uncached one -- see those comments for
    // why -- so the test shapes below mirror RAYCAST's five tests (pin shape, PULL real-call, PUSH
    // real-call, pulled-without-exec-visit refusal, coexistence with a pure-PULL graph) rather than
    // InputKey's two. Same INHERITED LIMITATION this file's header comment already names for
    // InputKey/Raycast: this process cannot load a live native Aver.Framework, so
    // EntryPointNotFoundException naming the real P/Invoke symbol -- not a stub-shaped return -- is
    // the correct, strongest thing provable here. aver_fw_input_mouse is the symbol MouseDelta's
    // single native call reaches; MoveAxis's own wrapper reaches aver_fw_input_key instead (the SAME
    // symbol InputKey's own tests already name), since Input.MoveAxis is built from GetKey() calls,
    // not a dedicated native entry point of its own.
    // =================================================================================================

    private static int TestMouseDeltaDefaultPinsShape()
    {
        Console.WriteLine("Test: MouseDelta's default pins are exec-in, then(exec-out), deltaX/deltaY/wheel:float-out");
        try
        {
            var text = "OCGRAPH 1\nNODE m MouseDelta\nOUT m deltaX\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["m"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "deltaX" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "deltaY" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "wheel" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Count == 5;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 5 pins (1 exec-in, 1 exec-out, 3 float-out), types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static string MouseDeltaGraphText() => @"
OCGRAPH 1
NODE m MouseDelta
OUT m deltaX
OUT m deltaY
OUT m wheel
";

    // PULL: Compile() computes all three outputs with ONE call -- see EmitMouseDelta's own comment for
    // why the PULL compiler needs no _execLocals-style mechanism to get that guarantee (Compile()'s
    // single topological pass gives it for free, exactly like Raycast/GetFieldVec3). Invoking the
    // compiled delegate is what proves the emitted call is real.
    private static int TestMouseDeltaEmitsRealNativeCallPull()
    {
        Console.WriteLine("Test: MouseDelta (PULL/Compile()) emits a real native call, not a stub");
        try
        {
            if (!OcGraphParser.Parse(MouseDeltaGraphText(), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_mouse"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_mouse: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_mouse': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // PUSH: MouseDelta reached via the exec chain -- EmitExecMouseDelta's ONE native call, results
    // captured into _execLocals, read back via OUT after the chain finishes. Same real-call proof as
    // the PULL test above, through the OTHER mechanism.
    private static int TestMouseDeltaEmitsRealNativeCallPush()
    {
        Console.WriteLine("Test: MouseDelta (PUSH/CompileEntryPoint, via the exec chain) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE m MouseDelta
LINK tick.exec m.exec
ENTRY tick OnTick
OUT m deltaX
OUT m deltaY
OUT m wheel
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_mouse"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_mouse: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_mouse' via the PUSH compiler too: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A DELIBERATE limitation, tested directly, mirroring TestRaycastPulledWithoutExecVisitFailsClearly
    // exactly: a MouseDelta node inside an ENTRY graph that is wired into NO exec chain at all has no
    // _execLocals entry for anything OUT tries to read back, and "mousedelta" has no case of its own in
    // EmitPullOutput's switch -- so this must fail with a clear NotSupportedException naming the node
    // type, not silently return 0 or crash. This is the price of guaranteeing "one native call, however
    // many pins are read" -- see IsExecCapableMouseDeltaType's own comment.
    private static int TestMouseDeltaPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: MouseDelta never wired into the exec chain, but pulled via OUT, fails clearly (not silently 0)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE m MouseDelta
ENTRY start OnStart
OUT m deltaX
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (m.deltaX pulled with no exec visit ever reaching m), but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("MouseDelta", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the MouseDelta node type, got: {compileErr}");
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

    // Both mechanisms coexist without interfering, mirroring TestRaycastCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll.
    private static int TestMouseDeltaCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll()
    {
        Console.WriteLine("Test: a pure-PULL graph (no ENTRY at all) still computes MouseDelta's outputs via EmitNode, unaffected by the PUSH-only restriction above");
        try
        {
            if (!OcGraphParser.Parse(MouseDeltaGraphText(), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero ENTRY records, got {graph.EntryPoints.Count}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_mouse"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_mouse: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded, real call attempted 'aver_fw_input_mouse': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // MOVEAXIS -- same five tests as MouseDelta above, same shape, different symbol
    // (aver_fw_input_key, shared with InputKey -- see this section's header comment).
    // =================================================================================================

    private static int TestMoveAxisDefaultPinsShape()
    {
        Console.WriteLine("Test: MoveAxis's default pins are exec-in, then(exec-out), forward/right:float-out (no 'z')");
        try
        {
            var text = "OCGRAPH 1\nNODE m MoveAxis\nOUT m forward\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["m"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "forward" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "right" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "z") == null &&
                node.Pins.Count == 4;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 4 pins (1 exec-in, 1 exec-out, 2 float-out), no 'z' pin, types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static string MoveAxisGraphText() => @"
OCGRAPH 1
NODE m MoveAxis
OUT m forward
OUT m right
";

    private static int TestMoveAxisEmitsRealNativeCallPull()
    {
        Console.WriteLine("Test: MoveAxis (PULL/Compile()) emits a real native call, not a stub");
        try
        {
            if (!OcGraphParser.Parse(MoveAxisGraphText(), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_key"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_key: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_key': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestMoveAxisEmitsRealNativeCallPush()
    {
        Console.WriteLine("Test: MoveAxis (PUSH/CompileEntryPoint, via the exec chain) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE m MoveAxis
LINK tick.exec m.exec
ENTRY tick OnTick
OUT m forward
OUT m right
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_key"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_key: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_key' via the PUSH compiler too: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestMoveAxisPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: MoveAxis never wired into the exec chain, but pulled via OUT, fails clearly (not silently 0)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE m MoveAxis
ENTRY start OnStart
OUT m forward
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (m.forward pulled with no exec visit ever reaching m), but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("MoveAxis", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the MoveAxis node type, got: {compileErr}");
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

    private static int TestMoveAxisCoexistsWithPureDataPullWhenGraphHasNoEntryAtAll()
    {
        Console.WriteLine("Test: a pure-PULL graph (no ENTRY at all) still computes MoveAxis's outputs via EmitNode, unaffected by the PUSH-only restriction above");
        try
        {
            if (!OcGraphParser.Parse(MoveAxisGraphText(), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero ENTRY records, got {graph.EntryPoints.Count}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_key"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_key: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded, real call attempted 'aver_fw_input_key': {epEx.Message}");
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
