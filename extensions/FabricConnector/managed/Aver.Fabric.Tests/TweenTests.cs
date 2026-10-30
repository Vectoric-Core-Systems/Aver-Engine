// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tween: the easing curves against reference values (worked out from Penner's published equations with an independent script), and the
// tween's sequencing, parallel steps, loops, delays, pause modes, process modes and signals, driven by the managed frame loop.

using Aver.Fabric;

static class TweenTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("Tween curves: every transition and ease starts at 0 and ends at 1", CurveEndpoints);
        T.Run("Tween curves: reference values of In, Out, InOut and OutIn", CurveValues);
        T.Run("Tween: tween_property moves a property along the curve", PropertyBasics);
        T.Run("Tween: sub-properties, relative, from, delay", PropertyOptions);
        T.Run("Tween: steps run in order; set_parallel, parallel and chain", Sequencing);
        T.Run("Tween: callbacks, intervals and tween_method", CallbacksAndMethods);
        T.Run("Tween: loops and the finished, step_finished and loop_finished signals", LoopsAndSignals);
        T.Run("Tween: pause, play, stop, kill", Control);
        T.Run("Tween: pause modes follow the tree and the bound node", PauseModes);
        T.Run("Tween: physics process mode, speed scale, bound to a freed node", ProcessModes);
        T.Run("Tween: interpolate_value for vectors, colours and ints", Values);
    });

    private static readonly Tween.TransitionType[] Trans = Enum.GetValues<Tween.TransitionType>();
    private static readonly Tween.EaseType[] Eases = Enum.GetValues<Tween.EaseType>();

    private static double At(Tween.TransitionType tr, Tween.EaseType ea, double t) =>
        Tween.InterpolateValue(0.0, 1.0, t, 1.0, tr, ea).AsDouble();

    private static void CurveEndpoints()
    {
        bool ok = true;
        string bad = "";
        foreach (var tr in Trans)
            foreach (var ea in Eases)
            {
                double a = At(tr, ea, 0), b = At(tr, ea, 1);
                if (Math.Abs(a) > 1e-9 || Math.Abs(b - 1) > 1e-9) { ok = false; bad += $" {tr}/{ea}({a},{b})"; }
            }
        T.Check("48 curves: f(0) = 0 and f(1) = 1", ok, bad);
        T.Check("OutIn passes through the middle", Trans.All(tr => Math.Abs(At(tr, Tween.EaseType.OutIn, 0.5) - 0.5) < 1e-9));
        T.Check("a duration of 0 is the end value", Tween.InterpolateValue(2.0, 3.0, 0, 0, Tween.TransitionType.Quad, Tween.EaseType.In).AsDouble() == 5.0);
        T.Check("it scales and offsets: from 10 by 20 over 4 s, 1 s in, linear", Math.Abs(Tween.InterpolateValue(10.0, 20.0, 1, 4, Tween.TransitionType.Linear, Tween.EaseType.In).AsDouble() - 15.0) < 1e-9);
    }

    // name -> In at .25/.5/.75, Out at .25/.5/.75, InOut at .25/.75, OutIn at .25/.75
    private static readonly (Tween.TransitionType Tr, double[] In, double[] Out, double[] InOut, double[] OutIn)[] Ref =
    {
        (Tween.TransitionType.Linear,  new[] { 0.25, 0.5, 0.75 }, new[] { 0.25, 0.5, 0.75 }, new[] { 0.25, 0.75 }, new[] { 0.25, 0.75 }),
        (Tween.TransitionType.Sine,    new[] { 0.07612, 0.292893, 0.617317 }, new[] { 0.382683, 0.707107, 0.92388 }, new[] { 0.146447, 0.853553 }, new[] { 0.353553, 0.646447 }),
        (Tween.TransitionType.Quint,   new[] { 0.000977, 0.03125, 0.237305 }, new[] { 0.762695, 0.96875, 0.999023 }, new[] { 0.015625, 0.984375 }, new[] { 0.484375, 0.515625 }),
        (Tween.TransitionType.Quart,   new[] { 0.003906, 0.0625, 0.316406 }, new[] { 0.683594, 0.9375, 0.996094 }, new[] { 0.03125, 0.96875 }, new[] { 0.46875, 0.53125 }),
        (Tween.TransitionType.Quad,    new[] { 0.0625, 0.25, 0.5625 }, new[] { 0.4375, 0.75, 0.9375 }, new[] { 0.125, 0.875 }, new[] { 0.375, 0.625 }),
        (Tween.TransitionType.Expo,    new[] { 0.005524, 0.03125, 0.176777 }, new[] { 0.823223, 0.96875, 0.994476 }, new[] { 0.015625, 0.984375 }, new[] { 0.484375, 0.515625 }),
        (Tween.TransitionType.Elastic, new[] { -0.005524, -0.015625, 0.088388 }, new[] { 0.911612, 1.015625, 1.005524 }, new[] { 0.011969, 0.988031 }, new[] { 0.507812, 0.492188 }),
        (Tween.TransitionType.Cubic,   new[] { 0.015625, 0.125, 0.421875 }, new[] { 0.578125, 0.875, 0.984375 }, new[] { 0.0625, 0.9375 }, new[] { 0.4375, 0.5625 }),
        (Tween.TransitionType.Circ,    new[] { 0.031754, 0.133975, 0.338562 }, new[] { 0.661438, 0.866025, 0.968246 }, new[] { 0.066987, 0.933013 }, new[] { 0.433013, 0.566987 }),
        (Tween.TransitionType.Bounce,  new[] { 0.027344, 0.234375, 0.527344 }, new[] { 0.472656, 0.765625, 0.972656 }, new[] { 0.117188, 0.882812 }, new[] { 0.382812, 0.617188 }),
        (Tween.TransitionType.Back,    new[] { -0.064137, -0.087698, 0.18259 }, new[] { 0.81741, 1.087697, 1.064137 }, new[] { -0.099682, 1.099682 }, new[] { 0.543849, 0.456151 }),
        (Tween.TransitionType.Spring,  new[] { 0.013655, -0.051016, 0.336663 }, new[] { 0.663337, 1.051016, 0.986345 }, new[] { -0.025508, 1.025508 }, new[] { 0.525508, 0.474492 }),
    };

    private static bool Near(double a, double b) => Math.Abs(a - b) < 2e-6;

    private static void CurveValues()
    {
        foreach (var r in Ref)
        {
            double[] ts = { 0.25, 0.5, 0.75 };
            bool inOk = true, outOk = true, ioOk = true, oiOk = true;
            string detail = "";
            for (int i = 0; i < 3; i++)
            {
                if (!Near(At(r.Tr, Tween.EaseType.In, ts[i]), r.In[i])) { inOk = false; detail += $" In@{ts[i]}={At(r.Tr, Tween.EaseType.In, ts[i])}"; }
                if (!Near(At(r.Tr, Tween.EaseType.Out, ts[i]), r.Out[i])) { outOk = false; detail += $" Out@{ts[i]}={At(r.Tr, Tween.EaseType.Out, ts[i])}"; }
            }
            double[] ts2 = { 0.25, 0.75 };
            for (int i = 0; i < 2; i++)
            {
                if (!Near(At(r.Tr, Tween.EaseType.InOut, ts2[i]), r.InOut[i])) { ioOk = false; detail += $" InOut@{ts2[i]}={At(r.Tr, Tween.EaseType.InOut, ts2[i])}"; }
                if (!Near(At(r.Tr, Tween.EaseType.OutIn, ts2[i]), r.OutIn[i])) { oiOk = false; detail += $" OutIn@{ts2[i]}={At(r.Tr, Tween.EaseType.OutIn, ts2[i])}"; }
            }
            T.Check($"{r.Tr}: In", inOk, detail);
            T.Check($"{r.Tr}: Out", outOk, detail);
            T.Check($"{r.Tr}: InOut", ioOk, detail);
            T.Check($"{r.Tr}: OutIn", oiOk, detail);
        }
        // Closed forms that need no table.
        T.Check("quad In at 0.5 is 0.25 and cubic In is 0.125", Near(At(Tween.TransitionType.Quad, Tween.EaseType.In, 0.5), 0.25) && Near(At(Tween.TransitionType.Cubic, Tween.EaseType.In, 0.5), 0.125));
        T.Check("sine In at 0.5 is 1 - cos(pi/4)", Near(At(Tween.TransitionType.Sine, Tween.EaseType.In, 0.5), 1 - Math.Cos(Math.PI / 4)));
        T.Check("bounce Out at 0.5 hits the 0.765625 rebound", Near(At(Tween.TransitionType.Bounce, Tween.EaseType.Out, 0.5), 0.765625));
        T.Check("back overshoots both ways", At(Tween.TransitionType.Back, Tween.EaseType.In, 0.3) < 0 && At(Tween.TransitionType.Back, Tween.EaseType.Out, 0.7) > 1);
        T.Check("elastic Out overshoots past 1 and settles", At(Tween.TransitionType.Elastic, Tween.EaseType.Out, 0.5) > 1 && Math.Abs(At(Tween.TransitionType.Elastic, Tween.EaseType.Out, 0.99) - 1) < 0.01);
    }

    private static Node3D Spawn(string name = "N")
    {
        var n = new Node3D { Name = name };
        SceneTree.Instance.Root.AddChild(n);
        return n;
    }

    private static void PropertyBasics()
    {
        Node3D n = Spawn();
        Tween tw = SceneTree.Instance.CreateTween();
        tw.TweenProperty(n, "position", new Vector3(10, 0, 0), 1.0);
        T.Check("a new tween runs and is valid", tw.IsRunning() && tw.IsValid());
        T.Frame(0.25);
        T.CheckVec("a quarter of the way after a quarter second (linear)", new Vector3(2.5f, 0, 0), n.Position);
        T.Frame(0.5);
        T.CheckVec("three quarters", new Vector3(7.5f, 0, 0), n.Position);
        T.Check("still running", tw.IsRunning() && tw.IsValid());
        T.Frame(0.25);
        T.CheckVec("the end value exactly at the duration", new Vector3(10, 0, 0), n.Position);
        T.Check("a finished tween is not valid or running", !tw.IsValid() && !tw.IsRunning());
        T.Check("and the tree dropped it", SceneTree.Instance.GetProcessedTweens().Count == 0);

        Node3D m = Spawn("M");
        Tween q = SceneTree.Instance.CreateTween().SetTrans(Tween.TransitionType.Quad).SetEase(Tween.EaseType.In);
        q.TweenProperty(m, "position", new Vector3(8, 0, 0), 2.0);
        T.Frame(1.0);
        T.CheckVec("the tween's default transition and ease apply (quad in: 1/4 of 8)", new Vector3(2, 0, 0), m.Position);

        Node3D k = Spawn("K");
        Tween per = SceneTree.Instance.CreateTween().SetTrans(Tween.TransitionType.Quad);
        per.TweenProperty(k, "position", new Vector3(8, 0, 0), 2.0).SetTrans(Tween.TransitionType.Cubic).SetEase(Tween.EaseType.Out);
        T.Frame(1.0);
        T.CheckVec("a tweener's own transition and ease win (cubic out at 0.5 = 0.875)", new Vector3(7, 0, 0), k.Position);

        Node3D big = Spawn("Big");
        Tween overshoot = SceneTree.Instance.CreateTween();
        overshoot.TweenProperty(big, "position", new Vector3(4, 0, 0), 1.0);
        T.Frame(5.0);
        T.CheckVec("a frame longer than the tween ends it at the end value", new Vector3(4, 0, 0), big.Position);
    }

    private static void PropertyOptions()
    {
        Node3D n = Spawn();
        n.Position = new Vector3(1, 2, 3);
        Tween tw = SceneTree.Instance.CreateTween();
        tw.TweenProperty(n, "position:x", 9.0f, 1.0);
        T.Frame(0.5);
        T.CheckVec("a sub-property changes one component", new Vector3(5, 2, 3), n.Position);
        T.Frame(0.5);

        Node3D r = Spawn("R");
        r.Position = new Vector3(10, 0, 0);
        Tween rel = SceneTree.Instance.CreateTween();
        rel.TweenProperty(r, "position", new Vector3(4, 0, 0), 1.0).AsRelative();
        T.Frame(1.0);
        T.CheckVec("as_relative adds to the start", new Vector3(14, 0, 0), r.Position);

        Node3D f = Spawn("F");
        f.Position = new Vector3(50, 0, 0);
        Tween from = SceneTree.Instance.CreateTween();
        from.TweenProperty(f, "position", new Vector3(10, 0, 0), 1.0).From(new Vector3(0, 0, 0));
        T.Frame(0.5);
        T.CheckVec("from starts at the given value, not the current one", new Vector3(5, 0, 0), f.Position);

        Node3D d = Spawn("D");
        Tween delay = SceneTree.Instance.CreateTween();
        delay.TweenProperty(d, "position", new Vector3(10, 0, 0), 1.0).SetDelay(0.5);
        T.Frame(0.25);
        T.CheckVec("nothing moves during the delay", Vector3.Zero, d.Position);
        T.Frame(0.25);
        T.CheckVec("and it starts at the end of it", Vector3.Zero, d.Position);
        T.Frame(0.5);
        T.CheckVec("then half way after half the duration", new Vector3(5, 0, 0), d.Position);

        Node3D c = Spawn("C");
        Tween fc = SceneTree.Instance.CreateTween();
        fc.TweenProperty(c, "position", new Vector3(10, 0, 0), 1.0).SetDelay(0.5);
        T.Frame(0.25);
        c.Position = new Vector3(100, 0, 0);
        T.Frame(0.75);
        T.CheckVec("a delayed tweener reads the start when its delay ends", new Vector3(55, 0, 0), c.Position);

        Node3D col = Spawn("Col");
        var mat = new Hero();   // a script with a Color export
        SceneTree.Instance.Root.AddChild(mat);
        Tween ct = SceneTree.Instance.CreateTween();
        ct.TweenProperty(mat, "Tint", new Color(0, 0, 0, 0), 1.0);
        T.Frame(0.5);
        T.Check("colours tween per channel", T.Near(mat.Tint.R, 0.5f) && T.Near(mat.Tint.A, 0.5f), mat.Tint.ToString());

        Tween ca = SceneTree.Instance.CreateTween();
        ca.TweenProperty(mat, "Tint:a", 1.0f, 1.0);
        T.Frame(1.0);
        T.Check("a colour component by name", T.Near(mat.Tint.A, 1f));
    }

    private static void Sequencing()
    {
        var log = new List<string>();
        Node3D a = Spawn("A"), b = Spawn("B");
        Tween tw = SceneTree.Instance.CreateTween();
        tw.TweenProperty(a, "position:x", 10.0f, 1.0);
        tw.TweenCallback(Callable.From(() => log.Add("after-a")));
        tw.TweenProperty(b, "position:x", 10.0f, 1.0);
        T.Frame(0.5);
        T.Check("the first step is running, the second is not", a.Position.X > 4.9f && b.Position.X == 0);
        T.Frame(0.5);
        T.Check("the first step ends exactly at its duration; the next step starts next frame", T.Near(a.Position.X, 10f) && log.Count == 0);
        T.Frame(0.5);
        T.CheckSeq("the callback runs at the start of the next step", new[] { "after-a" }, log);
        T.Check("and the frame's time goes into the step after it", T.Near(b.Position.X, 5f), b.Position.X.ToString());
        T.Frame(0.5);
        T.Check("it finishes", T.Near(b.Position.X, 10f) && !tw.IsValid());

        // Leftover time of a frame goes on into the next step.
        Node3D c = Spawn("C"), d = Spawn("D");
        Tween carry = SceneTree.Instance.CreateTween();
        carry.TweenProperty(c, "position:x", 10.0f, 1.0);
        carry.TweenProperty(d, "position:x", 10.0f, 1.0);
        T.Frame(1.5);
        T.Check("the time left over after step one moves step two in the same frame", T.Near(c.Position.X, 10f) && T.Near(d.Position.X, 5f), $"{c.Position.X} {d.Position.X}");

        // set_parallel: everything alongside the first.
        Node3D p = Spawn("P"), q = Spawn("Q");
        Tween par = SceneTree.Instance.CreateTween().SetParallel(true);
        par.TweenProperty(p, "position:x", 10.0f, 1.0);
        par.TweenProperty(q, "position:y", 20.0f, 2.0);
        T.Frame(0.5);
        T.Check("with set_parallel both run at once", T.Near(p.Position.X, 5f) && T.Near(q.Position.Y, 5f), $"{p.Position.X} {q.Position.Y}");
        T.Frame(0.5);
        T.Check("the step lasts as long as its longest tweener", T.Near(p.Position.X, 10f) && T.Near(q.Position.Y, 10f));
        T.Frame(1.0);
        T.Check("and ends with it", T.Near(q.Position.Y, 20f) && !par.IsValid());

        // parallel() for one tweener, chain() to go back to sequential.
        Node3D x = Spawn("X"), y = Spawn("Y"), z = Spawn("Z");
        Tween mixed = SceneTree.Instance.CreateTween();
        mixed.TweenProperty(x, "position:x", 10.0f, 1.0);
        mixed.Parallel().TweenProperty(y, "position:x", 10.0f, 1.0);
        mixed.TweenProperty(z, "position:x", 10.0f, 1.0);
        T.Frame(0.5);
        T.Check("parallel() puts the next tweener beside the previous", T.Near(x.Position.X, 5f) && T.Near(y.Position.X, 5f) && z.Position.X == 0);
        T.Frame(1.0);
        T.Check("and the one after it waits for both", T.Near(z.Position.X, 5f), z.Position.X.ToString());

        Node3D s1 = Spawn("S1"), s2 = Spawn("S2"), s3 = Spawn("S3");
        Tween chain = SceneTree.Instance.CreateTween().SetParallel(true);
        chain.TweenProperty(s1, "position:x", 10.0f, 1.0);
        chain.Chain().TweenProperty(s2, "position:x", 10.0f, 1.0);
        chain.TweenProperty(s3, "position:x", 10.0f, 1.0);
        T.Frame(0.5);
        T.Check("chain() starts a new step in a parallel tween, and parallel carries on after it", T.Near(s1.Position.X, 5f) && s2.Position.X == 0 && s3.Position.X == 0);
        T.Frame(1.0);
        T.Check("the chained pair then run together", T.Near(s2.Position.X, 5f) && T.Near(s3.Position.X, 5f), $"{s2.Position.X} {s3.Position.X}");
    }

    private static void CallbacksAndMethods()
    {
        var log = new List<string>();
        Tween tw = SceneTree.Instance.CreateTween();
        tw.TweenInterval(0.5);
        tw.TweenCallback(Callable.From(() => log.Add("cb"))).SetDelay(0.25);
        var values = new List<float>();
        tw.TweenMethod(Callable.From<float>(v => values.Add(v)), 0.0f, 10.0f, 1.0);
        T.Frame(0.5);
        T.Check("an interval waits", log.Count == 0 && values.Count == 0);
        T.Frame(0.25);
        T.CheckSeq("a callback with a delay waits for it", new[] { "cb" }, log);
        T.Frame(0.25);
        T.Check("tween_method calls the method with an interpolated value each frame", values.Count == 1 && T.Near(values[0], 2.5f), string.Join(",", values));
        T.Frame(0.75);
        T.Check("and the end value last", values.Count == 2 && T.Near(values[^1], 10f), string.Join(",", values));
        T.Check("the tween is done", !tw.IsValid());

        var eased = new List<float>();
        Tween e = SceneTree.Instance.CreateTween();
        e.TweenMethod(Callable.From<float>(v => eased.Add(v)), 0.0f, 8.0f, 2.0).SetTrans(Tween.TransitionType.Quad).SetEase(Tween.EaseType.In);
        T.Frame(1.0);
        T.Check("tween_method honours trans and ease", eased.Count == 1 && T.Near(eased[0], 2f), string.Join(",", eased));

        Tween bad = SceneTree.Instance.CreateTween();
        bad.TweenCallback(Callable.From(() => throw new InvalidOperationException("nope")));
        bad.TweenCallback(Callable.From(() => log.Add("after-throw")));
        T.Frames(2, 0.1);
        T.Check("a callback that throws is logged and the tween goes on", log.Contains("after-throw") && T.ErrorCount("tween callback") == 1);
    }

    private static void LoopsAndSignals()
    {
        Node3D n = Spawn();
        var events = new List<string>();
        Tween tw = SceneTree.Instance.CreateTween().SetLoops(3);
        tw.StepFinished += idx => events.Add("step " + idx);
        tw.LoopFinished += loop => events.Add("loop " + loop);
        tw.Finished += () => events.Add("finished");
        tw.TweenProperty(n, "position:x", 10.0f, 1.0).From(0.0f);
        tw.TweenInterval(0.5);
        T.Check("loops left before it starts", tw.GetLoopsLeft() == 3);
        T.Frame(1.0);
        T.Check("first step done", T.Near(n.Position.X, 10f));
        T.Frame(0.5);
        T.CheckSeq("a loop ends with step_finished for each step then loop_finished", new[] { "step 0", "step 1", "loop 1" }, events);
        T.Check("two loops left", tw.GetLoopsLeft() == 2);
        T.Frame(0.5);
        T.Check("from() restarts the property at the start of each loop (half a second in: 5)", T.Near(n.Position.X, 5f), n.Position.X.ToString());
        T.Frame(2.5);
        T.CheckSeq("and finished comes after the last loop's steps, not loop_finished", new[] { "step 0", "step 1", "loop 1", "step 0", "step 1", "loop 2", "step 0", "step 1", "finished" }, events);
        T.Check("the tween ends", !tw.IsValid());

        Node3D m = Spawn("M");
        Tween inf = SceneTree.Instance.CreateTween().SetLoops();
        int loops = 0;
        inf.LoopFinished += _ => loops++;
        inf.TweenProperty(m, "position:x", 1.0f, 1.0).From(0.0f);
        T.Frames(10, 0.5);
        T.Check("set_loops(0) loops forever (5 seconds, 5 loops)", loops == 4 || loops == 5, loops.ToString());
        T.Check("and has -1 loops left", inf.GetLoopsLeft() == -1 && inf.IsValid());

        Tween empty = SceneTree.Instance.CreateTween();
        T.Frame(0.1);
        T.Check("a tween with no tweeners is dropped", !empty.IsValid());
        Tween zero = SceneTree.Instance.CreateTween().SetLoops();
        zero.TweenCallback(Callable.From(() => { }));
        T.Frame(0.1);
        T.Check("an infinite tween that takes no time does not hang the frame", zero.IsValid());
    }

    private static void Control()
    {
        Node3D n = Spawn();
        Tween tw = SceneTree.Instance.CreateTween();
        tw.TweenProperty(n, "position:x", 10.0f, 1.0);
        T.Frame(0.25);
        tw.Pause();
        T.Check("pause stops it", !tw.IsRunning() && tw.IsValid());
        T.Frame(0.5);
        T.Check("and nothing moves", T.Near(n.Position.X, 2.5f));
        tw.Play();
        T.Frame(0.25);
        T.Check("play carries on", T.Near(n.Position.X, 5f));
        tw.Stop();
        T.Check("stop rewinds", !tw.IsRunning() && tw.GetTotalElapsedTime() == 0);
        T.Frame(0.5);
        T.Check("a stopped tween does not move", T.Near(n.Position.X, 5f));
        tw.Play();
        T.Frame(0.5);
        T.Check("play after stop starts again from the current value", T.Near(n.Position.X, 7.5f), n.Position.X.ToString());
        tw.Kill();
        T.Check("kill ends it for good", !tw.IsValid() && !tw.IsRunning());
        T.Frame(1.0);
        T.Check("nothing more happens", T.Near(n.Position.X, 7.5f));
        bool threw = false;
        try { tw.TweenProperty(n, "position:x", 1.0f, 1.0); } catch (InvalidOperationException) { threw = true; }
        T.Check("a dead tween takes no more tweeners", threw);

        Node3D m = Spawn("M");
        Tween manual = SceneTree.Instance.CreateTween();
        manual.TweenProperty(m, "position:x", 10.0f, 1.0);
        manual.Pause();
        manual.CustomStep(0.5);
        T.Check("custom_step moves a paused tween by hand", T.Near(m.Position.X, 5f) && !manual.IsRunning());
    }

    private static void PauseModes()
    {
        var tree = SceneTree.Instance;
        Node3D free = Spawn("Free");
        Node3D always = Spawn("Always");
        always.ProcessMode = Node.ProcessModeEnum.Always;
        Node3D pausable = Spawn("Pausable");

        Tween unbound = tree.CreateTween();
        unbound.TweenProperty(free, "position:x", 10.0f, 1.0);
        Tween process = tree.CreateTween().SetPauseMode(Tween.TweenPauseMode.Process);
        process.TweenProperty(free, "position:y", 10.0f, 1.0);
        Tween boundAlways = always.CreateTween();
        boundAlways.TweenProperty(always, "position:x", 10.0f, 1.0);
        Tween boundPausable = pausable.CreateTween();
        boundPausable.TweenProperty(pausable, "position:x", 10.0f, 1.0);

        tree.Paused = true;
        T.Frame(0.5);
        T.Check("a paused tree stops an unbound tween", free.Position.X == 0);
        T.Check("but not one in Process pause mode", T.Near(free.Position.Y, 5f));
        T.Check("a tween bound to an Always node keeps running", T.Near(always.Position.X, 5f));
        T.Check("one bound to a pausable node stops with it", pausable.Position.X == 0);
        tree.Paused = false;
        T.Frame(0.5);
        T.Check("they all go on when the tree resumes", T.Near(free.Position.X, 5f) && T.Near(pausable.Position.X, 5f));

        Node3D off = Spawn("Off");
        Tween b = off.CreateTween();
        b.TweenProperty(off, "position:x", 10.0f, 1.0);
        T.Frame(0.25);
        tree.Root.RemoveChild(off);
        T.Frame(0.5);
        T.Check("a tween bound to a node outside the tree waits", T.Near(off.Position.X, 2.5f) && b.IsValid());
        tree.Root.AddChild(off);
        T.Frame(0.25);
        T.Check("and resumes when the node is back", T.Near(off.Position.X, 5f));
    }

    private static void ProcessModes()
    {
        Node3D n = Spawn();
        Tween ph = SceneTree.Instance.CreateTween().SetProcessMode(Tween.TweenProcessMode.Physics);
        ph.TweenProperty(n, "position:x", 6.0f, 1.0);
        T.Frame(0.5);
        T.Check("a physics tween ignores process frames", n.Position.X == 0);
        T.Frame(1.0 / 60.0, physicsSteps: 30);
        T.Check("and advances with physics steps (30 steps of 1/60 s)", T.Near(n.Position.X, 3f, 1e-3f), n.Position.X.ToString());

        Node3D f = Spawn("F");
        Tween fast = SceneTree.Instance.CreateTween().SetSpeedScale(2f);
        fast.TweenProperty(f, "position:x", 10.0f, 1.0);
        T.Frame(0.25);
        T.Check("speed_scale 2 runs it twice as fast", T.Near(f.Position.X, 5f));

        Node3D gone = Spawn("Gone");
        Tween bound = gone.CreateTween();
        bound.TweenProperty(gone, "position:x", 10.0f, 1.0);
        T.Frame(0.25);
        gone.Free();
        T.Frame(0.25);
        T.Check("a tween bound to a freed node is dropped", SceneTree.Instance.GetProcessedTweens().All(t => t != bound));

        Node3D target = Spawn("Target");
        Tween free = SceneTree.Instance.CreateTween();
        free.TweenProperty(target, "position:x", 10.0f, 1.0);
        T.Frame(0.25);
        target.Free();
        T.Frame(0.25);
        T.Check("freeing the target ends an unbound tween without a crash", T.ErrorCount() == 0);
    }

    private static void Values()
    {
        Variant v = Tween.InterpolateValue(new Vector3(0, 0, 0), new Vector3(10, 20, 30), 0.5, 1.0, Tween.TransitionType.Linear, Tween.EaseType.In);
        T.CheckVec("a Vector3 per component", new Vector3(5, 10, 15), v.AsVector3());
        Variant c = Tween.InterpolateValue(new Color(1, 0, 0, 1), new Color(-1, 1, 0, -1), 0.5, 1.0, Tween.TransitionType.Linear, Tween.EaseType.In);
        T.Check("a Color per channel", T.Near(c.AsColor().R, 0.5f) && T.Near(c.AsColor().G, 0.5f) && T.Near(c.AsColor().A, 0.5f));
        Variant i = Tween.InterpolateValue(10, 10, 0.5, 1.0, Tween.TransitionType.Linear, Tween.EaseType.In);
        T.Check("an int stays an int", i.VariantType == Variant.Type.Int && i.AsInt32() == 15);
        Variant s = Tween.InterpolateValue("a", "b", 0.5, 1.0, Tween.TransitionType.Linear, Tween.EaseType.In);
        T.Check("a string keeps its start", s.AsString() == "a");
        Variant v2 = Tween.InterpolateValue(new Vector2(1, 1), new Vector2(2, 4), 1.0, 2.0, Tween.TransitionType.Quad, Tween.EaseType.In);
        T.Check("a Vector2 with a curve (quad in at half: a quarter)", T.Near(v2.AsVector2().X, 1.5f) && T.Near(v2.AsVector2().Y, 2f));
        Quaternion q0 = Quaternion.Identity, q1 = new Quaternion(new Vector3(0, 1, 0), Mathf.Pi / 2);
        Variant qv = Tween.InterpolateValue(q0, q0.Inverse() * q1, 0.5, 1.0, Tween.TransitionType.Linear, Tween.EaseType.In);
        T.Check("a Quaternion slerps (half of 90 degrees about Y)", T.Near(qv.AsQuaternion(), new Quaternion(new Vector3(0, 1, 0), Mathf.Pi / 4), 1e-4f));
    }
}
