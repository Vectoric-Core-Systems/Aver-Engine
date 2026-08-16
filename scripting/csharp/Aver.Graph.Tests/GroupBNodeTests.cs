// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Tests for SetMesh / SetMaterial -- the "SetMesh/SetMaterial" node-vocabulary gap this slice's design
// pass decided to close with two DEDICATED coarse nodes (mirroring Spawn's shape), NOT a generalised
// I64-capable SetField, NOT an exposed asset-path lookup, and NOT a generic "add a missing component"
// node. Each wraps ONE line of Aver.Framework.GraphInterop (SetMeshForGraph/SetMaterialForGraph), which
// forwards straight to Entity.SetMesh/SetMaterial (EntityScene.cs) -- the SAME composition the C# side
// of a coarse actor already uses (EnsureMeshRenderer() + Assets.ObjectIdOf/aver_scene_material +
// SetInt64/SetInt). See GraphCompiler.cs's EmitSetMesh/EmitSetMaterial/EmitExecSetMesh/
// EmitExecSetMaterial comments for the design these tests prove, and OcGraphParser.cs's "SetMesh /
// SetMaterial" section for the pin shapes.
//
// DISPATCHED SetField-STYLE, LIKE SetParent/SetViewEntity/SetName (see GroupANodeTests.cs's own header
// comment for the full reasoning) -- EnsureMeshRenderer's own "if already present, do nothing" guard is
// what makes re-running this write every tick harmless, the same idempotence excuse that reasoning
// rests on.
//
// THE FIRST NATIVE CALL EITHER NODE'S IL REACHES IS aver_scene_has_component, NOT aver_scene_set_i64 OR
// aver_scene_material -- because EnsureMeshRenderer (EntityScene.cs) checks HasComponent BEFORE it ever
// adds a component or writes a field. Asserted explicitly below rather than assumed, the same "which
// symbol, exactly" discipline Vec3FieldTests.cs's own header comment applies to its own arity-guard
// ordering claim.
//
// Assets.ObjectIdOf (SetMesh's path-to-id step) is a PURE LOCAL FNV1a64 HASH -- no native call, no I/O,
// no lookup table (Aver.Scene/Native.cs) -- confirmed directly below by TestAssetObjectIdOfIsAPureLocalHash,
// the one assertion in this file that needs no compiled graph and no native call at all, and the
// strongest form of "assert on a real value" available for this pairing: the exact hash a real project
// asset path would produce.
using System;
using Aver.Graph;
using Aver.Scene;

static class GroupBNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        failures += TestAssetObjectIdOfIsAPureLocalHash();

        // ---- SetMesh ----
        failures += TestSetMeshDefaultPinsShapeAndMeshAttributeValue();
        failures += TestSetMeshMissingMeshAttributeFailsCompile();
        failures += TestSetMeshEmitsRealNativeCallFromPullCompile();
        failures += TestSetMeshOnExecChainEmitsRealNativeCall();
        failures += TestSetMeshPulledWithoutExecVisitFailsClearly();

        // ---- SetMaterial ----
        failures += TestSetMaterialDefaultPinsShapeAndMaterialAttributeValue();
        failures += TestSetMaterialMissingMaterialAttributeFailsCompile();
        failures += TestSetMaterialEmitsRealNativeCallFromPullCompile();
        failures += TestSetMaterialOnExecChainEmitsRealNativeCall();
        failures += TestSetMaterialPulledWithoutExecVisitFailsClearly();

        return failures;
    }

    // A REAL VALUE ASSERTION touching zero graph machinery: Assets.ObjectIdOf is public, pure, and
    // deterministic -- fnv1a64 over the UTF-8 bytes of the path, offset 0xcbf29ce484222325,
    // prime 0x100000001b3 (Aver.Scene/Native.cs). Computed by hand here (not copy-pasted from the
    // implementation) so this genuinely checks the ALGORITHM, not merely that the method returns
    // whatever it returns.
    private static int TestAssetObjectIdOfIsAPureLocalHash()
    {
        Console.WriteLine("Test: Assets.ObjectIdOf is a pure local FNV1a64 hash (no native call) -- checked against a hand-computed value");
        try
        {
            const string path = "Content/Meshes/Prop.ocmesh";
            unchecked
            {
                ulong h = 0xcbf29ce484222325UL;
                foreach (byte b in System.Text.Encoding.UTF8.GetBytes(path))
                {
                    h ^= b;
                    h *= 0x100000001b3UL;
                }
                long expected = (long)h;
                long actual = Assets.ObjectIdOf(path);
                if (actual != expected)
                {
                    Console.WriteLine($"  FAIL: expected {expected}, got {actual}");
                    return 1;
                }
                if (Assets.ObjectIdOf("") != 0)
                {
                    Console.WriteLine($"  FAIL: expected ObjectIdOf(\"\") == 0, got {Assets.ObjectIdOf("")}");
                    return 1;
                }
                Console.WriteLine($"  PASS: ObjectIdOf('{path}') == {actual}, matches a hand-computed fnv1a64; empty path == 0");
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
    // SETMESH
    // =================================================================================================

    private static int TestSetMeshDefaultPinsShapeAndMeshAttributeValue()
    {
        Console.WriteLine("Test: SetMesh's default pins are entity:int-in, success:bool-out, no exec; mesh= parses to the exact string");
        try
        {
            var text = "OCGRAPH 1\nNODE sm SetMesh mesh=Content/Meshes/Prop.ocmesh\nPINVAL sm entity 1\nOUT sm success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["sm"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 2;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.MeshPath != "Content/Meshes/Prop.ocmesh")
            {
                Console.WriteLine($"  FAIL: expected MeshPath 'Content/Meshes/Prop.ocmesh' from mesh= attribute, got '{node.MeshPath}'");
                return 1;
            }
            Console.WriteLine("  PASS: 2 pins, no exec, and mesh= parsed to the exact literal path");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetMeshMissingMeshAttributeFailsCompile()
    {
        Console.WriteLine("Test: SetMesh node with no mesh= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE sm SetMesh
PINVAL sm entity 1
OUT sm success
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
                Console.WriteLine("  FAIL: expected compilation to fail (SetMesh has no mesh= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("mesh="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing mesh= attribute, got: {compileErr}");
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

    private static int TestSetMeshEmitsRealNativeCallFromPullCompile()
    {
        Console.WriteLine("Test: SetMesh (PULL/Compile(), no ENTRY at all) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE sm SetMesh mesh=Content/Meshes/Prop.ocmesh
LINK e.value sm.entity
OUT sm success
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
                if (!epEx.Message.Contains("aver_scene_has_component"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_has_component (EnsureMeshRenderer's own first call): {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded (SetMesh is dual-reachable, like SetField), real call reached 'aver_scene_has_component': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetMeshOnExecChainEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetMesh wired onto the exec chain by hand (PUSH) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE tick OnTick
NODE e Param param=entity
NODE sm SetMesh mesh=Content/Meshes/Prop.ocmesh
PIN sm exec in exec
PIN sm entity in int
PIN sm then out exec
PIN sm success out bool
LINK tick.exec sm.exec
LINK e.value sm.entity
ENTRY tick OnTick
OUT sm success
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
                if (!epEx.Message.Contains("aver_scene_has_component"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_has_component: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_has_component' via the exec chain (EmitExecSetMesh): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetMeshPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SetMesh's 'success' pulled as a data value (no exec visit) fails clearly, naming SetMesh");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE sm SetMesh mesh=Content/Meshes/Prop.ocmesh
PINVAL sm entity 1
ENTRY start OnStart
OUT sm success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sm.success pulled with no exec visit ever reaching sm), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SetMesh", StringComparison.OrdinalIgnoreCase) < 0 ||
                !compileErr.Contains("side effect"))
            {
                Console.WriteLine($"  FAIL: expected an error naming SetMesh's side effect, got: {compileErr}");
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
    // SETMATERIAL
    // =================================================================================================

    private static int TestSetMaterialDefaultPinsShapeAndMaterialAttributeValue()
    {
        Console.WriteLine("Test: SetMaterial's default pins are entity:int-in, success:bool-out, no exec; material= parses to the exact string");
        try
        {
            var text = "OCGRAPH 1\nNODE smat SetMaterial material=M_Prop\nPINVAL smat entity 1\nOUT smat success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["smat"];
            bool ok =
                node.Pins.Find(p => p.Name == "entity" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 2;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set: [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.MaterialName != "M_Prop")
            {
                Console.WriteLine($"  FAIL: expected MaterialName 'M_Prop' from material= attribute, got '{node.MaterialName}'");
                return 1;
            }
            Console.WriteLine("  PASS: 2 pins, no exec, and material= parsed to the exact literal 'M_Prop'");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetMaterialMissingMaterialAttributeFailsCompile()
    {
        Console.WriteLine("Test: SetMaterial node with no material= attribute fails to compile");
        try
        {
            var text = @"
OCGRAPH 1
NODE smat SetMaterial
PINVAL smat entity 1
OUT smat success
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
                Console.WriteLine("  FAIL: expected compilation to fail (SetMaterial has no material= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("material="))
            {
                Console.WriteLine($"  FAIL: expected an error mentioning the missing material= attribute, got: {compileErr}");
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

    private static int TestSetMaterialEmitsRealNativeCallFromPullCompile()
    {
        Console.WriteLine("Test: SetMaterial (PULL/Compile(), no ENTRY at all) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE e Param param=entity
NODE smat SetMaterial material=M_Prop
LINK e.value smat.entity
OUT smat success
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
                if (!epEx.Message.Contains("aver_scene_has_component"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_has_component (EnsureMeshRenderer's own first call): {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: pure-PULL compile succeeded (SetMaterial is dual-reachable, like SetField), real call reached 'aver_scene_has_component': {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetMaterialOnExecChainEmitsRealNativeCall()
    {
        Console.WriteLine("Test: SetMaterial wired onto the exec chain by hand (PUSH) emits a real native call, not a stub");
        try
        {
            var text = @"
OCGRAPH 1
PARAM entity int
NODE tick OnTick
NODE e Param param=entity
NODE smat SetMaterial material=M_Prop
PIN smat exec in exec
PIN smat entity in int
PIN smat then out exec
PIN smat success out bool
LINK tick.exec smat.exec
LINK e.value smat.entity
ENTRY tick OnTick
OUT smat success
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
                if (!epEx.Message.Contains("aver_scene_has_component"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_scene_has_component: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_scene_has_component' via the exec chain (EmitExecSetMaterial): {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestSetMaterialPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SetMaterial's 'success' pulled as a data value (no exec visit) fails clearly, naming SetMaterial");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE smat SetMaterial material=M_Prop
PINVAL smat entity 1
ENTRY start OnStart
OUT smat success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (smat.success pulled with no exec visit ever reaching smat), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SetMaterial", StringComparison.OrdinalIgnoreCase) < 0 ||
                !compileErr.Contains("side effect"))
            {
                Console.WriteLine($"  FAIL: expected an error naming SetMaterial's side effect, got: {compileErr}");
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
