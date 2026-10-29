// UI foundations (docs/plans/fabric/G5_PARITY.md, U1): Theme and StyleBox resources, font files, the properties of the Control family and the project's
// stretch / theme settings.
//
// A Godot Theme becomes Content/Themes/<name>.octheme (aver/ui/UiThemeSheet.hpp writes it, so the text form has one writer); the project theme is named by the
// project key UI.THEME. A StyleBox a node or script holds becomes an `Object(StyleBoxFlat, "bg_color": Color(...), ...)` value Aver.Fabric builds with Godot's
// property names. Style boxes inside a theme go through the same property reader, so a property is rated once whichever way it travels.
#include "FabricInternal.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiThemeSheet.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

namespace aver::fabric {

namespace {

constexpr const char* kProject = "res://project.godot";

std::string extLower(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(path.substr(dot));
}

const Var* findIn(const std::vector<gd::GodotProperty>& props, const char* name) {
    for (const gd::GodotProperty& p : props) if (p.name == name) return &p.value;
    return nullptr;
}

u32 packColor(const Var* v, u32 def) {
    if (!v || v->type != VType::Color || v->nums.size() < 4) return def;
    const auto q = [](double c) { return static_cast<u32>(std::lround(std::min(1.0, std::max(0.0, c)) * 255.0)); };
    return q(v->nums[0]) | (q(v->nums[1]) << 8) | (q(v->nums[2]) << 16) | (q(v->nums[3]) << 24);
}

// PNG width and height from the IHDR chunk.
bool pngDims(const std::string& disk, int& w, int& h) {
    std::vector<u8> b;
    if (disk.empty() || !readFileBytes(disk, b) || b.size() < 24) return false;
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) if (b[static_cast<size_t>(i)] != sig[i]) return false;
    w = static_cast<int>((u32(b[16]) << 24) | (u32(b[17]) << 16) | (u32(b[18]) << 8) | u32(b[19]));
    h = static_cast<int>((u32(b[20]) << 24) | (u32(b[21]) << 16) | (u32(b[22]) << 8) | u32(b[23]));
    return w > 0 && h > 0;
}

struct BoxFamily {
    const char* type;
    std::vector<const char*> known;
    std::vector<std::pair<const char*, const char*>> approx;
};

const std::vector<BoxFamily>& boxFamilies() {
    static const std::vector<const char*> content = {"content_margin_left", "content_margin_top", "content_margin_right", "content_margin_bottom"};
    static const std::vector<BoxFamily> f = {
        {"StyleBoxFlat",
         {"bg_color", "draw_center", "border_width_left", "border_width_top", "border_width_right", "border_width_bottom", "border_color", "border_blend",
          "corner_radius_top_left", "corner_radius_top_right", "corner_radius_bottom_right", "corner_radius_bottom_left", "corner_detail", "expand_margin_left",
          "expand_margin_top", "expand_margin_right", "expand_margin_bottom", "content_margin_left", "content_margin_top", "content_margin_right", "content_margin_bottom"},
         {{"skew", "drawn as a lean of the whole box: half the value to the right at the top, half to the left at the bottom; Godot shifts the corner points by its own rule"},
          {"shadow_color", "the shadow is the box shifted by shadow_offset with a feather of shadow_size outside it; Godot blurs it with its own falloff"},
          {"shadow_size", "the shadow is the box shifted by shadow_offset with a feather of shadow_size outside it; Godot blurs it with its own falloff"},
          {"shadow_offset", "the shadow is the box shifted by shadow_offset with a feather of shadow_size outside it; Godot blurs it with its own falloff"},
          {"anti_aliasing", "a one pixel (anti_aliasing_size) coverage fringe on the outline; Godot's own smoothing differs in the exact falloff"},
          {"anti_aliasing_size", "a one pixel (anti_aliasing_size) coverage fringe on the outline; Godot's own smoothing differs in the exact falloff"}}},
        {"StyleBoxTexture",
         {"texture", "texture_margin_left", "texture_margin_top", "texture_margin_right", "texture_margin_bottom", "expand_margin_left", "expand_margin_top",
          "expand_margin_right", "expand_margin_bottom", "axis_stretch_horizontal", "axis_stretch_vertical", "region_rect", "modulate_color", "draw_center",
          "content_margin_left", "content_margin_top", "content_margin_right", "content_margin_bottom"},
         {}},
        {"StyleBoxLine", {"color", "thickness", "grow_begin", "grow_end", "vertical", "content_margin_left", "content_margin_top", "content_margin_right", "content_margin_bottom"}, {}},
        {"StyleBoxEmpty", content, {}},
    };
    return f;
}

const BoxFamily* boxFamily(const std::string& type) {
    for (const BoxFamily& f : boxFamilies()) if (type == f.type) return &f;
    return nullptr;
}

bool ignorable(const std::string& n) {
    return n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script";
}

// The Control properties every class of the family reads.
const std::set<std::string>& controlKnown() {
    static const std::set<std::string> s = {
        "layout_mode", "anchors_preset", "anchor_left", "anchor_top", "anchor_right", "anchor_bottom", "offset_left", "offset_top", "offset_right", "offset_bottom",
        "grow_horizontal", "grow_vertical", "size_flags_horizontal", "size_flags_vertical", "size_flags_stretch_ratio", "custom_minimum_size", "position", "size",
        "pivot_offset", "clip_contents", "mouse_filter", "focus_mode", "focus_neighbor_left", "focus_neighbor_top", "focus_neighbor_right", "focus_neighbor_bottom",
        "focus_next", "focus_previous", "tooltip_text", "theme", "theme_type_variation", "visible", "modulate", "self_modulate", "top_level"};
    return s;
}

const std::vector<std::pair<const char*, const char*>>& controlApprox() {
    static const std::vector<std::pair<const char*, const char*>> a = {
        {"rotation", "turns the hit area and the global rectangle; style boxes and rectangles are drawn upright (the UI draw list has no rotation)"},
        {"rotation_degrees", "turns the hit area and the global rectangle; style boxes and rectangles are drawn upright (the UI draw list has no rotation)"},
        {"scale", "scales the drawn rectangles and the hit area; a style box's corner radii and border widths keep their size"},
        {"mouse_default_cursor_shape", "stored and readable; the engine host shows no custom cursors"},
        {"mouse_force_pass_scroll_events", "stored; wheel events follow mouse_filter like any other"},
        {"layout_direction", "stored; no right-to-left layout of containers"},
        {"text_direction", "stored; the text controls arrive with the widget package"},
        {"auto_translate", "stored"},
        {"localize_numeral_system", "stored"},
        {"z_index", "stored; Controls draw in tree order (later siblings on top)"},
        {"z_as_relative", "stored; Controls draw in tree order"},
        {"tooltip_auto_translate_mode", "stored"},
        {"auto_translate_mode", "stored"},
        {"shortcut_context", "stored"},
    };
    return a;
}

const std::set<std::string>& controlUnsupported() {
    static const std::set<std::string> s = {"material", "use_parent_material", "clip_children", "light_mask", "visibility_layer", "show_behind_parent", "texture_filter",
                                            "texture_repeat", "y_sort_enabled"};
    return s;
}

struct ContainerProps {
    const char* type;
    std::vector<const char*> known;
    std::vector<std::pair<const char*, const char*>> approx;
};

const std::vector<ContainerProps>& containerProps() {
    static const std::vector<ContainerProps> c = {
        {"BoxContainer", {"alignment", "vertical"}, {}},
        {"HBoxContainer", {"alignment"}, {}},
        {"VBoxContainer", {"alignment"}, {}},
        {"GridContainer", {"columns"}, {}},
        {"CenterContainer", {"use_top_left"}, {}},
        {"ScrollContainer", {"horizontal_scroll_mode", "vertical_scroll_mode", "scroll_horizontal", "scroll_vertical", "follow_focus", "scroll_deadzone"},
         {{"scroll_horizontal_custom_step", "stored; the wheel scrolls an eighth of the view"}, {"scroll_vertical_custom_step", "stored; the wheel scrolls an eighth of the view"},
          {"draw_focus_border", "stored; the focus border is not drawn"}}},
        {"MarginContainer", {}, {}},
        {"PanelContainer", {}, {}},
        {"Panel", {}, {}},
        {"Container", {}, {}},
        {"Control", {}, {}},
    };
    return c;
}

const ContainerProps* containerFor(const std::string& type) {
    for (const ContainerProps& c : containerProps()) if (type == c.type) return &c;
    return nullptr;
}

bool isRealUiClass(const std::string& t) {
    return t == "Control" || t == "Container" || t == "BoxContainer" || t == "HBoxContainer" || t == "VBoxContainer" || t == "GridContainer" || t == "MarginContainer" ||
           t == "CenterContainer" || t == "PanelContainer" || t == "ScrollContainer" || t == "Panel";
}

}  // namespace

bool isStyleBoxType(const std::string& t) { return boxFamily(t) != nullptr; }

bool isFontExt(const std::string& ext) { return ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2"; }

std::string Converter::fontAsset(const std::string& resPath, const Where& w) {
    if (const auto it = fontAssets_.find(resPath); it != fontAssets_.end()) return it->second;
    std::string asset;
    std::vector<u8> bytes;
    const std::string disk = fs_.toDiskPath(resPath);
    if (disk.empty() || !readFileBytes(disk, bytes)) {
        report_.add("resource", resPath, w.node, "Font", "", Rating::Unsupported, "the font file is missing", 0);
    } else {
        const std::string rel = resPath.rfind("res://", 0) == 0 ? resPath.substr(6) : resPath;
        asset = "Fonts/" + rel;
        const std::string dst = contentDir_ + "/" + asset;
        createDirectories(std::filesystem::path(dst).parent_path().string());
        if (!writeFileBytes(dst, bytes.data(), bytes.size())) {
            report_.add("resource", resPath, w.node, "Font", "", Rating::Unsupported, "could not write " + dst, 0);
            asset.clear();
        } else {
            noteFontAsset(asset);
            report_.add("resource", resPath, "", "FontFile", "", Rating::Converted,
                        "copied to " + asset + " and named as a family (" + stemOf(asset) + ") of the engine's font set UI/Fonts.ocfonts; the engine rasterises the TrueType outlines at run time. "
                        "MSDF, hinting modes, OpenType substitutions and variable-font axes are not applied", 0);
        }
    }
    fontAssets_[resPath] = asset;
    return asset;
}

namespace {

// Reads a StyleBox resource's properties twice over: into the theme file's box and into an Object(...) value, rating each once.
struct BoxReader {
    Converter& c;
    const std::string& type;
    const gd::GodotDocBase& doc;
    const Where& w;
    ui::UiThemeStyleBox box;
    std::vector<std::pair<std::string, Var>> entries;
    int approximated = 0, unknown = 0;

    BoxReader(Converter& conv, const std::string& t, const gd::GodotDocBase& d, const Where& where) : c(conv), type(t), doc(d), w(where) {
        using namespace aver::ui;
        UiStyleBox& s = box.box;
        if (t == "StyleBoxFlat") { s.kind = static_cast<i32>(UiStyleBoxKind::Flat); s.flags = kSbDrawCenter | kSbAntiAlias; }
        else if (t == "StyleBoxTexture") { s.kind = static_cast<i32>(UiStyleBoxKind::Texture); s.flags = kSbDrawCenter; }
        else if (t == "StyleBoxLine") { s.kind = static_cast<i32>(UiStyleBoxKind::Line); s.flags = 0; }
        else { s.kind = static_cast<i32>(UiStyleBoxKind::Empty); s.flags = 0; }
    }

    static void setFlag(ui::UiStyleBox& s, u32 bit, bool on) { if (on) s.flags |= bit; else s.flags &= ~bit; }

    void apply(const gd::GodotProperty& p, const Var& converted) {
        using namespace aver::ui;
        UiStyleBox& s = box.box;
        const std::string& n = p.name;
        const Var& v = p.value;
        const auto f = [&](double def = 0) { return static_cast<f32>(numOf(&v, def)); };
        if (n == "bg_color") s.bgColor = packColor(&v, s.bgColor);
        else if (n == "draw_center") setFlag(s, kSbDrawCenter, boolOf(&v, true));
        else if (n == "skew" && v.type == VType::Vector2) { s.skewX = static_cast<f32>(v.num(0)); s.skewY = static_cast<f32>(v.num(1)); }
        else if (n == "border_width_left") s.borderL = f();
        else if (n == "border_width_top") s.borderT = f();
        else if (n == "border_width_right") s.borderR = f();
        else if (n == "border_width_bottom") s.borderB = f();
        else if (n == "border_color") { const u32 col = packColor(&v, s.borderColorL); s.borderColorL = s.borderColorT = s.borderColorR = s.borderColorB = col; }
        else if (n == "border_blend") setFlag(s, kSbBorderBlend, boolOf(&v, false));
        else if (n == "corner_radius_top_left") s.radiusTL = f();
        else if (n == "corner_radius_top_right") s.radiusTR = f();
        else if (n == "corner_radius_bottom_right") s.radiusBR = f();
        else if (n == "corner_radius_bottom_left") s.radiusBL = f();
        else if (n == "corner_detail") s.cornerDetail = std::max(1, std::min(20, static_cast<int>(std::lround(numOf(&v, 8)))));
        else if (n == "expand_margin_left") s.expandL = f();
        else if (n == "expand_margin_top") s.expandT = f();
        else if (n == "expand_margin_right") s.expandR = f();
        else if (n == "expand_margin_bottom") s.expandB = f();
        else if (n == "shadow_color") s.shadowColor = packColor(&v, s.shadowColor);
        else if (n == "shadow_size") s.shadowSize = f();
        else if (n == "shadow_offset" && v.type == VType::Vector2) { s.shadowOffX = static_cast<f32>(v.num(0)); s.shadowOffY = static_cast<f32>(v.num(1)); }
        else if (n == "anti_aliasing") setFlag(s, kSbAntiAlias, boolOf(&v, true));
        else if (n == "anti_aliasing_size") s.aaSize = f(1);
        else if (n == "content_margin_left") s.contentL = f(-1);
        else if (n == "content_margin_top") s.contentT = f(-1);
        else if (n == "content_margin_right") s.contentR = f(-1);
        else if (n == "content_margin_bottom") s.contentB = f(-1);
        else if (n == "texture_margin_left") s.patchL = f();
        else if (n == "texture_margin_top") s.patchT = f();
        else if (n == "texture_margin_right") s.patchR = f();
        else if (n == "texture_margin_bottom") s.patchB = f();
        else if (n == "axis_stretch_horizontal") s.axisH = static_cast<int>(std::lround(numOf(&v, 0)));
        else if (n == "axis_stretch_vertical") s.axisV = static_cast<int>(std::lround(numOf(&v, 0)));
        else if (n == "region_rect" && v.type == VType::Rect2) { s.regionX = static_cast<f32>(v.num(0)); s.regionY = static_cast<f32>(v.num(1)); s.regionW = static_cast<f32>(v.num(2)); s.regionH = static_cast<f32>(v.num(3)); }
        else if (n == "modulate_color") s.modulate = packColor(&v, s.modulate);
        else if (n == "texture") {
            if (converted.type == VType::Resource) {
                box.texturePath = converted.s;
                int tw = 0, th = 0;
                if (pngDims(c.fs_.toDiskPath(converted.s.rfind("Textures/", 0) == 0 ? "res://" + converted.s.substr(9) : converted.s), tw, th)) { s.texW = static_cast<f32>(tw); s.texH = static_cast<f32>(th); }
            }
        }
        else if (n == "color") s.lineColor = packColor(&v, s.lineColor);
        else if (n == "thickness") s.thickness = f(1);
        else if (n == "grow_begin") s.growBegin = f(1);
        else if (n == "grow_end") s.growEnd = f(1);
        else if (n == "vertical") setFlag(s, kSbVertical, boolOf(&v, false));
    }

    void read(const std::vector<gd::GodotProperty>& props, int line) {
        const BoxFamily* fam = boxFamily(type);
        for (const gd::GodotProperty& p : props) {
            if (ignorable(p.name)) continue;
            bool known = false;
            for (const char* k : fam->known) if (p.name == k) { known = true; break; }
            if (!known)
                for (const auto& a : fam->approx)
                    if (p.name == a.first) {
                        known = true;
                        ++approximated;
                        c.report_.add("property", doc.file, w.node, type, p.name, Rating::Approximated, a.second, p.line);
                        break;
                    }
            if (!known) {
                ++unknown;
                c.report_.add("property", doc.file, w.node, type, p.name, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
                continue;
            }
            Var converted = c.convertValue(p.value, doc, w, p.name);
            apply(p, converted);
            entries.emplace_back(p.name, std::move(converted));
        }
        c.report_.add("resource", doc.file, w.node, type, "", unknown ? Rating::Approximated : Rating::Converted,
                      "a style box with Godot's property names, drawn by the engine's style box tessellator (aver/ui/UiStyleBox.hpp)" +
                          (approximated ? "; " + std::to_string(approximated) + " property(ies) approximated" : std::string()) +
                          (unknown ? "; " + std::to_string(unknown) + " unknown property(ies) dropped" : std::string()),
                      line);
    }
};

}  // namespace

Var Converter::styleBoxObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line) {
    BoxReader r(*this, type, doc, w);
    r.read(props, line);
    return makeObject(type, std::move(r.entries));
}

std::string Converter::themeAsset(const std::string& key, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w) {
    if (const auto it = themeAssets_.find(key); it != themeAssets_.end()) return it->second;
    std::string stem = safeName(nameHint, "Theme");
    for (int n = 2; themeStemsUsed_.count(stem); ++n) stem = safeName(nameHint, "Theme") + "_" + std::to_string(n);
    themeStemsUsed_.insert(stem);

    ui::UiThemeSheet sheet;
    sheet.name = stem;
    int items = 0, dropped = 0;
    // A theme font is a font file; a FontVariation is carried as its base file (the .octheme names a file), a SystemFont as the default family.
    std::function<std::string(const Var&, const std::string&, int)> fontOf = [&](const Var& v, const std::string& prop, int line) -> std::string {
        if (v.type == VType::ExtResource) {
            const gd::GodotExtResource* e = doc.findExt(v.s);
            const std::string res = e ? fs_.resolve(*e, nullptr, doc.file) : std::string();
            if (!res.empty() && isFontExt(extLower(res))) return fontAsset(res, w);
        }
        if (v.type == VType::SubResource) {
            if (const gd::GodotSubResource* sr = doc.findSub(v.s)) {
                if (sr->type == "FontVariation") {
                    for (const gd::GodotProperty& p : sr->props)
                        if (p.name == "base_font") {
                            report_.add("property", doc.file, w.node, "Theme", prop, Rating::Approximated,
                                        "a FontVariation is carried as its base font file; its embolden, transform and spacing are on the Object(FontVariation) a node or script holds, not in the .octheme", line);
                            return fontOf(p.value, prop, line);
                        }
                }
                if (sr->type == "SystemFont") {
                    report_.add("property", doc.file, w.node, "Theme", prop, Rating::Approximated, "a SystemFont is the default family, whose SYSTEM auto entry reaches the operating system's fonts", line);
                    return {};
                }
            }
        }
        report_.add("property", doc.file, w.node, "Theme", prop, Rating::Unsupported,
                    "only font files (.ttf, .otf, .woff), a FontVariation over one and a SystemFont are carried by a theme", line);
        return {};
    };
    for (const gd::GodotProperty& p : props) {
        const std::string& n = p.name;
        if (ignorable(n)) continue;
        if (n == "default_font") { sheet.defaultFont = fontOf(p.value, n, p.line); continue; }
        if (n == "default_font_size") { sheet.defaultFontSize = static_cast<i32>(std::lround(numOf(&p.value, 16))); continue; }
        if (n == "default_base_scale") { sheet.defaultBaseScale = static_cast<f32>(numOf(&p.value, 1)); continue; }
        const size_t s1 = n.find('/');
        if (s1 == std::string::npos) {
            report_.add("property", doc.file, w.node, "Theme", n, Rating::Unsupported, "not a theme property", p.line);
            ++dropped;
            continue;
        }
        const std::string type = n.substr(0, s1);
        const std::string rest = n.substr(s1 + 1);
        if (rest == "base_type") {
            sheet.variations[type] = stringOf(&p.value);
            ++items;
            continue;
        }
        const size_t s2 = rest.find('/');
        const std::string kind = s2 == std::string::npos ? rest : rest.substr(0, s2);
        const std::string item = s2 == std::string::npos ? std::string() : rest.substr(s2 + 1);
        if (item.empty()) { ++dropped; continue; }
        ui::UiThemeType& t = sheet.types[type];
        if (kind == "colors") { t.colors[item] = packColor(&p.value, 0xFFFFFFFF); ++items; }
        else if (kind == "constants") { t.constants[item] = static_cast<i32>(std::lround(numOf(&p.value, 0))); ++items; }
        else if (kind == "font_sizes") { t.fontSizes[item] = static_cast<i32>(std::lround(numOf(&p.value, 16))); ++items; }
        else if (kind == "fonts") {
            const std::string f = fontOf(p.value, n, p.line);
            if (!f.empty()) { t.fonts[item] = f; ++items; } else ++dropped;
        }
        else if (kind == "icons") {
            const Var c = convertValue(p.value, doc, w, n);
            if (c.type == VType::Resource) { t.icons[item] = c.s; ++items; } else ++dropped;
        }
        else if (kind == "styles") {
            const gd::GodotSubResource* sr = p.value.type == VType::SubResource ? doc.findSub(p.value.s) : nullptr;
            const gd::GodotExtResource* er = p.value.type == VType::ExtResource ? doc.findExt(p.value.s) : nullptr;
            std::string boxType;
            const std::vector<gd::GodotProperty>* boxProps = nullptr;
            const gd::GodotDocBase* boxDoc = &doc;
            int boxLine = p.line;
            if (sr) { boxType = sr->type; boxProps = &sr->props; boxLine = sr->line; }
            else if (er) {
                const std::string res = fs_.resolve(*er, nullptr, doc.file);
                if (const gd::GodotResource* r = res.empty() ? nullptr : resourceDoc(res)) { boxType = r->header.type; boxProps = &r->props; boxDoc = r; boxLine = 0; }
            }
            if (!boxProps || !isStyleBoxType(boxType)) {
                report_.add("property", doc.file, w.node, "Theme", n, Rating::Unsupported, boxType.empty() ? "the style box could not be resolved" : boxType + " is not a style box class the runtime has", p.line);
                ++dropped;
                continue;
            }
            BoxReader r(*this, boxType, *boxDoc, w);
            r.read(*boxProps, boxLine);
            t.styleBoxes[item] = r.box;
            ++items;
        }
        else { report_.add("property", doc.file, w.node, "Theme", n, Rating::Unsupported, "unknown theme item kind '" + kind + "'", p.line); ++dropped; }
    }
    const std::string asset = "Themes/" + stem + ".octheme";
    const std::string dst = contentDir_ + "/" + asset;
    createDirectories(std::filesystem::path(dst).parent_path().string());
    const std::string text = ui::uiThemeWrite(sheet);
    if (!writeFileText(dst, text)) {
        report_.add("resource", doc.file, w.node, "Theme", "", Rating::Unsupported, "could not write " + dst, 0);
        themeAssets_[key] = {};
        return {};
    }
    report_.add("resource", doc.file, w.node, "Theme", "", dropped ? Rating::Approximated : Rating::Converted,
                "written as " + asset + " (" + std::to_string(items) + " item(s)" + (dropped ? ", " + std::to_string(dropped) + " dropped" : std::string()) +
                    "); lookup order: control override, node chain, project theme, built-in default", 0);
    themeAssets_[key] = asset;
    return asset;
}

void Converter::noteUiNode(const FNode& fn) {
    const bool widget = isWidgetClass(fn.type);
    const bool real = isRealUiClass(fn.type) || widget;
    const ContainerProps* own = containerFor(fn.type);
    for (const Prop& pr : fn.props) {
        const std::string& k = pr.key;
        if (k == "_name" || k == "script" || k == "unique_name_in_owner" || k.rfind("metadata/", 0) == 0 || k == "process_mode" || k == "process_priority" ||
            k == "process_physics_priority" || k == "scene_file_path" || k == "editor_description")
            continue;
        const auto row = [&](Rating r, const std::string& why) { report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, r, why, pr.line); };
        if (controlKnown().count(k)) { row(Rating::Converted, "Aver.Fabric Control reads it"); continue; }
        if (k.rfind("theme_override_", 0) == 0) { row(Rating::Converted, "a theme override on the control (Control.add_theme_*_override)"); continue; }
        bool done = false;
        for (const auto& a : controlApprox()) if (k == a.first) { row(Rating::Approximated, a.second); done = true; break; }
        if (done) continue;
        if (controlUnsupported().count(k)) { row(Rating::Unsupported, "canvas item materials, clipping children by shader and render layers have no counterpart in the UI draw list"); continue; }
        if (widget) {   // G5 U2: the widget's own table, then the classes it derives from
            std::string why;
            const int r = widgetPropertyRating(fn.type, k, why);
            if (r == 1) { row(Rating::Converted, why); continue; }
            if (r == 2) { row(Rating::Approximated, why); continue; }
        }
        // The container's own properties, and those of the classes it derives from.
        if (own) {
            for (const char* known : own->known) if (k == known) { done = true; break; }
            if (done) { row(Rating::Converted, "Aver.Fabric " + fn.type + " reads it"); continue; }
            for (const auto& a : own->approx) if (k == a.first) { done = true; row(Rating::Approximated, a.second); break; }
            if (done) continue;
            if (fn.type == "HBoxContainer" || fn.type == "VBoxContainer") {
                if (k == "vertical") { row(Rating::Converted, "Aver.Fabric BoxContainer reads it"); continue; }
            }
        }
        if (real) row(Rating::Unsupported, "Aver.Fabric " + fn.type + " has no such property");
        else row(Rating::Approximated, "carried as node metadata: Aver.Fabric has no " + fn.type + " widget, so it is a placeholder Control");
    }
}

void Converter::convertProjectUi(fmt::ProjectDesc& d) {
    const std::string mode = settingString("display/window/stretch/mode", "disabled");
    if (mode != "disabled") {
        d.uiStretch = mode;
        d.uiAspect = settingString("display/window/stretch/aspect", "keep");
        d.uiBaseW = d.windowWidth;
        d.uiBaseH = d.windowHeight;
        report_.add("setting", kProject, "", "", "display/window/stretch/mode", Rating::Converted, "UI.STRETCH " + mode + ", UI.ASPECT " + d.uiAspect + ", UI.BASESIZE (the viewport size)", 0);
    }
    const std::string scaleMode = settingString("display/window/stretch/scale_mode", "fractional");
    if (scaleMode != "fractional") {
        d.uiStretchScaleMode = scaleMode;
        report_.add("setting", kProject, "", "", "display/window/stretch/scale_mode", Rating::Converted, "UI.STRETCHSCALEMODE " + scaleMode, 0);
    }
    const double scale = setting("display/window/stretch/scale", 1.0);
    if (scale != 1.0 && scale > 0.0) {
        d.uiStretchScale = static_cast<f32>(scale);
        report_.add("setting", kProject, "", "", "display/window/stretch/scale", Rating::Converted, "UI.STRETCHSCALE", 0);
    }
    const double tip = setting("gui/timers/tooltip_delay_sec", 0.5);
    if (tip != 0.5 && tip >= 0.0) {
        d.uiTooltipDelay = static_cast<f32>(tip);
        report_.add("setting", kProject, "", "", "gui/timers/tooltip_delay_sec", Rating::Converted, "UI.TOOLTIPDELAY", 0);
    }
    const std::string themePath = settingString("gui/theme/custom", "");
    if (!themePath.empty()) {
        const std::string res = themePath;
        const gd::GodotResource* r = fs_.exists(res) ? resourceDoc(res) : nullptr;
        if (r && r->header.type == "Theme") {
            const Where w{res, "", "Theme"};
            const std::string asset = themeAsset(res, r->props, *r, stemOf(res), w);
            if (!asset.empty()) {
                d.uiTheme = asset;
                report_.add("setting", kProject, "", "", "gui/theme/custom", Rating::Converted, "UI.THEME " + asset, 0);
            }
        } else {
            report_.add("setting", kProject, "", "", "gui/theme/custom", Rating::Unsupported, themePath + (r ? " is not a Theme resource" : " is missing or a binary resource (resave it as .tres)"), 0);
        }
    }
    // The default font: the project's custom font, else the project theme's default_font, else the first converted font.
    std::string defaultFont;
    const std::string customFont = settingString("gui/theme/custom_font", "");
    if (!customFont.empty()) {
        const std::string res = customFont;
        if (fs_.exists(res) && isFontExt(extLower(res))) {
            defaultFont = fontAsset(res, Where{res, "", ""});
            if (!defaultFont.empty()) report_.add("setting", kProject, "", "", "gui/theme/custom_font", Rating::Converted, "the default family of UI/Fonts.ocfonts is " + stemOf(defaultFont), 0);
        } else {
            report_.add("setting", kProject, "", "", "gui/theme/custom_font", Rating::Unsupported, customFont + " is not a font file the converter can read (a .tres font resource: set the theme's default_font)", 0);
        }
    }
    if (defaultFont.empty() && !d.uiTheme.empty()) {
        std::string text, err;
        ui::UiThemeSheet sheet;
        if (readFileText(contentDir_ + "/" + d.uiTheme, text) && ui::uiThemeParse(text, sheet, &err)) defaultFont = sheet.defaultFont;
    }
    writeFontSet(defaultFont);
}

}  // namespace aver::fabric
