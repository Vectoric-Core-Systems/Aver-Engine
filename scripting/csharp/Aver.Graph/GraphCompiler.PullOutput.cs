// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The pull side of the exec compiler: resolving a node input or output to a value on the stack.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// Pulls the value linked into `node`'s input pin `pinName`: follows a LINK into EmitPullOutput,
    /// falls back to a PINVAL, then to the pin's zero-ish default -- LoadPin's same three-step
    /// fallback, re-emitted here since the exec compiler never populates `_pinLocals` for data nodes.
    private void EmitPullInput(Node node, string pinName)
    {
        EmitPullInputCore(node, pinName);
        if (_il == null || !GraphDebug.Armed) return;

        // Armed: copy the value just pushed to the debugger on its way past (widened to double), so
        // the editor can label the wire. Data inputs only; an exec pin carries no value.
        var pin = node.Pins.FirstOrDefault(p => p.Name == pinName && !p.IsOutput);
        if (pin == null || !PinTypeRules.IsNumeric(pin.Type)) return;
        var tmp = _il.DeclareLocal(PinTypeToCLRType(pin.Type));
        _il.Emit(OpCodes.Stloc, tmp);
        _il.Emit(OpCodes.Ldstr, _graph.Name);
        _il.Emit(OpCodes.Ldstr, node.Id);
        _il.Emit(OpCodes.Ldstr, pinName);
        _il.Emit(OpCodes.Ldloc, tmp);
        _il.Emit(OpCodes.Conv_R8);
        _il.Emit(OpCodes.Call, DebugRecordPinMethod);
        _il.Emit(OpCodes.Ldloc, tmp);
    }

    /// Emits a PINVAL as its pin's declared type -- the parser typed it by the literal's shape, and a
    /// float literal on an int pin (or a bool on a float pin) used to emit IL the runtime refuses with
    /// an "invalid program" naming no node. A value that cannot fit the pin is refused here, naming it.
    private void EmitPinnedValue(string nodeId, string nodeType, PinnedValue pv, Pin? pin)
    {
        if (_il == null) return;
        object v = pv.Value;
        if (pin != null && pin.Type != PinType.Exec)
        {
            if (!PinnedValue.TryFit(pin.Type, pv.Value, out v, out var why))
                throw new InvalidOperationException(
                    $"PINVAL on node '{nodeId}' ({nodeType}) pin '{pv.PinName}' ({pin.Type}): value '{pv.Value}' does not fit -- {why}");
        }
        if (v is float f) _il.Emit(OpCodes.Ldc_R4, f);
        else if (v is int i) _il.Emit(OpCodes.Ldc_I4, i);
        else if (v is bool b) _il.Emit(OpCodes.Ldc_I4, b ? 1 : 0);
        else if (v is System.Numerics.Vector3 vec) EmitVector3(vec);
        else if (v is string s) _il.Emit(OpCodes.Ldstr, s);
    }

    private void EmitPullInputCore(Node node, string pinName)
    {
        if (_il == null) return;

        var link = _graph.Links.FirstOrDefault(l => l.TargetNodeId == node.Id && l.TargetPinName == pinName);
        if (link != null)
        {
            if (!_graph.Nodes.TryGetValue(link.SourceNodeId, out var sourceNode))
                throw new InvalidOperationException(
                    $"link into '{node.Id}.{pinName}' names unknown source node '{link.SourceNodeId}'");
            EmitPullOutput(sourceNode, link.SourcePinName);
            return;
        }

        var pin = node.Pins.FirstOrDefault(p => p.Name == pinName);

        var pv = _graph.PinnedValues.FirstOrDefault(p => p.NodeId == node.Id && p.PinName == pinName);
        if (pv != null)
        {
            EmitPinnedValue(node.Id, node.Type, pv, pin);
            return;
        }

        // Absent is not the same as unconnected. `pin == null` means the emitter asked for an input
        // this node doesn't have -- the state any hand-written PIN record leaves it in, since one
        // explicit PIN suppresses every default AddDefaultPins would add. The old code fell back to
        // `pin?.Type ?? PinType.Float` and pushed a float zero regardless, so a missing `entity` (an
        // int32) got a float32 instead -- not a wrong number but IL the runtime refuses to run, with
        // a "Common Language Runtime detected an invalid program" message naming neither node, pin,
        // nor PIN record.
        // A pin that exists but has no incoming link still falls through to zero: an Add with only
        // `a` wired legally means "a + 0", and samples in this repo rely on it.
        if (pin == null)
            throw new InvalidOperationException(
                $"node '{node.Id}' ({node.Type}) has no input pin '{pinName}' to read. " +
                $"A node's default pins are suppressed entirely as soon as it declares ANY pin by hand, " +
                $"so if this node has PIN records, it needs one for '{pinName}' too (or a LINK into it)");

        EmitZeroOf(pin.Type, pin.StructName);
    }

    /// Every node type only the PUSH compiler can run: a side effect, a write, or a creation with no
    /// meaning in a pure-dataflow graph. Not the same predicate as IsExecCapableSideEffectType:
    /// SetField has exec pins AND a data output someone might wrongly try to read, so it stays
    /// reachable to refuse with its own error; the kinds below have no data output at all. Was a
    /// seventeen-term disjunction inline in EmitPullOutput, needed in two places (here and EmitNode's
    /// default arm, which lacked it) so a stray Jump/PrintInt/one of the eleven physics writers,
    /// compiled by Compile(), gets an accurate refusal rather than "not supported" (the old message,
    /// true of neither compiler, implied there was no Jump node at all). Spawn/CharacterMove/
    /// FireEvent/SetVar/SaveGame/LoadGame keep their own hand-written cases above deliberately, since
    /// each says something more specific (Spawn creates an entity every tick) that folding into this
    /// generic predicate would lose.
    private static bool IsPushOnlySideEffectType(string type) =>
        IsExecCapableSideEffectType(type) || IsExecCapableVecSideEffectType(type) ||
        IsExecCapableSpawnType(type) || IsExecCapableVarSideEffectType(type) ||
        IsExecCapableSetParentType(type) || IsExecCapableSetViewEntityType(type) ||
        IsExecCapableSetNameType(type) || IsExecCapableSetMeshType(type) ||
        IsExecCapableSetMaterialType(type) || IsExecCapableSetSkeletonType(type) ||
        IsExecCapablePlayAnimationType(type) || IsExecCapableSetControlRigType(type) ||
        IsExecCapableCharacterMoveType(type) ||
        IsExecCapableFireEventType(type) || IsExecCapableJumpType(type) ||
        IsExecCapablePrintType(type) || IsExecCapableApiCallType(type) ||
        IsExecCapableTransformWriteType(type) || IsExecCapablePhysicsWriteType(type) ||
        IsExecCapablePhysicsCreateType(type) || IsExecCapableSaveLoadType(type) ||
        IsExecCapableJointCreateType(type) || IsExecCapableJointOpType(type) ||
        IsExecCapablePhysicsQueryType(type) || IsExecCapableRagdollType(type) ||
        IsExecCapableInputBindingOpType(type) || IsExecCapableRebindActionType(type) ||
        DataNodes.IsExec(type) || GameSystemNodes.IsExec(type);

    /// Node kinds that exist only to be walked by the exec/PUSH compiler, with no data value to
    /// pull -- Compile() skips these rather than failing, since a graph may carry both halves with
    /// no interaction. "onhit"/"customevent" sit beside "onstart"/"ontick" purely for shape (a bare
    /// exec-output trigger with no data value, per OcGraphParser.AddDefaultPins), not because this
    /// compiler cares when GraphHost fires them or that an event's name is author-chosen -- any
    /// future trigger-only node belongs here for the same reason. "funcentry" joins them since its
    /// data outputs are the enclosing method's arguments, which no topological pass can compute --
    /// belt-and-braces, since it only appears inside a function body the event graph's pass already
    /// skips wholesale.
    private static bool IsExecOnlyNodeType(string type) => type.ToLowerInvariant() switch
    {
        "onstart" or "ontick" or "onhit" or "customevent" or "branch" or "sequence" or "while" or "foreach"
            or "funcentry" or "switchint" or "foreachinarray" => true,
        _ => GameSystemNodes.IsTrigger(type),
    };

    /// Pushes node `source`'s output pin `pinName` onto the IL stack, computed fresh every call (no
    /// memoization -- see the section header). Checked first against `_execLocals` (a live loop
    /// counter or captured side-effect result), then dispatched by node type for pure expressions.
    /// SetField (and any IsExecCapableSideEffectType type) is refused here on purpose.
    private void EmitPullOutput(Node source, string pinName)
    {
        if (_il == null) return;

        if (_execLocals.TryGetValue((source.Id, pinName), out var local))
        {
            _il.Emit(OpCodes.Ldloc, local);
            return;
        }

        // Generalised from a SetField-only message the moment a second side-effecting type
        // (SetFieldVec3) existed -- names source.Type, so the message stays accurate for whichever
        // side-effecting type triggered it and for any future type IsPushOnlySideEffectType grows to
        // cover (see TestSetVarPulledWithoutExecVisitFailsClearly).
        if (IsPushOnlySideEffectType(source.Type))
            throw new InvalidOperationException(
                $"'{source.Id}.{pinName}' cannot be read as a data value: {source.Type} has a side effect " +
                "and must be reached by wiring it directly into the exec chain (give it exec pins), not " +
                "by pulling its output from somewhere else -- pulling could run its write more than once, " +
                "or not at all, depending on what else reads it");

        // See _pullVisiting's declaration for why an uncaught cycle here kills the process rather
        // than raising something catchable; reported as an ordinary error naming both ends.
        if (!_pullVisiting.Add(source.Id))
            throw new InvalidOperationException(
                $"data cycle detected while evaluating '{source.Id}.{pinName}': this node's value " +
                "depends, through a chain of data links, on itself. Data links must form a directed " +
                "acyclic graph -- only exec links may loop, and only through While/ForEach, which are " +
                "bounded. Break the cycle by removing one of the data links into this node.");
        try
        {

        // Pure reads from the table-driven families (GraphGameSystemNodes.cs).
        if (GameSystemNodes.TryGet(source.Type, out var gameSpec) && gameSpec.Kind == GenericKind.Pure)
        {
            EmitGenericPurePull(source, gameSpec, pinName);
            return;
        }

        if (DataNodes.IsPure(source.Type))
        {
            EmitPureDataPull(source, pinName);
            return;
        }

        switch (source.Type.ToLowerInvariant())
        {
            case "constfloat":
            case "const_f32":
            {
                float v = 0f;
                var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == source.Id);
                if (co?.Value is float f) v = f;
                _il.Emit(OpCodes.Ldc_R4, v);
                return;
            }
            case "constint":
            case "const_i32":
            {
                int v = 0;
                var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == source.Id);
                if (co?.Value is int i) v = i;
                _il.Emit(OpCodes.Ldc_I4, v);
                return;
            }
            case "constbool":
            case "const_bool":
            {
                bool v = false;
                var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == source.Id);
                if (co?.Value is bool b) v = b;
                _il.Emit(OpCodes.Ldc_I4, v ? 1 : 0);
                return;
            }
            // ---- scalar operators, PULL path (see the PUSH arm) ---------------------
            case "not": case "abs": case "negate":
            case "sqrt": case "floor": case "ceil":
            case "round": case "saturate":
                EmitPullInput(source, "a"); EmitScalarOp(source.Type); return;
            case "and": case "or":
            case "xor": case "greater":
            case "greaterequal": case "less":
            case "lessequal": case "equal":
            case "notequal": case "min": case "max":
            case "mod": case "pow":
                EmitPullInput(source, "a"); EmitPullInput(source, "b");
                EmitScalarOp(source.Type); return;
            case "clamp":
                EmitPullInput(source, "a"); EmitPullInput(source, "min");
                EmitPullInput(source, "max"); EmitScalarOp(source.Type); return;
            case "lerp":
                EmitPullInput(source, "a"); EmitPullInput(source, "b");
                EmitPullInput(source, "t"); EmitScalarOp(source.Type); return;
            case "add":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Add); return;
            case "multiply":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Mul); return;
            case "vecadd":
            case "vecsub":
            case "vecscale":
            case "veccross":
            case "vecnormalize":
            case "veclerp":
            case "vecdot":
            case "veclength":
            case "vecdistance":
                EmitVecComponent(source, pinName); return;
            case "subtract":
            case "sub":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Sub); return;
            case "divide":
            case "div":
                EmitPullDivide(source); return;
            case "compare":
            case "compare_f32":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Cgt); return;
            case "sin":
                EmitPullInput(source, "a");
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathSinMethod); _il.Emit(OpCodes.Conv_R4);
                return;
            case "cos":
                EmitPullInput(source, "a");
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathCosMethod); _il.Emit(OpCodes.Conv_R4);
                return;
            case "getfield":
                EmitPullGetField(source); return;
            case "getfieldvec3":
                EmitPullGetFieldVec3(source, pinName); return;
            case "getforward":
            case "get_forward":
                EmitPullGetForward(source, pinName); return;
            case "getviewentity":
            case "get_view_entity":
                EmitPullGetViewEntity(source, pinName); return;
            case "getvelocity":
                EmitPullGetVelocity(source, pinName); return;
            // Inside a function body only. Both are legal to PULL: an argument is always available,
            // and a pure call is by definition safe to evaluate whenever a reader asks.
            case "funcentry":
                EmitFuncEntryLoad(pinName); return;
            case "callfunc":
                EmitPullCallFunc(source, pinName); return;
            // A data reroute IS its input. No instruction of its own, in either compiler.
            case "reroutefloat":
            case "rerouteint":
            case "reroutebool":
                EmitPullInput(source, "a"); return;
            case "getbodyposition":
                EmitPullVec3Read(source, pinName, BodyPositionMethod, -1, "body"); return;
            case "getbodyvelocity":
                EmitPullVec3Read(source, pinName, BodyVelocityMethod, -1, "body"); return;
            case "isbodyvalid":
                EmitPullInput(source, "body"); _il.Emit(OpCodes.Call, BodyValidMethod); return;
            case "getbodycount":
                _il.Emit(OpCodes.Call, BodyCountMethod); return;
            case "getbodyangularvelocity":
                EmitPullVec3Read(source, pinName, BodyAngularVelocityMethod, -1, "body"); return;
            case "isbodyactive":
                EmitPullInput(source, "body"); _il.Emit(OpCodes.Call, BodyActiveMethod); return;
            case "getbodymass":
                EmitPullScalarRead(source, pinName, BodyMassMethod, typeof(float), "body"); return;
            case "getbodymotiontype":
                EmitPullScalarRead(source, pinName, BodyMotionTypeMethod, typeof(int), "body"); return;
            case "getbodylayer":
                EmitPullScalarRead(source, pinName, BodyLayerMethod, typeof(int), "body"); return;
            case "getjointvalue":
                EmitPullScalarRead(source, pinName, JointValueMethod, typeof(float), "joint"); return;
            case "isphysicsready":
                _il.Emit(OpCodes.Call, PhysicsReadyMethod); return;
            case "getfixedstep":
                _il.Emit(OpCodes.Call, PhysicsFixedStepMethod); return;
            case "findentity": {
                // name= is edit-time data, so it is pushed as an Ldstr literal exactly as SetName's
                // own emit does -- there is no string PIN it could arrive on.
                if (string.IsNullOrEmpty(source.NameValue))
                    throw new InvalidOperationException(
                        $"FindEntity node '{source.Id}' has no name= attribute naming the entity to look up");
                _il.Emit(OpCodes.Ldstr, source.NameValue);
                var foundL = _il.DeclareLocal(typeof(int));
                _il.Emit(OpCodes.Ldloca, foundL);
                _il.Emit(OpCodes.Call, FindEntityMethod);
                if (pinName == "found") return;   // the bool return IS that pin
                _il.Emit(OpCodes.Pop);
                _il.Emit(OpCodes.Ldloc, foundL);
                return;
            }
            case "raycastany":
                EmitPullInput(source, "originX");
                EmitPullInput(source, "originY");
                EmitPullInput(source, "originZ");
                EmitPullInput(source, "dirX");
                EmitPullInput(source, "dirY");
                EmitPullInput(source, "dirZ");
                EmitPullInput(source, "maxDist");
                _il.Emit(OpCodes.Call, RaycastAnyMethod); return;
            case "getworldposition":
                EmitPullVec3Read(source, pinName, WorldPositionMethod, -1); return;
            case "getentityforward":
                EmitPullVec3Read(source, pinName, EntityAxisMethod, 0); return;
            case "getentityright":
                EmitPullVec3Read(source, pinName, EntityAxisMethod, 1); return;
            case "getentityup":
                EmitPullVec3Read(source, pinName, EntityAxisMethod, 2); return;
            case "getlocalscale":
                EmitPullVec3Read(source, pinName, LocalScaleMethod, -1); return;
            case "getsynapsetarget":
                EmitPullVec3Read(source, pinName, SynapseGetTargetMethod, -1); return;
            case "synapsesteer":
                EmitPullSynapseSteer(source, pinName); return;
            case "getsynapseperception":
                EmitPullSynapsePerception(source, pinName); return;
            case "issoundplaying":
                EmitPullInput(source, "voice");
                _il.Emit(OpCodes.Call, IsSoundPlayingMethod); return;
            case "isalive":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, IsAliveMethod); return;
            case "isactor":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, IsActorMethod); return;
            // int and bool are both I4 on the CIL stack, so widening either to a float is the same
            // one instruction -- the node types differ so the GRAPH can tell them apart, not the IL.
            case "inttofloat":
            case "booltofloat":
                EmitPullInput(source, "a"); _il.Emit(OpCodes.Conv_R4); return;
            case "floattoint":
                EmitPullInput(source, "a"); _il.Emit(OpCodes.Conv_I4); return;
            case "isgrounded":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, IsGroundedMethod); return;
            case "charactercanstand":
                EmitPullCharacterCanStand(source, pinName); return;
            case "hastag":
                EmitPullInput(source, "entity"); EmitPullInput(source, "mask");
                _il.Emit(OpCodes.Call, HasTagMethod); return;
            case "gettags":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, TagsMethod); return;
            case "getplayerpawn":
                EmitPullInput(source, "index"); _il.Emit(OpCodes.Call, PlayerPawnMethod); return;
            case "getplayercontroller":
                EmitPullInput(source, "index"); _il.Emit(OpCodes.Call, PlayerControllerMethod); return;
            case "getgamemode":
                _il.Emit(OpCodes.Call, GameModeMethod); return;
            case "isplaying":
                _il.Emit(OpCodes.Call, IsPlayingMethod); return;
            case "getvar":
                EmitPullGetVar(source); return;
            case "param":
            case "getparam":
            {
                int index = _graph.Parameters.FindIndex(p => p.Name == source.ParamName);
                if (index < 0)
                    throw new InvalidOperationException(
                        $"Param node '{source.Id}' references undeclared parameter '{source.ParamName}'");
                _il.Emit(OpCodes.Ldarg, (short)index);
                return;
            }
            case "inputkey":
                EmitPullInput(source, "key"); _il.Emit(OpCodes.Call, InputKeyMethod); return;
            case "inputkeypressed":
                EmitPullInput(source, "key"); _il.Emit(OpCodes.Call, InputKeyPressedMethod); return;
            case "inputkeyreleased":
                EmitPullInput(source, "key"); _il.Emit(OpCodes.Call, InputKeyReleasedMethod); return;
            case "inputaction":
                EmitPullInputAction(source, pinName); return;
            case "inputactionpressed":
                PullActionHandle(source); _il.Emit(OpCodes.Call, ActionPressedMethod); return;
            case "inputactionreleased":
                PullActionHandle(source); _il.Emit(OpCodes.Call, ActionReleasedMethod); return;
            case "getactionkey":
                EmitPullGetActionKey(source, pinName); return;
            case "getpressedkey":
                EmitPullGetPressedKey(source, pinName); return;
            // GetAnimCurve gets a standalone pull path, unlike Raycast/MouseDelta/MoveAxis below:
            // those three must run exactly once however many pins are read, so behaving differently
            // on and off the exec chain would be a trap. A curve read has neither property -- it
            // has one output and no side effect, same number on re-read since the playhead doesn't
            // move between pulls.
            case "getanimcurve":
                if (string.IsNullOrEmpty(source.CurveName))
                    throw new InvalidOperationException(
                        $"GetAnimCurve node '{source.Id}' has no curve= attribute naming which curve to read");
                EmitPullInput(source, "entity");
                _il.Emit(OpCodes.Ldstr, source.CurveName);
                _il.Emit(OpCodes.Call, GetAnimCurveMethod);
                return;
            case "select":
            {
                // Mirrors EmitSelect's branch shape but pulled (recursive, uncached), not stored to
                // _pinLocals -- see the section header for why the exec compiler re-emits rather than
                // caches. Unlike EmitSelect's PULL-compiler branch (both arms already ran during the
                // topological walk, so it only skips which local gets loaded), this is a real
                // short-circuit: EmitPullInput recursively runs only the chosen arm's subgraph, so
                // only one of ifTrue/ifFalse's cost is paid per pull.
                EmitPullInput(source, "cond");
                var elseLabel = _il.DefineLabel();
                var endLabel = _il.DefineLabel();
                _il.Emit(OpCodes.Brfalse, elseLabel);
                EmitPullInput(source, "ifTrue");
                _il.Emit(OpCodes.Br, endLabel);
                _il.MarkLabel(elseLabel);
                EmitPullInput(source, "ifFalse");
                _il.MarkLabel(endLabel);
                return;
            }
            // "raycast" (and "mousedelta"/"moveaxis", same reason) has no case here, deliberately:
            // reached via the exec chain, it populates _execLocals for all five outputs
            // (EmitExecRaycast), checked before this switch runs, so a visited one needs no dispatch.
            // One never visited by exec falls to `default` and reports a clear NotSupportedException
            // -- a deliberate Phase-1 limitation: unlike GetField, no standalone pull path, since one
            // would let a node type behave differently on and off the exec chain, a worse trap than a
            // clear error. A pure-pull graph (no ENTRY) never reaches this method at all; EmitRaycast
            // (PULL) handles it there. MouseDelta/MoveAxis inherit this shape for consistency, not cost.
            default:
                throw new NotSupportedException(
                    $"node type '{source.Type}' cannot be pulled as a data value inside an exec chain " +
                    "(it is not one of the pure expression kinds this compiler knows, and has no exec " +
                    "pins reaching it directly either)");
        }

        }
        finally
        {
            // In finally, not after the switch, since EVERY arm above returns or throws (no
            // fallthrough) -- removing on the way out is what keeps a diamond legal while a cycle isn't.
            _pullVisiting.Remove(source.Id);
        }
    }
}
