// Tests for the ON-DEMAND event path: an event the HOST fires through GraphHost.Fire(), outside Tick(), with its payload
// (the graph's PARAM values, passed positionally) reaching the graph. "OnHit" is the worked example, not a special case:
// GraphHost never names it, and TestFireWorksForAnyNonTickEventName fires "OnSomethingElse" through the same path.
// No native host runs in this process, so TestFireEmitsRealNativeCallNotAStub proves the emitted IL reaches a real native
// symbol by the EntryPointNotFoundException it causes. Fire() invokes through Delegate.DynamicInvoke, which wraps that in a
// TargetInvocationException, so the test unwraps one level.
using System;
using Aver.Graph;

static class OnHitEventTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += Guarded(TestOnHitDefaultPinsShape);
        failures += Guarded(TestOnHitNodeSkippedByPullCompilerWhenGraphHasBothHalves);

        failures += Guarded(TestGraphHostLoadsOnHitOnlyGraphThatUsedToBeRejected);
        failures += Guarded(TestFireInvokesOnHitAndReturnsComputedPayload);
        failures += Guarded(TestFireCanBeFiredRepeatedlyWithDifferentPayloads);
        failures += Guarded(TestFireWorksForAnyNonTickEventName);
        failures += Guarded(TestFireReturnsFalseForUndeclaredEventName);
        failures += Guarded(TestFireBeforeLoadIsASafeNoOp);
        failures += Guarded(TestFireArgCountMismatchThrowsClearError);
        failures += Guarded(TestFireEmitsRealNativeCallNotAStub);

        failures += Guarded(TestTickIsANoOpForAnOnHitOnlyGraph);
        failures += Guarded(TestOnTickOnlyGraphStillLoadsAndTicksAfterTheRefactor);
        failures += Guarded(TestOnStartAndOnHitCanShareACompatibleParamList);
        failures += Guarded(TestMixingOnTickAndOnHitWithAnIncompatibleParamFailsToLoad);

        return failures;
    }

    // An exception escaping a test fails that test, reported like any other failure.
    private static int Guarded(Func<int> test)
    {
        try { return test(); }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static GraphHost NewHost() => new GraphHost(positionSink: (int e, float x, float y, float z) => { });

    // Loads the graph text; on failure prints the error and returns false.
    private static bool LoadOk(GraphHost host, string text)
    {
        if (host.LoadFromText(text, out var err)) return true;
        Console.WriteLine($"  FAIL: {err}");
        return false;
    }

    // =================================================================================================
    // NODE-TYPE SHAPE (parser + PULL-compiler parity with OnStart/OnTick)
    // =================================================================================================

    private static int TestOnHitDefaultPinsShape()
    {
        Console.WriteLine("Test: OnHit's default pins are a single exec-out, no inputs (same shape as OnStart/OnTick)");
        var text = "OCGRAPH 1\nNODE h OnHit\n";
        if (!OcGraphParser.Parse(text, out var graph, out var err))
        {
            Console.WriteLine($"  FAIL: Parse error: {err}");
            return 1;
        }
        var node = graph.Nodes["h"];
        bool ok = node.Pins.Count == 1 &&
                  node.Pins[0].Name == "exec" && node.Pins[0].IsOutput && node.Pins[0].Type == PinType.Exec;
        if (!ok)
        {
            Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
            return 1;
        }
        Console.WriteLine("  PASS: exactly one pin, exec, output");
        return 0;
    }

    // The PULL compiler never sees an event-driven graph, but a bare GraphCompiler caller might: an OnHit node must be
    // skipped there, as OnStart and OnTick are, and must not disturb the dataflow result.
    private static int TestOnHitNodeSkippedByPullCompilerWhenGraphHasBothHalves()
    {
        Console.WriteLine("Test: Compile() (PULL) skips an OnHit node and still computes the dataflow result correctly");
        var text = @"
OCGRAPH 1
NODE a ConstFloat value=3.0
NODE b ConstFloat value=4.0
NODE add Add
LINK a.value add.a
LINK b.value add.b
NODE h OnHit
ENTRY h OnHit
OUT add result
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
        if (Math.Abs(result - 7.0f) > 1e-4)
        {
            Console.WriteLine($"  FAIL: expected 3+4=7, got {result}");
            return 1;
        }
        Console.WriteLine($"  PASS: OnHit node present but skipped, dataflow result correct ({result})");
        return 0;
    }

    // =================================================================================================
    // GraphHost: load + Fire()
    // =================================================================================================

    // Regression: LoadEventGraph used to apply the entity/time/deltaTime PARAM check to every event-driven graph, so an
    // OnHit-only graph with a custom PARAM was rejected before anything looked at its entries. It must now load.
    private static int TestGraphHostLoadsOnHitOnlyGraphThatUsedToBeRejected()
    {
        Console.WriteLine("Test: GraphHost loads an OnHit-only graph with a non-entity/time/deltaTime PARAM (used to be rejected)");
        var text = @"
OCGRAPH 1
PARAM otherEntity int
NODE h OnHit
NODE p Param param=otherEntity
OUT p value
ENTRY h OnHit
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;
        if (!host.Ready)
        {
            Console.WriteLine("  FAIL: Load succeeded but Ready is false");
            return 1;
        }
        if (!host.IsEventDriven)
        {
            Console.WriteLine("  FAIL: IsEventDriven is false for a graph with an ENTRY record");
            return 1;
        }

        if (!host.Fire("OnHit", new object[] { 99 }, out var result))
        {
            Console.WriteLine("  FAIL: Fire('OnHit', ...) returned false for a graph that declares it");
            return 1;
        }
        if (result is not int echoed || echoed != 99)
        {
            Console.WriteLine($"  FAIL: expected the echoed otherEntity=99 back, got {result}");
            return 1;
        }

        Console.WriteLine("  PASS: loaded, Ready, IsEventDriven, and Fire() echoed the payload correctly");
        return 0;
    }

    // The PARAM contract with real computation, not just an echo: otherEntity (int) passes straight through, and damage
    // (float) is doubled by an upstream Multiply, so the shape (order, boxed types, length) and the substance are both checked.
    private static int TestFireInvokesOnHitAndReturnsComputedPayload()
    {
        Console.WriteLine("Test: Fire('OnHit', [otherEntity, damage]) runs real computation against caller-supplied payload");
        var text = @"
OCGRAPH 1
PARAM otherEntity int
PARAM damage float
NODE h OnHit
NODE po Param param=otherEntity
NODE pd Param param=damage
NODE two ConstFloat value=2.0
NODE mul Multiply
LINK pd.value mul.a
LINK two.value mul.b
OUT po value
OUT mul result
ENTRY h OnHit
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;

        if (!host.Fire("OnHit", new object[] { 42, 10.0f }, out var result))
        {
            Console.WriteLine("  FAIL: Fire('OnHit', ...) returned false");
            return 1;
        }
        if (result is not object[] outputs || outputs.Length != 2)
        {
            Console.WriteLine($"  FAIL: expected a 2-element object[] result, got {result}");
            return 1;
        }
        if (outputs[0] is not int otherEntity || otherEntity != 42)
        {
            Console.WriteLine($"  FAIL: expected otherEntity=42 passed through, got {outputs[0]}");
            return 1;
        }
        if (outputs[1] is not float doubledDamage || Math.Abs(doubledDamage - 20.0f) > 1e-4)
        {
            Console.WriteLine($"  FAIL: expected damage*2=20, got {outputs[1]}");
            return 1;
        }

        Console.WriteLine($"  PASS: Fire() returned [{outputs[0]}, {outputs[1]}] -- otherEntity echoed, damage genuinely doubled");
        return 0;
    }

    // Two payloads through the same compiled entry point must give two different, correct results, so Fire() is not
    // returning a cached value.
    private static int TestFireCanBeFiredRepeatedlyWithDifferentPayloads()
    {
        Console.WriteLine("Test: Fire() fired twice with different payloads returns two different, correct results");
        var text = @"
OCGRAPH 1
PARAM damage float
NODE h OnHit
NODE pd Param param=damage
NODE two ConstFloat value=2.0
NODE mul Multiply
LINK pd.value mul.a
LINK two.value mul.b
OUT mul result
ENTRY h OnHit
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;

        if (!host.Fire("OnHit", new object[] { 5.0f }, out var r1) || r1 is not float f1 || Math.Abs(f1 - 10.0f) > 1e-4)
        {
            Console.WriteLine($"  FAIL: first fire (damage=5) expected 10, got {r1}");
            return 1;
        }
        if (!host.Fire("OnHit", new object[] { 30.0f }, out var r2) || r2 is not float f2 || Math.Abs(f2 - 60.0f) > 1e-4)
        {
            Console.WriteLine($"  FAIL: second fire (damage=30) expected 60, got {r2}");
            return 1;
        }

        Console.WriteLine($"  PASS: fire(5)->{f1}, fire(30)->{f2}, both correct and distinct");
        return 0;
    }

    // An event named "OnSomethingElse" goes through the same load and Fire() path as "OnHit" and works identically. If
    // GraphHost special-cased the string "OnHit", this test (which never uses it) would fail.
    private static int TestFireWorksForAnyNonTickEventName()
    {
        Console.WriteLine("Test: an on-demand event named something other than 'OnHit' works identically (no hardcoded name)");
        // "OnSomethingElse" is not a node type this catalog knows, but an ENTRY may name a node of any type, so the NODE
        // line reuses OnStart's bare-trigger shape and the ENTRY record gives it the new event name.
        var text = @"
OCGRAPH 1
PARAM n int
NODE w OnStart
NODE p Param param=n
OUT p value
ENTRY w OnSomethingElse
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;
        if (!host.Fire("OnSomethingElse", new object[] { 7 }, out var result) || result is not int echoed || echoed != 7)
        {
            Console.WriteLine($"  FAIL: expected Fire('OnSomethingElse', [7]) to echo 7, got ok={result}");
            return 1;
        }
        // "OnHit" was never declared by this graph, so it must not fire: the two names are not aliases.
        if (host.Fire("OnHit", Array.Empty<object>(), out var shouldBeNull) || shouldBeNull != null)
        {
            Console.WriteLine("  FAIL: Fire('OnHit', ...) should be false/null -- this graph never declared it");
            return 1;
        }
        Console.WriteLine("  PASS: a non-'OnHit' on-demand event name fires correctly; 'OnHit' (undeclared here) correctly does not");
        return 0;
    }

    private static int TestFireReturnsFalseForUndeclaredEventName()
    {
        Console.WriteLine("Test: Fire() on an event name the graph never declared is a documented no-op (false, null)");
        var text = @"
OCGRAPH 1
NODE h OnHit
ENTRY h OnHit
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;
        bool fired = host.Fire("OnOverlap", Array.Empty<object>(), out var result);
        if (fired || result != null)
        {
            Console.WriteLine($"  FAIL: expected (false, null), got ({fired}, {result})");
            return 1;
        }
        Console.WriteLine("  PASS: Fire('OnOverlap', ...) returned (false, null) with no throw");
        return 0;
    }

    private static int TestFireBeforeLoadIsASafeNoOp()
    {
        Console.WriteLine("Test: Fire() before any successful Load() is a safe no-op, same as Tick()'s own contract");
        var host = NewHost();
        bool fired = host.Fire("OnHit", Array.Empty<object>(), out var result);
        if (fired || result != null)
        {
            Console.WriteLine($"  FAIL: expected (false, null) with no graph ever loaded, got ({fired}, {result})");
            return 1;
        }
        Console.WriteLine("  PASS: Fire() on a never-loaded GraphHost returned (false, null), did not throw");
        return 0;
    }

    // Fire()'s args are positional, matching the graph's PARAM list. A wrong count must fail loudly and specifically, not
    // with a TargetParameterCountException from deep inside Delegate.DynamicInvoke.
    private static int TestFireArgCountMismatchThrowsClearError()
    {
        Console.WriteLine("Test: Fire() with the wrong number of args throws a clear ArgumentException naming both counts");
        var text = @"
OCGRAPH 1
PARAM otherEntity int
PARAM damage float
NODE h OnHit
NODE po Param param=otherEntity
OUT po value
ENTRY h OnHit
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;

        try
        {
            host.Fire("OnHit", new object[] { 1 }, out _); // graph declares 2 PARAMs, only 1 supplied
            Console.WriteLine("  FAIL: expected an ArgumentException for a 1-arg call against a 2-PARAM graph");
            return 1;
        }
        catch (ArgumentException aex)
        {
            if (!aex.Message.Contains("2") || !aex.Message.Contains("1"))
            {
                Console.WriteLine($"  FAIL: expected the message to name both counts (2 declared, 1 supplied), got: {aex.Message}");
                return 1;
            }
            Console.WriteLine($"  PASS: {aex.Message}");
            return 0;
        }
    }

    // The real-call proof, with the same evidence bar as the InputKey, Raycast and Spawn tests. InputKey is used because its
    // native symbol is resolved at call time, so Fire() reaches it at invoke time rather than failing at compile time.
    private static int TestFireEmitsRealNativeCallNotAStub()
    {
        Console.WriteLine("Test: Fire() invokes IL that makes a genuine native call, not a stub (via InputKey inside the OnHit chain)");
        var text = @"
OCGRAPH 1
PARAM otherEntity int
NODE h OnHit
NODE p Param param=otherEntity
NODE ik InputKey
LINK p.value ik.key
OUT ik down
ENTRY h OnHit
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;

        try
        {
            bool fired = host.Fire("OnHit", new object[] { 5 }, out var unused);
            Console.WriteLine($"  FAIL: expected invoking this to throw (see the header comment) but Fire returned ({fired}, {unused})");
            return 1;
        }
        // Delegate.DynamicInvoke wraps the target's exception, so the EntryPointNotFoundException is the inner one.
        catch (System.Reflection.TargetInvocationException tiEx)
        {
            if (tiEx.InnerException is not EntryPointNotFoundException epEx || !epEx.Message.Contains("aver_fw_input_key"))
            {
                Console.WriteLine($"  FAIL: expected InnerException to be EntryPointNotFoundException naming aver_fw_input_key, got: {tiEx.InnerException?.GetType().FullName}: {tiEx.InnerException?.Message}");
                return 1;
            }
            Console.WriteLine($"  PASS: Fire() propagated a real call attempt for 'aver_fw_input_key' (not swallowed, exactly like Tick()'s own runtime-error contract): {tiEx.InnerException.Message}");
            return 0;
        }
    }

    // =================================================================================================
    // Interaction with Tick() / OnStart / OnTick
    // =================================================================================================

    private static int TestTickIsANoOpForAnOnHitOnlyGraph()
    {
        Console.WriteLine("Test: Tick() is a safe no-op for a graph with only an on-demand event (no OnStart/OnTick)");
        var text = @"
OCGRAPH 1
NODE h OnHit
ENTRY h OnHit
";
        bool sinkCalled = false;
        var host = new GraphHost(positionSink: (int e, float x, float y, float z) => sinkCalled = true);
        if (!LoadOk(host, text)) return 1;

        for (int i = 0; i < 5; i++)
            host.Tick(1, i * 0.1f);

        if (sinkCalled)
        {
            Console.WriteLine("  FAIL: PositionSink was invoked by Tick() on an on-demand-only graph");
            return 1;
        }
        Console.WriteLine("  PASS: 5x Tick() on an OnHit-only graph did not throw and did not call the sink");
        return 0;
    }

    // Regression: the ParamSlot-matching loop is gated on OnStart/OnTick, so an ordinary OnTick-only graph using
    // deltaTime must still load and tick.
    private static int TestOnTickOnlyGraphStillLoadsAndTicksAfterTheRefactor()
    {
        Console.WriteLine("Test: an ordinary OnTick-only graph (deltaTime) still loads and ticks after the on-demand refactor");
        var text = @"
OCGRAPH 1
PARAM deltaTime float
NODE tick OnTick
NODE pd Param param=deltaTime
OUT pd value
ENTRY tick OnTick
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;
        if (!host.Ready)
        {
            Console.WriteLine("  FAIL: Ready is false after a successful Load()");
            return 1;
        }
        for (int i = 0; i < 10; i++)
            host.Tick(1, 0.05f);
        Console.WriteLine("  PASS: loaded and ticked 10x without throwing");
        return 0;
    }

    // A graph may declare both OnStart and an on-demand event, provided the shared PARAM list stays within what Tick()
    // can supply (here, just entity). This shows the PARAM restriction below is about compatibility, not about the two
    // kinds of event being unable to share a file.
    private static int TestOnStartAndOnHitCanShareACompatibleParamList()
    {
        Console.WriteLine("Test: OnStart and OnHit sharing a Tick()-compatible PARAM list (entity only) both work from one file");
        var text = @"
OCGRAPH 1
PARAM entity int
NODE start OnStart
NODE h OnHit
NODE pe Param param=entity
ENTRY start OnStart
ENTRY h OnHit
OUT pe value
";
        var host = NewHost();
        if (!LoadOk(host, text)) return 1;

        // Tick() drives OnStart (entity 7 through PARAM entity) and Fire() drives OnHit (entity 3), against the same
        // compiled graph. Tick() used to throw here (see BuildArgs in GraphHost.cs for the int/float widening bug this
        // found), so "entity int" alone is kept as a regression check.
        host.Tick(7, 0f); // must not throw; OnStart's own single-int OUT is not position-shaped
        if (!host.Fire("OnHit", new object[] { 3 }, out var result) || result is not int echoed || echoed != 3)
        {
            Console.WriteLine($"  FAIL: expected Fire('OnHit', [3]) to echo 3, got {result}");
            return 1;
        }
        Console.WriteLine("  PASS: OnStart ticked without throwing, OnHit fired on demand and echoed its own entity arg");
        return 0;
    }

    // PARAM is graph-wide, shared by every ENTRY in the file, so a PARAM only OnHit needs (otherEntity) would also be asked
    // of OnTick's delegate, and Tick() has no value to supply for it. This must fail at Load(), naming the PARAM.
    private static int TestMixingOnTickAndOnHitWithAnIncompatibleParamFailsToLoad()
    {
        Console.WriteLine("Test: OnTick + OnHit sharing a PARAM list OnTick cannot supply (otherEntity) fails to load, naming it");
        var text = @"
OCGRAPH 1
PARAM entity int
PARAM time float
PARAM deltaTime float
PARAM otherEntity int
NODE tick OnTick
NODE h OnHit
ENTRY tick OnTick
ENTRY h OnHit
";
        var host = NewHost();
        if (host.LoadFromText(text, out var err))
        {
            Console.WriteLine("  FAIL: expected Load to fail (OnTick cannot supply 'otherEntity'), but it succeeded");
            return 1;
        }
        if (err == null || !err.Contains("otherEntity"))
        {
            Console.WriteLine($"  FAIL: expected the error to name 'otherEntity', got: {err}");
            return 1;
        }
        if (host.Ready)
        {
            Console.WriteLine("  FAIL: Ready is true after a failed Load()");
            return 1;
        }
        Console.WriteLine($"  PASS: {err}");
        return 0;
    }
}
