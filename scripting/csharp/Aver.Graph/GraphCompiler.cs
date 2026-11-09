// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// IL compiler for graphs: uses System.Reflection.Emit (not Roslyn) so games need no compiler at
// runtime. The compiled method is cached in the AssemblyLoadContext for reuse.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

/// <summary>Resolves a scene field's dense id and kind by qualified name so getfield/setfield are
/// checked at COMPILE time, not silently no-op'd at runtime. fieldId 0 (return false) means
/// unknown; kind mirrors aver::scene::FieldKind (Fields.hpp).
///
/// Default impl calls Native.aver_scene_field/aver_scene_field_kind, which needs a loadable native
/// Aver.Scene library -- a scene-less unit test should supply its own resolver instead.</summary>
public delegate bool FieldResolver(string qualifiedName, out int fieldId, out int kind);

/// Compiles a graph to a DynamicMethod and invokes it.
public partial class GraphCompiler
{
    // aver::scene::FieldKind (Fields.hpp): checked by getfield/setfield (F32) and
    // getfieldvec3/setfieldvec3 (Vec3 -- CLocal.position, CLight.colour, etc, Builtins.cpp). Vec3
    // needed no new pin type: reads/writes as three Float pins (x/y/z), like Raycast's multi-output
    // call. Quat (arity 4)/Mat4 (arity 16) out of scope; RequireVec3Field guards
    // getfieldvec3/setfieldvec3's arity.
    private const int FieldKindF32 = 0;
    private const int FieldKindVec3 = 1;

    private Graph _graph;
    private readonly FieldResolver _fieldResolver;

    // One DynamicMethod per FUNC, created for ALL functions before any body is emitted, so a call can
    // target a callee whose IL isn't written yet (including itself, or one emitted later) --
    // Emit(OpCodes.Call, ...) can reference a DynamicMethod before its IL exists.
    private readonly Dictionary<string, DynamicMethod> _funcMethods = new(StringComparer.OrdinalIgnoreCase);
    private bool _functionsCompiled;
    // Null while emitting the event graph. FuncEntry turns a pin name into an argument index via
    // this; FuncReturn finds its output locals via this.
    private GraphFunction? _currentFunc;
    private List<LocalBuilder> _funcOutLocals = new();
    private Dictionary<string, LocalBuilder> _nodeLocals = new();
    private Dictionary<(string, string), LocalBuilder> _pinLocals = new();
    private ILGenerator? _il;

    // Exec/PUSH compilation state -- see the PUSH VS PULL comment above CompileEntryPoint().

    // Hard cap on a while/forEach's iteration count: stops a bad `cond`/`count` from hanging the
    // caller (a live frame, or this test suite) instead of erroring. Public so GraphFlowTests.cs
    // can assert the real number, not a drifting copy.
    public const int MaxLoopIterations = 100_000;

    // Loop/branch-local values valid only during/after a specific exec run (a loop counter, a
    // branch's taken side, SetField's return code) -- keyed like _pinLocals (nodeId, pinName),
    // populated by EmitExecNode's control-flow emitters. EmitPullOutput checks this first, so e.g.
    // `OUT whileNode iterations` reads the live counter, not a re-derived pure expression.
    private Dictionary<(string, string), LocalBuilder> _execLocals = new();

    // Nodes on the exec walk's own call stack right now (not "ever visited" -- see EmitExecNode).
    // Catches a hand-authored exec link cycle -- distinct from while/forEach's internal loop-back,
    // which is not a graph link (see EmitWhile/EmitForEach) -- at compile time, not via stack overflow.
    private HashSet<string> _execVisiting = new();
    // Same guard, for PULL recursion (EmitPullOutput -> EmitPullInput): a hand-authored .ocgraph can
    // wire two data nodes into each other with nothing upstream rejecting it (no parser/Validate()
    // cycle check), and _execVisiting doesn't see PULL-reached nodes. Unbounded recursion here is an
    // UNCATCHABLE StackOverflowException that kills the host process (editor included) -- worse than
    // a wrong number, and hand-authored .ocgraph is normal input here, not exotic.
    // Keyed on node id, not (id, pin): re-entering by ANY pin on the current path is a cycle. A
    // diamond (two consumers pulling the same node) is fine -- try/finally below clears the node
    // once its own evaluation completes.
    private HashSet<string> _pullVisiting = new();

    // The event CompileEntryPoint() is compiling, so a loop-guard warning from deep inside
    // EmitWhile/EmitForEach can name which entry point misbehaved.
    private string _currentEventName = "";

    // Graph-local persistent variables (VAR / GraphVarStore).

    // Index of the trailing GraphVarStore parameter, or -1 if no VAR records (common case) --
    // additive: paramTypes only grows when Variables.Count > 0, so a VAR-less graph's delegate shape
    // is unchanged. Set once per Compile()/CompileEntryPoint() before any node is emitted;
    // EmitGetVar/EmitPullGetVar/EmitExecSetVar are the only readers.
    private int _varStoreArgIndex = -1;

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

    /// Compiles the graph to a DynamicMethod: one argument per declared PARAM in order (no PARAM
    /// records means zero arguments), returning the single output pin's value or void for multiple
    /// outputs. Returns null on failure; check log for errors.
    public Delegate? Compile(out string? err)
    {
        err = null;

        if (!_graph.Validate(out var validateErr))
        {
            err = validateErr;
            return null;
        }

        // Functions first: emitting one takes over _il and every local map this class owns (see
        // EnsureFunctionsCompiled). Doing it before this method's own DynamicMethod exists avoids a
        // save/restore dance at every call site.
        if (!EnsureFunctionsCompiled(out err)) return null;

        // An OUT record can't name an exec pin -- control flow has nothing to "hand back". Checked
        // here rather than letting PinTypeToCLRType's typeof(void) reach DeclareLocal(typeof(void))
        // and throw a confusing runtime ArgumentException; Validate() doesn't check pin types for
        // Outputs (Graph.cs), so this is the first point that can catch it. Shared with
        // CompileEntryPoint(), which faces the identical question.
        if (!ValidateOutputsAreData(out err)) return null;

        try
        {
            // Return type: zero outputs -> void; one -> that pin's CLR type (keeps existing
            // single-output graphs' Func<T> shape); 2+ -> object[], one boxed entry per Outputs
            // record in file order -- see OUTPUTS ARRAY below for why object[] over a 2nd delegate
            // convention.
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

            // Trailing GraphVarStore param iff >=1 VAR declared (see _varStoreArgIndex) -- kept
            // separate from PARAM because PARAM is caller-supplied, VAR is graph-owned; merging the
            // two slots would erase that distinction.
            if (_graph.Variables.Count > 0)
            {
                _varStoreArgIndex = paramTypes.Length;
                paramTypes = paramTypes.Append(typeof(GraphVarStore)).ToArray();
            }
            else
            {
                _varStoreArgIndex = -1;
            }

            var method = new DynamicMethod(
                "CompiledGraph",
                returnType,
                paramTypes,
                restrictedSkipVisibility: true
            );

            _il = method.GetILGenerator();
            _nodeLocals.Clear();
            _pinLocals.Clear();

            var sortedNodes = TopologicalSort();
            if (sortedNodes == null)
            {
                err = "Graph has a cycle";
                return null;
            }

            // Emit each node, skipping exec-only ones: OcGraph.hpp says a graph may be pure dataflow,
            // pure exec, or both, with Compile() "driven entirely by `outputs`" -- but it used to emit
            // every node, so a graph with both halves died on `Node type 'OnTick' is not supported`
            // (found when the cross-implementation fixture grew an exec chain). Skips by "is this
            // node exec-capable" rather than by reachability from OUT, so an unreachable pure-DATA
            // node still gets emitted and still reports its own errors -- a typo in an unused
            // subgraph isn't silently swallowed.
            foreach (var node in sortedNodes)
            {
                // A node inside a FUNCTION isn't part of this method -- TopologicalSort walks the
                // whole file, so without this check the event graph would re-emit every function
                // body, with FuncEntry trying to Ldarg an argument this method lacks -- bodies are
                // already emitted into their own methods above.
                if (node.FuncOwner != null) continue;
                if (IsExecOnlyNodeType(node.Type)) continue;
                EmitNode(node);
            }

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
                // OUTPUTS ARRAY: multiple OUT records used to compile to void with the computed
                // values stuck in unrecoverable locals -- the graph "ran" but produced nothing
                // readable. Now boxed object[Outputs.Count], one entry per OUT record in file order
                // -- lets multiple outputs (e.g. a drone's x/y/z) come from one compile without
                // triple-computing shared math (which could drift). A typed tuple was rejected:
                // MakeGenericType over ValueTuple's arity would need the same by-name dispatch
                // GetDelegateType already does for Func/Action. GraphHost unboxes by each local's
                // declared CLR type (LocalBuilder.LocalType), i.e. the pin's own type.
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

    /// Topological sort of the nodes (outputs depend on inputs); returns null if there's a cycle.
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

    /// Shared by every Emit* method whose native call takes an out-parameter address (Ldloca, not
    /// Ldloc) -- Raycast's five, GetFieldVec3's three, etc. A missing pin here can't be skipped like
    /// LoadPin's callers skip a missing _pinLocals entry, since there'd be nothing to push where the
    /// call signature requires an address -- thrown here rather than leaving the IL stack unbalanced.
    /// Originally Raycast-only (message hardcoded "Raycast needs all five of ..."); generalised to
    /// name the node's own type/pin once a second caller needed it.
    private LocalBuilder RequirePinLocal(Node node, string pinName)
    {
        if (!_pinLocals.TryGetValue((node.Id, pinName), out var local))
            throw new InvalidOperationException(
                $"{node.Type} node '{node.Id}' has no output pin '{pinName}' declared -- see " +
                $"OcGraphParser.AddDefaultPins's '{node.Type.ToLowerInvariant()}' case for the pins this " +
                "node type needs, or give the node explicit PIN records for all of them");
        return local;
    }

    /// True when an input pin has neither a link into it nor a PIN record -- i.e. both compilers are
    /// about to fall back to a literal zero. Exists for SetControlRig's weight, where zero is
    /// actively harmful: a rig bound at weight 0 loads, validates, resolves its bones, then scales
    /// every op to nothing, indistinguishable from a rig that doesn't fit -- and this only fires for
    /// a hand-authored .ocgraph, exactly the file that gets no UI warning (the editor's node-creation
    /// path always writes weight's "1" default as a PIN record, copying GraphNodeDefs.hpp's
    /// defaultValue). Deliberately not applied to PlayAnimation's loop pin: an unwired zero there
    /// just means "play once", a different clip, not a dead node.
    private bool IsPinUnwired(string nodeId, string pinName) =>
        !_graph.Links.Any(l => l.TargetNodeId == nodeId && l.TargetPinName == pinName) &&
        !_graph.PinnedValues.Any(p => p.NodeId == nodeId && p.PinName == pinName);

    /// Loads a pin value onto the stack: from a linked node's local, else a pinned constant, else a
    /// type default. nodeId is a string to support both integer and arbitrary string node IDs.
    private void LoadPin(string nodeId, string pinName)
    {
        if (_il == null) return;

        var link = _graph.Links.FirstOrDefault(l =>
            l.TargetNodeId == nodeId && l.TargetPinName == pinName
        );

        if (link != null)
        {
            if (_pinLocals.TryGetValue((link.SourceNodeId, link.SourcePinName), out var sourceLocal))
            {
                _il.Emit(OpCodes.Ldloc, sourceLocal);
                return;
            }
        }

        var node = _graph.Nodes[nodeId];
        var pin = node.Pins.FirstOrDefault(p => p.Name == pinName);

        var pv = _graph.PinnedValues.FirstOrDefault(p =>
            p.NodeId == nodeId && p.PinName == pinName
        );

        if (pv != null)
        {
            if (pv.Value is float f)
                _il.Emit(OpCodes.Ldc_R4, f);
            else if (pv.Value is int i)
                // A whole-number PINVAL on a float pin (parser tries int.TryParse before
                // float.TryParse) used to emit ldc.i4 into a float slot -> invalid-program at invoke
                // time ("Common Language Runtime detected an invalid program"); every float pin in
                // the engine had this (0 and 1 are what authors type). The pin's declared type is
                // the authority, so widen to it here.
                if (pin != null && pin.Type == PinType.Float) _il.Emit(OpCodes.Ldc_R4, (float)i);
                else _il.Emit(OpCodes.Ldc_I4, i);
            else if (pv.Value is bool b)
                _il.Emit(OpCodes.Ldc_I4, b ? 1 : 0);
            return;
        }

        // A missing pin is refused (below); merely unconnected still reads zero. Same shape as
        // EmitPullInput's guard in the other compiler, and worse here: `if (pin != null)` used to
        // guard the entire emit, so a missing pin pushed nothing and the following Call silently
        // consumed whatever was beneath it on the stack.
        if (pin == null)
            throw new InvalidOperationException(
                $"node '{nodeId}' ({node.Type}) has no input pin '{pinName}' to read. " +
                $"A node's default pins are suppressed entirely as soon as it declares ANY pin by hand, " +
                $"so if this node has PIN records, it needs one for '{pinName}' too (or a LINK into it)");

        if (pin.Type == PinType.Float)
            _il.Emit(OpCodes.Ldc_R4, 0f);
        else if (pin.Type == PinType.Int)
            _il.Emit(OpCodes.Ldc_I4, 0);
        else if (pin.Type == PinType.Bool)
            _il.Emit(OpCodes.Ldc_I4, 0);
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

    private static readonly MethodInfo GraphCallGuardEnter =
        typeof(GraphCallGuard).GetMethod(nameof(GraphCallGuard.Enter))!;
    private static readonly MethodInfo GraphCallGuardExit =
        typeof(GraphCallGuard).GetMethod(nameof(GraphCallGuard.Exit))!;

    private static Type PinTypeToCLRType(PinType pt) => pt switch
    {
        PinType.Float => typeof(float),
        PinType.Int => typeof(int),
        PinType.Bool => typeof(bool),
        // PinType.Exec falls here (typeof(void)) but should never actually be asked for -- Compile()
        // rejects an exec-typed OUT, EmitNode skips exec pins for output locals, and OcGraphParser
        // refuses an exec-typed PARAM.
        _ => typeof(void)
    };

    // EXEC / PUSH COMPILATION
    //
    // PUSH VS PULL: two compilers in this file. Above this line is the original PULL compiler: a node
    // with no exec edge runs once, whenever the topological pass reaches it, caching its value in
    // `_pinLocals` so every consumer reads the same answer -- correct for a DAG of pure expressions,
    // unaffected by adding exec support: a no-ENTRY graph still produces the exact IL it always did.
    //
    // CompileEntryPoint(), below, PUSHES: a node reached via an exec edge runs at an ordered point in
    // time, possibly more than once (a loop) or never (an untaken branch arm) -- caching in a local
    // would be wrong (a loop's `cond` must be read fresh every pass). So the exec compiler never
    // touches `_pinLocals`/TopologicalSort/LoadPin; its own primitive, EmitPullInput/EmitPullOutput
    // below, is recursive and uncached -- every call re-emits the upstream subgraph's IL on the spot.
    // Two consequences: (1) a shared pure sub-expression pulled from two exec sites/loop iterations is
    // computed twice, correct but not free, with no cross-site memoization -- Phase 1 is about being
    // ABLE to decide, not how cheaply (flagged again in the phase-2 handoff notes); (2) a node with a genuine
    // side effect (SetField, etc) must never be reached by a PULL -- EmitPullOutput refuses to pull a
    // side-effecting node's output, so the write happens only once, when the exec walk visits the node
    // directly (EmitExecSideEffect).
    //
    // No special case for "Sequence": firing exec-out pins is one generic operation (EmitExecFanOut)
    // that follows every exec-typed output pin with a link, in file order. EmitExecNode's switch
    // special-cases only branch (fires exactly one of two) and while/forEach (fire one, N times).

    /// <summary>Compiles ONE declared entry point (`ENTRY &lt;nodeId&gt; &lt;eventName&gt;`) to a
    /// Delegate by walking PUSH/exec edges outward from that node -- see the PUSH VS PULL comment
    /// above for how this differs from Compile()'s PULL compilation, which this method never touches.
    ///
    /// Parameters: the graph's PARAM list in declaration order (as Compile()), plus a trailing
    /// GraphVarStore iff the graph declares a VAR (_varStoreArgIndex), invisible to the public
    /// Fire()/Tick() caller. This is also how OnTick receives delta time: an ordinary `PARAM
    /// deltaTime float` read by a `param` node, not a special-cased rule, so it generalizes to a
    /// future event with different args.
    ///
    /// Return value: same OUT convention as Compile() (void / one CLR type / boxed object[] -- see
    /// its OUTPUTS ARRAY comment), read after the chain finishes. Since EmitPullOutput checks
    /// `_execLocals` first, this can surface a loop/branch/side-effect value the walk itself produced
    /// (a while's `iterations`, a branch's `tookTrue`, a SetField's `success` -- see
    /// OcGraphParser.AddDefaultPins), observable with no live native scene needed.
    ///
    /// Returns null and reports err on: no ENTRY for `eventName`, the ENTRY node missing, an OUT
    /// naming an exec pin, an exec-output pin wired to more than one link, an exec cycle not mediated
    /// by a while/forEach, or an unrecognized node type directly on the exec chain (data-linked nodes
    /// are unaffected).</summary>
    public Delegate? CompileEntryPoint(string eventName, out string? err)
    {
        err = null;

        if (!_graph.Validate(out var validateErr))
        {
            err = validateErr;
            return null;
        }
        if (!ValidateOutputsAreData(out err)) return null;
        if (!EnsureFunctionsCompiled(out err)) return null;   // see Compile() for why this is first

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
            // Graph.Validate() already refuses this at parse time, but a Graph can also be built
            // programmatically rather than through the parser, so this is not unreachable in principle.
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

            // Same additive trailing-parameter rule as Compile() -- see that method's own comment and
            // _varStoreArgIndex's field comment.
            if (_graph.Variables.Count > 0)
            {
                _varStoreArgIndex = paramTypes.Length;
                paramTypes = paramTypes.Append(typeof(GraphVarStore)).ToArray();
            }
            else
            {
                _varStoreArgIndex = -1;
            }

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
            // local the walk populated, like reading a variable after a function body finishes.
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

    /// Declares (once) or returns (on later calls) the IL local backing one exec-scoped pin -- a
    /// loop's live counter, a branch's "which side" flag, a captured SetField return code. Declared
    /// once per compile, but the same slot is written afresh every runtime pass since the IL that
    /// writes it sits inside the loop's branch-back range -- IL loops via jumps, not by re-emitting
    /// the body N times.
    private LocalBuilder GetOrCreateExecLocal(string nodeId, string pinName, Type type)
    {
        if (_il == null) throw new InvalidOperationException("no active ILGenerator");
        var key = (nodeId, pinName);
        if (_execLocals.TryGetValue(key, out var existing)) return existing;
        var local = _il.DeclareLocal(type);
        _execLocals[key] = local;
        return local;
    }
    private static readonly MethodInfo WarnLoopGuardMethod =
        typeof(GraphCompiler).GetMethod(nameof(WarnLoopGuardTripped), BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("GraphCompiler.WarnLoopGuardTripped was not found by reflection");
    // RecordNodeHit: called by EVERY exec node as it runs, so the editor can highlight live control
    // flow. Resolved by reflection here like every other GraphInterop entry point, which also means a
    // renamed or removed method fails at type-init rather than silently never recording.
    private static readonly MethodInfo RecordNodeHitMethod =
        typeof(GraphInterop).GetMethod("RecordNodeHitForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RecordNodeHitForGraph was not found by reflection");
    // The debugger's two hooks (GraphDebug.cs), emitted only when GraphDebug.Armed.
    private static readonly MethodInfo DebugNodeReachedMethod =
        typeof(GraphDebug).GetMethod(nameof(GraphDebug.NodeReached), BindingFlags.Public | BindingFlags.Static)
        ?? throw new InvalidOperationException("GraphDebug.NodeReached was not found by reflection");
    private static readonly MethodInfo DebugRecordPinMethod =
        typeof(GraphDebug).GetMethod(nameof(GraphDebug.RecordPin), BindingFlags.Public | BindingFlags.Static)
        ?? throw new InvalidOperationException("GraphDebug.RecordPin was not found by reflection");

    /// Called from emitted IL (EmitWhile/EmitForEach), not ordinary C# control flow, when a loop's
    /// iteration count crosses MaxLoopIterations. Logs loudly (node + event) and lets `done` run
    /// anyway, as if `cond`/`count` ran out normally, rather than throwing mid-tick -- a graph bug
    /// should be visible, not a crashed frame for whatever else the game was doing. Static/private,
    /// reachable from IL only via CompileEntryPoint's restrictedSkipVisibility:true -- the same
    /// mechanism letting EmitGetField/EmitSetField call Aver.Scene.Native's internal P/Invoke methods.
    private static void WarnLoopGuardTripped(string nodeId, string eventName)
    {
        Console.Error.WriteLine(
            $"[GraphCompiler] loop guard tripped: node '{nodeId}' (event '{eventName}') exceeded " +
            $"{MaxLoopIterations} iterations and was stopped -- this graph has (or was about to have) " +
            "an infinite loop; check its 'cond'/'count' wiring");
    }

    /// Builds the Action/Action&lt;...&gt; or Func&lt;...,TResult&gt; matching paramTypes and
    /// returnType. Generalized over BCL Action`N/Func`N by name (up to 16 type parameters) rather
    /// than hand-listing every arity -- a graph declaring a 5th PARAM needs no hardcoded case here.
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
