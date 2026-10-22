#pragma once
// Palette entries for the cloth and destruction nodes. NOT standalone: include it from GraphNodeDefs.hpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendSoftBodyNodeDefs(t) just before buildCatalog returns (after appendMotionMatchNodeDefs).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs pin for pin (docs/SOFTBODY.md). A soft body's
// events (SoftBodyTorn, SoftBodyDetached, SoftBodyDust, SoftBodyImpact, SoftBodyRepaired) reach a graph through OnEvent; Pop Soft Body Event
// gives their position and value.

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
    // Version 2 (wave 10): seven rows appended; the eight above and their pins do not change.
    t.push_back({"PopSoftBodyEvent", "Pop Soft Body Event", "Physics", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("found", "bool", true), pin("kind", "int", true), pin("entity", "int", true),
        pin("piece", "int", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("value", "float", true),
        pin("success", "bool", true)}});
    t.push_back({"SetSoftBodyBudget", "Set Soft Body Budget", "Physics", {
        pin("exec", "exec", false), pin("maxPieces", "int", false, "48"), pin("maxParticles", "int", false, "20000"),
        pin("maxRebuilds", "int", false, "2"), pin("lifetime", "float", false, "12"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"ClearSoftBodyDebris", "Clear Soft Body Debris", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSoftBodyAsleep", "Set Soft Body Asleep", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("asleep", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSoftBodyEffects", "Set Soft Body Effects", "Physics", {
        pin("exec", "exec", false), pin("enabled", "bool", false, "true"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"RepairAllSoftBodies", "Repair All Soft Bodies", "Physics", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("repaired", "int", true), pin("success", "bool", true)}});
    t.push_back({"GetSoftBodyDents", "Get Soft Body Dents", "Physics", {
        pin("entity", "int", false), pin("found", "bool", true), pin("dentedEdges", "int", true), pin("maxSetCm", "float", true),
        pin("debrisPieces", "int", true)}});
    // Version 3 (wave 11): ten rows appended; the fifteen above and their pins do not change. A grab id is the vertex index + 1 (0 = nothing in reach).
    t.push_back({"SetSoftPressure", "Set Soft Pressure", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("kPa", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetSoftVolume", "Get Soft Volume", "Physics", {
        pin("entity", "int", false), pin("found", "bool", true), pin("restCm3", "float", true), pin("nowCm3", "float", true),
        pin("ratio", "float", true)}});
    t.push_back({"GrabSoftBody", "Grab Soft Body", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("reach", "float", false, "30"), pin("targetX", "float", false), pin("targetY", "float", false), pin("targetZ", "float", false),
        pin("then", "exec", true), pin("grab", "int", true), pin("success", "bool", true)}});
    t.push_back({"MoveSoftGrab", "Move Soft Grab", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("grab", "int", false), pin("x", "float", false), pin("y", "float", false),
        pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"ReleaseSoftGrab", "Release Soft Grab", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("grab", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSoftBuoyancy", "Set Soft Buoyancy", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("density", "float", false, "1000"), pin("drag", "float", false, "1"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"GetSoftSubmerged", "Get Soft Submerged", "Physics", {
        pin("entity", "int", false), pin("found", "bool", true), pin("fraction", "float", true), pin("volumeCm3", "float", true)}});
    t.push_back({"SetSoftContact", "Set Soft Contact", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("softScale", "float", false, "1"), pin("otherScale", "float", false, "1"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSoftPairMode", "Set Soft Pair Mode", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("other", "int", false), pin("mode", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AddSoftJelly", "Add Soft Jelly", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("preset", "int", false, "5"),
        pin("then", "exec", true), pin("success", "bool", true)}});
}
