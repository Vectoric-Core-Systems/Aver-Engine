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
}

/// A single data input or output on a node.
public class Pin
{
    public required string Name { get; init; }
    public required PinType Type { get; init; }
    public required bool IsOutput { get; init; }
    public required int NodeId { get; init; }
}

/// A constant output value for a Const node (provided during node creation).
public class ConstantOutput
{
    public required int NodeId { get; init; }
    public required object Value { get; init; }  // float, int, or bool
}

/// A pinned value (constant) clamped to an input pin.
public class PinnedValue
{
    public required int NodeId { get; init; }
    public required string PinName { get; init; }
    public required object Value { get; init; }  // float, int, or bool
}

/// A link connecting an output pin to an input pin.
public class Link
{
    public required int SourceNodeId { get; init; }
    public required string SourcePinName { get; init; }
    public required int TargetNodeId { get; init; }
    public required string TargetPinName { get; init; }
}

/// A computation node in the graph.
public class Node
{
    public required int Id { get; init; }
    public required string Type { get; init; }  // "Const", "Add", "Multiply", etc.
    public List<Pin> Pins { get; init; } = new();
}

/// A complete graph: nodes, links, pinned values, and output pins to evaluate.
public class Graph
{
    public Dictionary<int, Node> Nodes { get; set; } = new();
    public List<Link> Links { get; set; } = new();
    public List<ConstantOutput> ConstantOutputs { get; set; } = new();
    public List<PinnedValue> PinnedValues { get; set; } = new();
    public List<(int NodeId, string PinName)> Outputs { get; set; } = new();

    /// Validates the graph for consistency. Returns false if invalid; sets err to a message.
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
