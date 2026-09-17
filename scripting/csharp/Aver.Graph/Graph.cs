// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Minimal in-memory graph model: nodes, pins, links, and pinned values.
// Comment explains WHY: a graph is a DAG of computation nodes. Each node has inputs and outputs.
// Links connect pins. Pinned values are constants clamped to a node's input pin. The model carries
// enough to evaluate the graph once compiled to IL.

using System;
using System.Collections.Generic;
using System.Globalization;
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

    // WHICH SUBGRAPH THIS NODE LIVES IN. Set from the NODE line's "func=<name>" attribute; null
    // means the EVENT GRAPH, which is every node in every graph written before functions existed.
    //
    // Ownership is an attribute on the node rather than a list on the function, so that a node
    // belongs to exactly one place by construction -- a node cannot appear in two functions' lists,
    // and there is no second structure to keep in step when a node is deleted. It also means the
    // C++ reader needed no change at all to preserve a function body: func= rides in the same
    // extraTokens that already carry param=/field=/class=/var=.
    public string? FuncOwner { get; set; }

    // Which function a "callfunc" node CALLS. Set from the NODE line's "call=<name>" attribute.
    //
    // A SEPARATE ATTRIBUTE FROM func=, deliberately, because a call node has both: it LIVES in one
    // subgraph (func=, possibly null for the event graph) and it CALLS another (call=). Overloading
    // one attribute with both meanings would make a recursive call -- a node inside Fib that calls
    // Fib -- indistinguishable from a node that merely lives there.
    public string? CallTarget { get; set; }

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

    // Which socket an "attachtosocket" node hangs its entity on, e.g. "Hand_R". Set from the NODE
    // line's "socket=<name>" attribute -- the same key=value mechanism MeshPath/MaterialName/
    // EventName use, and for the same reason: PinType has no String member, so a literal name has
    // no other route into a node. Carries DATA, not a key resolved here: the name is hashed with
    // fnv1a64 at invocation time and matched against the parent rig's own socket names by
    // AnimSystem, which this assembly cannot see. Null for every other node type.
    public string? SocketName { get; set; }

    // Which curve a "getanimcurve" node reads, e.g. "ReloadProgress". Set from the NODE line's
    // "curve=<name>" attribute -- same mechanism as SocketName above, same reason. Null for every
    // other node type.
    public string? CurveName { get; set; }

    // The file a "savegame" node writes to, or a "loadgame" node reads from, e.g.
    // "Saves/Slot1.ocsave". Set from the NODE line's "path=<value>" attribute -- same mechanism as
    // NameValue above (the value IS the data, not a lookup key). ONE property for BOTH node types,
    // unlike every other string attribute above, which is one property per node type: SaveGame and
    // LoadGame both name "the file this node touches", the identical kind of value, just read in
    // one case and written in the other -- a second property would carry the same doc comment
    // twice. Null for every other node type.
    public string? SavePath { get; set; }

    /// The sound file a "playsound"/"playsoundat" node plays, from a `sound=<path>` NODE-line
    /// attribute. Same "the value IS the data" family as NameValue and SavePath above, and a
    /// NODE-line attribute for the same reason all of them are: PinType has no String member, so an
    /// edit-time path has no pin it could arrive on. ONE property for both node types, matching
    /// SavePath's own precedent -- flat and positioned playback name the same kind of thing.
    /// Null for every other node type.
    public string? SoundPath { get; set; }

    /// The message a "printstring" node writes, from a `text=<message>` NODE-line attribute. Same
    /// "the value IS the data" family as SoundPath above and for the identical reason: PinType has
    /// no String member, so an authored message has no pin it could arrive on.
    ///
    /// WHY THE NODE EXISTS AT ALL. Print and PrintInt both need a VALUE wired before they say
    /// anything, so proving a branch was taken meant inventing a number to route through it -- and
    /// they label their line with the node's auto-generated id, so the log reads "print3 = 1" and
    /// the author has to work out which node that was. Here the author writes the label, and the
    /// node needs nothing wired but exec. Null for every other node type.
    public string? PrintText { get; set; }

    // Which skeleton asset a "setskeleton" node binds, e.g. "Content/Skeletons/Hero.ocskel". Set
    // from the NODE line's "skeleton=<path>" attribute -- same "the value IS the data" mechanism
    // MeshPath uses (Assets.ObjectIdOf is a pure local hash, computed at INVOCATION time in
    // Aver.Framework, not here). Null for every other node type.
    public string? SkeletonPath { get; set; }

    // Which animation clip a "playanimation" node plays, e.g. "Content/Anims/Run.ocanim". Set from
    // the NODE line's "clip=<path>" attribute -- same mechanism as SkeletonPath immediately above.
    // The loop flag is NOT here: unlike the clip path, it is genuine runtime data a graph may
    // compute, so it rides an ordinary bool PIN (Node.Pins) instead of a NODE-line attribute -- see
    // GraphNodeDefs.hpp's own "PlayAnimation gets a THIRD input pin" comment for why. Null for every
    // other node type.
    public string? ClipPath { get; set; }

    // Which control rig a "setcontrolrig" node binds, e.g. "Content/Rigs/ArmReach.ocrig". Set from
    // the NODE line's "rig=<path>" attribute -- same mechanism as SkeletonPath and ClipPath above.
    // The weight is NOT here, for exactly ClipPath's reason: which rig is worn is edit-time naming,
    // how strongly it is worn is runtime data a graph may compute, so weight rides a float PIN.
    // Null for every other node type.
    public string? RigPath { get; set; }

    // Which declared INPUT ACTION an "inputaction"/"inputactionpressed"/"inputactionreleased" node
    // names, e.g. "Jump" -- and, on "rebindaction"/"getactionkey", the action those two REQUIRE (see
    // each node's own doc comment in OcGraphParser.AddDefaultPins). Set from the NODE line's
    // "action=<name>" attribute -- the same generic key=value mechanism ClassName/EventName/CurveName
    // already use, and for the identical reason: PinType has no String member, so an author-chosen
    // action name has no pin it could arrive on.
    //
    // OPTIONAL ON InputAction/InputActionPressed/InputActionReleased, UNLIKE EVERYWHERE ELSE THIS
    // FAMILY OF ATTRIBUTES IS USED. Those three nodes predate this attribute and already have a
    // working (if unrebindable-by-name) route to the same data: a plain Int `action` pin carrying the
    // handle `aver_fw_action_register`/`_find` returned. When ActionName is set the compiler resolves
    // the handle itself, at runtime, via GraphInterop.ActionHandleForGraph(ActionName), and the
    // `action` pin is ignored entirely; when it is null (every graph authored before this attribute
    // existed), nothing changes -- the pin is read exactly as it always was. See
    // GraphCompiler.LoadActionHandle/PullActionHandle for where that branch happens.
    //
    // REQUIRED on RebindAction and GetActionKey, where there is no pin fallback at all: neither node
    // has an `action` pin to fall back to (RebindAction's inputs are `slot`/`key`; GetActionKey's is
    // `slot`), so an empty ActionName here can never do anything useful. GraphCompiler's
    // EmitExecRebindAction/EmitGetActionKey/EmitPullGetActionKey require it non-empty at COMPILE time,
    // mirroring Spawn's class= and SaveGame/LoadGame's path=.
    public string? ActionName { get; set; }
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

/// One entry in a class graph's COMPONENT TREE, from a `COMP <id> <Kind> [key=value]...` record.
///
/// THIS SIDE IS THE SEMANTIC HALF, which is why the fields here are typed and the C++ reader's
/// OcGraphComponent keeps everything as verbatim tokens. That split is the same one VAR already
/// follows -- C++ asks for a name and a type token, this side knows PinType is Float/Int/Bool and
/// rejects Exec. Here it means C++ guarantees a well-formed line whose parent chain is a tree, and
/// this class decides what a Kind is, what units a transform is in, and which attributes a Kind
/// actually reads.
///
/// UNITS MATCH THE SCENE: centimetres, +Z up, rotation authored in DEGREES as YAW, PITCH, ROLL -- the order Aver.Scene.Rot itself takes and
/// stored that way -- unlike CCamera.fovYRad, which stores radians, because a transform is a thing
/// an author types into a details panel and a field of view is a thing a shader reads.
public class GraphComponent
{
    public required string Id { get; init; }

    /// The component kind: "Scene", "Mesh", "Light", "Camera", "SkeletalMesh" or "Particles".
    /// Held as the authored string rather than an enum so an unrecognised kind can be REPORTED by
    /// name at spawn instead of being silently coerced to a default -- the same tolerance an
    /// unrecognised NODE-line key=value already gets, but louder, because a component that quietly
    /// becomes a Scene node is an actor missing a limb with nothing in the log about it.
    public required string Kind { get; init; }

    /// The id of the component this one hangs off, or null for one attached to the actor's own
    /// entity. The C++ reader has already proved every non-null parent names a real component and
    /// that the chain terminates, so a walk over these is a tree and needs no cycle guard.
    public string? Parent { get; set; }

    /// Local transform relative to the parent. Centimetres; degrees; scale multiplier.
    public float[] Position { get; set; } = new float[3];
    public float[] RotationDeg { get; set; } = new float[3];
    public float[] Scale { get; set; } = { 1f, 1f, 1f };

    /// Every other key=value on the line, by key, with the transform and parent keys REMOVED --
    /// they are the fields above. Kind-specific: `mesh=`/`material=` on a Mesh, `fov=`/`near=`/
    /// `far=` on a Camera, `effect=` on Particles. A dictionary rather than a field per attribute
    /// because the set is open: a Kind added later brings its own keys, and this class should not
    /// need editing for that to work.
    public Dictionary<string, string> Attributes { get; } = new(StringComparer.OrdinalIgnoreCase);

    /// Reads an attribute as a float, falling back rather than throwing -- an authored `fov=wide`
    /// should give a usable camera and a warning, not a class that fails to spawn.
    public float AttrFloat(string key, float fallback) =>
        Attributes.TryGetValue(key, out string? v) &&
        float.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out float f) ? f : fallback;

    public string AttrString(string key) => Attributes.TryGetValue(key, out string? v) ? v : "";
}

/// A complete graph: nodes, links, pinned values, and output pins to evaluate.
/// One user-defined FUNCTION: a named, callable subgraph living inside the same .ocgraph as the
/// event graph, with its own inputs, its own outputs, and its own body of nodes.
///
/// WHY THIS IS A REAL CALL AND NOT AN INLINE EXPANSION. Inlining was the cheaper design and it was
/// seriously considered -- it needs no new emission code at all, because the compiler already
/// re-emits pure expressions inline in EmitPullOutput. It was rejected on one fact, established by
/// spike rather than by argument: a DynamicMethod on this toolchain CAN emit a direct IL `Call` to
/// another DynamicMethod, including to ITSELF and to one whose body has not been written yet.
/// Recursion therefore costs nothing here, and an inlined function cannot recurse at all -- the
/// expansion would not terminate. Blueprint draws that same line between a Function and a Macro,
/// and this is the Function.
///
/// ONE FILE, MANY FUNCTIONS, and node ids stay a single FILE-WIDE namespace. A function's body is
/// ordinary NODE/PIN/LINK records carrying a `func=` attribute naming their owner -- which means the
/// C++ reader needed no change at all to preserve a function body, since `func=` rides in the same
/// extraTokens every other attribute already uses. Scoping ids per function would have been tidier
/// on paper and would have broken the one duplicate-id check the two readers already disagree about.
public class GraphFunction
{
    public required string Name { get; init; }

    /// Inputs, in declaration order -- the arguments of the emitted method. Read inside the body
    /// from the function's FuncEntry node, whose output pins are exactly these.
    public List<GraphParameter> Inputs { get; } = new();

    /// Outputs, in declaration order. Written inside the body by a FuncReturn node, whose input pins
    /// are exactly these, and read at the call site from the CallFunc node's output pins.
    public List<GraphParameter> Outputs { get; } = new();

    /// PURE means "no exec pins, callable from a data wire", exactly as Blueprint means it.
    ///
    /// DECLARED, NOT INFERRED, and that is a deliberate divergence from Blueprint, which decides by
    /// looking at what the body contains. Inference has a hole that is invisible to its author: a
    /// function whose body is only a call to ANOTHER function is pure or impure according to what
    /// THAT one does, transitively, and a rule that inspects node types in the body alone gets it
    /// wrong. Declaring it makes the author state the intent and lets Validate() check the body
    /// against it -- including transitively, which is the case inference cannot see.
    public bool IsPure { get; set; }

    /// The node id of this function's FuncEntry, and of its FuncReturn if it has one. Resolved by
    /// the parser from the `func=` attribute rather than declared in the FUNC record: a function has
    /// exactly one of each by construction (Validate refuses a second), so naming them twice would
    /// be two places to disagree.
    public string? EntryNodeId { get; set; }
    public string? ReturnNodeId { get; set; }
}

/// Which LANGUAGE a .ocgraph's nodes are written in -- the C# mirror of aver::fmt::OcGraphDomain
/// (modules/formats/include/aver/formats/OcGraph.hpp), which owns the authoritative comment.
///
/// The short version of why it exists: one extension, several unrelated languages. This compiler
/// emits IL and its nodes call the framework; a material graph is compiled to HLSL by C++ and its
/// nodes are arithmetic on a surface. HostBridge.DeclareGraphClasses walks EVERY *.ocgraph under a
/// project's content directory, so without a marker it would reach a material graph and try to make
/// an actor class out of it.
///
/// ABSENT IS Gameplay; AN UNRECOGNISED NAME IS Unknown, NOT Gameplay. Every graph written before the
/// record existed is a gameplay graph, so a missing DOMAIN cannot be an error -- but a file naming a
/// domain this build has never heard of has said out loud that it is not one, and the safe reading
/// of that is "skip it", not "compile it anyway".
public enum GraphDomain
{
    Gameplay,
    Material,
    Unknown,
}

public class Graph
{
    public string Name { get; set; } = "untitled";
    public string Description { get; set; } = "";

    /// The raw text of the top-level `DOMAIN <name>` record, or null when the file has none.
    /// Kept as the author wrote it rather than normalised, for the reason the C++ side's own
    /// OcGraphData::domain gives: a build that does not recognise a domain must not rewrite it.
    /// Ask DomainKind, not this, unless you are writing the file back out.
    public string? Domain { get; set; }

    /// This graph's domain as one of the three answers a consumer actually has. See GraphDomain.
    public GraphDomain DomainKind =>
        string.IsNullOrEmpty(Domain) ? GraphDomain.Gameplay
        : Domain.Equals("gameplay", StringComparison.OrdinalIgnoreCase) ? GraphDomain.Gameplay
        : Domain.Equals("material", StringComparison.OrdinalIgnoreCase) ? GraphDomain.Material
        : GraphDomain.Unknown;
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

    // Declared via top-level `FUNC` / `FUNCIN` / `FUNCOUT` records. Empty for every graph written
    // before functions existed, exactly as Parameters was empty before PARAM existed -- and, as
    // with every record before it, a reader that does not know FUNC skips it rather than failing,
    // so an older runtime meeting a function-bearing graph loses the function and keeps the file.
    public List<GraphFunction> Functions { get; set; } = new();

    // Declared via top-level `COMP <id> <Kind> [key=value]...` records, in FILE order -- which is
    // not tree order, because a component may name a parent declared below it. Empty for every
    // graph that predates this and for every graph that is not a class, exactly as Variables was
    // empty before VAR existed.
    //
    // Only meaningful alongside a non-null ClassName: a component tree describes what a SPAWNED
    // instance is made of, and a graph nothing spawns has no instance to hang one on. The parser
    // does not refuse that combination -- a graph mid-edit, with its components authored before
    // its CLASS line, is a normal state to be in and not an error to report.
    public List<GraphComponent> Components { get; set; } = new();

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

    // Optional first-/third-person camera default, from the CLASS record's `view=firstperson` /
    // `view=thirdperson` attribute (case-insensitive; unrecognised text is warned about and ignored --
    // see HostBridge.cs's DispBind, the only consumer). Null when the record omits it, which leaves
    // AverCharacter.CameraViewMode at its own C# default (ThirdPerson).
    //
    // WHY THIS EXISTS: AverCharacter.CameraViewMode is a plain public C# field, set only by a project's
    // own C# (Aver.Framework.SampleActor's PlayDemo, at the time this was added) -- there was no scene
    // field, no CLASS attribute and no node anywhere that reached it, so a graph-declared `CLASS ...
    // Character` had no way to ask for a first-person camera at all. That made a graph-only "first
    // person" template a contradiction in terms: the character would run, but the camera would sit
    // behind it regardless. `mesh=`/`material=` already prove a CLASS record can hand an actor-kind
    // attribute down to its spawned instances without a node; `view=` is the same shape applied to the
    // one C# field a Character-parented graph class had no other way to touch.
    //
    // Meaningless for a class whose native ancestor is not Character (or does not derive from it) --
    // HostBridge silently ignores it there rather than failing the whole class over an attribute one
    // ancestor happens not to use, the same tolerance an unrecognised NODE-line key=value already gets.
    public string? ClassView { get; set; }

    // Optional default pawn CLASS NAME, from the CLASS record's `pawn=<ClassName>` attribute. Null when
    // the record omits it. Only meaningful on a GameMode; HostBridge warns and ignores it elsewhere.
    //
    // WHY THIS EXISTS: aver_fw_begin_play already spawns a GameMode's controller and pawn and POSSESSES
    // the pawn -- the machinery is all there, driven by ClassRecord::defaultPawnName, and C# GameModes
    // reach it through GameModeInfo.DefaultPawnClass. A GRAPH GameMode had no way to name one, so a
    // graph-only project's character was placed but never possessed. That matters far more than it
    // sounds: GameApp's camera follows `aver_fw_controlled_pawn(...)` and returns early when there is
    // none, so a shipped game showed its default camera and no amount of `view=firstperson` reached it.
    // A first-person template that cannot be seen in first person was the symptom.
    //
    // RESOLVED BY NAME AT SEAL (aver_fw_class_set_default_pawn), which is why HostBridge applies these
    // in a SECOND pass after every graph class has been declared, rather than inline: naming a pawn
    // whose own class had not been declared yet would resolve to 0 and silently possess nothing, and
    // which graphs got that treatment would depend on filename order.
    public string? ClassPawn { get; set; }

    // Optional player-controller CLASS NAME, from the CLASS record's `controller=<ClassName>` attribute.
    // Only meaningful on a GameMode, like ClassPawn.
    //
    // REQUIRED FOR pawn= TO DO ANYTHING, which is why the two arrived together rather than one at a
    // time. aver_fw_begin_play possesses only when it has BOTH ("if (ctrl && pawn)"), and the built-in
    // `PlayerController` class is ABSTRACT -- aver_fw_spawn refuses an abstract class outright, so a
    // GameMode that named no controller of its own got ctrl == 0 and never possessed anything, however
    // correct its pawn was. Naming a pawn alone would have looked wired up and changed nothing.
    //
    // A graph can supply one with no C# at all: ABSTRACT is deliberately NOT inherited (see
    // kInheritableKindFlags), so `CLASS AN_FPController PlayerController` is a concrete, spawnable class
    // that still carries the CONTROLLER flag possession checks for -- the same thing a Blueprint
    // subclass of APlayerController is.
    public string? ClassController { get; set; }

    /// Declares the `entity` parameter that `Self` nodes read, if the graph did not declare one
    /// itself. Called by OcGraphParser only when a Self node was seen -- by then each has already
    /// been built as a `Param` node naming "entity" (Node.Type is init-only, so the rewrite happens
    /// at construction; see the NODE case in OcGraphParser). Runs BEFORE AddDefaultPins, so the
    /// rewritten node gets its output pin typed from this parameter like any other Param node.
    ///
    /// WHY SELF EXISTS. Nearly every Scene, Character, Physics, Animation and Audio node takes an
    /// `entity` pin, and `PARAM entity int` plus a Param node was the ONLY route to the graph's own
    /// handle. That is fine in hand-written text and impossible in the editor, which has no way to
    /// write a PARAM record: aver::fmt::OcGraphData (modules/formats/include/aver/formats/OcGraph.hpp)
    /// models nodes, links, variables, components, functions and outputs, and no parameters at all.
    /// So a graph authored entirely on the canvas could not name the entity it was running on, and
    /// therefore could not drive anything. That, not polish, is why every gameplay graph in this
    /// repository is hand-written text.
    ///
    /// SUGAR, NOT A NODE THE COMPILER KNOWS, so it costs nothing anywhere else: not in the IL
    /// emitter, not in CompileFunction's argument rebasing, not in GraphHost's entity/time/deltaTime
    /// slot mapping, not in FireForEntity's by-name binding, and not in the C++ writer. All of those
    /// already handle `PARAM entity int`, and none of them ever sees a Self node.
    ///
    /// THE ONE VISIBLE CONSEQUENCE is that a graph using Self gains a parameter it did not write.
    /// Every caller in the engine binds arguments BY NAME (GraphHost.LoadEventGraph maps the
    /// {entity, time, deltaTime} vocabulary; FireForEntity does the same), so this is invisible to
    /// them. Only Fire()'s raw positional overload counts arguments, and it refuses a mismatch with
    /// a message naming the expected count rather than binding the wrong value.
    public bool ResolveSelfNodes(out string? err)
    {
        err = null;

        // Reuse a declaration the author already made rather than adding a second one -- a
        // hand-written graph that mixes `PARAM entity int` with a canvas-placed Self node is an
        // ordinary thing to end up with once the editor can save these files, and two parameters
        // would change the compiled method's arity and break every caller.
        var declared = Parameters.FirstOrDefault(
            p => p.Name.Equals("entity", System.StringComparison.OrdinalIgnoreCase));
        if (declared == null)
        {
            Parameters.Add(new GraphParameter { Name = "entity", Type = PinType.Int });
            return true;
        }

        if (declared.Type != PinType.Int)
        {
            // A FLOAT CANNOT HOLD AN ENTITY HANDLE. Handles start above 2^24, where a float's
            // mantissa has already run out of integers, so the value would round to a NEIGHBOURING
            // handle -- a real entity, just not this one. Refused rather than silently accepted,
            // because the resulting bug looks like a framework fault rather than a typed-it-wrong
            // fault: every call downstream succeeds, on somebody else's entity.
            err = $"Self resolves to the graph's 'entity' parameter, which this graph declares as " +
                  $"{declared.Type}. An entity handle does not fit in a {declared.Type} -- declare " +
                  "'PARAM entity int', or delete the declaration and let Self add it.";
            return false;
        }

        // The declared spelling may differ in case from the "entity" the parser wrote onto each Self
        // node, and Validate's parameter lookup is case-SENSITIVE. Re-point them at what was declared.
        if (declared.Name != "entity")
            foreach (var n in Nodes.Values)
                if (n.ParamName == "entity") n.ParamName = declared.Name;

        return true;
    }

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

        // ---- FUNCTIONS -----------------------------------------------------------------------------
        //
        // Runs BEFORE the Param/GetVar blocks below for the same reason those run before the Outputs
        // check: a node whose func= names nothing gets no derived pins at all (see AddDefaultPins),
        // so every later check would fail with a generic "no pin" message that names the symptom.
        if (Functions.Count > 0 || Nodes.Values.Any(n => n.FuncOwner != null || n.CallTarget != null))
        {
            var byName = new Dictionary<string, GraphFunction>(System.StringComparer.OrdinalIgnoreCase);
            foreach (var f in Functions) byName[f.Name] = f;

            foreach (var node in Nodes.Values)
            {
                if (node.FuncOwner != null && !byName.ContainsKey(node.FuncOwner))
                {
                    err = $"Node '{node.Id}' has func={node.FuncOwner}, which no FUNC record declares";
                    return false;
                }
                string t = node.Type.ToLowerInvariant();
                if ((t == "funcentry" || t == "funcreturn") && node.FuncOwner == null)
                {
                    err = $"Node '{node.Id}' is a {node.Type} but has no func= attribute saying which function it belongs to";
                    return false;
                }
                if (t == "callfunc")
                {
                    if (string.IsNullOrEmpty(node.CallTarget))
                    {
                        err = $"Node '{node.Id}' is a CallFunc but has no call= attribute naming which function it calls";
                        return false;
                    }
                    if (!byName.ContainsKey(node.CallTarget))
                    {
                        err = $"Node '{node.Id}' calls '{node.CallTarget}', which no FUNC record declares";
                        return false;
                    }
                }
            }

            foreach (var f in Functions)
            {
                int entries = Nodes.Values.Count(n => n.Type.Equals("funcentry", System.StringComparison.OrdinalIgnoreCase) &&
                                                       string.Equals(n.FuncOwner, f.Name, System.StringComparison.OrdinalIgnoreCase));
                int returns = Nodes.Values.Count(n => n.Type.Equals("funcreturn", System.StringComparison.OrdinalIgnoreCase) &&
                                                       string.Equals(n.FuncOwner, f.Name, System.StringComparison.OrdinalIgnoreCase));
                if (entries != 1)
                {
                    err = entries == 0
                        ? $"function '{f.Name}' has no FuncEntry node -- a function needs somewhere to begin"
                        : $"function '{f.Name}' has {entries} FuncEntry nodes; a function begins in exactly one place";
                    return false;
                }
                if (returns > 1)
                {
                    // ONE return, not one per branch, and the reason is in the emitter: a function's
                    // outputs are read from their locals AFTER the exec chain has finished, exactly as
                    // CompileEntryPoint reads OUT records, so the emitted method has a single Ret. Two
                    // FuncReturn nodes would be two nodes writing the same locals with no rule about
                    // which ran last.
                    err = $"function '{f.Name}' has {returns} FuncReturn nodes; a function returns from exactly one place " +
                          "(branch INTO the single FuncReturn instead)";
                    return false;
                }
                if (f.Outputs.Count > 0 && returns == 0)
                {
                    err = $"function '{f.Name}' declares {f.Outputs.Count} output(s) but has no FuncReturn node to produce them";
                    return false;
                }
            }

            // A WIRE MAY NOT LEAVE THE SUBGRAPH IT IS IN. This is the rule that makes a function a
            // function rather than a naming convention: the event graph and each function compile to
            // SEPARATE methods, so a link between them would be a link between two different method
            // bodies' locals -- which the emitter cannot express and would either drop silently or
            // read as garbage. Arguments cross the boundary through FuncEntry, results through
            // FuncReturn, and nothing else crosses at all.
            foreach (var link in Links)
            {
                if (!Nodes.TryGetValue(link.SourceNodeId, out var sn)) continue;
                if (!Nodes.TryGetValue(link.TargetNodeId, out var tn)) continue;
                if (string.Equals(sn.FuncOwner, tn.FuncOwner, System.StringComparison.OrdinalIgnoreCase)) continue;
                string Where(Node n) => n.FuncOwner == null ? "the event graph" : $"function '{n.FuncOwner}'";
                err = $"link '{link.SourceNodeId}.{link.SourcePinName}' -> '{link.TargetNodeId}.{link.TargetPinName}' " +
                      $"crosses from {Where(sn)} into {Where(tn)}. A wire cannot leave the subgraph it is in -- " +
                      "pass the value in through the function's FuncEntry, or back out through its FuncReturn.";
                return false;
            }

            // ENTRY and OUT describe the EVENT GRAPH. An ENTRY naming a node inside a function would
            // ask GraphHost to fire an event into a method it never compiled for that purpose.
            foreach (var (entryNodeId, eventName) in EntryPoints)
            {
                if (Nodes.TryGetValue(entryNodeId, out var en) && en.FuncOwner != null)
                {
                    err = $"ENTRY '{eventName}' names node '{entryNodeId}', which lives inside function " +
                          $"'{en.FuncOwner}'. An event begins in the event graph.";
                    return false;
                }
            }
            foreach (var (outNodeId, outPinName) in Outputs)
            {
                if (Nodes.TryGetValue(outNodeId, out var on) && on.FuncOwner != null)
                {
                    err = $"OUT '{outNodeId} {outPinName}' names a node inside function '{on.FuncOwner}'. " +
                          "OUT is what the GRAPH hands back; a function hands its own results back through FuncReturn.";
                    return false;
                }
            }

            // PURITY IS CHECKED TRANSITIVELY, which is the whole reason it is declared rather than
            // inferred. A rule that only looked at the node types physically present in a body would
            // call a function pure when its body is a single call to an impure one -- the exact hole
            // an inference-based design cannot see, and the one this loop closes. Iterating to a fixed
            // point rather than recursing keeps a mutually recursive pair from spinning.
            var impure = new HashSet<string>(System.StringComparer.OrdinalIgnoreCase);
            bool changed = true;
            while (changed)
            {
                changed = false;
                foreach (var f in Functions)
                {
                    if (impure.Contains(f.Name)) continue;
                    foreach (var node in Nodes.Values)
                    {
                        if (!string.Equals(node.FuncOwner, f.Name, System.StringComparison.OrdinalIgnoreCase)) continue;
                        bool bad = node.Pins.Any(p => p.Type == PinType.Exec) &&
                                    !node.Type.Equals("funcentry", System.StringComparison.OrdinalIgnoreCase) &&
                                    !node.Type.Equals("funcreturn", System.StringComparison.OrdinalIgnoreCase);
                        if (!bad && node.Type.Equals("callfunc", System.StringComparison.OrdinalIgnoreCase) &&
                            node.CallTarget != null && impure.Contains(node.CallTarget))
                            bad = true;
                        if (bad) { impure.Add(f.Name); changed = true; break; }
                    }
                }
            }
            foreach (var f in Functions)
            {
                if (!f.IsPure || !impure.Contains(f.Name)) continue;
                err = $"function '{f.Name}' is declared pure, but its body has control flow or calls a function that does. " +
                      "Remove the `pure` flag on its FUNC record, or move the side effect out of it.";
                return false;
            }
        }

        // AN EXEC OUTPUT MAY DRIVE EXACTLY ONE LINK, and this check exists here so the EDITOR can say
        // so. The rule itself was already enforced -- GraphCompiler.FindExecTarget throws a
        // well-worded InvalidOperationException for it -- but only at COMPILE time, only for a node
        // the exec walk actually reaches, and as an exception rather than a validation error. An
        // author's first sight of it was the engine log at project open, on a graph they had finished
        // and put away. Checked here, it reaches the canvas as a badge on the offending node the
        // moment they press Validate.
        //
        // DATA OUTPUTS ARE UNAFFECTED and deliberately so: a value may fan out to as many readers as
        // want it. It is control flow that cannot fork without saying which order it forks in, which
        // is what a Sequence node is for.
        //
        // NAMED SO THE EDITOR CAN FIND THE NODE: the message leads with Node '<id>' because
        // GraphEditor.errorNodeId reads the first single-quoted token and badges it only if it is a
        // real node id. Leading with 'nodeId.pinName' -- which is what the compiler's own message
        // does -- would badge nothing.
        foreach (var node in Nodes.Values)
        {
            foreach (var pin in node.Pins)
            {
                if (!pin.IsOutput || pin.Type != PinType.Exec) continue;
                int driven = 0;
                foreach (var l in Links)
                    if (l.SourceNodeId == node.Id && l.SourcePinName == pin.Name) ++driven;
                if (driven <= 1) continue;
                err = $"Node '{node.Id}' has exec output '{pin.Name}' wired to {driven} links -- an " +
                      "exec output can only continue to ONE place, unlike a data output (which may " +
                      "fan out to many readers). Wire a Sequence node here if more than one thing " +
                      "should run from this point.";
                return false;
            }
        }

        // A PARAM NODE CANNOT LIVE INSIDE A FUNCTION, and until this check existed nothing said so.
        // EmitParam emits `Ldarg <index into Graph.Parameters>`. Inside a function body the argument
        // slots are the FUNCTION's own inputs (see CompileFunction, which rebases _varStoreArgIndex
        // onto fn.Inputs.Count for exactly this reason), so a Param node there reads whichever
        // function input happens to sit at the graph parameter's index -- a different value, silently,
        // or a CLR verification failure if the types differ. It compiled, ran, and produced a wrong
        // number, which is the worst of the three outcomes.
        //
        // Refused rather than rebased, because there is nothing to rebase ONTO: a function is called
        // from anywhere and does not receive the event graph's arguments at all. Passing the value in
        // as a function input is the only thing that can work, and the message says so.
        foreach (var node in Nodes.Values)
        {
            // "self" is listed for a graph built in code rather than parsed from text: the parser
            // rewrites Self into Param before Validate ever runs, so through that path this arm is
            // unreachable and the offender arrives spelled "Param".
            bool isParamNodeInFunc = node.Type.Equals("param", System.StringComparison.OrdinalIgnoreCase) ||
                                     node.Type.Equals("getparam", System.StringComparison.OrdinalIgnoreCase) ||
                                     node.Type.Equals("self", System.StringComparison.OrdinalIgnoreCase);
            if (!isParamNodeInFunc || node.FuncOwner == null) continue;
            err = $"Node '{node.Id}' is a {node.Type} node inside function '{node.FuncOwner}'. " +
                  "A function does not receive the event graph's parameters -- its arguments are its " +
                  "own declared inputs -- so this would have read whichever input sits at that index. " +
                  $"Add an input to '{node.FuncOwner}' and pass the value in at the call site instead.";
            return false;
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
