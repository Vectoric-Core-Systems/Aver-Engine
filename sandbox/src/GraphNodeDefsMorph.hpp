#pragma once
// Palette entries for the blend-shape and lip-sync nodes. NOT standalone: include it from GraphNodeDefs.hpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendMorphNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphMorphNodeTable.cs pin for pin
// (docs/plans/wave9/MORPH_TARGETS.md section 2.9). Target, pose, rig, track and viseme names are attributes, never
// pins; an empty rig or clear target means "the default" / "every override". Exec nodes take part in the PUSH compiler
// only; the two pure reads work under either.

inline void appendMorphNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"SetMorphWeight", "Set Morph Weight", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("weight", "float", false), pin("blendSeconds", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("target", "Morph target")}});
    t.push_back({"SetFacePose", "Set Face Pose", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("weight", "float", false), pin("blendSeconds", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("pose", "Face pose")}});
    t.push_back({"ClearMorphWeights", "Clear Morph Weights", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("blendSeconds", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("target", "Morph target or pose (empty = all)")}});
    t.push_back({"GetMorphWeight", "Get Morph Weight", "Animation", {
        pin("entity", "int", false), pin("found", "bool", true), pin("weight", "float", true)},
        {attr("target", "Morph target")}});
    t.push_back({"SetFaceRig", "Set Face Rig", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("rig", "Face rig (.ocface; empty = built-in)")}});
    t.push_back({"PlayLipSync", "Play Lip Sync", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("rate", "float", false, "1"), pin("startSeconds", "float", false, "0"),
        pin("strength", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("track", "Lip-sync track (.ocvis)")}});
    t.push_back({"StopLipSync", "Stop Lip Sync", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("blendSeconds", "float", false, "0.1"),
        pin("then", "exec", true), pin("success", "bool", true)}, {}});
    t.push_back({"QueueViseme", "Queue Viseme", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("startSeconds", "float", false, "0"), pin("durationSeconds", "float", false, "0.1"),
        pin("intensity", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("viseme", "Viseme or phoneme")}});
    t.push_back({"GetLipSyncState", "Get Lip Sync State", "Animation", {
        pin("entity", "int", false), pin("playing", "bool", true), pin("time", "float", true), pin("length", "float", true), pin("strength", "float", true)},
        {}});
}
