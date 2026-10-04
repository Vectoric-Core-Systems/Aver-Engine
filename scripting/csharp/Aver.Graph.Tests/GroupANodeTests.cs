// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for SetParent / SetViewEntity / SetName -- the three "one-ABI-call" node-vocabulary gaps:
// aver_scene_set_parent, aver_fw_set_view_entity, aver_scene_set_name, wrapped as graph nodes exactly
// per GraphCompiler.cs's EmitSetParent/EmitSetViewEntity/EmitSetName (PULL) and
// EmitExecSetParent/EmitExecSetViewEntity/EmitExecSetName (PUSH/exec) comments, and
// OcGraphParser.cs's "SetParent / SetViewEntity / SetName" section for the pin shapes.
//
// DISPATCHED SetField-STYLE, NOT Spawn/SetVar-STYLE -- the design decision documented at length in
// OcGraphParser.AddDefaultPins and repeated on each IsExecCapableSetXxxType predicate: no exec pins by
// default, reachable from BOTH Compile() (the pure-PULL compiler, which runs the write unconditionally
// on every invocation -- safe because reparenting/renaming/republishing-a-view-entity to the same value
// twice is idempotent, unlike Spawn creating a new entity or SetVar clobbering remembered state) and
// CompileEntryPoint() (if given exec pins by hand and wired onto an exec chain). Still fully refused
// when PULLED as a bare data value with no exec visit inside an ENTRY-driven graph -- proven below for
// all three, mirroring TestSetFieldVec3PulledAsDataValueWithoutExecVisitFailsClearly exactly.
//
// SAME HONEST NATIVE-CALL LIMITATION AS EVERY OTHER WRITE NODE THIS SUITE HAS (see SpawnNodeTests.cs's
// and Vec3FieldTests.cs's own header comments for the full explanation): nothing in THIS test process
// boots a live native scene, so "emits a real native call" below proves exactly what those tests prove
// -- the EMITTED IL performs a REAL call reaching the named native symbol (not a hardcoded stub), by
// observing the SPECIFIC EntryPointNotFoundException that call produces. It does NOT and CANNOT prove
// what aver_scene_set_parent itself returns for a cycle/self-parent/doomed-parent (there is no
// injectable seam for this call the way FieldResolver fakes GetField/SetField's field-kind lookup --
// SetParent/SetName call straight through to Aver.Scene.Native with no seam at all, exactly like
// SetViewEntity calls straight through to Aver.Framework.Fw). THAT refusal logic is proven, with a
// real, live, running native World, by tests/scene/src/SceneTest.cpp -- see its testHierarchy() (line
// ~636: "reparenting a live entity ONTO a doomed parent is refused") and testFieldRoundTrip()-adjacent
// checks (line ~775: "set_parent on a stale child is rejected"; line ~820: "set_ref refuses to
// self-parent"; line ~825: "the guarded setter DOES attach the parent") -- run as part of this slice's
// own build/test pass (scripts/build.ps1, then the built SceneTest.exe). This suite's job is the
// GRAPH's own wiring: that the real ABI call is genuinely reached (not stubbed), and that a "success"
// pin, when declared, is what receives its return value (an IL-shape fact, not a runtime one) rather
// than the return code being silently discarded -- which TestSetParentWithNoSuccessPinStillCompiles
// below proves from the other direction (the Pop-discard path balances the IL stack correctly when no
// pin captures it).
using System;
using Aver.Graph;

static class GroupANodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- SetParent ----
        failures += TestSetParentDefaultPinsShape();
        failures += TestSetParentEmitsRealNativeCallFromPullCompile();
        failures += TestSetParentOnExecChainEmitsRealNativeCall();
        failures += TestSetParentPulledWithoutExecVisitFailsClearly();
        failures += TestSetParentWithNoSuccessPinStillCompiles();

        // ---- SetViewEntity ----
        failures += TestSetViewEntityDefaultPinsShape();
        failures += TestSetViewEntityEmitsRealNativeCallFromPullCompile();
        failures += TestSetViewEntityOnExecChainEmitsRealNativeCall();

        // ---- SetName ----
        failures += TestSetNameDefaultPinsShapeAndNameAttributeValue();
        failures += TestSetNameMissingNameAttributeFailsCompile();
        failures += TestSetNameEmitsRealNativeCallFromPullCompile();
        failures += TestSetNameOnExecChainEmitsRealNativeCall();
        failures += TestSetNamePulledWithoutExecVisitFailsClearly();

        return failures;
    }

    // =================================================================================================
    // SETPARENT
    // =================================================================================================

    private static int TestSetParentDefaultPinsShape()
    {
        Console.WriteLine("Test: SetParent's default pins are child/parent:int-in, success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE sp SetParent\nPINVAL sp child 1\nPINVAL sp parent 2\nOUT sp success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sp"];
            bool ok =
                node.Pins.Find(p => p.Name == "child" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "parent" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 3; // no exec pins by default -- SetField-style, unlike Spawn
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 3 pins, no exec, types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Proves SetParent is DUAL-REACHABLE like SetField/SetFieldVec3 -- Compile()'s PULL/topological
    // walk visits it exactly once per invocation via EmitNode's own "setparent" case, with NO ENTRY
    // record anywhere in this graph (so CompileEntryPoint is not even in play).
    private static int TestSetParentEmitsRealNativeCallFromPullCompile()
    {
        Console.WriteLine("Test: SetParent (PULL/Compile(), no ENTRY at all) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM child int
PARAM parent int
NODE c Param param=child
NODE p Param param=parent
NODE sp SetParent
LINK c.value sp.child
LINK p.value sp.parent
OUT sp success
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
            if (compiled is not Func<int, int, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn(1, 2);
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_set_parent"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_set_parent: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded (SetParent is dual-reachable, like SetField), real call reached 'aver_scene_set_parent': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // SetParent given EXPLICIT exec pins by hand and wired into ENTRY OnTick -- reaches
    // EmitExecSetParent via IsExecCapableSetParentType/EmitExecNode's dispatch, exactly mirroring
    // Vec3FieldTests.cs's TestSetFieldVec3OnExecChainEmitsRealNativeCall.
    private static int TestSetParentOnExecChainEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetParent wired onto the exec chain by hand (PUSH) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM child int
PARAM parent int
NODE tick OnTick
NODE c Param param=child
NODE p Param param=parent
NODE sp SetParent
PIN sp exec in exec
PIN sp child in int
PIN sp parent in int
PIN sp then out exec
PIN sp success out bool
LINK tick.exec sp.exec
LINK c.value sp.child
LINK p.value sp.parent
ENTRY tick OnTick
OUT sp success
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<int, int, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn(1, 2);
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_set_parent"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_set_parent: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_set_parent' via the exec chain (EmitExecSetParent): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A DELIBERATE guard, tested directly: SetParent has NO case in EmitPullOutput's switch, so this
    // must be caught by the generalised side-effect refusal at the top of EmitPullOutput -- proving
    // "the PULL path must refuse a write" (the task's own instruction) holds for SetParent, not just
    // SetField/SetFieldVec3/Spawn/SetVar.
    private static int TestSetParentPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SetParent's 'success' pulled as a data value (no exec visit) fails clearly, naming SetParent");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE sp SetParent
PINVAL sp child 1
PINVAL sp parent 2
ENTRY start OnStart
OUT sp success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sp.success pulled with no exec visit ever reaching sp), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SetParent", StringComparison.OrdinalIgnoreCase) < 0 ||
                !compileErr.Contains("side effect"))
            {
                Console.WriteLine($"  FAIL: expected an error naming SetParent's side effect, got: {compileErr}");
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

    // "success" is OPTIONAL, exactly like SetField's own "success" -- a graph author who only wants
    // the reparent (does not care whether it was refused) can omit it. Proven by compiling (not
    // invoking, since invoking always throws at the native boundary regardless): EmitSetParent's own
    // "else Pop" (added deliberately -- see this method's own name and EmitSetParent's comment) keeps
    // the IL stack balanced when no local captures the return value, unlike EmitSetField's own PULL-path
    // case (GraphCompiler.cs's EmitSetField, no "else" branch at all), which would leave an int on the
    // stack if ever compiled with no declared "success" pin.
    private static int TestSetParentWithNoSuccessPinStillCompiles()
    {
        Console.WriteLine("Test: SetParent with no 'success' output pin still compiles via Compile() (Pop path, not required)");
        try
        {
            var text = @"
OCGRAPH 1
NODE sp SetParent
PIN sp child in int
PIN sp parent in int
PINVAL sp child 1
PINVAL sp parent 2
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sp"];
            if (node.Pins.Exists(p => p.Name == "success"))
            {
                Console.WriteLine("  FAIL: test fixture is wrong -- 'sp' should have no 'success' pin declared");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled == null)
            {
                Console.WriteLine($"  FAIL: expected compilation to succeed (success output is optional), got: {compileErr}");
                return 1;
            }
            Console.WriteLine("  PASS: compiled with no 'success' output pin declared (Pop-discard path balances the IL stack)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // SETVIEWENTITY
    // =================================================================================================

    private static int TestSetViewEntityDefaultPinsShape()
    {
        Console.WriteLine("Test: SetViewEntity's default pins are entity:int-in only -- no output pin, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE sv SetViewEntity\nPINVAL sv entity 1\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sv"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Count == 1 && // the ABI returns void -- no output pin exists at all
                !node.Pins.Exists(p => p.IsOutput);
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 1 pin (entity:int-in), zero output pins (void ABI, no invented success pin), no exec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetViewEntityEmitsRealNativeCallFromPullCompile()
    {
        Console.WriteLine("Test: SetViewEntity (PULL/Compile(), no ENTRY at all) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE sv SetViewEntity
LINK e.value sv.entity
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Action<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                fn(1);
                Console.WriteLine("  FAIL: expected invoking this to throw (see this file's header comment) but it returned normally");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_set_view_entity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_set_view_entity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded (void return, Action<int>), real call reached 'aver_fw_set_view_entity': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetViewEntityOnExecChainEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetViewEntity wired onto the exec chain by hand (PUSH) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE tick OnTick
NODE e Param param=entity
NODE sv SetViewEntity
PIN sv exec in exec
PIN sv entity in int
PIN sv then out exec
LINK tick.exec sv.exec
LINK e.value sv.entity
ENTRY tick OnTick
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Action<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                fn(1);
                Console.WriteLine("  FAIL: expected invoking this to throw but it returned normally");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_set_view_entity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_set_view_entity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_set_view_entity' via the exec chain (EmitExecSetViewEntity): {epEx.Message}");
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
    // SETNAME
    // =================================================================================================

    // A GENUINE VALUE ASSERTION, not just a shape check: name= is parsed straight into node.NameValue
    // and compared against the EXACT literal string, proving OcGraphParser's new "name" case (added
    // beside field=/class=/param=/var=) round-trips the value byte for byte -- this needs no native
    // call and no compile step at all, unlike every other proof in this file.
    private static int TestSetNameDefaultPinsShapeAndNameAttributeValue()
    {
        Console.WriteLine("Test: SetName's default pins are entity:int-in, success:bool-out, no exec; name= parses to the exact string");
        try
        {
            var text = "OCGRAPH 1\nNODE sn SetName name=Held_Weapon\nPINVAL sn entity 1\nOUT sn success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sn"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 2;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.NameValue != "Held_Weapon")
            {
                Console.WriteLine($"  FAIL: expected NameValue 'Held_Weapon' from name= attribute, got '{node.NameValue}'");
                return 1;
            }
            Console.WriteLine("  PASS: 2 pins, no exec, and name= parsed to the exact literal 'Held_Weapon'");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetNameMissingNameAttributeFailsCompile()
    {
        Console.WriteLine("Test: SetName node with no name= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE sn SetName
PINVAL sn entity 1
OUT sn success
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (SetName has no name= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("name="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing name= attribute, got: {compileErr}");
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

    private static int TestSetNameEmitsRealNativeCallFromPullCompile()
    {
        Console.WriteLine("Test: SetName (PULL/Compile(), no ENTRY at all) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE sn SetName name=Held_Weapon
LINK e.value sn.entity
OUT sn success
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<int, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn(1);
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_set_name"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_set_name: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded (SetName is dual-reachable, like SetField), real call reached 'aver_scene_set_name': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetNameOnExecChainEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetName wired onto the exec chain by hand (PUSH) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE tick OnTick
NODE e Param param=entity
NODE sn SetName name=Held_Weapon
PIN sn exec in exec
PIN sn entity in int
PIN sn then out exec
PIN sn success out bool
LINK tick.exec sn.exec
LINK e.value sn.entity
ENTRY tick OnTick
OUT sn success
";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<int, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn(1);
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_set_name"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_set_name: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_set_name' via the exec chain (EmitExecSetName): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetNamePulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SetName's 'success' pulled as a data value (no exec visit) fails clearly, naming SetName");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE sn SetName name=Held_Weapon
PINVAL sn entity 1
ENTRY start OnStart
OUT sn success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sn.success pulled with no exec visit ever reaching sn), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SetName", StringComparison.OrdinalIgnoreCase) < 0 ||
                !compileErr.Contains("side effect"))
            {
                Console.WriteLine($"  FAIL: expected an error naming SetName's side effect, got: {compileErr}");
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
