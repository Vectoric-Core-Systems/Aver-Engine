#pragma once
// Palette entries for the scene query nodes. NOT standalone: it is included by the generated GraphNodeFamilies.gen.inc inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphSceneQueryNodeTable.cs pin for pin (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2).
// Class and component names are attributes. Only AN_AddComponent is Exec; the rest work under either compiler.

inline void appendSceneQueryNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_CountActorsWithTag", "Count Actors With Tag", "Scene", {
        pin("mask", "int", false), pin("count", "int", true)}});
    t.push_back({"AN_GetActorWithTagAt", "Get Actor With Tag At", "Scene", {
        pin("mask", "int", false), pin("index", "int", false), pin("entity", "int", true)}});
    t.push_back({"AN_CountActorsOfClass", "Count Actors Of Class", "Scene", {
        pin("count", "int", true)},
        {attr("class", "Class")}});
    t.push_back({"AN_GetActorOfClassAt", "Get Actor Of Class At", "Scene", {
        pin("index", "int", false), pin("entity", "int", true)},
        {attr("class", "Class")}});
    t.push_back({"AN_GetChildCount", "Get Child Count", "Scene", {
        pin("entity", "int", false), pin("count", "int", true)}});
    t.push_back({"AN_GetChildAt", "Get Child At", "Scene", {
        pin("entity", "int", false), pin("index", "int", false), pin("child", "int", true)}});
    t.push_back({"AN_GetParent", "Get Parent", "Scene", {
        pin("entity", "int", false), pin("parent", "int", true)}});
    t.push_back({"AN_HasComponent", "Has Component", "Scene", {
        pin("entity", "int", false), pin("has", "bool", true)},
        {attr("component", "Component (for example CLight)")}});
    t.push_back({"AN_AddComponent", "Add Component", "Scene", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("component", "Component (for example CLight)")}});
    t.push_back({"AN_GetDistance", "Get Distance", "Scene", {
        pin("a", "int", false), pin("b", "int", false), pin("distance", "float", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetRagdollState", "Get Ragdoll State", "Scene", {
        pin("entity", "int", false), pin("mode", "int", true), pin("has", "bool", true)}});
    t.push_back({"AN_GetActorCount", "Get Actor Count", "Scene", {
        pin("count", "int", true)}});
}
