// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the ON-DEMAND event path -- a "was I just hit"-shaped gap named by a demo project's own
// spec: a third ENTRY-style event, fired by a HOST on demand rather than driven by GraphHost's own
// Tick() cadence, with a way for the payload (who hit me, how hard) to actually reach the graph. This
// engine file names no project -- see the standing architectural rule that the engine must not know
// any particular template exists. See GraphHost.cs's own PHASE 3 comment and Fire()'s doc comment for
// the design these tests prove, and OcGraphParser.cs's "onhit" case / GraphCompiler.cs's
// IsExecOnlyNodeType for the node-type side.
//
// "OnHit" IS THE WORKED EXAMPLE, NOT A SPECIAL CASE -- and this file's own tests are the proof of
// that claim, not just an assertion of it: TestFireWorksForAnyNonTickEventName below fires an event
// literally named "OnSomethingElse" through the exact same mechanism and gets the exact same
// behaviour, with zero code in GraphHost.cs naming it. Everywhere else in this file "OnHit" is used
// because it is the concrete event this slice exists to unblock, not because GraphHost treats it
// differently from any other name.
//
// HONEST SCOPE, same limitation every other node-vocabulary test file in this project already names:
// nothing in this bare test process boots a live Aver.Scene/Aver.Framework native host. Fire()'s
// "real native call" test (below) proves the emitted IL reaches for a genuine native symbol by
// observing the exact failure that call produces here -- the same evidence bar NewNodeTests.cs/
// SpawnNodeTests.cs already established for InputKey/Raycast/Spawn, ADJUSTED for one real difference:
// those tests invoke a directly-cast delegate (`fn(5)`), so the native EntryPointNotFoundException
// propagates raw; GraphHost.Fire() (like Tick()) invokes via Delegate.DynamicInvoke, which -- unlike
// a plain delegate call -- WRAPS whatever the target throws in a
// System.Reflection.TargetInvocationException (confirmed empirically while writing this test, not
// assumed: the first version of this test caught EntryPointNotFoundException directly and failed with
// "Exception has been thrown by the target of an invocation," which IS TargetInvocationException's own
// message). So this test unwraps one level via InnerException, which is exactly what GraphHost's own
// class-level doc comment already promises ("RUNTIME ERRORS ... are NOT swallowed ... propagates out
// of Tick() exactly as any other unhandled per-frame bug would") -- DynamicInvoke's wrapping is a
// detail of HOW it propagates, not a case of it being swallowed. GetField/SetField are NOT used for
// this proof in THIS file: GraphHost's own compiler always uses GraphCompiler.DefaultFieldResolver (no
// injectable FieldResolver on GraphHost's public surface, unlike a bare GraphCompiler in
// NewNodeTests.cs/Program.cs), and DefaultFieldResolver's OWN P/Invoke call would throw at COMPILE
// time -- caught by CompileEntryPoint's blanket try/catch and reported as a compile ERROR, not the
// invoke-time proof this file wants. InputKey has no such compile-time native call (its native symbol
// is resolved once, by reflection, at class load), so it is what makes the invoke-time proof possible
// here.
using System;
using Aver.Graph;

static class OnHitEventTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestOnHitDefaultPinsShape();
        failures += TestOnHitNodeSkippedByPullCompilerWhenGraphHasBothHalves();

        failures += TestGraphHostLoadsOnHitOnlyGraphThatUsedToBeRejected();
        failures += TestFireInvokesOnHitAndReturnsComputedPayload();
        failures += TestFireCanBeFiredRepeatedlyWithDifferentPayloads();
        failures += TestFireWorksForAnyNonTickEventName();
        failures += TestFireReturnsFalseForUndeclaredEventName();
        failures += TestFireBeforeLoadIsASafeNoOp();
        failures += TestFireArgCountMismatchThrowsClearError();
        failures += TestFireEmitsRealNativeCallNotAStub();

        failures += TestTickIsANoOpForAnOnHitOnlyGraph();
        failures += TestOnTickOnlyGraphStillLoadsAndTicksAfterTheRefactor();
        failures += TestOnStartAndOnHitCanShareACompatibleParamList();
        failures += TestMixingOnTickAndOnHitWithAnIncompatibleParamFailsToLoad();

        return failures;
    }

    // =================================================================================================
    // NODE-TYPE SHAPE (parser + PULL-compiler parity with OnStart/OnTick)
    // =================================================================================================

    private static int TestOnHitDefaultPinsShape()
    {
        Console.WriteLine("Test: OnHit's default pins are a single exec-out, no inputs (same shape as OnStart/OnTick)");
        try
        {
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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Proves the class-level "the two halves do not interact" promise still holds with OnHit in the
    // mix: a graph carrying BOTH a pure dataflow chain reaching OUT and an unrelated OnHit-typed node
    // (with its own ENTRY) still compiles and evaluates correctly through Compile() -- the PULL-only
    // compiler GraphHost never calls for an event-driven graph, but that a bare GraphCompiler caller
    // still might (exactly the scenario IsExecOnlyNodeType exists to keep safe for OnStart/OnTick
    // already; OnHit needed the identical treatment, not a new one).
    private static int TestOnHitNodeSkippedByPullCompilerWhenGraphHasBothHalves()
    {
        Console.WriteLine("Test: Compile() (PULL) skips an OnHit node and still computes the dataflow result correctly");
        try
        {
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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // GraphHost: load + Fire()
    // =================================================================================================

    // THE ACTUAL BUG BEING FIXED, PROVEN DIRECTLY: before this slice, LoadEventGraph applied the
    // entity/time/deltaTime ParamSlot check to EVERY event-driven graph unconditionally, so a graph
    // declaring ONLY an OnHit entry with `PARAM otherEntity int` was rejected before LoadEventGraph
    // ever noticed it had no OnStart/OnTick entry to drive via Tick() in the first place. This graph
    // is exactly that shape; it must now load.
    private static int TestGraphHostLoadsOnHitOnlyGraphThatUsedToBeRejected()
    {
        Console.WriteLine("Test: GraphHost loads an OnHit-only graph with a non-entity/time/deltaTime PARAM (used to be rejected)");
        try
        {
            var text = @"
OCGRAPH 1
PARAM otherEntity int
NODE h OnHit
NODE p Param param=otherEntity
OUT p value
ENTRY h OnHit
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE PARAM CONTRACT, PROVEN WITH REAL COMPUTATION, NOT JUST AN ECHO: two payload values --
    // otherEntity (int) and damage (float) -- reach the graph via Fire()'s positional args, and the
    // graph does real arithmetic on one of them (damage * 2) while passing the other straight
    // through, proving BOTH the shape (object[] of the right length, right boxed types, right order)
    // and the substance (an upstream Multiply genuinely ran against the caller-supplied value).
    private static int TestFireInvokesOnHitAndReturnsComputedPayload()
    {
        Console.WriteLine("Test: Fire('OnHit', [otherEntity, damage]) runs real computation against caller-supplied payload");
        try
        {
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
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Not a photograph: two different payloads through the SAME compiled entry point (compiled once,
    // per GraphHost's own load-once contract) must produce two different, independently-correct
    // results -- the same "prove it's not stuck/cached" bar GraphHostTests.cs's own drone tests set
    // for Tick(), applied here to Fire().
    private static int TestFireCanBeFiredRepeatedlyWithDifferentPayloads()
    {
        Console.WriteLine("Test: Fire() fired twice with different payloads returns two different, correct results");
        try
        {
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
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE GENERALITY CLAIM, PROVEN, NOT ASSERTED: an event named "OnSomethingElse" -- not "OnHit",
    // never mentioned anywhere in GraphHost.cs -- goes through the exact same LoadEventGraph/Fire()
    // machinery and works identically. If GraphHost secretly special-cased the string "OnHit"
    // anywhere, this test (which never uses that string) would fail to load or fail to fire.
    private static int TestFireWorksForAnyNonTickEventName()
    {
        Console.WriteLine("Test: an on-demand event named something other than 'OnHit' works identically (no hardcoded name)");
        try
        {
            // "OnSomethingElse" is not a node TYPE this catalog knows (only OnStart/OnTick/OnHit are),
            // but ENTRY's own contract (see OcGraphParser's comment, and Graph.Validate's) is that the
            // node an ENTRY names may be of ANY type -- so this reuses "OnStart"'s bare-trigger shape
            // for the NODE line and gives the ENTRY record the new event name instead. This is the
            // literal mechanism ENTRY's own parser comment promises: "adding a future event ... is one
            // more ENTRY record naming a different node, with NO format change."
            var text = @"
OCGRAPH 1
PARAM n int
NODE w OnStart
NODE p Param param=n
OUT p value
ENTRY w OnSomethingElse
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            if (!host.Fire("OnSomethingElse", new object[] { 7 }, out var result) || result is not int echoed || echoed != 7)
            {
                Console.WriteLine($"  FAIL: expected Fire('OnSomethingElse', [7]) to echo 7, got ok={result}");
                return 1;
            }
            // And "OnHit" -- which THIS graph never declared -- correctly does nothing here, proving
            // the two names are not aliases of one another.
            if (host.Fire("OnHit", Array.Empty<object>(), out var shouldBeNull) || shouldBeNull != null)
            {
                Console.WriteLine("  FAIL: Fire('OnHit', ...) should be false/null -- this graph never declared it");
                return 1;
            }
            Console.WriteLine("  PASS: a non-'OnHit' on-demand event name fires correctly; 'OnHit' (undeclared here) correctly does not");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestFireReturnsFalseForUndeclaredEventName()
    {
        Console.WriteLine("Test: Fire() on an event name the graph never declared is a documented no-op (false, null)");
        try
        {
            var text = @"
OCGRAPH 1
NODE h OnHit
ENTRY h OnHit
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            bool fired = host.Fire("OnOverlap", Array.Empty<object>(), out var result);
            if (fired || result != null)
            {
                Console.WriteLine($"  FAIL: expected (false, null), got ({fired}, {result})");
                return 1;
            }
            Console.WriteLine("  PASS: Fire('OnOverlap', ...) returned (false, null) with no throw");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestFireBeforeLoadIsASafeNoOp()
    {
        Console.WriteLine("Test: Fire() before any successful Load() is a safe no-op, same as Tick()'s own contract");
        try
        {
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            bool fired = host.Fire("OnHit", Array.Empty<object>(), out var result);
            if (fired || result != null)
            {
                Console.WriteLine($"  FAIL: expected (false, null) with no graph ever loaded, got ({fired}, {result})");
                return 1;
            }
            Console.WriteLine("  PASS: Fire() on a never-loaded GraphHost returned (false, null), did not throw");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex.Message}");
            return 1;
        }
    }

    // Fire()'s args are POSITIONAL, matching the graph's own PARAM list -- see Fire()'s own doc
    // comment for why this is the right contract instead of a second named-slot vocabulary. A wrong
    // arg count must fail loudly and specifically, not via a cryptic TargetParameterCountException
    // from deep inside Delegate.DynamicInvoke.
    private static int TestFireArgCountMismatchThrowsClearError()
    {
        Console.WriteLine("Test: Fire() with the wrong number of args throws a clear ArgumentException naming both counts");
        try
        {
            var text = @"
OCGRAPH 1
PARAM otherEntity int
PARAM damage float
NODE h OnHit
NODE po Param param=otherEntity
OUT po value
ENTRY h OnHit
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE REAL-CALL PROOF, same evidence bar as InputKey/Raycast/Spawn's own tests (see this file's
    // header comment for why InputKey, specifically, is what makes this provable through GraphHost's
    // real Load()->Fire() path rather than a bare GraphCompiler call with a fake FieldResolver).
    private static int TestFireEmitsRealNativeCallNotAStub()
    {
        Console.WriteLine("Test: Fire() invokes IL that makes a genuine native call, not a stub (via InputKey inside the OnHit chain)");
        try
        {
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
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            try
            {
                bool fired = host.Fire("OnHit", new object[] { 5 }, out var unused);
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but Fire returned ({fired}, {unused})");
                return 1;
            }
            // DynamicInvoke wraps the target's exception -- see this file's header comment for why
            // this is TargetInvocationException, not a raw EntryPointNotFoundException, unlike the
            // directly-cast-delegate pattern InputKey's OTHER tests (NewNodeTests.cs) use.
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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // Interaction with Tick() / OnStart / OnTick -- regression + the mixed-file cases
    // =================================================================================================

    private static int TestTickIsANoOpForAnOnHitOnlyGraph()
    {
        Console.WriteLine("Test: Tick() is a safe no-op for a graph with only an on-demand event (no OnStart/OnTick)");
        try
        {
            var text = @"
OCGRAPH 1
NODE h OnHit
ENTRY h OnHit
";
            bool sinkCalled = false;
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => sinkCalled = true);
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Regression: the ParamSlot-matching loop moved inside an `if (wantsStart || wantsTick)` gate as
    // part of this slice (previously unconditional) -- an ordinary OnTick-only graph using deltaTime
    // must still load and tick without throwing, exactly as before.
    private static int TestOnTickOnlyGraphStillLoadsAndTicksAfterTheRefactor()
    {
        Console.WriteLine("Test: an ordinary OnTick-only graph (deltaTime) still loads and ticks after the on-demand refactor");
        try
        {
            var text = @"
OCGRAPH 1
PARAM deltaTime float
NODE tick OnTick
NODE pd Param param=deltaTime
OUT pd value
ENTRY tick OnTick
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A graph MAY legitimately declare both OnStart and an on-demand event, PROVIDED the shared
    // PARAM list stays inside what Tick() can supply (here: just `entity`). Proves the mixing
    // restriction below is about PARAM compatibility specifically, not about the two kinds of event
    // being forbidden to coexist in one file.
    private static int TestOnStartAndOnHitCanShareACompatibleParamList()
    {
        Console.WriteLine("Test: OnStart and OnHit sharing a Tick()-compatible PARAM list (entity only) both work from one file");
        try
        {
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
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            // Tick() drives OnStart (entity id 7 passed through PARAM entity), Fire() drives OnHit
            // (entity id 3) -- independently, against the SAME compiled graph. Tick() used to throw
            // here (see BuildArgs' own comment, in GraphHost.cs, for the int/float switch-expression
            // widening bug this test found and this slice fixed) -- kept as a real regression check,
            // not simplified away, because "entity int" alone is exactly the shape that tripped it.
            host.Tick(7, 0f); // must not throw; OnStart's own single-int OUT is not position-shaped
            if (!host.Fire("OnHit", new object[] { 3 }, out var result) || result is not int echoed || echoed != 3)
            {
                Console.WriteLine($"  FAIL: expected Fire('OnHit', [3]) to echo 3, got {result}");
                return 1;
            }
            Console.WriteLine("  PASS: OnStart ticked without throwing, OnHit fired on demand and echoed its own entity arg");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE FOOTGUN THIS SLICE MUST STILL CATCH: PARAM is graph-WIDE (Graph.cs), shared by every ENTRY
    // in the file, so a PARAM only OnHit needs (otherEntity) becomes something OnTick's own compiled
    // delegate would ALSO be asked to accept -- and Tick() has no value to supply for it. This must
    // fail loudly at Load(), naming the offending PARAM, not silently compile something Tick() would
    // later choke on.
    private static int TestMixingOnTickAndOnHitWithAnIncompatibleParamFailsToLoad()
    {
        Console.WriteLine("Test: OnTick + OnHit sharing a PARAM list OnTick cannot supply (otherEntity) fails to load, naming it");
        try
        {
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
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
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
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
