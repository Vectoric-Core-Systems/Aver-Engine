// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The graph debugger's managed half (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md, E4).
//
// ARMED IS READ AT COMPILE TIME. GraphCompiler asks GraphDebug.Armed once per node it emits; with it
// off the IL is byte-for-byte what it was before this file existed, so a packaged game and an
// undebugged editor pay nothing -- not even a static bool test per node. Arming applies to graphs
// compiled afterwards, which is why the editor says "takes effect on the next Play".
//
// A BREAKPOINT DOES NOT STOP THE TICK. NodeReached only latches the hit; the host reads it back once
// per frame (TakeBreak) and pauses the session at the end of that frame, because the engine loop is
// single-threaded and the editor draws on the same thread -- there is no second thread to sit on
// while the author looks at the graph. Frame Skip is the step.

using System.Globalization;
using System.Text;

namespace Aver.Graph;

public static class GraphDebug
{
    /// <summary>The key a breakpoint travels under: graph name and node id around one unit separator.</summary>
    public const char KeySeparator = '\x1f';

    private static volatile bool s_armed;
    private static readonly object s_lock = new();

    // graph -> node ids, replaced wholesale so the hot path reads one reference and takes no lock.
    private static volatile Dictionary<string, HashSet<string>> s_breakpoints = new(StringComparer.Ordinal);
    private static string? s_pending;

    // graph -> "node\x1fpin" -> last value (floats, ints and bools widened to double).
    private static readonly Dictionary<string, Dictionary<string, double>> s_values = new(StringComparer.Ordinal);

    /// <summary>Whether graphs compiled from now on carry the debugger's hooks.</summary>
    public static bool Armed
    {
        get => s_armed;
        set
        {
            s_armed = value;
            if (!value) Reset();
        }
    }

    /// <summary>Replaces the breakpoint set from lines of <c>graph\x1fnode</c>. Blank and malformed lines are skipped.</summary>
    public static void SetBreakpoints(string? text)
    {
        var next = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        if (!string.IsNullOrEmpty(text))
        {
            foreach (string raw in text.Split('\n'))
            {
                string line = raw.TrimEnd('\r');
                int at = line.IndexOf(KeySeparator);
                if (at <= 0 || at == line.Length - 1) continue;
                string graph = line.Substring(0, at), node = line.Substring(at + 1);
                if (!next.TryGetValue(graph, out var set)) next[graph] = set = new HashSet<string>(StringComparer.Ordinal);
                set.Add(node);
            }
        }
        s_breakpoints = next;
    }

    /// <summary>Called by an armed graph at the top of every exec node. Latches the first break until it is taken.</summary>
    public static void NodeReached(string graph, string node)
    {
        var bps = s_breakpoints;
        if (bps.Count == 0) return;
        if (!bps.TryGetValue(graph, out var set) || !set.Contains(node)) return;
        lock (s_lock) s_pending ??= graph + KeySeparator + node;
    }

    /// <summary>The latched break as <c>graph\x1fnode</c>, once; empty afterwards until another node trips one.</summary>
    public static string TakeBreak()
    {
        lock (s_lock)
        {
            string r = s_pending ?? "";
            s_pending = null;
            return r;
        }
    }

    /// <summary>Called by an armed graph after it pulls a value into a node's input pin.</summary>
    public static void RecordPin(string graph, string node, string pin, double value)
    {
        lock (s_lock)
        {
            if (!s_values.TryGetValue(graph, out var pins)) s_values[graph] = pins = new Dictionary<string, double>(StringComparer.Ordinal);
            pins[node + KeySeparator + pin] = value;
        }
    }

    /// <summary>The last value seen on each input pin of <paramref name="graph"/>, as <c>node.pin=value;</c> repeated.</summary>
    public static string GetValues(string graph)
    {
        var sb = new StringBuilder();
        lock (s_lock)
        {
            if (!s_values.TryGetValue(graph, out var pins)) return "";
            foreach (var kv in pins)
            {
                int at = kv.Key.IndexOf(KeySeparator);
                sb.Append(kv.Key, 0, at).Append('.').Append(kv.Key, at + 1, kv.Key.Length - at - 1)
                  .Append('=').Append(kv.Value.ToString("R", CultureInfo.InvariantCulture)).Append(';');
            }
        }
        return sb.ToString();
    }

    /// <summary>Forgets latched breaks and recorded values (kept: the breakpoint set, which is the author's).</summary>
    public static void Reset()
    {
        lock (s_lock)
        {
            s_pending = null;
            s_values.Clear();
        }
    }
}
