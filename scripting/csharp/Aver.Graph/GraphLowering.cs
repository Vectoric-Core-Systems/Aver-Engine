using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;

namespace Aver.Graph;

/// The pre-pass both compilers run before validating and emitting: macros are inlined and reroute (knot)
/// nodes are removed, rewriting links into direct ones, so no emitter ever sees a Reroute, MacroInstance,
/// MacroInput or MacroOutput.
///
/// Macro expansion copies the macro body once per instance with node ids prefixed by the instance's id. The
/// entry and exit tunnels are not copied: each macro pin becomes a Reroute, the instance's links are retargeted
/// onto those, and reroute elimination then joins everything (exec and data alike, including a body that wires
/// an input straight to an output).
internal static class GraphLowering
{
    private const int MaxExpansions = 20000;

    private static bool Is(Node n, string type) => n.Type.Equals(type, StringComparison.OrdinalIgnoreCase);
    private static bool SameName(string? a, string? b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);

    /// Macro declaration problems of the raw graph. True when `report` asked to stop.
    internal static bool Diagnose(Graph g, Func<string, string, string?, string?, bool> report)
    {
        if (g.Macros.Count == 0 && !g.Nodes.Values.Any(n => n.MacroOwner != null || Is(n, "macroinstance"))) return false;

        GraphMacro? Find(string? name) => g.Macros.FirstOrDefault(m => SameName(m.Name, name));

        foreach (var node in g.Nodes.Values)
        {
            if (node.MacroOwner != null && Find(node.MacroOwner) == null &&
                report("macro.owner", $"Node '{node.Id}' has macro={node.MacroOwner}, which no MACRO record declares", node.Id, null)) return true;
            if ((Is(node, "macroinput") || Is(node, "macrooutput")) && node.MacroOwner == null &&
                report("macro.owner", $"Node '{node.Id}' is a {node.Type} but has no macro= attribute saying which macro it belongs to", node.Id, null)) return true;
            if (Is(node, "macroinstance"))
            {
                if (string.IsNullOrEmpty(node.CallTarget))
                {
                    if (report("macro.call", $"Node '{node.Id}' is a MacroInstance but has no call= attribute naming which macro it uses", node.Id, null)) return true;
                }
                else if (Find(node.CallTarget) == null &&
                         report("macro.call", $"Node '{node.Id}' uses macro '{node.CallTarget}', which no MACRO record declares", node.Id, null)) return true;
            }
        }

        foreach (var link in g.Links)
        {
            if (!g.Nodes.TryGetValue(link.SourceNodeId, out var sn) || !g.Nodes.TryGetValue(link.TargetNodeId, out var tn)) continue;
            if (SameName(sn.MacroOwner, tn.MacroOwner)) continue;
            string Where(Node n) => n.MacroOwner == null ? "outside any macro" : $"macro '{n.MacroOwner}'";
            if (report("macro.crosslink", $"link '{link.SourceNodeId}.{link.SourcePinName}' -> '{link.TargetNodeId}.{link.TargetPinName}' " +
                       $"crosses from {Where(sn)} into {Where(tn)}. A wire cannot leave a macro body -- go through its MacroInput and MacroOutput.",
                       link.SourceNodeId, link.SourcePinName)) return true;
        }

        foreach (var m in g.Macros)
        {
            int ins = g.Nodes.Values.Count(n => Is(n, "macroinput") && SameName(n.MacroOwner, m.Name));
            int outs = g.Nodes.Values.Count(n => Is(n, "macrooutput") && SameName(n.MacroOwner, m.Name));
            if (ins > 1 && report("macro.entry", $"macro '{m.Name}' has {ins} MacroInput nodes; it has at most one", null, null)) return true;
            if (outs > 1 && report("macro.exit", $"macro '{m.Name}' has {outs} MacroOutput nodes; it has at most one", null, null)) return true;
        }

        // A macro that contains itself, directly or through another, would expand forever.
        var uses = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
        foreach (var m in g.Macros) uses[m.Name] = new List<string>();
        foreach (var n in g.Nodes.Values)
            if (Is(n, "macroinstance") && n.MacroOwner != null && n.CallTarget != null && uses.TryGetValue(n.MacroOwner, out var list))
                list.Add(n.CallTarget);
        var state = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);   // 1 = on the stack, 2 = done
        var stack = new List<string>();
        bool Cycle(string name)
        {
            if (!uses.ContainsKey(name)) return false;
            if (state.TryGetValue(name, out int s)) return s == 1;
            state[name] = 1;
            stack.Add(name);
            foreach (var next in uses[name])
                if (Cycle(next)) return true;
            stack.RemoveAt(stack.Count - 1);
            state[name] = 2;
            return false;
        }
        foreach (var m in g.Macros)
        {
            stack.Clear();
            if (!Cycle(m.Name)) continue;
            if (report("macro.recursion", $"macro '{m.Name}' contains itself (via {string.Join(" -> ", stack)}). A macro is inlined, so it cannot recurse -- use a function for recursion.", null, null)) return true;
            break;
        }
        return false;
    }

    /// Returns `lowered` as `g` itself when there is nothing to do, else a copy with macros expanded and
    /// reroutes removed. False with `err` when a macro problem makes expansion impossible.
    internal static bool Lower(Graph g, out Graph lowered, out string? err)
    {
        lowered = g;
        err = null;
        bool hasMacros = g.Nodes.Values.Any(n => n.MacroOwner != null || Is(n, "macroinstance"));
        bool hasReroutes = g.Nodes.Values.Any(n => Is(n, "reroute"));
        if (!hasMacros && !hasReroutes) return true;

        string? problem = null;
        Diagnose(g, (code, msg, _, _) => { if (code.StartsWith("macro.", StringComparison.Ordinal)) problem = msg; return problem != null; });
        if (problem != null) { err = problem; return false; }

        var nodes = new Dictionary<string, Node>(g.Nodes);
        var links = new List<Link>(g.Links);
        var consts = new List<ConstantOutput>(g.ConstantOutputs);
        var pinned = new List<PinnedValue>(g.PinnedValues);
        var outputs = new List<(string NodeId, string PinName)>(g.Outputs);

        if (hasMacros && !Expand(g, nodes, links, consts, pinned, outputs, out err)) return false;
        EliminateReroutes(nodes, links, consts, pinned, outputs);

        lowered = g.ShallowClone();
        lowered.Nodes = nodes;
        lowered.Links = links;
        lowered.ConstantOutputs = consts;
        lowered.PinnedValues = pinned;
        lowered.Outputs = outputs;
        return true;
    }

    private static readonly PropertyInfo[] s_nodeProps = typeof(Node).GetProperties()
        .Where(p => p.SetMethod != null && p.Name != nameof(Node.Id) && p.Name != nameof(Node.Type) &&
                    (p.PropertyType == typeof(string) || !typeof(IEnumerable).IsAssignableFrom(p.PropertyType)))
        .ToArray();

    private static Node CloneNode(Node n, string id, string? funcOwner)
    {
        var c = new Node { Id = id, Type = n.Type };
        foreach (var p in s_nodeProps) p.SetValue(c, p.GetValue(n));
        foreach (var kv in n.Attrs) c.Attrs[kv.Key] = kv.Value;
        foreach (var pin in n.Pins)
            c.Pins.Add(new Pin { Name = pin.Name, Type = pin.Type, IsOutput = pin.IsOutput, NodeId = id, StructName = pin.StructName });
        c.FuncOwner = funcOwner;
        c.MacroOwner = null;
        return c;
    }

    private static Node MakeReroute(string id, GraphParameter p, string? funcOwner)
    {
        var r = new Node { Id = id, Type = "Reroute", FuncOwner = funcOwner };
        r.Pins.Add(new Pin { Name = "in", Type = p.Type, IsOutput = false, NodeId = id, StructName = p.StructName });
        r.Pins.Add(new Pin { Name = "out", Type = p.Type, IsOutput = true, NodeId = id, StructName = p.StructName });
        return r;
    }

    private static PinnedValue Repin(PinnedValue pv, string nodeId, string pinName) =>
        new() { NodeId = nodeId, PinName = pinName, Value = pv.Value, FromPinDefault = pv.FromPinDefault, Raw = pv.Raw };

    private static bool Expand(Graph g, Dictionary<string, Node> nodes, List<Link> links, List<ConstantOutput> consts,
                               List<PinnedValue> pinned, List<(string NodeId, string PinName)> outputs, out string? err)
    {
        err = null;
        var bodyIds = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
        foreach (var n in g.Nodes.Values)
            if (n.MacroOwner != null)
            {
                if (!bodyIds.TryGetValue(n.MacroOwner, out var set)) bodyIds[n.MacroOwner] = set = new HashSet<string>();
                set.Add(n.Id);
            }
        bool InBody(string id) => g.Nodes.TryGetValue(id, out var n) && n.MacroOwner != null;
        var bodyLinks = g.Links.Where(l => InBody(l.SourceNodeId) && InBody(l.TargetNodeId) &&
                                           SameName(g.Nodes[l.SourceNodeId].MacroOwner, g.Nodes[l.TargetNodeId].MacroOwner)).ToList();

        // The live graph keeps only what is outside every macro body: bodies live on as templates.
        foreach (var id in g.Nodes.Values.Where(n => n.MacroOwner != null).Select(n => n.Id)) nodes.Remove(id);
        links.RemoveAll(l => InBody(l.SourceNodeId) || InBody(l.TargetNodeId));
        consts.RemoveAll(c => InBody(c.NodeId));
        pinned.RemoveAll(p => InBody(p.NodeId));

        var queue = new Queue<string>(nodes.Values.Where(n => Is(n, "macroinstance")).Select(n => n.Id));
        int expansions = 0;
        while (queue.Count > 0)
        {
            string instId = queue.Dequeue();
            if (!nodes.TryGetValue(instId, out var inst)) continue;
            if (++expansions > MaxExpansions) { err = "macro expansion did not finish (too many nested instances)"; return false; }
            var macro = g.Macros.FirstOrDefault(m => SameName(m.Name, inst.CallTarget));
            if (macro == null) { err = $"Node '{instId}' uses macro '{inst.CallTarget}', which no MACRO record declares"; return false; }

            string prefix = instId + "~";
            string? fn = inst.FuncOwner;
            bodyIds.TryGetValue(macro.Name, out var ids);
            var tunnelIn = new HashSet<string>();
            var tunnelOut = new HashSet<string>();
            if (ids != null)
                foreach (var bid in ids)
                {
                    var bn = g.Nodes[bid];
                    if (Is(bn, "macroinput")) tunnelIn.Add(bid);
                    else if (Is(bn, "macrooutput")) tunnelOut.Add(bid);
                    else
                    {
                        var copy = CloneNode(bn, prefix + bid, fn);
                        nodes[copy.Id] = copy;
                        if (Is(copy, "macroinstance")) queue.Enqueue(copy.Id);
                    }
                }
            foreach (var p in macro.Inputs) nodes[prefix + "in~" + p.Name] = MakeReroute(prefix + "in~" + p.Name, p, fn);
            foreach (var p in macro.Outputs) nodes[prefix + "out~" + p.Name] = MakeReroute(prefix + "out~" + p.Name, p, fn);

            (string Node, string Pin) MapEnd(string nodeId, string pin) =>
                tunnelIn.Contains(nodeId) ? (prefix + "in~" + pin, "out")
                : tunnelOut.Contains(nodeId) ? (prefix + "out~" + pin, "in")
                : (prefix + nodeId, pin);

            foreach (var l in bodyLinks)
            {
                if (ids == null || !ids.Contains(l.SourceNodeId) || !ids.Contains(l.TargetNodeId)) continue;
                var s = MapEnd(l.SourceNodeId, l.SourcePinName);
                var t = MapEnd(l.TargetNodeId, l.TargetPinName);
                links.Add(new Link { SourceNodeId = s.Node, SourcePinName = s.Pin, TargetNodeId = t.Node, TargetPinName = t.Pin });
            }
            if (ids != null)
            {
                foreach (var c in g.ConstantOutputs)
                    if (ids.Contains(c.NodeId) && !tunnelIn.Contains(c.NodeId) && !tunnelOut.Contains(c.NodeId))
                        consts.Add(new ConstantOutput { NodeId = prefix + c.NodeId, Value = c.Value });
                foreach (var pv in g.PinnedValues)
                    if (ids.Contains(pv.NodeId) && !tunnelIn.Contains(pv.NodeId) && !tunnelOut.Contains(pv.NodeId))
                        pinned.Add(Repin(pv, prefix + pv.NodeId, pv.PinName));
            }

            // The instance's own links and typed-in values move onto the tunnel reroutes.
            for (int i = 0; i < links.Count; i++)
            {
                var l = links[i];
                if (l.TargetNodeId == instId)
                    links[i] = new Link { SourceNodeId = l.SourceNodeId, SourcePinName = l.SourcePinName, TargetNodeId = prefix + "in~" + l.TargetPinName, TargetPinName = "in" };
                else if (l.SourceNodeId == instId)
                    links[i] = new Link { SourceNodeId = prefix + "out~" + l.SourcePinName, SourcePinName = "out", TargetNodeId = l.TargetNodeId, TargetPinName = l.TargetPinName };
            }
            for (int i = 0; i < outputs.Count; i++)
                if (outputs[i].NodeId == instId) outputs[i] = (prefix + "out~" + outputs[i].PinName, "out");
            for (int i = 0; i < pinned.Count; i++)
                if (pinned[i].NodeId == instId) pinned[i] = Repin(pinned[i], prefix + "in~" + pinned[i].PinName, "in");
            nodes.Remove(instId);
        }
        return true;
    }

    private static void EliminateReroutes(Dictionary<string, Node> nodes, List<Link> links, List<ConstantOutput> consts,
                                          List<PinnedValue> pinned, List<(string NodeId, string PinName)> outputs)
    {
        var reroutes = new HashSet<string>(nodes.Values.Where(n => Is(n, "reroute")).Select(n => n.Id));
        if (reroutes.Count == 0) return;

        var upstream = new Dictionary<string, Link>();
        foreach (var l in links)
            if (reroutes.Contains(l.TargetNodeId) && !upstream.ContainsKey(l.TargetNodeId)) upstream[l.TargetNodeId] = l;

        // The real source behind a reroute output: null when the chain starts at a reroute with nothing
        // wired in (then `root` is that reroute, which may still carry a typed-in value).
        (string Node, string Pin)? Resolve(string node, string pin, out string root)
        {
            root = node;
            for (int guard = 0; reroutes.Contains(node); ++guard)
            {
                root = node;
                if (guard > reroutes.Count || !upstream.TryGetValue(node, out var u)) return null;
                node = u.SourceNodeId;
                pin = u.SourcePinName;
            }
            return (node, pin);
        }

        var result = new List<Link>(links.Count);
        foreach (var l in links)
        {
            if (reroutes.Contains(l.TargetNodeId)) continue;
            if (!reroutes.Contains(l.SourceNodeId)) { result.Add(l); continue; }
            var src = Resolve(l.SourceNodeId, l.SourcePinName, out string root);
            if (src != null)
            {
                result.Add(new Link { SourceNodeId = src.Value.Node, SourcePinName = src.Value.Pin, TargetNodeId = l.TargetNodeId, TargetPinName = l.TargetPinName });
                continue;
            }
            var pv = pinned.FirstOrDefault(p => p.NodeId == root && p.PinName == "in");
            if (pv != null)
            {
                pinned.RemoveAll(p => p.NodeId == l.TargetNodeId && p.PinName == l.TargetPinName);   // a wire outranks the pin's own value
                pinned.Add(Repin(pv, l.TargetNodeId, l.TargetPinName));
            }
        }
        links.Clear();
        links.AddRange(result);

        for (int i = outputs.Count - 1; i >= 0; i--)
        {
            if (!reroutes.Contains(outputs[i].NodeId)) continue;
            var src = Resolve(outputs[i].NodeId, outputs[i].PinName, out _);
            if (src != null) outputs[i] = src.Value; else outputs.RemoveAt(i);
        }
        pinned.RemoveAll(p => reroutes.Contains(p.NodeId));
        consts.RemoveAll(c => reroutes.Contains(c.NodeId));
        foreach (var id in reroutes) nodes.Remove(id);
    }
}
