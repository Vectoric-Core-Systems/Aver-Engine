// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The topological (Compile) pass: the per-node emit switch for pure dataflow graphs.

using System;
using System.Collections.Generic;
using System.Linq;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// Emits IL code for a single node.
    private void EmitNode(Node node)
    {
        if (_il == null) return;

        // Locals for DATA output pins only: PinTypeToCLRType(PinType.Exec) is typeof(void) and
        // DeclareLocal(typeof(void)) throws. A pure-dataflow node never legitimately has one, but
        // this guards a flow node (branch/while/forEach) reaching Compile() (the old PULL-only path)
        // instead of CompileEntryPoint() -- the switch below still throws NotSupportedException for
        // it either way; this just makes that the first failure instead of an unrelated
        // ArgumentException from the runtime.
        foreach (var pin in node.Pins.Where(p => p.IsOutput && p.Type != PinType.Exec))
        {
            var local = _il.DeclareLocal(PinTypeToCLRType(pin.Type));
            _pinLocals[(node.Id, pin.Name)] = local;
        }

        // Emit code based on node type.
        switch (node.Type.ToLowerInvariant())
        {
            case "constfloat":
            case "const_f32":
                EmitConstFloat(node);
                break;

            case "constint":
            case "const_i32":
                EmitConstInt(node);
                break;

            case "constbool":
            case "const_bool":
                EmitConstBool(node);
                break;

            // Scalar operators, PUSH path: operands pushed from stored pin locals into one shared
            // emitter. The matching PULL arm is in EmitPullOutput -- a node present in one and absent
            // from the other is the exact defect shape this file has hit repeatedly.
            case "not": case "abs": case "negate":
            case "sqrt": case "floor": case "ceil":
            case "round": case "saturate":
                EmitScalarUnary(node);
                break;
            case "and": case "or":
            case "xor": case "greater":
            case "greaterequal": case "less":
            case "lessequal": case "equal":
            case "notequal": case "min": case "max":
            case "mod": case "pow":
                EmitScalarBinary(node);
                break;
            case "clamp": case "lerp":
                EmitScalarTernary(node);
                break;

            case "add":
                EmitAdd(node);
                break;

            case "multiply":
                EmitMultiply(node);
                break;

            case "vecadd":
            case "vecsub":
            case "vecscale":
            case "veccross":
            case "vecnormalize":
            case "veclerp":
            case "vecdot":
            case "veclength":
            case "vecdistance":
                EmitVecNode(node);
                break;

            case "compare":
            case "compare_f32":
                EmitCompare(node);
                break;

            case "getfield":
                EmitGetField(node);
                break;

            case "setfield":
                EmitSetField(node);
                break;

            case "getfieldvec3":
                EmitGetFieldVec3(node);
                break;

            case "getforward":
            case "get_forward":
                EmitGetForward(node);
                break;

            case "getviewentity":
            case "get_view_entity":
                EmitGetViewEntity(node);
                break;

            case "getvelocity":
                EmitGetVelocity(node);
                break;

            // Pure CallFunc in a pure-dataflow file's event graph, via EmitSimpleApiRead's "call,
            // store results in pin locals" shape; the impure case never reaches here since Compile()
            // has no exec walk.
            case "callfunc":
                EmitCallFuncTopological(node);
                break;

            case "reroutefloat":
            case "rerouteint":
            case "reroutebool":
                EmitSimpleApiRead(node);
                break;

            case "getbodyposition":
            case "getbodyvelocity":
            case "isbodyvalid":
            case "getbodycount":
            case "raycastany":
                EmitSimpleApiRead(node);
                break;

            // Physics reads: pure (handle in, value out), so welcome here like the five above.
            case "getbodyangularvelocity":
            case "getbodymass":
            case "getbodymotiontype":
            case "isbodyactive":
            case "getbodylayer":
            case "getjointvalue":
                EmitSimpleApiRead(node);
                break;

            case "getworldposition":
            case "getentityforward":
            case "getentityright":
            case "getentityup":
            case "getlocalscale":
            case "isalive":
            case "isactor":
            case "getsynapsetarget":
            case "synapsesteer":
            case "getsynapseperception":
                EmitSimpleApiRead(node);
                break;

            case "inttofloat":
            case "booltofloat":
            case "floattoint":
                EmitSimpleApiRead(node);
                break;
            case "isgrounded":
            case "charactercanstand":
            case "hastag":
            case "gettags":
            case "getplayerpawn":
            case "getplayercontroller":
            case "getgamemode":
            case "isplaying":
                EmitSimpleApiRead(node);
                break;

            case "setfieldvec3":
                // Like "setfield": topological pass visits each non-exec-only node exactly once, so
                // this is one deterministic write, not the double-write risk EmitPullOutput's
                // refusal (below) guards against (a different risk -- its own recursive pull
                // mechanism). See EmitSetField's "dual-reachable" reasoning.
                EmitSetFieldVec3(node);
                break;

            case "setparent":
                // Same dual-reachable reasoning as "setfieldvec3" above -- see
                // OcGraphParser.AddDefaultPins's setparent/setviewentity/setname comment.
                EmitSetParent(node);
                break;

            case "setviewentity":
                EmitSetViewEntity(node);
                break;

            case "setname":
                EmitSetName(node);
                break;

            case "isphysicsready":
            case "getfixedstep":
            case "findentity":
                // Pure reads, welcome here like getbodycount/getworldposition (EmitPullOutput above).
                EmitSimpleApiRead(node);
                break;

            case "issoundplaying":
                // The one pure audio read -- checking playback state changes nothing.
                EmitSimpleApiRead(node);
                break;

            case "playsound":
            case "playsoundat":
            case "stopsound":
            case "setlistener":
            case "setbusvolume":
                // SIDE EFFECTS (sound, listener, volume) -- refused for the same reason as Spawn/
                // CreateEntity below: an ungated pull would make a sound (etc.) every invocation.
                throw new InvalidOperationException(
                    $"{node.Type} node '{node.Id}' cannot be compiled by Compile() -- it has a side " +
                    "effect with no notion of 'when' in a pure-dataflow graph, and Compile()'s " +
                    "topological pass would run it unconditionally on every invocation with no way " +
                    "to gate it. Give this node an ENTRY-driven exec chain and reach it through " +
                    "CompileEntryPoint() instead.");

            case "createentity":
                // A SIDE EFFECT: mints a new entity -- same reason as "spawn" below: an ungated pull
                // would mint one per invocation.
                throw new InvalidOperationException(
                    $"CreateEntity node '{node.Id}' cannot be compiled by Compile() -- creating an " +
                    "entity is a side effect with no notion of 'when' in a pure-dataflow graph, and " +
                    "Compile()'s topological pass would run it unconditionally on every invocation " +
                    "with no way to gate it. Give this node an ENTRY-driven exec chain and reach it " +
                    "through CompileEntryPoint() instead.");

            case "setmesh":
                EmitSetMesh(node);
                break;

            case "setmaterial":
                EmitSetMaterial(node);
                break;

            case "attachtosocket":
                EmitAttachToSocket(node);
                break;

            case "getanimcurve":
                EmitGetAnimCurve(node);
                break;

            case "setskeleton":
                EmitSetSkeleton(node);
                break;

            case "playanimation":
                EmitPlayAnimation(node);
                break;

            case "setcontrolrig":
                EmitSetControlRig(node);
                break;

            case "sin":
                EmitSin(node);
                break;

            case "cos":
                EmitCos(node);
                break;

            case "subtract":
            case "sub":
                EmitSubtract(node);
                break;

            case "divide":
            case "div":
                EmitDivide(node);
                break;

            case "param":
            case "getparam":
                EmitParam(node);
                break;

            case "select":
                EmitSelect(node);
                break;

            case "inputkey":
                EmitInputKey(node);
                break;

            case "inputkeypressed":
            case "inputkeyreleased":
                EmitInputKeyEdge(node);
                break;

            case "inputaction":
                EmitInputAction(node);
                break;

            case "inputactionpressed":
            case "inputactionreleased":
                EmitInputActionEdge(node);
                break;

            case "getactionkey":
                EmitGetActionKey(node);
                break;

            case "getpressedkey":
                EmitGetPressedKey(node);
                break;

            case "raycast":
                EmitRaycast(node);
                break;

            case "mousedelta":
                EmitMouseDelta(node);
                break;

            case "moveaxis":
                EmitMoveAxis(node);
                break;

            case "spawn":
                // Side-effecting (creates a new entity; see IsExecCapableSpawnType) -- refused
                // explicitly rather than skipped via IsExecOnlyNodeType (no signal) or run
                // unconditionally like SetField/SetFieldVec3 (harmless to repeat): Spawn has no such
                // pass, so ticked every frame with no branch to gate it, an ungated Spawn would mint
                // a new entity every tick.
                throw new InvalidOperationException(
                    $"Spawn node '{node.Id}' cannot be compiled by Compile() -- spawning an entity is a " +
                    "side effect with no notion of 'when' in a pure-dataflow graph, and Compile()'s " +
                    "topological pass would run it unconditionally on every invocation with no way to " +
                    "gate it. Give this node an ENTRY-driven exec chain and reach it through " +
                    "CompileEntryPoint() instead.");

            case "charactermove":
                // Drives a real actor (yaw/pitch/capsule velocity via AverCharacter.Drive; see
                // IsExecCapableCharacterMoveType). Same reason as "spawn" -- must run only when the
                // exec chain reaches it, not per arbitrary pull.
                throw new InvalidOperationException(
                    $"CharacterMove node '{node.Id}' cannot be compiled by Compile() -- driving a " +
                    "character is a side effect with no notion of 'when' in a pure-dataflow graph, " +
                    "and Compile()'s topological pass would run it unconditionally on every " +
                    "invocation with no way to gate it. Give this node an ENTRY-driven exec chain " +
                    "and reach it through CompileEntryPoint() instead.");

            case "fireevent":
                // Runs ANOTHER ENTITY'S WHOLE EXEC CHAIN, not a scalar write (IsExecCapableFireEventType).
                // Worse than spawn/charactermove: ungated, it'd run a stranger's OnHit every pull.
                throw new InvalidOperationException(
                    $"FireEvent node '{node.Id}' cannot be compiled by Compile() -- firing an event " +
                    "runs another entity's exec chain and is a side effect with no notion of 'when' " +
                    "in a pure-dataflow graph, and Compile()'s topological pass would run it " +
                    "unconditionally on every invocation with no way to gate it. Give this node an " +
                    "ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.");

            case "savegame":
            case "loadgame":
                // Writes or REPLACES THE ENTIRE WORLD. LoadGame is worst case: tears down and
                // rebuilds everything, including whatever entity's graph pulled it.
                throw new InvalidOperationException(
                    $"{(node.Type.Equals("savegame", StringComparison.OrdinalIgnoreCase) ? "SaveGame" : "LoadGame")} " +
                    $"node '{node.Id}' cannot be compiled by Compile() -- it is a side effect with no " +
                    "notion of 'when' in a pure-dataflow graph, and Compile()'s topological pass would " +
                    "run it unconditionally on every invocation with no way to gate it. Give this node " +
                    "an ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.");

            case "saveinputbindings":
            case "loadinputbindings":
            case "resetinputbindings":
                // SaveGame/LoadGame's rebindable-input twin: Save writes to disk, Load/Reset both
                // overwrite every pushed context's live bindings -- none safe to run unconditionally
                // on every pull.
                throw new InvalidOperationException(
                    $"{node.Type} node '{node.Id}' cannot be compiled by Compile() -- it is a side " +
                    "effect with no notion of 'when' in a pure-dataflow graph, and Compile()'s " +
                    "topological pass would run it unconditionally on every invocation with no way " +
                    "to gate it. Give this node an ENTRY-driven exec chain and reach it through " +
                    "CompileEntryPoint() instead.");

            case "rebindaction":
                // Side-effecting: mutates one binding, same reason as "setvar" below -- a write with
                // no notion of "when" in a pure-dataflow graph.
                throw new InvalidOperationException(
                    $"RebindAction node '{node.Id}' cannot be compiled by Compile() -- rebinding an " +
                    "action is a side effect with no notion of 'when' in a pure-dataflow graph, and " +
                    "Compile()'s topological pass would run it unconditionally on every invocation " +
                    "with no way to gate it. Give this node an ENTRY-driven exec chain and reach it " +
                    "through CompileEntryPoint() instead.");

            case "getvar":
                // A pure read (see IsExecCapableVarSideEffectType) -- unlike SetVar, welcome here like GetField.
                EmitGetVar(node);
                break;

            case "setvar":
                // A WRITE IS A SIDE EFFECT, same reason as "spawn" above. Unlike SetField/SetFieldVec3
                // (safe to repeat), a variable write has no such excuse and the PULL path must refuse
                // it (see TestSetVarPulledWithoutExecVisitFailsClearly).
                throw new InvalidOperationException(
                    $"SetVar node '{node.Id}' cannot be compiled by Compile() -- writing a variable is a " +
                    "side effect with no notion of 'when' in a pure-dataflow graph, and Compile()'s " +
                    "topological pass would run it unconditionally on every invocation with no way to " +
                    "gate it. Give this node an ENTRY-driven exec chain and reach it through " +
                    "CompileEntryPoint() instead.");

            default:
                // The table-driven families (GraphGameSystemNodes.cs): a pure read is computed here once
                // into its pin locals; an exec node is refused like every other side effect.
                if (GameSystemNodes.TryGet(node.Type, out var gameSpec))
                {
                    if (gameSpec.Kind == GenericKind.Pure) { EmitGenericPureTopological(node, gameSpec); break; }
                    if (gameSpec.Kind == GenericKind.Exec) throw GenericPushOnly(node);
                }
                // A PUSH-only node here is not an unknown node type -- saying so sent authors looking
                // for a node they already had. See IsPushOnlySideEffectType.
                if (IsPushOnlySideEffectType(node.Type))
                    throw new InvalidOperationException(
                        $"{node.Type} node '{node.Id}' cannot be compiled by Compile() -- it has a " +
                        "side effect, and a pure-dataflow graph has no notion of WHEN to run one: " +
                        "Compile()'s topological pass would run it unconditionally on every " +
                        "invocation, with no branch structure available to gate it. The node type is " +
                        "supported; this compiler is not the one that runs it. Give the node an " +
                        "ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.");
                throw new NotSupportedException($"Node type '{node.Type}' is not supported");
        }
    }
}
