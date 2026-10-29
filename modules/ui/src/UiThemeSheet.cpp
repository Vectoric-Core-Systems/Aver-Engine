// The .octheme resource: model, text form, lookup order and the built-in Godot-like default (UiThemeSheet.hpp).
#include "aver/ui/UiThemeSheet.hpp"

#include "aver/core/CharConv.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace aver::ui {

namespace {

// ---- text helpers --------------------------------------------------------------------------------------------------------

std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool any = false, quoted = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
            if (any) { out.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

std::string quoteIf(const std::string& s) {
    if (s.empty() || s.find_first_of(" \t\"=") != std::string::npos) return "\"" + s + "\"";
    return s;
}

std::string numStr(f32 v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}

bool parseF(std::string_view s, f32& out) {
    f32 v = 0;
    const auto r = fromChars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

bool parseI(std::string_view s, i32& out) {
    i32 v = 0;
    const auto r = fromChars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

// #RRGGBBAA <-> 0xAABBGGRR
std::string colorStr(u32 c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%02X%02X%02X%02X", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF);
    return b;
}

bool parseColor(std::string_view s, u32& out) {
    if (s.size() != 9 || s[0] != '#') return false;
    u32 v[4];
    for (int i = 0; i < 4; ++i) {
        u32 b = 0;
        const auto r = fromChars(s.data() + 1 + i * 2, s.data() + 3 + i * 2, b, 16);
        if (r.ec != std::errc() || r.ptr != s.data() + 3 + i * 2) return false;
        v[i] = b;
    }
    out = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    return true;
}

std::vector<std::string_view> splitComma(std::string_view s) {
    std::vector<std::string_view> out;
    usize at = 0;
    while (true) {
        const usize c = s.find(',', at);
        if (c == std::string_view::npos) { out.push_back(s.substr(at)); break; }
        out.push_back(s.substr(at, c - at));
        at = c + 1;
    }
    return out;
}

std::string floats(std::initializer_list<f32> v) {
    std::string s;
    for (f32 f : v) { if (!s.empty()) s += ','; s += numStr(f); }
    return s;
}

bool parseFloats(std::string_view s, f32* out, usize n) {
    const auto parts = splitComma(s);
    if (parts.size() != n) return false;
    for (usize i = 0; i < n; ++i) if (!parseF(parts[i], out[i])) return false;
    return true;
}

const char* axisName(i32 a) { return a == 1 ? "tile" : a == 2 ? "tile_fit" : "stretch"; }
i32 axisFrom(std::string_view s) { return s == "tile" ? 1 : s == "tile_fit" ? 2 : 0; }

const char* kindName(i32 k) {
    switch (static_cast<UiStyleBoxKind>(k)) {
        case UiStyleBoxKind::Flat: return "flat";
        case UiStyleBoxKind::Texture: return "texture";
        case UiStyleBoxKind::Line: return "line";
        default: return "empty";
    }
}

void writeStyleBox(std::string& out, const std::string& type, const std::string& item, const UiThemeStyleBox& tb) {
    const UiStyleBox& s = tb.box;
    const UiStyleBox d;   // fields at their default are not written
    out += "STYLEBOX " + type + " " + item + " " + kindName(s.kind);
    const auto kv = [&](const char* k, const std::string& v) { out += " "; out += k; out += "="; out += quoteIf(v); };
    if (s.contentL != d.contentL || s.contentT != d.contentT || s.contentR != d.contentR || s.contentB != d.contentB) kv("content", floats({s.contentL, s.contentT, s.contentR, s.contentB}));
    if (s.expandL != 0 || s.expandT != 0 || s.expandR != 0 || s.expandB != 0) kv("expand", floats({s.expandL, s.expandT, s.expandR, s.expandB}));
    if (s.flags != d.flags) kv("flags", std::to_string(s.flags));
    switch (static_cast<UiStyleBoxKind>(s.kind)) {
        case UiStyleBoxKind::Flat:
            if (s.bgColor != d.bgColor) kv("bg", colorStr(s.bgColor));
            if (s.borderColorL != d.borderColorL || s.borderColorT != d.borderColorT || s.borderColorR != d.borderColorR || s.borderColorB != d.borderColorB) {
                if (s.borderColorL == s.borderColorT && s.borderColorT == s.borderColorR && s.borderColorR == s.borderColorB) kv("border_color", colorStr(s.borderColorL));
                else kv("border_colors", colorStr(s.borderColorL) + "," + colorStr(s.borderColorT) + "," + colorStr(s.borderColorR) + "," + colorStr(s.borderColorB));
            }
            if (s.borderL != 0 || s.borderT != 0 || s.borderR != 0 || s.borderB != 0) kv("border", floats({s.borderL, s.borderT, s.borderR, s.borderB}));
            if (s.radiusTL != 0 || s.radiusTR != 0 || s.radiusBR != 0 || s.radiusBL != 0) kv("radius", floats({s.radiusTL, s.radiusTR, s.radiusBR, s.radiusBL}));
            if (s.cornerDetail != d.cornerDetail) kv("detail", std::to_string(s.cornerDetail));
            if (s.aaSize != d.aaSize) kv("aa_size", numStr(s.aaSize));
            if (s.skewX != 0 || s.skewY != 0) kv("skew", floats({s.skewX, s.skewY}));
            if (s.shadowColor != d.shadowColor) kv("shadow_color", colorStr(s.shadowColor));
            if (s.shadowSize != 0) kv("shadow_size", numStr(s.shadowSize));
            if (s.shadowOffX != 0 || s.shadowOffY != 0) kv("shadow_offset", floats({s.shadowOffX, s.shadowOffY}));
            break;
        case UiStyleBoxKind::Texture:
            if (!tb.texturePath.empty()) kv("tex", tb.texturePath);
            if (s.texW != 0 || s.texH != 0) kv("tex_size", floats({s.texW, s.texH}));
            if (s.regionW > 0 && s.regionH > 0) kv("region", floats({s.regionX, s.regionY, s.regionW, s.regionH}));
            if (s.patchL != 0 || s.patchT != 0 || s.patchR != 0 || s.patchB != 0) kv("patch", floats({s.patchL, s.patchT, s.patchR, s.patchB}));
            if (s.axisH != 0) kv("axis_h", axisName(s.axisH));
            if (s.axisV != 0) kv("axis_v", axisName(s.axisV));
            if (s.modulate != d.modulate) kv("modulate", colorStr(s.modulate));
            break;
        case UiStyleBoxKind::Line:
            if (s.lineColor != d.lineColor) kv("color", colorStr(s.lineColor));
            if (s.thickness != d.thickness) kv("thickness", numStr(s.thickness));
            if (s.growBegin != d.growBegin || s.growEnd != d.growEnd) kv("grow", floats({s.growBegin, s.growEnd}));
            break;
        default: break;
    }
    out += '\n';
}

bool readStyleBox(const std::vector<std::string>& t, UiThemeStyleBox& tb, std::string& why) {
    UiStyleBox& s = tb.box;
    const std::string& kind = t[3];
    if (kind == "flat") s.kind = static_cast<i32>(UiStyleBoxKind::Flat);
    else if (kind == "texture") s.kind = static_cast<i32>(UiStyleBoxKind::Texture);
    else if (kind == "line") s.kind = static_cast<i32>(UiStyleBoxKind::Line);
    else if (kind == "empty") s.kind = static_cast<i32>(UiStyleBoxKind::Empty);
    else { why = "unknown style box kind '" + kind + "'"; return false; }
    for (usize i = 4; i < t.size(); ++i) {
        const usize eq = t[i].find('=');
        if (eq == std::string::npos) { why = "expected key=value, got '" + t[i] + "'"; return false; }
        const std::string k = t[i].substr(0, eq);
        const std::string v = t[i].substr(eq + 1);
        f32 f[4] = {0, 0, 0, 0};
        i32 n = 0;
        bool ok = true;
        if (k == "content") { ok = parseFloats(v, f, 4); s.contentL = f[0]; s.contentT = f[1]; s.contentR = f[2]; s.contentB = f[3]; }
        else if (k == "expand") { ok = parseFloats(v, f, 4); s.expandL = f[0]; s.expandT = f[1]; s.expandR = f[2]; s.expandB = f[3]; }
        else if (k == "flags") { ok = parseI(v, n); s.flags = static_cast<u32>(n); }
        else if (k == "bg") ok = parseColor(v, s.bgColor);
        else if (k == "border_color") { ok = parseColor(v, s.borderColorL); s.borderColorT = s.borderColorR = s.borderColorB = s.borderColorL; }
        else if (k == "border_colors") {
            const auto p = splitComma(v);
            ok = p.size() == 4 && parseColor(p[0], s.borderColorL) && parseColor(p[1], s.borderColorT) && parseColor(p[2], s.borderColorR) && parseColor(p[3], s.borderColorB);
        }
        else if (k == "border") { ok = parseFloats(v, f, 4); s.borderL = f[0]; s.borderT = f[1]; s.borderR = f[2]; s.borderB = f[3]; }
        else if (k == "radius") { ok = parseFloats(v, f, 4); s.radiusTL = f[0]; s.radiusTR = f[1]; s.radiusBR = f[2]; s.radiusBL = f[3]; }
        else if (k == "detail") ok = parseI(v, s.cornerDetail);
        else if (k == "aa_size") ok = parseF(v, s.aaSize);
        else if (k == "skew") { ok = parseFloats(v, f, 2); s.skewX = f[0]; s.skewY = f[1]; }
        else if (k == "shadow_color") ok = parseColor(v, s.shadowColor);
        else if (k == "shadow_size") ok = parseF(v, s.shadowSize);
        else if (k == "shadow_offset") { ok = parseFloats(v, f, 2); s.shadowOffX = f[0]; s.shadowOffY = f[1]; }
        else if (k == "tex") tb.texturePath = v;
        else if (k == "tex_size") { ok = parseFloats(v, f, 2); s.texW = f[0]; s.texH = f[1]; }
        else if (k == "region") { ok = parseFloats(v, f, 4); s.regionX = f[0]; s.regionY = f[1]; s.regionW = f[2]; s.regionH = f[3]; }
        else if (k == "patch") { ok = parseFloats(v, f, 4); s.patchL = f[0]; s.patchT = f[1]; s.patchR = f[2]; s.patchB = f[3]; }
        else if (k == "axis_h") s.axisH = axisFrom(v);
        else if (k == "axis_v") s.axisV = axisFrom(v);
        else if (k == "modulate") ok = parseColor(v, s.modulate);
        else if (k == "color") ok = parseColor(v, s.lineColor);
        else if (k == "thickness") ok = parseF(v, s.thickness);
        else if (k == "grow") { ok = parseFloats(v, f, 2); s.growBegin = f[0]; s.growEnd = f[1]; }
        else continue;   // a key a newer build wrote
        if (!ok) { why = "bad value for '" + k + "'"; return false; }
    }
    return true;
}

template <class T>
const T* findIn(const UiThemeSheet& th, const std::string& type, const std::map<std::string, T> UiThemeType::*member, std::string_view name) {
    const auto t = th.types.find(type);
    if (t == th.types.end()) return nullptr;
    const auto& m = t->second.*member;
    const auto it = m.find(std::string(name));
    return it == m.end() ? nullptr : &it->second;
}

template <class T>
const UiThemeSheet* lookup(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name,
                           const std::map<std::string, T> UiThemeType::*member, T& out) {
    for (const UiThemeSheet* th : themes) {
        if (!th) continue;
        for (const std::string& ty : types) {
            if (const T* v = findIn<T>(*th, ty, member, name)) { out = *v; return th; }
        }
    }
    return nullptr;
}

} // namespace

bool UiThemeSheet::hasItem(UiThemeItemKind kind, std::string_view type, std::string_view item) const {
    const auto t = types.find(std::string(type));
    if (t == types.end()) return false;
    const std::string k(item);
    switch (kind) {
        case UiThemeItemKind::Color: return t->second.colors.count(k) != 0;
        case UiThemeItemKind::Constant: return t->second.constants.count(k) != 0;
        case UiThemeItemKind::Font: return t->second.fonts.count(k) != 0;
        case UiThemeItemKind::FontSize: return t->second.fontSizes.count(k) != 0;
        case UiThemeItemKind::Icon: return t->second.icons.count(k) != 0;
        case UiThemeItemKind::StyleBox: return t->second.styleBoxes.count(k) != 0;
    }
    return false;
}

usize UiThemeSheet::itemCount() const {
    usize n = 0;
    for (const auto& [_, t] : types) n += t.colors.size() + t.constants.size() + t.fonts.size() + t.fontSizes.size() + t.icons.size() + t.styleBoxes.size();
    return n;
}

bool uiThemeParse(std::string_view text, UiThemeSheet& out, std::string* err) {
    UiThemeSheet th;
    bool header = false;
    usize lineNo = 0, at = 0;
    const auto fail = [&](const std::string& why) {
        if (err) *err = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    while (at <= text.size()) {
        usize nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        ++lineNo;
        const std::vector<std::string> t = tokens(line);
        if (t.empty() || t[0][0] == '#') { if (nl == text.size()) break; continue; }
        const std::string& rec = t[0];
        if (!header) {
            i32 v = 0;
            if (rec != "OCTHEME" || t.size() < 2 || !parseI(t[1], v)) return fail("expected 'OCTHEME <version>'");
            if (v < 1) return fail("bad version");
            header = true;
            if (nl == text.size()) break;
            continue;
        }
        std::string why;
        if (rec == "NAME") { if (t.size() >= 2) th.name = t[1]; }
        else if (rec == "DEFAULTFONT") { if (t.size() >= 2) th.defaultFont = t[1]; }
        else if (rec == "DEFAULTFONTSIZE") { i32 v; if (t.size() < 2 || !parseI(t[1], v)) return fail("bad DEFAULTFONTSIZE"); th.defaultFontSize = v; }
        else if (rec == "BASESCALE") { f32 v; if (t.size() < 2 || !parseF(t[1], v)) return fail("bad BASESCALE"); th.defaultBaseScale = v; }
        else if (rec == "VARIATION") { if (t.size() < 3) return fail("VARIATION needs a name and a base"); th.variations[t[1]] = t[2]; }
        else if (rec == "COLOR") { u32 c; if (t.size() < 4 || !parseColor(t[3], c)) return fail("bad COLOR"); th.types[t[1]].colors[t[2]] = c; }
        else if (rec == "CONSTANT") { i32 v; if (t.size() < 4 || !parseI(t[3], v)) return fail("bad CONSTANT"); th.types[t[1]].constants[t[2]] = v; }
        else if (rec == "FONT") { if (t.size() < 4) return fail("bad FONT"); th.types[t[1]].fonts[t[2]] = t[3]; }
        else if (rec == "FONTSIZE") { i32 v; if (t.size() < 4 || !parseI(t[3], v)) return fail("bad FONTSIZE"); th.types[t[1]].fontSizes[t[2]] = v; }
        else if (rec == "ICON") { if (t.size() < 4) return fail("bad ICON"); th.types[t[1]].icons[t[2]] = t[3]; }
        else if (rec == "STYLEBOX") {
            if (t.size() < 4) return fail("bad STYLEBOX");
            UiThemeStyleBox tb;
            if (!readStyleBox(t, tb, why)) return fail(why);
            th.types[t[1]].styleBoxes[t[2]] = std::move(tb);
        }
        // unknown records are skipped: a file written by a newer build still loads
        if (nl == text.size()) break;
    }
    if (!header) { lineNo = 1; return fail("empty document"); }
    out = std::move(th);
    return true;
}

std::string uiThemeWrite(const UiThemeSheet& sheet) {
    std::string out = "OCTHEME " + std::to_string(kUiThemeVersion) + "\n";
    if (!sheet.name.empty()) out += "NAME " + quoteIf(sheet.name) + "\n";
    if (!sheet.defaultFont.empty()) out += "DEFAULTFONT " + quoteIf(sheet.defaultFont) + "\n";
    if (sheet.defaultFontSize >= 0) out += "DEFAULTFONTSIZE " + std::to_string(sheet.defaultFontSize) + "\n";
    if (sheet.defaultBaseScale > 0) out += "BASESCALE " + numStr(sheet.defaultBaseScale) + "\n";
    for (const auto& [v, b] : sheet.variations) out += "VARIATION " + quoteIf(v) + " " + quoteIf(b) + "\n";
    for (const auto& [ty, items] : sheet.types) {
        for (const auto& [k, v] : items.colors) out += "COLOR " + quoteIf(ty) + " " + quoteIf(k) + " " + colorStr(v) + "\n";
        for (const auto& [k, v] : items.constants) out += "CONSTANT " + quoteIf(ty) + " " + quoteIf(k) + " " + std::to_string(v) + "\n";
        for (const auto& [k, v] : items.fonts) out += "FONT " + quoteIf(ty) + " " + quoteIf(k) + " " + (v.empty() ? "\"\"" : quoteIf(v)) + "\n";
        for (const auto& [k, v] : items.fontSizes) out += "FONTSIZE " + quoteIf(ty) + " " + quoteIf(k) + " " + std::to_string(v) + "\n";
        for (const auto& [k, v] : items.icons) out += "ICON " + quoteIf(ty) + " " + quoteIf(k) + " " + (v.empty() ? "\"\"" : quoteIf(v)) + "\n";
        for (const auto& [k, v] : items.styleBoxes) writeStyleBox(out, quoteIf(ty), quoteIf(k), v);
    }
    return out;
}

std::vector<std::string> uiThemeTypeList(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& classChain, std::string_view variation) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    const auto push = [&](const std::string& s) { if (!s.empty() && seen.insert(s).second) out.push_back(s); };
    std::string t(variation);
    for (int guard = 0; !t.empty() && guard < 16; ++guard) {
        push(t);
        std::string base;
        for (const UiThemeSheet* th : themes) {
            if (!th) continue;
            const auto it = th->variations.find(t);
            if (it != th->variations.end()) { base = it->second; break; }
        }
        t = base;
    }
    for (const std::string& c : classChain) push(c);
    return out;
}

const UiThemeSheet* uiThemeFindColor(const std::vector<const UiThemeSheet*>& th, const std::vector<std::string>& ty, std::string_view n, u32& out) {
    return lookup<u32>(th, ty, n, &UiThemeType::colors, out);
}
const UiThemeSheet* uiThemeFindConstant(const std::vector<const UiThemeSheet*>& th, const std::vector<std::string>& ty, std::string_view n, i32& out) {
    return lookup<i32>(th, ty, n, &UiThemeType::constants, out);
}
const UiThemeSheet* uiThemeFindFont(const std::vector<const UiThemeSheet*>& th, const std::vector<std::string>& ty, std::string_view n, std::string& out) {
    return lookup<std::string>(th, ty, n, &UiThemeType::fonts, out);
}
const UiThemeSheet* uiThemeFindFontSize(const std::vector<const UiThemeSheet*>& th, const std::vector<std::string>& ty, std::string_view n, i32& out) {
    return lookup<i32>(th, ty, n, &UiThemeType::fontSizes, out);
}
const UiThemeSheet* uiThemeFindIcon(const std::vector<const UiThemeSheet*>& th, const std::vector<std::string>& ty, std::string_view n, std::string& out) {
    return lookup<std::string>(th, ty, n, &UiThemeType::icons, out);
}
const UiThemeSheet* uiThemeFindStyleBox(const std::vector<const UiThemeSheet*>& th, const std::vector<std::string>& ty, std::string_view n, UiThemeStyleBox& out) {
    return lookup<UiThemeStyleBox>(th, ty, n, &UiThemeType::styleBoxes, out);
}

bool uiThemeDefaultFont(const std::vector<const UiThemeSheet*>& themes, std::string& out) {
    for (const UiThemeSheet* th : themes)
        if (th && !th->defaultFont.empty()) { out = th->defaultFont; return true; }
    return false;
}

i32 uiThemeDefaultFontSize(const std::vector<const UiThemeSheet*>& themes, i32 fallback) {
    for (const UiThemeSheet* th : themes)
        if (th && th->defaultFontSize >= 0) return th->defaultFontSize;
    return fallback;
}

// ---- the built-in default ---------------------------------------------------------------------------------------------------

namespace {

u32 rgba(f32 r, f32 g, f32 b, f32 a = 1.0f) {
    const auto q = [](f32 v) { return static_cast<u32>(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f))); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

UiThemeStyleBox flat(u32 bg, f32 radius, f32 border, u32 borderColor, f32 cx, f32 cy) {
    UiThemeStyleBox t;
    UiStyleBox& s = t.box;
    s.kind = static_cast<i32>(UiStyleBoxKind::Flat);
    s.bgColor = bg;
    s.radiusTL = s.radiusTR = s.radiusBR = s.radiusBL = radius;
    s.borderL = s.borderT = s.borderR = s.borderB = border;
    s.borderColorL = s.borderColorT = s.borderColorR = s.borderColorB = borderColor;
    s.contentL = s.contentR = cx;
    s.contentT = s.contentB = cy;
    s.flags |= kSbAntiAlias;
    return t;
}

UiThemeStyleBox emptyBox(f32 cx = -1, f32 cy = -1) {
    UiThemeStyleBox t;
    t.box.kind = static_cast<i32>(UiStyleBoxKind::Empty);
    t.box.contentL = t.box.contentR = cx;
    t.box.contentT = t.box.contentB = cy;
    return t;
}

UiThemeSheet buildDefault() {
    UiThemeSheet th;
    th.name = "godot_default";
    th.defaultFontSize = 16;
    th.defaultBaseScale = 1.0f;

    // Buttons (BaseButton and Button share the look; U2's widgets read these names).
    UiThemeType& btn = th.types["Button"];
    const u32 fBg = rgba(0.1f, 0.1f, 0.1f, 0.6f), hBg = rgba(0.225f, 0.225f, 0.225f, 0.6f), pBg = rgba(0.0f, 0.0f, 0.0f, 0.6f), dBg = rgba(0.1f, 0.1f, 0.1f, 0.3f);
    const u32 edge = rgba(0.8f, 0.8f, 0.8f, 0.0f);
    btn.styleBoxes["normal"] = flat(fBg, 3, 0, edge, 8, 4);
    btn.styleBoxes["hover"] = flat(hBg, 3, 0, edge, 8, 4);
    btn.styleBoxes["pressed"] = flat(pBg, 3, 0, edge, 8, 4);
    btn.styleBoxes["disabled"] = flat(dBg, 3, 0, edge, 8, 4);
    {
        UiThemeStyleBox f = flat(rgba(0, 0, 0, 0), 3, 2, rgba(0.1f, 0.1f, 0.1f, 1.0f), 8, 4);
        f.box.borderColorL = f.box.borderColorT = f.box.borderColorR = f.box.borderColorB = rgba(0.95f, 0.95f, 0.95f, 1.0f);
        f.box.flags &= ~kSbDrawCenter;
        btn.styleBoxes["focus"] = f;
    }
    btn.colors["font_color"] = rgba(0.875f, 0.875f, 0.875f);
    btn.colors["font_hover_color"] = rgba(0.95f, 0.95f, 0.95f);
    btn.colors["font_hover_pressed_color"] = rgba(1.0f, 1.0f, 1.0f);
    btn.colors["font_pressed_color"] = rgba(1.0f, 1.0f, 1.0f);
    btn.colors["font_focus_color"] = rgba(0.95f, 0.95f, 0.95f);
    btn.colors["font_disabled_color"] = rgba(0.875f, 0.875f, 0.875f, 0.5f);
    btn.colors["font_outline_color"] = rgba(0, 0, 0);
    btn.colors["icon_normal_color"] = rgba(1, 1, 1);
    btn.colors["icon_hover_color"] = rgba(1, 1, 1);
    btn.colors["icon_pressed_color"] = rgba(1, 1, 1);
    btn.colors["icon_disabled_color"] = rgba(1, 1, 1, 0.4f);
    btn.constants["h_separation"] = 4;
    btn.constants["icon_max_width"] = 0;
    btn.constants["outline_size"] = 0;

    UiThemeType& label = th.types["Label"];
    label.colors["font_color"] = rgba(1, 1, 1);
    label.colors["font_shadow_color"] = rgba(0, 0, 0, 0);
    label.colors["font_outline_color"] = rgba(0, 0, 0);
    label.constants["shadow_offset_x"] = 1;
    label.constants["shadow_offset_y"] = 1;
    label.constants["outline_size"] = 0;
    label.constants["shadow_outline_size"] = 1;
    label.constants["line_spacing"] = 3;
    label.styleBoxes["normal"] = emptyBox();

    UiThemeType& panel = th.types["Panel"];
    panel.styleBoxes["panel"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, -1, -1);
    th.types["PanelContainer"].styleBoxes["panel"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, -1, -1);

    UiThemeType& tip = th.types["TooltipPanel"];
    tip.styleBoxes["panel"] = flat(rgba(0.0f, 0.0f, 0.0f, 0.9f), 0, 0, edge, 2, 0);
    UiThemeType& tipl = th.types["TooltipLabel"];
    tipl.colors["font_color"] = rgba(1, 1, 1);
    tipl.colors["font_shadow_color"] = rgba(0, 0, 0, 0);
    tipl.constants["shadow_offset_x"] = 1;
    tipl.constants["shadow_offset_y"] = 1;
    tipl.constants["outline_size"] = 0;
    tipl.fontSizes["font_size"] = 14;

    th.types["BoxContainer"].constants["separation"] = 4;
    th.types["HBoxContainer"].constants["separation"] = 4;
    th.types["VBoxContainer"].constants["separation"] = 4;
    th.types["GridContainer"].constants["h_separation"] = 4;
    th.types["GridContainer"].constants["v_separation"] = 4;
    UiThemeType& margin = th.types["MarginContainer"];
    margin.constants["margin_left"] = 0;
    margin.constants["margin_top"] = 0;
    margin.constants["margin_right"] = 0;
    margin.constants["margin_bottom"] = 0;
    UiThemeType& scroll = th.types["ScrollContainer"];
    scroll.styleBoxes["panel"] = emptyBox();
    scroll.constants["scrollbar_h_separation"] = 4;
    scroll.constants["scrollbar_v_separation"] = 4;
    th.types["HScrollBar"].constants["scrollbar_size"] = 12;
    th.types["VScrollBar"].constants["scrollbar_size"] = 12;

    UiThemeType& edit = th.types["LineEdit"];
    edit.styleBoxes["normal"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 4, 4);
    edit.styleBoxes["focus"] = flat(rgba(0, 0, 0, 0), 0, 2, rgba(0.95f, 0.95f, 0.95f), 4, 4);
    edit.styleBoxes["focus"].box.flags &= ~kSbDrawCenter;
    edit.styleBoxes["read_only"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.3f), 0, 0, edge, 4, 4);
    edit.colors["font_color"] = rgba(0.875f, 0.875f, 0.875f);
    edit.colors["font_uneditable_color"] = rgba(0.875f, 0.875f, 0.875f, 0.5f);
    edit.colors["font_placeholder_color"] = rgba(0.875f, 0.875f, 0.875f, 0.6f);
    edit.colors["caret_color"] = rgba(0.95f, 0.95f, 0.95f);
    edit.colors["selection_color"] = rgba(0.5f, 0.5f, 0.5f, 0.35f);
    edit.constants["minimum_character_width"] = 4;
    edit.constants["caret_width"] = 1;
    edit.constants["outline_size"] = 0;

    UiThemeType& slider = th.types["HSlider"];
    slider.styleBoxes["slider"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 2, 0, edge, -1, -1);
    slider.constants["center_grabber"] = 0;
    slider.constants["grabber_offset"] = 0;
    UiThemeType& bar = th.types["ProgressBar"];
    bar.styleBoxes["background"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 2, 2);
    bar.styleBoxes["fill"] = flat(rgba(0.5f, 0.5f, 0.5f, 1.0f), 0, 0, edge, -1, -1);
    bar.colors["font_color"] = rgba(0.875f, 0.875f, 0.875f);
    bar.constants["outline_size"] = 0;

    // ---- the widgets of G5 U2 (docs/plans/fabric/G5_PARITY.md): Godot's default look as flat boxes and numbers, no images ----
    const u32 text = rgba(0.875f, 0.875f, 0.875f), textHover = rgba(0.95f, 0.95f, 0.95f), textDim = rgba(0.875f, 0.875f, 0.875f, 0.5f);
    const u32 panelBg = rgba(0.1f, 0.1f, 0.1f, 0.6f), hoverBg = rgba(0.225f, 0.225f, 0.225f, 0.6f), selBg = rgba(0.3f, 0.5f, 0.8f, 0.5f);
    const auto lineBox = [](u32 color, f32 thickness) {
        UiThemeStyleBox t;
        t.box.kind = static_cast<i32>(UiStyleBoxKind::Line);
        t.box.lineColor = color;
        t.box.thickness = thickness;
        t.box.contentL = t.box.contentR = t.box.contentT = t.box.contentB = -1;
        return t;
    };
    th.types["Button"].styleBoxes["hover_pressed"] = th.types["Button"].styleBoxes["pressed"];
    th.types["Button"].colors["icon_hover_pressed_color"] = rgba(1, 1, 1);
    th.types["CheckBox"].constants["h_separation"] = 4;
    th.types["CheckBox"].constants["check_v_offset"] = 0;
    th.types["CheckButton"].constants["h_separation"] = 4;
    th.types["CheckButton"].constants["check_v_offset"] = 0;
    {
        UiThemeType& link = th.types["LinkButton"];
        link.colors["font_color"] = text;
        link.colors["font_hover_color"] = textHover;
        link.colors["font_pressed_color"] = rgba(1, 1, 1);
        link.colors["font_focus_color"] = textHover;
        link.colors["font_disabled_color"] = textDim;
        link.colors["font_outline_color"] = rgba(0, 0, 0);
        link.constants["outline_size"] = 0;
        link.constants["underline_spacing"] = 2;
        link.styleBoxes["focus"] = btn.styleBoxes["focus"];
    }
    th.types["OptionButton"].constants["arrow_margin"] = 4;
    th.types["OptionButton"].constants["h_separation"] = 4;

    {
        UiThemeType& pm = th.types["PopupMenu"];
        pm.styleBoxes["panel"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.95f), 0, 1, rgba(0.3f, 0.3f, 0.3f, 1.0f), 4, 4);
        pm.styleBoxes["hover"] = flat(hoverBg, 2, 0, edge, -1, -1);
        pm.styleBoxes["separator"] = lineBox(rgba(0.5f, 0.5f, 0.5f, 0.5f), 1);
        pm.styleBoxes["labeled_separator_left"] = lineBox(rgba(0.5f, 0.5f, 0.5f, 0.5f), 1);
        pm.styleBoxes["labeled_separator_right"] = lineBox(rgba(0.5f, 0.5f, 0.5f, 0.5f), 1);
        pm.colors["font_color"] = text;
        pm.colors["font_hover_color"] = textHover;
        pm.colors["font_disabled_color"] = textDim;
        pm.colors["font_accelerator_color"] = rgba(0.7f, 0.7f, 0.7f, 0.8f);
        pm.colors["font_separator_color"] = rgba(0.875f, 0.875f, 0.875f, 0.6f);
        pm.colors["font_outline_color"] = rgba(0, 0, 0);
        pm.constants["h_separation"] = 4;
        pm.constants["v_separation"] = 4;
        pm.constants["item_start_padding"] = 2;
        pm.constants["item_end_padding"] = 2;
        pm.constants["indent"] = 10;
        pm.constants["outline_size"] = 0;
        th.types["PopupPanel"].styleBoxes["panel"] = pm.styleBoxes["panel"];
        th.types["AcceptDialog"].styleBoxes["panel"] = flat(rgba(0.12f, 0.12f, 0.12f, 0.98f), 0, 1, rgba(0.3f, 0.3f, 0.3f, 1.0f), 8, 8);
        th.types["Window"].colors["title_color"] = rgba(0.875f, 0.875f, 0.875f);
        th.types["Window"].constants["title_height"] = 24;
    }

    th.types["VSlider"] = th.types["HSlider"];
    th.types["HSlider"].styleBoxes["grabber_area"] = flat(rgba(0.4f, 0.55f, 0.8f, 0.8f), 2, 0, edge, -1, -1);
    th.types["VSlider"].styleBoxes["grabber_area"] = th.types["HSlider"].styleBoxes["grabber_area"];
    for (const char* name : {"HScrollBar", "VScrollBar"}) {
        UiThemeType& sb = th.types[name];
        sb.styleBoxes["scroll"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 2, 0, edge, -1, -1);
        sb.styleBoxes["scroll_focus"] = sb.styleBoxes["scroll"];
        sb.styleBoxes["grabber"] = flat(rgba(0.8f, 0.8f, 0.8f, 0.4f), 2, 0, edge, -1, -1);
        sb.styleBoxes["grabber_highlight"] = flat(rgba(0.9f, 0.9f, 0.9f, 0.6f), 2, 0, edge, -1, -1);
        sb.styleBoxes["grabber_pressed"] = flat(rgba(1.0f, 1.0f, 1.0f, 0.7f), 2, 0, edge, -1, -1);
    }
    bar.colors["font_outline_color"] = rgba(0, 0, 0);
    bar.colors["font_shadow_color"] = rgba(0, 0, 0, 0);
    bar.constants["shadow_offset_x"] = 1;
    bar.constants["shadow_offset_y"] = 1;
    bar.constants["shadow_outline_size"] = 1;
    edit.colors["font_selected_color"] = rgba(1, 1, 1);
    edit.colors["font_outline_color"] = rgba(0, 0, 0);

    {
        UiThemeType& te = th.types["TextEdit"];
        te.styleBoxes["normal"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 4, 4);
        te.styleBoxes["focus"] = edit.styleBoxes["focus"];
        te.styleBoxes["read_only"] = edit.styleBoxes["read_only"];
        te.colors["font_color"] = text;
        te.colors["font_readonly_color"] = textDim;
        te.colors["font_placeholder_color"] = rgba(0.875f, 0.875f, 0.875f, 0.6f);
        te.colors["font_selected_color"] = rgba(1, 1, 1);
        te.colors["caret_color"] = textHover;
        te.colors["selection_color"] = rgba(0.5f, 0.5f, 0.5f, 0.35f);
        te.colors["font_outline_color"] = rgba(0, 0, 0);
        te.constants["line_spacing"] = 4;
        te.constants["caret_width"] = 1;
        te.constants["outline_size"] = 0;
    }
    {
        UiThemeType& il = th.types["ItemList"];
        il.styleBoxes["panel"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 4, 4);
        il.styleBoxes["focus"] = edit.styleBoxes["focus"];
        il.styleBoxes["selected"] = flat(selBg, 0, 0, edge, -1, -1);
        il.styleBoxes["selected_focus"] = flat(rgba(0.3f, 0.5f, 0.8f, 0.7f), 0, 0, edge, -1, -1);
        il.styleBoxes["hovered"] = flat(hoverBg, 0, 0, edge, -1, -1);
        il.styleBoxes["cursor"] = edit.styleBoxes["focus"];
        il.styleBoxes["cursor_unfocused"] = edit.styleBoxes["focus"];
        il.colors["font_color"] = text;
        il.colors["font_hovered_color"] = textHover;
        il.colors["font_selected_color"] = rgba(1, 1, 1);
        il.colors["font_outline_color"] = rgba(0, 0, 0);
        il.colors["guide_color"] = rgba(0.7f, 0.7f, 0.7f, 0.25f);
        il.constants["v_separation"] = 4;
        il.constants["h_separation"] = 4;
        il.constants["icon_margin"] = 4;
        il.constants["line_separation"] = 2;
        il.constants["outline_size"] = 0;
    }
    {
        UiThemeType& tr = th.types["Tree"];
        tr.styleBoxes["panel"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 4, 4);
        tr.styleBoxes["focus"] = edit.styleBoxes["focus"];
        tr.styleBoxes["selected"] = flat(selBg, 0, 0, edge, -1, -1);
        tr.styleBoxes["selected_focus"] = flat(rgba(0.3f, 0.5f, 0.8f, 0.7f), 0, 0, edge, -1, -1);
        tr.styleBoxes["cursor"] = edit.styleBoxes["focus"];
        tr.styleBoxes["cursor_unfocused"] = edit.styleBoxes["focus"];
        tr.styleBoxes["title_button_normal"] = flat(rgba(0.15f, 0.15f, 0.15f, 0.9f), 0, 0, edge, 4, 2);
        tr.styleBoxes["title_button_hover"] = flat(hoverBg, 0, 0, edge, 4, 2);
        tr.styleBoxes["title_button_pressed"] = flat(rgba(0.0f, 0.0f, 0.0f, 0.6f), 0, 0, edge, 4, 2);
        tr.colors["font_color"] = text;
        tr.colors["font_selected_color"] = rgba(1, 1, 1);
        tr.colors["title_button_color"] = text;
        tr.colors["font_outline_color"] = rgba(0, 0, 0);
        tr.colors["guide_color"] = rgba(0.7f, 0.7f, 0.7f, 0.25f);
        tr.constants["v_separation"] = 4;
        tr.constants["h_separation"] = 4;
        tr.constants["item_margin"] = 16;
        tr.constants["button_margin"] = 4;
        tr.constants["draw_guides"] = 1;
        tr.constants["scroll_border"] = 4;
        tr.constants["scroll_speed"] = 12;
        tr.constants["outline_size"] = 0;
    }
    for (const char* name : {"TabBar", "TabContainer"}) {
        UiThemeType& tb = th.types[name];
        tb.styleBoxes["tab_selected"] = flat(rgba(0.2f, 0.2f, 0.2f, 0.9f), 0, 0, edge, 6, 4);
        tb.styleBoxes["tab_unselected"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 6, 4);
        tb.styleBoxes["tab_hovered"] = flat(hoverBg, 0, 0, edge, 6, 4);
        tb.styleBoxes["tab_disabled"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.3f), 0, 0, edge, 6, 4);
        tb.styleBoxes["tab_focus"] = edit.styleBoxes["focus"];
        tb.styleBoxes["tabbar_background"] = emptyBox();
        tb.colors["font_selected_color"] = rgba(1, 1, 1);
        tb.colors["font_unselected_color"] = rgba(0.7f, 0.7f, 0.7f);
        tb.colors["font_hovered_color"] = textHover;
        tb.colors["font_disabled_color"] = textDim;
        tb.colors["font_outline_color"] = rgba(0, 0, 0);
        tb.constants["h_separation"] = 4;
        tb.constants["icon_separation"] = 4;
        tb.constants["outline_size"] = 0;
    }
    th.types["TabContainer"].styleBoxes["panel"] = flat(rgba(0.1f, 0.1f, 0.1f, 0.6f), 0, 0, edge, 4, 4);
    th.types["SplitContainer"].constants["separation"] = 12;
    th.types["SplitContainer"].constants["minimum_grab_thickness"] = 6;
    th.types["SplitContainer"].styleBoxes["split_bar_background"] = emptyBox();
    th.types["FlowContainer"].constants["h_separation"] = 4;
    th.types["FlowContainer"].constants["v_separation"] = 4;
    for (const char* name : {"HSeparator", "VSeparator"}) {
        th.types[name].constants["separation"] = 4;
        th.types[name].styleBoxes["separator"] = lineBox(rgba(0.8f, 0.8f, 0.8f, 0.2f), 4);
    }
    {
        UiThemeType& rt = th.types["RichTextLabel"];
        rt.styleBoxes["normal"] = emptyBox();
        rt.styleBoxes["focus"] = edit.styleBoxes["focus"];
        rt.colors["default_color"] = rgba(1, 1, 1);
        rt.colors["font_selected_color"] = rgba(0, 0, 0, 0);
        rt.colors["selection_color"] = rgba(0.1f, 0.1f, 1.0f, 0.8f);
        rt.colors["font_shadow_color"] = rgba(0, 0, 0, 0);
        rt.colors["font_outline_color"] = rgba(0, 0, 0);
        rt.constants["line_separation"] = 0;
        rt.constants["table_h_separation"] = 3;
        rt.constants["table_v_separation"] = 3;
        rt.constants["outline_size"] = 0;
        rt.constants["shadow_offset_x"] = 1;
        rt.constants["shadow_offset_y"] = 1;
        rt.constants["shadow_outline_size"] = 1;
    }
    th.types["ColorPicker"].constants["sv_width"] = 256;
    th.types["ColorPicker"].constants["sv_height"] = 256;
    th.types["ColorPicker"].constants["h_width"] = 30;
    th.types["ColorPicker"].constants["margin"] = 4;
    return th;
}

} // namespace

const UiThemeSheet& uiGodotDefaultTheme() {
    static const UiThemeSheet th = buildDefault();
    return th;
}

const std::string& uiGodotDefaultThemeText() {
    static const std::string text = uiThemeWrite(uiGodotDefaultTheme());
    return text;
}

} // namespace aver::ui
