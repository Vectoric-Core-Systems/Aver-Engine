#pragma once
// Palette entries for the floating-origin nodes. NOT standalone: included from GraphNodeDefs.hpp (via GraphNodeFamilies.gen.inc) inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphWorldOriginNodeTable.cs pin for pin (docs/plans/wave11/LARGE_WORLD.md 2.5). A graph float is a single,
// so the nodes carry whole regions (integers; one region is 16.384 km) plus a position relative to that region's centre in centimetres, never a
// bare absolute. Get World Origin and Get World Region are pure reads; Set World Region is an exec node.

inline void appendWorldOriginNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_GetWorldOrigin", "Get World Origin", "World", {
        pin("regionX", "int", true), pin("regionY", "int", true), pin("generation", "int", true), pin("enabled", "bool", true)}});
    t.push_back({"AN_GetWorldRegion", "Get World Region", "World", {
        pin("entity", "int", false),
        pin("regionX", "int", true), pin("regionY", "int", true), pin("regionZ", "int", true),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_SetWorldRegion", "Set World Region", "World", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("regionX", "int", false), pin("regionY", "int", false), pin("regionZ", "int", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
}
