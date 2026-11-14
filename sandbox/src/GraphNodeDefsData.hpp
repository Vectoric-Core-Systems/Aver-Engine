#pragma once
// Palette entries for the float3 (vector), string and entity pin nodes. NOT standalone: GraphNodeFamilies.gen.inc includes it from
// GraphNodeDefs.hpp inside namespace aver::editor::detail, after pin()/attr() are defined, and calls appendDataNodeDefs(t).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphDataNodes.cs pin for pin; the ids are the .ocgraph node types verbatim. A float3
// pin's literal is `x,y,z`, a string's is %-escaped (fmt::encodeGraphString), an entity's is an integer. An entity pin links to
// and from an int pin. The older VecAdd-style nodes with loose x/y/z pins are unchanged.

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
}
