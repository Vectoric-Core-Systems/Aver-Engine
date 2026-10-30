// The widgets of G5 U2 (docs/plans/fabric/G5_PARITY.md): which Godot class is placed as which Aver.Fabric class, which properties each one reads, which it
// approximates and which it drops; the sub-resources the widgets hold (LabelSettings, FontVariation, SystemFont, ButtonGroup, Shortcut, input events) as
// Object(...) values or shared Resource() references; and the project font set (Content/UI/Fonts.ocfonts) that names every converted font as a family of
// the engine's text engine.
#include "FabricInternal.hpp"

#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace aver::fabric {

namespace {

using Reasons = std::vector<std::pair<const char*, const char*>>;

struct ClassRow {
    const char* type;
    const char* base;                 // the Godot class it derives from, "" for a Control or a Window
    std::vector<const char*> known;   // read by Aver.Fabric as written
    Reasons approx;                   // read, with a difference (the reason)
    std::vector<const char*> dynamic; // key prefixes Aver.Fabric reads by their scene names ("popup/item_0/text")
};

const Reasons kStoredText = {
    {"language", "stored; the engine picks faces by the UI language set for the project, not per control"},
    {"text_direction", "stored; the text engine decides the direction of each line (auto), the property does not force it"},
    {"structured_text_bidi_override", "no structured text override: the text engine's bidi rules apply"},
    {"structured_text_bidi_override_options", "no structured text override: the text engine's bidi rules apply"},
};

std::vector<ClassRow> buildRows() {
    std::vector<ClassRow> r;
    const auto add = [&](const char* type, const char* base, std::vector<const char*> known, Reasons approx = {}, std::vector<const char*> dyn = {}) {
        r.push_back({type, base, std::move(known), std::move(approx), std::move(dyn)});
    };
    Reasons textApprox = kStoredText;
    {
        Reasons a = textApprox;
        a.push_back({"justification_flags", "stored; lines are not justified (only the rich text [fill] aligns that way)"});
        a.push_back({"visible_characters_behavior", "characters are counted before shaping in every mode"});
        a.push_back({"tab_stops", "stored; a tab is four spaces wide"});
        a.push_back({"paragraph_separator", "stored; only a newline separates paragraphs"});
        add("Label", "", {"text", "label_settings", "horizontal_alignment", "vertical_alignment", "autowrap_mode", "clip_text", "text_overrun_behavior", "ellipsis_char", "uppercase",
                          "lines_skipped", "max_lines_visible", "visible_characters", "visible_ratio"}, a);
    }
    {
        Reasons a = textApprox;
        a.push_back({"custom_effects", "custom RichTextEffect classes run when their script is converted; the resource itself is not carried"});
        a.push_back({"threaded", "stored; the text is laid out on the game thread"});
        a.push_back({"progress_bar_delay", "stored; there is no loading bar"});
        a.push_back({"visible_characters_behavior", "characters are counted before shaping in every mode"});
        a.push_back({"context_menu_enabled", "stored; there is no context menu"});
        a.push_back({"hint_underlined", "stored; a hint is shown as a tooltip and the text is not underlined for it"});
        a.push_back({"text_overrun_behavior", "stored; the rich text wraps and scrolls instead of trimming"});
        add("RichTextLabel", "", {"text", "bbcode_enabled", "fit_content", "scroll_active", "scroll_following", "autowrap_mode", "tab_size", "shortcut_keys_enabled", "meta_underlined",
                                  "visible_characters", "visible_ratio", "selection_enabled", "deselect_on_focus_loss_enabled"}, a);
    }
    add("BaseButton", "", {"disabled", "toggle_mode", "button_pressed", "action_mode", "button_mask", "keep_pressed_outside", "button_group", "shortcut", "shortcut_feedback", "shortcut_in_tooltip"});
    {
        Reasons a = textApprox;
        a.push_back({"autowrap_trim_flags", "stored"});
        add("Button", "BaseButton", {"text", "icon", "flat", "alignment", "icon_alignment", "vertical_icon_alignment", "expand_icon", "clip_text", "text_overrun_behavior", "autowrap_mode"}, a);
    }
    add("CheckBox", "Button", {});
    add("CheckButton", "Button", {});
    {
        Reasons a = textApprox;
        a.push_back({"uri", "stored; pressing the button does not open a browser"});
        add("LinkButton", "BaseButton", {"text", "underline"}, a);
    }
    add("TextureButton", "BaseButton", {"texture_normal", "texture_pressed", "texture_hover", "texture_disabled", "texture_focused", "ignore_texture_size", "stretch_mode", "flip_h", "flip_v"},
        {{"texture_click_mask", "stored; the button's rectangle decides where it is hit, not the mask's opaque pixels"}});
    add("OptionButton", "Button", {"selected", "item_count", "fit_to_longest_item", "allow_reselect"}, {}, {"popup/item_"});
    add("MenuButton", "Button", {"item_count"}, {{"switch_on_hover", "stored; the menu opens on a press only"}}, {"popup/item_"});
    add("ColorPickerButton", "Button", {"color", "edit_alpha"}, {{"edit_intensity", "stored; the simple picker has no intensity mode"}});
    add("Range", "", {"min_value", "max_value", "step", "page", "value", "exp_edit", "rounded", "allow_greater", "allow_lesser"});
    add("Slider", "Range", {"editable", "scrollable", "tick_count", "ticks_on_borders"});
    add("HSlider", "Slider", {});
    add("VSlider", "Slider", {});
    add("ScrollBar", "Range", {"custom_step"});
    add("HScrollBar", "ScrollBar", {});
    add("VScrollBar", "ScrollBar", {});
    add("ProgressBar", "Range", {"show_percentage", "fill_mode"},
        {{"indeterminate", "stored; the indeterminate animation is not drawn"}, {"editor_preview_indeterminate", "stored; editor only"}});
    add("TextureProgressBar", "Range", {"texture_under", "texture_over", "texture_progress", "texture_progress_offset", "tint_under", "tint_over", "tint_progress", "fill_mode", "nine_patch_stretch",
                                        "stretch_margin_left", "stretch_margin_top", "stretch_margin_right", "stretch_margin_bottom"},
        {{"radial_initial_angle", "the angles are kept and readable; a radial fill is drawn as a left-to-right reveal (the UI draw list has no wedge)"},
         {"radial_fill_degrees", "the angles are kept and readable; a radial fill is drawn as a left-to-right reveal (the UI draw list has no wedge)"},
         {"radial_center_offset", "the angles are kept and readable; a radial fill is drawn as a left-to-right reveal (the UI draw list has no wedge)"}});
    add("SpinBox", "Range", {"prefix", "suffix", "editable", "update_on_text_changed", "custom_arrow_step", "select_all_on_focus", "alignment"});
    {
        Reasons a = textApprox;
        a.push_back({"context_menu_enabled", "stored; there is no context menu"});
        a.push_back({"emoji_menu_enabled", "stored; there is no emoji menu"});
        a.push_back({"virtual_keyboard_enabled", "stored; the engine host shows no on-screen keyboard"});
        a.push_back({"virtual_keyboard_show_on_focus", "stored; the engine host shows no on-screen keyboard"});
        a.push_back({"virtual_keyboard_type", "stored; the engine host shows no on-screen keyboard"});
        a.push_back({"clear_button_enabled", "stored; the clear button is not drawn (clear() and the text work)"});
        a.push_back({"middle_mouse_paste_enabled", "stored; the middle button does not paste"});
        a.push_back({"caret_blink", "stored; the caret does not blink"});
        a.push_back({"caret_blink_interval", "stored; the caret does not blink"});
        a.push_back({"drag_and_drop_selection_enabled", "stored; selected text cannot be dragged"});
        a.push_back({"draw_control_chars", "stored; control characters are not drawn"});
        a.push_back({"right_icon", "stored; the right icon is not drawn"});
        add("LineEdit", "", {"text", "placeholder_text", "alignment", "max_length", "editable", "secret", "secret_character", "expand_to_text_length", "shortcut_keys_enabled", "selecting_enabled",
                             "deselect_on_focus_loss_enabled", "flat", "select_all_on_focus", "caret_column", "caret_force_displayed", "caret_mid_grapheme"}, a);
    }
    {
        Reasons a = textApprox;
        a.push_back({"context_menu_enabled", "stored; there is no context menu"});
        a.push_back({"emoji_menu_enabled", "stored; there is no emoji menu"});
        a.push_back({"virtual_keyboard_enabled", "stored; the engine host shows no on-screen keyboard"});
        a.push_back({"middle_mouse_paste_enabled", "stored; the middle button does not paste"});
        a.push_back({"caret_blink", "stored; the caret does not blink"});
        a.push_back({"caret_blink_interval", "stored; the caret does not blink"});
        a.push_back({"caret_multiple", "stored; there is one caret"});
        a.push_back({"caret_type", "stored; the caret is a line"});
        a.push_back({"scroll_smooth", "stored; scrolling is not smoothed"});
        a.push_back({"scroll_v_scroll_speed", "stored; the wheel moves three lines"});
        a.push_back({"minimap_draw", "stored; there is no minimap"});
        a.push_back({"minimap_width", "stored; there is no minimap"});
        a.push_back({"draw_control_chars", "stored; control characters are not drawn"});
        a.push_back({"draw_tabs", "stored; tabs are not drawn"});
        a.push_back({"draw_spaces", "stored; spaces are not drawn"});
        a.push_back({"highlight_all_occurrences", "stored; occurrences are not highlighted"});
        a.push_back({"highlight_current_line", "stored; the line is not highlighted"});
        a.push_back({"drag_and_drop_selection_enabled", "stored; selected text cannot be dragged"});
        a.push_back({"use_default_word_separators", "stored; words are letters, digits and underscores"});
        a.push_back({"use_custom_word_separators", "stored; words are letters, digits and underscores"});
        a.push_back({"custom_word_separators", "stored; words are letters, digits and underscores"});
        a.push_back({"syntax_highlighter", "no syntax highlighting: the text draws in the font colour"});
        add("TextEdit", "", {"text", "placeholder_text", "editable", "shortcut_keys_enabled", "selecting_enabled", "deselect_on_focus_loss_enabled", "wrap_mode", "scroll_past_end_of_file", "scroll_vertical",
                             "scroll_horizontal", "scroll_fit_content_height", "scroll_fit_content_width", "caret_move_on_right_click", "caret_mid_grapheme"}, a);
    }
    add("ItemList", "", {"select_mode", "allow_reselect", "allow_rmb_select", "allow_search", "auto_width", "auto_height", "text_overrun_behavior", "item_count", "max_text_lines", "max_columns",
                         "same_column_width", "fixed_column_width", "icon_mode", "icon_scale", "fixed_icon_size"},
        {{"wraparound_items", "stored; the cursor stops at the first and last item"}, {"scroll_hint_mode", "stored; no scroll hint"}, {"tile_scroll_hint", "stored; no scroll hint"}}, {"item_"});
    add("Tree", "", {"columns", "column_titles_visible", "allow_reselect", "allow_rmb_select", "allow_search", "hide_folding", "enable_recursive_folding", "hide_root", "select_mode",
                     "scroll_horizontal_enabled", "scroll_vertical_enabled"},
        {{"drop_mode_flags", "stored; a Tree takes no drops itself (use _can_drop_data)"}, {"scroll_hint_mode", "stored; no scroll hint"}, {"tile_scroll_hint", "stored; no scroll hint"}});
    add("TabBar", "", {"current_tab", "tab_alignment", "clip_tabs", "tab_close_display_policy", "max_tab_width", "scrolling_enabled", "drag_to_rearrange_enabled", "tabs_rearrange_group",
                       "select_with_rmb", "tab_count"},
        {{"switch_on_drag_hover", "stored; dragging over a tab does not switch to it"}, {"deselect_enabled", "stored; a tab cannot be deselected"}, {"scroll_to_selected", "stored; clipped tabs do not scroll"}}, {"tab_"});
    add("TabContainer", "", {"tab_alignment", "current_tab", "tabs_position", "clip_tabs", "tabs_visible", "drag_to_rearrange_enabled", "tabs_rearrange_group", "use_hidden_tabs_for_min_size", "tab_focus_mode"},
        {{"all_tabs_in_front", "stored; the tab row is drawn above the panel as usual"}, {"switch_on_drag_hover", "stored; dragging over a tab does not switch to it"}, {"deselect_enabled", "stored; a tab cannot be deselected"}});
    add("SplitContainer", "", {"split_offset", "collapsed", "dragger_visibility", "vertical", "dragging_enabled"}, {{"split_offsets", "only the first offset is read (two children)"}});
    add("HSplitContainer", "SplitContainer", {});
    add("VSplitContainer", "SplitContainer", {});
    add("FlowContainer", "", {"alignment", "last_wrap_alignment", "vertical", "reverse_fill"});
    add("HFlowContainer", "FlowContainer", {});
    add("VFlowContainer", "FlowContainer", {});
    add("AspectRatioContainer", "", {"ratio", "stretch_mode", "alignment_horizontal", "alignment_vertical"});
    add("NinePatchRect", "", {"texture", "draw_center", "region_rect", "patch_margin_left", "patch_margin_top", "patch_margin_right", "patch_margin_bottom", "axis_stretch_horizontal", "axis_stretch_vertical"});
    add("TextureRect", "", {"texture", "expand_mode", "stretch_mode", "flip_h", "flip_v"});
    add("ColorRect", "", {"color"});
    add("Separator", "", {});
    add("HSeparator", "Separator", {});
    add("VSeparator", "Separator", {});
    add("ColorPicker", "", {"color", "edit_alpha", "deferred_mode", "can_add_swatches", "presets_visible"},
        {{"edit_intensity", "stored; the simple picker has no intensity mode"}, {"color_mode", "stored; the simple picker shows no numeric modes"}, {"picker_shape", "stored; the picker is always the rectangle"},
         {"sampler_visible", "stored; there is no screen sampler"}, {"color_modes_visible", "stored; there are no modes to show"}, {"sliders_visible", "stored; the simple picker has no sliders"},
         {"hex_visible", "stored; the simple picker has no hex field"}, {"deferred_mode", "stored; the colour changes while dragging"}});
    // The window-like classes: placed as top-level Controls (G5_PARITY.md U2).
    const Reasons windowApprox = {
        {"initial_position", "stored; popup() and popup_centered() decide where the popup opens"},
        {"transient_to_focused", "stored; there are no separate operating-system windows"},
        {"popup_window", "stored; every popup is drawn in the game window"},
        {"wrap_controls", "stored; the popup keeps the size it is given"},
        {"transparent", "stored; the popup draws on the game window"},
        {"min_size", "kept as the smallest the popup takes"},
        {"max_size", "kept as the largest the popup takes"},
        {"always_on_top", "stored; popups are drawn above the scene"},
        {"unresizable", "stored; popups cannot be resized by the player"},
        {"borderless", "kept; a dialog with no border has no title row"},
        {"force_native", "stored; no native windows"},
        {"mode", "stored; popups do not maximise"},
    };
    {
        Reasons a = windowApprox;
        add("Popup", "", {"title", "exclusive", "transient"}, a);
    }
    add("PopupPanel", "Popup", {});
    add("PopupMenu", "Popup", {"hide_on_item_selection", "hide_on_checkable_item_selection", "hide_on_state_item_selection", "submenu_popup_delay", "allow_search", "item_count"},
        {{"system_menu_id", "no native menus"}, {"prefer_native_menu", "no native menus"}, {"shrink_height", "the menu is as tall as its items"}, {"shrink_width", "the menu is as wide as its items"}}, {"item_"});
    add("AcceptDialog", "Popup", {"dialog_text", "dialog_hide_on_ok", "dialog_close_on_escape", "dialog_autowrap", "ok_button_text"});
    add("ConfirmationDialog", "AcceptDialog", {"cancel_button_text"});
    return r;
}

const std::vector<ClassRow>& rows() {
    static const std::vector<ClassRow> r = buildRows();
    return r;
}

const ClassRow* rowOf(const std::string& type) {
    for (const ClassRow& r : rows()) if (type == r.type) return &r;
    return nullptr;
}

// Window properties that Controls placed as popups read like any node's.
bool isWindowLike(const std::string& t) {
    return t == "Popup" || t == "PopupPanel" || t == "PopupMenu" || t == "AcceptDialog" || t == "ConfirmationDialog";
}

struct ChainHit {
    bool known = false;
    const char* approx = nullptr;
};

ChainHit lookup(const std::string& type, const std::string& key) {
    ChainHit hit;
    for (const ClassRow* row = rowOf(type); row; row = row->base[0] ? rowOf(row->base) : nullptr) {
        for (const char* k : row->known) if (key == k) { hit.known = true; return hit; }
        for (const auto& a : row->approx) if (key == a.first) { hit.approx = a.second; return hit; }
    }
    return hit;
}

}  // namespace

bool isWidgetClass(const std::string& type) { return rowOf(type) != nullptr; }

bool isWidgetDynamicKey(const std::string& type, const std::string& key) {
    for (const ClassRow* row = rowOf(type); row; row = row->base[0] ? rowOf(row->base) : nullptr)
        for (const char* p : row->dynamic) if (key.rfind(p, 0) == 0) return true;
    return false;
}

// 1 converted, 2 approximated (reason in `why`), 0 not a property of the class.
int widgetPropertyRating(const std::string& type, const std::string& key, std::string& why) {
    if (isWidgetDynamicKey(type, key)) { why = "Aver.Fabric " + type + " reads it by its scene name"; return 1; }
    const ChainHit h = lookup(type, key);
    if (h.known) { why = "Aver.Fabric " + type + " reads it"; return 1; }
    if (h.approx) { why = h.approx; return 2; }
    return 0;
}

// ---- classes that are not the plain class -------------------------------------------------------------------------------------------

bool widgetClassChoice(const std::string& t, ClassChoice& c) {
    if (!rowOf(t)) return false;
    c.fabricClass = t;
    c.rating = Rating::Converted;
    if (isWindowLike(t)) {
        c.rating = Rating::Approximated;
        c.reason = "placed as a top-level Control that is hidden until popup() (not as an operating-system window): it is hit first, drawn above the scene and closed by a click outside; "
                   "it is not a Window, so get_window(), embedded-window decorations and multiple windows do not apply";
    } else if (t == "TextEdit") {
        c.rating = Rating::Approximated;
        c.reason = "multi-line editing with selection, undo, search and soft wrapping; no syntax highlighting, gutters, folding or minimap, and the scroll bars are not drawn";
    } else if (t == "RichTextLabel") {
        c.rating = Rating::Approximated;
        c.reason = "BBCode, the push_* API, images, lists, tables and the built-in effects are laid out by Aver.Fabric and drawn with the engine's text engine; bold and italic ask the family for "
                   "those faces, wave / tornado / shake / fade / rainbow follow Godot's formulas per glyph";
    } else if (t == "Tree") {
        c.rating = Rating::Approximated;
        c.reason = "items, columns, folding, selection modes and signals are Godot's; text cells are not edited in place and the scroll bar is not drawn";
    } else if (t == "ColorPicker" || t == "ColorPickerButton") {
        c.rating = Rating::Approximated;
        c.reason = "a simple picker: a saturation / value square, hue and alpha bars and presets; no wheel shapes, sliders or hex field";
    } else if (t == "TextureProgressBar") {
        c.rating = Rating::Approximated;
        c.reason = "linear and bilinear fills are drawn from the texture region; a radial fill keeps its angles and is drawn as a left-to-right reveal";
    } else if (t == "NinePatchRect") {
        c.rating = Rating::Approximated;
        c.reason = "drawn through the engine's 9-patch tessellator; it appears once the host gives the texture an engine handle (G5_PARITY.md U1)";
    } else if (t == "ItemList") {
        c.rating = Rating::Approximated;
        c.reason = "items, selection modes, columns and signals are Godot's; the scroll bar is not drawn (get_v_scroll_bar() is a detached bar holding the position)";
    }
    return true;
}

// ---- the resources widgets hold ----------------------------------------------------------------------------------------------------------

bool isWidgetResourceType(const std::string& t) {
    return t == "LabelSettings" || t == "FontVariation" || t == "SystemFont" || t == "FontFile" || t == "ButtonGroup" || t == "Shortcut" || t == "InputEventKey" ||
           t == "InputEventMouseButton" || t == "BitMap" || t == "RichTextEffect";
}

namespace {

struct ResRow {
    const char* type;
    std::vector<const char*> known;
    Reasons approx;
};

const std::vector<ResRow>& resRows() {
    static const std::vector<ResRow> r = {
        {"LabelSettings", {"line_spacing", "paragraph_spacing", "font", "font_size", "font_color", "outline_size", "outline_color", "shadow_size", "shadow_color", "shadow_offset"}, {}},
        {"FontVariation", {"base_font", "variation_embolden", "variation_face_index", "variation_transform", "variation_opentype", "opentype_features", "spacing_glyph", "spacing_space", "spacing_top",
                           "spacing_bottom", "baseline_offset"},
         {{"spacing_glyph", "stored; the engine draws glyphs at their own advance"}, {"spacing_space", "stored; the engine draws spaces at their own advance"},
          {"spacing_top", "stored; line height is the font's"}, {"spacing_bottom", "stored; line height is the font's"},
          {"variation_opentype", "stored; variable-font axes are not applied (the engine rasterises the default instance)"},
          {"opentype_features", "stored; OpenType substitutions (GSUB) are not applied"}, {"variation_face_index", "stored; the first face of a collection is used"},
          {"variation_embolden", "a value above zero asks the family for its bold face; there is no synthetic emboldening"},
          {"variation_transform", "an x skew asks for the italic face; otherwise the transform is not applied"}, {"baseline_offset", "stored"}}},
        {"SystemFont", {"font_names", "font_italic", "font_weight", "font_stretch", "antialiasing", "generate_mipmaps", "allow_system_fallback", "force_autohinter", "hinting", "subpixel_positioning",
                        "multichannel_signed_distance_field", "msdf_pixel_range", "msdf_size", "oversampling"},
         {{"font_names", "the default family is used (its SYSTEM auto entry reaches the operating system's fonts); the names are kept"},
          {"multichannel_signed_distance_field", "stored; the engine rasterises coverage glyphs at the drawn size (no MSDF)"}, {"msdf_pixel_range", "stored; no MSDF"}, {"msdf_size", "stored; no MSDF"},
          {"hinting", "stored; no hinting modes"}, {"force_autohinter", "stored; no autohinter"}, {"subpixel_positioning", "stored"}, {"oversampling", "stored"}, {"antialiasing", "stored; glyphs are antialiased"},
          {"generate_mipmaps", "stored"}, {"font_stretch", "stored"}}},
        {"FontFile", {"font_name", "style_name", "font_style", "fixed_size", "fixed_size_scale_mode", "antialiasing", "generate_mipmaps", "multichannel_signed_distance_field", "msdf_pixel_range",
                      "msdf_size", "allow_system_fallback", "force_autohinter", "hinting", "subpixel_positioning", "oversampling"},
         {{"multichannel_signed_distance_field", "stored; the engine rasterises coverage glyphs at the drawn size (no MSDF)"}, {"msdf_pixel_range", "stored; no MSDF"}, {"msdf_size", "stored; no MSDF"},
          {"hinting", "stored; no hinting modes"}, {"force_autohinter", "stored; no autohinter"}, {"subpixel_positioning", "stored"}, {"oversampling", "stored"}, {"antialiasing", "stored; glyphs are antialiased"},
          {"generate_mipmaps", "stored"}, {"fixed_size", "stored; the font draws at the size asked for"}, {"fixed_size_scale_mode", "stored"}}},
        {"ButtonGroup", {"allow_unpress"}, {}},
        {"Shortcut", {"events"}, {}},
        {"InputEventKey", {"keycode", "physical_keycode", "key_label", "unicode", "pressed", "echo", "shift_pressed", "ctrl_pressed", "alt_pressed", "meta_pressed", "device"}, {}},
        {"InputEventMouseButton", {"button_index", "pressed", "double_click", "shift_pressed", "ctrl_pressed", "alt_pressed", "meta_pressed", "device"}, {}},
    };
    return r;
}

}  // namespace

Var Converter::widgetResource(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line, const std::string& id) {
    if (type == "RichTextEffect") {
        report_.add("resource", doc.file, w.node, type, "", Rating::Unsupported, "a RichTextEffect is a script: convert its script and add the converted class to the label's custom_effects in code", line);
        return makeNil();
    }
    if (type == "BitMap") {
        report_.add("resource", doc.file, w.node, type, "", Rating::Unsupported, "a click mask is not converted; the button's rectangle decides where it is hit", line);
        return makeNil();
    }
    if (type == "ButtonGroup") {
        // One shared group: every button that names it gets the same Resource() path, which Aver.Fabric's loader turns into one ButtonGroup.
        bool allow = false;
        for (const gd::GodotProperty& p : props) if (p.name == "allow_unpress") allow = boolOf(&p.value, false);
        const std::string key = doc.file + "#" + id;
        auto it = buttonGroups_.find(key);
        if (it == buttonGroups_.end())
            it = buttonGroups_.emplace(key, "ButtonGroups/" + safeName(stemOf(doc.file) + "_" + (id.empty() ? std::to_string(buttonGroups_.size()) : id), "Group") + (allow ? "_unpress" : "") + ".ocbuttongroup").first;
        report_.add("resource", doc.file, w.node, type, "", Rating::Converted,
                    "shared by name: " + it->second + " (one Aver.Fabric ButtonGroup for every button that names it" + (allow ? "; allow_unpress is the _unpress suffix" : "") + ")", line);
        return makeResource(it->second);
    }
    const ResRow* row = nullptr;
    for (const ResRow& r : resRows()) if (type == r.type) row = &r;
    std::vector<std::pair<std::string, Var>> entries;
    bool storedOnly = false;
    for (const gd::GodotProperty& p : props) {
        const std::string& n = p.name;
        if (n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script") continue;
        bool known = false;
        if (row) {
            for (const char* k : row->known) if (n == k) known = true;
            for (const auto& a : row->approx)
                if (n == a.first) {
                    report_.add("property", doc.file, w.node, type, n, Rating::Approximated, a.second, p.line);
                    storedOnly = true;
                }
        }
        if (!known) {
            report_.add("property", doc.file, w.node, type, n, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
            continue;
        }
        Var cv = convertValue(p.value, doc, w, n);
        if (cv.isNil() && !p.value.isNil()) continue;
        entries.emplace_back(n, std::move(cv));
    }
    if (type == "FontFile") {
        for (const gd::GodotProperty& p : props)
            if (p.name == "data") {
                report_.add("resource", doc.file, w.node, type, "data", Rating::Unsupported, "font bytes embedded in the resource are not extracted; use a .ttf / .otf file", p.line);
                return makeNil();
            }
    }
    report_.add("resource", doc.file, w.node, type, "", storedOnly ? Rating::Approximated : Rating::Converted, "kept as Object(" + type + ") with Godot's property names; Aver.Fabric builds it", line);
    return makeObject(type, std::move(entries));
}

// ---- the font set -----------------------------------------------------------------------------------------------------------------------

void Converter::noteFontAsset(const std::string& asset) { fontFiles_.insert(asset); }

void Converter::writeFontSet(const std::string& defaultFontAsset) {
    if (fontFiles_.empty()) return;
    const auto familyOf = [](const std::string& asset) {
        const size_t slash = asset.find_last_of('/');
        std::string n = slash == std::string::npos ? asset : asset.substr(slash + 1);
        const size_t dot = n.find_last_of('.');
        return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
    };
    std::string defaultFamily = defaultFontAsset.empty() ? std::string() : familyOf(defaultFontAsset);
    if (defaultFamily.empty()) defaultFamily = familyOf(*fontFiles_.begin());
    std::string text = "OCFONTS 1\nDEFAULT " + defaultFamily + "\n";
    std::set<std::string> seen;
    for (const std::string& asset : fontFiles_) {
        const std::string family = familyOf(asset);
        if (!seen.insert(lower(family)).second) {
            report_.add("resource", "", "", "FontFile", "", Rating::Approximated, asset + " shares the file name " + family + " with another font; the engine's font set names families by file name, so only the first is reachable", 0);
            continue;
        }
        const std::string lowered = lower(asset);
        const bool bold = lowered.find("bold") != std::string::npos || lowered.find("heavy") != std::string::npos || lowered.find("black") != std::string::npos;
        const bool italic = lowered.find("italic") != std::string::npos || lowered.find("oblique") != std::string::npos;
        text += "FAMILY " + family + "\nFACE " + asset + (bold ? " weight=bold" : "") + (italic ? " italic" : "") + "\nSYSTEM auto\nBITMAP legacy\n";
    }
    const std::string dst = contentDir_ + "/UI/Fonts.ocfonts";
    createDirectories(std::filesystem::path(dst).parent_path().string());
    if (writeFileText(dst, text))
        report_.add("resource", "", "", "FontFile", "", Rating::Converted,
                    "UI/Fonts.ocfonts names every converted font as a family of the engine's text engine (file name = family name, default " + defaultFamily + "); each family falls back to the operating system's fonts", 0);
    else
        report_.add("resource", "", "", "FontFile", "", Rating::Unsupported, "could not write " + dst, 0);
}

}  // namespace aver::fabric
