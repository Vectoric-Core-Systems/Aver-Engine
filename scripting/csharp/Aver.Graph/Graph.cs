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

    // Which declared VAR a "getvar"/"setvar" node addresses, e.g. "score". Set from the NODE line's
    // "var=<name>" attribute -- the same generic key=value mechanism ParamName/FieldName/ClassName
    // already use. Null for every other node type. UNLIKE FieldName, there is no external table to
    // resolve this against (a VAR is declared in THIS graph file, not a scene-wide registry), so
    // Graph.Validate() checks it directly against Variables -- see that block's own comment.
    public string? VarName { get; set; }

    // The literal string a "setname" node writes via aver_scene_set_name, e.g. "Held_Weapon". Set
    // from the NODE line's "name=<value>" attribute -- the same generic key=value mechanism
    // ParamName/FieldName/ClassName/VarName already use. Null for every other node type. UNLIKE those
    // four, this is the first NODE-line attribute whose value is used AS DATA (the actual string
    // written to the entity) rather than as a lookup key into some table -- see GraphCompiler's
    // EmitSetName/EmitExecSetName for why that distinction does not change how it is parsed or
    // stored: PinType has no String member (Graph.cs's own PinType enum: Float/Int/Bool/Exec only),
    // so a NODE-line attribute remains the only route a string reaches ANY node in this format today,
    // whether it names something (field=/class=/var=) or IS the something (name=).
    public string? NameValue { get; set; }

    // Which asset path a "setmesh" node writes (via Aver.Framework.GraphInterop.SetMeshForGraph ->
    // Entity.SetMesh -> Assets.ObjectIdOf), e.g. "Content/Meshes/Prop.ocmesh". Set from the NODE
    // line's "mesh=<path>" attribute -- same mechanism as NameValue immediately above; the path
    // string is data the node writes, not a key resolved against any table this parser or compiler
    // knows about (Assets.ObjectIdOf is a pure local hash, computed at INVOCATION time in
    // Aver.Framework, not here). Null for every other node type.
    public string? MeshPath { get; set; }

    // Which material name a "setmaterial" node writes (via GraphInterop.SetMaterialForGraph ->
    // Entity.SetMaterial -> aver_scene_material), e.g. "M_Weapon". Set from the NODE line's
    // "material=<name>" attribute -- same mechanism as MeshPath immediately above. Null for every
    // other node type.
    public string? MaterialName { get; set; }

    // Which declared event a "fireevent" node fires on ANOTHER entity's graph, e.g. "OnHit". Set
    // from the NODE line's "event=<name>" attribute -- the same generic key=value NODE-line
    // mechanism NameValue/MeshPath/MaterialName already use (PinType has no String member, so this
    // remains the only route a string reaches this node -- see NameValue's own comment for the full
    // "carries data, not a lookup key" reasoning, which applies unchanged here: EventName is the
    // literal event name FireEventForGraph passes to the target's GraphHost.Fire, not something
    // resolved against a table this file knows about). Null for every other node type.
    public string? EventName { get; set; }
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

/// One variable a GRAPH remembers between ticks -- e.g. a hit count, a cooldown timer, a round state.
/// Declared with a top-level `VAR <name> <type> [default]` record, mirroring GraphParameter/PARAM
/// immediately above -- but the two are opposites, not siblings: a PARAM is the CALLER-supplies-this
/// contract (GraphHost.LoadEventGraph maps it onto entity/time/deltaTime, and refuses anything else,
/// because Tick()/Fire() are the only things that can ever hand a value in). A VAR is never supplied
/// by a caller at all; the graph OWNS it. Storage lives on the GraphHost instance driving this graph
/// (see GraphVarStore), created once at Load() time and persisting across every subsequent
/// Tick()/Fire() call on that SAME host -- two hosts sharing one .ocgraph file (the ordinary case: one
/// idle-motion graph driving several actors, each with its own GraphHost) get independent storage, not
/// one shared pool. A node reads or writes one by naming it in a GetVar/SetVar node's `var=<name>`
/// attribute (see Node.VarName), the same generic key=value NODE-line attribute mechanism
/// ParamName/FieldName/ClassName already use.
///
/// TYPE SET: Float/Int/Bool only, mirroring PARAM's own restriction (PinType has no String or Vec3 at
/// all -- see the PinType enum above) -- Exec is rejected at parse time, the same reasoning PARAM's own
/// Exec rejection already gives (a variable is data a graph remembers, not control flow).
///
/// WHAT IS NOT PERSISTED: a VAR's value does NOT survive a process restart, an Unload+Load cycle (a
/// level/scene reload does this to every script-hosted entity today), or any Load() call replacing a
/// still-live graph (hot-reloading the file while playing resets every VAR to its declared default).
/// That is the owner's explicitly accepted trade-off for this shape over a scene-scratch component --
/// see GraphVarStore's own comment for the deterministic "read before any write" contract this implies.
public class GraphVariable
{
    public required string Name { get; init; }
    public required PinType Type { get; init; }

    // The value a fresh GraphVarStore seeds this variable with. Always a concrete float/int/bool of
    // the DECLARED type by the time OcGraphParser is done with it (never null) -- an omitted or
    // unparseable `[default]` token falls back to the type's own zero value there, exactly like PIN's
    // own default-value convention (see OcGraphParser's VAR-parsing comment). Typed `object` rather
    // than three separate nullable fields for the same reason ConstantOutput/PinnedValue already do.
    public required object Default { get; init; }
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

    // Declared via top-level `VAR <name> <type> [default]` records -- see GraphVariable's own comment
    // for the full storage/lifetime contract. Empty for every graph that predates this (including
    // every existing .ocgraph and PARAM's own checked-in fixtures), exactly like Parameters was empty
    // before PARAM existed -- GraphCompiler only appends a GraphVarStore argument to the compiled
    // delegate when this list is non-empty, so a VAR-less graph's delegate shape is unchanged.
    public List<GraphVariable> Variables { get; set; } = new();

    // Declared via top-level `ENTRY <nodeId> <eventName>` records -- which node begins the PUSH/exec
    // chain for a named event (e.g. "OnStart", "OnTick"). Empty for every graph that predates this,
    // exactly like Parameters was empty before PARAM existed -- see
    // modules/formats/include/aver/formats/OcGraph.hpp's OcGraphData::entryPoints comment for the full
    // backward-compatibility argument, which applies here unchanged.
    public List<(string NodeId, string EventName)> EntryPoints { get; set; } = new();

    // Declared via an OPTIONAL top-level `CLASS <name> [parentName] [mesh=<path>] [material=<name>]`
    // record -- this is what turns a plain .ocgraph into a spawnable actor CLASS, the way a Blueprint
    // asset carries a parent class and class defaults alongside its event graph. Null when the graph
    // declares no CLASS record (every graph that predates this, and every ordinary project-utility
    // graph that only computes values against GameApp's synthetic-entity discovery -- see
    // GameApp.cpp's discoverProjectGraphs) -- that is the overwhelmingly common case and stays
    // completely unaffected: nothing about VAR/PARAM/ENTRY/NODE parsing or GraphHost.Tick() changes
    // for a CLASS-less graph. A NEW record, not an unknown one, for the identical reason PARAM/VAR's
    // own comments give: the C++ reader (OcGraph.cpp's classifyLine) has no "Class" case, so a CLASS
    // line falls into the same OwnedLineKind::Other bucket PARAM/VAR already ride through on, and
    // round-trips byte-for-byte through a same-path load->save with zero C++ reader changes needed --
    // verified against a hand-written fixture via OcGraphTest.exe's --roundtrip diagnostic.
    //
    // WHO CONSUMES THIS: not this format layer, and not GraphHost -- a CLASS record is read by
    // Aver.Scripting.Bridge's HostBridge (the same file that already declares C# actor classes
    // through the framework ABI), which registers ClassName as a real aver_fw_class_declare(...) row
    // with ClassParent as its parent, then gives EVERY SPAWNED INSTANCE its own GraphHost bound to
    // its own real entity. See HostBridge.cs's "graph classes" region for the full registration and
    // per-instance binding story.
    public string? ClassName { get; set; }

    // The declared parent's class name, e.g. "Actor" or a project's own C# actor class. Defaults to
    // "Actor" when the CLASS record omits it (see OcGraphParser's CLASS-record comment) -- mirroring
    // HostBridge's own BaseRegistryName default for a plain, component-less AverActor. Meaningless
    // (and left null) when ClassName is null.
    public string? ClassParent { get; set; }

    // Optional mesh/material the class's own entity gets as class DEFAULTS (via
    // aver_fw_class_add_component(CMeshRenderer) + aver_fw_class_set_default_i64/i32), so a
    // class-placed instance has a visible default look even before its own graph's OnStart/OnTick
    // (e.g. a SetMesh node) has run -- notably, before aver_fw_spawn_preview, which never dispatches
    // OnBeginPlay at all. Both null when the CLASS record states neither; ClassMaterial is only ever
    // meaningful alongside a non-null ClassMesh.
    public string? ClassMesh { get; set; }
    public string? ClassMaterial { get; set; }

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

        // Check VAR declarations are unique -- the same reason two PARAM declarations can't share a
        // name, above: two variables with the same name would make "which one does a GetVar/SetVar
        // node's var= attribute mean" ambiguous.
        var varNames = new HashSet<string>();
        foreach (var v in Variables)
        {
            if (!varNames.Add(v.Name))
            {
                err = $"Duplicate VAR declaration '{v.Name}'";
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

        // Check every GetVar/SetVar node names a variable that was actually declared, and that if it
        // already has an explicit 'value' pin (a hand-written PIN record, rather than the parser's
        // default-pins path) that pin's type agrees with the VAR's declared type. Mirrors the Param-node
        // block immediately above in shape and reasoning -- including running BEFORE the Outputs check
        // below for the identical reason that block gives: an undeclared var= produces no default pin
        // (see OcGraphParser.AddDefaultPins' "getvar"/"setvar" cases), so a LINK/OUT touching it should
        // fail with the real reason, not a generic "no pin" message.
        //
        // GetVar's 'value' pin is an OUTPUT; SetVar's is an INPUT -- `p.IsOutput == isGetVar` picks the
        // right one for whichever node type is being checked without two near-duplicate blocks.
        foreach (var node in Nodes.Values)
        {
            bool isGetVar = node.Type.Equals("getvar", System.StringComparison.OrdinalIgnoreCase);
            bool isSetVar = node.Type.Equals("setvar", System.StringComparison.OrdinalIgnoreCase);
            if (!isGetVar && !isSetVar) continue;

            string kind = isGetVar ? "GetVar" : "SetVar";

            if (string.IsNullOrEmpty(node.VarName))
            {
                err = $"Node '{node.Id}' is a {kind} node but has no var= attribute naming which variable it addresses";
                return false;
            }

            var declaredVar = Variables.FirstOrDefault(v => v.Name == node.VarName);
            if (declaredVar == null)
            {
                err = $"Node '{node.Id}' references undeclared variable '{node.VarName}' -- add 'VAR {node.VarName} <type>'";
                return false;
            }

            var valuePin2 = node.Pins.FirstOrDefault(p => p.Name == "value" && p.IsOutput == isGetVar);
            if (valuePin2 != null && valuePin2.Type != declaredVar.Type)
            {
                err = $"Node '{node.Id}' declares {(isGetVar ? "output" : "input")} pin 'value' as {valuePin2.Type} but variable '{node.VarName}' is {declaredVar.Type}";
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
