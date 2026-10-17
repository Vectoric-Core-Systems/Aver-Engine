#pragma once
// Palette entries for the game-systems node families: timers and events, blackboard, streamed audio,
// animation state machines, game UI, prefabs and decals, Enhanced Input, data tables, localization and
// montages. (Gameplay abilities are in GraphNodeDefsGas.hpp.) NOT standalone: include it from
// GraphNodeDefs.hpp inside namespace aver::editor::detail, after pin()/attr() are defined and before
// buildCatalog(), then call appendGameSystemNodeDefs(t) just before buildCatalog returns.
//
// The crowd/hearing/cover rows are the synapse agent's own file (GraphNodeDefsSynapseAi.hpp).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs pin for pin, the same hand-kept
// parity GraphNodeDefs.hpp states at its top: a spawned node's pins are written into the file as
// PIN records, and the parser skips its default pins for any node that has them. Each row names its
// string arguments (event=, key=, sound=, ...) as attributes; those are NODE-line key=value pairs.
// Exec nodes take part in the PUSH compiler only; the pure reads work under either compiler.

inline void appendGameSystemNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_OnEvent", "On Event", "Events", {
        pin("exec", "exec", true)}});
    t.push_back({"AN_SetTimer", "Set Timer", "Events", {
        pin("exec", "exec", false), pin("target", "int", false), pin("delay", "float", false, "1"), pin("looping", "bool", false), pin("then", "exec", true), pin("handle", "int", true)},
        {attr("event", "Event")}});
    t.push_back({"AN_ClearTimer", "Clear Timer", "Events", {
        pin("exec", "exec", false), pin("handle", "int", false), pin("then", "exec", true), pin("cleared", "bool", true)}});
    t.push_back({"AN_DispatchEvent", "Dispatch Event", "Events", {
        pin("exec", "exec", false), pin("sender", "int", false), pin("target", "int", false), pin("i", "int", false), pin("f", "float", false), pin("immediate", "bool", false), pin("then", "exec", true)},
        {attr("event", "Event")}});
    t.push_back({"AN_EventPayload", "Event Payload", "Events", {
        pin("exec", "exec", false), pin("index", "int", false), pin("then", "exec", true), pin("sender", "int", true), pin("target", "int", true), pin("i", "int", true), pin("f", "float", true), pin("b", "bool", true)}});
    t.push_back({"GetBlackboardFloat", "Get Blackboard Float", "Blackboard", {
        pin("entity", "int", false), pin("value", "float", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"SetBlackboardFloat", "Set Blackboard Float", "Blackboard", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"GetBlackboardInt", "Get Blackboard Int", "Blackboard", {
        pin("entity", "int", false), pin("value", "int", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"SetBlackboardInt", "Set Blackboard Int", "Blackboard", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"GetBlackboardBool", "Get Blackboard Bool", "Blackboard", {
        pin("entity", "int", false), pin("value", "bool", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"SetBlackboardBool", "Set Blackboard Bool", "Blackboard", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"GetBlackboardEntity", "Get Blackboard Entity", "Blackboard", {
        pin("entity", "int", false), pin("value", "int", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"SetBlackboardEntity", "Set Blackboard Entity", "Blackboard", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"GetBlackboardVec3", "Get Blackboard Vec3", "Blackboard", {
        pin("entity", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"SetBlackboardVec3", "Set Blackboard Vec3", "Blackboard", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Key")}});
    t.push_back({"AN_PlayStream", "Play Stream", "Audio", {
        pin("exec", "exec", false), pin("volume", "float", false, "1"), pin("pitch", "float", false, "1"), pin("looping", "bool", false), pin("bus", "int", false), pin("fadeInSeconds", "float", false), pin("then", "exec", true), pin("voice", "int", true), pin("success", "bool", true)},
        {attr("sound", "Sound")}});
    t.push_back({"AN_PlayStreamAt", "Play Stream At", "Audio", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("volume", "float", false, "1"), pin("pitch", "float", false, "1"), pin("looping", "bool", false), pin("bus", "int", false), pin("innerCm", "float", false, "200"), pin("outerCm", "float", false, "2000"), pin("fadeInSeconds", "float", false), pin("then", "exec", true), pin("voice", "int", true), pin("success", "bool", true)},
        {attr("sound", "Sound")}});
    t.push_back({"AN_PlayMusic", "Play Music", "Audio", {
        pin("exec", "exec", false), pin("volume", "float", false, "1"), pin("fadeSeconds", "float", false, "2"), pin("looping", "bool", false, "true"), pin("curve", "int", false, "1"), pin("then", "exec", true), pin("voice", "int", true), pin("success", "bool", true)},
        {attr("sound", "Sound")}});
    t.push_back({"AN_StopMusic", "Stop Music", "Audio", {
        pin("exec", "exec", false), pin("fadeSeconds", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_IsMusicPlaying", "Is Music Playing", "Audio", {
        pin("playing", "bool", true)}});
    t.push_back({"AN_FadeSound", "Fade Sound", "Audio", {
        pin("exec", "exec", false), pin("voice", "int", false), pin("targetGain", "float", false), pin("seconds", "float", false, "1"), pin("curve", "int", false, "1"), pin("stopWhenDone", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetVoiceOcclusion", "Set Voice Occlusion", "Audio", {
        pin("exec", "exec", false), pin("voice", "int", false), pin("occlusion", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetReverb", "Set Reverb", "Audio", {
        pin("exec", "exec", false), pin("wet", "float", false, "0.3"), pin("decaySeconds", "float", false, "1.8"), pin("damping", "float", false, "0.4"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_AttachAudioOcclusion", "Attach Audio Occlusion", "Audio", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("voice", "int", false), pin("rayCount", "int", false, "5"), pin("probeRadiusCm", "float", false, "50"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetReverbZone", "Set Reverb Zone", "Audio", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("halfX", "float", false, "500"), pin("halfY", "float", false, "500"), pin("halfZ", "float", false, "300"), pin("blendDistanceCm", "float", false, "200"), pin("wet", "float", false, "0.35"), pin("decaySeconds", "float", false, "1.8"), pin("damping", "float", false, "0.4"), pin("priority", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetAnimGraph", "Set Anim Graph", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("playRate", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("machine", "State machine")}});
    t.push_back({"SetAnimParamFloat", "Set Anim Param Float", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("param", "Parameter")}});
    t.push_back({"SetAnimParamBool", "Set Anim Param Bool", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("value", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("param", "Parameter")}});
    t.push_back({"TriggerAnim", "Trigger Anim", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("param", "Trigger")}});
    t.push_back({"IsInAnimState", "Is In Anim State", "Animation", {
        pin("entity", "int", false), pin("result", "bool", true)},
        {attr("state", "State")}});
    t.push_back({"GetAnimStateTime", "Get Anim State Time", "Animation", {
        pin("entity", "int", false), pin("time", "float", true)}});
    t.push_back({"AN_OpenUiLayout", "Open UI Layout", "UI", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("layout", "int", true), pin("success", "bool", true)},
        {attr("path", "Layout file")}});
    t.push_back({"AN_CloseUiLayout", "Close UI Layout", "UI", {
        pin("exec", "exec", false), pin("layout", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_FindUiWidget", "Find UI Widget", "UI", {
        pin("exec", "exec", false), pin("layout", "int", false), pin("then", "exec", true), pin("widget", "int", true), pin("success", "bool", true)},
        {attr("name", "Widget name")}});
    t.push_back({"AN_CreateUiWidget", "Create UI Widget", "UI", {
        pin("exec", "exec", false), pin("parent", "int", false), pin("kind", "int", false), pin("then", "exec", true), pin("widget", "int", true), pin("success", "bool", true)},
        {attr("name", "Widget name")}});
    t.push_back({"AN_SetUiText", "Set UI Text", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("text", "Text")}});
    t.push_back({"AN_SetUiValue", "Set UI Value", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetUiChecked", "Set UI Checked", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("checked", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetUiVisible", "Set UI Visible", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("visible", "bool", false, "true"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetUiEnabled", "Set UI Enabled", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("enabled", "bool", false, "true"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetUiValue", "Get UI Value", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("then", "exec", true), pin("value", "float", true), pin("checked", "bool", true), pin("selected", "int", true), pin("success", "bool", true)}});
    t.push_back({"AN_UiWasClicked", "UI Was Clicked", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("then", "exec", true), pin("clicked", "bool", true)}});
    t.push_back({"AN_UiCommandFired", "UI Command Fired", "UI", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("fired", "bool", true)},
        {attr("name", "Command")}});
    t.push_back({"AN_SetUiFocus", "Set UI Focus", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_OpenUiSettings", "Open UI Settings", "UI", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("layout", "int", true), pin("success", "bool", true)}});
    t.push_back({"AN_SpawnPrefab", "Spawn Prefab", "Prefab", {
        pin("exec", "exec", false), pin("parent", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("yaw", "float", false), pin("scale", "float", false, "1"), pin("then", "exec", true), pin("entity", "int", true)},
        {attr("prefab", "Prefab")}});
    t.push_back({"AN_DestroyPrefab", "Destroy Prefab", "Prefab", {
        pin("exec", "exec", false), pin("root", "int", false), pin("then", "exec", true)}});
    t.push_back({"AN_GetPrefabRoot", "Get Prefab Root", "Prefab", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("root", "int", true)}});
    t.push_back({"AN_FindPrefabNode", "Find Prefab Node", "Prefab", {
        pin("exec", "exec", false), pin("root", "int", false), pin("then", "exec", true), pin("entity", "int", true)},
        {attr("node", "Node path")}});
    t.push_back({"AN_RevertPrefab", "Revert Prefab", "Prefab", {
        pin("exec", "exec", false), pin("root", "int", false), pin("then", "exec", true)}});
    t.push_back({"AN_SpawnDecal", "Spawn Decal", "Decal", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("nx", "float", false), pin("ny", "float", false), pin("nz", "float", false, "1"), pin("sx", "float", false, "100"), pin("sy", "float", false, "100"), pin("sz", "float", false, "100"), pin("roll", "float", false), pin("lifetime", "float", false), pin("fadeOut", "float", false, "0.5"), pin("then", "exec", true), pin("entity", "int", true)},
        {attr("base", "Base colour"), attr("normalmap", "Normal map"), attr("orm", "ORM")}});
    t.push_back({"AN_ClearDecals", "Clear Decals", "Decal", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetDecalCapacity", "Set Decal Capacity", "Decal", {
        pin("exec", "exec", false), pin("capacity", "int", false, "256"), pin("then", "exec", true), pin("success", "bool", true)}});

    // ---- Enhanced Input (docs/plans/wave2/ENHANCED_INPUT.md) ----
    t.push_back({"InputActionState", "Input Action State", "Input", {
        pin("state", "int", true), pin("started", "bool", true), pin("ongoing", "bool", true), pin("triggered", "bool", true), pin("completed", "bool", true), pin("canceled", "bool", true), pin("elapsed", "float", true), pin("triggeredTime", "float", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)},
        {attr("action", "Action")}});
    t.push_back({"AddInputTrigger", "Add Input Trigger", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("action", "Action"), attr("trigger", "Trigger spec")}});
    t.push_back({"AddInputModifier", "Add Input Modifier", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("action", "Action"), attr("modifier", "Modifier spec")}});
    t.push_back({"ClearInputConfig", "Clear Input Config", "Input", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("action", "Action")}});
    t.push_back({"OnInputAction", "On Input Action", "Event", {
        pin("exec", "exec", true)},
        {attr("action", "Action"), attr("event", "Event (Started, Ongoing, Triggered, Completed, Canceled)")}});
    // ---- data tables and localization (docs/plans/wave2/DATA_TABLES_LOCALIZATION.md) ----
    t.push_back({"AN_FindDataTableRow", "Find Data Table Row", "Data", {
        pin("row", "int", true), pin("found", "bool", true)},
        {attr("table", "Data table"), attr("row", "Row name")}});
    t.push_back({"AN_FindDataTableRowByInt", "Find Data Table Row By Int", "Data", {
        pin("value", "int", false), pin("row", "int", true), pin("found", "bool", true)},
        {attr("table", "Data table"), attr("column", "Column")}});
    t.push_back({"AN_DataTableRowCount", "Data Table Row Count", "Data", {
        pin("count", "int", true)},
        {attr("table", "Data table")}});
    t.push_back({"AN_GetDataTableInt", "Get Data Table Int", "Data", {
        pin("row", "int", false), pin("value", "int", true), pin("success", "bool", true)},
        {attr("table", "Data table"), attr("column", "Column")}});
    t.push_back({"AN_GetDataTableFloat", "Get Data Table Float", "Data", {
        pin("row", "int", false), pin("value", "float", true), pin("success", "bool", true)},
        {attr("table", "Data table"), attr("column", "Column")}});
    t.push_back({"AN_GetDataTableBool", "Get Data Table Bool", "Data", {
        pin("row", "int", false), pin("value", "bool", true), pin("success", "bool", true)},
        {attr("table", "Data table"), attr("column", "Column")}});
    t.push_back({"AN_GetDataTableVec3", "Get Data Table Vec3", "Data", {
        pin("row", "int", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("success", "bool", true)},
        {attr("table", "Data table"), attr("column", "Column")}});
    t.push_back({"AN_SetUiTextFromTable", "Set UI Text From Table", "Data", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("row", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("table", "Data table"), attr("column", "Column")}});
    t.push_back({"AN_SetCulture", "Set Culture", "Localization", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("culture", "Culture")}});
    t.push_back({"AN_IsCulture", "Is Culture", "Localization", {
        pin("matches", "bool", true)},
        {attr("culture", "Culture")}});
    t.push_back({"AN_CultureCount", "Culture Count", "Localization", {
        pin("count", "int", true)}});
    t.push_back({"AN_CurrentCultureIndex", "Current Culture Index", "Localization", {
        pin("index", "int", true)}});
    t.push_back({"AN_SetCultureByIndex", "Set Culture By Index", "Localization", {
        pin("exec", "exec", false), pin("index", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetUiItemsToCultures", "Set UI Items To Cultures", "Localization", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    // ---- montages and root motion (docs/plans/wave2/MONTAGES_ROOT_MOTION.md) ----
    t.push_back({"PlayMontage", "Play Montage", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("rate", "float", false, "1"), pin("blendIn", "float", false, "-1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("montage", "Montage"), attr("section", "Section"), attr("slot", "Slot")}});
    t.push_back({"StopMontage", "Stop Montage", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("blendOut", "float", false, "-1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("montage", "Montage")}});
    t.push_back({"StopMontageSlot", "Stop Montage Slot", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("blendOut", "float", false, "-1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("slot", "Slot")}});
    t.push_back({"JumpToMontageSection", "Jump To Montage Section", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("montage", "Montage"), attr("section", "Section")}});
    t.push_back({"SetMontageNextSection", "Set Montage Next Section", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("montage", "Montage"), attr("from", "From section"), attr("to", "To section (empty = authored)")}});
    t.push_back({"SetMontageRate", "Set Montage Rate", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("rate", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("montage", "Montage")}});
    t.push_back({"IsMontagePlaying", "Is Montage Playing", "Animation", {
        pin("entity", "int", false), pin("result", "bool", true)},
        {attr("montage", "Montage")}});
    t.push_back({"GetMontageState", "Get Montage State", "Animation", {
        pin("entity", "int", false), pin("found", "bool", true), pin("position", "float", true), pin("length", "float", true), pin("weight", "float", true)},
        {attr("montage", "Montage")}});
    t.push_back({"IsInMontageSection", "Is In Montage Section", "Animation", {
        pin("entity", "int", false), pin("result", "bool", true)},
        {attr("montage", "Montage"), attr("section", "Section")}});
    t.push_back({"SetRootMotion", "Set Root Motion", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("mode", "int", false), pin("axes", "int", false, "11"), pin("scale", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"ConsumeRootMotion", "Consume Root Motion", "Animation", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("yaw", "float", true), pin("seconds", "float", true)}});
    // ---- model import (docs/plans/wave9/FBX_IMPORT.md 2.10): editor hosts only, a packaged game reports unavailable ----
    t.push_back({"ImportModel", "Import Model", "Assets", {
        pin("exec", "exec", false), pin("overwrite", "bool", false), pin("scale", "float", false, "100"), pin("then", "exec", true), pin("success", "bool", true), pin("meshes", "int", true), pin("skeletons", "int", true), pin("clips", "int", true)},
        {attr("source", "Source file"), attr("dest", "Destination folder (Content-relative)")}});
    t.push_back({"IsModelImportAvailable", "Is Model Import Available", "Assets", {
        pin("available", "bool", true)}});
}
