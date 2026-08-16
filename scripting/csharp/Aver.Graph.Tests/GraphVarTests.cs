// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for graph-local persistent variables (VAR / GetVar / SetVar / GraphVarStore) -- the addition
// that closes "a compiled graph is a pure function of its PARAMs; nothing survives between ticks". See
// GraphVariable's own comment (Graph.cs), GraphVarStore's own comment, and GraphCompiler.cs's
// EmitGetVar/EmitPullGetVar/EmitExecSetVar/IsExecCapableVarSideEffectType comments for the design these
// tests prove, and OcGraphParser.cs's "VAR" record / "GetVar / SetVar" AddDefaultPins section for the
// grammar and pin shapes.
//
// UNLIKE MOST OF THE NODE VOCABULARY THIS PROJECT HAS ADDED SO FAR (GetField/SetField/Raycast/Spawn/
// MouseDelta/...), GraphVarStore is NOT a native surface at all -- it is a plain in-process C# object
// GraphCompiler's own emitted IL calls into directly. So, unlike GetFieldTests/NewNodeTests/
// SpawnNodeTests (which can only prove "the emitted IL reaches for a genuine native symbol" by
// observing the SPECIFIC failure that reaching produces, because nothing in this test process boots a
// live native scene), the tests below invoke compiled delegates for real and assert on the VALUES they
// return -- the honest limitation every other node-vocabulary test file in this project names does not
// apply here, because there is no native boundary to be honest about.
using System;
using Aver.Graph;

static class GraphVarTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- grammar / parsing ----
        failures += TestVarRecordParsesWithDefault();
        failures += TestVarWithoutDefaultFallsBackToZero();
        failures += TestVarRejectsExecType();
        failures += TestVarDuplicateDeclarationFailsToParse();
        failures += TestGraphWithoutVarRecordsHasEmptyVariablesList();

        // ---- Validate(): the three named rejections, each naming the variable ----
        failures += TestGetVarMissingAttributeFailsToParse();
        failures += TestSetVarMissingAttributeFailsToParse();
        failures += TestGetVarUndeclaredVariableFailsToParse();
        failures += TestSetVarUndeclaredVariableFailsToParse();
        failures += TestGetVarTypeMismatchOnExplicitPinFailsToParse();
        failures += TestSetVarTypeMismatchOnExplicitPinFailsToParse();

        // ---- default pin shapes ----
        failures += TestGetVarDefaultPinsShape();
        failures += TestSetVarDefaultPinsShape();

        // ---- the core deliverable: values a compiled delegate actually returns ----
        failures += TestVariableReadBeforeAnyWriteReturnsDeclaredDefault();
        failures += TestVariableReadBeforeAnyWriteReturnsZeroWhenNoDefaultGiven();
        failures += TestSetVarThenGetVarWithinSameInvocation();
        failures += TestVariablePersistsAcrossTwoSeparateInvocationsOfSameCompiledDelegate();
        failures += TestGetVarPullableFromBothCompilePaths();

        // ---- SetVar is exec-only: a proven refusal on the pull path ----
        failures += TestSetVarRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestSetVarPulledWithoutExecVisitFailsClearly();

        // ---- THE TEST THAT MATTERS: per-host-instance storage, not per-graph-file ----
        failures += TestTwoGraphHostsOverSameGraphFileHaveIndependentVariables();
        failures += TestGraphHostVariablesResetToDeclaredDefaultsOnReload();

        // ---- adversarial pass added during verification: exec-chain ordering and control flow ----
        failures += TestSetVarInUntakenBranchArmDoesNotWrite();
        failures += TestGetVarPullBeforeLaterSetVarSeesThePreWriteValue();

        return failures;
    }

    // =================================================================================================
    // GRAMMAR / PARSING
    // =================================================================================================

    private static int TestVarRecordParsesWithDefault()
    {
        Console.WriteLine("Test: VAR <name> <type> <default> parses into graph.Variables");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 7\nVAR cooldown float 1.5\nVAR armed bool true\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.Variables.Count != 3)
            {
                Console.WriteLine($"  FAIL: expected 3 declared variables, got {graph.Variables.Count}");
                return 1;
            }
            var score = graph.Variables.Find(v => v.Name == "score");
            var cooldown = graph.Variables.Find(v => v.Name == "cooldown");
            var armed = graph.Variables.Find(v => v.Name == "armed");
            bool ok = score != null && score.Type == PinType.Int && score.Default is int si && si == 7 &&
                      cooldown != null && cooldown.Type == PinType.Float && cooldown.Default is float cf && Math.Abs(cf - 1.5f) < 1e-6 &&
                      armed != null && armed.Type == PinType.Bool && armed.Default is bool ab && ab == true;
            if (!ok)
            {
                Console.WriteLine("  FAIL: one or more declared variables did not have the expected name/type/default");
                return 1;
            }
            Console.WriteLine("  PASS: score:int=7, cooldown:float=1.5, armed:bool=true all parsed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // An omitted [default] falls back to the type's own zero value, deterministically -- never null,
    // never garbage. See GraphVarStore.CreateFor's own comment for why Default is never null by the
    // time it reaches runtime.
    private static int TestVarWithoutDefaultFallsBackToZero()
    {
        Console.WriteLine("Test: VAR with no [default] token falls back to the type's zero value");
        try
        {
            var text = "OCGRAPH 1\nVAR speed float\nVAR count int\nVAR flag bool\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var speed = graph.Variables.Find(v => v.Name == "speed");
            var count = graph.Variables.Find(v => v.Name == "count");
            var flag = graph.Variables.Find(v => v.Name == "flag");
            bool ok = speed != null && speed.Default is float sf && sf == 0f &&
                      count != null && count.Default is int ci && ci == 0 &&
                      flag != null && flag.Default is bool fb && fb == false;
            if (!ok)
            {
                Console.WriteLine("  FAIL: one or more defaultless variables did not fall back to a zero value");
                return 1;
            }
            Console.WriteLine("  PASS: speed=0f, count=0, flag=false");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // VAR is DATA a graph remembers between ticks, not control flow -- rejected at PARSE time, the
    // identical reasoning PARAM's own Exec rejection already has (see
    // TestExecTypedParamIsRejectedAtParseTime in GraphFlowTests.cs).
    private static int TestVarRejectsExecType()
    {
        Console.WriteLine("Test: VAR declared exec is rejected at parse time");
        try
        {
            var text = "OCGRAPH 1\nVAR trigger exec\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (VAR cannot be exec), but it succeeded");
                return 1;
            }
            if (err == null || err.IndexOf("exec", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error mentioning exec, got: {err}");
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

    // Two VAR records with the same name make "which one does var= mean" ambiguous -- rejected by
    // Graph.Validate(), the same reason two PARAM declarations can't share a name.
    private static int TestVarDuplicateDeclarationFailsToParse()
    {
        Console.WriteLine("Test: duplicate VAR declaration fails to parse");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 0\nVAR score int 5\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (duplicate VAR 'score'), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("Duplicate VAR") || !err.Contains("score"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the duplicate VAR 'score', got: {err}");
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

    // Backward compatibility, mirroring TestGraphWithoutParamsStillHasNoArguments in Program.cs: a
    // graph with no VAR records at all -- i.e. every .ocgraph file that existed before this change --
    // has an empty Variables list, and (proven separately by every other pre-existing test in this
    // whole suite continuing to compile to its ORIGINAL delegate shape) GraphCompiler appends no
    // trailing GraphVarStore parameter for it.
    private static int TestGraphWithoutVarRecordsHasEmptyVariablesList()
    {
        Console.WriteLine("Test: graph with no VAR records has an empty Variables list");
        try
        {
            var text = "OCGRAPH 1\nNODE 1 ConstFloat value=5.0\nOUT 1 value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.Variables.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero declared variables, got {graph.Variables.Count}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: expected a zero-argument Func<float> (no GraphVarStore parameter), got {compiled?.GetType().Name ?? "null"} ({compileErr})");
                return 1;
            }
            Console.WriteLine($"  PASS: {fn()}, delegate shape unchanged by the VAR addition");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // Validate(): the three named rejections
    // =================================================================================================

    private static int TestGetVarMissingAttributeFailsToParse()
    {
        Console.WriteLine("Test: GetVar node with no var= attribute fails to parse");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 0\nNODE gv GetVar\nOUT gv value\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (GetVar has no var= attribute), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("GetVar") || !err.Contains("var="))
            {
                Console.WriteLine($"  FAIL: expected an error naming GetVar and the missing var= attribute, got: {err}");
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

    private static int TestSetVarMissingAttributeFailsToParse()
    {
        Console.WriteLine("Test: SetVar node with no var= attribute fails to parse");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 0\nNODE start OnStart\nNODE sv SetVar\nLINK start.exec sv.exec\nENTRY start OnStart\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (SetVar has no var= attribute), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("SetVar") || !err.Contains("var="))
            {
                Console.WriteLine($"  FAIL: expected an error naming SetVar and the missing var= attribute, got: {err}");
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

    private static int TestGetVarUndeclaredVariableFailsToParse()
    {
        Console.WriteLine("Test: GetVar referencing an undeclared variable fails to parse");
        try
        {
            var text = "OCGRAPH 1\nNODE gv GetVar var=score\nOUT gv value\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail ('score' was never declared with VAR), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("undeclared variable") || !err.Contains("score"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the undeclared variable 'score', got: {err}");
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

    private static int TestSetVarUndeclaredVariableFailsToParse()
    {
        Console.WriteLine("Test: SetVar referencing an undeclared variable fails to parse");
        try
        {
            var text = "OCGRAPH 1\nNODE start OnStart\nNODE sv SetVar var=score\nLINK start.exec sv.exec\nENTRY start OnStart\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail ('score' was never declared with VAR), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("undeclared variable") || !err.Contains("score"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the undeclared variable 'score', got: {err}");
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

    // A hand-authored PIN record on GetVar whose type disagrees with the declared VAR's type must be
    // rejected, exactly like Param's own valuePin.Type mismatch check.
    private static int TestGetVarTypeMismatchOnExplicitPinFailsToParse()
    {
        Console.WriteLine("Test: GetVar's explicit 'value' pin disagreeing with the VAR's declared type fails to parse");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 0\nNODE gv GetVar var=score\nPIN gv value out float\nOUT gv value\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (pin says Float, VAR 'score' is Int), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("score") || !err.Contains("Float") || !err.Contains("Int"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the type mismatch, got: {err}");
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

    private static int TestSetVarTypeMismatchOnExplicitPinFailsToParse()
    {
        Console.WriteLine("Test: SetVar's explicit 'value' pin disagreeing with the VAR's declared type fails to parse");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 0\nNODE start OnStart\nNODE sv SetVar var=score\n" +
                       "PIN sv exec in exec\nPIN sv value in bool\nPIN sv then out exec\n" +
                       "LINK start.exec sv.exec\nENTRY start OnStart\n";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (pin says Bool, VAR 'score' is Int), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("score") || !err.Contains("Bool") || !err.Contains("Int"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the type mismatch, got: {err}");
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

    // =================================================================================================
    // DEFAULT PIN SHAPES
    // =================================================================================================

    private static int TestGetVarDefaultPinsShape()
    {
        Console.WriteLine("Test: GetVar's default pins are exactly one output, 'value', typed to the declared VAR");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 7\nNODE gv GetVar var=score\nOUT gv value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gv"];
            bool ok = node.Pins.Count == 1 &&
                      node.Pins[0].Name == "value" && node.Pins[0].IsOutput && node.Pins[0].Type == PinType.Int;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: exactly one pin, 'value', Int, output -- no exec pins");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetVarDefaultPinsShape()
    {
        Console.WriteLine("Test: SetVar's default pins are exec-in + value-in:typed + then(exec-out) -- exec pins present by default");
        try
        {
            var text = "OCGRAPH 1\nVAR cooldown float 1.5\nNODE start OnStart\nNODE sv SetVar var=cooldown\nLINK start.exec sv.exec\nENTRY start OnStart\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sv"];
            bool ok = node.Pins.Count == 3 &&
                      node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                      node.Pins.Find(p => p.Name == "value" && !p.IsOutput && p.Type == PinType.Float) != null &&
                      node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: exec-in, value-in:Float, then-out:Exec -- no 'success' pin (see EmitExecSetVar's own comment for why none is needed)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // THE CORE DELIVERABLE: values a compiled delegate actually returns
    // =================================================================================================

    // A variable read before any write returns its declared default, deterministically -- proven by
    // compiling a PURE-PULL graph (Compile(), no ENTRY at all) with a GetVar as its only node, and
    // seeding a store the same way GraphHost itself does (GraphVarStore.CreateFor).
    private static int TestVariableReadBeforeAnyWriteReturnsDeclaredDefault()
    {
        Console.WriteLine("Test: GetVar, before any SetVar ever runs, returns the declared default");
        try
        {
            var text = "OCGRAPH 1\nVAR score int 42\nNODE gv GetVar var=score\nOUT gv value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<GraphVarStore, int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape -- expected a trailing GraphVarStore parameter)"}");
                return 1;
            }
            var store = GraphVarStore.CreateFor(graph);
            int result = fn(store);
            if (result != 42)
            {
                Console.WriteLine($"  FAIL: expected 42 (the declared default), got {result}");
                return 1;
            }
            Console.WriteLine($"  PASS: fn(store) = {result}, no SetVar ever ran");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestVariableReadBeforeAnyWriteReturnsZeroWhenNoDefaultGiven()
    {
        Console.WriteLine("Test: GetVar, with no [default] declared at all, reads back the type's zero value");
        try
        {
            var text = "OCGRAPH 1\nVAR level float\nNODE gv GetVar var=level\nOUT gv value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var compileErr);
            if (compiled is not Func<GraphVarStore, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            var store = GraphVarStore.CreateFor(graph);
            float result = fn(store);
            if (result != 0f)
            {
                Console.WriteLine($"  FAIL: expected 0f, got {result}");
                return 1;
            }
            Console.WriteLine($"  PASS: fn(store) = {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // SetVar writes, then GetVar reads the SAME store within the SAME invocation of the SAME compiled
    // delegate -- proves the write is genuinely visible before the method returns, exec-chain-ordered
    // (SetVar runs on the OnStart chain; the read happens via OUT, evaluated after the chain finishes).
    private static int TestSetVarThenGetVarWithinSameInvocation()
    {
        Console.WriteLine("Test: SetVar then GetVar, within one invocation, reads back the just-written value");
        try
        {
            var text = @"
OCGRAPH 1
VAR score int 0
NODE start OnStart
NODE five ConstInt value=5
NODE sv SetVar var=score
NODE gv GetVar var=score
LINK start.exec sv.exec
LINK five.value sv.value
ENTRY start OnStart
OUT gv value
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<GraphVarStore, int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            var store = GraphVarStore.CreateFor(graph);
            int result = fn(store);
            if (result != 5)
            {
                Console.WriteLine($"  FAIL: expected 5 (written by SetVar, read back by GetVar), got {result}");
                return 1;
            }
            Console.WriteLine($"  PASS: fn(store) = {result}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE DIRECT PROOF THAT THE GAP IS CLOSED: ONE compiled delegate, invoked TWICE with the SAME
    // GraphVarStore instance, remembers what the first invocation wrote. Before this slice, every
    // compiled graph was a pure function of its PARAMs alone -- a second, separate invocation could
    // never see anything the first one did. score += delta, read back, on each call.
    private static int TestVariablePersistsAcrossTwoSeparateInvocationsOfSameCompiledDelegate()
    {
        Console.WriteLine("Test: a variable persists across two SEPARATE invocations of the SAME compiled delegate");
        try
        {
            var text = @"
OCGRAPH 1
VAR score float 0
PARAM delta float
NODE tick OnTick
NODE pd Param param=delta
NODE gv GetVar var=score
NODE add Add
NODE sv SetVar var=score
LINK tick.exec sv.exec
LINK gv.value add.a
LINK pd.value add.b
LINK add.result sv.value
ENTRY tick OnTick
OUT gv value
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<float, GraphVarStore, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            var store = GraphVarStore.CreateFor(graph);

            float afterFirst = fn(5f, store);
            if (Math.Abs(afterFirst - 5f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: after first invocation (delta=5, starting from default 0) expected 5, got {afterFirst}");
                return 1;
            }

            // THE ASSERTION THAT MATTERS: a SECOND, entirely separate call to the SAME compiled
            // delegate, with the SAME store, sees the first call's write and accumulates onto it.
            float afterSecond = fn(3f, store);
            if (Math.Abs(afterSecond - 8f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: after second invocation (delta=3, expected to accumulate onto the 5 the first invocation left) expected 8, got {afterSecond}");
                return 1;
            }

            Console.WriteLine($"  PASS: fn(5,store)={afterFirst}, then fn(3,store)={afterSecond} -- the second call saw the first call's write");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // GetVar is reachable from BOTH compile paths -- Compile() (pure PULL/dataflow) and
    // CompileEntryPoint() (PUSH/exec) -- exactly like GetField, and unlike SetVar (which only the
    // second path accepts; see TestSetVarRefusedByPullCompilerEvenWithNoEntryAtAll below).
    private static int TestGetVarPullableFromBothCompilePaths()
    {
        Console.WriteLine("Test: GetVar is pullable from both Compile() and CompileEntryPoint()");
        try
        {
            // Path A: Compile() -- no ENTRY at all.
            var textA = "OCGRAPH 1\nVAR level int 3\nNODE gv GetVar var=level\nOUT gv value\n";
            if (!OcGraphParser.Parse(textA, out var graphA, out var errA))
            {
                Console.WriteLine($"  FAIL: Parse error (path A): {errA}");
                return 1;
            }
            var compiledA = new GraphCompiler(graphA).Compile(out var compileErrA);
            if (compiledA is not Func<GraphVarStore, int> fnA)
            {
                Console.WriteLine($"  FAIL: Compile() error: {compileErrA ?? "(wrong delegate shape)"}");
                return 1;
            }
            int resultA = fnA(GraphVarStore.CreateFor(graphA));
            if (resultA != 3)
            {
                Console.WriteLine($"  FAIL: Compile() path: expected 3, got {resultA}");
                return 1;
            }

            // Path B: CompileEntryPoint() -- GetVar reached purely via OUT, with NO exec wiring to it
            // at all (proving it needs no exec visit, unlike SetVar).
            var textB = "OCGRAPH 1\nVAR level int 3\nNODE start OnStart\nNODE gv GetVar var=level\nENTRY start OnStart\nOUT gv value\n";
            if (!OcGraphParser.Parse(textB, out var graphB, out var errB))
            {
                Console.WriteLine($"  FAIL: Parse error (path B): {errB}");
                return 1;
            }
            var compiledB = new GraphCompiler(graphB).CompileEntryPoint("OnStart", out var compileErrB);
            if (compiledB is not Func<GraphVarStore, int> fnB)
            {
                Console.WriteLine($"  FAIL: CompileEntryPoint() error: {compileErrB ?? "(wrong delegate shape)"}");
                return 1;
            }
            int resultB = fnB(GraphVarStore.CreateFor(graphB));
            if (resultB != 3)
            {
                Console.WriteLine($"  FAIL: CompileEntryPoint() path: expected 3, got {resultB}");
                return 1;
            }

            Console.WriteLine($"  PASS: Compile()={resultA}, CompileEntryPoint()={resultB}, both read the declared default with no exec visit required");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // SetVar IS EXEC-ONLY: a proven refusal on the pull path
    // =================================================================================================

    // Mirrors SpawnNodeTests.TestSpawnRefusedByPullCompilerEvenWithNoEntryAtAll: a SetVar node in a
    // graph with NO ENTRY at all (so Compile(), the pure-PULL compiler, is the only one that could ever
    // run it) must fail to compile, with a message naming SetVar specifically -- not silently write on
    // every single invocation with no way to gate it.
    private static int TestSetVarRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: SetVar in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, SetVar-naming error");
        try
        {
            var text = "OCGRAPH 1\nVAR score float 0\nNODE one ConstFloat value=1.0\nNODE sv SetVar var=score\nLINK one.value sv.value\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero ENTRY records, got {graph.EntryPoints.Count}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected Compile() to refuse a SetVar node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("SetVar", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the SetVar node type, got: {compileErr}");
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

    // Mirrors SpawnNodeTests.TestSpawnPulledWithoutExecVisitFailsClearly: even INSIDE an ENTRY-driven
    // graph, a SetVar node the exec chain never actually visits must not be readable as a data value via
    // OUT. SetVar has NO default output pin (unlike Spawn's "entity"), so this hand-authors one via
    // explicit PIN records -- exactly the scenario IsExecCapableVarSideEffectType's own comment names
    // ("an author could still hand-add one via explicit PIN records") to prove the refusal holds even
    // then.
    private static int TestSetVarPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SetVar never wired into the exec chain, but pulled via a hand-authored output pin, fails clearly");
        try
        {
            var text = @"
OCGRAPH 1
VAR score float 0
NODE start OnStart
NODE sv SetVar var=score
PIN sv exec in exec
PIN sv value in float
PIN sv result out float
ENTRY start OnStart
OUT sv result
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).CompileEntryPoint("OnStart", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sv.result pulled with no exec visit ever reaching sv), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SetVar", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming SetVar and its side effect, got: {compileErr}");
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

    // =================================================================================================
    // THE TEST THAT MATTERS: per-host-instance storage, not per-graph-file
    // =================================================================================================

    // Two GraphHost instances, loading the exact SAME .ocgraph TEXT, each write a DIFFERENT value into
    // the same-named variable, then both read back through a SEPARATE, write-free event -- and must not
    // see each other. If storage were (incorrectly) keyed by graph file/text rather than by GraphHost
    // instance, hostB's write would clobber what hostA reads back; this test fails immediately if that
    // regression is ever introduced.
    private static int TestTwoGraphHostsOverSameGraphFileHaveIndependentVariables()
    {
        Console.WriteLine("Test: two GraphHosts over the SAME .ocgraph have INDEPENDENT variables");
        try
        {
            var text = @"
OCGRAPH 1
VAR score float 0
PARAM v float
NODE evt OnHit
NODE pv Param param=v
NODE sv SetVar var=score
NODE gv GetVar var=score
NODE q OnHit
LINK evt.exec sv.exec
LINK pv.value sv.value
ENTRY evt SetScore
ENTRY q Query
OUT gv value
";
            var hostA = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            var hostB = new GraphHost(positionSink: (int e, float x, float y, float z) => { });

            if (!hostA.LoadFromText(text, out var errA))
            {
                Console.WriteLine($"  FAIL: hostA.LoadFromText: {errA}");
                return 1;
            }
            if (!hostB.LoadFromText(text, out var errB))
            {
                Console.WriteLine($"  FAIL: hostB.LoadFromText: {errB}");
                return 1;
            }

            // Each host writes a DIFFERENT value.
            if (!hostA.Fire("SetScore", new object[] { 10f }, out var resultA) || resultA is not float scoreA || Math.Abs(scoreA - 10f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostA.Fire('SetScore', [10]) expected 10, got {resultA}");
                return 1;
            }
            if (!hostB.Fire("SetScore", new object[] { 99f }, out var resultB) || resultB is not float scoreB || Math.Abs(scoreB - 99f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostB.Fire('SetScore', [99]) expected 99, got {resultB}");
                return 1;
            }

            // A SEPARATE, write-free event on EACH host must read back ITS OWN value, not the other
            // host's -- the assertion that actually distinguishes "independent" from "shared".
            if (!hostA.Fire("Query", new object[] { 0f }, out var queryA) || queryA is not float qa || Math.Abs(qa - 10f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostA.Fire('Query') expected 10 (hostA's OWN write), got {queryA} -- hosts are NOT independent");
                return 1;
            }
            if (!hostB.Fire("Query", new object[] { 0f }, out var queryB) || queryB is not float qb || Math.Abs(qb - 99f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostB.Fire('Query') expected 99 (hostB's OWN write), got {queryB} -- hosts are NOT independent");
                return 1;
            }

            Console.WriteLine($"  PASS: hostA reads back {qa} (wrote 10), hostB reads back {qb} (wrote 99) -- same file, independent storage");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A reload (a second LoadFromText() call on the same host) replaces the variable store with a
    // BRAND NEW one, seeded fresh from declared defaults -- a hot-reloaded graph's variables reset
    // rather than carrying over whatever an earlier compile accumulated. See _varStore's own field
    // comment on GraphHost for why this is the owner's explicitly accepted trade-off, not a bug.
    private static int TestGraphHostVariablesResetToDeclaredDefaultsOnReload()
    {
        Console.WriteLine("Test: reloading the same host resets its variables to their declared defaults");
        try
        {
            var text = @"
OCGRAPH 1
VAR score float 0
PARAM v float
NODE bump OnHit
NODE pv Param param=v
NODE sv SetVar var=score
NODE gv GetVar var=score
NODE q OnHit
LINK bump.exec sv.exec
LINK pv.value sv.value
ENTRY bump Bump
ENTRY q Query
OUT gv value
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });

            if (!host.LoadFromText(text, out var err1))
            {
                Console.WriteLine($"  FAIL: first LoadFromText: {err1}");
                return 1;
            }
            if (!host.Fire("Bump", new object[] { 50f }, out var bumped) || bumped is not float bf || Math.Abs(bf - 50f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: Fire('Bump', [50]) expected 50, got {bumped}");
                return 1;
            }

            // Reload the SAME text on the SAME host -- LoadFromText's own "each call fully replaces
            // whatever compiled before it" contract, now extended to variables too.
            if (!host.LoadFromText(text, out var err2))
            {
                Console.WriteLine($"  FAIL: reload LoadFromText: {err2}");
                return 1;
            }
            if (!host.Fire("Query", new object[] { 0f }, out var afterReload) || afterReload is not float rf)
            {
                Console.WriteLine($"  FAIL: Fire('Query') after reload returned {afterReload}, expected a float");
                return 1;
            }
            if (Math.Abs(rf - 0f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected 0 (the declared default) after reload, got {rf} -- the old 50 leaked across Load()");
                return 1;
            }

            Console.WriteLine($"  PASS: score=50 before reload, score={rf} (declared default) after reload");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // ADVERSARIAL PASS ADDED DURING VERIFICATION: exec-chain ordering and control flow
    // =================================================================================================
    //
    // Neither of the two tests below was in the original slice. Both scenarios are DEFINED behaviour
    // -- not an accident of implementation -- because they fall out of mechanisms this file already
    // proves elsewhere for other node types: EmitBranch emits a REAL conditional branch (Brfalse/Br;
    // see GraphFlowTests.cs's own TestBranchTakesTrueSide/TestBranchTakesFalseSide for the same
    // mechanism proven on "tookTrue"), so an untaken arm's IL is simply never reached, and
    // EmitPullOutput's PULL path is UNCACHED/recursive by design (see that method's own section-level
    // comment) -- every pull site re-emits a fresh read, so a pull's position in the IL stream, not
    // some memoised value, decides whether it observes an earlier SetVar's write. Both were exercised
    // independently (fresh names/values) during adversarial review before being folded in here.

    // A SetVar wired into the FALSE arm of a Branch must not fire when the TRUE arm is taken, and vice
    // versa -- proven by reading the variable back (via a write-free GetVar/OUT) after each branch,
    // rather than merely trusting that Compile() succeeded.
    private static int TestSetVarInUntakenBranchArmDoesNotWrite()
    {
        Console.WriteLine("Test: SetVar wired into a Branch arm only fires when that arm is actually taken");
        try
        {
            var text = @"
OCGRAPH 1
VAR hits int 0
PARAM cond bool
NODE start OnStart
NODE b Branch
NODE pc Param param=cond
NODE one ConstInt value=1
NODE trueSet SetVar var=hits
NODE gv GetVar var=hits
LINK start.exec b.exec
LINK pc.value b.cond
LINK b.true trueSet.exec
LINK one.value trueSet.value
ENTRY start OnStart
OUT gv value
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<bool, GraphVarStore, int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            int resultFalse = fn(false, GraphVarStore.CreateFor(graph));
            if (resultFalse != 0)
            {
                Console.WriteLine($"  FAIL: cond=false took the branch's FALSE arm (nothing wired there); " +
                                   $"'hits' should still be its declared default 0, but SetVar on the untaken TRUE arm fired -- got {resultFalse}");
                return 1;
            }

            int resultTrue = fn(true, GraphVarStore.CreateFor(graph));
            if (resultTrue != 1)
            {
                Console.WriteLine($"  FAIL: cond=true should take the TRUE arm and let SetVar write 1, got {resultTrue}");
                return 1;
            }

            Console.WriteLine($"  PASS: cond=false -> hits={resultFalse} (untaken arm's SetVar did not fire), cond=true -> hits={resultTrue}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Within ONE invocation, a GetVar pulled EARLY in the exec chain (before a SetVar on the SAME
    // variable runs later in that same chain) must see the PRE-write value, not the value the later
    // SetVar is about to install -- proving pulls are live re-reads at their own position in the IL
    // stream, not a value memoised once per compiled method the way GetField/GetVar's sibling in the
    // OLD pure-PULL compiler caches into _pinLocals. Sequence fires "then0" (snapshot := a's CURRENT
    // value) before "then1" (a := 10); both are read back via a separate write-free GetVar/OUT pair
    // after the whole chain finishes.
    private static int TestGetVarPullBeforeLaterSetVarSeesThePreWriteValue()
    {
        Console.WriteLine("Test: a GetVar pulled BEFORE a later SetVar (same variable, same exec chain) sees the pre-write value");
        try
        {
            var text = @"
OCGRAPH 1
VAR a int 1
VAR snapshot int 0
NODE start OnStart
NODE seq Sequence
NODE gvEarly GetVar var=a
NODE snapSet SetVar var=snapshot
NODE ten ConstInt value=10
NODE aSet SetVar var=a
NODE gvSnap GetVar var=snapshot
NODE gvA GetVar var=a
LINK start.exec seq.exec
LINK seq.then0 snapSet.exec
LINK gvEarly.value snapSet.value
LINK seq.then1 aSet.exec
LINK ten.value aSet.value
ENTRY start OnStart
OUT gvSnap value
OUT gvA value
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<GraphVarStore, object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            var result = fn(GraphVarStore.CreateFor(graph));
            int snapshot = (int)result[0];
            int aFinal = (int)result[1];

            if (snapshot != 1)
            {
                Console.WriteLine($"  FAIL: expected snapshot=1 (a's declared default, captured BEFORE aSet ran later in the same chain), got {snapshot} -- " +
                                   "if this is 10, the early pull incorrectly observed the LATER write");
                return 1;
            }
            if (aFinal != 10)
            {
                Console.WriteLine($"  FAIL: expected a=10 (read back AFTER the whole chain finishes, via its own GetVar/OUT), got {aFinal}");
                return 1;
            }

            Console.WriteLine($"  PASS: snapshot={snapshot} (pre-write, captured before aSet ran), a={aFinal} (post-write)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
