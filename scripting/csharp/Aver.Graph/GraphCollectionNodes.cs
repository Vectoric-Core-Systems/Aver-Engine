// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The array and struct nodes. Their pins depend on an attribute (`of=` the element type, `struct=` and `field=` a
// declared STRUCT), so they are resolved per node rather than listed in GraphDataNodes' fixed table.
// Palette mirror: sandbox/src/GraphNodeDefsData.hpp. Emission: GraphCompiler.Collections.cs.

using System;
using System.Collections.Generic;
using System.Linq;

namespace Aver.Graph;

internal static class CollectionNodes
{
    /// <summary>Struct field names that would collide with a struct node's own pins.</summary>
    internal static readonly string[] ReservedFieldNames = { "value", "result" };

    private static readonly Dictionary<string, DataNodeKind> s_kinds = new(StringComparer.OrdinalIgnoreCase)
    {
        ["MakeArray"] = DataNodeKind.Pure,
        ["ArrayLength"] = DataNodeKind.Pure,
        ["ArrayGet"] = DataNodeKind.Pure,
        ["ArrayContains"] = DataNodeKind.Pure,
        ["ArrayFind"] = DataNodeKind.Pure,
        ["ArrayAdd"] = DataNodeKind.Exec,
        ["ArraySet"] = DataNodeKind.Exec,
        ["ArrayRemoveAt"] = DataNodeKind.Exec,
        ["ArrayClear"] = DataNodeKind.Exec,
        ["ForEachInArray"] = DataNodeKind.Flow,
        ["MakeStruct"] = DataNodeKind.Pure,
        ["BreakStruct"] = DataNodeKind.Pure,
        ["SetStructField"] = DataNodeKind.Pure,
    };

    internal static DataNodeKind? KindOf(string type) => s_kinds.TryGetValue(type, out var k) ? k : null;

    internal static bool IsStructNode(string type) =>
        type.Equals("MakeStruct", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("BreakStruct", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("SetStructField", StringComparison.OrdinalIgnoreCase);

    /// <summary>The element type from `of=` (float when absent); false when it names no array element type.</summary>
    internal static bool TryElement(Node node, out PinType elem)
    {
        elem = PinType.Float;
        if (!node.Attrs.TryGetValue("of", out var of) || of.Length == 0) return true;
        return PinTypeRules.TryParse(of, out elem, out _) && PinTypeRules.IsValueScalar(elem);
    }

    internal static GraphStructDef? StructOf(Node node, Graph graph) =>
        node.Attrs.TryGetValue("struct", out var name) ? graph.Structs.FirstOrDefault(s => s.Name == name) : null;

    /// <summary>The pins a collection node gets, or false when its attributes do not resolve (Validate says why).</summary>
    internal static bool TryResolve(Node node, Graph graph, out DataNodeSpec spec)
    {
        spec = null!;
        var kind = KindOf(node.Type);
        if (kind == null) return false;
        string type = node.Type.ToLowerInvariant();
        DataPin[] ins, outs;

        if (IsStructNode(type))
        {
            var def = StructOf(node, graph);
            if (def == null) return false;
            var st = new DataPin("value", PinType.Struct, null, def.Name);
            var fields = def.Fields.Select(f => new DataPin(f.Name, f.Type)).ToArray();
            switch (type)
            {
                case "makestruct": ins = fields; outs = new[] { st }; break;
                case "breakstruct": ins = new[] { st }; outs = fields; break;
                default:
                {
                    node.Attrs.TryGetValue("field", out var field);
                    var f = def.Fields.FirstOrDefault(x => x.Name == field);
                    if (f == null) return false;
                    ins = new[] { st, new DataPin(f.Name, f.Type) };
                    outs = new[] { new DataPin("result", PinType.Struct, null, def.Name) };
                    break;
                }
            }
        }
        else
        {
            if (!TryElement(node, out var elem) || !PinTypeRules.TryArrayOf(elem, out var arr)) return false;
            var array = new DataPin("array", arr);
            var item = new DataPin("item", elem);
            var index = new DataPin("index", PinType.Int);
            switch (type)
            {
                case "makearray":
                    ins = Enumerable.Range(0, 4).Select(i => new DataPin("item" + i, elem)).ToArray();
                    outs = new[] { new DataPin("array", arr) };
                    break;
                case "arraylength": ins = new[] { array }; outs = new[] { new DataPin("length", PinType.Int) }; break;
                case "arrayget": ins = new[] { array, index }; outs = new[] { new DataPin("item", elem) }; break;
                case "arraycontains": ins = new[] { array, item }; outs = new[] { new DataPin("result", PinType.Bool) }; break;
                case "arrayfind": ins = new[] { array, item }; outs = new[] { new DataPin("index", PinType.Int) }; break;
                case "arrayadd": ins = new[] { item }; outs = Array.Empty<DataPin>(); break;
                case "arrayset": ins = new[] { index, item }; outs = Array.Empty<DataPin>(); break;
                case "arrayremoveat": ins = new[] { index }; outs = Array.Empty<DataPin>(); break;
                case "arrayclear": ins = Array.Empty<DataPin>(); outs = Array.Empty<DataPin>(); break;
                default: // foreachinarray
                    ins = new[] { array };
                    outs = new[] { new DataPin("loop", PinType.Exec), new DataPin("element", elem), index, new DataPin("completed", PinType.Exec) };
                    break;
            }
        }
        spec = new DataNodeSpec { Type = node.Type, Kind = kind.Value, Inputs = ins, Outputs = outs };
        return true;
    }

    /// <summary>The graph checks for STRUCT declarations, struct and array pins, and the collection nodes' attributes.</summary>
    internal static bool Check(Graph g, Func<string, string, string?, string?, bool> report)
    {
        var seen = new HashSet<string>();
        foreach (var s in g.Structs)
        {
            if (!seen.Add(s.Name) && report("struct.duplicate", $"Duplicate STRUCT declaration '{s.Name}'", null, null)) return true;
            var fields = new HashSet<string>();
            foreach (var f in s.Fields)
            {
                if (!fields.Add(f.Name) && report("struct.field.duplicate", $"STRUCT '{s.Name}' declares field '{f.Name}' twice", null, null)) return true;
                if (!PinTypeRules.IsValueScalar(f.Type) &&
                    report("struct.field.type", $"STRUCT '{s.Name}' field '{f.Name}' is {f.Type}; a field is float, int, bool, float3, string or entity", null, null)) return true;
                if (Array.IndexOf(ReservedFieldNames, f.Name) >= 0 &&
                    report("struct.field.name", $"STRUCT '{s.Name}' field '{f.Name}' is a reserved name (it would collide with a struct node's own pin)", null, null)) return true;
            }
        }

        bool Known(string? name) => g.Structs.Any(s => s.Name == name);
        foreach (var v in g.Variables)
            if (v.Type == PinType.Struct && !Known(v.StructName) &&
                report("struct.unknown", $"VAR '{v.Name}' names struct '{v.StructName}', which no STRUCT record declares", null, null)) return true;
        foreach (var f in g.Functions)
            foreach (var p in f.Inputs.Concat(f.Outputs))
                if (p.Type == PinType.Struct && !Known(p.StructName) &&
                    report("struct.unknown", $"function '{f.Name}' pin '{p.Name}' names struct '{p.StructName}', which no STRUCT record declares", null, null)) return true;

        foreach (var node in g.Nodes.Values)
        {
            foreach (var pin in node.Pins)
                if (pin.Type == PinType.Struct && !Known(pin.StructName) &&
                    report("struct.unknown", $"Node '{node.Id}' pin '{pin.Name}' names struct '{pin.StructName}', which no STRUCT record declares", node.Id, pin.Name)) return true;

            var kind = KindOf(node.Type);
            if (kind == null) continue;
            if (IsStructNode(node.Type))
            {
                if (!node.Attrs.TryGetValue("struct", out var sn) || sn.Length == 0)
                {
                    if (report("struct.name", $"Node '{node.Id}' is a {node.Type} but has no struct= attribute naming the struct", node.Id, null)) return true;
                    continue;
                }
                var def = g.Structs.FirstOrDefault(s => s.Name == sn);
                if (def == null)
                {
                    if (report("struct.unknown", $"Node '{node.Id}' names struct '{sn}', which no STRUCT record declares", node.Id, null)) return true;
                    continue;
                }
                if (node.Type.Equals("SetStructField", StringComparison.OrdinalIgnoreCase) &&
                    !(node.Attrs.TryGetValue("field", out var fn) && def.Fields.Any(f => f.Name == fn)) &&
                    report("struct.field", $"Node '{node.Id}' is a SetStructField but its field= does not name a field of struct '{sn}'", node.Id, null)) return true;
                continue;
            }

            if (!TryElement(node, out var elem))
            {
                if (report("array.of", $"Node '{node.Id}' has of={node.Attrs["of"]}, which is not an array element type (float, int, bool, float3, string or entity)", node.Id, null)) return true;
                continue;
            }
            if (kind != DataNodeKind.Exec) continue;

            string kindName = node.Type;
            if (string.IsNullOrEmpty(node.VarName))
            {
                if (report("array.var", $"Node '{node.Id}' is an {kindName} but has no var= attribute naming the array variable", node.Id, null)) return true;
                continue;
            }
            var declared = g.Variables.FirstOrDefault(v => v.Name == node.VarName);
            if (declared == null)
            {
                if (report("var.undeclared", $"Node '{node.Id}' references undeclared variable '{node.VarName}' -- add 'VAR {node.VarName} {PinTypeRules.Format(PinType.Float)}[]'", node.Id, null)) return true;
                continue;
            }
            if (!PinTypeRules.TryArrayOf(elem, out var want) || declared.Type != want)
                if (report("array.var", $"Node '{node.Id}' is an {kindName} of {PinTypeRules.Format(elem)} but variable '{node.VarName}' is {PinTypeRules.Format(declared.Type, declared.StructName)}", node.Id, null)) return true;
        }
        return false;
    }
}
