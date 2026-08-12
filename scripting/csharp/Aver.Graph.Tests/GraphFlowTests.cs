// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for the exec/flow addition to Aver.Graph: branch, sequence, while, forEach, and event entry
// points (ENTRY/CompileEntryPoint). See GraphCompiler.cs's own "PUSH VS PULL" comment for the design
// these tests are proving, and OcGraphParser.cs's "flow / exec nodes" section for the pin shapes.
//
// OBSERVABILITY, AND WHY THESE GRAPHS LOOK THE WAY THEY DO. Nothing in this test process boots a live
// native scene (see GraphHostTests.cs's own class comment for the identical, pre-existing limitation
// this file inherits), so a graph cannot prove itself by writing a scene field and reading it back.
// Every test below instead reads OUT records after an exec chain finishes -- CompileEntryPoint()
// supports OUT exactly like Compile() does, sourced from whatever the exec walk left behind (a loop's
// "iterations"/"index", a branch's "tookTrue", a Sequence's "fireLog") as well as ordinary pure data
// nodes. That is real observability, not a test-only backdoor: those pins exist in the node catalog
// for any graph author to read, not just for this file.

using System;
using System.Collections.Generic;
using System.IO;
using Aver.Graph;

static class GraphFlowTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestExecRecordsRoundTripThroughTheParser();
        failures += TestBranchTakesTrueSide();
        failures += TestBranchTakesFalseSide();
        failures += TestSequenceFiresArmsInOrder();
        failures += TestWhileLoopCountsAndTerminatesNormally();
        failures += TestWhileLoopTerminatesAtTheGuard();
        failures += TestForEachRepeatsCountTimes();
        failures += TestForEachWithZeroCountRunsNoIterations();
        failures += TestOnTickEntryPointCompilesAndRuns();
        failures += TestNoEntryForRequestedEventFailsClearly();
        failures += TestOldGraphWithoutExecStillCompilesTheOldWay();
        failures += TestExecTypedParamIsRejectedAtParseTime();
        failures += TestOutReferencingAnExecPinIsRejectedAtCompileTime();
        failures += TestDataCycleIsACompileErrorNotAProcessKill();
        return failures;
    }

    // ---- The one failure mode in this compiler that cannot be reported, only survived. ---------------
    //
    // EmitPullOutput recurses through EmitPullInput to evaluate operands, and a hand-authored .ocgraph
    // can wire two data nodes into each other. Nothing upstream rejects it: the parser accepts it and
    // Graph.Validate() has no data-cycle check. The exec walk's own _execVisiting never sees these
    // nodes, because they are reached by PULL and it guards PUSH.
    //
    // Unguarded, that recursion does not throw -- it overflows the stack, and .NET makes
    // StackOverflowException uncatchable by design. The compiler's own catch(Exception) cannot
    // intercept it and the whole host process dies, editor included, on input a user typed. So this
    // test asserts something stronger than "an error is raised": it asserts the process is STILL ALIVE
    // afterwards and the error is an ordinary catchable one naming the cycle. If the guard is ever
    // removed, this test does not fail -- the test RUNNER disappears, which is its own loud signal.
    private static int TestDataCycleIsACompileErrorNotAProcessKill()
    {
        Console.WriteLine("Test: a data cycle is a catchable compile error, not a stack overflow");
        try
        {
            // add.a <- mul.result and mul.a <- add.result: a two-node data cycle, reachable from the
            // exec chain through the Branch condition, which is exactly how a real graph would hit it.
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE br Branch
NODE add add
NODE mul multiply
NODE k constfloat 2
NODE cmp compare
LINK tick.exec br.exec
LINK add.result mul.a
LINK mul.result add.a
LINK k.value mul.b
LINK k.value add.b
LINK add.result cmp.a
LINK k.value cmp.b
LINK cmp.result br.cond
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                // Rejecting it at parse time is an even better answer than rejecting it at compile
                // time -- the point is that it is refused somewhere, in a way a caller can handle.
                Console.WriteLine($"  PASS: refused at parse time: {err}");
                return 0;
            }

            var compiled = new GraphCompiler(graph).CompileEntryPoint("OnTick", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: a data cycle compiled without complaint");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("cycle", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: refused, but the message does not name a cycle: {compileErr ?? "(null)"}");
                return 1;
            }
            Console.WriteLine($"  PASS: process alive, refused with: {compileErr}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 1. The parser understands ENTRY and exec-typed PIN records at all. --------------------------
    private static int TestExecRecordsRoundTripThroughTheParser()
    {
        Console.WriteLine("Test: ENTRY and exec-typed PIN records parse");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE seq Sequence
LINK tick.exec seq.exec
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 1 || graph.EntryPoints[0].NodeId != "tick" || graph.EntryPoints[0].EventName != "OnTick")
            {
                Console.WriteLine($"  FAIL: expected one ENTRY (tick, OnTick), got {graph.EntryPoints.Count} entr" +
                                   (graph.EntryPoints.Count == 1 ? "y" : "ies"));
                return 1;
            }
            if (!graph.Nodes.TryGetValue("tick", out var tickNode) ||
                tickNode.Pins.Find(p => p.Name == "exec" && p.IsOutput)?.Type != PinType.Exec)
            {
                Console.WriteLine("  FAIL: 'tick' node's default 'exec' output pin did not parse as PinType.Exec");
                return 1;
            }

            Console.WriteLine("  PASS: ENTRY parsed, exec pin typed correctly");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 2/3. Branch: taking each side is independently provable via "tookTrue", regardless of what
    // (if anything) each arm is wired to -- see GraphCompiler.EmitBranch's own comment. -------------
    private static int TestBranchTakesTrueSide() => RunBranchCase(true);
    private static int TestBranchTakesFalseSide() => RunBranchCase(false);

    private static int RunBranchCase(bool condValue)
    {
        Console.WriteLine($"Test: Branch takes the {(condValue ? "true" : "false")} side");
        try
        {
            var text = @"
OCGRAPH 1
PARAM cond bool
NODE start OnStart
NODE b Branch
NODE pc Param param=cond
LINK start.exec b.exec
LINK pc.value b.cond
ENTRY start OnStart
OUT b tookTrue
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<bool, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            bool tookTrue = fn(condValue);
            if (tookTrue != condValue)
            {
                Console.WriteLine($"  FAIL: cond={condValue} but tookTrue={tookTrue}");
                return 1;
            }

            Console.WriteLine($"  PASS: cond={condValue} -> tookTrue={tookTrue}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 4. Sequence: N exec outputs fired IN ORDER. fireLog = fireLog*10 + (armIndex+1) before each
    // arm fires, so three arms firing in file order leaves fireLog == 123 -- a value that could only
    // result from all three arms firing, in exactly that order (a shuffled or partial firing would
    // leave a different number). ------------------------------------------------------------------
    private static int TestSequenceFiresArmsInOrder()
    {
        Console.WriteLine("Test: Sequence fires its arms in file order (then0, then1, then2)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE seq Sequence
PIN seq exec in exec
PIN seq then0 out exec
PIN seq then1 out exec
PIN seq then2 out exec
PIN seq fireLog out int
LINK start.exec seq.exec
ENTRY start OnStart
OUT seq fireLog
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            int fireLog = fn();
            if (fireLog != 123)
            {
                Console.WriteLine($"  FAIL: expected fireLog 123 (arms fired 1, then 2, then 3, in that order), got {fireLog}");
                return 1;
            }

            Console.WriteLine($"  PASS: fireLog={fireLog}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 5. while: a normal, non-guard termination. cond = (limit > w.iterations), so the loop
    // counts up until iterations reaches limit -- proving the condition is re-read fresh every pass
    // (a cached/hoisted read would never see iterations change and would either never run or never
    // stop) and that termination is driven by the graph's own data, not just the safety guard. -------
    private static int TestWhileLoopCountsAndTerminatesNormally()
    {
        Console.WriteLine("Test: while loop counts up to a data-driven limit and stops normally");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE w While
NODE limit ConstInt value=5
NODE cmp Compare
PIN cmp a in int
PIN cmp b in int
PIN cmp result out bool
LINK limit.value cmp.a
LINK w.iterations cmp.b
LINK cmp.result w.cond
LINK start.exec w.exec
ENTRY start OnStart
OUT w iterations
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            int iterations = fn();
            if (iterations != 5)
            {
                Console.WriteLine($"  FAIL: expected the loop to run exactly 5 times (limit=5), got {iterations}");
                return 1;
            }
            if (iterations >= GraphCompiler.MaxLoopIterations)
            {
                Console.WriteLine("  FAIL: iterations reached MaxLoopIterations -- this should have stopped via 'cond', not the guard");
                return 1;
            }

            Console.WriteLine($"  PASS: loop ran {iterations} times and stopped on its own condition");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 6. while: the guard, not the graph's own condition, is what stops a `cond` that never goes
    // false -- "a loop with a guard against running forever", proven by actually tripping it rather
    // than trusting the constant is wired up correctly. Also captures stderr and checks the warning
    // names the right node and event, so the guard is not merely silent. ----------------------------
    private static int TestWhileLoopTerminatesAtTheGuard()
    {
        Console.WriteLine("Test: while loop with an always-true condition stops at MaxLoopIterations, not forever");
        var originalError = Console.Error;
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE w While
NODE alwaysTrue ConstBool value=true
LINK alwaysTrue.value w.cond
LINK start.exec w.exec
ENTRY start OnStart
OUT w iterations
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            var captured = new StringWriter();
            Console.SetError(captured);
            int iterations = fn();
            Console.SetError(originalError);

            if (iterations != GraphCompiler.MaxLoopIterations)
            {
                Console.WriteLine($"  FAIL: expected exactly MaxLoopIterations ({GraphCompiler.MaxLoopIterations}), got {iterations} -- " +
                                   "the loop either stopped early or the guard did not fire");
                return 1;
            }
            var warning = captured.ToString();
            if (!warning.Contains("'w'") || !warning.Contains("OnStart"))
            {
                Console.WriteLine($"  FAIL: expected a warning naming node 'w' and event 'OnStart' on stderr, got: '{warning}'");
                return 1;
            }

            Console.WriteLine($"  PASS: guard tripped at {iterations} iterations; warning: {warning.Trim()}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.SetError(originalError);
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 7. forEach: the counted-repeat variant runs exactly `count` passes. ------------------------
    private static int TestForEachRepeatsCountTimes()
    {
        Console.WriteLine("Test: forEach runs exactly 'count' passes");
        try
        {
            var text = @"
OCGRAPH 1
PARAM n int
NODE start OnStart
NODE fe ForEach
NODE pn Param param=n
LINK pn.value fe.count
LINK start.exec fe.exec
ENTRY start OnStart
OUT fe index
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<int, int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            foreach (int n in new[] { 1, 5, 12 })
            {
                int finalIndex = fn(n);
                if (finalIndex != n)
                {
                    Console.WriteLine($"  FAIL: fn({n}) expected final index {n} (ran {n} passes, 0..{n - 1}), got {finalIndex}");
                    return 1;
                }
            }

            Console.WriteLine("  PASS: fn(1)=1, fn(5)=5, fn(12)=12");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 8. forEach with count=0: the loop body never runs, no guard trips, no crash -- the edge a
    // "run N times" primitive most needs to get right for free (N=0 is not special-cased anywhere in
    // EmitForEach; it falls out of `index(0) >= count(0)` being true on the very first check). --------
    private static int TestForEachWithZeroCountRunsNoIterations()
    {
        Console.WriteLine("Test: forEach with count=0 runs zero iterations");
        try
        {
            var text = @"
OCGRAPH 1
PARAM n int
NODE start OnStart
NODE fe ForEach
NODE pn Param param=n
LINK pn.value fe.count
LINK start.exec fe.exec
ENTRY start OnStart
OUT fe index
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<int, int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            int finalIndex = fn(0);
            if (finalIndex != 0)
            {
                Console.WriteLine($"  FAIL: expected index to stay 0 for count=0, got {finalIndex}");
                return 1;
            }

            Console.WriteLine("  PASS: fn(0)=0, no iterations ran");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 9. OnTick: an entry point that reads its own PARAM (deltaTime, the same plumbing every
    // dataflow graph uses for 'time') AND runs real exec logic (a Sequence, proven via fireLog) in the
    // same compile -- the concrete shape the task asked for: "OnTick (per frame, receiving delta
    // time)". Two OUT records, so this also exercises CompileEntryPoint's object[] convention. --------
    private static int TestOnTickEntryPointCompilesAndRuns()
    {
        Console.WriteLine("Test: OnTick entry point compiles, receives deltaTime via PARAM, and its exec chain runs");
        try
        {
            var text = @"
OCGRAPH 1
PARAM deltaTime float
NODE tick OnTick
NODE seq Sequence
NODE speed ConstFloat value=10.0
NODE dt Param param=deltaTime
NODE dist Multiply
LINK tick.exec seq.exec
LINK speed.value dist.a
LINK dt.value dist.b
ENTRY tick OnTick
OUT seq fireLog
OUT dist result
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<float, object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            object[] result = fn(0.5f);
            if (result.Length != 2)
            {
                Console.WriteLine($"  FAIL: expected 2 outputs, got {result.Length}");
                return 1;
            }
            int fireLog = (int)result[0];
            float dist = (float)result[1];

            // fireLog==12: Sequence's two default arms (then0, then1) both fire, in order, whether or
            // not anything is wired to them -- see GraphCompiler.EmitExecFanOut's own comment. A
            // fireLog of 0 would mean OnTick's exec chain never actually reached the Sequence node.
            if (fireLog != 12)
            {
                Console.WriteLine($"  FAIL: expected fireLog 12 (proving the OnTick exec chain reached the Sequence), got {fireLog}");
                return 1;
            }
            if (Math.Abs(dist - 5.0f) > 1e-5)
            {
                Console.WriteLine($"  FAIL: expected dist = speed(10) * deltaTime(0.5) = 5.0, got {dist}");
                return 1;
            }

            Console.WriteLine($"  PASS: fireLog={fireLog}, dist={dist}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 10. Asking for an event with no matching ENTRY record fails clearly, not with a null
    // delegate and no explanation. --------------------------------------------------------------------
    private static int TestNoEntryForRequestedEventFailsClearly()
    {
        Console.WriteLine("Test: CompileEntryPoint fails clearly when no ENTRY declares the requested event");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
ENTRY start OnStart
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnCollide", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected CompileEntryPoint('OnCollide') to fail (no such ENTRY), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("OnCollide"))
            {
                Console.WriteLine($"  FAIL: expected an error naming 'OnCollide', got: {compileErr}");
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

    // ---- 11. THE REGRESSION TEST THIS FEATURE MOST NEEDS. A graph with no exec pins and no ENTRY
    // record at all -- the only kind that existed before this change -- still parses with zero
    // EntryPoints and still compiles and runs through the OLD Compile() method exactly as before.
    // Reuses the exact fixture and expected value TestCrossImplementationFixture (Program.cs) already
    // established: (5 + 7) * 3 == 36. -----------------------------------------------------------------
    private static int TestOldGraphWithoutExecStillCompilesTheOldWay()
    {
        Console.WriteLine("Test: a pre-exec graph (no ENTRY, no exec pins) still compiles via Compile(), unaffected");
        try
        {
            // ITS OWN INPUT, NOT THE SHARED FIXTURE. This test used to read cross_impl_test.ocgraph,
            // which was reasonable while that file was pure dataflow -- but that fixture's job is to
            // carry EVERY format feature so the C++ and C# readers can be checked against each other,
            // so it now contains an exec chain and an ENTRY by design. A backward-compatibility test
            // must own a genuinely pre-exec graph, or the day the shared fixture grows a feature the
            // test starts asserting something false about a file that is no longer its example.
            //
            // Same shape and same expected value as before: (5 + 7) * 3 == 36.
            string graphText = @"
OCGRAPH 1
NAME PreExecDataflow
NODE c1 ConstFloat
NODE c2 ConstFloat
NODE c3 ConstFloat
NODE sum Add
NODE prod Multiply
PIN c1 value out float 5
PIN c2 value out float 7
PIN c3 value out float 3
PIN sum a in float
PIN sum b in float
PIN sum result out float
PIN prod a in float
PIN prod b in float
PIN prod result out float
LINK c1.value sum.a
LINK c2.value sum.b
LINK sum.result prod.a
LINK c3.value prod.b
OUT prod result
";

            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero entry points on a pre-exec graph, got {graph.EntryPoints.Count}");
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
            if (Math.Abs(result - 36.0f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected 36 (unchanged from before exec support existed), got {result}");
                return 1;
            }

            // And CompileEntryPoint on the SAME graph correctly refuses -- there is nothing to compile,
            // and it says so, rather than silently returning something.
            var entryCompiled = compiler.CompileEntryPoint("OnStart", out var entryErr);
            if (entryCompiled != null)
            {
                Console.WriteLine("  FAIL: CompileEntryPoint succeeded on a graph with no ENTRY records at all");
                return 1;
            }

            Console.WriteLine($"  PASS: Compile() still returns {result}; CompileEntryPoint correctly refuses ({entryErr})");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 12. PARAM cannot be declared exec -- caught at PARSE time with a clear message, not a
    // confusing runtime exception three layers into compilation. ---------------------------------------
    private static int TestExecTypedParamIsRejectedAtParseTime()
    {
        Console.WriteLine("Test: 'PARAM x exec' is rejected at parse time");
        try
        {
            var text = @"
OCGRAPH 1
PARAM trigger exec
NODE start OnStart
";
            if (OcGraphParser.Parse(text, out _, out var err))
            {
                Console.WriteLine("  FAIL: expected parse to fail (PARAM cannot be exec), but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("exec"))
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

    // ---- 13. OUT cannot name an exec pin, in EITHER compiler -- checked here against
    // CompileEntryPoint specifically (Compile()'s own copy of this check is exercised implicitly by
    // every other test in Program.cs that never hits it; this is the one place that deliberately
    // tries to trigger it). -------------------------------------------------------------------------
    private static int TestOutReferencingAnExecPinIsRejectedAtCompileTime()
    {
        Console.WriteLine("Test: OUT naming an exec pin is rejected by CompileEntryPoint");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
ENTRY start OnStart
OUT start exec
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (OUT names an exec pin), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("exec pin"))
            {
                Console.WriteLine($"  FAIL: expected an error naming the exec pin problem, got: {compileErr}");
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
}
