#pragma once
// Palette entries for the sky clouds and fluid volume nodes. NOT standalone: it is included by the generated GraphNodeFamilies.gen.inc inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphWorldNodeTable.cs pin for pin (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2).
// All three are Exec and return success; a fluid volume is queued, so success means a provider accepted it.

inline void appendWorldNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_SetSkyClouds", "Set Sky Clouds", "World", {
        pin("exec", "exec", false), pin("seed", "int", false), pin("coverage", "float", false, "0.45"), pin("density", "float", false, "1"), pin("bottomCm", "float", false, "150000"), pin("topCm", "float", false, "280000"),
        pin("featureScale", "float", false, "0.00002"), pin("windX", "float", false, "900"), pin("windY", "float", false, "260"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_ClearSkyClouds", "Clear Sky Clouds", "World", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SpawnFluidVolume", "Spawn Fluid Volume", "World", {
        pin("exec", "exec", false), pin("cx", "float", false), pin("cy", "float", false), pin("cz", "float", false), pin("hx", "float", false), pin("hy", "float", false),
        pin("hz", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("name", "Name (optional)")}});
}
