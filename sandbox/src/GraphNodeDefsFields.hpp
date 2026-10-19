#pragma once
// Palette entries for the Fields nodes (soft-body fields). NOT standalone: GraphNodeFamilies.gen.inc includes it from
// GraphNodeDefs.hpp inside namespace aver::editor::detail, after pin()/attr() are defined, and calls appendFieldsNodeDefs(t).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphFieldsNodeTable.cs pin for pin (docs/plans/wave10/SOFTBODY_DEPTH.md 2.13). The ids
// carry the AN_ prefix so pulse, impact and start stay free for the plain verbs. Enum values: kind 0 radial, 1 directional,
// 2 damage, 3 release anchors, 4 kill, 5 sleep; shape 0 sphere, 1 box, 2 capsule, 3 everywhere; targets a mask (1 cloth,
// 2 destructible, 4 debris, 7 all).

inline void appendFieldsNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_SoftFieldPulse", "Soft Field Pulse", "Physics", {
        pin("exec", "exec", false), pin("kind", "int", false), pin("shape", "int", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("ex", "float", false), pin("ey", "float", false), pin("ez", "float", false),
        pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false),
        pin("strength", "float", false), pin("energy", "float", false), pin("falloff", "float", false, "1"), pin("targets", "int", false, "7"),
        pin("then", "exec", true), pin("reached", "int", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoftFieldImpact", "Soft Field Impact", "Physics", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false),
        pin("radius", "float", false, "40"), pin("energy", "float", false, "500"), pin("kick", "float", false, "0"),
        pin("then", "exec", true), pin("reached", "int", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoftFieldStart", "Start Soft Field", "Physics", {
        pin("exec", "exec", false), pin("kind", "int", false), pin("shape", "int", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("ex", "float", false), pin("ey", "float", false), pin("ez", "float", false),
        pin("dirX", "float", false), pin("dirY", "float", false), pin("dirZ", "float", false),
        pin("strength", "float", false), pin("energy", "float", false), pin("falloff", "float", false, "1"), pin("targets", "int", false, "7"),
        pin("lifetime", "float", false, "5"),
        pin("then", "exec", true), pin("handle", "int", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoftFieldStop", "Stop Soft Field", "Physics", {
        pin("exec", "exec", false), pin("handle", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SpawnSoftField", "Spawn Soft Field", "Physics", {
        pin("exec", "exec", false), pin("kind", "int", false), pin("shape", "int", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("ex", "float", false), pin("ey", "float", false), pin("ez", "float", false),
        pin("strength", "float", false), pin("energy", "float", false), pin("falloff", "float", false, "1"), pin("lifetime", "float", false, "5"),
        pin("then", "exec", true), pin("entity", "int", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoftFieldSetEnabled", "Set Soft Field Enabled", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("enabled", "bool", false, "true"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoftFieldSetStrength", "Set Soft Field Strength", "Physics", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("strength", "float", false), pin("energy", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetSoftFieldTouched", "Get Soft Field Touched", "Physics", {
        pin("entity", "int", false), pin("found", "bool", true), pin("touched", "int", true)}});
}
