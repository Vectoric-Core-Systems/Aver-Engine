// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The registry behind the game-systems node families (timers and events, blackboard, streamed audio,
// animation state machines, game UI, prefabs, decals, crowds/hearing/cover). Each of those families
// ships a static "ForGraph" method per node; this table says which pins feed which parameter, so the
// compiler needs one generic emitter (GraphCompilerGameSystems.cs) instead of a bespoke one per node
// and the parser needs no per-node pin case. The rows live in GraphGameSystemNodeTable.cs.

using System.Globalization;
using System.Reflection;

namespace Aver.Graph;

/// <summary>How a table node takes part in a graph.</summary>
internal enum GenericKind
{
    /// <summary>Runs when the exec walk reaches it; refused by the pure compiler.</summary>
    Exec,
    /// <summary>A read with no side effect: works under either compiler and is recomputed per reader.</summary>
    Pure,
    /// <summary>A bare exec-output entry node (like OnHit); the ENTRY record is what binds it.</summary>
    Trigger,
}

/// <summary>One table node: its pins, the method it calls and how the method's parameters are fed.</summary>
internal sealed class GenericNodeSpec
{
    public required string Type { get; init; }
    public required GenericKind Kind { get; init; }
    public required Type Owner { get; init; }
    public required string MethodName { get; init; }
    public required (string Name, PinType Type, object? Default)[] Inputs { get; init; }
    public required (string Name, PinType Type)[] Outputs { get; init; }

    /// <summary>One token per method parameter, in order: <c>p:pin</c> an input pin, <c>@a|b</c> a NODE-line
    /// attribute (first of the alternatives present; a trailing <c>?</c> makes it optional and empty),
    /// <c>=text</c> a string literal, <c>i:N</c> / <c>f:X</c> / <c>b:0</c> a constant, <c>&gt;pin</c> an
    /// out parameter feeding that output pin (<c>&gt;_</c> discards it).</summary>
    public required string[] Args { get; init; }

    /// <summary>The output pin that receives the method's return value; null discards it.</summary>
    public string? Ret { get; init; }

    /// <summary>The table the row came from (the method name without its Table suffix; the main table is
    /// GameSystem). Set when the tables are collected.</summary>
    internal string Family { get; set; } = "";

    private MethodInfo? _method;

    /// <summary>The resolved method; looked up on first use so a missing one fails only the graph that uses it.</summary>
    public MethodInfo Method => _method ??= Resolve();

    private MethodInfo Resolve()
    {
        var m = Owner.GetMethod(MethodName, BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
        if (m == null)
            throw new InvalidOperationException(
                $"{Type}: {Owner.FullName}.{MethodName} was not found by reflection");
        return m;
    }

    /// <summary>Whether the node has an output pin of this name (a hand-declared PIN set may omit some).</summary>
    public bool HasOutput(string name) => Outputs.Any(o => o.Name == name);
}

internal static partial class GameSystemNodes
{
    private static Dictionary<string, GenericNodeSpec>? s_byType;
    private static List<string> s_duplicates = new();

    private static Dictionary<string, GenericNodeSpec> ByType()
    {
        if (s_byType != null) return s_byType;
        var dups = new List<string>();
        var d = Build(TableMethods(), dups);
        s_duplicates = dups;
        return s_byType = d;
    }

    /// <summary>The family a table method belongs to: its name without the Table suffix, the main table being GameSystem.</summary>
    internal static string FamilyOf(string methodName) =>
        methodName == "Table" ? "GameSystem"
        : methodName.Length > 5 && methodName.EndsWith("Table", StringComparison.Ordinal) ? methodName[..^5]
        : methodName;

    /// <summary>Every table, found by reflection: any static zero-argument method of this class returning
    /// GenericNodeSpec[]. The main table first, the rest by name, so the order does not depend on the compiler.</summary>
    internal static IEnumerable<(string Family, GenericNodeSpec[] Rows)> TableMethods()
    {
        var methods = typeof(GameSystemNodes)
            .GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly)
            .Where(m => m.GetParameters().Length == 0 && m.ReturnType == typeof(GenericNodeSpec[]))
            .OrderBy(m => m.Name == "Table" ? "" : m.Name, StringComparer.Ordinal);
        foreach (var m in methods)
        {
            GenericNodeSpec[] rows;
            try { rows = (GenericNodeSpec[])m.Invoke(null, null)!; }
            catch (TargetInvocationException e) when (e.InnerException != null)
            {
                throw new InvalidOperationException($"node table {m.Name} failed to build: {e.InnerException.Message}", e.InnerException);
            }
            yield return (FamilyOf(m.Name), rows);
        }
    }

    /// <summary>Folds tables into one dictionary by type. The first row of a type wins. A later row with the same
    /// type is benign when it is the same row (an old hand-written Concat line that discovery also found), otherwise
    /// it is recorded in <paramref name="duplicates"/> as "type: defined by A and B".</summary>
    internal static Dictionary<string, GenericNodeSpec> Build(
        IEnumerable<(string Family, GenericNodeSpec[] Rows)> tables, List<string> duplicates)
    {
        var d = new Dictionary<string, GenericNodeSpec>(StringComparer.OrdinalIgnoreCase);
        foreach (var (family, rows) in tables)
            foreach (var s in rows)
            {
                s.Family = family;
                if (d.TryAdd(s.Type, s)) continue;
                var first = d[s.Type];
                if (!SameRow(first, s)) duplicates.Add($"{s.Type}: defined by {first.Family} and {family}");
            }
        return d;
    }

    private static bool SameRow(GenericNodeSpec a, GenericNodeSpec b) =>
        a.Owner == b.Owner && a.MethodName == b.MethodName && a.Kind == b.Kind && a.Ret == b.Ret &&
        a.Args.SequenceEqual(b.Args) && a.Outputs.SequenceEqual(b.Outputs) &&
        a.Inputs.Length == b.Inputs.Length &&
        a.Inputs.Zip(b.Inputs).All(p => p.First.Name == p.Second.Name && p.First.Type == p.Second.Type && Equals(p.First.Default, p.Second.Default));

    /// <summary>Types defined by two different rows (a row repeated unchanged is not listed); Validate() reports them too.</summary>
    internal static IReadOnlyList<string> Duplicates { get { ByType(); return s_duplicates; } }

    /// <summary>Every registered node, for tests.</summary>
    internal static IEnumerable<GenericNodeSpec> All => ByType().Values;

    internal static bool TryGet(string type, out GenericNodeSpec spec) => ByType().TryGetValue(type, out spec!);

    internal static bool IsExec(string type) => TryGet(type, out var s) && s.Kind == GenericKind.Exec;

    internal static bool IsPure(string type) => TryGet(type, out var s) && s.Kind == GenericKind.Pure;

    internal static bool IsTrigger(string type) => TryGet(type, out var s) && s.Kind == GenericKind.Trigger;

    /// <summary>Gives a table node its default pins (and default pin values) when the file declares none.</summary>
    internal static void AddDefaultPins(Node node, Graph graph)
    {
        if (!TryGet(node.Type, out var s)) return;
        void Add(string name, PinType t, bool isOut) =>
            node.Pins.Add(new Pin { Name = name, Type = t, IsOutput = isOut, NodeId = node.Id });

        if (s.Kind == GenericKind.Trigger) { Add("exec", PinType.Exec, true); return; }
        if (s.Kind == GenericKind.Exec) Add("exec", PinType.Exec, false);
        foreach (var (name, type, dflt) in s.Inputs)
        {
            Add(name, type, false);
            if (dflt != null) graph.PinnedValues.Add(new PinnedValue { NodeId = node.Id, PinName = name, Value = dflt });
        }
        if (s.Kind == GenericKind.Exec) Add("then", PinType.Exec, true);
        foreach (var (name, type) in s.Outputs) Add(name, type, true);
    }

    // ---- table construction ------------------------------------------------------------------------

    private static PinType ParseType(string t) => t switch
    {
        "float" => PinType.Float,
        "int" => PinType.Int,
        "bool" => PinType.Bool,
        _ => throw new ArgumentException("pin type " + t),
    };

    private static object ParseDefault(PinType t, string text) => t switch
    {
        PinType.Float => float.Parse(text, CultureInfo.InvariantCulture),
        PinType.Int => int.Parse(text, CultureInfo.InvariantCulture),
        _ => bool.Parse(text),
    };

    /// <summary>One row. <paramref name="ins"/> is <c>name:type[=default],...</c>, <paramref name="outs"/> is
    /// <c>name:type,...</c>.</summary>
    private static GenericNodeSpec Reg(string type, GenericKind kind, Type owner, string method, string ins,
                                       string outs, string args, string? ret)
    {
        var inputs = new List<(string, PinType, object?)>();
        foreach (var part in ins.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = part.Split('=', 2);
            var nt = eq[0].Split(':');
            var pt = ParseType(nt[1]);
            inputs.Add((nt[0], pt, eq.Length > 1 ? ParseDefault(pt, eq[1]) : null));
        }
        var outputs = new List<(string, PinType)>();
        foreach (var part in outs.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var nt = part.Split(':');
            outputs.Add((nt[0], ParseType(nt[1])));
        }
        return new GenericNodeSpec
        {
            Type = type, Kind = kind, Owner = owner, MethodName = method,
            Inputs = inputs.ToArray(), Outputs = outputs.ToArray(),
            Args = args.Split(',', StringSplitOptions.RemoveEmptyEntries), Ret = ret,
        };
    }

    /// <summary>Checks every row against the real method it names: parameter count, parameter types
    /// against the pins that feed them, the return value against its pin, and that no type is defined twice. Returns one line per problem.</summary>
    internal static List<string> Validate()
    {
        var problems = new List<string>(Duplicates);
        foreach (var s in All)
        {
            if (s.Kind == GenericKind.Trigger) continue;
            MethodInfo m;
            try { m = s.Method; }
            catch (Exception e) { problems.Add(e.Message); continue; }
            var ps = m.GetParameters();
            if (ps.Length != s.Args.Length)
            {
                problems.Add($"{s.Type}: {s.MethodName} takes {ps.Length} parameters, the row feeds {s.Args.Length}");
                continue;
            }
            for (int i = 0; i < ps.Length; i++)
            {
                string a = s.Args[i];
                Type pt = ps[i].ParameterType;
                if (a.StartsWith('>'))
                {
                    if (!ps[i].IsOut) problems.Add($"{s.Type}: parameter {i} ({ps[i].Name}) is not an out parameter");
                    string pin = a[1..];
                    if (pin == "_") continue;
                    var o = s.Outputs.FirstOrDefault(x => x.Name == pin);
                    if (o.Name == null) problems.Add($"{s.Type}: out parameter {ps[i].Name} names missing output pin '{pin}'");
                    else if (!Matches(pt.GetElementType()!, o.Type)) problems.Add($"{s.Type}: out {ps[i].Name} is {pt.GetElementType()!.Name}, pin '{pin}' is {o.Type}");
                }
                else if (a.StartsWith("p:"))
                {
                    var inp = s.Inputs.FirstOrDefault(x => x.Name == a[2..]);
                    if (inp.Name == null) problems.Add($"{s.Type}: parameter {ps[i].Name} names missing input pin '{a[2..]}'");
                    else if (!Matches(pt, inp.Type)) problems.Add($"{s.Type}: parameter {ps[i].Name} is {pt.Name}, pin '{a[2..]}' is {inp.Type}");
                }
                else if (a.StartsWith('@') || a.StartsWith('='))
                {
                    if (pt != typeof(string)) problems.Add($"{s.Type}: parameter {ps[i].Name} is {pt.Name}, a string was supplied");
                }
                else if (a.StartsWith("i:")) { if (pt != typeof(int)) problems.Add($"{s.Type}: parameter {ps[i].Name} is not int"); }
                else if (a.StartsWith("f:")) { if (pt != typeof(float)) problems.Add($"{s.Type}: parameter {ps[i].Name} is not float"); }
                else if (a.StartsWith("b:")) { if (pt != typeof(bool)) problems.Add($"{s.Type}: parameter {ps[i].Name} is not bool"); }
                else problems.Add($"{s.Type}: argument token '{a}' is not understood");
            }
            if (s.Ret != null)
            {
                var o = s.Outputs.FirstOrDefault(x => x.Name == s.Ret);
                if (o.Name == null) problems.Add($"{s.Type}: return pin '{s.Ret}' is not an output");
                else if (m.ReturnType == typeof(void)) problems.Add($"{s.Type}: {s.MethodName} returns void but pin '{s.Ret}' expects its value");
                else if (!Matches(m.ReturnType, o.Type)) problems.Add($"{s.Type}: returns {m.ReturnType.Name}, pin '{s.Ret}' is {o.Type}");
            }
            foreach (var inp in s.Inputs)
                if (!s.Args.Contains("p:" + inp.Name)) problems.Add($"{s.Type}: input pin '{inp.Name}' feeds no parameter");
        }
        return problems;
    }

    private static bool Matches(Type clr, PinType pin) => pin switch
    {
        PinType.Float => clr == typeof(float),
        PinType.Int => clr == typeof(int),
        PinType.Bool => clr == typeof(bool),
        _ => false,
    };
}
