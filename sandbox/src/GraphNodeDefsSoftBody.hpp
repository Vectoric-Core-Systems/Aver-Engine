#pragma once
// Palette entries for the cloth and destruction nodes. NOT standalone: include it from GraphNodeDefs.hpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendSoftBodyNodeDefs(t) just before buildCatalog returns (after appendMotionMatchNodeDefs).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs pin for pin (docs/SOFTBODY.md). A soft body's
// events (SoftBodyTorn, SoftBodyDetached, SoftBodyDust, SoftBodyImpact, SoftBodyRepaired) reach a graph through OnEvent.

inline void appendSoftBodyNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"SoftBodyDamage", "Soft Body Damage", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("radius", "float", false, "30"), pin("energy", "float", false, "100"),
        pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SoftBodyExplode", "Soft Body Explode", "Physics", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("radius", "float", false, "300"), pin("impulse", "float", false, "600"), pin("energy", "float", false, "500"),
        pin("then", "exec", true), pin("touched", "int", true), pin("success", "bool", true)}});
    t.push_back({"SoftBodyTear", "Soft Body Tear", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("radius", "float", false, "20"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SoftBodyRepair", "Soft Body Repair", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"ReleaseSoftBody", "Release Soft Body", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSoftBodyWind", "Set Soft Body Wind", "Physics", {
        pin("exec", "exec", false), pin("vx", "float", false), pin("vy", "float", false), pin("vz", "float", false),
        pin("gust", "float", false, "0"), pin("gustHz", "float", false, "0.3"), pin("turbulence", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSoftBodyPreset", "Set Soft Body Preset", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("preset", "int", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetSoftBodyState", "Get Soft Body State", "Physics", {
        pin("entity", "int", false), pin("found", "bool", true), pin("damage", "float", true), pin("maxStrain", "float", true),
        pin("pieces", "int", true), pin("brokenEdges", "int", true)}});
}
