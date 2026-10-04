// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the four parity-backlog nodes: CreateEntity / FindEntity (SetName's own name= family)
// and IsPhysicsReady / GetFixedStep (the two Physics status reads).
//
// WHY name= NEEDED NO NEW PARSING. SetName already carries Node.NameValue off the NODE line, and the
// C++ writer round-trips every key=value through one generic extraTokens path -- so these two nodes
// reuse that machinery verbatim. These tests assert that reuse actually holds, because "it should
// just work" is exactly the claim that has been wrong before in this file's neighbours.
//
// The native-call tests follow SaveLoadGameNodeTests.cs's own proof shape: this bare process has no
// engine binary beside it, so a real P/Invoke throws EntryPointNotFoundException naming the exact
// symbol -- which is what proves the attribute reached the call rather than being dropped.
using System;
using Aver.Graph;

static class NameAndPhysicsStatusNodeTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestPinShapes();
        failures += TestCreateEntityRefusedByPullCompiler();
        failures += TestCreateEntityWithoutNameFailsToCompile();
        failures += TestFindEntityWithoutNameFailsToCompile();
        failures += TestFindEntityReachesTheNativeLookup();
        failures += TestPhysicsStatusReadsCompileOnBothCompilers();
        return failures;
    }

    private static int TestPinShapes()
    {
        Console.WriteLine("Test: the four new nodes have exactly the pins the palette declares");
        try
        {
            const string text =
                "OCGRAPH 1\n" +
                "NODE ce CreateEntity name=Rock\n" +
                "NODE fe FindEntity name=Rock\n" +
                "NODE pr IsPhysicsReady\n" +
                "NODE fs GetFixedStep\n" +
                "OUT fe entity\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var ce = graph.Nodes["ce"];
            bool ceOk =
                ce.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                ce.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                ce.Pins.Find(p => p.Name == "entity" && p.IsOutput && p.Type == PinType.Int) != null &&
                ce.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                ce.Pins.Count == 4 && ce.NameValue == "Rock";
            if (!ceOk)
            {
                Console.WriteLine($"  FAIL: CreateEntity pins/name: [{string.Join(", ", ce.Pins.ConvertAll(p => p.Name))}] name='{ce.NameValue}'");
                return 1;
            }

            var fe = graph.Nodes["fe"];
            bool feOk =
                fe.Pins.Find(p => p.Name == "entity" && p.IsOutput && p.Type == PinType.Int) != null &&
                fe.Pins.Find(p => p.Name == "found" && p.IsOutput && p.Type == PinType.Bool) != null &&
                fe.Pins.Count == 2 && fe.NameValue == "Rock" &&
                fe.Pins.TrueForAll(p => p.Type != PinType.Exec);
            if (!feOk)
            {
                Console.WriteLine($"  FAIL: FindEntity pins/name: [{string.Join(", ", fe.Pins.ConvertAll(p => p.Name))}] name='{fe.NameValue}'");
                return 1;
            }

            var pr = graph.Nodes["pr"];
            var fs = graph.Nodes["fs"];
            bool statusOk =
                pr.Pins.Count == 1 && pr.Pins[0].Name == "ready" && pr.Pins[0].Type == PinType.Bool && pr.Pins[0].IsOutput &&
                fs.Pins.Count == 1 && fs.Pins[0].Name == "seconds" && fs.Pins[0].Type == PinType.Float && fs.Pins[0].IsOutput;
            if (!statusOk)
            {
                Console.WriteLine("  FAIL: physics status pins are not the single declared output each");
                return 1;
            }

            Console.WriteLine("  PASS: all four pin shapes, and name= parsed onto BOTH new name-family nodes");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // CreateEntity mints an entity, so it must be refused by the pure-dataflow compiler exactly as
    // Spawn is -- otherwise a topological pass would create one per invocation, forever.
    private static int TestCreateEntityRefusedByPullCompiler()
    {
        Console.WriteLine("Test: CreateEntity in a no-ENTRY (pure-PULL) graph is refused, naming CreateEntity");
        try
        {
            const string text = "OCGRAPH 1\nNODE ce CreateEntity name=Rock\nOUT ce entity\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected Compile() to refuse an ungated CreateEntity, but it succeeded");
                return 1;
            }
            if (cerr == null || cerr.IndexOf("CreateEntity", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming CreateEntity, got: {cerr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {cerr}");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestCreateEntityWithoutNameFailsToCompile()
    {
        Console.WriteLine("Test: CreateEntity with no name= attribute fails to compile, naming the node");
        return MissingNameCase(
            "OCGRAPH 1\nNODE tick OnTick\nNODE ce CreateEntity\nLINK tick.exec ce.exec\nENTRY tick OnTick\nOUT ce entity\n",
            usePush: true, expect: "CreateEntity");
    }

    private static int TestFindEntityWithoutNameFailsToCompile()
    {
        Console.WriteLine("Test: FindEntity with no name= attribute fails to compile, naming the node");
        return MissingNameCase("OCGRAPH 1\nNODE fe FindEntity\nOUT fe entity\n", usePush: false, expect: "FindEntity");
    }

    private static int MissingNameCase(string text, bool usePush, string expect)
    {
        try
        {
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                // Refusing at PARSE is an equally acceptable place, as long as it is caught.
                Console.WriteLine($"  PASS: refused at parse: {perr}");
                return 0;
            }
            var compiler = new GraphCompiler(graph);
            var fn = usePush ? compiler.CompileEntryPoint("OnTick", out var cerr) : compiler.Compile(out cerr);
            if (fn != null)
            {
                Console.WriteLine($"  FAIL: expected compilation to fail with no name=, but it succeeded");
                return 1;
            }
            if (cerr == null || cerr.IndexOf(expect, StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming {expect}, got: {cerr}");
                return 1;
            }
            Console.WriteLine($"  PASS: {cerr}");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // Proves name= actually reached the native lookup rather than being parsed and dropped -- the
    // failure mode a pin-shape test alone would not catch.
    private static int TestFindEntityReachesTheNativeLookup()
    {
        Console.WriteLine("Test: FindEntity's name= reaches a real native call naming aver_scene_find");
        try
        {
            const string text = "OCGRAPH 1\nNODE fe FindEntity name=Rock\nOUT fe found\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: compile: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                var got = fn();
                Console.WriteLine($"  FAIL: expected a real native call to throw here, but it returned {got}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_find"))
                {
                    Console.WriteLine($"  FAIL: threw, but not naming aver_scene_find: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_find', proving name= reached Game.Find: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.InnerException?.Message ?? ex.Message}");
            return 1;
        }
    }

    // BOTH COMPILERS -- the single most repeated bug shape in GraphCompiler.cs's history is a node
    // implemented in only one of the two paths.
    private static int TestPhysicsStatusReadsCompileOnBothCompilers()
    {
        Console.WriteLine("Test: IsPhysicsReady and GetFixedStep compile on BOTH compilers (they are pure reads)");
        try
        {
            const string pull = "OCGRAPH 1\nNODE pr IsPhysicsReady\nNODE fs GetFixedStep\nOUT pr ready\nOUT fs seconds\n";
            if (!OcGraphParser.Parse(pull, out var g1, out var e1))
            {
                Console.WriteLine($"  FAIL: Parse error: {e1}");
                return 1;
            }
            if (new GraphCompiler(g1).Compile(out var c1) == null)
            {
                Console.WriteLine($"  FAIL: Compile() refused the pure reads: {c1}");
                return 1;
            }

            const string push = "OCGRAPH 1\nNODE tick OnTick\nENTRY tick OnTick\nNODE pr IsPhysicsReady\nNODE fs GetFixedStep\nOUT pr ready\nOUT fs seconds\n";
            if (!OcGraphParser.Parse(push, out var g2, out var e2))
            {
                Console.WriteLine($"  FAIL: Parse error: {e2}");
                return 1;
            }
            if (new GraphCompiler(g2).CompileEntryPoint("OnTick", out var c2) == null)
            {
                Console.WriteLine($"  FAIL: CompileEntryPoint() refused the pure reads: {c2}");
                return 1;
            }
            Console.WriteLine("  PASS: both compilers accept both status reads");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }
}
