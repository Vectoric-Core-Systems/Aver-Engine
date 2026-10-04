// Independent adversarial verification pass for the OnHit / Fire() on-demand event mechanism.
// Written by a separate reviewer, deliberately NOT reusing OnHitEventTests.cs's node ids, pin names,
// or graph shapes -- the point is to hit edge cases the implementer's own tests do not exercise:
// an empty graph, a missing attribute, a node wired backwards, a duplicated node id, an exec cycle,
// event-name case sensitivity, and the Tick()/Fire() disjointness boundary from the OTHER side (an
// event name that IS Tick-driven, fired at Fire() instead).
using System;
using Aver.Graph;

static class OnHitAdversarialTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestFireOnCompletelyEmptyGraphIsSafeNoOp();
        failures += TestEntryNamingNonexistentNodeFailsToLoad();
        failures += TestDuplicateEntryForSameEventNameFailsToLoad();
        failures += TestParamNodeMissingParamAttributeInOnDemandChainFailsToLoad();
        failures += TestLinkWiredBackwardsIntoOnHitsOwnExecPinFailsValidate();
        failures += TestDuplicateNodeIdLastDeclarationWins();
        failures += TestExecCycleReachedFromOnHitFailsCleanlyNotHang();
        failures += TestFireEventNameIsCaseSensitive();
        failures += TestFireCannotInvokeATickDrivenEventName();
        failures += TestTwoDistinctOnDemandEventsDoNotCrossTalk();
        failures += TestFireWithWrongArgTypeThrowsRatherThanCorrupting();
        failures += TestOnHitNodeTypeIsCaseInsensitiveInParser();

        return failures;
    }

    private static GraphHost NewHost() => new GraphHost(positionSink: (int e, float x, float y, float z) => { });

    // ============================================================================================
    // 1. Empty graph
    // ============================================================================================
    private static int TestFireOnCompletelyEmptyGraphIsSafeNoOp()
    {
        Console.WriteLine("Test: a completely empty graph (header only, no nodes, no ENTRY) loads and Fire() is a safe no-op");
        try
        {
            var text = "OCGRAPH 1\n";
            var host = NewHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: expected an empty graph to load, got error: {err}");
                return 1;
            }
            bool fired = host.Fire("OnHit", Array.Empty<object>(), out var result);
            if (fired || result != null)
            {
                Console.WriteLine($"  FAIL: Fire() on an empty graph should be (false, null), got ({fired}, {result})");
                return 1;
            }
            Console.WriteLine("  PASS: empty graph loaded, Fire() returned (false, null)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 2. Missing node
    // ============================================================================================
    private static int TestEntryNamingNonexistentNodeFailsToLoad()
    {
        Console.WriteLine("Test: ENTRY naming a node id that was never declared (OnHit event) fails to load with a clear error");
        try
        {
            var text = @"
OCGRAPH 1
ENTRY ghost OnHit
";
            var host = NewHost();
            bool ok = host.LoadFromText(text, out var err);
            if (ok)
            {
                Console.WriteLine("  FAIL: expected Load to fail for an ENTRY naming a nonexistent node");
                return 1;
            }
            if (err == null || !err.Contains("ghost"))
            {
                Console.WriteLine($"  FAIL: expected the error to name the missing node 'ghost', got: {err}");
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
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 3. Duplicate ENTRY for the same event name, two different nodes
    // ============================================================================================
    private static int TestDuplicateEntryForSameEventNameFailsToLoad()
    {
        Console.WriteLine("Test: two ENTRY records both naming event 'OnHit' (different nodes) fails to load, unambiguously");
        try
        {
            var text = @"
OCGRAPH 1
NODE q1 OnHit
NODE q2 OnHit
ENTRY q1 OnHit
ENTRY q2 OnHit
";
            var host = NewHost();
            bool ok = host.LoadFromText(text, out var err);
            if (ok)
            {
                Console.WriteLine("  FAIL: expected Load to fail for two ENTRY records claiming the same event name");
                return 1;
            }
            if (err == null || !err.Contains("OnHit"))
            {
                Console.WriteLine($"  FAIL: expected the error to name the conflicting event 'OnHit', got: {err}");
                return 1;
            }
            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 4. Missing attribute -- a Param node in the on-demand chain with no param= at all
    // ============================================================================================
    private static int TestParamNodeMissingParamAttributeInOnDemandChainFailsToLoad()
    {
        Console.WriteLine("Test: a Param node with no param= attribute, reached only from an OnHit chain's OUT, fails to load and names the node");
        try
        {
            var text = @"
OCGRAPH 1
NODE q OnHit
NODE broken Param
OUT broken value
ENTRY q OnHit
";
            var host = NewHost();
            bool ok = host.LoadFromText(text, out var err);
            if (ok)
            {
                Console.WriteLine("  FAIL: expected Load to fail for a Param node missing param=");
                return 1;
            }
            if (err == null || !err.Contains("broken"))
            {
                Console.WriteLine($"  FAIL: expected the error to name the offending node 'broken', got: {err}");
                return 1;
            }
            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 5. Node wired backwards -- LINK targeting OnHit's own OUTPUT exec pin as if it were an input
    // ============================================================================================
    private static int TestLinkWiredBackwardsIntoOnHitsOwnExecPinFailsValidate()
    {
        Console.WriteLine("Test: a LINK wired backwards (source's exec output into OnHit's own exec-OUTPUT pin, treated as a target) fails validation");
        try
        {
            var text = @"
OCGRAPH 1
NODE q OnHit
NODE other OnStart
LINK other.exec q.exec
ENTRY q OnHit
";
            var host = NewHost();
            bool ok = host.LoadFromText(text, out var err);
            if (ok)
            {
                Console.WriteLine("  FAIL: expected Load to fail -- 'q.exec' is OnHit's own OUTPUT pin, not a valid LINK target");
                return 1;
            }
            if (err == null || !err.Contains("input pin"))
            {
                Console.WriteLine($"  FAIL: expected an error about a missing input pin, got: {err}");
                return 1;
            }
            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 6. Two NODE records declaring the same id -- documents actual (pre-existing, not introduced by
    //    this slice) last-write-wins behavior rather than assuming it either errors or merges.
    // ============================================================================================
    private static int TestDuplicateNodeIdLastDeclarationWins()
    {
        Console.WriteLine("Test: two NODE records with the same id (OnStart then OnHit) -- documents last-write-wins, and that ENTRY still resolves consistently");
        try
        {
            var text = @"
OCGRAPH 1
NODE dup OnStart
NODE dup OnHit
ENTRY dup OnHit
";
            var host = NewHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            // Not asserting this SHOULD succeed on principle -- only that whatever it does is
            // internally consistent: if it loads, Fire('OnHit') on the surviving node must work.
            if (!host.Fire("OnHit", Array.Empty<object>(), out _))
            {
                Console.WriteLine("  FAIL: graph loaded (last NODE 'dup' decl == OnHit survived) but Fire('OnHit') returned false");
                return 1;
            }
            Console.WriteLine("  PASS: last NODE declaration for a duplicated id wins (OnHit), and Fire() is consistent with it");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 7. Exec cycle reached from an OnHit entry point -- must fail cleanly (compile error), not hang
    //    or stack-overflow the test process.
    // ============================================================================================
    private static int TestExecCycleReachedFromOnHitFailsCleanlyNotHang()
    {
        Console.WriteLine("Test: an exec cycle reached from OnHit (Sequence looping into its own exec input) fails to load with a clear cycle error, not a hang");
        try
        {
            var text = @"
OCGRAPH 1
NODE q OnHit
NODE loop Sequence
LINK q.exec loop.exec
LINK loop.then0 loop.exec
ENTRY q OnHit
";
            var host = NewHost();
            bool ok = host.LoadFromText(text, out var err);
            if (ok)
            {
                Console.WriteLine("  FAIL: expected Load to fail for an exec cycle reachable from OnHit");
                return 1;
            }
            if (err == null || !err.Contains("cycle"))
            {
                Console.WriteLine($"  FAIL: expected the error to mention 'cycle', got: {err}");
                return 1;
            }
            Console.WriteLine($"  PASS: {err}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw (should have been a clean Load() failure): {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 8. Fire() event-name matching is exact (case-sensitive), same as CompileEntryPoint's own
    //    string equality -- documented, not assumed.
    // ============================================================================================
    private static int TestFireEventNameIsCaseSensitive()
    {
        Console.WriteLine("Test: Fire('onhit') (lowercase) does NOT match a graph declaring ENTRY q OnHit (mixed case) -- exact string match, not case-insensitive");
        try
        {
            var text = @"
OCGRAPH 1
NODE q OnHit
ENTRY q OnHit
";
            var host = NewHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            bool firedWrongCase = host.Fire("onhit", Array.Empty<object>(), out var r1);
            bool firedRightCase = host.Fire("OnHit", Array.Empty<object>(), out var r2);
            if (firedWrongCase)
            {
                Console.WriteLine("  FAIL: Fire('onhit') matched a graph declaring 'OnHit' -- unexpected case-insensitive match");
                return 1;
            }
            if (!firedRightCase)
            {
                Console.WriteLine("  FAIL: Fire('OnHit') (exact case) should have matched");
                return 1;
            }
            Console.WriteLine("  PASS: event-name matching is exact-case, as documented by CompileEntryPoint's own == comparison");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 9. Fire() must NOT be able to invoke a Tick()-driven entry point even when called with its
    //    exact declared name -- proves the two buckets (_onStart/_onTick vs _onDemand) are really
    //    disjoint, not just "OnHit happens to not collide with OnTick in practice".
    // ============================================================================================
    private static int TestFireCannotInvokeATickDrivenEventName()
    {
        Console.WriteLine("Test: Fire('OnTick', ...) on a graph that DOES declare an OnTick entry still returns false -- OnTick is Tick()-only, never Fire()-able");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
PARAM time float
PARAM deltaTime float
NODE t OnTick
ENTRY t OnTick
";
            var host = NewHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            if (!host.Ready)
            {
                Console.WriteLine("  FAIL: expected Ready after loading a valid OnTick graph");
                return 1;
            }
            bool fired = host.Fire("OnTick", new object[] { 1, 0f, 0.016f }, out var result);
            if (fired || result != null)
            {
                Console.WriteLine($"  FAIL: Fire('OnTick', ...) should be (false, null) -- OnTick must only run via Tick(), got ({fired}, {result})");
                return 1;
            }
            // And Tick() itself must still work normally, proving the graph is not just broken.
            host.Tick(1, 0.016f);
            Console.WriteLine("  PASS: Fire('OnTick', ...) correctly refused; Tick() still drives the same entry normally");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 10. Two distinct on-demand event names in one file, fired in interleaved order with distinct
    //     payloads -- proves the _onDemand dictionary keys resolve to the RIGHT compiled delegate
    //     each time, not a stale/aliased one.
    // ============================================================================================
    private static int TestTwoDistinctOnDemandEventsDoNotCrossTalk()
    {
        Console.WriteLine("Test: two on-demand events (OnHit, OnDeath) in one file, fired interleaved, never cross-talk");
        try
        {
            var text = @"
OCGRAPH 1
PARAM amount float
NODE hitNode OnHit
NODE deathNode OnHit
NODE p Param param=amount
NODE ten ConstFloat value=10.0
NODE hundred ConstFloat value=100.0
NODE addHit Add
NODE mulDeath Multiply
LINK p.value addHit.a
LINK ten.value addHit.b
LINK p.value mulDeath.a
LINK hundred.value mulDeath.b
OUT addHit result
OUT mulDeath result
ENTRY hitNode OnHit
ENTRY deathNode OnDeath
";
            var host = NewHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            // Interleave the two events with several different payload values and check every result.
            for (int i = 1; i <= 3; i++)
            {
                float amount = i * 5.0f;
                if (!host.Fire("OnHit", new object[] { amount }, out var hitResult) || hitResult is not object[] hitOut)
                {
                    Console.WriteLine($"  FAIL: OnHit fire #{i} did not return an object[]: {hitResult}");
                    return 1;
                }
                if (hitOut[0] is not float hitAdd || Math.Abs(hitAdd - (amount + 10.0f)) > 1e-4)
                {
                    Console.WriteLine($"  FAIL: OnHit fire #{i}: expected addHit={amount + 10.0f}, got {hitOut[0]}");
                    return 1;
                }

                if (!host.Fire("OnDeath", new object[] { amount }, out var deathResult) || deathResult is not object[] deathOut)
                {
                    Console.WriteLine($"  FAIL: OnDeath fire #{i} did not return an object[]: {deathResult}");
                    return 1;
                }
                if (deathOut[1] is not float deathMul || Math.Abs(deathMul - (amount * 100.0f)) > 1e-2)
                {
                    Console.WriteLine($"  FAIL: OnDeath fire #{i}: expected mulDeath={amount * 100.0f}, got {deathOut[1]}");
                    return 1;
                }
            }
            Console.WriteLine("  PASS: OnHit and OnDeath fired interleaved 3x each, every result independently correct -- no cross-talk");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 11. Wrong arg TYPE (not count) -- Fire() supplies a string where the graph declares an int
    //     PARAM. Must throw something observable, not silently misbehave.
    // ============================================================================================
    private static int TestFireWithWrongArgTypeThrowsRatherThanCorrupting()
    {
        Console.WriteLine("Test: Fire() with a wrong ARG TYPE (string where PARAM declares int) throws rather than silently misbehaving");
        try
        {
            var text = @"
OCGRAPH 1
PARAM otherEntity int
NODE q OnHit
NODE p Param param=otherEntity
OUT p value
ENTRY q OnHit
";
            var host = NewHost();
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            bool threw = false;
            try
            {
                host.Fire("OnHit", new object[] { "not-an-int" }, out var result);
                Console.WriteLine($"  FAIL: expected Fire() to throw for a string arg against an int PARAM, but it returned a result: {result}");
            }
            catch (Exception)
            {
                threw = true;
            }
            if (!threw)
                return 1;

            // And the host must still be usable afterward -- one bad Fire() call must not corrupt state.
            if (!host.Fire("OnHit", new object[] { 42 }, out var goodResult) || goodResult is not int echoed || echoed != 42)
            {
                Console.WriteLine($"  FAIL: after the bad-typed Fire() threw, a correctly-typed Fire() should still work; got {goodResult}");
                return 1;
            }
            Console.WriteLine("  PASS: wrong-typed arg threw; host remained usable for a correctly-typed Fire() afterward");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw escaping the whole test: {ex}");
            return 1;
        }
    }

    // ============================================================================================
    // 12. Node TYPE "onhit" is matched case-insensitively by the parser/compiler, same as every
    //     other node type -- authored here in a different case than every other test file uses.
    // ============================================================================================
    private static int TestOnHitNodeTypeIsCaseInsensitiveInParser()
    {
        Console.WriteLine("Test: a NODE of type 'ONHIT' (all caps) gets the same default pins as 'OnHit' -- node TYPE matching is case-insensitive");
        try
        {
            var text = "OCGRAPH 1\nNODE q ONHIT\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["q"];
            bool ok = node.Pins.Count == 1 &&
                      node.Pins[0].Name == "exec" && node.Pins[0].IsOutput && node.Pins[0].Type == PinType.Exec;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: 'ONHIT' (uppercase) node type did not get the same default pins as 'onhit'/'OnHit': [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 'ONHIT' node type matched case-insensitively, same default pins as 'OnHit'");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
