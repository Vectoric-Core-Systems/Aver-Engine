// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GameThread: deferred calls, end-of-frame destroys and an await that resumes on the game thread (Godot's
// call_deferred, queue_free and await).

using System.Collections.Concurrent;
using Aver.Scripting;

namespace Aver.Framework;

/// <summary>Work for the game thread at the end of the current tick pass (Godot's <c>call_deferred</c>).</summary>
public static class Deferred
{
    /// <summary>Runs <paramref name="action"/> on the game thread when the current tick pass ends. Safe to call from
    /// any thread; a call queued while the queue flushes runs in the same flush.</summary>
    public static void Call(Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        GameThread.Post(static a => ((Action)a!)(), action);
    }
}

/// <summary>Awaitable points in the frame. An <c>await</c> started on the game thread resumes there.</summary>
public static class Frame
{
    /// <summary>Completes at the start of the next OnTick pass (Godot's <c>process_frame</c>).</summary>
    public static Task Next() => GameThread.WaitFor(physics: false);

    /// <summary>Completes at the start of the next OnPhysicsTick pass (Godot's <c>physics_frame</c>).</summary>
    public static Task NextPhysics() => GameThread.WaitFor(physics: true);

    /// <summary>Completes after <paramref name="seconds"/> of game time: scaled by the time scale and held while paused
    /// (Godot's <c>create_timer(seconds).timeout</c>).</summary>
    public static Task Delay(float seconds)
    {
        var done = new TaskCompletionSource();
        Timers.Set(MathF.Max(seconds, 0f), () => done.TrySetResult());
        return done.Task;
    }
}

// The game thread's queue: deferred calls and await continuations, flushed at the end of each tick pass.
internal static class GameThread
{
    // Enough for any honest frame; a call that keeps re-queuing itself is cut off and finishes next flush.
    private const int MaxPerFlush = 100_000;

    private static readonly ConcurrentQueue<(SendOrPostCallback Fn, object? State)> s_queue = new();
    private static readonly ConcurrentQueue<AverActor> s_destroys = new();
    private static List<TaskCompletionSource> s_nextProcess = new(), s_nextPhysics = new();
    private static int s_threadId = -1;

    internal static bool OnGameThread => Environment.CurrentManagedThreadId == s_threadId;

    // Makes the calling thread the game thread and routes its awaits through the queue.
    internal static void Install()
    {
        s_threadId = Environment.CurrentManagedThreadId;
        SynchronizationContext.SetSynchronizationContext(GameSyncContext.Instance);
    }

    internal static void Post(SendOrPostCallback fn, object? state) => s_queue.Enqueue((fn, state));

    internal static void QueueDestroy(AverActor actor) => s_destroys.Enqueue(actor);

    internal static Task WaitFor(bool physics)
    {
        var tcs = new TaskCompletionSource();
        lock (s_queue) (physics ? s_nextPhysics : s_nextProcess).Add(tcs);
        return tcs.Task;
    }

    // The start of a tick pass: wakes what awaited it, then runs the continuations before any actor ticks.
    internal static void BeginPass(bool physics)
    {
        List<TaskCompletionSource> due;
        lock (s_queue)
        {
            due = physics ? s_nextPhysics : s_nextProcess;
            if (due.Count > 0) { if (physics) s_nextPhysics = new(); else s_nextProcess = new(); }
        }
        foreach (TaskCompletionSource t in due) t.TrySetResult();
        Flush();   // those continuations, and anything a timer or event queued since the last pass
    }

    // Runs every queued call, including ones queued while flushing, up to MaxPerFlush. Returns how many ran.
    internal static int Flush()
    {
        int n = 0;
        while (n < MaxPerFlush && s_queue.TryDequeue(out var item))
        {
            ++n;
            try { item.Fn(item.State); }
            catch (Exception ex) { Log.Error($"[Deferred] a deferred call threw: {ex.GetType().Name}: {ex.Message}"); }
        }
        if (n == MaxPerFlush && !s_queue.IsEmpty)
            Log.Warn($"[Deferred] {MaxPerFlush} calls in one flush; the rest wait for the next (a call re-queuing itself?)");
        return n;
    }

    // The end of the frame: deferred calls, then the destroys queued with QueueDestroy (Godot's queue_free).
    internal static void EndFrame()
    {
        Flush();
        while (s_destroys.TryDequeue(out AverActor? a))
            if (a.Self.IsValid) a.Self.Destroy();
        Flush();
    }

    // Drops everything: the script context that queued it is being unloaded.
    internal static void Reset()
    {
        s_queue.Clear();
        s_destroys.Clear();
        lock (s_queue) { s_nextProcess = new(); s_nextPhysics = new(); }
    }

    private sealed class GameSyncContext : SynchronizationContext
    {
        internal static readonly GameSyncContext Instance = new();

        public override void Post(SendOrPostCallback d, object? state) => GameThread.Post(d, state);

        // Inline on the game thread; elsewhere it queues and waits for the game thread to run it.
        public override void Send(SendOrPostCallback d, object? state)
        {
            if (OnGameThread) { d(state); return; }
            using var done = new ManualResetEventSlim();
            Exception? error = null;
            GameThread.Post(_ => { try { d(state); } catch (Exception ex) { error = ex; } finally { done.Set(); } }, null);
            done.Wait();
            if (error != null) throw error;
        }

        public override SynchronizationContext CreateCopy() => this;
    }
}
