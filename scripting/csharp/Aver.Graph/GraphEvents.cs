// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The seam FireEvent's compiled IL calls -- the ONLY place a graph reaches ANOTHER entity's GraphHost.
using System;

namespace Aver.Graph;

/// <summary>Routes a FireEvent node's runtime call to whichever GraphHost is bound to the target
/// entity, WITHOUT this assembly ever naming the table that binding lives in.
///
/// WHY A DELEGATE, NOT A DIRECT CALL. The actual entity-to-GraphHost lookup is two dictionaries
/// (s_graphInstances -- a class-spawned instance's own host -- and s_graphs -- the drone/project-
/// graph table) that live in Aver.Scripting.Bridge's HostBridge. The assembly reference graph is
/// Aver.Scripting.Bridge -> Aver.Graph -> Aver.Framework -> Aver.Scene: Aver.Graph sits BELOW
/// Aver.Scripting.Bridge, so it cannot reference HostBridge without a circular project reference.
/// This mirrors Aver.Framework.Actors.Resolver exactly -- a plain static delegate field on the
/// LOWER assembly, installed once by HostBridge.SetupManagedActors at bootstrap -- for the identical
/// reason: Aver.Framework cannot see HostBridge either, and already solved this the same way for
/// Actors.Get. See GraphCompiler's own FireEventMethod for the emitted IL call site: a plain static-
/// method call on THIS class, exactly like every other GraphInterop wrapper GraphCompiler already
/// calls by reflection -- no native ABI, no ScriptHost export, because the whole call graph (compiled
/// IL -> this router -> the target's own GraphHost.Fire) never leaves managed code. See this task's
/// own report for why a native "ScriptHost::graphFire" seam was investigated and NOT added: nothing
/// in native code fires a graph event today (no physics contact, no engine-side hit calls into
/// scripting at all -- grepped and confirmed empty), so an export with no caller would be dead code,
/// not a seam.</summary>
public static class GraphEvents
{
    /// <summary>Installed once, at bootstrap, by HostBridge.SetupManagedActors -- resolves (target
    /// entity, event name) to "did something receive and run it". Null in any host that never installs
    /// one (a bare unit test, notably, or a project with no scripting host at all), in which case
    /// <see cref="FireEventForGraph"/> refuses cleanly (false, a log line) rather than throwing a
    /// NullReferenceException from inside compiled IL -- the same "documented no-op, not a crash"
    /// contract every other GraphInterop wrapper's own missing-dependency path already has (compare
    /// GraphInterop.CharacterMoveForGraph's "no live actor" branch, reached the identical way when
    /// Actors.Resolver itself is uninstalled).</summary>
    public static Func<int, string, bool>? Router;

    // THE REENTRANCY GUARD. Three shapes this slice's own brief names explicitly, and what happens to
    // each:
    //   1. SELF-FIRE (a node fires the event its own OnHit/OnTick chain is already running inside):
    //      the router looks up the SAME GraphHost and calls Fire() again while the outer Fire()/Tick()
    //      call is still on the C# call stack. GraphHost.Fire has no reentrancy guard of its own (see
    //      its own doc comment) -- nothing there corrupts (DynamicMethod locals are per-invocation),
    //      so a single self-fire completes normally. It is bullet 3 below (recursion) that catches an
    //      UNBOUNDED self-fire.
    //   2. MUTUAL FIRE (graph A's handler fires an event at graph B, whose handler fires one back at
    //      A): unbounded without a guard -- each call nests one C# stack frame deeper
    //      (FireEventForGraph -> Router -> GraphHost.Fire -> compiled IL -> Call FireEventForGraph ->
    //      ...) forever. A StackOverflowException is UNCATCHABLE in .NET (see below) -- there is no
    //      try/catch anywhere in this codebase, including HostBridge's own per-entity Tick/Fire
    //      wrappers, that can survive it. The process dies.
    //   3. A FIRE DURING A HANDLER THAT FIRES AGAIN (the general case bullets 1 and 2 are both
    //      instances of): any chain of Fire calls, however many distinct graphs it passes through,
    //      that never bottoms out.
    //
    // ONE COUNTER, NOT PER-ENTITY-PAIR TRACKING, IS THE CHOSEN GUARD -- a depth cap mirrors
    // GraphCompiler's own MaxLoopIterations guard for while/forEach EXACTLY: past the cap, log once
    // (naming the chain) and refuse (false), so a runaway chain costs one tick's worth of correctness
    // instead of the process. A per-pair "who already fired whom this chain" set would also work and
    // would distinguish "legitimate deep but finite chain" from "genuine cycle" more precisely, but
    // this game's own event chains (a hit forwarding a score update forwarding a UI ping) are not
    // expected to run more than two or three deep -- MaxDepth is chosen well above that and well below
    // any real .NET stack's own limit (a few thousand frames of trivial methods), so it trips on a
    // genuine runaway long before the CLR's own stack would, which is the entire point: the graceful
    // "false, logged" refusal below must fire BEFORE the hard, uncatchable crash would.
    //
    // [ThreadStatic], NOT A PLAIN STATIC INT -- every managed dispatch thunk in HostBridge runs on the
    // single game thread today (see HostBridge.cs's own header comment), so this is not defending
    // against real concurrent Fire chains; it is defending against ONE Fire chain's depth being
    // corrupted by an UNRELATED chain that happened to start (and finish, incrementing then
    // decrementing) on a different thread first, if this process is ever hosted from more than one
    // thread in the future. Costs nothing today and cannot be wrong later.
    [ThreadStatic] private static int s_depth;

    /// <summary>How many FireEvent calls may be nested on one call stack before this router refuses
    /// rather than recurse further -- see s_depth's own comment for the full reasoning. Chosen well
    /// above any legitimate chain this engine's own content is expected to build (a hit forwarding one
    /// or two further events) and well below where an accidental C# StackOverflowException could ever
    /// be reached first.</summary>
    public const int MaxDepth = 8;

    /// <summary>The method FireEvent's compiled IL calls directly (see GraphCompiler.FireEventMethod).
    /// Returns false -- never throws -- on every refusal: no router installed, the depth guard
    /// tripped, or the router itself reports the target had nothing to fire at (see HostBridge's own
    /// router closure for the entity-has-no-graph / graph-has-no-such-event split, each logged with
    /// its own message).</summary>
    public static bool FireEventForGraph(int targetEntity, string eventName)
    {
        var router = Router;
        if (router == null)
        {
            Console.Error.WriteLine(
                $"[Graph] FireEvent: entity {targetEntity} event '{eventName}' not fired -- no router " +
                "installed (no live scripting host in this process?)");
            return false;
        }

        if (s_depth >= MaxDepth)
        {
            Console.Error.WriteLine(
                $"[Graph] FireEvent: refusing to fire '{eventName}' at entity {targetEntity} -- " +
                $"{MaxDepth} FireEvent calls are already nested on this call stack (a self-fire or a " +
                "cycle between two or more graphs firing each other). This tick's chain loses one " +
                "event rather than overflowing the stack.");
            return false;
        }

        s_depth++;
        try
        {
            return router(targetEntity, eventName);
        }
        finally
        {
            s_depth--;
        }
    }
}
