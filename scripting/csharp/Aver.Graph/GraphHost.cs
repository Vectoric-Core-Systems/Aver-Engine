// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Hosts one compiled graph against one entity, ticked once per frame: loads a graph from disk,
// compiles it once, keeps the delegate, and feeds it a live time value every frame -- the seam
// between "the compiler works" (GraphCompiler) and "a graph moves something in the scene".
//
// PHASE 2 (event-driven graphs): originally drove only Compile()'s PULL/dataflow delegate (PARAM
// entity/time, 2-3 OUT floats as a position). Phase 1 (a9038da) added exec pins, ENTRY records and
// GraphCompiler.CompileEntryPoint(), but nothing here used that path yet -- an ENTRY-bearing graph
// would have failed here with a PARAM-shape error, or worse -- see LoadEventGraph.
// GameApp (Runtime/src/GameApp.cpp) discovers a project's .ocgraph files and drives them through the
// same ScriptHost::graphLoad/graphTick API SandboxApp's graph-driven drone uses, with a synthetic id
// standing in for "the project" (safe with no native/Aver.Scripting.Bridge change; see GameApp.cpp).
//
// PHASE 3 (on-demand events, e.g. "OnHit"): adds running a compiled entry point OUTSIDE Tick() via
// Fire(), for events a host fires when something HAPPENS rather than every frame -- CompileEntryPoint
// already handled that fine; the gap was LoadEventGraph requiring OnStart/OnTick and no method to run
// an entry outside Tick(). "OnHit" is only an illustrative example in comments -- any non-OnStart/
// OnTick event name takes this path (see LoadEventGraph).
//
// VARIABLES (graph-local persistent state): a compiled delegate used to be a pure function of its
// PARAMs, so a graph could not hold a score, ammo count or cooldown. See _varStore's field comment
// for the storage (one GraphVarStore per GraphHost, owning lifetime/passing) and GraphVarStore's own
// comment for the contract.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Aver.Scene;

namespace Aver.Graph;

/// <summary>Applies one tick's computed (x, y, z) to an entity. Needed because getfield/setfield
/// nodes only support F32-kind fields (see GraphCompiler's FieldKindF32 comment) while
/// CLocal.position is Vec3-kind, needing aver_scene_set_vec's 3-float buffer instead. The default
/// (<see cref="GraphHost.DefaultPositionSink"/>) calls that P/Invoke via Aver.Scene.Native, like
/// GraphCompiler's DefaultFieldResolver does for scalars. Tests with no live scene supply their own
/// sink instead -- see Aver.Graph.Tests.</summary>
public delegate void PositionSink(int entityId, float x, float y, float z);

/// <summary>Loads an .ocgraph, compiles it EXACTLY ONCE, and ticks the already-compiled delegate
/// every frame against one entity. <see cref="Tick"/> never touches OcGraphParser or GraphCompiler
/// again after <see cref="Load"/>.
///
/// TWO KINDS OF GRAPH, one host, decided once from graph.EntryPoints.Count (mirrors GraphCompiler's
/// own Compile()-vs-CompileEntryPoint choice). No ENTRY records = dataflow: PARAM entity/time in,
/// 0-3 OUT floats out, PULLED fresh every Tick() with no memory of previous ticks (see Compile()'s
/// doc). At least one ENTRY = event-driven: Tick() drives "OnStart" once and "OnTick" every call via
/// GraphCompiler.CompileEntryPoint -- see LoadEventGraph. The two paths never mix.
///
/// COMPILE ERRORS: <see cref="Load"/> returns false, sets <see cref="LoadError"/>, logs once --
/// <see cref="Ready"/> is then false and <see cref="Tick"/> is a documented no-op, no throw, no retry.
///
/// RUNTIME ERRORS are NOT swallowed: a throw from a compiled delegate (e.g. no native Aver.Scene
/// loaded, so a getfield/setfield node's P/Invoke can't resolve) propagates out of Tick() like any
/// other per-frame bug -- a caller wanting non-fatal ticks must catch around its own Tick() call.
/// Both real callers do: HostBridge.GraphLoad/GraphTick (Aver.Scripting.Bridge, "graph hosting"
/// region) catch, log, and drop the offending graph.
///
/// PARAMETER WIRING: dataflow PARAMs must be a subset of {"entity" (Int), "time" (Float)}; anything
/// else fails at Load() naming the offending PARAM. Event-driven graphs additionally honour
/// "deltaTime" (Float) -- see LoadEventGraph for why only on that path.
///
/// OUTPUT WIRING: 2-3 OUT records read as (x, y[, z]), z defaulting to 0, applied via PositionSink.
/// 0 or 1 OUT records still ticks (setfield side effects run) but makes no position write -- a scope
/// decision, not a failure (see ApplyResult), same for event-driven OUT after the exec chain finishes
/// (see CompileEntryPoint's "THE RETURN VALUE" comment); a scalar or void result is always a no-op.</summary>
public class GraphHost
{
    private enum ParamSlot { Entity, Time, DeltaTime }

    // ---- dataflow (PULL) path -- UNCHANGED from before phase 2 ------------------------------------
    private Delegate? _compiled;
    private ParamSlot[] _argSlots = Array.Empty<ParamSlot>();

    // ---- event-driven (PUSH/exec) path -- new in phase 2 -------------------------------------------
    private Delegate? _onStart;
    private Delegate? _onTick;
    private ParamSlot[] _eventArgSlots = Array.Empty<ParamSlot>();
    // Fires OnStart on the FIRST Tick(), not inside Load(): Load() can run long before the game loop's
    // first real frame (GameApp compiles every .ocgraph at project open), and "play begins" reads as
    // "the first Tick() call" -- so a compile error surfaces at Load(), an OnStart runtime error at
    // frame 1, matching the class doc's load/runtime split.
    private bool _startInvoked;
    // Accumulates deltaTime so PARAM time float (if declared, instead of/alongside deltaTime) still
    // gets a monotonic clock -- GameApp always passes a per-frame delta here, never an already-
    // accumulated absolute time like a dataflow graph's caller might.
    private float _execSimTime;

    // ---- on-demand (Fire()) path -- new in phase 3 -------------------------------------------------
    // One compiled delegate per declared event name other than "OnStart"/"OnTick" (see LoadEventGraph
    // for how names sort into this bucket, Fire() for invocation). Disjoint from _onStart/_onTick --
    // two entry-point sets sharing one Graph, same as OcGraph.hpp's dataflow-vs-exec split.
    private Dictionary<string, Delegate> _onDemand = new();

    // ---- graph-local persistent variables (VAR) -- new in the variables slice -----------------------
    // ONE INSTANCE FIELD PER GraphHost -- the whole storage/lifetime contract on this class's side
    // (GraphVarStore owns the "how"). Created in LoadFromText(), after a successful parse and before
    // either compile path, via GraphVarStore.CreateFor(graph) (seeds every VAR at its default, so a
    // read before any write is deterministic). Null when the graph declares no VAR (the common case),
    // so Tick()/Fire() append nothing extra to DynamicInvoke args -- identical to before VAR existed.
    //
    // Two GraphHosts over the same .ocgraph get two independent stores for free: nothing keys storage
    // by file path or shares a static table -- the same "one GraphHost, one everything" property that
    // already stops several actors sharing one idle-motion .ocgraph from stacking at the origin (see
    // GraphVarTests.cs's TestTwoGraphHostsOverSameGraphFileHaveIndependentVariables). A reload
    // (second Load()/LoadFromText()) replaces this field with a brand-new store seeded from defaults
    // -- ResetCompiledState() nulls it first, like every other compiled-state field, so variables
    // reset rather than carrying over stale values.
    private GraphVarStore? _varStore;

    private Graph? _graph;
    private readonly PositionSink _positionSink;

    // Resolved once, process-wide, the first time the DEFAULT sink is used -- not at construction, so
    // a GraphHost never requires a live scene to exist yet. 0 mirrors the ABI's "unknown field"
    // sentinel (SceneAbi.cpp), doubling as "not yet resolved" with no separate bool.
    private static int s_cachedPositionFieldId;

    /// <summary>Null until a Load() call fails; then the reason, exactly as reported at that time.</summary>
    public string? LoadError { get; private set; }

    /// <summary>True once a graph compiled and something will run it -- via Tick() (dataflow delegate,
    /// or OnStart/OnTick) or on demand via Fire() (an on-demand entry point). A graph with only an
    /// on-demand event is genuinely Ready even though Tick() does nothing -- deliberate, not a gap.</summary>
    public bool Ready => _compiled != null || _onStart != null || _onTick != null || _onDemand.Count > 0;

    /// <summary>True for a graph loaded via the event-driven (ENTRY/exec) path -- Tick()-driven,
    /// Fire()-able, or both. Lets a caller (GameApp's discovery log) say which kind it found without
    /// re-deriving it from Graph.EntryPoints.</summary>
    public bool IsEventDriven => _onStart != null || _onTick != null || _onDemand.Count > 0;

    /// <summary>The parsed graph, for inspection (name/description/node count) -- null until a
    /// successful Load(). Exposed read-only; GraphHost owns compiling it, nothing else should.</summary>
    public Graph? Graph => _graph;

    /// <summary>This host's own VAR storage -- null exactly when <see cref="Graph"/> is null or the
    /// graph declares no VAR (see <c>_varStore</c>'s field comment). Read-only, same reason as
    /// <see cref="Graph"/>: a caller (a save provider capturing/restoring VAR values) reaches the
    /// store GraphHost already owns rather than keeping a second copy.</summary>
    public GraphVarStore? VarStore => _varStore;

    /// <param name="positionSink">Where computed positions go. Defaults to writing the live scene's
    /// CLocal.position via Aver.Scene.Native.aver_scene_set_vec; pass a fake in tests with no native
    /// scene (see GraphHostTests), mirroring GraphCompiler's injectable FieldResolver.</param>
    public GraphHost(PositionSink? positionSink = null)
    {
        _positionSink = positionSink ?? DefaultPositionSink;
    }

    /// <summary>Reads, parses, and compiles the .ocgraph at <paramref name="path"/>. Returns false and
    /// sets LoadError/err on failure (file not found, parse, compile) -- Ready stays false, Tick() is
    /// a no-op. Safe to call again (e.g. after a file edit); each call fully replaces what compiled
    /// before.</summary>
    public bool Load(string path, out string? err)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (Exception ex)
        {
            ResetCompiledState();
            LoadError = err = $"Could not read '{path}': {ex.Message}";
            Console.Error.WriteLine($"[GraphHost] {LoadError}");
            return false;
        }

        return LoadFromText(text, out err);
    }

    /// <summary>Same as <see cref="Load"/> but from an in-memory .ocgraph string -- what tests use
    /// so they don't depend on a file's location relative to the process's working directory (the
    /// exact relative-path landmine TestCrossImplementationFixture already hit once).</summary>
    public bool LoadFromText(string ocgraphText, out string? err)
    {
        ResetCompiledState();
        err = null;

        if (!OcGraphParser.Parse(ocgraphText, out var graph, out var parseErr))
        {
            LoadError = err = $"Parse error: {parseErr}";
            Console.Error.WriteLine($"[GraphHost] {LoadError}");
            return false;
        }

        // THE ONE CHOKEPOINT WHERE A FOREIGN GRAPH IS TURNED AWAY -- both Load(path) and this method
        // funnel here. Without it a material graph's HLSL-named nodes would reach GraphCompiler and
        // fail with "unknown node type" instead of "wrong compiler". See Graph.DomainKind for why an
        // unrecognised domain is refused here too.
        if (graph.DomainKind != GraphDomain.Gameplay)
        {
            LoadError = err = $"this is a '{graph.Domain}' graph, not a gameplay graph -- " +
                               "GraphHost compiles gameplay graphs only";
            Console.Error.WriteLine($"[GraphHost] {LoadError}");
            return false;
        }

        // WHICH PATH, decided once like GraphCompiler's own Compile()-vs-CompileEntryPoint choice: any
        // ENTRY record means event-driven; EntryPoints.Count == 0 takes the original dataflow path.
        if (graph.EntryPoints.Count > 0)
            return LoadEventGraph(graph, out err);

        var slots = new ParamSlot[graph.Parameters.Count];
        for (int i = 0; i < graph.Parameters.Count; i++)
        {
            var p = graph.Parameters[i];
            if (p.Type == PinType.Int && p.Name.Equals("entity", StringComparison.OrdinalIgnoreCase))
            {
                slots[i] = ParamSlot.Entity;
            }
            else if (p.Type == PinType.Float && p.Name.Equals("time", StringComparison.OrdinalIgnoreCase))
            {
                slots[i] = ParamSlot.Time;
            }
            else
            {
                LoadError = err = $"PARAM '{p.Name}' ({p.Type}) is not something GraphHost.Tick(entityId, timeSeconds) " +
                                   "can supply -- only PARAM entity int and PARAM time float are wired";
                Console.Error.WriteLine($"[GraphHost] {LoadError}");
                return false;
            }
        }

        var compiled = new GraphCompiler(graph).Compile(out var compileErr);
        if (compiled == null)
        {
            LoadError = err = $"Compile error: {compileErr}";
            Console.Error.WriteLine($"[GraphHost] {LoadError}");
            return false;
        }

        _graph = graph;
        _argSlots = slots;
        _compiled = compiled;
        // Committed at the SAME point as _graph/_compiled, not earlier (see _varStore's field comment)
        // -- a compile failure above returns before touching it, preserving the all-at-once replace.
        _varStore = graph.Variables.Count > 0 ? GraphVarStore.CreateFor(graph) : null;
        return true;
    }

    /// <summary>Compiles the "OnStart"/"OnTick" entry points an event-driven graph declares. Returns
    /// false and sets LoadError/err if a wanted entry fails to compile, or the graph declares ENTRY
    /// records naming no event this host drives -- one clear reason, not a graph that silently does
    /// nothing. A working OnTick with a broken OnStart (or vice versa) fails as a whole rather than
    /// running half-compiled, matching Load()'s all-or-nothing swap contract.</summary>
    private bool LoadEventGraph(Graph graph, out string? err)
    {
        err = null;

        var compiler = new GraphCompiler(graph);
        bool wantsStart = graph.EntryPoints.Any(e => e.EventName == "OnStart");
        bool wantsTick  = graph.EntryPoints.Any(e => e.EventName == "OnTick");

        // EVERY OTHER declared event name -- "OnHit", or anything a future project invents -- is
        // ON-DEMAND: compiled here like OnStart/OnTick but fired later via Fire(). No name is special-
        // cased; any name that is not "OnStart"/"OnTick" takes this path, so a new event is free here.
        var onDemandNames = graph.EntryPoints.Select(e => e.EventName)
            .Where(n => n != "OnStart" && n != "OnTick")
            .Distinct()
            .ToList();

        // The {"entity" Int, "time" Float, "deltaTime" Float} ParamSlot convention exists SOLELY
        // because Tick(entityId, timeSeconds) has a fixed two-argument signature, and does not apply
        // to on-demand-only events -- so the check below is gated on wantsStart/wantsTick: a graph
        // with ONLY an on-demand entry may declare any PARAM its payload needs (e.g. `otherEntity`)
        // with nothing here to reject it (fixes a past bug: this used to reject `PARAM otherEntity
        // int` before LoadEventGraph noticed there was no OnStart/OnTick). See Fire()'s doc comment
        // for why positional args, not a second named-slot vocabulary, is the right contract.
        //
        // Parameters is ONE list for the WHOLE FILE (Graph.cs), shared by every ENTRY, so mixing an
        // on-demand event with OnStart/OnTick is not specially supported -- a PARAM only the on-
        // demand event needs would also be asked of OnTick's own delegate, which Tick() cannot
        // supply. One event's payload needs its own file, separate from any OnStart/OnTick graph.
        var slots = new ParamSlot[graph.Parameters.Count];
        if (wantsStart || wantsTick)
        {
            for (int i = 0; i < graph.Parameters.Count; i++)
            {
                var p = graph.Parameters[i];
                if (p.Type == PinType.Int && p.Name.Equals("entity", StringComparison.OrdinalIgnoreCase))
                    slots[i] = ParamSlot.Entity;
                else if (p.Type == PinType.Float && p.Name.Equals("time", StringComparison.OrdinalIgnoreCase))
                    slots[i] = ParamSlot.Time;
                else if (p.Type == PinType.Float && p.Name.Equals("deltaTime", StringComparison.OrdinalIgnoreCase))
                    slots[i] = ParamSlot.DeltaTime;
                else
                {
                    LoadError = err = $"PARAM '{p.Name}' ({p.Type}) is not something an event-driven " +
                                       "GraphHost can supply to OnStart/OnTick -- only PARAM entity int, " +
                                       "PARAM time float and PARAM deltaTime float are wired for them. A " +
                                       "graph that wants a different PARAM must not ALSO declare an " +
                                       "OnStart or OnTick entry in this same file (PARAM is graph-wide, " +
                                       "not per-ENTRY) -- give the on-demand event its own file instead, " +
                                       "fired via Fire(), which places no restriction on PARAM names.";
                    Console.Error.WriteLine($"[GraphHost] {LoadError}");
                    return false;
                }
            }
        }

        // A GRAPH THAT DOES NOTHING LOOKS EXACTLY LIKE A GRAPH THAT WORKS -- the one failure an author
        // can't diagnose from the log. An ENTRY node with no exec OUTPUT pin (typo'd type, or unwired)
        // compiles fine (zero exec outputs is a legal fan-out of nothing) and just runs silently doing
        // nothing. Warn instead of failing -- a half-built graph must still load (the normal build
        // order is entry node first, wiring after) -- but name the node once per load, so the author
        // sees "not connected yet" instead of "my template is broken".
        foreach (var (entryNodeId, entryEventName) in graph.EntryPoints)
        {
            if (!graph.Nodes.TryGetValue(entryNodeId, out var entryNode)) continue;   // Validate() reports this
            if (!entryNode.Pins.Any(pin => pin.IsOutput && pin.Type == PinType.Exec))
                Console.Error.WriteLine(
                    $"[GraphHost] warning: ENTRY '{entryEventName}' names node '{entryNodeId}' " +
                    $"(type '{entryNode.Type}'), which has no exec OUTPUT pin. It will run every time " +
                    "the event fires and do nothing. Check the node type is spelled correctly and that " +
                    "its exec pin is connected to something.");
        }

        Delegate? onTick = null, onStart = null;
        if (wantsTick)
        {
            onTick = compiler.CompileEntryPoint("OnTick", out var tickErr);
            if (onTick == null)
            {
                LoadError = err = $"OnTick compile error: {tickErr}";
                Console.Error.WriteLine($"[GraphHost] {LoadError}");
                return false;
            }
        }
        if (wantsStart)
        {
            onStart = compiler.CompileEntryPoint("OnStart", out var startErr);
            if (onStart == null)
            {
                LoadError = err = $"OnStart compile error: {startErr}";
                Console.Error.WriteLine($"[GraphHost] {LoadError}");
                return false;
            }
        }

        // Every other declared event name is compiled here too, cached by name for Fire() to invoke.
        // Same all-or-nothing swap as OnStart/OnTick: a compile failure anywhere fails the WHOLE
        // Load() rather than leaving some events working and others not.
        var onDemand = new Dictionary<string, Delegate>();
        foreach (var name in onDemandNames)
        {
            var onDemandCompiled = compiler.CompileEntryPoint(name, out var onDemandErr);
            if (onDemandCompiled == null)
            {
                LoadError = err = $"'{name}' compile error: {onDemandErr}";
                Console.Error.WriteLine($"[GraphHost] {LoadError}");
                return false;
            }
            onDemand[name] = onDemandCompiled;
        }

        // NO "nothing would ever run" CHECK NEEDED HERE, UNLIKE BEFORE PHASE 3 (which fired whenever
        // neither OnStart nor OnTick compiled). wantsStart, wantsTick and onDemandNames now partition
        // EVERY distinct event name exhaustively (Graph.Validate() refuses a duplicate ENTRY per name,
        // so there is no fourth bucket), and a compile failure in any bucket already returned false
        // above -- so a graph reaching this line always leaves with something runnable via Tick() or
        // Fire(), which is why LoadFromText only calls this method when EntryPoints.Count > 0.
        _graph = graph;
        _eventArgSlots = slots;
        _onStart = onStart;
        _onTick = onTick;
        _onDemand = onDemand;
        _startInvoked = false;
        _execSimTime = 0f;
        // Committed at the SAME point as _graph/_onStart/_onTick/_onDemand, not earlier (see the
        // dataflow path's identical comment, and _varStore's field comment). Shared by OnStart, OnTick
        // and every on-demand event: storage lives at host/graph level, so Fire("OnHit", ...) and
        // OnTick see the SAME variable pool.
        _varStore = graph.Variables.Count > 0 ? GraphVarStore.CreateFor(graph) : null;
        return true;
    }

    /// <summary>Ticks the already-compiled graph for one frame. No-op if Load() never succeeded (see
    /// Ready) -- no throw, no re-log, no recompile attempt.
    ///
    /// DATAFLOW graph: entityId is both the PARAM entity value and the position-write target;
    /// timeSeconds passes straight through to PARAM time.
    ///
    /// EVENT-DRIVEN graph: entityId is unchanged, but timeSeconds is THIS TICK'S DELTA, not an
    /// absolute clock (see LoadEventGraph for why "deltaTime" exists only here). The first call also
    /// fires OnStart (if declared) before OnTick -- GameApp's first Tick() on a freshly loaded graph
    /// IS "play begins" for it, by construction (see GameApp.cpp).</summary>
    public void Tick(int entityId, float timeSeconds)
    {
        if (_onStart != null || _onTick != null)
        {
            TickEventGraph(entityId, timeSeconds);
            return;
        }
        if (_compiled == null) return;

        // One extra trailing slot for _varStore, after every declared PARAM (see GraphCompiler's
        // "trailing GraphVarStore parameter" comment for the ordering). Zero extra slots for the
        // common VAR-less graph, where _varStore is null.
        var args = new object[_argSlots.Length + (_varStore != null ? 1 : 0)];
        for (int i = 0; i < _argSlots.Length; i++)
            args[i] = _argSlots[i] == ParamSlot.Entity ? (object)entityId : (object)timeSeconds;
        if (_varStore != null) args[_argSlots.Length] = _varStore;

        // DynamicInvoke, not a statically-typed Func<>: GraphHost doesn't know at compile time how
        // many PARAMs a graph declares. Costs a reflection dispatch per tick -- fine for this slice's
        // handful of entities; a hot path with thousands would want GraphHost to specialize by arity
        // (mirroring GetDelegateType's dispatch) -- a real, named limitation, not a hidden one.
        GraphCallGuard.Reset();   // see the Tick() path for why -- same reason, other entry point
        object? result = _compiled.DynamicInvoke(args);
        ApplyResult(entityId, result);
    }

    private void TickEventGraph(int entityId, float deltaTime)
    {
        _execSimTime += deltaTime;

        object[] BuildArgs(ParamSlot[] slots)
        {
            // Same trailing _varStore slot Tick()'s dataflow loop appends (see that comment). OnStart
            // and OnTick share ONE _varStore, letting an OnHit-shaped handler set a VAR and OnTick read it.
            var args = new object[slots.Length + (_varStore != null ? 1 : 0)];
            for (int i = 0; i < slots.Length; i++)
                // (object) ON EVERY ARM IS LOAD-BEARING, NOT STYLE. Without it, C# infers the switch's
                // own natural type first -- `int`/`float` unify to `float` (int widens, float doesn't
                // narrow) -- so entityId got silently boxed as System.Single, and a `PARAM entity int`-
                // only graph threw ArgumentException ("Object of type 'System.Single' cannot be
                // converted to type 'System.Int32'") when DynamicInvoke bound it to the delegate's real
                // `int` param (found by OnHitEventTests.TestOnStartAndOnHitCanShareACompatibleParamList;
                // never caught before since no prior test drove an ENTRY graph through GraphHost.Tick()
                // itself -- existing Select/InputKey/Raycast/Spawn tests call CompileEntryPoint directly).
                // The dataflow loop above already casts both its ternary arms to (object) for the same
                // reason; this switch hadn't. Casting forces the natural type to `object` so each arm
                // boxes as its own type instead of unifying first.
                args[i] = slots[i] switch
                {
                    ParamSlot.Entity    => (object)entityId,
                    ParamSlot.Time      => (object)_execSimTime,
                    ParamSlot.DeltaTime => (object)deltaTime,
                    _ => throw new InvalidOperationException($"unhandled {nameof(ParamSlot)} {slots[i]}"),
                };
            if (_varStore != null) args[slots.Length] = _varStore;
            return args;
        }

        // A compiled graph function counts its own recursion depth, reset here at every top-level
        // entry rather than unwound by try/finally in emitted IL (see GraphCallGuard): an exception
        // thrown mid-graph skips the Exit() that would decrement it, so without this reset the leak
        // accumulates until an innocent later tick trips the limit.
        GraphCallGuard.Reset();

        // OnStart first, and only ever once: see _startInvoked's own comment for why this lazy site
        // (first Tick(), not inside Load()) is where "play begins" is decided for this class.
        if (_onStart != null && !_startInvoked)
        {
            _startInvoked = true;
            object? startResult = _onStart.DynamicInvoke(BuildArgs(_eventArgSlots));
            LogEntryFired("OnStart", entityId, startResult);
            ApplyResult(entityId, startResult);
        }

        if (_onTick != null)
        {
            object? tickResult = _onTick.DynamicInvoke(BuildArgs(_eventArgSlots));
            LogEntryFired("OnTick", entityId, tickResult);
            ApplyResult(entityId, tickResult);
        }
    }

    /// <summary>Runs ONE already-compiled, ON-DEMAND entry point RIGHT NOW -- the Fire()-side
    /// counterpart to Tick(), for any declared event name that is not "OnStart"/"OnTick" (see
    /// LoadEventGraph for how a name sorts into this bucket). Returns false and leaves
    /// <paramref name="result"/> null when <paramref name="eventName"/> was never declared, or Load()
    /// never succeeded -- the same documented-no-op contract Tick() has for a failed compile (see
    /// Ready), so a caller can just call Fire() and check the bool.
    ///
    /// NOT PARAMSLOT-BASED, UNLIKE Tick(). Tick() can only supply the two values it receives, so
    /// LoadEventGraph maps PARAM names onto a closed vocabulary ("entity"/"time"/"deltaTime") for
    /// OnStart/OnTick; Fire()'s caller already HAS the specific event's payload at the call site, so a
    /// second named-slot vocabulary would grow forever -- the trap LoadEventGraph declines for PARAM.
    /// <paramref name="args"/> is POSITIONAL instead, matching the fired entry's declared PARAM list in
    /// file order, the same contract CompileEntryPoint's compiled delegate has; GraphHost adds no
    /// translation layer, it just forwards the call by name, like Tick() forwards to _onTick.
    ///
    /// <paramref name="result"/> follows the same zero/one/many convention as Tick()/ApplyResult but is
    /// NOT auto-applied to a PositionSink: an on-demand event has no fixed entity target (Fire() isn't
    /// even given one) -- a graph that wants to write a scene value can already do so via a SetField/
    /// SetFieldVec3 node on its own exec chain.</summary>
    public bool Fire(string eventName, object[] args, out object? result)
    {
        result = null;
        if (!_onDemand.TryGetValue(eventName, out var compiled))
            return false;

        int wantCount = _graph?.Parameters.Count ?? 0;
        if (args.Length != wantCount)
            throw new ArgumentException(
                $"GraphHost.Fire('{eventName}'): graph '{_graph?.Name}' declares {wantCount} PARAM(s) " +
                $"but {args.Length} argument(s) were supplied -- args must match the graph's PARAM list " +
                "positionally, in declaration order, exactly like CompileEntryPoint's own compiled " +
                "delegate (see this method's own doc comment)", nameof(args));

        // Same trailing _varStore slot Tick()/TickEventGraph append, added HERE internally so the
        // "args match the PARAM list positionally" contract stays true from the caller's perspective.
        object[] callArgs = args;
        if (_varStore != null)
        {
            callArgs = new object[args.Length + 1];
            Array.Copy(args, callArgs, args.Length);
            callArgs[args.Length] = _varStore;
        }

        GraphCallGuard.Reset();
        result = compiled.DynamicInvoke(callArgs);
        Console.WriteLine($"[GraphHost] '{_graph?.Name}': {eventName}(fired on demand) -> {DescribeResult(result)}");
        return true;
    }

    /// <summary>GAP 3's entry point: the FireEvent-node counterpart to Fire(), for a caller that does
    /// NOT know this graph's PARAM payload the way Fire()'s positional-args contract assumes. A
    /// FireEvent node's router (Aver.Graph.GraphEvents, via HostBridge) has only an entity and an event
    /// name at the call site -- it can't know the target's PARAM shape. So this method builds
    /// positional args from a small, closed vocabulary shared with the Tick()-driven path (see
    /// LoadEventGraph's "entity"/"time"/"deltaTime" comment): "entity" is <paramref name="entityId"/>
    /// (the TARGET's own entity); "time" is this host's accumulated sim clock (_execSimTime, same value
    /// OnTick reads); "deltaTime" is always 0f (an on-demand event has no duration).
    ///
    /// ANY OTHER PARAM NAME IS A VISIBLE REFUSAL (naming the graph, event and PARAM), NEVER A GUESS:
    /// Fire() itself WIDENS a mismatched-kind argument rather than rejecting it (a boxed int reads back
    /// as float for a Float PARAM, confirmed empirically), so guessing a value for an unrecognised
    /// PARAM could silently feed nonsense into a graph's logic with no error.
    ///
    /// Returns false with a non-null <paramref name="refusal"/> when this graph never declared
    /// <paramref name="eventName"/>, or declared it but ALSO a PARAM outside {entity, time, deltaTime}
    /// (failing before touching DynamicInvoke). Otherwise returns true with <paramref name="result"/>
    /// set to whatever Fire() returns.</summary>
    public bool FireForEntity(string eventName, int entityId, out object? result, out string? refusal)
    {
        result = null;
        refusal = null;

        if (_graph == null || !_onDemand.ContainsKey(eventName))
        {
            refusal = $"graph '{_graph?.Name}' has no on-demand event '{eventName}'";
            return false;
        }

        var args = new object[_graph.Parameters.Count];
        for (int i = 0; i < _graph.Parameters.Count; i++)
        {
            var p = _graph.Parameters[i];
            if (p.Type == PinType.Int && p.Name.Equals("entity", StringComparison.OrdinalIgnoreCase))
                args[i] = entityId;
            else if (p.Type == PinType.Float && p.Name.Equals("time", StringComparison.OrdinalIgnoreCase))
                args[i] = _execSimTime;
            else if (p.Type == PinType.Float && p.Name.Equals("deltaTime", StringComparison.OrdinalIgnoreCase))
                args[i] = 0f;
            else
            {
                refusal = $"graph '{_graph.Name}' PARAM '{p.Name}' ({p.Type}) is not something FireEvent " +
                           "can supply -- only entity (int), time (float) and deltaTime (float) are wired " +
                           "for it, the same vocabulary OnStart/OnTick's own PARAM list is restricted to " +
                           "(see LoadEventGraph's own comment). Give this event its own PARAM-free-of-that-" +
                           "restriction file if it needs a real payload, and reach it through Fire() " +
                           "directly instead, with a caller that knows the shape.";
                return false;
            }
        }

        return Fire(eventName, args, out result);
    }

    // Success-path diagnostic, deliberately on stdout (not the LoadError family's Console.Error) since
    // this isn't a failure. AverEngineRuntime.exe is WIN32-subsystem with no console, but .NET's
    // Console still writes to whatever stdout handle the process inherited -- exactly the redirected
    // handle a headless `--frames N` capture provides, so this proves an entry point actually ran,
    // naming the graph, event and result, one line per fire.
    private void LogEntryFired(string eventName, int entityId, object? result)
    {
        // ONCE PER ENTITY FOR OnTick -- the whole point of the diagnostic. OnStart fires once by
        // construction; OnTick fires every frame for every entity, and used to log every time. MEASURED
        // on PTTest (three targets + a rules object, 120 frames, no play session):
        //
        //     360 lines  50.1%  [GraphHost] 'FPTarget' entity N: OnTick -> N
        //     119 lines  16.6%  [GraphHost] 'FPRules'  entity N: OnTick -> N
        //
        // 479 of 718 lines, two thirds of the log, from four objects sitting still -- a wall hiding
        // every other message, including ones this codebase logs because they once hid a bug.
        //
        // The value kept is unchanged ("proof CompileEntryPoint's output was really running every
        // frame" -- one line per entity proves that); it just stops re-proving it sixty times a second.
        if (eventName == "OnTick" && !_tickLogged.Add(entityId))
            return;

        var note = eventName == "OnTick" ? "  (OnTick fires every frame; said once per entity)" : "";
        Console.WriteLine($"[GraphHost] '{_graph?.Name}' entity {entityId}: {eventName} -> {DescribeResult(result)}{note}");
    }

    // Entities whose OnTick has already been reported. Per graph host instance, which is the same
    // scope _graph itself has, so reloading a graph legitimately reports its first tick again.
    private readonly HashSet<int> _tickLogged = new HashSet<int>();

    private static string DescribeResult(object? result) => result switch
    {
        null => "(void)",
        object[] arr => "[" + string.Join(", ", arr) + "]",
        _ => result.ToString() ?? "(unknown)",
    };

    private void ResetCompiledState()
    {
        _compiled = null;
        _graph = null;
        LoadError = null;
        _onStart = null;
        _onTick = null;
        _eventArgSlots = Array.Empty<ParamSlot>();
        _startInvoked = false;
        _execSimTime = 0f;
        _onDemand = new();
        _varStore = null;
    }

    private void ApplyResult(int entityId, object? result)
    {
        // GraphCompiler.Compile()/CompileEntryPoint() return object[] (one boxed value per OUT, file
        // order) for 2+ outputs (see GraphCompiler.cs's OUTPUTS ARRAY comment); void or a bare scalar
        // for 0-1 outputs -- no defined meaning for "one number is a position", so GraphHost makes no
        // position write, but the graph still ran (setfield side effects, and any OnStart/OnTick log
        // line above, already happened during DynamicInvoke). Same rule for dataflow and event-driven.
        if (result is object[] outputs && outputs.Length >= 2)
        {
            float x = ToFloat(outputs[0]);
            float y = ToFloat(outputs[1]);
            float z = outputs.Length >= 3 ? ToFloat(outputs[2]) : 0f;
            _positionSink(entityId, x, y, z);
        }
    }

    private static float ToFloat(object? boxed) => boxed switch
    {
        float f => f,
        int i => i,
        _ => 0f,
    };

    private static void DefaultPositionSink(int entityId, float x, float y, float z)
    {
        if (s_cachedPositionFieldId == 0)
        {
            s_cachedPositionFieldId = Native.aver_scene_field("CLocal.position");
            if (s_cachedPositionFieldId == 0)
                throw new InvalidOperationException(
                    "GraphHost: could not resolve scene field 'CLocal.position' -- is a scene loaded? " +
                    "(pass a PositionSink to GraphHost's constructor to bypass the live scene, e.g. in a test)");
        }

        Native.aver_scene_set_vec(entityId, s_cachedPositionFieldId, new[] { x, y, z });
    }
}
