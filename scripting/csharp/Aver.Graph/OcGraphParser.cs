// Parser for .ocgraph text format.
// Comment explains WHY: the format is line-based UTF-8 text matching the OC family pattern.
// Lines are stripped of trailing semicolons and comments (after '#'). This parser supports
// both the C++ writer format (string node IDs, dot-notation links, "in"/"out" directions)
// and the C# format (integer node IDs as strings, space-separated link tokens, "input"/"output").
// Unknown records are skipped, not failed.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Aver.Graph;

/// Parses a .ocgraph from memory.
public class OcGraphParser
{
    /// Parses text into a Graph. Handles both C++ (string IDs, dot notation) and C# (int IDs as strings)
    /// format variants. Unknown records are skipped, not failed.
    public static bool Parse(string text, out Graph graph, out string? err)
    {
        graph = new Graph();
        err = null;
        bool sawHeader = false;
        var nodes = new Dictionary<string, Node>();

        var lines = text.Split(new[] { "\n" }, StringSplitOptions.None);

        foreach (var rawLine in lines)
        {
            // A '#' STARTS A COMMENT ONLY AT THE START OF A LINE, never mid-line.
            //
            // This used to strip from the first '#' anywhere, which is what OcWorld does -- and it is
            // wrong here for a reason this format cannot escape: the very first graph written by the
            // C++ side had the description "parsed and executed by the C# runtime", and it arrived
            // as "...by the C". A format whose entire subject matter is C# scripting will meet '#'
            // inside values constantly: type names, descriptions, node labels.
            //
            // The C++ parser already had the right rule (OcGraph.cpp:27 tests only l[0] == '#'), so
            // this is the two implementations being brought into agreement rather than a new policy.
            // The cost is that a trailing comment on a record is no longer possible; whole-line
            // comments, which is what the writer emits, still are.
            var line = rawLine.TrimEnd();
            if (line.EndsWith(";")) line = line.Substring(0, line.Length - 1);
            if (line.TrimStart().StartsWith("#")) continue;
            line = line.TrimEnd();

            if (string.IsNullOrWhiteSpace(line)) continue;

            var tokens = SplitWhitespace(line);
            if (tokens.Count == 0) continue;

            var key = tokens[0];

            if (key.Equals("OCGRAPH", StringComparison.OrdinalIgnoreCase) ||
                key.Equals("OCGRAF", StringComparison.OrdinalIgnoreCase))
            {
                int version = tokens.Count > 1 ? ParseI32(tokens[1], 1) : 1;
                if (version != 1)
                {
                    err = $"Unsupported OCGRAPH version {version}";
                    return false;
                }
                sawHeader = true;
            }
            else if (key.Equals("NAME", StringComparison.OrdinalIgnoreCase))
            {
                // NAME <graphname>
                if (tokens.Count > 1)
                {
                    graph.Name = tokens[1];
                }
            }
            else if (key.Equals("DESCRIPTION", StringComparison.OrdinalIgnoreCase))
            {
                // DESCRIPTION <rest of line>
                // Take everything after "DESCRIPTION" as the description (preserves spaces in multi-word descriptions).
                if (tokens.Count > 1)
                {
                    int descStart = line.IndexOf("DESCRIPTION", StringComparison.OrdinalIgnoreCase);
                    if (descStart >= 0)
                    {
                        string desc = line.Substring(descStart + 11).Trim();
                        graph.Description = desc;
                    }
                }
            }
            else if (key.Equals("NODE", StringComparison.OrdinalIgnoreCase))
            {
                // NODE <id> <type> [key=value ...]
                // or NODE <id> <type> <x> <y> (C++ format, x and y are stored but not used in this implementation)
                if (tokens.Count < 3)
                {
                    err = "NODE requires at least id and type";
                    return false;
                }

                string nodeId = tokens[1];  // Store as string to support both int and string IDs
                string nodeType = tokens[2];
                var node = new Node { Id = nodeId, Type = nodeType };

                // Parse optional key=value pairs for node configuration (e.g., value=42.5 for constants).
                for (int i = 3; i < tokens.Count; i++)
                {
                    var token = tokens[i];
                    if (!token.Contains("=")) continue;
                    var parts = token.Split('=', 2);
                    if (parts.Length != 2) continue;
                    var k = parts[0];
                    var v = parts[1];

                    // For any node, value= provides the constant output for Const nodes or constant data.
                    if (k == "value")
                    {
                        object? constVal = null;
                        if (v.Equals("true", StringComparison.OrdinalIgnoreCase))
                            constVal = true;
                        else if (v.Equals("false", StringComparison.OrdinalIgnoreCase))
                            constVal = false;
                        else if (int.TryParse(v, out var intVal))
                            constVal = intVal;
                        else if (float.TryParse(v, CultureInfo.InvariantCulture, out var f))
                            constVal = f;

                        if (constVal != null)
                        {
                            graph.ConstantOutputs.Add(new ConstantOutput { NodeId = nodeId, Value = constVal });
                        }
                    }
                }

                nodes[nodeId] = node;
            }
            else if (key.Equals("PIN", StringComparison.OrdinalIgnoreCase))
            {
                // PIN <nodeId> <name> <in|out|input|output> <type> [default]
                // Supports both C++ format (in/out) and C# format (input/output)
                // Default values are parsed from the optional 5th token (6th overall, index 5).
                if (tokens.Count < 4)
                {
                    err = "PIN requires nodeId, name, and direction (at minimum)";
                    return false;
                }

                string nodeId = tokens[1];  // String to support both int and string IDs
                string pinName = tokens[2];
                string direction = tokens[3].ToLowerInvariant();
                bool isOutput = direction.Equals("out") || direction.Equals("output");

                // Type is optional in C# format with defaults, but required in C++ format.
                if (tokens.Count < 5 && direction.Equals("out") && tokens.Count == 4)
                {
                    // Assume it's C++ format where type might be omitted for out pins; skip for now
                    err = "PIN requires a type specification";
                    return false;
                }

                string typeName = tokens.Count > 4 ? tokens[4] : "Float";  // Default to Float if not specified

                if (!nodes.TryGetValue(nodeId, out var node))
                {
                    err = $"PIN references unknown node {nodeId}";
                    return false;
                }

                if (!Enum.TryParse<PinType>(typeName, true, out var pinType))
                {
                    err = $"Unknown pin type '{typeName}'";
                    return false;
                }

                var pin = new Pin
                {
                    Name = pinName,
                    Type = pinType,
                    IsOutput = isOutput,
                    NodeId = nodeId
                };
                node.Pins.Add(pin);

                // If there's a default value (6th token), parse it and add as ConstantOutput if it's an output pin.
                // This handles the C++ format where constant values are stored on output pins.
                // Only do this for output pins (constants are provided on output pins of Const nodes).
                if (tokens.Count > 5 && isOutput)
                {
                    string valueStr = tokens[5];

                    // TYPED BY THE PIN'S DECLARED TYPE, NOT BY THE LITERAL'S SHAPE.
                    //
                    // This used to try int.TryParse before float.TryParse, so a pin declared
                    // `PIN c1 value out float 5` became an INT constant -- the literal has no
                    // decimal point, and nothing consulted the `float` sitting right beside it. The
                    // C++ writer emits exactly that form (num() drops a trailing ".0"), so the very
                    // first graph written by C++ and run by C# compiled cleanly and evaluated to 0:
                    // the arithmetic nodes were handed ints where they expected floats.
                    //
                    // The format states the type explicitly. Believing the literal instead is
                    // guessing when the answer is already written down.
                    object? constVal = null;
                    if (typeName.Equals("bool", StringComparison.OrdinalIgnoreCase))
                    {
                        if (bool.TryParse(valueStr, out var b)) constVal = b;
                    }
                    else if (typeName.Equals("int", StringComparison.OrdinalIgnoreCase))
                    {
                        if (int.TryParse(valueStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i)) constVal = i;
                    }
                    else if (typeName.Equals("float", StringComparison.OrdinalIgnoreCase))
                    {
                        if (float.TryParse(valueStr, NumberStyles.Float, CultureInfo.InvariantCulture, out var f)) constVal = f;
                    }
                    else
                    {
                        // An unknown pin type: fall back to inferring from the literal, which is the
                        // best that can be done when the declared type means nothing here.
                        if (valueStr.Equals("true", StringComparison.OrdinalIgnoreCase)) constVal = true;
                        else if (valueStr.Equals("false", StringComparison.OrdinalIgnoreCase)) constVal = false;
                        else if (int.TryParse(valueStr, out var iv)) constVal = iv;
                        else if (float.TryParse(valueStr, CultureInfo.InvariantCulture, out var fv)) constVal = fv;
                    }

                    if (constVal != null)
                    {
                        graph.ConstantOutputs.Add(new ConstantOutput { NodeId = nodeId, Value = constVal });
                    }
                }
            }
            else if (key.Equals("LINK", StringComparison.OrdinalIgnoreCase))
            {
                // Supports two formats:
                // 1. C++ format: LINK <srcNode>.<srcPin> <tgtNode>.<tgtPin>
                // 2. C# format: LINK <srcNodeId> <srcPinName> <tgtNodeId> <tgtPinName>
                if (tokens.Count < 2)
                {
                    err = "LINK requires at least source and target";
                    return false;
                }

                string srcNodeId = "";
                string srcPinName = "";
                string tgtNodeId = "";
                string tgtPinName = "";

                if (tokens.Count == 3)
                {
                    // C++ format: LINK source.pin dest.pin
                    var srcParts = tokens[1].Split('.');
                    var tgtParts = tokens[2].Split('.');
                    if (srcParts.Length != 2 || tgtParts.Length != 2)
                    {
                        err = "LINK in dot notation requires format: LINK source.pin dest.pin";
                        return false;
                    }
                    srcNodeId = srcParts[0];
                    srcPinName = srcParts[1];
                    tgtNodeId = tgtParts[0];
                    tgtPinName = tgtParts[1];
                }
                else if (tokens.Count >= 5)
                {
                    // C# format: LINK <srcNodeId> <srcPinName> <tgtNodeId> <tgtPinName>
                    srcNodeId = tokens[1];
                    srcPinName = tokens[2];
                    tgtNodeId = tokens[3];
                    tgtPinName = tokens[4];
                }
                else
                {
                    err = "LINK requires either 2 tokens (dot notation) or 4 tokens (space-separated)";
                    return false;
                }

                var link = new Link
                {
                    SourceNodeId = srcNodeId,
                    SourcePinName = srcPinName,
                    TargetNodeId = tgtNodeId,
                    TargetPinName = tgtPinName
                };
                graph.Links.Add(link);
            }
            else if (key.Equals("PINVAL", StringComparison.OrdinalIgnoreCase))
            {
                // PINVAL <nodeId> <pinName> <value>
                if (tokens.Count < 4)
                {
                    err = "PINVAL requires nodeId, pinName, and value";
                    return false;
                }

                string nodeId = tokens[1];  // String to support both formats
                string pinName = tokens[2];
                string valueStr = tokens[3];

                object? value = null;
                if (valueStr.Equals("true", StringComparison.OrdinalIgnoreCase))
                    value = true;
                else if (valueStr.Equals("false", StringComparison.OrdinalIgnoreCase))
                    value = false;
                else if (int.TryParse(valueStr, out var intVal))
                    value = intVal;
                else if (float.TryParse(valueStr, CultureInfo.InvariantCulture, out var f))
                    value = f;

                if (value == null)
                {
                    err = $"Could not parse value '{valueStr}' as float, int, or bool";
                    return false;
                }

                var pv = new PinnedValue { NodeId = nodeId, PinName = pinName, Value = value };
                graph.PinnedValues.Add(pv);
            }
            else if (key.Equals("OUT", StringComparison.OrdinalIgnoreCase))
            {
                // OUT <nodeId> <pinName>
                if (tokens.Count < 3)
                {
                    err = "OUT requires nodeId and pinName";
                    return false;
                }

                string nodeId = tokens[1];  // String to support both formats
                string pinName = tokens[2];
                graph.Outputs.Add((nodeId, pinName));
            }
            // Unknown records are silently skipped (not failed)
        }

        if (!sawHeader)
        {
            err = "not an .ocgraph file (no OCGRAPH header)";
            return false;
        }

        graph.Nodes = nodes;

        // Default pins for built-in node types if not explicitly declared.
        foreach (var node in graph.Nodes.Values)
        {
            AddDefaultPins(node);
        }

        // Validate the graph.
        if (!graph.Validate(out var validateErr))
        {
            err = validateErr;
            return false;
        }

        return true;
    }

    /// Adds default pins for built-in node types if they weren't explicitly declared.
    /// This allows a compact text format where most pins are implicit.
    private static void AddDefaultPins(Node node)
    {
        // Only add defaults if no pins are explicitly declared.
        if (node.Pins.Count > 0) return;

        switch (node.Type.ToLowerInvariant())
        {
            case "constfloat":
            case "const_f32":
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "constint":
            case "const_i32":
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "constbool":
            case "const_bool":
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "add":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "multiply":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "compare":
            case "compare_f32":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getfield":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "setfield":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;
        }
    }

    /// Splits a line into whitespace-separated tokens, respecting no quoting.
    private static List<string> SplitWhitespace(string line)
    {
        var tokens = new List<string>();
        var current = new StringBuilder();

        foreach (char c in line)
        {
            if (char.IsWhiteSpace(c))
            {
                if (current.Length > 0)
                {
                    tokens.Add(current.ToString());
                    current.Clear();
                }
            }
            else
            {
                current.Append(c);
            }
        }

        if (current.Length > 0)
            tokens.Add(current.ToString());

        return tokens;
    }

    private static int ParseI32(string s, int dflt = 0)
    {
        return int.TryParse(s, out var i) ? i : dflt;
    }
}
