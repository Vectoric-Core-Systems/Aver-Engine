// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// The runtime storage for a graph's VAR declarations -- the piece that closes "a compiled graph is a
// pure function of its PARAMs; nothing survives between ticks". A DynamicMethod is stateless by
// construction (no static/instance fields of its own, ever), so somewhere OUTSIDE the compiled IL has
// to hold a value across two separate invocations. This class is that somewhere: a small, ordinary
// managed object GraphCompiler's emitted IL calls into (Ldarg the trailing parameter, Ldstr the
// variable's name, Call one of the typed accessors below) and GraphHost owns ONE OF, per instance,
// created fresh at Load() time.
//
// PER-HOST-INSTANCE, NOT PER-GRAPH-FILE -- the single most important property of this class, and the
// one most likely to get silently undone by a well-meaning "optimisation" later. One .ocgraph file is
// deliberately shared by many entities (a demo project in this repo family has one idle-motion graph
// driving three separate targets, each owning its own GraphHost with its own PositionSink -- that
// per-instance independence is exactly what stops them stacking at the origin today, before any
// variable work existed). If storage were keyed by graph FILE PATH (a tempting cache key: "we already
// parsed this file, why not share its variables too?"), all three targets would silently share one
// score. Keying it by HOST INSTANCE instead -- a plain object field on GraphHost, never a static or
// path-keyed table anywhere in this file -- is what keeps them independent. See
// GraphHost's own field comment for where that instance actually lives, and
// Aver.Graph.Tests/GraphVarTests.cs's TestTwoGraphHostsOverSameGraphFileHaveIndependentVariables for
// the test that would fail immediately if this property were ever lost.
//
// WHY A PLAIN Dictionary<string, object> AND NOT SIX TYPED DICTIONARIES OR A STRUCT-OF-ARRAYS LAYOUT.
// The number of VAR records a graph declares is small (a handful at most -- score, ammo, a cooldown or
// two), and this is read/written at most a few times per Tick()/Fire() call, not per-vertex or
// per-pixel. A dictionary keyed by the variable's own name, boxing its value, costs nothing that
// matters at this scale and needs no second name-to-slot table the way a dense array would.
using System;
using System.Collections.Generic;

namespace Aver.Graph;

/// <summary>Per-instance storage for one graph's declared VAR variables. See this file's own header
/// comment for the full "per-host-instance, not per-graph-file" contract and why that is the load-
/// bearing property of this class.
///
/// A WRITE IS A SIDE EFFECT (see GraphCompiler.IsExecCapableVarSideEffectType): every method here is
/// safe to call any number of times in any order from GENERATED IL -- there is no notion here of
/// "reached twice by mistake" the way a native field write has one (unknown entity, read-only field).
/// The compiler is what enforces that a write only ever happens through the exec chain, exactly once
/// per visit; this class itself has no opinion and would not notice either way.</summary>
public sealed class GraphVarStore
{
    private readonly Dictionary<string, object> _values = new();

    /// <summary>Reads a Float variable. 0f if never set (should not happen in practice --
    /// <see cref="CreateFor"/> seeds every declared variable before any compiled delegate ever runs --
    /// but a defined fallback beats an exception for a name that was, say, declared Int and mis-typed
    /// here by a caller bypassing the compiler's own type checking).</summary>
    public float GetFloat(string name) => _values.TryGetValue(name, out var v) && v is float f ? f : 0f;

    /// <summary>Writes a Float variable, creating it if this is somehow the first write (ordinary
    /// operation always finds it already seeded by <see cref="CreateFor"/>).</summary>
    public void SetFloat(string name, float value) => _values[name] = value;

    /// <summary>Reads an Int variable. 0 if never set -- see <see cref="GetFloat"/>'s own comment.</summary>
    public int GetInt(string name) => _values.TryGetValue(name, out var v) && v is int i ? i : 0;

    /// <summary>Writes an Int variable.</summary>
    public void SetInt(string name, int value) => _values[name] = value;

    /// <summary>Reads a Bool variable. false if never set -- see <see cref="GetFloat"/>'s own comment.</summary>
    public bool GetBool(string name) => _values.TryGetValue(name, out var v) && v is bool b ? b : false;

    /// <summary>Writes a Bool variable.</summary>
    public void SetBool(string name, bool value) => _values[name] = value;

    /// <summary>Builds a FRESH store, seeded from every VAR <paramref name="graph"/> declares, using
    /// each one's own GraphVariable.Default (itself already resolved to a concrete zero-or-declared
    /// value by OcGraphParser -- see that class's VAR-parsing comment). This is what makes "a variable
    /// read before any write returns its declared default, deterministically" true: it runs ONCE, at
    /// Load() time (see GraphHost's own field comment), strictly before the compiled delegate this
    /// store will be handed to is ever invoked -- there is no code path that reads a GraphVarStore
    /// before this method has populated it.
    ///
    /// Exposed publicly, not only called from GraphHost internally, so a test exercising
    /// GraphCompiler.Compile()/CompileEntryPoint() directly (bypassing GraphHost entirely) can build a
    /// store with the EXACT SAME seeding rule GraphHost itself uses, rather than re-deriving it and
    /// risking the two drifting apart.</summary>
    public static GraphVarStore CreateFor(Graph graph)
    {
        var store = new GraphVarStore();
        foreach (var v in graph.Variables)
        {
            switch (v.Type)
            {
                case PinType.Float:
                    store.SetFloat(v.Name, v.Default is float f ? f : 0f);
                    break;
                case PinType.Int:
                    store.SetInt(v.Name, v.Default is int i ? i : 0);
                    break;
                case PinType.Bool:
                    store.SetBool(v.Name, v.Default is bool b ? b : false);
                    break;
                default:
                    // Unreachable through the text parser (OcGraphParser rejects VAR ... exec at parse
                    // time, and PinType has no other members), but a Graph built programmatically
                    // (Graph.Validate's own note on why it re-checks parser-time things) could reach
                    // this, so it fails loudly and names the culprit rather than silently seeding
                    // nothing for a variable that will then read back as whatever CLR default happens
                    // to apply.
                    throw new InvalidOperationException(
                        $"VAR '{v.Name}' has type {v.Type}, which GraphVarStore has no accessor for -- " +
                        "VAR only supports Float/Int/Bool, the same restriction PARAM already has");
            }
        }
        return store;
    }
}
