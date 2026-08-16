// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for FireEvent -- GAP 3, cross-entity events: a graph can DECLARE an on-demand event (OnHit
// already proved that, see OnHitEventTests.cs) but until this node existed nothing could FIRE one on
// ANOTHER entity's graph. See GraphCompiler.cs's EmitExecFireEvent/IsExecCapableFireEventType
// comments, OcGraphParser.cs's "FireEvent" section, and Aver.Graph/GraphEvents.cs's own class comment
// for the design these tests prove.
//
// COMPILE-TIME SHAPE MIRRORS SPAWN/CHARACTERMOVE'S OWN TESTS: exec-only, refused by the PULL
// compiler's topological pass ENTIRELY (TestFireEventRefusedByPullCompilerEvenWithNoEntryAtAll), and
// refused when pulled as a bare data value with no exec visit (TestFireEventPulledWithoutExecVisit
// FailsClearly) -- this is the "refused on the pull path with its actual error text asserted" bullet
// the task names explicitly.
//
// UNLIKE EVERY PRIOR GraphInterop WRAPPER'S OWN TESTS, THE WHOLE MECHANISM IS REACHABLE FROM THIS BARE
// PROCESS WITH NO NATIVE CALL ANYWHERE. GraphEvents (the router FireEvent's IL calls) lives in THIS
// SAME ASSEMBLY (Aver.Graph), is plain C# with a PUBLIC static Router field (no reflection needed,
// unlike CharacterMoveNodeTests.cs's Actors.Resolver dance -- Aver.Framework only grants
// InternalsVisibleTo to Aver.Graph/Aver.Scripting.Bridge, but GraphEvents does not need that grant at
// all since it never leaves Aver.Graph). That means the reentrancy-guard tests below are GENUINE,
// end-to-end, VALUE-asserting proofs: real compiled IL, real GraphHost.Fire/FireForEntity calls, a
// real recursive call chain through GraphEvents.FireEventForGraph -- not a simulation of one.
//
// EVERY TEST THAT INSTALLS A Router SAVES AND RESTORES THE PREVIOUS VALUE IN A finally BLOCK -- this
// suite's own Program.cs runs every test suite sequentially in ONE process with no isolation, so a
// leaked router would corrupt whatever runs after it, exactly the discipline CharacterMoveNodeTests.cs
// already established for Actors.Resolver.
using System;
using System.Collections.Generic;
using Aver.Graph;

static class FireEventNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- compile-time shape ---------------------------------------------------------------
        failures += TestFireEventDefaultPinsShape();
        failures += TestFireEventRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestFireEventPulledWithoutExecVisitFailsClearly();
        failures += TestFireEventMissingEventAttributeFailsCompile();
        failures += TestFireEventWithNoFiredOutputPinStillCompiles();

        // ---- GraphEvents: the router seam ------------------------------------------------------
        failures += TestFireEventNoRouterInstalledFailsVisibly();
        failures += TestFireEventRouterReceivesTargetAndEventNameVerbatim();
        failures += TestFireEventFiredPinReflectsRouterOutcomeBothWays();

        // ---- GraphHost.FireForEntity: building the target's PARAM list --------------------------
        failures += TestFireForEntitySuppliesEntityTimeDeltaTimeByName();
        failures += TestFireForEntityRefusesUndeclaredEvent();
        failures += TestFireForEntityRefusesAnUnsupportedParamNameNamingIt();

        // ---- end-to-end cross-entity: two REAL GraphHosts, wired through a REAL router -----------
        failures += TestFireEventDrivesAnotherEntitysGraphEndToEnd();

        // ---- reentrancy: the exact arithmetic the depth guard promises, unit-level ---------------
        failures += TestDepthGuardRefusesExactlyAtMaxDepth();

        // ---- reentrancy: self-fire, mutual fire, and a fire-during-a-fire chain -- REAL compiled
        // graphs, REAL recursive Fire() calls, asserted on the FINAL VAR VALUE each scenario leaves
        // behind, not merely "it didn't crash".
        failures += TestSelfFireIsBoundedNotUnbounded();
        failures += TestMutualFireBetweenTwoGraphsIsBounded();
        failures += TestThreeWayFireCycleIsBounded();

        return failures;
    }

    // =================================================================================================
    // COMPILE-TIME SHAPE
    // =================================================================================================

    private static int TestFireEventDefaultPinsShape()
    {
        Console.WriteLine("Test: FireEvent's default pins are exec-in + target:int-in + then(exec-out) + fired(bool-out), event= parsed");
        try
        {
            var text = "OCGRAPH 1\nNODE fe FireEvent event=OnHit\nOUT fe fired\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["fe"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "target" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "fired" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 4;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.EventName != "OnHit")
            {
                Console.WriteLine($"  FAIL: expected EventName 'OnHit' from event= attribute, got '{node.EventName}'");
                return 1;
            }
            Console.WriteLine("  PASS: 4 pins (1 exec-in, 1 int-in, 1 exec-out, 1 bool-out), event= parsed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestFireEventRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: FireEvent in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, FireEvent-naming error");
        try
        {
            var text = @"
OCGRAPH 1
NODE tgt ConstInt value=1
NODE fe FireEvent event=OnHit
LINK tgt.value fe.target
OUT fe fired
";
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
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected Compile() to refuse a FireEvent node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("FireEvent", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the FireEvent node type, got: {compileErr}");
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

    // THE "refused on the pull path with its actual error text asserted" REQUIREMENT: even inside an
    // ENTRY-driven graph, a FireEvent node the exec chain never actually visits must not be readable
    // as a data value via OUT -- the exact same refusal SetField/Spawn/CharacterMove already share.
    private static int TestFireEventPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: FireEvent never wired into the exec chain, but pulled via OUT, fails clearly (not silently false)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE tgt ConstInt value=1
NODE fe FireEvent event=OnHit
LINK tgt.value fe.target
ENTRY start OnStart
OUT fe fired
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (fe.fired pulled with no exec visit ever reaching fe), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("FireEvent", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming FireEvent and its side effect, got: {compileErr}");
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

    private static int TestFireEventMissingEventAttributeFailsCompile()
    {
        Console.WriteLine("Test: FireEvent node with no event= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE fe FireEvent
LINK tick.exec fe.exec
ENTRY tick OnTick
OUT fe fired
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (FireEvent has no event= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("event="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing event= attribute, got: {compileErr}");
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

    // "fired" is OPTIONAL, exactly like Spawn's "entity" / CharacterMove's "success" -- a graph author
    // who only wants the side effect (fire it, don't care whether the target had a graph) can omit it,
    // and EmitExecFireEvent discards the outcome with a Pop.
    private static int TestFireEventWithNoFiredOutputPinStillCompiles()
    {
        Console.WriteLine("Test: FireEvent with no 'fired' output pin still compiles (Pop path)");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE fe FireEvent
PIN fe exec in exec
PIN fe target in int
PIN fe then out exec
LINK tick.exec fe.exec
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["fe"];
            node.EventName = "OnHit"; // no NODE-line attribute to parse in this hand-written PIN fixture
            if (node.Pins.Exists(p => p.Name == "fired"))
            {
                Console.WriteLine("  FAIL: test fixture is wrong -- 'fe' should have no 'fired' pin declared");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled == null)
            {
                Console.WriteLine($"  FAIL: expected compilation to succeed ('fired' output is optional), got: {compileErr}");
                return 1;
            }
            Console.WriteLine("  PASS: compiled with no 'fired' output pin declared (Pop-discard path)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // GraphEvents: the router seam
    // =================================================================================================

    private static string FireEventGraphText(int target, string eventName) => $@"
OCGRAPH 1
NODE tick OnTick
NODE tgt ConstInt value={target}
NODE fe FireEvent event={eventName}
LINK tick.exec fe.exec
LINK tgt.value fe.target
ENTRY tick OnTick
OUT fe fired
";

    private static int TestFireEventNoRouterInstalledFailsVisibly()
    {
        Console.WriteLine("Test: FireEvent with no GraphEvents.Router installed returns fired=false, logs why, never throws");
        var saved = GraphEvents.Router;
        try
        {
            GraphEvents.Router = null;

            if (!OcGraphParser.Parse(FireEventGraphText(777, "OnHit"), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            var originalErr = Console.Error;
            var captured = new System.IO.StringWriter();
            Console.SetError(captured);
            bool fired;
            try { fired = fn(); }
            finally { Console.SetError(originalErr); }

            if (fired)
            {
                Console.WriteLine("  FAIL: expected fired=false (no router installed), got true");
                return 1;
            }
            string logged = captured.ToString();
            if (!logged.Contains("no router installed"))
            {
                Console.WriteLine($"  FAIL: expected a log line saying 'no router installed', got: {logged}");
                return 1;
            }
            Console.WriteLine($"  PASS: fired=false; logged: {logged.Trim()}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    private static int TestFireEventRouterReceivesTargetAndEventNameVerbatim()
    {
        Console.WriteLine("Test: FireEvent passes the linked 'target' pin and the event= attribute to the router, unmodified");
        var saved = GraphEvents.Router;
        try
        {
            int? capturedEntity = null;
            string? capturedEvent = null;
            GraphEvents.Router = (entity, evt) => { capturedEntity = entity; capturedEvent = evt; return true; };

            if (!OcGraphParser.Parse(FireEventGraphText(4242, "OnSomethingSpecific"), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            bool fired = fn();
            if (!fired || capturedEntity != 4242 || capturedEvent != "OnSomethingSpecific")
            {
                Console.WriteLine($"  FAIL: expected (fired=true, entity=4242, event='OnSomethingSpecific'), got (fired={fired}, entity={capturedEntity}, event={capturedEvent})");
                return 1;
            }
            Console.WriteLine($"  PASS: router received (entity={capturedEntity}, event='{capturedEvent}') verbatim");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    private static int TestFireEventFiredPinReflectsRouterOutcomeBothWays()
    {
        Console.WriteLine("Test: the 'fired' pin is the router's REAL return value, both true and false, not a hardcoded stub");
        var saved = GraphEvents.Router;
        try
        {
            if (!OcGraphParser.Parse(FireEventGraphText(1, "OnHit"), out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            GraphEvents.Router = (_, _) => true;
            bool whenTrue = fn();
            GraphEvents.Router = (_, _) => false;
            bool whenFalse = fn();

            if (!whenTrue || whenFalse)
            {
                Console.WriteLine($"  FAIL: expected (true, false), got ({whenTrue}, {whenFalse})");
                return 1;
            }
            Console.WriteLine($"  PASS: fired={whenTrue} when router says true, fired={whenFalse} when router says false");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    // =================================================================================================
    // GraphHost.FireForEntity: building the target's PARAM list from the entity/time/deltaTime
    // vocabulary, never guessing
    // =================================================================================================

    private static int TestFireForEntitySuppliesEntityTimeDeltaTimeByName()
    {
        Console.WriteLine("Test: FireForEntity supplies entity/time/deltaTime by NAME, matching real values (entity=target, deltaTime=0)");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
PARAM deltaTime float
NODE h OnHit
NODE pe Param param=entity
NODE pd Param param=deltaTime
OUT pe value
OUT pd value
ENTRY h OnHit
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            if (!host.FireForEntity("OnHit", 909, out var result, out var refusal))
            {
                Console.WriteLine($"  FAIL: FireForEntity returned false: {refusal}");
                return 1;
            }
            if (result is not object[] outputs || outputs.Length != 2)
            {
                Console.WriteLine($"  FAIL: expected a 2-element object[] result, got {result}");
                return 1;
            }
            if (outputs[0] is not int entityEcho || entityEcho != 909)
            {
                Console.WriteLine($"  FAIL: expected entity=909 (the FIRE target, not the firer), got {outputs[0]}");
                return 1;
            }
            if (outputs[1] is not float dt || Math.Abs(dt - 0f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected deltaTime=0 (an event has no duration), got {outputs[1]}");
                return 1;
            }
            Console.WriteLine($"  PASS: entity={outputs[0]}, deltaTime={outputs[1]}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestFireForEntityRefusesUndeclaredEvent()
    {
        Console.WriteLine("Test: FireForEntity on an event name the graph never declared refuses cleanly, naming it");
        try
        {
            var text = "OCGRAPH 1\nNODE h OnHit\nENTRY h OnHit\n";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            bool ok = host.FireForEntity("OnOverlap", 1, out var result, out var refusal);
            if (ok || result != null)
            {
                Console.WriteLine($"  FAIL: expected (false, null), got ({ok}, {result})");
                return 1;
            }
            if (refusal == null || !refusal.Contains("OnOverlap"))
            {
                Console.WriteLine($"  FAIL: expected a refusal naming 'OnOverlap', got: {refusal}");
                return 1;
            }
            Console.WriteLine($"  PASS: {refusal}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE "NEVER GUESS" REQUIREMENT: a target graph declaring a PARAM outside {entity, time,
    // deltaTime} must be REFUSED, by name, not silently fed some plausible-looking value -- see
    // FireForEntity's own doc comment for why guessing is exactly the failure mode this method must
    // not have (GraphHost.Fire itself WIDENS a mismatched-kind argument rather than rejecting it).
    private static int TestFireForEntityRefusesAnUnsupportedParamNameNamingIt()
    {
        Console.WriteLine("Test: FireForEntity refuses a target PARAM outside {entity,time,deltaTime}, naming the offending PARAM");
        try
        {
            var text = "OCGRAPH 1\nPARAM amount float\nNODE h OnHit\nENTRY h OnHit\n";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            bool ok = host.FireForEntity("OnHit", 1, out var result, out var refusal);
            if (ok || result != null)
            {
                Console.WriteLine($"  FAIL: expected (false, null) -- 'amount' is not in the supported vocabulary, got ({ok}, {result})");
                return 1;
            }
            if (refusal == null || !refusal.Contains("amount"))
            {
                Console.WriteLine($"  FAIL: expected a refusal naming the offending PARAM 'amount', got: {refusal}");
                return 1;
            }
            Console.WriteLine($"  PASS: {refusal}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // END TO END: two REAL entities, two REAL GraphHosts, one REAL router -- the shape the acceptance
    // project's own "one entity reacts to another via FireEvent" scene actually uses.
    // =================================================================================================

    // A minimal stand-in for HostBridge's own FireEventRouter (see HostBridge.cs's own comment): a
    // plain entity->GraphHost dictionary. HostBridge additionally checks TWO tables and does warn-once
    // logging -- neither is reachable from this bare process (Aver.Graph.Tests does not, and must not,
    // reference Aver.Scripting.Bridge) -- but the interesting logic (arg building, the depth guard)
    // lives entirely in Aver.Graph and is fully exercised here, mirroring GraphClassRecordTests.cs's
    // own documented "registration lives in the bridge, unreachable from this process" split.
    private sealed class FakeEntityRouter
    {
        private readonly Dictionary<int, GraphHost> _hosts = new();
        public void Register(int entity, GraphHost host) => _hosts[entity] = host;
        public bool Route(int entity, string eventName) =>
            _hosts.TryGetValue(entity, out GraphHost? host) && host.FireForEntity(eventName, entity, out _, out _);
    }

    private static int TestFireEventDrivesAnotherEntitysGraphEndToEnd()
    {
        Console.WriteLine("Test: entity A's FireEvent node fires entity B's OnHit, B's own VAR updates -- real IL, real router, real value");
        var saved = GraphEvents.Router;
        try
        {
            const int entityA = 10, entityB = 20;

            // A: on OnTick, fires "OnHit" at B.
            var hostA = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!hostA.LoadFromText(FireEventGraphText(entityB, "OnHit"), out var errA))
            {
                Console.WriteLine($"  FAIL: hostA load: {errA}");
                return 1;
            }

            // B: an OnHit handler that bumps its own 'hits' VAR and a Query entry to read it back --
            // the same dual-entry pattern GraphVarTests.cs's own Bump/Query tests already establish.
            var textB = @"
OCGRAPH 1
VAR hits float 0
NODE h OnHit
NODE cur GetVar var=hits
NODE one ConstFloat value=1.0
NODE next Add
LINK cur.value next.a
LINK one.value next.b
NODE setHits SetVar var=hits
LINK next.result setHits.value
LINK h.exec setHits.exec
NODE q OnStart
NODE curQ GetVar var=hits
ENTRY h OnHit
ENTRY q Query
OUT curQ value
";
            var hostB = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!hostB.LoadFromText(textB, out var errB))
            {
                Console.WriteLine($"  FAIL: hostB load: {errB}");
                return 1;
            }

            var router = new FakeEntityRouter();
            router.Register(entityB, hostB);
            GraphEvents.Router = router.Route;

            hostA.Tick(entityA, 0.016f); // runs A's OnTick, whose FireEvent node fires B's OnHit

            if (!hostB.Fire("Query", Array.Empty<object>(), out var hitsAfter) ||
                hitsAfter is not float hf || Math.Abs(hf - 1f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected B's 'hits' VAR to read back 1 after one FireEvent from A, got {hitsAfter}");
                return 1;
            }

            // A second tick fires it again -- B's own state is genuinely accumulating, not a
            // one-shot fluke.
            hostA.Tick(entityA, 0.016f);
            if (!hostB.Fire("Query", Array.Empty<object>(), out var hitsAfter2) ||
                hitsAfter2 is not float hf2 || Math.Abs(hf2 - 2f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected B's 'hits' VAR to read back 2 after a second FireEvent from A, got {hitsAfter2}");
                return 1;
            }

            Console.WriteLine($"  PASS: B's 'hits' VAR reached {hf} then {hf2} -- entity A genuinely drove entity B's own graph state");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    // =================================================================================================
    // REENTRANCY: the depth guard's exact arithmetic, isolated from GraphHost -- see GraphEvents.cs's
    // own s_depth comment for the three named shapes (self-fire, mutual fire, a fire-during-a-fire
    // chain) and why ONE counter, not per-pair tracking, is the chosen guard.
    // =================================================================================================

    // A hand-rolled, UNCONDITIONALLY recursive router -- no GraphHost, no compiled IL, just
    // GraphEvents.FireEventForGraph calling itself through the router as many times as the guard will
    // allow. Isolates the counting arithmetic itself (independent of anything GraphHost/Fire might add
    // or subtly change) before the real-compiled-graph tests below layer it back on top.
    private static int TestDepthGuardRefusesExactlyAtMaxDepth()
    {
        Console.WriteLine($"Test: the depth guard allows exactly {GraphEvents.MaxDepth} nested FireEvent calls, refuses the next, and returns cleanly (no stack overflow)");
        var saved = GraphEvents.Router;
        try
        {
            int invocations = 0;
            bool sawRefusal = false;
            Func<int, string, bool>? recurse = null;
            recurse = (entity, evt) =>
            {
                invocations++;
                bool inner = GraphEvents.FireEventForGraph(entity, evt); // recurse unconditionally
                if (!inner) sawRefusal = true;
                return true;
            };
            GraphEvents.Router = recurse;

            bool topLevel = GraphEvents.FireEventForGraph(1, "OnHit");

            if (!topLevel)
            {
                Console.WriteLine("  FAIL: expected the TOP-level call to still return true (it succeeded; only the innermost nested attempt was refused)");
                return 1;
            }
            if (!sawRefusal)
            {
                Console.WriteLine("  FAIL: expected the guard to have refused at least one nested call, but every recursive call reported success");
                return 1;
            }
            if (invocations != GraphEvents.MaxDepth)
            {
                Console.WriteLine($"  FAIL: expected exactly {GraphEvents.MaxDepth} nested router invocations (one per allowed depth level), got {invocations}");
                return 1;
            }
            Console.WriteLine($"  PASS: exactly {invocations} nested invocations ran, the guard refused the {invocations + 1}th, top-level call returned true, process did not crash");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw (this is exactly what the guard must prevent): {ex.GetType().Name}: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    // SELF-FIRE: a graph whose own OnHit handler fires "OnHit" at ITS OWN entity, unconditionally,
    // sharing one GraphHost (and therefore one GraphVarStore) across every nested call. Each
    // invocation increments its own 'count' VAR by exactly 1 before attempting to recurse, so the
    // FINAL value is a precise, independently-derivable proof of exactly how many times the handler
    // ran: 1 (the test's own top-level kick, which does NOT go through GraphEvents at all) + MaxDepth
    // (every nested attempt the guard allows) = MaxDepth + 1.
    private static int TestSelfFireIsBoundedNotUnbounded()
    {
        Console.WriteLine("Test: a graph firing its OWN event at itself is bounded by the depth guard, not unbounded recursion");
        var saved = GraphEvents.Router;
        try
        {
            const int self = 9001;
            var text = FireEventCounterGraph(self, "OnHit");
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            var router = new FakeEntityRouter();
            router.Register(self, host);
            GraphEvents.Router = router.Route;

            if (!host.Fire("OnHit", Array.Empty<object>(), out _))
            {
                Console.WriteLine("  FAIL: the top-level Fire('OnHit') call itself returned false");
                return 1;
            }

            int expected = GraphEvents.MaxDepth + 1;
            if (!host.Fire("Query", Array.Empty<object>(), out var final) ||
                final is not float ff || Math.Abs(ff - expected) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected 'count' to read back exactly {expected} (1 top-level + {GraphEvents.MaxDepth} nested), got {final}");
                return 1;
            }
            Console.WriteLine($"  PASS: self-fire ran exactly {ff} times total, then the guard cut it off -- no stack overflow, no silent drop");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    // MUTUAL FIRE: two DIFFERENT graphs, A firing B's "OnHit" and B firing A's "OnHit" right back,
    // unconditionally -- the shape that recurses through TWO GraphHosts and TWO GraphVarStores rather
    // than one. Same MaxDepth+1 TOTAL invocations as the self-fire case (the guard counts total nested
    // depth, not per-graph), but split alternately: A gets the top-level kick plus every EVEN-numbered
    // nested call, B gets every ODD-numbered one.
    private static int TestMutualFireBetweenTwoGraphsIsBounded()
    {
        Console.WriteLine("Test: two graphs firing each other's event back and forth is bounded, and the split between them is exact");
        var saved = GraphEvents.Router;
        try
        {
            const int entityA = 111, entityB = 222;
            var hostA = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            var hostB = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!hostA.LoadFromText(FireEventCounterGraph(entityB, "OnHit"), out var errA))
            {
                Console.WriteLine($"  FAIL: hostA load: {errA}");
                return 1;
            }
            if (!hostB.LoadFromText(FireEventCounterGraph(entityA, "OnHit"), out var errB))
            {
                Console.WriteLine($"  FAIL: hostB load: {errB}");
                return 1;
            }

            var router = new FakeEntityRouter();
            router.Register(entityA, hostA);
            router.Register(entityB, hostB);
            GraphEvents.Router = router.Route;

            if (!hostA.Fire("OnHit", Array.Empty<object>(), out _))
            {
                Console.WriteLine("  FAIL: the top-level Fire('OnHit') call on hostA returned false");
                return 1;
            }

            hostA.Fire("Query", Array.Empty<object>(), out var finalA);
            hostB.Fire("Query", Array.Empty<object>(), out var finalB);

            // Derived the same way the depth-guard test above derives its own expectation: MaxDepth+1
            // total invocations across BOTH hosts, alternating starting with A (the top-level kick).
            int total = GraphEvents.MaxDepth + 1;
            int expectedA = (total + 1) / 2; // A gets the kick (#1) plus every other one after it
            int expectedB = total / 2;

            if (finalA is not float fa || Math.Abs(fa - expectedA) > 1e-6 ||
                finalB is not float fb || Math.Abs(fb - expectedB) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected (A={expectedA}, B={expectedB}) out of {total} total invocations, got (A={finalA}, B={finalB})");
                return 1;
            }
            Console.WriteLine($"  PASS: A ran {fa} times, B ran {fb} times, {fa + fb} total -- matches the guard's own budget exactly, no stack overflow");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    // A FIRE DURING AN OnHit THAT FIRES AGAIN, THREE DEEP: A fires B, B fires C, C fires A, repeating
    // -- the general "chain of graphs firing each other" shape the mutual-fire (2-cycle) test above is
    // only the simplest case of. MaxDepth (8) happens to divide evenly by 3 hosts... actually it does
    // not (9 total invocations / 3 = 3 exactly, since MaxDepth+1 = 9) -- chosen because 9 is exactly
    // divisible by 3, giving each of the three hosts an identical, easily-asserted final count rather
    // than an off-by-one split like the 2-host case needed.
    private static int TestThreeWayFireCycleIsBounded()
    {
        Console.WriteLine("Test: a 3-graph fire cycle (A->B->C->A->...) is bounded, and the guard divides the budget evenly");
        var saved = GraphEvents.Router;
        try
        {
            const int entityA = 1001, entityB = 1002, entityC = 1003;
            var hostA = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            var hostB = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            var hostC = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            // `|`, not `||`, DELIBERATELY: every LoadFromText call must run regardless of an earlier
            // one failing, so errA/errB/errC are always assigned by the time the failure message
            // below reads all three (a short-circuiting `||` would leave errB/errC unassigned if
            // hostA's own load already failed).
            bool loadedA = hostA.LoadFromText(FireEventCounterGraph(entityB, "OnHit"), out var errA);
            bool loadedB = hostB.LoadFromText(FireEventCounterGraph(entityC, "OnHit"), out var errB);
            bool loadedC = hostC.LoadFromText(FireEventCounterGraph(entityA, "OnHit"), out var errC);
            if (!loadedA || !loadedB || !loadedC)
            {
                Console.WriteLine($"  FAIL: load error(s): A={errA} B={errB} C={errC}");
                return 1;
            }

            var router = new FakeEntityRouter();
            router.Register(entityA, hostA);
            router.Register(entityB, hostB);
            router.Register(entityC, hostC);
            GraphEvents.Router = router.Route;

            if (!hostA.Fire("OnHit", Array.Empty<object>(), out _))
            {
                Console.WriteLine("  FAIL: the top-level Fire('OnHit') call on hostA returned false");
                return 1;
            }

            hostA.Fire("Query", Array.Empty<object>(), out var fa);
            hostB.Fire("Query", Array.Empty<object>(), out var fb);
            hostC.Fire("Query", Array.Empty<object>(), out var fc);

            int total = GraphEvents.MaxDepth + 1; // 9 -- exactly 3 per host
            float expectedEach = total / 3f;
            bool ok = fa is float va && Math.Abs(va - expectedEach) < 1e-6 &&
                      fb is float vb && Math.Abs(vb - expectedEach) < 1e-6 &&
                      fc is float vc && Math.Abs(vc - expectedEach) < 1e-6;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: expected each of A/B/C to have run exactly {expectedEach} times, got (A={fa}, B={fb}, C={fc})");
                return 1;
            }
            Console.WriteLine($"  PASS: A={fa}, B={fb}, C={fc} -- {(float)fa! + (float)fb! + (float)fc!} total, evenly split across the 3-cycle, no stack overflow");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            GraphEvents.Router = saved;
        }
    }

    // A graph that, on "OnHit", increments its own 'count' VAR by 1 and then unconditionally fires
    // <eventName> at <target> -- the shared building block every reentrancy test above uses. "Query"
    // reads 'count' back without touching it, the same dual-ENTRY pattern
    // GraphVarTests.TestGraphHostVariablesResetToDeclaredDefaultsOnReload already established.
    private static string FireEventCounterGraph(int target, string eventName) => $@"
OCGRAPH 1
VAR count float 0
NODE h OnHit
NODE cur GetVar var=count
NODE one ConstFloat value=1.0
NODE next Add
LINK cur.value next.a
LINK one.value next.b
NODE setCount SetVar var=count
LINK next.result setCount.value
LINK h.exec setCount.exec
NODE tgt ConstInt value={target}
NODE fe FireEvent event={eventName}
LINK tgt.value fe.target
LINK setCount.then fe.exec
NODE q OnStart
NODE curQ GetVar var=count
ENTRY h OnHit
ENTRY q Query
OUT curQ value
";
}
