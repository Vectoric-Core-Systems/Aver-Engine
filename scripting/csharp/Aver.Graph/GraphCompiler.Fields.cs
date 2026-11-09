// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Scene field reads and writes (getfield, setfield and their Vec3 forms).

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
    private void EmitGetField(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.FieldName))
            throw new InvalidOperationException($"GetField node '{node.Id}' has no field= attribute naming which scene field to read");

        if (!_fieldResolver(node.FieldName, out int fieldId, out int kind))
            throw new InvalidOperationException($"GetField node '{node.Id}' references unknown scene field '{node.FieldName}'");

        if (kind != FieldKindF32)
            throw new InvalidOperationException(
                $"GetField node '{node.Id}' field '{node.FieldName}' is not an F32 field (kind={kind}); " +
                "getfield only supports F32 fields today, not Vec3/Quat/Bool/I32/etc");

        // Calls the same P/Invoke extern Aver.Scene's C# consumers use (Native.cs), not a second
        // DllImport surface. Needs InternalsVisibleTo("Aver.Graph") from Aver.Scene.csproj to name
        // the internal Native type at compile time; DynamicMethod's restrictedSkipVisibility:true
        // (Compile()) lets the emitted IL call it.
        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        _il.Emit(OpCodes.Call, GetFieldMethod);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitSetField(Node node)
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

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        LoadPin(node.Id, "value");
        _il.Emit(OpCodes.Call, SetFieldMethod);
        // aver_scene_set_f32 returns 1 on success, 0 on rejection (unknown entity, read-only field
        // e.g. CWorld.matrix, missing component) -- now reaches "success"; the old stub hardcoded 1
        // regardless of outcome.

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Shared by all 4 getfieldvec3/setfieldvec3 call sites. Requires FieldKindVec3 specifically,
    /// not just "not F32": GetFieldVecForGraph/SetFieldVecForGraph (Aver.Framework) copy exactly 3
    /// floats through a fixed scratch buffer, so a Quat (arity 4) or Mat4 (arity 16) field would be
    /// a buffer overrun, not a caught type error. Centralised rather than duplicated 4x.
    private int RequireVec3Field(Node node, string nodeTypeLabel)
    {
        if (string.IsNullOrEmpty(node.FieldName))
            throw new InvalidOperationException(
                $"{nodeTypeLabel} node '{node.Id}' has no field= attribute naming which scene field to address");

        if (!_fieldResolver(node.FieldName, out int fieldId, out int kind))
            throw new InvalidOperationException(
                $"{nodeTypeLabel} node '{node.Id}' references unknown scene field '{node.FieldName}'");

        if (kind != FieldKindVec3)
            throw new InvalidOperationException(
                $"{nodeTypeLabel} node '{node.Id}' field '{node.FieldName}' is not a Vec3 field (kind={kind}); " +
                $"{nodeTypeLabel.ToLowerInvariant()} only supports Vec3 fields today (arity 3) -- not F32/Quat/Mat4/etc");

        return fieldId;
    }

    /// GetFieldVec3(entity) -> x,y,z: reads a Vec3-kind field (CLocal.position, CLight.colour, ...)
    /// as three scalar pins instead of GetField's single F32. Pure/idempotent, so safe to pull as
    /// often as wanted through either compiler -- but three separate pulls cost three native calls,
    /// not one (see EmitPullGetFieldVec3). Reads back 0,0,0 on runtime rejection (unknown
    /// entity/component), mirroring GetField rather than adding a "found" pin; wrong field/kind is a
    /// compile error via RequireVec3Field.
    private void EmitGetFieldVec3(Node node)
    {
        if (_il == null) return;

        int fieldId = RequireVec3Field(node, "GetFieldVec3");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "x"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "y"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "z"));
        _il.Emit(OpCodes.Call, GetFieldVecMethod);
    }

    /// SetFieldVec3(entity, x, y, z) -> success: mirrors EmitSetField, including getting no exec pins
    /// by default (AddDefaultPins's "setfieldvec3" case; add explicit PIN records for the exec chain).
    private void EmitSetFieldVec3(Node node)
    {
        if (_il == null) return;

        int fieldId = RequireVec3Field(node, "SetFieldVec3");

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        LoadPin(node.Id, "x");
        LoadPin(node.Id, "y");
        LoadPin(node.Id, "z");
        _il.Emit(OpCodes.Call, SetFieldVecMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// SetFieldVec3's own version of EmitExecSideEffect -- mirrors EmitSetFieldVec3's field=/resolver
    /// logic (RequireVec3Field) but pulls inputs via EmitPullInput and captures "success" into an
    /// exec-local.
    private void EmitExecSetFieldVec3(Node node)
    {
        if (_il == null) return;

        int fieldId = RequireVec3Field(node, "SetFieldVec3");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        EmitPullInput(node, "x");
        EmitPullInput(node, "y");
        EmitPullInput(node, "z");
        _il.Emit(OpCodes.Call, SetFieldVecMethod);

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

    /// Mirrors EmitGetField's field=/resolver checks but pushes the read value rather than storing to
    /// `_pinLocals`. Reading is idempotent, so unlike SetField this is safe to pull more than once
    /// (see IsExecCapableSideEffectType).
    private void EmitPullGetField(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.FieldName))
            throw new InvalidOperationException($"GetField node '{node.Id}' has no field= attribute naming which scene field to read");
        if (!_fieldResolver(node.FieldName, out int fieldId, out int kind))
            throw new InvalidOperationException($"GetField node '{node.Id}' references unknown scene field '{node.FieldName}'");
        if (kind != FieldKindF32)
            throw new InvalidOperationException(
                $"GetField node '{node.Id}' field '{node.FieldName}' is not an F32 field (kind={kind}); " +
                "getfield only supports F32 fields today, not Vec3/Quat/Bool/I32/etc");
        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);
        _il.Emit(OpCodes.Call, GetFieldMethod);
    }

    /// Mirrors EmitGetFieldVec3's field=/RequireVec3Field checks but pushes one requested component
    /// rather than storing all three to `_pinLocals`, since EmitPullOutput's contract is "push the
    /// one pin asked for" and GetFieldVec3 has three (x/y/z) unlike GetField's one. Not free: makes
    /// one full native call and discards the two unasked components every time it runs, so x/y/z via
    /// three separate readers costs three calls -- the PUSH VS PULL point 1 recompute tradeoff, not a
    /// new cost. Not given Raycast's _execLocals caching (that exists because a physics query is
    /// expensive, IsExecCapableQueryType) since a Vec3 read is cost-equal to GetField's single-float
    /// read.
    private void EmitPullGetFieldVec3(Node node, string pinName)
    {
        if (_il == null) return;

        int fieldId = RequireVec3Field(node, "GetFieldVec3");

        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Ldc_I4, fieldId);

        var xLocal = _il.DeclareLocal(typeof(float));
        var yLocal = _il.DeclareLocal(typeof(float));
        var zLocal = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, xLocal);
        _il.Emit(OpCodes.Ldloca, yLocal);
        _il.Emit(OpCodes.Ldloca, zLocal);
        _il.Emit(OpCodes.Call, GetFieldVecMethod);

        var wanted = pinName switch
        {
            "x" => xLocal,
            "y" => yLocal,
            "z" => zLocal,
            _ => throw new InvalidOperationException(
                $"GetFieldVec3 node '{node.Id}' has no output pin '{pinName}' (only x/y/z)"),
        };
        _il.Emit(OpCodes.Ldloc, wanted);
    }

    // Resolved once by reflection: Native is internal to Aver.Scene, so these use BindingFlags.NonPublic
    // rather than a method-group reference. Independent of the InternalsVisibleTo grant on
    // Aver.Scene.csproj (which only lets GraphCompiler.cs name the `Native` type at compile time --
    // GetMethod finds an internal method either way).
    private static readonly MethodInfo GetFieldMethod =
        typeof(Native).GetMethod("aver_scene_get_f32", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_get_f32 was not found by reflection");
    private static readonly MethodInfo SetFieldMethod =
        typeof(Native).GetMethod("aver_scene_set_f32", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_set_f32 was not found by reflection");
    private static readonly MethodInfo GetFieldVecMethod =
        typeof(GraphInterop).GetMethod("GetFieldVecForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GetFieldVecForGraph was not found by reflection");
    private static readonly MethodInfo SetFieldVecMethod =
        typeof(GraphInterop).GetMethod("SetFieldVecForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetFieldVecForGraph was not found by reflection");
}
