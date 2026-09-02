// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
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
public class GraphCompiler
{
    // aver::scene::FieldKind (Fields.hpp) values checked by getfield/setfield (F32 only) and
    // getfieldvec3/setfieldvec3 (Vec3 -- CLocal.position, CLight.colour, etc, Builtins.cpp). Vec3
    // support needed NO new pin type: a Vec3 field reads/writes as three Float pins (x/y/z), same
    // shape as Raycast's multi-output call. Quat (arity 4) and Mat4 (arity 16) stay out of scope;
    // RequireVec3Field's guard (below) stops getfieldvec3/setfieldvec3 from misreading their arity.
    private const int FieldKindF32 = 0;
    private const int FieldKindVec3 = 1;

    private Graph _graph;
    private readonly FieldResolver _fieldResolver;

    // One DynamicMethod per FUNC, created for ALL functions before any body is emitted, so a call can
    // reference a callee whose IL is not written yet -- including itself or one emitted later. A
    // DynamicMethod can be the target of Emit(OpCodes.Call, ...) before its own IL exists.
    private readonly Dictionary<string, DynamicMethod> _funcMethods = new(StringComparer.OrdinalIgnoreCase);
    private bool _functionsCompiled;
    // The function currently being emitted -- null while emitting the event graph. FuncEntry reads
    // it to turn a pin name into an argument index; FuncReturn reads it to find its output locals.
    private GraphFunction? _currentFunc;
    private List<LocalBuilder> _funcOutLocals = new();
    private Dictionary<string, LocalBuilder> _nodeLocals = new();
    private Dictionary<(string, string), LocalBuilder> _pinLocals = new();
    private ILGenerator? _il;

    // Exec/PUSH compilation state -- see the PUSH VS PULL comment above CompileEntryPoint().

    // Hard cap on a single while/forEach's iteration count: stops a bad `cond`/`count` from hanging
    // the caller (a live frame, or this test suite) instead of erroring. PUBLIC so a test can assert
    // against the real number rather than a drifting copy (GraphFlowTests.cs's guard test).
    public const int MaxLoopIterations = 100_000;

    // Loop/branch-local values valid only DURING/AFTER a specific exec run (a loop counter, a
    // branch's taken side, SetField's return code) -- keyed like _pinLocals (nodeId, pinName) but
    // populated by EmitExecNode's control-flow emitters. EmitPullOutput checks this FIRST so e.g.
    // `OUT whileNode iterations` reads the live counter, not a re-derived pure expression.
    private Dictionary<(string, string), LocalBuilder> _execLocals = new();

    // Nodes on the exec walk's OWN call stack right now (not "ever visited" -- see EmitExecNode).
    // Catches a hand-authored exec LINK cycle -- distinct from while/forEach's internal loop-back,
    // which is not a graph link (see EmitWhile/EmitForEach) -- at COMPILE time, not via stack overflow.
    private HashSet<string> _execVisiting = new();
    // Same guard, for PULL recursion: EmitPullOutput recurses through EmitPullInput, and a hand-
    // authored .ocgraph can wire two data nodes into each other (add.a <- multiply.result,
    // multiply.a <- add.result) -- nothing upstream rejects it (no parser/Validate() cycle check),
    // and _execVisiting never sees these nodes since they're reached by PULL, not PUSH. Unbounded
    // recursion here is StackOverflowException, which .NET makes UNCATCHABLE -- it kills the whole
    // host process (editor included), worse than any wrong number this compiler could produce, and
    // hand-authored .ocgraph is normal input here, not exotic.
    //
    // Keyed on node id, not (id, pin): re-entering a node by ANY pin on the current path is a cycle.
    // A diamond (two consumers pulling the same node) is unaffected -- the try/finally below clears
    // the node once its own evaluation completes.
    private HashSet<string> _pullVisiting = new();

    // The event name CompileEntryPoint() is currently compiling, purely so a loop-guard warning
    // emitted from deep inside EmitWhile/EmitForEach can name which entry point misbehaved.
    private string _currentEventName = "";

    // Graph-local persistent variables (VAR / GraphVarStore).

    // Index of the trailing GraphVarStore parameter, or -1 if this graph declares no VAR records
    // (the common case) -- additive, not breaking: paramTypes only grows when Variables.Count > 0, so
    // a VAR-less graph's delegate shape is unchanged. Set once per Compile()/CompileEntryPoint() call
    // before any node is emitted; EmitGetVar/EmitPullGetVar/EmitExecSetVar are the only readers.
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

        // FUNCTIONS BEFORE ANYTHING ELSE: emitting one takes over _il and every local map this class
        // owns (see EnsureFunctionsCompiled). Doing it before this method's own DynamicMethod exists
        // avoids a save/restore dance at every call site.
        if (!EnsureFunctionsCompiled(out err)) return null;

        // An OUT record can't name an exec pin -- control flow has nothing to "hand back". Checked
        // explicitly here rather than letting PinTypeToCLRType's typeof(void) reach
        // DeclareLocal(typeof(void)) and throw a confusing runtime ArgumentException: Validate()
        // doesn't check pin TYPES for Outputs (Graph.cs), so this is the first point that can catch
        // it. Shared with CompileEntryPoint(), which faces the identical question.
        if (!ValidateOutputsAreData(out err)) return null;

        try
        {
            // Return type: zero outputs -> void; one output -> that pin's CLR type (keeps every
            // existing single-output graph's Func<T> shape); 2+ outputs -> object[], one boxed entry
            // per Outputs record in file order -- see the OUTPUTS ARRAY comment below for why object[]
            // rather than a second delegate convention.
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

            // Append a trailing GraphVarStore param iff this graph declares >=1 VAR (see
            // _varStoreArgIndex) -- kept separate from PARAM because PARAM is caller-supplied and VAR
            // is graph-owned; merging the two slots would erase that distinction.
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

            // Emit each node, SKIPPING ones this compiler has no business touching. OcGraph.hpp says a
            // graph may be pure dataflow, pure exec, or both, with Compile() "driven entirely by
            // `outputs`" -- but it used to emit EVERY node, so a graph with both halves died on
            // `Node type 'OnTick' is not supported`. Found when the cross-implementation fixture grew
            // an exec chain. Skips by "is this node exec-capable" rather than by reachability from OUT,
            // so a pure DATA node that's genuinely unreachable still gets emitted and still reports its
            // own errors -- a typo in an unused subgraph isn't silently swallowed.
            foreach (var node in sortedNodes)
            {
                // A node inside a FUNCTION is not part of this method. TopologicalSort walks every
                // node in the file, so without this the event graph would re-emit every function
                // body too, with its FuncEntry trying to Ldarg an argument this method lacks --
                // bodies are already emitted into their own methods above.
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
                // values stuck in unrecoverable DynamicMethod locals -- the graph "ran" but produced
                // nothing readable. A drone's flight path needs x/y/z from ONE compile (three
                // single-output graphs would triple-compute the shared math and could drift), so this
                // builds boxed object[Outputs.Count], one entry per OUT record in file order. A typed
                // tuple was rejected: MakeGenericType over ValueTuple's arity needs the same by-NAME
                // dispatch GetDelegateType already does for Func/Action. GraphHost unboxes by each
                // local's declared CLR type (LocalBuilder.LocalType), i.e. the pin's own type.
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

        // Create locals for all DATA output pins. Exec-typed output pins are skipped --
        // PinTypeToCLRType(PinType.Exec) is typeof(void) and DeclareLocal(typeof(void)) throws. A
        // pure-dataflow node never legitimately has one, but TopologicalSort() walks every node
        // regardless of type, so this guards a flow node (branch/while/forEach) reaching Compile()
        // (the old PULL-only path) instead of CompileEntryPoint() -- the switch below still throws
        // NotSupportedException for it either way; this just keeps that the FIRST failure instead of
        // an unrelated ArgumentException from the runtime.
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

            // A pure CallFunc in the EVENT graph of a pure-dataflow file, via EmitSimpleApiRead's
            // "call, store results in pin locals" shape; the impure case never reaches here since
            // Compile() has no exec walk.
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
            case "hastag":
            case "gettags":
            case "getplayerpawn":
            case "getplayercontroller":
            case "getgamemode":
            case "isplaying":
                EmitSimpleApiRead(node);
                break;

            case "setfieldvec3":
                // Like "setfield" above: Compile()'s topological pass visits every non-exec-only node
                // exactly once, so this is a single deterministic write per invocation, not the
                // double-write EmitPullOutput's refusal (below) guards against (a different risk --
                // its own recursive pull mechanism). See EmitSetField's "dual-reachable" reasoning.
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
                // Side-effecting (creates a new entity) -- see IsExecCapableSpawnType. REFUSED
                // explicitly rather than skipped via IsExecOnlyNodeType (no signal) or run
                // unconditionally like SetField/SetFieldVec3 (safe for them: overwriting a field
                // twice is harmless). Spawn has no such pass -- ticked every frame with no branch to
                // gate it, a stray Spawn would create a new entity every tick.
                throw new InvalidOperationException(
                    $"Spawn node '{node.Id}' cannot be compiled by Compile() -- spawning an entity is a " +
                    "side effect with no notion of 'when' in a pure-dataflow graph, and Compile()'s " +
                    "topological pass would run it unconditionally on every invocation with no way to " +
                    "gate it. Give this node an ENTRY-driven exec chain and reach it through " +
                    "CompileEntryPoint() instead.");

            case "charactermove":
                // Side-effecting: drives a real actor (yaw/pitch/capsule velocity via
                // AverCharacter.Drive; see IsExecCapableCharacterMoveType). Same reason as "spawn"
                // above -- must run only when the exec chain reaches it, not per arbitrary pull.
                throw new InvalidOperationException(
                    $"CharacterMove node '{node.Id}' cannot be compiled by Compile() -- driving a " +
                    "character is a side effect with no notion of 'when' in a pure-dataflow graph, " +
                    "and Compile()'s topological pass would run it unconditionally on every " +
                    "invocation with no way to gate it. Give this node an ENTRY-driven exec chain " +
                    "and reach it through CompileEntryPoint() instead.");

            case "fireevent":
                // Side-effecting: runs ANOTHER ENTITY'S WHOLE EXEC CHAIN, not a scalar write (see
                // IsExecCapableFireEventType). Worse than spawn/charactermove: an ungated FireEvent
                // would run a STRANGER's OnHit handler on every pull.
                throw new InvalidOperationException(
                    $"FireEvent node '{node.Id}' cannot be compiled by Compile() -- firing an event " +
                    "runs another entity's exec chain and is a side effect with no notion of 'when' " +
                    "in a pure-dataflow graph, and Compile()'s topological pass would run it " +
                    "unconditionally on every invocation with no way to gate it. Give this node an " +
                    "ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.");

            case "savegame":
            case "loadgame":
                // Side-effecting -- writes or REPLACES THE ENTIRE WORLD. LoadGame is worst case: it
                // tears down and rebuilds EVERYTHING, including whatever entity's graph pulled it.
                throw new InvalidOperationException(
                    $"{(node.Type.Equals("savegame", StringComparison.OrdinalIgnoreCase) ? "SaveGame" : "LoadGame")} " +
                    $"node '{node.Id}' cannot be compiled by Compile() -- it is a side effect with no " +
                    "notion of 'when' in a pure-dataflow graph, and Compile()'s topological pass would " +
                    "run it unconditionally on every invocation with no way to gate it. Give this node " +
                    "an ENTRY-driven exec chain and reach it through CompileEntryPoint() instead.");

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

    /// One output component of a vector node, onto the stack.
    ///
    /// RECOMPUTED PER OUTPUT rather than hoisted into shared locals, deliberately -- both compilers
    /// already re-emit a pure expression at every read (that's what EmitPullOutput IS), and Normalize
    /// (worst case, a sqrt per component) is still just a handful of float ops.
    ///
    /// Dup-then-Mul squares so a squared term pulls its input ONCE -- pulling twice would emit a
    /// subgraph input twice.
    private void EmitVecComponent(Node node, string pinName)
    {
        if (_il == null) return;

        void In(string p) => EmitPullInput(node, p);
        void Sq(string p) { In(p); _il.Emit(OpCodes.Dup); _il.Emit(OpCodes.Mul); }
        void DiffSq(string a, string b) { In(a); In(b); _il.Emit(OpCodes.Sub); _il.Emit(OpCodes.Dup); _il.Emit(OpCodes.Mul); }
        void Sqrt() { _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathSqrtMethod); _il.Emit(OpCodes.Conv_R4); }
        void Len() { Sq("ax"); Sq("ay"); _il.Emit(OpCodes.Add); Sq("az"); _il.Emit(OpCodes.Add); Sqrt(); }

        string other = pinName == "x" ? "ax" : pinName == "y" ? "ay" : "az";
        string bOther = pinName == "x" ? "bx" : pinName == "y" ? "by" : "bz";

        switch (node.Type.ToLowerInvariant())
        {
            case "vecadd": In(other); In(bOther); _il.Emit(OpCodes.Add); return;
            case "vecsub": In(other); In(bOther); _il.Emit(OpCodes.Sub); return;
            case "vecscale": In(other); In("s"); _il.Emit(OpCodes.Mul); return;

            // x = ay*bz - az*by, and the two cyclic rotations of it.
            case "veccross":
                if (pinName == "x") { In("ay"); In("bz"); _il.Emit(OpCodes.Mul); In("az"); In("by"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Sub); }
                else if (pinName == "y") { In("az"); In("bx"); _il.Emit(OpCodes.Mul); In("ax"); In("bz"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Sub); }
                else { In("ax"); In("by"); _il.Emit(OpCodes.Mul); In("ay"); In("bx"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Sub); }
                return;

            // Divided by max(length, 1e-6): a zero vector normalises to zero rather than to NaN,
            // and a NaN here would propagate into a transform and take the actor with it.
            case "vecnormalize":
                In(other); Len(); _il.Emit(OpCodes.Ldc_R4, 1e-6f); _il.Emit(OpCodes.Call, MathMaxMethod);
                _il.Emit(OpCodes.Div); return;

            // a + (b - a) * t, which is exact at t = 0 and t = 1 -- unlike a*(1-t) + b*t.
            case "veclerp":
                In(other); In(bOther); In(other); _il.Emit(OpCodes.Sub); In("t"); _il.Emit(OpCodes.Mul);
                _il.Emit(OpCodes.Add); return;

            case "vecdot":
                In("ax"); In("bx"); _il.Emit(OpCodes.Mul);
                In("ay"); In("by"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Add);
                In("az"); In("bz"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Add); return;

            case "veclength": Len(); return;

            case "vecdistance":
                DiffSq("ax", "bx"); DiffSq("ay", "by"); _il.Emit(OpCodes.Add);
                DiffSq("az", "bz"); _il.Emit(OpCodes.Add); Sqrt(); return;

            default:
                throw new InvalidOperationException(
                    $"vector node '{node.Id}' has type {node.Type}, which EmitVecComponent does not know");
        }
    }

    /// The PULL compiler's topological visit of a vector node: every output with a declared local
    /// gets computed and stored; an unread output has no local, so an unread Cross costs nothing.
    private void EmitVecNode(Node node)
    {
        if (_il == null) return;
        foreach (var p in node.Pins)
        {
            if (!p.IsOutput) continue;
            if (!_pinLocals.TryGetValue((node.Id, p.Name), out var local)) continue;
            EmitVecComponent(node, p.Name);
            _il.Emit(OpCodes.Stloc, local);
        }
    }

    private void EmitAdd(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Add);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitMultiply(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Mul);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitCompare(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Cgt);
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

        // Calls the same P/Invoke extern Aver.Scene's C# consumers use (Native.cs), not a second
        // DllImport surface. Needs InternalsVisibleTo("Aver.Graph") from Aver.Scene.csproj to name the
        // internal Native type at compile time; DynamicMethod's restrictedSkipVisibility:true (set in
        // Compile()) is what lets the EMITTED IL actually call it.
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

    /// Shared by all 4 getfieldvec3/setfieldvec3 call sites. Requires FieldKindVec3 SPECIFICALLY, not
    /// just "not F32": GraphInterop.GetFieldVecForGraph/SetFieldVecForGraph (Aver.Framework) copy
    /// exactly 3 floats through a fixed scratch buffer, so a Quat (arity 4) or Mat4 (arity 16) field
    /// would be a buffer overrun, not a caught type error. Centralised rather than duplicated 4x.
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
    /// often as wanted through either compiler -- but three separate x/y/z pulls cost three native
    /// calls, not one (see EmitPullGetFieldVec3). Reads back 0,0,0 on runtime rejection (unknown
    /// entity/component), mirroring GetField's convention rather than adding a "found" pin; wrong
    /// field/kind is a hard COMPILE error via RequireVec3Field.
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

    /// GetForward(entity) -> x,y,z + eyeX,eyeY,eyeZ + success: look direction and eye position in one
    /// call. Pure/idempotent like GetFieldVec3, so no _execLocals caching needed. Six out-parameters,
    /// one native call (same shape as EmitGetFieldVec3/EmitRaycast): Ldloca per output, never Ldloc,
    /// since the callee writes THROUGH them.
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

        // The bool return is ALWAYS consumed -- stored when the node declares `success`, popped when an
        // explicit PIN list left it out. Leaving it on the stack would unbalance the method.
        if (_pinLocals.TryGetValue((node.Id, "success"), out var local)) _il.Emit(OpCodes.Stloc, local);
        else                                                            _il.Emit(OpCodes.Pop);
    }

    /// The PULL half of GetForward, mirroring EmitPullGetFieldVec3: one native call, then push the
    /// pin asked for and discard the rest -- reading x, y, eyeZ separately costs three calls, an
    /// acceptable trade since this only reads two already-computed vectors off a managed object.
    /// Exists because a node implemented only in the PUSH compiler silently fails the moment
    /// something reads it through OUT or a pure graph -- the recurring bug shape in this file. Both
    /// paths, or neither.
    /// Jump(entity) -> jumped: a side effect, refused when pulled as data (IsExecCapableSideEffectType).
    /// The bool is stored when the node declares `jumped`, popped otherwise, to balance the stack.
    private void EmitJump(Node node)
    {
        if (_il == null) return;
        // EmitPullInput, NOT LoadPin -- the difference is the whole of this node working or not.
        // LoadPin reads `_pinLocals`, which the EXEC compiler never populates for data nodes (see
        // EmitPullInput's own comment below). Every other exec-path emitter (CharacterMove, SetParent,
        // Spawn, FireEvent, SetVar...) pulls its inputs; this one read an unset local and pushed zero,
        // so Jump received entity 0 on every call, found no actor, warned, and returned false -- it
        // has never once made a character jump.
        //
        // Looked like a once-a-run glitch rather than a dead node because nothing in the shipped
        // FirstPerson template presses jump, and --play-test only synthesises Space from frame 100
        // (SandboxApp.cpp). Read the warning as "Jump is broken", not "the first frame is odd".
        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Call, JumpMethod);
        if (_pinLocals.TryGetValue((node.Id, "jumped"), out var local)) _il.Emit(OpCodes.Stloc, local);
        else                                                           _il.Emit(OpCodes.Pop);
    }

    /// Print(value) -> then: writes one line to the log labelled with the NODE'S OWN ID (no attribute
    /// needed), e.g. `NODE muzzleLen Print` prints "muzzleLen = 35". Pushed as a compile-time constant.
    /// The four writing API calls differ only in which method they call and what they push; each
    /// bool return is stored into `success` when declared, popped otherwise (the EmitJump shape).
    /// A three-out-parameter read of an entity for whichever component was asked for; `axis` is
    /// pushed only when not -1, letting the three orientation nodes share one interop surface with
    /// the two that take no extra argument.
    private void EmitPullVec3Read(Node node, string pinName, MethodInfo method, int axis,
                                  string inputPin = "entity")
    {
        if (_il == null) return;
        EmitPullInput(node, inputPin);
        if (axis >= 0) _il.Emit(OpCodes.Ldc_I4, axis);
        var xL = _il.DeclareLocal(typeof(float));
        var yL = _il.DeclareLocal(typeof(float));
        var zL = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, xL);
        _il.Emit(OpCodes.Ldloca, yL);
        _il.Emit(OpCodes.Ldloca, zL);
        _il.Emit(OpCodes.Call, method);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        _il.Emit(OpCodes.Ldloc, pinName == "x" ? xL : pinName == "y" ? yL : zL);
    }

    /// EmitPullVec3Read's shape narrowed to ONE out-parameter: GetBodyMass/GetBodyMotionType/
    /// GetBodyLayer/GetJointValue each call `bool Method(int handle, out T value)`. `success` is the
    /// bool return; the one remaining pin needs no x/y/z-style dispatch.
    private void EmitPullScalarRead(Node node, string pinName, MethodInfo method, Type outType, string inputPin)
    {
        if (_il == null) return;
        EmitPullInput(node, inputPin);
        var outL = _il.DeclareLocal(outType);
        _il.Emit(OpCodes.Ldloca, outL);
        _il.Emit(OpCodes.Call, method);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        _il.Emit(OpCodes.Ldloc, outL);
    }

    /// SynapseSteer's own emission -- not EmitPullVec3Read, which only produces three floats + bool:
    /// this node has four real outputs (forward, right, yawDelta, arrived) plus the method's own
    /// bool return ("success", same "entity was not alive" meaning as elsewhere). Same overall shape
    /// otherwise: push inputs, push one local address per out-parameter, Call, then either leave the
    /// bool on the stack (pinName == "success") or pop it and push the requested local.
    private void EmitPullSynapseSteer(Node node, string pinName)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        EmitPullInput(node, "dt");
        EmitPullInput(node, "targetX");
        EmitPullInput(node, "targetY");
        EmitPullInput(node, "targetZ");
        EmitPullInput(node, "turnRate");
        EmitPullInput(node, "arriveRadius");
        var forwardL = _il.DeclareLocal(typeof(float));
        var rightL = _il.DeclareLocal(typeof(float));
        var yawDeltaL = _il.DeclareLocal(typeof(float));
        var arrivedL = _il.DeclareLocal(typeof(bool));
        _il.Emit(OpCodes.Ldloca, forwardL);
        _il.Emit(OpCodes.Ldloca, rightL);
        _il.Emit(OpCodes.Ldloca, yawDeltaL);
        _il.Emit(OpCodes.Ldloca, arrivedL);
        _il.Emit(OpCodes.Call, SynapseSteerMethod);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        switch (pinName)
        {
            case "forward":  _il.Emit(OpCodes.Ldloc, forwardL);  break;
            case "right":    _il.Emit(OpCodes.Ldloc, rightL);    break;
            case "yawDelta": _il.Emit(OpCodes.Ldloc, yawDeltaL); break;
            case "arrived":  _il.Emit(OpCodes.Ldloc, arrivedL);  break;
        }
    }

    /// GetSynapsePerception's emission -- same shape as EmitPullSynapseSteer (three real outputs of
    /// MIXED type + bool return), since EmitPullVec3Read only produces float/float/float, not
    /// bool/int/float.
    private void EmitPullSynapsePerception(Node node, string pinName)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        var canSeeL = _il.DeclareLocal(typeof(bool));
        var lastTargetL = _il.DeclareLocal(typeof(int));
        var timeSinceSeenL = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, canSeeL);
        _il.Emit(OpCodes.Ldloca, lastTargetL);
        _il.Emit(OpCodes.Ldloca, timeSinceSeenL);
        _il.Emit(OpCodes.Call, SynapseGetPerceptionMethod);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        switch (pinName)
        {
            case "canSeeTarget":    _il.Emit(OpCodes.Ldloc, canSeeL);         break;
            case "lastKnownTarget": _il.Emit(OpCodes.Ldloc, lastTargetL);     break;
            case "timeSinceSeen":   _il.Emit(OpCodes.Ldloc, timeSinceSeenL);  break;
        }
    }

    /// The three transform writers: same shape as EmitExecApiCall -- push, call, keep or drop the bool.
    /// The physics writers: push the body (none for SetGravity), push the vector, call, keep or drop.
    private void EmitExecPhysicsWrite(Node node)
    {
        if (_il == null) return;
        string t = node.Type.ToLowerInvariant();
        if (t != "setgravity") EmitPullInput(node, "body");
        switch (t)
        {
            case "setbodyposition":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetBodyPositionMethod); break;
            case "setbodyvelocity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetBodyVelocityMethod); break;
            case "addbodyvelocity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddBodyVelocityMethod); break;
            // Forces and impulses -- same "pull body, pull x/y/z, call" shape as addbodyvelocity
            // just above; only which native call gets made differs.
            case "addforce":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddForceMethod); break;
            case "addimpulse":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddImpulseMethod); break;
            case "addtorque":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddTorqueMethod); break;
            case "addangularimpulse":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddAngularImpulseMethod); break;
            case "setbodyangularvelocity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetBodyAngularVelocityMethod); break;
            case "setbodyentity":
                EmitPullInput(node, "entity");
                _il.Emit(OpCodes.Call, SetBodyEntityMethod); break;
            // Material, mass, motion type, layer -- one scalar pin each after body.
            case "setbodyfriction":
                EmitPullInput(node, "friction");
                _il.Emit(OpCodes.Call, SetBodyFrictionMethod); break;
            case "setbodyrestitution":
                EmitPullInput(node, "restitution");
                _il.Emit(OpCodes.Call, SetBodyRestitutionMethod); break;
            case "setbodygravityfactor":
                EmitPullInput(node, "factor");
                _il.Emit(OpCodes.Call, SetBodyGravityFactorMethod); break;
            case "setbodymass":
                EmitPullInput(node, "mass");
                _il.Emit(OpCodes.Call, SetBodyMassMethod); break;
            case "setbodymotiontype":
                EmitPullInput(node, "motionType");
                _il.Emit(OpCodes.Call, SetBodyMotionTypeMethod); break;
            case "activatebody":
                _il.Emit(OpCodes.Call, ActivateBodyMethod); break;
            case "setbodylayer":
                EmitPullInput(node, "layer");
                _il.Emit(OpCodes.Call, SetBodyLayerMethod); break;
            case "setgravity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetGravityMethod); break;
            default:
                _il.Emit(OpCodes.Call, DestroyBodyMethod); break;
        }
        // INTO AN EXEC LOCAL, NOT _pinLocals (see EmitExecSideEffect) -- this emitter runs only from
        // exec dispatch, where _pinLocals is empty, so this lookup never hit and `success` on every
        // physics writer fell to Pop: a failed write (stale handle) looked identical to a working one.
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// The creators: each returns a BODY HANDLE into an exec-local, not a pin local -- the body pin
    /// is read AFTER this node runs by whatever the exec chain reaches next, which is exactly what
    /// _execLocals guarantees (one creation, many readers). A pin local would be wrong here.
    private void EmitExecPhysicsCreate(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "cx"); EmitPullInput(node, "cy"); EmitPullInput(node, "cz");
        switch (node.Type.ToLowerInvariant())
        {
            case "addstaticbox":
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                _il.Emit(OpCodes.Call, AddStaticBoxMethod); break;
            case "adddynamicbox":
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                EmitPullInput(node, "mass");
                _il.Emit(OpCodes.Call, AddDynamicBoxMethod); break;
            case "adddynamicsphere":
                EmitPullInput(node, "radius"); EmitPullInput(node, "mass");
                _il.Emit(OpCodes.Call, AddDynamicSphereMethod); break;
            case "addsensorbox":
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                _il.Emit(OpCodes.Call, AddSensorBoxMethod); break;
            default:
                EmitPullInput(node, "radius");
                _il.Emit(OpCodes.Call, AddSensorSphereMethod); break;
        }
        _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "body", typeof(int)));
    }

    /// The joint creators: bodyA/bodyB first (bodyB == 0 means "the world", not "dead" -- see
    /// GraphNodeDefs.hpp's JOINTS banner), then shape-specific floats, then a call whose return goes
    /// into exec-local "joint" instead of "body" -- same EmitExecPhysicsCreate reasoning otherwise.
    private void EmitExecJointCreate(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "bodyA"); EmitPullInput(node, "bodyB");
        switch (node.Type.ToLowerInvariant())
        {
            case "jointfixed":
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                EmitPullInput(node, "axX"); EmitPullInput(node, "axY"); EmitPullInput(node, "axZ");
                EmitPullInput(node, "ayX"); EmitPullInput(node, "ayY"); EmitPullInput(node, "ayZ");
                _il.Emit(OpCodes.Call, JointFixedMethod); break;
            case "jointpoint":
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                _il.Emit(OpCodes.Call, JointPointMethod); break;
            case "jointdistance":
                EmitPullInput(node, "paX"); EmitPullInput(node, "paY"); EmitPullInput(node, "paZ");
                EmitPullInput(node, "pbX"); EmitPullInput(node, "pbY"); EmitPullInput(node, "pbZ");
                EmitPullInput(node, "minDist"); EmitPullInput(node, "maxDist");
                _il.Emit(OpCodes.Call, JointDistanceMethod); break;
            case "jointslider":
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                EmitPullInput(node, "sx"); EmitPullInput(node, "sy"); EmitPullInput(node, "sz");
                EmitPullInput(node, "nx"); EmitPullInput(node, "ny"); EmitPullInput(node, "nz");
                EmitPullInput(node, "minCm"); EmitPullInput(node, "maxCm");
                _il.Emit(OpCodes.Call, JointSliderMethod); break;
            default: // jointhinge
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                EmitPullInput(node, "nx"); EmitPullInput(node, "ny"); EmitPullInput(node, "nz");
                EmitPullInput(node, "minAngleRad"); EmitPullInput(node, "maxAngleRad");
                _il.Emit(OpCodes.Call, JointHingeMethod); break;
        }
        _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "joint", typeof(int)));
    }

    /// The joint operations: a JOINT handle (never a body) pulled first, then op-specific args, then
    /// the same "success into an exec local" tail as EmitExecPhysicsWrite/EmitExecTransformWrite/
    /// EmitExecApiCall (see EmitExecPhysicsWrite for why exec local, not pin local).
    private void EmitExecJointOp(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "joint");
        switch (node.Type.ToLowerInvariant())
        {
            case "jointsetmotor":
                EmitPullInput(node, "state"); EmitPullInput(node, "target");
                _il.Emit(OpCodes.Call, JointSetMotorMethod); break;
            case "jointsetenabled":
                EmitPullInput(node, "enabled");
                _il.Emit(OpCodes.Call, JointSetEnabledMethod); break;
            default: // jointremove
                _il.Emit(OpCodes.Call, JointRemoveMethod); break;
        }
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// SphereCast, shaped exactly like EmitExecRaycast: one call, N results into N exec-locals, so
    /// a sweep costs the same whether one output pin is read or six.
    private void EmitExecSphereCast(Node node)
    {
        if (_il == null) return;
        foreach (string a in new[] { "originX", "originY", "originZ", "dirX", "dirY", "dirZ", "maxDist", "radius" })
            EmitPullInput(node, a);
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "hit", typeof(bool)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "body", typeof(int)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointX", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointY", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointZ", typeof(float)));
        _il.Emit(OpCodes.Call, SphereCastMethod);
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
            default:
                _il.Emit(OpCodes.Call, DestroyEntityMethod); break;
        }
        // INTO AN EXEC LOCAL, NOT _pinLocals (see EmitExecSideEffect, which fixed this mistake
        // first) -- identical to the physics writer above. Translate/SetLocalScale/DestroyEntity
        // each return a real bool that no graph could see.
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    private void EmitExecApiCall(Node node)
    {
        if (_il == null) return;
        switch (node.Type.ToLowerInvariant())
        {
            case "setvelocity":
                EmitPullInput(node, "entity"); EmitPullInput(node, "x");
                EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetVelocityMethod); break;
            case "teleport":
                EmitPullInput(node, "entity"); EmitPullInput(node, "x");
                EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, TeleportMethod); break;
            case "possess":
                EmitPullInput(node, "controller"); EmitPullInput(node, "pawn");
                _il.Emit(OpCodes.Call, PossessMethod); break;
            case "setvisible":
                EmitPullInput(node, "entity"); EmitPullInput(node, "visible");
                _il.Emit(OpCodes.Call, SetVisibleMethod); break;
            case "addtag":
                EmitPullInput(node, "entity"); EmitPullInput(node, "mask");
                _il.Emit(OpCodes.Call, AddTagMethod); break;
            case "removetag":
                EmitPullInput(node, "entity"); EmitPullInput(node, "mask");
                _il.Emit(OpCodes.Call, RemoveTagMethod); break;
            // SetLayerCollision has no "body"/"entity" prefix -- it edits the world's shared layer
            // matrix, which is why it sits here rather than in EmitExecPhysicsWrite (body-first shape).
            case "setlayercollision":
                EmitPullInput(node, "layerA"); EmitPullInput(node, "layerB"); EmitPullInput(node, "collide");
                _il.Emit(OpCodes.Call, SetLayerCollisionMethod); break;
            default:
                EmitPullInput(node, "controller");
                _il.Emit(OpCodes.Call, UnpossessMethod); break;
        }
        // INTO AN EXEC LOCAL, NOT _pinLocals -- a fix, not a preference. _pinLocals is empty while
        // CompileEntryPoint runs, so every one of these nodes fell to Pop and `success` was unreadable;
        // wiring into the exec chain didn't help either, since EmitPullOutput checks _execLocals FIRST.
        // `success` is now readable for SetVelocity, Teleport, Possess, Unpossess, SetVisible, AddTag,
        // RemoveTag -- for Teleport (false on a capsule-less character) that's the difference between
        // noticing a failure and silently continuing.
        //
        // CORRECTION: this note used to name ten emitters with the bug. Eight don't have it --
        // EmitSetField/SetParent/SetName/SetMesh/SetMaterial/SetFieldVec3/GetForward/GetViewEntity run
        // only from EmitNode's PULL switch (_pinLocals is right there); only EmitExecPhysicsWrite and
        // EmitExecTransformWrite really had it, and both are fixed the same way. Nothing outstanding.
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// GetVelocity(entity) -> x, y, z + success. Three out-parameters wide, the same shape
    /// EmitPullGetForward uses and for the same reason: one native call, several pins.
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

    /// The single-output API reads: at most one input pin, one call, one output local.
    private void EmitSimpleApiRead(Node node)
    {
        if (_il == null) return;
        foreach (var p in node.Pins)
        {
            if (!p.IsOutput) continue;
            if (!_pinLocals.TryGetValue((node.Id, p.Name), out var local)) continue;
            EmitPullOutput(node, p.Name);
            _il.Emit(OpCodes.Stloc, local);
        }
    }

    private void EmitExecPrint(Node node)
    {
        if (_il == null) return;
        _il.Emit(OpCodes.Ldstr, node.Id);
        EmitPullInput(node, "value");
        _il.Emit(OpCodes.Call,
                 node.Type.Equals("printint", StringComparison.OrdinalIgnoreCase) ? PrintIntMethod : PrintMethod);
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

    /// SetFieldVec3(entity, x, y, z) -> success: mirrors EmitSetField, including getting NO exec pins
    /// by default (OcGraphParser.AddDefaultPins's "setfieldvec3" case) -- an author wanting it on the
    /// exec chain adds explicit PIN records, same convention as SetField. aver_scene_set_vec's return
    /// (1 success, 0 on any rejection) reaches "success" if declared, like SetField's.
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

    /// SetParent(child, parent) -> success: reparents `child` under `parent` (0 makes it a root) --
    /// wraps aver_scene_set_parent(int32,int32) directly (scene_abi.h:105), same
    /// reflect-straight-into-Aver.Scene.Native template as GetField/SetField, no GraphInterop wrapper
    /// needed (unlike Raycast/Spawn). The ABI ALREADY refuses a cycle, self-parent, or doomed parent --
    /// returning 0, not throwing -- so the real return code reaches "success" (same fix as SetField's
    /// old hardcoded-1 stub): wiring a cycle gets a live false, not a silent no-op. See
    /// EmitExecSetParent for the PUSH twin and EmitNode's "setparent" case for why both compilers reach this.
    private void EmitSetParent(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "child");
        LoadPin(node.Id, "parent");
        _il.Emit(OpCodes.Call, SetParentMethod);

        if (_pinLocals.TryGetValue((node.Id, "success"), out var local))
            _il.Emit(OpCodes.Stloc, local);
        else
            _il.Emit(OpCodes.Pop); // nothing declared to read the return code; discard it.
    }

    /// SetViewEntity(entity) -> (nothing): publishes which entity the camera follows -- wraps
    /// aver_fw_set_view_entity(int32) -> void directly (framework_abi.h:206), reflected like
    /// EmitInputKey. VOID means exactly that: no output pin exists at all (see
    /// OcGraphParser.AddDefaultPins's "setviewentity" case), so nothing to Stloc or Pop -- the call is
    /// the entire effect.
    private void EmitSetViewEntity(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "entity");
        _il.Emit(OpCodes.Call, SetViewEntityMethod);
    }

    /// SetName(entity, name) -> success: writes the entity's name -- wraps aver_scene_set_name
    /// (int32,const char*)->int32 directly (scene_abi.h:113). `name` is a NODE-line attribute
    /// (Node.NameValue, from name=), not a pin: PinType has no String member (Graph.cs), so a
    /// NODE-line attribute is the only route a literal string reaches a node, same as class= for
    /// Spawn. Required at COMPILE time, the same "fail loudly, not silently at runtime" rule
    /// GetField/SetField/Spawn apply to field=/class=.
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

    /// SetMesh(entity) -> success: sets the drawn mesh by asset path -- wraps
    /// GraphInterop.SetMeshForGraph(int,string), which composes EnsureMeshRenderer() +
    /// Assets.ObjectIdOf(path) + SetInt64 (Entity.SetMesh). `mesh` is a NODE-line attribute
    /// (Node.MeshPath), same "literal string can only reach a node this way" reasoning as EmitSetName.
    /// Required at compile time for the same reason field=/class=/name= all are.
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
    /// current playhead. Wraps GraphInterop.GetAnimCurveForGraph(int,string).
    ///
    /// PURE, so no exec twin and no IsExecCapable predicate: it reads and writes nothing, so running
    /// it once per invocation is exactly right, and it's absent from IsPushOnlySideEffectType too.
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
    /// Wraps GraphInterop.AttachToSocketForGraph(int,int,string).
    ///
    /// TWO PIN LOADS, in argument order, which is the one thing that makes this different from every
    /// other Set*-shaped emitter here: the IL stack must carry entity then parent then the socket
    /// string, and swapping the two entity pins compiles perfectly and attaches the rig to the gun.
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

    /// PlayAnimation(entity, loop) -> success: mirrors EmitSetMesh but with a SECOND pin load (loop)
    /// before the call, since GraphInterop.PlayAnimationForGraph takes (int,string,bool). Stack order
    /// matches the method's params: entity, clip, loop.
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

    private void EmitSin(Node node)
    {
        if (_il == null) return;

        // System.Math.Sin takes/returns double; the graph is float end to end, so an explicit
        // widen/narrow is required. Skipping either still compiles (the IL verifier can accept a bare
        // double where a float local was declared) but silently reinterprets bits -- wrong numbers,
        // no error.
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

        // DIVIDE-BY-ZERO CONVENTION: b == 0.0 yields 0.0, not IEEE754 NaN/Infinity. A bare `div`
        // never throws (5/0 = +Inf, 0/0 = NaN), and any of those reaching a transform is hard to
        // trace back -- it propagates silently and shows up frames later as an object that vanished
        // or exploded. 0.0 is inert and a flight path can continue through it; NaN cannot. Deliberate
        // choice, not the IEEE default left alone -- flag if a consumer ever needs propagating NaN.
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

        // Graph.Validate() (top of Compile(), and the parser) already checked param= names a declared
        // PARAM with a matching type; this repeats the lookup defensively, the same pattern LoadPin
        // follows for its own node/pin lookups below.
        int index = _graph.Parameters.FindIndex(p => p.Name == node.ParamName);
        if (index < 0)
            throw new InvalidOperationException($"Param node '{node.Id}' references undeclared parameter '{node.ParamName}'");

        _il.Emit(OpCodes.Ldarg, (short)index);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// GetVar(var=name) -> value: reads a persistent VAR slot (GraphVariable/GraphVarStore). A PURE
    /// READ -- always safe to read twice, unlike SetVar's write -- so welcome in EITHER compiler like
    /// GetField/EmitParam; EmitPullGetVar (below) is the PULL-recursive twin for EmitPullOutput.
    ///
    /// Graph.Validate() already checked var= names a declared VAR with an agreeing type; this repeats
    /// the lookup defensively, same pattern as EmitParam above.
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

    /// Select(cond, ifTrue, ifFalse) -> result: picks one of two float values by a bool condition.
    ///
    /// NOT SHORT-CIRCUITING, AND NOT "BOTH ARMS COST NOTHING" either. The Brfalse/Br pair only
    /// decides which LOCAL gets loaded into `result` -- ifTrue/ifFalse were already computed upstream,
    /// since Compile()'s single topological pass calls EmitNode on every node exactly once regardless
    /// of any condition. Unlike Branch's exec fan-out (EmitExecFanOut), which genuinely skips the
    /// untaken arm's nodes, Select cannot skip computing either side. The branch here only saves the
    /// LDLOC (a branchless "compute both, keep one" would be equally correct) -- written this way
    /// because it's the shape LoadPin's caching model makes free.
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

    /// InputKey(key) -> down: reads Aver.Framework's polled input state. Pure/idempotent like
    /// GetField, so safe to pull repeatedly through either compiler with no _execLocals caching,
    /// unlike Raycast. aver_fw_input_key returns 0/1 as int32 -- the exact bit pattern Stloc expects
    /// for a bool local, same "no conversion needed" property EmitCompare's Cgt result relies on.
    private void EmitInputKey(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "key");
        _il.Emit(OpCodes.Call, InputKeyMethod);

        if (_pinLocals.TryGetValue((node.Id, "down"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// InputKeyPressed / InputKeyReleased: the rising/falling EDGE of a key, from framework ABI
    /// entry points that already answer that. Same shape as EmitInputKey (int in, bool out); node
    /// type picks the call.
    ///
    /// Output pin is `triggered`, not `down` -- `down` is a state, this is an EVENT: a key held for a
    /// second yields one true and fifty-nine falses, which is what jump/fire/toggle actually want.
    private void EmitInputKeyEdge(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "key");
        _il.Emit(OpCodes.Call, node.Type.ToLowerInvariant() == "inputkeyreleased"
                                   ? InputKeyReleasedMethod : InputKeyPressedMethod);

        if (_pinLocals.TryGetValue((node.Id, "triggered"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// InputAction(action) -> x, y, held: float2 value + digital-active state of a named action set up
    /// elsewhere (aver_fw_action_register/_bind, framework_abi.h Named Actions) -- InputKey's
    /// higher-level, PREFERRED sibling (see OcGraphParser's "inputaction" case). `action` is the
    /// ACTION HANDLE from aver_fw_action_register/_find, a plain Int like InputKey's "key" (PinType
    /// has no String member).
    ///
    /// TWO NATIVE CALLS: aver_fw_action_value2 (x,y) and aver_fw_action_held (held) are separate ABI
    /// entries (framework_abi.h:444,447), unlike GetFieldVec3/GetForward's single call -- both are
    /// cheap array-scan reads, so no _execLocals caching is needed despite that.
    ///
    /// aver_fw_action_value2 takes a `float[]` out-param, not `ref float` x3 like GetFieldVecForGraph
    /// -- so this Newarr's a 2-element array and Dup's the reference before the call, the one place
    /// this differs from EmitGetFieldVec3/EmitRaycast's "address per out-param" pattern.
    private void EmitInputAction(Node node)
    {
        if (_il == null) return;

        // Dup the fresh array: one reference feeds the call (as out2), the other survives in a local
        // to read back afterward -- the call returns void, so there's no other way to reach index 0/1.
        LoadPin(node.Id, "action");
        _il.Emit(OpCodes.Ldc_I4_2);
        _il.Emit(OpCodes.Newarr, typeof(float));
        var arrLocal = _il.DeclareLocal(typeof(float[]));
        _il.Emit(OpCodes.Dup);
        _il.Emit(OpCodes.Stloc, arrLocal);
        _il.Emit(OpCodes.Call, ActionValue2Method);

        _il.Emit(OpCodes.Ldloc, arrLocal);
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Ldelem_R4);
        _il.Emit(OpCodes.Stloc, RequirePinLocal(node, "x"));

        _il.Emit(OpCodes.Ldloc, arrLocal);
        _il.Emit(OpCodes.Ldc_I4_1);
        _il.Emit(OpCodes.Ldelem_R4);
        _il.Emit(OpCodes.Stloc, RequirePinLocal(node, "y"));

        // held: a separate ABI call (see doc comment above for why two calls, not one). Optional like
        // EmitGetForward's "success": stored when declared, popped otherwise to balance the stack.
        LoadPin(node.Id, "action");
        _il.Emit(OpCodes.Call, ActionHeldMethod);
        if (_pinLocals.TryGetValue((node.Id, "held"), out var heldLocal)) _il.Emit(OpCodes.Stloc, heldLocal);
        else                                                              _il.Emit(OpCodes.Pop);
    }

    /// InputActionPressed / InputActionReleased: action-level twin of EmitInputKeyEdge -- handle in,
    /// bool "triggered" out, node type picks aver_fw_action_pressed vs _released. Same "state vs
    /// event" reasoning as EmitInputKeyEdge for why the pin is `triggered`, not `held`.
    private void EmitInputActionEdge(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "action");
        _il.Emit(OpCodes.Call, node.Type.ToLowerInvariant() == "inputactionreleased"
                                   ? ActionReleasedMethod : ActionPressedMethod);

        if (_pinLocals.TryGetValue((node.Id, "triggered"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Raycast(originX,Y,Z, dirX,Y,Z, maxDist) -> hit, entity, pointX,Y,Z: one native call, five
    /// results. PULL computes every node once per Compile() regardless of reader count (EmitNode's
    /// doc), so unlike PUSH's EmitExecRaycast (needs _execLocals for the same guarantee), a plain
    /// "one call, five _pinLocals stores" already works: push 7 inputs, push each output local's
    /// ADDRESS (RequirePinLocal/Ldloca), Call. See GraphInterop.RaycastForGraph for why out-params
    /// rather than a returned struct.
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

    /// MouseDelta() -> deltaX, deltaY, wheel: one native call (aver_fw_input_mouse), mirroring
    /// EmitGetFieldVec3's "one call, several _pinLocals stores" shape. Compile()'s topological pass
    /// already guarantees one run per invocation regardless of reader count, so no extra machinery is
    /// needed here (see GraphInterop.MouseDeltaForGraph for why PUSH needs more). Zero data inputs.
    private void EmitMouseDelta(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "deltaX"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "deltaY"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "wheel"));
        _il.Emit(OpCodes.Call, MouseDeltaMethod);
    }

    /// MoveAxis() -> forward, right: MouseDelta's MOVE-input sibling (see EmitMouseDelta) --
    /// GraphInterop.MoveAxisForGraph explains why Z is not a pin.
    private void EmitMoveAxis(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "forward"));
        _il.Emit(OpCodes.Ldloca, RequirePinLocal(node, "right"));
        _il.Emit(OpCodes.Call, MoveAxisMethod);
    }

    /// Shared by every Emit* method whose native call takes an OUT-PARAMETER ADDRESS rather than a
    /// loaded value (Ldloca, not Ldloc) -- Raycast's five, and now GetFieldVec3's three: unlike every
    /// other Emit* method's "load, compute, maybe Stloc" shape, a missing pin here can't just be
    /// skipped the way LoadPin's callers skip a missing _pinLocals entry, because there would be
    /// nothing to push where the call signature requires an address. Thrown here, at the one shared
    /// call site that needs it, rather than silently leaving the IL stack unbalanced. Originally
    /// Raycast-only (the message used to hardcode "Raycast needs all five of ..."); generalised to name
    /// the node's own TYPE and PIN rather than a fixed node kind and a fixed pin count once a second
    /// caller needed it -- see OcGraphParser.AddDefaultPins for which pins a given type actually needs.
    private LocalBuilder RequirePinLocal(Node node, string pinName)
    {
        if (!_pinLocals.TryGetValue((node.Id, pinName), out var local))
            throw new InvalidOperationException(
                $"{node.Type} node '{node.Id}' has no output pin '{pinName}' declared -- see " +
                $"OcGraphParser.AddDefaultPins's '{node.Type.ToLowerInvariant()}' case for the pins this " +
                "node type needs, or give the node explicit PIN records for all of them");
        return local;
    }

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

        var node = _graph.Nodes[nodeId];
        var pin = node.Pins.FirstOrDefault(p => p.Name == pinName);

        // SAME DEFECT AS EmitPullInput's, in the other compiler, and WORSE: `if (pin != null)` used to
        // guard the entire emit, so a missing pin pushed NOTHING and the following Call silently
        // consumed whatever was beneath it on the stack (wrong operand or invalid IL). As in the PULL
        // path: absent is refused, merely unconnected still reads zero.
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

    // USER-DEFINED FUNCTIONS

    /// Creates a DynamicMethod for every declared function and emits each body. Idempotent; called at
    /// the TOP of both Compile() and CompileEntryPoint(), before either creates its own method --
    /// emitting a body overwrites _il/_pinLocals/_execLocals, so it can't run mid-emission.
    /// (EmitFunctionBody still saves/restores them, since a CallFunc in a body re-enters this class.)
    private bool EnsureFunctionsCompiled(out string? err)
    {
        err = null;
        if (_functionsCompiled || _graph.Functions.Count == 0) return true;
        _functionsCompiled = true;   // set FIRST: a failure leaves the partial state, and retrying
                                      // would emit every body a second time into fresh methods.

        // PASS 1 -- every signature, before any body. See _funcMethods.
        foreach (var fn in _graph.Functions)
        {
            Type[] argTypes = fn.Inputs.Select(p => PinTypeToCLRType(p.Type)).ToArray();
            if (_graph.Variables.Count > 0)
                argTypes = argTypes.Append(typeof(GraphVarStore)).ToArray();
            Type ret = fn.Outputs.Count switch
            {
                0 => typeof(void),
                1 => PinTypeToCLRType(fn.Outputs[0].Type),
                // Several outputs box into an object[], same as Compile() for several OUT records --
                // one convention, so a caller unpacking one has already seen how.
                _ => typeof(object[]),
            };
            _funcMethods[fn.Name] = new DynamicMethod($"GraphFunc_{fn.Name}", ret, argTypes, restrictedSkipVisibility: true);
        }

        // PASS 2 -- the bodies.
        foreach (var fn in _graph.Functions)
        {
            if (!EmitFunctionBody(fn, out err)) return false;
        }
        return true;
    }

    private bool EmitFunctionBody(GraphFunction fn, out string? err)
    {
        err = null;
        var dm = _funcMethods[fn.Name];

        // SAVE EVERYTHING: a CallFunc inside this body doesn't re-enter here (bodies emit one at a
        // time in pass 2), but a future caller might. Cheap, and removes a whole class of "why did
        // the event graph get this function's locals" bug before it can exist.
        var savedIl = _il;
        var savedPin = _pinLocals;
        var savedExec = _execLocals;
        var savedNode = _nodeLocals;
        var savedExecVisiting = _execVisiting;
        var savedPullVisiting = _pullVisiting;
        var savedVarStore = _varStoreArgIndex;
        var savedFunc = _currentFunc;
        var savedOutLocals = _funcOutLocals;
        try
        {
            _il = dm.GetILGenerator();
            _pinLocals = new Dictionary<(string, string), LocalBuilder>();
            _execLocals = new Dictionary<(string, string), LocalBuilder>();
            _nodeLocals = new Dictionary<string, LocalBuilder>();
            _execVisiting = new HashSet<string>();
            _pullVisiting = new HashSet<string>();
            _varStoreArgIndex = _graph.Variables.Count > 0 ? fn.Inputs.Count : -1;
            _currentFunc = fn;
            _funcOutLocals = new List<LocalBuilder>();
            foreach (var o in fn.Outputs) _funcOutLocals.Add(_il.DeclareLocal(PinTypeToCLRType(o.Type)));

            // THE RECURSION GUARD, as a helper call rather than IL. An unbounded recursive function
            // would overflow the CLR stack -- StackOverflowException is UNCATCHABLE, taking the whole
            // editor down with it -- so this counts depth in managed code and throws an ordinary
            // catchable exception instead.
            //
            // NOT a try/finally in emitted IL: a protected region's verifiability rules around
            // branching out of it would spread into every branch/loop emitter here, for a guarantee
            // not needed -- GraphCallGuard resets to zero at the start of every top-level invocation,
            // so a leaked depth (from an exception unwinding past an Exit) can't accumulate across calls.
            _il.Emit(OpCodes.Ldstr, fn.Name);
            _il.Emit(OpCodes.Call, GraphCallGuardEnter);

            if (fn.IsPure)
            {
                // No exec chain to walk: pull each declared output straight through the FuncReturn's
                // matching input pin, which recursively evaluates the body's data graph.
                if (fn.Outputs.Count > 0)
                {
                    if (fn.ReturnNodeId == null || !_graph.Nodes.TryGetValue(fn.ReturnNodeId, out var rn))
                    {
                        err = $"function '{fn.Name}' has outputs but no FuncReturn node";
                        return false;
                    }
                    for (int i = 0; i < fn.Outputs.Count; i++)
                    {
                        EmitPullInput(rn, fn.Outputs[i].Name);
                        _il.Emit(OpCodes.Stloc, _funcOutLocals[i]);
                    }
                }
            }
            else
            {
                if (fn.EntryNodeId == null || !_graph.Nodes.TryGetValue(fn.EntryNodeId, out var en))
                {
                    err = $"function '{fn.Name}' has no FuncEntry node";
                    return false;
                }
                EmitExecNode(en);
            }

            _il.Emit(OpCodes.Call, GraphCallGuardExit);

            // ONE Ret, reached after the whole body runs -- why a function has exactly one FuncReturn
            // (Validate refuses a second): it STORES into these locals when reached, not returns, so
            // every branch inside the body converges here.
            if (fn.Outputs.Count == 1)
            {
                _il.Emit(OpCodes.Ldloc, _funcOutLocals[0]);
            }
            else if (fn.Outputs.Count > 1)
            {
                _il.Emit(OpCodes.Ldc_I4, fn.Outputs.Count);
                _il.Emit(OpCodes.Newarr, typeof(object));
                for (int i = 0; i < fn.Outputs.Count; i++)
                {
                    _il.Emit(OpCodes.Dup);
                    _il.Emit(OpCodes.Ldc_I4, i);
                    _il.Emit(OpCodes.Ldloc, _funcOutLocals[i]);
                    _il.Emit(OpCodes.Box, _funcOutLocals[i].LocalType!);
                    _il.Emit(OpCodes.Stelem_Ref);
                }
            }
            _il.Emit(OpCodes.Ret);
            return true;
        }
        finally
        {
            _il = savedIl;
            _pinLocals = savedPin;
            _execLocals = savedExec;
            _nodeLocals = savedNode;
            _execVisiting = savedExecVisiting;
            _pullVisiting = savedPullVisiting;
            _varStoreArgIndex = savedVarStore;
            _currentFunc = savedFunc;
            _funcOutLocals = savedOutLocals;
        }
    }

    /// Loads a CallFunc node's arguments and emits the direct IL Call, leaving the callee's return
    /// (or nothing, or object[]) on the stack -- callers decide what to do with it: a pure pull wants
    /// ONE value, an exec call wants every output stored into its own local.
    private GraphFunction? EmitCallFuncInvoke(Node node)
    {
        if (_il == null) return null;
        var fn = _graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.CallTarget, StringComparison.OrdinalIgnoreCase));
        if (fn == null || !_funcMethods.TryGetValue(fn.Name, out var dm)) return null;
        foreach (var p in fn.Inputs) EmitPullInput(node, p.Name);
        // The variable store rides through every call, so a function can read and write the same
        // per-instance VARs its caller can. Loaded from the CALLER's own store argument, which is why
        // the argument exists on every function in a VAR-bearing graph whether that function touches
        // one or not -- a uniform signature costs one argument and removes a whole conditional.
        if (_graph.Variables.Count > 0 && _varStoreArgIndex >= 0)
            _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Call, dm);
        return fn;
    }

    /// A CallFunc reached by the exec walk: call once, store every output into its own exec local, so
    /// reading two of a function's outputs downstream does not call it twice.
    private void EmitExecCallFunc(Node node)
    {
        if (_il == null) return;
        var fn = EmitCallFuncInvoke(node);
        if (fn == null) return;
        if (fn.Outputs.Count == 0) return;
        if (fn.Outputs.Count == 1)
        {
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, fn.Outputs[0].Name, PinTypeToCLRType(fn.Outputs[0].Type)));
            return;
        }
        // object[] on the stack: keep it in a local, then unbox one element per output pin.
        var arr = _il.DeclareLocal(typeof(object[]));
        _il.Emit(OpCodes.Stloc, arr);
        for (int i = 0; i < fn.Outputs.Count; i++)
        {
            Type t = PinTypeToCLRType(fn.Outputs[i].Type);
            _il.Emit(OpCodes.Ldloc, arr);
            _il.Emit(OpCodes.Ldc_I4, i);
            _il.Emit(OpCodes.Ldelem_Ref);
            _il.Emit(OpCodes.Unbox_Any, t);
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, fn.Outputs[i].Name, t));
        }
    }

    /// A CallFunc pulled as data -- only legal for a PURE function: safe to evaluate whenever and as
    /// often as a reader asks. Leaves the ONE requested pin's value on the stack.
    private void EmitPullCallFunc(Node node, string pinName)
    {
        if (_il == null) return;
        var fn = EmitCallFuncInvoke(node);
        if (fn == null) throw new InvalidOperationException(
            $"CallFunc node '{node.Id}' calls '{node.CallTarget}', which is not a declared function");
        int idx = fn.Outputs.FindIndex(o => o.Name == pinName);
        if (idx < 0) throw new InvalidOperationException(
            $"function '{fn.Name}' has no output named '{pinName}'");
        if (fn.Outputs.Count == 1) return;   // already the value on the stack
        Type t = PinTypeToCLRType(fn.Outputs[idx].Type);
        _il.Emit(OpCodes.Ldc_I4, idx);
        _il.Emit(OpCodes.Ldelem_Ref);
        _il.Emit(OpCodes.Unbox_Any, t);
    }

    /// A CallFunc met by the PULL compiler's topological pass -- a call in a graph with no ENTRY
    /// records, the shape every pre-exec .ocgraph in this repo still has.
    ///
    /// AN IMPURE FUNCTION IS REFUSED HERE, same reason as Spawn/CharacterMove/FireEvent/SetVar: the
    /// topological pass runs every node exactly once per invocation with no branch structure to gate
    /// anything, so an exec-chain function's side effects would run whether the author meant it or
    /// not. Declaring a function `pure` is the promise that this is safe.
    private void EmitCallFuncTopological(Node node)
    {
        if (_il == null) return;
        var fn = _graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.CallTarget, StringComparison.OrdinalIgnoreCase));
        if (fn == null)
            throw new InvalidOperationException(
                $"CallFunc node '{node.Id}' calls '{node.CallTarget}', which is not a declared function");
        if (!fn.IsPure)
            throw new InvalidOperationException(
                $"CallFunc node '{node.Id}' calls '{fn.Name}', which is not pure, and Compile() has no exec " +
                "chain to run it from -- a pure-dataflow graph has no notion of WHEN to run a side effect. " +
                "Either declare the function pure (FUNC " + fn.Name + " pure), or give this graph an ENTRY " +
                "record and wire the call into its exec chain so CompileEntryPoint() runs it.");

        // One call, results into this node's pin locals -- same "call once, store every output"
        // shape as EmitExecCallFunc, but against _pinLocals since that's what LoadPin reads.
        var invoked = EmitCallFuncInvoke(node);
        if (invoked == null) return;
        if (fn.Outputs.Count == 0) return;
        if (fn.Outputs.Count == 1)
        {
            _il.Emit(OpCodes.Stloc, RequirePinLocal(node, fn.Outputs[0].Name));
            return;
        }
        var arr = _il.DeclareLocal(typeof(object[]));
        _il.Emit(OpCodes.Stloc, arr);
        for (int i = 0; i < fn.Outputs.Count; i++)
        {
            _il.Emit(OpCodes.Ldloc, arr);
            _il.Emit(OpCodes.Ldc_I4, i);
            _il.Emit(OpCodes.Ldelem_Ref);
            _il.Emit(OpCodes.Unbox_Any, PinTypeToCLRType(fn.Outputs[i].Type));
            _il.Emit(OpCodes.Stloc, RequirePinLocal(node, fn.Outputs[i].Name));
        }
    }

    /// A FuncReturn reached by the exec walk: pulls each declared output into the enclosing
    /// function's output local. Does NOT return -- see EmitFunctionBody for the single shared Ret.
    private void EmitFuncReturnStores(Node node)
    {
        if (_il == null || _currentFunc == null) return;
        for (int i = 0; i < _currentFunc.Outputs.Count && i < _funcOutLocals.Count; i++)
        {
            EmitPullInput(node, _currentFunc.Outputs[i].Name);
            _il.Emit(OpCodes.Stloc, _funcOutLocals[i]);
        }
    }

    /// A FuncEntry's output pin IS an argument of the enclosing method (same shape as EmitParam for a
    /// graph-level PARAM). The index is the pin's position in the function's input list, so a FUNCIN
    /// reordered in the file reorders the arguments and loads together.
    private void EmitFuncEntryLoad(string pinName)
    {
        if (_il == null || _currentFunc == null) return;
        int index = _currentFunc.Inputs.FindIndex(p => p.Name == pinName);
        if (index < 0)
            throw new InvalidOperationException(
                $"function '{_currentFunc.Name}' has no input named '{pinName}' -- add 'FUNCIN {_currentFunc.Name} {pinName} <type>'");
        _il.Emit(OpCodes.Ldarg, (short)index);
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
        // PinType.Exec falls here (typeof(void)) but should never actually be asked for: Compile()
        // rejects an exec-typed OUT before reaching a DeclareLocal call, EmitNode skips exec pins
        // when creating output locals, and OcGraphParser refuses an exec-typed PARAM at parse time.
        _ => typeof(void)
    };

    // EXEC / PUSH COMPILATION
    //
    // PUSH VS PULL: two compilers in this file. Above this line (Compile/TopologicalSort/EmitNode/
    // LoadPin) is the ORIGINAL, UNTOUCHED PULL compiler: a node with no exec edge runs once, whenever
    // the topological pass reaches it, caching its value in `_pinLocals` so every consumer reads the
    // same answer -- correct for a DAG of pure expressions, and unaffected by adding exec support: a
    // graph with no ENTRY record still produces the exact IL it always did.
    //
    // CompileEntryPoint(), below, PUSHES: a node reached via an exec edge runs at an ordered point in
    // time, possibly more than once (a loop) or never (an untaken branch arm) -- caching in a local is
    // EXACTLY WRONG for that (a loop's `cond` must be read fresh every pass). So the exec compiler
    // never touches `_pinLocals`/TopologicalSort/LoadPin; its own primitive, EmitPullInput/
    // EmitPullOutput below, is RECURSIVE AND UNCACHED -- every call re-emits the upstream subgraph's
    // IL on the spot. Two deliberate consequences:
    //
    //   1. A shared pure sub-expression pulled from two exec sites (or two loop iterations) is
    //      computed TWICE -- correct, not free. No cross-site memoization; Phase 1 is about a graph
    //      being ABLE to decide, not how cheaply -- left rough (flagged again in the phase-2 handoff
    //      notes).
    //
    //   2. A node with a genuine SIDE EFFECT (SetField, and others) must never be reached by a PULL --
    //      pulling it twice would silently perform its write twice. EmitPullOutput refuses to pull a
    //      side-effecting node's output; the write happens only when the exec walk visits the node
    //      directly (EmitExecSideEffect), exactly once per visit.
    //
    // NO SPECIAL CASE FOR "Sequence": firing exec-out pins is one generic operation (EmitExecFanOut,
    // below) that follows every EXEC-typed output pin with a link, in file order. EmitExecNode's
    // switch special-cases only the two shapes that are NOT "fire every exec-out pin": branch (fires
    // exactly one of two) and while/forEach (fire one, N times, looping).

    /// <summary>Compiles ONE declared entry point (`ENTRY &lt;nodeId&gt; &lt;eventName&gt;`) to a
    /// Delegate by walking PUSH/exec edges outward from that node -- see the PUSH VS PULL comment
    /// above for how this differs from Compile()'s PULL compilation, which this method never touches.
    ///
    /// Parameters: the graph's PARAM list in declaration order (same convention as Compile()), plus a
    /// trailing GraphVarStore iff the graph declares a VAR (see _varStoreArgIndex) -- invisible to the
    /// public Fire()/Tick() caller. Deliberately how OnTick receives delta time too: `PARAM deltaTime
    /// float` read by an ordinary `param` node, not a special "OnTick's second pin is always dt" rule --
    /// this generalizes to a future event with different args, with no new node type or format change.
    ///
    /// Return value: same OUT convention as Compile() (void / one CLR type / boxed object[] -- see its
    /// OUTPUTS ARRAY comment), read AFTER the chain finishes. Since EmitPullOutput checks `_execLocals`
    /// first, this can surface a loop/branch/side-effect value the walk itself produced (a while's
    /// `iterations`, a branch's `tookTrue`, a SetField's `success` -- see OcGraphParser.AddDefaultPins),
    /// making an otherwise-internal per-invocation value observable with no live native scene needed.
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

    /// Emits IL for one step of the exec walk: run `node`'s own logic (if any is worth sequencing),
    /// then hand control on. `_execVisiting` guards against a hand-authored exec LINK cycle that skips
    /// while/forEach's internal loop-back (see that field's comment), throwing a clear compile error
    /// instead of recursing forever. A DIAMOND (two `branch` arms reaching the same downstream node)
    /// is NOT a cycle: `_execVisiting` only tracks the CURRENT recursion path, cleared in `finally`
    /// once that path returns, so the false arm can still reach a node the true arm already visited.
    /// Cost: that shared node's IL is emitted twice (inlined at the end of each arm) rather than once
    /// with a jump -- an accepted Phase 1 rough edge (see PUSH VS PULL above); a real join-point/
    /// basic-block compiler is out of scope here.
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
                case "switchint":
                    EmitSwitchInt(node);
                    return; // same reason as branch: picks ONE arm; a generic fan-out would run the rest too
                case "doonce":
                    EmitDoOnce(node);
                    return;
                case "gate":
                    EmitGate(node);
                    return;
                case "flipflop":
                    EmitFlipFlop(node);
                    return;
                case "while":
                    EmitWhile(node);
                    return;
                case "foreach":
                    EmitForEach(node);
                    return;
                default:
                    // Every other node type reached via exec: run its side effect if it has one worth
                    // sequencing (SetField/SetFieldVec3/Spawn/SetVar -- separate predicate/emitter
                    // pairs, see each IsExecCapable*Type comment) or its cached QUERY if it has one
                    // (today, only Raycast), then fall through to the generic multi-exec-out fan-out --
                    // which handles a plain node with 0/1/N exec-output pins with no special case.
                    if (IsExecCapableSideEffectType(node.Type)) EmitExecSideEffect(node);
                    else if (IsExecCapableVecSideEffectType(node.Type)) EmitExecSetFieldVec3(node);
                    else if (IsExecCapableSpawnType(node.Type)) EmitExecSpawn(node);
                    else if (IsExecCapableVarSideEffectType(node.Type)) EmitExecSetVar(node);
                    else if (IsExecCapableQueryType(node.Type)) EmitExecRaycast(node);
                    else if (IsExecCapableMouseDeltaType(node.Type)) EmitExecMouseDelta(node);
                    else if (IsExecCapableMoveAxisType(node.Type)) EmitExecMoveAxis(node);
                    else if (IsExecCapableSetParentType(node.Type)) EmitExecSetParent(node);
                    else if (IsExecCapableSetViewEntityType(node.Type)) EmitExecSetViewEntity(node);
                    else if (IsExecCapableSetNameType(node.Type)) EmitExecSetName(node);
                    else if (IsExecCapableSetMeshType(node.Type)) EmitExecSetMesh(node);
                    else if (IsExecCapableSetMaterialType(node.Type)) EmitExecSetMaterial(node);
                    else if (IsExecCapableAttachToSocketType(node.Type)) EmitExecAttachToSocket(node);
                    else if (IsExecCapableSetSkeletonType(node.Type)) EmitExecSetSkeleton(node);
                    else if (IsExecCapablePlayAnimationType(node.Type)) EmitExecPlayAnimation(node);
                    else if (IsExecCapableCharacterMoveType(node.Type)) EmitExecCharacterMove(node);
                    else if (IsExecCapableJumpType(node.Type)) EmitJump(node);
                    else if (IsExecCapablePrintType(node.Type)) EmitExecPrint(node);
                    else if (IsExecCapableCallFuncType(node.Type)) EmitExecCallFunc(node);
                    else if (IsExecCapableFuncReturnType(node.Type)) EmitFuncReturnStores(node);
                    else if (IsExecCapableApiCallType(node.Type)) EmitExecApiCall(node);
                    else if (IsExecCapableTransformWriteType(node.Type)) EmitExecTransformWrite(node);
                    else if (IsExecCapablePhysicsWriteType(node.Type)) EmitExecPhysicsWrite(node);
                    else if (IsExecCapablePhysicsCreateType(node.Type)) EmitExecPhysicsCreate(node);
                    else if (IsExecCapableJointCreateType(node.Type)) EmitExecJointCreate(node);
                    else if (IsExecCapableJointOpType(node.Type)) EmitExecJointOp(node);
                    else if (IsExecCapableSphereCastType(node.Type)) EmitExecSphereCast(node);
                    else if (IsExecCapableFireEventType(node.Type)) EmitExecFireEvent(node);
                    else if (IsExecCapableSaveLoadType(node.Type)) EmitExecSaveLoad(node);
                    else if (IsExecCapableCreateEntityType(node.Type)) EmitExecCreateEntity(node);
                    else if (IsExecCapableAudioType(node.Type)) EmitExecAudio(node);
                    EmitExecFanOut(node);
                    return;
            }
        }
        finally
        {
            _execVisiting.Remove(node.Id);
        }
    }

    /// Fires every declared EXEC-typed OUTPUT pin on `node`, in node.Pins order -- file order for an
    /// explicit PIN record, catalog order for a default-pinned node (OcGraphParser.AddDefaultPins /
    /// GraphNodeDefs.hpp). See the section comment above for why this alone implements "Sequence".
    private void EmitExecFanOut(Node node)
    {
        if (_il == null) return;

        var execOuts = node.Pins.Where(p => p.IsOutput && p.Type == PinType.Exec).ToList();

        // "fireLog" -- OPT-IN OBSERVABILITY, not part of the control-flow contract; a no-op unless
        // the node declares an int output pin named "fireLog" (Sequence's default pins do). Proving
        // "a sequence fires its arms in file order" needs a channel that survives to the end of the
        // method: no live native scene to observe a SetField write (GraphHostTests.cs has the same
        // limitation), and a pure pull can't prove a node was actually VISITED, only computable on
        // demand. Updated as `fireLog = fireLog*10 + (armIndex+1)` before each arm; three arms in
        // order leaves fireLog == 123.
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

    /// Resolves the single exec LINK leaving `nodeId.pinName`; null means unwired, which is not an
    /// error (same permissiveness LoadPin extends to an unwired DATA input, defaulting rather than failing).
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

    /// branch: a bool condition, ONE incoming exec pulse, exactly one of `true`/`false` fires --
    /// unlike EmitExecFanOut, which fires ALL exec-out pins. switchint (Blueprint's Switch on Int):
    /// evaluate the selector once, run exactly ONE case chain or the default.
    ///
    /// A CHAIN OF COMPARES, NOT AN IL `switch` OPCODE: that needs a dense jump table from zero, but
    /// the selector is an arbitrary author-wired int (negative/sparse/out-of-range are ordinary).
    /// Four compares cost nothing at this scale.
    ///
    /// THE SELECTOR IS PULLED ONCE into a local, not re-pulled per case -- EmitPullOutput is
    /// deliberately uncached, so re-pulling would re-evaluate it (a Raycast-fed selector would trace
    /// the ray once per case).
    private void EmitSwitchInt(Node node)
    {
        if (_il == null) return;

        var sel = _il.DeclareLocal(typeof(int));
        EmitPullInput(node, "selector");
        _il.Emit(OpCodes.Stloc, sel);

        var takenPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "taken" && p.Type == PinType.Int);
        LocalBuilder? taken = takenPin != null ? GetOrCreateExecLocal(node.Id, "taken", typeof(int)) : null;

        var endLabel = _il.DefineLabel();
        // Every case that the node actually declares a pin for -- so a hand-written file that gives a
        // SwitchInt only case0 and default compiles to exactly that, rather than to four branches
        // three of which can never be reached.
        for (int i = 0; ; ++i)
        {
            string pinName = "case" + i.ToString(System.Globalization.CultureInfo.InvariantCulture);
            if (!node.Pins.Any(p => p.IsOutput && p.Name == pinName && p.Type == PinType.Exec)) break;

            var nextTest = _il.DefineLabel();
            _il.Emit(OpCodes.Ldloc, sel);
            _il.Emit(OpCodes.Ldc_I4, i);
            _il.Emit(OpCodes.Bne_Un, nextTest);
            if (taken != null) { _il.Emit(OpCodes.Ldc_I4, i); _il.Emit(OpCodes.Stloc, taken); }
            Node? target = FindExecTarget(node.Id, pinName);
            if (target != null) EmitExecNode(target);
            _il.Emit(OpCodes.Br, endLabel);
            _il.MarkLabel(nextTest);
        }

        // The default arm. -1 rather than the case count, so "no case matched" is one value whatever
        // the node's width is and a graph reading `taken` does not have to know how many cases exist.
        if (taken != null) { _il.Emit(OpCodes.Ldc_I4_M1); _il.Emit(OpCodes.Stloc, taken); }
        Node? def = FindExecTarget(node.Id, "default");
        if (def != null) EmitExecNode(def);

        _il.MarkLabel(endLabel);
    }

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

    /// while: `cond` (re-pulled fresh every pass -- see PUSH VS PULL for why not LoadPin's cached
    /// approach) gates a `loop` body, up to MaxLoopIterations -- past that, WarnLoopGuardTripped logs
    /// (node + event) and the loop stops as if `cond` went false, so a buggy graph loses one tick's
    /// correctness rather than hanging the caller. `done` runs once either way.
    // The reserved GraphVarStore key a stateful flow node keeps its memory under -- node ids are
    // unique and a VAR name cannot contain '$', so this can never collide with an author's variable.
    private static string FlowStateKey(Node node) => "$flow$" + node.Id;

    /// Emits `store.GetBool("$flow$<id>")` onto the stack.
    private void EmitLoadFlowState(Node node)
    {
        _il!.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, FlowStateKey(node));
        _il.Emit(OpCodes.Call, VarGetMethodFor(PinType.Bool));
    }

    /// Emits `store.SetBool("$flow$<id>", <value already on the stack>)`.
    private void EmitStoreFlowState(Node node)
    {
        var tmp = _il!.DeclareLocal(typeof(bool));
        _il.Emit(OpCodes.Stloc, tmp);
        _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, FlowStateKey(node));
        _il.Emit(OpCodes.Ldloc, tmp);
        _il.Emit(OpCodes.Call, VarSetMethodFor(PinType.Bool));
    }

    /// Every stateful flow node needs somewhere to remember; a graph with no VAR records has no store
    /// argument, so name which node needs one rather than letting the IL reference argument -1.
    private void RequireFlowState(Node node)
    {
        if (_varStoreArgIndex < 0)
            throw new InvalidOperationException(
                $"'{node.Type}' node '{node.Id}' remembers state between activations and needs a " +
                "variable store, but this graph declares no VAR records -- declare any VAR to give it one");
    }

    /// doOnce: `then` fires the FIRST time exec runs and never again, until `reset` runs.
    /// The stored bool means "already fired", so a fresh instance starts false and fires.
    private void EmitDoOnce(Node node)
    {
        if (_il == null) return;
        RequireFlowState(node);

        // RESET IS SAMPLED FIRST, so reset=true re-arms and fires in the same pass -- what a caller
        // wiring "reset and go" means, and lets one exec chain use the node instead of needing two.
        var afterReset = _il.DefineLabel();
        EmitPullInput(node, "reset");
        _il.Emit(OpCodes.Brfalse, afterReset);
        _il.Emit(OpCodes.Ldc_I4_0);
        EmitStoreFlowState(node);
        _il.MarkLabel(afterReset);

        var alreadyFired = _il.DefineLabel();
        EmitLoadFlowState(node);
        _il.Emit(OpCodes.Brtrue, alreadyFired);

        _il.Emit(OpCodes.Ldc_I4_1);
        EmitStoreFlowState(node);
        Node? then = FindExecTarget(node.Id, "then");
        if (then != null) EmitExecNode(then);

        _il.MarkLabel(alreadyFired);
    }

    /// gate: `then` fires only while open. `open`/`close` are sampled every activation and applied
    /// BEFORE the test, so opening and firing in one activation works. Closed is default (Blueprint's Gate).
    private void EmitGate(Node node)
    {
        if (_il == null) return;
        RequireFlowState(node);

        var afterOpen = _il.DefineLabel();
        EmitPullInput(node, "open");
        _il.Emit(OpCodes.Brfalse, afterOpen);
        _il.Emit(OpCodes.Ldc_I4_1);
        EmitStoreFlowState(node);
        _il.MarkLabel(afterOpen);

        // close AFTER open, so a graph wiring both true ends closed -- one rule, stated here,
        // rather than an order that depends on which link the parser happened to read first.
        var afterClose = _il.DefineLabel();
        EmitPullInput(node, "close");
        _il.Emit(OpCodes.Brfalse, afterClose);
        _il.Emit(OpCodes.Ldc_I4_0);
        EmitStoreFlowState(node);
        _il.MarkLabel(afterClose);

        var shut = _il.DefineLabel();
        EmitLoadFlowState(node);
        _il.Emit(OpCodes.Brfalse, shut);
        Node? then = FindExecTarget(node.Id, "then");
        if (then != null) EmitExecNode(then);
        _il.MarkLabel(shut);
    }

    /// flipFlop: alternates between the `a` and `b` exec outputs, starting with `a`, and
    /// reports which one it just took on `isA`.
    private void EmitFlipFlop(Node node)
    {
        if (_il == null) return;
        RequireFlowState(node);

        // Stored bool means "b is next". Starts false, so the first activation takes `a`.
        var takeB = _il.DefineLabel();
        var done = _il.DefineLabel();

        EmitLoadFlowState(node);
        _il.Emit(OpCodes.Brtrue, takeB);

        _il.Emit(OpCodes.Ldc_I4_1);
        EmitStoreFlowState(node);
        if (_pinLocals.TryGetValue((node.Id, "isA"), out var isATrue))
        {
            _il.Emit(OpCodes.Ldc_I4_1);
            _il.Emit(OpCodes.Stloc, isATrue);
        }
        Node? a = FindExecTarget(node.Id, "a");
        if (a != null) EmitExecNode(a);
        _il.Emit(OpCodes.Br, done);

        _il.MarkLabel(takeB);
        _il.Emit(OpCodes.Ldc_I4_0);
        EmitStoreFlowState(node);
        if (_pinLocals.TryGetValue((node.Id, "isA"), out var isAFalse))
        {
            _il.Emit(OpCodes.Ldc_I4_0);
            _il.Emit(OpCodes.Stloc, isAFalse);
        }
        Node? b = FindExecTarget(node.Id, "b");
        if (b != null) EmitExecNode(b);

        _il.MarkLabel(done);
    }

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

    /// forEach: the COUNTED-REPEAT variant (no array/collection pin type exists yet -- see
    /// OcGraphParser.AddDefaultPins's "foreach" case). `index` runs 0..count-1 through `loop`; the
    /// natural `index >= count` bound already prevents "forever", and MaxLoopIterations is a second,
    /// redundant guard for a corrupted/absurd `count` -- cheap insurance, not the primary mechanism.
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

    /// Declares (once) or returns (on later calls) the IL local backing one exec-scoped pin -- a
    /// loop's live counter, a branch's "which side" flag, a captured SetField return code. Declared
    /// ONCE per compile, but the SAME slot is written afresh every RUNTIME pass, since the IL that
    /// writes it sits inside the loop's branch-back range -- IL loops via jumps, not by re-emitting
    /// the body N times, which is what makes a loop counter "just work".
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

    /// Node types with NO side effect that still want the exec walk's "compute once per visit, cache
    /// into _execLocals" shape -- today, only Raycast. Separate from IsExecCapableSideEffectType
    /// because the reason differs: SetField is on the exec chain for CORRECTNESS (pulling twice would
    /// silently write twice); Raycast is there for COST (pulling twice re-runs an expensive query).
    /// EmitPullOutput COULD compute a query type fresh (unlike a side effect, which it refuses), it
    /// just doesn't today, by choice (see its "raycast has NO case here" note).
    private static bool IsExecCapableQueryType(string type) =>
        type.Equals("raycast", StringComparison.OrdinalIgnoreCase);

    /// MouseDelta's own version of IsExecCapableQueryType -- separate native surface (Aver.Framework's
    /// polled input, not physics), different arity (3 out-params vs 5) and cost class (memcpy-class
    /// copy, not a BVH walk); see GraphInterop.MouseDeltaForGraph for why it shares Raycast's caching
    /// SHAPE without the caching REASON. Kept separate so a future continuous-input type has its own spot.
    private static bool IsExecCapableMouseDeltaType(string type) =>
        type.Equals("mousedelta", StringComparison.OrdinalIgnoreCase);

    /// MoveAxis's own version of IsExecCapableMouseDeltaType (see that comment) -- different native
    /// surface (Input.MoveAxis's ~8 GetKey reads, not one mouse call) and arity (2 out-params not 3).
    private static bool IsExecCapableMoveAxisType(string type) =>
        type.Equals("moveaxis", StringComparison.OrdinalIgnoreCase);

    /// SetFieldVec3's own version of IsExecCapableSideEffectType -- separate because SetField/
    /// SetFieldVec3 write through different wrappers, arities, and field-kind checks (F32 vs Vec3);
    /// folding them would force EmitExecSideEffect to type-switch internally.
    private static bool IsExecCapableVecSideEffectType(string type) =>
        type.Equals("setfieldvec3", StringComparison.OrdinalIgnoreCase);

    /// Spawn's own version, but stricter than SetField/SetFieldVec3: those are refused as a PULL only
    /// by EmitPullOutput's refusal (below); Spawn is ALSO refused by EmitNode's "spawn" case in the
    /// topological pass entirely. Own predicate because SpawnForGraph is a different native surface
    /// (Aver.Framework's class registry, not Aver.Scene's field table) resolved by NAME at runtime.
    private static bool IsExecCapableSpawnType(string type) =>
        type.Equals("spawn", StringComparison.OrdinalIgnoreCase);

    /// SetVar's own version -- writes through GraphVarStore (in-process, type dispatch), not a native
    /// P/Invoke, so it has none of a native write's failure modes (see EmitExecSetVar for why that
    /// means no "success" pin either). Still shares the family's PULL refusal (EmitPullOutput below,
    /// EmitNode's "setvar" case).
    private static bool IsExecCapableVarSideEffectType(string type) =>
        type.Equals("setvar", StringComparison.OrdinalIgnoreCase);

    /// SetParent's own version -- SetParentForGraph (direct reflect into Aver.Scene.Native, no
    /// wrapper) has a different arity (two entity ids) and refusal contract (cycle/self-parent
    /// rejection, not missing-component). See OcGraphParser.AddDefaultPins's setparent/setviewentity/
    /// setname comment for why this family is dispatched SetField-style, not Spawn/SetVar-style.
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

    /// SetMesh's own version -- unlike SetParent/SetName (reflect straight into Aver.Scene.Native),
    /// this goes through a GraphInterop wrapper (SetMeshForGraph), needing Entity's internal
    /// constructor like GetFieldVecForGraph/SetFieldVecForGraph.
    private static bool IsExecCapableSetMeshType(string type) =>
        type.Equals("setmesh", StringComparison.OrdinalIgnoreCase);

    /// SetMaterial's own version -- see IsExecCapableSetMeshType's comment, which applies unchanged.
    private static bool IsExecCapableSetMaterialType(string type) =>
        type.Equals("setmaterial", StringComparison.OrdinalIgnoreCase);

    /// SetSkeleton's own version -- see IsExecCapableSetMeshType's comment, which applies unchanged
    /// (SetSkeletonForGraph is the identical "needs Entity's internal constructor" GraphInterop shape).
    private static bool IsExecCapableSetSkeletonType(string type) =>
        type.Equals("setskeleton", StringComparison.OrdinalIgnoreCase);

    /// PlayAnimation's own version (see IsExecCapableSetMeshType). Grouped with SetMesh/SetMaterial/
    /// SetSkeleton, not Spawn/PlaySound: only one CAnimator per entity, so replaying a clip twice
    /// overwrites the same fields, unlike PlaySound's genuinely NEW voice each call.
    private static bool IsExecCapablePlayAnimationType(string type) =>
        type.Equals("playanimation", StringComparison.OrdinalIgnoreCase);

    /// AttachToSocket's own version. Grouped with SetMesh/SetMaterial, not Spawn: IDEMPOTENT --
    /// attaching twice is the same state, not two attachments -- so allowed in the PULL compiler too
    /// (NOT in IsPushOnlySideEffectType).
    private static bool IsExecCapableAttachToSocketType(string type) =>
        type.Equals("attachtosocket", StringComparison.OrdinalIgnoreCase);

    /// CharacterMove's own version of IsExecCapableSpawnType -- refused by the PULL compiler's
    /// topological pass ENTIRELY (EmitNode's "charactermove" case), the same stricter-than-SetField
    /// treatment as Spawn: SetParent/SetViewEntity/SetName/SetMesh/SetMaterial write through an
    /// idempotent "same value twice is harmless" ABI call, but CharacterMoveForGraph ->
    /// AverCharacter.Drive mutates _yaw/_pitch and capsule velocity on every call -- a materially
    /// worse hazard than a stray field overwrite. Own predicate: a different native surface
    /// (AverCharacter/Actors) with its own signature (six scalars in, bool out).
    private static bool IsExecCapableCharacterMoveType(string type) =>
        type.Equals("charactermove", StringComparison.OrdinalIgnoreCase);

    /// Jump gets its OWN predicate rather than joining CharacterMove's -- a different call, emitter,
    /// and failure mode (declined in mid-air, ordinary); a predicate matching two unrelated types is
    /// one whose name stops describing what it matches.
    private static bool IsExecCapableJumpType(string type) =>
        type.Equals("jump", StringComparison.OrdinalIgnoreCase);

    /// Print gets its own predicate for the reason every side effect above it does: one per type,
    /// so a predicate's name never stops describing what it matches.
    private static bool IsExecCapablePrintType(string type) =>
        type.Equals("print", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("printint", StringComparison.OrdinalIgnoreCase);

    /// The four framework WRITE calls, grouped into ONE predicate (unlike Jump/CharacterMove): all
    /// share EmitExecApiCall exactly. The three transform writers share one emitter for the same
    /// reason. Physics writers/creators/sweep get three predicates for three emitters (a write
    /// returns bool, a creator returns a body handle, the sweep fills exec-locals like Raycast).
    /// WIDENED past the original six for forces/impulses/mass/motion-type/layer etc -- each shares
    /// EmitExecPhysicsWrite's "pull body (unless setgravity), pull scalars, call, store/pop bool"
    /// shape, joining its switch rather than getting a new one.
    private static bool IsExecCapablePhysicsWriteType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "setbodyposition" || t == "setbodyvelocity" || t == "addbodyvelocity" || t == "destroybody" ||
               t == "setbodyentity" || t == "setgravity" ||
               t == "addforce" || t == "addimpulse" || t == "addtorque" || t == "addangularimpulse" ||
               t == "setbodyangularvelocity" || t == "setbodyfriction" || t == "setbodyrestitution" ||
               t == "setbodygravityfactor" || t == "setbodymass" || t == "setbodymotiontype" ||
               t == "activatebody" || t == "setbodylayer";
    }

    private static bool IsExecCapablePhysicsCreateType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "addstaticbox" || t == "adddynamicbox" || t == "adddynamicsphere" || t == "addsensorbox" || t == "addsensorsphere";
    }

    private static bool IsExecCapableSphereCastType(string type) =>
        type.Equals("spherecast", StringComparison.OrdinalIgnoreCase);

    /// The joint creators -- EmitExecJointCreate's dispatch list. A FOURTH physics-family predicate,
    /// not folded into IsExecCapablePhysicsCreateType: return goes into exec-local "joint" not "body",
    /// and inputs start with bodyA/bodyB, not a single centre point.
    private static bool IsExecCapableJointCreateType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "jointhinge" || t == "jointpoint" || t == "jointdistance" || t == "jointslider" || t == "jointfixed";
    }

    /// The joint operations -- EmitExecJointOp's dispatch list. Apart from IsExecCapablePhysicsWriteType
    /// for the same reason as the creators: these key off a JOINT handle, never a body.
    private static bool IsExecCapableJointOpType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "jointsetmotor" || t == "jointsetenabled" || t == "jointremove";
    }

    private static bool IsExecCapableTransformWriteType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "translate" || t == "setlocalscale" || t == "setlocalposition" || t == "destroyentity";
    }

    /// A CallFunc reached by the exec walk. Purity doesn't decide this -- a PURE function can be
    /// called from an exec chain too (it just has no exec pins to wire); this only asks "is it a call".
    private static bool IsExecCapableCallFuncType(string type) =>
        type.Equals("callfunc", StringComparison.OrdinalIgnoreCase);

    private static bool IsExecCapableFuncReturnType(string type) =>
        type.Equals("funcreturn", StringComparison.OrdinalIgnoreCase);

    private static bool IsExecCapableApiCallType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "setvelocity" || t == "teleport" || t == "possess" || t == "unpossess" ||
               t == "setvisible" || t == "addtag" || t == "removetag" || t == "setlayercollision";
    }

    /// FireEvent's own version of IsExecCapableSpawnType -- refused by the PULL compiler's
    /// topological pass ENTIRELY, same stricter treatment as Spawn/CharacterMove. GraphEvents.
    /// FireEventForGraph routes into a DIFFERENT GraphHost (not P/Invoke, not GraphVarStore), with
    /// its own signature (target int, event name string in; bool "did it run" out) and failure mode
    /// (no live graph bound, or one that never declared this event).
    private static bool IsExecCapableFireEventType(string type) =>
        type.Equals("fireevent", StringComparison.OrdinalIgnoreCase);

    /// SaveGame/LoadGame's own version -- ONE predicate (unlike Jump/CharacterMove) since both share
    /// one emitter (EmitExecSaveLoad). Refused by the PULL compiler entirely -- the worst case here:
    /// LoadGame doesn't write one field or spawn one entity, it replaces the world.
    private static bool IsExecCapableSaveLoadType(string type) =>
        type.Equals("savegame", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("loadgame", StringComparison.OrdinalIgnoreCase);

    /// Runs a SetField node's write exactly once, when the exec walk reaches it -- mirrors
    /// EmitSetField's field=/resolver/native-call logic, but pulls "entity"/"value" via EmitPullInput,
    /// not LoadPin/_pinLocals (see the section comment for why). A declared "success" pin captures the
    /// real return code into an exec-local (same mechanism as branch's "tookTrue"); otherwise discarded.
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

    /// SetFieldVec3's own version of EmitExecSideEffect -- mirrors EmitSetFieldVec3's field=/resolver
    /// logic (RequireVec3Field) but pulls inputs via EmitPullInput and captures "success" into an
    /// exec-local, for the same reason EmitExecSideEffect does: this runs from the exec walk, where
    /// nothing was pre-computed by a topological pass.
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
            _il.Emit(OpCodes.Pop); // nothing declared to read the return code; discard it
        }
    }

    /// Runs a Spawn node's native call once, when the exec walk reaches it -- mirrors
    /// EmitExecSideEffect/EmitExecSetFieldVec3's shape (pull inputs, Call, capture-or-discard into an
    /// exec-local), but class= is resolved by GraphInterop.SpawnForGraph at RUNTIME, not a compile-time
    /// fieldId. class= is required at compile time: an empty one can never spawn anything, so failing
    /// loudly now beats a silent entity-0 at runtime.
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
            _il.Emit(OpCodes.Pop); // nothing declared to read the new entity id; discard it
        }
    }

    /// CreateEntity is exec-only, same reason as Spawn one tier down: Spawn mints an ACTOR, this a
    /// bare entity, both side effects a dataflow pull can't gate. Same shape as EmitExecSpawn -- push
    /// the name= literal, Call, capture the two outputs or discard them.
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
            _il.Emit(OpCodes.Pop);   // nothing declared to read it; the stack still has to balance

        if (node.Pins.Any(p => p.IsOutput && p.Name == "entity"))
        {
            _il.Emit(OpCodes.Ldloc, entityLocal);
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "entity", typeof(int)));
        }
    }

    private static bool IsExecCapableCreateEntityType(string type) =>
        type.Equals("createentity", StringComparison.OrdinalIgnoreCase);

    /// The five exec-capable audio nodes. Playing a sound, stopping one, moving the ears and moving
    /// a slider are all side effects; IsSoundPlaying is the one pure read and is not in this list.
    private static bool IsExecCapableAudioType(string type) => type.ToLowerInvariant() switch
    {
        "playsound" or "playsoundat" or "stopsound" or "setlistener" or "setbusvolume" => true,
        _ => false,
    };

    /// Runs one audio node's call at the point the exec walk reaches it. Same shape as
    /// EmitExecSpawn: push the sound= literal where there is one (there is no string pin it could
    /// arrive on), pull the ordinary pins, Call, then capture or discard each output.
    private void EmitExecAudio(Node node)
    {
        if (_il == null) return;
        string t = node.Type.ToLowerInvariant();

        if (t == "playsound" || t == "playsoundat")
        {
            if (string.IsNullOrEmpty(node.SoundPath))
                throw new InvalidOperationException(
                    $"{node.Type} node '{node.Id}' has no sound= attribute naming the file to play");
            _il.Emit(OpCodes.Ldstr, node.SoundPath);
            if (t == "playsoundat")
            {
                EmitPullInput(node, "x");
                EmitPullInput(node, "y");
                EmitPullInput(node, "z");
            }
            EmitPullInput(node, "volume");
            EmitPullInput(node, "pitch");
            EmitPullInput(node, "looping");
            EmitPullInput(node, "bus");
            if (t == "playsoundat")
            {
                EmitPullInput(node, "innerCm");
                EmitPullInput(node, "outerCm");
            }
            var voiceLocal = _il.DeclareLocal(typeof(int));
            _il.Emit(OpCodes.Ldloca, voiceLocal);
            _il.Emit(OpCodes.Call, t == "playsound" ? PlaySoundMethod : PlaySoundAtMethod);

            if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
                _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
            else
                _il.Emit(OpCodes.Pop);   // the stack still has to balance

            if (node.Pins.Any(p => p.IsOutput && p.Name == "voice"))
            {
                _il.Emit(OpCodes.Ldloc, voiceLocal);
                _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "voice", typeof(int)));
            }
            return;
        }

        switch (t)
        {
            case "stopsound":     EmitPullInput(node, "voice");  _il.Emit(OpCodes.Call, StopSoundMethod); break;
            case "setlistener":   EmitPullInput(node, "entity"); _il.Emit(OpCodes.Call, SetListenerMethod); break;
            case "setbusvolume":
                EmitPullInput(node, "bus");
                EmitPullInput(node, "volume");
                _il.Emit(OpCodes.Call, SetBusVolumeMethod);
                break;
            default: return;
        }
        if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else
            _il.Emit(OpCodes.Pop);
    }

    /// Runs a CharacterMove node's native call once, when reached -- mirrors EmitExecSpawn's shape
    /// (pull inputs, Call, capture-or-discard into an exec-local), but no attribute check: unlike
    /// Spawn's class=, all six CharacterMoveForGraph parameters are ordinary pins (see
    /// OcGraphParser.AddDefaultPins's "CharacterMove" comment).
    private void EmitExecCharacterMove(Node node)
    {
        if (_il == null) return;

        EmitPullInput(node, "entity");
        EmitPullInput(node, "dt");
        EmitPullInput(node, "forward");
        EmitPullInput(node, "right");
        EmitPullInput(node, "yawDelta");
        EmitPullInput(node, "pitchDelta");
        _il.Emit(OpCodes.Call, CharacterMoveMethod);

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

    /// Runs a FireEvent node's call once, when reached -- mirrors EmitExecSpawn/EmitExecCharacterMove's
    /// shape, but event= is an edit-time NODE-line attribute (Node.EventName) required non-empty at
    /// COMPILE time, like Spawn's class= (an empty one can never fire anything, so fail loudly now).
    /// UNLIKE class=, `target` IS an ordinary pin -- the entity to fire at is computed at RUNTIME
    /// (a Spawn's entity output, a VAR, a Raycast's entity pin), not chosen at edit time.
    private void EmitExecFireEvent(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.EventName))
            throw new InvalidOperationException(
                $"FireEvent node '{node.Id}' has no event= attribute naming which event to fire");

        EmitPullInput(node, "target");
        _il.Emit(OpCodes.Ldstr, node.EventName);
        _il.Emit(OpCodes.Call, FireEventMethod);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "fired"))
        {
            var firedLocal = GetOrCreateExecLocal(node.Id, "fired", typeof(bool));
            _il.Emit(OpCodes.Stloc, firedLocal);
        }
        else
        {
            _il.Emit(OpCodes.Pop); // nothing declared to read the outcome; discard it
        }
    }

    /// Runs a SaveGame or LoadGame write once, when reached -- the ONE emitter both share (see
    /// IsExecCapableSaveLoadType). Mirrors EmitExecSetName otherwise: push path=, call, capture-or-
    /// discard "success". NO ENTITY PULLED, unlike the rest of this family -- these act on the whole
    /// world, not one thing in it.
    private void EmitExecSaveLoad(Node node)
    {
        if (_il == null) return;

        bool isSave = node.Type.Equals("savegame", StringComparison.OrdinalIgnoreCase);
        if (string.IsNullOrEmpty(node.SavePath))
            throw new InvalidOperationException(
                $"{(isSave ? "SaveGame" : "LoadGame")} node '{node.Id}' has no path= attribute naming the file");

        _il.Emit(OpCodes.Ldstr, node.SavePath);
        _il.Emit(OpCodes.Call, isSave ? SaveGameMethod : LoadGameMethod);

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

    /// Runs a SetVar node's write once, when reached -- mirrors EmitExecSideEffect/
    /// EmitExecSetFieldVec3/EmitExecSpawn's shape, but var= is checked directly against
    /// _graph.Variables (no native lookup: a VAR is declared in THIS graph file, not a scene/class
    /// registry like field=/class=).
    ///
    /// NO "success" PIN -- a REAL difference, not a missing feature: SetField/SetFieldVec3's success
    /// reflects a REAL native return code (unknown entity, read-only field, etc), but a write into
    /// GraphVarStore's Dictionary, keyed by a name Graph.Validate() already confirmed declared and
    /// typed, has nothing left to fail at runtime.
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

    /// Runs an AttachToSocket node once, when reached.
    ///
    /// EmitPullInput, NOT LoadPin -- the rule that made Jump dead from birth: on the exec path a pin
    /// has no _pinLocals entry, so LoadPin reads an unset local and silently attaches entity 0 to entity 0.
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
    ///
    /// EmitPullInput, NOT LoadPin, on both data pins -- see EmitExecAttachToSocket for why: on the
    /// exec path LoadPin would silently read an unset local.
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

    /// Runs a MouseDelta native read once, when reached -- mirrors EmitExecRaycast's "one call, N
    /// results into N exec-locals" shape (zero inputs, since MouseDelta takes none), gated by
    /// IsExecCapableMouseDeltaType instead of IsExecCapableQueryType. See
    /// GraphInterop.MouseDeltaForGraph for why this needs the exec-cached shape, and
    /// EmitPullOutput's "no case for mousedelta/moveaxis" note for what happens if never visited by exec.
    private void EmitExecMouseDelta(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "deltaX", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "deltaY", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "wheel", typeof(float)));
        _il.Emit(OpCodes.Call, MouseDeltaMethod);
    }

    /// MoveAxis's own version of EmitExecMouseDelta -- see that method's comment, which applies
    /// unchanged here.
    private void EmitExecMoveAxis(Node node)
    {
        if (_il == null) return;

        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "forward", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "right", typeof(float)));
        _il.Emit(OpCodes.Call, MoveAxisMethod);
    }

    /// Pulls the value linked into `node`'s input pin `pinName`: follows a LINK into EmitPullOutput,
    /// falls back to a PINVAL, then to the pin's zero-ish default -- LoadPin's same three-step
    /// fallback, re-emitted here since the exec compiler never populates `_pinLocals` for data nodes.
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

        // ABSENT IS NOT THE SAME AS UNCONNECTED, and conflating them emitted an INVALID PROGRAM.
        // `pin == null` means the emitter asked for an input this node doesn't have -- the state ANY
        // hand-written PIN record leaves it in, since one explicit PIN suppresses every default
        // AddDefaultPins would add. The old code fell back to `pin?.Type ?? PinType.Float` and pushed
        // a FLOAT zero, so a missing `entity` (an INT) put a float32 where an int32 was expected --
        // not a wrong number but IL the runtime refuses to run, with a "Common Language Runtime
        // detected an invalid program" message naming neither node, pin, nor PIN record.
        //
        // A pin that EXISTS but has no incoming link still falls through to zero: an Add with only
        // `a` wired legally means "a + 0", and samples in this repo rely on it.
        if (pin == null)
            throw new InvalidOperationException(
                $"node '{node.Id}' ({node.Type}) has no input pin '{pinName}' to read. " +
                $"A node's default pins are suppressed entirely as soon as it declares ANY pin by hand, " +
                $"so if this node has PIN records, it needs one for '{pinName}' too (or a LINK into it)");

        if (pin.Type == PinType.Bool) _il.Emit(OpCodes.Ldc_I4_0);
        else if (pin.Type == PinType.Int) _il.Emit(OpCodes.Ldc_I4_0);
        else _il.Emit(OpCodes.Ldc_R4, 0f);
    }

    /// Pushes node `source`'s output pin `pinName` onto the IL stack, computed fresh every call (no
    /// memoization -- see the section header). Checked first against `_execLocals` (a live loop
    /// counter or captured side-effect result), then dispatched by node TYPE for pure expressions.
    /// SetField (and any IsExecCapableSideEffectType type) is refused here on purpose.
    /// True for node kinds that exist ONLY to be walked by the exec/PUSH compiler, with no data value
    /// to pull -- Compile() skips these rather than failing, since a graph may carry both halves with
    /// no interaction.
    ///
    /// NOT the same predicate as IsExecCapableSideEffectType: SetField has exec pins AND a data output
    /// someone might wrongly try to read, so it stays reachable to refuse with its own error. The
    /// kinds below have no data output at all -- nothing for the pull compiler to do but fail.
    // EVERY node type only the PUSH compiler can run: a side effect, a write, or a creation with no
    // meaning in a pure-dataflow graph.
    //
    // Was a seventeen-term disjunction inline in EmitPullOutput, needed in TWO places -- there and in
    // EmitNode's default arm, which lacked it. Not a wrong answer but a wrong SENTENCE: a stray Jump,
    // PrintInt, SetGravity, or any of the eleven physics writers, compiled by Compile(), got "Node
    // type 'Jump' is not supported" -- true of neither compiler, implying there was no Jump node at all.
    //
    // Spawn/CharacterMove/FireEvent/SetVar/SaveGame/LoadGame keep their own hand-written cases above
    // deliberately: each says something specific and true (Spawn creates an entity EVERY TICK) that
    // folding into this generic predicate would lose. This is the floor, not the ceiling.
    private static bool IsPushOnlySideEffectType(string type) =>
        IsExecCapableSideEffectType(type) || IsExecCapableVecSideEffectType(type) ||
        IsExecCapableSpawnType(type) || IsExecCapableVarSideEffectType(type) ||
        IsExecCapableSetParentType(type) || IsExecCapableSetViewEntityType(type) ||
        IsExecCapableSetNameType(type) || IsExecCapableSetMeshType(type) ||
        IsExecCapableSetMaterialType(type) || IsExecCapableSetSkeletonType(type) ||
        IsExecCapablePlayAnimationType(type) || IsExecCapableCharacterMoveType(type) ||
        IsExecCapableFireEventType(type) || IsExecCapableJumpType(type) ||
        IsExecCapablePrintType(type) || IsExecCapableApiCallType(type) ||
        IsExecCapableTransformWriteType(type) || IsExecCapablePhysicsWriteType(type) ||
        IsExecCapablePhysicsCreateType(type) || IsExecCapableSaveLoadType(type) ||
        IsExecCapableJointCreateType(type) || IsExecCapableJointOpType(type);

    private static bool IsExecOnlyNodeType(string type) => type.ToLowerInvariant() switch
    {
        // "onhit"/"customevent" sit beside "onstart"/"ontick" purely for SHAPE -- a bare exec-output
        // trigger with no data value (OcGraphParser.AddDefaultPins), not because this compiler cares
        // when GraphHost fires them or that an event's name is author-chosen. Any future trigger-only
        // node belongs here for the same reason.
        // "funcentry" joins them too: its data outputs are the enclosing method's ARGUMENTS, which no
        // topological pass can compute -- belt-and-braces, since it only appears inside a function
        // body the event graph's pass already skips wholesale.
        "onstart" or "ontick" or "onhit" or "customevent" or "branch" or "sequence" or "while" or "foreach"
            or "funcentry" or "switchint" => true,
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

        // Generalised from a SetField-only message the moment a SECOND side-effecting type
        // (SetFieldVec3) existed -- naming source.Type keeps this accurate for whichever type
        // triggered it, and for any future type IsPushOnlySideEffectType grows to cover. Spawn and
        // SetVar were added unchanged -- only a new predicate name in the condition, proving the
        // point of naming source.Type instead of hardcoding one (see
        // TestSetVarPulledWithoutExecVisitFailsClearly).
        if (IsPushOnlySideEffectType(source.Type))
            throw new InvalidOperationException(
                $"'{source.Id}.{pinName}' cannot be read as a data value: {source.Type} has a side effect " +
                "and must be reached by wiring it directly into the exec chain (give it exec pins), not " +
                "by pulling its output from somewhere else -- pulling could run its write more than once, " +
                "or not at all, depending on what else reads it");

        // See _pullVisiting's declaration for why an uncaught cycle here kills the process rather
        // than raising something catchable; reported as an ordinary error naming both ends.
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
            // ---- scalar operators, PULL path (see the PUSH arm) ---------------------
            case "not": case "abs": case "negate":
            case "sqrt": case "floor": case "ceil":
            case "round": case "saturate":
                EmitPullInput(source, "a"); EmitScalarOp(source.Type); return;
            case "and": case "or":
            case "xor": case "greater":
            case "greaterequal": case "less":
            case "lessequal": case "equal":
            case "notequal": case "min": case "max":
            case "mod": case "pow":
                EmitPullInput(source, "a"); EmitPullInput(source, "b");
                EmitScalarOp(source.Type); return;
            case "clamp":
                EmitPullInput(source, "a"); EmitPullInput(source, "min");
                EmitPullInput(source, "max"); EmitScalarOp(source.Type); return;
            case "lerp":
                EmitPullInput(source, "a"); EmitPullInput(source, "b");
                EmitPullInput(source, "t"); EmitScalarOp(source.Type); return;
            case "add":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Add); return;
            case "multiply":
                EmitPullInput(source, "a"); EmitPullInput(source, "b"); _il.Emit(OpCodes.Mul); return;
            case "vecadd":
            case "vecsub":
            case "vecscale":
            case "veccross":
            case "vecnormalize":
            case "veclerp":
            case "vecdot":
            case "veclength":
            case "vecdistance":
                EmitVecComponent(source, pinName); return;
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
            case "getfieldvec3":
                EmitPullGetFieldVec3(source, pinName); return;
            case "getforward":
            case "get_forward":
                EmitPullGetForward(source, pinName); return;
            case "getviewentity":
            case "get_view_entity":
                EmitPullGetViewEntity(source, pinName); return;
            case "getvelocity":
                EmitPullGetVelocity(source, pinName); return;
            // Inside a function body only. Both are legal to PULL: an argument is always available,
            // and a pure call is by definition safe to evaluate whenever a reader asks.
            case "funcentry":
                EmitFuncEntryLoad(pinName); return;
            case "callfunc":
                EmitPullCallFunc(source, pinName); return;
            // A data reroute IS its input. No instruction of its own, in either compiler.
            case "reroutefloat":
            case "rerouteint":
            case "reroutebool":
                EmitPullInput(source, "a"); return;
            case "getbodyposition":
                EmitPullVec3Read(source, pinName, BodyPositionMethod, -1, "body"); return;
            case "getbodyvelocity":
                EmitPullVec3Read(source, pinName, BodyVelocityMethod, -1, "body"); return;
            case "isbodyvalid":
                EmitPullInput(source, "body"); _il.Emit(OpCodes.Call, BodyValidMethod); return;
            case "getbodycount":
                _il.Emit(OpCodes.Call, BodyCountMethod); return;
            case "getbodyangularvelocity":
                EmitPullVec3Read(source, pinName, BodyAngularVelocityMethod, -1, "body"); return;
            case "isbodyactive":
                EmitPullInput(source, "body"); _il.Emit(OpCodes.Call, BodyActiveMethod); return;
            case "getbodymass":
                EmitPullScalarRead(source, pinName, BodyMassMethod, typeof(float), "body"); return;
            case "getbodymotiontype":
                EmitPullScalarRead(source, pinName, BodyMotionTypeMethod, typeof(int), "body"); return;
            case "getbodylayer":
                EmitPullScalarRead(source, pinName, BodyLayerMethod, typeof(int), "body"); return;
            case "getjointvalue":
                EmitPullScalarRead(source, pinName, JointValueMethod, typeof(float), "joint"); return;
            case "isphysicsready":
                _il.Emit(OpCodes.Call, PhysicsReadyMethod); return;
            case "getfixedstep":
                _il.Emit(OpCodes.Call, PhysicsFixedStepMethod); return;
            case "findentity": {
                // name= is edit-time data, so it is pushed as an Ldstr literal exactly as SetName's
                // own emit does -- there is no string PIN it could arrive on.
                if (string.IsNullOrEmpty(source.NameValue))
                    throw new InvalidOperationException(
                        $"FindEntity node '{source.Id}' has no name= attribute naming the entity to look up");
                _il.Emit(OpCodes.Ldstr, source.NameValue);
                var foundL = _il.DeclareLocal(typeof(int));
                _il.Emit(OpCodes.Ldloca, foundL);
                _il.Emit(OpCodes.Call, FindEntityMethod);
                if (pinName == "found") return;   // the bool return IS that pin
                _il.Emit(OpCodes.Pop);
                _il.Emit(OpCodes.Ldloc, foundL);
                return;
            }
            case "raycastany":
                EmitPullInput(source, "originX");
                EmitPullInput(source, "originY");
                EmitPullInput(source, "originZ");
                EmitPullInput(source, "dirX");
                EmitPullInput(source, "dirY");
                EmitPullInput(source, "dirZ");
                EmitPullInput(source, "maxDist");
                _il.Emit(OpCodes.Call, RaycastAnyMethod); return;
            case "getworldposition":
                EmitPullVec3Read(source, pinName, WorldPositionMethod, -1); return;
            case "getentityforward":
                EmitPullVec3Read(source, pinName, EntityAxisMethod, 0); return;
            case "getentityright":
                EmitPullVec3Read(source, pinName, EntityAxisMethod, 1); return;
            case "getentityup":
                EmitPullVec3Read(source, pinName, EntityAxisMethod, 2); return;
            case "getlocalscale":
                EmitPullVec3Read(source, pinName, LocalScaleMethod, -1); return;
            case "getsynapsetarget":
                EmitPullVec3Read(source, pinName, SynapseGetTargetMethod, -1); return;
            case "synapsesteer":
                EmitPullSynapseSteer(source, pinName); return;
            case "getsynapseperception":
                EmitPullSynapsePerception(source, pinName); return;
            case "issoundplaying":
                EmitPullInput(source, "voice");
                _il.Emit(OpCodes.Call, IsSoundPlayingMethod); return;
            case "isalive":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, IsAliveMethod); return;
            case "isactor":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, IsActorMethod); return;
            // int and bool are both I4 on the CIL stack, so widening either to a float is the same
            // one instruction -- the node types differ so the GRAPH can tell them apart, not the IL.
            case "inttofloat":
            case "booltofloat":
                EmitPullInput(source, "a"); _il.Emit(OpCodes.Conv_R4); return;
            case "floattoint":
                EmitPullInput(source, "a"); _il.Emit(OpCodes.Conv_I4); return;
            case "isgrounded":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, IsGroundedMethod); return;
            case "hastag":
                EmitPullInput(source, "entity"); EmitPullInput(source, "mask");
                _il.Emit(OpCodes.Call, HasTagMethod); return;
            case "gettags":
                EmitPullInput(source, "entity"); _il.Emit(OpCodes.Call, TagsMethod); return;
            case "getplayerpawn":
                EmitPullInput(source, "index"); _il.Emit(OpCodes.Call, PlayerPawnMethod); return;
            case "getplayercontroller":
                EmitPullInput(source, "index"); _il.Emit(OpCodes.Call, PlayerControllerMethod); return;
            case "getgamemode":
                _il.Emit(OpCodes.Call, GameModeMethod); return;
            case "isplaying":
                _il.Emit(OpCodes.Call, IsPlayingMethod); return;
            case "getvar":
                EmitPullGetVar(source); return;
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
            case "inputkeypressed":
                EmitPullInput(source, "key"); _il.Emit(OpCodes.Call, InputKeyPressedMethod); return;
            case "inputkeyreleased":
                EmitPullInput(source, "key"); _il.Emit(OpCodes.Call, InputKeyReleasedMethod); return;
            case "inputaction":
                EmitPullInputAction(source, pinName); return;
            case "inputactionpressed":
                EmitPullInput(source, "action"); _il.Emit(OpCodes.Call, ActionPressedMethod); return;
            case "inputactionreleased":
                EmitPullInput(source, "action"); _il.Emit(OpCodes.Call, ActionReleasedMethod); return;
            // GetAnimCurve DOES get a standalone pull path, unlike Raycast/MouseDelta/MoveAxis below:
            // those three must run exactly ONCE however many pins are read, so behaving differently on
            // and off the exec chain would be a trap. A curve read has neither property -- one output,
            // no side effect, same number on re-read since the playhead doesn't move between pulls.
            case "getanimcurve":
                if (string.IsNullOrEmpty(source.CurveName))
                    throw new InvalidOperationException(
                        $"GetAnimCurve node '{source.Id}' has no curve= attribute naming which curve to read");
                EmitPullInput(source, "entity");
                _il.Emit(OpCodes.Ldstr, source.CurveName);
                _il.Emit(OpCodes.Call, GetAnimCurveMethod);
                return;
            case "select":
            {
                // Mirrors EmitSelect's branch shape but PULLED (recursive, uncached), not stored to
                // _pinLocals -- see the section header for why the exec compiler re-emits rather than
                // caches. UNLIKE EmitSelect's PULL-compiler branch (which only skips which local gets
                // LOADED, since both arms already ran during the topological walk), this IS a real
                // short-circuit: EmitPullInput recursively runs only the chosen arm's subgraph, so
                // only ONE of ifTrue/ifFalse's cost is paid per pull.
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
            // "raycast" (and "mousedelta"/"moveaxis", same reason) has NO case here, deliberately.
            // Reached VIA THE EXEC CHAIN, Raycast populates _execLocals for all five outputs
            // (EmitExecRaycast), and this method checks _execLocals before the switch runs, so a
            // visited Raycast needs no dispatch. One never visited by exec falls to `default` and
            // reports a clear NotSupportedException -- a deliberate Phase-1 limitation: unlike
            // GetField, Raycast has no standalone pull path, because one would let a node type behave
            // differently on and off the exec chain, a worse trap than a clear error. A pure-pull
            // graph (no ENTRY) never reaches this method at all -- EmitRaycast (PULL) handles it.
            // MouseDelta/MoveAxis inherit this shape for consistency, not cost.
            default:
                throw new NotSupportedException(
                    $"node type '{source.Type}' cannot be pulled as a data value inside an exec chain " +
                    "(it is not one of the pure expression kinds this compiler knows, and has no exec " +
                    "pins reaching it directly either)");
        }

        }
        finally
        {
            // In finally, not after the switch, since EVERY arm above returns or throws (no
            // fallthrough) -- removing on the way out is what keeps a diamond legal while a cycle isn't.
            _pullVisiting.Remove(source.Id);
        }
    }

    /// PULL half of EmitInputAction -- mirrors EmitPullVec3Read's "recompute per reader" shape rather
    /// than caching: an "x"/"y" pull re-runs aver_fw_action_value2 in full and discards the unwanted
    /// half; "held" runs the separate aver_fw_action_held call -- pulling all three costs THREE native
    /// calls. Safe since both ABI calls are pure, idempotent array-scan reads (see EmitInputAction).
    private void EmitPullInputAction(Node source, string pinName)
    {
        if (_il == null) return;

        if (pinName == "held")
        {
            EmitPullInput(source, "action");
            _il.Emit(OpCodes.Call, ActionHeldMethod);
            return;
        }

        // x/y: same Newarr + Dup shape EmitInputAction uses -- see that method's own comment for why
        // aver_fw_action_value2's `float[]` out-parameter needs it instead of a plain Ldloca address.
        EmitPullInput(source, "action");
        _il.Emit(OpCodes.Ldc_I4_2);
        _il.Emit(OpCodes.Newarr, typeof(float));
        var arrLocal = _il.DeclareLocal(typeof(float[]));
        _il.Emit(OpCodes.Dup);
        _il.Emit(OpCodes.Stloc, arrLocal);
        _il.Emit(OpCodes.Call, ActionValue2Method);

        _il.Emit(OpCodes.Ldloc, arrLocal);
        _il.Emit(OpCodes.Ldc_I4, pinName == "x" ? 0 : 1);
        _il.Emit(OpCodes.Ldelem_R4);
    }

    /// Mirrors EmitDivide's zero-divisor convention (b == 0 yields 0.0, never NaN/Infinity -- see
    /// that comment), but pushes the result rather than storing it to `_pinLocals`.
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

    /// Mirrors EmitGetFieldVec3's field=/RequireVec3Field checks but pushes ONE requested component
    /// rather than storing all three to `_pinLocals` -- EmitPullOutput's contract is "push the ONE pin
    /// asked for", and GetFieldVec3 has three (x/y/z) unlike GetField's one.
    ///
    /// NOT FREE: this makes ONE full native call (all three components) and discards the two unasked,
    /// every time it runs -- reading x, y, AND z via three separate readers costs three native calls
    /// for one logical field. Just the "shared sub-expression recomputed, not cached" tradeoff PUSH VS
    /// PULL point 1 already accepts, not a new cost. Deliberately NOT given Raycast's _execLocals
    /// caching: that exists because a PHYSICS QUERY is expensive (IsExecCapableQueryType); a Vec3 read
    /// is cost-equal to GetField's single-float read, so a cache here would solve a non-problem.
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
    // Aver.Scene.csproj (which only lets GraphCompiler.cs name the `Native` TYPE at compile time --
    // GetMethod finds an internal method either way).
    private static readonly MethodInfo GetFieldMethod =
        typeof(Native).GetMethod("aver_scene_get_f32", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_get_f32 was not found by reflection");
    private static readonly MethodInfo SetFieldMethod =
        typeof(Native).GetMethod("aver_scene_set_f32", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Scene.Native.aver_scene_set_f32 was not found by reflection");
    // ONE EMITTER, CALLED FROM BOTH COMPILERS: the arithmetic between "operands on the stack" and
    // "result on the stack" is identical whether PUSH stores it to a local or PULL re-emits it at
    // every use, so it lives once here. Adding an operator to one compiler and not the other is the
    // recurring defect shape in this file.
    // The PUSH-path wrappers: load each input from its pin local, run the shared arithmetic, store
    // the result. Split by arity, not one variadic helper, so a wrong-arity node fails at compile
    // time in the emitter it named, rather than silently reading a stale stack slot.
    private void EmitScalarUnary(Node node)
    {
        if (_il == null) return;
        LoadPin(node.Id, "a");
        EmitScalarOp(node.Type);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local)) _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitScalarBinary(Node node)
    {
        if (_il == null) return;
        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        EmitScalarOp(node.Type);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local)) _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitScalarTernary(Node node)
    {
        if (_il == null) return;
        if (node.Type == "clamp")
        {
            LoadPin(node.Id, "a"); LoadPin(node.Id, "min"); LoadPin(node.Id, "max");
        }
        else
        {
            LoadPin(node.Id, "a"); LoadPin(node.Id, "b"); LoadPin(node.Id, "t");
        }
        EmitScalarOp(node.Type);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local)) _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitScalarOp(string type)
    {
        if (_il == null) return;
        // LOWERCASED HERE, not by callers: Node.Type keeps whatever case the .ocgraph wrote (palette
        // emits "Lerp", hand-written may say "lerp"), and only AddDefaultPins was normalising. Both
        // compilers hand this the raw type, so doing it once here stops "Lerp" compiling and "lerp" not.
        switch (type.ToLowerInvariant())
        {
            // Bools are I4 on the stack, so bitwise ops ARE logical ops -- every producer of a Bool
            // pin emits Ceq/Cgt/Clt or a const, guaranteeing 0 or 1, never an arbitrary integer.
            case "and": _il.Emit(OpCodes.And); break;
            case "or":   _il.Emit(OpCodes.Or); break;
            case "xor": _il.Emit(OpCodes.Xor); break;
            // NOT is "== 0", not a bitwise complement: ~1 is -2, which is truthy everywhere it
            // would later be tested.
            case "not": _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;

            case "greater": _il.Emit(OpCodes.Cgt); break;
            case "less":    _il.Emit(OpCodes.Clt); break;
            case "equal":   _il.Emit(OpCodes.Ceq); break;
            // CIL has no Cge/Cle/Cne, so each is the opposite comparison inverted by "== 0".
            case "greaterequal":
                _il.Emit(OpCodes.Clt); _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;
            case "lessequal":
                _il.Emit(OpCodes.Cgt); _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;
            case "notequal":
                _il.Emit(OpCodes.Ceq); _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;

            case "min": _il.Emit(OpCodes.Call, MathMinMethod); break;
            case "max": _il.Emit(OpCodes.Call, MathMaxMethod); break;
            case "mod": _il.Emit(OpCodes.Rem); break;
            case "abs": _il.Emit(OpCodes.Call, MathAbsMethod); break;
            case "negate": _il.Emit(OpCodes.Neg); break;
            case "pow":
                // Two operands, so BOTH need widening -- and the stack order forbids simply
                // converting the top one twice. Round-trip through locals rather than guess.
                {
                    var pb = _il.DeclareLocal(typeof(float));
                    _il.Emit(OpCodes.Stloc, pb);
                    _il.Emit(OpCodes.Conv_R8);
                    _il.Emit(OpCodes.Ldloc, pb);
                    _il.Emit(OpCodes.Conv_R8);
                    _il.Emit(OpCodes.Call, MathPowMethod);
                    _il.Emit(OpCodes.Conv_R4);
                }
                break;
            case "sqrt":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathSqrtMethod); _il.Emit(OpCodes.Conv_R4); break;
            case "floor":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathFloorMethod); _il.Emit(OpCodes.Conv_R4); break;
            case "ceil":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathCeilMethod); _il.Emit(OpCodes.Conv_R4); break;
            case "round":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathRoundMethod); _il.Emit(OpCodes.Conv_R4); break;
            // saturate is clamp(x, 0, 1), spelled with the same Min/Max the clamp node uses so
            // the two cannot disagree about edge behaviour.
            case "saturate":
                _il.Emit(OpCodes.Ldc_R4, 1.0f); _il.Emit(OpCodes.Call, MathMinMethod);
                _il.Emit(OpCodes.Ldc_R4, 0.0f); _il.Emit(OpCodes.Call, MathMaxMethod); break;
            // clamp takes THREE operands (a, min, max) already on the stack in that order.
            case "clamp":
                {
                    var hi = _il.DeclareLocal(typeof(float));
                    _il.Emit(OpCodes.Stloc, hi);              // stack: a, min
                    _il.Emit(OpCodes.Call, MathMaxMethod);    // stack: max(a, min)
                    _il.Emit(OpCodes.Ldloc, hi);
                    _il.Emit(OpCodes.Call, MathMinMethod);
                }
                break;
            // lerp(a, b, t) = a + (b - a) * t, with a, b, t on the stack in that order.
            case "lerp":
                {
                    var lt = _il.DeclareLocal(typeof(float));
                    var lb = _il.DeclareLocal(typeof(float));
                    var la = _il.DeclareLocal(typeof(float));
                    _il.Emit(OpCodes.Stloc, lt);
                    _il.Emit(OpCodes.Stloc, lb);
                    _il.Emit(OpCodes.Stloc, la);
                    _il.Emit(OpCodes.Ldloc, la);
                    _il.Emit(OpCodes.Ldloc, lb);
                    _il.Emit(OpCodes.Ldloc, la);
                    _il.Emit(OpCodes.Sub);
                    _il.Emit(OpCodes.Ldloc, lt);
                    _il.Emit(OpCodes.Mul);
                    _il.Emit(OpCodes.Add);
                }
                break;
            default:
                throw new InvalidOperationException($"EmitScalarOp has no arithmetic for '{type}'");
        }
    }

    private static readonly MethodInfo MathSinMethod =
        typeof(Math).GetMethod(nameof(Math.Sin), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Sin(double) was not found by reflection");
    private static readonly MethodInfo MathCosMethod =
        typeof(Math).GetMethod(nameof(Math.Cos), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Cos(double) was not found by reflection");
    // Double-precision System.Math entry points the scalar nodes call; each call site brackets with
    // Conv_R8/Conv_R4 since the graph value type is float throughout. Sin/Cos below predate these and
    // use the identical shape.
    private static readonly MethodInfo MathMinMethod =
        typeof(Math).GetMethod(nameof(Math.Min), new[] { typeof(float), typeof(float) })
        ?? throw new InvalidOperationException("System.Math.Min(float,float) was not found by reflection");
    private static readonly MethodInfo MathMaxMethod =
        typeof(Math).GetMethod(nameof(Math.Max), new[] { typeof(float), typeof(float) })
        ?? throw new InvalidOperationException("System.Math.Max(float,float) was not found by reflection");
    private static readonly MethodInfo MathAbsMethod =
        typeof(Math).GetMethod(nameof(Math.Abs), new[] { typeof(float) })
        ?? throw new InvalidOperationException("System.Math.Abs(float) was not found by reflection");
    private static readonly MethodInfo MathSqrtMethod =
        typeof(Math).GetMethod(nameof(Math.Sqrt), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Sqrt(double) was not found by reflection");
    private static readonly MethodInfo MathPowMethod =
        typeof(Math).GetMethod(nameof(Math.Pow), new[] { typeof(double), typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Pow(double,double) was not found by reflection");
    private static readonly MethodInfo MathFloorMethod =
        typeof(Math).GetMethod(nameof(Math.Floor), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Floor(double) was not found by reflection");
    private static readonly MethodInfo MathCeilMethod =
        typeof(Math).GetMethod(nameof(Math.Ceiling), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Ceiling(double) was not found by reflection");
    private static readonly MethodInfo MathRoundMethod =
        typeof(Math).GetMethod(nameof(Math.Round), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Round(double) was not found by reflection");
    private static readonly MethodInfo WarnLoopGuardMethod =
        typeof(GraphCompiler).GetMethod(nameof(WarnLoopGuardTripped), BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("GraphCompiler.WarnLoopGuardTripped was not found by reflection");
    // Aver.Framework internals, reached the same way as Aver.Scene's Native above -- see
    // Aver.Framework.csproj's InternalsVisibleTo("Aver.Graph") grant.
    private static readonly MethodInfo InputKeyMethod =
        typeof(Fw).GetMethod("aver_fw_input_key", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key was not found by reflection");
    // The edge-triggered pair beside aver_fw_input_key -- separate ABI entry points because that's
    // how the framework exposes them; a selector flag here would duplicate "pressed" logic.
    private static readonly MethodInfo InputKeyPressedMethod =
        typeof(Fw).GetMethod("aver_fw_input_key_pressed", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key_pressed was not found by reflection");
    private static readonly MethodInfo InputKeyReleasedMethod =
        typeof(Fw).GetMethod("aver_fw_input_key_released", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_input_key_released was not found by reflection");
    // InputAction's own twin of aver_fw_input_key/_pressed/_released above -- see EmitInputAction's own
    // comment for why "action" is a HANDLE (an int the caller already holds), not a name.
    private static readonly MethodInfo ActionValue2Method =
        typeof(Fw).GetMethod("aver_fw_action_value2", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_value2 was not found by reflection");
    private static readonly MethodInfo ActionHeldMethod =
        typeof(Fw).GetMethod("aver_fw_action_held", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_held was not found by reflection");
    private static readonly MethodInfo ActionPressedMethod =
        typeof(Fw).GetMethod("aver_fw_action_pressed", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_pressed was not found by reflection");
    private static readonly MethodInfo ActionReleasedMethod =
        typeof(Fw).GetMethod("aver_fw_action_released", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.Fw.aver_fw_action_released was not found by reflection");
    private static readonly MethodInfo RaycastMethod =
        typeof(GraphInterop).GetMethod("RaycastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RaycastForGraph was not found by reflection");
    private static readonly MethodInfo GetFieldVecMethod =
        typeof(GraphInterop).GetMethod("GetFieldVecForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GetFieldVecForGraph was not found by reflection");
    private static readonly MethodInfo SetFieldVecMethod =
        typeof(GraphInterop).GetMethod("SetFieldVecForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetFieldVecForGraph was not found by reflection");
    private static readonly MethodInfo SpawnMethod =
        typeof(GraphInterop).GetMethod("SpawnForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SpawnForGraph was not found by reflection");
    private static readonly MethodInfo MouseDeltaMethod =
        typeof(GraphInterop).GetMethod("MouseDeltaForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.MouseDeltaForGraph was not found by reflection");
    private static readonly MethodInfo MoveAxisMethod =
        typeof(GraphInterop).GetMethod("MoveAxisForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.MoveAxisForGraph was not found by reflection");
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
    // AttachToSocket, reflected the same way -- Entity.AttachToSocket needs Entity's internal
    // constructor, so the call has to enter through Aver.Framework rather than from here.
    private static readonly MethodInfo AttachToSocketMethod =
        typeof(GraphInterop).GetMethod("AttachToSocketForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AttachToSocketForGraph was not found by reflection");
    private static readonly MethodInfo GetAnimCurveMethod =
        typeof(GraphInterop).GetMethod("GetAnimCurveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.GetAnimCurveForGraph was not found by reflection");
    // CharacterMove: the last Blueprint-parity node, reflected like every other GraphInterop wrapper
    // above -- see GraphInterop.CharacterMoveForGraph for why this call, not a cast, is the seam.
    private static readonly MethodInfo CharacterMoveMethod =
        typeof(GraphInterop).GetMethod("CharacterMoveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.CharacterMoveForGraph was not found by reflection");
    // GetForward: CharacterMove's read-side counterpart -- where the character is LOOKING, unaskable
    // before (_yaw/_pitch are private; the scene rotation is a Quat GetField/GetFieldVec3 can't read).
    private static readonly MethodInfo LookDirectionMethod =
        typeof(GraphInterop).GetMethod("LookDirectionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.LookDirectionForGraph was not found by reflection");
    // GetViewEntity: the camera node a character looks through -- what a viewmodel parents to.
    // The physics surfaces.
    private static readonly MethodInfo BodyPositionMethod =
        typeof(GraphInterop).GetMethod("BodyPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyPositionForGraph was not found by reflection");
    private static readonly MethodInfo BodyVelocityMethod =
        typeof(GraphInterop).GetMethod("BodyVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyVelocityForGraph was not found by reflection");
    private static readonly MethodInfo BodyValidMethod =
        typeof(GraphInterop).GetMethod("BodyValidForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyValidForGraph was not found by reflection");
    private static readonly MethodInfo BodyCountMethod =
        typeof(GraphInterop).GetMethod("BodyCountForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyCountForGraph was not found by reflection");
    private static readonly MethodInfo PlaySoundMethod =
        typeof(GraphInterop).GetMethod("PlaySoundForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlaySoundForGraph was not found by reflection");
    private static readonly MethodInfo PlaySoundAtMethod =
        typeof(GraphInterop).GetMethod("PlaySoundAtForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlaySoundAtForGraph was not found by reflection");
    private static readonly MethodInfo StopSoundMethod =
        typeof(GraphInterop).GetMethod("StopSoundForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.StopSoundForGraph was not found by reflection");
    private static readonly MethodInfo IsSoundPlayingMethod =
        typeof(GraphInterop).GetMethod("IsSoundPlayingForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsSoundPlayingForGraph was not found by reflection");
    private static readonly MethodInfo SetListenerMethod =
        typeof(GraphInterop).GetMethod("SetListenerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetListenerForGraph was not found by reflection");
    private static readonly MethodInfo SetBusVolumeMethod =
        typeof(GraphInterop).GetMethod("SetBusVolumeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBusVolumeForGraph was not found by reflection");
    private static readonly MethodInfo CreateEntityMethod =
        typeof(GraphInterop).GetMethod("CreateEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.CreateEntityForGraph was not found by reflection");
    private static readonly MethodInfo FindEntityMethod =
        typeof(GraphInterop).GetMethod("FindEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.FindEntityForGraph was not found by reflection");
    private static readonly MethodInfo PhysicsReadyMethod =
        typeof(GraphInterop).GetMethod("PhysicsReadyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PhysicsReadyForGraph was not found by reflection");
    private static readonly MethodInfo PhysicsFixedStepMethod =
        typeof(GraphInterop).GetMethod("PhysicsFixedStepForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PhysicsFixedStepForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyPositionMethod =
        typeof(GraphInterop).GetMethod("SetBodyPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyPositionForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyVelocityMethod =
        typeof(GraphInterop).GetMethod("SetBodyVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyVelocityForGraph was not found by reflection");
    private static readonly MethodInfo AddBodyVelocityMethod =
        typeof(GraphInterop).GetMethod("AddBodyVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddBodyVelocityForGraph was not found by reflection");
    private static readonly MethodInfo DestroyBodyMethod =
        typeof(GraphInterop).GetMethod("DestroyBodyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.DestroyBodyForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyEntityMethod =
        typeof(GraphInterop).GetMethod("SetBodyEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyEntityForGraph was not found by reflection");
    private static readonly MethodInfo SetGravityMethod =
        typeof(GraphInterop).GetMethod("SetGravityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetGravityForGraph was not found by reflection");
    private static readonly MethodInfo AddStaticBoxMethod =
        typeof(GraphInterop).GetMethod("AddStaticBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddStaticBoxForGraph was not found by reflection");
    private static readonly MethodInfo AddDynamicBoxMethod =
        typeof(GraphInterop).GetMethod("AddDynamicBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddDynamicBoxForGraph was not found by reflection");
    private static readonly MethodInfo AddDynamicSphereMethod =
        typeof(GraphInterop).GetMethod("AddDynamicSphereForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddDynamicSphereForGraph was not found by reflection");
    private static readonly MethodInfo AddSensorBoxMethod =
        typeof(GraphInterop).GetMethod("AddSensorBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddSensorBoxForGraph was not found by reflection");
    private static readonly MethodInfo AddSensorSphereMethod =
        typeof(GraphInterop).GetMethod("AddSensorSphereForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddSensorSphereForGraph was not found by reflection");
    private static readonly MethodInfo RaycastAnyMethod =
        typeof(GraphInterop).GetMethod("RaycastAnyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RaycastAnyForGraph was not found by reflection");
    private static readonly MethodInfo SphereCastMethod =
        typeof(GraphInterop).GetMethod("SphereCastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SphereCastForGraph was not found by reflection");
    // Forces, impulses, spin, material, mass, motion type, layers -- all body-keyed, resolved by name
    // up front like every MethodInfo above: a lookup naming a nonexistent method throws HERE, at
    // static init, breaking every graph in the process, not just the one using the new node.
    private static readonly MethodInfo AddForceMethod =
        typeof(GraphInterop).GetMethod("AddForceForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddForceForGraph was not found by reflection");
    private static readonly MethodInfo AddImpulseMethod =
        typeof(GraphInterop).GetMethod("AddImpulseForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddImpulseForGraph was not found by reflection");
    private static readonly MethodInfo AddTorqueMethod =
        typeof(GraphInterop).GetMethod("AddTorqueForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddTorqueForGraph was not found by reflection");
    private static readonly MethodInfo AddAngularImpulseMethod =
        typeof(GraphInterop).GetMethod("AddAngularImpulseForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddAngularImpulseForGraph was not found by reflection");
    private static readonly MethodInfo BodyAngularVelocityMethod =
        typeof(GraphInterop).GetMethod("BodyAngularVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyAngularVelocityForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyAngularVelocityMethod =
        typeof(GraphInterop).GetMethod("SetBodyAngularVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyAngularVelocityForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyFrictionMethod =
        typeof(GraphInterop).GetMethod("SetBodyFrictionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyFrictionForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyRestitutionMethod =
        typeof(GraphInterop).GetMethod("SetBodyRestitutionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyRestitutionForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyGravityFactorMethod =
        typeof(GraphInterop).GetMethod("SetBodyGravityFactorForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyGravityFactorForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyMassMethod =
        typeof(GraphInterop).GetMethod("SetBodyMassForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyMassForGraph was not found by reflection");
    private static readonly MethodInfo BodyMassMethod =
        typeof(GraphInterop).GetMethod("BodyMassForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyMassForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyMotionTypeMethod =
        typeof(GraphInterop).GetMethod("SetBodyMotionTypeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyMotionTypeForGraph was not found by reflection");
    private static readonly MethodInfo BodyMotionTypeMethod =
        typeof(GraphInterop).GetMethod("BodyMotionTypeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyMotionTypeForGraph was not found by reflection");
    private static readonly MethodInfo ActivateBodyMethod =
        typeof(GraphInterop).GetMethod("ActivateBodyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.ActivateBodyForGraph was not found by reflection");
    private static readonly MethodInfo BodyActiveMethod =
        typeof(GraphInterop).GetMethod("BodyActiveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyActiveForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyLayerMethod =
        typeof(GraphInterop).GetMethod("SetBodyLayerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyLayerForGraph was not found by reflection");
    private static readonly MethodInfo BodyLayerMethod =
        typeof(GraphInterop).GetMethod("BodyLayerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyLayerForGraph was not found by reflection");
    private static readonly MethodInfo SetLayerCollisionMethod =
        typeof(GraphInterop).GetMethod("SetLayerCollisionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLayerCollisionForGraph was not found by reflection");
    // Joints.
    private static readonly MethodInfo JointFixedMethod =
        typeof(GraphInterop).GetMethod("JointFixedForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointFixedForGraph was not found by reflection");
    private static readonly MethodInfo JointPointMethod =
        typeof(GraphInterop).GetMethod("JointPointForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointPointForGraph was not found by reflection");
    private static readonly MethodInfo JointDistanceMethod =
        typeof(GraphInterop).GetMethod("JointDistanceForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointDistanceForGraph was not found by reflection");
    private static readonly MethodInfo JointHingeMethod =
        typeof(GraphInterop).GetMethod("JointHingeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointHingeForGraph was not found by reflection");
    private static readonly MethodInfo JointSliderMethod =
        typeof(GraphInterop).GetMethod("JointSliderForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSliderForGraph was not found by reflection");
    private static readonly MethodInfo JointSetMotorMethod =
        typeof(GraphInterop).GetMethod("JointSetMotorForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSetMotorForGraph was not found by reflection");
    private static readonly MethodInfo JointSetEnabledMethod =
        typeof(GraphInterop).GetMethod("JointSetEnabledForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSetEnabledForGraph was not found by reflection");
    private static readonly MethodInfo JointRemoveMethod =
        typeof(GraphInterop).GetMethod("JointRemoveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointRemoveForGraph was not found by reflection");
    private static readonly MethodInfo JointValueMethod =
        typeof(GraphInterop).GetMethod("JointValueForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointValueForGraph was not found by reflection");
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
    private static readonly MethodInfo SynapseGetTargetMethod =
        typeof(GraphInterop).GetMethod("SynapseGetTargetForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SynapseGetTargetForGraph was not found by reflection");
    private static readonly MethodInfo SynapseSteerMethod =
        typeof(GraphInterop).GetMethod("SynapseSteerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SynapseSteerForGraph was not found by reflection");
    private static readonly MethodInfo SynapseGetPerceptionMethod =
        typeof(GraphInterop).GetMethod("SynapseGetPerceptionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SynapseGetPerceptionForGraph was not found by reflection");
    private static readonly MethodInfo TranslateMethod =
        typeof(GraphInterop).GetMethod("TranslateForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.TranslateForGraph was not found by reflection");
    private static readonly MethodInfo SetLocalScaleMethod =
        typeof(GraphInterop).GetMethod("SetLocalScaleForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLocalScaleForGraph was not found by reflection");

    private static readonly MethodInfo SetLocalPositionMethod =
        typeof(GraphInterop).GetMethod("SetLocalPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLocalPositionForGraph was not found by reflection");
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
    private static readonly MethodInfo SetVelocityMethod =
        typeof(GraphInterop).GetMethod("SetVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetVelocityForGraph was not found by reflection");
    private static readonly MethodInfo IsGroundedMethod =
        typeof(GraphInterop).GetMethod("IsGroundedForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsGroundedForGraph was not found by reflection");

    // The tag/visibility family. Bound by REFLECTION like every other interop method here, which is
    // why the throw names the member: a rename on the Aver.Framework side would otherwise surface as
    // a null MethodInfo inside an emitter, thousands of lines from the cause.
    private static readonly MethodInfo SetVisibleMethod =
        typeof(GraphInterop).GetMethod("SetVisibleForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetVisibleForGraph was not found by reflection");
    private static readonly MethodInfo AddTagMethod =
        typeof(GraphInterop).GetMethod("AddTagForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddTagForGraph was not found by reflection");
    private static readonly MethodInfo RemoveTagMethod =
        typeof(GraphInterop).GetMethod("RemoveTagForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RemoveTagForGraph was not found by reflection");
    private static readonly MethodInfo HasTagMethod =
        typeof(GraphInterop).GetMethod("HasTagForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.HasTagForGraph was not found by reflection");
    private static readonly MethodInfo TagsMethod =
        typeof(GraphInterop).GetMethod("TagsForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.TagsForGraph was not found by reflection");
    private static readonly MethodInfo TeleportMethod =
        typeof(GraphInterop).GetMethod("TeleportForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.TeleportForGraph was not found by reflection");
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
    private static readonly MethodInfo PossessMethod =
        typeof(GraphInterop).GetMethod("PossessForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PossessForGraph was not found by reflection");
    private static readonly MethodInfo UnpossessMethod =
        typeof(GraphInterop).GetMethod("UnpossessForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.UnpossessForGraph was not found by reflection");
    private static readonly MethodInfo SaveGameMethod =
        typeof(GraphInterop).GetMethod("SaveGameForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SaveGameForGraph was not found by reflection");
    private static readonly MethodInfo LoadGameMethod =
        typeof(GraphInterop).GetMethod("LoadGameForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.LoadGameForGraph was not found by reflection");
    // PrintInt: the same line for an INT pin, because a float cannot hold an entity handle.
    private static readonly MethodInfo PrintIntMethod =
        typeof(GraphInterop).GetMethod("PrintIntForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PrintIntForGraph was not found by reflection");
    // Print: one line to the log, labelled with the node id the emitter pushes.
    private static readonly MethodInfo PrintMethod =
        typeof(GraphInterop).GetMethod("PrintForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PrintForGraph was not found by reflection");
    // Jump: one call into AverCharacter.Jump, which declines in mid-air on its own.
    private static readonly MethodInfo JumpMethod =
        typeof(GraphInterop).GetMethod("JumpForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JumpForGraph was not found by reflection");
    private static readonly MethodInfo ViewEntityMethod =
        typeof(GraphInterop).GetMethod("ViewEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.ViewEntityForGraph was not found by reflection");
    // FireEvent: GAP 3, the cross-entity event node -- reflected differently from every wrapper
    // above. GraphEvents lives in THIS SAME ASSEMBLY (Aver.Graph) with a PUBLIC Router-dispatching
    // method (see GraphEvents.cs for why), so an ordinary public GetMethod suffices -- no
    // BindingFlags.NonPublic, the same shape the GraphVarStore accessors below use, unlike the
    // internal-member-of-a-different-assembly shape Native/Fw/GraphInterop need.
    private static readonly MethodInfo FireEventMethod =
        typeof(GraphEvents).GetMethod(nameof(GraphEvents.FireEventForGraph))
        ?? throw new InvalidOperationException("Aver.Graph.GraphEvents.FireEventForGraph was not found by reflection");

    // GraphVarStore's own typed accessors -- PUBLIC instance methods on a plain class in THIS assembly
    // (unlike Native/Fw/GraphInterop's internal members of a DIFFERENT assembly, reached only via
    // NonPublic|Static), so an ordinary public GetMethod lookup suffices.
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
        PinType.Int => VarGetIntMethod,
        PinType.Bool => VarGetBoolMethod,
        _ => throw new InvalidOperationException(
            $"VAR type {t} has no GraphVarStore read accessor -- VAR only supports Float/Int/Bool"),
    };

    /// Picks GraphVarStore's write accessor for a declared VAR's type -- EmitExecSetVar's own dispatch.
    private static MethodInfo VarSetMethodFor(PinType t) => t switch
    {
        PinType.Float => VarSetFloatMethod,
        PinType.Int => VarSetIntMethod,
        PinType.Bool => VarSetBoolMethod,
        _ => throw new InvalidOperationException(
            $"VAR type {t} has no GraphVarStore write accessor -- VAR only supports Float/Int/Bool"),
    };

    /// Called FROM EMITTED IL (EmitWhile/EmitForEach), not ordinary C# control flow, when a loop's
    /// iteration count crosses MaxLoopIterations. Logs loudly (node + event) and lets `done` run
    /// anyway, as if `cond`/`count` ran out normally, rather than throwing mid-tick: a graph bug
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
