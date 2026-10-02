// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for CharacterMove -- the last Blueprint-parity node: one coarse, exec-only wrapper around
// AverCharacter.Drive (via AverCharacter.DriveFromGraph -> GraphInterop.CharacterMoveForGraph). See
// GraphCompiler.cs's EmitExecCharacterMove/IsExecCapableCharacterMoveType comments for the design
// these tests prove, and OcGraphParser.cs's "CharacterMove" section for the pin shapes.
//
// COMPILE-TIME SHAPE MIRRORS SPAWN'S OWN TESTS (SpawnNodeTests.cs): exec-only, refused by the PULL
// compiler's topological pass ENTIRELY (not merely restricted in EmitPullOutput -- see
// TestCharacterMoveRefusedByPullCompilerEvenWithNoEntryAtAll), and refused when pulled as a bare data
// value with no exec visit (TestCharacterMovePulledWithoutExecVisitFailsClearly). Unlike Spawn's
// class=, there is no NODE-line attribute at all -- every input is an ordinary pin -- so there is no
// "missing attribute" compile failure to test here.
//
// WHAT'S DIFFERENT FROM EVERY OTHER NODE THIS SLICE HAS ADDED, AND WHY IT ACTUALLY HELPS: every prior
// GraphInterop wrapper (RaycastForGraph, SpawnForGraph, InputKey, ...) makes an UNCONDITIONAL native
// P/Invoke call, so this test process (which boots no live engine -- see NewNodeTests.cs's own header
// comment for the full explanation of why) can only prove "real dispatch happened" by catching the
// specific EntryPointNotFoundException/DllNotFoundException that call produces.
// GraphInterop.CharacterMoveForGraph is different: its FIRST action is Actors.Get(entity), which is
// PURE C# (Entity.IsValid is a field compare; Aver.Framework.Actors.Resolver is a plain internal
// static delegate field, uninstalled -- null -- in a bare process with no scripting host bootstrap).
// That means the "no live actor" and "not an AverCharacter" failure paths run to completion, for
// real, inside this test process, with no native call anywhere near them -- so
// TestCharacterMoveNoActorFailsVisibly and TestCharacterMoveWrongActorTypeFailsVisibly below are
// genuine end-to-end behavioural proofs (invoke the compiled delegate, read back a REAL success=false
// and a REAL logged warning), not compile-time-only checks or "it threw the expected exception name"
// substitutes. Only the THIRD path -- entity really is a live AverCharacter -- reaches
// AverCharacter.Drive's first statement, Fw.aver_fw_set_view, a genuine P/Invoke; that one inherits
// the same honest limitation as Spawn/Raycast/InputKey (see
// TestCharacterMoveRealCharacterReachesDrive).
//
// Actors.Resolver is set via reflection (BindingFlags.NonPublic) rather than a direct C# reference:
// Aver.Framework.csproj grants InternalsVisibleTo to Aver.Scripting.Bridge and Aver.Graph only, not
// Aver.Graph.Tests. Reflection bypasses that (as it always can for a same-process, fully-trusted .NET
// Core app) without widening the grant just for a test. EVERY test that installs a fake resolver
// saves and restores the previous value in a finally block -- this suite's own Program.cs runs every
// test suite sequentially in ONE process with no isolation, so a leaked fake resolver would silently
// corrupt any later test that happens to touch Actors.Get with a colliding entity handle.
using System;
using System.IO;
using System.Reflection;
using Aver.Framework;
using Aver.Graph;

static class CharacterMoveNodeTests
{
    // A live actor that is deliberately NOT an AverCharacter -- stands in for "the level/graph author
    // pointed this node at the wrong entity" (a lamp, a pickup, anything that isn't a character).
    private sealed class NonCharacterTestActor : AverActor { }

    // A real, minimal AverCharacter -- AverCharacter declares no abstract members, so this needs no
    // body at all. Used only to prove real dispatch reaches Drive(); its fields are left at their
    // documented defaults (CameraViewMode = ThirdPerson, EyeHeight = 160, BoomLength = 450).
    private sealed class TestCharacter : AverCharacter { }

    private static FieldInfo ResolverField =>
        typeof(Actors).GetField("Resolver", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException(
            "Aver.Framework.Actors.Resolver was not found by reflection -- this test's fake-resolver " +
            "installation relies on it existing exactly as GraphInterop.CharacterMoveForGraph's own " +
            "Actors.Get call does");

    public static int RunAll()
    {
        int failures = 0;

        failures += TestCharacterMoveDefaultPinsShape();
        failures += TestCharacterMoveRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestCharacterMovePulledWithoutExecVisitFailsClearly();
        failures += TestCharacterMoveWithNoSuccessOutputPinStillCompiles();
        failures += TestCharacterMoveNoActorFailsVisibly();
        failures += TestCharacterMoveWrongActorTypeFailsVisibly();
        failures += TestCharacterMoveRealCharacterReachesDrive();

        return failures;
    }

    private static int TestCharacterMoveDefaultPinsShape()
    {
        Console.WriteLine("Test: CharacterMove's default pins are exec-in + entity:int-in + dt/forward/right/yawDelta/pitchDelta:float-in + then(exec-out) + success(bool-out)");
        try
        {
            var text = "OCGRAPH 1\nNODE cm CharacterMove\nOUT cm success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["cm"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "dt" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "forward" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "right" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "yawDelta" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "pitchDelta" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 9;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 9 pins (1 exec-in, 1 int-in, 5 float-in, 1 exec-out, 1 bool-out), no NODE-line attribute needed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Mirrors TestSpawnRefusedByPullCompilerEvenWithNoEntryAtAll: a CharacterMove node in a graph
    // with NO ENTRY at all (so Compile(), the pure-PULL compiler, is the only one that could ever run
    // it) must fail to compile, with a message naming CharacterMove specifically.
    private static int TestCharacterMoveRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: CharacterMove in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, CharacterMove-naming error");
        try
        {
            var text = @"
OCGRAPH 1
NODE ent ConstInt value=1
NODE dt ConstFloat value=0.0
NODE fwd ConstFloat value=0.0
NODE rgt ConstFloat value=0.0
NODE yaw ConstFloat value=0.0
NODE pit ConstFloat value=0.0
NODE cm CharacterMove
LINK ent.value cm.entity
LINK dt.value cm.dt
LINK fwd.value cm.forward
LINK rgt.value cm.right
LINK yaw.value cm.yawDelta
LINK pit.value cm.pitchDelta
OUT cm success
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
                Console.WriteLine("  FAIL: expected Compile() to refuse a CharacterMove node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("CharacterMove", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the CharacterMove node type, got: {compileErr}");
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

    // Mirrors TestSpawnPulledWithoutExecVisitFailsClearly: even inside an ENTRY-driven graph, a
    // CharacterMove node the exec chain never actually visits must not be readable as a data value
    // via OUT.
    private static int TestCharacterMovePulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: CharacterMove never wired into the exec chain, but pulled via OUT, fails clearly (not silently false)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE ent ConstInt value=1
NODE dt ConstFloat value=0.0
NODE fwd ConstFloat value=0.0
NODE rgt ConstFloat value=0.0
NODE yaw ConstFloat value=0.0
NODE pit ConstFloat value=0.0
NODE cm CharacterMove
LINK ent.value cm.entity
LINK dt.value cm.dt
LINK fwd.value cm.forward
LINK rgt.value cm.right
LINK yaw.value cm.yawDelta
LINK pit.value cm.pitchDelta
ENTRY start OnStart
OUT cm success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (cm.success pulled with no exec visit ever reaching cm), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("CharacterMove", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming CharacterMove and its side effect, got: {compileErr}");
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

    // "success" is OPTIONAL, exactly like Spawn's "entity" -- a graph author who only wants the side
    // effect (drive the character, don't care to branch on whether it worked) can omit it, and
    // EmitExecCharacterMove discards the return value with a Pop.
    private static int TestCharacterMoveWithNoSuccessOutputPinStillCompiles()
    {
        Console.WriteLine("Test: CharacterMove with no 'success' output pin still compiles (Pop path)");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE cm CharacterMove
PIN cm exec in exec
PIN cm entity in int
PIN cm dt in float
PIN cm forward in float
PIN cm right in float
PIN cm yawDelta in float
PIN cm pitchDelta in float
PIN cm then out exec
LINK tick.exec cm.exec
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["cm"];
            if (node.Pins.Exists(p => p.Name == "success"))
            {
                Console.WriteLine("  FAIL: test fixture is wrong -- 'cm' should have no 'success' pin declared");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled == null)
            {
                Console.WriteLine($"  FAIL: expected compilation to succeed (success output is optional), got: {compileErr}");
                return 1;
            }
            Console.WriteLine("  PASS: compiled with no 'success' output pin declared (Pop-discard path)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static string CharacterMoveGraphText(int entity) => $@"
OCGRAPH 1
NODE tick OnTick
NODE ent ConstInt value={entity}
NODE dt ConstFloat value=0.016
NODE fwd ConstFloat value=1.0
NODE rgt ConstFloat value=0.0
NODE yaw ConstFloat value=2.0
NODE pit ConstFloat value=-1.0
NODE cm CharacterMove
LINK tick.exec cm.exec
LINK ent.value cm.entity
LINK dt.value cm.dt
LINK fwd.value cm.forward
LINK rgt.value cm.right
LINK yaw.value cm.yawDelta
LINK pit.value cm.pitchDelta
ENTRY tick OnTick
OUT cm success
";

    // THE FIRST OF THE TWO "FAIL VISIBLY" REQUIREMENTS THE TASK NAMES: an entity with no live actor
    // bound to it at all. A REAL invocation of the compiled delegate -- not a mocked-out shortcut --
    // proves both halves of "visibly": a genuine success=false on the pin, AND a genuine Log.Warn
    // line naming the entity and the reason, captured from the actual Console.Out this process's
    // Aver.Scripting.Log falls back to when no host sink is installed.
    private static int TestCharacterMoveNoActorFailsVisibly()
    {
        Console.WriteLine("Test: CharacterMove against an entity with no live actor bound returns success=false, logs why, and never throws");
        FieldInfo field = ResolverField;
        object? saved = field.GetValue(null);
        try
        {
            const int entity = 4321;
            // No host has installed a resolver in this bare test process -- Actors.Get(e) returns
            // null for ANY handle. Set explicitly (rather than trusting the ambient state) so this
            // test is correct regardless of what ran before it in this same process.
            field.SetValue(null, null);

            if (!OcGraphParser.Parse(CharacterMoveGraphText(entity), out var graph, out var err))
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

            TextWriter originalOut = Console.Out;
            var captured = new StringWriter();
            bool success;
            Console.SetOut(captured);
            try { success = fn(); }
            finally { Console.SetOut(originalOut); }

            if (success)
            {
                Console.WriteLine($"  FAIL: expected success=false (entity {entity} has no bound actor), got true");
                return 1;
            }
            string logged = captured.ToString();
            if (!logged.Contains(entity.ToString()) || !logged.Contains("no live actor is bound to it"))
            {
                Console.WriteLine($"  FAIL: expected a Log.Warn naming entity {entity} and 'no live actor is bound to it', got: {logged}");
                return 1;
            }
            Console.WriteLine($"  PASS: success=false; logged: {logged.Trim()}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            field.SetValue(null, saved);
        }
    }

    // THE SECOND OF THE TWO "FAIL VISIBLY" REQUIREMENTS: a live actor IS bound to the entity, but it
    // is not an AverCharacter. A fake resolver (installed via reflection -- see this file's header
    // comment) stands in for Actors.Resolver so this is a REAL Actors.Get(entity) -> pattern-match
    // failure, not a simulated one.
    private static int TestCharacterMoveWrongActorTypeFailsVisibly()
    {
        Console.WriteLine("Test: CharacterMove against a live actor that is NOT an AverCharacter returns success=false, logs why, and never throws");
        FieldInfo field = ResolverField;
        object? saved = field.GetValue(null);
        try
        {
            const int entity = 5555;
            var notACharacter = new NonCharacterTestActor();
            field.SetValue(null, new Func<int, AverActor?>(handle => handle == entity ? notACharacter : null));

            if (!OcGraphParser.Parse(CharacterMoveGraphText(entity), out var graph, out var err))
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

            TextWriter originalOut = Console.Out;
            var captured = new StringWriter();
            bool success;
            Console.SetOut(captured);
            try { success = fn(); }
            finally { Console.SetOut(originalOut); }

            if (success)
            {
                Console.WriteLine($"  FAIL: expected success=false (entity {entity}'s actor is not an AverCharacter), got true");
                return 1;
            }
            string logged = captured.ToString();
            if (!logged.Contains(entity.ToString()) ||
                !logged.Contains("not an AverCharacter") ||
                !logged.Contains(nameof(NonCharacterTestActor)))
            {
                Console.WriteLine($"  FAIL: expected a Log.Warn naming entity {entity}, its actual type '{nameof(NonCharacterTestActor)}', and 'not an AverCharacter', got: {logged}");
                return 1;
            }
            Console.WriteLine($"  PASS: success=false; logged: {logged.Trim()}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            field.SetValue(null, saved);
        }
    }

    // THE POSITIVE PATH: the entity really is a live AverCharacter. This is where the honest native-
    // call limitation every other node in this codebase's own test suite already documents applies
    // (see this file's header comment, and NewNodeTests.cs's): DriveFromGraph forwards straight into
    // the existing, untouched Drive(), whose FIRST statement is Fw.aver_fw_set_view -- a real
    // P/Invoke. Invoking the compiled delegate is expected to THROW EntryPointNotFoundException
    // naming that exact export, proving real dispatch reached Drive() rather than a stub silently
    // returning success=true.
    private static int TestCharacterMoveRealCharacterReachesDrive()
    {
        Console.WriteLine("Test: CharacterMove against a real AverCharacter dispatches into Drive (via DriveFromGraph) -- a genuine call, not a stub");
        FieldInfo field = ResolverField;
        object? saved = field.GetValue(null);
        try
        {
            const int entity = 9999;
            var character = new TestCharacter();
            field.SetValue(null, new Func<int, AverActor?>(handle => handle == entity ? character : null));

            if (!OcGraphParser.Parse(CharacterMoveGraphText(entity), out var graph, out var err))
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

            try
            {
                bool unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (Drive's first statement is a real P/Invoke -- see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_set_view"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_set_view: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real dispatch reached AverCharacter.Drive via DriveFromGraph, attempting 'aver_fw_set_view': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
        finally
        {
            field.SetValue(null, saved);
        }
    }
}
