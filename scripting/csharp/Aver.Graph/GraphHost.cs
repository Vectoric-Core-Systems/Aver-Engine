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
//
// PHASE 3 ADDITION (ON-DEMAND events -- "OnHit" is the worked example, not a special case): every
// event this class knew how to run before now was driven by Tick() -- OnStart once, OnTick every
// call. That is wrong for an event a HOST fires when something HAPPENS (a hit lands, an overlap
// begins) rather than on a fixed per-frame cadence, and CompileEntryPoint() already compiled such a
// thing perfectly well before this addition -- the gap was entirely here, in LoadEventGraph (which
// refused to load a graph unless one of its ENTRY records literally named "OnStart" or "OnTick") and
// in the total absence of any method to run a compiled entry point OUTSIDE of Tick(). See Fire()'s
// own doc comment for the on-demand contract. "OnHit" appears below only as an ILLUSTRATIVE EXAMPLE,
// in comments -- no branch, switch or string comparison anywhere in this file singles it out from
// any other non-OnStart/OnTick event name a project might declare. See LoadEventGraph's own comment
// for exactly how a declared event name sorts into "Tick()-driven" versus "Fire()-able".
//
// VARIABLES ADDITION (graph-local persistent state -- "nothing survives between ticks" closed): every
// compiled delegate before this addition was a pure function of its declared PARAMs -- a graph could
// not hold a score, an ammo count, or a cooldown, because nothing outside the DynamicMethod's own
// arguments lived longer than one invocation. See _varStore's own field comment for the storage this
// class now owns, one instance per GraphHost, and GraphVariable/GraphVarStore's own comments for the
// full contract. This is a GraphHost-level addition, same as Phase 2/3 above: GraphCompiler only knows
// how to accept and use a GraphVarStore argument; deciding that ONE instance lives for the lifetime of
// a GraphHost and gets passed to every Tick()/Fire() call on it is entirely this file's own job.
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

    // ---- on-demand (Fire()) path -- new in phase 3 -------------------------------------------------
    // One compiled delegate per declared event name OTHER than "OnStart"/"OnTick" -- see
    // LoadEventGraph's own comment for how a name sorts into this bucket versus the Tick()-driven pair
    // above, and Fire()'s own comment for how one of these gets invoked. Never touched by Tick()/
    // TickEventGraph, exactly as _onStart/_onTick are never touched by Fire() -- two disjoint sets of
    // entry points sharing one compiled Graph, the same "the two halves do not interact" property
    // OcGraph.hpp's own contract already promises for dataflow-vs-exec within a single file.
    private Dictionary<string, Delegate> _onDemand = new();

    // ---- graph-local persistent variables (VAR) -- new in the variables slice -----------------------
    // ONE INSTANCE FIELD, PER GraphHost OBJECT -- this is the entire storage/lifetime contract from
    // this class's side (GraphVarStore itself owns the "how"; see its own comment). Created fresh in
    // LoadFromText(), right after a successful parse and before either compile path runs, from
    // GraphVarStore.CreateFor(graph) -- which seeds every declared VAR at its own default, so a read
    // before any write is always deterministic (see GraphVarStore.CreateFor's own comment). Null when
    // the loaded graph declares no VAR records (the overwhelmingly common case), so Tick()/Fire() below
    // append nothing extra to a VAR-less graph's DynamicInvoke args -- byte-for-behavior-identical to
    // before VAR existed.
    //
    // TWO GraphHost INSTANCES OVER THE SAME .ocgraph FILE THEREFORE GET TWO INDEPENDENT STORES, with no
    // extra work: each host's own LoadFromText() call constructs its own GraphVarStore, and nothing in
    // this class (or GraphVarStore, or GraphCompiler) ever keys storage by file path or shares a static
    // table. This is exactly the property that already stops several actors sharing one idle-motion
    // .ocgraph from stacking at the origin via their own separate PositionSink closures -- VAR's
    // independence rides the same "one GraphHost, one everything" architecture, not a new mechanism.
    // See Aver.Graph.Tests/GraphVarTests.cs's TestTwoGraphHostsOverSameGraphFileHaveIndependentVariables.
    //
    // RELOADING THE SAME GRAPH (a second Load()/LoadFromText() call on this same host) replaces this
    // field with a BRAND NEW store, seeded fresh from declared defaults -- ResetCompiledState() clears
    // it to null first, exactly like every other compiled-state field on this class, so a hot-reloaded
    // graph's variables reset rather than carrying over stale values from whatever compiled before.
    private GraphVarStore? _varStore;

    private Graph? _graph;
    private readonly PositionSink _positionSink;

    // Resolved once, process-wide, the first time the DEFAULT sink is actually used -- not at
    // GraphHost construction, so building a GraphHost never requires a live scene to already exist.
    // 0 mirrors the ABI's own "unknown field" sentinel (SceneAbi.cpp), so this doubles as
    // "not yet resolved" with no separate bool needed.
    private static int s_cachedPositionFieldId;

    /// <summary>Null until a Load() call fails; then the reason, exactly as reported at that time.</summary>
    public string? LoadError { get; private set; }

    /// <summary>True once a graph has compiled successfully and something will actually run it --
    /// EITHER via Tick() (the dataflow delegate, or at least one of OnStart/OnTick) OR on demand via
    /// Fire() (at least one other compiled entry point). A graph loaded with ONLY an on-demand event
    /// (no OnStart/OnTick, no dataflow OUT) is genuinely Ready even though Tick() will do nothing for
    /// it at all -- see Fire()'s own doc comment; that split is deliberate, not a gap.</summary>
    public bool Ready => _compiled != null || _onStart != null || _onTick != null || _onDemand.Count > 0;

    /// <summary>True for a graph loaded via the event-driven (ENTRY/exec) path rather than the
    /// original dataflow one -- Tick()-driven, Fire()-able, or both. Exposed so a caller that cares
    /// (GameApp's discovery log, notably) can say which kind of graph it found without re-deriving it
    /// from Graph.EntryPoints itself.</summary>
    public bool IsEventDriven => _onStart != null || _onTick != null || _onDemand.Count > 0;

    /// <summary>The parsed graph, for inspection (name/description/node count) -- null until a
    /// successful Load(). Exposed read-only; GraphHost owns compiling it, nothing else should.</summary>
    public Graph? Graph => _graph;

    /// <summary>This host's own VAR storage -- null exactly when <see cref="Graph"/> is null, or when
    /// the loaded graph declares no VAR records (see <c>_varStore</c>'s own field comment). Exposed
    /// read-only for the SAME reason <see cref="Graph"/> is: a caller outside this class (a save
    /// provider reading every declared VAR's current value to capture it, or restoring one after a
    /// load) needs to reach the store GraphHost already owns, not a second copy of the idea.</summary>
    public GraphVarStore? VarStore => _varStore;

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

        // THE ONE CHOKEPOINT WHERE A FOREIGN GRAPH IS TURNED AWAY. Both Load(path) and this method
        // funnel here, so refusing once covers every gameplay compile in the process. A material
        // graph's nodes are HLSL arithmetic with names this compiler has never heard of; without
        // this it would get as far as GraphCompiler and fail with "unknown node type", which reads
        // like a broken graph rather than a graph handed to the wrong compiler. See Graph.DomainKind
        // for why an UNRECOGNISED domain is refused here too rather than treated as gameplay.
        if (graph.DomainKind != GraphDomain.Gameplay)
        {
            LoadError = err = $"this is a '{graph.Domain}' graph, not a gameplay graph -- " +
                               "GraphHost compiles gameplay graphs only";
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
        // Committed at the SAME point as _graph/_compiled above, not earlier -- see _varStore's own
        // field comment. A compile failure above already returned false without touching _varStore, so
        // this host keeps whatever it had before (null, after ResetCompiledState(), for a first Load()),
        // preserving the "each call fully replaces whatever compiled before it, all at once" contract
        // this class's own doc comment already promises for every other field.
        _varStore = graph.Variables.Count > 0 ? GraphVarStore.CreateFor(graph) : null;
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

        var compiler = new GraphCompiler(graph);
        bool wantsStart = graph.EntryPoints.Any(e => e.EventName == "OnStart");
        bool wantsTick  = graph.EntryPoints.Any(e => e.EventName == "OnTick");

        // EVERY OTHER declared event name -- "OnHit", or anything a future project invents -- is
        // ON-DEMAND: compiled here, same as OnStart/OnTick, but fired later by a caller through
        // Fire() rather than driven by this class's own Tick() cadence. Nothing below (or in Fire())
        // spells out "OnHit" as a name GraphHost recognises -- ANY name that is not "OnStart" or
        // "OnTick" takes this path, which is what makes a truly new event (not just OnHit) free at
        // this layer, exactly as ENTRY's own parser comment already promised for the FORMAT.
        var onDemandNames = graph.EntryPoints.Select(e => e.EventName)
            .Where(n => n != "OnStart" && n != "OnTick")
            .Distinct()
            .ToList();

        // The {"entity" Int, "time" Float, "deltaTime" Float} ParamSlot convention below exists
        // SOLELY because Tick(entityId, timeSeconds) has a fixed two-argument signature -- those are
        // the only two values this class can hand OnStart/OnTick without a caller supplying anything
        // extra. That constraint has no bearing on an ON-DEMAND event, so the check is now gated on
        // actually wanting OnStart/OnTick rather than applied to every event-driven graph regardless:
        // a graph with ONLY an on-demand entry (no OnStart/OnTick at all) declares whatever PARAM list
        // its payload needs -- `otherEntity`, or anything else -- with NOTHING here to reject it. See
        // Fire()'s own doc comment for why a caller-supplied positional arg list, not a second named-
        // slot vocabulary, is the right contract for a payload this class cannot anticipate the shape
        // of. (This is also the fix for the exact failure this class used to have: an on-demand-only
        // graph declaring `PARAM otherEntity int` used to be rejected HERE, before LoadEventGraph ever
        // got far enough to notice the graph had no OnStart/OnTick entry to begin with.)
        //
        // A graph that mixes an on-demand event with OnStart/OnTick is NOT specially supported either
        // way -- Parameters is ONE list for the WHOLE FILE (see Graph.cs), shared by every ENTRY in
        // it, so a PARAM only the on-demand event needs becomes something OnTick's OWN compiled
        // delegate would also be asked to accept, and Tick() has no value to supply for it. The check
        // below still runs whenever wantsStart/wantsTick is true and still catches exactly that
        // mistake, with the message updated to say so -- which is why "one event's payload gets its
        // own file, separate from any OnStart/OnTick graph" is a real constraint this format enforces
        // on any project authoring on-demand events, not merely a style preference left to an
        // author's discipline.
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

        // Every OTHER declared event name is compiled here too, exactly like OnStart/OnTick just
        // were, and cached by NAME for Fire() to invoke later. Same all-or-nothing swap philosophy as
        // OnStart/OnTick above (a compile failure anywhere fails the WHOLE Load(), not just this one
        // event) -- a graph that sometimes has a working OnHit and sometimes does not, depending on
        // which other entries happened to compile, is exactly the "half-compiled and inconsistent"
        // outcome this class's own doc comment already refuses to produce.
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

        // NO "nothing would ever run" CHECK NEEDED HERE, UNLIKE BEFORE PHASE 3. It used to fire
        // whenever neither OnStart nor OnTick compiled -- correct THEN, because those were the only
        // two things this class knew how to run at all, so an ENTRY naming anything else really was
        // dead weight. That is no longer true: wantsStart, wantsTick and onDemandNames together
        // partition EVERY distinct event name this graph declares (the two Any() checks and the
        // Where/Distinct above cover disjoint, exhaustive cases -- Graph.Validate() already refuses a
        // duplicate ENTRY for the same event name, so there is no fourth bucket for a name to fall
        // into), and a compile failure in ANY bucket already returned false above. A graph reaching
        // this line therefore always leaves with at least one thing runnable, either via Tick() or
        // via Fire() -- which is exactly why LoadFromText only calls this method at all when
        // graph.EntryPoints.Count > 0.
        _graph = graph;
        _eventArgSlots = slots;
        _onStart = onStart;
        _onTick = onTick;
        _onDemand = onDemand;
        _startInvoked = false;
        _execSimTime = 0f;
        // Committed at the SAME point as _graph/_onStart/_onTick/_onDemand above, not earlier -- see
        // the dataflow path's identical comment on this same line shape, and _varStore's own field
        // comment. Shared by OnStart, OnTick AND every on-demand event this file compiles: storage lives
        // at the host/graph level, not per entry point, so Fire("OnHit", ...) and Tick()-driven OnTick
        // in the same file see the SAME variable pool -- the score/ammo/cooldown shape "nothing survives
        // between ticks" was blocking.
        _varStore = graph.Variables.Count > 0 ? GraphVarStore.CreateFor(graph) : null;
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

        // One extra trailing slot for _varStore, appended AFTER every declared PARAM -- see
        // GraphCompiler's own "trailing GraphVarStore parameter" comment for why the compiled
        // delegate's argument order puts it last, never mixed into the PARAM slots above. Zero extra
        // slots (and zero extra work) for the overwhelmingly common VAR-less graph, where _varStore is
        // null and this array is exactly the same shape it always was.
        var args = new object[_argSlots.Length + (_varStore != null ? 1 : 0)];
        for (int i = 0; i < _argSlots.Length; i++)
            args[i] = _argSlots[i] == ParamSlot.Entity ? (object)entityId : (object)timeSeconds;
        if (_varStore != null) args[_argSlots.Length] = _varStore;

        // DynamicInvoke, not a statically-typed Func<> call: GraphHost does not know at compile
        // time (of THIS C# file) how many PARAMs a given graph declares, so it cannot cast to a
        // fixed Func<...> arity the way individual GraphCompiler tests do. This costs a reflection
        // dispatch per tick -- fine for the handful of graph-driven entities this slice targets;
        // a hot path driving thousands of entities would want GraphHost to specialize by arity
        // (mirroring GetDelegateType's own by-name dispatch) instead of DynamicInvoke, and that is
        // a real, named limitation, not a hidden one.
        GraphCallGuard.Reset();   // see the Tick() path for why -- same reason, other entry point
        object? result = _compiled.DynamicInvoke(args);
        ApplyResult(entityId, result);
    }

    private void TickEventGraph(int entityId, float deltaTime)
    {
        _execSimTime += deltaTime;

        object[] BuildArgs(ParamSlot[] slots)
        {
            // Same trailing _varStore slot Tick()'s own dataflow-args loop appends, above -- see that
            // comment. OnStart and OnTick share ONE _varStore (this class's own field, not a per-entry
            // one), which is exactly what lets an OnHit-shaped handler increment a VAR and OnTick read
            // it back later, or vice versa.
            var args = new object[slots.Length + (_varStore != null ? 1 : 0)];
            for (int i = 0; i < slots.Length; i++)
                // (object) ON EVERY ARM IS LOAD-BEARING, NOT STYLE. Without it, the switch expression's
                // arms are `int` (entityId) and two `float`s (_execSimTime/deltaTime) with no explicit
                // target type here (`args[i]` is `object`, but C# still infers the switch expression's
                // OWN natural type first) -- the compiler picks the narrowest type every arm converts
                // TO, which is `float` (int implicitly widens to float; float does not narrow to int).
                // So `entityId` got silently WIDENED to float and boxed as System.Single, and any graph
                // whose PARAM list is `entity int` ALONE (found by this slice's own OnHit test -- see
                // OnHitEventTests.TestOnStartAndOnHitCanShareACompatibleParamList, which failed on this
                // exact line before this fix) threw ArgumentException: "Object of type 'System.Single'
                // cannot be converted to type 'System.Int32'" the moment DynamicInvoke tried to bind it
                // against the compiled delegate's real `int` parameter. Never caught before because no
                // prior test drove an ENTRY-based graph through GraphHost.Tick() (not
                // GraphCompiler.CompileEntryPoint() called directly, which every existing Select/
                // InputKey/Raycast/Spawn test uses instead) with an int-typed PARAM in the mix -- the
                // OLDER dataflow-args loop just above Tick()'s own branch into this method already casts
                // both its ternary arms to (object) for the identical reason; this switch just never
                // got the same treatment when it was added. Casting explicitly here forces the switch
                // expression's natural type to `object`, so each arm boxes AS ITS OWN TYPE instead of
                // being unified to a common numeric type first.
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

        // A COMPILED GRAPH FUNCTION COUNTS ITS OWN RECURSION DEPTH, and the counter is reset here,
        // at every top-level entry, rather than unwound by a try/finally in emitted IL. See
        // GraphCallGuard for the full argument; the short version is that an exception thrown out of
        // a graph function skips the Exit() that would have decremented it, and without this line
        // that leak would accumulate until an innocent later tick tripped the limit.
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
    /// counterpart to Tick(), for whatever declared event name is not "OnStart"/"OnTick" (see
    /// LoadEventGraph's own comment for how a name sorts into this bucket versus the Tick()-driven
    /// pair). Returns false and leaves <paramref name="result"/> null when <paramref name="eventName"/>
    /// was never declared by this graph, or Load() never succeeded -- the same "documented no-op, not
    /// a throw" contract Tick() already has for a graph that failed to compile (see Ready) -- so a
    /// caller does not have to ask "does this particular actor's graph even have an OnHit?" before
    /// firing one at every actor uniformly; it can just call Fire() and check the bool back.
    ///
    /// NOT PARAMSLOT-BASED, DELIBERATELY, UNLIKE Tick(). Tick() can only ever hand a graph the two
    /// values it itself receives (entityId, timeSeconds), so LoadEventGraph maps declared PARAM names
    /// onto a closed, hand-maintained vocabulary of what those two values MEAN ("entity"/"time"/
    /// "deltaTime") -- seeing a PARAM outside that vocabulary is a load-time error precisely because
    /// Tick() would have nothing to put there. Fire()'s caller is different in kind: it is the one
    /// place in this whole class that already HAS whatever a specific event's payload is (who hit me,
    /// where, how hard -- or, for some future event, something else entirely) at the exact moment it
    /// calls this method. Inventing a second named-slot vocabulary here to describe payloads this
    /// class cannot anticipate would only grow forever, one event at a time -- exactly the trap
    /// LoadEventGraph's own comment already declines for PARAM. So <paramref name="args"/> is
    /// POSITIONAL, matching the fired entry's declared PARAM list IN FILE ORDER -- the exact same
    /// contract CompileEntryPoint's own compiled delegate already has (see its doc comment: "the
    /// compiled method's parameters are exactly the graph's declared PARAM list, in declaration
    /// order"). GraphHost adds no translation layer on top of that; it only remembers which compiled
    /// delegate a NAME refers to and forwards the call, the same as Tick() forwards to _onTick.
    ///
    /// <paramref name="result"/> is the entry's OUT value on a true return, by the same zero/one/many
    /// convention Tick()/ApplyResult already use for their own results (null for a void-returning
    /// graph, the boxed scalar for one OUT, an object[] for two-plus) -- but UNLIKE Tick()'s result,
    /// it is NOT auto-applied to a PositionSink: an on-demand event has no fixed entity target the way
    /// Tick(entityId, ...) does (Fire() is not even given one), and a graph that wants to write a
    /// scene value from inside its own exec chain already can, generically, via a SetField/
    /// SetFieldVec3 node on its own exec chain -- this class does not need a second mechanism for the
    /// same thing.</summary>
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

        // Same trailing _varStore slot Tick()/TickEventGraph append -- appended HERE, internally, so
        // the "args must match the graph's PARAM list positionally" contract just checked above stays
        // literally true from the CALLER's perspective; the caller never has to know or care that a
        // variables-store argument exists.
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

    /// <summary>GAP 3's own entry point: the FireEvent-node counterpart to Fire(), for a caller that
    /// does NOT already know this graph's PARAM payload the way Fire()'s own positional-args contract
    /// assumes -- see Fire()'s own doc comment for why args are positional there. A FireEvent node's
    /// router (Aver.Graph.GraphEvents, installed by HostBridge) has only two things in hand at the
    /// call site: which entity to fire at, and which event name -- it does not, and should not, know
    /// the SHAPE of whatever that target graph's own PARAM list happens to be. So this method builds
    /// the positional args ITSELF, from a small, closed vocabulary this class already owns for the
    /// Tick()-driven path (see LoadEventGraph's own "entity"/"time"/"deltaTime" comment): "entity" is
    /// <paramref name="entityId"/> -- the TARGET's OWN entity (not the firer's), so it means exactly
    /// what OnTick's own "entity" PARAM already means inside this same file, for consistency an author
    /// reading either entry point can rely on; "time" is this host's own accumulated sim clock
    /// (_execSimTime, the same value OnTick's "time" PARAM reads); "deltaTime" is always 0f -- an
    /// on-demand event has no duration to report, unlike a per-frame tick.
    ///
    /// ANY OTHER PARAM NAME IS A VISIBLE REFUSAL (refusal names the graph, the event, and the specific
    /// PARAM), NEVER A GUESS. Guessing is the one thing this method must not do: GraphHost.Fire itself
    /// happily WIDENS a mismatched-kind argument rather than rejecting it (a boxed int silently reads
    /// back as a float for a Float-typed PARAM -- confirmed empirically while designing this feature),
    /// so a router that filled an unrecognised PARAM with, say, the firing entity's id regardless of
    /// what that PARAM was actually FOR could silently feed a nonsense value into a graph's own logic
    /// with no error anywhere. Refusing by name instead is the same "fail loudly now beats a silent
    /// wrong answer nobody can trace" reasoning every NODE-line required-attribute check in this
    /// codebase already applies (class=/field=/event=).
    ///
    /// Returns false with a non-null <paramref name="refusal"/> in two disjoint cases: this graph
    /// never declared <paramref name="eventName"/> at all (mirrors Fire()'s own false/null contract
    /// for an undeclared name -- <paramref name="refusal"/> still names it, so a caller does not have
    /// to re-derive the reason), or it declared the event but ALSO declares a PARAM outside the
    /// {entity, time, deltaTime} vocabulary above (this method never even attempts Fire() in that
    /// case -- failing before touching DynamicInvoke at all). Returns true, with
    /// <paramref name="result"/> set to whatever Fire() itself returns (null for a void-returning
    /// event, exactly like Fire()'s own contract), otherwise.</summary>
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
        _onDemand = new();
        _varStore = null;
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
