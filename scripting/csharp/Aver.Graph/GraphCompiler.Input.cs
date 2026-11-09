// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Input nodes: keys, actions, bindings, mouse delta and move axis.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// InputKey(key) -> down: reads Aver.Framework's polled input state. Pure/idempotent like
    /// GetField (no _execLocals caching needed), unlike Raycast. aver_fw_input_key returns 0/1 as
    /// int32, the bit pattern Stloc expects for a bool local directly, no conversion -- same
    /// property EmitCompare's Cgt result relies on.
    private void EmitInputKey(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "key");
        _il.Emit(OpCodes.Call, InputKeyMethod);

        if (_pinLocals.TryGetValue((node.Id, "down"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// InputKeyPressed / InputKeyReleased: the rising/falling edge of a key. Same shape as
    /// EmitInputKey (int in, bool out); node type picks the call. Output pin is `triggered`, not
    /// `down` -- this is an event (held for a second yields one true, fifty-nine falses), which is
    /// what jump/fire/toggle actually want.
    private void EmitInputKeyEdge(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "key");
        _il.Emit(OpCodes.Call, node.Type.ToLowerInvariant() == "inputkeyreleased"
                                   ? InputKeyReleasedMethod : InputKeyPressedMethod);

        if (_pinLocals.TryGetValue((node.Id, "triggered"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// InputAction(action) -> x, y, held: float2 value + digital-active state of a named action set
    /// up via aver_fw_action_register/_bind (framework_abi.h Named Actions) -- InputKey's
    /// higher-level, preferred sibling. `action` is a plain Int handle from
    /// aver_fw_action_register/_find, like InputKey's "key" (PinType has no String member). Two native calls
    /// (aver_fw_action_value2, aver_fw_action_held: framework_abi.h:444,447), both cheap array-scan
    /// reads, so no _execLocals caching needed. action_value2 takes a `float[]` out-param, not
    /// `ref float` x3 like GetFieldVecForGraph -- Newarr's a 2-element array and Dups the reference,
    /// the one place this differs from EmitGetFieldVec3/EmitRaycast's address-per-out-param pattern.
    private void EmitInputAction(Node node)
    {
        if (_il == null) return;

        // Dup the fresh array: one reference feeds the call (as out2), the other survives in a local
        // to read back afterward -- the call returns void, so there's no other way to reach index 0/1.
        LoadActionHandle(node);
        _il.Emit(OpCodes.Ldc_I4_2);
        _il.Emit(OpCodes.Newarr, typeof(float));
        var arrLocal = _il.DeclareLocal(typeof(float[]));
        _il.Emit(OpCodes.Dup);
        _il.Emit(OpCodes.Stloc, arrLocal);
        _il.Emit(OpCodes.Call, ActionValue2Method);

        _il.Emit(OpCodes.Ldloc, arrLocal);
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Ldelem_R4);
        _il.Emit(OpCodes.Stloc, RequirePinLocal(node, "x"));

        _il.Emit(OpCodes.Ldloc, arrLocal);
        _il.Emit(OpCodes.Ldc_I4_1);
        _il.Emit(OpCodes.Ldelem_R4);
        _il.Emit(OpCodes.Stloc, RequirePinLocal(node, "y"));

        // held: a separate ABI call (see doc comment above for why two calls, not one). Optional like
        // EmitGetForward's "success": stored when declared, popped otherwise to balance the stack.
        LoadActionHandle(node);
        _il.Emit(OpCodes.Call, ActionHeldMethod);
        if (_pinLocals.TryGetValue((node.Id, "held"), out var heldLocal)) _il.Emit(OpCodes.Stloc, heldLocal);
        else                                                              _il.Emit(OpCodes.Pop);
    }

    /// InputActionPressed / InputActionReleased: action-level twin of EmitInputKeyEdge -- handle in,
    /// bool "triggered" out, node type picks aver_fw_action_pressed vs _released. Same state-vs-event
    /// reasoning as EmitInputKeyEdge (state pin here is `held`, not `down`).
    private void EmitInputActionEdge(Node node)
    {
        if (_il == null) return;

        LoadActionHandle(node);
        _il.Emit(OpCodes.Call, node.Type.ToLowerInvariant() == "inputactionreleased"
                                   ? ActionReleasedMethod : ActionPressedMethod);

        if (_pinLocals.TryGetValue((node.Id, "triggered"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Loads the action handle an InputAction/InputActionPressed/InputActionReleased node reads,
    /// shared by their three LoadPin-based emitters (EmitInputAction x2, EmitInputActionEdge; see
    /// Node.ActionName for the full trade). With a node-line `action=` attribute, resolves by
    /// name at runtime via ActionHandleForGraph (a cached, idempotent-by-name aver_fw_action_find)
    /// and never reads the `action` pin; with ActionName null (graphs authored before this
    /// attribute existed), this is exactly LoadPin(node.Id, "action").
    private void LoadActionHandle(Node node)
    {
        if (_il == null) return;

        if (!string.IsNullOrEmpty(node.ActionName))
        {
            _il.Emit(OpCodes.Ldstr, node.ActionName);
            _il.Emit(OpCodes.Call, ActionHandleForGraphMethod);
            return;
        }

        LoadPin(node.Id, "action");
    }

    /// LoadActionHandle's twin for the PULL emitters (EmitPullInputAction and the
    /// inputactionpressed/inputactionreleased cases below): same branch, but EmitPullInput in place
    /// of LoadPin, since the exec compiler never populates `_pinLocals` for a data node.
    private void PullActionHandle(Node node)
    {
        if (_il == null) return;

        if (!string.IsNullOrEmpty(node.ActionName))
        {
            _il.Emit(OpCodes.Ldstr, node.ActionName);
            _il.Emit(OpCodes.Call, ActionHandleForGraphMethod);
            return;
        }

        EmitPullInput(node, "action");
    }

    /// GetActionKey(slot) -> key, bound: which physical key/button/axis is bound to action=<Name>'s
    /// slot-th binding (GetActionKeyForGraph, itself EnhancedInput.TryGetBindingKey's slot counting --
    /// walking pushed contexts highest-priority first, same order SaveBindings walks). action= is
    /// required at compile time -- unlike InputAction, there's no `action` pin to fall back to,
    /// mirroring Spawn's class=/SaveGame's path=. One native call returns a single int (-1 =
    /// unbound); `key` and `bound` (`key != -1`, the same Ceq idiom as EmitCompare's notequal arm)
    /// both come from that one captured local, not two calls.
    private void EmitGetActionKey(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.ActionName))
            throw new InvalidOperationException(
                $"GetActionKey node '{node.Id}' has no action= attribute naming which action to read");

        _il.Emit(OpCodes.Ldstr, node.ActionName);
        LoadPin(node.Id, "slot");
        _il.Emit(OpCodes.Call, GetActionKeyMethod);

        var keyRaw = _il.DeclareLocal(typeof(int));
        _il.Emit(OpCodes.Stloc, keyRaw);

        _il.Emit(OpCodes.Ldloc, keyRaw);
        if (_pinLocals.TryGetValue((node.Id, "key"), out var keyLocal)) _il.Emit(OpCodes.Stloc, keyLocal);
        else _il.Emit(OpCodes.Pop);

        if (_pinLocals.TryGetValue((node.Id, "bound"), out var boundLocal))
        {
            _il.Emit(OpCodes.Ldloc, keyRaw);
            _il.Emit(OpCodes.Ldc_I4_M1);
            _il.Emit(OpCodes.Ceq);
            _il.Emit(OpCodes.Ldc_I4_0);
            _il.Emit(OpCodes.Ceq);
            _il.Emit(OpCodes.Stloc, boundLocal);
        }
    }

    /// GetPressedKey() -> key, pressed: the lowest-valued key/mouse-button down this frame
    /// (GetPressedKeyForGraph, itself Input.FirstKeyPressedThisFrame, -1 when nothing is down) --
    /// the read a rebinding UI's "press a key to bind" prompt needs. No attribute, no pins in;
    /// otherwise same shape as EmitGetActionKey above.
    private void EmitGetPressedKey(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Call, GetPressedKeyMethod);

        var keyRaw = _il.DeclareLocal(typeof(int));
        _il.Emit(OpCodes.Stloc, keyRaw);

        _il.Emit(OpCodes.Ldloc, keyRaw);
        if (_pinLocals.TryGetValue((node.Id, "key"), out var keyLocal)) _il.Emit(OpCodes.Stloc, keyLocal);
        else _il.Emit(OpCodes.Pop);

        if (_pinLocals.TryGetValue((node.Id, "pressed"), out var pressedLocal))
        {
            _il.Emit(OpCodes.Ldloc, keyRaw);
            _il.Emit(OpCodes.Ldc_I4_M1);
            _il.Emit(OpCodes.Ceq);
            _il.Emit(OpCodes.Ldc_I4_0);
            _il.Emit(OpCodes.Ceq);
            _il.Emit(OpCodes.Stloc, pressedLocal);
        }
    }

    /// MouseDelta() -> deltaX, deltaY, wheel: one native call (aver_fw_input_mouse), mirroring
    /// EmitGetFieldVec3's shape (see MouseDeltaForGraph for why PUSH needs more). Zero data inputs.
    private void EmitMouseDelta(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "deltaX"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "deltaY"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "wheel"));
        _il.Emit(OpCodes.Call, MouseDeltaMethod);
    }

    /// MoveAxis() -> forward, right: MouseDelta's MOVE-input sibling (see EmitMouseDelta) --
    /// GraphInterop.MoveAxisForGraph explains why Z is not a pin.
    private void EmitMoveAxis(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "forward"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "right"));
        _il.Emit(OpCodes.Call, MoveAxisMethod);
    }

    /// MouseDelta's own version of IsExecCapableQueryType -- separate native surface (polled input,
    /// not physics), different arity (3 out-params vs 5) and cost class (memcpy, not a BVH walk), so
    /// it shares Raycast's caching shape without the caching reason (see MouseDeltaForGraph). Kept
    /// separate so a future continuous-input type has its own spot.
    private static bool IsExecCapableMouseDeltaType(string type) =>
        type.Equals("mousedelta", StringComparison.OrdinalIgnoreCase);

    /// MoveAxis's own version of IsExecCapableMouseDeltaType (see that comment) -- different native
    /// surface (Input.MoveAxis's ~8 GetKey reads, not one mouse call) and arity (2 out-params not 3).
    private static bool IsExecCapableMoveAxisType(string type) =>
        type.Equals("moveaxis", StringComparison.OrdinalIgnoreCase);

    /// SaveInputBindings/LoadInputBindings/ResetInputBindings' own version of IsExecCapableSaveLoadType
    /// -- one predicate, one emitter (EmitExecInputBindingOp), for the same "no attribute, three
    /// near-identical calls" reason SaveGame/LoadGame share one. Refused by the PULL compiler
    /// entirely, same as SaveGame/LoadGame: Save writes to disk, Load and Reset both overwrite every
    /// pushed context's live bindings.
    private static bool IsExecCapableInputBindingOpType(string type) => type.ToLowerInvariant() switch
    {
        "saveinputbindings" or "loadinputbindings" or "resetinputbindings" => true,
        _ => false,
    };

    /// RebindAction's own version -- action= is REQUIRED (see EmitExecRebindAction), and this is
    /// refused by the PULL compiler for the same reason SetVar is: mutating one binding is a side
    /// effect with no notion of "when" in a pure-dataflow graph.
    private static bool IsExecCapableRebindActionType(string type) =>
        type.Equals("rebindaction", StringComparison.OrdinalIgnoreCase);

    /// Runs SaveInputBindings/LoadInputBindings/ResetInputBindings' native call once, when reached --
    /// the one emitter all three share (see IsExecCapableInputBindingOpType), mirroring
    /// EmitExecSaveLoad's shape except no path= attribute and no entity pulled: all three act on
    /// every pushed EnhancedInput context.
    private void EmitExecInputBindingOp(Node node)
    {
        if (_il == null) return;

        MethodInfo method = node.Type.ToLowerInvariant() switch
        {
            "saveinputbindings" => SaveInputBindingsMethod,
            "loadinputbindings" => LoadInputBindingsMethod,
            _ => ResetInputBindingsMethod,
        };
        _il.Emit(OpCodes.Call, method);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
        {
            var successLocal = GetOrCreateExecLocal(node.Id, "success", typeof(bool));
            _il.Emit(OpCodes.Stloc, successLocal);
        }
        else
        {
            _il.Emit(OpCodes.Pop);
        }
    }

    /// Runs a RebindAction node's write once, when reached -- mirrors EmitExecSaveLoad's shape (no
    /// entity pulled, a compile-time-required node-line attribute), but for a different reason than
    /// SaveGame/LoadGame's path=: RebindAction has no pin at all that could name the action. Unlike
    /// InputAction/InputActionPressed/InputActionReleased, where an Int `action` pin is the fallback
    /// when action= is absent (see Node.ActionName's own comment), RebindAction has no pin fallback
    /// at all.
    private void EmitExecRebindAction(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.ActionName))
            throw new InvalidOperationException(
                $"RebindAction node '{node.Id}' has no action= attribute naming which action to rebind");

        _il.Emit(OpCodes.Ldstr, node.ActionName);
        EmitPullInput(node, "slot");
        EmitPullInput(node, "key");
        _il.Emit(OpCodes.Call, RebindActionMethod);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
        {
            var successLocal = GetOrCreateExecLocal(node.Id, "success", typeof(bool));
            _il.Emit(OpCodes.Stloc, successLocal);
        }
        else
        {
            _il.Emit(OpCodes.Pop);
        }
    }

    /// Runs a MouseDelta native read once, when reached -- mirrors EmitExecRaycast's "one call, N
    /// results into N exec-locals" shape (zero inputs, since MouseDelta takes none), gated by
    /// IsExecCapableMouseDeltaType instead of IsExecCapableQueryType. See GraphInterop.MouseDeltaForGraph
    /// for why this needs the exec-cached shape, and EmitPullOutput's "no case for mousedelta/moveaxis"
    /// note for what happens if never visited by exec.
    private void EmitExecMouseDelta(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "deltaX", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "deltaY", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "wheel", typeof(float)));
        _il.Emit(OpCodes.Call, MouseDeltaMethod);
    }

    /// MoveAxis's own version of EmitExecMouseDelta -- see that method's comment, which applies
    /// unchanged here.
    private void EmitExecMoveAxis(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "forward", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "right", typeof(float)));
        _il.Emit(OpCodes.Call, MoveAxisMethod);
    }

    /// PULL half of EmitInputAction -- mirrors EmitPullVec3Read's "recompute per reader" shape rather
    /// than caching: an "x"/"y" pull re-runs aver_fw_action_value2 in full and discards the unwanted
    /// half; "held" runs the separate aver_fw_action_held call, so pulling all three costs three
    /// native calls. Safe since both ABI calls are pure, idempotent array-scan reads (see
    /// EmitInputAction).
    private void EmitPullInputAction(Node source, string pinName)
    {
        if (_il == null) return;

        if (pinName == "held")
        {
            PullActionHandle(source);
            _il.Emit(OpCodes.Call, ActionHeldMethod);
            return;
        }

        // x/y: same Newarr + Dup shape EmitInputAction uses -- see that method's own comment for why
        // aver_fw_action_value2's `float[]` out-parameter needs it instead of a plain Ldloca address.
        PullActionHandle(source);
        _il.Emit(OpCodes.Ldc_I4_2);
        _il.Emit(OpCodes.Newarr, typeof(float));
        var arrLocal = _il.DeclareLocal(typeof(float[]));
        _il.Emit(OpCodes.Dup);
        _il.Emit(OpCodes.Stloc, arrLocal);
        _il.Emit(OpCodes.Call, ActionValue2Method);

        _il.Emit(OpCodes.Ldloc, arrLocal);
        _il.Emit(OpCodes.Ldc_I4, pinName == "x" ? 0 : 1);
        _il.Emit(OpCodes.Ldelem_R4);
    }

    /// PULL half of EmitGetActionKey -- recomputes per reader rather than caching (EmitPullOutput's
    /// own "no memoization" contract), so pulling both `key` and `bound` pays the native call twice
    /// rather than sharing one result the way the topological compiler's local can. Safe for the same
    /// reason EmitPullInputAction accepts the same cost: GetActionKeyForGraph is a pure, idempotent
    /// read. `bound` reuses the call result via the same Ceq/Ceq "not equal to -1" idiom
    /// EmitGetActionKey uses, since there's no second native entry point for it.
    private void EmitPullGetActionKey(Node source, string pinName)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(source.ActionName))
            throw new InvalidOperationException(
                $"GetActionKey node '{source.Id}' has no action= attribute naming which action to read");

        _il.Emit(OpCodes.Ldstr, source.ActionName);
        EmitPullInput(source, "slot");
        _il.Emit(OpCodes.Call, GetActionKeyMethod);

        if (pinName == "key") return;

        _il.Emit(OpCodes.Ldc_I4_M1);
        _il.Emit(OpCodes.Ceq);
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Ceq);
    }

    /// PULL half of EmitGetPressedKey -- same recompute-per-reader shape as EmitPullGetActionKey
    /// immediately above, with no attribute and no inputs to pull first.
    private void EmitPullGetPressedKey(Node source, string pinName)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Call, GetPressedKeyMethod);

        if (pinName == "key") return;

        _il.Emit(OpCodes.Ldc_I4_M1);
        _il.Emit(OpCodes.Ceq);
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Ceq);
    }
    // Aver.Framework internals, reached the same way as Aver.Scene's Native above -- see
    // Aver.Framework.csproj's InternalsVisibleTo("Aver.Graph") grant.
    private static readonly MethodInfo InputKeyMethod =
        typeof(Fw).GetMethod("aver_fw_input_key", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key was not found by reflection");
    // The edge-triggered pair beside aver_fw_input_key -- separate ABI entry points because that's
    // how the framework exposes them; a selector flag here would duplicate "pressed" logic.
    private static readonly MethodInfo InputKeyPressedMethod =
        typeof(Fw).GetMethod("aver_fw_input_key_pressed", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key_pressed was not found by reflection");
    private static readonly MethodInfo InputKeyReleasedMethod =
        typeof(Fw).GetMethod("aver_fw_input_key_released", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key_released was not found by reflection");
    // InputAction's own twin of aver_fw_input_key/_pressed/_released above -- see EmitInputAction's own
    // comment for why "action" is a HANDLE (an int the caller already holds), not a name.
    private static readonly MethodInfo ActionValue2Method =
        typeof(Fw).GetMethod("aver_fw_action_value2", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_value2 was not found by reflection");
    private static readonly MethodInfo ActionHeldMethod =
        typeof(Fw).GetMethod("aver_fw_action_held", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_held was not found by reflection");
    private static readonly MethodInfo ActionPressedMethod =
        typeof(Fw).GetMethod("aver_fw_action_pressed", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_pressed was not found by reflection");
    private static readonly MethodInfo ActionReleasedMethod =
        typeof(Fw).GetMethod("aver_fw_action_released", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_released was not found by reflection");
    // LoadActionHandle/PullActionHandle's target when a node names its action by NODE-line attribute
    // (Node.ActionName) instead of an Int pin -- GraphInterop, not Fw directly, because it caches the
    // aver_fw_action_find lookup per name (see Node.ActionName's own comment).
    private static readonly MethodInfo ActionHandleForGraphMethod =
        typeof(GraphInterop).GetMethod("ActionHandleForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.ActionHandleForGraph was not found by reflection");
    private static readonly MethodInfo MouseDeltaMethod =
        typeof(GraphInterop).GetMethod("MouseDeltaForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.MouseDeltaForGraph was not found by reflection");
    private static readonly MethodInfo MoveAxisMethod =
        typeof(GraphInterop).GetMethod("MoveAxisForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.MoveAxisForGraph was not found by reflection");
    // SaveInputBindings/LoadInputBindings/ResetInputBindings/RebindAction/GetActionKey/GetPressedKey --
    // the rebindable-input family beside SaveGame/LoadGame above, same reflection-by-name treatment.
    private static readonly MethodInfo SaveInputBindingsMethod =
        typeof(GraphInterop).GetMethod("SaveInputBindingsForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SaveInputBindingsForGraph was not found by reflection");
    private static readonly MethodInfo LoadInputBindingsMethod =
        typeof(GraphInterop).GetMethod("LoadInputBindingsForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.LoadInputBindingsForGraph was not found by reflection");
    private static readonly MethodInfo ResetInputBindingsMethod =
        typeof(GraphInterop).GetMethod("ResetInputBindingsForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.ResetInputBindingsForGraph was not found by reflection");
    private static readonly MethodInfo RebindActionMethod =
        typeof(GraphInterop).GetMethod("RebindActionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RebindActionForGraph was not found by reflection");
    private static readonly MethodInfo GetActionKeyMethod =
        typeof(GraphInterop).GetMethod("GetActionKeyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GetActionKeyForGraph was not found by reflection");
    private static readonly MethodInfo GetPressedKeyMethod =
        typeof(GraphInterop).GetMethod("GetPressedKeyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GetPressedKeyForGraph was not found by reflection");
}
