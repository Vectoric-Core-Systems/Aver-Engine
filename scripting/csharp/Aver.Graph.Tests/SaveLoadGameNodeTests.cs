// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for SaveGame / LoadGame -- the last two Aver Node entries the persistence work left open.
// Both wrap Game.SaveGame(path)/LoadGame(path) (Game.cs), which already exist, are already tested at
// the C++ level (tests/save), and are already reachable from C# directly -- these nodes are the
// Aver Node surface on top, nothing more. See GraphCompiler.cs's EmitExecSaveLoad/
// IsExecCapableSaveLoadType comments, OcGraphParser.cs's "savegame"/"loadgame" section, and
// GraphInterop.cs's SaveGameForGraph/LoadGameForGraph for the design these tests prove.
//
// ONE PREDICATE, ONE EMITTER FOR BOTH TYPES, unlike FireEvent/Spawn/CharacterMove each getting their
// own -- see IsExecCapableSaveLoadType's own comment for why that split is right here. Tested
// together in one file for the identical reason GroupBNodeTests.cs covers SetMesh+SetMaterial
// together: one emitter, one set of tests, run twice.
//
// REFUSED BY THE PULL COMPILER ENTIRELY, mirroring Spawn/CharacterMove/FireEvent's own tests --
// LoadGame is the WORST-CASE member of that whole family (it does not write one field or spawn one
// entity, it replaces the world), so if anything in this family deserved that refusal, this does.
//
// THE NATIVE-CALL TESTS FOLLOW Program.cs's OWN GetField/SetField PRECEDENT (see its class comment
// above TestGetFieldEmitsRealNativeCall): this test bin directory happens to hold the MANAGED
// Aver.Framework.dll sitting exactly where NativeResolver.cs looks for the NATIVE one, so
// NativeLibrary.TryLoad succeeds against it (a managed assembly is still a loadable PE/COFF image)
// and the P/Invoke fails one step later, on "find the export", with EntryPointNotFoundException
// naming the exact symbol (aver_fw_save_write / aver_fw_save_load). That is only reachable if the
// emitted IL genuinely issued that call with the path= literal on the stack -- proof the wiring is
// real, not a stub, without booting a native scene.
using System;
using Aver.Graph;

static class SaveLoadGameNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- compile-time shape, both types ----------------------------------------------------
        failures += TestSaveGameDefaultPinsShapeAndPathAttributeValue();
        failures += TestLoadGameDefaultPinsShapeAndPathAttributeValue();
        failures += TestSaveGameMissingPathAttributeFailsCompile();
        failures += TestLoadGameMissingPathAttributeFailsCompile();
        failures += TestSaveGameWithNoSuccessOutputPinStillCompiles();

        // ---- refused by the PULL compiler, exactly like Spawn/CharacterMove/FireEvent ----------
        failures += TestSaveGameRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestLoadGameRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestSaveGamePulledWithoutExecVisitFailsClearly();
        failures += TestLoadGamePulledWithoutExecVisitFailsClearly();

        // ---- the emitted IL genuinely calls Game.SaveGame/LoadGame, path= literal and all --------
        failures += TestSaveGameOnExecChainEmitsRealNativeCallWithThePathLiteral();
        failures += TestLoadGameOnExecChainEmitsRealNativeCallWithThePathLiteral();

        return failures;
    }

    // =================================================================================================
    // COMPILE-TIME SHAPE
    // =================================================================================================

    private static int TestSaveGameDefaultPinsShapeAndPathAttributeValue()
    {
        Console.WriteLine("Test: SaveGame's default pins are exec-in + then(exec-out) + success(bool-out), NO entity pin, path= parsed");
        try
        {
            var text = "OCGRAPH 1\nNODE sg SaveGame path=Saves/Slot1.ocsave\nOUT sg success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sg"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 3;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.SavePath != "Saves/Slot1.ocsave")
            {
                Console.WriteLine($"  FAIL: expected SavePath 'Saves/Slot1.ocsave' from path= attribute, got '{node.SavePath}'");
                return 1;
            }
            Console.WriteLine("  PASS: 3 pins (exec-in, exec-out, bool-out), no entity pin, path= parsed to the exact literal");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestLoadGameDefaultPinsShapeAndPathAttributeValue()
    {
        Console.WriteLine("Test: LoadGame's default pins are exec-in + then(exec-out) + success(bool-out), NO entity pin, path= parsed");
        try
        {
            var text = "OCGRAPH 1\nNODE lg LoadGame path=Saves/Slot1.ocsave\nOUT lg success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["lg"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 3;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.SavePath != "Saves/Slot1.ocsave")
            {
                Console.WriteLine($"  FAIL: expected SavePath 'Saves/Slot1.ocsave' from path= attribute, got '{node.SavePath}'");
                return 1;
            }
            Console.WriteLine("  PASS: 3 pins (exec-in, exec-out, bool-out), no entity pin, path= parsed to the exact literal");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSaveGameMissingPathAttributeFailsCompile()
    {
        Console.WriteLine("Test: SaveGame node with no path= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE sg SaveGame
LINK tick.exec sg.exec
ENTRY tick OnTick
OUT sg success
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
                Console.WriteLine("  FAIL: expected compilation to fail (SaveGame has no path= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("path="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing path= attribute, got: {compileErr}");
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

    private static int TestLoadGameMissingPathAttributeFailsCompile()
    {
        Console.WriteLine("Test: LoadGame node with no path= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE lg LoadGame
LINK tick.exec lg.exec
ENTRY tick OnTick
OUT lg success
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
                Console.WriteLine("  FAIL: expected compilation to fail (LoadGame has no path= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("path="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing path= attribute, got: {compileErr}");
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

    // "success" is OPTIONAL, exactly like FireEvent's "fired" -- an author who only wants the side
    // effect (save it, don't care whether the write landed) can omit it, and EmitExecSaveLoad
    // discards the outcome with a Pop.
    private static int TestSaveGameWithNoSuccessOutputPinStillCompiles()
    {
        Console.WriteLine("Test: SaveGame with no 'success' output pin still compiles (Pop path)");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE sg SaveGame path=Saves/Slot1.ocsave
PIN sg exec in exec
PIN sg then out exec
LINK tick.exec sg.exec
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sg"];
            node.SavePath = "Saves/Slot1.ocsave"; // no NODE-line attribute to parse in this hand-written PIN fixture
            if (node.Pins.Exists(p => p.Name == "success"))
            {
                Console.WriteLine("  FAIL: test fixture is wrong -- 'sg' should have no 'success' pin declared");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled == null)
            {
                Console.WriteLine($"  FAIL: expected compilation to succeed ('success' output is optional), got: {compileErr}");
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

    // =================================================================================================
    // REFUSED BY THE PULL COMPILER -- the worst-case member of the Spawn/CharacterMove/FireEvent
    // family: LoadGame does not write one field or spawn one entity, it replaces the world.
    // =================================================================================================

    private static int TestSaveGameRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: SaveGame in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, SaveGame-naming error");
        try
        {
            var text = "OCGRAPH 1\nNODE sg SaveGame path=Saves/Slot1.ocsave\nOUT sg success\n";
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
                Console.WriteLine("  FAIL: expected Compile() to refuse a SaveGame node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("SaveGame", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the SaveGame node type, got: {compileErr}");
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

    private static int TestLoadGameRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: LoadGame in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, LoadGame-naming error");
        try
        {
            var text = "OCGRAPH 1\nNODE lg LoadGame path=Saves/Slot1.ocsave\nOUT lg success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected Compile() to refuse a LoadGame node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("LoadGame", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the LoadGame node type, got: {compileErr}");
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

    private static int TestSaveGamePulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SaveGame never wired into the exec chain, but pulled via OUT, fails clearly (not silently false)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE sg SaveGame path=Saves/Slot1.ocsave
ENTRY start OnStart
OUT sg success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sg.success pulled with no exec visit ever reaching sg), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SaveGame", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming SaveGame and its side effect, got: {compileErr}");
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

    private static int TestLoadGamePulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: LoadGame never wired into the exec chain, but pulled via OUT, fails clearly (not silently false)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE lg LoadGame path=Saves/Slot1.ocsave
ENTRY start OnStart
OUT lg success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (lg.success pulled with no exec visit ever reaching lg), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("LoadGame", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming LoadGame and its side effect, got: {compileErr}");
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

    // =================================================================================================
    // THE EMITTED IL GENUINELY CALLS Game.SaveGame/LoadGame -- see this file's own header comment for
    // why EntryPointNotFoundException, naming the exact P/Invoke symbol, is the proof available in
    // this bare process, and why it is strong evidence rather than a workaround.
    // =================================================================================================

    private static int TestSaveGameOnExecChainEmitsRealNativeCallWithThePathLiteral()
    {
        Console.WriteLine("Test: SaveGame on a real ENTRY-driven exec chain emits a real native call naming aver_fw_save_write");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE sg SaveGame path=Saves/Slot1.ocsave
LINK tick.exec sg.exec
ENTRY tick OnTick
OUT sg success
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
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
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_save_write"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_save_write: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_save_write', proving path= reached Game.SaveGame(path): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestLoadGameOnExecChainEmitsRealNativeCallWithThePathLiteral()
    {
        Console.WriteLine("Test: LoadGame on a real ENTRY-driven exec chain emits a real native call naming aver_fw_save_load");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE lg LoadGame path=Saves/Slot1.ocsave
LINK tick.exec lg.exec
ENTRY tick OnTick
OUT lg success
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
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
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_save_load"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_save_load: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_save_load', proving path= reached Game.LoadGame(path): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }
}
