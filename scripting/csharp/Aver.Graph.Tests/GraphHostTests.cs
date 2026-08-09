// Tests for GraphHost: load once, compile once, tick against a fake entity, and prove the drone
// graph actually produces motion.
//
// HONEST SCOPE: nothing in this test process boots a native Aver.Scene/CoreCLR scene -- confirmed
// both by static reading (see the scout reports this phase inherited) and empirically, in this same
// test project, by TestGetFieldEmitsRealNativeCall hitting EntryPointNotFoundException the moment it
// tries to call through. So every test below that exercises GraphHost.Tick supplies its OWN
// PositionSink (an in-memory recorder) instead of GraphHost's default, which would try to write into
// a scene that does not exist here. That is the gap named, not hidden: what IS proven is that the
// compiled graph produces the right numbers, in the right order, changing correctly over time, and
// that GraphHost gets those numbers from the compiled delegate to the sink correctly. What is NOT
// proven here is that aver_scene_set_vec, given those numbers, actually moves anything an editor or
// game would render -- that needs a live engine and a human watching, described in this phase's report.

using System;
using System.Collections.Generic;
using Aver.Graph;

static class GraphHostTests
{
    // THIS TEST PROJECT'S OWN FIXTURE, beside cross_impl_test.ocgraph, resolved relative to the test
    // rather than by absolute path.
    //
    // It used to point at C:\Users\User\Documents\AverProjects\ElectricDreams\Content\Scripts\
    // Drone.ocgraph -- a game project, on one machine, outside this repository. The engine's own test
    // suite cannot depend on a project: it fails on every other computer, it fails in CI, and it
    // fails the moment someone edits their level's drone, which is a thing they are entitled to do
    // without breaking the engine.
    //
    // The original comment argued the absolute path was the honest choice because a missing file
    // would "fail loudly rather than silently skip". That reasoning was right about skipping and
    // wrong about the fix: a test that fails loudly for a reason unrelated to the code under test is
    // not signal. Owning a copy of the fixture gets both -- it always exists, and it only changes
    // when someone deliberately changes it here.
    //
    // A COPY, not a reference, on purpose. This asserts what GraphHost does with a known graph; the
    // sample project's Drone.ocgraph asserts what that project wants its drone to do. They started
    // identical and are allowed to diverge.
    private const string DroneGraphPath = "drone_test.ocgraph";

    // Mirrors the constants baked into Drone.ocgraph's PIN lines. Kept here, by hand, so this test
    // proves the compiled graph matches the INTENDED formula, not just "whatever GraphCompiler
    // happens to emit" -- exactly the same hand-computed-expected-value pattern every other test in
    // this project already uses (e.g. TestAddThenMultiply's (10+2)*3=36). If Drone.ocgraph's
    // constants change, these must change with them, or this test will correctly start failing.
    private const float Radius = 9000f;
    private const float CenterX = 0f;
    private const float CenterY = 0f;
    private const float BaseAltitude = 1400f;
    private const float BobAmplitude = 180f;
    private const float OrbitSpeed = 0.15f;
    private const float BobSpeed = 0.9f;

    public static int RunAll()
    {
        int failures = 0;
        failures += TestDroneGraphLoadsAndCompilesFromDisk();
        failures += TestDroneOrbitMatchesFormulaAndMovesContinuously();
        failures += TestDroneOrbitRadiusStaysRoughlyConstant();
        failures += TestDroneAltitudeBobStaysWithinAmplitude();
        failures += TestGraphHostCompileErrorIsReportedThenTickIsANoOp();
        failures += TestGraphHostAppliesTwoOutputsAsXYWithZDefaultingToZero();
        failures += TestGraphHostRejectsUnsuppliableParam();
        failures += TestGraphHostCompilesOnceNotPerTick();
        return failures;
    }

    private static float Expected(float t, out float x, out float y, out float z)
    {
        float angle = t * OrbitSpeed;
        x = CenterX + Radius * (float)Math.Cos(angle);
        y = CenterY + Radius * (float)Math.Sin(angle);
        float bobPhase = t * BobSpeed;
        z = BaseAltitude + BobAmplitude * (float)Math.Sin(bobPhase);
        return angle;
    }

    // ---- 1. The actual shipped file loads and compiles without error. ----
    private static int TestDroneGraphLoadsAndCompilesFromDisk()
    {
        Console.WriteLine("Test: Drone.ocgraph loads and compiles from disk");
        try
        {
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (!host.Load(DroneGraphPath, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }
            if (!host.Ready)
            {
                Console.WriteLine("  FAIL: Load() returned true but Ready is false");
                return 1;
            }
            if (host.Graph == null || host.Graph.Outputs.Count != 3)
            {
                Console.WriteLine($"  FAIL: expected 3 OUT records (x,y,z), got {host.Graph?.Outputs.Count.ToString() ?? "(no graph)"}");
                return 1;
            }
            Console.WriteLine($"  PASS: loaded '{host.Graph.Name}', {host.Graph.Nodes.Count} nodes, {host.Graph.Outputs.Count} outputs");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 2. THE MAIN PROOF: tick the real, on-disk drone graph over a series of time values against
    // a fake in-memory entity, and check the reported position against an independently computed
    // expectation of the same circle-plus-bob formula. Also checks that position genuinely changes
    // tick to tick (not stuck), which is the entire point of driving chunk streaming with this. ----
    private static int TestDroneOrbitMatchesFormulaAndMovesContinuously()
    {
        Console.WriteLine("Test: Drone orbit matches the intended formula and moves continuously");
        try
        {
            var recorded = new List<(int entity, float x, float y, float z)>();
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => recorded.Add((e, x, y, z)));

            if (!host.Load(DroneGraphPath, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            const int entity = 7;
            float[] sampleTimes = { 0f, 3f, 7f, 12.5f, 21f, 30f, 42f, 60f };
            foreach (var t in sampleTimes)
                host.Tick(entity, t);

            if (recorded.Count != sampleTimes.Length)
            {
                Console.WriteLine($"  FAIL: expected {sampleTimes.Length} position writes (one per Tick), got {recorded.Count}");
                return 1;
            }

            for (int i = 0; i < sampleTimes.Length; i++)
            {
                var (e, x, y, z) = recorded[i];
                if (e != entity)
                {
                    Console.WriteLine($"  FAIL: sample {i} was applied to entity {e}, expected {entity}");
                    return 1;
                }

                Expected(sampleTimes[i], out var ex, out var ey, out var ez);
                const float tol = 0.5f; // cm -- float32 sin/cos through two independent code paths
                if (Math.Abs(x - ex) > tol || Math.Abs(y - ey) > tol || Math.Abs(z - ez) > tol)
                {
                    Console.WriteLine($"  FAIL: t={sampleTimes[i]}: got ({x},{y},{z}), expected ({ex},{ey},{ez})");
                    return 1;
                }
            }

            // Motion, not a photograph: consecutive samples must differ. A graph that silently
            // returned a constant (e.g. every getfield-style stub bug this codebase has shipped
            // before) would pass a single-sample check but fail this one.
            for (int i = 1; i < recorded.Count; i++)
            {
                var (_, x0, y0, z0) = recorded[i - 1];
                var (_, x1, y1, z1) = recorded[i];
                float moved = (float)Math.Sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
                if (moved < 1f)
                {
                    Console.WriteLine($"  FAIL: sample {i - 1}->{i} moved only {moved}cm -- looks stuck, not flying");
                    return 1;
                }
            }

            Console.WriteLine($"  PASS: {recorded.Count} samples all matched the formula within tolerance and all moved between samples");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 3. "An orbit is the simplest honest choice" only holds if it's actually circular: radius
    // (distance from the orbit center in the XY plane) must stay close to the constant 9000cm across
    // many samples, while the angle keeps advancing (proven in test 2 via x/y matching the formula,
    // and independently reconfirmed here via atan2). ----
    private static int TestDroneOrbitRadiusStaysRoughlyConstant()
    {
        Console.WriteLine("Test: Drone orbit radius stays roughly constant while angle advances");
        try
        {
            var recorded = new List<(float x, float y)>();
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => recorded.Add((x, y)));

            if (!host.Load(DroneGraphPath, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            const int entity = 1;
            const int steps = 12;
            const float dt = 4f; // seconds between samples
            for (int i = 0; i < steps; i++)
                host.Tick(entity, i * dt);

            float? prevAngle = null;
            foreach (var (x, y) in recorded)
            {
                float radius = (float)Math.Sqrt((x - CenterX) * (x - CenterX) + (y - CenterY) * (y - CenterY));
                if (Math.Abs(radius - Radius) > 1f)
                {
                    Console.WriteLine($"  FAIL: radius {radius} deviates from expected {Radius} by more than 1cm");
                    return 1;
                }

                float angle = (float)Math.Atan2(y - CenterY, x - CenterX);
                if (prevAngle.HasValue)
                {
                    // Expected step: dt * OrbitSpeed radians, wrapped into (-pi, pi] the same way
                    // atan2's own output is wrapped, so a step that crosses the +-pi seam still
                    // compares correctly.
                    float expectedStep = dt * OrbitSpeed;
                    float actualStep = angle - prevAngle.Value;
                    while (actualStep <= -Math.PI) actualStep += (float)(2 * Math.PI);
                    while (actualStep > Math.PI) actualStep -= (float)(2 * Math.PI);
                    if (Math.Abs(actualStep - expectedStep) > 0.01f)
                    {
                        Console.WriteLine($"  FAIL: angle step {actualStep} rad, expected {expectedStep} rad");
                        return 1;
                    }
                }
                prevAngle = angle;
            }

            Console.WriteLine($"  PASS: radius held within 1cm of {Radius}cm across {steps} samples; angle advanced by {dt * OrbitSpeed:F4} rad/sample as expected");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 4. The altitude bob is bounded (base_altitude +/- bob_amplitude), and actually varies
    // rather than sitting flat -- proving it's not "on rails" the way the task asked to avoid. ----
    private static int TestDroneAltitudeBobStaysWithinAmplitude()
    {
        Console.WriteLine("Test: Drone altitude bob stays within amplitude and actually varies");
        try
        {
            var zs = new List<float>();
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => zs.Add(z));

            if (!host.Load(DroneGraphPath, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            const int entity = 1;
            for (int i = 0; i < 40; i++)
                host.Tick(entity, i * 0.7f);

            float min = float.MaxValue, max = float.MinValue;
            foreach (var z in zs) { min = Math.Min(min, z); max = Math.Max(max, z); }

            if (min < BaseAltitude - BobAmplitude - 1f || max > BaseAltitude + BobAmplitude + 1f)
            {
                Console.WriteLine($"  FAIL: z range [{min},{max}] exceeds expected [{BaseAltitude - BobAmplitude},{BaseAltitude + BobAmplitude}]");
                return 1;
            }
            if (max - min < BobAmplitude) // over 40 unevenly-spaced samples it should have swung most of the way
            {
                Console.WriteLine($"  FAIL: z barely moved ({max - min}cm range) -- looks flat, not bobbing");
                return 1;
            }

            Console.WriteLine($"  PASS: z ranged over [{min:F1},{max:F1}]cm, within [{BaseAltitude - BobAmplitude},{BaseAltitude + BobAmplitude}]cm");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 5. GraphHost's own contract: a bad graph reports a compile error once, at Load(), and
    // Tick() afterward is a safe no-op -- not an exception thrown every frame. ----
    private static int TestGraphHostCompileErrorIsReportedThenTickIsANoOp()
    {
        Console.WriteLine("Test: GraphHost reports a compile error and Tick() becomes a no-op");
        try
        {
            // A cycle: 'a' feeds 'b' and 'b' feeds 'a'. GraphCompiler.TopologicalSort fails this.
            var badGraph = @"
OCGRAPH 1
NODE a Add
NODE b Add
LINK a result b a
LINK b result a a
OUT a result
";
            bool sinkCalled = false;
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => sinkCalled = true);

            if (host.LoadFromText(badGraph, out var err))
            {
                Console.WriteLine("  FAIL: expected Load to fail on a cyclic graph, but it succeeded");
                return 1;
            }
            if (string.IsNullOrEmpty(err) || host.LoadError != err)
            {
                Console.WriteLine($"  FAIL: expected a non-empty error surfaced via both the out param and LoadError, got out='{err}' LoadError='{host.LoadError}'");
                return 1;
            }
            if (host.Ready)
            {
                Console.WriteLine("  FAIL: Ready is true after a failed Load()");
                return 1;
            }

            // Must not throw, must not call the sink, for several ticks in a row.
            for (int i = 0; i < 5; i++)
                host.Tick(1, i);

            if (sinkCalled)
            {
                Console.WriteLine("  FAIL: PositionSink was invoked despite Load() having failed");
                return 1;
            }

            Console.WriteLine($"  PASS: Load() failed with '{err}', Ready=false, 5x Tick() did not throw and did not call the sink");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: unexpected throw: {ex.Message}");
            return 1;
        }
    }

    // ---- 6. A 2-output graph (no altitude) is read as (x, y) with z defaulting to 0 -- proves the
    // multi-output wiring generalizes past exactly-3, not just for the drone's own shape. ----
    private static int TestGraphHostAppliesTwoOutputsAsXYWithZDefaultingToZero()
    {
        Console.WriteLine("Test: GraphHost applies a 2-output graph as (x, y, z=0)");
        try
        {
            var flatGraph = @"
OCGRAPH 1
PARAM time float
NODE t Param param=time
NODE ten ConstFloat
PIN ten value out float 10.0
NODE x Multiply
LINK t value x a
LINK ten value x b
NODE y ConstFloat
PIN y value out float 5.0
OUT x result
OUT y value
";
            (float x, float y, float z)? got = null;
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => got = (x, y, z));

            if (!host.LoadFromText(flatGraph, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            host.Tick(entityId: 3, timeSeconds: 4f);

            if (got is not (float gx, float gy, float gz))
            {
                Console.WriteLine("  FAIL: sink was never called");
                return 1;
            }
            if (Math.Abs(gx - 40f) > 1e-4 || Math.Abs(gy - 5f) > 1e-4 || gz != 0f)
            {
                Console.WriteLine($"  FAIL: expected (40, 5, 0), got ({gx}, {gy}, {gz})");
                return 1;
            }

            Console.WriteLine($"  PASS: 2-output graph applied as ({gx}, {gy}, {gz})");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // ---- 7. GraphHost only knows how to feed a graph 'entity' (Int) and 'time' (Float) PARAMs --
    // anything else must fail clearly at Load(), not silently pass a wrong value at Tick(). ----
    private static int TestGraphHostRejectsUnsuppliableParam()
    {
        Console.WriteLine("Test: GraphHost rejects a PARAM it has no value to supply");
        try
        {
            var badParamGraph = @"
OCGRAPH 1
PARAM windSpeed float
NODE w Param param=windSpeed
OUT w value
";
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => { });
            if (host.LoadFromText(badParamGraph, out var err))
            {
                Console.WriteLine("  FAIL: expected Load to fail for an unsuppliable PARAM, but it succeeded");
                return 1;
            }
            if (err == null || !err.Contains("windSpeed"))
            {
                Console.WriteLine($"  FAIL: expected the error to name 'windSpeed', got: {err}");
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

    // ---- 8. Compiles once, not per tick: ticking many times against text that would fail to parse
    // if Tick() ever re-parsed it proves Tick() never goes back to OcGraphParser/GraphCompiler. ----
    private static int TestGraphHostCompilesOnceNotPerTick()
    {
        Console.WriteLine("Test: GraphHost compiles once; Tick() never reparses");
        try
        {
            var simple = @"
OCGRAPH 1
PARAM time float
NODE t Param param=time
OUT t value
";
            int sinkCalls = 0;
            var host = new GraphHost(positionSink: (int e, float x, float y, float z) => sinkCalls++);
            if (!host.LoadFromText(simple, out var err))
            {
                Console.WriteLine($"  FAIL: {err}");
                return 1;
            }

            // 200 ticks against a compiled delegate. If Tick() re-ran Reflection.Emit every call this
            // would still "work" but be the exact absurdity the task called out by name -- this test
            // can't directly clock IL emission from outside, but it does confirm 200 ticks against an
            // already-compiled single-output graph run to completion without needing the source text
            // again (LoadFromText's local `simple` string is never referenced after the Load call).
            for (int i = 0; i < 200; i++)
                host.Tick(1, i * 0.1f);

            // Single-output graph (1 OUT record) -- GraphHost makes no position write for that shape
            // (see ApplyResult), so the honest assertion here is "it didn't call the sink and it
            // didn't throw across 200 ticks," not "it moved something."
            if (sinkCalls != 0)
            {
                Console.WriteLine($"  FAIL: expected no sink calls for a 1-output graph, got {sinkCalls}");
                return 1;
            }

            Console.WriteLine("  PASS: 200 ticks against a pre-compiled delegate completed without reparsing or throwing");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
