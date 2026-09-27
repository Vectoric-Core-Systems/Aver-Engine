#pragma once
// THE ONE node-type descriptor table for the .ocgraph editor: the Add-Node palette, node spawning
// and pin-type display all read it. Adding a node type is ONE call to node(...) in
// graphNodeCatalog() below (the table docs/VISUAL_SCRIPTING.md Slice 6 asks for). Pure data: no
// ImGui, no Engine, no scene:: -- testable headless.
//
// FOR STARTING SHAPE ONLY: an existing node instance carries its own pins in the file
// (aver::fmt::OcGraphNode::pins, OcGraph.hpp); drawing/editing reads THOSE, not this table, so a
// nonstandard pin set still renders. This table only supplies the palette's shape and a
// freshly-spawned node's initial pins.
//
// PROVENANCE: mirrors GraphCompiler.cs/OcGraphParser.cs's AddDefaultPins, as read 2026-08-13 --
// including the flow/exec vocabulary (Branch, Sequence, While, ForEach, OnStart, OnTick) copied
// field-for-field, pin-for-pin, same order, from AddDefaultPins' own "flow / exec nodes" section. A
// hand-kept copy, NOT generated, so the C++ build has no C# dependency; if GraphCompiler.cs's node
// set moves, update this table to match by hand.
// PARITY MATTERS MOST FOR FLOW TYPES: a spawned node's pins get written into the file as real PIN
// records on save (GraphEditor.cpp's add-node popup copies them verbatim), and AddDefaultPins skips
// any node with explicit pins already, so a mismatch here means an editor-authored graph and a
// hand/C#-authored one diverge (the "agree by coincidence" trap OcGraph.hpp's `outputs` comment
// warns about).
//
// field=/param=/class=: generic NODE-line key=value attributes, captured verbatim into
// OcGraphNode::extraTokens (OcGraph.cpp's parser keeps only a numeric x/y as position), so they
// round-trip load/save -- the grammar was never the gap. The FORMER gap, now closed: this editor had
// no property panel for ANY node, so extraTokens round-tripped opaquely but wasn't readable/editable
// from the GUI. A type's `attributes` list is what the details panel shows as labelled rows; anything
// else in extraTokens shows too, as a generic row (GraphEditorGeometry.hpp's computeAttributeRows).
#include <algorithm>    // stable_sort, for graphPaletteSearch's ranking
#include <cstdint>
#include <string>
#include <string_view>  // graphPaletteSearch takes its query as one
#include <utility>
#include <vector>

namespace aver::editor {

// One pin a freshly-spawned node of a given type starts with. Field names/order mirror
// aver::fmt::OcGraphPin exactly so a catalog entry converts into a real pin with no per-field mapping.
struct GraphPinSpec {
    std::string name;
    std::string type;         // "float"|"int"|"bool"|"string" (OcGraphPin::type); material pins also
                               // use "float2"/"float3"/"float4" -- see MaterialGraphHlsl.cpp's
                               // typeFromPin() and this header's Material section below.
    bool isOutput = false;
    std::string defaultValue; // only meaningful for input pins; empty = none
};

// One NODE-line key=value attribute a node type declares (param=/field=/class= today). Just
// (key, label): computeAttributeRows (GraphEditorGeometry.hpp) draws it as a labelled InputText row,
// taking a plain vector<pair<string,string>> rather than this type, to stay decoupled from the
// catalog (same reason computeNodeLayout takes `title` as a parameter).
struct GraphAttributeSpec {
    std::string key;   // matches the extraTokens `key=` half exactly, e.g. "class"
    std::string label; // shown in the details panel, e.g. "Class"
};

// Which .ocgraph DOMAIN(s) a node type may appear in. Mirrors aver::fmt::OcGraphDomain
// (OcGraph.hpp): kDomainGameplay = no DOMAIN record or `DOMAIN gameplay` (compiled to IL by
// GraphCompiler.cs); kDomainMaterial = `DOMAIN material` (compiled to HLSL by
// aver::pbr::compileMaterialGraph(), MaterialGraphHlsl.cpp). No flag mirrors ::Unknown -- that's an
// unrecognised DOMAIN, not a third kind of node.
//
// A BITMASK, NOT a copy of that enum: a loaded graph is unambiguously one domain, but a node TYPE
// STRING can be valid in both (e.g. ConstFloat means the same literal either way) while some names
// differ in SHAPE per domain (Add: scalar gameplay pins vs float3 material ones, see the Material
// section below) and need two differently-shaped entries under the same type name. The bitmask lets
// each case choose: kDomainBoth on one entry when shape-identical, two entries when not.
//
// Plain enum, not enum class: only ever combined/tested with `|`/`&`, so the implicit-int-conversion
// hazard enum class guards against doesn't apply here (same reasoning as EditorKeybinds.hpp's Scope).
enum GraphNodeDomain : std::uint32_t {
    kDomainGameplay = 1u << 0,
    kDomainMaterial = 1u << 1,
    kDomainBoth     = kDomainGameplay | kDomainMaterial,
};

// One entry in the node palette / spawn table.
struct GraphNodeDesc {
    std::string typeId;                // matches OcGraphNode::type; looked up case-insensitively
    std::string displayName;           // node header / palette label
    std::string category;              // palette grouping
    std::vector<GraphPinSpec> pins;    // inputs and outputs mixed; isOutput distinguishes which
    std::vector<GraphAttributeSpec> attributes; // NODE-line key=value attributes this type takes;
                                                 // empty for every type that has none (most of them).
    // Domain(s) this type belongs to -- see GraphNodeDomain above. Defaults to kDomainGameplay so
    // every pre-existing shorter braced-init `t.push_back({...})` above still compiles and means
    // what it did (a short init list leaves trailing members at their default) -- not one of those
    // ~200 lines needed touching. Only the Material section at the bottom of buildCatalog() sets
    // this explicitly.
    GraphNodeDomain domain = kDomainGameplay;
};

namespace detail {

inline GraphPinSpec pin(std::string name, std::string type, bool isOutput, std::string def = {}) {
    return GraphPinSpec{std::move(name), std::move(type), isOutput, std::move(def)};
}

inline GraphAttributeSpec attr(std::string key, std::string label) {
    return GraphAttributeSpec{std::move(key), std::move(label)};
}

// One line per node type. This is the "one entry" the build task and Slice 6 both call for.
inline std::vector<GraphNodeDesc> buildCatalog() {
    std::vector<GraphNodeDesc> t;
    // -- constants: a single output pin carrying the literal as its default value --
    // The `value` attribute is what makes these editable: a fresh Const spawned from the palette had
    // only its PIN default and the details panel shows nothing for a type with no declared
    // attributes, so every editor-built constant was permanently 0 until this was added. The parser
    // already read `value=` on a Const, by the node's DECLARED TYPE (OcGraphParser.cs) rather than
    // guessing from the literal -- declaring it here was all that was missing (most of why this
    // repo's graphs were hand-written text instead of authored in the editor).
    t.push_back({"ConstFloat", "Const Float", "Const", {pin("value", "float", true, "0")}, {attr("value", "Value")}});
    t.push_back({"ConstInt",   "Const Int",   "Const", {pin("value", "int",   true, "0")}, {attr("value", "Value")}});
    t.push_back({"ConstBool",  "Const Bool",  "Const", {pin("value", "bool",  true, "false")}, {attr("value", "Value")}});
    t.push_back({"Add",      "Add",      "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    // -- Print: no way existed to observe ANYTHING from inside a graph (no value, no branch, no
    //    event) short of an OUT record read from the GraphHost tick log. Labelled by NODE ID rather
    //    than an attribute: `NODE muzzleLen Print` already names itself uniquely, where an attribute
    //    would have needed a parser field, a writer field and an editor row to say the same for free.
    // -- VECTOR MATHS: every math node here was SCALAR in a centimetre, three-axis world, so a
    //    direction/distance/offset had to be spelled out component-by-component (AN_FPCharacter's
    //    wall of Const Float). No Vec3 PIN TYPE exists (Float/Int/Bool/Exec only), so these take and
    //    return loose components, the convention GetFieldVec3/SetFieldVec3 already established.
    t.push_back({"VecAdd", "Vec Add", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecSub", "Vec Subtract", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecScale", "Vec Scale", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("s", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecCross", "Vec Cross", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecNormalize", "Vec Normalize", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecLerp", "Vec Lerp", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("t", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecDot", "Vec Dot", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("result", "float", true)}});
    t.push_back({"VecLength", "Vec Length", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("result", "float", true)}});
    t.push_back({"VecDistance", "Vec Distance", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("result", "float", true)}});
    // -- THE ENGINE'S OWN API, reachable from a graph. Measured before these existed: 72/77 public
    //    members of Aver.Framework had no node (AverCharacter 1/18, Game 0/12, AverPlayerController
    //    0/4) -- the three classes a gameplay graph reaches for first.
    //
    //    Teleport != SetFieldVec3 on CLocal.position: the transform alone leaves the physics capsule
    //    behind and the character snaps back next step. Teleport moves both and clears velocity.
    t.push_back({"GetVelocity", "Get Velocity", "Character", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"IsGrounded", "Is Grounded", "Character", {pin("entity", "int", false), pin("grounded", "bool", true)}});
    t.push_back({"SetVelocity", "Set Velocity", "Character", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"Teleport", "Teleport", "Character", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetPlayerPawn", "Get Player Pawn", "Game", {pin("index", "int", false), pin("entity", "int", true)}});
    t.push_back({"GetPlayerController", "Get Player Controller", "Game", {pin("index", "int", false), pin("entity", "int", true)}});
    t.push_back({"GetGameMode", "Get Game Mode", "Game", {pin("entity", "int", true)}});
    t.push_back({"IsPlaying", "Is Playing", "Game", {pin("playing", "bool", true)}});
    t.push_back({"Possess", "Possess", "Game", {pin("exec", "exec", false), pin("controller", "int", false), pin("pawn", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"Unpossess", "Unpossess", "Game", {pin("exec", "exec", false), pin("controller", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});

    // -- SaveGame / LoadGame: no entity pin at all, unlike everything else in this category -- both
    //    act on the WHOLE world. path= names the file, the same NODE-line-attribute-as-data
    //    mechanism socket=/curve=/mesh= above already use, because PinType has no string member.
    t.push_back({"SaveGame", "Save Game", "Game", {pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("path", "Path")}});
    t.push_back({"LoadGame", "Load Game", "Game", {pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("path", "Path")}});
    // -- CONVERSION: Validate refuses a link whose pins differ in type (Graph.cs's srcPin.Type !=
    //    tgtPin.Type), so an int/bool cannot reach a float input without an explicit, visible cast
    //    node (Unreal converts silently with a cast bubble; this vocabulary has none).
    //    FloatToInt TRUNCATES toward zero (C# (int)f); Floor exists for the other rounding.
    // INT TO FLOAT IS LOSSY ABOVE 2^24 (float32's 24 mantissa bits; only EVEN integers survive past
    // 16777216) -- and ENTITY HANDLES START AT 16777216, so converting one silently rounds to its
    // neighbour -- cost an hour debugging what read exactly like the engine handing back the wrong
    // entity: a controller printed as 16777224 while the ABI returned 16777225. Use PrintInt for
    // handles.
    t.push_back({"IntToFloat", "Int To Float", "Convert", {pin("a", "int", false), pin("result", "float", true)}});
    t.push_back({"BoolToFloat", "Bool To Float", "Convert", {pin("a", "bool", false), pin("result", "float", true)}});
    t.push_back({"FloatToInt", "Float To Int", "Convert", {pin("a", "float", false), pin("result", "int", true)}});
    // PrintInt exists because Print takes a float and a float cannot hold an entity handle --
    // see the IntToFloat note above. Anything counting entities, indices or ids wants this one.
    // -- THE ENTITY TRANSFORM: GetFieldVec3 on CLocal.position only half covered this. LOCAL IS NOT
    //    WORLD (a gun parented to a camera keeps the same local position forever), so a graph
    //    measuring distance needs the world one. The three axis nodes are the transform's own
    //    orientation, distinct from GetForward, which reads an AverCharacter's look + pitch clamp.
    t.push_back({"GetWorldPosition", "Get World Position", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetEntityForward", "Get Forward Axis", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetEntityRight", "Get Right Axis", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetEntityUp", "Get Up Axis", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetLocalScale", "Get Local Scale", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"IsAlive", "Is Alive", "Transform", {pin("entity", "int", false), pin("alive", "bool", true)}});
    t.push_back({"IsActor", "Is Actor", "Transform", {pin("entity", "int", false), pin("isActor", "bool", true)}});
    t.push_back({"Translate", "Translate", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // SetLocalPosition shares SetLocalScale's pin shape exactly (OcGraphParser's note on the pair):
    // moving and resizing shouldn't be two different things to learn. Translate is its RELATIVE peer.
    t.push_back({"SetLocalPosition", "Set Local Position", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // THE PALETTE COULD MOVE, SCALE, PARENT AND DESTROY AN ENTITY BUT NOT TURN ONE (turret tracking,
    // AI facing, a swinging door): Entity::SetLocalRotation was implemented but unreachable from a
    // graph, the only Transform verb missing.
    //
    // YAW/PITCH/ROLL IN DEGREES, not x/y/z -- load-bearing, not cosmetic: with no quaternion pin
    // type, three floats named x/y/z next to a position node's own x/y/z is how someone wires roll
    // into yaw. The pin names alone carry which axis is which.
    //
    // Values mean what .ocmap's PLACE records mean: the bridge composes them via Rot::ToQuat, not a
    // second Euler convention.
    t.push_back({"SetLocalRotation", "Set Local Rotation", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("yaw", "float", false), pin("pitch", "float", false), pin("roll", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // Yaw and pitch only, roll left at zero -- rolling toward a target is what a stunt plane does,
    // not what anything aiming does. Same pin shape as Set Local Position on purpose: "move there"
    // and "face there" take the same three numbers and should not be two things to learn.
    t.push_back({"LookAt", "Look At", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetLocalScale", "Set Local Scale", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"DestroyEntity", "Destroy Entity", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // -- PHYSICS. A BODY IS NOT AN ENTITY: a body is a Jolt handle with shape+velocity, an entity is
    //    a scene node that may or may not own one, SetBodyEntity bridges them. A body with no owner
    //    stamped on reports nothing to Raycast, so a graph-built trigger is invisible to it. Body
    //    handles ride on INT pins. OverlapSphere is deliberately absent: it returns an ARRAY, and
    //    there's no container pin.
    t.push_back({"GetBodyPosition", "Get Body Position", "Physics", {pin("body", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetBodyVelocity", "Get Body Velocity", "Physics", {pin("body", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"IsBodyValid", "Is Body Valid", "Physics", {pin("body", "int", false), pin("valid", "bool", true)}});
    t.push_back({"GetBodyCount", "Body Count", "Physics", {pin("count", "int", true)}});
    //    IsPhysicsReady / GetFixedStep: pure STATUS reads (Physics.Ready/Physics.FixedStep), no exec,
    //    no inputs. Needed because a graph adding bodies before aver_phys_init has run gets silent
    //    zeros back from every creator and, until now, had no way to ask first. GetFixedStep lets
    //    hand-integration match the sim's real step instead of a hardcoded 1/60.
    t.push_back({"IsPhysicsReady", "Is Physics Ready", "Physics", {pin("ready", "bool", true)}});
    t.push_back({"GetFixedStep", "Fixed Step", "Physics", {pin("seconds", "float", true)}});
    t.push_back({"RaycastAny", "Raycast Any", "Physics", {pin("originX", "float", false), pin("originY", "float", false), pin("originZ", "float", false), pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false), pin("maxDist", "float", false), pin("hit", "bool", true)}});
    t.push_back({"SetBodyPosition", "Set Body Position", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetBodyVelocity", "Set Body Velocity", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddBodyVelocity", "Add Body Velocity", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"DestroyBody", "Destroy Body", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetBodyEntity", "Set Body Entity", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetGravity", "Set Gravity", "Physics", {pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddStaticBox", "Add Static Box", "Physics", {pin("exec", "exec", false), pin("cx", "float", false), pin("cy", "float", false), pin("cz", "float", false), pin("hx", "float", false), pin("hy", "float", false), pin("hz", "float", false), pin("then", "exec", true), pin("body", "int", true)}});
    t.push_back({"AddDynamicBox", "Add Dynamic Box", "Physics", {pin("exec", "exec", false), pin("cx", "float", false), pin("cy", "float", false), pin("cz", "float", false), pin("hx", "float", false), pin("hy", "float", false), pin("hz", "float", false), pin("mass", "float", false), pin("then", "exec", true), pin("body", "int", true)}});
    t.push_back({"AddDynamicSphere", "Add Dynamic Sphere", "Physics", {pin("exec", "exec", false), pin("cx", "float", false), pin("cy", "float", false), pin("cz", "float", false), pin("radius", "float", false), pin("mass", "float", false), pin("then", "exec", true), pin("body", "int", true)}});
    t.push_back({"AddSensorBox", "Add Sensor Box", "Physics", {pin("exec", "exec", false), pin("cx", "float", false), pin("cy", "float", false), pin("cz", "float", false), pin("hx", "float", false), pin("hy", "float", false), pin("hz", "float", false), pin("then", "exec", true), pin("body", "int", true)}});
    t.push_back({"AddSensorSphere", "Add Sensor Sphere", "Physics", {pin("exec", "exec", false), pin("cx", "float", false), pin("cy", "float", false), pin("cz", "float", false), pin("radius", "float", false), pin("then", "exec", true), pin("body", "int", true)}});
    t.push_back({"SphereCast", "Sphere Cast", "Physics", {pin("exec", "exec", false), pin("originX", "float", false), pin("originY", "float", false), pin("originZ", "float", false), pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false), pin("maxDist", "float", false), pin("radius", "float", false), pin("then", "exec", true), pin("hit", "bool", true), pin("body", "int", true), pin("pointX", "float", true), pin("pointY", "float", true), pin("pointZ", "float", true)}});

    // -- PHYSICS: FORCES, MATERIAL, MOTION AND LAYERS, on the body handles the creators above return.
    //    A force/torque lasts one step and must be reapplied to push continuously; an impulse changes
    //    velocity instantly and doesn't accumulate (units: force kg*cm/s^2, torque kg*cm^2/s^2, see
    //    Aver.Physics/Body.cs). Angular velocity is RADIANS/s, never degrees, matching every rotation here.
    t.push_back({"AddForce", "Add Force", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddImpulse", "Add Impulse", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddTorque", "Add Torque", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddAngularImpulse", "Add Angular Impulse", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetBodyAngularVelocity", "Get Body Angular Velocity", "Physics", {pin("body", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"SetBodyAngularVelocity", "Set Body Angular Velocity", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    //    Material and mass: mass applies to DYNAMIC BODIES ONLY -- static/kinematic have infinite
    //    mass by definition, so SetBodyMass on one fails (success=false) rather than changing the body.
    t.push_back({"SetBodyFriction", "Set Body Friction", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("friction", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetBodyRestitution", "Set Body Restitution", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("restitution", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetBodyGravityFactor", "Set Body Gravity Factor", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("factor", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetBodyMass", "Set Body Mass", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("mass", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetBodyMass", "Get Body Mass", "Physics", {pin("body", "int", false), pin("mass", "float", true), pin("success", "bool", true)}});
    //    Motion type and sleeping: motionType is an INT PIN, Aver.Physics.MotionType's numbering
    //    (0 Static, 1 Kinematic, 2 Dynamic). GetBodyMotionType returns -1 for a dead handle, hence success.
    t.push_back({"SetBodyMotionType", "Set Body Motion Type", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("motionType", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetBodyMotionType", "Get Body Motion Type", "Physics", {pin("body", "int", false), pin("motionType", "int", true), pin("success", "bool", true)}});
    t.push_back({"ActivateBody", "Activate Body", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"IsBodyActive", "Is Body Active", "Physics", {pin("body", "int", false), pin("active", "bool", true)}});
    //    Layers: 0..15, the same bitmask family Physics.SetLayerCollision (below) enables/disables
    //    pairs of. Changing a body's layer never changes whether it is static or dynamic.
    t.push_back({"SetBodyLayer", "Set Body Layer", "Physics", {pin("exec", "exec", false), pin("body", "int", false), pin("layer", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetBodyLayer", "Get Body Layer", "Physics", {pin("body", "int", false), pin("layer", "int", true), pin("success", "bool", true)}});
    //    SetLayerCollision has NO body pin -- it edits the world's shared layer matrix, SYMMETRIC
    //    ((a,b) also sets (b,a)), the same matrix a level teardown resets to all-colliding.
    t.push_back({"SetLayerCollision", "Set Layer Collision", "Physics", {pin("exec", "exec", false), pin("layerA", "int", false), pin("layerB", "int", false), pin("collide", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});

    // -- JOINTS: a constraint between two body handles, created ONCE at a WORLD-SPACE point/axis --
    //    move the bodies into place FIRST, then join them there (like Add*Box). BODY B == 0 (an
    //    unwired int pin's default) JOINS BODY A TO THE WORLD instead of to nothing -- like a door
    //    hinged to a wall that isn't itself simulated, not a joint with a missing argument. A JOINT
    //    HANDLE RIDES AN INT PIN LIKE A BODY HANDLE BUT THE TWO ARE NOT INTERCHANGEABLE (Jolt's
    //    ranges overlap, no type system catches it). Angles RADIANS, distances/points CENTIMETRES.
    //    Hinge/Slider's second axis trio (nx/ny/nz) MUST be perpendicular to the first: the zero
    //    reference, not a travel direction.
    t.push_back({"JointFixed", "Joint: Fixed", "Physics", {pin("exec", "exec", false), pin("bodyA", "int", false), pin("bodyB", "int", false), pin("px", "float", false), pin("py", "float", false), pin("pz", "float", false), pin("axX", "float", false), pin("axY", "float", false), pin("axZ", "float", false), pin("ayX", "float", false), pin("ayY", "float", false), pin("ayZ", "float", false), pin("then", "exec", true), pin("joint", "int", true)}});
    t.push_back({"JointPoint", "Joint: Point", "Physics", {pin("exec", "exec", false), pin("bodyA", "int", false), pin("bodyB", "int", false), pin("px", "float", false), pin("py", "float", false), pin("pz", "float", false), pin("then", "exec", true), pin("joint", "int", true)}});
    t.push_back({"JointDistance", "Joint: Distance", "Physics", {pin("exec", "exec", false), pin("bodyA", "int", false), pin("bodyB", "int", false), pin("paX", "float", false), pin("paY", "float", false), pin("paZ", "float", false), pin("pbX", "float", false), pin("pbY", "float", false), pin("pbZ", "float", false), pin("minDist", "float", false), pin("maxDist", "float", false), pin("then", "exec", true), pin("joint", "int", true)}});
    t.push_back({"JointHinge", "Joint: Hinge", "Physics", {pin("exec", "exec", false), pin("bodyA", "int", false), pin("bodyB", "int", false), pin("px", "float", false), pin("py", "float", false), pin("pz", "float", false), pin("hx", "float", false), pin("hy", "float", false), pin("hz", "float", false), pin("nx", "float", false), pin("ny", "float", false), pin("nz", "float", false), pin("minAngleRad", "float", false), pin("maxAngleRad", "float", false), pin("then", "exec", true), pin("joint", "int", true)}});
    t.push_back({"JointSlider", "Joint: Slider", "Physics", {pin("exec", "exec", false), pin("bodyA", "int", false), pin("bodyB", "int", false), pin("px", "float", false), pin("py", "float", false), pin("pz", "float", false), pin("sx", "float", false), pin("sy", "float", false), pin("sz", "float", false), pin("nx", "float", false), pin("ny", "float", false), pin("nz", "float", false), pin("minCm", "float", false), pin("maxCm", "float", false), pin("then", "exec", true), pin("joint", "int", true)}});
    //    Motor/limit/enable/remove/value key off the JOINT handle, never a body. state on
    //    JointSetMotor is Aver.Physics.MotorState (0 Off, 1 Velocity [target rad or cm/s], 2 Position
    //    [target absolute rad/cm]). Always axis 0 -- every named joint above has at most one
    //    motorised axis; the six-DOF per-axis motor isn't exposed here.
    t.push_back({"JointSetMotor", "Joint Set Motor", "Physics", {pin("exec", "exec", false), pin("joint", "int", false), pin("state", "int", false), pin("target", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"JointSetEnabled", "Joint Set Enabled", "Physics", {pin("exec", "exec", false), pin("joint", "int", false), pin("enabled", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"JointRemove", "Joint Remove", "Physics", {pin("exec", "exec", false), pin("joint", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetJointValue", "Get Joint Value", "Physics", {pin("joint", "int", false), pin("value", "float", true), pin("success", "bool", true)}});

    // -- FUNCTION: the three node types a user-defined function is made of, present here only for
    //    DISPLAY NAME and HEADER COLOUR -- the palette skips "Function" entirely since none of the
    //    three is placed by an author directly (a second FuncEntry breaks the function -- Validate:
    //    "a function begins in exactly one place"; a bare FuncReturn has no FUNCOUT to fill, a
    //    CallFunc has no pin shape at all until it knows its callee). The Functions panel creates
    //    them with the right pins. PINS HERE ARE EMPTY: each takes its real pins from a FUNC
    //    declaration, derived by the single place that does so, GraphEditor::resyncFunctionNodePins,
    //    which agrees pin-for-pin with AddDefaultPins.
    t.push_back({"FuncEntry", "Function Entry", "Function", {}});
    t.push_back({"FuncReturn", "Return", "Function", {}});
    t.push_back({"CallFunc", "Call Function", "Function", {}});

    // -- TAGS AND VISIBILITY: Entity's own tag bitmask, unreachable from a graph until now. A tag is
    //    an int bit pattern, not a string, so it needs none of class=/name=/var='s attribute
    //    machinery -- and a graph can COMPUTE a mask, which a string attribute never could.
    t.push_back({"SetVisible", "Set Visible", "Scene", {pin("exec", "exec", false), pin("entity", "int", false),
        pin("visible", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddTag", "Add Tag", "Scene", {pin("exec", "exec", false), pin("entity", "int", false),
        pin("mask", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"RemoveTag", "Remove Tag", "Scene", {pin("exec", "exec", false), pin("entity", "int", false),
        pin("mask", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"HasTag", "Has Tag", "Scene", {pin("entity", "int", false), pin("mask", "int", false),
        pin("has", "bool", true)}});
    t.push_back({"GetTags", "Get Tags", "Scene", {pin("entity", "int", false), pin("mask", "int", true)}});

    // -- SWITCH: Blueprint's Switch on Int -- routes the exec chain to ONE of several outputs by an
    //    integer instead of nesting Branches (a three-way choice used to cost two Branch+compare
    //    pairs). FOUR CASES PLUS A DEFAULT, fixed rather than grown: pins derive from a node's TYPE
    //    (AddDefaultPins), so a variable count would need hand-kept PIN records; a fifth case is a
    //    second Switch off `default`. `taken` reports which output fired (-1 for default), the same
    //    reason Branch has `tookTrue`.
    t.push_back({"SwitchInt", "Switch on Int", "Flow", {pin("exec", "exec", false), pin("selector", "int", false), pin("case0", "exec", true), pin("case1", "exec", true), pin("case2", "exec", true), pin("case3", "exec", true), pin("default", "exec", true), pin("taken", "int", true)}});

    // -- REROUTE: returns exactly what it was given, existing only to bend a WIRE. Ordinary small
    //    nodes rather than Blueprint's bare dot, since the pin-drawing code already handles one pin
    //    per side. ONE PER TYPE (four nodes, not one): Validate refuses a link whose pins differ in
    //    type and there are no generics here; a wildcard type would weaken that check. Compile to
    //    NOTHING: a data reroute emits its input with no instruction of its own; the exec one falls
    //    through to the usual fan-out.
    t.push_back({"RerouteFloat", "Reroute (Float)", "Flow", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"RerouteInt", "Reroute (Int)", "Flow", {pin("a", "int", false), pin("result", "int", true)}});
    t.push_back({"RerouteBool", "Reroute (Bool)", "Flow", {pin("a", "bool", false), pin("result", "bool", true)}});
    t.push_back({"RerouteExec", "Reroute (Exec)", "Flow", {pin("exec", "exec", false), pin("then", "exec", true)}});
    t.push_back({"PrintInt", "Print Int", "Debug", {
        pin("exec", "exec", false), pin("value", "int", false), pin("then", "exec", true)}});
    t.push_back({"Print", "Print", "Debug", {
        pin("exec", "exec", false), pin("value", "float", false), pin("then", "exec", true)}});
    // -- PRINT STRING: answers "did control flow reach here, and in what order" -- Print/PrintInt
    //    can't, since both need a VALUE wired to say anything. No string PIN type exists
    //    (Float/Int/Bool/Exec only), so the message is a NODE-line ATTRIBUTE -- the same "the value
    //    IS the data" treatment sound=/mesh=/clip= get. Also fixes Print's labelling: it logs
    //    "print3 = 1" (the auto-generated id); here the author writes the label directly.
    t.push_back({"PrintString", "Print String", "Debug", {
        pin("exec", "exec", false), pin("then", "exec", true)}, {attr("text", "Text")}});
    t.push_back({"Multiply", "Multiply", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Subtract", "Subtract", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Divide",   "Divide",   "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    // -- trig (concurrent-workflow additions) --
    t.push_back({"Sin", "Sin", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Cos", "Cos", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    // -- math: the standard library, previously unexpressable in a graph --------------
    // Every one is a PURE VALUE node (no exec pins), composing into either compiler. Names are the
    // .ocgraph node types verbatim -- a mismatch here produces a node that saves and never loads.
    t.push_back({"Min", "Min", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Max", "Max", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Mod", "Modulo", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Pow", "Power", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Abs", "Absolute", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Negate", "Negate", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Sqrt", "Square Root", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Floor", "Floor", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Ceil", "Ceiling", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Round", "Round", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Saturate", "Saturate (0..1)", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Clamp", "Clamp", "Math", {pin("a", "float", false), pin("min", "float", false), pin("max", "float", false), pin("result", "float", true)}});
    t.push_back({"Lerp", "Lerp", "Math", {pin("a", "float", false), pin("b", "float", false), pin("t", "float", false), pin("result", "float", true)}});
    // -- gated flow control: nodes that REMEMBER between activations, in the same per-instance store
    //    a VAR uses (two entities sharing one graph file gate independently). reset/open/close are
    //    BOOL inputs, not exec pins, because an activation carries no record of which pin it arrived on.
    t.push_back({"DoOnce", "Do Once", "Flow", {pin("exec", "exec", false), pin("reset", "bool", false), pin("then", "exec", true)}});
    t.push_back({"Gate", "Gate", "Flow", {pin("exec", "exec", false), pin("open", "bool", false), pin("close", "bool", false), pin("then", "exec", true)}});
    t.push_back({"FlipFlop", "Flip Flop", "Flow", {pin("exec", "exec", false), pin("a", "exec", true), pin("b", "exec", true), pin("isA", "bool", true)}});
    // -- logic --
    // BOOLEAN OPERATORS, previously absent entirely: "A and B" meant nesting Branches, OR meant
    // restructuring everything downstream, and NOT meant swapping two exec wires.
    t.push_back({"And", "AND", "Logic", {pin("a", "bool", false), pin("b", "bool", false), pin("result", "bool", true)}});
    t.push_back({"Or", "OR", "Logic", {pin("a", "bool", false), pin("b", "bool", false), pin("result", "bool", true)}});
    t.push_back({"Xor", "XOR", "Logic", {pin("a", "bool", false), pin("b", "bool", false), pin("result", "bool", true)}});
    t.push_back({"Not", "NOT", "Logic", {pin("a", "bool", false), pin("result", "bool", true)}});
    // The comparisons `Compare` alone could not make: it is a strict a > b, so >= needed
    // Compare plus a NOT that did not exist, and equality was simply unreachable.
    t.push_back({"Greater", "a > b", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    t.push_back({"GreaterEqual", "a >= b", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    t.push_back({"Less", "a < b", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    t.push_back({"LessEqual", "a <= b", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    t.push_back({"Equal", "a == b", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    t.push_back({"NotEqual", "a != b", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    t.push_back({"Compare", "Compare", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    // -- scene field access: entity id in, value in/out. field= names which scene field -- see the
    //    header comment above and this table's `attributes` field. --
    t.push_back({"GetField", "Get Field", "Scene", {pin("entity", "int", false), pin("value", "float", true)},
        {attr("field", "Field")}});
    t.push_back({"SetField", "Set Field", "Scene", {pin("entity", "int", false), pin("value", "float", false), pin("success", "bool", true)},
        {attr("field", "Field")}});
    // -- GetField/SetField's Vec3 siblings: a Vec3-kind field (CLocal.position, CLight.colour, ...)
    //    as three float pins rather than a new pin TYPE -- see GraphCompiler.cs's
    //    FieldKindVec3/RequireVec3Field. Pins copied field-for-field from AddDefaultPins's
    //    "getfieldvec3"/"setfieldvec3" cases; no exec pins by default, same as GetField/SetField.
    t.push_back({"GetFieldVec3", "Get Field (Vec3)", "Scene", {
        pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)},
        {attr("field", "Field")}});
    t.push_back({"SetFieldVec3", "Set Field (Vec3)", "Scene", {
        pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("success", "bool", true)},
        {attr("field", "Field")}});
    // -- GetForward: where a Character is LOOKING, plus its eye position. Filed under Scene, not
    //    Input, because it reads accumulated STATE (yaw/pitch after clamp), not this frame's device
    //    movement -- reading it twice gives the same answer twice, the property that decides the
    //    grouping. Six outputs, no exec pins (AddDefaultPins' "getforward"); eye position rides along
    //    since a direction with no origin can't build a ray (GraphInterop.LookDirectionForGraph).
    // -- Jump: one call into AverCharacter.Jump, which declines mid-air by itself -- wiring straight
    //    to a key gets single jumps, no flight, with no testing needed. `jumped` reports whether it
    //    actually happened, which "the key was pressed" is not.
    t.push_back({"Jump", "Jump", "Actor", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("then", "exec", true), pin("jumped", "bool", true)},
        {}});
    // -- GetViewEntity: the CAMERA node a character looks through. A first-person viewmodel parents
    //    to this, not the character -- parenting a gun to the pawn leaves it behind as the camera
    //    pitches. Filed under Scene beside GetForward: it reads state, not this frame's input.
    t.push_back({"GetViewEntity", "Get View Entity", "Scene", {
        pin("entity", "int", false), pin("view", "int", true), pin("success", "bool", true)},
        {}});
    t.push_back({"GetForward", "Get Forward (Look)", "Scene", {
        pin("entity", "int", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true),
        pin("eyeX", "float", true), pin("eyeY", "float", true), pin("eyeZ", "float", true),
        pin("success", "bool", true)},
        {}});
    // -- MouseDelta / MoveAxis: continuous input -- look and move, neither expressible through
    //    InputKey's digital state. Exec pins by default (mirroring Spawn/Raycast): one frame's input
    //    must cost exactly one native call regardless of pin count (GraphCompiler.cs's
    //    IsExecCapableMouseDeltaType/MoveAxisType) -- not because either read is itself expensive,
    //    only for that one-call-per-frame guarantee. MoveAxis has no "z": Input.MoveAxis's Z is
    //    hardcoded 0 always (Aver.Framework/Input.cs). LOW-LEVEL PATH: reads the device directly, no
    //    name/rebinding -- InputAction (below) is the one to use for anything reconfigurable.
    t.push_back({"MouseDelta", "Mouse Delta", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true),
        pin("deltaX", "float", true), pin("deltaY", "float", true), pin("wheel", "float", true)}});
    t.push_back({"MoveAxis", "Move Axis", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true),
        pin("forward", "float", true), pin("right", "float", true)}});
    // -- InputKey: digital key state, the discrete half of input beside MouseDelta/MoveAxis. NO EXEC
    //    PINS, deliberately: a polled key read is idempotent within a frame, so there's nothing to
    //    cache or anchor a visit to (unlike its neighbours, which fill several outputs per call).
    //    `key` is a plain int -- no symbolic enum lookup, so an author writes Aver.Framework's Key
    //    enum value directly.
    t.push_back({"InputKey", "Input Key", "Input", {
        pin("key", "int", false), pin("down", "bool", true)}});
    // -- InputKeyPressed / InputKeyReleased: the EDGE, where InputKey gives the STATE. `down` stays
    //    true every held frame, wrong for jump/fire-semi-auto/toggle, which need once-per-press;
    //    building that from InputKey needs a DoOnce+variable per key, but the ABI already answers it
    //    directly (aver_fw_input_key_pressed/_released). `triggered`, not `down`: it's an EVENT.
    t.push_back({"InputKeyPressed", "Input Key Pressed", "Input", {
        pin("key", "int", false), pin("triggered", "bool", true)}});
    t.push_back({"InputKeyReleased", "Input Key Released", "Input", {
        pin("key", "int", false), pin("triggered", "bool", true)}});
    // -- InputAction / InputActionPressed / InputActionReleased: the PREFERRED path over
    //    InputKey/MouseDelta/MoveAxis -- one named, rebindable ACTION (aver_fw_action_register/
    //    bind/value2/held/pressed/released, framework_abi.h's Named Actions section, minor 5)
    //    instead of a literal key or raw axis. `action` is a HANDLE (aver_fw_action_register/_find's
    //    int), NOT a name: no string PinType exists, and the name-by-attribute mechanism
    //    ClassName/EventName/CurveName use lives on Node in Graph.cs, outside this slice's files
    //    (see GraphCompiler.cs's EmitInputAction, incl. why less stable to author than InputKey's
    //    "key"). Mirrors InputKey's shape (NO exec pins, pure array-scan reads) but widens `down`
    //    into float2 + held.
    //
    // action= (OWNER DECISION, 2026-09-17): when present, GraphInterop.ActionHandleForGraph(name)
    // supplies the handle instead of the `action` pin, which is then ignored. OPTIONAL, unlike
    // RebindAction/GetActionKey's action= below: a pre-existing graph still wires the pin by hand.
    t.push_back({"InputAction", "Input Action", "Input", {
        pin("action", "int", false),
        pin("x", "float", true), pin("y", "float", true), pin("held", "bool", true)}, {attr("action", "Action")}});
    t.push_back({"InputActionPressed", "Input Action Pressed", "Input", {
        pin("action", "int", false), pin("triggered", "bool", true)}, {attr("action", "Action")}});
    t.push_back({"InputActionReleased", "Input Action Released", "Input", {
        pin("action", "int", false), pin("triggered", "bool", true)}, {attr("action", "Action")}});
    // -- REBINDABLE INPUT: SaveInputBindings/LoadInputBindings/ResetInputBindings/RebindAction/
    //    GetActionKey/GetPressedKey -- Unreal-Enhanced-Input-style rebinding menu with no C# at all
    //    (OWNER DECISION, 2026-09-17; Aver.Framework EnhancedInput.* via GraphInterop's *ForGraph
    //    wrappers). All six read/write the SAME pushed-context bindings InputAction/Pressed/Released
    //    already use -- there is no separate "graph-owned" binding table -- so a rebind is visible
    //    to those three next frame, scheme-loaded context included (InputScheme.cs pushes a loaded
    //    .ocinput through the same EnhancedInput.AddContext path).
    //
    //    `action=` NAMES THE ACTION (not a handle, unlike InputAction's pin, predating name-by-
    //    attribute) -- all six are new. SAVE/LOAD/RESET ARE EXEC-ONLY (matching SaveGame/LoadGame):
    //    they act on the whole stack, not one action, so there's nothing for a data pin to name.
    t.push_back({"SaveInputBindings", "Save Input Bindings", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"LoadInputBindings", "Load Input Bindings", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"ResetInputBindings", "Reset Input Bindings", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // RebindAction's action= is REQUIRED, not optional like InputAction's: EmitRebindAction
    // (GraphCompiler.cs) refuses to compile one missing it. Declared here like any attribute
    // regardless -- this table only says WHICH rows the details panel shows, not which are required;
    // that distinction lives in the compiler's error, not a second flag here.
    t.push_back({"RebindAction", "Rebind Action", "Input", {
        pin("exec", "exec", false), pin("slot", "int", false), pin("key", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)}, {attr("action", "Action")}});
    // GetActionKey: pure data, like InputAction -- reading a binding has no side effect and costs
    // nothing to redo per pull. `key` is -1 when the slot names no binding (out of range, or unbound).
    t.push_back({"GetActionKey", "Get Action Key", "Input", {
        pin("slot", "int", false), pin("key", "int", true), pin("bound", "bool", true)}, {attr("action", "Action")}});
    // GetPressedKey: which key was pressed THIS FRAME, for a "press any key to rebind" capture step --
    // the one thing no InputKey* node can answer, since those take a key rather than finding one. -1 when none.
    t.push_back({"GetPressedKey", "Get Pressed Key", "Input", {
        pin("key", "int", true), pin("pressed", "bool", true)}});
    // -- Select: pick one of two values by a bool. Pure data, no exec pins. In the PULL compiler BOTH
    //    arms are computed regardless of cond (GraphCompiler.EmitSelect) -- not a missing short-circuit.
    t.push_back({"Select", "Select", "Logic", {
        pin("cond", "bool", false), pin("ifTrue", "float", false), pin("ifFalse", "float", false),
        pin("result", "float", true)}});
    // -- Raycast: the physics query, and the node whose one-call-per-exec-visit shape MouseDelta and
    //    MoveAxis were later modelled on. 14 pins: exec in, seven floats of ray, exec out, and five
    //    results. --
    t.push_back({"Raycast", "Raycast", "Scene", {
        pin("exec", "exec", false),
        pin("originX", "float", false), pin("originY", "float", false), pin("originZ", "float", false),
        pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false),
        pin("maxDist", "float", false),
        pin("then", "exec", true),
        pin("hit", "bool", true), pin("entity", "int", true),
        pin("pointX", "float", true), pin("pointY", "float", true), pin("pointZ", "float", true)}});
    // WHY THESE THREE ARRIVE LATE: Select, InputKey and Raycast were added to OcGraphParser and both
    // compilers (1425b67) but never to this table -- runtime-supported yet absent from the palette
    // for two slices (authorable only by hand-editing .ocgraph text), since this is a DELIBERATE
    // separate copy with no build-time link to catch the omission (see header comment). Adding
    // MouseDelta/MoveAxis made the gap visible: Input would show the mouse but not the keyboard.

    // -- graph parameter read; type defaults to float (the common case) and is editable per-instance
    //    since pin-typing always prefers a node's own recorded pins over this table. param= names
    //    which declared PARAM this node reads.
    t.push_back({"Param", "Param", "Param", {pin("value", "float", true)}, {attr("param", "Param Name")}});
    // -- SELF: without which a canvas-authored graph couldn't drive anything. Nearly every Scene/
    //    Character/Physics/Animation/Audio node takes an `entity` pin, but reaching it needed
    //    `PARAM entity int` + a Param node -- a top-level record the editor can't write (OcGraphData
    //    has no parameter model), so Param alone was unusable from the canvas -- why every gameplay
    //    graph in this repo is hand-written text, alongside the Const rows' missing `value` attribute.
    //
    //    NO ATTRIBUTES: Graph.ResolveSelfNodes rewrites it into `Param entity` at parse time
    //    (declaring the PARAM if missing), so no Self node survives to the compiler, GraphHost or
    //    C++ writer -- hence no counterpart in GraphCompiler.cs.
    //
    //    OUTPUT PIN IS `value`, NOT `entity` -- not cosmetic: EmitParam stores into the local for
    //    pin "value" specifically (a pin named "entity" would leave the value on the stack, storing
    //    nothing), keeping the rewrite a pure type change with no pin/LINK rewriting.
    t.push_back({"Self", "Self", "Param", {pin("value", "int", true)}});

    // -- flow / exec: control flow, not data flow. "exec" is a PIN TYPE like "float"/"int"/"bool"
    //    (OcGraph.hpp's OcGraphLink comment) -- the whole format change this needed. Pin sets below
    //    match OcGraphParser.cs's AddDefaultPins EXACTLY (parity is load-bearing, see header comment).
    //
    // branch: a bool condition and one incoming exec pulse; exactly one of "true"/"false" fires.
    //    "tookTrue" is OPT-IN OBSERVABILITY (GraphCompiler.EmitBranch), not required wiring.
    t.push_back({"Branch", "Branch", "Flow", {
        pin("exec", "exec", false), pin("cond", "bool", false),
        pin("true", "exec", true), pin("false", "exec", true), pin("tookTrue", "bool", true)}});
    // sequence: fires each exec output in file order -- two by default ("then0","then1"); widen via
    //    PIN records. "fireLog" is opt-in observability, sequence's version of branch's "tookTrue".
    t.push_back({"Sequence", "Sequence", "Flow", {
        pin("exec", "exec", false), pin("then0", "exec", true), pin("then1", "exec", true),
        pin("fireLog", "int", true)}});
    // while: "cond" is re-checked every pass, never cached (GraphCompiler's PUSH VS PULL comment);
    //    "loop" is the body, "done" fires once after, "iterations" counts passes (also proves a
    //    runaway-loop guard actually bit).
    t.push_back({"While", "While", "Flow", {
        pin("exec", "exec", false), pin("cond", "bool", false),
        pin("loop", "exec", true), pin("done", "exec", true), pin("iterations", "int", true)}});
    // forEach: COUNTED-REPEAT, not per-element -- no array/collection pin type exists yet, so a real
    //    "for each item in a list" can't be expressed (GraphCompiler.EmitForEach, "left rough for
    //    phase 2"). "count" is total passes; "index" is 0..count-1.
    t.push_back({"ForEach", "For Each (counted)", "Flow", {
        pin("exec", "exec", false), pin("count", "int", false),
        pin("loop", "exec", true), pin("index", "int", true), pin("done", "exec", true)}});
    // onstart / ontick: event entry points -- what runs one is a top-level ENTRY <nodeId>
    //    <eventName> record (OcGraphData::entryPoints), not the node's TYPE; these are just
    //    convenience triggers (single exec output, no inputs) to mark with ENTRY. Per-tick data
    //    (delta time) is an ordinary PARAM (e.g. `PARAM deltaTime float`) read via a `param` node,
    //    the same plumbing every dataflow graph uses for `time`/`entity`. "Event", NOT "Flow": these
    //    are where execution ENTERS the graph (an ENTRY record drives them), unlike Branch/Sequence
    //    which reorder execution already running -- grouping with flow control would read as though
    //    OnTick were a kind of Branch, sharing the most-common-node colour with the one most wanted.
    t.push_back({"OnStart", "On Start", "Event", {pin("exec", "exec", true)}});
    t.push_back({"OnTick", "On Tick", "Event", {pin("exec", "exec", true)}});
    // onhit: same bare-trigger shape as onstart/ontick -- an ENTRY record's labelled starting point,
    //    nothing more. Firing ON DEMAND (GraphHost.Fire, vs OnStart/OnTick's fixed Tick() cadence) is
    //    a GraphHost.cs concept; this editor treats "OnHit" as just another ENTRY event name, so a
    //    project inventing a new event needs no new catalog entry -- any bare-trigger type plus its
    //    own ENTRY record naming the event suffices.
    t.push_back({"OnHit", "On Hit", "Event", {pin("exec", "exec", true)}});
    // -- CustomEvent: an entry point whose event NAME is the author's, not one the palette shipped.
    //    Same treatment as onstart/ontick/onhit: an ENTRY record fires it, never the node TYPE. The
    //    `name=` attribute keeps the NODE line in step with that record for the canvas to show/edit;
    //    nothing at runtime reads it.
    t.push_back({"CustomEvent", "Custom Event", "Event", {pin("exec", "exec", true)}});

    // Spawn: SIDE-EFFECTING (creates a scene entity), so -- unlike GetField/SetField/GetFieldVec3/
    //    SetFieldVec3 -- it gets exec pins by default, mirroring Raycast (see GraphCompiler.cs's
    //    IsExecCapableSpawnType: refused by the pure-dataflow/PULL compiler even more strictly than
    //    SetField). class= names the registered class to spawn, the same field=/param= mechanism.
    t.push_back({"Spawn", "Spawn", "Actor", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("entity", "int", true)},
        {attr("class", "Class")}});

    // CharacterMove: the last Blueprint-parity node -- ONE coarse, exec-only wrapper around
    //    AverCharacter.Drive (DriveFromGraph -> GraphInterop.CharacterMoveForGraph): CharacterMove
    //    (entity, dt, forward, right, yawDelta, pitchDelta) -> then, success. Refused by the PULL
    //    compiler (GraphCompiler.cs's IsExecCapableCharacterMoveType) as strictly as Spawn. UNLIKE
    //    Spawn's class=, NO NODE-line attribute: every input is computed at RUNTIME, not chosen at
    //    edit time. "success" is false (logged, no throw) when the entity isn't a live AverCharacter.
    t.push_back({"CharacterMove", "Character Move", "Actor", {
        pin("exec", "exec", false),
        pin("entity", "int", false), pin("dt", "float", false),
        pin("forward", "float", false), pin("right", "float", false),
        pin("yawDelta", "float", false), pin("pitchDelta", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // GetSynapseTarget: a PURE node reading CSynapseAgent's current steering target (native
    //    AgentSystem tick, aver_fw_synapse_target). NO exec pins, like GetWorldPosition (refused by
    //    side-effect rules for the same reason -- see IsExecCapableCharacterMoveType on what "PURE"
    //    means here). "success" reuses EmitPullVec3Read's x/y/z-plus-bool shape; false is a real
    //    "nothing to head toward" outcome (no CSynapseAgent, or not Pathing), not an error
    //    (GraphInterop.SynapseGetTargetForGraph).
    t.push_back({"GetSynapseTarget", "Get Synapse Target", "Actor", {
        pin("entity", "int", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true),
        pin("success", "bool", true)}});

    // SynapseSteer: a PURE node turning "where am I, where do I want to go" into the
    //    forward/right/yawDelta CharacterMove consumes (GraphInterop.SynapseSteerForGraph). Takes an
    //    EXPLICIT target (x/y/z), never CSynapseAgent's own, so one node does both direct chase and
    //    path-following (feeding it GetSynapseTarget's output) with neither this node nor the
    //    compiler needing to know which. "success" (entity alive) is SEPARATE from "arrived": a
    //    graph that never checks success still gets usable-looking zeros for a dead entity, but a
    //    caller can tell "nothing happened" from "arrived and correctly holding still".
    t.push_back({"SynapseSteer", "Synapse Steer", "Actor", {
        pin("entity", "int", false), pin("dt", "float", false),
        pin("targetX", "float", false), pin("targetY", "float", false), pin("targetZ", "float", false),
        pin("turnRate", "float", false), pin("arriveRadius", "float", false),
        pin("forward", "float", true), pin("right", "float", true), pin("yawDelta", "float", true),
        pin("arrived", "bool", true), pin("success", "bool", true)}});

    // GetSynapsePerception: a PURE node reading CSynapsePerception's sight state (native
    //    PerceptionSystem tick, aver_fw_synapse_perception) -- the companion query "OnSeeTarget"
    //    needs, since the graph-event seam (ScriptHost::graphFire) carries no payload naming WHICH
    //    entity was seen. "success" means something DIFFERENT here: NOT "could I see the target"
    //    (canSeeTarget answers that; false is common and meaningful) but "does this entity carry
    //    CSynapsePerception at all" (GraphInterop.SynapseGetPerceptionForGraph).
    t.push_back({"GetSynapsePerception", "Get Synapse Perception", "Actor", {
        pin("entity", "int", false),
        pin("canSeeTarget", "bool", true), pin("lastKnownTarget", "int", true),
        pin("timeSinceSeen", "float", true), pin("success", "bool", true)}});

    // -- AUDIO: the mixer, WASAPI device and aver_audio_* C ABI were built and tested but never
    //    called by anything for weeks (docs' "declared but unread" shape). These six connect it,
    //    alongside Aver.Framework's Audio class. sound= is a PATH attribute, not a pin (no String
    //    PinType); cached natively (same path -> same handle), so playing every tick costs a lookup.
    //    PlaySound/PlaySoundAt are EXEC (a side effect -- a dataflow pull would fire one per
    //    invocation with nothing able to gate it, like Spawn/CreateEntity) and return a VOICE
    //    int to stop/steer it. "voice" is 0 with no audio device -- SUPPORTED, not an error.
    t.push_back({"PlaySound", "Play Sound", "Audio", {
        pin("exec", "exec", false),
        pin("volume", "float", false), pin("pitch", "float", false),
        pin("looping", "bool", false), pin("bus", "int", false),
        pin("then", "exec", true), pin("voice", "int", true), pin("success", "bool", true)},
        {attr("sound", "Sound")}});

    t.push_back({"PlaySoundAt", "Play Sound At", "Audio", {
        pin("exec", "exec", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("volume", "float", false), pin("pitch", "float", false),
        pin("looping", "bool", false), pin("bus", "int", false),
        pin("innerCm", "float", false), pin("outerCm", "float", false),
        pin("then", "exec", true), pin("voice", "int", true), pin("success", "bool", true)},
        {attr("sound", "Sound")}});

    //    StopSound / SetListener / SetBusVolume are exec too -- all three change something.
    //    IsSoundPlaying is a PURE read, so it is welcome in either compiler.
    t.push_back({"StopSound", "Stop Sound", "Audio", {
        pin("exec", "exec", false), pin("voice", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    t.push_back({"IsSoundPlaying", "Is Sound Playing", "Audio", {
        pin("voice", "int", false), pin("playing", "bool", true)}});

    //    SetListener takes the ENTITY whose transform the ears follow -- usually the camera or the
    //    player. Without it every positioned sound is panned against the world origin.
    t.push_back({"SetListener", "Set Listener", "Audio", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    t.push_back({"SetBusVolume", "Set Bus Volume", "Audio", {
        pin("exec", "exec", false), pin("bus", "int", false), pin("volume", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // FireEvent: GAP 3, cross-entity events -- fires a DECLARED event (event=, e.g. "OnHit") on
    //    ANOTHER entity's graph. SIDE-EFFECTING, exec pins by default (mirroring Spawn/CharacterMove),
    //    refused entirely by the PULL compiler. "target" is a runtime int PIN, NOT an attribute like
    //    event= (same split as Spawn's x/y/z-vs-class=). "fired" is false (logged, never silently
    //    true) when the target has no live graph declaring this event (GraphEvents.cs has the
    //    failure-mode table and the reentrancy guard against a self-fire/fire cycle stack-overflowing
    //    the process).
    t.push_back({"FireEvent", "Fire Event", "Actor", {
        pin("exec", "exec", false), pin("target", "int", false),
        pin("then", "exec", true), pin("fired", "bool", true)},
        {attr("event", "Event")}});

    // GetVar / SetVar: graph-local PERSISTENT variables -- closes the "nothing survives between
    // ticks" gap via storage the GraphHost owns per instance (GraphVarStore.cs). var= names the
    // declared VAR, the same field=/param=/class= mechanism. GetVar: a PURE READ (no exec pins),
    // pin type defaulting to float, adjustable per-instance.
    t.push_back({"GetVar", "Get Var", "Var", {pin("value", "float", true)}, {attr("var", "Var Name")}});
    // SetVar: A WRITE IS A SIDE EFFECT (GraphCompiler.IsExecCapableVarSideEffectType), so -- unlike
    // GetField/SetField/GetFieldVec3/SetFieldVec3 -- it gets exec pins by default (mirroring
    // Spawn/Raycast): SetVar has no legitimate non-exec path, so a freshly spawned node needs to
    // already be usable. No "success" pin: an in-process write has no runtime failure mode (unknown
    // entity, read-only field, missing component) to report.
    t.push_back({"SetVar", "Set Var", "Var", {
        pin("exec", "exec", false), pin("value", "float", false), pin("then", "exec", true)},
        {attr("var", "Var Name")}});

    // -- SetParent / SetViewEntity / SetName: three one-ABI-call writes, dispatched SetField-style --
    //    no exec pins by default (unlike Spawn/SetVar), reachable from both C# compilers, still
    //    refused if pulled with no exec visit (OcGraphParser.cs's "SetParent / SetViewEntity /
    //    SetName" comment has the full reasoning). SetParent: aver_scene_set_parent(child, parent)
    //    -> success (scene_abi.h:105); refuses a cycle, self-parent, or doomed parent (returns 0).
    t.push_back({"SetParent", "Set Parent", "Scene", {
        pin("child", "int", false), pin("parent", "int", false), pin("success", "bool", true)}});
    //    SetViewEntity: aver_fw_set_view_entity(entity) -> void (framework_abi.h:206). NO OUTPUT PIN --
    //    the ABI returns nothing, so there is no return code to invent one for.
    t.push_back({"SetViewEntity", "Set View Entity", "Actor", {
        pin("entity", "int", false)}});
    //    SetName: aver_scene_set_name(entity, name) -> success (scene_abi.h:113). name= is a
    //    NODE-line attribute, not a pin, since PinType has no String member (OcGraphParser.cs).
    t.push_back({"SetName", "Set Name", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("name", "Name")}});

    //    CreateEntity / FindEntity: SetName's name= family siblings (Entity.Create(name),
    //    Game.Find(name)), reusing name=/Node.NameValue verbatim. CreateEntity is EXEC (a side
    //    effect, refused like Spawn); FindEntity is PURE (idempotent), returning 0 for no match (a
    //    real answer, not an error) -- "found" disambiguates, as GetSynapsePerception's success does.
    t.push_back({"CreateEntity", "Create Entity", "Scene", {
        pin("exec", "exec", false),
        pin("then", "exec", true), pin("entity", "int", true), pin("success", "bool", true)},
        {attr("name", "Name")}});
    t.push_back({"FindEntity", "Find Entity", "Scene", {
        pin("entity", "int", true), pin("found", "bool", true)},
        {attr("name", "Name")}});

    // -- SetMesh / SetMaterial: coarse, dedicated nodes wrapping Entity.SetMesh/SetMaterial
    //    (EntityScene.cs) via GraphInterop.SetMeshForGraph/SetMaterialForGraph -- NOT a generalised
    //    SetField and NOT a generic "add missing component" node (OcGraphParser.cs). Same
    //    SetField-style dispatch as above; EnsureMeshRenderer's idempotent add-if-absent guard makes
    //    re-running this every tick harmless.
    t.push_back({"SetMesh", "Set Mesh", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("mesh", "Mesh")}});
    t.push_back({"SetMaterial", "Set Material", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("material", "Material")}});

    // -- AttachToSocket: hangs `entity` on a named socket of `parent`'s rig, riding the posed bone
    //    every frame. TWO entity pins, unlike every Set* node, since an attachment is a relationship
    //    between two things. socket= names it, the same mesh=/material=/name= NODE-line mechanism
    //    (no string PinType exists).
    t.push_back({"AttachToSocket", "Attach To Socket", "Scene", {
        pin("entity", "int", false), pin("parent", "int", false), pin("success", "bool", true)},
        {attr("socket", "Socket")}});

    // -- GetAnimCurve: a PURE read (no exec pins, no exec emitter) -- the only reader in this
    //    animation group. curve= names it, the usual NODE-line attribute mechanism.
    t.push_back({"GetAnimCurve", "Get Anim Curve", "Scene", {
        pin("entity", "int", false), pin("value", "float", true)},
        {attr("curve", "Curve")}});

    // -- SetSkeleton / PlayAnimation: SetMesh/SetMaterial's animation-family siblings, wrapping
    //    Entity.SetSkeleton/PlayAnimation (Animation.cs) via GraphInterop.SetSkeletonForGraph/
    //    PlayAnimationForGraph -- same SetField-style dispatch and idempotent add-if-absent guard
    //    (each adds its component, CSkeletalMesh/CAnimator, like EnsureMeshRenderer).
    //
    //    BINDING IS BY ARRAY INDEX, NOT NAME (AnimSampler.cpp's `t.boneIndex` bounds-check only) --
    //    this node can't enforce that a skeleton= and a clip= on the same entity agree on joint
    //    order; a mismatch drives the wrong bone or silently drops the track, with no error raised.
    //    A content problem outside this graph layer's visibility, not a gap in the node.
    t.push_back({"SetSkeleton", "Set Skeleton", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("skeleton", "Skeleton")}});
    //    PlayAnimation gets a THIRD input pin -- loop -- since Entity.PlayAnimation takes a second
    //    scalar argument (Animation.cs's `bool loop = true`), unlike SetMesh/SetMaterial/SetSkeleton's
    //    single string write. A PIN, not an attribute (opposite of clip=): loop is runtime data a
    //    graph may compute (e.g. "loop unless this is the death clip"), like PlaySound's "looping"
    //    pin. Default "true" mirrors Animation.cs's default, so a fresh node behaves like calling
    //    PlayAnimation(clip) untouched.
    t.push_back({"PlayAnimation", "Play Animation", "Scene", {
        pin("entity", "int", false), pin("loop", "bool", false, "true"), pin("success", "bool", true)},
        {attr("clip", "Clip")}});
    // -- SetControlRig: the third animation-family node, making a control rig reachable from a
    //    LEVEL. Everything under it (twoBoneIk/aimAt, .ocrig, CControlRig via AnimSystem's
    //    pose-modifier seam) was built and tested but only attachable from C++, since a skinned
    //    character reaches a level via a graph CLASS (no skeleton field on .ocworld PLACE).
    //
    //    NOT A BUILT-IN (unlike SetSkeleton): CControlRig is registered at runtime (docs/SYNAPSE.md
    //    section 6), attached by NAME through aver_scene_component (scene ABI 1.4); a host that
    //    never registered it gets false, no change.
    //
    //    weight is a PIN, rig= an attribute (same split as PlayAnimation's loop): which rig is
    //    edit-time, how strongly worn is runtime data. Default "1" matches SetControlRig's own default.
    t.push_back({"SetControlRig", "Set Control Rig", "Scene", {
        pin("entity", "int", false), pin("weight", "float", false, "1"), pin("success", "bool", true)},
        {attr("rig", "Rig")}});

    // ============================================================================================
    // MATERIAL NODES -- DOMAIN material, compiled to HLSL by aver::pbr::compileMaterialGraph()
    // (MaterialGraphHlsl.cpp). READ emitNode() FIRST: it is the authority on every node type/pin
    // name/promotion rule below; a mismatch here produces the exact failure this table exists to
    // prevent -- a node that spawns and then refuses to compile.
    //
    // NO EXEC PIN, EVER: a gameplay node describes a STEP; a material node describes a VALUE. No
    // exec pins, ENTRY point or OUT record (PULL, NOT PUSH -- MaterialGraphHlsl.cpp point 1):
    // averEvalMaterial runs once per pixel with no room for "and then do this". compileMaterialGraph
    // walks BACKWARDS from MaterialOutput, emitting only what's actually read -- emit order is
    // whatever the walk decides, never canvas position or wiring order.
    //
    // WIDTH IS NOMINAL, NOT ENFORCED: generic-width nodes (Add, Sin, Saturate, Clamp, ...) declare
    // float3 below (what an author reaches for first), but emitNode's widestInput() re-derives the
    // REAL width from what's linked at compile time (an input that is only a literal doesn't count
    // towards it) -- wiring a float2 UV computes at float2 regardless. What this table must get
    // right: pin NAMES and the DEFAULT LITERAL on unwired pins.
    //
    // A KNOWN, ACCEPTED NAME COLLISION: Add, Subtract, Multiply, Divide, Min, Max, Lerp, Clamp,
    // Saturate, Abs, Floor, Ceil, Sqrt, Sin, Cos and ConstFloat are ALSO gameplay type names -- both
    // compilers key off the literal TYPE string, so these spellings are shared, not accidental.
    // findGraphNodeDesc (no domain param) returns the FIRST match, always gameplay's, so today the
    // Add-Node popup's Const/Math/Vector/Input categories show both shapes side by side, and
    // addNodeFromCatalog (GraphEditor.cpp) resolves either menu item to the SAME gameplay shape
    // until that lookup learns the open graph's domain (a GraphEditor.cpp fix). ConstFloat here is
    // byte-for-byte gameplay's ConstFloat (a bare literal means the same to both) -- a separate entry
    // rather than kDomainBoth, for the same "don't touch the ~200 existing rows" reason domain
    // defaults to gameplay; GraphNodeDomain exists for a future cleanup to use, not to force one now.
    // ============================================================================================

    // -- CONST: one literal entry per width, matching MaterialGraphHlsl.cpp's ConstFloat/2/3/4 case:
    //    a single `value` output whose default IS the constant (same idiom as gameplay's ConstFloat)
    //    -- on the pin, not a NODE-line attribute, since a PIN record already round-trips defaults.
    t.push_back({"ConstFloat",  "Const Float",  "Const", {pin("value", "float",  true, "0")},
        {}, kDomainMaterial});
    t.push_back({"ConstFloat2", "Const Float2", "Const", {pin("value", "float2", true, "0,0")},
        {}, kDomainMaterial});
    t.push_back({"ConstFloat3", "Const Float3", "Const", {pin("value", "float3", true, "0,0,0")},
        {}, kDomainMaterial});
    t.push_back({"ConstFloat4", "Const Float4", "Const", {pin("value", "float4", true, "0,0,0,0")},
        {}, kDomainMaterial});

    // -- INPUT: what the renderer already knows about this pixel/object, read-only, no wiring needed
    //    (emitNode's "what the renderer knows" section). UV is averSurfaceUV; the rest are xyz reads
    //    off the vertex/camera/instance transform (see MaterialGraphHlsl.cpp for exact bindings).
    t.push_back({"UV",             "UV",             "Input", {pin("uv",  "float2", true)}, {}, kDomainMaterial});
    t.push_back({"WorldPosition",  "World Position",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"WorldNormal",    "World Normal",    "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"ViewDirection",  "View Direction",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"CameraPosition", "Camera Position",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"ObjectPosition", "Object Position",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});

    // -- MATH: generic-width arithmetic, promoted at COMPILE TIME to the widest linked input (see
    //    "float3 is nominal" above). Pin names a/b/result match emitNode's binary()/call() helpers.
    t.push_back({"Add",      "Add",      "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Subtract", "Subtract", "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Multiply", "Multiply", "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Divide",   "Divide",   "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Min",      "Min",      "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Max",      "Max",      "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    // Power/Modulo -- named for what they DO, not gameplay's Pow/Mod: emitNode's switch checks
    // `ciEquals(ty, "Power")`/`"Modulo"` verbatim, so these spellings are load-bearing, not stylistic.
    t.push_back({"Power",    "Power",    "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Modulo",   "Modulo",   "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});

    t.push_back({"Lerp", "Lerp", "Math", {
        pin("a", "float3", false), pin("b", "float3", false), pin("t", "float", false, "0.5"),
        pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Clamp", "Clamp", "Math", {
        pin("x", "float3", false), pin("lo", "float", false, "0"), pin("hi", "float", false, "1"),
        pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Smoothstep", "Smoothstep", "Math", {
        pin("edge0", "float", false, "0"), pin("edge1", "float", false, "1"), pin("x", "float", false),
        pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Step", "Step", "Math", {
        pin("edge", "float", false, "0.5"), pin("x", "float", false), pin("result", "float", true)},
        {}, kDomainMaterial});
    // Remap: emitted as arithmetic rather than a call, because HLSL has no intrinsic for it -- see
    // emitNode's own Remap comment. Five inputs, all generic-width together.
    t.push_back({"Remap", "Remap", "Math", {
        pin("x", "float", false), pin("inMin", "float", false, "0"), pin("inMax", "float", false, "1"),
        pin("outMin", "float", false, "0"), pin("outMax", "float", false, "1"), pin("result", "float", true)},
        {}, kDomainMaterial});

    // The single-input standard library: one `x` in, one `result` out, generic-width, fourteen types
    // sharing one shape -- emitNode dispatches all through call(), differing only in the HLSL intrinsic.
    t.push_back({"Saturate",  "Saturate",  "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Abs",       "Abs",       "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Frac",      "Frac",      "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Floor",     "Floor",     "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Ceil",      "Ceil",      "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Sign",      "Sign",      "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Sqrt",      "Sqrt",      "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Exp",       "Exp",       "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Log",       "Log",       "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Sin",       "Sin",       "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Cos",       "Cos",       "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Tan",       "Tan",       "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Normalize", "Normalize", "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"OneMinus",  "One Minus", "Math", {pin("x", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});

    // -- VECTOR: geometry ops gameplay's own Vec* nodes don't cover in this shape -- real
    //    float2/float3/float4 pins, since a material pin genuinely IS that wide (OcGraphPin::type),
    //    unlike gameplay PinType, which has no vector type and spells a direction as loose floats.
    t.push_back({"Dot",      "Dot",      "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Length",   "Length",   "Vector", {pin("x", "float3", false), pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Distance", "Distance", "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Cross",    "Cross",    "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Reflect",  "Reflect",  "Vector", {pin("i", "float3", false), pin("n", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"BlendNormals", "Blend Normals", "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});

    // Assembling and taking apart: MakeFloatN builds a wider value from scalars, Split is the
    // inverse. Split's INPUT and first OUTPUT pin are both named "x" -- not a typo (emitNode's Split
    // reads input "x", answers "x"/"y"/"z"/"w"); isOutput tells the two apart, as always.
    t.push_back({"MakeFloat2", "Make Float2", "Vector", {
        pin("x", "float", false), pin("y", "float", false), pin("result", "float2", true)}, {}, kDomainMaterial});
    t.push_back({"MakeFloat3", "Make Float3", "Vector", {
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"MakeFloat4", "Make Float4", "Vector", {
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("w", "float", false),
        pin("result", "float4", true)}, {}, kDomainMaterial});
    t.push_back({"Split", "Split", "Vector", {
        pin("x", "float3", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("w", "float", true)},
        {}, kDomainMaterial});
    // Swizzle: the one node whose OUTPUT WIDTH is an ATTRIBUTE (mask=), not a pin type (emitNode
    // validates the mask). Declared `result` type is nominal float; REAL width is mask='s length (1-4).
    t.push_back({"Swizzle", "Swizzle", "Vector", {pin("x", "float3", false), pin("result", "float", true)},
        {attr("mask", "Mask")}, kDomainMaterial});

    // -- UV: coordinate transforms, reading THE SURFACE'S OWN UV when `uv` is left unwired --
    //    emitNode's uvInput(), the one place an unlinked pin isn't simply its literal default. The
    //    `uv` pin still needs to exist, named exactly "uv", for a LINK to land on; its default is
    //    never read.
    t.push_back({"TilingOffset", "Tiling / Offset", "UV", {
        pin("uv", "float2", false), pin("tiling", "float2", false, "1,1"), pin("offset", "float2", false, "0,0"),
        pin("result", "float2", true)}, {}, kDomainMaterial});
    t.push_back({"Rotator", "Rotator", "UV", {
        pin("uv", "float2", false), pin("centre", "float2", false, "0.5,0.5"), pin("angle", "float", false, "0"),
        pin("result", "float2", true)}, {}, kDomainMaterial});

    // -- PROCEDURAL: the same unwired-uv-means-the-surface's-own convention as TilingOffset/Rotator
    //    above.
    t.push_back({"Noise", "Noise", "Procedural", {
        pin("uv", "float2", false), pin("scale", "float", false, "8"), pin("result", "float", true)},
        {}, kDomainMaterial});
    t.push_back({"Checker", "Checker", "Procedural", {
        pin("uv", "float2", false), pin("scale", "float", false, "8"), pin("result", "float", true)},
        {}, kDomainMaterial});

    // -- TEXTURE: the one sampling node -- ONE call, THREE output pins (rgb/a/rgba) off the same
    //    sample, so reading only `.a` costs one sample+swizzle, not three. slot= names one of the
    //    eight texture slots emitNode's kSlotNames accepts (basecolor, metalrough, normal, occlusion,
    //    emissive, layer1basecolor, layer1metalrough, layer1normal). Not validated here, same as
    //    field=/class= -- a bad slot= is a clear compile-time error from compileMaterialGraph.
    t.push_back({"SampleTexture", "Sample Texture", "Texture", {
        pin("uv", "float2", false),
        pin("rgb", "float3", true), pin("a", "float", true), pin("rgba", "float4", true)},
        {attr("slot", "Slot")}, kDomainMaterial});

    // -- UTILITY --
    t.push_back({"Fresnel", "Fresnel", "Utility", {
        pin("power", "float", false, "5"), pin("result", "float", true)}, {}, kDomainMaterial});
    // If: a branchless select (lerp+step under the hood, not HLSL's `?:` -- see emitNode). `a`/`b`
    // are the scalars compared; `ifTrue`/`ifFalse` are the generic-width arms actually returned.
    t.push_back({"If", "If", "Utility", {
        pin("a", "float", false), pin("b", "float", false),
        pin("ifTrue", "float3", false), pin("ifFalse", "float3", false),
        pin("result", "float3", true)}, {}, kDomainMaterial});

    // -- OUTPUT: the one sink a material graph has. NO OUTPUT PINS AT ALL -- nothing ever reads a
    //    MaterialOutput, since it's where the backward walk starts. NO DEFAULT on any of its
    //    seventeen inputs, load-bearing not an oversight: compileMaterialGraph treats an input as
    //    DRIVEN when linked OR carrying a NON-EMPTY literal, so a default would make a fresh node
    //    drive all seventeen fields immediately, destroying partial-graph behaviour (a graph saying
    //    only "base colour is red" leaving roughness/normal/alpha at the stock material's values --
    //    see compileMaterialGraph's "ONLY THE FIELDS THE AUTHOR ACTUALLY DROVE"). Do not add a "0"
    //    defaultValue here without reading that comment first.
    t.push_back({"MaterialOutput", "Material Output", "Output", {
        pin("BaseColor", "float3", false), pin("Metallic", "float", false), pin("Roughness", "float", false),
        pin("Normal", "float3", false), pin("Emissive", "float3", false), pin("Occlusion", "float", false),
        pin("Opacity", "float", false), pin("AlphaCutoff", "float", false),
        // Subsurface: worth a pin, not just a material constant, since a MASK -- thin parts scatter
        // more than thick ones -- is what makes it read correctly (a texture-driven SubsurfaceRadius
        // vs. a uniformly waxy object). Same no-default rule as every pin above.
        pin("SubsurfaceWeight", "float", false), pin("SubsurfaceRadius", "float", false),
        // The dielectric pair: a mask-driven Transmission makes one mesh a clear window with a
        // frosted band instead of two materials. Ior is per-pixel for the same reason, moving less often.
        pin("Ior", "float", false), pin("Transmission", "float", false),
        // The volume: AttenuationColor is the transmittance after AttenuationDistance centimetres
        // (kOutputFields, MaterialGraphHlsl.cpp) -- per-pixel driving gives a thin clear pane and a
        // deep green edge instead of one tint. Same no-default rule; 0-or-less already means "no volume".
        pin("AttenuationColor", "float3", false), pin("AttenuationDistance", "float", false),
        // The coat: a weight mask makes one material polished where handled, bare where worn; a
        // roughness mask puts a clear panel and a scuffed edge on the same car paint. Present
        // whether or not the layered BSDF is compiled in -- AverAuthored carries the fields
        // unconditionally so a graph doesn't stop compiling when the setting changes.
        pin("CoatWeight", "float", false), pin("CoatRoughness", "float", false),
        pin("CoatF0", "float", false)},
        {}, kDomainMaterial});

    return t;
}

inline bool ciEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

} // namespace detail

// THE TABLE: inline + function-local static so this header can be included from multiple translation
// units (palette, node-spawning code, headless test) without an ODR violation and no .cpp of its own.
inline const std::vector<GraphNodeDesc>& graphNodeCatalog() {
    static const std::vector<GraphNodeDesc> table = detail::buildCatalog();
    return table;
}

// Case-insensitive lookup: the file format and GraphCompiler.cs both accept mixed case ("ConstFloat"
// in the fixture, "constfloat" in the C# switch). Returns nullptr for an unknown type -- the node
// still draws from its own recorded pins (GraphEditorGeometry.hpp), just can't be spawned fresh.
inline const GraphNodeDesc* findGraphNodeDesc(const std::string& typeId) {
    for (const GraphNodeDesc& d : graphNodeCatalog()) {
        if (detail::ciEquals(d.typeId, typeId)) return &d;
    }
    return nullptr;
}

// The same lookup, but preferring an entry that serves `domain`.
//
// SIXTEEN NAMES ARE IN BOTH VOCABULARIES (Add, Multiply, Lerp, Saturate, Sin, ...) and are NOT the
// same node: gameplay's Add takes two scalars (no vector PinType), the material one two float3s.
// Resolving by name alone would give a material graph the scalar shape, with pins that don't fit
// anything around them -- this overload exists to avoid that.
//
// FALLS BACK TO THE PLAIN LOOKUP rather than null: a type only one domain declares is still the
// right answer for the other (an older graph naming a type since moved between domains, or a
// hand-edited file's gameplay-only node) -- refusing would turn a cosmetic mismatch into a node that
// can't be drawn at all.
inline const GraphNodeDesc* findGraphNodeDescIn(const std::string& typeId, GraphNodeDomain domain) {
    for (const GraphNodeDesc& d : graphNodeCatalog()) {
        if (detail::ciEquals(d.typeId, typeId) && (d.domain & domain) != 0u) return &d;
    }
    return findGraphNodeDesc(typeId);
}

// ---- searching the palette ---------------------------------------------------------------------
//
// WHY A SEARCH EXISTS: this catalog holds 240 node types across 23 categories, previously reachable
// only through a right-click submenu-per-category with no filter (finding `VecAdd` meant knowing
// it's filed under Vector, not Math). A palette usable only by whoever already knows the layout.
//
// HERE RATHER THAN IN THE POPUP, so it can be tested with no ImGui context -- the same reason
// GraphEditor.cpp keeps addNodeFromCatalog separate from the menu item that calls it.

namespace detail {

// Case-insensitive find. Returns npos when absent, like std::string::find.
inline usize ciFind(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return 0;
    if (needle.size() > hay.size()) return std::string_view::npos;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return i;
    }
    return std::string_view::npos;
}

} // namespace detail

// Palette rows in `domain` matching `query`, best first, at most `limit` of them.
//
// THE RANKING IS THREE TIERS, because a flat substring match puts `SetFieldVec3` above `Add` for "add":
//   0  the display name STARTS with the query        -- "add" -> Add, AddChild
//   1  the display name contains it                  -- "add" -> VecAdd
//   2  only the type id or the category contains it   -- "vector" -> every Vector row
// Ties keep catalog order, grouping a family rather than shuffling it.
//
// An empty query returns nothing rather than "everything": the caller shows category menus instead,
// and "everything" would just be the catalog with extra steps.
inline std::vector<const GraphNodeDesc*> graphPaletteSearch(std::string_view query,
                                                            GraphNodeDomain domain,
                                                            usize limit = 40) {
    std::vector<const GraphNodeDesc*> out;
    if (query.empty() || limit == 0) return out;

    std::vector<std::pair<int, const GraphNodeDesc*>> hits;
    for (const GraphNodeDesc& d : graphNodeCatalog()) {
        if ((d.domain & domain) == 0u) continue;
        // Function rows are excluded for the reason the category menu excludes them: the Functions
        // panel creates them knowing their owner; a bare one has no pins and no owner.
        if (d.category == "Function") continue;

        const usize inName = detail::ciFind(d.displayName, query);
        int rank = -1;
        if (inName == 0)                              rank = 0;
        else if (inName != std::string_view::npos)    rank = 1;
        else if (detail::ciFind(d.typeId, query) != std::string_view::npos ||
                 detail::ciFind(d.category, query) != std::string_view::npos) rank = 2;
        if (rank >= 0) hits.push_back({rank, &d});
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& h : hits) {
        if (out.size() >= limit) break;
        out.push_back(h.second);
    }
    return out;
}

// How many rows `graphPaletteSearch` would return with no limit -- so a capped list can report how
// many it's not showing, instead of silently ending as if it were complete.
inline usize graphPaletteSearchCount(std::string_view query, GraphNodeDomain domain) {
    return graphPaletteSearch(query, domain, static_cast<usize>(-1)).size();
}

} // namespace aver::editor
