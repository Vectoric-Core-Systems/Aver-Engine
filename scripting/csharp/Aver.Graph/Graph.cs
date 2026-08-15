// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Minimal in-memory graph model: nodes, pins, links, and pinned values.
// Comment explains WHY: a graph is a DAG of computation nodes. Each node has inputs and outputs.
// Links connect pins. Pinned values are constants clamped to a node's input pin. The model carries
// enough to evaluate the graph once compiled to IL.

using System.Collections.Generic;
using System.Linq;

namespace Aver.Graph;

/// The pin types the compiler supports. Expanded to cover more types on later slices.
public enum PinType
{
    Float,
    Int,
    Bool,

    // EXEC -- a control-flow pin, not a data pin. Written and parsed exactly like any other typed pin
    // (`PIN node name in exec` / `PIN node name out exec`) -- see
    // modules/formats/include/aver/formats/OcGraph.hpp's comment on OcGraphLink for the format-level
    // reasoning. A LINK between two exec pins IS an exec link; there is no separate record for it, and
    // no format change was needed to add it, because Validate()'s existing type-equality check
    // (srcPin.Type != tgtPin.Type, below) already refuses to connect an exec pin to anything but
    // another exec pin, for free, the moment this enum value exists. See GraphCompiler's own PUSH VS
    // PULL comment for what an exec pin means to the COMPILER, as distinct from what it means here
    // (just one more pin type a link can agree or disagree about).
    Exec,
}

/// A single data input or output on a node.
public class Pin
{
    public required string Name { get; init; }
    public required PinType Type { get; init; }
    public required bool IsOutput { get; init; }
    public required string NodeId { get; init; }  // String to support both int and string IDs from different formats.
}

/// A constant output value for a Const node (provided during node creation).
public class ConstantOutput
{
    public required string NodeId { get; init; }  // String to support both int and string IDs from different formats.
    public required object Value { get; init; }  // float, int, or bool
}

/// A pinned value (constant) clamped to an input pin.
public class PinnedValue
{
    public required string NodeId { get; init; }  // String to support both int and string IDs from different formats.
    public required string PinName { get; init; }
    public required object Value { get; init; }  // float, int, or bool
}

/// A link connecting an output pin to an input pin.
public class Link
{
    public required string SourceNodeId { get; init; }  // String to support both int and string IDs from different formats.
    public required string SourcePinName { get; init; }
    public required string TargetNodeId { get; init; }  // String to support both int and string IDs from different formats.
    public required string TargetPinName { get; init; }
}

/// A computation node in the graph.
public class Node
{
    public required string Id { get; init; }  // String to support both int and string IDs from different formats.
    public required string Type { get; init; }  // "Const", "Add", "Multiply", etc.
    public List<Pin> Pins { get; init; } = new();

    // Which graph PARAMETER a "param" node reads, e.g. "entity" or "time". Set from the NODE
    // line's "param=<name>" attribute. Null for every other node type.
    public string? ParamName { get; set; }

    // Which scene field a "getfield"/"setfield" node addresses, e.g. "CLocal.position". Set from
    // the NODE line's "field=<qualifiedName>" attribute. Null for every other node type. The name
    // is resolved to a dense field id at COMPILE time (GraphCompiler), not here -- resolution needs
    // the live scene's field table, which the format layer has no access to.
    public string? FieldName { get; set; }

    // Which registered class a "spawn" node creates an instance of, e.g. "Widget". Set from the NODE
    // line's "class=<name>" attribute -- the same generic key=value mechanism FieldName/ParamName
    // already use. Null for every other node type. UNLIKE FieldName, this is deliberately NOT resolved
    // to a handle here or at compile time -- see GraphInterop.SpawnForGraph's own comment for why a
    // project's actor classes (declared by that project's own Scripts.dll, at a point in the host's
    // boot order that field ids' engine-global table never has to worry about) are resolved by NAME at
    // invocation time instead.
    public string? ClassName { get; set; }
}

/// One parameter the compiled method accepts -- e.g. the entity a graph drives, or the current
/// time. Declared with a top-level `PARAM <name> <type>` record; order is declaration order and
/// becomes the compiled method's argument order. A node reads one by naming it in a "param" node's
/// `param=<name>` attribute (see Node.ParamName) rather than the format inventing a second way to
/// wire data into a node beyond LINK/PINVAL that every other node type already uses.
public class GraphParameter
{
    public required string Name { get; init; }
    public required PinType Type { get; init; }
}

/// A complete graph: nodes, links, pinned values, and output pins to evaluate.
public class Graph
{
    public string Name { get; set; } = "untitled";
    public string Description { get; set; } = "";
    public Dictionary<string, Node> Nodes { get; set; } = new();  // String keys to support both int and string IDs from different formats.
    public List<Link> Links { get; set; } = new();
    public List<ConstantOutput> ConstantOutputs { get; set; } = new();
    public List<PinnedValue> PinnedValues { get; set; } = new();
    public List<(string NodeId, string PinName)> Outputs { get; set; } = new();
    public List<GraphParameter> Parameters { get; set; } = new();  // Declared via top-level PARAM records; empty means the compiled method takes no arguments, exactly as before this existed.

    // Declared via top-level `ENTRY <nodeId> <eventName>` records -- which node begins the PUSH/exec
    // chain for a named event (e.g. "OnStart", "OnTick"). Empty for every graph that predates this,
    // exactly like Parameters was empty before PARAM existed -- see
    // modules/formats/include/aver/formats/OcGraph.hpp's OcGraphData::entryPoints comment for the full
    // backward-compatibility argument, which applies here unchanged.
    public List<(string NodeId, string EventName)> EntryPoints { get; set; } = new();

    /// Validates the graph for consistency. Returns false if invalid; sets err to a message.
    /// Note: Comparison is case-sensitive for node IDs. If nodes are added as "1" and referenced as "1",
    /// they must match exactly. The C# parser uses string representations of integer IDs, and the C++ writer
    /// uses string node IDs; both must be normalized consistently.
    public bool Validate(out string? err)
    {
        err = null;

        // Check all links reference valid nodes and pins.
        foreach (var link in Links)
        {
            if (!Nodes.TryGetValue(link.SourceNodeId, out var srcNode))
            {
                err = $"Link references unknown source node {link.SourceNodeId}";
                return false;
            }
            if (!Nodes.TryGetValue(link.TargetNodeId, out var tgtNode))
            {
                err = $"Link references unknown target node {link.TargetNodeId}";
                return false;
            }

            var srcPin = srcNode.Pins.FirstOrDefault(p => p.Name == link.SourcePinName && p.IsOutput);
            if (srcPin == null)
            {
                err = $"Source node {link.SourceNodeId} has no output pin '{link.SourcePinName}'";
                return false;
            }

            var tgtPin = tgtNode.Pins.FirstOrDefault(p => p.Name == link.TargetPinName && !p.IsOutput);
            if (tgtPin == null)
            {
                err = $"Target node {link.TargetNodeId} has no input pin '{link.TargetPinName}'";
                return false;
            }

            // Pin types must match.
            if (srcPin.Type != tgtPin.Type)
            {
                err = $"Link {link.SourceNodeId}.{link.SourcePinName} ({srcPin.Type}) to {link.TargetNodeId}.{link.TargetPinName} ({tgtPin.Type}): type mismatch";
                return false;
            }
        }

        // Check all pinned values reference valid nodes and input pins.
        foreach (var pv in PinnedValues)
        {
            if (!Nodes.TryGetValue(pv.NodeId, out var node))
            {
                err = $"Pinned value references unknown node {pv.NodeId}";
                return false;
            }

            var pin = node.Pins.FirstOrDefault(p => p.Name == pv.PinName && !p.IsOutput);
            if (pin == null)
            {
                err = $"Node {pv.NodeId} has no input pin '{pv.PinName}'";
                return false;
            }
        }

        // Check PARAM declarations are unique -- two parameters with the same name would make
        // "which argument does this node read" ambiguous.
        var paramNames = new HashSet<string>();
        foreach (var p in Parameters)
        {
            if (!paramNames.Add(p.Name))
            {
                err = $"Duplicate PARAM declaration '{p.Name}'";
                return false;
            }
        }

        // Every ENTRY must name a real node, and two ENTRY records must not claim the same event --
        // which node handles "OnTick" has to be unambiguous, the same reason two PARAM declarations
        // can't share a name just above. The node named by ENTRY may be of ANY type (a Sequence, a
        // SetField given exec pins by hand, even a plain data node with no exec pins at all, which
        // then just runs once and continues nowhere) -- ENTRY says WHERE a chain starts, not what
        // shape a starting node must have.
        var seenEvents = new HashSet<string>();
        foreach (var (entryNodeId, eventName) in EntryPoints)
        {
            if (!Nodes.ContainsKey(entryNodeId))
            {
                err = $"ENTRY '{eventName}' names unknown node '{entryNodeId}'";
                return false;
            }
            if (!seenEvents.Add(eventName))
            {
                err = $"duplicate ENTRY for event '{eventName}' -- only one node may handle a given event";
                return false;
            }
        }

        // Check every Param node names a parameter that was actually declared, and that if it
        // already has an explicit output pin (hand-written PIN line, rather than the parser's
        // default-pins path) that pin's type agrees with the PARAM's declared type. Checked here
        // rather than only in GraphCompiler because Validate() runs both at parse time and again
        // at the start of Compile() -- a graph built programmatically (not through the text parser)
        // gets the same check.
        //
        // Deliberately BEFORE the Outputs check below: a Param node with a bad param= attribute has
        // no default output pin (see AddDefaultPins), so any LINK or OUT record touching it would
        // otherwise fail first with a generic "no output pin 'value'" that names the symptom, not the
        // param= problem that caused it.
        foreach (var node in Nodes.Values)
        {
            bool isParamNode = node.Type.Equals("param", System.StringComparison.OrdinalIgnoreCase) ||
                                node.Type.Equals("getparam", System.StringComparison.OrdinalIgnoreCase);
            if (!isParamNode) continue;

            if (string.IsNullOrEmpty(node.ParamName))
            {
                err = $"Node '{node.Id}' is a Param node but has no param= attribute naming which parameter it reads";
                return false;
            }

            var declared = Parameters.FirstOrDefault(p => p.Name == node.ParamName);
            if (declared == null)
            {
                err = $"Node '{node.Id}' references undeclared parameter '{node.ParamName}' -- add 'PARAM {node.ParamName} <type>'";
                return false;
            }

            var valuePin = node.Pins.FirstOrDefault(p => p.Name == "value" && p.IsOutput);
            if (valuePin != null && valuePin.Type != declared.Type)
            {
                err = $"Node '{node.Id}' declares output pin 'value' as {valuePin.Type} but parameter '{node.ParamName}' is {declared.Type}";
                return false;
            }
        }

        // Check all output pins reference valid nodes and output pins.
        foreach (var (nodeId, pinName) in Outputs)
        {
            if (!Nodes.TryGetValue(nodeId, out var node))
            {
                err = $"Output pin references unknown node {nodeId}";
                return false;
            }

            var pin = node.Pins.FirstOrDefault(p => p.Name == pinName && p.IsOutput);
            if (pin == null)
            {
                err = $"Node {nodeId} has no output pin '{pinName}'";
                return false;
            }
        }

        return true;
    }
}
