// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Hosts one compiled graph against one entity, ticked once per frame.
// Comment explains WHY: GraphCompiler proves a graph CAN become IL; nothing before this file
// loaded a graph from disk, kept the compiled delegate around, and fed it a live time value every
// frame. That is the missing piece between "the compiler works" and "a graph moves something in
// the scene" -- this class is exactly that piece and nothing more.

using System;
using System.Collections.Generic;
using System.IO;
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
/// COMPILE ERRORS: if the graph fails to parse or compile, <see cref="Load"/> returns false, sets
/// <see cref="LoadError"/>, and logs once. <see cref="Ready"/> is then false and <see cref="Tick"/>
/// becomes a documented no-op -- it does not throw every frame, and it does not retry compilation.
///
/// RUNTIME ERRORS are a different thing and are NOT swallowed: if invoking the compiled delegate
/// throws (for example, no native Aver.Scene library is loaded so the emitted P/Invoke call inside
/// a getfield/setfield node cannot resolve), that exception propagates out of Tick() exactly as any
/// other unhandled per-frame bug would. Hiding that would be the same mistake this codebase has
/// already made once (see aver-unbacked-verification): a caller that wants tick failures to be
/// non-fatal must catch around its own Tick() call and decide what "safe" means for its frame loop,
/// because GraphHost cannot tell a transient hiccup from a graph that will never work.
///
/// PARAMETER WIRING: GraphHost only knows how to supply two things to a compiled graph's PARAMs --
/// the entity id and the current time -- because those are the only two values <see cref="Tick"/>
/// itself receives. A graph's PARAM records must therefore be a subset of {"entity" (Int), "time"
/// (Float)}; anything else fails at Load() with a clear message naming the offending PARAM, rather
/// than Tick() silently passing a wrong or default value every frame.
///
/// OUTPUT WIRING: a graph with 2 or 3 OUT records is read as (x, y[, z]) -- missing z defaults to
/// 0 -- and applied via the PositionSink. A graph with 0 or 1 OUT records is still ticked (so its
/// own setfield side effects, if any, still run) but GraphHost does not know how to turn a single
/// scalar into a position, so it makes no position write for that shape; this is a scope decision,
/// not a silent failure -- see ApplyResult.</summary>
public class GraphHost
{
    private enum ParamSlot { Entity, Time }

    private Delegate? _compiled;
    private Graph? _graph;
    private ParamSlot[] _argSlots = Array.Empty<ParamSlot>();
    private readonly PositionSink _positionSink;

    // Resolved once, process-wide, the first time the DEFAULT sink is actually used -- not at
    // GraphHost construction, so building a GraphHost never requires a live scene to already exist.
    // 0 mirrors the ABI's own "unknown field" sentinel (SceneAbi.cpp), so this doubles as
    // "not yet resolved" with no separate bool needed.
    private static int s_cachedPositionFieldId;

    /// <summary>Null until a Load() call fails; then the reason, exactly as reported at that time.</summary>
    public string? LoadError { get; private set; }

    /// <summary>True once a graph has compiled successfully and Tick() will actually run it.</summary>
    public bool Ready => _compiled != null;

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
            _compiled = null;
            _graph = null;
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
        _compiled = null;
        _graph = null;
        LoadError = null;
        err = null;

        if (!OcGraphParser.Parse(ocgraphText, out var graph, out var parseErr))
        {
            LoadError = err = $"Parse error: {parseErr}";
            Console.Error.WriteLine($"[GraphHost] {LoadError}");
            return false;
        }

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

    /// <summary>Ticks the already-compiled graph for one frame. No-op if Load() never succeeded
    /// (see Ready) -- does not throw, does not log again, does not attempt to recompile. entityId is
    /// both what gets passed to any declared PARAM entity and what a position result gets written
    /// to.</summary>
    public void Tick(int entityId, float timeSeconds)
    {
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

    private void ApplyResult(int entityId, object? result)
    {
        // GraphCompiler.Compile() returns object[] (one boxed value per OUT record, in file order)
        // when a graph has 2+ outputs -- see the OUTPUTS ARRAY comment in GraphCompiler.cs. A
        // 0- or 1-output graph returns void or a bare scalar; GraphHost has no defined meaning for
        // "one number is a position" so it makes no position write for that shape. That graph still
        // ran (any setfield node inside it already had its effect during DynamicInvoke above) --
        // this method only decides whether a POSITION was among the outputs.
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
