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
#include <string>
#include <utility>
#include <vector>

namespace aver::editor {

// One pin a freshly-spawned node of a given type starts with. Field names/order mirror
// aver::fmt::OcGraphPin exactly so a catalog entry converts into a real pin with no per-field mapping.
struct GraphPinSpec {
    std::string name;
    std::string type;         // "float" | "int" | "bool" | "string" -- matches OcGraphPin::type
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

// One entry in the node palette / spawn table.
struct GraphNodeDesc {
    std::string typeId;                // matches OcGraphNode::type; looked up case-insensitively
    std::string displayName;           // node header / palette label
    std::string category;              // palette grouping
    std::vector<GraphPinSpec> pins;    // inputs and outputs mixed; isOutput distinguishes which
    std::vector<GraphAttributeSpec> attributes; // NODE-line key=value attributes this type takes;
                                                 // empty for every type that has none (most of them).
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
    // -- CONVERSION. Validate refuses a LINK whose two pins differ in type (Graph.cs's
    //    srcPin.Type != tgtPin.Type check), which is what stops an exec pin being wired to a
    //    float -- correct, and it also meant an int or a bool could not reach a float input at
    //    all. Unreal converts silently and shows a little cast bubble on the wire; this
    //    vocabulary has no such machinery, so the cast is a node you can see.
    //    FloatToInt TRUNCATES toward zero, which is what C# (int)f does -- Floor exists for the
    //    other rounding, and having both means neither has to be guessed.
    t.push_back({"IntToFloat", "Int To Float", "Convert", {pin("a", "int", false), pin("result", "float", true)}});
    t.push_back({"BoolToFloat", "Bool To Float", "Convert", {pin("a", "bool", false), pin("result", "float", true)}});
    t.push_back({"FloatToInt", "Float To Int", "Convert", {pin("a", "float", false), pin("result", "int", true)}});
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

} // namespace aver::editor
