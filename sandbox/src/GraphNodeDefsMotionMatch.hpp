#pragma once
// Palette entries for the motion-matching nodes. NOT standalone: include it from GraphNodeDefsFamilies.cpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendMotionMatchNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphMotionMatchNodeTable.cs pin for pin
// (docs/plans/wave5/MOTION_MATCHING.md section 11). Database, tag lists and clip are attributes, never pins.
// A motion-matching state itself needs no node: it is a state in the .ocasm.

inline void appendMotionMatchNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"SetMotionMatching", "Set Motion Matching", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("facing", "int", false, "0"), pin("drive", "int", false, "0"), pin("playRate", "float", false, "1"),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("database", "Database (empty = from a state)")}});
    t.push_back({"SetMotionInput", "Set Motion Input", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("velocityX", "float", false), pin("velocityY", "float", false), pin("facingYaw", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetMotionTags", "Set Motion Tags", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("required", "Required tags (comma-separated)"), attr("excluded", "Excluded tags (comma-separated)")}});
    t.push_back({"RequestMotionSearch", "Request Motion Search", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetMotionState", "Get Motion State", "Animation", {
        pin("entity", "int", false), pin("found", "bool", true), pin("clipTime", "float", true), pin("cost", "float", true), pin("switches", "int", true)}});
    t.push_back({"IsMotionClip", "Is Motion Clip", "Animation", {
        pin("entity", "int", false), pin("result", "bool", true)},
        {attr("clip", "Clip")}});
}
