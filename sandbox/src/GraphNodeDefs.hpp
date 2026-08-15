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
// KNOWN GAP, not fixed here: GetField/SetField (and their Vec3 siblings GetFieldVec3/SetFieldVec3
// below) address a scene field by name via a `field=` NODE-line attribute on the C# side's own reader
// (scripting/csharp/Aver.Graph/OcGraphParser.cs, "field=" case). Spawn (below) addresses a registered
// class the same way, via `class=` -- same reader, same generic key=value mechanism, same gap.
//
// THIS COMMENT USED TO CLAIM "the shared C++ .ocgraph grammar has no key=value attribute syntax at
// all" -- CHECKED AGAINST THE CODE (as of this note) AND FOUND STALE, per the task that asked whoever
// next touched this file to verify it: modules/formats/src/OcGraph.cpp's NODE-line parsing (see
// isNumericToken's callers) reads only what parses as a numeric x/y coordinate as position; every
// OTHER trailing token -- `field=CLocal.position`, `param=time`, anything -- is captured VERBATIM into
// OcGraphNode::extraTokens (modules/formats/include/aver/formats/OcGraph.hpp) and re-emitted on save.
// So `field=` genuinely DOES survive an editor load/save round trip today; the grammar was never the
// gap. The REAL gap is narrower and still true: this editor has no property/inspector panel for ANY
// node (grep sandbox/src for ImGui::Input*/Drag*/Combo* -- zero matches), so extraTokens round-trips
// opaquely but is not READABLE or EDITABLE from the GUI -- a GetField/SetField/GetFieldVec3/
// SetFieldVec3 node's `field=` can be carried through the editor but not authored or inspected by it.
// That is a property-panel gap, not a file-format gap; still flagging it for the human, corrected.
#include <string>
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

// One entry in the node palette / spawn table.
struct GraphNodeDesc {
    std::string typeId;                // matches OcGraphNode::type; looked up case-insensitively
    std::string displayName;           // node header / palette label
    std::string category;              // palette grouping
    std::vector<GraphPinSpec> pins;    // inputs and outputs mixed; isOutput distinguishes which
};

namespace detail {

inline GraphPinSpec pin(std::string name, std::string type, bool isOutput, std::string def = {}) {
    return GraphPinSpec{std::move(name), std::move(type), isOutput, std::move(def)};
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
    t.push_back({"Multiply", "Multiply", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Subtract", "Subtract", "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"Divide",   "Divide",   "Math", {pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    // -- trig: a in; result out (both concurrent-workflow additions, one line each) --
    t.push_back({"Sin", "Sin", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"Cos", "Cos", "Math", {pin("a", "float", false), pin("result", "float", true)}});
    // -- logic --
    t.push_back({"Compare", "Compare", "Logic", {pin("a", "float", false), pin("b", "float", false), pin("result", "bool", true)}});
    // -- scene field access: entity id in, value in/out. See the KNOWN GAP note above re: field=. --
    t.push_back({"GetField", "Get Field", "Scene", {pin("entity", "int", false), pin("value", "float", true)}});
    t.push_back({"SetField", "Set Field", "Scene", {pin("entity", "int", false), pin("value", "float", false), pin("success", "bool", true)}});
    // -- GetField/SetField's Vec3 siblings: a Vec3-kind scene field (CLocal.position, CLight.colour,
    //    ...) as three ordinary float pins rather than one new pin TYPE -- see
    //    scripting/csharp/Aver.Graph/GraphCompiler.cs's FieldKindVec3/RequireVec3Field comments for why
    //    that shape was chosen over adding a "vec3" pin type. Pin sets copied field for field from
    //    OcGraphParser.AddDefaultPins's "getfieldvec3"/"setfieldvec3" cases, same as GetField/SetField
    //    above. No exec pins on either by default (mirrors GetField/SetField exactly).
    t.push_back({"GetFieldVec3", "Get Field (Vec3)", "Scene", {
        pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"SetFieldVec3", "Set Field (Vec3)", "Scene", {
        pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("success", "bool", true)}});
    // -- graph parameter read, another concurrent-workflow addition; type defaults to float, the
    //    common case, and can be edited per-instance like any other pin since layout/pin-typing
    //    always prefers the node's own recorded pins over this table (see the header comment). --
    t.push_back({"Param", "Param", "Param", {pin("value", "float", true)}});

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
    t.push_back({"OnStart", "On Start", "Flow", {pin("exec", "exec", true)}});
    t.push_back({"OnTick", "On Tick", "Flow", {pin("exec", "exec", true)}});
    // onhit: same bare-trigger shape as onstart/ontick above -- a labelled starting point an ENTRY
    //    record points at, nothing more. What makes it fire ON DEMAND (a host calling
    //    Aver.Graph.GraphHost.Fire, rather than the fixed per-frame Tick() cadence OnStart/OnTick get)
    //    is entirely a scripting/csharp/Aver.Graph/GraphHost.cs concept -- this editor, like the C#
    //    parser's own AddDefaultPins, treats "OnHit" as nothing more than one more ENTRY event name; a
    //    project inventing a different one needs no new catalog entry to place ITS trigger node, only
    //    a differently-named NODE of type OnStart/OnTick/OnHit (any bare-trigger type already
    //    suffices) and its own ENTRY record naming the event.
    t.push_back({"OnHit", "On Hit", "Flow", {pin("exec", "exec", true)}});

    // Spawn: SIDE-EFFECTING (creates a new scene entity), so -- unlike GetField/SetField/GetFieldVec3/
    //    SetFieldVec3 above -- it gets exec pins by default, mirroring Raycast's own reasoning. See
    //    scripting/csharp/Aver.Graph/GraphCompiler.cs's IsExecCapableSpawnType comment for why it is
    //    refused by the pure-dataflow (PULL) compiler even more strictly than SetField is. class= names
    //    which registered class to spawn -- the same generic key=value NODE-line attribute field=/
    //    param= already use; this table has no property panel to author it from either, same KNOWN GAP
    //    as field= above.
    t.push_back({"Spawn", "Spawn", "Actor", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("entity", "int", true)}});
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
