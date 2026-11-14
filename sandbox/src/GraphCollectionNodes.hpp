#pragma once
// The array and struct pin types as the editor sees them: type-string helpers, and the pins of the nodes whose shape follows an
// attribute (`of=` the element type, `struct=` and `field=` a declared STRUCT) instead of being fixed. Header-only and headless.
// MIRRORS scripting/csharp/Aver.Graph/GraphCollectionNodes.cs: the same pins in the same order.
#include "GraphEditorGeometry.hpp"
#include "GraphNodeDefs.hpp"
#include "aver/formats/OcGraph.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace aver::editor {

inline std::string lowerTypeName(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// float, int, bool, float3, string or entity (any case): what an array holds and a struct field has.
inline bool isScalarTypeName(const std::string& type) {
    const std::string t = lowerTypeName(type);
    return t == "float" || t == "int" || t == "bool" || t == "float3" || t == "string" || t == "entity";
}

inline bool isArrayTypeName(const std::string& type) {
    return type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0 && isScalarTypeName(type.substr(0, type.size() - 2));
}

inline bool isStructTypeName(const std::string& type) {
    return type.size() > 7 && lowerTypeName(type.substr(0, 7)) == "struct:";
}

inline std::string structNameOfType(const std::string& type) { return isStructTypeName(type) ? type.substr(7) : std::string(); }

inline std::string arrayTypeOf(const std::string& elem) { return elem + "[]"; }

// "Vector" for float3, the capitalised name otherwise: the suffix of a palette variant ("Array Get (Vector)").
inline std::string elementFriendlyName(const std::string& elem) {
    if (lowerTypeName(elem) == "float3") return "Vector";
    std::string s = elem;
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// A type a VAR, a function pin or a struct field may have in a graph declaring `structs`: a scalar, an array of one, or a
// declared struct (structs == nullptr accepts any struct:Name). Struct fields are scalars only; see isScalarTypeName.
inline bool isDataTypeName(const std::string& type, const std::vector<fmt::OcGraphStruct>* structs) {
    if (isScalarTypeName(type) || isArrayTypeName(type)) return true;
    if (!isStructTypeName(type)) return false;
    if (!structs) return true;
    const std::string name = structNameOfType(type);
    return std::any_of(structs->begin(), structs->end(), [&](const fmt::OcGraphStruct& s) { return s.name == name; });
}

// Every type a Variables or Functions combo offers: the scalars, their arrays, then one struct:Name per declared struct.
inline std::vector<std::string> dataTypeChoices(const std::vector<fmt::OcGraphStruct>& structs) {
    std::vector<std::string> out = {"float", "int", "bool", "float3", "string", "entity"};
    const usize scalars = out.size();
    for (usize i = 0; i < scalars; ++i) out.push_back(arrayTypeOf(out[i]));
    for (const auto& s : structs) out.push_back("struct:" + s.name);
    return out;
}

// The pins of an element-typed entry, written for float, with the element pins retyped to `elem`.
inline std::vector<GraphPinSpec> retypeElementPins(std::vector<GraphPinSpec> pins, const std::string& elem) {
    for (GraphPinSpec& p : pins) {
        if (p.type == "float") p.type = elem;
        else if (p.type == "float[]") p.type = arrayTypeOf(elem);
    }
    return pins;
}

inline std::string nodeAttrValue(const fmt::OcGraphNode& n, const char* key) {
    const GraphNodeAttribute a = getNodeAttribute(n, key);
    return a.found ? a.value : std::string();
}

// The element type an array node works on: its of= attribute, float when absent.
inline std::string elementOf(const fmt::OcGraphNode& n) {
    const std::string of = nodeAttrValue(n, "of");
    return of.empty() ? std::string("float") : of;
}

// The pins of MakeStruct, BreakStruct or SetStructField on `def`; `field` is SetStructField's field= (empty gives no pins
// for it when the struct has no such field).
inline std::vector<GraphPinSpec> structNodePins(const std::string& nodeType, const fmt::OcGraphStruct& def, const std::string& field) {
    std::vector<GraphPinSpec> out;
    const std::string st = "struct:" + def.name;
    if (nodeType == "MakeStruct") {
        for (const auto& f : def.fields) out.push_back({f.name, f.type, false, {}});
        out.push_back({"value", st, true, {}});
    } else if (nodeType == "BreakStruct") {
        out.push_back({"value", st, false, {}});
        for (const auto& f : def.fields) out.push_back({f.name, f.type, true, {}});
    } else if (nodeType == "SetStructField") {
        const auto it = std::find_if(def.fields.begin(), def.fields.end(), [&](const fmt::OcGraphStructField& f) { return f.name == field; });
        if (it == def.fields.end()) return out;
        out.push_back({"value", st, false, {}});
        out.push_back({it->name, it->type, false, {}});
        out.push_back({"result", st, true, {}});
    }
    return out;
}

// The pins a node of this catalog entry's kind has now: the entry's own, retyped by of= or rebuilt from its STRUCT. Empty when
// the node names a struct (or field) this graph does not declare; callers then leave the node as written.
inline std::vector<GraphPinSpec> derivedNodePins(const fmt::OcGraphNode& n, const GraphNodeDesc& desc,
                                                  const std::vector<fmt::OcGraphStruct>& structs) {
    if (desc.derivation == GraphPinDerivation::Element) return retypeElementPins(desc.pins, elementOf(n));
    if (desc.derivation == GraphPinDerivation::Struct) {
        const std::string name = nodeAttrValue(n, "struct");
        for (const auto& s : structs)
            if (s.name == name) return structNodePins(n.type, s, nodeAttrValue(n, "field"));
        return {};
    }
    return desc.pins;
}

// The palette rows for the declared structs: Make and Break, and a Set row per field. Their typeIds carry an '@' (never a node
// type), and nodeType plus presets say what a click writes.
inline std::vector<GraphNodeDesc> structPaletteEntries(const std::vector<fmt::OcGraphStruct>& structs) {
    std::vector<GraphNodeDesc> out;
    const auto entry = [&](const std::string& type, const std::string& id, const std::string& display, const fmt::OcGraphStruct& s,
                            const std::string& field) {
        GraphNodeDesc d;
        d.typeId = type + "@" + id;
        d.displayName = display;
        d.category = "Struct";
        d.pins = structNodePins(type, s, field);
        d.nodeType = type;
        d.presets = {{"struct", s.name}};
        if (!field.empty()) d.presets.push_back({"field", field});
        d.derivation = GraphPinDerivation::Struct;
        out.push_back(std::move(d));
    };
    for (const auto& s : structs) {
        entry("MakeStruct", s.name, "Make " + s.name, s, {});
        entry("BreakStruct", s.name, "Break " + s.name, s, {});
        for (const auto& f : s.fields) entry("SetStructField", s.name + "." + f.name, "Set " + s.name + "." + f.name, s, f.name);
    }
    return out;
}

} // namespace aver::editor
