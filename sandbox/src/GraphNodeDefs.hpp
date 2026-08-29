#pragma once
// THE ONE node-type descriptor table for the .ocgraph editor.
//
// Every place in the editor that needs to know what pins a node type has -- the Add-Node palette,
// spawning a fresh node onto the canvas, and pin-type display -- reads this table. Adding a node
// type is ONE call to node(...) in graphNodeCatalog() below, not a hunt through drawing code. This
// is the table the build task (and docs/VISUAL_SCRIPTING.md Slice 6) asks for.
//
// Deliberately pure data: no ImGui, no Engine, no scene:: include. It can be read from a headless
// test the same way it is read from the ImGui palette.
//
// WHAT THIS TABLE IS FOR, AND WHAT IT IS NOT FOR:
// A loaded/edited graph's node instances already carry their own pins in the file
// (aver::fmt::OcGraphNode::pins -- see modules/formats/include/aver/formats/OcGraph.hpp). Drawing
// and editing an EXISTING node reads pins from the node instance itself, not from this table, so a
// node saved by some future tool with a nonstandard pin set still renders correctly. This table is
// consulted only when the editor itself must invent a node's starting shape: populating the palette,
// and giving a freshly-spawned node its initial pins.
//
// PROVENANCE: the pin names/types below mirror the shape scripting/csharp/Aver.Graph/GraphCompiler.cs
// and OcGraphParser.cs (AddDefaultPins) expect, as read on 2026-08-13 (updated to add the exec/flow
// vocabulary: Branch, Sequence, While, ForEach, OnStart, OnTick -- see OcGraphParser.AddDefaultPins'
// own "flow / exec nodes" section, which this table's flow entries below were copied from field for
// field, pin for pin, in the same order). Those C# files are owned by a concurrent workflow and are
// NOT included or generated from here -- this table owns its own copy of the vocabulary so a C++
// editor build never depends on a C# file. If GraphCompiler.cs's node set has moved since, update this
// table to match; it is one line per node type by design.
//
// EXACT PARITY MATTERS MORE FOR THE FLOW TYPES THAN IT DID BEFORE. A node spawned from this catalog
// gets its pins written into the file as real PIN records when the editor saves (see the "add node"
// popup in GraphEditor.cpp, which copies a GraphNodeDesc's pins verbatim onto the new OcGraphNode).
// Once a node has ANY explicit pins, OcGraphParser.AddDefaultPins skips it entirely (its early-return
// on `node.Pins.Count > 0`) -- so if this table and AddDefaultPins ever disagree on a flow type's
// shape, an editor-authored graph gets one pin set and a hand-written or C#-authored graph of the same
// type gets another, which is exactly the "two implementations agree by coincidence, not by
// construction" trap the `outputs` field's own comment in OcGraph.hpp warns about.
//
// GetField/SetField (and their Vec3 siblings GetFieldVec3/SetFieldVec3 below) address a scene field by
// name via a `field=` NODE-line attribute on the C# side's own reader
// (scripting/csharp/Aver.Graph/OcGraphParser.cs, "field=" case). Spawn (below) addresses a registered
// class the same way, via `class=`; Param (below) names a declared PARAM via `param=`. Same reader,
// same generic key=value mechanism (modules/formats/src/OcGraph.cpp's NODE-line parsing reads only
// what parses as a numeric x/y coordinate as position; every OTHER trailing token is captured VERBATIM
// into OcGraphNode::extraTokens -- modules/formats/include/aver/formats/OcGraph.hpp -- and re-emitted
// on save), so `field=`/`param=`/`class=` all genuinely DO survive an editor load/save round trip; the
// grammar was never the gap. The FORMER gap, NOW CLOSED (see GraphNodeDesc::attributes below and
// sandbox/src/GraphEditor.cpp's details panel): this editor used to have no property/inspector panel
// for ANY node, so extraTokens round-tripped opaquely but was not READABLE or EDITABLE from the GUI. A
// node type's `attributes` list below is what the details panel reads to know which key=value tokens
// to show as labelled, always-present rows for that type; anything else the node's extraTokens still
// carries -- a key this table doesn't declare, on ANY node type -- is shown too, as a generic row, by
// GraphEditorGeometry.hpp's computeAttributeRows (see its own header comment for why that split is a
// hybrid, not a fully generic key=value editor: a node TYPE still gets to say what it expects, the way
// it already says what pins it has, but nothing the table doesn't know about is ever dropped).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aver::editor {

// One pin a freshly-spawned node of a given type starts with. Field names/order mirror
// aver::fmt::OcGraphPin exactly so a catalog entry converts into a real pin with no per-field mapping.
struct GraphPinSpec {
    std::string name;
    std::string type;         // "float" | "int" | "bool" | "string" -- matches OcGraphPin::type. A
                               // material node's pin also uses this same free string for "float2" /
                               // "float3" / "float4" -- see aver::pbr::MaterialGraphHlsl.cpp's
                               // typeFromPin(), which reads exactly those spellings, plus this
                               // header's own Material section below.
    bool isOutput = false;
    std::string defaultValue; // only meaningful for input pins; empty = none
};

// One NODE-line key=value attribute a node type declares -- param=/field=/class= today. Deliberately
// just (key, label): the details panel that reads this needs nothing more to draw a labelled,
// always-present InputText row (see GraphEditorGeometry.hpp's computeAttributeRows, which takes a
// plain vector<pair<string,string>> rather than this type directly, for the same "stay decoupled from
// the catalog" reason computeNodeLayout takes `title` as a parameter -- see that function's comment).
struct GraphAttributeSpec {
    std::string key;   // matches the extraTokens `key=` half exactly, e.g. "class"
    std::string label; // shown in the details panel, e.g. "Class"
};

// Which .ocgraph DOMAIN(s) a node type may appear in. Mirrors aver::fmt::OcGraphDomain
// (modules/formats/include/aver/formats/OcGraph.hpp): kDomainGameplay is a graph with no DOMAIN
// record, or `DOMAIN gameplay`, compiled to IL by scripting/csharp/Aver.Graph/GraphCompiler.cs;
// kDomainMaterial is `DOMAIN material`, compiled to HLSL by aver::pbr::compileMaterialGraph()
// (modules/render.pbr/src/MaterialGraphHlsl.cpp). No flag mirrors OcGraphDomain::Unknown -- that
// value means "a DOMAIN this build does not recognise", which is never something a catalog entry
// is FOR; it is the absence of an answer, not a third kind of node.
//
// A BITMASK, NOT A COPY OF THAT ENUM. OcGraphDomain is a plain enum because a LOADED GRAPH is
// answering a different question than a CATALOG ENTRY is: a .ocgraph file carries exactly one
// DOMAIN record, so a graph is unambiguously gameplay or material, never both, and an enum is the
// right shape for a value that is always exactly one thing. A palette entry answers "which
// domain(s) is this node TYPE STRING valid in", and that is not always a single answer: ConstFloat
// means the same thing -- a literal number -- whichever compiler reads it, so one node type can
// belong to both at once. An enum could only ever pick one, which would force either an entry that
// lies about the domain it left out, or -- the choice this table actually makes for every name
// whose SHAPE genuinely differs between the two compilers, e.g. Add's scalar gameplay pins versus
// its float3 material ones (see the Material section of buildCatalog() below) -- a second,
// differently-shaped entry under the same type name. The bitmask makes that a choice made per node
// type rather than forced on every one of them: a future case that really is shape-identical in
// both domains sets kDomainBoth on ONE entry instead of adding a duplicate row that looks different
// only in its category field.
//
// A PLAIN ENUM, NOT enum class, for the reason EditorKeybinds.hpp's Scope bitmask is not one
// either: this value is only ever combined and tested with `|`/`&`, never passed somewhere its
// implicit conversion to int would be a hazard, so enum class would buy nothing but an operator
// overload this header has no other use for.
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
    // Which domain(s) this node type belongs to -- see GraphNodeDomain above. Defaults to
    // kDomainGameplay, matching every entry that predates this field: a braced-init-list shorter
    // than the struct's member count leaves the trailing members it did not mention at their own
    // default member initializer, so every existing `t.push_back({...})` call above -- whether it
    // supplied four elements, five, or anything in between -- keeps compiling and keeps meaning
    // exactly what it meant before, with not one of those ~200 lines touched. Only the Material
    // section at the bottom of buildCatalog() sets this explicitly.
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
    t.push_back({"ConstFloat", "Const Float", "Const", {pin("value", "float", true, "0")}});
    t.push_back({"ConstInt",   "Const Int",   "Const", {pin("value", "int",   true, "0")}});
    t.push_back({"ConstBool",  "Const Bool",  "Const", {pin("value", "bool",  true, "false")}});
    // -- arithmetic: a, b in; result out --
    t.push_back({"Add",      "Add",      "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    // -- Print: the node this vocabulary has never had, and the one a Blueprint author reaches for
    //    first. There was no way to observe ANYTHING from inside a graph -- no value, no branch
    //    taken, no event fired -- so debugging one meant adding an OUT record and reading the
    //    GraphHost tick log, which only works for a value you can route all the way to the
    //    graph's own output. LABELLED BY NODE ID rather than by an attribute: `NODE muzzleLen
    //    Print` already names itself, the id is already unique within the graph, and an
    //    attribute would have needed a parser field, a writer field and an editor row to say
    //    what the id says for free.
    // -- VECTOR MATHS. Every math node in this table was SCALAR, in an engine whose world is
    //    centimetres in three axes: a graph wanting a direction, a distance or an offset had to
    //    spell it out one component at a time out of Add and Multiply, which is how
    //    AN_FPCharacter ends up a wall of Const Float. There is no Vec3 PIN TYPE to carry these
    //    (PinType is Float, Int, Bool, Exec), so they take and return loose components -- exactly
    //    the convention GetFieldVec3/SetFieldVec3 already established.
    t.push_back({"VecAdd", "Vec Add", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecSub", "Vec Subtract", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecScale", "Vec Scale", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("s", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecCross", "Vec Cross", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecNormalize", "Vec Normalize", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecLerp", "Vec Lerp", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("t", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VecDot", "Vec Dot", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("result", "float", true)}});
    t.push_back({"VecLength", "Vec Length", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("result", "float", true)}});
    t.push_back({"VecDistance", "Vec Distance", "Vector", {pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false), pin("result", "float", true)}});
    // -- THE ENGINE'S OWN API, reachable from a graph. Measured before these were written: 72 of
    //    the 77 public members of Aver.Framework had no node at all. AverCharacter had exactly one
    //    (Jump) out of eighteen, Game had none out of twelve, AverPlayerController none out of four.
    //    These are the three classes a gameplay graph reaches for first.
    //
    //    Teleport is NOT SetFieldVec3 on CLocal.position, and the difference matters: setting the
    //    transform alone leaves the physics capsule where it was and the character snaps back on
    //    the next step. Teleport moves both and clears velocity.
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
    // -- CONVERSION. Validate refuses a LINK whose two pins differ in type (Graph.cs's
    //    srcPin.Type != tgtPin.Type check), which is what stops an exec pin being wired to a
    //    float -- correct, and it also meant an int or a bool could not reach a float input at
    //    all. Unreal converts silently and shows a little cast bubble on the wire; this
    //    vocabulary has no such machinery, so the cast is a node you can see.
    //    FloatToInt TRUNCATES toward zero, which is what C# (int)f does -- Floor exists for the
    //    other rounding, and having both means neither has to be guessed.
    // INT TO FLOAT IS LOSSY ABOVE 2^24, and in this engine that is not a corner case: a float32
    // carries 24 mantissa bits, so past 16777216 only EVEN integers survive -- and ENTITY HANDLES
    // START AT 16777216. Converting one to a float silently rounds it to its neighbour. That cost
    // an hour: a graph printed a player controller as 16777224 while the ABI returned 16777225,
    // and it read exactly like the engine handing back the wrong entity. Use PrintInt for handles.
    t.push_back({"IntToFloat", "Int To Float", "Convert", {pin("a", "int", false), pin("result", "float", true)}});
    t.push_back({"BoolToFloat", "Bool To Float", "Convert", {pin("a", "bool", false), pin("result", "float", true)}});
    t.push_back({"FloatToInt", "Float To Int", "Convert", {pin("a", "float", false), pin("result", "int", true)}});
    // PrintInt exists because Print takes a float and a float cannot hold an entity handle --
    // see the IntToFloat note above. Anything counting entities, indices or ids wants this one.
    // -- THE ENTITY TRANSFORM, which GetFieldVec3 on CLocal.position only half covered. LOCAL IS
    //    NOT WORLD: a gun parented to a camera has the same local position forever, and a graph
    //    measuring a distance or aiming something needs where it actually is. The three axis nodes
    //    are the transform's own orientation, distinct from the existing GetForward, which reads
    //    an AverCharacter's look direction and its pitch clamp.
    t.push_back({"GetWorldPosition", "Get World Position", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetEntityForward", "Get Forward Axis", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetEntityRight", "Get Right Axis", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetEntityUp", "Get Up Axis", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetLocalScale", "Get Local Scale", "Transform", {pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"IsAlive", "Is Alive", "Transform", {pin("entity", "int", false), pin("alive", "bool", true)}});
    t.push_back({"IsActor", "Is Actor", "Transform", {pin("entity", "int", false), pin("isActor", "bool", true)}});
    t.push_back({"Translate", "Translate", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetLocalScale", "Set Local Scale", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"DestroyEntity", "Destroy Entity", "Transform", {pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // -- PHYSICS. A BODY IS NOT AN ENTITY: a body is a Jolt handle with a shape and a velocity, an
    //    entity is a scene node that may or may not own one, and SetBodyEntity is the bridge. A body
    //    a graph creates reports NO owner to Raycast until something stamps one on, so a trigger the
    //    graph built is invisible to the graph asking what it hit. Body handles ride on INT pins.
    //    OverlapSphere is deliberately absent: it returns an ARRAY, and there is no container pin.
    t.push_back({"GetBodyPosition", "Get Body Position", "Physics", {pin("body", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"GetBodyVelocity", "Get Body Velocity", "Physics", {pin("body", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)}});
    t.push_back({"IsBodyValid", "Is Body Valid", "Physics", {pin("body", "int", false), pin("valid", "bool", true)}});
    t.push_back({"GetBodyCount", "Body Count", "Physics", {pin("count", "int", true)}});
    //    IsPhysicsReady / GetFixedStep: the two Physics STATUS reads (Physics.Ready, Physics.FixedStep).
    //    Pure, no exec, no inputs -- they ask the simulation about itself. Worth nodes because a graph
    //    that adds bodies before aver_phys_init has run gets silent zeros back from every creator, and
    //    until now it had no way to ASK. GetFixedStep is what a graph integrating by hand needs so it
    //    matches the simulation's own step rather than a hardcoded 1/60.
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
    // -- FUNCTION. The three node types a user-defined function is made of. They are in this catalog
    //    for their DISPLAY NAME and their HEADER COLOUR, and deliberately NOT for dropping: the
    //    palette skips the whole "Function" category (see GraphEditor.cpp's Add Node popup), because
    //    none of the three is a node an author places on its own.
    //
    //    A second FuncEntry breaks the function (Validate: "a function begins in exactly one place"),
    //    a FuncReturn with no FUNCOUT to fill has nothing to do, and a CallFunc has no pin shape at
    //    all until it knows its callee. All three are created by the Functions panel instead, which
    //    knows which function they belong to and can give them the right pins immediately.
    //
    //    THEIR PINS HERE ARE EMPTY, and that is correct rather than lazy: every one of the three
    //    takes its pins from a FUNC declaration elsewhere in the file, so a fixed list here would be
    //    a second answer to a question that already has one. GraphEditor::resyncFunctionNodePins is
    //    the single place that derives them, and it agrees pin-for-pin with the C# side's own
    //    AddDefaultPins.
    t.push_back({"FuncEntry", "Function Entry", "Function", {}});
    t.push_back({"FuncReturn", "Return", "Function", {}});
    t.push_back({"CallFunc", "Call Function", "Function", {}});

    // -- TAGS AND VISIBILITY. Entity's own tag bitmask, which the C# side has had all along and the
    //    graph vocabulary could not reach. A tag here is an int bit pattern, not a string, so these
    //    needed none of the compile-time-attribute machinery class=/name=/var= exist for -- and a
    //    graph can COMPUTE a mask, which a string attribute could never do.
    t.push_back({"SetVisible", "Set Visible", "Scene", {pin("exec", "exec", false), pin("entity", "int", false),
        pin("visible", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddTag", "Add Tag", "Scene", {pin("exec", "exec", false), pin("entity", "int", false),
        pin("mask", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"RemoveTag", "Remove Tag", "Scene", {pin("exec", "exec", false), pin("entity", "int", false),
        pin("mask", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"HasTag", "Has Tag", "Scene", {pin("entity", "int", false), pin("mask", "int", false),
        pin("has", "bool", true)}});
    t.push_back({"GetTags", "Get Tags", "Scene", {pin("entity", "int", false), pin("mask", "int", true)}});

    // -- SWITCH. Blueprint's Switch on Int: route the exec chain to ONE of several outputs by an
    //    integer, instead of nesting Branches. Until this existed a three-way choice cost two Branch
    //    nodes and a comparison each, and the graph said "is it 0, else is it 1, else" rather than
    //    what it meant.
    //
    //    FOUR CASES PLUS A DEFAULT, a fixed set rather than a pin count an author grows. Blueprint
    //    lets you add pins; this format derives a node's pins from its TYPE (AddDefaultPins), so a
    //    variable count would need per-node PIN records written into the file and kept in step with
    //    the wiring by hand. Four covers the cases a state machine or a weapon-slot selector actually
    //    has, and a fifth is a second Switch off `default` -- which is exactly what the nesting looks
    //    like when it IS warranted.
    //
    //    `taken` reports which output fired, and -1 for the default, so a graph can observe its own
    //    routing without a parallel chain of comparisons. Same reason Branch has `tookTrue`.
    t.push_back({"SwitchInt", "Switch on Int", "Flow", {pin("exec", "exec", false), pin("selector", "int", false), pin("case0", "exec", true), pin("case1", "exec", true), pin("case2", "exec", true), pin("case3", "exec", true), pin("default", "exec", true), pin("taken", "int", true)}});

    // -- REROUTE. A node that returns exactly what it was given, and exists only so a WIRE can be
    //    bent around something. Blueprint draws these as a bare dot; here they are ordinary small
    //    nodes, because the pin-drawing code already knows how to put one pin on each side and a
    //    special case would be a second thing to maintain for a cosmetic gain.
    //
    //    ONE PER TYPE, because Validate refuses a link whose pins differ in type and there are no
    //    generics here. That is four nodes instead of one, and the alternative -- a wildcard pin
    //    type -- would weaken the check that catches every genuinely wrong wiring.
    //
    //    They compile to NOTHING. A data reroute emits its input and no instruction of its own; the
    //    exec one falls through to the fan-out every exec node ends with. Bending a wire costs a
    //    graph author nothing at run time, which is the only way a purely visual node is honest.
    t.push_back({"RerouteFloat", "Reroute (Float)", "Flow", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"RerouteInt", "Reroute (Int)", "Flow", {pin("a", "int", false), pin("result", "int", true)}});
    t.push_back({"RerouteBool", "Reroute (Bool)", "Flow", {pin("a", "bool", false), pin("result", "bool", true)}});
    t.push_back({"RerouteExec", "Reroute (Exec)", "Flow", {pin("exec", "exec", false), pin("then", "exec", true)}});
    t.push_back({"PrintInt", "Print Int", "Debug", {
        pin("exec", "exec", false), pin("value", "int", false), pin("then", "exec", true)}});
    t.push_back({"Print", "Print", "Debug", {
        pin("exec", "exec", false), pin("value", "float", false), pin("then", "exec", true)}});
    t.push_back({"Multiply", "Multiply", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Subtract", "Subtract", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Divide",   "Divide",   "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    // -- trig: a in; result out (both concurrent-workflow additions, one line each) --
    t.push_back({"Sin", "Sin", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Cos", "Cos", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    // -- math: the standard library a graph could not previously express ------------
    // Every one of these is a PURE VALUE node: no exec pins, so it composes into either
    // compiler. The names are the .ocgraph node types verbatim -- the palette writes what
    // the parser reads, and a mismatch here produces a node that saves and never loads.
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
    // -- gated flow control: the nodes that REMEMBER between activations -----------
    // Their state lives in the same per-instance store a VAR uses, so two entities sharing
    // one graph file gate independently. reset/open/close are BOOL inputs rather than exec
    // pins because an activation does not carry which pin it arrived on -- see the parser.
    t.push_back({"DoOnce", "Do Once", "Flow", {pin("exec", "exec", false), pin("reset", "bool", false), pin("then", "exec", true)}});
    t.push_back({"Gate", "Gate", "Flow", {pin("exec", "exec", false), pin("open", "bool", false), pin("close", "bool", false), pin("then", "exec", true)}});
    t.push_back({"FlipFlop", "Flip Flop", "Flow", {pin("exec", "exec", false), pin("a", "exec", true), pin("b", "exec", true), pin("isA", "bool", true)}});
    // -- logic --
    // BOOLEAN OPERATORS, which this vocabulary did not have at all. Without them "A and B"
    // is a Branch whose true-exec runs a second Branch, OR is not expressible without
    // restructuring everything downstream, and NOT requires swapping two exec wires.
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
    // -- GetField/SetField's Vec3 siblings: a Vec3-kind scene field (CLocal.position, CLight.colour,
    //    ...) as three ordinary float pins rather than one new pin TYPE -- see
    //    scripting/csharp/Aver.Graph/GraphCompiler.cs's FieldKindVec3/RequireVec3Field comments for why
    //    that shape was chosen over adding a "vec3" pin type. Pin sets copied field for field from
    //    OcGraphParser.AddDefaultPins's "getfieldvec3"/"setfieldvec3" cases, same as GetField/SetField
    //    above. No exec pins on either by default (mirrors GetField/SetField exactly).
    t.push_back({"GetFieldVec3", "Get Field (Vec3)", "Scene", {
        pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)},
        {attr("field", "Field")}});
    t.push_back({"SetFieldVec3", "Set Field (Vec3)", "Scene", {
        pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("success", "bool", true)},
        {attr("field", "Field")}});
    // -- GetForward: where a Character is LOOKING, plus where its eyes are. Filed under Scene beside the
    //    field readers rather than under Input, because it reads accumulated STATE (the character's own
    //    yaw/pitch after its clamp) and not this frame's device movement the way MouseDelta does --
    //    reading it twice in a frame gives the same answer twice, which is the property that decides
    //    which group a node belongs in here. Six outputs and no exec pins, copied from
    //    OcGraphParser.AddDefaultPins's "getforward" case; the eye position rides along because a
    //    direction with no origin cannot build a ray (see GraphInterop.LookDirectionForGraph).
    // -- Jump: one call into AverCharacter.Jump, which declines in mid-air by itself, so a graph
    //    wiring this straight to a key gets single jumps and no flight without testing anything.
    //    `jumped` reports whether it actually happened, which "the key was pressed" is not.
    t.push_back({"Jump", "Jump", "Actor", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("then", "exec", true), pin("jumped", "bool", true)},
        {}});
    // -- GetViewEntity: the CAMERA node a character looks through. A first-person viewmodel parents to
    //    this, not to the character -- parent a gun to the pawn and it stays put while the camera
    //    pitches around it. Filed under Scene beside GetForward for the same reason: it reads state,
    //    not this frame's input.
    t.push_back({"GetViewEntity", "Get View Entity", "Scene", {
        pin("entity", "int", false), pin("view", "int", true), pin("success", "bool", true)},
        {}});
    t.push_back({"GetForward", "Get Forward (Look)", "Scene", {
        pin("entity", "int", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true),
        pin("eyeX", "float", true), pin("eyeY", "float", true), pin("eyeZ", "float", true),
        pin("success", "bool", true)},
        {}});
    // -- MouseDelta / MoveAxis: continuous input -- look and move, the two things a first-person
    //    controller is made of, neither of which InputKey's digital key state can express. Both get
    //    exec pins by default (unlike GetField/SetField/GetFieldVec3/SetFieldVec3 above, mirroring
    //    Spawn/Raycast instead) -- see scripting/csharp/Aver.Graph/GraphCompiler.cs's
    //    IsExecCapableMouseDeltaType/IsExecCapableMoveAxisType comments for why: one frame's input
    //    must cost exactly one native call regardless of how many output pins a graph reads, and that
    //    guarantee needs the same exec-visit-cached shape Raycast already established, even though
    //    neither read is expensive the way a physics query is. Pin sets copied field for field from
    //    OcGraphParser.AddDefaultPins's "mousedelta"/"moveaxis" cases. MoveAxis has no "z" pin --
    //    Input.MoveAxis's own Z is hardcoded 0 always (Aver.Framework/Input.cs).
    t.push_back({"MouseDelta", "Mouse Delta", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true),
        pin("deltaX", "float", true), pin("deltaY", "float", true), pin("wheel", "float", true)}});
    t.push_back({"MoveAxis", "Move Axis", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true),
        pin("forward", "float", true), pin("right", "float", true)}});
    // -- InputKey: digital key state, the discrete half of input beside MouseDelta/MoveAxis above.
    //    NO EXEC PINS, and that is the difference from its neighbours rather than an oversight:
    //    reading one polled key is idempotent and returns the same answer however often it is asked
    //    within a frame, so there is nothing to cache and no visit to anchor the read to. Its two
    //    louder neighbours need the exec shape only because they fill several outputs from one call.
    //    `key` is a plain int -- this format has no symbolic enum lookup, so an author writes the
    //    numeric value from Aver.Framework's Key enum. --
    t.push_back({"InputKey", "Input Key", "Input", {
        pin("key", "int", false), pin("down", "bool", true)}});
    // -- InputKeyPressed / InputKeyReleased: the EDGE, where InputKey above gives the STATE.
    //    `down` is true every frame a key is held, which is the wrong answer for jumping, firing
    //    a semi-auto, or toggling anything -- all of which fire once per press. Building that from
    //    InputKey needs a DoOnce and a variable per key; the framework ABI has answered it
    //    directly all along (aver_fw_input_key_pressed / _released). The output pin is named
    //    `triggered` rather than `down` because it is an EVENT and not a state. --
    t.push_back({"InputKeyPressed", "Input Key Pressed", "Input", {
        pin("key", "int", false), pin("triggered", "bool", true)}});
    t.push_back({"InputKeyReleased", "Input Key Released", "Input", {
        pin("key", "int", false), pin("triggered", "bool", true)}});
    // -- Select: pick one of two values by a bool. Pure data, no exec pins. In the PULL compiler BOTH
    //    arms are computed regardless of cond -- see GraphCompiler.EmitSelect, which explains why
    //    that is correct and not a missing short-circuit. --
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
    // WHY THESE THREE ARRIVE LATE. Select, InputKey and Raycast were added to OcGraphParser and to
    // both compilers in 1425b67 and never to this table, so for two slices they were fully supported
    // by the runtime and completely absent from the Add-Node palette -- authorable only by hand-
    // editing .ocgraph text. Nothing caught it because this table is a DELIBERATE separate copy of
    // the vocabulary (see the header comment) with no build-time link to the C# side that would
    // notice the omission. Adding MouseDelta/MoveAxis is what made it visible: the palette would
    // have shown an Input category holding the mouse but not the keyboard.

    // -- graph parameter read, another concurrent-workflow addition; type defaults to float, the
    //    common case, and can be edited per-instance like any other pin since layout/pin-typing
    //    always prefers the node's own recorded pins over this table (see the header comment).
    //    param= names which declared PARAM this node reads. --
    t.push_back({"Param", "Param", "Param", {pin("value", "float", true)}, {attr("param", "Param Name")}});

    // -- flow / exec: control flow, not data flow. "exec" is a PIN TYPE, exactly like "float"/"int"/
    //    "bool" above -- see modules/formats/include/aver/formats/OcGraph.hpp's comment on
    //    OcGraphLink for why that alone is the whole format change this needed. Pin sets below match
    //    scripting/csharp/Aver.Graph/OcGraphParser.cs's AddDefaultPins EXACTLY -- see this file's own
    //    header comment for why that parity is load-bearing, not cosmetic.
    //
    // branch: a bool condition and one incoming exec pulse; exactly one of "true"/"false" fires.
    //    "tookTrue" is OPT-IN OBSERVABILITY (see GraphCompiler.EmitBranch's comment), not required
    //    wiring -- present so a graph author (or a test) can inspect which way a branch went.
    t.push_back({"Branch", "Branch", "Flow", {
        pin("exec", "exec", false), pin("cond", "bool", false),
        pin("true", "exec", true), pin("false", "exec", true), pin("tookTrue", "bool", true)}});
    // sequence: fires each of its exec outputs in file order -- two by default ("then0" then
    //    "then1"); add more via PIN records to widen it. "fireLog" is opt-in observability, the
    //    sequence equivalent of branch's "tookTrue" (see GraphCompiler.EmitExecFanOut's comment).
    t.push_back({"Sequence", "Sequence", "Flow", {
        pin("exec", "exec", false), pin("then0", "exec", true), pin("then1", "exec", true),
        pin("fireLog", "int", true)}});
    // while: "cond" is re-checked every pass (never cached -- see GraphCompiler's PUSH VS PULL
    //    comment); "loop" is the body, "done" fires once after; "iterations" counts completed passes,
    //    both a genuinely useful runtime value and the proof a runaway loop's guard actually bit.
    t.push_back({"While", "While", "Flow", {
        pin("exec", "exec", false), pin("cond", "bool", false),
        pin("loop", "exec", true), pin("done", "exec", true), pin("iterations", "int", true)}});
    // forEach: the COUNTED-REPEAT variant, not a per-element iterator -- the format has no
    //    array/collection pin type yet, so a real "for each item in a list" cannot be expressed
    //    today (see GraphCompiler.EmitForEach's comment for the honest "left rough for phase 2"
    //    note). "count" says how many passes; "index" is the current one, 0..count-1.
    t.push_back({"ForEach", "For Each (counted)", "Flow", {
        pin("exec", "exec", false), pin("count", "int", false),
        pin("loop", "exec", true), pin("index", "int", true), pin("done", "exec", true)}});
    // onstart / ontick: event entry points -- what actually makes one of these run is a top-level
    //    ENTRY <nodeId> <eventName> record (OcGraphData::entryPoints), not anything about this node's
    //    TYPE; these two are just convenience triggers with a single exec output and no inputs of
    //    their own to place at the head of a chain and mark with ENTRY. Per-tick data (delta time, in
    //    particular) is deliberately NOT a special pin here -- it is an ordinary PARAM the graph
    //    declares (e.g. `PARAM deltaTime float`) and reads with a `param` node inside the chain, the
    //    same plumbing every dataflow graph already uses for `time`/`entity`.
    // "Event", NOT "Flow", and the distinction is the whole point of colouring by category. These
    // are where execution ENTERS the graph -- nothing upstream drives them, an ENTRY record does --
    // whereas Branch and Sequence merely reorder execution that is already running. Grouping them
    // with flow control made the palette read as though OnTick were a kind of Branch, and gave the
    // one node a reader most wants to find at a glance the same colour as the most common node on
    // the canvas.
    t.push_back({"OnStart", "On Start", "Event", {pin("exec", "exec", true)}});
    t.push_back({"OnTick", "On Tick", "Event", {pin("exec", "exec", true)}});
    // onhit: same bare-trigger shape as onstart/ontick above -- a labelled starting point an ENTRY
    //    record points at, nothing more. What makes it fire ON DEMAND (a host calling
    //    Aver.Graph.GraphHost.Fire, rather than the fixed per-frame Tick() cadence OnStart/OnTick get)
    //    is entirely a scripting/csharp/Aver.Graph/GraphHost.cs concept -- this editor, like the C#
    //    parser's own AddDefaultPins, treats "OnHit" as nothing more than one more ENTRY event name; a
    //    project inventing a different one needs no new catalog entry to place ITS trigger node, only
    //    a differently-named NODE of type OnStart/OnTick/OnHit (any bare-trigger type already
    //    suffices) and its own ENTRY record naming the event.
    t.push_back({"OnHit", "On Hit", "Event", {pin("exec", "exec", true)}});
    // -- CustomEvent: an entry point whose event NAME is the author's, not one of three the
    //    palette happened to ship. Same shape as the three above and the same treatment
    //    everywhere -- what fires an event has always been the top-level ENTRY record, never the
    //    node type. The editor keeps a `name=` attribute on the NODE line in step with that
    //    record so the canvas has something to show and edit; nothing at runtime reads it. --
    t.push_back({"CustomEvent", "Custom Event", "Event", {pin("exec", "exec", true)}});

    // Spawn: SIDE-EFFECTING (creates a new scene entity), so -- unlike GetField/SetField/GetFieldVec3/
    //    SetFieldVec3 above -- it gets exec pins by default, mirroring Raycast's own reasoning. See
    //    scripting/csharp/Aver.Graph/GraphCompiler.cs's IsExecCapableSpawnType comment for why it is
    //    refused by the pure-dataflow (PULL) compiler even more strictly than SetField is. class= names
    //    which registered class to spawn -- the same generic key=value NODE-line attribute field=/
    //    param= already use.
    t.push_back({"Spawn", "Spawn", "Actor", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("entity", "int", true)},
        {attr("class", "Class")}});

    // CharacterMove: the last Blueprint-parity node -- ONE coarse, exec-only wrapper around
    //    AverCharacter.Drive (via AverCharacter.DriveFromGraph -> GraphInterop.CharacterMoveForGraph),
    //    matching the owner's own literal signature: CharacterMove(entity, dt, forward, right,
    //    yawDelta, pitchDelta) -> then, success. See
    //    scripting/csharp/Aver.Graph/GraphCompiler.cs's IsExecCapableCharacterMoveType comment for why
    //    it is refused by the pure-dataflow (PULL) compiler exactly as strictly as Spawn is. UNLIKE
    //    Spawn's class= above, this node has NO NODE-line attribute at all: every input the native
    //    call needs is an ordinary pin, because a graph author computes dt/forward/right/yawDelta/
    //    pitchDelta at RUNTIME (a PARAM, a MoveAxis, a MouseDelta), never chooses them at edit time
    //    the way a class name is chosen. "success" is a real outcome -- false, with a log line, never
    //    a throw and never a silent no-op -- when the entity is not a live actor, or is a live actor
    //    that is not an AverCharacter; see GraphInterop.CharacterMoveForGraph's own comment.
    t.push_back({"CharacterMove", "Character Move", "Actor", {
        pin("exec", "exec", false),
        pin("entity", "int", false), pin("dt", "float", false),
        pin("forward", "float", false), pin("right", "float", false),
        pin("yawDelta", "float", false), pin("pitchDelta", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // GetSynapseTarget: a PURE node reading CSynapseAgent's current steering target, tracked by the
    //    native AgentSystem tick (aver_fw_synapse_target, framework_abi.h). NO exec pins -- a data
    //    read exactly like GetWorldPosition, refused by side-effect rules for the identical reason
    //    (see IsExecCapableCharacterMoveType's own comment on what "PURE" means here). "success" --
    //    not a more specific name -- to reuse EmitPullVec3Read's own x/y/z-plus-bool shape exactly as
    //    GetWorldPosition does, and because false here is a real "nothing to head toward right now"
    //    outcome (no CSynapseAgent, or its status is not Pathing), not an error -- see
    //    GraphInterop.SynapseGetTargetForGraph's own comment.
    t.push_back({"GetSynapseTarget", "Get Synapse Target", "Actor", {
        pin("entity", "int", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true),
        pin("success", "bool", true)}});

    // SynapseSteer: a PURE node turning "where am I, where do I want to go" into the
    //    forward/right/yawDelta CharacterMove above already consumes -- see
    //    GraphInterop.SynapseSteerForGraph's own comment for the full contract. Takes an EXPLICIT
    //    target (x/y/z), never CSynapseAgent's own: the identical node does direct chase (a seen
    //    enemy's live position) and path-following (GetSynapseTarget's own output above) for that
    //    reason, and neither this node nor the compiler needs to know which one a graph is doing.
    //    "success" -- the entity was alive, matching every other pure node's meaning for that pin --
    //    is a SEPARATE output from "arrived": a graph that never checks success still gets usable
    //    (if meaningless) zeros for a dead entity, but a caller that DOES check it can tell "nothing
    //    happened" from "arrived and correctly holding still".
    t.push_back({"SynapseSteer", "Synapse Steer", "Actor", {
        pin("entity", "int", false), pin("dt", "float", false),
        pin("targetX", "float", false), pin("targetY", "float", false), pin("targetZ", "float", false),
        pin("turnRate", "float", false), pin("arriveRadius", "float", false),
        pin("forward", "float", true), pin("right", "float", true), pin("yawDelta", "float", true),
        pin("arrived", "bool", true), pin("success", "bool", true)}});

    // GetSynapsePerception: a PURE node reading CSynapsePerception's current sight state, tracked
    //    by the native PerceptionSystem tick (aver_fw_synapse_perception, framework_abi.h). The
    //    companion query "OnSeeTarget" itself needs -- the graph-event seam
    //    (ScriptHost::graphFire) carries no payload, so a handler for "I just saw something" has no
    //    other way to ask WHICH entity that was. "success" means something DIFFERENT here than on
    //    every other node above: it is NOT "could I see the target" (canSeeTarget answers that, and
    //    false is a real, common, meaningful state -- see GraphInterop.SynapseGetPerceptionForGraph's
    //    own comment) -- it means "does this entity carry CSynapsePerception at all".
    t.push_back({"GetSynapsePerception", "Get Synapse Perception", "Actor", {
        pin("entity", "int", false),
        pin("canSeeTarget", "bool", true), pin("lastKnownTarget", "int", true),
        pin("timeSinceSeen", "float", true), pin("success", "bool", true)}});

    // -- AUDIO. The mixer, the WASAPI device and the whole aver_audio_* C ABI were built, tested and
    //    then never called by anything for weeks -- see docs' own "declared but unread" shape. These
    //    six are the graph half of connecting it, alongside Aver.Framework's Audio class.
    //
    //    sound= is a PATH and a NODE-line ATTRIBUTE, not a pin, for SetName's exact reason: which
    //    file to play is edit-time data and PinType has no String member. The load behind it is
    //    cached natively (same path -> same handle, decoded once), so a node that plays every tick
    //    costs a lookup rather than a decode.
    //
    //    PlaySound/PlaySoundAt are EXEC: making a noise is a side effect, and a dataflow pull would
    //    fire one per invocation with nothing able to gate it -- the identical argument Spawn and
    //    CreateEntity already make. Both hand back a VOICE int so a graph can stop or steer it.
    //    "voice" is 0 when there is no audio device, which is a SUPPORTED configuration rather than
    //    an error, so success being false does not mean something went wrong.
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
    //    ANOTHER entity's own graph. SIDE-EFFECTING (runs a stranger's whole exec chain, not a scalar
    //    write) -- exec pins by default, mirroring Spawn/CharacterMove above rather than SetField; see
    //    scripting/csharp/Aver.Graph/GraphCompiler.cs's IsExecCapableFireEventType comment for why it
    //    is refused by the pure-dataflow (PULL) compiler entirely, the same strictness Spawn/
    //    CharacterMove get. "target" is an ordinary int PIN (computed at runtime -- a Spawn's own
    //    entity output, a VAR, a Raycast's entity pin), NOT a NODE-line attribute, unlike event=:
    //    which entity to fire at is runtime data, exactly the same "pin vs attribute" split Spawn's
    //    x/y/z-pins-vs-class=-attribute already established. "fired" is a real outcome -- false, with
    //    a log line, never a silent true -- when the target has no live graph at all, or one that
    //    never declared this event; see Aver.Graph/GraphEvents.cs's own comment for the full failure-
    //    mode table and the reentrancy guard that keeps a self-fire or a fire cycle between graphs
    //    from stack-overflowing the process.
    t.push_back({"FireEvent", "Fire Event", "Actor", {
        pin("exec", "exec", false), pin("target", "int", false),
        pin("then", "exec", true), pin("fired", "bool", true)},
        {attr("event", "Event")}});

    // GetVar / SetVar: graph-local PERSISTENT variables -- the "nothing survives between ticks" gap,
    // closed by storage the GraphHost driving a compiled graph owns per instance (see
    // scripting/csharp/Aver.Graph/GraphVarStore.cs's own comment for the full contract). var= names
    // which declared VAR the node addresses, the same generic key=value NODE-line attribute mechanism
    // field=/param=/class= already use.
    //
    // GetVar: a PURE READ, so -- like GetField/GetFieldVec3 above -- no exec pins. Pin type defaults to
    // float here (adjustable per-instance, same convention Param's own catalog entry documents, since
    // layout/pin-typing always prefers a node's own recorded pins over this table -- see the header
    // comment).
    t.push_back({"GetVar", "Get Var", "Var", {pin("value", "float", true)}, {attr("var", "Var Name")}});
    // SetVar: A WRITE IS A SIDE EFFECT (see GraphCompiler.IsExecCapableVarSideEffectType's own comment),
    // so -- UNLIKE GetField/SetField/GetFieldVec3/SetFieldVec3, which get NO exec pins by default --
    // this DOES get exec pins by default, mirroring Spawn/Raycast rather than SetField: SetVar has no
    // legitimate non-exec path at all, so a freshly palette-spawned node needs to already be usable, not
    // require an author to hand-add exec pins before it does anything useful. No "success" pin -- a
    // write into an in-process store has no runtime failure mode a native field write does (unknown
    // entity, read-only field, missing component), so there is nothing left to report.
    t.push_back({"SetVar", "Set Var", "Var", {
        pin("exec", "exec", false), pin("value", "float", false), pin("then", "exec", true)},
        {attr("var", "Var Name")}});

    // -- SetParent / SetViewEntity / SetName: three one-ABI-call writes, dispatched SetField-style --
    //    no exec pins by default (unlike Spawn/SetVar above), reachable from BOTH C# compilers, and
    //    still refused if pulled as a bare data value with no exec visit -- see
    //    scripting/csharp/Aver.Graph/OcGraphParser.cs's "SetParent / SetViewEntity / SetName" comment
    //    for the full "why SetField-style, not Spawn/SetVar-style" reasoning this table's own pin sets
    //    were copied from field for field.
    //
    //    SetParent: aver_scene_set_parent(child, parent) -> success (scene_abi.h:105). Already refuses
    //    a cycle, a self-parent, and a doomed parent, returning 0 -- surfaced on "success" rather than
    //    swallowed.
    t.push_back({"SetParent", "Set Parent", "Scene", {
        pin("child", "int", false), pin("parent", "int", false), pin("success", "bool", true)}});
    //    SetViewEntity: aver_fw_set_view_entity(entity) -> void (framework_abi.h:206). NO OUTPUT PIN --
    //    the ABI returns nothing, so there is no return code to invent one for.
    t.push_back({"SetViewEntity", "Set View Entity", "Actor", {
        pin("entity", "int", false)}});
    //    SetName: aver_scene_set_name(entity, name) -> success (scene_abi.h:113). name= is a NODE-line
    //    attribute, not a pin -- the string IS the data this node writes, not a lookup key, but PinType
    //    has no String member (see OcGraphParser.cs's own PinType-has-no-String comment), so a
    //    NODE-line attribute is still the only route it can reach this node.
    t.push_back({"SetName", "Set Name", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("name", "Name")}});

    //    CreateEntity / FindEntity: the other two members of SetName's own name= family
    //    (Entity.Create(name), Game.Find(name)). They REUSE name=/Node.NameValue verbatim -- the
    //    parser already carries it for SetName, so neither needed a single line of new parsing, and
    //    the C++ writer round-trips it through the generic extraTokens path like every other key=value.
    //
    //    CreateEntity is EXEC (it makes a new entity -- a side effect, refused by the pure-dataflow
    //    compiler exactly as Spawn is), FindEntity is PURE (a lookup is idempotent, like GetWorldPosition).
    //    FindEntity returns 0 when nothing matches, which is a real, common answer and not an error --
    //    "found" says which case it is, the same split GetSynapsePerception's own success pin uses.
    t.push_back({"CreateEntity", "Create Entity", "Scene", {
        pin("exec", "exec", false),
        pin("then", "exec", true), pin("entity", "int", true), pin("success", "bool", true)},
        {attr("name", "Name")}});
    t.push_back({"FindEntity", "Find Entity", "Scene", {
        pin("entity", "int", true), pin("found", "bool", true)},
        {attr("name", "Name")}});

    // -- SetMesh / SetMaterial: coarse, dedicated nodes wrapping Entity.SetMesh/SetMaterial
    //    (EntityScene.cs) through Aver.Framework.GraphInterop.SetMeshForGraph/SetMaterialForGraph --
    //    NOT a generalised I64-capable SetField and NOT a generic "add a missing component" node; see
    //    OcGraphParser.cs's own "SetMesh / SetMaterial" comment. Same SetField-style dispatch as the
    //    three types just above (EnsureMeshRenderer's own idempotent "add if absent" guard is what
    //    makes re-running this every tick harmless).
    t.push_back({"SetMesh", "Set Mesh", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("mesh", "Mesh")}});
    t.push_back({"SetMaterial", "Set Material", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("material", "Material")}});

    // -- AttachToSocket: hangs `entity` on a named socket of `parent`'s rig, so it rides the posed
    //    bone every frame. TWO entity pins rather than one, unlike every Set* node above, because
    //    an attachment is a relationship: the thing and what it hangs from. socket= names it, the
    //    same NODE-line-attribute mechanism mesh=/material=/name= use, because PinType has no
    //    string member and this is the only way a literal name reaches a node.
    t.push_back({"AttachToSocket", "Attach To Socket", "Scene", {
        pin("entity", "int", false), pin("parent", "int", false), pin("success", "bool", true)},
        {attr("socket", "Socket")}});

    // -- GetAnimCurve: a PURE read, so it has no exec pins and needs no exec emitter -- the only
    //    node in this animation group that reads rather than writes. curve= names it, the same
    //    NODE-line attribute mechanism every other string carrier here uses.
    t.push_back({"GetAnimCurve", "Get Anim Curve", "Scene", {
        pin("entity", "int", false), pin("value", "float", true)},
        {attr("curve", "Curve")}});

    // -- SetSkeleton / PlayAnimation: SetMesh/SetMaterial's own animation-family siblings, wrapping
    //    Aver.Framework.Entity.SetSkeleton/PlayAnimation (Animation.cs) through
    //    GraphInterop.SetSkeletonForGraph/PlayAnimationForGraph -- same SetField-style dispatch, same
    //    "EnsureComponent's own idempotent add-if-absent guard is what makes re-running this every
    //    tick harmless" reasoning SetMesh's own comment gives (SetSkeleton/PlayAnimation each add
    //    their component -- CSkeletalMesh/CAnimator -- the identical way EnsureMeshRenderer does).
    //
    //    BINDING IS BY ARRAY INDEX, NOT NAME (AnimSampler.cpp's `t.boneIndex` bounds-check, never an
    //    identity-check) -- this node cannot enforce that a skeleton= and a clip= authored on the same
    //    entity actually agree on joint order; a mismatch drives the wrong bone, or silently drops the
    //    track if out of range, with no error this node -- or anything downstream of it -- can raise.
    //    That is a content problem this graph layer has no visibility into, not a gap in the node.
    t.push_back({"SetSkeleton", "Set Skeleton", "Scene", {
        pin("entity", "int", false), pin("success", "bool", true)},
        {attr("skeleton", "Skeleton")}});
    //    PlayAnimation gets a THIRD input pin -- loop -- that no Set*-shaped node above needs, because
    //    Entity.PlayAnimation itself takes a second scalar argument (Animation.cs's own `bool loop =
    //    true`), unlike SetMesh/SetMaterial/SetSkeleton's single string write. A PIN, not a NODE-line
    //    attribute, for the opposite reason clip= is one: loop is genuine runtime data a graph may
    //    reasonably compute (e.g. "loop unless this is the death clip"), not edit-time-only naming, so
    //    it belongs on the wire the same way PlaySound's own "looping" pin does just above. The default
    //    "true" mirrors Animation.cs's own default parameter -- unlike PlaySound's looping (which
    //    defaults to non-looping when left unwired), a freshly spawned PlayAnimation node should behave
    //    like calling PlayAnimation(clip) from C# with nothing else touched.
    t.push_back({"PlayAnimation", "Play Animation", "Scene", {
        pin("entity", "int", false), pin("loop", "bool", false, "true"), pin("success", "bool", true)},
        {attr("clip", "Clip")}});

    // ============================================================================================
    // MATERIAL NODES -- DOMAIN material, compiled to HLSL by aver::pbr::compileMaterialGraph()
    // (modules/render.pbr/src/MaterialGraphHlsl.cpp). READ THAT FILE'S emitNode() FIRST: it is the
    // authority on every node type below, on every pin name, and on every promotion rule this table
    // only describes; this section is the palette's VIEW of that authority, not a second definition
    // of it. A mismatch here produces exactly the failure this whole table exists to prevent -- a
    // node that spawns from the Add-Node menu and then refuses to compile.
    //
    // WHAT A MATERIAL NODE IS, AND WHY IT CAN NEVER CARRY AN EXEC PIN. Every gameplay node above
    // describes a STEP: it may run a side effect, and it is reached by an exec pulse that arrives on
    // one pin and leaves on another, in an order an ENTRY record and the exec wiring decide. A
    // material node describes a VALUE, not a step. A material graph has no exec pins anywhere in it,
    // no ENTRY point and no OUT record (see MaterialGraphHlsl.cpp's own header comment, point 1 --
    // "IT IS PULL, NOT PUSH"), because averEvalMaterial runs once per pixel and produces one surface;
    // "once per pixel" has no room for "and then do this". Every node below is pure data-flow:
    // compileMaterialGraph starts at the one MaterialOutput node and walks BACKWARDS along links,
    // emitting a node only when something downstream actually reads it, so the order code comes out
    // in is whatever that dependency walk decides, never the order nodes were dropped on the canvas
    // or wired left to right. There is no `exec`/`then` pair on a single entry in this section, and
    // there never can be one for the same reason there is no Branch or Sequence in HLSL's per-pixel
    // evaluation: "this happens before that" is not a question a pixel shader's data-flow answers.
    //
    // WIDTH IS NOMINAL HERE, NOT ENFORCED HERE. Every node the vocabulary generalises over width
    // (Add, Sin, Saturate, Clamp, ...) is declared float3 below, because a colour, a direction or a
    // position -- float3 -- is what an author reaches for one of these on first. The compiler does
    // not actually hold a freshly spawned node to that width: emitNode's widestInput() re-derives
    // the REAL width from whatever is actually linked into a generic node's inputs at compile time
    // (an input that is only a literal does not count towards it -- see widestInput's own comment),
    // so wiring a float2 UV into an Add's `a` computes at float2, not float3, whatever this table
    // says. What this table DOES have to get exactly right, because nothing downstream re-derives
    // it, is the pin NAMES a link or a literal is addressed by, and the DEFAULT LITERAL on a pin
    // nothing gets wired to -- both ride on a freshly spawned node verbatim, straight from here.
    //
    // A KNOWN, ACCEPTED NAME COLLISION. Several material type names below -- Add, Subtract,
    // Multiply, Divide, Min, Max, Lerp, Clamp, Saturate, Abs, Floor, Ceil, Sqrt, Sin, Cos, and
    // ConstFloat -- are ALSO existing gameplay type names above, because both compilers independently
    // reached for the same short verb for the same arithmetic. That is not a naming accident this
    // table can paper over: emitNode() and GraphCompiler.cs's own switch each key off the literal
    // node TYPE string, so a material Add must be spelled exactly "Add" for the material compiler to
    // recognise it -- the identical string the gameplay compiler already owns for its own,
    // differently-shaped, scalar Add. findGraphNodeDesc(typeId) has no domain parameter and returns
    // the FIRST entry whose type matches, which for every name on that list is still the gameplay
    // entry pushed earlier in this function; so today the Add-Node popup's Const/Math/Vector/Input
    // categories show both a name's gameplay and material shapes side by side, and
    // addNodeFromCatalog (GraphEditor.cpp) resolves either menu item to the SAME gameplay shape
    // until something teaches that lookup which domain the open graph actually is. Fixing that
    // belongs to GraphEditor.cpp, not to this table: this table's job here is to describe the
    // material vocabulary completely and exactly, one entry per node type, the same as every
    // gameplay entry above it. ConstFloat's shape below is worth calling out on its own -- it is
    // byte-for-byte the SAME as the gameplay ConstFloat entry at the top of this function (one
    // `value` output, default "0"), because a bare literal number means the same thing to both
    // compilers. It still gets its own entry here rather than kDomainBoth on the existing one, for
    // the identical "do not touch the ~200 existing rows" reason the domain field itself defaults to
    // gameplay; GraphNodeDomain exists so a future cleanup that does touch that row has a value to
    // set on it, not to force one here.
    // ============================================================================================

    // -- CONST: the vocabulary's own literals, one entry per width. Shape matches
    //    MaterialGraphHlsl.cpp's ConstFloat/ConstFloat2/ConstFloat3/ConstFloat4 case exactly: a
    //    single `value` output whose own default IS the constant, the same idiom the gameplay
    //    ConstFloat entry above already established -- the literal rides on the pin, not on a
    //    NODE-line attribute, because a PIN record already round-trips a default through load/save.
    t.push_back({"ConstFloat",  "Const Float",  "Const", {pin("value", "float",  true, "0")},
        {}, kDomainMaterial});
    t.push_back({"ConstFloat2", "Const Float2", "Const", {pin("value", "float2", true, "0,0")},
        {}, kDomainMaterial});
    t.push_back({"ConstFloat3", "Const Float3", "Const", {pin("value", "float3", true, "0,0,0")},
        {}, kDomainMaterial});
    t.push_back({"ConstFloat4", "Const Float4", "Const", {pin("value", "float4", true, "0,0,0,0")},
        {}, kDomainMaterial});

    // -- INPUT: what the renderer already knows about this pixel or this object, read-only and
    //    needing no wiring at all -- emitNode's own "what the renderer knows about this pixel"
    //    section. UV is the surface's own UV (averSurfaceUV); the rest are xyz reads off the
    //    vertex, the camera or the instance transform -- see MaterialGraphHlsl.cpp for exactly which
    //    field each one binds.
    t.push_back({"UV",             "UV",             "Input", {pin("uv",  "float2", true)}, {}, kDomainMaterial});
    t.push_back({"WorldPosition",  "World Position",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"WorldNormal",    "World Normal",    "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"ViewDirection",  "View Direction",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"CameraPosition", "Camera Position",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"ObjectPosition", "Object Position",  "Input", {pin("xyz", "float3", true)}, {}, kDomainMaterial});

    // -- MATH: generic-width arithmetic and the standard library over it, promoted at COMPILE TIME
    //    to the widest of whatever is actually linked in -- see the section comment above on why
    //    "float3" here is nominal, not enforced. Pin names a/b/result match emitNode's binary()/
    //    call() helpers exactly.
    t.push_back({"Add",      "Add",      "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Subtract", "Subtract", "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Multiply", "Multiply", "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Divide",   "Divide",   "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Min",      "Min",      "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Max",      "Max",      "Math", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    // Power/Modulo -- named for what they DO, not gameplay's Pow/Mod type strings: emitNode's own
    // switch checks `ciEquals(ty, "Power")` / `ciEquals(ty, "Modulo")` verbatim, so these exact
    // spellings are load-bearing, not a style choice this table is free to shorten.
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

    // The single-input standard library: one `x` in, one `result` out, both generic-width. Fourteen
    // node types sharing one shape -- emitNode dispatches every one of these through the same call()
    // helper, differing only in which HLSL intrinsic (or, for OneMinus, expression) it names.
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

    // -- VECTOR: the geometry ops a shader author reaches for that gameplay's own Vec* nodes above
    //    do not cover in this shape -- these take and return real float2/float3/float4 pins, because
    //    a material pin genuinely IS that wide (OcGraphPin::type carries it), unlike PinType in the
    //    gameplay/exec vocabulary, which has no vector type at all and spells a direction out as
    //    three loose floats instead.
    t.push_back({"Dot",      "Dot",      "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Length",   "Length",   "Vector", {pin("x", "float3", false), pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Distance", "Distance", "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float", true)}, {}, kDomainMaterial});
    t.push_back({"Cross",    "Cross",    "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"Reflect",  "Reflect",  "Vector", {pin("i", "float3", false), pin("n", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});
    t.push_back({"BlendNormals", "Blend Normals", "Vector", {pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}, {}, kDomainMaterial});

    // Assembling and taking apart: MakeFloatN builds a wider value from loose scalars, Split is its
    // inverse. Split's INPUT pin and its first OUTPUT pin are both named "x" -- that duplication is
    // the vocabulary's own (emitNode's Split case reads input pin "x" and answers output pins
    // "x"/"y"/"z"/"w"), not a typo here; isOutput is what tells the two apart, the same as every
    // other pin pair in this table.
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
    // Swizzle: the one node whose OUTPUT WIDTH is an ATTRIBUTE (mask=), not a pin type -- see
    // emitNode's own comment on why the mask is validated there rather than trusted. The declared
    // `result` pin type below is the nominal float the vocabulary gives it; the REAL width is
    // however many characters mask= names, one to four, decided at compile time.
    t.push_back({"Swizzle", "Swizzle", "Vector", {pin("x", "float3", false), pin("result", "float", true)},
        {attr("mask", "Mask")}, kDomainMaterial});

    // -- UV: coordinate transforms, both reading THE SURFACE'S OWN UV when their `uv` input is left
    //    unwired -- emitNode's uvInput(), the one place in this compiler where an unlinked pin is
    //    not simply its literal default (see uvInput's own comment for why). The `uv` pin below still
    //    needs to exist and be named exactly "uv" for a LINK to land on; it carries no default worth
    //    writing, since one is never actually read.
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

    // -- TEXTURE: the one sampling node -- ONE call and THREE output pins (rgb/a/rgba) off the same
    //    sample, so a graph reading only `.a` costs one sample and one swizzle, not three (see
    //    emitNode's own comment). slot= names which of the material's own texture slots to read --
    //    basecolor, metalrough, normal, occlusion, emissive, layer1basecolor, layer1metalrough or
    //    layer1normal, the exact eight emitNode's kSlotNames accepts. This table cannot validate the
    //    slot= VALUE any more than it validates field=/class= elsewhere in the gameplay vocabulary; a
    //    bad one is a compile-time error from compileMaterialGraph, named clearly, same as every
    //    other attribute this mechanism carries.
    t.push_back({"SampleTexture", "Sample Texture", "Texture", {
        pin("uv", "float2", false),
        pin("rgb", "float3", true), pin("a", "float", true), pin("rgba", "float4", true)},
        {attr("slot", "Slot")}, kDomainMaterial});

    // -- UTILITY --
    t.push_back({"Fresnel", "Fresnel", "Utility", {
        pin("power", "float", false, "5"), pin("result", "float", true)}, {}, kDomainMaterial});
    // If: a branchless select (lerp+step under the hood -- see emitNode's own comment for why not
    // HLSL's `?:`). `a`/`b` are the scalars compared; `ifTrue`/`ifFalse` are the generic-width arms
    // actually returned.
    t.push_back({"If", "If", "Utility", {
        pin("a", "float", false), pin("b", "float", false),
        pin("ifTrue", "float3", false), pin("ifFalse", "float3", false),
        pin("result", "float3", true)}, {}, kDomainMaterial});

    // -- OUTPUT: the one sink a material graph has. NO OUTPUT PINS AT ALL -- nothing ever reads a
    //    MaterialOutput, by construction, since it is where the backward walk that reads everything
    //    else in the graph starts. AND NO DEFAULT VALUE ON ANY OF ITS TWELVE INPUTS -- that emptiness
    //    is load-bearing, not an oversight: compileMaterialGraph treats an input as DRIVEN when it is
    //    linked OR carries a NON-EMPTY literal, so a default here would make a freshly spawned
    //    MaterialOutput drive all eight fields the moment it exists, destroying the partial-graph
    //    behaviour that lets a real graph say only "base colour is red" and leave roughness, the
    //    normal map and alpha exactly what the stock material already had. See
    //    compileMaterialGraph's own "ONLY THE FIELDS THE AUTHOR ACTUALLY DROVE" comment for the full
    //    reasoning; this entry's job is only to not silently break it by typing a "0" into a
    //    defaultValue some future edit adds without reading that comment first.
    t.push_back({"MaterialOutput", "Material Output", "Output", {
        pin("BaseColor", "float3", false), pin("Metallic", "float", false), pin("Roughness", "float", false),
        pin("Normal", "float3", false), pin("Emissive", "float3", false), pin("Occlusion", "float", false),
        pin("Opacity", "float", false), pin("AlphaCutoff", "float", false),
        // Subsurface, and the reason it is worth a pin rather than only a material constant: the
        // scalar in the .ocmat is one number for a whole object, while the thing that actually makes
        // subsurface read correctly is a MASK -- thin parts of a mesh scatter more than thick ones.
        // Driving SubsurfaceRadius from a texture is the difference between a uniformly waxy object
        // and one whose ears and fingers light up. Same no-default rule as every pin above.
        pin("SubsurfaceWeight", "float", false), pin("SubsurfaceRadius", "float", false),
        // The dielectric pair. Driving Transmission from a mask is one mesh that is a clear window
        // with a frosted band, or a bottle with an opaque label, instead of two meshes and two
        // materials. Ior is per-pixel for the same reason, though it moves far less often.
        pin("Ior", "float", false), pin("Transmission", "float", false),
        // The coat, and this is where a coat stops being three numbers and starts being a surface:
        // a weight mask makes one material polished where an object is handled and bare where it is
        // worn, and a roughness mask puts a clear panel and a scuffed edge on the same car-paint
        // material. Present whether or not the layered BSDF is compiled in -- AverAuthored carries
        // the fields unconditionally so a graph does not stop compiling when the setting changes.
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

// THE TABLE. inline + function-local static so this header can be included from multiple translation
// units (the palette, node-spawning code, and the headless test) without an ODR violation and without
// needing its own .cpp.
inline const std::vector<GraphNodeDesc>& graphNodeCatalog() {
    static const std::vector<GraphNodeDesc> table = detail::buildCatalog();
    return table;
}

// Case-insensitive lookup: the file format and GraphCompiler.cs both accept mixed case for node type
// names ("ConstFloat" in the checked-in cross-impl fixture, "constfloat" in the C# switch). Returns
// nullptr for a type this catalog does not know -- the node still draws from its own recorded pins
// (see GraphEditorGeometry.hpp), it just cannot be spawned fresh from the palette.
inline const GraphNodeDesc* findGraphNodeDesc(const std::string& typeId) {
    for (const GraphNodeDesc& d : graphNodeCatalog()) {
        if (detail::ciEquals(d.typeId, typeId)) return &d;
    }
    return nullptr;
}

// The same lookup, but preferring an entry that serves `domain`.
//
// SIXTEEN NAMES ARE IN BOTH VOCABULARIES -- Add, Multiply, Lerp, Saturate, Sin and the rest -- and
// they are NOT the same node: the gameplay Add takes two scalars because PinType has no vector
// types at all, while the material one takes two float3s. Resolving by name alone therefore gives a
// material graph the scalar shape, and an author dropping Add into a material graph gets a node
// whose pins do not fit anything around them. This is what the overload exists for.
//
// FALLS BACK TO THE PLAIN LOOKUP rather than returning null, deliberately. A node type that only
// one domain declares is still the right answer for the other: an OLDER graph naming a type this
// build has since moved between domains, or a gameplay-only node a material author is looking at in
// a file someone hand-edited, should still draw with the pins the catalog knows rather than lose
// them. Refusing here would turn a cosmetic mismatch into a node that cannot be drawn at all.
inline const GraphNodeDesc* findGraphNodeDescIn(const std::string& typeId, GraphNodeDomain domain) {
    for (const GraphNodeDesc& d : graphNodeCatalog()) {
        if (detail::ciEquals(d.typeId, typeId) && (d.domain & domain) != 0u) return &d;
    }
    return findGraphNodeDesc(typeId);
}

} // namespace aver::editor
