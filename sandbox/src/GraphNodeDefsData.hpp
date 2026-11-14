#pragma once
// Palette entries for the float3 (vector), string and entity pin nodes. NOT standalone: GraphNodeFamilies.gen.inc includes it from
// GraphNodeDefs.hpp inside namespace aver::editor::detail, after pin()/attr() are defined, and calls appendDataNodeDefs(t).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphDataNodes.cs pin for pin; the ids are the .ocgraph node types verbatim. A float3
// pin's literal is `x,y,z`, a string's is %-escaped (fmt::encodeGraphString), an entity's is an integer. An entity pin links to
// and from an int pin. The older VecAdd-style nodes with loose x/y/z pins are unchanged.

// A palette variant of every element-typed entry for each other element type, named "<TypeId>@<elem>" with of=<elem> preset.
// Built here rather than listed, so a new array node is one row above and the six element types stay in step.
inline void appendElementVariants(std::vector<GraphNodeDesc>& t) {
    static const char* const kElems[] = {"int", "bool", "float3", "string", "entity"};
    const usize count = t.size();
    for (usize i = 0; i < count; ++i) {
        if (t[i].derivation != GraphPinDerivation::Element) continue;
        const GraphNodeDesc base = t[i];
        for (const char* elem : kElems) {
            GraphNodeDesc d = base;
            d.typeId = base.typeId + "@" + elem;
            d.nodeType = base.typeId;
            d.presets = {{"of", elem}};
            const std::string pretty = std::string(elem) == "float3" ? "Vector" : std::string(1, static_cast<char>(elem[0] - 'a' + 'A')) + (elem + 1);
            d.displayName = base.displayName + " (" + pretty + ")";
            for (GraphPinSpec& p : d.pins) {
                if (p.type == "float") p.type = elem;
                else if (p.type == "float[]") p.type = std::string(elem) + "[]";
            }
            t.push_back(std::move(d));
        }
    }
}

inline void appendDataNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"ConstVector", "Const Vector", "Const", {pin("value", "float3", true, "0,0,0")}, {attr("value", "Value")}});
    t.push_back({"ConstString", "Const String", "Const", {pin("value", "string", true)}, {attr("value", "Value")}});

    t.push_back({"MakeVector", "Make Vector", "Vector", {
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("v", "float3", true)}});
    t.push_back({"BreakVector", "Break Vector", "Vector", {
        pin("v", "float3", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"VectorAdd", "Vector Add", "Vector", {
        pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}});
    t.push_back({"VectorSubtract", "Vector Subtract", "Vector", {
        pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}});
    t.push_back({"VectorScale", "Vector Scale", "Vector", {
        pin("v", "float3", false), pin("s", "float", false, "1"), pin("result", "float3", true)}});
    t.push_back({"VectorDot", "Vector Dot", "Vector", {
        pin("a", "float3", false), pin("b", "float3", false), pin("result", "float", true)}});
    t.push_back({"VectorCross", "Vector Cross", "Vector", {
        pin("a", "float3", false), pin("b", "float3", false), pin("result", "float3", true)}});
    t.push_back({"VectorLength", "Vector Length", "Vector", {
        pin("v", "float3", false), pin("result", "float", true)}});
    t.push_back({"VectorNormalize", "Vector Normalize", "Vector", {
        pin("v", "float3", false), pin("result", "float3", true)}});
    t.push_back({"VectorLerp", "Vector Lerp", "Vector", {
        pin("a", "float3", false), pin("b", "float3", false), pin("t", "float", false), pin("result", "float3", true)}});
    t.push_back({"VectorDistance", "Vector Distance", "Vector", {
        pin("a", "float3", false), pin("b", "float3", false), pin("result", "float", true)}});

    t.push_back({"StringAppend", "String Append", "String", {
        pin("a", "string", false), pin("b", "string", false), pin("result", "string", true)}});
    t.push_back({"FloatToString", "Float To String", "String", {
        pin("a", "float", false), pin("result", "string", true)}});
    t.push_back({"IntToString", "Int To String", "String", {
        pin("a", "int", false), pin("result", "string", true)}});
    t.push_back({"BoolToString", "Bool To String", "String", {
        pin("a", "bool", false), pin("result", "string", true)}});
    t.push_back({"VectorToString", "Vector To String", "String", {
        pin("v", "float3", false), pin("result", "string", true)}});
    t.push_back({"StringEquals", "String Equals", "String", {
        pin("a", "string", false), pin("b", "string", false), pin("result", "bool", true)}});
    t.push_back({"StringLength", "String Length", "String", {
        pin("s", "string", false), pin("result", "int", true)}});
    t.push_back({"PrintText", "Print Text", "Debug", {
        pin("exec", "exec", false), pin("text", "string", false), pin("then", "exec", true)}});

    t.push_back({"IsValidEntity", "Is Valid Entity", "Scene", {
        pin("e", "entity", false), pin("result", "bool", true)}});
    t.push_back({"GetEntityLocation", "Get Entity Location", "Scene", {
        pin("e", "entity", false), pin("location", "float3", true)}});
    t.push_back({"SetEntityLocation", "Set Entity Location", "Scene", {
        pin("exec", "exec", false), pin("e", "entity", false), pin("location", "float3", false), pin("then", "exec", true)}});
    t.push_back({"RaycastVector", "Raycast Vector", "Scene", {
        pin("exec", "exec", false), pin("origin", "float3", false), pin("direction", "float3", false),
        pin("maxDist", "float", false, "10000"), pin("then", "exec", true),
        pin("hit", "bool", true), pin("entity", "entity", true), pin("point", "float3", true)}});

    // ---- arrays: element-typed. Written for float; GraphPinDerivation::Element retypes the float pins to a node's of=
    //      element, and appendElementVariants above adds the other element types as palette variants (Array Get (Vector), ...).
    //      MIRRORS GraphCollectionNodes.cs. The Exec ones name a VARIABLE array with var=.
    constexpr GraphNodeDomain kG = kDomainGameplay;
    constexpr GraphPinDerivation kElem = GraphPinDerivation::Element;
    t.push_back({"MakeArray", "Make Array", "Array", {
        pin("item0", "float", false), pin("item1", "float", false), pin("item2", "float", false), pin("item3", "float", false),
        pin("array", "float[]", true)}, {attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayLength", "Array Length", "Array", {
        pin("array", "float[]", false), pin("length", "int", true)}, {attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayGet", "Array Get", "Array", {
        pin("array", "float[]", false), pin("index", "int", false), pin("item", "float", true)}, {attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayContains", "Array Contains", "Array", {
        pin("array", "float[]", false), pin("item", "float", false), pin("result", "bool", true)}, {attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayFind", "Array Find", "Array", {
        pin("array", "float[]", false), pin("item", "float", false), pin("index", "int", true)}, {attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayAdd", "Array Add", "Array", {
        pin("exec", "exec", false), pin("item", "float", false), pin("then", "exec", true)},
        {attr("var", "Array variable"), attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArraySet", "Array Set", "Array", {
        pin("exec", "exec", false), pin("index", "int", false), pin("item", "float", false), pin("then", "exec", true)},
        {attr("var", "Array variable"), attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayRemoveAt", "Array Remove At", "Array", {
        pin("exec", "exec", false), pin("index", "int", false), pin("then", "exec", true)},
        {attr("var", "Array variable"), attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ArrayClear", "Array Clear", "Array", {
        pin("exec", "exec", false), pin("then", "exec", true)},
        {attr("var", "Array variable"), attr("of", "Element type")}, kG, "", {}, kElem});
    t.push_back({"ForEachInArray", "For Each In Array", "Array", {
        pin("exec", "exec", false), pin("array", "float[]", false), pin("loop", "exec", true), pin("element", "float", true),
        pin("index", "int", true), pin("completed", "exec", true)}, {attr("of", "Element type")}, kG, "", {}, kElem});
    appendElementVariants(t);

    // ---- structs: the pins come from the STRUCT named by struct= (and field=), so these rows carry none. The palette offers
    //      per-struct rows instead (structPaletteEntries, GraphCollectionNodes.hpp); these exist so a placed node has a name, a
    //      colour and its attributes. Category "Struct" is therefore left out of the generic palette menus.
    constexpr GraphPinDerivation kStruct = GraphPinDerivation::Struct;
    t.push_back({"MakeStruct", "Make Struct", "Struct", {}, {attr("struct", "Struct")}, kG, "", {}, kStruct});
    t.push_back({"BreakStruct", "Break Struct", "Struct", {}, {attr("struct", "Struct")}, kG, "", {}, kStruct});
    t.push_back({"SetStructField", "Set Struct Field", "Struct", {}, {attr("struct", "Struct"), attr("field", "Field")}, kG, "", {}, kStruct});
}
