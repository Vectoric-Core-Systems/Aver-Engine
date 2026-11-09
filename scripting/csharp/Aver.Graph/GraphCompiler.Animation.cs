// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Mesh, material, skeleton, animation, control rig and socket nodes.

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
    /// SetMesh(entity) -> success: wraps GraphInterop.SetMeshForGraph(int,string) (composes
    /// EnsureMeshRenderer + Assets.ObjectIdOf + Entity.SetMesh). `mesh` is a node-line attribute
    /// (Node.MeshPath), same reasoning as EmitSetName; required at compile time likewise.
    private void EmitSetMesh(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.MeshPath))
            throw new InvalidOperationException($"SetMesh node '{node.Id}' has no mesh= attribute naming which asset to set");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.MeshPath);
        _il.Emit(OpCodes.Call, SetMeshMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// GetAnimCurve(entity) -> value: the named curve on whatever clip `entity` is playing, at its
    /// current playhead. Wraps GraphInterop.GetAnimCurveForGraph(int,string). Pure (reads/writes
    /// nothing), so no exec twin, no IsExecCapable predicate, absent from IsPushOnlySideEffectType.
    private void EmitGetAnimCurve(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.CurveName))
            throw new InvalidOperationException($"GetAnimCurve node '{node.Id}' has no curve= attribute naming which curve to read");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.CurveName);
        _il.Emit(OpCodes.Call, GetAnimCurveMethod);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// AttachToSocket(entity, parent) -> success: hangs `entity` on a named socket of `parent`'s rig.
    /// Wraps GraphInterop.AttachToSocketForGraph(int,int,string). Two pin loads in argument order --
    /// stack must carry entity then parent then socket string; swapping the two compiles fine but
    /// attaches the rig to the gun.
    private void EmitAttachToSocket(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.SocketName))
            throw new InvalidOperationException($"AttachToSocket node '{node.Id}' has no socket= attribute naming which socket to hang on");

        LoadPin(node.Id, "entity");
        LoadPin(node.Id, "parent");
        _il.Emit(OpCodes.Ldstr, node.SocketName);
        _il.Emit(OpCodes.Call, AttachToSocketMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// SetMaterial(entity) -> success: sets the material by name -- mirrors EmitSetMesh exactly, see
    /// that method's comment, wrapping GraphInterop.SetMaterialForGraph(int,string) instead.
    private void EmitSetMaterial(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.MaterialName))
            throw new InvalidOperationException($"SetMaterial node '{node.Id}' has no material= attribute naming which material to set");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.MaterialName);
        _il.Emit(OpCodes.Call, SetMaterialMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// SetSkeleton(entity) -> success: binds a skeleton asset by path -- mirrors EmitSetMesh exactly,
    /// see that method's comment, wrapping GraphInterop.SetSkeletonForGraph(int,string) instead.
    private void EmitSetSkeleton(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.SkeletonPath))
            throw new InvalidOperationException($"SetSkeleton node '{node.Id}' has no skeleton= attribute naming which asset to bind");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.SkeletonPath);
        _il.Emit(OpCodes.Call, SetSkeletonMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// PlayAnimation(entity, loop) -> success: mirrors EmitSetMesh plus a second pin load (loop)
    /// before the call, since PlayAnimationForGraph takes (int,string,bool).
    private void EmitPlayAnimation(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.ClipPath))
            throw new InvalidOperationException($"PlayAnimation node '{node.Id}' has no clip= attribute naming which clip to play");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.ClipPath);
        LoadPin(node.Id, "loop");
        _il.Emit(OpCodes.Call, PlayAnimationMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// SetControlRig(entity, weight) -> success: binds an .ocrig by path. Shaped like
    /// EmitPlayAnimation (second pin load before the call), since SetControlRigForGraph takes
    /// (int,string,float).
    private void EmitSetControlRig(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.RigPath))
            throw new InvalidOperationException($"SetControlRig node '{node.Id}' has no rig= attribute naming which rig to bind");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldstr, node.RigPath);
        // An unwired weight is 1, not 0 -- see IsPinUnwired for why this one pin earns the exception.
        if (IsPinUnwired(node.Id, "weight")) _il.Emit(OpCodes.Ldc_R4, 1.0f);
        else LoadPin(node.Id, "weight");
        _il.Emit(OpCodes.Call, SetControlRigMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop);
    }

    /// SetMesh's own version -- unlike SetParent/SetName (reflect straight into Aver.Scene.Native),
    /// this goes through a GraphInterop wrapper (SetMeshForGraph), needing Entity's internal
    /// constructor like GetFieldVecForGraph/SetFieldVecForGraph.
    private static bool IsExecCapableSetMeshType(string type) =>
        type.Equals("setmesh", StringComparison.OrdinalIgnoreCase);

    /// SetMaterial's own version -- see IsExecCapableSetMeshType's comment, which applies unchanged.
    private static bool IsExecCapableSetMaterialType(string type) =>
        type.Equals("setmaterial", StringComparison.OrdinalIgnoreCase);

    /// SetSkeleton's own version -- see IsExecCapableSetMeshType, same GraphInterop shape.
    private static bool IsExecCapableSetSkeletonType(string type) =>
        type.Equals("setskeleton", StringComparison.OrdinalIgnoreCase);

    /// PlayAnimation's own version (see IsExecCapableSetMeshType). Grouped with SetMesh/SetMaterial/
    /// SetSkeleton, not Spawn/PlaySound: only one CAnimator per entity, so replaying a clip twice
    /// overwrites the same fields, unlike PlaySound's genuinely NEW voice each call.
    private static bool IsExecCapablePlayAnimationType(string type) =>
        type.Equals("playanimation", StringComparison.OrdinalIgnoreCase);

    /// SetControlRig's own version. Grouped with SetSkeleton/PlayAnimation, not Spawn: one CControlRig
    /// per entity, so binding the same rig twice leaves the same state rather than stacking two rigs.
    private static bool IsExecCapableSetControlRigType(string type) =>
        type.Equals("setcontrolrig", StringComparison.OrdinalIgnoreCase);

    /// AttachToSocket's own version. Grouped with SetMesh/SetMaterial, not Spawn: IDEMPOTENT --
    /// attaching twice is the same state, not two attachments -- so allowed in the PULL compiler too
    /// (NOT in IsPushOnlySideEffectType).
    private static bool IsExecCapableAttachToSocketType(string type) =>
        type.Equals("attachtosocket", StringComparison.OrdinalIgnoreCase);

    /// Runs a SetMesh write once, when reached -- mirrors EmitSetMesh's mesh=/native-call shape but
    /// pulls "entity" via EmitPullInput and captures "success" into an exec-local.
    private void EmitExecSetMesh(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.MeshPath))
            throw new InvalidOperationException($"SetMesh node '{node.Id}' has no mesh= attribute naming which asset to set");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldstr, node.MeshPath);
        _il.Emit(OpCodes.Call, SetMeshMethod);

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

    /// Runs an AttachToSocket node once, when reached. Must use EmitPullInput, not LoadPin -- the
    /// rule that made Jump dead from birth: on the exec path a pin has no _pinLocals entry, so
    /// LoadPin reads an unset local and silently attaches entity 0 to entity 0.
    private void EmitExecAttachToSocket(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.SocketName))
            throw new InvalidOperationException($"AttachToSocket node '{node.Id}' has no socket= attribute naming which socket to hang on");

        EmitPullInput(node, "entity");
        EmitPullInput(node, "parent");
        _il.Emit(OpCodes.Ldstr, node.SocketName);
        _il.Emit(OpCodes.Call, AttachToSocketMethod);

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

    /// Runs a SetMaterial node's write exactly once, at the point the exec walk reaches it -- mirrors
    /// EmitExecSetMesh exactly, see that method's comment.
    private void EmitExecSetMaterial(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.MaterialName))
            throw new InvalidOperationException($"SetMaterial node '{node.Id}' has no material= attribute naming which material to set");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldstr, node.MaterialName);
        _il.Emit(OpCodes.Call, SetMaterialMethod);

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

    /// Runs a SetSkeleton node's write exactly once, at the point the exec walk reaches it -- mirrors
    /// EmitExecSetMesh exactly, see that method's comment, wrapping SetSkeletonMethod instead.
    private void EmitExecSetSkeleton(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.SkeletonPath))
            throw new InvalidOperationException($"SetSkeleton node '{node.Id}' has no skeleton= attribute naming which asset to bind");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldstr, node.SkeletonPath);
        _il.Emit(OpCodes.Call, SetSkeletonMethod);

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

    /// Runs a PlayAnimation write once, when reached -- mirrors EmitExecSetMesh plus a second
    /// EmitPullInput (loop) before the call (see EmitPlayAnimation for the PULL-compiler twin).
    /// EmitPullInput, not LoadPin, on both data pins -- see EmitExecAttachToSocket for why.
    private void EmitExecPlayAnimation(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.ClipPath))
            throw new InvalidOperationException($"PlayAnimation node '{node.Id}' has no clip= attribute naming which clip to play");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldstr, node.ClipPath);
        EmitPullInput(node, "loop");
        _il.Emit(OpCodes.Call, PlayAnimationMethod);

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

    /// Runs a SetControlRig write once, when reached -- mirrors EmitExecPlayAnimation exactly, with
    /// a float weight where that has a bool loop (see EmitSetControlRig for the PULL-compiler twin).
    /// EmitPullInput, not LoadPin, on both data pins -- see EmitExecAttachToSocket for why.
    private void EmitExecSetControlRig(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.RigPath))
            throw new InvalidOperationException($"SetControlRig node '{node.Id}' has no rig= attribute naming which rig to bind");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldstr, node.RigPath);
        // Same unwired-weight-is-1 rule as the PULL twin; see IsPinUnwired.
        if (IsPinUnwired(node.Id, "weight")) _il.Emit(OpCodes.Ldc_R4, 1.0f);
        else EmitPullInput(node, "weight");
        _il.Emit(OpCodes.Call, SetControlRigMethod);

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
    // SetMesh/SetMaterial DO go through a GraphInterop wrapper -- see SetMeshForGraph's own comment for
    // why (Entity's internal constructor is only callable from Aver.Framework code).
    private static readonly MethodInfo SetMeshMethod =
        typeof(GraphInterop).GetMethod("SetMeshForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetMeshForGraph was not found by reflection");
    private static readonly MethodInfo SetMaterialMethod =
        typeof(GraphInterop).GetMethod("SetMaterialForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetMaterialForGraph was not found by reflection");
    // SetSkeleton/PlayAnimation, reflected the same way as SetMesh/SetMaterial immediately above --
    // same "needs Entity's internal constructor" reason.
    private static readonly MethodInfo SetSkeletonMethod =
        typeof(GraphInterop).GetMethod("SetSkeletonForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetSkeletonForGraph was not found by reflection");
    private static readonly MethodInfo PlayAnimationMethod =
        typeof(GraphInterop).GetMethod("PlayAnimationForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlayAnimationForGraph was not found by reflection");
    // SetControlRig, reflected the same way -- same "needs Entity's internal constructor" reason.
    private static readonly MethodInfo SetControlRigMethod =
        typeof(GraphInterop).GetMethod("SetControlRigForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetControlRigForGraph was not found by reflection");
    // AttachToSocket, reflected the same way -- Entity.AttachToSocket needs Entity's internal
    // constructor, so the call has to enter through Aver.Framework rather than from here.
    private static readonly MethodInfo AttachToSocketMethod =
        typeof(GraphInterop).GetMethod("AttachToSocketForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AttachToSocketForGraph was not found by reflection");
    private static readonly MethodInfo GetAnimCurveMethod =
        typeof(GraphInterop).GetMethod("GetAnimCurveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GetAnimCurveForGraph was not found by reflection");
}
