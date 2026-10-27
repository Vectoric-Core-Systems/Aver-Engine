// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GameThreadTests: Godot's call_deferred and await on the game thread -- what runs when, in what order,
// and from which thread. A bare process: this test's thread plays the game thread.

using Aver.Framework;

static class GameThreadTests
{
    public static int RunAll() => T.Suite(() =>
    {
        SynchronizationContext? before = SynchronizationContext.Current;
        GameThread.Install();
        try
        {
            T.Run("deferred: runs at the flush, in order, a call queued while flushing in the same flush", DeferredOrder);
            T.Run("deferred: a call that throws does not stop the rest", DeferredThrows);
            T.Run("await Frame.Next resumes at the start of the next OnTick pass, on the game thread", AwaitNext);
            T.Run("await Frame.NextPhysics ignores OnTick passes", AwaitPhysics);
            T.Run("another thread's Send waits for the game thread to run it", SendFromWorker);
            T.Run("reset drops queued calls and waiters", ResetDrops);
        }
        finally
        {
            GameThread.Reset();
            SynchronizationContext.SetSynchronizationContext(before);
        }
    });

    private static void DeferredOrder()
    {
        var seen = new List<int>();
        Deferred.Call(() => { seen.Add(1); Deferred.Call(() => seen.Add(3)); });
        Deferred.Call(() => seen.Add(2));
        T.Check("nothing runs before the flush", seen.Count == 0);
        T.Check("the flush runs three calls", GameThread.Flush() == 3);
        T.Check("in queue order, the re-queued one last", seen.SequenceEqual(new[] { 1, 2, 3 }));
        T.Check("an empty flush runs nothing", GameThread.Flush() == 0);
    }

    private static void DeferredThrows()
    {
        bool after = false;
        Deferred.Call(() => throw new InvalidOperationException("boom"));
        Deferred.Call(() => after = true);
        GameThread.Flush();
        T.Check("the call after the throwing one still ran", after);
    }

    private static void AwaitNext()
    {
        int thread = -1;
        bool resumed = false;
        async Task Body()
        {
            await Frame.Next();
            thread = Environment.CurrentManagedThreadId;
            resumed = true;
        }
        Task t = Body();
        GameThread.Flush();
        T.Check("not resumed by a flush alone", !resumed);
        GameThread.BeginPass(physics: true);
        T.Check("not resumed by a physics pass", !resumed);
        GameThread.BeginPass(physics: false);
        T.Check("resumed at the start of the OnTick pass", resumed && t.IsCompletedSuccessfully);
        T.Check("on the game thread", thread == Environment.CurrentManagedThreadId);
    }

    private static void AwaitPhysics()
    {
        int steps = 0;
        async Task Body()
        {
            await Frame.NextPhysics();
            ++steps;
            await Frame.NextPhysics();
            ++steps;
        }
        Body();
        GameThread.BeginPass(physics: false);
        T.Check("an OnTick pass does not wake it", steps == 0);
        GameThread.BeginPass(physics: true);
        T.Check("one physics pass resumes it once", steps == 1);
        GameThread.BeginPass(physics: true);
        T.Check("the next physics pass resumes the second await", steps == 2);
    }

    private static void SendFromWorker()
    {
        int ranOn = -1;
        SynchronizationContext ctx = SynchronizationContext.Current!;
        var worker = Task.Run(() => ctx.Send(_ => ranOn = Environment.CurrentManagedThreadId, null));
        for (int i = 0; i < 200 && !worker.IsCompleted; ++i)
        {
            GameThread.Flush();
            Thread.Sleep(5);
        }
        T.Check("the worker's Send completed", worker.IsCompletedSuccessfully);
        T.Check("and ran on the game thread", ranOn == Environment.CurrentManagedThreadId);
    }

    private static void ResetDrops()
    {
        bool ran = false, resumed = false;
        Deferred.Call(() => ran = true);
        async Task Body() { await Frame.Next(); resumed = true; }
        Body();
        GameThread.Reset();
        GameThread.BeginPass(physics: false);
        GameThread.Flush();
        T.Check("the queued call was dropped", !ran);
        T.Check("the waiter was dropped", !resumed);
    }
}
