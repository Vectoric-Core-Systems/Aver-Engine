// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Minimal in-memory graph model: nodes with typed input/output pins, links between pins, and
// pinned constants on an input pin. A DAG of computation nodes; carries enough to compile to IL.

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

    // EXEC -- a control-flow pin, not a data pin, parsed like any other typed pin (`PIN node name
    // in/out exec`; see OcGraph.hpp's OcGraphLink comment for the format-level reasoning). A link
    // between two exec pins IS an exec link with no separate record for it; Validate()'s
    // type-equality check below already refuses mixing it with any other type, for free. See
    // GraphCompiler's PUSH VS PULL comment for what an exec pin means to the compiler.
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
    public required string NodeId { get; init; }  // See Pin.NodeId: string, both id formats.
    public required object Value { get; init; }  // float, int, or bool
}

/// A pinned value (constant) clamped to an input pin.
public class PinnedValue
{
    public required string NodeId { get; init; }  // See Pin.NodeId: string, both id formats.
    public required string PinName { get; init; }
    public required object Value { get; init; }  // float, int, or bool
}

/// A link connecting an output pin to an input pin.
public class Link
{
    public required string SourceNodeId { get; init; }  // Both string, both id formats (see Pin.NodeId).
    public required string SourcePinName { get; init; }
    public required string TargetNodeId { get; init; }
    public required string TargetPinName { get; init; }
}

/// A computation node in the graph.
public class Node
{
    public required string Id { get; init; }  // See Pin.NodeId: string, both id formats.
    public required string Type { get; init; }  // "Const", "Add", "Multiply", etc.
    public List<Pin> Pins { get; init; } = new();

    // Which graph PARAMETER a "param" node reads, e.g. "entity" or "time". Set from the NODE
    // line's "param=<name>" attribute. Null for every other node type.
    public string? ParamName { get; set; }

    // Which subgraph this node lives in ("func=<name>" NODE-line attribute); null means the event
    // graph (every pre-function graph). An attribute rather than a list on the function so a node
    // belongs to exactly one place by construction, with nothing to keep in sync -- and func=
    // rides in the same extraTokens as param=/field=/class=/var=, so the C++ reader needed no change.
    public string? FuncOwner { get; set; }

    // Which function a "callfunc" node calls ("call=<name>"). Separate from func= deliberately: a
    // call node both LIVES in one subgraph (func=) and CALLS another (call=) -- one attribute for
    // both would make a recursive call (a node inside Fib calling Fib) indistinguishable from a
    // node that merely lives there.
    public string? CallTarget { get; set; }

    // Which scene field a "getfield"/"setfield" node addresses (e.g. "CLocal.position"), from the
    // NODE line's "field=<qualifiedName>" attribute; null otherwise. Resolved to a dense field id
    // at COMPILE time (GraphCompiler), not here -- needs the live scene's field table.
    public string? FieldName { get; set; }

    // Which registered class a "spawn" node instantiates (e.g. "Widget"), from "class=<name>" --
    // same key=value mechanism as FieldName/ParamName. UNLIKE FieldName, this is NOT resolved to a
    // handle at compile time: a project's actor classes (declared by that project's own
    // Scripts.dll, later in boot order than the engine-global field table) are resolved by NAME at
    // invocation instead (see GraphInterop.SpawnForGraph).
    public string? ClassName { get; set; }

    // Which declared VAR a "getvar"/"setvar" node addresses (e.g. "score"), from "var=<name>" --
    // same key=value mechanism as ParamName/FieldName/ClassName. UNLIKE FieldName there is no
    // external table: a VAR is declared in THIS file, so Graph.Validate() checks it against
    // Variables directly.
    public string? VarName { get; set; }

    // The literal string a "setname" node writes via aver_scene_set_name (e.g. "Held_Weapon"),
    // from "name=<value>" -- same mechanism as ParamName/FieldName/ClassName/VarName, but the
    // first whose value IS the data written, not a lookup key (see GraphCompiler's
    // EmitSetName/EmitExecSetName). PinType has no String member, so a NODE-line attribute is the
    // only route a string reaches a node here.
    public string? NameValue { get; set; }

    // Which asset path a "setmesh" node writes (GraphInterop.SetMeshForGraph -> Entity.SetMesh ->
    // Assets.ObjectIdOf), e.g. "Content/Meshes/Prop.ocmesh", from "mesh=<path>" -- same "value IS
    // the data" family as NameValue; Assets.ObjectIdOf hashes it at INVOCATION time in
    // Aver.Framework, not here.
    public string? MeshPath { get; set; }

    // Which material name a "setmaterial" node writes (GraphInterop.SetMaterialForGraph ->
    // Entity.SetMaterial -> aver_scene_material), e.g. "M_Weapon", from "material=<name>" -- same
    // mechanism as MeshPath.
    public string? MaterialName { get; set; }

    // Which declared event a "fireevent" node fires on ANOTHER entity's graph, e.g. "OnHit", from
    // "event=<name>" -- same "value IS the data" family as NameValue (see its comment). The
    // literal name FireEventForGraph passes to the target's GraphHost.Fire.
    public string? EventName { get; set; }

    // Which socket an "attachtosocket" node hangs its entity on, e.g. "Hand_R", from
    // "socket=<name>" -- same family as MeshPath/MaterialName/EventName. Hashed with fnv1a64 at
    // invocation and matched against the parent rig's socket names by AnimSystem, which this
    // assembly cannot see.
    public string? SocketName { get; set; }

    // Which curve a "getanimcurve" node reads, e.g. "ReloadProgress", from "curve=<name>" -- same
    // mechanism as SocketName.
    public string? CurveName { get; set; }

    // The file a "savegame" node writes to, or "loadgame" reads from, e.g. "Saves/Slot1.ocsave",
    // from "path=<value>" -- same "value IS the data" family as NameValue. ONE property for BOTH
    // node types (unlike every other string attribute here): both just name "the file this node
    // touches", read vs written.
    public string? SavePath { get; set; }

    /// The sound file a "playsound"/"playsoundat" node plays, from `sound=<path>` -- same "value
    /// IS the data" family as NameValue/SavePath. ONE property for both node types (flat vs
    /// positioned playback name the same thing).
    public string? SoundPath { get; set; }

    /// The message a "printstring" node writes, from `text=<message>` -- same "value IS the data"
    /// family as SoundPath. Exists because Print/PrintInt need a VALUE wired to prove a branch
    /// ran, labelled only by auto-generated id ("print3 = 1"); PrintString needs nothing wired but
    /// exec and the author writes the label directly.
    public string? PrintText { get; set; }

    // Which skeleton asset a "setskeleton" node binds, e.g. "Content/Skeletons/Hero.ocskel", from
    // "skeleton=<path>" -- same mechanism as MeshPath (Assets.ObjectIdOf hashes it at invocation time).
    public string? SkeletonPath { get; set; }

    // Which animation clip a "playanimation" node plays, e.g. "Content/Anims/Run.ocanim", from
    // "clip=<path>" -- same mechanism as SkeletonPath. The loop flag is NOT here: it's runtime
    // data a graph may compute, so it rides a bool PIN instead (see GraphNodeDefs.hpp's "THIRD
    // input pin" comment).
    public string? ClipPath { get; set; }

    // Which control rig a "setcontrolrig" node binds, e.g. "Content/Rigs/ArmReach.ocrig", from
    // "rig=<path>" -- same mechanism as SkeletonPath/ClipPath. Weight is NOT here for ClipPath's
    // same reason: it rides a float PIN instead (runtime data, not edit-time naming).
    public string? RigPath { get; set; }

    // Which declared INPUT ACTION an "inputaction"/"inputactionpressed"/"inputactionreleased" node
    // names, e.g. "Jump" -- and, on "rebindaction"/"getactionkey", the action those two REQUIRE
    // (see each node's own doc comment in OcGraphParser.AddDefaultPins), from "action=<name>" (same
    // mechanism as ClassName/EventName/CurveName).
    // OPTIONAL on the three InputAction* nodes (predate this attribute, fall back to a plain Int
    // `action` pin carrying the `aver_fw_action_register`/`_find` handle; when set,
    // GraphInterop.ActionHandleForGraph resolves it at runtime instead and the pin is ignored; when
    // null, the pin is read exactly as it always was -- see
    // GraphCompiler.LoadActionHandle/PullActionHandle). REQUIRED on RebindAction/GetActionKey, which
    // have no pin fallback (RebindAction's inputs are `slot`/`key`; GetActionKey's is `slot`) --
    // GraphCompiler's EmitExecRebindAction/EmitGetActionKey/EmitPullGetActionKey enforce it
    // non-empty at COMPILE time, like Spawn's class= and SaveGame/LoadGame's path=.
    public string? ActionName { get; set; }
}

/// One parameter the compiled method accepts (e.g. the entity a graph drives, or the current time).
/// Declared via top-level `PARAM <name> <type>`; declaration order is the method's argument order.
/// Read by a "param" node's `param=<name>` attribute (Node.ParamName), not a second LINK/PINVAL-style wiring mechanism.
public class GraphParameter
{
    public required string Name { get; init; }
    public required PinType Type { get; init; }
}

/// One variable a GRAPH remembers between ticks -- e.g. a hit count, cooldown timer, round state.
/// Declared via `VAR <name> <type> [default]`, mirroring GraphParameter/PARAM but opposite: PARAM
/// is caller-supplied (GraphHost.LoadEventGraph maps entity/time/deltaTime, refuses anything else,
/// since Tick()/Fire() are the only things that can ever hand a value in); a VAR is owned by the
/// graph. Storage lives on the GraphHost instance (GraphVarStore), created at Load() and persisting
/// across Tick()/Fire() on that SAME host -- hosts sharing one .ocgraph file get independent
/// storage. Read/written by a GetVar/SetVar node's `var=<name>` (Node.VarName).
/// TYPE SET: Float/Int/Bool only (mirrors PARAM); Exec is rejected -- data, not control flow.
/// NOT PERSISTED across a restart, an Unload+Load cycle (a level/scene reload does this to every
/// script-hosted entity today), or a Load() replacing a still-live graph (hot-reload resets every
/// VAR to its default) -- the owner's explicitly accepted trade-off vs a scene-scratch component;
/// see GraphVarStore's "read before any write" contract.
public class GraphVariable
{
    public required string Name { get; init; }
    public required PinType Type { get; init; }

    // The value a fresh GraphVarStore seeds this variable with. Always a concrete float/int/bool of
    // the DECLARED type (never null) -- an omitted/unparseable `[default]` falls back to the
    // type's zero value (see OcGraphParser's VAR-parsing comment), like PIN's own default-value
    // convention. Typed `object` rather than three separate nullable fields, same reason as
    // ConstantOutput/PinnedValue.
    public required object Default { get; init; }
}

/// One entry in a class graph's COMPONENT TREE, from a `COMP <id> <Kind> [key=value]...` record.
/// THE SEMANTIC HALF: fields here are typed, while the C++ reader's OcGraphComponent keeps
/// everything as verbatim tokens (same split VAR follows); C++ guarantees a well-formed tree, this
/// class decides what a Kind is, what units a transform is in, and which attributes a Kind reads.
/// UNITS MATCH THE SCENE: centimetres, +Z up, rotation in DEGREES as YAW/PITCH/ROLL (Aver.Scene.Rot's
/// order) -- unlike CCamera.fovYRad's radians, because a transform is authored and an FOV is shader-read.
public class GraphComponent
{
    public required string Id { get; init; }

    /// The component kind: "Scene", "Mesh", "Light", "Camera", "SkeletalMesh" or "Particles". Held
    /// as the authored string, not an enum, so an unrecognised kind is REPORTED by name at spawn
    /// rather than silently coerced to a default (the same tolerance a NODE-line key=value gets, but louder).
    public required string Kind { get; init; }

    /// The id of the component this one hangs off, or null for the actor's own entity. The C++
    /// reader has already proved the parent chain is a cycle-free tree, so no cycle guard is needed here.
    public string? Parent { get; set; }

    /// Local transform relative to the parent. Centimetres; degrees; scale multiplier.
    public float[] Position { get; set; } = new float[3];
    public float[] RotationDeg { get; set; } = new float[3];
    public float[] Scale { get; set; } = { 1f, 1f, 1f };

    /// Every other key=value on the line (transform/parent keys removed -- those are the fields
    /// above). Kind-specific: `mesh=`/`material=` on a Mesh, `fov=`/`near=`/`far=` on a Camera,
    /// `effect=` on Particles. A dictionary, not a field per attribute, so a new Kind's own keys need no edit here.
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
/// event graph, with its own inputs, outputs, and body of nodes.
/// A REAL CALL, NOT AN INLINE EXPANSION: inlining needed no new emission code (the compiler
/// already re-emits pure expressions inline in EmitPullOutput) but was rejected on one fact,
/// established by spike rather than argument: a DynamicMethod here CAN emit a direct IL `Call` to
/// another DynamicMethod, including to ITSELF or one not yet written -- recursion is free, and an
/// inlined function cannot recurse. Blueprint's Function/Macro split draws the same line; this is
/// the Function.
/// ONE FILE, MANY FUNCTIONS: node ids stay a single FILE-WIDE namespace -- a function's body is
/// ordinary NODE/PIN/LINK records with a `func=` owner attribute riding the same extraTokens every
/// other attribute uses, so scoping ids per function would have broken the one duplicate-id check
/// the two readers already disagree about.
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
    /// DECLARED, NOT INFERRED: a function whose body only calls ANOTHER function is pure or impure
    /// transitively, which inspecting node types alone gets wrong -- Validate() checks the body against the stated intent instead.
    public bool IsPure { get; set; }

    /// The node id of this function's FuncEntry, and of its FuncReturn if it has one. Resolved by
    /// the parser from `func=` rather than declared in the FUNC record -- a function has exactly
    /// one of each by construction (Validate refuses a second), so naming them twice would just disagree.
    public string? EntryNodeId { get; set; }
    public string? ReturnNodeId { get; set; }
}

/// Which LANGUAGE a .ocgraph's nodes are written in -- the C# mirror of aver::fmt::OcGraphDomain
/// (modules/formats/include/aver/formats/OcGraph.hpp), which owns the authoritative comment.
/// Exists because one extension covers several unrelated languages: this compiler emits IL, a
/// material graph compiles to HLSL -- HostBridge.DeclareGraphClasses walks EVERY *.ocgraph under a
/// project, so without a marker it would try to make an actor class out of a material graph.
/// ABSENT IS Gameplay, since every graph written before the record existed is a gameplay graph and
/// a missing DOMAIN cannot be an error; an UNRECOGNISED name is Unknown, not Gameplay -- a name
/// this build has never heard of is safest skipped, not compiled anyway.
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
    public Dictionary<string, Node> Nodes { get; set; } = new();  // String keys: both id formats (see Pin.NodeId).
    public List<Link> Links { get; set; } = new();
    public List<ConstantOutput> ConstantOutputs { get; set; } = new();
    public List<PinnedValue> PinnedValues { get; set; } = new();
    public List<(string NodeId, string PinName)> Outputs { get; set; } = new();
    public List<GraphParameter> Parameters { get; set; } = new();  // Top-level PARAM records; empty means the compiled method takes no arguments.

    // Declared via top-level `VAR <name> <type> [default]` records (see GraphVariable for the full
    // storage/lifetime contract). Empty for every pre-VAR graph, like Parameters was empty before
    // PARAM -- GraphCompiler only appends a GraphVarStore delegate argument when this is non-empty,
    // so a VAR-less graph's delegate shape is unchanged.
    public List<GraphVariable> Variables { get; set; } = new();

    // Declared via top-level `FUNC`/`FUNCIN`/`FUNCOUT` records. Empty for every pre-function
    // graph; a reader that doesn't know FUNC skips it rather than failing, so an older runtime
    // keeps the file and just loses the function.
    public List<GraphFunction> Functions { get; set; } = new();

    // Declared via top-level `COMP <id> <Kind> [key=value]...` records, in FILE order (not tree
    // order -- a component may name a parent declared below it). Empty for every pre-component
    // graph, and for every graph that is not a class.
    //
    // Only meaningful alongside a non-null ClassName: it describes what a SPAWNED instance is made
    // of. Not refused without one -- a graph mid-edit, components authored before its CLASS line, is a normal state, not an error.
    public List<GraphComponent> Components { get; set; } = new();

    // Declared via top-level `ENTRY <nodeId> <eventName>` records -- which node begins the
    // PUSH/exec chain for a named event (e.g. "OnStart"/"OnTick"). Empty for pre-ENTRY graphs; see
    // OcGraph.hpp's OcGraphData::entryPoints for the backward-compat argument.
    public List<(string NodeId, string EventName)> EntryPoints { get; set; } = new();

    // Declared via an OPTIONAL top-level `CLASS <name> [parentName] [mesh=<path>] [material=<name>]`
    // record -- turns a plain .ocgraph into a spawnable actor CLASS, like a Blueprint asset carrying
    // a parent class and defaults. Null when absent (the common case: every pre-CLASS graph, plus
    // every project-utility graph that only computes values against GameApp.cpp's
    // discoverProjectGraphs; VAR/PARAM/ENTRY/NODE parsing and GraphHost.Tick() are unaffected). The
    // C++ reader has no "Class" case, so a CLASS line rides the same OwnedLineKind::Other bucket
    // PARAM/VAR use and round-trips byte-for-byte with zero reader changes -- verified via
    // OcGraphTest.exe's --roundtrip.
    // CONSUMED BY Aver.Scripting.Bridge's HostBridge (not this format layer, not GraphHost), which
    // registers ClassName as an aver_fw_class_declare(...) row with ClassParent as its parent, then
    // gives EVERY SPAWNED INSTANCE its own GraphHost bound to its own entity (HostBridge.cs's
    // "graph classes" region).
    public string? ClassName { get; set; }

    // The declared parent's class name, e.g. "Actor" or a project's own C# actor class. Defaults to
    // "Actor" when omitted (see OcGraphParser's CLASS-record comment; mirrors HostBridge's
    // BaseRegistryName default). Null (meaningless) when ClassName is null.
    public string? ClassParent { get; set; }

    // Optional mesh/material the class's entity gets as class DEFAULTS (via
    // aver_fw_class_add_component(CMeshRenderer) + aver_fw_class_set_default_i64/i32), so an
    // instance has a visible look before its own OnStart/OnTick runs -- notably before
    // aver_fw_spawn_preview, which never dispatches OnBeginPlay. Both null if the CLASS record
    // states neither; ClassMaterial needs a non-null ClassMesh.
    public string? ClassMesh { get; set; }
    public string? ClassMaterial { get; set; }

    // Optional first-/third-person camera default, from `view=firstperson`/`view=thirdperson`
    // (case-insensitive; unrecognised text warned and ignored -- see HostBridge.cs's DispBind, the
    // only consumer). Null leaves AverCharacter.CameraViewMode at its C# default (ThirdPerson).
    // EXISTS BECAUSE CameraViewMode is a plain public C# field, set only by a project's own C#
    // (Aver.Framework.SampleActor's PlayDemo, at the time this was added) -- no scene field, CLASS
    // attribute or node reached it, so a graph-only "first person" template was a contradiction,
    // the camera stayed behind the character. `view=` applies the same "attribute hands data to
    // spawned instances without a node" shape `mesh=`/`material=` already prove. Meaningless off a
    // non-Character ancestor -- HostBridge silently ignores it there rather than failing the whole class.
    public string? ClassView { get; set; }

    // Optional default pawn CLASS NAME, from `pawn=<ClassName>`. Null if omitted. Only meaningful
    // on a GameMode; HostBridge warns and ignores it elsewhere.
    // EXISTS BECAUSE aver_fw_begin_play already spawns and POSSESSES a GameMode's pawn via
    // ClassRecord::defaultPawnName (C# GameModes reach it through GameModeInfo.DefaultPawnClass),
    // but a GRAPH GameMode had no way to name one -- GameApp's camera (which follows
    // `aver_fw_controlled_pawn(...)`) stayed on its default and no `view=firstperson` reached it: a
    // first-person template that cannot be seen in first person was the symptom.
    // RESOLVED BY NAME AT SEAL (aver_fw_class_set_default_pawn) in a SECOND pass after every graph
    // class is declared -- naming a not-yet-declared pawn class inline would resolve to 0 depending on filename order.
    public string? ClassPawn { get; set; }

    // Optional player-controller CLASS NAME, from `controller=<ClassName>`. Only meaningful on a
    // GameMode, like ClassPawn.
    // REQUIRED FOR pawn= TO DO ANYTHING: aver_fw_begin_play possesses only when it has BOTH ("if
    // (ctrl && pawn)"), and the built-in `PlayerController` is ABSTRACT (aver_fw_spawn refuses it)
    // -- a GameMode naming no controller got ctrl == 0 and never possessed anything, however correct
    // its pawn: naming a pawn alone would have looked wired up and changed nothing.
    // A graph can supply one with no C#: ABSTRACT is deliberately NOT inherited
    // (kInheritableKindFlags), so `CLASS AN_FPController PlayerController` is concrete and
    // spawnable while still carrying the CONTROLLER flag possession checks -- the same as a Blueprint subclass of APlayerController.
    public string? ClassController { get; set; }

    /// Declares the `entity` parameter that `Self` nodes read, if the graph did not declare one
    /// itself. Called by OcGraphParser only when a Self node was seen (each already rewritten to a
    /// `Param` node naming "entity" at construction, since Node.Type is init-only). Runs BEFORE
    /// AddDefaultPins so the rewritten node gets its output pin typed from this parameter.
    /// EXISTS BECAUSE nearly every Scene, Character, Physics, Animation and Audio node takes an
    /// `entity` pin, and the editor has no way to write a PARAM record (OcGraphData models nodes,
    /// links, variables, components, functions and outputs, but no parameters), so a canvas-only
    /// graph could not name the entity it runs on -- `PARAM entity int` + a Param node is otherwise
    /// the ONLY route to that handle, which is why every gameplay graph in this repo is hand-written text.
    /// SUGAR, not a node the compiler knows -- costs nothing in the IL emitter, CompileFunction's
    /// argument rebasing, GraphHost's entity/time/deltaTime slot mapping, FireForEntity, or the C++
    /// writer, all of which already handle `PARAM entity int` and never see a Self node.
    /// VISIBLE CONSEQUENCE: a graph using Self gains an unwritten parameter, invisible to every
    /// by-name caller -- only Fire()'s raw positional overload counts arguments, and refuses a mismatch by naming the expected count.
    public bool ResolveSelfNodes(out string? err)
    {
        err = null;

        // Reuse a declaration the author already made rather than adding a second one -- mixing
        // `PARAM entity int` with a canvas Self node is ordinary once the editor can save files,
        // and two parameters would change the compiled method's arity.
        var declared = Parameters.FirstOrDefault(
            p => p.Name.Equals("entity", System.StringComparison.OrdinalIgnoreCase));
        if (declared == null)
        {
            Parameters.Add(new GraphParameter { Name = "entity", Type = PinType.Int });
            return true;
        }

        if (declared.Type != PinType.Int)
        {
            // A FLOAT CANNOT HOLD AN ENTITY HANDLE: handles start above 2^24, where a float's
            // mantissa runs out of integers, so the value rounds to a NEIGHBOURING (real, wrong)
            // entity. Refused outright -- silently accepting it looks like a framework fault, not a
            // typed-it-wrong fault: every call downstream succeeds, on somebody else's entity.
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
    /// Node ID comparison is case-SENSITIVE -- the C# parser and C++ writer both use string IDs and must normalize them consistently.
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

        // Check VAR declarations are unique -- same reason as PARAM above: two variables sharing a
        // name would make a GetVar/SetVar node's var= ambiguous.
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
        // unambiguous, same reason as PARAM/VAR above. The named node may be of ANY type (Sequence,
        // a hand-given-exec SetField, even a plain data node that just runs once) -- ENTRY says
        // WHERE a chain starts, not what shape it must have.
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
        // Runs BEFORE the Param/GetVar blocks below, same reason those run before Outputs: a node
        // whose func= names nothing gets no derived pins (see AddDefaultPins), so later checks
        // would fail with a generic "no pin" message, not the real cause.
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
                    // ONE return, not one per branch: outputs are read from locals AFTER the exec
                    // chain finishes (like CompileEntryPoint reads OUT records), so the method has a
                    // single Ret. Two FuncReturn nodes would race to write the same locals.
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

            // A WIRE MAY NOT LEAVE THE SUBGRAPH IT IS IN -- the rule that makes a function a function
            // rather than a naming convention: the event graph and each function compile to SEPARATE
            // methods, so a cross-link would reference another method body's locals, which the
            // emitter cannot express (dropped silently, or read as garbage). Only FuncEntry/FuncReturn
            // cross the boundary.
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

            // PURITY IS CHECKED TRANSITIVELY -- the whole reason it is declared, not inferred: a rule
            // looking only at node types present would call a function pure when it merely calls an
            // impure one. Iterating to a fixed point, not recursing, keeps a mutually recursive pair from spinning.
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

        // AN EXEC OUTPUT MAY DRIVE EXACTLY ONE LINK -- checked here so the EDITOR can say so. Already
        // enforced by GraphCompiler.FindExecTarget (throws InvalidOperationException at COMPILE time,
        // only for a reached node, as an exception not a validation error -- previously surfaced only
        // in the engine log at project open); here it badges the node the moment Validate runs.
        // DATA OUTPUTS ARE UNAFFECTED: a value may fan out freely; only control flow must say which
        // order it forks in (a Sequence node's job). Message leads with Node '<id>', not
        // 'nodeId.pinName', because GraphEditor.errorNodeId reads the first single-quoted token to badge it.
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

        // A PARAM NODE CANNOT LIVE INSIDE A FUNCTION: EmitParam emits `Ldarg <index into
        // Graph.Parameters>`, but inside a function body the argument slots are the FUNCTION's own
        // inputs (CompileFunction rebases _varStoreArgIndex onto fn.Inputs.Count) -- so a Param node
        // there silently reads the wrong input (compiles, runs, wrong number -- the worst outcome),
        // or fails CLR verification. Refused, not rebased -- there is nothing to rebase ONTO, since a
        // function receives no event-graph arguments at all.
        foreach (var node in Nodes.Values)
        {
            // "self" is for a graph built in code, not parsed from text: the parser rewrites Self
            // into Param before Validate runs, so the offender always arrives spelled "Param".
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

        // Check every Param node names a declared parameter, and an explicit output pin (a
        // hand-written PIN line, not the parser's default-pins path) agrees with PARAM's declared
        // type. Checked here too, not just in GraphCompiler, since Validate() also runs at Compile()
        // -- so a graph built programmatically (not through the text parser) gets the same check.
        // Deliberately BEFORE the Outputs check: a bad param= has no default output pin
        // (AddDefaultPins), so a LINK/OUT touching it would otherwise fail with a generic
        // "no output pin 'value'" message, not the param= cause.
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

        // Check every GetVar/SetVar node names a declared variable and its 'value' pin type agrees
        // with VAR -- mirrors the Param-node block above, including running BEFORE Outputs: an
        // undeclared var= produces no default pin (see OcGraphParser.AddDefaultPins), so it would
        // otherwise fail with a generic "no pin" message, not the real cause. GetVar's 'value' pin
        // is an OUTPUT; SetVar's is an INPUT -- `p.IsOutput == isGetVar` picks the right one without
        // two near-duplicate blocks.
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
