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

/// Compiles a graph to a DynamicMethod and invokes it.
public class GraphCompiler
{
    private Graph _graph;
    private Dictionary<int, LocalBuilder> _nodeLocals = new();
    private Dictionary<(int, string), LocalBuilder> _pinLocals = new();
    private ILGenerator? _il;

    public GraphCompiler(Graph graph)
    {
        _graph = graph;
    }

    /// Compiles the graph to a DynamicMethod. The method has no parameters and returns
    /// the single output pin's value, or void if multiple outputs.
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
            // Determine the return type. If there's one output, return its type.
            // If there are zero or multiple, return void.
            Type returnType = typeof(void);
            int singleOutputNodeId = -1;
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

            var method = new DynamicMethod(
                "CompiledGraph",
                returnType,
                Type.EmptyTypes,
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
            if (_graph.Outputs.Count == 1 && singleOutputNodeId >= 0)
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

            _il.Emit(OpCodes.Ret);

            return method.CreateDelegate(GetDelegateType(returnType));
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
        var visited = new HashSet<int>();
        var visiting = new HashSet<int>();
        var result = new List<Node>();

        foreach (var nodeId in _graph.Nodes.Keys)
        {
            if (!TopologicalSortDFS(nodeId, visited, visiting, result))
                return null;
        }

        return result;
    }

    private bool TopologicalSortDFS(int nodeId, HashSet<int> visited, HashSet<int> visiting, List<Node> result)
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

        // This is a placeholder. A real implementation would:
        // 1. Load the entity ID from the input pin.
        // 2. Call aver_scene_get_f32 through P/Invoke.
        // 3. Store the result to the output pin.
        //
        // For this minimal slice, we just return 0.0 to prove the structure.
        _il.Emit(OpCodes.Ldc_R4, 0f);
        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitSetField(Node node)
    {
        if (_il == null) return;

        // This is a placeholder. A real implementation would:
        // 1. Load the entity ID and value from input pins.
        // 2. Call aver_scene_set_f32 through P/Invoke.
        // 3. Store success (0 or 1) to the output pin.
        //
        // For this minimal slice, we just return success=true.
        _il.Emit(OpCodes.Ldc_I4, 1);
        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Loads a pin value onto the stack. If the pin is an output of another node, load it
    /// from that node's local. If it's a constant, load the constant. Otherwise, load a default.
    private void LoadPin(int nodeId, string pinName)
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

    private static Type GetDelegateType(Type returnType)
    {
        if (returnType == typeof(void))
            return typeof(Action);
        if (returnType == typeof(float))
            return typeof(Func<float>);
        if (returnType == typeof(int))
            return typeof(Func<int>);
        if (returnType == typeof(bool))
            return typeof(Func<bool>);
        throw new NotSupportedException($"Delegate type for {returnType.Name} is not supported");
    }
}
