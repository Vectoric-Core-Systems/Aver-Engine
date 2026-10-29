// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// AnimationTree and the graph's base classes (Godot's AnimationNode family): the node resources, the parameters/... store, and the leaf
// (AnimationNodeAnimation), the output and the blend tree. The graph is evaluated here, every pass, from the root; each animation it wants
// played is handed to the engine player (PropPlayer::treeAdd, Aver.Anim.Abi) with its playhead, weight and per-track filter weights, and the
// engine mixes them with the AnimationPlayer's rules (weights, discrete keys, method order, root motion). The blend nodes are in
// AnimationBlendNodes.cs, the state machine in AnimationStateMachine.cs, the blend spaces in AnimationBlendSpace.cs.
// docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>How far an evaluated node has played: the length of what it plays, the playhead, and the direction. Used for "at end" transitions
/// and for synchronised switches.</summary>
internal readonly record struct NodeTime(double Length, double Position, bool Backward, bool Loop = false)
{
    public double Remain => Backward ? Math.Max(0, Position) : Math.Max(0, Length - Position);
}

/// <summary>One filter's effect on the animations below it: a track on the filter list is weighted by <see cref="In"/>, any other by <see cref="Out"/>.</summary>
internal sealed class FilterLayer
{
    public HashSet<string> Paths = new(StringComparer.Ordinal);
    public double In = 1, Out = 1;
}

/// <summary>What a node is being evaluated under: the tree, its parameter prefix, the weight reaching it, the active filters, and how its
/// inputs are found when a blend tree evaluates it.</summary>
internal sealed class Frame
{
    public AnimationTree Tree = null!;
    public string Base = "parameters/";
    public double Blend = 1.0;
    public List<FilterLayer>? Layers;
    public Func<int, (AnimationNode? Node, string Base)>? Inputs;
}

/// <summary>A node of an AnimationTree's graph (Godot's AnimationNode).</summary>
public class AnimationNode : Resource
{
    public enum FilterAction { Ignore = 0, Pass = 1, Stop = 2, Blend = 3 }

    public new class SignalName : Resource.SignalName
    {
        public static readonly StringName TreeChanged = "tree_changed";
        public static readonly StringName AnimationNodeRenamed = "animation_node_renamed";
        public static readonly StringName AnimationNodeRemoved = "animation_node_removed";
    }

    private readonly List<string> _filters = new();

    /// <summary>When true the node's filter list decides which tracks its inputs affect (blend, add and one-shot nodes).</summary>
    public bool FilterEnabled { get; set; }

    /// <summary>Adds or removes a track path on the filter list.</summary>
    public void SetFilterPath(NodePath path, bool enable)
    {
        string p = path.ToString();
        if (enable) { if (!_filters.Contains(p)) _filters.Add(p); }
        else _filters.Remove(p);
        EmitSignal(SignalName.TreeChanged);
    }

    public bool IsPathFiltered(NodePath path) => _filters.Contains(path.ToString());

    /// <summary>The filter list as a scene stores it: an array of node paths.</summary>
    public Collections.Array Filters
    {
        get
        {
            var a = new Collections.Array();
            foreach (string f in _filters) a.Add(new NodePath(f));
            return a;
        }
        set
        {
            _filters.Clear();
            foreach (Variant v in value) _filters.Add(v.AsString());
        }
    }

    internal IReadOnlyList<string> FilterPaths => _filters;

    public virtual string GetCaption() => "Node";

    public virtual bool HasFilter() => false;

    // ---- evaluation (internal) ----

    /// <summary>One pass over this node. <paramref name="time"/> is the step in seconds, or the playhead when <paramref name="seek"/>;
    /// <paramref name="testOnly"/> only measures (no state changes, nothing added to the mix).</summary>
    internal virtual NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly) => default;

    /// <summary>Puts the node back at its start (a state is entered with reset).</summary>
    internal virtual void ResetState(AnimationTree tree, string @base) { }

    /// <summary>Registers the parameters this node (and the nodes below it) own, with their defaults.</summary>
    internal virtual void CollectParameters(AnimationTree tree, string @base) { }

    // Evaluates a child (an input or a state) under this node's filter.
    internal NodeTime BlendChild(Frame f, AnimationNode? child, string childBase, double time, bool seek, bool external, double blend,
        FilterAction filter, bool sync, bool testOnly, Func<int, (AnimationNode? Node, string Base)>? inputs = null)
    {
        if (child == null) return default;
        var cf = new Frame { Tree = f.Tree, Base = childBase, Blend = f.Blend, Layers = f.Layers, Inputs = inputs };
        if (filter != FilterAction.Ignore && FilterEnabled)
        {
            var layer = new FilterLayer();
            foreach (string p in _filters) layer.Paths.Add(p);
            switch (filter)
            {
                case FilterAction.Pass: layer.In = blend; layer.Out = 0; break;
                case FilterAction.Stop: layer.In = 0; layer.Out = blend; break;
                default: layer.In = blend; layer.Out = 1; break;
            }
            cf.Layers = new List<FilterLayer>(f.Layers ?? new List<FilterLayer>()) { layer };
        }
        else cf.Blend = f.Blend * blend;
        // A blend of 0 that is not synchronised does not advance its input.
        if (!sync && Math.Abs(blend) < 1e-9) testOnly = true;
        return child.Run(cf, time, seek, external, testOnly);
    }

    // Evaluates the node wired to input <paramref name="index"/> of this node in its blend tree.
    internal NodeTime BlendInput(Frame f, int index, double time, bool seek, bool external, double blend, FilterAction filter, bool sync, bool testOnly)
    {
        if (f.Inputs == null) return default;
        var (node, childBase) = f.Inputs(index);
        return BlendChild(f, node, childBase, time, seek, external, blend, filter, sync, testOnly, null);
    }

    // The node of a pair whose weight is larger (the first on a tie): what the parent reports as its own time.
    internal static NodeTime Dominant(NodeTime a, double wa, NodeTime b, double wb) => wb > wa ? b : a;
}

/// <summary>A node that can be the root of a tree (Godot's AnimationRootNode).</summary>
public class AnimationRootNode : AnimationNode
{
}

/// <summary>Plays one animation (Godot's AnimationNodeAnimation).</summary>
public class AnimationNodeAnimation : AnimationRootNode
{
    public enum PlayModeEnum { Forward = 0, Backward = 1 }

    private sealed class LeafState
    {
        public double Time;
        public double Dir = 1;
        public bool Fresh = true;
    }

    /// <summary>The key of the animation in the tree's libraries ("walk" or "library/walk").</summary>
    public StringName Animation { get; set; } = StringName.Empty;

    public PlayModeEnum PlayMode { get; set; } = PlayModeEnum.Forward;

    /// <summary>Stored; the first pass always includes the key at the start.</summary>
    public bool AdvanceOnStart { get; set; }

    /// <summary>Stored: the timeline of the animation itself is used (a custom start, length and stretch are not applied).</summary>
    public bool UseCustomTimeline { get; set; }
    public double TimelineLength { get; set; } = 1.0;
    public bool StretchTimeScale { get; set; } = true;
    public double StartOffset { get; set; }
    public Animation.LoopModeEnum LoopMode { get; set; } = Aver.Fabric.Animation.LoopModeEnum.None;

    public override string GetCaption() => "Animation";

    internal override void ResetState(AnimationTree tree, string @base) => tree.StateOf<LeafState>(@base + "time").Fresh = true;

    internal override void CollectParameters(AnimationTree tree, string @base) => tree.DeclareState<LeafState>(@base + "time");

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        AnimationTree tree = f.Tree;
        Animation? anim = tree.GetAnimation(Animation);
        if (anim == null)
        {
            tree.WarnOnce("a", Animation.ToString(), $"there is no animation '{Animation}' in the tree's libraries");
            return default;
        }
        double len = anim.Length;
        bool backward = PlayMode == PlayModeEnum.Backward;
        LeafState st = tree.StateOf<LeafState>(f.Base + "time");

        double prev, step = 0;
        bool seeked = false, includeStart = false;
        if (st.Fresh)
        {
            prev = backward ? len : 0;
            st.Dir = 1;
            includeStart = true;
        }
        else prev = st.Time;
        if (seek) { prev = time; seeked = true; includeStart = false; }
        else step = time * (backward ? -st.Dir : st.Dir);

        if (testOnly)
        {
            PropStepper.Step(len, (int)anim.LoopMode, prev, seeked ? 0 : step, out double peek, out bool wrapped, out int bounces);
            bool heading = (backward ? -st.Dir : st.Dir) < 0;
            // A step that wraps or bounces reports the end of the cycle it finishes (so an at-end transition sees a loop's last frame), not the wrapped playhead.
            if (wrapped) peek = heading ? 0 : len;
            bool pback = wrapped ? heading : heading ^ ((bounces & 1) != 0);
            return new NodeTime(len, peek, pback, anim.LoopMode != Aver.Fabric.Animation.LoopModeEnum.None);
        }

        double[]? tw = tree.TrackWeights(anim, f.Layers);
        var r = tree.TreeAdd(Animation.ToString(), prev, step, f.Blend, tw, seeked, external, includeStart);
        if (!r.Ok) return default;
        st.Time = r.Next;
        st.Fresh = false;
        if (r.Flipped) st.Dir = -st.Dir;
        return new NodeTime(len, r.Next, (backward ? -st.Dir : st.Dir) < 0, anim.LoopMode != Aver.Fabric.Animation.LoopModeEnum.None);
    }
}

/// <summary>The sink of a blend tree (Godot's AnimationNodeOutput): its one input is what the tree plays.</summary>
public class AnimationNodeOutput : AnimationNode
{
    public override string GetCaption() => "Output";
}

/// <summary>A graph of blend nodes wired from an output (Godot's AnimationNodeBlendTree).</summary>
public class AnimationNodeBlendTree : AnimationRootNode
{
    public enum ConnectionError { Ok = 0, NoInput = 1, NoInputIndex = 2, NoOutput = 3, SameNode = 4, ConnectionExists = 5 }

    public const string OutputName = "output";

    private sealed class Entry
    {
        public AnimationNode Node = null!;
        public Vector2 Position;
    }

    private readonly Dictionary<string, Entry> _nodes = new(StringComparer.Ordinal);
    private readonly List<string> _order = new();
    private readonly Dictionary<(string Node, int Port), string> _connections = new();

    public Vector2 GraphOffset { get; set; }

    public AnimationNodeBlendTree()
    {
        _nodes[OutputName] = new Entry { Node = new AnimationNodeOutput(), Position = new Vector2(300, 150) };
        _order.Add(OutputName);
    }

    public override string GetCaption() => "BlendTree";

    // ---- nodes ----

    public void AddNode(StringName name, AnimationNode node, Vector2 position = default)
    {
        string n = name.ToString();
        if (n.Length == 0 || _nodes.ContainsKey(n) || node == null) return;
        _nodes[n] = new Entry { Node = node, Position = position };
        _order.Add(n);
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public AnimationNode? GetNode(StringName name) => _nodes.TryGetValue(name.ToString(), out Entry? e) ? e.Node : null;

    public bool HasNode(StringName name) => _nodes.ContainsKey(name.ToString());

    public void RemoveNode(StringName name)
    {
        string n = name.ToString();
        if (n == OutputName || !_nodes.Remove(n)) return;
        _order.Remove(n);
        foreach (var k in _connections.Where(c => c.Key.Node == n || c.Value == n).Select(c => c.Key).ToList()) _connections.Remove(k);
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void RenameNode(StringName name, StringName newName)
    {
        string o = name.ToString(), n = newName.ToString();
        if (o == OutputName || !_nodes.TryGetValue(o, out Entry? e) || n.Length == 0 || _nodes.ContainsKey(n)) return;
        _nodes.Remove(o);
        _nodes[n] = e;
        _order[_order.IndexOf(o)] = n;
        foreach (var k in _connections.Keys.ToList())
        {
            string src = _connections[k];
            if (k.Node == o) { _connections.Remove(k); _connections[(n, k.Port)] = src == o ? n : src; }
            else if (src == o) _connections[k] = n;
        }
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void SetNodePosition(StringName name, Vector2 position) { if (_nodes.TryGetValue(name.ToString(), out Entry? e)) e.Position = position; }

    public Vector2 GetNodePosition(StringName name) => _nodes.TryGetValue(name.ToString(), out Entry? e) ? e.Position : default;

    /// <summary>The names of the nodes in the order they were added (the output first).</summary>
    public StringName[] GetNodeList() => _order.Select(n => new StringName(n)).ToArray();

    // ---- wiring ----

    /// <summary>Wires the output of <paramref name="outputNode"/> to input <paramref name="inputIndex"/> of <paramref name="inputNode"/>.</summary>
    public void ConnectNode(StringName inputNode, int inputIndex, StringName outputNode)
    {
        string i = inputNode.ToString(), o = outputNode.ToString();
        if (!_nodes.ContainsKey(i) || !_nodes.ContainsKey(o) || i == o || inputIndex < 0 || _connections.ContainsKey((i, inputIndex))) return;
        _connections[(i, inputIndex)] = o;
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void DisconnectNode(StringName inputNode, int inputIndex)
    {
        if (_connections.Remove((inputNode.ToString(), inputIndex))) EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public ConnectionError CanConnect(StringName inputNode, int inputIndex, StringName outputNode)
    {
        string i = inputNode.ToString(), o = outputNode.ToString();
        if (!_nodes.ContainsKey(i)) return ConnectionError.NoInput;
        if (!_nodes.ContainsKey(o)) return ConnectionError.NoOutput;
        if (i == o) return ConnectionError.SameNode;
        if (inputIndex < 0) return ConnectionError.NoInputIndex;
        return _connections.ContainsKey((i, inputIndex)) ? ConnectionError.ConnectionExists : ConnectionError.Ok;
    }

    /// <summary>The wiring as scenes store it: input node, input index and source node, repeated.</summary>
    public Collections.Array NodeConnections
    {
        get
        {
            var a = new Collections.Array();
            foreach (var c in _connections.OrderBy(c => _order.IndexOf(c.Key.Node)).ThenBy(c => c.Key.Port))
            {
                a.Add(new StringName(c.Key.Node));
                a.Add(c.Key.Port);
                a.Add(new StringName(c.Value));
            }
            return a;
        }
        set
        {
            _connections.Clear();
            for (int i = 0; i + 2 < value.Count; i += 3) _connections[(value[i].AsString(), value[i + 1].AsInt32())] = value[i + 2].AsString();
        }
    }

    // "nodes/Name/node" and "nodes/Name/position", the way a scene stores the nodes.
    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("nodes/", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 3) return false;
        string n = p[1];
        if (p[2] == "node" && value.AsGodotObject() is AnimationNode node)
        {
            if (n == OutputName) { if (_nodes[n].Node is not AnimationNodeOutput) _nodes[n].Node = node; return true; }
            if (_pendingPositions.Remove(n)) { _nodes[n].Node = node; return true; }
            AddNode(n, node);
            return true;
        }
        if (p[2] == "position")
        {
            if (!_nodes.ContainsKey(n)) { _nodes[n] = new Entry { Node = new AnimationNodeOutput() }; _order.Add(n); _pendingPositions.Add(n); }
            _nodes[n].Position = value.AsVector2();
            return true;
        }
        return false;
    }

    // A "position" can arrive before its "node" (the output node has only a position); a placeholder is replaced when the node comes.
    private readonly HashSet<string> _pendingPositions = new();

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        if (!name.StartsWith("nodes/", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 3 || !_nodes.TryGetValue(p[1], out Entry? e)) return false;
        if (p[2] == "node") value = e.Node;
        else if (p[2] == "position") value = e.Position;
        else return false;
        return true;
    }

    // ---- evaluation ----

    private (AnimationNode? Node, string Base) Source(string @base, string node, int port)
    {
        if (!_connections.TryGetValue((node, port), out string? src) || !_nodes.TryGetValue(src, out Entry? e)) return (null, "");
        return (e.Node, @base + src + "/");
    }

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        var (node, childBase) = Source(f.Base, OutputName, 0);
        if (node == null) return default;
        string src = _connections[(OutputName, 0)];
        return BlendChild(f, node, childBase, time, seek, external, 1.0, FilterAction.Ignore, true, testOnly, port => Source(f.Base, src, port));
    }

    internal override void ResetState(AnimationTree tree, string @base)
    {
        foreach (var kv in _nodes) kv.Value.Node.ResetState(tree, @base + kv.Key + "/");
    }

    internal override void CollectParameters(AnimationTree tree, string @base)
    {
        foreach (var kv in _nodes) kv.Value.Node.CollectParameters(tree, @base + kv.Key + "/");
    }
}

/// <summary>The pure playhead movement of PropPlayer's stepClip, for evaluating a node without touching the engine player.</summary>
internal static class PropStepper
{
    /// <summary>Moves <paramref name="prev"/> by <paramref name="d"/> seconds under a loop mode (0 none, 1 linear, 2 ping-pong).</summary>
    internal static void Step(double len, int loop, double prev, double d, out double next, out bool looped, out int bounces)
    {
        looped = false;
        bounces = 0;
        next = prev + d;
        switch (loop)
        {
            case 0: next = Math.Clamp(next, 0.0, Math.Max(len, 0.0)); break;
            case 1:
                if (len <= 0) next = 0;
                else if (d == 0) { if (next < 0 || next > len) next = PosMod(next, len); }
                else if (d > 0 && next >= len) { next = PosMod(next - len, len); looped = true; }
                else if (d < 0 && next <= 0) { next = len - PosMod(-next, len); looped = true; }
                break;
            default:
            {
                if (len <= 0) { next = 0; break; }
                double pos = prev;
                if (pos < 0 || pos > len) { pos = PosMod(pos, 2 * len); if (pos > len) pos = 2 * len - pos; }
                double dir = d >= 0 ? 1.0 : -1.0;
                if (d == 0) { next = pos; break; }
                double remaining = Math.Abs(d);
                for (int guard = 0; remaining > 0 && guard < 16; guard++)
                {
                    double room = dir > 0 ? len - pos : pos;
                    if (remaining <= room) { pos += dir * remaining; remaining = 0; break; }
                    pos = dir > 0 ? len : 0.0;
                    remaining -= room;
                    dir = -dir;
                    bounces++;
                    looped = true;
                }
                next = pos;
                break;
            }
        }
    }

    private static double PosMod(double x, double y)
    {
        if (y == 0) return 0;
        double r = x % y;
        return r < 0 ? r + y : r;
    }
}

/// <summary>Plays a graph of animation nodes (Godot's AnimationTree): a state machine, a blend tree or a blend space, evaluated every pass
/// and mixed by the engine.</summary>
public class AnimationTree : AnimationMixer
{
    private AnimationRootNode? _root;
    private readonly Dictionary<string, Variant> _params = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, object> _state = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> _warned = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Expression> _expressions = new(StringComparer.Ordinal);
    private NodePath _advanceBase = new(".");
    private NodePath _animPlayer = new();

    public AnimationTree()
    {
        // The tree starts inactive and mixes deterministically (an additive node adds to the initial value instead of averaging).
        if (Player != 0)
        {
            PplayerNative.aver_pplayer_set_active(Player, 0);
            PplayerNative.aver_pplayer_set_deterministic(Player, 1);
        }
    }

    /// <summary>The root of the graph.</summary>
    public AnimationRootNode? TreeRoot
    {
        get => _root;
        set
        {
            _root = value;
            _state.Clear();
            _warned.Clear();
            _root?.CollectParameters(this, "parameters/");
        }
    }

    /// <summary>The AnimationPlayer whose animation libraries and root node this tree uses.</summary>
    public NodePath AnimPlayer
    {
        get => _animPlayer;
        set { _animPlayer = value; SyncFromPlayer(); }
    }

    /// <summary>The node whose properties and methods a transition's advance_expression reads (default: the tree itself).</summary>
    public NodePath AdvanceExpressionBaseNode
    {
        get => _advanceBase;
        set => _advanceBase = value;
    }

    internal override bool ShouldStep() => Player != 0 && Active && _root != null;

    // ---- parameters ----

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("parameters/", StringComparison.OrdinalIgnoreCase)) return false;
        _params[name] = value;
        return true;
    }

    internal override bool TryDynamicGet(string name, out Variant value)
    {
        value = default;
        if (!name.StartsWith("parameters/", StringComparison.OrdinalIgnoreCase)) return false;
        return _params.TryGetValue(name, out value);
    }

    /// <summary>The value of a parameter (<c>parameters/Blend2/blend_amount</c>), or <paramref name="fallback"/> when it was never set.</summary>
    internal Variant Param(string name, Variant fallback) => _params.TryGetValue(name, out Variant v) ? v : fallback;

    internal void SetParam(string name, Variant value) => _params[name] = value;

    /// <summary>Registers a parameter's default without replacing a value a scene already set.</summary>
    internal void DeclareParam(string name, Variant fallback) => _params.TryAdd(name, fallback);

    internal T StateOf<T>(string key) where T : class, new()
    {
        if (_state.TryGetValue(key, out object? o) && o is T t) return t;
        t = new T();
        _state[key] = t;
        return t;
    }

    internal void DeclareState<T>(string key) where T : class, new() => StateOf<T>(key);

    internal void WarnOnce(string kind, string what, string message)
    {
        if (_warned.Add(kind + ":" + what)) Aver.Scripting.Log.Warn($"[Fabric] AnimationTree '{Name}': {message}");
    }

    // ---- libraries from an AnimationPlayer ----

    private void SyncFromPlayer()
    {
        if (_animPlayer.IsEmpty || !IsInsideTree()) return;
        if (GetNodeOrNull(_animPlayer) is not AnimationPlayer ap) return;
        var wanted = ap.GetAnimationLibraryList();
        foreach (StringName n in GetAnimationLibraryList())
            if (!wanted.Contains(n)) RemoveAnimationLibrary(n);
        foreach (StringName n in wanted)
        {
            AnimationLibrary? lib = ap.GetAnimationLibrary(n);
            if (lib == null || ReferenceEquals(GetAnimationLibrary(n), lib)) continue;
            RemoveAnimationLibrary(n);
            AddAnimationLibrary(n, lib);
        }
        Node? track = ap.GetNodeOrNull(ap.RootNode);
        if (track != null) RootNode = GetPathTo(track);
    }

    internal override void OnReadying()
    {
        base.OnReadying();
        SyncFromPlayer();
    }

    // ---- stepping ----

    public override void Advance(double delta)
    {
        if (Player == 0 || _root == null || !Active) return;
        SyncFromPlayer();
        SyncLibraries();
        var f = new Frame { Tree = this, Base = "parameters/", Blend = 1.0 };
        _root.Run(f, delta, false, false, false);
        PplayerNative.aver_pplayer_tree_mix(Player, 0);
        DispatchEvents();
    }

    /// <summary>What a leaf asks of the engine player for one animation of this pass.</summary>
    internal readonly record struct AddResult(bool Ok, double Next, bool Looped, bool Flipped);

    internal AddResult TreeAdd(string key, double prev, double delta, double weight, double[]? trackWeights, bool seeked, bool external, bool includeStart)
    {
        var next = new double[1];
        int r = PplayerNative.aver_pplayer_tree_add(Player, key, prev, delta, weight, trackWeights!, trackWeights?.Length ?? 0, seeked ? 1 : 0, external ? 1 : 0, includeStart ? 1 : 0, next);
        if (r < 0) return default;
        return new AddResult(true, next[0], (r & 1) != 0, (r & 2) != 0);
    }

    // The weight each track of the clip gets from the filters above the leaf, or null when no filter is active.
    internal double[]? TrackWeights(Animation anim, List<FilterLayer>? layers)
    {
        if (layers == null || layers.Count == 0) return null;
        int n = anim.GetTrackCount();
        var w = new double[n];
        for (int i = 0; i < n; i++)
        {
            string path = anim.TrackGetPath(i).ToString();
            double x = 1;
            foreach (FilterLayer l in layers) x *= l.Paths.Contains(path) ? l.In : l.Out;
            w[i] = x;
        }
        return w;
    }

    // ---- advance_expression ----

    internal bool EvaluateExpression(string text)
    {
        if (text.Length == 0) return true;
        if (!_expressions.TryGetValue(text, out Expression? e))
        {
            e = new Expression();
            if (e.Parse(text) != Error.Ok)
            {
                WarnOnce("expr", text, $"the advance expression '{text}' cannot be parsed: {e.GetErrorText()}");
                _expressions[text] = e;
                return false;
            }
            _expressions[text] = e;
        }
        GodotObject? baseObject = _advanceBase.IsEmpty || _advanceBase.ToString() == "." ? this : GetNodeOrNull(_advanceBase);
        if (baseObject == null)
        {
            WarnOnce("exprbase", _advanceBase.ToString(), $"the advance expression base node '{_advanceBase}' does not exist");
            return false;
        }
        Variant v = e.Execute(null, baseObject, false);
        if (e.HasExecuteFailed())
        {
            WarnOnce("exprrun", text, $"the advance expression '{text}' failed: {e.GetErrorText()}");
            return false;
        }
        return v.AsBool();
    }
}
