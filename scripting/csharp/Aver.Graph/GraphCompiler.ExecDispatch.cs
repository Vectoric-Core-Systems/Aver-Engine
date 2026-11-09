// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The exec walk of CompileEntryPoint: per-node dispatch, exec fan-out and the exec-capable node classification.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// Emits IL for one step of the exec walk: run `node`'s own logic (if any), then hand control on.
    /// `_execVisiting` guards against a hand-authored exec LINK cycle that skips while/forEach's
    /// internal loop-back, throwing a compile error instead of recursing forever. A diamond (two
    /// `branch` arms reaching the same downstream node) is not a cycle: `_execVisiting` only tracks
    /// the current recursion path, cleared in `finally`, so the false arm can still reach a node the
    /// true arm already visited -- cost is that shared node's IL gets emitted twice (inlined per arm)
    /// rather than joined with a jump; a real basic-block compiler is out of scope here.
    private void EmitExecNode(Node node)
    {
        if (_il == null) return;
        if (!_execVisiting.Add(node.Id))
            throw new InvalidOperationException(
                $"exec cycle detected at node '{node.Id}' while compiling event '{_currentEventName}' -- " +
                "a node's exec chain reached itself with no while/forEach in between. A loop must go " +
                "through a while/forEach node (its 'loop' exec-output pin is what repeats); a direct " +
                "exec link back to an earlier node is not supported and would compile-recurse forever.");

        // Every exec node reports that it ran, from this one site -- covers
        // branch/switchInt/doOnce/gate/flipFlop/while/forEach and the default bucket in one edit,
        // and can't be forgotten for a future node type. Two compile-time constants and a call: the
        // receiving side is a static bool test when the editor isn't looking (RecordNodeHitForGraph),
        // so an uninstrumented-feeling graph really is uninstrumented at runtime. Keyed by graph name,
        // not entity: the method's arguments come from the graph's own PARAM list, so there's no
        // "entity is always argument 0" to key on (a graph with no entity PARAM has no entity to
        // name), and the editor's canvas shows a class anyway.
        _il.Emit(OpCodes.Ldstr, _graph.Name);
        _il.Emit(OpCodes.Ldstr, node.Id);
        _il.Emit(OpCodes.Call, RecordNodeHitMethod);

        // The debugger's breakpoint check, emitted ONLY when it was armed before this graph compiled --
        // unarmed, the IL is exactly what it was without a debugger (see GraphDebug).
        if (GraphDebug.Armed)
        {
            _il.Emit(OpCodes.Ldstr, _graph.Name);
            _il.Emit(OpCodes.Ldstr, node.Id);
            _il.Emit(OpCodes.Call, DebugNodeReachedMethod);
        }

        try
        {
            switch (node.Type.ToLowerInvariant())
            {
                case "branch":
                    EmitBranch(node);
                    return; // branch owns its own fan-out (exactly one of two arms); no generic fan-out after it
                case "switchint":
                    EmitSwitchInt(node);
                    return; // same reason as branch: picks ONE arm; a generic fan-out would run the rest too
                case "doonce":
                    EmitDoOnce(node);
                    return;
                case "gate":
                    EmitGate(node);
                    return;
                case "flipflop":
                    EmitFlipFlop(node);
                    return;
                case "while":
                    EmitWhile(node);
                    return;
                case "foreach":
                    EmitForEach(node);
                    return;
                default:
                    // Every other node type reached via exec: run its side effect if it has one
                    // (SetField/SetFieldVec3/Spawn/SetVar -- separate predicate/emitter pairs, see
                    // each IsExecCapable*Type) or its cached
                    // query if it has one (today, only Raycast), then fall through to the generic
                    // fan-out, which handles a plain node with 0/1/N exec-output pins.
                    if (IsExecCapableSideEffectType(node.Type)) EmitExecSideEffect(node);
                    else if (IsExecCapableVecSideEffectType(node.Type)) EmitExecSetFieldVec3(node);
                    else if (IsExecCapableSpawnType(node.Type)) EmitExecSpawn(node);
                    else if (IsExecCapableVarSideEffectType(node.Type)) EmitExecSetVar(node);
                    else if (IsExecCapableQueryType(node.Type)) EmitExecRaycast(node);
                    else if (IsExecCapableMouseDeltaType(node.Type)) EmitExecMouseDelta(node);
                    else if (IsExecCapableMoveAxisType(node.Type)) EmitExecMoveAxis(node);
                    else if (IsExecCapableSetParentType(node.Type)) EmitExecSetParent(node);
                    else if (IsExecCapableSetViewEntityType(node.Type)) EmitExecSetViewEntity(node);
                    else if (IsExecCapableSetNameType(node.Type)) EmitExecSetName(node);
                    else if (IsExecCapableSetMeshType(node.Type)) EmitExecSetMesh(node);
                    else if (IsExecCapableSetMaterialType(node.Type)) EmitExecSetMaterial(node);
                    else if (IsExecCapableAttachToSocketType(node.Type)) EmitExecAttachToSocket(node);
                    else if (IsExecCapableSetSkeletonType(node.Type)) EmitExecSetSkeleton(node);
                    else if (IsExecCapablePlayAnimationType(node.Type)) EmitExecPlayAnimation(node);
                    else if (IsExecCapableSetControlRigType(node.Type)) EmitExecSetControlRig(node);
                    else if (IsExecCapableCharacterMoveType(node.Type)) EmitExecCharacterMove(node);
                    else if (IsExecCapableJumpType(node.Type)) EmitJump(node);
                    else if (IsExecCapablePrintType(node.Type)) EmitExecPrint(node);
                    else if (IsExecCapableCallFuncType(node.Type)) EmitExecCallFunc(node);
                    else if (IsExecCapableFuncReturnType(node.Type)) EmitFuncReturnStores(node);
                    else if (IsExecCapableApiCallType(node.Type)) EmitExecApiCall(node);
                    else if (IsExecCapableTransformWriteType(node.Type)) EmitExecTransformWrite(node);
                    else if (IsExecCapablePhysicsWriteType(node.Type)) EmitExecPhysicsWrite(node);
                    else if (IsExecCapablePhysicsCreateType(node.Type)) EmitExecPhysicsCreate(node);
                    else if (IsExecCapableJointCreateType(node.Type)) EmitExecJointCreate(node);
                    else if (IsExecCapableJointOpType(node.Type)) EmitExecJointOp(node);
                    else if (IsExecCapableSphereCastType(node.Type)) EmitExecSphereCast(node);
                    else if (IsExecCapablePhysicsQueryType(node.Type)) EmitExecPhysicsQuery(node);
                    else if (IsExecCapableRagdollType(node.Type)) EmitExecRagdoll(node);
                    else if (IsExecCapableFireEventType(node.Type)) EmitExecFireEvent(node);
                    else if (IsExecCapableSaveLoadType(node.Type)) EmitExecSaveLoad(node);
                    else if (IsExecCapableInputBindingOpType(node.Type)) EmitExecInputBindingOp(node);
                    else if (IsExecCapableRebindActionType(node.Type)) EmitExecRebindAction(node);
                    else if (IsExecCapableCreateEntityType(node.Type)) EmitExecCreateEntity(node);
                    else if (IsExecCapableAudioType(node.Type)) EmitExecAudio(node);
                    else if (GameSystemNodes.IsExec(node.Type)) EmitExecGeneric(node);
                    EmitExecFanOut(node);
                    return;
            }
        }
        finally
        {
            _execVisiting.Remove(node.Id);
        }
    }

    /// Fires every declared EXEC-typed OUTPUT pin on `node`, in node.Pins order -- file order for an
    /// explicit PIN record, catalog order for a default-pinned node (OcGraphParser.AddDefaultPins /
    /// GraphNodeDefs.hpp). See the section comment above for why this alone implements "Sequence".
    private void EmitExecFanOut(Node node)
    {
        if (_il == null) return;

        var execOuts = node.Pins.Where(p => p.IsOutput && p.Type == PinType.Exec).ToList();

        // "fireLog": opt-in observability, not part of the control-flow contract -- a no-op unless the
        // node declares an int output pin named "fireLog" (Sequence's default pins do). Updated as
        // `fireLog = fireLog*10 + (armIndex+1)` before each arm, so three arms in order leaves
        // fireLog == 123; needed since there's no live scene to observe a write (GraphHostTests.cs
        // has the same limitation), and a pure pull can't prove a node was actually visited.
        var fireLogPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "fireLog" && p.Type == PinType.Int);
        LocalBuilder? fireLog = null;
        if (fireLogPin != null)
        {
            fireLog = GetOrCreateExecLocal(node.Id, "fireLog", typeof(int));
            _il.Emit(OpCodes.Ldc_I4_0);
            _il.Emit(OpCodes.Stloc, fireLog);
        }

        for (int i = 0; i < execOuts.Count; i++)
        {
            if (fireLog != null)
            {
                _il.Emit(OpCodes.Ldloc, fireLog);
                _il.Emit(OpCodes.Ldc_I4, 10);
                _il.Emit(OpCodes.Mul);
                _il.Emit(OpCodes.Ldc_I4, i + 1);
                _il.Emit(OpCodes.Add);
                _il.Emit(OpCodes.Stloc, fireLog);
            }

            Node? target = FindExecTarget(node.Id, execOuts[i].Name);
            if (target != null) EmitExecNode(target);
        }
    }

    /// Resolves the single exec LINK leaving `nodeId.pinName`; null means unwired, which is not an
    /// error (same permissiveness LoadPin extends to an unwired DATA input, defaulting rather than failing).
    private Node? FindExecTarget(string nodeId, string pinName)
    {
        var links = _graph.Links.Where(l => l.SourceNodeId == nodeId && l.SourcePinName == pinName).ToList();
        if (links.Count == 0) return null;
        if (links.Count > 1)
            throw new InvalidOperationException(
                $"exec output '{nodeId}.{pinName}' drives {links.Count} links -- an exec output can " +
                "only continue to ONE place, unlike a data output pin (which may fan out to many " +
                "readers): wire a Sequence node here if more than one thing should run from this point");
        var link = links[0];
        if (!_graph.Nodes.TryGetValue(link.TargetNodeId, out var target))
            throw new InvalidOperationException(
                $"exec link from '{nodeId}.{pinName}' targets unknown node '{link.TargetNodeId}'");
        return target;
    }

    /// Node types with a real side effect worth running when the exec walk reaches them directly.
    /// Today, only SetField -- see the section-level PUSH VS PULL comment, point 2, for why this list
    /// exists at all and why EmitPullOutput refuses to pull FROM one of these instead.
    private static bool IsExecCapableSideEffectType(string type) =>
        type.Equals("setfield", StringComparison.OrdinalIgnoreCase);

    /// Node types with no side effect that still want the exec walk's "compute once per visit, cache
    /// into _execLocals" shape -- today, only Raycast. Separate from IsExecCapableSideEffectType
    /// because the reason differs: SetField is on the exec chain for correctness (pulling twice would
    /// silently write twice); Raycast is there for cost (pulling twice re-runs an expensive query) --
    /// EmitPullOutput could compute a query type fresh, it just doesn't today, by choice.
    private static bool IsExecCapableQueryType(string type) =>
        type.Equals("raycast", StringComparison.OrdinalIgnoreCase);

    /// SetFieldVec3's own version of IsExecCapableSideEffectType -- separate because SetField/
    /// SetFieldVec3 write through different wrappers, arities, and field-kind checks (F32 vs Vec3);
    /// folding them would force EmitExecSideEffect to type-switch internally.
    private static bool IsExecCapableVecSideEffectType(string type) =>
        type.Equals("setfieldvec3", StringComparison.OrdinalIgnoreCase);

    /// Runs a SetField node's write exactly once, when the exec walk reaches it -- mirrors
    /// EmitSetField's field=/resolver/native-call logic, but pulls "entity"/"value" via EmitPullInput,
    /// not LoadPin/_pinLocals. A declared "success" pin captures the real return code into an
    /// exec-local (same mechanism as branch's "tookTrue"); otherwise discarded.
    private void EmitExecSideEffect(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.FieldName))
            throw new InvalidOperationException($"SetField node '{node.Id}' has no field= attribute naming which scene field to write");
        if (!_fieldResolver(node.FieldName, out int fieldId, out int kind))
            throw new InvalidOperationException($"SetField node '{node.Id}' references unknown scene field '{node.FieldName}'");
        if (kind != FieldKindF32)
            throw new InvalidOperationException(
                $"SetField node '{node.Id}' field '{node.FieldName}' is not an F32 field (kind={kind}); " +
                "setfield only supports F32 fields today, not Vec3/Quat/Bool/I32/etc");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        EmitPullInput(node, "value");
        _il.Emit(OpCodes.Call, SetFieldMethod);

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
}
