// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// AnimationNodeStateMachine, its transitions and its playback (Godot's). States hold nodes; transitions carry a switch mode (immediate, sync,
// at end), an advance mode (disabled, enabled = travel only, auto), an advance_condition / advance_expression, an xfade time and curve, a
// reset flag and a priority. The playback answers travel() with the shortest path through the transitions, start(), next() and stop().
// docs/plans/fabric/G5_PARITY.md, A2 lists where this differs from Godot.

using Edge = Aver.Fabric.AnimationNodeStateMachine.Edge;

namespace Aver.Fabric;

/// <summary>The state a machine starts in: it plays nothing and its transitions leave at once (Godot's AnimationNodeStartState).</summary>
public class AnimationNodeStartState : AnimationRootNode
{
    public override string GetCaption() => "Start";
}

/// <summary>The state that ends a machine: reaching it stops the playback (Godot's AnimationNodeEndState).</summary>
public class AnimationNodeEndState : AnimationRootNode
{
    public override string GetCaption() => "End";
}

/// <summary>How a machine leaves one state for another (Godot's AnimationNodeStateMachineTransition).</summary>
public class AnimationNodeStateMachineTransition : Resource
{
    public enum SwitchModeEnum { Immediate = 0, Sync = 1, AtEnd = 2 }
    public enum AdvanceModeEnum { Disabled = 0, Enabled = 1, Auto = 2 }

    public new class SignalName : Resource.SignalName
    {
        public static readonly StringName AdvanceConditionChanged = "advance_condition_changed";
    }

    private StringName _condition = StringName.Empty;

    /// <summary>Immediate leaves at once; Sync leaves at once and starts the next state at the same fraction of its length; AtEnd waits for the current state to end.</summary>
    public SwitchModeEnum SwitchMode { get; set; } = SwitchModeEnum.Immediate;

    /// <summary>Disabled: never used. Enabled: used by travel() only. Auto: used by itself when its condition and expression hold.</summary>
    public AdvanceModeEnum AdvanceMode { get; set; } = AdvanceModeEnum.Auto;

    /// <summary>A <c>parameters/conditions/&lt;name&gt;</c> bool that must be true.</summary>
    public StringName AdvanceCondition
    {
        get => _condition;
        set { _condition = value; EmitSignal(SignalName.AdvanceConditionChanged); }
    }

    /// <summary>An expression over the tree's expression base node that must be true.</summary>
    public string AdvanceExpression { get; set; } = "";

    /// <summary>Seconds the two states are cross-faded for.</summary>
    public float XfadeTime { get; set; }

    public Curve? XfadeCurve { get; set; }

    /// <summary>When true the next state restarts from its beginning.</summary>
    public bool Reset { get; set; } = true;

    /// <summary>Of several usable transitions, the one with the lowest number is preferred (also when travelling).</summary>
    public int Priority { get; set; } = 1;

    /// <summary>Godot 4.0 name of <see cref="AdvanceMode"/>: true when the advance mode is Auto.</summary>
    public bool AutoAdvance { get => AdvanceMode == AdvanceModeEnum.Auto; set => AdvanceMode = value ? AdvanceModeEnum.Auto : AdvanceModeEnum.Enabled; }

    public bool Disabled { get => AdvanceMode == AdvanceModeEnum.Disabled; set => AdvanceMode = value ? AdvanceModeEnum.Disabled : AdvanceModeEnum.Enabled; }
}

/// <summary>A machine of states joined by transitions (Godot's AnimationNodeStateMachine).</summary>
public class AnimationNodeStateMachine : AnimationRootNode
{
    public enum StateMachineTypeEnum { Root = 0, Nested = 1, Grouped = 2 }

    public const string StartState = "Start";
    public const string EndState = "End";

    private sealed class Entry
    {
        public AnimationNode? Node;
        public Vector2 Position;
    }

    internal sealed class Edge
    {
        public string From = "", To = "";
        public AnimationNodeStateMachineTransition Transition = null!;
    }

    private readonly Dictionary<string, Entry> _states = new(StringComparer.Ordinal);
    private readonly List<string> _order = new();
    internal readonly List<Edge> Edges = new();

    public bool AllowTransitionToSelf { get; set; }
    public bool ResetEnds { get; set; }
    public StateMachineTypeEnum StateMachineType { get; set; } = StateMachineTypeEnum.Root;
    public Vector2 GraphOffset { get; set; }

    public AnimationNodeStateMachine()
    {
        _states[StartState] = new Entry { Node = new AnimationNodeStartState(), Position = new Vector2(200, 100) };
        _states[EndState] = new Entry { Node = new AnimationNodeEndState(), Position = new Vector2(900, 100) };
        _order.Add(StartState);
        _order.Add(EndState);
    }

    public override string GetCaption() => "StateMachine";

    // ---- states ----

    public void AddNode(StringName name, AnimationNode node, Vector2 position = default)
    {
        string n = name.ToString();
        if (n.Length == 0 || _states.ContainsKey(n) || node == null) return;
        _states[n] = new Entry { Node = node, Position = position };
        _order.Add(n);
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void ReplaceNode(StringName name, AnimationNode node)
    {
        if (_states.TryGetValue(name.ToString(), out Entry? e) && node != null) e.Node = node;
    }

    public AnimationNode? GetNode(StringName name) => _states.TryGetValue(name.ToString(), out Entry? e) ? e.Node : null;

    public bool HasNode(StringName name) => _states.ContainsKey(name.ToString());

    public StringName GetNodeName(AnimationNode node)
    {
        foreach (var kv in _states) if (ReferenceEquals(kv.Value.Node, node)) return kv.Key;
        return StringName.Empty;
    }

    public void RemoveNode(StringName name)
    {
        string n = name.ToString();
        if (n == StartState || n == EndState || !_states.Remove(n)) return;
        _order.Remove(n);
        Edges.RemoveAll(e => e.From == n || e.To == n);
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void RenameNode(StringName name, StringName newName)
    {
        string o = name.ToString(), n = newName.ToString();
        if (o == StartState || o == EndState || !_states.TryGetValue(o, out Entry? e) || n.Length == 0 || _states.ContainsKey(n)) return;
        _states.Remove(o);
        _states[n] = e;
        _order[_order.IndexOf(o)] = n;
        foreach (Edge ed in Edges)
        {
            if (ed.From == o) ed.From = n;
            if (ed.To == o) ed.To = n;
        }
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public StringName[] GetNodeList() => _order.Select(n => new StringName(n)).ToArray();

    public void SetNodePosition(StringName name, Vector2 position) { if (_states.TryGetValue(name.ToString(), out Entry? e)) e.Position = position; }

    public Vector2 GetNodePosition(StringName name) => _states.TryGetValue(name.ToString(), out Entry? e) ? e.Position : default;

    // ---- transitions ----

    public bool HasTransition(StringName from, StringName to) => Edges.Any(e => e.From == from.ToString() && e.To == to.ToString());

    public void AddTransition(StringName from, StringName to, AnimationNodeStateMachineTransition transition)
    {
        string f = from.ToString(), t = to.ToString();
        if (f == t || transition == null || !_states.ContainsKey(f) || !_states.ContainsKey(t) || f == EndState || t == StartState) return;
        if (HasTransition(from, to)) return;
        Edges.Add(new Edge { From = f, To = t, Transition = transition });
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public int GetTransitionCount() => Edges.Count;

    public AnimationNodeStateMachineTransition? GetTransition(int idx) => idx >= 0 && idx < Edges.Count ? Edges[idx].Transition : null;

    public StringName GetTransitionFrom(int idx) => idx >= 0 && idx < Edges.Count ? new StringName(Edges[idx].From) : StringName.Empty;

    public StringName GetTransitionTo(int idx) => idx >= 0 && idx < Edges.Count ? new StringName(Edges[idx].To) : StringName.Empty;

    public void RemoveTransitionByIndex(int idx)
    {
        if (idx < 0 || idx >= Edges.Count) return;
        Edges.RemoveAt(idx);
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void RemoveTransition(StringName from, StringName to)
    {
        if (Edges.RemoveAll(e => e.From == from.ToString() && e.To == to.ToString()) > 0) EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    /// <summary>The transitions as a scene stores them: from, to and the transition resource, repeated.</summary>
    public Collections.Array Transitions
    {
        get
        {
            var a = new Collections.Array();
            foreach (Edge e in Edges) { a.Add(new StringName(e.From)); a.Add(new StringName(e.To)); a.Add(e.Transition); }
            return a;
        }
        set
        {
            Edges.Clear();
            for (int i = 0; i + 2 < value.Count; i += 3)
                if (value[i + 2].AsGodotObject() is AnimationNodeStateMachineTransition t)
                    Edges.Add(new Edge { From = value[i].AsString(), To = value[i + 1].AsString(), Transition = t });
        }
    }

    // "states/Idle/node" and "states/Idle/position", the way a scene stores the states.
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("states/", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 3) return false;
        string n = p[1];
        if (p[2] == "node" && value.AsGodotObject() is AnimationNode node)
        {
            if (n == StartState || n == EndState) { _states[n].Node = node; return true; }
            if (_states.TryGetValue(n, out Entry? ex)) { ex.Node = node; return true; }
            AddNode(n, node);
            return true;
        }
        if (p[2] == "position")
        {
            if (!_states.TryGetValue(n, out Entry? e)) { e = new Entry(); _states[n] = e; _order.Add(n); }
            e.Position = value.AsVector2();
            return true;
        }
        return false;
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        if (!name.StartsWith("states/", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 3 || !_states.TryGetValue(p[1], out Entry? e)) return false;
        if (p[2] == "node") value = e.Node;
        else if (p[2] == "position") value = e.Position;
        else return false;
        return true;
    }

    // ---- paths ----

    /// <summary>The states to pass through, after <paramref name="from"/>, to get to <paramref name="to"/> by usable transitions: fewest transitions
    /// first, then the lowest total priority number. Null when there is no way.</summary>
    internal List<string>? ShortestPath(string from, string to)
    {
        if (from == to) return new List<string>();
        var cost = new Dictionary<string, long> { [from] = 0 };
        var prev = new Dictionary<string, string>();
        var done = new HashSet<string>();
        while (true)
        {
            string? u = null;
            long best = long.MaxValue;
            foreach (var kv in cost)
                if (!done.Contains(kv.Key) && kv.Value < best) { best = kv.Value; u = kv.Key; }
            if (u == null) return null;
            if (u == to) break;
            done.Add(u);
            foreach (Edge e in Edges)
            {
                if (e.From != u || e.Transition.AdvanceMode == AnimationNodeStateMachineTransition.AdvanceModeEnum.Disabled) continue;
                long c = best + 1_000_000L + Math.Clamp(e.Transition.Priority, 0, 900_000);
                if (!cost.TryGetValue(e.To, out long old) || c < old) { cost[e.To] = c; prev[e.To] = u; }
            }
        }
        var path = new List<string>();
        for (string at = to; at != from; at = prev[at]) path.Add(at);
        path.Reverse();
        return path;
    }

    // ---- evaluation ----

    internal AnimationNodeStateMachinePlayback PlaybackOf(AnimationTree tree, string @base)
    {
        Variant v = tree.Param(@base + "playback", default);
        if (v.AsGodotObject() is not AnimationNodeStateMachinePlayback pb)
        {
            pb = new AnimationNodeStateMachinePlayback();
            tree.SetParam(@base + "playback", pb);
        }
        pb.Machine = this;
        pb.Base = @base;
        return pb;
    }

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        return PlaybackOf(f.Tree, f.Base).Process(f, time, seek, external, testOnly);
    }

    internal override void ResetState(AnimationTree tree, string @base) => PlaybackOf(tree, @base).Restart();

    internal override void CollectParameters(AnimationTree tree, string @base)
    {
        PlaybackOf(tree, @base);
        foreach (Edge e in Edges)
        {
            string c = e.Transition.AdvanceCondition.ToString();
            if (c.Length > 0) tree.DeclareParam(@base + "conditions/" + c, false);
        }
        foreach (string n in _order)
            if (_states[n].Node is { } node) node.CollectParameters(tree, @base + n + "/");
    }
}

/// <summary>Where a state machine is and what it is doing; read and driven from scripts as <c>parameters/playback</c> (Godot's AnimationNodeStateMachinePlayback).</summary>
public class AnimationNodeStateMachinePlayback : Resource
{
    internal AnimationNodeStateMachine? Machine;
    internal string Base = "";

    private string _current = "";
    private bool _playing, _stopped;
    private string _fadingFrom = "";
    private double _fadeTime, _fadeElapsed;
    private Curve? _fadeCurve;
    private List<string> _path = new();
    private string? _travelTarget, _startRequest, _nestedTravel;
    private bool _travelReset = true, _startReset = true, _nextRequest;
    private NodeTime _time;

    // ---- the Godot API ----

    /// <summary>Goes to a state through the transitions (the shortest way). With no way, jumps there; <paramref name="resetOnTeleport"/> then restarts its animation.
    /// "Sub/State" travels into a nested machine.</summary>
    public void Travel(StringName toNode, bool resetOnTeleport = true)
    {
        _travelTarget = toNode.ToString();
        _travelReset = resetOnTeleport;
        _stopped = false;
    }

    /// <summary>Starts playing at a state, at once on the next pass.</summary>
    public void Start(StringName node, bool reset = true)
    {
        _startRequest = node.ToString();
        _startReset = reset;
        _stopped = false;
    }

    /// <summary>Takes the next transition now: the travel path's next step or the first automatic transition whose conditions hold.</summary>
    public void Next() => _nextRequest = true;

    public void Stop()
    {
        _playing = false;
        _stopped = true;
        _current = "";
        _fadingFrom = "";
        _path.Clear();
        _travelTarget = null;
        _startRequest = null;
    }

    public bool IsPlaying() => _playing;

    public StringName GetCurrentNode() => new(_current);

    public float GetCurrentPlayPosition() => (float)_time.Position;

    public float GetCurrentLength() => (float)_time.Length;

    public StringName GetFadingFromNode() => new(_fadingFrom);

    /// <summary>The states travel() will pass through next, in order, ending at its target.</summary>
    public StringName[] GetTravelPath() => _path.Select(p => new StringName(p)).ToArray();

    internal void Restart()
    {
        Start(AnimationNodeStateMachine.StartState, true);
        _path.Clear();
        _travelTarget = null;
    }

    // ---- one pass ----

    private AnimationNode? NodeOf(string state) => Machine?.GetNode(state);

    private NodeTime RunState(Frame f, string state, double time, bool seek, bool external, double blend, bool testOnly) =>
        Machine!.BlendChild(f, NodeOf(state), f.Base + state + "/", time, seek, external, blend, AnimationNode.FilterAction.Ignore, true, testOnly);

    internal NodeTime Process(Frame f, double delta, bool seek, bool external, bool testOnly)
    {
        AnimationNodeStateMachine sm = Machine!;
        if (testOnly) return _current.Length == 0 ? default : RunState(f, _current, delta, seek, external, 1.0, true);

        if (_startRequest != null)
        {
            string s = _startRequest;
            _startRequest = null;
            DoStart(f, s, _startReset);
        }
        else if (!_playing && !_stopped && _current.Length == 0 && sm.StateMachineType != AnimationNodeStateMachine.StateMachineTypeEnum.Grouped)
            DoStart(f, AnimationNodeStateMachine.StartState, true);
        if (!_playing) return default;

        if (_travelTarget != null) ResolveTravel(f);

        for (int hops = 0; hops < 8 && _playing; hops++)
        {
            NodeTime cur = RunState(f, _current, delta, false, false, 1.0, true);
            Edge? edge = Pick(f, cur, _nextRequest, out bool fromTravel);
            if (edge == null) break;
            _nextRequest = false;
            Goto(f, edge, fromTravel, cur);
        }
        if (!_playing) return default;

        double w = 1.0;
        if (_fadingFrom.Length > 0)
        {
            if (!seek) _fadeElapsed += Math.Abs(delta);
            if (_fadeTime <= 0 || _fadeElapsed >= _fadeTime) _fadingFrom = "";
            else
            {
                double x = _fadeElapsed / _fadeTime;
                w = _fadeCurve == null ? x : _fadeCurve.SampleBaked((float)x);
            }
        }
        NodeTime now = RunState(f, _current, delta, seek, external, w, false);
        if (_fadingFrom.Length > 0) RunState(f, _fadingFrom, delta, seek, external, 1.0 - w, false);
        _time = now;
        EnterNested(f);
        return now;
    }

    private void DoStart(Frame f, string state, bool reset)
    {
        _current = state;
        _playing = true;
        _fadingFrom = "";
        _path.Clear();
        if (reset) NodeOf(state)?.ResetState(f.Tree, f.Base + state + "/");
        if (state == AnimationNodeStateMachine.EndState) _playing = false;
    }

    private void ResolveTravel(Frame f)
    {
        AnimationNodeStateMachine sm = Machine!;
        string target = _travelTarget!;
        _travelTarget = null;
        _nestedTravel = null;
        int slash = target.IndexOf('/');
        if (slash > 0) { _nestedTravel = target[(slash + 1)..]; target = target[..slash]; }
        if (!sm.HasNode(target))
        {
            f.Tree.WarnOnce("travel", target, $"travel() names the state '{target}', which the state machine does not have");
            _nestedTravel = null;
            return;
        }
        if (target == _current)
        {
            if (sm.AllowTransitionToSelf && _nestedTravel == null) NodeOf(target)?.ResetState(f.Tree, f.Base + target + "/");
            return;
        }
        List<string>? path = sm.ShortestPath(_current, target);
        if (path == null)
        {
            // No way from here: jump.
            _fadingFrom = "";
            _path.Clear();
            _current = target;
            if (_travelReset) NodeOf(target)?.ResetState(f.Tree, f.Base + target + "/");
            if (target == AnimationNodeStateMachine.EndState) _playing = false;
        }
        else _path = path;
    }

    // After arriving at the first part of a "Sub/State" travel, the nested machine is told to go on.
    private void EnterNested(Frame f)
    {
        if (_nestedTravel == null || _path.Count > 0 || _current.Length == 0) return;
        if (NodeOf(_current) is AnimationNodeStateMachine inner)
        {
            inner.PlaybackOf(f.Tree, f.Base + _current + "/").Travel(_nestedTravel);
            _nestedTravel = null;
        }
    }

    private bool ConditionsHold(Frame f, AnimationNodeStateMachineTransition t)
    {
        string c = t.AdvanceCondition.ToString();
        if (c.Length > 0 && !f.Tree.Param(f.Base + "conditions/" + c, false).AsBool()) return false;
        return t.AdvanceExpression.Length == 0 || f.Tree.EvaluateExpression(t.AdvanceExpression);
    }

    private static bool ModeReady(AnimationNodeStateMachineTransition t, NodeTime cur) =>
        t.SwitchMode != AnimationNodeStateMachineTransition.SwitchModeEnum.AtEnd || cur.Remain <= Math.Max(t.XfadeTime, 0f) + 1e-9;

    // The transition to take now, if any.
    private Edge? Pick(Frame f, NodeTime cur, bool forced, out bool fromTravel)
    {
        AnimationNodeStateMachine sm = Machine!;
        fromTravel = false;
        if (_path.Count > 0)
        {
            string to = _path[0];
            Edge? best = null;
            foreach (Edge e in sm.Edges)
            {
                if (e.From != _current || e.To != to || e.Transition.AdvanceMode == AnimationNodeStateMachineTransition.AdvanceModeEnum.Disabled) continue;
                if (best == null || e.Transition.Priority < best.Transition.Priority) best = e;
            }
            if (best == null) { _path.Clear(); return null; }
            fromTravel = true;
            return forced || ModeReady(best.Transition, cur) ? best : null;
        }
        Edge? pick = null;
        foreach (Edge e in sm.Edges)
        {
            if (e.From != _current || e.Transition.AdvanceMode != AnimationNodeStateMachineTransition.AdvanceModeEnum.Auto) continue;
            if (!ConditionsHold(f, e.Transition)) continue;
            if (!forced && !ModeReady(e.Transition, cur)) continue;
            if (pick == null || e.Transition.Priority < pick.Transition.Priority) pick = e;
        }
        return pick;
    }

    private void Goto(Frame f, Edge edge, bool fromTravel, NodeTime cur)
    {
        AnimationNodeStateMachine sm = Machine!;
        AnimationNodeStateMachineTransition t = edge.Transition;
        string to = edge.To;
        if (fromTravel && _path.Count > 0) _path.RemoveAt(0);
        if (to == _current && !sm.AllowTransitionToSelf) return;

        string old = _current;
        _fadingFrom = old;
        _fadeTime = Math.Max(t.XfadeTime, 0f);
        _fadeElapsed = 0;
        _fadeCurve = t.XfadeCurve;
        _current = to;
        if (to == AnimationNodeStateMachine.EndState)
        {
            _playing = false;
            _fadingFrom = "";
            _path.Clear();
            if (sm.ResetEnds) NodeOf(old)?.ResetState(f.Tree, f.Base + old + "/");
            return;
        }
        AnimationNode? node = NodeOf(to);
        if (t.Reset && node != null) node.ResetState(f.Tree, f.Base + to + "/");
        if (t.SwitchMode == AnimationNodeStateMachineTransition.SwitchModeEnum.Sync && node != null && cur.Length > 0)
        {
            // The next state starts at the same fraction of its length as the old one has reached.
            NodeTime next = RunState(f, to, 0, false, false, 1.0, true);
            double at = Math.Clamp(cur.Position / cur.Length, 0.0, 1.0) * next.Length;
            RunState(f, to, at, true, false, 0.0, false);
        }
    }
}
