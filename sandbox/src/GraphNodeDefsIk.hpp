#pragma once
// Palette entries for the IK and rig-input nodes. NOT standalone: include it from GraphNodeDefsFamilies.cpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendIkNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphIkNodeTable.cs pin for pin
// (docs/plans/wave5/IK_RETARGETING.md section 9). Rig, goal, foot and input names are attributes, never
// pins; an empty goal, foot or input means "all". Exec nodes take part in the PUSH compiler only; the
// pure reads work under either.

inline void appendIkNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"SetIkRig", "Set IK Rig", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("weight", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("rig", "IK rig")}});
    t.push_back({"SetIkGoal", "Set IK Goal", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("useRotation", "bool", false), pin("pitch", "float", false), pin("yaw", "float", false), pin("roll", "float", false), pin("space", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("goal", "Goal")}});
    t.push_back({"BindIkGoal", "Bind IK Goal", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("target", "int", false), pin("ox", "float", false), pin("oy", "float", false), pin("oz", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("goal", "Goal"), attr("socket", "Socket (empty = target origin)")}});
    t.push_back({"SetIkGoalWeight", "Set IK Goal Weight", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("weight", "float", false), pin("blendSeconds", "float", false, "0.15"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("goal", "Goal")}});
    t.push_back({"ClearIkGoal", "Clear IK Goal", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("goal", "Goal (empty = all)")}});
    t.push_back({"GetIkGoalState", "Get IK Goal State", "Animation", {
        pin("entity", "int", false), pin("found", "bool", true), pin("alpha", "float", true), pin("errorCm", "float", true), pin("reached", "bool", true)},
        {attr("goal", "Goal")}});
    t.push_back({"SetFootIkWeight", "Set Foot IK Weight", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("weight", "float", false), pin("blendSeconds", "float", false, "0.2"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("foot", "Foot (empty = all feet)")}});
    t.push_back({"GetFootIkState", "Get Foot IK State", "Animation", {
        pin("entity", "int", false), pin("found", "bool", true), pin("offsetCm", "float", true), pin("plant", "float", true), pin("nx", "float", true), pin("ny", "float", true), pin("nz", "float", true)},
        {attr("foot", "Foot (empty = pelvis)")}});
    t.push_back({"SetRigInputFloat", "Set Rig Input (Float)", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("input", "Input")}});
    t.push_back({"SetRigInputVec3", "Set Rig Input (Vector)", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("input", "Input")}});
    t.push_back({"ClearRigInput", "Clear Rig Input", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("input", "Input (empty = all)")}});
}
