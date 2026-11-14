// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Graph parameters and graph-local variables (VAR / GraphVarStore).

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
    private void EmitParam(Node node)
    {
        if (_il == null) return;

        // Validate() already checked param= names a declared PARAM with a matching type; this
        // repeats the lookup defensively, same pattern LoadPin follows below.
        int index = _graph.Parameters.FindIndex(p => p.Name == node.ParamName);
        if (index < 0)
            throw new InvalidOperationException($"Param node '{node.Id}' references undeclared parameter '{node.ParamName}'");

        _il.Emit(OpCodes.Ldarg, (short)index);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// GetVar(var=name) -> value: reads a persistent VAR slot (GraphVariable/GraphVarStore). A pure
    /// read -- always safe to read twice, unlike SetVar's write -- so welcome in either compiler
    /// like GetField/EmitParam; EmitPullGetVar is the PULL-recursive twin for EmitPullOutput.
    /// Validate() already checked var= names a declared VAR with an agreeing type; repeated here
    /// defensively, same pattern as EmitParam.
    private void EmitGetVar(Node node)
    {
        if (_il == null) return;

        var declared = _graph.Variables.FirstOrDefault(v => v.Name == node.VarName);
        if (declared == null)
            throw new InvalidOperationException($"GetVar node '{node.Id}' references undeclared variable '{node.VarName}'");
        if (_varStoreArgIndex < 0)
            throw new InvalidOperationException(
                $"GetVar node '{node.Id}' needs a variable store argument, but this graph declares no " +
                "VAR records (internal error -- Graph.Validate() should already have refused this graph)");

        _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, node.VarName!);
        _il.Emit(OpCodes.Call, VarGetMethodFor(declared.Type));

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// SetVar's own version -- writes through GraphVarStore (in-process, type dispatch), not a native
    /// P/Invoke, so it has none of a native write's failure modes (no "success" pin either). Still
    /// shares the family's PULL refusal.
    private static bool IsExecCapableVarSideEffectType(string type) =>
        type.Equals("setvar", StringComparison.OrdinalIgnoreCase);

    /// Runs a SetVar node's write once, when reached -- mirrors EmitExecSideEffect/EmitExecSetFieldVec3/
    /// EmitExecSpawn's shape, but var= is checked directly against _graph.Variables, not a native
    /// lookup (a VAR is declared in this graph file, not a scene/class registry like field=/class=).
    /// No "success" pin: a real difference, not a missing feature -- a write into GraphVarStore's
    /// Dictionary, keyed by a name Graph.Validate() already confirmed declared and typed, has
    /// nothing left to fail at runtime.
    private void EmitExecSetVar(Node node)
    {
        if (_il == null) return;

        if (string.IsNullOrEmpty(node.VarName))
            throw new InvalidOperationException($"SetVar node '{node.Id}' has no var= attribute naming which variable to write");
        var declared = _graph.Variables.FirstOrDefault(v => v.Name == node.VarName);
        if (declared == null)
            throw new InvalidOperationException($"SetVar node '{node.Id}' references undeclared variable '{node.VarName}'");
        if (_varStoreArgIndex < 0)
            throw new InvalidOperationException(
                $"SetVar node '{node.Id}' needs a variable store argument, but this graph declares no " +
                "VAR records (internal error -- Graph.Validate() should already have refused this graph)");

        _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, node.VarName);
        EmitPullInput(node, "value");
        _il.Emit(OpCodes.Call, VarSetMethodFor(declared.Type));
    }

    /// Mirrors EmitGetVar's var=/declared-type lookup but pushes the read value rather than storing to
    /// `_pinLocals` (same pattern as EmitPullGetField for GetField). Reading a variable is idempotent
    /// (GraphVarStore.GetFloat/Int/Bool have no side effect), so safe to pull as often as wanted.
    private void EmitPullGetVar(Node node)
    {
        if (_il == null) return;

        var declared = _graph.Variables.FirstOrDefault(v => v.Name == node.VarName);
        if (declared == null)
            throw new InvalidOperationException($"GetVar node '{node.Id}' references undeclared variable '{node.VarName}'");
        if (_varStoreArgIndex < 0)
            throw new InvalidOperationException(
                $"GetVar node '{node.Id}' needs a variable store argument, but this graph declares no " +
                "VAR records (internal error -- Graph.Validate() should already have refused this graph)");

        _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, node.VarName!);
        _il.Emit(OpCodes.Call, VarGetMethodFor(declared.Type));
    }

    // GraphVarStore's own typed accessors -- public instance methods on a plain class in this
    // assembly, so an ordinary public GetMethod lookup suffices.
    private static readonly MethodInfo VarGetFloatMethod =
        typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetFloat))
        ?? throw new InvalidOperationException("Aver.Graph.GraphVarStore.GetFloat was not found by reflection");
    private static readonly MethodInfo VarSetFloatMethod =
        typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetFloat))
        ?? throw new InvalidOperationException("Aver.Graph.GraphVarStore.SetFloat was not found by reflection");
    private static readonly MethodInfo VarGetIntMethod =
        typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetInt))
        ?? throw new InvalidOperationException("Aver.Graph.GraphVarStore.GetInt was not found by reflection");
    private static readonly MethodInfo VarSetIntMethod =
        typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetInt))
        ?? throw new InvalidOperationException("Aver.Graph.GraphVarStore.SetInt was not found by reflection");
    private static readonly MethodInfo VarGetVector3Method = typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetVector3))!;
    private static readonly MethodInfo VarSetVector3Method = typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetVector3))!;
    private static readonly MethodInfo VarGetStringMethod = typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetString))!;
    private static readonly MethodInfo VarSetStringMethod = typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetString))!;
    private static readonly MethodInfo VarGetBoolMethod =
        typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetBool))
        ?? throw new InvalidOperationException("Aver.Graph.GraphVarStore.GetBool was not found by reflection");
    private static readonly MethodInfo VarSetBoolMethod =
        typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetBool))
        ?? throw new InvalidOperationException("Aver.Graph.GraphVarStore.SetBool was not found by reflection");

    /// Picks GraphVarStore's read accessor for a declared VAR's type -- EmitGetVar/EmitPullGetVar's
    /// shared dispatch, the same "one switch, both callers" shape VarSetMethodFor below has for writes.
    private static MethodInfo VarGetMethodFor(PinType t) => t switch
    {
        PinType.Float => VarGetFloatMethod,
        PinType.Int or PinType.Entity => VarGetIntMethod,
        PinType.Float3 => VarGetVector3Method,
        PinType.String => VarGetStringMethod,
        PinType.Bool => VarGetBoolMethod,
        PinType.Struct => typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetStruct))!,
        _ when PinTypeRules.IsArray(t) => typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.GetArray))!
            .MakeGenericMethod(PinTypeRules.ClrType(PinTypeRules.ElementOf(t))),
        _ => throw new InvalidOperationException(
            $"VAR type {t} has no GraphVarStore read accessor"),
    };

    /// Picks GraphVarStore's write accessor for a declared VAR's type -- EmitExecSetVar's own dispatch.
    private static MethodInfo VarSetMethodFor(PinType t) => t switch
    {
        PinType.Float => VarSetFloatMethod,
        PinType.Int or PinType.Entity => VarSetIntMethod,
        PinType.Float3 => VarSetVector3Method,
        PinType.String => VarSetStringMethod,
        PinType.Bool => VarSetBoolMethod,
        PinType.Struct => typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetStruct))!,
        _ when PinTypeRules.IsArray(t) => typeof(GraphVarStore).GetMethod(nameof(GraphVarStore.SetArray))!
            .MakeGenericMethod(PinTypeRules.ClrType(PinTypeRules.ElementOf(t))),
        _ => throw new InvalidOperationException(
            $"VAR type {t} has no GraphVarStore write accessor"),
    };
}
