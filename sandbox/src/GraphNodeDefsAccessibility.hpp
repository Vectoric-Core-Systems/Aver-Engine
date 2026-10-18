#pragma once
// Palette entries for the Accessibility nodes. NOT standalone: include it from GraphNodeDefs.hpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendAccessibilityNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphAccessibilityNodeTable.cs pin for pin (docs/plans/wave9/ACCESSIBILITY.md
// section 2.10). Option keys, cues, speakers and texts are attributes, never pins. Option changes arrive as AN_OnEvent
// entries named Access.Changed, so they need no node here.

inline void appendAccessibilityNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"SetAccessibilityOption", "Set Accessibility Option", "Accessibility", {
        pin("exec", "exec", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Option key (e.g. access.ui.textScale)")}});
    t.push_back({"GetAccessibilityOption", "Get Accessibility Option", "Accessibility", {
        pin("value", "float", true), pin("found", "bool", true)},
        {attr("key", "Option key")}});
    t.push_back({"ShowCaption", "Show Caption", "Accessibility", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("positional", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("cue", "Cue id")}});
    t.push_back({"ShowCaptionText", "Show Caption Text", "Accessibility", {
        pin("exec", "exec", false), pin("seconds", "float", false, "3"), pin("kind", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("speaker", "Speaker (text or @Table:Key, optional)"), attr("text", "Text or @Table:Key")}});
    t.push_back({"StopCaption", "Stop Caption", "Accessibility", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("cue", "Cue id (empty = all)")}});
    t.push_back({"Announce", "Announce", "Accessibility", {
        pin("exec", "exec", false), pin("interrupt", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("text", "Text or @Table:Key")}});
    t.push_back({"StopSpeaking", "Stop Speaking", "Accessibility", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"IsSpeaking", "Is Speaking", "Accessibility", {
        pin("result", "bool", true)}});
    t.push_back({"AddCameraShake", "Add Camera Shake", "Accessibility", {
        pin("exec", "exec", false), pin("trauma", "float", false, "0.5"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetCameraBob", "Set Camera Bob", "Accessibility", {
        pin("exec", "exec", false), pin("amount", "float", false), pin("speed", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetActionToggleAllowed", "Set Action Toggle Allowed", "Accessibility", {
        pin("exec", "exec", false), pin("allowed", "bool", false, "true"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("action", "Action name")}});
}
