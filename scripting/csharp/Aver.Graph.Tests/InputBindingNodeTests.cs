// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the graph-side half of Unreal Enhanced Input-style rebinding: the `action=` attribute now
// optional on InputAction/InputActionPressed/InputActionReleased, required on the two new nodes that
// have no pin fallback (RebindAction, GetActionKey), and the four exec nodes that save/load/reset a
// rebind or read the frame's first pressed key (SaveInputBindings/LoadInputBindings/
// ResetInputBindings/GetPressedKey). See Graph.cs's Node.ActionName, OcGraphParser.cs's "action="
// case and the new default-pins cases beside "savegame"/"loadgame", and GraphCompiler.cs's
// LoadActionHandle/PullActionHandle/EmitGetActionKey/EmitGetPressedKey/EmitExecInputBindingOp/
// EmitExecRebindAction for the design these tests prove.
//
// SAVEGAME/LOADGAME'S OWN SHAPE, EXTENDED: SaveInputBindings/LoadInputBindings/ResetInputBindings
// share ONE predicate and ONE emitter for the identical reason SaveGame/LoadGame do (see
// SaveLoadGameNodeTests.cs's own header), and are refused by the PULL compiler entirely for the same
// reason -- none of the three has a notion of "when" a pure-dataflow graph can express. RebindAction
// joins them there too: mutating one binding is a side effect, exactly like SetVar.
//
// WHAT THE "REAL CALL" TESTS BELOW CAN AND CANNOT PROVE, split two ways:
//
//  - InputAction/InputActionPressed/InputActionReleased's action= tests, and GetPressedKey's, assert
//    an EXACT P/Invoke symbol (aver_fw_action_find, aver_fw_input_key_pressed) the same way
//    SaveLoadGameNodeTests.cs asserts aver_fw_save_write/aver_fw_save_load -- these are safe to pin
//    down because either the CONTRACT names the exact ABI entry point (ActionHandleForGraph is
//    documented as "aver_fw_action_find, cached per name") or the symbol is one this codebase already
//    depends on elsewhere (aver_fw_input_key_pressed backs the existing InputKeyPressed node).
//
//  - SaveInputBindings/LoadInputBindings/ResetInputBindings/RebindAction/GetActionKey do NOT get that
//    treatment. Their GraphInterop implementations live in Aver.Framework's EnhancedInput/
//    InputScheme/Settings -- files this slice does not own and which were being written in parallel
//    with this one -- so which native call, if any, they reach with an empty EnhancedInput router (the
//    state this bare test process is always in) is not this slice's contract to assert. What IS this
//    slice's contract: that the emitted IL pushes the right argument COUNT, ORDER and TYPE for the
//    GraphInterop method GraphCompiler's own reflection resolved. A mismatch there fails at the CLR
//    boundary (a type-check failure, TargetParameterCountException, or InvalidProgramException) rather
//    than at a graceful managed return or a friendly native one -- see
//    AssertReachesRealCallWithoutWiringBug's own comment for exactly what is and is not asserted.
using System;
using Aver.Graph;

static class InputBindingNodeTests
{
    public static int RunAll()
    {
        int failures = 0;

        // ---- compile-time shape: the four exec-only nodes, RebindAction, GetActionKey, GetPressedKey
        failures += TestSaveInputBindingsDefaultPinsShape();
        failures += TestLoadInputBindingsDefaultPinsShape();
        failures += TestResetInputBindingsDefaultPinsShape();
        failures += TestRebindActionDefaultPinsShapeAndActionAttributeValue();
        failures += TestGetActionKeyDefaultPinsShapeAndActionAttributeValue();
        failures += TestGetPressedKeyDefaultPinsShape();

        // ---- action= on InputAction/InputActionPressed/InputActionReleased: optional, parses onto
        // Node.ActionName, does not change the pin shape (the `action` pin stays, just unread)
        failures += TestInputActionWithActionAttributeParsesActionNameAndKeepsActionPin();
        failures += TestInputActionPressedWithActionAttributeParsesActionName();
        failures += TestInputActionReleasedWithActionAttributeParsesActionName();

        // ---- missing action= is a compile-time error naming the node, on the two nodes with no pin
        // fallback at all (RebindAction, GetActionKey) -- InputAction/etc need no such test since the
        // `action` pin is a real, always-valid fallback, not a compile error.
        failures += TestRebindActionMissingActionAttributeFailsCompile();
        failures += TestGetActionKeyMissingActionAttributeFailsCompile();

        // ---- GetActionKey/GetPressedKey are pure (InputKey's own D-path shape): welcome on both
        // compilers, unlike the four exec-only nodes below them.
        failures += TestGetActionKeyIsPureOnBothCompilers();
        failures += TestGetPressedKeyIsPureOnBothCompilers();

        // ---- refused by the PULL compiler entirely, exactly like SaveGame/LoadGame/SetVar
        failures += TestSaveInputBindingsRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestLoadInputBindingsRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestResetInputBindingsRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestRebindActionRefusedByPullCompilerEvenWithNoEntryAtAll();
        failures += TestSaveInputBindingsPulledWithoutExecVisitFailsClearly();

        // ---- action= reaches ActionHandleForGraph (real aver_fw_action_find), and the action PIN
        // fallback still reaches the unchanged aver_fw_action_value2 -- proving the branch in
        // LoadActionHandle/PullActionHandle actually picks the right side.
        failures += TestInputActionWithActionAttributeCallsActionHandleForGraph();
        failures += TestInputActionWithoutActionAttributeStillUsesThePin();
        failures += TestInputActionPressedWithActionAttributeCallsActionHandleForGraph();

        // ---- GetPressedKey's real call reaches the SAME native entry point InputKeyPressed already
        // depends on (aver_fw_input_key_pressed) -- see this file's header for why this one symbol,
        // unlike the GraphInterop.EnhancedInput family below, is safe to pin down exactly.
        failures += TestGetPressedKeyReachesRealInputPoll();

        // ---- the GraphInterop.EnhancedInput family: proof of WIRING (right argument shape reaches a
        // real method), not of a specific native symbol -- see this file's header for why.
        failures += TestSaveInputBindingsReachesRealCallWithoutWiringBug();
        failures += TestLoadInputBindingsReachesRealCallWithoutWiringBug();
        failures += TestResetInputBindingsReachesRealCallWithoutWiringBug();
        failures += TestRebindActionReachesRealCallWithoutWiringBug();
        failures += TestGetActionKeyReachesRealCallWithoutWiringBug();

        return failures;
    }

    // =================================================================================================
    // COMPILE-TIME SHAPE
    // =================================================================================================

    private static int TestSaveInputBindingsDefaultPinsShape() =>
        AssertExecOnlyShape("saveinputbindings", "SaveInputBindings");

    private static int TestLoadInputBindingsDefaultPinsShape() =>
        AssertExecOnlyShape("loadinputbindings", "LoadInputBindings");

    private static int TestResetInputBindingsDefaultPinsShape() =>
        AssertExecOnlyShape("resetinputbindings", "ResetInputBindings");

    // Shared by the three above -- identical shape to SaveGame/LoadGame's own default pins
    // (TestSaveGameDefaultPinsShapeAndPathAttributeValue), minus the path= attribute: exec-in,
    // then(exec-out), success(bool-out), no entity, no attribute.
    private static int AssertExecOnlyShape(string nodeType, string displayName)
    {
        Console.WriteLine($"Test: {displayName}'s default pins are exec-in + then(exec-out) + success(bool-out), no other pins, no attribute");
        try
        {
            var text = $"OCGRAPH 1\nNODE n {displayName}\nOUT n success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["n"];
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
            Console.WriteLine("  PASS: 3 pins (exec-in, exec-out, bool-out), no attribute needed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestRebindActionDefaultPinsShapeAndActionAttributeValue()
    {
        Console.WriteLine("Test: RebindAction's default pins are exec-in + slot/key(int-in) + then(exec-out) + success(bool-out), action= parsed");
        try
        {
            var text = "OCGRAPH 1\nNODE rb RebindAction action=Jump\nOUT rb success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["rb"];
            bool ok =
                node.Pins.Find(p => p.Name == "exec" && !p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "slot" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "key" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "then" && p.IsOutput && p.Type == PinType.Exec) != null &&
                node.Pins.Find(p => p.Name == "success" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 5;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.ActionName != "Jump")
            {
                Console.WriteLine($"  FAIL: expected ActionName 'Jump' from action= attribute, got '{node.ActionName}'");
                return 1;
            }
            Console.WriteLine("  PASS: 5 pins, action= parsed to the exact literal");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestGetActionKeyDefaultPinsShapeAndActionAttributeValue()
    {
        Console.WriteLine("Test: GetActionKey's default pins are slot(int-in) + key(int-out) + bound(bool-out), no exec, action= parsed");
        try
        {
            var text = "OCGRAPH 1\nNODE gak GetActionKey action=Jump\nOUT gak key\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gak"];
            bool ok =
                node.Pins.Find(p => p.Name == "slot" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "key" && p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "bound" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.TrueForAll(p => p.Type != PinType.Exec) &&
                node.Pins.Count == 3;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            if (node.ActionName != "Jump")
            {
                Console.WriteLine($"  FAIL: expected ActionName 'Jump' from action= attribute, got '{node.ActionName}'");
                return 1;
            }
            Console.WriteLine("  PASS: 3 pins, no exec, action= parsed to the exact literal");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestGetPressedKeyDefaultPinsShape()
    {
        Console.WriteLine("Test: GetPressedKey's default pins are key(int-out) + pressed(bool-out), no exec, no attribute");
        try
        {
            var text = "OCGRAPH 1\nNODE gpk GetPressedKey\nOUT gpk key\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["gpk"];
            bool ok =
                node.Pins.Find(p => p.Name == "key" && p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "pressed" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.TrueForAll(p => p.Type != PinType.Exec) &&
                node.Pins.Count == 2;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: unexpected pin set ({node.Pins.Count} pins): [{string.Join(", ", node.Pins.ConvertAll(p => $"{p.Name}:{p.Type}:{(p.IsOutput ? "out" : "in")}"))}]");
                return 1;
            }
            Console.WriteLine("  PASS: 2 pins, no exec, no attribute needed");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // action= ON InputAction / InputActionPressed / InputActionReleased -- OPTIONAL, and the pin shape
    // is UNCHANGED by it (the `action` Int pin stays declared, just unread when ActionName is set).
    // =================================================================================================

    private static int TestInputActionWithActionAttributeParsesActionNameAndKeepsActionPin()
    {
        Console.WriteLine("Test: InputAction action=Jump parses onto Node.ActionName and keeps its 4-pin shape unchanged");
        try
        {
            var text = "OCGRAPH 1\nNODE ia InputAction action=Jump\nOUT ia x\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["ia"];
            if (node.ActionName != "Jump")
            {
                Console.WriteLine($"  FAIL: expected ActionName 'Jump', got '{node.ActionName}'");
                return 1;
            }
            bool ok =
                node.Pins.Find(p => p.Name == "action" && !p.IsOutput && p.Type == PinType.Int) != null &&
                node.Pins.Find(p => p.Name == "x" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "y" && p.IsOutput && p.Type == PinType.Float) != null &&
                node.Pins.Find(p => p.Name == "held" && p.IsOutput && p.Type == PinType.Bool) != null &&
                node.Pins.Count == 4;
            if (!ok)
            {
                Console.WriteLine($"  FAIL: action= must not change the pin shape; got [{string.Join(", ", node.Pins.ConvertAll(p => p.Name))}]");
                return 1;
            }
            Console.WriteLine("  PASS: ActionName parsed, `action` pin still declared (just unread when ActionName is set)");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestInputActionPressedWithActionAttributeParsesActionName() =>
        AssertActionNameParses("InputActionPressed", "Jump");

    private static int TestInputActionReleasedWithActionAttributeParsesActionName() =>
        AssertActionNameParses("InputActionReleased", "Jump");

    private static int AssertActionNameParses(string nodeType, string actionName)
    {
        Console.WriteLine($"Test: {nodeType} action={actionName} parses onto Node.ActionName");
        try
        {
            var text = $"OCGRAPH 1\nNODE n {nodeType} action={actionName}\nOUT n triggered\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var node = graph.Nodes["n"];
            if (node.ActionName != actionName)
            {
                Console.WriteLine($"  FAIL: expected ActionName '{actionName}', got '{node.ActionName}'");
                return 1;
            }
            Console.WriteLine("  PASS");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // =================================================================================================
    // MISSING action= FAILS COMPILE -- only on the two nodes with no pin fallback at all.
    // =================================================================================================

    private static int TestRebindActionMissingActionAttributeFailsCompile()
    {
        Console.WriteLine("Test: RebindAction with no action= attribute fails to compile, naming the node");
        try
        {
            var text = @"
OCGRAPH 1
NODE tick OnTick
NODE rb RebindAction
LINK tick.exec rb.exec
ENTRY tick OnTick
OUT rb success
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
                Console.WriteLine("  FAIL: expected compilation to fail (RebindAction has no action= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("action=") || !compileErr.Contains("'rb'"))
            {
                Console.WriteLine($"  FAIL: expected an error naming node 'rb' and its missing action= attribute, got: {compileErr}");
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

    private static int TestGetActionKeyMissingActionAttributeFailsCompile()
    {
        Console.WriteLine("Test: GetActionKey with no action= attribute fails to compile, naming the node");
        try
        {
            var text = "OCGRAPH 1\nNODE gak GetActionKey\nOUT gak key\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected compilation to fail (GetActionKey has no action= attribute), but it succeeded");
                return 1;
            }
            if (compileErr == null || !compileErr.Contains("action=") || !compileErr.Contains("'gak'"))
            {
                Console.WriteLine($"  FAIL: expected an error naming node 'gak' and its missing action= attribute, got: {compileErr}");
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
    // GetActionKey / GetPressedKey ARE PURE -- welcome on both compilers, InputKey's own shape.
    // =================================================================================================

    private static int TestGetActionKeyIsPureOnBothCompilers()
    {
        Console.WriteLine("Test: GetActionKey compiles on BOTH compilers (it is a pure read)");
        try
        {
            const string pull = "OCGRAPH 1\nNODE s ConstInt value=0\nNODE gak GetActionKey action=Jump\nLINK s.value gak.slot\nOUT gak key\n";
            if (!OcGraphParser.Parse(pull, out var g1, out var e1))
            {
                Console.WriteLine($"  FAIL: pull parse: {e1}");
                return 1;
            }
            if (new GraphCompiler(g1).Compile(out var c1) == null)
            {
                Console.WriteLine($"  FAIL: Compile() refused a pure read: {c1}");
                return 1;
            }

            const string push = "OCGRAPH 1\nNODE tick OnTick\nENTRY tick OnTick\nNODE s ConstInt value=0\nNODE gak GetActionKey action=Jump\nLINK s.value gak.slot\nOUT gak key\n";
            if (!OcGraphParser.Parse(push, out var g2, out var e2))
            {
                Console.WriteLine($"  FAIL: push parse: {e2}");
                return 1;
            }
            if (new GraphCompiler(g2).CompileEntryPoint("OnTick", out var c2) == null)
            {
                Console.WriteLine($"  FAIL: CompileEntryPoint() refused a pure read: {c2}");
                return 1;
            }
            Console.WriteLine("  PASS: both compilers accept it");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    private static int TestGetPressedKeyIsPureOnBothCompilers()
    {
        Console.WriteLine("Test: GetPressedKey compiles on BOTH compilers (it is a pure read)");
        try
        {
            const string pull = "OCGRAPH 1\nNODE gpk GetPressedKey\nOUT gpk key\n";
            if (!OcGraphParser.Parse(pull, out var g1, out var e1))
            {
                Console.WriteLine($"  FAIL: pull parse: {e1}");
                return 1;
            }
            if (new GraphCompiler(g1).Compile(out var c1) == null)
            {
                Console.WriteLine($"  FAIL: Compile() refused a pure read: {c1}");
                return 1;
            }

            const string push = "OCGRAPH 1\nNODE tick OnTick\nENTRY tick OnTick\nNODE gpk GetPressedKey\nOUT gpk key\n";
            if (!OcGraphParser.Parse(push, out var g2, out var e2))
            {
                Console.WriteLine($"  FAIL: push parse: {e2}");
                return 1;
            }
            if (new GraphCompiler(g2).CompileEntryPoint("OnTick", out var c2) == null)
            {
                Console.WriteLine($"  FAIL: CompileEntryPoint() refused a pure read: {c2}");
                return 1;
            }
            Console.WriteLine("  PASS: both compilers accept it");
            return 0;
        }
        catch (Exception ex) { Console.WriteLine($"  FAIL: {ex.Message}"); return 1; }
    }

    // =================================================================================================
    // REFUSED BY THE PULL COMPILER -- SaveInputBindings/LoadInputBindings/ResetInputBindings/
    // RebindAction, mirroring SaveGame/LoadGame/SetVar's own tests in SaveLoadGameNodeTests.cs.
    // =================================================================================================

    private static int TestSaveInputBindingsRefusedByPullCompilerEvenWithNoEntryAtAll() =>
        AssertRefusedByPullCompiler("SaveInputBindings");

    private static int TestLoadInputBindingsRefusedByPullCompilerEvenWithNoEntryAtAll() =>
        AssertRefusedByPullCompiler("LoadInputBindings");

    private static int TestResetInputBindingsRefusedByPullCompilerEvenWithNoEntryAtAll() =>
        AssertRefusedByPullCompiler("ResetInputBindings");

    private static int AssertRefusedByPullCompiler(string nodeType)
    {
        Console.WriteLine($"Test: {nodeType} in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, {nodeType}-naming error");
        try
        {
            var text = $"OCGRAPH 1\nNODE n {nodeType}\nOUT n success\n";
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
                Console.WriteLine($"  FAIL: expected Compile() to refuse a {nodeType} node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf(nodeType, StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the {nodeType} node type, got: {compileErr}");
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

    private static int TestRebindActionRefusedByPullCompilerEvenWithNoEntryAtAll()
    {
        Console.WriteLine("Test: RebindAction in a no-ENTRY (pure-PULL) graph fails Compile() with a clear, RebindAction-naming error");
        try
        {
            var text = "OCGRAPH 1\nNODE rb RebindAction action=Jump\nOUT rb success\n";
            if (!OcGraphParser.Parse(text, out var graph, out var err))
            {
                Console.WriteLine($"  FAIL: Parse error: {err}");
                return 1;
            }
            var compiler = new GraphCompiler(graph);
            var compiled = compiler.Compile(out var compileErr);
            if (compiled != null)
            {
                Console.WriteLine("  FAIL: expected Compile() to refuse a RebindAction node with no exec chain to gate it, but it succeeded");
                return 1;
            }
            if (compileErr == null || compileErr.IndexOf("RebindAction", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming the RebindAction node type, got: {compileErr}");
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

    // Mirrors TestSaveGamePulledWithoutExecVisitFailsClearly: a node never wired into the exec chain,
    // but pulled via OUT, must fail clearly (naming the type and "side effect"), not silently.
    private static int TestSaveInputBindingsPulledWithoutExecVisitFailsClearly()
    {
        Console.WriteLine("Test: SaveInputBindings never wired into the exec chain, but pulled via OUT, fails clearly (not silently false)");
        try
        {
            var text = @"
OCGRAPH 1
NODE start OnStart
NODE sib SaveInputBindings
ENTRY start OnStart
OUT sib success
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
                Console.WriteLine("  FAIL: expected CompileEntryPoint to fail (sib.success pulled with no exec visit ever reaching sib), but it succeeded");
                return 1;
            }
            if (compileErr == null ||
                compileErr.IndexOf("SaveInputBindings", StringComparison.OrdinalIgnoreCase) < 0 ||
                compileErr.IndexOf("side effect", StringComparison.OrdinalIgnoreCase) < 0)
            {
                Console.WriteLine($"  FAIL: expected an error naming SaveInputBindings and its side effect, got: {compileErr}");
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
    // action= REACHES GraphInterop.ActionHandleForGraph -> a real aver_fw_action_find, and the PLAIN
    // `action` PIN FALLBACK IS UNCHANGED -- still aver_fw_action_value2, never aver_fw_action_find.
    // See this file's header comment for why THIS symbol, unlike the EnhancedInput family below, is
    // safe to pin down exactly (the contract names it outright: "aver_fw_action_find, cached by name").
    // =================================================================================================

    private static int TestInputActionWithActionAttributeCallsActionHandleForGraph()
    {
        Console.WriteLine("Test: InputAction action=Jump resolves the handle via ActionHandleForGraph -> real call to aver_fw_action_find");
        try
        {
            var text = "OCGRAPH 1\nNODE ia InputAction action=Jump\nOUT ia x\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                float unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_action_find"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_action_find: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_action_find', proving action= reached GraphInterop.ActionHandleForGraph: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestInputActionWithoutActionAttributeStillUsesThePin()
    {
        Console.WriteLine("Test: InputAction with NO action= attribute is unchanged -- still reads the `action` pin, real call to aver_fw_action_value2");
        try
        {
            var text = "OCGRAPH 1\nNODE h ConstInt value=1\nNODE ia InputAction\nLINK h.value ia.action\nOUT ia x\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var node = graph.Nodes["ia"];
            if (node.ActionName != null)
            {
                Console.WriteLine($"  FAIL: test fixture is wrong -- 'ia' should have no ActionName, got '{node.ActionName}'");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<float> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                float unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw (see this file's header comment) but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_action_value2"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_action_value2 (got a different call -- action= handling may have run even though it is absent): {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_action_value2', proving the pin fallback still runs when action= is absent: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    private static int TestInputActionPressedWithActionAttributeCallsActionHandleForGraph()
    {
        Console.WriteLine("Test: InputActionPressed action=Jump resolves the handle via ActionHandleForGraph -> real call to aver_fw_action_find (not aver_fw_action_pressed)");
        try
        {
            var text = "OCGRAPH 1\nNODE iap InputActionPressed action=Jump\nOUT iap triggered\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                bool unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_action_find"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_action_find: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_action_find' BEFORE aver_fw_action_pressed, proving action= is resolved first: {epEx.Message}");
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
    // GetPressedKey -> Input.FirstKeyPressedThisFrame -> Input.GetKeyDown -> the SAME
    // aver_fw_input_key_pressed the existing InputKeyPressed node already depends on (see
    // EdgeInputNodeTests.cs's own "not aver_fw_input_key" test for that symbol's precedent).
    // =================================================================================================

    private static int TestGetPressedKeyReachesRealInputPoll()
    {
        Console.WriteLine("Test: GetPressedKey polls real input -- real call reaches aver_fw_input_key_pressed");
        try
        {
            var text = "OCGRAPH 1\nNODE gpk GetPressedKey\nOUT gpk key\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<int> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                int unused = fn();
                Console.WriteLine($"  FAIL: expected invoking this to throw but it returned {unused}");
                return 1;
            }
            catch (EntryPointNotFoundException epEx)
            {
                if (!epEx.Message.Contains("aver_fw_input_key_pressed"))
                {
                    Console.WriteLine($"  FAIL: threw EntryPointNotFoundException, but not naming aver_fw_input_key_pressed: {epEx.Message}");
                    return 1;
                }
                Console.WriteLine($"  PASS: real call attempted 'aver_fw_input_key_pressed': {epEx.Message}");
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
    // THE GraphInterop.EnhancedInput FAMILY -- proof of WIRING, not of one specific managed/native call.
    // See this file's header comment (and AssertReachesRealCallWithoutWiringBug's own) for why.
    // =================================================================================================

    private static int TestSaveInputBindingsReachesRealCallWithoutWiringBug()
    {
        Console.WriteLine("Test: SaveInputBindings on a real exec chain reaches a real GraphInterop.SaveInputBindingsForGraph call");
        var text = @"
OCGRAPH 1
NODE tick OnTick
NODE sib SaveInputBindings
LINK tick.exec sib.exec
ENTRY tick OnTick
OUT sib success
";
        return CompileExecBoolAndAssertNoWiringBug(text, "OnTick", "SaveInputBindings");
    }

    private static int TestLoadInputBindingsReachesRealCallWithoutWiringBug()
    {
        Console.WriteLine("Test: LoadInputBindings on a real exec chain reaches a real GraphInterop.LoadInputBindingsForGraph call");
        var text = @"
OCGRAPH 1
NODE tick OnTick
NODE lib LoadInputBindings
LINK tick.exec lib.exec
ENTRY tick OnTick
OUT lib success
";
        return CompileExecBoolAndAssertNoWiringBug(text, "OnTick", "LoadInputBindings");
    }

    private static int TestResetInputBindingsReachesRealCallWithoutWiringBug()
    {
        Console.WriteLine("Test: ResetInputBindings on a real exec chain reaches a real GraphInterop.ResetInputBindingsForGraph call");
        var text = @"
OCGRAPH 1
NODE tick OnTick
NODE rib ResetInputBindings
LINK tick.exec rib.exec
ENTRY tick OnTick
OUT rib success
";
        return CompileExecBoolAndAssertNoWiringBug(text, "OnTick", "ResetInputBindings");
    }

    private static int TestRebindActionReachesRealCallWithoutWiringBug()
    {
        Console.WriteLine("Test: RebindAction on a real exec chain reaches a real GraphInterop.RebindActionForGraph(action, slot, key) call");
        var text = @"
OCGRAPH 1
NODE tick OnTick
NODE slot ConstInt value=0
NODE key ConstInt value=32
NODE rb RebindAction action=Jump
LINK tick.exec rb.exec
LINK slot.value rb.slot
LINK key.value rb.key
ENTRY tick OnTick
OUT rb success
";
        return CompileExecBoolAndAssertNoWiringBug(text, "OnTick", "RebindAction");
    }

    // Compiles `text` through CompileEntryPoint, casts to Func<bool> and runs
    // AssertReachesRealCallWithoutWiringBug on it -- shared by the four tests immediately above.
    private static int CompileExecBoolAndAssertNoWiringBug(string text, string entryEvent, string displayName)
    {
        try
        {
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).CompileEntryPoint(entryEvent, out var cerr);
            if (compiled is not Func<bool> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            return AssertReachesRealCallWithoutWiringBug(displayName, fn);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {ex.Message}");
            return 1;
        }
    }

    // Invokes a compiled exec node's delegate as proof the emitted IL genuinely reaches its
    // GraphInterop method with the right argument shape, WITHOUT committing to what that method's own
    // implementation does with an EnhancedInput router that has never had a context pushed onto it (the
    // state this bare test process is always in) -- EnhancedInput/InputScheme/Settings are a different
    // slice's files, written in parallel with this one, so asserting an exact P/Invoke symbol here
    // would be a guess this slice has no business making, unlike SaveGame/LoadGame or the action=
    // tests above, where the contract names the exact ABI entry point outright.
    //
    // What this DOES assert: a genuine wiring bug in THIS slice's emitted IL -- the wrong argument
    // count, order, or CLR type for the MethodInfo GraphCompiler's own reflection resolved -- fails at
    // the CLR boundary (a type-check failure, TargetParameterCountException, or InvalidProgramException
    // out of the DynamicMethod), never at a graceful managed return or a friendly P/Invoke-missing one.
    // Both of the latter two are therefore treated as PASS; only the former is a FAIL.
    private static int AssertReachesRealCallWithoutWiringBug(string displayName, Func<bool> fn)
    {
        try
        {
            bool result = fn();
            Console.WriteLine($"  PASS: {displayName} ran to completion and returned {result} (a real, graceful outcome with no live EnhancedInput state in this bare process)");
            return 0;
        }
        catch (EntryPointNotFoundException epEx)
        {
            Console.WriteLine($"  PASS: {displayName} reached a real native call boundary: {epEx.Message}");
            return 0;
        }
        catch (DllNotFoundException dllEx)
        {
            Console.WriteLine($"  PASS: {displayName} reached a real native call boundary: {dllEx.Message}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: {displayName} threw a wiring-shaped exception, not a graceful/native-boundary one: {ex}");
            return 1;
        }
    }

    private static int TestGetActionKeyReachesRealCallWithoutWiringBug()
    {
        Console.WriteLine("Test: GetActionKey reaches a real GraphInterop.GetActionKeyForGraph(action, slot) call");
        try
        {
            var text = "OCGRAPH 1\nNODE s ConstInt value=0\nNODE gak GetActionKey action=Jump\nLINK s.value gak.slot\nOUT gak key\nOUT gak bound\n";
            if (!OcGraphParser.Parse(text, out var graph, out var perr))
            {
                Console.WriteLine($"  FAIL: Parse error: {perr}");
                return 1;
            }
            var compiled = new GraphCompiler(graph).Compile(out var cerr);
            if (compiled is not Func<object[]> fn)
            {
                Console.WriteLine($"  FAIL: Compile error: {cerr ?? "(wrong delegate shape)"}");
                return 1;
            }
            try
            {
                var got = fn();
                if (got.Length != 2 || got[0] is not int key || got[1] is not bool bound)
                {
                    Console.WriteLine($"  FAIL: expected 2 OUT values (int key, bool bound), got [{string.Join(", ", got)}]");
                    return 1;
                }
                bool expectedBound = key != -1;
                if (bound != expectedBound)
                {
                    Console.WriteLine($"  FAIL: bound must be exactly (key != -1); got key={key}, bound={bound}");
                    return 1;
                }
                Console.WriteLine($"  PASS: GetActionKey ran to completion, key={key}, bound={bound} (a real, graceful outcome with no live EnhancedInput state in this bare process)");
                return 0;
            }
            catch (EntryPointNotFoundException epEx)
            {
                Console.WriteLine($"  PASS: GetActionKey reached a real native call boundary: {epEx.Message}");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  FAIL: GetActionKey threw a wiring-shaped exception, not a graceful/native-boundary one: {ex}");
            return 1;
        }
    }
}
