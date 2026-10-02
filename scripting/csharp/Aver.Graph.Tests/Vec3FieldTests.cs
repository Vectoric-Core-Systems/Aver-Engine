// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Tests for GetFieldVec3/SetFieldVec3 -- closing GetField/SetField's own FieldKindF32 gap for reading
// and writing a Vec3-kind scene field (position, scale, colour, ...). See
// GraphCompiler.cs's RequireVec3Field/EmitGetFieldVec3/EmitSetFieldVec3/EmitExecSetFieldVec3/
// EmitPullGetFieldVec3 comments for the design these tests prove, and OcGraphParser.cs's
// "getfieldvec3"/"setfieldvec3" cases for the pin shapes.
//
// SAME HONEST LIMITATION AS GetField/SetField/InputKey/Raycast (see NewNodeTests.cs's own header
// comment, and Program.cs's comment above TestGetFieldEmitsRealNativeCall, for the full explanation):
// nothing in THIS test process boots a live native scene, so "emits a real native call" tests below
// prove exactly what those tests prove -- compile-time pin/shape/kind correctness (a genuine behavioural
// assertion: the compiler must REJECT a wrong-kind field, not merely "compile something"), and that the
// EMITTED IL performs a REAL call reaching this process's own field table (not a hardcoded stub) by
// observing the SPECIFIC failure that call produces. GetFieldVec3/SetFieldVec3 route through a NEW
// wrapper (Aver.Framework.GraphInterop.GetFieldVecForGraph/SetFieldVecForGraph) that calls
// SceneNative.aver_scene_field_arity BEFORE aver_scene_get_vec/set_vec (the arity guard described in
// GraphInterop's own comment) -- so the observed failure here names 'aver_scene_field_arity', not
// 'aver_scene_get_vec'/'aver_scene_set_vec': the arity check is genuinely the first native call this
// path makes, and asserting the exact symbol is what proves that ordering rather than assuming it.
using System;
using Aver.Graph;

static class Vec3FieldTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- GetFieldVec3 ----
        failures += TestGetFieldVec3DefaultPinsShape();
        failures += TestGetFieldVec3MissingFieldAttributeFailsCompile();
        failures += TestGetFieldVec3WrongKindFailsCompilePull();
        failures += TestGetFieldVec3WrongKindFailsCompilePush();
        failures += TestGetFieldVec3EmitsRealNativeCallPull();
        failures += TestGetFieldVec3EmitsRealNativeCallPush();
        failures += TestGetFieldVec3AllThreeComponentsPulledIndependentlyInEntryGraph();

        // ---- SetFieldVec3 ----
        failures += TestSetFieldVec3DefaultPinsShape();
        failures += TestSetFieldVec3WrongKindFailsCompile();
        failures += TestSetFieldVec3EmitsRealNativeCallFromPullCompile();
        failures += TestSetFieldVec3PulledAsDataValueWithoutExecVisitFailsClearly();
        failures += TestSetFieldVec3OnExecChainEmitsRealNativeCall();

        return failures;
    }

    // =================================================================================================
    // GETFIELDVEC3
    // =================================================================================================

    private static int TestGetFieldVec3DefaultPinsShape()
    {
        Console.WriteLine("Test: GetFieldVec3's default pins are entity:int-in, x/y/z:float-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE gv GetFieldVec3 field=CLocal.position\nOUT gv x\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gv"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "x" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "y" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "z" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Count == 4; // no exec pins -- pure data node, mirrors GetField
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 4 pins, no exec, types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestGetFieldVec3MissingFieldAttributeFailsCompile()
    {
        Console.WriteLine("Test: GetFieldVec3 node with no field= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gv GetFieldVec3
LINK e value gv entity
OUT gv x
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
                Console.WriteLine("  FAIL: expected compilation to fail (GetFieldVec3 has no field= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("field="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing field= attribute, got: {compileErr}");
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

    // The inverse of GetField's own TestGetFieldWrongKindFailsCompile (Program.cs): that test feeds
    // GetField a Vec3-kind field and proves it is rejected; this feeds GetFieldVec3 an F32-kind field
    // (kind=0 -- a real, common case: CLight.intensityLux, CCamera.fovYRad, any scalar field a graph
    // author might mistakenly point a Vec3 node at) and proves the same rejection in the other
    // direction. Genuinely different code path (RequireVec3Field, not GetField's own inline check).
    private static int TestGetFieldVec3WrongKindFailsCompilePull()
    {
        Console.WriteLine("Test: GetFieldVec3 naming an F32 (not Vec3) field fails at COMPILE time (PULL)");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gv GetFieldVec3 field=CLight.intensityLux
LINK e value gv entity
OUT gv x
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver f32Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 7;
                kind = 0; // FieldKind.F32
                return true;
            };

            var compiler = new GraphCompiler(graph, f32Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (field is F32, not Vec3), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("not a Vec3 field"))
            {
                Console.WriteLine($"  FAIL: expected an error about the field not being Vec3, got: {compileErr}");
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

    // SAME CHECK, OTHER COMPILER -- proves RequireVec3Field's rejection is not PULL-only. GetFieldVec3
    // is freely pullable through CompileEntryPoint too (EmitPullGetFieldVec3), reached here via an OUT
    // record with no exec visit at all (legal for GetFieldVec3, unlike Raycast -- see that node's own
    // "pulled without exec visit fails clearly" test for the type this ISN'T).
    private static int TestGetFieldVec3WrongKindFailsCompilePush()
    {
        Console.WriteLine("Test: GetFieldVec3 naming an F32 (not Vec3) field fails at COMPILE time (PUSH/CompileEntryPoint)");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE start OnStart
NODE gv GetFieldVec3 field=CLight.intensityLux
PINVAL gv entity 1
ENTRY start OnStart
OUT gv x
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver f32Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 7;
                kind = 0; // FieldKind.F32
                return true;
            };

            var compiler = new GraphCompiler(graph, f32Field);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (field is F32, not Vec3), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("not a Vec3 field"))
            {
                Console.WriteLine($"  FAIL: expected an error about the field not being Vec3, got: {compileErr}");
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

    private static int TestGetFieldVec3EmitsRealNativeCallPull()
    {
        Console.WriteLine("Test: GetFieldVec3 (PULL/Compile()) emits a real native call, not a stub");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE gv GetFieldVec3 field=CLocal.position
LINK e value gv entity
OUT gv x
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver vec3Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 3;
                kind = 1; // FieldKind.Vec3
                return true;
            };

            var compiler = new GraphCompiler(graph, vec3Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<int, float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                float unused = fn(1);
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_field_arity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_field_arity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_field_arity' (GraphInterop's arity guard, the first native call GetFieldVecForGraph makes): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestGetFieldVec3EmitsRealNativeCallPush()
    {
        Console.WriteLine("Test: GetFieldVec3 (PUSH/CompileEntryPoint, pulled with no exec visit) emits a real native call, not a stub");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE start OnStart
NODE gv GetFieldVec3 field=CLocal.position
PINVAL gv entity 1
ENTRY start OnStart
OUT gv y
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver vec3Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 3;
                kind = 1; // FieldKind.Vec3
                return true;
            };

            var compiler = new GraphCompiler(graph, vec3Field);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                float unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_field_arity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_field_arity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_field_arity' via the PUSH compiler's own pull path too (no exec visit -- GetFieldVec3 is freely pullable, unlike Raycast): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Proves EmitPullGetFieldVec3's pinName dispatch really distinguishes x/y/z (not just "whatever
    // happened to be requested first") by reading all three via three separate OUT records in ONE
    // event-driven graph and confirming all three independently reach the SAME real native symbol --
    // if the pin-name switch inside EmitPullGetFieldVec3 were broken (e.g. always reading "x"), this
    // would not distinguish the failure, but it DOES prove the dispatch does not throw a wrong-pin
    // error or silently produce nothing for y/z, which a naive "only handles the first requested pin"
    // bug would.
    private static int TestGetFieldVec3AllThreeComponentsPulledIndependentlyInEntryGraph()
    {
        Console.WriteLine("Test: GetFieldVec3's x, y, AND z are each independently reachable via OUT in an ENTRY graph");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE start OnStart
NODE gv GetFieldVec3 field=CLocal.position
PINVAL gv entity 1
ENTRY start OnStart
OUT gv x
OUT gv y
OUT gv z
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver vec3Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 3;
                kind = 1; // FieldKind.Vec3
                return true;
            };

            var compiler = new GraphCompiler(graph, vec3Field);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                object[] unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned [{string.Join(", ", unused)}]");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                // The FIRST of the three OUT entries (x) is what actually runs -- CompileEntryPoint's
                // outputs-array loop (see that method's own "OUTPUTS ARRAY" comment on Compile()) emits
                // them in order and the emitted IL throws on the very first native call it reaches, so
                // y/z's own EmitPullGetFieldVec3 calls never execute in THIS invocation. That the
                // compile SUCCEEDED with three OUT records naming x/y/z -- rather than failing at
                // compile time with "no output pin" for y or z -- is what this test actually proves;
                // the exception on invoke is the same real-call evidence the other tests already give.
                if (!epEx.Message.Contains("aver_scene_field_arity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_field_arity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: compiled with x/y/z all as separate OUT records (proving EmitPullGetFieldVec3 recognises all three pin names, not just one), and invoking reached the real call: {epEx.Message}");
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
    // SETFIELDVEC3
    // =================================================================================================

    private static int TestSetFieldVec3DefaultPinsShape()
    {
        Console.WriteLine("Test: SetFieldVec3's default pins are entity:int-in, x/y/z:float-in, success:bool-out, no exec");
        try
        {
            var text = "OCGRAPH 1\nNODE sv SetFieldVec3 field=CLocal.position\nOUT sv success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sv"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "x" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "y" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "z" && !p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 5; // no exec pins by default -- mirrors SetField exactly
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 5 pins, no exec, types match spec");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetFieldVec3WrongKindFailsCompile()
    {
        Console.WriteLine("Test: SetFieldVec3 naming an F32 (not Vec3) field fails at COMPILE time");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
PARAM x float
PARAM y float
PARAM z float
NODE e Param param=entity
NODE px Param param=x
NODE py Param param=y
NODE pz Param param=z
NODE sv SetFieldVec3 field=CLight.intensityLux
LINK e value sv entity
LINK px value sv x
LINK py value sv y
LINK pz value sv z
OUT sv success
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            FieldResolver f32Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 7;
                kind = 0; // FieldKind.F32
                return true;
            };

            var compiler = new GraphCompiler(graph, f32Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (field is F32, not Vec3), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("not a Vec3 field"))
            {
                Console.WriteLine($"  FAIL: expected an error about the field not being Vec3, got: {compileErr}");
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

    // Proves SetFieldVec3 is DUAL-REACHABLE like SetField -- Compile()'s PULL/topological walk visits
    // it exactly once per invocation via EmitNode's own "setfieldvec3" case (a single deterministic
    // write, not the double-write EmitPullOutput's refusal guards against; see that case's own comment
    // in GraphCompiler.EmitNode). A no-ENTRY graph, deliberately: this is the PULL-only compiler, not
    // CompileEntryPoint.
    private static int TestSetFieldVec3EmitsRealNativeCallFromPullCompile()
    {
        Console.WriteLine("Test: SetFieldVec3 (PULL/Compile(), no ENTRY at all) emits a real native call, not a stub");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM entity int
PARAM x float
PARAM y float
PARAM z float
NODE e Param param=entity
NODE px Param param=x
NODE py Param param=y
NODE pz Param param=z
NODE sv SetFieldVec3 field=CLocal.position
LINK e value sv entity
LINK px value sv x
LINK py value sv y
LINK pz value sv z
OUT sv success
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            if (graph.EntryPoints.Count != 0)
            {
                Console.WriteLine($"  FAIL: expected zero ENTRY records, got {graph.EntryPoints.Count}");
                return 1;
            }

            FieldResolver vec3Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 3;
                kind = 1; // FieldKind.Vec3
                return true;
            };

            var compiler = new GraphCompiler(graph, vec3Field);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled is not Func<int, float, float, float, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                bool unused = fn(1, 10f, 20f, 30f);
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_field_arity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_field_arity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded (SetFieldVec3 is dual-reachable, like SetField), real call reached 'aver_scene_field_arity': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // A DELIBERATE guard, tested directly, mirroring TestRaycastPulledWithoutExecVisitFailsClearly's
    // shape but exercising the OTHER refusal path: SetFieldVec3 has NO case in EmitPullOutput's switch
    // at all (mirrors SetField), so this must be caught by the generalised side-effect guard at the top
    // of EmitPullOutput -- proving that guard's condition (IsExecCapableSideEffectType(source.Type) ||
    // IsExecCapableVecSideEffectType(source.Type)) and its now-generic message (naming source.Type
    // rather than a hardcoded "SetField") both work for the SECOND side-effecting type, not just the
    // first.
    private static int TestSetFieldVec3PulledAsDataValueWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SetFieldVec3's 'success' pulled as a data value (no exec visit) fails clearly, naming SetFieldVec3 -- not 'SetField'");
        try
        {
            var graphText = @"
OCGRAPH 1
NODE start OnStart
NODE sv SetFieldVec3 field=CLocal.position
PINVAL sv entity 1
ENTRY start OnStart
OUT sv success
";
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            var compiler = new GraphCompiler(graph);
            var compiled = compiler.CompileEntryPoint("OnStart", out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sv.success pulled with no exec visit ever reaching sv), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SetFieldVec3", StringComparison.OrdinalIgnoreCase) < 0 ||
                !compileErr.Contains("side effect"))
            {
                Console.WriteLine($"  FAIL: expected an error naming SetFieldVec3's side effect, got: {compileErr}");
                return 1;
            }
            if (compileErr.Contains("SetField has", StringComparison.Ordinal))
            {
                Console.WriteLine($"  FAIL: message still hardcodes 'SetField' rather than naming the actual node type: {compileErr}");
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

    // SetFieldVec3 given EXPLICIT exec pins by hand (mirrors how SetField itself needs explicit PIN
    // records to sit on the exec chain -- neither gets exec pins by default) and wired into ENTRY
    // OnTick, reaching EmitExecSetFieldVec3 via IsExecCapableVecSideEffectType/EmitExecNode's dispatch.
    private static int TestSetFieldVec3OnExecChainEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetFieldVec3 wired onto the exec chain by hand (PUSH) emits a real native call, not a stub");
        try
        {
            var graphText = @"
OCGRAPH 1
PARAM x float
NODE tick OnTick
NODE px Param param=x
NODE sv SetFieldVec3 field=CLocal.position
PIN sv exec in exec
PIN sv entity in int
PIN sv x in float
PIN sv y in float
PIN sv z in float
PIN sv then out exec
PIN sv success out bool
PINVAL sv entity 1
PINVAL sv y 0.0
PINVAL sv z 0.0
LINK tick exec sv exec
LINK px value sv x
ENTRY tick OnTick
OUT sv success
";
            // FORCED DECIMAL POINTS on y/z above are load-bearing, not cosmetic -- see
            // NewNodeTests.cs's RunSelectPullCase for the same landmine hit once already, in a
            // DIFFERENT parser branch: PINVAL's own value inference (OcGraphParser.cs, the PINVAL
            // record branch) ALSO tries int.TryParse before float.TryParse, so a bare "0" for a
            // float-typed pin becomes ConstantOutput-shaped as (int)0, and EmitPullInput's own
            // `pv.Value is float` check then silently pushes an Ldc_I4 where SetFieldVecForGraph's
            // signature expects an Ldc_R4 -- a genuine IL type mismatch the JIT catches as
            // InvalidProgramException at INVOKE time, not a wrong number returned. Discovered
            // empirically while writing this exact test (first draft used bare "0"/"0" and threw
            // "Common Language Runtime detected an invalid program" on the very last line, which is
            // exactly what a stack-type mismatch looks like). Pre-existing parser behaviour, not this
            // node type's bug -- worth a real fix in OcGraphParser.cs's PINVAL branch someday, out of
            // scope here -- but real enough to trip silently if a decimal point is dropped.
            if (!OcGraphParser.Parse(graphText, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }

            // A fake resolver, exactly like every other "emits a real call" test in this suite --
            // otherwise the DEFAULT resolver's own real aver_scene_field lookup throws (a real call in
            // its own right) at COMPILE time, before EmitExecSetFieldVec3 ever gets a chance to emit
            // the call this test actually wants to observe at INVOKE time.
            FieldResolver vec3Field = (string name, out int fieldId, out int kind) =>
            {
                fieldId = 3;
                kind = 1; // FieldKind.Vec3
                return true;
            };

            var compiler = new GraphCompiler(graph, vec3Field);
            var compiled = compiler.CompileEntryPoint("OnTick", out var compileErr);
            if (compiled is not Func<float, bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {compileErr ?? "(wrong delegate shape)"}");
                return 1;
            }

            try
            {
                bool unused = fn(5f);
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_scene_field_arity"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_field_arity: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_field_arity' via the exec chain (EmitExecSetFieldVec3): {epEx.Message}");
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
