// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Everything wrong with a graph, not just the first thing: what the editor's Problems panel lists
// (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md, E1). Run() parses without Graph.Validate's early
// exit, collects Graph.Diagnose's errors, adds the checks only a list can afford (a missing required
// attribute, an exec node nothing reaches, a variable nobody reads), then compiles every entry to turn
// the compiler's own refusals into entries too.

using System.Text;

namespace Aver.Graph;

public enum DiagnosticSeverity
{
    Error,
    Warning,
}

/// <summary>One finding: how bad, which node and pin (when it has one), a stable code and a sentence.</summary>
public sealed class GraphDiagnostic
{
    public DiagnosticSeverity Severity { get; }
    public string Code { get; }
    public string Message { get; }
    public string? NodeId { get; }
    public string? Pin { get; }

    public GraphDiagnostic(DiagnosticSeverity severity, string code, string message, string? nodeId = null, string? pin = null)
    {
        Severity = severity;
        Code = code;
        Message = message;
        NodeId = nodeId;
        Pin = pin;
    }
}

public static class GraphDiagnostics
{
    /// <summary>All diagnostics for .ocgraph text, in detection order. A text that does not parse is one
    /// error with code "parse". Never throws.</summary>
    public static List<GraphDiagnostic> Run(string ocgraphText)
    {
        var list = new List<GraphDiagnostic>();
        try
        {
            if (!OcGraphParser.Parse(ocgraphText, out var graph, out var err, validate: false))
            {
                list.Add(new GraphDiagnostic(DiagnosticSeverity.Error, "parse", err ?? "the graph could not be parsed"));
                return list;
            }

            // A material graph is another language; none of the gameplay checks mean anything to it.
            if (graph.DomainKind != GraphDomain.Gameplay) return list;

            graph.Diagnose(list);
            bool validates = list.Count == 0;
            AttributeChecks(graph, list);
            ReachabilityChecks(graph, list);
            VariableChecks(graph, list);

            // Compiling only when the graph validates: CompileEntryPoint starts with Validate(), so on a
            // graph that does not, it would only repeat the first error already listed above.
            if (validates) CompileChecks(graph, list);
        }
        catch (Exception ex)
        {
            list.Add(new GraphDiagnostic(DiagnosticSeverity.Error, "internal", $"diagnostics threw: {ex.GetType().Name}: {ex.Message}"));
        }
        return list;
    }

    /// <summary>The wire form the bridge hands the editor: one line per diagnostic,
    /// <c>E|W TAB nodeId TAB pin TAB code TAB message</c>, cut at a whole line when it would not fit
    /// <paramref name="maxBytes"/> and ended by <c>...N more</c> in that case.</summary>
    public static string ToWire(IReadOnlyList<GraphDiagnostic> items, int maxBytes)
    {
        var sb = new StringBuilder();
        int used = 0;
        for (int i = 0; i < items.Count; i++)
        {
            string line = Line(items[i]);
            int bytes = Encoding.UTF8.GetByteCount(line) + 1;
            // Leaves room for the "...N more" line, which is short but not free.
            if (used + bytes > maxBytes - 24)
            {
                sb.Append("...").Append(items.Count - i).Append(" more\n");
                return sb.ToString();
            }
            sb.Append(line).Append('\n');
            used += bytes;
        }
        return sb.ToString();
    }

    private static string Line(GraphDiagnostic d) =>
        string.Join('\t', d.Severity == DiagnosticSeverity.Error ? "E" : "W", Clean(d.NodeId), Clean(d.Pin), Clean(d.Code), Clean(d.Message));

    private static string Clean(string? s) => s == null ? "" : s.Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');

    // ---- the checks only a list can afford ---------------------------------------------------------

    /// <summary>A table node whose required NODE-line attribute is missing: the compiler would throw the
    /// same at the moment the exec walk reaches the node, which is the wrong time to find out.</summary>
    private static void AttributeChecks(Graph g, List<GraphDiagnostic> into)
    {
        foreach (var node in g.Nodes.Values)
        {
            if (!GameSystemNodes.TryGet(node.Type, out var spec) || spec.Kind == GenericKind.Trigger) continue;
            foreach (string arg in spec.Args)
            {
                if (!arg.StartsWith('@')) continue;
                string a = arg[1..];
                if (a.EndsWith('?')) continue;
                string[] keys = a.Split('|');
                bool have = keys.Any(k => node.Attrs.TryGetValue(k, out var v) && !string.IsNullOrEmpty(v));
                if (have) continue;
                into.Add(new GraphDiagnostic(DiagnosticSeverity.Error, "attr.missing",
                    $"{node.Type} node '{node.Id}' has no {string.Join("= / ", keys)}= attribute", node.Id));
            }
        }
    }

    /// <summary>An exec node (anything with an exec pin) that no ENTRY, FuncEntry or exec wire leads to,
    /// so it can never run. A warning: a half-built graph is a normal state.</summary>
    private static void ReachabilityChecks(Graph g, List<GraphDiagnostic> into)
    {
        var reached = new HashSet<string>();
        var queue = new Queue<string>();
        void Seed(string id) { if (g.Nodes.ContainsKey(id) && reached.Add(id)) queue.Enqueue(id); }

        foreach (var (nodeId, _) in g.EntryPoints) Seed(nodeId);
        foreach (var n in g.Nodes.Values)
            if (n.Type.Equals("funcentry", StringComparison.OrdinalIgnoreCase)) Seed(n.Id);

        var execOut = new Dictionary<string, List<string>>();
        foreach (var l in g.Links)
        {
            if (!g.Nodes.TryGetValue(l.SourceNodeId, out var sn)) continue;
            var pin = sn.Pins.FirstOrDefault(p => p.IsOutput && p.Name == l.SourcePinName);
            if (pin == null || pin.Type != PinType.Exec) continue;
            if (!execOut.TryGetValue(l.SourceNodeId, out var targets)) execOut[l.SourceNodeId] = targets = new List<string>();
            targets.Add(l.TargetNodeId);
        }
        while (queue.Count > 0)
        {
            string id = queue.Dequeue();
            if (execOut.TryGetValue(id, out var targets))
                foreach (var t in targets) Seed(t);
        }

        foreach (var n in g.Nodes.Values)
        {
            if (reached.Contains(n.Id)) continue;
            if (!n.Pins.Any(p => p.Type == PinType.Exec)) continue;
            bool trigger = !n.Pins.Any(p => p.Type == PinType.Exec && !p.IsOutput);
            into.Add(new GraphDiagnostic(DiagnosticSeverity.Warning, "exec.unreachable",
                trigger
                    ? $"Node '{n.Id}' ({n.Type}) starts a chain but no ENTRY record names it, so it never fires"
                    : $"Node '{n.Id}' ({n.Type}) is not reached by any event or function, so it never runs",
                n.Id));
        }
    }

    /// <summary>A VAR nobody touches, or one that is written and never read.</summary>
    private static void VariableChecks(Graph g, List<GraphDiagnostic> into)
    {
        foreach (var v in g.Variables)
        {
            bool read = false, written = false;
            foreach (var n in g.Nodes.Values)
            {
                if (!string.Equals(n.VarName, v.Name, StringComparison.Ordinal)) continue;
                if (n.Type.Equals("getvar", StringComparison.OrdinalIgnoreCase)) read = true;
                else if (n.Type.Equals("setvar", StringComparison.OrdinalIgnoreCase)) written = true;
            }
            if (!read && !written)
                into.Add(new GraphDiagnostic(DiagnosticSeverity.Warning, "var.unused", $"Variable '{v.Name}' is declared and never used"));
            else if (written && !read)
                into.Add(new GraphDiagnostic(DiagnosticSeverity.Warning, "var.writeonly", $"Variable '{v.Name}' is written and never read"));
        }
    }

    /// <summary>Compiles every ENTRY (which compiles every function first), or the pull graph when there
    /// is none, and lists what the compiler refused. Native failures are the environment, not the graph.</summary>
    private static void CompileChecks(Graph g, List<GraphDiagnostic> into)
    {
        var seen = new HashSet<string>();
        void Note(string? message)
        {
            if (string.IsNullOrEmpty(message) || IsEnvironmentFailure(message)) return;
            if (!seen.Add(message)) return;
            string? node = QuotedNodeId(message, g);
            // The attribute check already said it, better.
            if (node != null && into.Any(d => d.NodeId == node && d.Severity == DiagnosticSeverity.Error)) return;
            into.Add(new GraphDiagnostic(DiagnosticSeverity.Error, "compile", message, node));
        }

        var events = g.EntryPoints.Select(e => e.EventName).Distinct().ToList();
        foreach (string ev in events)
        {
            try
            {
                new GraphCompiler(g).CompileEntryPoint(ev, out var err);
                Note(err);
            }
            catch (Exception ex) { Note(ex.Message); }
        }
        if (events.Count == 0 && g.Functions.Count > 0)
        {
            try
            {
                new GraphCompiler(g).Compile(out var err);
                Note(err);
            }
            catch (Exception ex) { Note(ex.Message); }
        }
    }

    private static bool IsEnvironmentFailure(string message) =>
        message.Contains("Unable to load DLL", StringComparison.Ordinal) ||
        message.Contains("Unable to find an entry point", StringComparison.Ordinal) ||
        message.Contains("DllNotFoundException", StringComparison.Ordinal);

    /// <summary>The first single-quoted token that names a real node, or null: the compiler's sentences
    /// quote the node first, and a pin, event or parameter name must not badge an innocent node.</summary>
    private static string? QuotedNodeId(string message, Graph g)
    {
        int i = 0;
        while (true)
        {
            int a = message.IndexOf('\'', i);
            if (a < 0) return null;
            int b = message.IndexOf('\'', a + 1);
            if (b < 0) return null;
            string token = message.Substring(a + 1, b - a - 1);
            if (g.Nodes.ContainsKey(token)) return token;
            i = b + 1;
        }
    }
}
