// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for Spawn -- the "spawning/destroying an entity" node-vocabulary gap: a
// Spawn(className, x, y, z) exec node returning the new entity id as a data pin, wrapping the
// managed Aver.Framework.Actors.Spawn (see GraphInterop.SpawnForGraph's own comment for why the class
// name is resolved by NAME at invocation time, not baked to a handle at compile time). See
// GraphCompiler.cs's EmitExecSpawn/IsExecCapableSpawnType comments for the design these tests prove,
// and OcGraphParser.cs's "Spawn" section for the pin shapes.
//
// EXEC-ONLY, MORE STRICTLY THAN SetField/SetFieldVec3/Raycast. Every other side-effecting or
// costly-query node this codebase has (SetField, SetFieldVec3) still gets a case in Compile()'s own
// PULL/topological-pass switch (EmitNode), because that pass runs each node EXACTLY ONCE PER
// INVOCATION of the compiled delegate -- fine for an idempotent field overwrite, but not fine for
// entity creation, which would then run once per Tick() with no branch structure available to gate it.
// So Spawn is the first node type this codebase refuses in Compile() ENTIRELY, not merely restricted
// in EmitPullOutput -- see TestSpawnRefusedByPullCompilerEvenWithNoLinkAtAll below for the proof.
//
// SAME HONEST NATIVE-CALL LIMITATION AS EVERY OTHER NODE THIS SLICE HAS ADDED (see NewNodeTests.cs's
// own header comment for the full explanation): nothing in THIS test process boots a live scripting
// host, so "emits a real native call" below proves exactly what InputKey's own test proves -- the
// EMITTED IL performs a REAL call reaching Aver.Framework's class registry (not a hardcoded stub) by
// observing the SPECIFIC failure that call produces. GraphInterop.SpawnForGraph's first native call is
// ActorClass.Find -> Fw.aver_fw_class_find -- an ordinary DllImport extern in the SAME Aver.Framework.dll
// this test process already references managed-only (exactly InputKey's own situation, not Raycast's:
// Aver.Framework.dll's managed copy sits where NativeResolver looks for the native one, so the failure
// is EntryPointNotFoundException naming the missing export, not DllNotFoundException).
using System;
using Aver.Graph;

static class SpawnNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestSpawnDefaultPinsShape();
        failures += TestSpawnMissingClassAttributeFailsCompile();
        failures += TestSpawnEmitsRealNativeCallOnExecChain();
        failures += TestSpawnRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestSpawnPulledWithoutExecVisitFailsClearly();
        failures += TestSpawnWithNoEntityOutputPinStillCompiles();

        return failures;
    }

    private static int TestSpawnDefaultPinsShape()
    {
        Console.WriteLine("Test: Spawn's default pins are exec-in + x/y/z:float-in + then(exec-out) + entity(int-out)");
        try
        {
            var text = "OCGRAPH 1\nNODE s Spawn class=Widget\nOUT s entity\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["s"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "x" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "y" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "z" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "entity" && p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Count == 6;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.ClassName != "Widget")
            {
                Console.WriteLine($"  FAIL: expected ClassName 'Widget' from class= attribute, got '{node.ClassName}'");
                return 1;
            }
            Console.WriteLine("  PASS: 6 pins (1 exec-in, 3 float-in, 1 exec-out, 1 int-out), class= parsed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A Spawn node with no class= attribute names nothing to spawn -- required at COMPILE time (like
    // GetField/SetField's own field= check), so this fails loudly at compile rather than quietly
    // returning entity 0 at runtime for a reason nobody could see.
    private static int TestSpawnMissingClassAttributeFailsCompile()
    {
        Console.WriteLine("Test: Spawn node with no class= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE sp Spawn
LINK tick.exec sp.exec
ENTRY tick OnTick
OUT sp entity
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
                Console.WriteLine("  FAIL: expected compilation to fail (Spawn has no class= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("class="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing class= attribute, got: {compileErr}");
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

    // The real end-to-end shape: an ENTRY OnTick graph whose exec chain reaches Spawn, feeds it x/y/z
    // via Consts, and reads the new entity id back through OUT after the chain finishes -- proving the
    // IL compiles (the _execLocals write-then-OUT-read round trip balances), and that INVOKING it makes
    // a genuine call into Aver.Framework's class registry rather than returning a stubbed constant.
    private static int TestSpawnEmitsRealNativeCallOnExecChain()
    {
        Console.WriteLine("Test: Spawn (PUSH/CompileEntryPoint, via the exec chain) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE cx ConstFloat value=1.0
NODE cy ConstFloat value=2.0
NODE cz ConstFloat value=3.0
NODE sp Spawn class=Widget
LINK tick.exec sp.exec
LINK cx.value sp.x
LINK cy.value sp.y
LINK cz.value sp.z
ENTRY tick OnTick
OUT sp entity
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                int unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_class_find"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_class_find: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_class_find' via the exec chain: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // THE CORE DESIGN DECISION THIS SLICE MAKES, PROVEN DIRECTLY: a Spawn node in a graph with NO ENTRY
    // at all (so Compile(), the pure-PULL compiler, is the only one that could ever run it) must fail
    // to compile, with a message naming Spawn specifically -- not silently compile into something that
    // spawns an entity on every single invocation, and not silently do nothing via IsExecOnlyNodeType.
    // See EmitNode's own "spawn" case comment for the full reasoning (a stray Spawn ticked every frame
    // by a no-ENTRY GraphHost graph would create a new entity every tick, forever, with nothing able to
    // gate it -- unlike SetField/SetFieldVec3, which DO get a case in this same switch, because
    // overwriting a field with the same value every tick is harmless, not dangerous).
    private static int TestSpawnRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: Spawn in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, Spawn-naming error");
        try
        {
            var text = @"
OCGRAPH 1
NODE cx ConstFloat value=1.0
NODE cy ConstFloat value=2.0
NODE cz ConstFloat value=3.0
NODE sp Spawn class=Widget
LINK cx.value sp.x
LINK cy.value sp.y
LINK cz.value sp.z
OUT sp entity
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
                Console.WriteLine("  FAIL: expected Compile() to refuse a Spawn node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("Spawn", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the Spawn node type, got: {compileErr}");
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

    // The OTHER half of the exec-only constraint: even INSIDE an ENTRY-driven graph (so CompileEntryPoint
    // is the compiler running), a Spawn node the exec chain never actually visits (no LINK from any exec
    // pin into sp.exec) must not be readable as a data value via OUT -- pulling it would either run the
    // spawn on demand (once per pull, silently more than once if pulled from two places) or, if it were
    // simply left unhandled, read whatever garbage happens to be at an uninitialized local. EmitPullOutput's
    // now-three-way-generalised side-effect refusal (SetField/SetFieldVec3/Spawn) catches this at compile
    // time instead, exactly mirroring Raycast's own TestRaycastPulledWithoutExecVisitFailsClearly.
    private static int TestSpawnPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: Spawn never wired into the exec chain, but pulled via OUT, fails clearly (not silently 0)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE sp Spawn class=Widget
ENTRY start OnStart
OUT sp entity
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sp.entity pulled with no exec visit ever reaching sp), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("Spawn", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming Spawn and its side effect, got: {compileErr}");
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

    // "entity" is an OPTIONAL output, exactly like SetField's "success" -- a graph author who only
    // wants the side effect (spawn something, don't care what id it got) can omit it, and EmitExecSpawn
    // discards the return value with a Pop rather than requiring a local to store it in. Proven by
    // compiling (not invoking, since invoking always throws at the native boundary regardless): a
    // hand-authored Spawn node with explicit PIN records that do NOT declare "entity" as an output must
    // still produce balanced IL.
    private static int TestSpawnWithNoEntityOutputPinStillCompiles()
    {
        Console.WriteLine("Test: Spawn with no 'entity' output pin still compiles (Pop path, not required like Raycast's outputs)");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE sp Spawn class=Widget
PIN sp exec in exec
PIN sp x in float
PIN sp y in float
PIN sp z in float
PIN sp then out exec
LINK tick.exec sp.exec
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sp"];
            if (node.Pins.Exists(p => p.Name == "entity"))
            {
                Console.WriteLine("  FAIL: test fixture is wrong -- 'sp' should have no 'entity' pin declared");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled == null)
            {
                Console.WriteLine($"  FAIL: expected compilation to succeed (entity output is optional), got: {compileErr}");
                return 1;
            }
            Console.WriteLine("  PASS: compiled with no 'entity' output pin declared (Pop-discard path)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
