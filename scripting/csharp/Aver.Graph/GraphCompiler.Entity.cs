// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Entity nodes: transform, parent, name, spawn, raycast, velocity, view entity and direction reads.

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
    /// GetForward(entity) -> x,y,z + eyeX,eyeY,eyeZ + success: look direction and eye position in
    /// one call. Pure/idempotent like GetFieldVec3, so no _execLocals caching needed. Six
    /// out-parameters, one native call (same shape as EmitGetFieldVec3/EmitRaycast): Ldloca per
    /// output, never Ldloc, since the callee writes through them.
    private void EmitGetForward(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "x"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "y"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "z"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "eyeX"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "eyeY"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "eyeZ"));
        _il.Emit(OpCodes.Call, LookDirectionMethod);

        // Bool return always consumed: stored when `success` is declared, popped otherwise -- left
        // on the stack it would unbalance the method.
        if (_pinLocals.TryGetValue((node.Id, "success"), out var local)) _il.Emit(OpCodes.Stloc, local);
        else                                                            _il.Emit(OpCodes.Pop);
    }

    private void EmitExecTransformWrite(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        switch (node.Type.ToLowerInvariant())
        {
            case "translate":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, TranslateMethod); break;
            case "setlocalscale":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetLocalScaleMethod); break;
            case "setlocalposition":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetLocalPositionMethod); break;
            // Pin names are yaw/pitch/roll, not x/y/z (order matches SetLocalRotationForGraph) --
            // x/y/z would be a trap, since an author couldn't tell which axis is which.
            case "setlocalrotation":
                EmitPullInput(node, "yaw"); EmitPullInput(node, "pitch"); EmitPullInput(node, "roll");
                _il.Emit(OpCodes.Call, SetLocalRotationMethod); break;
            case "lookat":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, LookAtMethod); break;
            default:
                _il.Emit(OpCodes.Call, DestroyEntityMethod); break;
        }
        // Into an exec local, not _pinLocals (see EmitExecSideEffect) -- identical to the physics
        // writer above.
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// GetVelocity(entity) -> x, y, z + success. Same shape as EmitPullGetForward: one native call,
    /// several pins.
    private void EmitPullGetVelocity(Node node, string pinName)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        var xL = _il.DeclareLocal(typeof(float));
        var yL = _il.DeclareLocal(typeof(float));
        var zL = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, xL);
        _il.Emit(OpCodes.Ldloca, yL);
        _il.Emit(OpCodes.Ldloca, zL);
        _il.Emit(OpCodes.Call, VelocityMethod);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        _il.Emit(OpCodes.Ldloc, pinName == "x" ? xL : pinName == "y" ? yL : zL);
    }

    /// The PULL compiler's visit of GetVelocity: every declared output gets its own call, which
    /// costs one extra native read per pin and keeps this emitter the same shape as the pull one.
    private void EmitGetVelocity(Node node)
    {
        if (_il == null) return;
        foreach (var p in node.Pins)
        {
            if (!p.IsOutput) continue;
            if (!_pinLocals.TryGetValue((node.Id, p.Name), out var local)) continue;
            EmitPullGetVelocity(node, p.Name);
            _il.Emit(OpCodes.Stloc, local);
        }
    }

    /// GetViewEntity(entity) -> view + success. Same shape as EmitGetForward, one out-parameter wide.
    private void EmitGetViewEntity(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "view"));
        _il.Emit(OpCodes.Call, ViewEntityMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local)) _il.Emit(OpCodes.Stloc, local);
        else                                                            _il.Emit(OpCodes.Pop);
    }

    /// The PULL half of GetViewEntity. Both paths, for the reason EmitPullGetForward states.
    private void EmitPullGetViewEntity(Node node, string pinName)
    {
        if (_il == null) return;

        EmitPullInput(node, "entity");
        var viewLocal = _il.DeclareLocal(typeof(int));
        _il.Emit(OpCodes.Ldloca, viewLocal);
        _il.Emit(OpCodes.Call, ViewEntityMethod);

        if (pinName == "success") return;   // the bool return IS that pin
        _il.Emit(OpCodes.Pop);
        if (pinName != "view")
            throw new InvalidOperationException(
                $"GetViewEntity node '{node.Id}' has no output pin '{pinName}' (only view, success)");
        _il.Emit(OpCodes.Ldloc, viewLocal);
    }

    /// The PULL half of GetForward, mirroring EmitPullGetFieldVec3: one native call, then push the
    /// pin asked for and discard the rest -- reading x, y, eyeZ separately costs three calls, an
    /// acceptable trade since this only reads two already-computed vectors off a managed object.
    /// Both paths exist because a node implemented only in the PUSH compiler silently fails the
    /// moment something reads it through OUT or a pure graph -- a recurring bug shape here.
    private void EmitPullGetForward(Node node, string pinName)
    {
        if (_il == null) return;

        EmitPullInput(node, "entity");

        var xLocal    = _il.DeclareLocal(typeof(float));
        var yLocal    = _il.DeclareLocal(typeof(float));
        var zLocal    = _il.DeclareLocal(typeof(float));
        var eyeXLocal = _il.DeclareLocal(typeof(float));
        var eyeYLocal = _il.DeclareLocal(typeof(float));
        var eyeZLocal = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, xLocal);
        _il.Emit(OpCodes.Ldloca, yLocal);
        _il.Emit(OpCodes.Ldloca, zLocal);
        _il.Emit(OpCodes.Ldloca, eyeXLocal);
        _il.Emit(OpCodes.Ldloca, eyeYLocal);
        _il.Emit(OpCodes.Ldloca, eyeZLocal);
        _il.Emit(OpCodes.Call, LookDirectionMethod);

        // `success` IS the return value, so it is already the thing on the stack -- every other pin
        // needs that bool popped first.
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);

        var wanted = pinName switch
        {
            "x"    => xLocal,
            "y"    => yLocal,
            "z"    => zLocal,
            "eyeX" => eyeXLocal,
            "eyeY" => eyeYLocal,
            "eyeZ" => eyeZLocal,
            _ => throw new InvalidOperationException(
                $"GetForward node '{node.Id}' has no output pin '{pinName}' (only x/y/z, eyeX/eyeY/eyeZ, success)"),
        };
        _il.Emit(OpCodes.Ldloc, wanted);
    }

    /// SetParent(child, parent) -> success: reparents `child` under `parent` (0 makes it a root) --
    /// wraps aver_scene_set_parent(int32,int32) directly (scene_abi.h:105), same
    /// reflect-straight-into-Aver.Scene.Native template as GetField/SetField, no GraphInterop
    /// wrapper needed (unlike Raycast/Spawn). The ABI already refuses a cycle/self-parent/doomed
    /// parent by returning 0, not throwing, so `success` sees a live false rather than a silent
    /// no-op (same fix as SetField's old hardcoded-1 stub). See EmitExecSetParent for the PUSH twin
    /// and EmitNode's "setparent" case for why both compilers reach this.
    private void EmitSetParent(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "child");
        LoadPin(node.Id, "parent");
        _il.Emit(OpCodes.Call, SetParentMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// SetViewEntity(entity) -> (nothing): publishes which entity the camera follows -- wraps
    /// aver_fw_set_view_entity(int32)->void directly (framework_abi.h:206). No output pin exists at
    /// all (AddDefaultPins's "setviewentity" case), so nothing to Stloc or Pop; the call is the effect.
    private void EmitSetViewEntity(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Call, SetViewEntityMethod);
    }

    /// SetName(entity, name) -> success: wraps aver_scene_set_name(int32,const char*)->int32
    /// directly (scene_abi.h:113). `name` is a node-line attribute (Node.NameValue), not a pin --
    /// PinType has no String member (Graph.cs) -- same as class= for Spawn. Required at compile
    /// time, same "fail loudly" rule as GetField/SetField/Spawn's field=/class=.
    private void EmitSetName(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.NameValue))
            throw new InvalidOperationException($"SetName node '{node.Id}' has no name= attribute naming the string to write");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.NameValue);
        _il.Emit(OpCodes.Call, SetNameMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// Raycast(originX,Y,Z, dirX,Y,Z, maxDist) -> hit, entity, pointX,Y,Z: one native call, five
    /// results. PULL computes every node once per Compile() regardless of reader count, so unlike
    /// PUSH's EmitExecRaycast (needs _execLocals for the same guarantee), a plain "one call, five
    /// _pinLocals stores" already works: push 7 inputs, each output local's address via
    /// RequirePinLocal/Ldloca, Call (see RaycastForGraph for why out-params, not a struct).
    private void EmitRaycast(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "originX");
        LoadPin(node.Id, "originY");
        LoadPin(node.Id, "originZ");
        LoadPin(node.Id, "dirX");
        LoadPin(node.Id, "dirY");
        LoadPin(node.Id, "dirZ");
        LoadPin(node.Id, "maxDist");

        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "hit"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "entity"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "pointX"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "pointY"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "pointZ"));
        _il.Emit(OpCodes.Call, RaycastMethod);
    }

    /// Spawn's own version, stricter than SetField/SetFieldVec3: those are refused as a PULL only by
    /// EmitPullOutput; Spawn is also refused by EmitNode's "spawn" case in the topological pass
    /// entirely. Own predicate: SpawnForGraph resolves by name into the class registry, a different
    /// native surface than Aver.Scene's field table.
    private static bool IsExecCapableSpawnType(string type) =>
        type.Equals("spawn", StringComparison.OrdinalIgnoreCase);

    /// SetParent's own version -- SetParentForGraph (direct reflect, no wrapper) has a different
    /// arity (two entity ids) and refusal contract (cycle/self-parent, not missing-component). See
    /// OcGraphParser.AddDefaultPins's setparent/setviewentity/setname case for why this family
    /// dispatches SetField-style, not Spawn/SetVar-style.
    private static bool IsExecCapableSetParentType(string type) =>
        type.Equals("setparent", StringComparison.OrdinalIgnoreCase);

    /// SetViewEntity's own version (see IsExecCapableSetParentType). Its ABI call returns void, so
    /// EmitExecSetViewEntity/EmitSetViewEntity have no "capture or discard a return code" branch.
    private static bool IsExecCapableSetViewEntityType(string type) =>
        type.Equals("setviewentity", StringComparison.OrdinalIgnoreCase);

    /// SetName's own version (see IsExecCapableSetParentType) -- first of this family whose write
    /// carries a STRING (node.NameValue, from name=) rather than only scalar pins.
    private static bool IsExecCapableSetNameType(string type) =>
        type.Equals("setname", StringComparison.OrdinalIgnoreCase);

    private static bool IsExecCapableTransformWriteType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "translate" || t == "setlocalscale" || t == "setlocalposition" ||
               t == "setlocalrotation" || t == "lookat" || t == "destroyentity";
    }

    /// Runs a Spawn node's native call once, when the exec walk reaches it -- same shape as
    /// EmitExecSideEffect (pull inputs, Call, capture-or-discard), but class= resolves by name at
    /// runtime (SpawnForGraph), not a compile-time fieldId. Still required at compile time: an empty
    /// one can never spawn anything, so failing loudly now beats a silent entity-0 at runtime.
    private void EmitExecSpawn(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.ClassName))
            throw new InvalidOperationException($"Spawn node '{node.Id}' has no class= attribute naming which class to spawn");

        _il.Emit(OpCodes.Ldstr, node.ClassName);
        EmitPullInput(node, "x");
        EmitPullInput(node, "y");
        EmitPullInput(node, "z");
        _il.Emit(OpCodes.Call, SpawnMethod);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "entity"))
        {
            var entityLocal = GetOrCreateExecLocal(node.Id, "entity", typeof(int));
            _il.Emit(OpCodes.Stloc, entityLocal);
        }
        else
        {
            _il.Emit(OpCodes.Pop);
        }
    }

    /// CreateEntity is exec-only, same reason as Spawn: mints a bare entity (Spawn mints an actor),
    /// a side effect a dataflow pull can't gate. Same shape as EmitExecSpawn: push the name= literal,
    /// Call, capture the two outputs or discard them.
    private void EmitExecCreateEntity(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.NameValue))
            throw new InvalidOperationException(
                $"CreateEntity node '{node.Id}' has no name= attribute naming the entity to create");

        _il.Emit(OpCodes.Ldstr, node.NameValue);
        var entityLocal = _il.DeclareLocal(typeof(int));
        _il.Emit(OpCodes.Ldloca, entityLocal);
        _il.Emit(OpCodes.Call, CreateEntityMethod);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else
            _il.Emit(OpCodes.Pop);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "entity"))
        {
            _il.Emit(OpCodes.Ldloc, entityLocal);
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "entity", typeof(int)));
        }
    }

    private static bool IsExecCapableCreateEntityType(string type) =>
        type.Equals("createentity", StringComparison.OrdinalIgnoreCase);

    /// Runs a SetParent write once, when reached -- mirrors EmitSetParent's native-call shape, but
    /// pulls "child"/"parent" via EmitPullInput (not LoadPin/_pinLocals) and captures "success" into
    /// an exec-local, like EmitExecSideEffect does for SetField.
    private void EmitExecSetParent(Node node)
    {
        if (_il == null) return;

        EmitPullInput(node, "child");
        EmitPullInput(node, "parent");
        _il.Emit(OpCodes.Call, SetParentMethod);

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

    /// Runs a SetViewEntity write once, when reached -- mirrors EmitSetViewEntity but pulls "entity"
    /// via EmitPullInput. VOID means exactly that here too: no return code, so no capture/discard
    /// branch -- the Call itself is the entire effect.
    private void EmitExecSetViewEntity(Node node)
    {
        if (_il == null) return;

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Call, SetViewEntityMethod);
    }

    /// Runs a SetName write once, when reached -- mirrors EmitSetName's name=/native-call shape but
    /// pulls "entity" via EmitPullInput and captures "success" into an exec-local.
    private void EmitExecSetName(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.NameValue))
            throw new InvalidOperationException($"SetName node '{node.Id}' has no name= attribute naming the string to write");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldstr, node.NameValue);
        _il.Emit(OpCodes.Call, SetNameMethod);

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

    /// Runs a Raycast node's native query exactly once, at the point the exec walk reaches it --
    /// mirrors EmitRaycast's "one call, five results" shape, but pulls its 7 inputs through
    /// EmitPullInput rather than LoadPin, and stores each result into its own exec-local via
    /// GetOrCreateExecLocal -- the same mechanism EmitExecSideEffect uses for SetField's "success" --
    /// so a later `OUT raycastNode hit` reads the live value via EmitPullOutput's _execLocals check
    /// rather than re-deriving it (Raycast has no case of its own in EmitPullOutput's switch). Called
    /// from EmitExecNode's default case, exactly like EmitExecSideEffect, just gated by
    /// IsExecCapableQueryType instead of IsExecCapableSideEffectType.
    private void EmitExecRaycast(Node node)
    {
        if (_il == null) return;

        EmitPullInput(node, "originX");
        EmitPullInput(node, "originY");
        EmitPullInput(node, "originZ");
        EmitPullInput(node, "dirX");
        EmitPullInput(node, "dirY");
        EmitPullInput(node, "dirZ");
        EmitPullInput(node, "maxDist");

        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "hit", typeof(bool)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "entity", typeof(int)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointX", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointY", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointZ", typeof(float)));
        _il.Emit(OpCodes.Call, RaycastMethod);
    }
    private static readonly MethodInfo RaycastMethod =
        typeof(GraphInterop).GetMethod("RaycastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RaycastForGraph was not found by reflection");
    private static readonly MethodInfo SpawnMethod =
        typeof(GraphInterop).GetMethod("SpawnForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SpawnForGraph was not found by reflection");
    // SetParent/SetName reflect straight into Aver.Scene.Native, the same template GetField/SetField
    // already use (no GraphInterop wrapper -- their ABI signatures need no scalar reshaping).
    private static readonly MethodInfo SetParentMethod =
        typeof(Native).GetMethod("aver_scene_set_parent", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_set_parent was not found by reflection");
    private static readonly MethodInfo SetNameMethod =
        typeof(Native).GetMethod("aver_scene_set_name", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_set_name was not found by reflection");
    // SetViewEntity reflects straight into Aver.Framework.Fw, the same template EmitInputKey already
    // uses for aver_fw_input_key.
    private static readonly MethodInfo SetViewEntityMethod =
        typeof(Fw).GetMethod("aver_fw_set_view_entity", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_set_view_entity was not found by reflection");
    // GetForward: CharacterMove's read-side counterpart -- where the character is LOOKING, unaskable
    // before (_yaw/_pitch are private; the scene rotation is a Quat GetField/GetFieldVec3 can't read).
    private static readonly MethodInfo LookDirectionMethod =
        typeof(GraphInterop).GetMethod("LookDirectionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.LookDirectionForGraph was not found by reflection");
    private static readonly MethodInfo CreateEntityMethod =
        typeof(GraphInterop).GetMethod("CreateEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.CreateEntityForGraph was not found by reflection");
    private static readonly MethodInfo FindEntityMethod =
        typeof(GraphInterop).GetMethod("FindEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.FindEntityForGraph was not found by reflection");
    // The entity transform surfaces.
    private static readonly MethodInfo WorldPositionMethod =
        typeof(GraphInterop).GetMethod("WorldPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.WorldPositionForGraph was not found by reflection");
    private static readonly MethodInfo EntityAxisMethod =
        typeof(GraphInterop).GetMethod("EntityAxisForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.EntityAxisForGraph was not found by reflection");
    private static readonly MethodInfo LocalScaleMethod =
        typeof(GraphInterop).GetMethod("LocalScaleForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.LocalScaleForGraph was not found by reflection");
    private static readonly MethodInfo TranslateMethod =
        typeof(GraphInterop).GetMethod("TranslateForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.TranslateForGraph was not found by reflection");
    private static readonly MethodInfo SetLocalScaleMethod =
        typeof(GraphInterop).GetMethod("SetLocalScaleForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLocalScaleForGraph was not found by reflection");

    private static readonly MethodInfo SetLocalPositionMethod =
        typeof(GraphInterop).GetMethod("SetLocalPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLocalPositionForGraph was not found by reflection");

    private static readonly MethodInfo SetLocalRotationMethod =
        typeof(GraphInterop).GetMethod("SetLocalRotationForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new MissingMethodException("GraphInterop.SetLocalRotationForGraph");

    private static readonly MethodInfo LookAtMethod =
        typeof(GraphInterop).GetMethod("LookAtForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new MissingMethodException("GraphInterop.LookAtForGraph");
    private static readonly MethodInfo IsAliveMethod =
        typeof(GraphInterop).GetMethod("IsAliveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsAliveForGraph was not found by reflection");
    private static readonly MethodInfo IsActorMethod =
        typeof(GraphInterop).GetMethod("IsActorForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsActorForGraph was not found by reflection");
    private static readonly MethodInfo DestroyEntityMethod =
        typeof(GraphInterop).GetMethod("DestroyEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.DestroyEntityForGraph was not found by reflection");
    // The engine-API surfaces, reflected the same way every other GraphInterop entry point is.
    private static readonly MethodInfo VelocityMethod =
        typeof(GraphInterop).GetMethod("VelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.VelocityForGraph was not found by reflection");
    private static readonly MethodInfo IsGroundedMethod =
        typeof(GraphInterop).GetMethod("IsGroundedForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsGroundedForGraph was not found by reflection");
    private static readonly MethodInfo HasTagMethod =
        typeof(GraphInterop).GetMethod("HasTagForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.HasTagForGraph was not found by reflection");
    private static readonly MethodInfo TagsMethod =
        typeof(GraphInterop).GetMethod("TagsForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.TagsForGraph was not found by reflection");
    private static readonly MethodInfo PlayerPawnMethod =
        typeof(GraphInterop).GetMethod("PlayerPawnForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlayerPawnForGraph was not found by reflection");
    private static readonly MethodInfo PlayerControllerMethod =
        typeof(GraphInterop).GetMethod("PlayerControllerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlayerControllerForGraph was not found by reflection");
    private static readonly MethodInfo GameModeMethod =
        typeof(GraphInterop).GetMethod("GameModeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GameModeForGraph was not found by reflection");
    private static readonly MethodInfo IsPlayingMethod =
        typeof(GraphInterop).GetMethod("IsPlayingForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsPlayingForGraph was not found by reflection");
    private static readonly MethodInfo ViewEntityMethod =
        typeof(GraphInterop).GetMethod("ViewEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.ViewEntityForGraph was not found by reflection");
}
