#pragma once
// Palette entries for the data layer nodes. NOT standalone: included from GraphNodeDefs.hpp (via GraphNodeFamilies.gen.inc) inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphDataLayerNodeTable.cs pin for pin (docs/LEVEL_STREAMING.md). The layer is the node's layer= attribute.
// State is 0 unloaded, 1 loaded, 2 activated; a read of an unknown layer is -1. Set only changes layers declared `runtime=1`.

inline void appendDataLayerNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_GetDataLayerState", "Get Data Layer State", "World", {
        pin("state", "int", true)},
        {attr("layer", "Layer")}});
    t.push_back({"AN_SetDataLayerState", "Set Data Layer State", "World", {
        pin("exec", "exec", false), pin("state", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("layer", "Layer")}});
}
