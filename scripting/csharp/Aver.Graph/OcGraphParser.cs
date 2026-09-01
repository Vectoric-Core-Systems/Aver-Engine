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
            else if (key.Equals("DOMAIN", StringComparison.OrdinalIgnoreCase))
            {
                // DOMAIN <name> -- which language this graph's nodes are written in, so that the
                // passes which walk every *.ocgraph under a project (HostBridge.DeclareGraphClasses,
                // GameApp's discoverProjectGraphs) can tell one of ours from a material graph. See
                // Graph.DomainKind and, for the authoritative account, aver::fmt::OcGraphDomain.
                //
                // Refused when it names nothing, matching the C++ reader exactly: absent and
                // present-but-blank mean opposite things to DomainKind, so a file that says the word
                // and then says nothing has a defect worth reporting rather than silently becoming a
                // gameplay graph.
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
            else if (key.Equals("VAR", StringComparison.OrdinalIgnoreCase))
            {
                // VAR <name> <type> [default]
                //
                // Declares one variable the GRAPH remembers between ticks -- the opposite of PARAM
                // immediately above: a PARAM is supplied fresh by the CALLER on every invocation; a VAR
                // is owned by the graph/host and its value survives from one compiled-delegate
                // invocation to the next, on the SAME GraphHost instance. See GraphVariable's own
                // comment (Graph.cs) and GraphVarStore's own comment for the full storage/lifetime
                // contract -- this parser owns only the FORMAT half of that story.
                //
                // A NEW record, not an unknown one, for the identical reason PARAM's own comment gives:
                // the C++ side does not parse VAR at all today, so a VAR line classifies as an unowned
                // "Other" line in OcGraph.cpp's classifyLine and round-trips verbatim, in place, through
                // the same whole-file unknown-record preservation PARAM already relies on -- verified by
                // building and running OcGraphTest.exe's --roundtrip diagnostic against a hand-written
                // fixture carrying VAR records, not merely by reading the C++ source (see
                // tests/formats/src/OcGraphTest.cpp's testVarRecordsSurviveRoundTrip for the checked-in
                // version of that proof).
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
                // VAR is DATA a graph remembers between ticks, not control flow -- rejected here, at
                // parse time, for the identical reason PARAM rejects Exec just above (a confusing
                // runtime failure three layers into GraphCompiler beats a clear one right here).
                if (varType == PinType.Exec)
                {
                    err = $"VAR '{varName}' cannot be declared exec -- VAR is for data a graph " +
                          "remembers between ticks, not control flow; control flow is expressed with " +
                          "exec pins and LINK records, not a stored variable";
                    return false;
                }

                // TYPED BY THE DECLARED TYPE, NOT BY THE LITERAL'S SHAPE -- the exact lesson PIN's own
                // default-value parsing paid for already (see that comment, below). UNLIKE PIN's hard
                // "no default at all" fallback, an unparseable VAR default falls back to the type's own
                // zero value instead: GraphVarStore.CreateFor needs a concrete Default to seed from
                // (never null), and a graph author fat-fingering a default is far more likely than the
                // whole record being garbage -- the graph should still load, just with 0/0f/false for
                // that one variable.
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
                    // else: the literal did not parse as the declared type -- keep the zero-value
                    // fallback already assigned above rather than failing the whole graph over one
                    // malformed default.
                }

                graph.Variables.Add(new GraphVariable { Name = varName, Type = varType, Default = varDefault });
            }
            else if (key.Equals("COMP", StringComparison.OrdinalIgnoreCase))
            {
                // COMP <id> <Kind> [parent=<id>] [pos=x,y,z] [rot=yaw,pitch,roll] [scale=x,y,z] [key=value]...
                //
                // One child entity of a spawned class instance. See GraphComponent (Graph.cs) for
                // what each field means and why the transform is typed here while the C++ reader
                // keeps the whole line as tokens.
                //
                // THE STRUCTURAL RULES ARE NOT RE-CHECKED HERE. Duplicate ids, a parent naming
                // nothing, and a parent cycle are all refused by modules/formats' reader, which is
                // the one that runs when the editor opens a file. Repeating them would mean two
                // implementations of the same rule that can drift; what this side owns is the
                // meaning of a Kind and the units of a transform.
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
                // FUNCIN <func> <pin> <type> / FUNCOUT <func> <pin> <type> -- one argument or one
                // return, in declaration order. Two records rather than one with a direction token,
                // because the ORDER of inputs and the ORDER of outputs are independent lists and a
                // single interleaved record would make the file say which came first when nothing
                // depends on that.
                bool isIn = key.Equals("FUNCIN", StringComparison.OrdinalIgnoreCase);
                if (tokens.Count < 4)
                {
                    err = $"{key.ToUpperInvariant()} requires a function, a pin name and a type: {key.ToUpperInvariant()} func pin float|int|bool";
                    return false;
                }
                var owner = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, tokens[1], StringComparison.OrdinalIgnoreCase));
                if (owner == null)
                {
                    // Declared BEFORE its FUNC is a genuine error, not a forward reference: unlike a
                    // LINK naming a later NODE, there is no second pass here that could resolve it,
                    // and silently dropping the argument would produce a function with the wrong
                    // arity and no report of why.
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
                    // Exec is not a value. A function's control flow is decided by its `pure` flag,
                    // which is one place, rather than by an author declaring an exec argument here and
                    // a `pure` flag there that could disagree.
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
                //
                // Declares that THIS GRAPH FILE IS A SPAWNABLE ACTOR CLASS -- the Aver Node analogue
                // of a Blueprint asset carrying a parent class, not a component that references a
                // graph. See Graph.ClassName's own doc comment for the full "who consumes this and
                // why" story; this block owns only the FORMAT half.
                //
                // A NEW record, not an unknown one, for the identical reason PARAM/VAR's own comments
                // give (see those, just below/above): the C++ reader has no "Class" case, so it rides
                // through as an OwnedLineKind::Other line and round-trips verbatim.
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
                // Defaults to "Actor" when omitted -- mirrors HostBridge's own BaseRegistryName
                // default for a plain, component-less AverActor, so `CLASS Foo` alone (no parent
                // token at all) is a complete, sealable, spawnable declaration.
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
                        // firstperson/thirdperson -- see Graph.ClassView's own doc comment for why
                        // this exists and who consumes it (HostBridge.DispBind, Character ancestors
                        // only). Held as the raw string here, same as mesh/material; validated later.
                        else if (parts[0] == "view") classView = parts[1];
                        // The GameMode's default pawn CLASS NAME -- see Graph.ClassPawn for why a
                        // graph GameMode needed a way to say this at all. A class name, not a file
                        // path: it is resolved against declared classes at seal, not on disk.
                        else if (parts[0] == "pawn") classPawn = parts[1];
                        // The GameMode's player-controller class. Needed alongside pawn= for either to
                        // matter -- see Graph.ClassController for why possession requires both.
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
                        // PARSED BY THE NODE'S DECLARED TYPE, not by the shape of the literal.
                        //
                        // This used to guess -- true/false, then int.TryParse, then float.TryParse --
                        // and the guess silently produced the WRONG CLR TYPE for a value written the
                        // wrong-looking way. Both compilers read a ConstFloat's constant with a strict
                        // `is float f` and fall back to 0 when it does not match, with no warning, so:
                        //
                        //   NODE speed ConstFloat value=100    boxed an int, compiled to 0.0f
                        //   NODE n     ConstInt   value=7.0    boxed a float, compiled to 0
                        //   NODE b     ConstBool  value=1      boxed an int, compiled to false
                        //
                        // All three silently, in BOTH the PULL and PUSH compilers. A raycast direction,
                        // a movement speed or a timer duration written without a decimal point simply
                        // became zero and nothing said so.
                        //
                        // It survived because every sample and test in this tree writes float literals
                        // with an explicit decimal point by convention, so int.TryParse never got the
                        // chance to win. It was found twice independently -- once while building
                        // acceptance evidence for graph variables, and once from a throwaway raycast
                        // probe whose inputs all silently read zero.
                        //
                        // The `PIN <node> value out <type> <literal>` path a few hundred lines below
                        // never had this problem: it branches on the pin's DECLARED type before
                        // parsing. This is that same rule, applied where the type is equally well
                        // known -- nodeType is right there.
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
                            // A value= on any OTHER node type keeps the old shape-based inference. It
                            // has no declared type to consult, and nothing in the vocabulary reads one
                            // today -- narrowing it would be a guess in the other direction.
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
                    // class= names the registered class a "spawn" node creates an instance of (see
                    // Node.ClassName). Resolved by NAME at invocation time (GraphInterop.SpawnForGraph),
                    // not baked to a handle here or at compile time -- see that method's own comment.
                    else if (k == "class")
                    {
                        node.ClassName = v;
                    }
                    // var= names which declared VAR a "getvar"/"setvar" node addresses (see
                    // Node.VarName). No external table to resolve against -- Graph.Validate() checks it
                    // directly against Variables, not here.
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
                    // name= carries the literal string a "setname" node writes (see Node.NameValue) --
                    // the value itself, not a lookup key, unlike field=/class=/var= above. Nothing to
                    // resolve at parse time; GraphCompiler.EmitSetName/EmitExecSetName require it
                    // non-empty at COMPILE time (an empty name= can never write anything useful, so
                    // failing loudly then beats a silent no-op rejection at runtime for a reason nobody
                    // can see -- mirrors class='s own required-at-compile-time treatment for Spawn).
                    else if (k == "name")
                    {
                        node.NameValue = v;
                    }
                    // sound= names the audio file a "playsound"/"playsoundat" node plays. Same
                    // treatment as name=/path= above: the value IS the data, and there is no string
                    // pin it could arrive on. See Node.SoundPath.
                    else if (k == "sound")
                    {
                        node.SoundPath = v;
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
                    // clip= names the asset path a "playanimation" node plays (see Node.ClipPath) --
                    // same treatment as skeleton= immediately above. loop is NOT parsed here -- it is a
                    // pin value, not a NODE-line attribute; see Node.ClipPath's own comment for why.
                    else if (k == "clip")
                    {
                        node.ClipPath = v;
                    }
                    // event= names which event a "fireevent" node fires on another entity's graph (see
                    // Node.EventName) -- the value itself (an event name, e.g. "OnHit"), not a lookup
                    // key, the same "carries data" treatment name=/mesh=/material= already get. Nothing
                    // to resolve at parse time; GraphCompiler.EmitExecFireEvent requires it non-empty at
                    // COMPILE time, mirroring class='s own required-at-compile-time treatment for Spawn.
                    else if (k == "event")
                    {
                        node.EventName = v;
                    }
                    // path= names the file a "savegame" node writes or a "loadgame" node reads (see
                    // Node.SavePath) -- the value itself, not a lookup key, the same "carries data"
                    // treatment name=/mesh=/material=/event= already get. Nothing to resolve at parse
                    // time; GraphCompiler.EmitExecSaveLoad requires it non-empty at COMPILE time,
                    // mirroring every other required-at-compile-time attribute above.
                    else if (k == "path")
                    {
                        node.SavePath = v;
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

        // WHICH NODE STARTS AND ENDS EACH FUNCTION, resolved from the nodes rather than declared in
        // the FUNC record. A function has exactly one FuncEntry and at most one FuncReturn (Validate
        // refuses a second of either), so naming them on the FUNC line as well would be a second place
        // for the file to disagree with itself. Runs BEFORE AddDefaultPins because the entry node's
        // pins are the function's inputs and it has to know which function it belongs to first.
        foreach (var node in graph.Nodes.Values)
        {
            if (string.IsNullOrEmpty(node.FuncOwner)) continue;
            var owner = graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.FuncOwner, StringComparison.OrdinalIgnoreCase));
            if (owner == null) continue;   // reported by Validate, which can name every offender at once
            string t = node.Type.ToLowerInvariant();
            if (t == "funcentry" && owner.EntryNodeId == null) owner.EntryNodeId = node.Id;
            else if (t == "funcreturn" && owner.ReturnNodeId == null) owner.ReturnNodeId = node.Id;
        }

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

            // ---- boolean logic ----------------------------------------------------
            // THE VOCABULARY HAD NO BOOLEAN OPERATORS AT ALL until these, which is why every
            // graph in the tree expresses "A and B" as a Branch whose true-exec runs another
            // Branch. That works and is unreadable, and it cannot express OR or NOT at all
            // without inverting the whole downstream structure.
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
            // `compare` is a strict a > b and nothing else, so a >= b needed Compare plus Not,
            // and equality was not expressible at all.
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

            // Print(value) -> then. EXEC PINS BY DEFAULT: writing a line to the log is a side
            // effect with a definite "when", so it belongs on the chain the way SetVar does and
            // must never be pulled -- a pulled Print would fire once per reader, or not at all.
            // Vector maths: loose float components in, loose float components out. NO exec
            // pins -- these are pure, so they are safe to pull as often as anything asks, the
            // same reasoning GetFieldVec3 above carries.
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

            // The engine API nodes. The four that WRITE (SetVelocity, Teleport, Possess,
            // Unpossess) carry exec pins because they are side effects with a definite "when";
            // the readers do not, so they are safe to pull as often as anything asks.
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

            // SaveGame / LoadGame: no entity pin at all, unlike everything else in this file --
            // both act on the WHOLE world, not on one thing in it. path= names the file (see
            // Node.SavePath); nothing else about the shape differs from Possess/Unpossess just above.
            case "savegame":
            case "loadgame":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
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

            // Same pin shape as setlocalscale below, deliberately: entity + xyz in, then/success out.
            // A node that moves a child and a node that resizes one should not need to be learned
            // twice.
            case "setlocalposition":
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

            // CreateEntity / FindEntity -- SetName's own name= family. Both take their string from
            // the NODE line (Node.NameValue, already parsed for SetName), never a pin: PinType has
            // no String member. CreateEntity is exec (it makes something); FindEntity is a pure
            // lookup, and "found" is a real answer rather than an error -- see GraphNodeDefs.hpp.
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

            // Joints. Creators mirror addstaticbox/etc above: exec + params in, then + a handle out
            // (here "joint" rather than "body"). See GraphNodeDefs.hpp's own JOINTS banner for why
            // bodyB == 0 means the world, and why Hinge/Slider each carry a second (normal) axis.
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

            // Tags and visibility. A tag is an int bitmask -- see GraphInterop's own comment for why
            // that spared this family the compile-time-attribute machinery every string-shaped node needs.
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
            // (CLocal.position, CLocal.scale, CLight.colour, ...) read or written as three ORDINARY
            // float pins rather than one new pin TYPE. field= is reused verbatim (same NODE-line
            // attribute, same generic key=value parsing above -- nothing here is Vec3-specific about
            // how the attribute survives to GraphCompiler). No exec pins by default on EITHER, mirroring
            // getfield/setfield exactly, INCLUDING setfieldvec3 (a write) having none -- see
            // GraphCompiler.EmitSetFieldVec3's own comment for why that is deliberate, not an oversight.
            case "getfieldvec3":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // GetForward(entity) -> x,y,z (unit look direction) + eyeX,eyeY,eyeZ (where to fire FROM)
            // + success. NO exec pins by default, like GetFieldVec3 and for the same reason: it is a
            // pure, idempotent read of the character's own state, so it is safe to pull as often as
            // anything asks. The eye position rides along because a direction with no origin cannot
            // build a ray -- see GraphInterop.LookDirectionForGraph for why both halves are one call.
            // Jump(entity) -> jumped. EXEC PINS BY DEFAULT, unlike GetForward/GetViewEntity beside it:
            // jumping is a side effect with a definite "when", so it belongs on the chain the way
            // CharacterMove and Spawn do, not pulled as data. `jumped` is false when the character was
            // airborne -- AverCharacter.Jump refuses in mid-air -- which is ordinary, not an error.
            case "jump":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "jumped", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // GetViewEntity(entity) -> view + success. The CAMERA node a character looks through, which
            // is what a first-person viewmodel must be parented to -- parent it to the character and it
            // stays put while the camera pitches around it. Pure read, no exec pins, like GetForward.
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

            // ---- gated flow control -------------------------------------------------
            // These three REMEMBER something between activations, which no node here did
            // before: Branch and Sequence decide from their inputs alone. The state lives in
            // the same per-instance GraphVarStore a VAR uses, under a reserved name built
            // from the node id, so two entities running one graph file gate independently --
            // the property GraphVarStore's own header calls the one most likely to be
            // silently undone.
            case "doonce":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                // reset is a BOOL, not an exec input, and that is forced by the compiler rather
                // than chosen: an activation reaches a node through EmitExecNode, which is not
                // told WHICH input pin it arrived on, so two exec inputs would be
                // indistinguishable inside the emitter. Sampling a bool every activation says
                // the same thing and can actually be implemented. Same reasoning as Gate.
                node.Pins.Add(new Pin { Name = "reset", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "gate":
                // `open`/`close` are BOOL inputs rather than exec pins: an exec input can be
                // driven by many sources here, so three separate exec entries would make
                // "which one fired" unanswerable inside one activation.
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

            case "customevent":
                // A USER-NAMED ENTRY POINT. Identical shape to onstart/ontick/onhit -- a bare exec
                // output and nothing else -- and identical treatment in the compiler, because the
                // node type has never been what makes an event fire. The top-level `ENTRY <nodeId>
                // <eventName>` record is, and this node exists so an author can write one whose
                // event name is theirs rather than one of three the palette happened to ship.
                //
                // The name lives on the ENTRY record, not here. A `name=` attribute on the NODE
                // line is what the EDITOR shows and keeps in sync (GraphEditor writes both), but
                // nothing at runtime reads it -- CompileEntryPoint matches on the ENTRY record, so
                // a graph hand-written with only an ENTRY and no `name=` runs exactly the same.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                break;

            case "onhit":
                // Same bare-trigger shape as onstart/ontick -- one exec-output pin, no inputs -- for
                // the same reason: this node TYPE is just a labeled starting point an ENTRY record
                // points at; it carries no data of its own. What makes THIS trigger fire ON DEMAND
                // (a caller invoking Aver.Graph.GraphHost.Fire, rather than the fixed Tick() cadence
                // OnStart/OnTick get) lives one layer up, in GraphHost -- nothing about the FORMAT or
                // this parser treats "onhit" as special versus any other non-OnStart/OnTick ENTRY
                // event name a project might declare (see ENTRY's own comment, above, for why a new
                // event name is free at this layer). A payload this event wants to carry (who hit
                // whom, where, how hard) is an ordinary PARAM the graph declares and reads with a
                // `param` node, exactly like OnTick's deltaTime -- not a special pin here either.
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

            // ---- Select / InputKey / Raycast -----------------------------------------------------
            // All three have real DATA outputs (unlike branch/while/foreach, see
            // GraphCompiler.IsExecOnlyNodeType), so Compile() -- the PULL/dataflow compiler -- never
            // skips them; they are handled by BOTH compilers. See GraphCompiler.cs's own
            // EmitSelect/EmitInputKey/EmitRaycast comments for exactly how each one differs between
            // the two.

            case "select":
                // A pure data node, no exec pins at all -- picks one of two float values by a bool
                // condition. See GraphCompiler.EmitSelect's own comment for why BOTH ifTrue and
                // ifFalse are computed regardless of cond in the PULL compiler (not a bug, and not
                // short-circuiting the way Branch's exec fan-out is).
                node.Pins.Add(new Pin { Name = "cond", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ifTrue", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "ifFalse", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "result", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "inputkey":
                // Also a pure data node, no exec pins: reading polled input state is idempotent (no
                // side effect), so -- like GetField -- it is safe to pull as often as anything wants,
                // through either compiler, with no _execLocals caching needed. See
                // GraphCompiler.EmitInputKey.
                //
                // InputAction, below, is this node's newer NAMED sibling -- prefer it for anything a
                // project wants to REBIND without touching the graph. This node stays for the literal
                // key code case (and for content authored before InputAction existed) -- see
                // "InputAction / InputActionPressed / InputActionReleased"'s own comment for the trade.
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "down", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- edge-triggered input --------------------------------------------------------
            // THE HALF InputKey CANNOT EXPRESS. `down` answers "is it held", which is the wrong
            // question for most of what a graph does with a key: jumping, firing a semi-auto,
            // toggling a light, opening a menu. Held-means-true fires those every frame the key is
            // down, so a graph author's only recourse was a hand-built DoOnce-and-a-variable
            // rising-edge detector -- five nodes for a thing the framework ABI has always
            // answered directly (aver_fw_input_key_pressed / _released, which is what
            // Input.GetKeyDown/GetKeyUp already wrap for C#).
            //
            // Same pin shape as inputkey, and no exec pins for the same reason: the answer is
            // computed from this frame's and last frame's state, so asking twice within one frame
            // gives the same answer and there is nothing to cache.
            case "inputkeypressed":
            case "inputkeyreleased":
                node.Pins.Add(new Pin { Name = "key", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "triggered", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- InputAction / InputActionPressed / InputActionReleased ---------------------------
            // THE PREFERRED PATH over InputKey/MouseDelta/MoveAxis above, for anything a project wants
            // REBINDABLE rather than baked to a literal key or a raw device axis: aver_fw_action_*
            // (framework_abi.h's Named Actions section, minor 5 -- aver_fw_action_register/find/bind/
            // value2/held/pressed/released) reads whatever mix of keys/mouse a project's own setup code
            // bound to a name via aver_fw_action_bind, so a graph asks "did the player Jump" once and
            // never again cares which physical key that project -- or a later rebinding -- happens to
            // use.
            //
            // `action` IS AN INT HANDLE, NOT A NAME, and that is a real limitation worth spelling out
            // rather than leaving implicit. PinType has no String member (Graph.cs's own PinType enum:
            // Float/Int/Bool/Exec only), and the established "author-chosen string, resolved by NAME at
            // invocation" mechanism this format already has for exactly this situation -- Spawn's
            // class=, FireEvent's event=, GetAnimCurve's curve= -- lives on a dedicated Node property
            // (Graph.cs's ClassName/EventName/CurveName, set from a NODE-line attribute), and adding one
            // more of those means editing Graph.cs and this parser's key=value attribute loop both --
            // Graph.cs is a file this slice does not own (see the file's own exclusive-ownership list).
            // So, like InputKey's own `key` pin above, `action` is a plain Int the graph must already
            // hold a HANDLE for: the value aver_fw_action_register/_find returned, NOT the string that
            // was registered.
            //
            // UNLIKE a VK_/AVER_FW_KEY_* code, that handle is NOT a stable compile-time constant:
            // aver_fw_action_register appends to a vector and hands back its 1-based index
            // (FrameworkAbi.cpp:1245-1253's actionDefs()/aver_fw_action_register, idempotent by name --
            // FrameworkAbi.cpp:1234-1240's aver_fw_action_find), so the number assigned to "Jump"
            // depends on how many OTHER actions a project's own code had already registered earlier
            // that same run. A literal Const int authored into a .ocgraph file today is therefore only
            // as reliable as that registration order staying fixed -- see GraphCompiler.EmitInputAction's
            // own comment for the full accounting and what a real name pin would need instead.
            //
            // Pure data, no exec pins -- like InputKey just above, NOT like MouseDelta/MoveAxis below:
            // both native calls behind InputAction (aver_fw_action_value2, aver_fw_action_held) are
            // array-scan reads over actionBindings() with no side effect, the same "cheap enough to
            // redundantly pull" cost class GetForward's own comment already accepts for a six-output
            // read -- not a per-call cost that would justify MouseDelta's _execLocals caching shape.
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
                // UNLIKE Select/InputKey, Raycast DOES get exec pins by default: it is a real (if
                // read-only) native query, and the PUSH compiler wants to run it exactly once per
                // exec visit rather than once per pull -- see GraphCompiler.EmitExecRaycast and
                // IsExecCapableQueryType's own comment for why that matters even without a true side
                // effect. "then" (not "exec", to avoid reusing the input pin's own name for an
                // unrelated output pin) is this node's single continuation, fired after the native
                // call completes -- the same "one exec-out, EmitExecFanOut needs no special case"
                // shape SetField would have if given exec pins by hand.
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
            // The gap InputKey does NOT close: InputKey covers digital key state, but look and move --
            // the two things a first-person controller is made of -- are CONTINUOUS, not a single
            // this-frame-or-not bit. Both wrap ONE call into Aver.Framework's polled input (see
            // Aver.Framework.GraphInterop.MouseDeltaForGraph/MoveAxisForGraph's own comments) as a
            // handful of ordinary float pins, the same "one native call, several scalar pins" shape
            // GetFieldVec3/Raycast already established -- no new pin TYPE needed here either.
            //
            // BOTH get exec pins by default, mirroring Raycast rather than GetFieldVec3/SetFieldVec3
            // (which get none) -- see GraphCompiler.IsExecCapableMouseDeltaType/
            // IsExecCapableMoveAxisType's own comments for exactly why: even though neither read has
            // Raycast's kind of per-call COST (both are memcpy/GetKey-class, the same cost class
            // GetFieldVec3 itself reads under without caching), this slice's own requirement is that
            // one frame's input costs exactly one native call regardless of how many output pins a
            // graph reads, and the PUSH compiler only has one mechanism that guarantees that:
            // _execLocals caching keyed to a single exec visit, exactly like Raycast's.
            //
            // THE LOW-LEVEL PATH, now that InputAction exists (see that case's own comment, below
            // InputKeyPressed/InputKeyReleased): these two read the device DIRECTLY, with no name and
            // no rebinding in between -- exactly right for a raw camera look or a debug probe, and
            // exactly wrong for anything a project wants a player (or a future rebinding UI) to
            // reconfigure, where InputAction is the one to reach for instead. Behaviour here is
            // UNCHANGED by InputAction's addition; existing .ocgraph content wires MouseDelta/MoveAxis
            // directly and must keep doing exactly what it always did.

            case "mousedelta":
                // Zero data inputs -- nothing to read before the call. "then" (not "exec", for the
                // same reason Raycast's own continuation pin isn't named "exec" either -- see that
                // case's comment) is this node's single continuation, fired once the read completes.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "deltaX", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "deltaY", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "wheel", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            case "moveaxis":
                // Same shape as mousedelta above. Z is deliberately NOT a pin: Input.MoveAxis's own Z
                // component is hardcoded 0 always (Aver.Framework/Input.cs), so a pin that could only
                // ever read a compile-time-known constant would add noise, not information.
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "forward", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "right", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // ---- Spawn ---------------------------------------------------------------------------
            // SIDE-EFFECTING (creates a new scene entity), so -- UNLIKE getfieldvec3/setfieldvec3
            // above, which get no exec pins by default -- this DOES get exec pins by default, mirroring
            // raycast: the README's own spec frames this as "a Spawn(className, x, y, z) exec node",
            // and GraphCompiler.IsExecCapableSpawnType's own comment explains why it is refused by the
            // PULL-only compiler ENTIRELY, more strictly than SetField/SetFieldVec3 are -- a stray Spawn
            // in a no-ENTRY dataflow graph would create a new entity on every single invocation, with no
            // branch structure available to gate it. class= names which registered class to spawn (the
            // same generic key=value NODE-line attribute field=/param= already use -- see the NODE
            // parsing loop above) -- NOT a pin, because a class name is something the graph AUTHOR
            // chooses at edit time, not something an upstream node computes at runtime, mirroring how
            // field= is not a pin on GetField/SetField either.
            case "spawn":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "x", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "y", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "z", Type = PinType.Float, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = true, NodeId = node.Id });
                break;

            // ---- CharacterMove -----------------------------------------------------------------------
            // The last Blueprint-parity node: ONE coarse, exec-only wrapper around
            // AverCharacter.Drive (reached via AverCharacter.DriveFromGraph ->
            // GraphInterop.CharacterMoveForGraph -- see that method's own comment), matching the
            // owner's chosen signature exactly: CharacterMove(entity, dt, forward, right, yawDelta,
            // pitchDelta) -> then, success. UNLIKE Spawn just above, there is NO NODE-line attribute
            // here at all -- every one of the six inputs is an ordinary pin, because a graph author
            // computes dt/forward/right/yawDelta/pitchDelta at RUNTIME (a PARAM, a MoveAxis, a
            // MouseDelta), never chooses them at edit time the way Spawn's class= names a class.
            //
            // SIDE-EFFECTING (moves a real actor, mutates its yaw/pitch/capsule state every call), so
            // -- like Spawn/SetVar, unlike GetField/SetField -- this gets exec pins BY DEFAULT; see
            // GraphCompiler.IsExecCapableCharacterMoveType's own comment for why it is refused by the
            // pure-dataflow (PULL) compiler exactly as strictly as Spawn is.
            //
            // "success" is a REAL outcome, never a fake always-true stub: false (with a Log.Warn line,
            // never a throw, never a silent no-op) when the entity is not a live actor at all, or is a
            // live actor that is not an AverCharacter -- see GraphInterop.CharacterMoveForGraph's own
            // comment for the two distinct failure messages.
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

            // SynapseSteer: a PURE node turning "where am I, where do I want to go" into the
            // forward/right/yawDelta charactermove above already consumes -- see
            // GraphInterop.SynapseSteerForGraph's own comment for the full contract. Takes an EXPLICIT
            // target (targetX/Y/Z), never CSynapseAgent's own: the identical node does direct chase and
            // path-following (GetSynapseTarget's own output) for that reason. "arrived" and "success"
            // are deliberately separate outputs -- see GraphNodeDefs.hpp's own comment on why.
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
            // The graph half of connecting a mixer that was built and then never called. sound= is a
            // NODE-line attribute (Node.SoundPath), never a pin -- PinType has no String member.
            // Playing is a SIDE EFFECT, so these carry exec pins and the pure compiler refuses them;
            // IsSoundPlaying alone is a pure read. "voice" is 0 when there is no audio device, which
            // is supported rather than an error -- see GraphNodeDefs.hpp's own comment.
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
            // GAP 3: the cross-entity event node. SIDE-EFFECTING (runs ANOTHER entity's whole exec
            // chain, not merely a scalar write) -- exec pins by default, mirroring Spawn/CharacterMove
            // rather than SetField, and refused by the pure-dataflow (PULL) compiler entirely, for
            // the identical "no notion of 'when'" reasoning IsExecCapableSpawnType's own comment
            // gives, only stronger: firing an event mid-pull would run a stranger's exec chain on
            // every single invocation with no branch structure to gate it.
            //
            // "target" is the entity whose graph should receive the event -- an ordinary int PIN
            // (computed at runtime: a Spawn's entity output, a VAR, a Raycast's own entity pin),
            // unlike event=, which is edit-time data (see NODE-line parsing above, Node.EventName) --
            // exactly the same "pin vs attribute" split Spawn's x/y/z-pins-vs-class=-attribute already
            // established. "fired" is a REAL outcome, never a hardcoded true: false (with a Log.Warn
            // naming the entity and event) when the target has no live graph at all, or has one that
            // never declared this event -- see GraphEvents.FireEventForGraph's own comment for the
            // full failure-mode table and the reentrancy guard that keeps a self-fire or a mutual-fire
            // cycle from stack-overflowing the process.
            case "fireevent":
                node.Pins.Add(new Pin { Name = "exec", Type = PinType.Exec, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "target", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "then", Type = PinType.Exec, IsOutput = true, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "fired", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- GetVar / SetVar -------------------------------------------------------------------
            // Graph-local persistent variables -- see GraphVariable's own comment (Graph.cs) and
            // GraphVarStore's own comment for the full storage/lifetime story; this is only the pin
            // shape. var= names which declared VAR the node addresses (see Node.VarName), the same
            // generic key=value NODE-line attribute mechanism field=/param=/class= already use.
            //
            // GetVar: a PURE READ (reading twice is always safe -- no side effect), so, unlike SetVar,
            // it gets NO exec pins and is reachable from BOTH compilers, exactly like GetField. Only ONE
            // output pin, 'value', typed to the declared VAR's type -- not a fixed type the way most
            // other default-pin cases have one, mirroring "param"/"getparam" immediately above. If var=
            // is missing or names an undeclared variable, add NO pin at all: Graph.Validate() (run right
            // after this loop, and again at the start of every Compile()/CompileEntryPoint()) reports
            // the specific reason, more useful than a generic "no output pin 'value'" from whatever
            // LINK/OUT touches this node next -- the exact same reasoning "param"/"getparam" already
            // follows.
            // ---- FUNCTIONS -------------------------------------------------------------------------
            //
            // All three of these derive their pins from a DECLARATION elsewhere in the file rather than
            // from a fixed list, which is the same thing Param/GetVar/SetVar already do -- see their
            // cases above. That is what makes a function's signature a single source of truth: change a
            // FUNCIN and every call node's pins change with it on the next load, rather than drifting.
            //
            // A PURE function has no exec pins anywhere -- entry, return, or call site. An impure one
            // has them in all three. There is no half-way state, which is the point of declaring purity
            // once on the FUNC record instead of inferring it three times.
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

            // SetVar: A WRITE IS A SIDE EFFECT (see GraphCompiler.IsExecCapableVarSideEffectType's own
            // comment), so -- UNLIKE GetField/SetField/GetFieldVec3/SetFieldVec3, which get no exec pins
            // by default -- this DOES get exec pins BY DEFAULT, mirroring Spawn/Raycast rather than
            // SetField: SetVar has no legitimate non-exec path at all (there is no "overwriting the same
            // value twice is harmless" excuse the way SetField's own idempotent field write has), so a
            // freshly palette-spawned node needs to already be usable, not require an author to hand-add
            // exec pins before it does anything. 'value' is typed to the declared VAR's type, the same
            // conditional-add-or-nothing rule as GetVar's own output above -- present only when var=
            // resolves, so an undeclared-variable graph still gets Graph.Validate()'s specific error
            // rather than a mistyped default pin masking it.
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
            // Three one-ABI-call writes -- see GraphCompiler.cs's IsExecCapableSetParentType/
            // IsExecCapableSetViewEntityType/IsExecCapableSetNameType comments for the full dispatch
            // story. ALL THREE ARE DISPATCHED SetField-STYLE, DELIBERATELY, NOT Spawn/SetVar-STYLE: no
            // exec pins by default (so a graph can wire one into a pure dataflow the same way GetField/
            // SetField already can), and EmitNode's own "setparent"/"setviewentity"/"setname" cases run
            // them unconditionally on Compile()'s topological pass, exactly like EmitSetField does for
            // "setfield". That choice rests on the same excuse SetField's own comment already gives:
            // aver_scene_set_parent/aver_scene_set_name/aver_fw_set_view_entity are all REPUBLISH
            // operations -- reparenting to the same parent, renaming to the same name, or republishing
            // the same view entity every single tick is harmless and idempotent, unlike Spawn (which
            // creates a NEW entity every call) or SetVar (which has no "safe to repeat" excuse at all).
            // Still fully refused when PULLED as a bare data value with no exec visit inside an
            // ENTRY-driven graph -- EmitPullOutput's side-effect refusal names all three, exactly like
            // it already names SetField/SetFieldVec3/Spawn/SetVar -- so "the PULL path must refuse a
            // write" holds for these too; only Compile()'s own SEPARATE topological compiler gets the
            // idempotent-overwrite exception SetField already established.
            case "setparent":
                // aver_scene_set_parent(child, parent) -> success. "child"/"parent" name the ABI's own
                // parameters directly (scene_abi.h:105) rather than "entity"/"target", so the pins read
                // the same as the native signature they wrap.
                node.Pins.Add(new Pin { Name = "child", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "parent", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            case "setviewentity":
                // aver_fw_set_view_entity(entity) -> void (framework_abi.h:206). NO OUTPUT PIN AT ALL --
                // deliberately, not an oversight: the ABI returns nothing, so there is no real return
                // code to surface, and inventing a fake "success" pin here would repeat exactly the
                // mistake this codebase's own SetField comment says it already fixed once ("the old stub
                // hardcoded 1 regardless of whether anything happened").
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                break;

            case "setname":
                // aver_scene_set_name(entity, name) -> success. name= is a NODE-line attribute (see
                // Node.NameValue), not a pin -- PinType has no String member, so this is the only route
                // a string reaches this node, the same way class= is the only route Spawn's class name
                // reaches IT.
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- SetMesh / SetMaterial ---------------------------------------------------------------
            // Coarse, dedicated nodes wrapping Aver.Framework.Entity.SetMesh/SetMaterial (EntityScene.cs)
            // through GraphInterop.SetMeshForGraph/SetMaterialForGraph -- NOT a generalised I64-capable
            // SetField, NOT an exposed asset-path lookup (Assets.ObjectIdOf is a pure local FNV1a64 hash,
            // no native call, no I/O), and NOT a generic "add a missing component" node: EntityScene's
            // own EnsureMeshRenderer already does that composition, so the graph node needs nothing new
            // beyond the string attribute mechanism setname/spawn/getfield already established. Same
            // SetField-style dispatch as SetParent/SetViewEntity/SetName above -- EnsureMeshRenderer's
            // own "if already present, do nothing" guard is what makes re-running this every tick
            // harmless, the identical idempotence excuse SetField's own comment gives.
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
            // TWO entity inputs, which no other Set*-shaped node here has: an attachment is a
            // relationship between a thing and what it hangs from, and both ends are entities the
            // graph already holds. socket= names which socket on the parent's rig.
            case "attachtosocket":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "parent", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // ---- GetAnimCurve ------------------------------------------------------------------
            // A PURE READ: no exec pins, so it needs no exec emitter and no IsExecCapable predicate.
            // It is the first node in this group that only asks a question.
            case "getanimcurve":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "value", Type = PinType.Float, IsOutput = true, NodeId = node.Id });
                break;

            // ---- SetSkeleton / PlayAnimation ----------------------------------------------------
            // SetMesh/SetMaterial's own animation-family siblings -- same SetField-style dispatch, same
            // reason (EnsureComponent's own idempotent add-if-absent guard). skeleton=/clip= name the
            // asset paths (see Node.SkeletonPath/Node.ClipPath); see GraphNodeDefs.hpp's own comment on
            // this pair for the array-index bone-binding caveat neither node can enforce.
            case "setskeleton":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "success", Type = PinType.Bool, IsOutput = true, NodeId = node.Id });
                break;

            // PlayAnimation gets a THIRD pin -- loop -- that no Set*-shaped node above needs, because
            // Entity.PlayAnimation itself takes a second scalar argument. A pin, not a NODE-line
            // attribute, because it is runtime data a graph may compute, not edit-time-only naming --
            // see Node.ClipPath's own comment.
            case "playanimation":
                node.Pins.Add(new Pin { Name = "entity", Type = PinType.Int, IsOutput = false, NodeId = node.Id });
                node.Pins.Add(new Pin { Name = "loop", Type = PinType.Bool, IsOutput = false, NodeId = node.Id });
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

    /// Reads a `x,y,z` attribute value into `into`, leaving any component it cannot read at
    /// whatever the caller had there. That fallback direction is deliberate: `scale` arrives
    /// pre-filled with 1,1,1, so a malformed `scale=2,,2` gives 2,1,2 rather than an actor
    /// collapsed to zero size, and a partially-typed value mid-edit never makes a component vanish.
    ///
    /// Fewer than three parts is accepted and fills what is there -- `pos=0,0` is a plausible thing
    /// to type, and the third axis keeping its default is the least surprising reading of it.
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
