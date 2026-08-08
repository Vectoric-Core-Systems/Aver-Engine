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
// expects (its EmitAdd/EmitSin/etc. methods, and its `switch (node.Type.ToLowerInvariant())` node-type
// list) as read on 2026-08-08. That file is owned by a concurrent workflow and is NOT included or
// generated from here -- this table owns its own copy of the vocabulary so a C++ editor build never
// depends on a C# file. If GraphCompiler.cs's node set has moved since, update this table to match;
// it is one line per node type by design.
//
// KNOWN GAP, not fixed here: GetField/SetField address a scene field by name via a `field=` NODE-line
// attribute on the C# side's own reader (scripting/csharp/Aver.Graph/OcGraphParser.cs, "field=" case).
// The shared C++ .ocgraph grammar (modules/formats/src/OcGraph.cpp, NODE line) has no key=value
// attribute syntax at all -- only `NODE id type x y` -- so there is currently no way for THIS editor to
// read, show, or write which field a GetField/SetField node addresses. That is a file-format gap, not
// an editor bug, and is out of scope for this geometry core; flagging it for the human.
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
    // -- graph parameter read, another concurrent-workflow addition; type defaults to float, the
    //    common case, and can be edited per-instance like any other pin since layout/pin-typing
    //    always prefers the node's own recorded pins over this table (see the header comment). --
    t.push_back({"Param", "Param", "Param", {pin("value", "float", true)}});
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
