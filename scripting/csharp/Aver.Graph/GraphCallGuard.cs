// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
using System;

namespace Aver.Graph;

/// The recursion depth counter every compiled graph FUNCTION increments on entry and decrements on
/// exit.
///
/// WHY THIS EXISTS AT ALL. Graph functions can recurse — that is the difference between this design
/// and inlining, and it is a capability an author asked for. A recursive function with no base case,
/// or with a base case whose condition an author got backwards, would recurse until the CLR stack is
/// gone. A StackOverflowException **cannot be caught**: the process dies. In a running editor that
/// means the author loses whatever they had not saved, for a mistake the graph could have reported.
///
/// So a graph's recursion is bounded by a counter in ordinary managed code, and going past the bound
/// throws an InvalidOperationException that GraphHost catches like any other graph error — the same
/// choice GraphCompiler.MaxLoopIterations already makes for While/ForEach, and for the same reason
/// its own comment gives: a graph author's mistake must not be able to take down what is running the
/// graph.
///
/// NOT ENFORCED WITH try/finally IN EMITTED IL. A protected region has verifiability rules about
/// branching out of it that the branch and loop emitters would each have to learn, to buy a
/// guarantee that is not needed here: Reset() runs at the start of every top-level invocation, so a
/// depth leaked by an exception unwinding past an Exit() cannot accumulate from one call to the next.
///
/// [ThreadStatic] because two threads ticking two GraphHosts share nothing else, and a shared
/// counter would make one graph's depth limit depend on what an unrelated graph was doing.
public static class GraphCallGuard
{
    /// The cap. Deep enough that no hand-authored graph reaches it by accident — a recursive
    /// factorial or a tree walk over a game's worth of entities is far below — and shallow enough
    /// that hitting it leaves plenty of real stack, since each graph frame is a DynamicMethod frame
    /// carrying its own locals rather than a cheap one.
    ///
    /// Deliberately in the same range as Unreal's own Blueprint recursion limit (120), for the same
    /// reason the pin colours match: an author arriving from there should find their intuitions
    /// intact rather than have to relearn a number.
    public const int MaxDepth = 120;

    [ThreadStatic] private static int _depth;

    /// Called at the top of every compiled function body. Takes the function's name only so the
    /// error can say which one ran away — the counter itself is global to the call chain, not
    /// per-function, because mutual recursion between two functions is exactly as fatal as one
    /// function recursing into itself and must be caught by the same count.
    public static void Enter(string funcName)
    {
        if (++_depth > MaxDepth)
        {
            // Decrement before throwing: this frame is not going to reach its own Exit(), and
            // leaving the count raised would make the NEXT top-level call start closer to the cap.
            // Reset() covers that too, but a guard that only works because something else cleans up
            // after it is one bug away from not working.
            --_depth;
            throw new InvalidOperationException(
                $"graph function '{funcName}' recursed more than {MaxDepth} deep. That is almost " +
                "always a missing or inverted base case -- a Branch that never takes the exit side. " +
                "The call was stopped rather than allowed to exhaust the stack, which would have " +
                "taken the process down uncatchably.");
        }
    }

    /// Called immediately before every compiled function body's single Ret.
    public static void Exit()
    {
        if (_depth > 0) --_depth;
    }

    /// Called by the host before each top-level invocation of a compiled graph. See the class comment
    /// for why this, rather than try/finally in emitted IL, is what makes a leaked depth harmless.
    public static void Reset() => _depth = 0;

    /// Current depth. For tests, which otherwise have no way to prove Exit() actually runs -- a guard
    /// that only ever increments would pass every "does it stop runaway recursion" test and still be
    /// broken for ordinary repeated calls.
    public static int Depth => _depth;
}
