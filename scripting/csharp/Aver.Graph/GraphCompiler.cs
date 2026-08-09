// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// IL compiler for graphs.
// Comment explains WHY: System.Reflection.Emit lets us turn a graph into native .NET IL at runtime
// without needing a compiler (Roslyn). This removes the constraint that games must ship a compiler.
// The compiled method is cached in the AssemblyLoadContext so it can be invoked repeatedly.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Aver.Scene;

namespace Aver.Graph;

/// <summary>Resolves a scene field's dense id and kind by qualified name, so getfield/setfield
/// nodes can be checked at COMPILE time rather than silently doing nothing at runtime. fieldId
/// is 0 (and the return is false) when the name is unknown; kind mirrors aver::scene::FieldKind
/// (Fields.hpp) when it is.
///
/// GraphCompiler's default implementation calls straight through to the live scene via
/// Native.aver_scene_field/aver_scene_field_kind -- correct for a graph compiled inside a running
/// engine, where the scene's field table is exactly the authority a graph should be checked
/// against. That default requires the native Aver.Scene library to be loadable; a process with no
/// scene running (a bare unit test, notably) cannot exercise it and should supply its own resolver
/// instead of standing up a native scene just to compile a graph.</summary>
public delegate bool FieldResolver(string qualifiedName, out int fieldId, out int kind);

/// Compiles a graph to a DynamicMethod and invokes it.
public class GraphCompiler
{
    // aver::scene::FieldKind::F32 (Fields.hpp) -- getfield/setfield only support this kind today.
    // Reading/writing Vec3/Quat/Mat4 fields (e.g. CLocal.position) needs aver_scene_get_vec/set_vec
    // and a vector-typed pin the graph format doesn't have yet; out of scope for this slice.
    private const int FieldKindF32 = 0;

    private Graph _graph;
    private readonly FieldResolver _fieldResolver;
    private Dictionary<string, LocalBuilder> _nodeLocals = new();
    private Dictionary<(string, string), LocalBuilder> _pinLocals = new();
    private ILGenerator? _il;

    public GraphCompiler(Graph graph, FieldResolver? fieldResolver = null)
    {
        _graph = graph;
        _fieldResolver = fieldResolver ?? DefaultFieldResolver;
    }

    private static bool DefaultFieldResolver(string qualifiedName, out int fieldId, out int kind)
    {
        fieldId = Native.aver_scene_field(qualifiedName);
        kind = fieldId != 0 ? Native.aver_scene_field_kind(fieldId) : 0;
        return fieldId != 0;
    }

    /// Compiles the graph to a DynamicMethod. The method takes one argument per PARAM the graph
    /// declares, in declaration order (no PARAM records -- the common case today -- means zero
    /// arguments, exactly as before PARAM existed), and returns the single output pin's value, or
    /// void if multiple outputs.
    /// Returns null if compilation fails; check log for errors.
    public Delegate? Compile(out string? err)
    {
        err = null;

        // Validate the graph first.
        if (!_graph.Validate(out var validateErr))
        {
            err = validateErr;
            return null;
        }

        try
        {
            // Determine the return type. Zero outputs -> void (unchanged). One output -> that
            // pin's own CLR type, exactly as before (every existing single-output graph and test
            // keeps the same Func<T> shape). Two or more outputs -> object[], one boxed entry per
            // Outputs record IN FILE ORDER -- see the OUTPUTS ARRAY comment below Compile() for why
            // this is object[] and not a second delegate convention.
            Type returnType = typeof(void);
            string singleOutputNodeId = "";
            string singleOutputPinName = "";

            if (_graph.Outputs.Count == 1)
            {
                var (nodeId, pinName) = _graph.Outputs[0];
                if (_graph.Nodes.TryGetValue(nodeId, out var node))
                {
                    var pin = node.Pins.FirstOrDefault(p => p.Name == pinName && p.IsOutput);
                    if (pin != null)
                    {
                        returnType = PinTypeToCLRType(pin.Type);
                        singleOutputNodeId = nodeId;
                        singleOutputPinName = pinName;
                    }
                }
            }
            else if (_graph.Outputs.Count >= 2)
            {
                returnType = typeof(object[]);
            }

            Type[] paramTypes = _graph.Parameters.Select(p => PinTypeToCLRType(p.Type)).ToArray();

            var method = new DynamicMethod(
                "CompiledGraph",
                returnType,
                paramTypes,
                restrictedSkipVisibility: true
            );

            _il = method.GetILGenerator();
            _nodeLocals.Clear();
            _pinLocals.Clear();

            // Sort nodes in topological order.
            var sortedNodes = TopologicalSort();
            if (sortedNodes == null)
            {
                err = "Graph has a cycle";
                return null;
            }

            // Emit code for each node in order.
            foreach (var node in sortedNodes)
            {
                EmitNode(node);
            }

            // If there's a single output, load it and return.
            if (_graph.Outputs.Count == 1 && !string.IsNullOrEmpty(singleOutputNodeId))
            {
                if (_pinLocals.TryGetValue((singleOutputNodeId, singleOutputPinName), out var outLocal))
                {
                    _il.Emit(OpCodes.Ldloc, outLocal);
                }
                else
                {
                    err = $"Output pin {singleOutputNodeId}.{singleOutputPinName} was not computed";
                    return null;
                }
            }
            else if (_graph.Outputs.Count >= 2)
            {
                // OUTPUTS ARRAY: multiple OUT records used to compile to a void-returning method
                // whose computed values were locals inside the DynamicMethod and thus unrecoverable
                // by the caller -- the graph "ran" but nothing it produced could ever be read back.
                // A drone's flight path needs x, y, and z out of ONE compile (three separate
                // single-output graphs would triple-compute the shared angle/time math and, worse,
                // could drift out of sync if edited independently), so this builds a boxed
                // object[_graph.Outputs.Count] instead, one entry per OUT record IN FILE ORDER. A
                // typed tuple would be nicer to consume but MakeGenericType over ValueTuple's arity
                // needs the same "which arity" dispatch GetDelegateType already does for Func/Action
                // by NAME -- object[] avoids inventing that a second time for a return type instead
                // of a delegate type. The caller (GraphHost) unboxes by the LOCAL'S declared CLR
                // type, which is exactly the pin's own type -- see LocalBuilder.LocalType below.
                _il.Emit(OpCodes.Ldc_I4, _graph.Outputs.Count);
                _il.Emit(OpCodes.Newarr, typeof(object));

                for (int i = 0; i < _graph.Outputs.Count; i++)
                {
                    var (nodeId, pinName) = _graph.Outputs[i];
                    if (!_pinLocals.TryGetValue((nodeId, pinName), out var outLocal))
                    {
                        err = $"Output pin {nodeId}.{pinName} was not computed";
                        return null;
                    }

                    _il.Emit(OpCodes.Dup);
                    _il.Emit(OpCodes.Ldc_I4, i);
                    _il.Emit(OpCodes.Ldloc, outLocal);
                    _il.Emit(OpCodes.Box, outLocal.LocalType);
                    _il.Emit(OpCodes.Stelem_Ref);
                }
            }

            _il.Emit(OpCodes.Ret);

            return method.CreateDelegate(GetDelegateType(paramTypes, returnType));
        }
        catch (Exception ex)
        {
            err = $"Compilation failed: {ex.Message}";
            return null;
        }
    }

    /// Topologically sorts the nodes so outputs depend on inputs.
    /// Returns null if there is a cycle.
    private List<Node>? TopologicalSort()
    {
        var visited = new HashSet<string>();
        var visiting = new HashSet<string>();
        var result = new List<Node>();

        foreach (var nodeId in _graph.Nodes.Keys)
        {
            if (!TopologicalSortDFS(nodeId, visited, visiting, result))
                return null;
        }

        return result;
    }

    private bool TopologicalSortDFS(string nodeId, HashSet<string> visited, HashSet<string> visiting, List<Node> result)
    {
        if (visited.Contains(nodeId)) return true;
        if (visiting.Contains(nodeId)) return false;  // Cycle detected.

        visiting.Add(nodeId);

        // Visit all nodes this node depends on.
        foreach (var link in _graph.Links.Where(l => l.TargetNodeId == nodeId))
        {
            if (!TopologicalSortDFS(link.SourceNodeId, visited, visiting, result))
                return false;
        }

        visiting.Remove(nodeId);
        visited.Add(nodeId);

        if (_graph.Nodes.TryGetValue(nodeId, out var node))
            result.Add(node);

        return true;
    }

    /// Emits IL code for a single node.
    private void EmitNode(Node node)
    {
        if (_il == null) return;

        // Create locals for all output pins of this node.
        foreach (var pin in node.Pins.Where(p => p.IsOutput))
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

            case "add":
                EmitAdd(node);
                break;

            case "multiply":
                EmitMultiply(node);
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

            default:
                throw new NotSupportedException($"Node type '{node.Type}' is not supported");
        }
    }

    private void EmitConstFloat(Node node)
    {
        if (_il == null) return;

        float value = 0f;
        var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
        if (co != null && co.Value is float f)
            value = f;

        _il.Emit(OpCodes.Ldc_R4, value);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitConstInt(Node node)
    {
        if (_il == null) return;

        int value = 0;
        var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
        if (co != null && co.Value is int i)
            value = i;

        _il.Emit(OpCodes.Ldc_I4, value);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitConstBool(Node node)
    {
        if (_il == null) return;

        bool value = false;
        var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
        if (co != null && co.Value is bool b)
            value = b;

        _il.Emit(OpCodes.Ldc_I4, value ? 1 : 0);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitAdd(Node node)
    {
        if (_il == null) return;

        // Load input 'a'.
        LoadPin(node.Id, "a");
        // Load input 'b'.
        LoadPin(node.Id, "b");
        // Add.
        _il.Emit(OpCodes.Add);
        // Store to output 'result'.
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitMultiply(Node node)
    {
        if (_il == null) return;

        // Load input 'a'.
        LoadPin(node.Id, "a");
        // Load input 'b'.
        LoadPin(node.Id, "b");
        // Multiply.
        _il.Emit(OpCodes.Mul);
        // Store to output 'result'.
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitCompare(Node node)
    {
        if (_il == null) return;

        // Load input 'a'.
        LoadPin(node.Id, "a");
        // Load input 'b'.
        LoadPin(node.Id, "b");
        // Compare (greater than).
        _il.Emit(OpCodes.Cgt);
        // Store to output 'result' as bool (0 or 1).
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

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

        // Load the entity id (input pin), then the resolved field id, then call straight through to
        // the same P/Invoke extern Aver.Scene's own C# consumers use (Native.cs) -- not a second,
        // differently-configured DllImport surface. GraphCompiler needs InternalsVisibleTo("Aver.Graph")
        // from Aver.Scene.csproj to name the internal Native type at compile time; DynamicMethod's
        // restrictedSkipVisibility:true (set in Compile()) is what lets the EMITTED IL actually call it.
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
        // aver_scene_set_f32 returns 1 on success, 0 on any rejection (unknown entity, read-only
        // field -- e.g. CWorld.matrix -- or missing component). That real return code is now what
        // reaches the "success" pin; the old stub hardcoded 1 regardless of whether anything happened.

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitSin(Node node)
    {
        if (_il == null) return;

        // System.Math.Sin takes and returns double; the graph is float end to end, so the value
        // needs an explicit widen going in and an explicit narrow coming out. Skipping either
        // conversion still compiles (the IL verifier accepts a bare double left where a float local
        // was declared in some cases) but silently reinterprets bits rather than converting the
        // value -- wrong numbers with no error, exactly what this comment exists to not repeat.
        LoadPin(node.Id, "a");
        _il.Emit(OpCodes.Conv_R8);
        _il.Emit(OpCodes.Call, MathSinMethod);
        _il.Emit(OpCodes.Conv_R4);

        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitCos(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        _il.Emit(OpCodes.Conv_R8);
        _il.Emit(OpCodes.Call, MathCosMethod);
        _il.Emit(OpCodes.Conv_R4);

        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitSubtract(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Sub);

        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitDivide(Node node)
    {
        if (_il == null) return;

        // DIVIDE-BY-ZERO CONVENTION: b == 0.0 exactly yields 0.0, not IEEE754's NaN/Infinity.
        //
        // A bare `div` on floats never throws -- 5/0 is +Infinity, -5/0 is -Infinity, 0/0 is NaN --
        // and any of those reaching a transform (position, rotation, scale) is very hard to trace
        // back to the divide node that produced it: it propagates silently through every downstream
        // add/multiply and shows up frames later as an object that vanished or exploded, nowhere near
        // this node. 0.0 is a defined, inert value a drone flight path can just continue through;
        // NaN is not. This is a deliberate choice, not the IEEE default left alone -- flag if a
        // consumer ever needs the propagating-NaN behavior instead (e.g. to detect the condition
        // downstream via a compare-vs-self NaN check).
        var aLocal = _il.DeclareLocal(typeof(float));
        var bLocal = _il.DeclareLocal(typeof(float));
        LoadPin(node.Id, "a");
        _il.Emit(OpCodes.Stloc, aLocal);
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Stloc, bLocal);

        var zeroLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();

        _il.Emit(OpCodes.Ldloc, bLocal);
        _il.Emit(OpCodes.Ldc_R4, 0f);
        _il.Emit(OpCodes.Ceq);
        _il.Emit(OpCodes.Brtrue, zeroLabel);

        _il.Emit(OpCodes.Ldloc, aLocal);
        _il.Emit(OpCodes.Ldloc, bLocal);
        _il.Emit(OpCodes.Div);
        _il.Emit(OpCodes.Br, endLabel);

        _il.MarkLabel(zeroLabel);
        _il.Emit(OpCodes.Ldc_R4, 0f);

        _il.MarkLabel(endLabel);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitParam(Node node)
    {
        if (_il == null) return;

        // Graph.Validate() (run at the top of Compile(), and again by the parser) already checked
        // param= is present and names a declared PARAM with a matching pin type; this repeats the
        // lookup defensively rather than trusting a check made in a different method, the same
        // pattern LoadPin already follows for its own node/pin lookups below.
        int index = _graph.Parameters.FindIndex(p => p.Name == node.ParamName);
        if (index < 0)
            throw new InvalidOperationException($"Param node '{node.Id}' references undeclared parameter '{node.ParamName}'");

        _il.Emit(OpCodes.Ldarg, (short)index);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Loads a pin value onto the stack. If the pin is an output of another node, load it
    /// from that node's local. If it's a constant, load the constant. Otherwise, load a default.
    /// nodeId is a string to support both integer and arbitrary string node IDs.
    private void LoadPin(string nodeId, string pinName)
    {
        if (_il == null) return;

        // First, check if this pin is linked to an output pin of another node.
        var link = _graph.Links.FirstOrDefault(l =>
            l.TargetNodeId == nodeId && l.TargetPinName == pinName
        );

        if (link != null)
        {
            // Load from the source node's output pin.
            if (_pinLocals.TryGetValue((link.SourceNodeId, link.SourcePinName), out var sourceLocal))
            {
                _il.Emit(OpCodes.Ldloc, sourceLocal);
                return;
            }
        }

        // Otherwise, check if there's a pinned value.
        var pv = _graph.PinnedValues.FirstOrDefault(p =>
            p.NodeId == nodeId && p.PinName == pinName
        );

        if (pv != null)
        {
            if (pv.Value is float f)
                _il.Emit(OpCodes.Ldc_R4, f);
            else if (pv.Value is int i)
                _il.Emit(OpCodes.Ldc_I4, i);
            else if (pv.Value is bool b)
                _il.Emit(OpCodes.Ldc_I4, b ? 1 : 0);
            return;
        }

        // Load a default value based on the pin type.
        var node = _graph.Nodes[nodeId];
        var pin = node.Pins.FirstOrDefault(p => p.Name == pinName);
        if (pin != null)
        {
            if (pin.Type == PinType.Float)
                _il.Emit(OpCodes.Ldc_R4, 0f);
            else if (pin.Type == PinType.Int)
                _il.Emit(OpCodes.Ldc_I4, 0);
            else if (pin.Type == PinType.Bool)
                _il.Emit(OpCodes.Ldc_I4, 0);
        }
    }

    private static Type PinTypeToCLRType(PinType pt) => pt switch
    {
        PinType.Float => typeof(float),
        PinType.Int => typeof(int),
        PinType.Bool => typeof(bool),
        _ => typeof(void)
    };

    // Resolved once, by reflection: Native is internal to Aver.Scene, so these are looked up with
    // BindingFlags.NonPublic rather than a plain method-group reference. (This is independent of the
    // InternalsVisibleTo grant on Aver.Scene.csproj, which is what lets GraphCompiler.cs name the
    // `Native` TYPE at compile time in the first place; GetMethod itself would find an internal
    // method either way.)
    private static readonly MethodInfo GetFieldMethod =
        typeof(Native).GetMethod("aver_scene_get_f32", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_get_f32 was not found by reflection");
    private static readonly MethodInfo SetFieldMethod =
        typeof(Native).GetMethod("aver_scene_set_f32", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_set_f32 was not found by reflection");
    private static readonly MethodInfo MathSinMethod =
        typeof(Math).GetMethod(nameof(Math.Sin), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Sin(double) was not found by reflection");
    private static readonly MethodInfo MathCosMethod =
        typeof(Math).GetMethod(nameof(Math.Cos), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Cos(double) was not found by reflection");

    /// Builds the Action/Action&lt;...&gt; or Func&lt;...,TResult&gt; matching paramTypes and
    /// returnType. Generalized over BCL Action`N/Func`N by name (both go up to 16 type parameters)
    /// rather than hand-listing every arity GraphCompiler happens to need today -- a graph declaring
    /// a 5th PARAM should not require a matching hardcoded case here.
    private static Type GetDelegateType(Type[] paramTypes, Type returnType)
    {
        int n = paramTypes.Length;

        if (returnType == typeof(void))
        {
            if (n == 0) return typeof(Action);
            var openAction = Type.GetType($"System.Action`{n}")
                ?? throw new NotSupportedException($"Action with {n} parameters is not supported");
            return openAction.MakeGenericType(paramTypes);
        }

        var openFunc = Type.GetType($"System.Func`{n + 1}")
            ?? throw new NotSupportedException($"Func with {n} parameters is not supported");
        var typeArgs = new Type[n + 1];
        Array.Copy(paramTypes, typeArgs, n);
        typeArgs[n] = returnType;
        return openFunc.MakeGenericType(typeArgs);
    }
}
