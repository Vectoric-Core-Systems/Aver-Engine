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
using Aver.Framework;
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

    // ---- exec/PUSH compilation state -- see the PUSH VS PULL comment above CompileEntryPoint() -----

    // Hard cap on a single while/forEach's iteration count. Exists so a graph author's mistake (a
    // `cond` that never goes false, a `count` fed a huge or negative-looking value) cannot hang
    // whatever calls the compiled delegate -- a live game's frame, or this very test suite -- instead
    // of just running a very long time. PUBLIC so a test can assert against the real number rather
    // than a second, possibly-drifting copy of it (see GraphFlowTests.cs's guard test).
    public const int MaxLoopIterations = 100_000;

    // Loop/branch-local values that only make sense DURING or AFTER a specific run of the exec chain
    // -- a loop's live counter, a branch's "which side did I take", a SetField's captured return code
    // -- keyed the same way _pinLocals is (nodeId, pinName), but populated by EmitExecNode's control-
    // flow emitters rather than by the topological PULL pass. EmitPullOutput checks this FIRST, before
    // dispatching by node type, so `OUT whileNode iterations` (say) reads the live counter instead of
    // trying to re-derive "iterations" as if it were a pure expression -- it is not one.
    private Dictionary<(string, string), LocalBuilder> _execLocals = new();

    // Nodes currently on the exec walk's OWN call stack (not "ever visited" -- see EmitExecNode).
    // Catches a hand-authored exec LINK cycle that does not go through a while/forEach's internal
    // loop-back (which is not expressed as a graph link at all -- see EmitWhile/EmitForEach) at
    // COMPILE time, as a clear error, rather than recursing until the process's call stack overflows.
    private HashSet<string> _execVisiting = new();
    // THE SAME GUARD, FOR THE OTHER RECURSION, and it is not optional. EmitPullOutput recurses through
    // EmitPullInput to evaluate its operands, and a hand-authored .ocgraph can wire two data nodes into
    // each other -- add.a <- multiply.result and multiply.a <- add.result. Nothing upstream rejects it:
    // the parser accepts it, Graph.Validate() has no data-cycle check, and the exec walk's own
    // _execVisiting never sees these nodes because they are reached by PULL, not by PUSH.
    //
    // Without this the recursion is unbounded, and the failure is not a hang or an exception anyone can
    // report -- it is StackOverflowException, which .NET makes UNCATCHABLE by design. The compiler's own
    // catch(Exception) cannot intercept it; the entire host process dies, taking the editor with it, on
    // input a user typed. That is a worse outcome than any wrong number this compiler could produce, and
    // hand-authored .ocgraph is a normal input in this codebase, not an exotic one.
    //
    // Keyed on node id rather than (id, pin) deliberately: re-entering a node by ANY pin on the current
    // path is a cycle. A diamond -- two different consumers pulling the same node -- is not affected,
    // because the try/finally below removes the node when its own evaluation completes, so the second
    // consumer starts from an empty path.
    private HashSet<string> _pullVisiting = new();

    // The event name CompileEntryPoint() is currently compiling, purely so a loop-guard warning
    // emitted from deep inside EmitWhile/EmitForEach can name which entry point misbehaved.
    private string _currentEventName = "";

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

        // An OUT record cannot name an exec pin -- "what the graph hands back" is a data question;
        // control flow has nothing to hand back. Checked explicitly, here, rather than letting
        // PinTypeToCLRType's typeof(void) fall through to DeclareLocal(typeof(void)) a few lines below
        // and throw a confusing runtime ArgumentException: Validate() itself does not look at pin
        // TYPES for Outputs, only that the referenced pin exists and is an output (Graph.cs), so this
        // is the first point in Compile() that can see the mismatch. Shared with CompileEntryPoint(),
        // which has the identical question to answer about the identical field.
        if (!ValidateOutputsAreData(out err)) return null;

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

            // Emit code for each node in order -- SKIPPING the ones this compiler has no business
            // touching. OcGraph.hpp's own contract says a graph may be "pure dataflow, pure exec, or
            // both at once; the two halves do not interact", and that Compile() is "driven entirely
            // by `outputs`". It was not: it emitted EVERY node in the graph, so the first graph that
            // carried both halves died on `Node type 'OnTick' is not supported` -- a dataflow
            // compiler refusing to compile a graph because of nodes it was never meant to look at.
            //
            // Found the moment the cross-implementation fixture grew an exec chain, which is exactly
            // what that fixture is for. Skipping by "is this node exec-capable" rather than by
            // reachability from OUT keeps the change small and keeps the failure mode honest: a pure
            // DATA node that is genuinely unreachable still gets emitted and still reports its own
            // errors, so a typo in an unused subgraph is not silently swallowed.
            foreach (var node in sortedNodes)
            {
                if (IsExecOnlyNodeType(node.Type)) continue;
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

        // Create locals for all DATA output pins of this node. Exec-typed output pins are skipped --
        // PinTypeToCLRType(PinType.Exec) is typeof(void), and DeclareLocal(typeof(void)) throws; a
        // pure-dataflow node never legitimately has an exec pin (see the flow-node catalog in
        // OcGraphParser.AddDefaultPins), but TopologicalSort() walks EVERY node in the graph
        // regardless of type, so this guards the case where a flow node (branch/sequence/while/
        // forEach) ends up here because Compile() -- the OLD, PULL-only path -- was called on a graph
        // that also happens to declare one, rather than CompileEntryPoint(). EmitNode's switch below
        // still has no case for those types and throws NotSupportedException naming the type, which is
        // the right answer either way; this just keeps that the FIRST failure instead of an
        // unrelated-looking ArgumentException from the runtime.
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

            case "select":
                EmitSelect(node);
                break;

            case "inputkey":
                EmitInputKey(node);
                break;

            case "raycast":
                EmitRaycast(node);
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

    /// Select(cond, ifTrue, ifFalse) -> result: picks one of two float values by a bool condition.
    ///
    /// NOT SHORT-CIRCUITING, DELIBERATELY, AND THIS IS NOT THE SAME THING AS "BOTH ARMS COST
    /// NOTHING". The Brfalse/Br pair below only decides which LOCAL gets LOADED into `result` --
    /// ifTrue's and ifFalse's own upstream expressions were already computed by the time this method
    /// runs, because Compile()'s single topological pass (line ~191) calls EmitNode on EVERY node in
    /// the graph exactly once, regardless of what any other node's condition turns out to be. Unlike
    /// Branch's exec fan-out (EmitExecFanOut), which genuinely does not walk into the untaken arm's
    /// nodes at all, Select cannot skip computing either side -- both source nodes already ran before
    /// this method was ever called. The branch below is a real optimization (skip the LDLOC, not the
    /// computation) that would be equally correct as a branchless "compute both, keep one" -- it is
    /// written as a branch only because that is the shape LoadPin's caching model makes free.
    private void EmitSelect(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "cond");

        var falseLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();
        _il.Emit(OpCodes.Brfalse, falseLabel);

        LoadPin(node.Id, "ifTrue");
        _il.Emit(OpCodes.Br, endLabel);

        _il.MarkLabel(falseLabel);
        LoadPin(node.Id, "ifFalse");

        _il.MarkLabel(endLabel);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// InputKey(key) -> down: reads Aver.Framework's polled input state. Pure data, no exec pins --
    /// like GetField, reading is idempotent, so this is safe to call as many times as anything pulls
    /// it (through either compiler) with no _execLocals caching needed, unlike Raycast. aver_fw_input_key
    /// already returns 0/1 as an int32, which is exactly the bit pattern IL's Stloc expects for a bool
    /// local -- the same "no explicit conversion needed" property EmitCompare's Cgt result relies on.
    private void EmitInputKey(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "key");
        _il.Emit(OpCodes.Call, InputKeyMethod);

        if (_pinLocals.TryGetValue((node.Id, "down"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Raycast(originX,Y,Z, dirX,Y,Z, maxDist) -> hit, entity, pointX,Y,Z: one native call producing
    /// five results. This compiler (PULL) computes every node exactly once per Compile() regardless of
    /// how many things read its outputs (see EmitNode's own doc comment), so -- unlike the PUSH
    /// compiler's EmitExecRaycast, which needs _execLocals to get the same one-call guarantee -- a
    /// straightforward "one call, five _pinLocals stores" is already correct here with no extra
    /// mechanism: push the 7 inputs, push the ADDRESS of each of the 5 pre-declared output locals
    /// (RequirePinLocal/Ldloca), Call. See Aver.Framework.GraphInterop.RaycastForGraph's own comment
    /// for why the call is shaped as scalar out-params rather than a returned struct.
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

    /// Raycast's five out-parameters need a LOCAL's ADDRESS on the stack (Ldloca), not a loaded value
    /// -- unlike every other Emit* method's "load, compute, maybe Stloc" shape, so a missing pin can't
    /// just be skipped the way LoadPin's callers skip a missing _pinLocals entry (there would be
    /// nothing to push where the call signature requires an address). Thrown here, at the one call
    /// site that needs it, rather than silently leaving the IL stack unbalanced.
    private LocalBuilder RequirePinLocal(Node node, string pinName)
    {
        if (!_pinLocals.TryGetValue((node.Id, pinName), out var local))
            throw new InvalidOperationException(
                $"Raycast node '{node.Id}' has no output pin '{pinName}' declared -- Raycast needs all " +
                "five of hit/entity/pointX/pointY/pointZ to run (see OcGraphParser.AddDefaultPins's " +
                "'raycast' case, or give the node explicit PIN records for all five)");
        return local;
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

    /// Shared by Compile() and CompileEntryPoint(): an OUT record must name a DATA pin, never an exec
    /// pin -- see the call site in Compile() for the full reasoning. Returns true (err left null) when
    /// every Outputs entry is fine.
    private bool ValidateOutputsAreData(out string? err)
    {
        foreach (var (outNodeId, outPinName) in _graph.Outputs)
        {
            if (!_graph.Nodes.TryGetValue(outNodeId, out var outNode)) continue; // Validate() already refused this
            var outPin = outNode.Pins.FirstOrDefault(p => p.Name == outPinName && p.IsOutput);
            if (outPin != null && outPin.Type == PinType.Exec)
            {
                err = $"OUT '{outNodeId} {outPinName}' names an exec pin -- OUT is for data values; " +
                      "exec pins are control flow, not something a graph 'returns'";
                return false;
            }
        }
        err = null;
        return true;
    }

    private static Type PinTypeToCLRType(PinType pt) => pt switch
    {
        PinType.Float => typeof(float),
        PinType.Int => typeof(int),
        PinType.Bool => typeof(bool),
        // PinType.Exec falls here (typeof(void)) but should never actually be asked for: Compile()
        // rejects an exec-typed OUT before reaching a DeclareLocal call, EmitNode skips exec pins
        // when creating output locals, and OcGraphParser refuses an exec-typed PARAM at parse time.
        _ => typeof(void)
    };

    // =================================================================================================
    // EXEC / PUSH COMPILATION
    //
    // PUSH VS PULL, AND WHY THIS FILE HAS TWO COMPILERS NOW.
    //
    // Everything ABOVE this point -- Compile(), TopologicalSort(), EmitNode(), LoadPin() -- is the
    // ORIGINAL compiler and is UNTOUCHED by anything below: it PULLS. A node with no incoming exec
    // edge has no notion of "when" it runs -- it runs exactly once, whenever the topological pass
    // reaches it, and its value is cached into `_pinLocals` so every consumer reads the same computed
    // answer. That is correct for a DAG of pure expressions, which is all Compile() has ever had to
    // support, and adding exec support must not change how an existing graph -- one with no ENTRY
    // record and no exec pins at all -- compiles: Compile() is called exactly as before,
    // TopologicalSort/EmitNode/LoadPin are byte-for-byte unchanged above this line, and a graph the
    // parser has always accepted still produces the exact IL it always did.
    //
    // CompileEntryPoint(), below, PUSHES: a node reached via an exec edge runs at a specific, ordered
    // point in time, possibly more than once (inside a while/forEach loop) or not at all (the untaken
    // arm of a branch). "Cache the value in a local, computed once" is EXACTLY WRONG for that -- a
    // while loop's `cond` has to be a fresh read every pass, or the loop can never become false. So
    // the exec compiler does not reuse `_pinLocals`/TopologicalSort/LoadPin at all: it has its own
    // pull primitive, EmitPullInput/EmitPullOutput below, which is RECURSIVE AND UNCACHED -- every
    // call re-emits the full IL for whatever upstream subgraph produces the requested value, on the
    // spot, wherever in the method body that value is needed. Two consequences, both deliberate and
    // both named here rather than discovered later:
    //
    //   1. A shared pure sub-expression pulled from two different exec sites is computed TWICE (no
    //      common-subexpression elimination across exec sites, or across loop iterations). Correct,
    //      not free. A hot path would want a per-push-frame memoization scheme; Phase 1 is about a
    //      graph being ABLE to decide, not about how cheaply it decides, so this is left rough on
    //      purpose -- flagged again in the phase-2 handoff notes.
    //
    //   2. A node with a genuine SIDE EFFECT (today, only SetField) must never be reached through a
    //      PULL -- pulling it twice would perform its write twice, silently, which is a far worse bug
    //      than slow IL. EmitPullOutput refuses to pull from a side-effecting node's output and says
    //      why; SetField's write only happens when the exec walk reaches the node directly
    //      (EmitExecSideEffect, from EmitExecNode), exactly once per visit -- which is the entire point
    //      of giving a side-effecting node a place ON the exec chain instead of leaving it PULL-only.
    //
    // WHY THERE IS NO SPECIAL CASE FOR "Sequence". Firing a node's exec-output pins is a single
    // generic operation -- EmitExecFanOut, below -- that walks node.Pins in order and follows every
    // EXEC-typed OUTPUT pin that has a link. A node with exactly one such pin (OnStart, OnTick, or a
    // SetField given exec pins by hand) just continues the chain; a node with several (Sequence's
    // default two, or more added by hand via PIN records) fires them all, in file order. THAT is
    // "sequence": firing N things in a defined order is already what any multi-exec-out node means
    // once execution is pushed, so EmitExecNode's switch only special-cases the two node kinds whose
    // control flow is NOT "fire every exec-out pin" -- branch (fires exactly ONE of two) and
    // while/forEach (fire one exec-out pin a variable number of times, in a loop).
    // =================================================================================================

    /// <summary>Compiles ONE declared entry point -- an `ENTRY &lt;nodeId&gt; &lt;eventName&gt;` record
    /// -- to a Delegate, by walking PUSH/exec edges outward from that node. See the PUSH VS PULL
    /// comment above for how this differs from Compile()'s PULL/dataflow-only compilation, which this
    /// method does not touch, call, or depend on.
    ///
    /// The compiled method's parameters are exactly the graph's declared PARAM list, in declaration
    /// order -- the SAME convention Compile() already uses for its own delegate. This is deliberately
    /// how an OnTick entry point receives delta time: the graph declares `PARAM deltaTime float` and
    /// reads it with an ordinary `param` node inside the exec chain, rather than this compiler
    /// inventing a special "OnTick's second pin is always delta time" rule. No new node type is needed
    /// for "the current tick's delta time," and no format change is needed to add a future event that
    /// wants different arguments -- it just declares different PARAMs.
    ///
    /// THE RETURN VALUE follows the exact same OUT convention Compile() already established -- zero
    /// OUT records means void, one means that pin's own CLR type, two or more means a boxed object[]
    /// in file order (see the OUTPUTS ARRAY comment on Compile()). An exec chain's OUT is read AFTER
    /// the whole chain finishes running, by pulling each referenced pin's CURRENT value -- which,
    /// because EmitPullOutput checks `_execLocals` first, can be an ordinary pure expression OR one of
    /// the loop/branch/side-effect values the exec walk itself produced (a while's `iterations`, a
    /// branch's `tookTrue`, a Sequence's `fireLog`, a SetField's captured `success`) -- see those
    /// nodes' own pin comments in OcGraphParser.AddDefaultPins. This is what makes an otherwise
    /// internal, per-invocation-only value observable to a caller (or a test) without a live native
    /// scene to write into and read back from.
    ///
    /// Returns null and reports err on: no ENTRY record for `eventName`, the ENTRY node not existing,
    /// an OUT record naming an exec pin, an exec output pin wired to more than one link, an exec cycle
    /// not mediated by a while/forEach, or a node type this walker does not understand appearing
    /// directly on the exec chain (a node reachable only through ordinary DATA links is unaffected --
    /// only nodes the exec walk itself steps onto need to be something EmitExecNode/EmitPullOutput know
    /// how to handle).</summary>
    public Delegate? CompileEntryPoint(string eventName, out string? err)
    {
        err = null;

        if (!_graph.Validate(out var validateErr))
        {
            err = validateErr;
            return null;
        }
        if (!ValidateOutputsAreData(out err)) return null;

        string? startNodeId = null;
        foreach (var (entryNodeId, entryEventName) in _graph.EntryPoints)
        {
            if (entryEventName == eventName) { startNodeId = entryNodeId; break; }
        }
        if (startNodeId == null)
        {
            err = $"no ENTRY record declares event '{eventName}'";
            return null;
        }
        if (!_graph.Nodes.TryGetValue(startNodeId, out var startNode))
        {
            // Graph.Validate() already refuses this at parse time, but Compile()/CompileEntryPoint()
            // can also run against a Graph built programmatically rather than through the text parser
            // (see Graph.Validate's own note on why it re-checks things the parser also checked), so
            // this is not unreachable in principle.
            err = $"ENTRY '{eventName}' names node '{startNodeId}', which does not exist";
            return null;
        }

        try
        {
            // Return type: the same zero/one/many convention Compile() uses -- see this method's own
            // doc comment for why OUT means the same thing in both compilers.
            Type returnType = typeof(void);
            if (_graph.Outputs.Count == 1)
            {
                var (onlyOutNodeId, onlyOutPinName) = _graph.Outputs[0];
                if (_graph.Nodes.TryGetValue(onlyOutNodeId, out var onlyOutNode))
                {
                    var onlyOutPin = onlyOutNode.Pins.FirstOrDefault(p => p.Name == onlyOutPinName && p.IsOutput);
                    if (onlyOutPin != null) returnType = PinTypeToCLRType(onlyOutPin.Type);
                }
            }
            else if (_graph.Outputs.Count >= 2)
            {
                returnType = typeof(object[]);
            }

            Type[] paramTypes = _graph.Parameters.Select(p => PinTypeToCLRType(p.Type)).ToArray();
            var method = new DynamicMethod(
                $"CompiledGraphEvent_{eventName}",
                returnType,
                paramTypes,
                restrictedSkipVisibility: true
            );

            _il = method.GetILGenerator();
            _execLocals.Clear();
            _execVisiting.Clear();
            _pullVisiting.Clear();
            _currentEventName = eventName;

            EmitExecNode(startNode);

            // Read OUT AFTER the chain has fully run -- pulling now sees any loop/branch/side-effect
            // local the walk above populated, exactly like reading a variable after a function body
            // finishes.
            if (_graph.Outputs.Count == 1)
            {
                var (onlyOutNodeId, onlyOutPinName) = _graph.Outputs[0];
                if (!_graph.Nodes.TryGetValue(onlyOutNodeId, out var onlyOutNode))
                {
                    err = $"OUT '{onlyOutNodeId} {onlyOutPinName}' names a node that does not exist";
                    return null;
                }
                EmitPullOutput(onlyOutNode, onlyOutPinName);
            }
            else if (_graph.Outputs.Count >= 2)
            {
                _il.Emit(OpCodes.Ldc_I4, _graph.Outputs.Count);
                _il.Emit(OpCodes.Newarr, typeof(object));
                for (int i = 0; i < _graph.Outputs.Count; i++)
                {
                    var (outNodeId, outPinName) = _graph.Outputs[i];
                    if (!_graph.Nodes.TryGetValue(outNodeId, out var outNode))
                    {
                        err = $"OUT '{outNodeId} {outPinName}' names a node that does not exist";
                        return null;
                    }
                    var outPin = outNode.Pins.FirstOrDefault(p => p.Name == outPinName && p.IsOutput);
                    Type boxType = outPin != null ? PinTypeToCLRType(outPin.Type) : typeof(float);

                    _il.Emit(OpCodes.Dup);
                    _il.Emit(OpCodes.Ldc_I4, i);
                    EmitPullOutput(outNode, outPinName);
                    _il.Emit(OpCodes.Box, boxType);
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

    /// Emits IL for one step of the exec walk: run `node`'s own logic (if it has any worth
    /// sequencing), then hand control on. `_execVisiting` guards against a hand-authored exec LINK
    /// cycle that does not go through a while/forEach's internal loop-back -- see that field's own
    /// comment -- by throwing a clear compile error instead of recursing forever. A DIAMOND (two
    /// branches of an earlier `branch` both eventually reaching the same downstream node) is NOT a
    /// cycle and is explicitly allowed: `_execVisiting` only tracks nodes on the CURRENT recursion
    /// path, removed again in `finally` once that path returns, so the false arm can still reach a
    /// node the true arm already visited and returned from. The cost of allowing that is that shared
    /// downstream node's IL is emitted twice (once inlined at the end of each arm) rather than once
    /// with a jump -- accepted, and named, in the PUSH VS PULL comment above as a Phase 1 rough edge;
    /// a real join-point/basic-block compiler is out of scope here.
    private void EmitExecNode(Node node)
    {
        if (_il == null) return;
        if (!_execVisiting.Add(node.Id))
            throw new InvalidOperationException(
                $"exec cycle detected at node '{node.Id}' while compiling event '{_currentEventName}' -- " +
                "a node's exec chain reached itself with no while/forEach in between. A loop must go " +
                "through a while/forEach node (its 'loop' exec-output pin is what repeats); a direct " +
                "exec link back to an earlier node is not supported and would compile-recurse forever.");

        try
        {
            switch (node.Type.ToLowerInvariant())
            {
                case "branch":
                    EmitBranch(node);
                    return; // branch owns its own fan-out (exactly one of two arms); no generic fan-out after it
                case "while":
                    EmitWhile(node);
                    return;
                case "foreach":
                    EmitForEach(node);
                    return;
                default:
                    // Every other node type reached via exec: run its own side effect, if it has one
                    // worth sequencing (today, only SetField does -- see IsExecCapableSideEffectType),
                    // or its own cached QUERY, if it has one worth running exactly once per visit
                    // (today, only Raycast does -- see IsExecCapableQueryType), then fall through to
                    // the generic multi-exec-out fan-out, which is what makes a plain node with 0, 1,
                    // or N exec-output pins behave correctly (no-op passthrough, continue, or
                    // "sequence") with no special case here at all.
                    if (IsExecCapableSideEffectType(node.Type)) EmitExecSideEffect(node);
                    else if (IsExecCapableQueryType(node.Type)) EmitExecRaycast(node);
                    EmitExecFanOut(node);
                    return;
            }
        }
        finally
        {
            _execVisiting.Remove(node.Id);
        }
    }

    /// Fires every declared EXEC-typed OUTPUT pin on `node`, in the order node.Pins lists them --
    /// file order for an explicit PIN record, catalog order for a default-pinned node (see
    /// OcGraphParser.AddDefaultPins / sandbox/src/GraphNodeDefs.hpp). See the section-level comment
    /// above for why this alone is what implements "Sequence" with no dedicated case.
    private void EmitExecFanOut(Node node)
    {
        if (_il == null) return;

        var execOuts = node.Pins.Where(p => p.IsOutput && p.Type == PinType.Exec).ToList();

        // "fireLog" -- OPT-IN OBSERVABILITY, not part of the control-flow contract, and a no-op for
        // any node that doesn't declare an int output pin literally named "fireLog" (Sequence's
        // default pins do; see OcGraphParser.AddDefaultPins). Proving "a sequence really fires its
        // arms in the file's order" needs a channel that survives to the end of the compiled method:
        // a SetField write cannot be observed in this process (no live native scene -- see
        // GraphHostTests.cs's own class comment for the identical limitation elsewhere in this
        // codebase), and a pure DATA pull cannot prove a node was actually VISITED by the exec walk,
        // only that it CAN be computed on demand. Updated as `fireLog = fireLog*10 + (armIndex+1)`
        // before each arm fires; three arms firing in order leaves fireLog == 123.
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

    /// Resolves the single exec LINK leaving `nodeId.pinName`, if any -- null means that arm is simply
    /// not wired to anything, which is not an error (the same permissiveness LoadPin already extends
    /// to an unwired DATA input, which falls back to a default rather than failing).
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

    /// branch: a bool condition, ONE incoming exec pulse, and exactly one of two outgoing exec pins
    /// (`true`/`false`) fires -- unlike EmitExecFanOut, which fires ALL of a node's exec-out pins.
    private void EmitBranch(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "cond");

        var tookTruePin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "tookTrue" && p.Type == PinType.Bool);
        LocalBuilder? tookTrue = tookTruePin != null ? GetOrCreateExecLocal(node.Id, "tookTrue", typeof(bool)) : null;

        var falseLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();
        _il.Emit(OpCodes.Brfalse, falseLabel);

        if (tookTrue != null) { _il.Emit(OpCodes.Ldc_I4_1); _il.Emit(OpCodes.Stloc, tookTrue); }
        Node? trueTarget = FindExecTarget(node.Id, "true");
        if (trueTarget != null) EmitExecNode(trueTarget);
        _il.Emit(OpCodes.Br, endLabel);

        _il.MarkLabel(falseLabel);
        if (tookTrue != null) { _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Stloc, tookTrue); }
        Node? falseTarget = FindExecTarget(node.Id, "false");
        if (falseTarget != null) EmitExecNode(falseTarget);

        _il.MarkLabel(endLabel);
    }

    /// while: `cond` (re-pulled fresh every pass -- see the PUSH VS PULL comment for why this cannot
    /// be the cached-local approach LoadPin/EmitNode use) gates a `loop` body that may run any number
    /// of times, up to MaxLoopIterations -- past that, WarnLoopGuardTripped logs (naming the node and
    /// event) and the loop is stopped exactly as if `cond` had gone false, so a buggy graph loses one
    /// tick's worth of correctness rather than hanging whatever called it. `done` runs once, either
    /// when `cond` genuinely goes false or when the guard trips.
    private void EmitWhile(Node node)
    {
        if (_il == null) return;
        var iterations = GetOrCreateExecLocal(node.Id, "iterations", typeof(int));
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Stloc, iterations);

        var loopCheck = _il.DefineLabel();
        var guardTripped = _il.DefineLabel();
        var loopDone = _il.DefineLabel();

        _il.MarkLabel(loopCheck);
        EmitPullInput(node, "cond");
        _il.Emit(OpCodes.Brfalse, loopDone);

        _il.Emit(OpCodes.Ldloc, iterations);
        _il.Emit(OpCodes.Ldc_I4, MaxLoopIterations);
        _il.Emit(OpCodes.Bge, guardTripped);

        _il.Emit(OpCodes.Ldloc, iterations);
        _il.Emit(OpCodes.Ldc_I4_1);
        _il.Emit(OpCodes.Add);
        _il.Emit(OpCodes.Stloc, iterations);

        Node? body = FindExecTarget(node.Id, "loop");
        if (body != null) EmitExecNode(body);
        _il.Emit(OpCodes.Br, loopCheck);

        _il.MarkLabel(guardTripped);
        _il.Emit(OpCodes.Ldstr, node.Id);
        _il.Emit(OpCodes.Ldstr, _currentEventName);
        _il.Emit(OpCodes.Call, WarnLoopGuardMethod);

        _il.MarkLabel(loopDone);
        Node? done = FindExecTarget(node.Id, "done");
        if (done != null) EmitExecNode(done);
    }

    /// forEach: the COUNTED-REPEAT variant -- see OcGraphParser.AddDefaultPins's "foreach" case for
    /// why (no array/collection pin type exists yet to iterate a real collection). `index` runs
    /// 0..count-1 through the `loop` body; the natural `index >= count` bound already prevents
    /// "forever" for any finite count, and MaxLoopIterations is a second, redundant guard for a
    /// corrupted or absurdly large `count` -- cheap insurance, not the primary termination mechanism.
    private void EmitForEach(Node node)
    {
        if (_il == null) return;
        var count = _il.DeclareLocal(typeof(int)); // plain local: `count` is read once per activation, not exposed as a pin
        EmitPullInput(node, "count");
        _il.Emit(OpCodes.Stloc, count);

        var index = GetOrCreateExecLocal(node.Id, "index", typeof(int));
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Stloc, index);

        var loopCheck = _il.DefineLabel();
        var guardTripped = _il.DefineLabel();
        var loopDone = _il.DefineLabel();

        _il.MarkLabel(loopCheck);
        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldloc, count);
        _il.Emit(OpCodes.Bge, loopDone); // index >= count -> done (also the whole story for count <= 0)

        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldc_I4, MaxLoopIterations);
        _il.Emit(OpCodes.Bge, guardTripped);

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
        Node? done = FindExecTarget(node.Id, "done");
        if (done != null) EmitExecNode(done);
    }

    /// Declares (once) or returns (on every later call) the IL local backing one exec-scoped pin --
    /// a loop's live counter, a branch's "which side" flag, a captured SetField return code. Declared
    /// ONCE per compile (DeclareLocal is only ever called the first time), but the SAME slot is
    /// written afresh every time its owning node's code runs at RUNTIME -- including every pass of a
    /// loop, since the IL that writes it is emitted once but sits inside the loop's branch-back range.
    /// That single property is what makes a loop counter "just work" without re-declaring anything per
    /// iteration: IL loops via jumps, not by re-emitting the body N times.
    private LocalBuilder GetOrCreateExecLocal(string nodeId, string pinName, Type type)
    {
        if (_il == null) throw new InvalidOperationException("no active ILGenerator");
        var key = (nodeId, pinName);
        if (_execLocals.TryGetValue(key, out var existing)) return existing;
        var local = _il.DeclareLocal(type);
        _execLocals[key] = local;
        return local;
    }

    /// Node types with a real side effect worth running when the exec walk reaches them directly.
    /// Today, only SetField -- see the section-level PUSH VS PULL comment, point 2, for why this list
    /// exists at all and why EmitPullOutput refuses to pull FROM one of these instead.
    private static bool IsExecCapableSideEffectType(string type) =>
        type.Equals("setfield", StringComparison.OrdinalIgnoreCase);

    /// Node types with NO side effect (a plain read, safe to call any number of times with the same
    /// inputs) that STILL want the exec walk's "compute once per visit, cache into _execLocals" shape
    /// -- today, only Raycast. Deliberately a SEPARATE predicate from IsExecCapableSideEffectType,
    /// not a second name for the same list: SetField exists on the exec chain because pulling it twice
    /// would be WRONG (a write happening twice, silently); Raycast exists on the exec chain because
    /// pulling it twice would merely be WASTEFUL (an expensive native physics query re-run for no
    /// reason) -- correctness vs cost is a real distinction worth two names, even though both end up
    /// calling EmitExecNode's default case and both populate _execLocals the same way. A node in this
    /// list is safe for EmitPullOutput to compute fresh too (unlike a side-effect type, which
    /// EmitPullOutput actively refuses) -- Raycast simply has no such fallback today, by choice, not
    /// because pulling it would be incorrect; see EmitPullOutput's own "raycast has NO case here"
    /// comment for that choice's reasoning.
    private static bool IsExecCapableQueryType(string type) =>
        type.Equals("raycast", StringComparison.OrdinalIgnoreCase);

    /// Runs a SetField node's write exactly once, at the point the exec walk reaches it -- mirrors
    /// EmitSetField's own field=/resolver/native-call logic, but pulls its "entity"/"value" inputs
    /// through EmitPullInput rather than LoadPin/_pinLocals (see the section-level comment for why the
    /// two input mechanisms are not shared). If the node declares a "success" output pin, the real
    /// P/Invoke return code is captured into an exec-local (the same mechanism branch's "tookTrue" and
    /// the loop nodes' counters use) so it can be read back via OUT after the chain finishes; if not,
    /// the value is discarded rather than left on the stack.
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
            _il.Emit(OpCodes.Pop); // nothing declared to read the return code; discard it
        }
    }

    /// Runs a Raycast node's native query exactly once, at the point the exec walk reaches it --
    /// mirrors EmitRaycast's own "one call, five results" shape, but pulls its 7 inputs through
    /// EmitPullInput rather than LoadPin/_pinLocals (see the section-level comment for why the two
    /// input mechanisms are not shared), and stores each of its 5 results into its OWN exec-local via
    /// GetOrCreateExecLocal -- the same mechanism EmitExecSideEffect uses for SetField's "success", so
    /// a later `OUT raycastNode hit` (etc) reads the live, already-computed value via
    /// EmitPullOutput's _execLocals check rather than trying to re-derive it (Raycast has no case of
    /// its own in EmitPullOutput's switch -- see that method's "raycast has NO case here" comment).
    /// Called from EmitExecNode's default case, exactly like EmitExecSideEffect, just gated by
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

    /// Pulls the value linked into `node`'s input pin `pinName` and pushes it onto the IL stack:
    /// follows a LINK to its source and recurses into EmitPullOutput, falls back to a PINVAL, then to
    /// the pin's zero-ish default -- the exact same three-step fallback LoadPin already uses for the
    /// PULL-only compiler, just re-emitted at THIS call site instead of assumed pre-computed in
    /// `_pinLocals` (which the exec compiler never populates for data nodes -- see the section header).
    private void EmitPullInput(Node node, string pinName)
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

        var pv = _graph.PinnedValues.FirstOrDefault(p => p.NodeId == node.Id && p.PinName == pinName);
        if (pv != null)
        {
            if (pv.Value is float pf) _il.Emit(OpCodes.Ldc_R4, pf);
            else if (pv.Value is int pi) _il.Emit(OpCodes.Ldc_I4, pi);
            else if (pv.Value is bool pb) _il.Emit(OpCodes.Ldc_I4, pb ? 1 : 0);
            return;
        }

        var pin = node.Pins.FirstOrDefault(p => p.Name == pinName);
        var t = pin?.Type ?? PinType.Float;
        if (t == PinType.Bool) _il.Emit(OpCodes.Ldc_I4_0);
        else if (t == PinType.Int) _il.Emit(OpCodes.Ldc_I4_0);
        else _il.Emit(OpCodes.Ldc_R4, 0f);
    }

    /// Pushes node `source`'s output pin `pinName` onto the IL stack, computing it fresh every call
    /// (no memoization -- see the section header). Checked first against `_execLocals` (a live loop
    /// counter or a captured side-effect result), then dispatched by node TYPE for the pure expression
    /// kinds Compile()'s EmitNode already knows how to build. SetField (and any future side-effecting
    /// type IsExecCapableSideEffectType names) is refused here on purpose.
    /// True for node kinds that exist ONLY to be walked by the exec/PUSH compiler and have no data
    /// value to pull. Compile() (the dataflow/PULL compiler) skips these rather than failing on them:
    /// a graph is allowed to carry both halves, and the halves do not interact.
    ///
    /// Deliberately NOT the same predicate as IsExecCapableSideEffectType. SetField has exec pins AND
    /// a data output somebody might wrongly try to read -- it needs to be reachable so it can refuse
    /// with its own explanatory error. The kinds below have no data output at all, so there is nothing
    /// for the pull compiler to do with them except fail.
    private static bool IsExecOnlyNodeType(string type) => type.ToLowerInvariant() switch
    {
        "onstart" or "ontick" or "branch" or "sequence" or "while" or "foreach" => true,
        _ => false,
    };

    private void EmitPullOutput(Node source, string pinName)
    {
        if (_il == null) return;

        if (_execLocals.TryGetValue((source.Id, pinName), out var local))
        {
            _il.Emit(OpCodes.Ldloc, local);
            return;
        }

        if (IsExecCapableSideEffectType(source.Type))
            throw new InvalidOperationException(
                $"'{source.Id}.{pinName}' cannot be read as a data value: SetField has a side effect and " +
                "must be reached by wiring it directly into the exec chain (give it exec pins), not by " +
                "pulling its output from somewhere else -- pulling could run its write more than once, " +
                "or not at all, depending on what else reads it");

        // See _pullVisiting's declaration for why an uncaught cycle here kills the process rather than
        // raising something catchable. Reported as an ordinary compile error naming both ends, so the
        // author is told which wire to cut.
        if (!_pullVisiting.Add(source.Id))
            throw new InvalidOperationException(
                $"data cycle detected while evaluating '{source.Id}.{pinName}': this node's value " +
                "depends, through a chain of data links, on itself. Data links must form a directed " +
                "acyclic graph -- only exec links may loop, and only through While/ForEach, which are " +
                "bounded. Break the cycle by removing one of the data links into this node.");
        try
        {

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
            case "add":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Add); return;
            case "multiply":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Mul); return;
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
            case "select":
            {
                // Mirrors EmitSelect's own branch shape, but PULLED (recursive, uncached) rather than
                // stored to a _pinLocals entry -- see EmitPullInput/EmitPullOutput's own section header
                // for why the exec compiler re-emits an upstream expression at every pull site instead
                // of caching it. UNLIKE EmitSelect's PULL-compiler branch (which only skips which local
                // gets LOADED, since both arms' upstream nodes already ran during the topological
                // walk), this branch is a REAL short-circuit: EmitPullInput recursively emits and runs
                // whichever arm's upstream subgraph is chosen, so only ONE of ifTrue/ifFalse's cost is
                // ever paid per pull here.
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
            // "raycast" has NO case here, deliberately. A Raycast node reached VIA THE EXEC CHAIN
            // populates _execLocals for all five of its outputs (see EmitExecRaycast), and
            // EmitPullOutput already checks _execLocals before this switch runs (top of this method)
            // -- so a Raycast visited by the exec walk needs no dispatch code here at all. A Raycast
            // node that is NEVER visited by exec (only reached by a data LINK, with no incoming exec
            // edge wired to it, inside a graph CompileEntryPoint is compiling) falls through to the
            // `default` arm below and reports a clear NotSupportedException -- a deliberate Phase-1
            // limitation, not an oversight: unlike GetField (EmitPullGetField), Raycast has no
            // standalone "just call it" pull path in the PUSH compiler, because giving it one would
            // mean an author-visible node type behaves differently depending on whether it happens to
            // sit on the exec chain, which is a worse trap than a clear compile error naming the node.
            // A PURE-PULL graph (no ENTRY at all, see IsExecOnlyNodeType and Compile()'s own foreach)
            // is unaffected -- it never reaches EmitPullOutput in the first place; EmitRaycast (PULL)
            // handles it completely on its own.
            default:
                throw new NotSupportedException(
                    $"node type '{source.Type}' cannot be pulled as a data value inside an exec chain " +
                    "(it is not one of the pure expression kinds this compiler knows, and has no exec " +
                    "pins reaching it directly either)");
        }

        }
        finally
        {
            // In finally rather than after the switch because EVERY arm above returns or throws --
            // there is no fallthrough path to put this on. Removing on the way out is what keeps a
            // diamond (two consumers pulling the same node) legal while a genuine cycle is not.
            _pullVisiting.Remove(source.Id);
        }
    }

    /// Mirrors EmitDivide's own zero-divisor convention (b == 0 yields 0.0, never NaN/Infinity) --
    /// see that method's comment for the full reasoning -- but pushes the result rather than storing
    /// it to a `_pinLocals` entry.
    private void EmitPullDivide(Node node)
    {
        if (_il == null) return;
        var aLocal = _il.DeclareLocal(typeof(float));
        var bLocal = _il.DeclareLocal(typeof(float));
        EmitPullInput(node, "a"); _il.Emit(OpCodes.Stloc, aLocal);
        EmitPullInput(node, "b"); _il.Emit(OpCodes.Stloc, bLocal);

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
    }

    /// Mirrors EmitGetField's own field=/resolver checks -- see that method's comment -- but pushes
    /// the read value rather than storing it to a `_pinLocals` entry. Reading is idempotent (no state
    /// changes), so unlike SetField this is safe to pull more than once; see IsExecCapableSideEffectType.
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
    private static readonly MethodInfo WarnLoopGuardMethod =
        typeof(GraphCompiler).GetMethod(nameof(WarnLoopGuardTripped), BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("GraphCompiler.WarnLoopGuardTripped was not found by reflection");
    // Aver.Framework internals, reached the same way as Aver.Scene's Native above -- see
    // Aver.Framework.csproj's InternalsVisibleTo("Aver.Graph") grant.
    private static readonly MethodInfo InputKeyMethod =
        typeof(Fw).GetMethod("aver_fw_input_key", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key was not found by reflection");
    private static readonly MethodInfo RaycastMethod =
        typeof(GraphInterop).GetMethod("RaycastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RaycastForGraph was not found by reflection");

    /// Called FROM EMITTED IL (see EmitWhile/EmitForEach), not from ordinary C# control flow, when a
    /// loop's iteration count crosses MaxLoopIterations. Logs -- loudly, naming the exact node and
    /// event -- and lets the loop's `done` path run anyway, exactly as if `cond`/`count` had run out
    /// normally, rather than throwing mid-tick: a graph bug should be visible, not a crashed frame for
    /// whatever else the game was doing that tick. Static and private: reachable from the emitted IL
    /// only because CompileEntryPoint's DynamicMethod sets restrictedSkipVisibility:true, the same
    /// mechanism that already lets EmitGetField/EmitSetField call Aver.Scene.Native's own internal
    /// P/Invoke methods.
    private static void WarnLoopGuardTripped(string nodeId, string eventName)
    {
        Console.Error.WriteLine(
            $"[GraphCompiler] loop guard tripped: node '{nodeId}' (event '{eventName}') exceeded " +
            $"{MaxLoopIterations} iterations and was stopped -- this graph has (or was about to have) " +
            "an infinite loop; check its 'cond'/'count' wiring");
    }

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
