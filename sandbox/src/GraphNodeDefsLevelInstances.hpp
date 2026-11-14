#pragma once
// Palette entries for the level-instance nodes. NOT standalone: include it from GraphNodeDefsFamilies.cpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendLevelInstanceNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphLevelInstanceNodeTable.cs pin for pin
// (docs/plans/wave3/LEVEL_INSTANCES.md section 3). The level path and the child id path are attributes.
// Exec nodes take part in the PUSH compiler only; the pure reads work under either.

inline void appendLevelInstanceNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_SpawnLevelInstance", "Spawn Level Instance", "Level Instance", {
        pin("exec", "exec", false), pin("parent", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("yaw", "float", false), pin("scale", "float", false, "1"), pin("packed", "bool", false),
        pin("then", "exec", true), pin("entity", "int", true)},
        {attr("level", "Level")}});
    t.push_back({"AN_DestroyLevelInstance", "Destroy Level Instance", "Level Instance", {
        pin("exec", "exec", false), pin("root", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetLevelInstanceRoot", "Get Level Instance Root", "Level Instance", {
        pin("entity", "int", false), pin("root", "int", true)}});
    t.push_back({"AN_FindLevelInstanceEntity", "Find Level Instance Entity", "Level Instance", {
        pin("root", "int", false), pin("entity", "int", true)},
        {attr("path", "Id path (hex, / between levels)")}});
    t.push_back({"AN_SetLevelInstanceLoaded", "Set Level Instance Loaded", "Level Instance", {
        pin("exec", "exec", false), pin("root", "int", false), pin("loaded", "bool", false, "true"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_IsLevelInstanceLoaded", "Is Level Instance Loaded", "Level Instance", {
        pin("root", "int", false), pin("loaded", "bool", true)}});
    t.push_back({"AN_ReloadLevelInstance", "Reload Level Instance", "Level Instance", {
        pin("exec", "exec", false), pin("root", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetLevelInstancePacked", "Set Level Instance Packed", "Level Instance", {
        pin("exec", "exec", false), pin("root", "int", false), pin("packed", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetLevelInstanceBounds", "Get Level Instance Bounds", "Level Instance", {
        pin("root", "int", false),
        pin("minX", "float", true), pin("minY", "float", true), pin("minZ", "float", true), pin("maxX", "float", true), pin("maxY", "float", true), pin("maxZ", "float", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetLevelInstanceCount", "Get Level Instance Count", "Level Instance", {
        pin("count", "int", true)}});
    t.push_back({"AN_GetLevelInstanceAt", "Get Level Instance At", "Level Instance", {
        pin("index", "int", false), pin("root", "int", true)}});
}
