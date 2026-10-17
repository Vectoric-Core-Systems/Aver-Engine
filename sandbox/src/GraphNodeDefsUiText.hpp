#pragma once
// Palette entries for the Game UI text nodes. NOT standalone: include it from GraphNodeDefs.hpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendUiTextNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphUiTextNodeTable.cs pin for pin (docs/GAME_UI_TEXT.md section 2.13). The
// font family is an attribute, never a pin. Font-set loading, atlas settings, prewarming and glyph queries are C#
// only; the UI language follows the culture nodes. Only lineSpacing and a have default values, as in the table.

inline void appendUiTextNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_SetUiFont", "Set UI Font", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("size", "float", false), pin("bold", "bool", false),
        pin("italic", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("family", "Family (empty keeps the widget's)")}});
    t.push_back({"AN_SetUiTextStyle", "Set UI Text Style", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("align", "int", false), pin("dir", "int", false),
        pin("wrap", "bool", false), pin("ellipsis", "bool", false), pin("maxLines", "int", false),
        pin("lineSpacing", "float", false, "1"), pin("letterSpacing", "float", false), pin("rich", "bool", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetUiTextColor", "Set UI Text Color", "UI", {
        pin("exec", "exec", false), pin("widget", "int", false), pin("r", "float", false), pin("g", "float", false),
        pin("b", "float", false), pin("a", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetUiTextSize", "Get UI Text Size", "UI", {
        pin("widget", "int", false), pin("found", "bool", true), pin("width", "float", true), pin("height", "float", true),
        pin("lines", "int", true)}});
}
