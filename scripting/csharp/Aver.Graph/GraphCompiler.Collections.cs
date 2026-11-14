// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Emission for the array and struct nodes (GraphCollectionNodes.cs lists them). The pure ones are reached from
// EmitPureDataPull, the exec ones from EmitExecDataNode, ForEachInArray from EmitExecNode.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;

namespace Aver.Graph;

public partial class GraphCompiler
{
    private static MethodInfo ArrayOp(string name, Type elem) =>
        (typeof(GraphArrayOps).GetMethod(name) ?? throw new InvalidOperationException($"GraphArrayOps.{name} was not found by reflection"))
            .MakeGenericMethod(elem);

    private static MethodInfo StructOp(string name) =>
        typeof(GraphStructOps).GetMethod(name) ?? throw new InvalidOperationException($"GraphStructOps.{name} was not found by reflection");

    private PinType ElementOf(Node node)
    {
        if (!CollectionNodes.TryElement(node, out var elem))
            throw new InvalidOperationException($"node '{node.Id}' ({node.Type}) has of={node.Attrs["of"]}, which is not an array element type");
        return elem;
    }

    private GraphStructDef StructDefOf(Node node) =>
        CollectionNodes.StructOf(node, _graph)
        ?? throw new InvalidOperationException($"node '{node.Id}' ({node.Type}) names struct '{(node.Attrs.TryGetValue("struct", out var n) ? n : "")}', which no STRUCT record declares");

    private bool HasInputValue(Node node, string pinName) =>
        _graph.Links.Any(l => l.TargetNodeId == node.Id && l.TargetPinName == pinName) ||
        _graph.PinnedValues.Any(p => p.NodeId == node.Id && p.PinName == pinName);

    /// A value on the stack boxed for a struct field slot (reference types stay as they are).
    private void EmitBoxField(PinType type)
    {
        if (_il == null) return;
        Type clr = PinTypeRules.ClrType(type);
        if (clr.IsValueType) _il.Emit(OpCodes.Box, clr);
    }

    /// The zero struct of `def` on the stack.
    private void EmitZeroStruct(string name)
    {
        if (_il == null) return;
        var def = _graph.Structs.FirstOrDefault(d => d.Name == name)
            ?? throw new InvalidOperationException($"struct '{name}' is not declared by any STRUCT record");
        _il.Emit(OpCodes.Ldstr, def.Name);
        _il.Emit(OpCodes.Ldstr, def.Signature);
        _il.Emit(OpCodes.Call, StructOp(nameof(GraphStructOps.Default)));
    }

    /// One output of a pure array or struct node onto the stack.
    private void EmitCollectionPull(Node node, string pinName)
    {
        if (_il == null) return;
        void In(string p) => EmitPullInput(node, p);
        string type = node.Type.ToLowerInvariant();

        if (CollectionNodes.IsStructNode(type))
        {
            var def = StructDefOf(node);
            switch (type)
            {
                case "makestruct":
                    _il.Emit(OpCodes.Ldstr, def.Name);
                    _il.Emit(OpCodes.Ldc_I4, def.Fields.Count);
                    _il.Emit(OpCodes.Newarr, typeof(object));
                    for (int i = 0; i < def.Fields.Count; i++)
                    {
                        _il.Emit(OpCodes.Dup);
                        _il.Emit(OpCodes.Ldc_I4, i);
                        In(def.Fields[i].Name);
                        EmitBoxField(def.Fields[i].Type);
                        _il.Emit(OpCodes.Stelem_Ref);
                    }
                    _il.Emit(OpCodes.Call, StructOp(nameof(GraphStructOps.Make)));
                    return;
                case "breakstruct":
                {
                    int idx = def.Fields.FindIndex(f => f.Name == pinName);
                    if (idx < 0) throw new InvalidOperationException($"BreakStruct node '{node.Id}' has no output pin '{pinName}' (a field of '{def.Name}')");
                    In("value");
                    _il.Emit(OpCodes.Ldc_I4, idx);
                    _il.Emit(OpCodes.Call, StructOp(def.Fields[idx].Type switch
                    {
                        PinType.Float => nameof(GraphStructOps.GetFloat),
                        PinType.Bool => nameof(GraphStructOps.GetBool),
                        PinType.Float3 => nameof(GraphStructOps.GetVector),
                        PinType.String => nameof(GraphStructOps.GetString),
                        _ => nameof(GraphStructOps.GetInt),
                    }));
                    return;
                }
                default: // setstructfield
                {
                    node.Attrs.TryGetValue("field", out var field);
                    int idx = def.Fields.FindIndex(f => f.Name == field);
                    if (idx < 0) throw new InvalidOperationException($"SetStructField node '{node.Id}' has field={field}, which is not a field of struct '{def.Name}'");
                    In("value");
                    _il.Emit(OpCodes.Ldstr, def.Name);
                    _il.Emit(OpCodes.Ldstr, def.Signature);
                    _il.Emit(OpCodes.Ldc_I4, idx);
                    In(def.Fields[idx].Name);
                    EmitBoxField(def.Fields[idx].Type);
                    _il.Emit(OpCodes.Call, StructOp(nameof(GraphStructOps.With)));
                    return;
                }
            }
        }

        PinType elem = ElementOf(node);
        Type clr = PinTypeRules.ClrType(elem);
        switch (type)
        {
            case "makearray":
            {
                var list = typeof(List<>).MakeGenericType(clr);
                _il.Emit(OpCodes.Newobj, list.GetConstructor(Type.EmptyTypes)!);
                for (int i = 0; i < 4; i++)
                {
                    if (!HasInputValue(node, "item" + i)) continue;
                    _il.Emit(OpCodes.Dup);
                    In("item" + i);
                    _il.Emit(OpCodes.Callvirt, list.GetMethod("Add")!);
                }
                return;
            }
            case "arraylength": In("array"); _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Length), clr)); return;
            case "arrayget":
                In("array"); In("index");
                _il.Emit(OpCodes.Call, elem == PinType.String
                    ? typeof(GraphArrayOps).GetMethod(nameof(GraphArrayOps.GetString))!
                    : ArrayOp(nameof(GraphArrayOps.Get), clr));
                return;
            case "arraycontains": In("array"); In("item"); _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Contains), clr)); return;
            case "arrayfind": In("array"); In("item"); _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Find), clr)); return;
            default:
                throw new InvalidOperationException($"collection node '{node.Id}' has type {node.Type}, which EmitCollectionPull does not know");
        }
    }

    /// The array variable a mutating array node addresses, onto the stack as the live list.
    private void EmitLoadVarList(Node node)
    {
        if (_il == null) return;
        var declared = _graph.Variables.FirstOrDefault(v => v.Name == node.VarName)
            ?? throw new InvalidOperationException($"{node.Type} node '{node.Id}' references undeclared variable '{node.VarName}'");
        if (_varStoreArgIndex < 0)
            throw new InvalidOperationException(
                $"{node.Type} node '{node.Id}' needs a variable store argument, but this graph declares no VAR records " +
                "(internal error -- Graph.Validate() should already have refused this graph)");
        _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, declared.Name);
        _il.Emit(OpCodes.Call, VarGetMethodFor(declared.Type));
    }

    /// Runs one exec array node (Add, Set, RemoveAt, Clear on a variable) when the exec walk reaches it.
    private void EmitExecCollectionNode(Node node)
    {
        if (_il == null) return;
        Type clr = PinTypeRules.ClrType(ElementOf(node));
        EmitLoadVarList(node);
        switch (node.Type.ToLowerInvariant())
        {
            case "arrayadd": EmitPullInput(node, "item"); _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Add), clr)); return;
            case "arrayset": EmitPullInput(node, "index"); EmitPullInput(node, "item"); _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Set), clr)); return;
            case "arrayremoveat": EmitPullInput(node, "index"); _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.RemoveAt), clr)); return;
            case "arrayclear": _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Clear), clr)); return;
            default:
                throw new InvalidOperationException($"collection node '{node.Id}' has type {node.Type}, which EmitExecCollectionNode does not know");
        }
    }

    /// ForEachInArray: `loop` runs once per element with `element` and `index` set, then `completed`. The array is read
    /// once; it is walked live, so an Add in the body is seen (the iteration guard bounds a loop that never stops growing it).
    private void EmitForEachInArray(Node node)
    {
        if (_il == null) return;
        PinType elem = ElementOf(node);
        Type clr = PinTypeRules.ClrType(elem);
        var list = _il.DeclareLocal(PinTypeRules.ClrType(PinTypeRules.TryArrayOf(elem, out var arr) ? arr : PinType.FloatArray));
        EmitPullInput(node, "array");
        _il.Emit(OpCodes.Stloc, list);

        var index = GetOrCreateExecLocal(node.Id, "index", typeof(int));
        var element = GetOrCreateExecLocal(node.Id, "element", clr);
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Stloc, index);

        var loopCheck = _il.DefineLabel();
        var guardTripped = _il.DefineLabel();
        var loopDone = _il.DefineLabel();

        _il.MarkLabel(loopCheck);
        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldloc, list);
        _il.Emit(OpCodes.Call, ArrayOp(nameof(GraphArrayOps.Length), clr));
        _il.Emit(OpCodes.Bge, loopDone);

        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldc_I4, MaxLoopIterations);
        _il.Emit(OpCodes.Bge, guardTripped);

        _il.Emit(OpCodes.Ldloc, list);
        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Call, elem == PinType.String
            ? typeof(GraphArrayOps).GetMethod(nameof(GraphArrayOps.GetString))!
            : ArrayOp(nameof(GraphArrayOps.Get), clr));
        _il.Emit(OpCodes.Stloc, element);

        Node? body = FindExecTarget(node.Id, "loop");
        if (body != null) EmitExecNode(body);

        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldc_I4_1);
        _il.Emit(OpCodes.Add);
        _il.Emit(OpCodes.Stloc, index);
        _il.Emit(OpCodes.Br, loopCheck);

        _il.MarkLabel(guardTripped);
        _il.Emit(OpCodes.Ldstr, node.Id);
        _il.Emit(OpCodes.Ldstr, _currentEventName);
        _il.Emit(OpCodes.Call, WarnLoopGuardMethod);

        _il.MarkLabel(loopDone);
        Node? done = FindExecTarget(node.Id, "completed");
        if (done != null) EmitExecNode(done);
    }
}
