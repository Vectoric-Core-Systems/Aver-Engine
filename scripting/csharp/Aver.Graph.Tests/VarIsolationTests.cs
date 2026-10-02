// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// INDEPENDENT adversarial pass over graph-local persistent variables (VAR/GetVar/SetVar), written as an
// INDEPENDENT second pass, deliberately not reusing GraphVarTests.cs's own scenarios.
// Focused entirely on THE SHARING BUG: is variable storage really per-GraphHost-instance, or does it
// leak across hosts/graphs sharing a name, a file, or a static table somewhere.
using System;
using System.Collections.Generic;
using Aver.Graph;

static class VarIsolationTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestThreeHostsOverOneGraphFileAreAllIndependent();
        failures += TestTwoHostsOverTwoDifferentGraphsSharingAVariableNameAreIndependent();
        failures += TestVarLessGraphStillDrivesPositionThroughGraphHostUnchanged();
        failures += TestOneHostTwoDifferentVarsDoNotCrossTalk();
        failures += TestManyHostsInterleavedWritesStayIsolated();
        failures += TestReloadingOneHostDoesNotDisturbASiblingHost();
        failures += TestSameProcessConcurrentGraphHostsForDifferentGraphsAtOnce();

        return failures;
    }

    private static GraphHost NewHost() => new GraphHost(positionSink: (int e, float x, float y, float z) => { });

    // ---------------------------------------------------------------------------------------------
    // THREE hosts, not two -- the builder's own test only proves pairwise independence. If storage
    // were keyed by something coarser than "this exact GraphHost object" (a small LRU keyed by file
    // hash, say, sized for "the last 2 distinct hosts"), two hosts could easily look independent while
    // a third silently aliased one of the first two. Each of the three writes a DISTINCT value and all
    // three must read back only their own.
    // ---------------------------------------------------------------------------------------------
    private static int TestThreeHostsOverOneGraphFileAreAllIndependent()
    {
        Console.WriteLine("Test[reviewer]: THREE GraphHosts over the same .ocgraph text are independently isolated");
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
            var hosts = new List<GraphHost>();
            for (int i = 0; i < 3; i++)
            {
                var h = NewHost();
                if (!h.LoadFromText(text, out var err))
                {
                    Console.WriteLine($"  FAIL: host {i} LoadFromText: {err}");
                    return 1;
                }
                hosts.Add(h);
            }

            float[] written = { 11f, 22f, 33f };
            for (int i = 0; i < 3; i++)
            {
                if (!hosts[i].Fire("Bump", new object[] { written[i] }, out var result) ||
                    result is not float rf || Math.Abs(rf - written[i]) > 1e-6)
                {
                    Console.WriteLine($"  FAIL: host {i} Fire('Bump', [{written[i]}]) returned {result}");
                    return 1;
                }
            }

            // Query every host AFTER all three writes have happened -- if any pair shares storage,
            // whichever wrote last (33) would show up on more than one host here.
            for (int i = 0; i < 3; i++)
            {
                if (!hosts[i].Fire("Query", new object[] { 0f }, out var q) ||
                    q is not float qf || Math.Abs(qf - written[i]) > 1e-6)
                {
                    Console.WriteLine($"  FAIL: host {i} Query expected {written[i]} (its own write), got {q} -- cross-host leakage");
                    return 1;
                }
            }

            Console.WriteLine($"  PASS: hosts read back {written[0]}, {written[1]}, {written[2]} respectively -- no leakage among three");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Two DIFFERENT .ocgraph files (different NAME, different node ids, different unrelated PARAM)
    // that both happen to declare a variable named "score" -- if GraphVarStore or anything upstream
    // were keyed by variable NAME ALONE rather than scoped per (host, its own graph), one graph's
    // "score" could bleed into the other's. Each graph is hosted by its OWN GraphHost (the ordinary
    // case), so this also stands in for "two unrelated actor types with unrelated graphs that happen
    // to reuse a common variable name like 'health' or 'score'".
    // ---------------------------------------------------------------------------------------------
    private static int TestTwoHostsOverTwoDifferentGraphsSharingAVariableNameAreIndependent()
    {
        Console.WriteLine("Test[reviewer]: two DIFFERENT graphs that both declare VAR score are independent");
        try
        {
            var textAlpha = @"
OCGRAPH 1
NAME AlphaGraph
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
            // Deliberately shaped differently -- an extra unrelated ConstFloat node, a differently
            // named entry, different node ids -- so nothing here could pass by two files being
            // textually identical.
            var textBeta = @"
OCGRAPH 1
NAME BetaGraph
VAR score float 0
VAR unrelated int 999
PARAM w float
NODE trigger OnHit
NODE pw Param param=w
NODE noise ConstFloat value=123.0
NODE setter SetVar var=score
NODE reader GetVar var=score
NODE query OnHit
LINK trigger.exec setter.exec
LINK pw.value setter.value
ENTRY trigger Bump
ENTRY query Query
OUT reader value
";
            var hostAlpha = NewHost();
            var hostBeta = NewHost();

            if (!hostAlpha.LoadFromText(textAlpha, out var errA))
            {
                Console.WriteLine($"  FAIL: hostAlpha.LoadFromText: {errA}");
                return 1;
            }
            if (!hostBeta.LoadFromText(textBeta, out var errB))
            {
                Console.WriteLine($"  FAIL: hostBeta.LoadFromText: {errB}");
                return 1;
            }

            if (!hostAlpha.Fire("Bump", new object[] { 7f }, out var ra) || ra is not float raf || Math.Abs(raf - 7f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostAlpha.Fire('Bump',[7]) returned {ra}");
                return 1;
            }
            if (!hostBeta.Fire("Bump", new object[] { 250f }, out var rb) || rb is not float rbf || Math.Abs(rbf - 250f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostBeta.Fire('Bump',[250]) returned {rb}");
                return 1;
            }

            if (!hostAlpha.Fire("Query", new object[] { 0f }, out var qa) || qa is not float qaf || Math.Abs(qaf - 7f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostAlpha.Query expected 7 (its own 'score'), got {qa}");
                return 1;
            }
            if (!hostBeta.Fire("Query", new object[] { 0f }, out var qb) || qb is not float qbf || Math.Abs(qbf - 250f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostBeta.Query expected 250 (its own 'score'), got {qb}");
                return 1;
            }

            Console.WriteLine($"  PASS: AlphaGraph.score={qa}, BetaGraph.score={qb} -- same variable NAME, different graphs, no leakage");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // REGRESSION: a graph declaring NO VAR records at all, driven through GraphHost.Tick (not
    // GraphCompiler directly) exactly the way the pre-existing dataflow drone graphs are, must still
    // compute the same position it always did. This is the "old path did not regress" proof the task
    // asked for, exercised through the SAME entry point (GraphHost.Tick) the sharing tests above use,
    // rather than trusting GraphCompiler's own unit tests in isolation.
    // ---------------------------------------------------------------------------------------------
    private static int TestVarLessGraphStillDrivesPositionThroughGraphHostUnchanged()
    {
        Console.WriteLine("Test[reviewer]: a VAR-less dataflow graph, ticked through GraphHost, is unaffected by the VAR addition");
        try
        {
            // PARAM entity int, PARAM time float; OUT x=time*2, y=time*3, z=0 (only 2 OUT so z stays
            // implicit 0 per GraphHost's own OUTPUT WIRING convention).
            var text = @"
OCGRAPH 1
PARAM entity int
PARAM time float
NODE pe Param param=entity
NODE pt Param param=time
NODE two ConstFloat value=2.0
NODE three ConstFloat value=3.0
NODE mx Multiply
NODE my Multiply
LINK pt.value mx.a
LINK two.value mx.b
LINK pt.value my.a
LINK three.value my.b
OUT mx result
OUT my result
";
            float sx = 0, sy = 0, sz = 0;
            int sinkCalls = 0;
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) =>
            {
                sinkCalls++;
                sx = x; sy = y; sz = z;
            });

            if (!host.LoadFromText(text, out var err))
            {
                Console.WriteLine($"  FAIL: LoadFromText: {err}");
                return 1;
            }

            host.Tick(entityId: 42, timeSeconds: 5f);

            if (sinkCalls != 1)
            {
                Console.WriteLine($"  FAIL: expected exactly 1 PositionSink call, got {sinkCalls}");
                return 1;
            }
            if (Math.Abs(sx - 10f) > 1e-5 || Math.Abs(sy - 15f) > 1e-5 || Math.Abs(sz - 0f) > 1e-5)
            {
                Console.WriteLine($"  FAIL: expected (10, 15, 0) at time=5, got ({sx}, {sy}, {sz})");
                return 1;
            }

            // Second Tick with a different time -- a VAR-less graph must be a PURE function of its
            // PARAMs still: no memory, x/y recomputed fresh from the new time only.
            host.Tick(entityId: 42, timeSeconds: 2f);
            if (Math.Abs(sx - 4f) > 1e-5 || Math.Abs(sy - 6f) > 1e-5)
            {
                Console.WriteLine($"  FAIL: expected (4, 6, 0) at time=2 (fresh computation, no leftover state), got ({sx}, {sy}, {sz})");
                return 1;
            }

            Console.WriteLine($"  PASS: VAR-less dataflow graph through GraphHost.Tick still computes correctly and remains a pure function of PARAM");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // ONE host, TWO different variables. Confirms the per-instance Dictionary<string,object> keys
    // correctly by name WITHIN one store (a sanity check on GraphVarStore itself, not cross-host
    // isolation) -- writing 'ammo' must never disturb 'score' on the same store.
    // ---------------------------------------------------------------------------------------------
    private static int TestOneHostTwoDifferentVarsDoNotCrossTalk()
    {
        Console.WriteLine("Test[reviewer]: one host, two distinct VARs (score, ammo) do not cross-talk with each other");
        try
        {
            var text = @"
OCGRAPH 1
VAR score float 0
VAR ammo int 30
PARAM delta float
NODE tick OnTick
NODE pd Param param=delta
NODE gvScore GetVar var=score
NODE add Add
NODE svScore SetVar var=score
NODE gvAmmo GetVar var=ammo
LINK tick.exec svScore.exec
LINK gvScore.value add.a
LINK pd.value add.b
LINK add.result svScore.value
ENTRY tick OnTick
OUT gvScore value
OUT gvAmmo value
";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).CompileEntryPoint("OnTick", out var cerr);
            if (compiled is not Func<float, GraphVarStore, object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            var store = GraphVarStore.CreateFor(graph);

            var r1 = fn(4f, store);
            // score should now be 4 (0+4); ammo untouched, still 30 (its declared default).
            if (r1[0] is not float score1 || Math.Abs(score1 - 4f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected score=4 after first tick, got {r1[0]}");
                return 1;
            }
            if (r1[1] is not int ammo1 || ammo1 != 30)
            {
                Console.WriteLine($"  FAIL: expected ammo=30 (untouched default) after first tick, got {r1[1]}");
                return 1;
            }

            var r2 = fn(6f, store);
            if (r2[0] is not float score2 || Math.Abs(score2 - 10f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: expected score=10 after second tick (4+6), got {r2[0]}");
                return 1;
            }
            if (r2[1] is not int ammo2 || ammo2 != 30)
            {
                Console.WriteLine($"  FAIL: expected ammo still 30 after second tick (never written), got {r2[1]}");
                return 1;
            }

            Console.WriteLine($"  PASS: score accumulated 4 -> 10 while ammo stayed pinned at its default 30 throughout");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // MANY hosts (16), writes INTERLEAVED rather than done-host-by-host, then all read back at the
    // end. done-host-by-host is the shape both the builder's test and TestThreeHostsOverOneGraphFile
    // above use; interleaving is a materially different access pattern that would additionally catch
    // a bug where storage were a SINGLE shared table with last-write-wins semantics (which would
    // still coincidentally pass a strictly sequential write-then-immediately-read test on 2-3 hosts,
    // but not this).
    // ---------------------------------------------------------------------------------------------
    private static int TestManyHostsInterleavedWritesStayIsolated()
    {
        Console.WriteLine("Test[reviewer]: 16 hosts, writes interleaved across all of them, still read back independently");
        try
        {
            var text = @"
OCGRAPH 1
VAR counter int 0
PARAM v int
NODE bump OnHit
NODE pv Param param=v
NODE sv SetVar var=counter
NODE gv GetVar var=counter
NODE q OnHit
LINK bump.exec sv.exec
LINK pv.value sv.value
ENTRY bump Bump
ENTRY q Query
OUT gv value
";
            const int N = 16;
            var hosts = new GraphHost[N];
            for (int i = 0; i < N; i++)
            {
                hosts[i] = NewHost();
                if (!hosts[i].LoadFromText(text, out var err))
                {
                    Console.WriteLine($"  FAIL: host {i} LoadFromText: {err}");
                    return 1;
                }
            }

            // Interleave: round 1 writes i*10 to host i, round 2 writes i*10+1, etc, round-robin
            // across all hosts rather than finishing one host before starting the next.
            for (int round = 0; round < 3; round++)
            {
                for (int i = 0; i < N; i++)
                {
                    int val = i * 100 + round;
                    if (!hosts[i].Fire("Bump", new object[] { val }, out var result) ||
                        result is not int rv || rv != val)
                    {
                        Console.WriteLine($"  FAIL: host {i} round {round}: Fire('Bump',[{val}]) returned {result}");
                        return 1;
                    }
                }
            }

            // Final expected value per host is i*100 + 2 (the last round written).
            for (int i = 0; i < N; i++)
            {
                int expected = i * 100 + 2;
                if (!hosts[i].Fire("Query", new object[] { 0 }, out var q) || q is not int qv || qv != expected)
                {
                    Console.WriteLine($"  FAIL: host {i} final Query expected {expected}, got {q}");
                    return 1;
                }
            }

            Console.WriteLine($"  PASS: all {N} hosts retained their own interleaved final write with zero cross-contamination");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Reloading host A (a second LoadFromText call, which the design says replaces A's store with a
    // fresh one) must not disturb an entirely SEPARATE, already-loaded host B over the same file --
    // proves ResetCompiledState()/the fresh GraphVarStore.CreateFor() call on reload is scoped to
    // the reloading instance only, not some shared table both hosts happen to read through.
    // ---------------------------------------------------------------------------------------------
    private static int TestReloadingOneHostDoesNotDisturbASiblingHost()
    {
        Console.WriteLine("Test[reviewer]: reloading host A does not reset or disturb sibling host B's variables");
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
            var hostA = NewHost();
            var hostB = NewHost();
            if (!hostA.LoadFromText(text, out var errA)) { Console.WriteLine($"  FAIL: hostA load: {errA}"); return 1; }
            if (!hostB.LoadFromText(text, out var errB)) { Console.WriteLine($"  FAIL: hostB load: {errB}"); return 1; }

            hostA.Fire("Bump", new object[] { 1f }, out _);
            hostB.Fire("Bump", new object[] { 500f }, out _);

            // Reload A only.
            if (!hostA.LoadFromText(text, out var errA2)) { Console.WriteLine($"  FAIL: hostA reload: {errA2}"); return 1; }

            if (!hostB.Fire("Query", new object[] { 0f }, out var qb) || qb is not float qbf || Math.Abs(qbf - 500f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostB expected to still read back 500 (its own state, untouched by hostA's reload), got {qb}");
                return 1;
            }
            if (!hostA.Fire("Query", new object[] { 0f }, out var qa) || qa is not float qaf || Math.Abs(qaf - 0f) > 1e-6)
            {
                Console.WriteLine($"  FAIL: hostA expected 0 (fresh default after its own reload), got {qa}");
                return 1;
            }

            Console.WriteLine($"  PASS: hostA reset to {qa} on its own reload; hostB unaffected, still {qb}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Compiles TWO different graphs with the SAME GraphCompiler-adjacent machinery interleaved at the
    // Compile() call-site level (not just at the GraphHost level) -- guards against a bug where
    // GraphCompiler's own _varStoreArgIndex (an instance field, set fresh per Compile()/
    // CompileEntryPoint() call) could leak between two DIFFERENT GraphCompiler instances if it were
    // ever accidentally made static, or between two entry points of the same compiler if the field
    // were not actually reset every call. Two graphs, one with VAR, one without, compiled in
    // alternation through freshly-constructed compilers, must never see each other's arg-index.
    // ---------------------------------------------------------------------------------------------
    private static int TestSameProcessConcurrentGraphHostsForDifferentGraphsAtOnce()
    {
        Console.WriteLine("Test[reviewer]: alternating Compile() calls across a VAR graph and a VAR-less graph never cross-wire the trailing-arg index");
        try
        {
            var varText = "OCGRAPH 1\nVAR score int 9\nNODE gv GetVar var=score\nOUT gv value\n";
            var plainText = "OCGRAPH 1\nPARAM x float\nNODE px Param param=x\nOUT px value\n";

            if (!OcGraphParser.Parse(varText, out var varGraph, out var e1)) { Console.WriteLine($"  FAIL: parse varText: {e1}"); return 1; }
            if (!OcGraphParser.Parse(plainText, out var plainGraph, out var e2)) { Console.WriteLine($"  FAIL: parse plainText: {e2}"); return 1; }

            for (int round = 0; round < 5; round++)
            {
                // Compile the VAR-less graph FIRST each round -- if _varStoreArgIndex somehow survived
                // from the previous round's VAR graph compile (e.g. a static, or an object reused
                // without resetting), this is exactly the ordering that would catch it: a stray extra
                // Ldarg for a nonexistent trailing parameter would throw or produce a garbage delegate
                // shape.
                var plainCompiled = new GraphCompiler(plainGraph).Compile(out var pe);
                if (plainCompiled is not Func<float, float> plainFn)
                {
                    Console.WriteLine($"  FAIL: round {round}: VAR-less graph compile error or wrong shape: {pe ?? plainCompiled?.GetType().Name}");
                    return 1;
                }
                if (Math.Abs(plainFn(3.5f) - 3.5f) > 1e-6)
                {
                    Console.WriteLine($"  FAIL: round {round}: VAR-less graph should just pass PARAM x through, got {plainFn(3.5f)}");
                    return 1;
                }

                var varCompiled = new GraphCompiler(varGraph).Compile(out var ve);
                if (varCompiled is not Func<GraphVarStore, int> varFn)
                {
                    Console.WriteLine($"  FAIL: round {round}: VAR graph compile error or wrong shape: {ve ?? varCompiled?.GetType().Name}");
                    return 1;
                }
                var store = GraphVarStore.CreateFor(varGraph);
                if (varFn(store) != 9)
                {
                    Console.WriteLine($"  FAIL: round {round}: VAR graph should read declared default 9, got {varFn(store)}");
                    return 1;
                }
            }

            Console.WriteLine("  PASS: 5 rounds alternating VAR/VAR-less Compile() calls, no cross-wiring of the trailing-arg index");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}\n{ex.StackTrace}");
            return 1;
        }
    }
}
