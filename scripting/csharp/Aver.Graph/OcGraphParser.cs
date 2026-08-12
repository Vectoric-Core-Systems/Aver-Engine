// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
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
            else if (key.Equals("PARAM", StringComparison.OrdinalIgnoreCase))
            {
                // PARAM <name> <type>
                //
                // Declares one argument the compiled method accepts, in declaration order. Existing
                // .ocgraph files have no PARAM records at all, so graph.Parameters stays empty and
                // GraphCompiler.Compile() produces a zero-argument method exactly as it always has --
                // this is purely additive. A brand-new record rather than reusing NODE/PIN because a
                // parameter is not a node: it has no pins of its own to link into, and needs to be known
                // by name before any node can be validated against it (see AddDefaultPins below, and
                // Graph.Validate's Param-node checks).
                //
                // A NEW record, not an unknown one: the C++ side does not parse PARAM at all today, so
                // it round-trips PARAM lines as opaque unrecognised records (writeOcgraph preserves them
                // verbatim, per OcGraph.hpp's documented "unknown records are ignored during parse but
                // preserved during rewrite" contract) rather than failing on them. That keeps the two
                // implementations in agreement about every graph that predates this change -- including
                // the checked-in cross-implementation fixture, which has no PARAM records and is
                // therefore untouched byte-for-byte by this addition.
                if (tokens.Count < 3)
                {
                    err = "PARAM requires a name and a type";
                    return false;
                }

                string paramName = tokens[1];
                string paramTypeName = tokens[2];
                if (!Enum.TryParse<PinType>(paramTypeName, true, out var paramType))
                {
                    err = $"Unknown parameter type '{paramTypeName}'";
                    return false;
                }
                // A PARAM is a DATA argument the compiled method takes; "exec" describes control flow,
                // which is never something a caller passes IN as a value. Rejected here, at parse
                // time, rather than left to fail later as a confusing DeclareLocal(typeof(void))
                // exception three layers into GraphCompiler once something tries to use it.
                if (paramType == PinType.Exec)
                {
                    err = $"PARAM '{paramName}' cannot be declared exec -- PARAM is for data arguments a " +
                          "graph reads with a 'param' node; an exec entry point is declared with " +
                          "ENTRY <nodeId> <eventName> instead";
                    return false;
                }

                graph.Parameters.Add(new GraphParameter { Name = paramName, Type = paramType });
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
                    // param= names which declared PARAM a "param" node reads (see Node.ParamName).
                    else if (k == "param")
                    {
                        node.ParamName = v;
                    }
                    // field= names the qualified scene field a "getfield"/"setfield" node addresses
                    // (see Node.FieldName). Resolved to a dense id at compile time, not here.
                    else if (k == "field")
                    {
                        node.FieldName = v;
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
            else if (key.Equals("ENTRY", StringComparison.OrdinalIgnoreCase))
            {
                // ENTRY <nodeId> <eventName> -- declares which node begins the PUSH/exec chain for a
                // named event (e.g. "OnStart", "OnTick"). Purely additive, exactly like PARAM above:
                // an .ocgraph written before this existed has no ENTRY records, graph.EntryPoints
                // stays empty, and GraphCompiler.Compile() (the PULL/dataflow-only path) runs exactly
                // as it always has -- CompileEntryPoint() is a SEPARATE method nothing calls unless a
                // caller asks for a specific event by name. The node named here can be ANY node type;
                // ENTRY only says WHERE to start walking the exec graph, not what kind of node is
                // allowed to start it -- adding a future event (e.g. "OnCollide") is one more ENTRY
                // record naming a different node, with NO format change, exactly the extensibility
                // the task asked for. Existence of the node and event-name uniqueness are both
                // checked by Graph.Validate() below, not here -- the same division PARAM/Param-node
                // checks already follow.
                if (tokens.Count < 3)
                {
                    err = "ENTRY requires a node id and an event name";
                    return false;
                }
                graph.EntryPoints.Add((tokens[1], tokens[2]));
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
            AddDefaultPins(node, graph);
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
    private static void AddDefaultPins(Node node, Graph graph)
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

            case "sin":
            case "cos":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "subtract":
            case "sub":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "divide":
            case "div":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // ---- flow / exec nodes -------------------------------------------------------------
            // Pin shapes here MUST match sandbox/src/GraphNodeDefs.hpp's catalog entries for the same
            // types EXACTLY (same names, same order for the exec-output pins the compiler fans out in
            // pin order). A node spawned in the editor gets these pins written into the file as real
            // PIN records -- see GraphNodeDefs.hpp's own header comment on why this table exists and
            // GraphEditor.cpp's "add node" popup, which copies its pins verbatim -- and once a node
            // has ANY explicit pins, AddDefaultPins is skipped entirely for it (the early-return just
            // above this switch). So if this list and that C++ table ever disagree, an editor-authored
            // graph silently gets one shape and a hand-written or C#-only graph gets another, which is
            // exactly the "two implementations agree by coincidence" trap OcGraph.hpp's own `outputs`
            // comment warns about.
            case "branch":
                // A bool condition and one incoming exec pulse; two outgoing exec pins, exactly one
                // of which fires. "tookTrue" is OPT-IN OBSERVABILITY, not part of the control-flow
                // contract -- see GraphCompiler.EmitBranch's own comment for why an exec chain needs
                // a channel like this to be provable in a test without a live native scene to write
                // into and read back.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cond", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "true", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "false", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "tookTrue", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "sequence":
                // Two exec outputs by default ("then0" then "then1", fired in that order); add more
                // via explicit PIN records to widen it -- the compiler reads however many exec-output
                // pins the node actually has, in node.Pins order, and needs no special case to do it
                // (see GraphCompiler.EmitExecFanOut). "fireLog" is opt-in observability, like
                // branch's "tookTrue".
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then0", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then1", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "fireLog", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "while":
                // "cond" is re-pulled fresh every pass (see GraphCompiler's PUSH VS PULL comment for
                // why that rules out the old cached-local approach); "iterations" counts completed
                // passes and survives to be read after the loop -- both a genuinely useful runtime
                // value and this node's guard-tripped test's proof that the cap actually bites.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cond", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "loop", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "done", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "iterations", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "foreach":
                // The COUNTED-REPEAT variant, not a per-element iterator: the format has no
                // array/collection pin type yet (only float/int/bool/exec), so "for each element of a
                // list" cannot be expressed today. `count` says how many times to run; `index` is the
                // current pass (0..count-1, readable both inside the loop body and, holding its final
                // value, after it) -- see GraphCompiler.EmitForEach's own comment for the honest
                // "left rough for phase 2" note on this.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "count", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "loop", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "index", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "done", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "onstart":
                // No inputs at all -- an ENTRY record is what makes this node run, once, at the start
                // of the graph's life. Reads any PARAM it needs (there usually are none for OnStart)
                // the same way any other node does.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "ontick":
                // Also no inputs of its own -- per-tick data (delta time, in particular) is NOT a
                // special pin on this node type. It is an ordinary PARAM the graph declares (e.g.
                // `PARAM deltaTime float`) and reads with a `param` node inside the chain, the exact
                // same plumbing every dataflow graph already uses for `time`/`entity`. That keeps
                // "what OnTick receives" a property of the graph's own PARAM list -- inspectable and
                // extensible with no new node type -- rather than baked into this node's shape.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "param":
            case "getparam":
            {
                // The output pin's type comes from the referenced PARAM's declared type, not a fixed
                // type the way every other default-pin case has one. If param= is missing or names a
                // parameter that was never declared, add no pin at all: Graph.Validate() (run right
                // after this loop, and again at the start of every Compile()) reports the specific
                // reason, which is more useful than a generic "no output pin 'value'" from whatever
                // LINK or OUT record tries to use this node next.
                var declaredParam = graph.Parameters.FirstOrDefault(p => p.Name == node.ParamName);
                if (declaredParam != null)
                    node.Pins.Add(new Pin { Name = "value", Type = declaredParam.Type, IsOutput = true, NodeId = node.Id });
                break;
            }
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
