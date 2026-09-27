// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Parser for .ocgraph text format (line-based UTF-8, OC family pattern). Strips trailing ';'
// and '#' comments. Supports both the C++ writer format (string node IDs, dot-notation links,
// "in"/"out") and the C# format (int IDs as strings, space-separated tokens, "input"/"output").
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
        // Set when a NODE line names Self (rewritten to `Param entity` on the spot; see the NODE case).
        // Tells the post-parse pass whether the `entity` PARAM must be declared -- no Self node survives to count.
        bool selfSeen = false;
        var nodes = new Dictionary<string, Node>();

        var lines = text.Split(new[] { "\n" }, StringSplitOptions.None);

        foreach (var rawLine in lines)
        {
            // '#' starts a comment ONLY at the start of a line, never mid-line (unlike OcWorld, which
            // strips from the first '#' anywhere -- wrong here since C# graph text hits '#' constantly
            // in descriptions/labels, e.g. "...by the C#" truncating to "...by the C"). Matches the
            // C++ reader (OcGraph.cpp:27, l[0] == '#'). Cost: no trailing comments, only whole-line ones.
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
            else if (key.Equals("DOMAIN", StringComparison.OrdinalIgnoreCase))
            {
                // DOMAIN <name> -- which language this graph's nodes are in, so project-wide passes
                // (HostBridge.DeclareGraphClasses, GameApp.discoverProjectGraphs) can tell ours from a
                // material graph. See Graph.DomainKind / aver::fmt::OcGraphDomain for the full contract.
                // Refused when blank (matches C++ reader): absent vs present-but-blank mean opposite
                // things to DomainKind.
                if (tokens.Count < 2)
                {
                    err = "DOMAIN requires a name: DOMAIN gameplay|material";
                    return false;
                }
                if (!string.IsNullOrEmpty(graph.Domain))
                {
                    err = "duplicate DOMAIN record -- a graph belongs to at most one domain";
                    return false;
                }
                graph.Domain = tokens[1];
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
                // DESCRIPTION <rest of line> -- everything after the keyword, preserving spaces.
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
                // PARAM <name> <type> -- one compiled-method argument, in declaration order. Purely
                // additive: existing files have no PARAM records, so Parameters stays empty and
                // GraphCompiler.Compile() still emits a zero-arg method. Not a NODE/PIN: a parameter has
                // no pins and must be known by name before nodes can validate against it (AddDefaultPins
                // below, Graph.Validate's Param checks). The C++ side round-trips PARAM verbatim as an
                // unrecognised record, per OcGraph.hpp's "ignored during parse but preserved during
                // rewrite" contract, so older fixtures (incl. the cross-implementation one) are byte-for-byte untouched.
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
                // PARAM is a DATA argument; exec is control flow, never a value a caller passes in.
                // Rejected here at parse time, not later as a confusing DeclareLocal(typeof(void)) crash deep in GraphCompiler.
                if (paramType == PinType.Exec)
                {
                    err = $"PARAM '{paramName}' cannot be declared exec -- PARAM is for data arguments a " +
                          "graph reads with a 'param' node; an exec entry point is declared with " +
                          "ENTRY <nodeId> <eventName> instead";
                    return false;
                }

                graph.Parameters.Add(new GraphParameter { Name = paramName, Type = paramType });
            }
            else if (key.Equals("VAR", StringComparison.OrdinalIgnoreCase))
            {
                // VAR <name> <type> [default] -- a variable the GRAPH remembers between ticks, opposite
                // of PARAM: PARAM is supplied fresh per call, VAR is owned by the graph/host and survives
                // across invocations on the same GraphHost. See GraphVariable/GraphVarStore for the
                // storage/lifetime contract; this parser owns only the format. Round-trips through the
                // C++ side's unknown-record preservation (same as PARAM; classifyLine in OcGraph.cpp),
                // verified by OcGraphTest.cpp's testVarRecordsSurviveRoundTrip (--roundtrip), not just by reading it.
                if (tokens.Count < 3)
                {
                    err = "VAR requires a name and a type";
                    return false;
                }

                string varName = tokens[1];
                string varTypeName = tokens[2];
                if (!Enum.TryParse<PinType>(varTypeName, true, out var varType))
                {
                    err = $"Unknown variable type '{varTypeName}'";
                    return false;
                }
                // VAR is data, not control flow -- rejected here for the same reason PARAM rejects Exec
                // above (a clear failure here beats a confusing one deep in GraphCompiler).
                if (varType == PinType.Exec)
                {
                    err = $"VAR '{varName}' cannot be declared exec -- VAR is for data a graph " +
                          "remembers between ticks, not control flow; control flow is expressed with " +
                          "exec pins and LINK records, not a stored variable";
                    return false;
                }

                // Typed by the DECLARED type, not the literal's shape (same lesson PIN's default parsing
                // below paid for). Unlike PIN's hard "no default at all" fallback, an unparseable default
                // falls back to the type's zero value (GraphVarStore.CreateFor needs a concrete non-null
                // Default) rather than failing the graph -- a fat-fingered default is more likely than a garbage record.
                object varDefault = varType switch
                {
                    PinType.Bool => false,
                    PinType.Int => 0,
                    _ => 0f, // PinType.Float (Exec already rejected above)
                };
                if (tokens.Count > 3)
                {
                    string defaultStr = tokens[3];
                    if (varType == PinType.Bool && bool.TryParse(defaultStr, out var b))
                        varDefault = b;
                    else if (varType == PinType.Int && int.TryParse(defaultStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i))
                        varDefault = i;
                    else if (varType == PinType.Float && float.TryParse(defaultStr, NumberStyles.Float, CultureInfo.InvariantCulture, out var f))
                        varDefault = f;
                    // else: unparsed literal keeps the zero-value fallback above rather than failing the graph.
                }

                graph.Variables.Add(new GraphVariable { Name = varName, Type = varType, Default = varDefault });
            }
            else if (key.Equals("COMP", StringComparison.OrdinalIgnoreCase))
            {
                // COMP <id> <Kind> [parent=<id>] [pos=x,y,z] [rot=yaw,pitch,roll] [scale=x,y,z] [key=value]...
                // One child entity of a spawned class instance (see GraphComponent for field meanings,
                // and why the transform is typed here while the C++ reader keeps the whole line as tokens).
                // Structural rules (duplicate ids, dangling/cyclic parent) are NOT re-checked here --
                // modules/formats' reader (used when the editor opens a file) owns that, to avoid two
                // drifting implementations of the same rule. This side owns Kind's meaning and transform units.
                if (tokens.Count < 3)
                {
                    err = "COMP requires an id and a kind: COMP id Kind [key=value]...";
                    return false;
                }

                var comp = new GraphComponent { Id = tokens[1], Kind = tokens[2] };
                for (int i = 3; i < tokens.Count; i++)
                {
                    string[] parts = tokens[i].Split('=', 2);
                    if (parts.Length != 2) continue;   // a bare token, not an attribute
                    string k = parts[0];
                    string v = parts[1];
                    if (k.Equals("parent", StringComparison.OrdinalIgnoreCase)) comp.Parent = v;
                    else if (k.Equals("pos", StringComparison.OrdinalIgnoreCase)) ParseVec3(v, comp.Position);
                    else if (k.Equals("rot", StringComparison.OrdinalIgnoreCase)) ParseVec3(v, comp.RotationDeg);
                    else if (k.Equals("scale", StringComparison.OrdinalIgnoreCase)) ParseVec3(v, comp.Scale);
                    else comp.Attributes[k] = v;
                }
                graph.Components.Add(comp);
            }
            else if (key.Equals("FUNC", StringComparison.OrdinalIgnoreCase))
            {
                // FUNC <name> [pure] -- declares one user-defined function. See GraphFunction for why
                // purity is DECLARED here rather than inferred from the body.
                if (tokens.Count < 2)
                {
                    err = "FUNC requires a name: FUNC name [pure]";
                    return false;
                }
                string funcName = tokens[1];
                if (graph.Functions.Any(f => string.Equals(f.Name, funcName, StringComparison.OrdinalIgnoreCase)))
                {
                    err = $"duplicate FUNC name '{funcName}'";
                    return false;
                }
                var fn = new GraphFunction { Name = funcName };
                for (int i = 2; i < tokens.Count; i++)
                    if (tokens[i].Equals("pure", StringComparison.OrdinalIgnoreCase)) fn.IsPure = true;
                graph.Functions.Add(fn);
            }
            else if (key.Equals("FUNCIN", StringComparison.OrdinalIgnoreCase) ||
                     key.Equals("FUNCOUT", StringComparison.OrdinalIgnoreCase))
            {
                // FUNCIN/FUNCOUT <func> <pin> <type> -- one argument/return, in declaration order. Two
                // records, not one with a direction token, since input order and output order are independent
                // lists and one interleaved record would imply an order between them that nothing depends on.
                bool isIn = key.Equals("FUNCIN", StringComparison.OrdinalIgnoreCase);
                if (tokens.Count < 4)
                {
                    err = $"{key.ToUpperInvariant()} requires a function, a pin name and a type: {key.ToUpperInvariant()} func pin float|int|bool";
                    return false;
                }
                var owner = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, tokens[1], StringComparison.OrdinalIgnoreCase));
                if (owner == null)
                {
                    // Referencing a FUNC before it's declared is a real error, not a forward reference like a
                    // LINK naming a later NODE: there's no second pass to resolve it, and dropping it silently
                    // would mis-arity the function with no report of why.
                    err = $"{key.ToUpperInvariant()} names function '{tokens[1]}', which no FUNC record declares above it";
                    return false;
                }
                if (!Enum.TryParse<PinType>(tokens[3], true, out PinType ft))
                {
                    err = $"{key.ToUpperInvariant()} '{tokens[2]}' has type '{tokens[3]}'; expected float, int or bool";
                    return false;
                }
                if (ft == PinType.Exec)
                {
                    // Exec is not a value -- a function's control flow is decided by its `pure` flag
                    // alone, not also by an exec argument here that could disagree with it.
                    err = $"{key.ToUpperInvariant()} '{tokens[2]}' cannot be of type exec -- declare the function impure instead";
                    return false;
                }
                var list = isIn ? owner.Inputs : owner.Outputs;
                if (list.Any(p => p.Name == tokens[2]))
                {
                    err = $"function '{owner.Name}' already has {(isIn ? "an input" : "an output")} named '{tokens[2]}'";
                    return false;
                }
                list.Add(new GraphParameter { Name = tokens[2], Type = ft });
            }
            else if (key.Equals("CLASS", StringComparison.OrdinalIgnoreCase))
            {
                // CLASS <name> [parentName] [mesh=<path>] [material=<name>] [view=firstperson|thirdperson]
                // Declares this graph file as a SPAWNABLE ACTOR CLASS -- the Aver Node analogue of a
                // Blueprint asset with a parent class, not a component referencing a graph. See
                // Graph.ClassName for the full story. Like PARAM/VAR, the C++ reader round-trips it
                // verbatim as an unrecognised OwnedLineKind::Other line.
                if (tokens.Count < 2)
                {
                    err = "CLASS requires a name";
                    return false;
                }
                if (!string.IsNullOrEmpty(graph.ClassName))
                {
                    err = "duplicate CLASS record -- a graph may declare itself as at most one class";
                    return false;
                }

                string className = tokens[1];
                // Defaults to "Actor" when omitted, mirroring HostBridge's BaseRegistryName default,
                // so `CLASS Foo` alone is a complete, sealable, spawnable declaration.
                string classParent = "Actor";
                string? classMesh = null;
                string? classMaterial = null;
                string? classView = null;
                string? classPawn = null;
                string? classController = null;
                for (int i = 2; i < tokens.Count; i++)
                {
                    var token = tokens[i];
                    if (token.Contains('='))
                    {
                        var parts = token.Split('=', 2);
                        if (parts.Length != 2) continue;
                        if (parts[0] == "mesh") classMesh = parts[1];
                        else if (parts[0] == "material") classMaterial = parts[1];
                        // firstperson/thirdperson; see Graph.ClassView for consumers (HostBridge.DispBind,
                        // Character ancestors only). Raw string here like mesh/material; validated later.
                        else if (parts[0] == "view") classView = parts[1];
                        // GameMode's default pawn class name (see Graph.ClassPawn); resolved against
                        // declared classes at seal, not a file path.
                        else if (parts[0] == "pawn") classPawn = parts[1];
                        // GameMode's player-controller class; needs pawn= too since possession requires both (Graph.ClassController).
                        else if (parts[0] == "controller") classController = parts[1];
                        // An unrecognised key=value attribute is ignored rather than failing the
                        // whole graph -- mirrors NODE's own key=value loop, just below.
                    }
                    else if (i == 2)
                    {
                        // The one positional token right after the name, if it is not itself a
                        // key=value pair, is the parent class name.
                        classParent = token;
                    }
                    // A stray bare token past position 2 (not key=value, not the parent slot) is
                    // ignored -- malformed input should not fail a graph that otherwise parses fine.
                }

                graph.ClassName = className;
                graph.ClassParent = classParent;
                graph.ClassMesh = classMesh;
                graph.ClassMaterial = classMaterial;
                graph.ClassView = classView;
                graph.ClassPawn = classPawn;
                graph.ClassController = classController;
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

                // SELF IS SUGAR FOR `Param entity`, resolved HERE at construction: Node.Type is
                // init-only, so resolving later would mean rebuilding the node and copying every field
                // across by hand, which would silently drop whichever field is added to Node next. The
                // PARAM itself is declared after the full file is read (Graph.ResolveSelfNodes, below) --
                // a hand-written file may already declare `PARAM entity int` above or below its nodes, and
                // Self must reuse it, not add a second (would change the compiled method's arity). selfSeen
                // flags that a Self was seen; param="entity" marks the node. Needed because most Scene/
                // Character/Physics/Animation/Audio nodes take `entity`, and the EDITOR cannot write PARAM
                // records itself -- aver::fmt::OcGraphData models no parameters, so a canvas-only graph
                // could not otherwise reach the entity it runs on.
                bool isSelf = nodeType.Equals("self", StringComparison.OrdinalIgnoreCase);
                if (isSelf) { nodeType = "Param"; selfSeen = true; }
                var node = new Node { Id = nodeId, Type = nodeType };
                if (isSelf) node.ParamName = "entity";

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
                        // PARSED BY THE NODE'S DECLARED TYPE, not the literal's shape. Used to guess
                        // (true/false, then int, then float), which silently produced the WRONG CLR
                        // TYPE -- both compilers read a ConstFloat with a strict `is float f` and fall
                        // back to 0 with no warning:
                        //
                        //   NODE speed ConstFloat value=100    boxed an int, compiled to 0.0f
                        //   NODE n     ConstInt   value=7.0    boxed a float, compiled to 0
                        //   NODE b     ConstBool  value=1      boxed an int, compiled to false
                        //
                        // Silent in both PULL and PUSH compilers; survived because this tree's samples
                        // always write floats with a decimal point, so int.TryParse never won. Found
                        // twice independently (graph-variable acceptance evidence, a raycast probe).
                        // Mirrors the `PIN <node> value out <type> <literal>` path below, which already
                        // branches on the pin's declared type before parsing.
                        object? constVal = null;
                        var kind = nodeType.ToLowerInvariant();
                        if (kind == "constbool" || kind == "const_bool")
                        {
                            if (bool.TryParse(v, out var b)) constVal = b;
                        }
                        else if (kind == "constint" || kind == "const_i32")
                        {
                            if (int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i32))
                                constVal = i32;
                        }
                        else if (kind == "constfloat" || kind == "const_f32")
                        {
                            if (float.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var f32))
                                constVal = f32;
                        }
                        else
                        {
                            // Any OTHER node type keeps the old shape-based inference: no declared type
                            // to consult, and nothing in the vocabulary reads one today.
                            if (v.Equals("true", StringComparison.OrdinalIgnoreCase)) constVal = true;
                            else if (v.Equals("false", StringComparison.OrdinalIgnoreCase)) constVal = false;
                            else if (int.TryParse(v, out var intVal)) constVal = intVal;
                            else if (float.TryParse(v, CultureInfo.InvariantCulture, out var f)) constVal = f;
                        }

                        if (constVal != null)
                        {
                            graph.ConstantOutputs.Add(new ConstantOutput { NodeId = nodeId, Value = constVal });
                        }
                        else
                        {
                            // A Const node whose literal does not parse AS ITS OWN TYPE is refused by
                            // name rather than left to compile as zero. That silence was the whole bug.
                            err = $"Node '{nodeId}' ({nodeType}) has value='{v}', which is not a valid "
                                + "literal for that node's type";
                            return false;
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
                    // class= names the class a "spawn" node instantiates (Node.ClassName); resolved by
                    // NAME at invocation time (GraphInterop.SpawnForGraph), not baked to a handle earlier.
                    else if (k == "class")
                    {
                        node.ClassName = v;
                    }
                    // var= names the declared VAR a "getvar"/"setvar" node addresses (Node.VarName);
                    // Graph.Validate() checks it against Variables, not here.
                    else if (k == "var")
                    {
                        node.VarName = v;
                    }
                    // func= names the FUNCTION this node lives in; absent means the event graph. See
                    // Node.FuncOwner for why ownership is an attribute rather than a list.
                    else if (k == "func")
                    {
                        node.FuncOwner = v;
                    }
                    // call= names the function a "callfunc" node calls. Distinct from func= above,
                    // which says where the call node itself LIVES -- see Node.CallTarget.
                    else if (k == "call")
                    {
                        node.CallTarget = v;
                    }
                    // name= carries the literal string a "setname" node writes (Node.NameValue) -- the
                    // value itself, not a lookup key, unlike field=/class=/var= above (no String pin
                    // exists to carry it). GraphCompiler.EmitSetName/EmitExecSetName require it non-empty
                    // at COMPILE time (fail loudly, not a silent runtime no-op) -- mirrors class= for Spawn.
                    else if (k == "name")
                    {
                        node.NameValue = v;
                    }
                    // sound= names the audio file "playsound"/"playsoundat" plays (Node.SoundPath) --
                    // same "value IS the data" treatment as name=.
                    else if (k == "sound")
                    {
                        node.SoundPath = v;
                    }
                    // text= is the message a "printstring" node writes; same treatment as sound=.
                    else if (k == "text")
                    {
                        node.PrintText = v;
                    }
                    // mesh= names the asset path a "setmesh" node writes (see Node.MeshPath).
                    else if (k == "mesh")
                    {
                        node.MeshPath = v;
                    }
                    // curve= names the curve a "getanimcurve" node reads (see Node.CurveName).
                    else if (k == "curve")
                    {
                        node.CurveName = v;
                    }
                    // socket= names the socket an "attachtosocket" node hangs its entity on (see
                    // Node.SocketName).
                    else if (k == "socket")
                    {
                        node.SocketName = v;
                    }
                    // material= names the material a "setmaterial" node writes (see Node.MaterialName).
                    else if (k == "material")
                    {
                        node.MaterialName = v;
                    }
                    // skeleton= names the asset path a "setskeleton" node binds (see Node.SkeletonPath)
                    // -- same "the value IS the data" treatment mesh= gets just above.
                    else if (k == "skeleton")
                    {
                        node.SkeletonPath = v;
                    }
                    // clip= names the asset path "playanimation" plays (Node.ClipPath), same as skeleton=.
                    // loop is NOT parsed here -- it's a pin value, not a NODE-line attribute.
                    else if (k == "clip")
                    {
                        node.ClipPath = v;
                    }
                    // rig= names the .ocrig a "setcontrolrig" node binds (see Node.RigPath) -- same
                    // treatment as skeleton=/clip= above. weight is NOT parsed here; it is a pin.
                    else if (k == "rig")
                    {
                        node.RigPath = v;
                    }
                    // event= names the event a "fireevent" node fires on another entity's graph
                    // (Node.EventName), e.g. "OnHit" -- same "carries data" treatment as name=/mesh=;
                    // GraphCompiler.EmitExecFireEvent requires it non-empty at COMPILE time (like class=).
                    else if (k == "event")
                    {
                        node.EventName = v;
                    }
                    // path= names the file "savegame" writes / "loadgame" reads (Node.SavePath) -- same
                    // treatment; GraphCompiler.EmitExecSaveLoad requires it non-empty at COMPILE time too.
                    else if (k == "path")
                    {
                        node.SavePath = v;
                    }
                    // action= names the declared INPUT ACTION an "inputaction"/"inputactionpressed"/
                    // "inputactionreleased"/"rebindaction"/"getactionkey" node addresses (Node.ActionName).
                    // OPTIONAL on the first three (a plain Int `action` pin is the fallback); REQUIRED at
                    // COMPILE time on the last two, like class=/path=.
                    else if (k == "action")
                    {
                        node.ActionName = v;
                    }
                }

                nodes[nodeId] = node;
            }
            else if (key.Equals("PIN", StringComparison.OrdinalIgnoreCase))
            {
                // PIN <nodeId> <name> <in|out|input|output> <type> [default] -- supports both C++
                // (in/out) and C# (input/output). Default value is the optional 5th token (index 5).
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

                // 6th token (C++ format): a default value on an output pin, stored as a ConstantOutput.
                // Output pins only -- constants are provided on output pins of Const nodes.
                if (tokens.Count > 5 && isOutput)
                {
                    string valueStr = tokens[5];

                    // TYPED BY THE PIN'S DECLARED TYPE, NOT THE LITERAL'S SHAPE. Used to try int before
                    // float, so `PIN c1 value out float 5` became an INT constant (no decimal point) --
                    // exactly the form the C++ writer emits (num() drops a trailing ".0"), so the first
                    // C++-written graph run by C# compiled cleanly and evaluated to 0: the arithmetic nodes
                    // were handed ints where they expected floats. The format states the type explicitly;
                    // believing the literal instead is guessing when the answer is already written down.
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
                // C++ format: LINK <srcNode>.<srcPin> <tgtNode>.<tgtPin>
                // C# format: LINK <srcNodeId> <srcPinName> <tgtNodeId> <tgtPinName>
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
                // named event (e.g. "OnStart", "OnTick"). Purely additive, like PARAM: older files have
                // no ENTRY records, EntryPoints stays empty, and Compile() (PULL/dataflow) is unaffected --
                // CompileEntryPoint() is separate, called only when a caller asks for a named event. The
                // node can be ANY type; ENTRY only says where to start walking the exec graph, so a future
                // event needs no format change. Existence/uniqueness are checked by Graph.Validate() below,
                // not here -- same division PARAM/Param-node checks already follow.
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

        // Which node starts/ends each function, resolved from the nodes rather than declared on FUNC (a
        // second place to disagree). A function has exactly one FuncEntry and at most one FuncReturn --
        // Validate refuses a second of either. Runs BEFORE AddDefaultPins since the entry node's pins are
        // the function's inputs, and it must know which function it belongs to first.
        foreach (var node in graph.Nodes.Values)
        {
            if (string.IsNullOrEmpty(node.FuncOwner)) continue;
            var owner = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.FuncOwner, StringComparison.OrdinalIgnoreCase));
            if (owner == null) continue;   // reported by Validate, which can name every offender at once
            string t = node.Type.ToLowerInvariant();
            if (t == "funcentry" && owner.EntryNodeId == null) owner.EntryNodeId = node.Id;
            else if (t == "funcreturn" && owner.ReturnNodeId == null) owner.ReturnNodeId = node.Id;
        }

        // Self is shorthand for the graph's own entity handle, rewritten to `Param entity` here, between
        // func= ownership resolution (which the refusal below needs) and AddDefaultPins (which gives the
        // rewritten node its output pin from the parameter's type). See Graph.ResolveSelfNodes.
        if (selfSeen && !graph.ResolveSelfNodes(out var selfErr))
        {
            err = selfErr;
            return false;
        }

        foreach (var node in graph.Nodes.Values)
        {
            AddDefaultPins(node, graph);
        }

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

            // ---- boolean logic ----------------------------------------------------
            // No boolean operators existed before these (authors faked "A and B" with nested Branches,
            // which can't express OR/NOT without inverting the structure).
            case "and":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "or":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "xor":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "not":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- the rest of the comparisons ---------------------------------------
            // `compare` was a strict a > b alone, so a >= b needed Compare plus Not; equality didn't exist.
            case "greater":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "greaterequal":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "less":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "lessequal":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "equal":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "notequal":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- arithmetic and the standard library --------------------------------
            case "min":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "max":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "mod":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "pow":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "abs":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "negate":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "sqrt":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "floor":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "ceil":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "round":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "saturate":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "clamp":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "min", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "max", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "lerp":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "t", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
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

            // Print(value) -> then. Exec pins by default: logging is a side effect with a definite
            // "when" (like SetVar) -- pulled, it would fire once per reader or not at all.
            // Vector maths below: loose float components in/out, no exec pins -- pure, safe to pull
            // freely (cf. GetFieldVec3).
            case "vecadd":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "by", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "vecsub":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "by", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "vecscale":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "s", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "veccross":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "by", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "vecnormalize":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "veclerp":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "by", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "t", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "vecdot":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "by", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "veclength":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "vecdistance":
                node.Pins.Add(new Pin { Name = "ax", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ay", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "az", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "by", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // The four WRITE nodes (SetVelocity, Teleport, Possess, Unpossess) carry exec pins as side
            // effects with a definite "when"; the readers below don't, and are safe to pull freely.
            case "getvelocity":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "isgrounded":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "grounded", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setvelocity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "teleport":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getplayerpawn":
                node.Pins.Add(new Pin { Name = "index", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "getplayercontroller":
                node.Pins.Add(new Pin { Name = "index", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "getgamemode":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "isplaying":
                node.Pins.Add(new Pin { Name = "playing", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "possess":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "controller", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pawn", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "unpossess":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "controller", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // SaveGame / LoadGame: no entity pin, unlike everything else here -- both act on the WHOLE
            // world. path= names the file (Node.SavePath); shape otherwise matches Possess/Unpossess.
            case "savegame":
            case "loadgame":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // SaveInputBindings/LoadInputBindings/ResetInputBindings: SaveGame/LoadGame's rebindable-input
            // twin, same reason -- all three act on EVERY pushed EnhancedInput context, not one thing
            // (GraphCompiler.EmitExecInputBindingOp/IsExecCapableInputBindingOpType).
            case "saveinputbindings":
            case "loadinputbindings":
            case "resetinputbindings":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // RebindAction: changes ONE binding of ONE action, so unlike the three above it needs inputs:
            // action= names WHICH action (required at compile time, Node.ActionName), `slot` picks the
            // binding (same per-action counting as GraphInterop.EnhancedInput.SaveBindings), `key` is
            // the new physical key/button/axis (GraphCompiler.EmitExecRebindAction).
            case "rebindaction":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "slot", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // GetActionKey / GetPressedKey: InputKey's pure-data shape applied to the rebinding family --
            // no exec pins, safe to pull from either compiler (idempotent reads).
            //
            // GetActionKey: "what is bound to action=<Name>'s slot-th binding" -- action= is REQUIRED
            // at compile time (Node.ActionName; no pin fallback, unlike InputAction). `key` is -1 when
            // unbound; `bound` is exactly `key != -1`, computed rather than a second native call
            // (GraphCompiler.EmitGetActionKey/EmitPullGetActionKey).
            case "getactionkey":
                node.Pins.Add(new Pin { Name = "slot", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bound", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // GetPressedKey answers "what is the lowest-valued key/mouse-button down THIS FRAME" --
            // the read a rebinding UI's "press a key to bind" prompt needs, with no `action=` and no
            // pins at all going in. `key` is -1 when nothing is down; `pressed` is `key != -1`, the
            // same derived-not-called-twice shape GetActionKey uses.
            case "getpressedkey":
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pressed", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Conversions: one pin in, one pin out, pure.
            case "inttofloat":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "booltofloat":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "floattoint":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            // The entity transform family. The three writers carry exec pins; the readers do not.
            case "getworldposition":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getentityforward":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getentityright":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getentityup":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getlocalscale":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "isalive":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "alive", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "isactor":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "isActor", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "translate":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Same pin shape as setlocalscale below, deliberately: a node that moves a child and one
            // that resizes it shouldn't need to be learned twice.
            case "setlocalposition":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // DEGREES, named yaw/pitch/roll not x/y/z on purpose: PinType has no quaternion, and x/y/z
            // beside a position node's own x/y/z is how an author wires roll into yaw by mistake.
            case "setlocalrotation":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "yaw", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pitch", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "roll", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // World-target x/y/z, same pin shape as setlocalposition: "move there" and "face there"
            // take the same three numbers.
            case "lookat":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setlocalscale":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "destroyentity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // The physics family. Readers are pure; writers, creators and the sweep carry exec pins.
            case "getbodyposition":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getbodyvelocity":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "isbodyvalid":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "valid", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getbodycount":
                node.Pins.Add(new Pin { Name = "count", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            // The two Physics STATUS reads. No inputs at all -- they ask the simulation about
            // itself, so there is nothing to pass in. Pure, no exec, same as getbodycount above.
            case "isphysicsready":
                node.Pins.Add(new Pin { Name = "ready", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getfixedstep":
                node.Pins.Add(new Pin { Name = "seconds", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // CreateEntity/FindEntity -- SetName's own name= family (Node.NameValue), never a pin.
            // CreateEntity is exec (it makes something); FindEntity is pure, "found" a real answer, not an error.
            case "createentity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "findentity":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "found", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "raycastany":
                node.Pins.Add(new Pin { Name = "originX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "maxDist", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hit", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodyposition":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodyvelocity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "addbodyvelocity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "destroybody":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodyentity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setgravity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "addstaticbox":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "adddynamicbox":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "mass", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "adddynamicsphere":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "radius", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "mass", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "addsensorbox":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "addsensorsphere":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "radius", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "spherecast":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "maxDist", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "radius", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hit", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pointX", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pointY", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pointZ", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // Forces and impulses. Same shape as addbodyvelocity above: exec, body, three loose
            // floats, then/success out. A force/torque lasts one step; an impulse is instantaneous.
            case "addforce":
            case "addimpulse":
            case "addtorque":
            case "addangularimpulse":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Spin. GetBodyAngularVelocity is pure, same shape as getbodyvelocity; SetBodyAngularVelocity
            // is a write, same shape as setbodyvelocity.
            case "getbodyangularvelocity":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodyangularvelocity":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Material and mass. One scalar write each; GetBodyMass is the one pure read.
            case "setbodyfriction":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "friction", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodyrestitution":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "restitution", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodygravityfactor":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "factor", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbodymass":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "mass", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getbodymass":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "mass", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Motion type and sleeping.
            case "setbodymotiontype":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "motionType", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getbodymotiontype":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "motionType", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "activatebody":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "isbodyactive":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "active", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Layers. SetLayerCollision has no body pin -- it edits the world's shared matrix.
            case "setbodylayer":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "layer", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getbodylayer":
                node.Pins.Add(new Pin { Name = "body", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "layer", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setlayercollision":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "layerA", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "layerB", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "collide", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Joints. Creators mirror addstaticbox/etc: exec + params in, then + a "joint" handle out.
            // See GraphNodeDefs.hpp's JOINTS banner for why bodyB == 0 means the world, and why
            // Hinge/Slider each carry a second (normal) axis.
            case "jointfixed":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyA", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyB", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "px", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "py", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "axX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "axY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "axZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ayX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ayY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ayZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "jointpoint":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyA", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyB", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "px", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "py", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "jointdistance":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyA", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyB", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "paX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "paY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "paZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pbX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pbY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pbZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "minDist", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "maxDist", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "jointhinge":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyA", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyB", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "px", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "py", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "nx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ny", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "nz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "minAngleRad", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "maxAngleRad", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "jointslider":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyA", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bodyB", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "px", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "py", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "sx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "sy", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "sz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "nx", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ny", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "nz", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "minCm", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "maxCm", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "jointsetmotor":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "state", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "target", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "jointsetenabled":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "enabled", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "jointremove":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getjointvalue":
                node.Pins.Add(new Pin { Name = "joint", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // Reroute: one pin in, the same type out. Purely a place to bend a wire.
            case "reroutefloat":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "rerouteint":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "reroutebool":
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "rerouteexec":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            // Tags and visibility. A tag is an int bitmask, sparing this family the compile-time-attribute
            // machinery every string-shaped node needs (GraphInterop).
            case "setvisible":
            case "addtag":
            case "removetag":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin {
                    Name = node.Type.Equals("setvisible", StringComparison.OrdinalIgnoreCase) ? "visible" : "mask",
                    Type = node.Type.Equals("setvisible", StringComparison.OrdinalIgnoreCase) ? PinType.Bool : PinType.Int,
                    IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "hastag":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "mask", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "has", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "gettags":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "mask", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            // SwitchInt: an N-way Branch. See GraphNodeDefs.hpp for why the case count is fixed.
            case "switchint":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "selector", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "case0", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "case1", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "case2", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "case3", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "default", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "taken", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "printint":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "print":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            // Exec in, exec out, nothing else. PrintString's message is a NODE-line attribute (text=),
            // not a pin -- PinType has no String -- which is what lets it answer "did control flow reach
            // here" with only the exec chain wired. Must match GraphNodeDefs.hpp's PrintString row exactly.
            case "printstring":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
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

            // getfieldvec3/setfieldvec3: GetField/SetField's Vec3 siblings -- a Vec3-kind scene field
            // (CLocal.position/scale, CLight.colour, ...) as three ORDINARY float pins, no new pin TYPE.
            // field= is reused verbatim. No exec pins on either, mirroring getfield/setfield, INCLUDING
            // setfieldvec3 (a write) having none -- deliberate, see GraphCompiler.EmitSetFieldVec3.
            case "getfieldvec3":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // GetForward(entity) -> direction xyz + eye xyz (where to fire from) + success. No exec
            // pins, like GetFieldVec3: a pure, idempotent read, safe to pull freely (eye rides along
            // because a direction alone can't build a ray -- GraphInterop.LookDirectionForGraph).
            // Jump(entity) -> jumped. EXEC PINS, unlike GetForward: jumping is a side effect with a
            // definite "when". `jumped` is false when airborne (AverCharacter.Jump refuses mid-air) --
            // ordinary, not an error.
            case "jump":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "jumped", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // GetViewEntity(entity) -> view + success. The CAMERA node a character looks through -- a
            // first-person viewmodel parents to it to stay put as the camera pitches. Pure, like GetForward.
            case "getviewentity":
            case "get_view_entity":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "view", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "getforward":
            case "get_forward":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "eyeX", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "eyeY", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "eyeZ", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setfieldvec3":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
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
            // Pin shapes here MUST match sandbox/src/GraphNodeDefs.hpp's catalog EXACTLY (same names,
            // same exec-output order): an editor-spawned node writes explicit PIN records (GraphEditor.cpp's
            // "add node" popup copies them verbatim) and skips AddDefaultPins entirely (early-return
            // above), so if the two tables disagree, an editor-authored graph and a hand-written one
            // silently get different shapes -- the "two implementations agree by coincidence" trap
            // OcGraph.hpp's `outputs` comment warns about.
            case "branch":
                // Bool condition, one incoming exec pulse, two outgoing exec pins (exactly one fires).
                // "tookTrue" is opt-in observability, not part of the control-flow contract -- lets an
                // exec chain be provable in a test without a live native scene (GraphCompiler.EmitBranch).
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cond", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "true", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "false", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "tookTrue", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "sequence":
                // Two exec outputs by default ("then0"/"then1", fired in order); widen via explicit PIN
                // records -- the compiler fans out however many exec-output pins exist, in node.Pins
                // order, no special case (GraphCompiler.EmitExecFanOut). "fireLog" is opt-in observability, like branch's "tookTrue".
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then0", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then1", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "fireLog", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            // ---- gated flow control -------------------------------------------------
            // These three REMEMBER state between activations (Branch/Sequence decide from inputs
            // alone). State lives in the per-instance GraphVarStore a VAR uses, under a name keyed to
            // the node id, so two entities running one graph gate independently -- GraphVarStore's own
            // header calls this the property most likely to be silently undone.
            case "doonce":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                // reset is a BOOL, not an exec input: EmitExecNode isn't told WHICH input pin an
                // activation arrived on, so two exec inputs would be indistinguishable. Same as Gate.
                node.Pins.Add(new Pin { Name = "reset", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "gate":
                // `open`/`close` are BOOL, not exec pins: with many possible sources, three exec
                // entries would make "which one fired" unanswerable inside one activation.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "open", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "close", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "flipflop":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "a", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "b", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "isA", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "while":
                // "cond" is re-pulled fresh every pass (rules out a cached-local approach, see
                // GraphCompiler's PUSH vs PULL comment). "iterations" counts completed passes and
                // survives after the loop as proof the cap actually bites.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "cond", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "loop", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "done", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "iterations", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            case "foreach":
                // COUNTED-REPEAT, not per-element: no array/collection pin type exists yet (only
                // float/int/bool/exec). `count` sets the repeat count; `index` is the current pass
                // (0..count-1), holding its final value after the loop. See GraphCompiler.EmitForEach
                // ("left rough for phase 2").
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "count", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "loop", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "index", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "done", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "onstart":
                // No inputs -- an ENTRY record makes this node run once, at the start of the graph's life.
                // Reads any PARAM it needs the same way any node does (there usually are none for OnStart).
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "ontick":
                // No inputs of its own -- per-tick data (delta time) is an ordinary PARAM the graph
                // declares (e.g. `PARAM deltaTime float`) and reads via a `param` node, the same
                // plumbing as `time`/`entity`, keeping what OnTick receives inspectable/extensible with no new node type.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "customevent":
                // A USER-NAMED ENTRY POINT -- same bare-exec-output shape as onstart/ontick/onhit; the
                // top-level `ENTRY <nodeId> <eventName>` record is what makes an event fire, not the
                // node type, so this exists just to let an author pick their own event name. The name
                // lives on ENTRY, not here: `name=` is only what the EDITOR displays/syncs (GraphEditor
                // writes both); CompileEntryPoint matches on ENTRY, so a hand-written ENTRY with no `name=` runs the same.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "onhit":
                // Same bare-trigger shape as onstart/ontick: the node TYPE is just a labeled start an
                // ENTRY record points at. What fires it ON DEMAND (GraphHost.Fire, vs OnStart/OnTick's
                // fixed Tick() cadence) lives in GraphHost, not here -- "onhit" is no more special to
                // the parser than any other custom ENTRY event name. A payload (who/where/how hard) is
                // an ordinary PARAM read via `param`, like OnTick's deltaTime.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "param":
            case "getparam":
            {
                // Output type comes from the referenced PARAM, unlike every other case's fixed type. If
                // param= is missing/undeclared, add no pin: Graph.Validate() (right after this loop, and
                // again at the start of every Compile()) reports the specific reason.
                var declaredParam = graph.Parameters.FirstOrDefault(p => p.Name == node.ParamName);
                if (declaredParam != null)
                    node.Pins.Add(new Pin { Name = "value", Type = declaredParam.Type, IsOutput = true, NodeId = node.Id });
                break;
            }

            // ---- Select / InputKey / Raycast -----------------------------------------------------
            // All three have real DATA outputs (unlike branch/while/foreach; see IsExecOnlyNodeType),
            // so Compile() never skips them -- handled by BOTH compilers (GraphCompiler.cs's
            // EmitSelect/EmitInputKey/EmitRaycast).

            case "select":
                // Pure data, no exec pins -- picks ifTrue/ifFalse by cond. Both are computed regardless
                // of cond in the PULL compiler (not a bug, not short-circuiting like Branch's exec fan-out).
                node.Pins.Add(new Pin { Name = "cond", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ifTrue", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ifFalse", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "inputkey":
                // Pure data, no exec pins: polled input is idempotent, safe to pull freely through
                // either compiler with no _execLocals caching (GraphCompiler.EmitInputKey). InputAction
                // below is the newer NAMED, rebindable sibling; this stays for literal key codes and pre-InputAction content.
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "down", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- edge-triggered input --------------------------------------------------------
            // The half InputKey can't express: `down` means "held", the wrong question for jumping,
            // semi-auto fire, toggles -- held-means-true fires every frame, so authors needed a
            // hand-built DoOnce+variable edge detector (five nodes) instead of the framework ABI's
            // direct answer (aver_fw_input_key_pressed/_released, which Input.GetKeyDown/GetKeyUp already wrap).
            // Same pin shape as inputkey, no exec pins: computed from this frame vs last, nothing to cache.
            case "inputkeypressed":
            case "inputkeyreleased":
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "triggered", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- InputAction / InputActionPressed / InputActionReleased ---------------------------
            // Preferred over InputKey/MouseDelta/MoveAxis for anything REBINDABLE: aver_fw_action_*
            // (framework_abi.h, Named Actions, minor 5) reads whatever keys/mouse a project bound to a
            // name via aver_fw_action_bind, so a graph asks "did the player Jump" once, independent of
            // physical key.
            //
            // `action` is an INT HANDLE, not a name -- PinType has no String member, and giving it a
            // dedicated Node property (like class=/event=/curve=, i.e. Graph.cs's ClassName/EventName/
            // CurveName) would mean editing Graph.cs, which this slice does not own. It must hold the
            // handle aver_fw_action_register/_find returned, not the registered string.
            //
            // That handle is NOT a stable compile-time constant: aver_fw_action_register appends to a
            // vector and returns its 1-based index (FrameworkAbi.cpp:1245-1253, idempotent by name via
            // :1234-1240's _find), so a literal Const int is only as reliable as registration order
            // staying fixed -- see GraphCompiler.EmitInputAction for the full accounting.
            //
            // Pure data, no exec pins, like InputKey: both native calls (_value2, _held) are array-scan
            // reads with no side effect, cheap enough to redundantly pull (same cost class as GetForward's
            // six-output read).
            case "inputaction":
                node.Pins.Add(new Pin { Name = "action", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "held", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // InputActionPressed / InputActionReleased: the action-level twin of InputKeyPressed/
            // InputKeyReleased above -- identical reasoning, identical "triggered" pin name (an EVENT,
            // not a STATE -- see that case's own comment), one layer up over aver_fw_action_pressed/
            // _released instead of aver_fw_input_key_pressed/_released.
            case "inputactionpressed":
            case "inputactionreleased":
                node.Pins.Add(new Pin { Name = "action", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "triggered", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "raycast":
                // UNLIKE Select/InputKey, Raycast gets exec pins by default: a real (read-only) native
                // query, and the PUSH compiler wants it run exactly once per exec visit, not per pull
                // (GraphCompiler.EmitExecRaycast, IsExecCapableQueryType). "then" (not "exec", to avoid
                // reusing the input pin's name) is the single continuation fired after the call completes.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "originZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dirZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "maxDist", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "hit", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pointX", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pointY", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pointZ", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // ---- MouseDelta / MoveAxis (continuous input) -----------------------------------------
            // The gap InputKey doesn't close: look and move are CONTINUOUS, not a single frame bit.
            // Both wrap one call into Aver.Framework's polled input (GraphInterop.MouseDeltaForGraph/
            // MoveAxisForGraph) as ordinary float pins, the same "one call, several scalar pins" shape
            // GetFieldVec3/Raycast use.
            //
            // Both get exec pins by default, mirroring Raycast rather than GetFieldVec3/SetFieldVec3
            // (which get none): even though neither call has Raycast's per-call cost, this slice requires
            // one frame's input to cost exactly one native call regardless of pins read, and only
            // _execLocals caching (keyed to one exec visit) guarantees that
            // (IsExecCapableMouseDeltaType/IsExecCapableMoveAxisType).
            //
            // The low-level path: reads the device directly, no name, no rebinding -- right for a raw
            // camera look or debug probe; use InputAction instead when a project wants this rebindable.
            // Existing content using these directly is unaffected by InputAction's addition.

            case "mousedelta":
                // Zero data inputs. "then" (not "exec", same reason as Raycast's continuation pin) is
                // the single continuation, fired once the read completes.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "deltaX", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "deltaY", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "wheel", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "moveaxis":
                // Same shape as mousedelta. Z is deliberately NOT a pin: Input.MoveAxis's Z is hardcoded
                // 0 always (Aver.Framework/Input.cs) -- a pin reading a known constant would add noise.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "forward", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "right", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // ---- Spawn ---------------------------------------------------------------------------
            // SIDE-EFFECTING (creates a new entity), so it gets exec pins by default, mirroring raycast --
            // the README's own spec frames this as "a Spawn(className, x, y, z) exec node" -- and is
            // refused by the PULL-only compiler ENTIRELY (GraphCompiler.IsExecCapableSpawnType), more
            // strictly than SetField/SetFieldVec3: a stray Spawn in a no-ENTRY dataflow graph would create
            // a new entity every invocation with no branch structure to gate it. class= names the registered
            // class (same key=value NODE-line mechanism as field=/param=) -- NOT a pin, since it's an
            // edit-time author choice, not runtime data, mirroring field= on GetField/SetField.
            case "spawn":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            // ---- CharacterMove -----------------------------------------------------------------------
            // The last Blueprint-parity node: one coarse, exec-only wrapper around AverCharacter.Drive
            // (via DriveFromGraph -> GraphInterop.CharacterMoveForGraph), matching the owner's chosen
            // signature exactly: CharacterMove(entity, dt, forward, right, yawDelta, pitchDelta) -> then,
            // success. Unlike Spawn, NO NODE-line attribute -- all six inputs are ordinary pins since a
            // graph computes them at RUNTIME (PARAM/MoveAxis/MouseDelta), not chosen at edit time like
            // Spawn's class=.
            //
            // SIDE-EFFECTING (mutates yaw/pitch/capsule every call), so it gets exec pins BY DEFAULT
            // like Spawn/SetVar, refused just as strictly by the PULL compiler
            // (IsExecCapableCharacterMoveType). "success" is a real outcome, never a fake stub: false
            // (Log.Warn, never a throw) for two distinct cases -- not a live actor at all, or a live actor
            // that isn't an AverCharacter (GraphInterop.CharacterMoveForGraph).
            case "charactermove":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dt", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "forward", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "right", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "yawDelta", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pitchDelta", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- Synapse ------------------------------------------------------------------------------
            // GetSynapseTarget: a PURE read of CSynapseAgent's current steering target, tracked by the
            // native AgentSystem tick (aver_fw_synapse_target, framework_abi.h). NO exec pins, same as
            // getworldposition above -- "success" is a real "nothing to head toward right now" outcome
            // (no CSynapseAgent, or its status is not Pathing), not an error.
            case "getsynapsetarget":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // SynapseSteer: PURE, turns "where am I / where to go" into the forward/right/yawDelta
            // CharacterMove consumes (GraphInterop.SynapseSteerForGraph). Takes an EXPLICIT target
            // (targetX/Y/Z), not CSynapseAgent's own, so one node does both direct chase and
            // path-following (GetSynapseTarget's output). "arrived"/"success" are separate outputs (GraphNodeDefs.hpp).
            case "synapsesteer":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "dt", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "targetX", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "targetY", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "targetZ", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "turnRate", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "arriveRadius", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "forward", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "right", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "yawDelta", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "arrived", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // GetSynapsePerception: a PURE read of CSynapsePerception's current sight state, tracked
            // by the native PerceptionSystem tick (aver_fw_synapse_perception, framework_abi.h). NO
            // exec pins, same as getworldposition above. "success" means something DIFFERENT here
            // than on every other node in this file -- see GraphNodeDefs.hpp's own comment on why.
            case "getsynapseperception":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "canSeeTarget", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "lastKnownTarget", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "timeSinceSeen", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- Audio -------------------------------------------------------------------------------
            // The graph half of a mixer that was built and then never called. sound= is a NODE-line
            // attribute (Node.SoundPath), never a pin -- PinType has no String. Playing is a side effect:
            // exec pins, refused by the pure compiler; IsSoundPlaying alone is a pure read. "voice" is 0
            // with no audio device -- supported, not an error (GraphNodeDefs.hpp).
            case "playsound":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "volume", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pitch", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "looping", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bus", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "voice", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "playsoundat":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "volume", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "pitch", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "looping", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bus", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "innerCm", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "outerCm", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "voice", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "stopsound":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "voice", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "issoundplaying":
                node.Pins.Add(new Pin { Name = "voice", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "playing", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setlistener":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setbusvolume":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "bus", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "volume", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- FireEvent -------------------------------------------------------------------------
            // GAP 3: the cross-entity event node. SIDE-EFFECTING (runs ANOTHER entity's whole exec chain) --
            // exec pins by default, mirroring Spawn/CharacterMove, refused entirely by the PULL compiler
            // for the same "no notion of 'when'" reason IsExecCapableSpawnType gives, only stronger:
            // firing mid-pull would run a stranger's exec chain every invocation with no gate.
            //
            // "target" is an ordinary int PIN (runtime-computed), unlike event=, which is edit-time data
            // (Node.EventName) -- the same pin-vs-attribute split as Spawn's xyz-pins-vs-class=. "fired"
            // is real, not a hardcoded true: false (Log.Warn) when the target has no live graph, or one
            // that never declared this event -- see GraphEvents.FireEventForGraph, including the
            // reentrancy guard against a self-fire or mutual-fire cycle stack-overflowing the process.
            case "fireevent":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "target", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "fired", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- GetVar / SetVar -------------------------------------------------------------------
            // Graph-local persistent variables -- see GraphVariable/GraphVarStore (Graph.cs) for the
            // storage/lifetime story; this is only the pin shape. var= names the declared VAR (Node.VarName),
            // the same key=value mechanism as field=/param=/class=.
            //
            // GetVar: a PURE READ, so unlike SetVar it gets NO exec pins, reachable from BOTH compilers
            // like GetField. Only ONE output, 'value', typed to the VAR's declared type -- not fixed,
            // mirroring "param"/"getparam" above. If var= is missing/undeclared, add NO pin: Graph.Validate()
            // reports the specific reason instead of a generic "no output pin 'value'" downstream.
            // ---- FUNCTIONS -------------------------------------------------------------------------
            // All three derive their pins from a DECLARATION elsewhere (like Param/GetVar/SetVar above),
            // making a function's signature a single source of truth: change a FUNCIN and every call
            // node's pins change on next load, rather than drifting. A PURE function has no exec pins
            // anywhere (entry/return/call); an impure one has them in all three -- no half-way state,
            // the point of declaring purity once on FUNC instead of inferring it three times.
            case "funcentry":
            {
                var fn = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.FuncOwner, StringComparison.OrdinalIgnoreCase));
                if (fn == null) break;   // undeclared owner: Validate names it; do not invent pins over the top
                if (!fn.IsPure)
                    node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                foreach (var p in fn.Inputs)
                    node.Pins.Add(new Pin { Name = p.Name, Type = p.Type, IsOutput = true, NodeId = node.Id });
                break;
            }

            case "funcreturn":
            {
                var fn = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.FuncOwner, StringComparison.OrdinalIgnoreCase));
                if (fn == null) break;
                if (!fn.IsPure)
                    node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                foreach (var p in fn.Outputs)
                    node.Pins.Add(new Pin { Name = p.Name, Type = p.Type, IsOutput = false, NodeId = node.Id });
                break;
            }

            case "callfunc":
            {
                // Keyed off call=, NOT func= -- see Node.CallTarget for why a call node needs both.
                var fn = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.CallTarget, StringComparison.OrdinalIgnoreCase));
                if (fn == null) break;
                if (!fn.IsPure)
                {
                    node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                    node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                }
                foreach (var p in fn.Inputs)
                    node.Pins.Add(new Pin { Name = p.Name, Type = p.Type, IsOutput = false, NodeId = node.Id });
                foreach (var p in fn.Outputs)
                    node.Pins.Add(new Pin { Name = p.Name, Type = p.Type, IsOutput = true, NodeId = node.Id });
                break;
            }

            case "getvar":
            {
                var declaredVar = graph.Variables.FirstOrDefault(v => v.Name == node.VarName);
                if (declaredVar != null)
                    node.Pins.Add(new Pin { Name = "value", Type = declaredVar.Type, IsOutput = true, NodeId = node.Id });
                break;
            }

            // SetVar: a write is a side effect (IsExecCapableVarSideEffectType), so it gets exec pins BY
            // DEFAULT, mirroring Spawn/Raycast rather than SetField (or GetField/GetFieldVec3/SetFieldVec3)
            // -- there's no "idempotent overwrite" excuse SetField has, so a freshly palette-spawned node
            // must already be usable. 'value' is typed to
            // the declared VAR's type and added only when var= resolves (like GetVar above), so an
            // undeclared variable still gets Graph.Validate()'s specific error, not a mistyped default pin.
            case "setvar":
            {
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                var declaredVar = graph.Variables.FirstOrDefault(v => v.Name == node.VarName);
                if (declaredVar != null)
                    node.Pins.Add(new Pin { Name = "value", Type = declaredVar.Type, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;
            }

            // ---- SetParent / SetViewEntity / SetName -----------------------------------------------
            // Three one-ABI-call writes, DISPATCHED SetField-STYLE, not Spawn/SetVar-style: no exec pins
            // by default (wireable into a pure dataflow like GetField/SetField), and EmitNode runs them
            // unconditionally on Compile()'s topological pass like EmitSetField (see
            // IsExecCapableSetParentType/SetViewEntityType/SetNameType). Same excuse as SetField:
            // aver_scene_set_parent/set_name/aver_fw_set_view_entity are REPUBLISH operations, harmless
            // and idempotent every tick, unlike Spawn (new entity each call) or SetVar (no such excuse).
            // Still fully refused when PULLED with no exec visit in an ENTRY-driven graph
            // (EmitPullOutput names all three, like SetField/SetFieldVec3/Spawn/SetVar); only Compile()'s
            // separate topological compiler gets the idempotent-overwrite exception.
            case "setparent":
                // aver_scene_set_parent(child, parent) -> success. "child"/"parent" name the ABI's own
                // parameters (scene_abi.h:105) rather than "entity"/"target", matching the native signature.
                node.Pins.Add(new Pin { Name = "child", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "parent", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setviewentity":
                // aver_fw_set_view_entity(entity) -> void (framework_abi.h:206). No output pin at all,
                // deliberately: the ABI returns nothing, and a fake "success" pin would repeat the
                // exact mistake SetField's own comment says it already fixed (hardcoded 1 regardless).
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                break;

            case "setname":
                // aver_scene_set_name(entity, name) -> success. name= is a NODE-line attribute
                // (Node.NameValue), not a pin -- PinType has no String, same as class= for Spawn.
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- SetMesh / SetMaterial ---------------------------------------------------------------
            // Wrap Aver.Framework.Entity.SetMesh/SetMaterial (EntityScene.cs) via GraphInterop.SetMeshForGraph/
            // SetMaterialForGraph -- not a generic I64-capable SetField, not an exposed asset lookup
            // (Assets.ObjectIdOf is a pure local FNV1a64 hash, no native call, no I/O), not an
            // "add a missing component" node (EntityScene.EnsureMeshRenderer already does that). Same
            // SetField-style dispatch as SetParent/SetViewEntity/SetName: EnsureMeshRenderer's own
            // "already present, do nothing" guard makes re-running this every tick harmless.
            case "setmesh":
                // mesh= names the asset path (see Node.MeshPath), the same NODE-line-attribute-as-data
                // mechanism name= established for SetName just above.
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setmaterial":
                // material= names the material (see Node.MaterialName).
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- AttachToSocket ----------------------------------------------------------------
            // TWO entity inputs, unlike any other Set*-shaped node: an attachment relates two things
            // the graph already holds. socket= names the socket on the parent's rig.
            case "attachtosocket":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "parent", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- GetAnimCurve ------------------------------------------------------------------
            // A pure read: no exec pins, no exec emitter, no IsExecCapable predicate needed.
            case "getanimcurve":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // ---- SetSkeleton / PlayAnimation ----------------------------------------------------
            // SetMesh/SetMaterial's animation-family siblings -- same SetField-style dispatch, same
            // idempotent add-if-absent reason. skeleton=/clip= name the asset paths (Node.SkeletonPath/
            // ClipPath); GraphNodeDefs.hpp notes the array-index bone-binding caveat neither enforces.
            case "setskeleton":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // PlayAnimation gets a THIRD pin, loop, since Entity.PlayAnimation takes a second scalar
            // argument -- a pin, not a NODE-line attribute, because it's runtime data (Node.ClipPath).
            case "playanimation":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "loop", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // SetControlRig: shaped like PlayAnimation, not SetSkeleton -- a float weight pin, since
            // rig= is edit-time naming but weight is runtime data (component reached by NAME, not a
            // Component enum value -- GraphNodeDefs.hpp).
            case "setcontrolrig":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "weight", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
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

    /// Reads a `x,y,z` value into `into`, leaving any unparsed component at the caller's existing
    /// value -- deliberate: `scale` arrives pre-filled 1,1,1, so `scale=2,,2` gives 2,1,2, not a
    /// collapsed actor, and a partially-typed value mid-edit never vanishes a component. Fewer
    /// than three parts fills what's there (`pos=0,0` keeps its default z).
    private static void ParseVec3(string text, float[] into)
    {
        string[] parts = text.Split(',');
        for (int i = 0; i < parts.Length && i < 3; i++)
            if (float.TryParse(parts[i], NumberStyles.Float, CultureInfo.InvariantCulture, out float f))
                into[i] = f;
    }

    private static int ParseI32(string s, int dflt = 0)
    {
        return int.TryParse(s, out var i) ? i : dflt;
    }
}
