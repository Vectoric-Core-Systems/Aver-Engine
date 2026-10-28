// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// SignalTests: Callable (methods, delegates, bind, unbind, deferred) and the signal system with every ConnectFlags value.

using Aver.Fabric;

static class SignalTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("Callable: a method by name, a delegate, validity", CallableBasics);
        T.Run("Callable: bind and unbind", CallableBind);
        T.Run("Signal: connect, emit with arguments, disconnect", ConnectEmit);
        T.Run("Signal: only declared signals exist", Declared);
        T.Run("Signal flags: OneShot", OneShot);
        T.Run("Signal flags: Deferred", Deferred);
        T.Run("Signal flags: ReferenceCounted", RefCounted);
        T.Run("Signal flags: Persist is accepted", Persist);
        T.Run("Signal: a connection to a freed object goes away", FreedTarget);
        T.Run("Signal: a handler that throws does not stop the others", Throwing);
        T.Run("Signal: block signals, user signals, connection list", Misc);
        T.Run("Signal: delegate handlers (Callable.From)", DelegateHandlers);
        T.Run("Signal: await ToSignal resumes inside the emit", Awaiting);
        T.Run("Signal: built-in signals of Node and Timer", BuiltIn);
    });

    private static void CallableBasics()
    {
        var e = new Emitter();
        var c = new Callable(e, "on_hit");
        c.Call(5);
        T.Check("a snake_case method name reaches the C# method", e.Last == 5);
        T.Check("target and method", c.Target == e && c.Method == "on_hit" && c.IsValid() && !c.IsNull);
        int sum = 0;
        var d = Callable.From((int a, int b) => sum = a + b);
        d.Call(2, 3);
        T.Check("a delegate is called with converted arguments", sum == 5);
        d.Call(4.9, 2);
        T.Check("a double argument converts to int", sum == 6);
        var f = Callable.From((int a) => a * 2);
        T.Check("a delegate's return value comes back", f.Call(21).AsInt32() == 42);
        bool threw = false;
        try { d.Call(1); } catch (ArgumentException) { threw = true; }
        T.Check("wrong argument count throws", threw);
        T.Check("equal callables", new Callable(e, "on_hit") == c && new Callable(e, "on_died") != c);
        Callable same = Callable.From(e.OnDied);
        T.Check("a method group delegate compares equal to itself", same == Callable.From(e.OnDied));
        T.Check("the null callable", default(Callable).IsNull && !default(Callable).IsValid());
        e.Free();
        T.Check("a freed target makes it invalid", !c.IsValid() && !same.IsValid());
        bool freedThrew = false;
        try { c.Call(1); } catch (ObjectDisposedException) { freedThrew = true; }
        T.Check("and calling it throws", freedThrew);
    }

    private static void CallableBind()
    {
        var log = new List<string>();
        var c = Callable.From((string a, string b, string c2) => log.Add($"{a}{b}{c2}"));
        c.Bind("Y").Call("X", "-");
        T.Check("bound arguments follow the call's", log[^1] == "X-Y");
        c.Bind("1").Bind("2").Call("X");
        T.Check("a later bind comes before an earlier one", log[^1] == "X21");
        var two = Callable.From((string a, string b) => log.Add($"{a}{b}"));
        // bind(B).unbind(1): the outer unbind drops the caller's last argument, then B is added for the delegate.
        two.Bind("B").Unbind(1).Call("a", "dropped");
        T.Check("unbind drops the caller's last argument before the bound ones are added", log[^1] == "aB");
        // unbind(1).bind(B): B is added first, and the inner unbind drops it again.
        two.Unbind(1).Bind("B").Call("a", "kept");
        T.Check("unbind applied after bind drops the bound argument", log[^1] == "akept");
        var e = new Emitter();
        new Callable(e, "on_named").Bind(7.5).Call("Ann");
        T.Check("bind on a method callable", e.Got[^1] == "named Ann 7.5");
        T.Check("target and validity survive binding", new Callable(e, "on_hit").Bind(1).Target == e && new Callable(e, "on_hit").Bind(1).IsValid());
    }

    private static void ConnectEmit()
    {
        var e = new Emitter();
        var target = new Emitter();
        Error err = e.Connect("Hit", new Callable(target, "on_hit"));
        T.Check("connect returns Ok", err == Error.Ok && e.IsConnected("Hit", new Callable(target, "on_hit")) && e.HasConnections("Hit"));
        T.Check("emit calls the handler with the arguments", e.EmitSignal("Hit", 12) == Error.Ok && target.Last == 12);
        e.Connect(Emitter.SignalName.Named, new Callable(target, "on_named"));
        e.EmitSignal(Emitter.SignalName.Named, "Ann", 2.5);
        T.Check("several arguments of several types", target.Got[^1] == "named Ann 2.5");
        e.Connect("Died", Callable.From(() => target.Got.Add("lambda died")));
        e.EmitSignal("Died");
        T.Check("a lambda with no arguments", target.Got[^1] == "lambda died");
        e.Disconnect("Hit", new Callable(target, "on_hit"));
        e.EmitSignal("Hit", 99);
        T.Check("disconnect stops delivery", target.Last == 12 && !e.IsConnected("Hit", new Callable(target, "on_hit")));
        T.Check("handlers run in connection order", OrderOfHandlers());
        T.Check("the SignalName constants match the declared names", Emitter.SignalName.Hit == "Hit" && Node.SignalName.TreeEntered == "tree_entered" && Node.SignalName.Ready == "ready");
    }

    private static bool OrderOfHandlers()
    {
        var e = new Emitter();
        var order = new List<int>();
        e.Connect("Hit", Callable.From((int d) => order.Add(1)));
        e.Connect("Hit", Callable.From((int d) => order.Add(2)));
        e.Connect("Hit", Callable.From((int d) => order.Add(3)));
        e.EmitSignal("Hit", 0);
        return order.SequenceEqual(new[] { 1, 2, 3 });
    }

    private static void Declared()
    {
        var e = new Emitter();
        T.Check("[Signal] delegates are signals, without the EventHandler suffix", e.HasSignal("Hit") && e.HasSignal("Died") && e.HasSignal("Named"));
        T.Check("so are the built-in ones", e.HasSignal("tree_entered") && e.HasSignal("child_entered_tree") && e.HasSignal("renamed") && e.HasSignal("ready"));
        T.Check("an unknown signal does not exist", !e.HasSignal("nope"));
        T.Check("connecting to an unknown signal fails", e.Connect("nope", Callable.From(() => { })) == Error.InvalidParameter && T.ErrorCount("no signal") == 1);
        T.Check("emitting an unknown signal fails", e.EmitSignal("nope") == Error.Unavailable);
        T.Check("connecting a null callable fails", e.Connect("Hit", default) == Error.InvalidParameter);
    }

    private static void OneShot()
    {
        var e = new Emitter();
        int n = 0;
        e.Connect("Hit", Callable.From((int d) => n++), (uint)GodotObject.ConnectFlags.OneShot);
        e.EmitSignal("Hit", 1);
        e.EmitSignal("Hit", 1);
        T.Check("a one-shot handler runs once and is gone", n == 1 && !e.HasConnections("Hit"));
        // A one-shot is removed before it runs, so it may emit again without re-entering.
        int m = 0;
        e.Connect("Hit", Callable.From((int d) => { m++; e.EmitSignal("Hit", 0); }), (uint)GodotObject.ConnectFlags.OneShot);
        e.EmitSignal("Hit", 1);
        T.Check("and does not re-enter itself", m == 1);
    }

    private static void Deferred()
    {
        var e = new Emitter();
        var seen = new List<string>();
        e.Connect("Hit", Callable.From((int d) => seen.Add("deferred " + d)), (uint)GodotObject.ConnectFlags.Deferred);
        e.Connect("Hit", Callable.From((int d) => seen.Add("direct " + d)));
        e.EmitSignal("Hit", 4);
        T.Check("the direct handler runs now, the deferred one not yet", seen.SequenceEqual(new[] { "direct 4" }));
        Aver.Framework.GameThread.Flush();
        T.Check("the deferred handler runs at the flush", seen.SequenceEqual(new[] { "direct 4", "deferred 4" }));
        e.Connect("Died", Callable.From(() => seen.Add("late")), (uint)GodotObject.ConnectFlags.Deferred);
        e.EmitSignal("Died");
        e.Free();
        Aver.Framework.GameThread.Flush();
        T.Check("a handler whose signal owner is freed still runs (the owner is not the target)", seen.Contains("late"));
        var target = new Emitter();
        var src = new Emitter();
        src.Connect("Hit", new Callable(target, "on_hit"), (uint)GodotObject.ConnectFlags.Deferred);
        src.EmitSignal("Hit", 8);
        target.Free();
        Aver.Framework.GameThread.Flush();
        T.Check("a deferred call to a freed target is dropped", T.ErrorCount() == 0);
    }

    private static void RefCounted()
    {
        var e = new Emitter();
        var target = new Emitter();
        var c = new Callable(target, "on_hit");
        T.Check("the first connect is Ok", e.Connect("Hit", c) == Error.Ok);
        T.Check("a second connect without the flag is refused", e.Connect("Hit", c) == Error.InvalidParameter);
        uint rc = (uint)GodotObject.ConnectFlags.ReferenceCounted;
        var e2 = new Emitter();
        e2.Connect("Hit", c, rc);
        e2.Connect("Hit", c, rc);
        e2.EmitSignal("Hit", 1);
        T.Check("a reference-counted double connect still calls once", target.Got.Count(g => g == "hit 1") == 1);
        e2.Disconnect("Hit", c);
        T.Check("one disconnect leaves it connected", e2.IsConnected("Hit", c));
        e2.Disconnect("Hit", c);
        T.Check("the second removes it", !e2.IsConnected("Hit", c));
        int errorsBefore = T.ErrorCount();
        e2.Disconnect("Hit", c);
        T.Check("disconnecting what is not connected logs an error", T.ErrorCount() == errorsBefore + 1);
    }

    private static void Persist()
    {
        var e = new Emitter();
        var target = new Emitter();
        T.Check("Persist connects normally", e.Connect("Hit", new Callable(target, "on_hit"), (uint)GodotObject.ConnectFlags.Persist) == Error.Ok);
        e.EmitSignal("Hit", 3);
        T.Check("and delivers", target.Last == 3);
        uint all = (uint)(GodotObject.ConnectFlags.Persist | GodotObject.ConnectFlags.OneShot | GodotObject.ConnectFlags.Deferred);
        var e2 = new Emitter();
        int n = 0;
        e2.Connect("Hit", Callable.From((int d) => n++), all);
        e2.EmitSignal("Hit", 1); e2.EmitSignal("Hit", 1);
        Aver.Framework.GameThread.Flush();
        T.Check("flags combine: deferred one-shot runs once at the flush", n == 1);
    }

    private static void FreedTarget()
    {
        var e = new Emitter();
        var target = new Emitter();
        e.Connect("Hit", new Callable(target, "on_hit"));
        target.Free();
        T.Check("emit with a freed target does not throw", e.EmitSignal("Hit", 1) == Error.Ok);
        T.Check("and drops the connection", !e.HasConnections("Hit"));
        T.Check("without logging an error", T.ErrorCount() == 0);
    }

    private static void Throwing()
    {
        var e = new Emitter();
        var target = new Emitter();
        e.Connect("Hit", new Callable(target, "throws"));
        e.Connect("Hit", new Callable(target, "second"));
        e.EmitSignal("Hit", 5);
        T.Check("the next handler still runs", target.Got.Contains("second 5"));
        T.Check("the failure is logged with the exception", T.ErrorCount("handler failed") == 1);
    }

    private static void Misc()
    {
        var e = new Emitter();
        int n = 0;
        e.Connect("Hit", Callable.From((int d) => n++));
        e.SetBlockSignals(true);
        e.EmitSignal("Hit", 1);
        T.Check("blocked signals do not emit", n == 0 && e.IsBlockingSignals());
        e.SetBlockSignals(false);
        e.EmitSignal("Hit", 1);
        T.Check("unblocked they do", n == 1);
        e.AddUserSignal("custom");
        T.Check("user signals exist once added", e.HasSignal("custom") && e.Connect("custom", Callable.From(() => n += 10)) == Error.Ok);
        e.EmitSignal("custom");
        T.Check("and emit", n == 11);
        T.Check("the connection list", e.GetSignalConnections("Hit").Length == 1);
        var sig = new Signal(e, "Hit");
        T.Check("Signal struct connects and emits", sig.Connect(Callable.From((int d) => n += 100)) == Error.Ok && sig.Emit(1) == Error.Ok && n == 112);
        T.Check("Signal struct reports", !sig.IsNull && sig.Name == "Hit" && sig.Owner == e);
        e.Free();
        T.Check("a freed object cannot emit", e.EmitSignal("Hit", 1) == Error.Unavailable);
    }

    private static void DelegateHandlers()
    {
        var e = new Emitter();
        var seen = new List<int>();
        Emitter.HitEventHandler h = d => seen.Add(d);
        e.Connect("Hit", Callable.From(h));
        e.EmitSignal("Hit", 3);
        T.Check("a [Signal] delegate type works as a handler", seen.SequenceEqual(new[] { 3 }));
        T.Check("and is found again to disconnect", e.IsConnected("Hit", Callable.From(h)));
        e.Disconnect("Hit", Callable.From(h));
        e.EmitSignal("Hit", 4);
        T.Check("disconnected by delegate equality", seen.Count == 1);
    }

    private static async Task<int> WaitForHit(Emitter e)
    {
        Variant[] args = await e.ToSignal(e, "Hit");
        return args[0].AsInt32();
    }

    private static void Awaiting()
    {
        var e = new Emitter();
        Task<int> t = WaitForHit(e);
        T.Check("the await is pending before the emit", !t.IsCompleted);
        e.EmitSignal("Hit", 77);
        T.Check("it resumes inside the emit with the arguments", t.IsCompleted && t.Result == 77);
        e.EmitSignal("Hit", 1);
        T.Check("and only once", t.Result == 77 && !e.HasConnections("Hit"));
    }

    private static void BuiltIn()
    {
        var timer = new Aver.Fabric.Timer { WaitTime = 1.0, OneShot = true };
        int fired = 0;
        timer.Connect(Aver.Fabric.Timer.SignalName.Timeout, Callable.From(() => fired++));
        T.Check("Timer has its own signal and Node's", timer.HasSignal("timeout") && timer.HasSignal("tree_exiting"));
        var parent = new Rec("P");
        SceneTree.Instance.Root.AddChild(parent);
        parent.AddChild(timer);
        timer.Start();
        timer.OnTick(0.6f);
        T.Check("no timeout yet", fired == 0 && !timer.IsStopped());
        timer.OnTick(0.6f);
        T.Check("timeout after the wait time", fired == 1 && timer.IsStopped());
        timer.OnTick(5f);
        T.Check("a one-shot does not fire again", fired == 1);
        timer.OneShot = false; timer.Start(0.5);
        timer.OnTick(0.5f); timer.OnTick(0.5f);
        T.Check("a repeating timer fires every wait time", fired == 3 && !timer.IsStopped());
        timer.Paused = true;
        timer.OnTick(10f);
        T.Check("a paused timer does not count", fired == 3);
        timer.Paused = false; timer.Stop();
        timer.OnTick(10f);
        T.Check("a stopped one neither", fired == 3 && timer.TimeLeft == 0);
        var auto = new Aver.Fabric.Timer { Autostart = true, WaitTime = 2 };
        parent.AddChild(auto);
        T.Check("autostart starts when it is ready", !auto.IsStopped() && auto.TimeLeft == 2);
        SceneTree.Instance.Paused = true;
        int before = fired;
        timer.Start(0.1); timer.OnTick(1f);
        T.Check("the tree's pause stops a pausable timer", fired == before);
        timer.ProcessMode = Node.ProcessModeEnum.Always;
        timer.OnTick(1f);
        T.Check("unless its process mode is Always", fired == before + 1);
        SceneTree.Instance.Paused = false;
    }
}
