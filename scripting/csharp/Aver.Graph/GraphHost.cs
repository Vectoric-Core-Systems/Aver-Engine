// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Hosts one compiled graph against one entity, ticked once per frame.
// Comment explains WHY: GraphCompiler proves a graph CAN become IL; nothing before this file
// loaded a graph from disk, kept the compiled delegate around, and fed it a live time value every
// frame. That is the missing piece between "the compiler works" and "a graph moves something in
// the scene" -- this class is exactly that piece and nothing more.
//
// PHASE 2 ADDITION (visual scripting runs in a shipped game): this file originally only knew how
// to drive Compile()'s PULL/dataflow delegate -- a graph with PARAM entity/time and 2-3 OUT floats
// read as a position. Phase 1 (commit a9038da) added exec pins, ENTRY records and
// GraphCompiler.CompileEntryPoint(), but nothing ever taught the one class that LOADS a graph from
// disk and TICKS it how to use that path -- a graph with an ENTRY record would have failed here
// with a PARAM-shape error, or (before Compile() was fixed to skip exec-only nodes) worse. This
// file is still the single seam between "compiled graph" and "driven every frame", by design, so
// event-driven graphs are taught here rather than by inventing a second host: GameApp (see
// modules/runtime.game/src/GameApp.cpp) discovers a project's .ocgraph files and calls the SAME
// ScriptHost::graphLoad/graphTick entity-scoped API SandboxApp's graph-driven drone already uses,
// just with a synthetic id standing in for "the project" rather than a real scene entity -- see
// GameApp.cpp's own comment on why that is safe. No native or Aver.Scripting.Bridge change was
// needed for that reuse; see the runtime build plan / phase-2 report for the fuller reasoning.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Aver.Scene;

namespace Aver.Graph;

/// <summary>Applies one tick's computed (x, y, z) to an entity. GraphHost calls this instead of
/// emitting a vector scene-field write inside the graph itself, because getfield/setfield nodes
/// only support F32-kind fields today (see GraphCompiler's FieldKindF32 comment) and CLocal.position
/// is Vec3-kind -- writing it needs aver_scene_set_vec, which takes a 3-float buffer, not the
/// single-float get/set pair getfield/setfield emit. The default implementation
/// (<see cref="GraphHost.DefaultPositionSink"/>) calls straight through to that P/Invoke via
/// Aver.Scene.Native, exactly like GraphCompiler's own DefaultFieldResolver does for scalar fields.
/// A caller with no live native scene to write into (a bare unit test, notably) must supply its own
/// sink instead of standing one up -- see Aver.Graph.Tests for exactly that.</summary>
public delegate void PositionSink(int entityId, float x, float y, float z);

/// <summary>Loads an .ocgraph, compiles it EXACTLY ONCE, and ticks the already-compiled delegate
/// every frame against one entity. Reflection.Emit per frame would defeat the entire point of
/// compiling to IL in the first place, so <see cref="Load"/> does all the parsing and compiling and
/// <see cref="Tick"/> never touches OcGraphParser or GraphCompiler again.
///
/// TWO KINDS OF GRAPH, one host. A graph with no ENTRY records is the ORIGINAL dataflow kind: PARAM
/// entity/time in, 0-3 OUT floats out, PULLED fresh every Tick() with no memory of previous ticks
/// (see Compile()'s own doc). A graph with at least one ENTRY record is EVENT-DRIVEN: Tick() drives
/// its "OnStart" entry once (the first time this host is ever ticked) and its "OnTick" entry every
/// call, via GraphCompiler.CompileEntryPoint -- see LoadEventGraph below. The two paths do not mix:
/// which one a given .ocgraph gets is decided once, from graph.EntryPoints.Count, exactly the way
/// GraphCompiler itself decides between Compile() and CompileEntryPoint().
///
/// COMPILE ERRORS: if the graph fails to parse or compile, <see cref="Load"/> returns false, sets
/// <see cref="LoadError"/>, and logs once. <see cref="Ready"/> is then false and <see cref="Tick"/>
/// becomes a documented no-op -- it does not throw every frame, and it does not retry compilation.
///
/// RUNTIME ERRORS are a different thing and are NOT swallowed: if invoking a compiled delegate
/// throws (for example, no native Aver.Scene library is loaded so the emitted P/Invoke call inside
/// a getfield/setfield node cannot resolve), that exception propagates out of Tick() exactly as any
/// other unhandled per-frame bug would. Hiding that would be the same mistake this codebase has
/// already made once (see aver-unbacked-verification): a caller that wants tick failures to be
/// non-fatal must catch around its own Tick() call and decide what "safe" means for its frame loop,
/// because GraphHost cannot tell a transient hiccup from a graph that will never work. Both of
/// GraphHost's real callers already do exactly that -- Aver.Scripting.Bridge's HostBridge.GraphLoad
/// and HostBridge.GraphTick each wrap their call to this class in a try/catch that logs and drops
/// the offending graph from its table (see HostBridge.cs's "graph hosting" region) -- which is what
/// makes an event-driven graph's runtime exception behave exactly like phase 1's own paranoia
/// demands: visible in the log, and survived by the process, with zero new code needed here for it.
///
/// PARAMETER WIRING (dataflow graphs): GraphHost only knows how to supply two things to a compiled
/// graph's PARAMs -- the entity id and the current time -- because those are the only two values
/// <see cref="Tick"/> itself receives. A graph's PARAM records must therefore be a subset of
/// {"entity" (Int), "time" (Float)}; anything else fails at Load() with a clear message naming the
/// offending PARAM, rather than Tick() silently passing a wrong or default value every frame.
///
/// PARAMETER WIRING (event-driven graphs): the same "entity"/"time" names are honoured, PLUS
/// "deltaTime" (Float) -- see LoadEventGraph's own comment for why a third name exists only on this
/// path rather than being added to the dataflow one above.
///
/// OUTPUT WIRING: a graph with 2 or 3 OUT records is read as (x, y[, z]) -- missing z defaults to
/// 0 -- and applied via the PositionSink. A graph with 0 or 1 OUT records is still ticked (so its
/// own setfield side effects, if any, still run) but GraphHost does not know how to turn a single
/// scalar into a position, so it makes no position write for that shape; this is a scope decision,
/// not a silent failure -- see ApplyResult. Unchanged by phase 2: an event-driven graph's OUT is
/// read the exact same way, after its exec chain finishes running for that tick (see
/// CompileEntryPoint's own "THE RETURN VALUE" comment) -- a graph authored to drive a real entity's
/// position from OnTick works with no special case here, and one authored to just compute a number
/// for a caller/test to read (this file's own end-to-end proof does exactly that) is equally safe:
/// ApplyResult only ever acts on a 2+-element result, so a single float or void is always a no-op.</summary>
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
    // Fires OnStart on the FIRST Tick() call rather than inline inside Load(). Deliberate: Load() can
    // run long before the game loop's first real frame (GameApp discovers and compiles every
    // .ocgraph during project open), and "OnStart fires once when play begins" reads most literally
    // as "the first time the game loop actually drives this graph", which is exactly what the first
    // Tick() call is. It also means a compile error surfaces at Load() (early, during project open)
    // while an OnStart RUNTIME error surfaces at the first Tick() (frame 1) -- the same load/runtime
    // split the class-level doc comment above already promises, now true for this path too.
    private bool _startInvoked;
    // Accumulates whatever Tick() is handed, so an event-driven graph that declares PARAM time float
    // (instead of, or alongside, deltaTime) still gets a monotonically increasing clock -- built from
    // the SAME per-call value deltaTime uses, on the reasoning that GameApp's caller always passes a
    // per-frame delta for this path (see its own comment on why), never an already-accumulated
    // absolute time the way a dataflow graph's caller might.
    private float _execSimTime;

    private Graph? _graph;
    private readonly PositionSink _positionSink;

    // Resolved once, process-wide, the first time the DEFAULT sink is actually used -- not at
    // GraphHost construction, so building a GraphHost never requires a live scene to already exist.
    // 0 mirrors the ABI's own "unknown field" sentinel (SceneAbi.cpp), so this doubles as
    // "not yet resolved" with no separate bool needed.
    private static int s_cachedPositionFieldId;

    /// <summary>Null until a Load() call fails; then the reason, exactly as reported at that time.</summary>
    public string? LoadError { get; private set; }

    /// <summary>True once a graph has compiled successfully and Tick() will actually run it. True for
    /// EITHER kind of graph: the dataflow delegate, or at least one of OnStart/OnTick.</summary>
    public bool Ready => _compiled != null || _onStart != null || _onTick != null;

    /// <summary>True for a graph loaded via the event-driven (ENTRY/exec) path rather than the
    /// original dataflow one. Exposed so a caller that cares (GameApp's discovery log, notably) can
    /// say which kind of graph it found without re-deriving it from Graph.EntryPoints itself.</summary>
    public bool IsEventDriven => _onStart != null || _onTick != null;

    /// <summary>The parsed graph, for inspection (name/description/node count) -- null until a
    /// successful Load(). Exposed read-only; GraphHost owns compiling it, nothing else should.</summary>
    public Graph? Graph => _graph;

    /// <param name="positionSink">Where computed positions go. Defaults to writing the live scene's
    /// CLocal.position via Aver.Scene.Native.aver_scene_set_vec. Pass a fake here in a test that has
    /// no native scene running -- see GraphHostTests in Aver.Graph.Tests for exactly that pattern,
    /// which mirrors GraphCompiler's own injectable FieldResolver.</param>
    public GraphHost(PositionSink? positionSink = null)
    {
        _positionSink = positionSink ?? DefaultPositionSink;
    }

    /// <summary>Reads, parses, and compiles the .ocgraph at <paramref name="path"/>. Returns false
    /// and sets LoadError/err on any failure (file not found, parse error, compile error) -- Ready
    /// stays false and Tick() becomes a no-op. Safe to call again later (e.g. after a file edit) to
    /// pick up a corrected graph; each call fully replaces whatever compiled before it.</summary>
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

        // WHICH PATH, decided once, the same way GraphCompiler itself decides between Compile() and
        // CompileEntryPoint(): a graph with at least one ENTRY record is event-driven. A graph
        // predating ENTRY (or one that is deliberately pure dataflow) has EntryPoints.Count == 0 and
        // takes the exact code path this class has always used -- see the class doc's "TWO KINDS OF
        // GRAPH" note for why the two never mix.
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
        return true;
    }

    /// <summary>Compiles the "OnStart"/"OnTick" entry points an event-driven graph declares. Returns
    /// false and sets LoadError/err if EITHER a wanted entry point fails to compile, or if the graph
    /// declares ENTRY records but none of them name an event this host knows how to drive -- both are
    /// reported as a single clear reason rather than left as a graph that silently does nothing,
    /// which is exactly the "must log clearly, naming the file and the reason" bar visual-scripting
    /// phase 2 set for a graph that cannot run. A graph with a working OnTick but a broken OnStart (or
    /// vice versa) is failed as a whole rather than run half-compiled -- a graph that sometimes has a
    /// start and sometimes does not is a worse debugging experience than one that plainly does not
    /// load, and Load()'s existing contract ("each call fully replaces whatever compiled before it")
    /// already promises an all-or-nothing swap.</summary>
    private bool LoadEventGraph(Graph graph, out string? err)
    {
        err = null;

        // Same {"entity" Int, "time" Float} convention the dataflow path uses above, PLUS "deltaTime"
        // Float. The third name exists only here, not on the dataflow path, because "deltaTime" only
        // means something to a graph that runs every tick and wants THIS tick's slice rather than an
        // accumulated clock -- exactly the event-driven case, and exactly what the task that added
        // this class's phase-2 support asked an OnTick entry point be able to receive. Adding it to
        // the dataflow slot list too would cost nothing today (no existing dataflow graph declares
        // it) but would silently change what that path accepts for no graph that needs it yet; kept
        // separate so the two lists can be read as two independent, honest contracts.
        var slots = new ParamSlot[graph.Parameters.Count];
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
                                   "GraphHost can supply -- only PARAM entity int, PARAM time float and " +
                                   "PARAM deltaTime float are wired for OnStart/OnTick";
                Console.Error.WriteLine($"[GraphHost] {LoadError}");
                return false;
            }
        }

        var compiler = new GraphCompiler(graph);
        bool wantsStart = graph.EntryPoints.Any(e => e.EventName == "OnStart");
        bool wantsTick  = graph.EntryPoints.Any(e => e.EventName == "OnTick");

        // A GRAPH THAT DOES NOTHING LOOKS EXACTLY LIKE A GRAPH THAT WORKS, and that is the one failure
        // an author cannot diagnose from the log. If the node an ENTRY names has no exec OUTPUT pin --
        // a typo'd node type, or a node someone forgot to wire onward -- the compiler is right not to
        // reject it (zero exec outputs is a legal fan-out of nothing, and ordinary data nodes rely on
        // that), so it compiles, runs every frame, and does nothing at all. The log then reads the same
        // as a healthy graph.
        //
        // Warn instead of failing: a half-built graph must still load, because the normal way to build
        // one is to add the entry node first and wire it up afterwards. But say it out loud, once per
        // load, naming the node -- that sentence is the difference between "my template is broken" and
        // "I have not connected the exec pin yet".
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

        if (onStart == null && onTick == null)
        {
            // Reachable when every ENTRY record names an event this host does not drive yet (e.g. a
            // future "OnCollide") -- not a compile failure of anything, just nothing GraphHost can
            // turn into a Tick() call. Failing loudly here beats silently accepting a graph that will
            // never do anything and leaving whoever authored it to wonder why.
            string events = string.Join(", ", graph.EntryPoints.Select(e => e.EventName).Distinct());
            LoadError = err = $"graph declares ENTRY record(s) for [{events}] but GraphHost only " +
                               "drives 'OnStart' and 'OnTick' -- nothing in this graph would ever run";
            Console.Error.WriteLine($"[GraphHost] {LoadError}");
            return false;
        }

        _graph = graph;
        _eventArgSlots = slots;
        _onStart = onStart;
        _onTick = onTick;
        _startInvoked = false;
        _execSimTime = 0f;
        return true;
    }

    /// <summary>Ticks the already-compiled graph for one frame. No-op if Load() never succeeded
    /// (see Ready) -- does not throw, does not log again, does not attempt to recompile.
    ///
    /// For a DATAFLOW graph, entityId is both what gets passed to any declared PARAM entity and what
    /// a position result gets written to, and timeSeconds is passed straight through to PARAM time
    /// exactly as before phase 2.
    ///
    /// For an EVENT-DRIVEN graph, entityId is unchanged (still the PARAM entity/position-write
    /// target), but timeSeconds is read as THIS TICK'S DELTA, not an absolute clock -- see
    /// LoadEventGraph's own comment on why "deltaTime" only exists on this path. The very first call
    /// also fires the compiled OnStart delegate (if the graph declared one) before OnTick runs, which
    /// is what makes "OnStart fires once when play begins" true without a second entry point on this
    /// class: GameApp's game loop calling Tick() for the first time on a freshly loaded graph IS play
    /// beginning for that graph, by construction (see GameApp.cpp's own comment on why a shipped
    /// game has no separate begin-play moment to hook instead).</summary>
    public void Tick(int entityId, float timeSeconds)
    {
        if (_onStart != null || _onTick != null)
        {
            TickEventGraph(entityId, timeSeconds);
            return;
        }
        if (_compiled == null) return;

        var args = new object[_argSlots.Length];
        for (int i = 0; i < _argSlots.Length; i++)
            args[i] = _argSlots[i] == ParamSlot.Entity ? (object)entityId : (object)timeSeconds;

        // DynamicInvoke, not a statically-typed Func<> call: GraphHost does not know at compile
        // time (of THIS C# file) how many PARAMs a given graph declares, so it cannot cast to a
        // fixed Func<...> arity the way individual GraphCompiler tests do. This costs a reflection
        // dispatch per tick -- fine for the handful of graph-driven entities this slice targets;
        // a hot path driving thousands of entities would want GraphHost to specialize by arity
        // (mirroring GetDelegateType's own by-name dispatch) instead of DynamicInvoke, and that is
        // a real, named limitation, not a hidden one.
        object? result = _compiled.DynamicInvoke(args);
        ApplyResult(entityId, result);
    }

    private void TickEventGraph(int entityId, float deltaTime)
    {
        _execSimTime += deltaTime;

        object[] BuildArgs(ParamSlot[] slots)
        {
            var args = new object[slots.Length];
            for (int i = 0; i < slots.Length; i++)
                args[i] = slots[i] switch
                {
                    ParamSlot.Entity    => entityId,
                    ParamSlot.Time      => _execSimTime,
                    ParamSlot.DeltaTime => deltaTime,
                    _ => throw new InvalidOperationException($"unhandled {nameof(ParamSlot)} {slots[i]}"),
                };
            return args;
        }

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

    // Success-path diagnostic, deliberately distinct from the LoadError family's Console.Error use
    // above: this is not a failure, so it goes to stdout. A shipped AverGame.exe is a WIN32-subsystem
    // process with no console of its own, but .NET's Console class still writes to whatever stdout
    // handle the process inherited -- which is exactly the redirected/piped handle a headless
    // `--frames N` capture (or any other launcher that redirects output) provides, so this is not
    // silently lost in exactly the situation it exists to prove things to (see the phase-2 report's
    // end-to-end evidence). A template author gets, for free, one line per fired entry point naming
    // the graph, the event and what it computed -- the same kind of proof this file's own author
    // needed to first confirm CompileEntryPoint's output was really running every frame.
    private void LogEntryFired(string eventName, int entityId, object? result)
    {
        Console.WriteLine($"[GraphHost] '{_graph?.Name}' entity {entityId}: {eventName} -> {DescribeResult(result)}");
    }

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
    }

    private void ApplyResult(int entityId, object? result)
    {
        // GraphCompiler.Compile()/CompileEntryPoint() both return object[] (one boxed value per OUT
        // record, in file order) when a graph has 2+ outputs -- see the OUTPUTS ARRAY comment in
        // GraphCompiler.cs. A 0- or 1-output graph returns void or a bare scalar; GraphHost has no
        // defined meaning for "one number is a position" so it makes no position write for that
        // shape. That graph still ran (any setfield node inside it, or any OnStart/OnTick log line
        // above, already had its effect during DynamicInvoke) -- this method only decides whether a
        // POSITION was among the outputs, identically for a dataflow graph and an event-driven one.
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
