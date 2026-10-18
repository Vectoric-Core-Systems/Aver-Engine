#pragma once
// Palette entries for the game flow (clock, pause, timers, settings) nodes. NOT standalone: it is included by the generated GraphNodeFamilies.gen.inc inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphGameFlowNodeTable.cs pin for pin (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2).
// Settings keys are attributes, not pins. Exec rows take part in the PUSH compiler only.

inline void appendGameFlowNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_GetTime", "Get Time", "Game", {
        pin("seconds", "float", true)}});
    t.push_back({"AN_GetRealTime", "Get Real Time", "Game", {
        pin("seconds", "float", true)}});
    t.push_back({"AN_GetTimeDilation", "Get Time Dilation", "Game", {
        pin("value", "float", true)}});
    t.push_back({"AN_SetTimeDilation", "Set Time Dilation", "Game", {
        pin("exec", "exec", false), pin("value", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_IsGamePaused", "Is Game Paused", "Game", {
        pin("paused", "bool", true)}});
    t.push_back({"AN_SetGamePaused", "Set Game Paused", "Game", {
        pin("exec", "exec", false), pin("paused", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_QuitGame", "Quit Game", "Game", {
        pin("exec", "exec", false), pin("then", "exec", true)}});
    t.push_back({"AN_TimerRemaining", "Timer Remaining", "Events", {
        pin("handle", "int", false), pin("seconds", "float", true)}});
    t.push_back({"AN_IsTimerActive", "Is Timer Active", "Events", {
        pin("handle", "int", false), pin("active", "bool", true)}});
    t.push_back({"AN_PauseTimer", "Pause Timer", "Events", {
        pin("exec", "exec", false), pin("handle", "int", false), pin("paused", "bool", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_ResetTimer", "Reset Timer", "Events", {
        pin("exec", "exec", false), pin("handle", "int", false), pin("delay", "float", false, "-1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetTimerFireCount", "Get Timer Fire Count", "Events", {
        pin("handle", "int", false), pin("count", "int", true)}});
    t.push_back({"AN_GetSettingFloat", "Get Setting Float", "Settings", {
        pin("fallback", "float", false), pin("value", "float", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_GetSettingInt", "Get Setting Int", "Settings", {
        pin("fallback", "int", false), pin("value", "int", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_GetSettingBool", "Get Setting Bool", "Settings", {
        pin("fallback", "bool", false), pin("value", "bool", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_HasSetting", "Has Setting", "Settings", {
        pin("has", "bool", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_SetSettingFloat", "Set Setting Float", "Settings", {
        pin("exec", "exec", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_SetSettingInt", "Set Setting Int", "Settings", {
        pin("exec", "exec", false), pin("value", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_SetSettingBool", "Set Setting Bool", "Settings", {
        pin("exec", "exec", false), pin("value", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Setting key")}});
    t.push_back({"AN_FlushSettings", "Flush Settings", "Settings", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
}
