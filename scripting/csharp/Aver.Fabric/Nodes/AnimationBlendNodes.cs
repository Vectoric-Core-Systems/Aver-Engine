// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The blend tree's nodes (Godot's AnimationNodeBlend2 / Blend3 / Add2 / Add3 / Sub2 / OneShot / TimeScale / TimeSeek / Transition). Each reads
// its parameters from the tree (parameters/<path>/<name>), asks its inputs to play with a weight, and reports the time of the input that
// decides when the whole is over. docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>The base of the nodes with a <c>sync</c> switch (Godot's AnimationNodeSync): with it off, an input whose weight is 0 stands still.</summary>
public class AnimationNodeSync : AnimationNode
{
    public bool Sync { get; set; }

    public override bool HasFilter() => true;
}

/// <summary>Mixes two inputs by <c>parameters/.../blend_amount</c> (0 = the first, 1 = the second); a filter keeps the mix to some tracks.</summary>
public class AnimationNodeBlend2 : AnimationNodeSync
{
    public override string GetCaption() => "Blend2";

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "blend_amount", 0.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double amount = f.Tree.Param(f.Base + "blend_amount", 0.0).AsDouble();
        NodeTime a = BlendInput(f, 0, time, seek, external, 1.0 - amount, FilterAction.Blend, Sync, testOnly);
        NodeTime b = BlendInput(f, 1, time, seek, external, amount, FilterAction.Pass, Sync, testOnly);
        return Dominant(a, 1.0 - amount, b, amount);
    }
}

/// <summary>Mixes three inputs by <c>blend_amount</c> in -1..1 (-1 = the first, 0 = the middle one, 1 = the last).</summary>
public class AnimationNodeBlend3 : AnimationNodeSync
{
    public override string GetCaption() => "Blend3";

    public override bool HasFilter() => false;

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "blend_amount", 0.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double amount = f.Tree.Param(f.Base + "blend_amount", 0.0).AsDouble();
        double wLow = Math.Max(0.0, -amount), wMid = 1.0 - Math.Abs(amount), wHigh = Math.Max(0.0, amount);
        NodeTime lo = BlendInput(f, 0, time, seek, external, wLow, FilterAction.Ignore, Sync, testOnly);
        NodeTime mid = BlendInput(f, 1, time, seek, external, wMid, FilterAction.Ignore, Sync, testOnly);
        NodeTime hi = BlendInput(f, 2, time, seek, external, wHigh, FilterAction.Ignore, Sync, testOnly);
        NodeTime best = Dominant(mid, wMid, lo, wLow);
        return Dominant(best, Math.Max(wMid, wLow), hi, wHigh);
    }
}

/// <summary>Adds the second input on top of the first by <c>add_amount</c> (additive: the second's difference from its initial pose).</summary>
public class AnimationNodeAdd2 : AnimationNodeSync
{
    public override string GetCaption() => "Add2";

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "add_amount", 0.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double amount = f.Tree.Param(f.Base + "add_amount", 0.0).AsDouble();
        NodeTime a = BlendInput(f, 0, time, seek, external, 1.0, FilterAction.Ignore, Sync, testOnly);
        BlendInput(f, 1, time, seek, external, amount, FilterAction.Pass, Sync, testOnly);
        return a;
    }
}

/// <summary>Adds the first or the last input on top of the middle one by <c>add_amount</c> in -1..1.</summary>
public class AnimationNodeAdd3 : AnimationNodeSync
{
    public override string GetCaption() => "Add3";

    public override bool HasFilter() => false;

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "add_amount", 0.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double amount = f.Tree.Param(f.Base + "add_amount", 0.0).AsDouble();
        BlendInput(f, 0, time, seek, external, Math.Max(0.0, -amount), FilterAction.Pass, Sync, testOnly);
        NodeTime mid = BlendInput(f, 1, time, seek, external, 1.0, FilterAction.Ignore, Sync, testOnly);
        BlendInput(f, 2, time, seek, external, Math.Max(0.0, amount), FilterAction.Pass, Sync, testOnly);
        return mid;
    }
}

/// <summary>Takes the second input away from the first by <c>sub_amount</c> (the opposite of Add2).</summary>
public class AnimationNodeSub2 : AnimationNodeSync
{
    public override string GetCaption() => "Sub2";

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "sub_amount", 0.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double amount = f.Tree.Param(f.Base + "sub_amount", 0.0).AsDouble();
        NodeTime a = BlendInput(f, 0, time, seek, external, 1.0, FilterAction.Ignore, Sync, testOnly);
        BlendInput(f, 1, time, seek, external, -amount, FilterAction.Pass, Sync, testOnly);
        return a;
    }
}

/// <summary>Plays the second input over the first once, on request (Godot's AnimationNodeOneShot): <c>request</c> 1 fires it, 2 aborts it, 3 fades it out.</summary>
public class AnimationNodeOneShot : AnimationNodeSync
{
    public enum OneShotRequest { None = 0, Fire = 1, Abort = 2, FadeOut = 3 }
    public enum MixModeEnum { Blend = 0, Add = 1 }

    private sealed class Shot
    {
        public bool Active;
        public double Time;
        public double FadeOut = -1;       // seconds of an explicit fade-out left, -1 when none runs
        public double TimeToRestart = -1;
    }

    public MixModeEnum MixMode { get; set; } = MixModeEnum.Blend;
    public double FadeinTime { get; set; }
    public Curve? FadeinCurve { get; set; }
    public double FadeoutTime { get; set; }
    public Curve? FadeoutCurve { get; set; }
    public bool BreakLoopAtEnd { get; set; }
    public bool Autorestart { get; set; }
    public double AutorestartDelay { get; set; } = 1.0;
    public double AutorestartRandomDelay { get; set; }
    public bool AbortOnReset { get; set; }

    public override string GetCaption() => "OneShot";

    internal override void CollectParameters(AnimationTree tree, string @base)
    {
        tree.DeclareParam(@base + "request", (int)OneShotRequest.None);
        tree.DeclareParam(@base + "active", false);
        tree.DeclareParam(@base + "internal_active", false);
        tree.DeclareState<Shot>(@base + "state");
    }

    internal override void ResetState(AnimationTree tree, string @base)
    {
        if (AbortOnReset)
        {
            Shot st = tree.StateOf<Shot>(@base + "state");
            st.Active = false;
            st.FadeOut = -1;
            tree.SetParam(@base + "active", false);
            tree.SetParam(@base + "internal_active", false);
        }
    }

    private static double Shape(Curve? c, double x)
    {
        x = Math.Clamp(x, 0.0, 1.0);
        return c == null ? x : c.SampleBaked((float)x);
    }

    private void Fire(AnimationTree tree, string @base, Shot st, Frame f)
    {
        st.Active = true;
        st.Time = 0;
        st.FadeOut = -1;
        st.TimeToRestart = -1;
        if (f.Inputs?.Invoke(1) is { Node: { } shot } sh) shot.ResetState(tree, sh.Base);
    }

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        AnimationTree tree = f.Tree;
        Shot st = tree.StateOf<Shot>(f.Base + "state");
        if (!testOnly)
        {
            int request = tree.Param(f.Base + "request", 0).AsInt32();
            if (request != 0)
            {
                tree.SetParam(f.Base + "request", (int)OneShotRequest.None);
                switch ((OneShotRequest)request)
                {
                    case OneShotRequest.Fire: Fire(tree, f.Base, st, f); break;
                    case OneShotRequest.Abort: st.Active = false; st.FadeOut = -1; st.TimeToRestart = -1; break;
                    case OneShotRequest.FadeOut:
                        if (st.Active && st.FadeOut < 0) { if (FadeoutTime > 0) st.FadeOut = FadeoutTime; else st.Active = false; }
                        break;
                }
            }
        }

        if (!st.Active)
        {
            NodeTime idle = BlendInput(f, 0, time, seek, external, 1.0, FilterAction.Ignore, true, testOnly);
            if (!testOnly)
            {
                if (st.TimeToRestart >= 0)
                {
                    st.TimeToRestart -= time;
                    if (st.TimeToRestart <= 0) Fire(tree, f.Base, st, f);
                }
                tree.SetParam(f.Base + "active", st.Active);
                tree.SetParam(f.Base + "internal_active", st.Active);
            }
            if (!st.Active) return idle;
        }

        // Where the shot will be after this step decides its fade-out.
        NodeTime probe = BlendInput(f, 1, time, seek, external, 0.0, FilterAction.Ignore, true, true);
        bool breakable = !probe.Loop || BreakLoopAtEnd;
        double blend = FadeinTime > 0 ? Shape(FadeinCurve, st.Time / FadeinTime) : 1.0;
        bool finished = false;
        if (st.FadeOut >= 0)
        {
            double left = st.FadeOut - (testOnly ? 0 : time);
            if (!testOnly) st.FadeOut = left;
            blend *= FadeoutTime > 0 ? Shape(FadeoutCurve, Math.Max(left, 0) / FadeoutTime) : 0.0;
            finished = left <= 0;
        }
        else if (breakable)
        {
            if (FadeoutTime > 0 && probe.Remain < FadeoutTime) blend *= Shape(FadeoutCurve, probe.Remain / FadeoutTime);
            finished = probe.Remain <= 1e-9;
        }

        NodeTime baseTime;
        if (MixMode == MixModeEnum.Add)
        {
            baseTime = BlendInput(f, 0, time, seek, external, 1.0, FilterAction.Ignore, Sync, testOnly);
            BlendInput(f, 1, time, seek, external, blend, FilterAction.Pass, true, testOnly);
        }
        else
        {
            baseTime = BlendInput(f, 0, time, seek, external, 1.0 - blend, FilterAction.Blend, Sync, testOnly);
            BlendInput(f, 1, time, seek, external, blend, FilterAction.Pass, true, testOnly);
        }

        if (!testOnly)
        {
            st.Time += time;
            if (finished)
            {
                st.Active = false;
                st.FadeOut = -1;
                if (Autorestart) st.TimeToRestart = AutorestartDelay + GD.Randf() * AutorestartRandomDelay;
            }
            tree.SetParam(f.Base + "active", st.Active);
            tree.SetParam(f.Base + "internal_active", st.Active);
        }
        return baseTime;
    }
}

/// <summary>Plays its input at <c>scale</c> times the speed (Godot's AnimationNodeTimeScale).</summary>
public class AnimationNodeTimeScale : AnimationNode
{
    public override string GetCaption() => "TimeScale";

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "scale", 1.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double scale = f.Tree.Param(f.Base + "scale", 1.0).AsDouble();
        if (!seek) time *= scale;
        return BlendInput(f, 0, time, seek, external, 1.0, FilterAction.Ignore, true, testOnly);
    }
}

/// <summary>Jumps its input to a time when <c>seek_request</c> is 0 or more (Godot's AnimationNodeTimeSeek); the request clears itself.</summary>
public class AnimationNodeTimeSeek : AnimationNode
{
    public override string GetCaption() => "Seek";

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareParam(@base + "seek_request", -1.0);

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        double request = f.Tree.Param(f.Base + "seek_request", -1.0).AsDouble();
        if (request >= 0 && !testOnly)
        {
            f.Tree.SetParam(f.Base + "seek_request", -1.0);
            return BlendInput(f, 0, request, true, true, 1.0, FilterAction.Ignore, true, false);
        }
        return BlendInput(f, 0, time, seek, external, 1.0, FilterAction.Ignore, true, testOnly);
    }
}

/// <summary>Plays one of several inputs, cross-fading when the <c>transition_request</c> names another (Godot's AnimationNodeTransition).</summary>
public class AnimationNodeTransition : AnimationNodeSync
{
    private sealed class Sw
    {
        public int Current = -1;
        public int Previous = -1;
        public double FadeLeft;
        public bool Started;
    }

    private readonly List<(string Name, bool AutoAdvance, bool Reset)> _inputs = new();

    public double XfadeTime { get; set; }
    public Curve? XfadeCurve { get; set; }
    public bool AllowTransitionToSelf { get; set; }

    public int InputCount
    {
        get => _inputs.Count;
        set
        {
            value = Math.Max(value, 0);
            while (_inputs.Count > value) _inputs.RemoveAt(_inputs.Count - 1);
            while (_inputs.Count < value) _inputs.Add(("state_" + _inputs.Count, false, true));
        }
    }

    public override string GetCaption() => "Transition";

    public override bool HasFilter() => false;

    public void SetInputName(int input, string name) { if (input >= 0 && input < _inputs.Count) _inputs[input] = (name, _inputs[input].AutoAdvance, _inputs[input].Reset); }
    public string GetInputName(int input) => input >= 0 && input < _inputs.Count ? _inputs[input].Name : "";
    public int FindInput(string name) => _inputs.FindIndex(i => i.Name == name);
    public void SetInputAsAutoAdvance(int input, bool enable) { if (input >= 0 && input < _inputs.Count) _inputs[input] = (_inputs[input].Name, enable, _inputs[input].Reset); }
    public bool IsInputSetAsAutoAdvance(int input) => input >= 0 && input < _inputs.Count && _inputs[input].AutoAdvance;
    public void SetInputResetEnabled(int input, bool enable) { if (input >= 0 && input < _inputs.Count) _inputs[input] = (_inputs[input].Name, _inputs[input].AutoAdvance, enable); }
    public bool IsInputResetEnabled(int input) => input >= 0 && input < _inputs.Count && _inputs[input].Reset;

    // "input_2/name", "input_2/auto_advance", "input_2/reset", the way a scene stores the inputs.
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("input_", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 2 || !int.TryParse(p[0].AsSpan(6), out int i) || i < 0 || i >= _inputs.Count) return false;
        switch (p[1])
        {
            case "name": SetInputName(i, value.AsString()); return true;
            case "auto_advance": SetInputAsAutoAdvance(i, value.AsBool()); return true;
            case "reset": SetInputResetEnabled(i, value.AsBool()); return true;
        }
        return false;
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        if (!name.StartsWith("input_", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 2 || !int.TryParse(p[0].AsSpan(6), out int i) || i < 0 || i >= _inputs.Count) return false;
        switch (p[1])
        {
            case "name": value = _inputs[i].Name; return true;
            case "auto_advance": value = _inputs[i].AutoAdvance; return true;
            case "reset": value = _inputs[i].Reset; return true;
        }
        return false;
    }

    internal override void CollectParameters(AnimationTree tree, string @base)
    {
        tree.DeclareParam(@base + "transition_request", "");
        tree.DeclareParam(@base + "current_state", _inputs.Count > 0 ? _inputs[0].Name : "");
        tree.DeclareParam(@base + "current_index", _inputs.Count > 0 ? 0 : -1);
        tree.DeclareState<Sw>(@base + "state");
    }

    internal override void ResetState(AnimationTree tree, string @base)
    {
        Sw st = tree.StateOf<Sw>(@base + "state");
        st.Current = _inputs.Count > 0 ? 0 : -1;
        st.Previous = -1;
        st.Started = true;
    }

    private void SwitchTo(Sw st, int index, Frame f)
    {
        if (index < 0 || index >= _inputs.Count) return;
        if (_inputs[index].Reset && f.Inputs?.Invoke(index) is { Node: { } node } input) node.ResetState(f.Tree, input.Base);
        st.Previous = st.Current;
        st.Current = index;
        st.FadeLeft = XfadeTime;
        if (XfadeTime <= 0) st.Previous = -1;
    }

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        AnimationTree tree = f.Tree;
        Sw st = tree.StateOf<Sw>(f.Base + "state");
        if (!st.Started && _inputs.Count > 0) { st.Started = true; if (st.Current < 0) st.Current = 0; }
        if (!testOnly)
        {
            string request = tree.Param(f.Base + "transition_request", "").AsString();
            if (request.Length > 0)
            {
                tree.SetParam(f.Base + "transition_request", "");
                int idx = FindInput(request);
                if (idx < 0) tree.WarnOnce("tr", request, $"the transition input '{request}' does not exist");
                else if (idx != st.Current || AllowTransitionToSelf) SwitchTo(st, idx, f);
            }
            if (st.Previous >= 0)
            {
                st.FadeLeft -= Math.Abs(time);
                if (st.FadeLeft <= 0) st.Previous = -1;
            }
        }
        if (st.Current < 0) return default;

        double wCur = 1.0;
        if (st.Previous >= 0 && XfadeTime > 0)
        {
            double x = Math.Clamp(1.0 - st.FadeLeft / XfadeTime, 0.0, 1.0);
            wCur = XfadeCurve == null ? x : XfadeCurve.SampleBaked((float)x);
        }
        NodeTime cur = default;
        for (int i = 0; i < _inputs.Count; i++)
        {
            double w = i == st.Current ? wCur : i == st.Previous ? 1.0 - wCur : 0.0;
            NodeTime nt = BlendInput(f, i, time, seek, external, w, FilterAction.Ignore, Sync, testOnly);
            if (i == st.Current) cur = nt;
        }
        if (!testOnly)
        {
            tree.SetParam(f.Base + "current_state", _inputs[st.Current].Name);
            tree.SetParam(f.Base + "current_index", st.Current);
            if (_inputs[st.Current].AutoAdvance && cur.Length > 0 && cur.Remain <= 1e-9 && !cur.Loop && _inputs.Count > 1)
                SwitchTo(st, (st.Current + 1) % _inputs.Count, f);
        }
        return cur;
    }
}
