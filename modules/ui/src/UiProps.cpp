// The widget property table and the text helpers the layout asset shares.
#include "aver/ui/UiProps.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aver::ui {

namespace {

using P = UiWidgetProps;
constexpr u32 kAll = ~0u;

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

constexpr u32 bit(UiWidgetKind k) { return uiKindBit(k); }
constexpr u32 kTextual = bit(UiWidgetKind::Text) | bit(UiWidgetKind::Button) | bit(UiWidgetKind::Toggle) |
                         bit(UiWidgetKind::Choice) | bit(UiWidgetKind::List) | bit(UiWidgetKind::TextInput) |
                         bit(UiWidgetKind::KeyBind) | bit(UiWidgetKind::ProgressBar) | bit(UiWidgetKind::Slider);
constexpr u32 kLabelled = bit(UiWidgetKind::Text) | bit(UiWidgetKind::Button) | bit(UiWidgetKind::Toggle) |
                          bit(UiWidgetKind::ProgressBar) | bit(UiWidgetKind::KeyBind) | bit(UiWidgetKind::TextInput);
constexpr u32 kValued = bit(UiWidgetKind::Slider) | bit(UiWidgetKind::ProgressBar);
constexpr u32 kInteractive = bit(UiWidgetKind::Button) | bit(UiWidgetKind::Toggle) | bit(UiWidgetKind::Slider) |
                             bit(UiWidgetKind::Choice) | bit(UiWidgetKind::List) | bit(UiWidgetKind::TextInput) |
                             bit(UiWidgetKind::KeyBind);

UiPropDesc head(const char* key, const char* group, UiPropType t, u32 mask, bool rootOnly = false) {
    UiPropDesc d;
    d.key = key;
    d.group = group;
    d.type = t;
    d.kindMask = mask;
    d.rootOnly = rootOnly;
    return d;
}

UiPropDesc fprop(const char* key, const char* group, f32 P::*m, f32 lo, f32 hi, u32 mask = kAll) {
    UiPropDesc d = head(key, group, UiPropType::Float, mask);
    d.minV = lo;
    d.maxV = hi;
    d.get = [m](const P& w, std::vector<std::string>& o) { o.push_back(uiFormatFloat(w.*m)); };
    d.set = [m](P& w, const std::vector<std::string>& t) {
        f32 v = 0;
        if (t.size() != 1 || !uiParseFloat(t[0], v)) return false;
        w.*m = v;
        return true;
    };
    return d;
}

UiPropDesc iprop(const char* key, const char* group, i32 P::*m, f32 lo, f32 hi, u32 mask = kAll,
                 bool rootOnly = false) {
    UiPropDesc d = head(key, group, UiPropType::Int, mask, rootOnly);
    d.minV = lo;
    d.maxV = hi;
    d.get = [m](const P& w, std::vector<std::string>& o) { o.push_back(std::to_string(w.*m)); };
    d.set = [m](P& w, const std::vector<std::string>& t) {
        i32 v = 0;
        if (t.size() != 1 || !uiParseInt(t[0], v)) return false;
        w.*m = v;
        return true;
    };
    return d;
}

UiPropDesc bprop(const char* key, const char* group, bool P::*m, u32 mask = kAll, bool rootOnly = false) {
    UiPropDesc d = head(key, group, UiPropType::Bool, mask, rootOnly);
    d.get = [m](const P& w, std::vector<std::string>& o) { o.push_back((w.*m) ? "true" : "false"); };
    d.set = [m](P& w, const std::vector<std::string>& t) {
        if (t.size() != 1) return false;
        if (ieq(t[0], "true") || t[0] == "1" || ieq(t[0], "yes")) { w.*m = true; return true; }
        if (ieq(t[0], "false") || t[0] == "0" || ieq(t[0], "no")) { w.*m = false; return true; }
        return false;
    };
    return d;
}

UiPropDesc sprop(const char* key, const char* group, std::string P::*m, u32 mask = kAll, bool rootOnly = false) {
    UiPropDesc d = head(key, group, UiPropType::String, mask, rootOnly);
    d.get = [m](const P& w, std::vector<std::string>& o) { o.push_back(w.*m); };
    d.set = [m](P& w, const std::vector<std::string>& t) {
        std::string v;
        for (usize i = 0; i < t.size(); ++i) { if (i) v += ' '; v += t[i]; }
        w.*m = std::move(v);
        return true;
    };
    return d;
}

std::string hexColour(u32 v) {
    if (v == 0) return "none";
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
    return buf;
}

bool parseHexColour(std::string_view s, u32& out) {
    if (ieq(s, "none")) { out = 0; return true; }
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    u32 comp[4] = {0, 0, 0, 255};
    const usize n = (s.size() - 1) / 2;
    for (usize i = 0; i < n; ++i) {
        const std::string pair(s.substr(1 + i * 2, 2));
        char* end = nullptr;
        const unsigned long v = std::strtoul(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) return false;
        comp[i] = static_cast<u32>(v);
    }
    out = uiRgba(comp[0], comp[1], comp[2], comp[3]);
    return true;
}

UiPropDesc cprop(const char* key, const char* group, u32 P::*m, u32 mask = kAll) {
    UiPropDesc d = head(key, group, UiPropType::Color, mask);
    d.get = [m](const P& w, std::vector<std::string>& o) { o.push_back(hexColour(w.*m)); };
    d.set = [m](P& w, const std::vector<std::string>& t) {
        u32 v = 0;
        if (t.size() != 1 || !parseHexColour(t[0], v)) return false;
        w.*m = v;
        return true;
    };
    return d;
}

template <class E>
UiPropDesc eprop(const char* key, const char* group, E P::*m, std::vector<const char*> names,
                 u32 mask = kAll, bool rootOnly = false) {
    UiPropDesc d = head(key, group, UiPropType::Enum, mask, rootOnly);
    d.enumNames = names;
    d.get = [m, names](const P& w, std::vector<std::string>& o) {
        const usize i = static_cast<usize>(w.*m);
        o.push_back(i < names.size() ? names[i] : names[0]);
    };
    d.set = [m, names](P& w, const std::vector<std::string>& t) {
        if (t.size() != 1) return false;
        for (usize i = 0; i < names.size(); ++i)
            if (ieq(t[0], names[i])) { w.*m = static_cast<E>(i); return true; }
        return false;
    };
    return d;
}

// 1, 2 or 4 floats -> four.
bool floats(const std::vector<std::string>& t, f32* out4, bool allowShort) {
    if (t.empty() || t.size() > 4) return false;
    f32 v[4] = {0, 0, 0, 0};
    for (usize i = 0; i < t.size(); ++i) if (!uiParseFloat(t[i], v[i])) return false;
    if (t.size() == 4) { for (int i = 0; i < 4; ++i) out4[i] = v[i]; return true; }
    if (!allowShort) return false;
    if (t.size() == 1) { for (int i = 0; i < 4; ++i) out4[i] = v[0]; return true; }
    if (t.size() == 2) { out4[0] = out4[2] = v[0]; out4[1] = out4[3] = v[1]; return true; }
    return false;
}

void push4(std::vector<std::string>& o, f32 a, f32 b, f32 c, f32 d) {
    o.push_back(uiFormatFloat(a)); o.push_back(uiFormatFloat(b));
    o.push_back(uiFormatFloat(c)); o.push_back(uiFormatFloat(d));
}

UiPropDesc insetsProp(const char* key, const char* group, UiInsets P::*m, u32 mask = kAll) {
    UiPropDesc d = head(key, group, UiPropType::Insets, mask);
    d.get = [m](const P& w, std::vector<std::string>& o) {
        const UiInsets& i = w.*m;
        push4(o, i.left, i.top, i.right, i.bottom);
    };
    d.set = [m](P& w, const std::vector<std::string>& t) {
        f32 v[4];
        if (!floats(t, v, true)) return false;
        w.*m = UiInsets{v[0], v[1], v[2], v[3]};
        return true;
    };
    return d;
}

std::vector<UiPropDesc> build() {
    std::vector<UiPropDesc> v;
    const char* kPlace = "Placement";
    const char* kLay = "Layout";
    const char* kLook = "Appearance";
    const char* kCont = "Content";
    const char* kVal = "Value";
    const char* kNav = "Navigation";
    const char* kRoot = "Root";

    // ---- placement ----
    {
        UiPropDesc d = head("anchors", kPlace, UiPropType::Anchors, kAll);
        d.get = [](const P& w, std::vector<std::string>& o) {
            push4(o, w.anchors.minX, w.anchors.minY, w.anchors.maxX, w.anchors.maxY);
        };
        d.set = [](P& w, const std::vector<std::string>& t) {
            f32 a[4];
            if (t.size() == 2) {   // a point anchor
                if (!floats(t, a, true)) return false;
                w.anchors = UiAnchors{a[0], a[1], a[0], a[1]};
                return true;
            }
            if (!floats(t, a, false)) return false;
            w.anchors = UiAnchors{a[0], a[1], a[2], a[3]};
            return true;
        };
        v.push_back(std::move(d));
    }
    v.push_back(insetsProp("offsets", kPlace, &P::offsets));
    {
        UiPropDesc d = head("pivot", kPlace, UiPropType::Vec2, kAll);
        d.get = [](const P& w, std::vector<std::string>& o) {
            o.push_back(uiFormatFloat(w.pivot.x)); o.push_back(uiFormatFloat(w.pivot.y));
        };
        d.set = [](P& w, const std::vector<std::string>& t) {
            f32 x = 0, y = 0;
            if (t.size() != 2 || !uiParseFloat(t[0], x) || !uiParseFloat(t[1], y)) return false;
            w.pivot = {x, y};
            return true;
        };
        v.push_back(std::move(d));
    }
    v.push_back(fprop("width", kPlace, &P::width, -1, 4000));
    v.push_back(fprop("height", kPlace, &P::height, -1, 4000));
    v.push_back(fprop("minWidth", kPlace, &P::minWidth, 0, 4000));
    v.push_back(fprop("minHeight", kPlace, &P::minHeight, 0, 4000));
    v.push_back(fprop("maxWidth", kPlace, &P::maxWidth, 0, 4000));
    v.push_back(fprop("maxHeight", kPlace, &P::maxHeight, 0, 4000));
    v.push_back(insetsProp("margin", kPlace, &P::margin));
    v.push_back(eprop("alignH", kPlace, &P::alignH, {"start", "center", "end", "stretch"}));
    v.push_back(eprop("alignV", kPlace, &P::alignV, {"start", "center", "end", "stretch"}));
    v.push_back(fprop("fillW", kPlace, &P::fillW, 0, 10));
    v.push_back(fprop("fillH", kPlace, &P::fillH, 0, 10));

    // ---- children layout ----
    v.push_back(insetsProp("padding", kLay, &P::padding));
    v.push_back(eprop("layout", kLay, &P::layout, {"none", "vstack", "hstack", "grid"}));
    v.push_back(fprop("spacing", kLay, &P::spacing, 0, 200));
    v.push_back(fprop("spacingY", kLay, &P::spacingY, -1, 200));
    v.push_back(iprop("columns", kLay, &P::columns, 1, 32));
    v.push_back(fprop("cellWidth", kLay, &P::cellWidth, -1, 2000));
    v.push_back(fprop("cellHeight", kLay, &P::cellHeight, -1, 2000));
    v.push_back(eprop("justify", kLay, &P::justify, {"start", "center", "end", "spacebetween"}));

    // ---- appearance ----
    v.push_back(bprop("visible", kLook, &P::visible));
    v.push_back(bprop("enabled", kLook, &P::enabled));
    v.push_back(fprop("opacity", kLook, &P::opacity, 0, 1));
    v.push_back(bprop("clip", kLook, &P::clip));
    v.push_back(eprop("hit", kLook, &P::hit, {"auto", "always", "never"}));
    v.push_back(sprop("style", kLook, &P::style));
    v.push_back(cprop("bgColor", kLook, &P::bgColor));
    v.push_back(cprop("textColor", kLook, &P::textColor));
    v.push_back(cprop("accentColor", kLook, &P::accentColor));

    // ---- content ----
    v.push_back(sprop("text", kCont, &P::text, kLabelled));
    v.push_back(eprop("textAlign", kCont, &P::textAlign, {"left", "center", "right"}, kTextual));
    v.push_back(fprop("fontSize", kCont, &P::fontSize, 0, 200, kTextual));
    v.push_back(bprop("wrap", kCont, &P::wrap, bit(UiWidgetKind::Text)));
    v.push_back(sprop("image", kCont, &P::image, bit(UiWidgetKind::Image)));
    {
        UiPropDesc d = head("uv", kCont, UiPropType::Uv, bit(UiWidgetKind::Image));
        d.get = [](const P& w, std::vector<std::string>& o) { push4(o, w.uv.u0, w.uv.v0, w.uv.u1, w.uv.v1); };
        d.set = [](P& w, const std::vector<std::string>& t) {
            f32 a[4];
            if (!floats(t, a, false)) return false;
            w.uv = UiUvRect{a[0], a[1], a[2], a[3]};
            return true;
        };
        v.push_back(std::move(d));
    }
    v.push_back(eprop("imageMode", kCont, &P::imageMode, {"stretch", "fit"}, bit(UiWidgetKind::Image)));
    v.push_back(cprop("tint", kCont, &P::tint, bit(UiWidgetKind::Image)));
    v.push_back(sprop("placeholder", kCont, &P::placeholder, bit(UiWidgetKind::TextInput)));
    v.push_back(iprop("maxLength", kCont, &P::maxLength, 0, 4096, bit(UiWidgetKind::TextInput)));
    v.push_back(bprop("masked", kCont, &P::masked, bit(UiWidgetKind::TextInput)));

    // ---- values ----
    v.push_back(fprop("value", kVal, &P::value, 0, 0, kValued));
    v.push_back(fprop("minValue", kVal, &P::minValue, 0, 0, kValued));
    v.push_back(fprop("maxValue", kVal, &P::maxValue, 0, 0, kValued));
    v.push_back(fprop("step", kVal, &P::step, 0, 0, bit(UiWidgetKind::Slider)));
    v.push_back(bprop("showValue", kVal, &P::showValue, kValued));
    v.push_back(bprop("checked", kVal, &P::checked, bit(UiWidgetKind::Toggle)));
    v.push_back(iprop("selected", kVal, &P::selected, 0, 0, bit(UiWidgetKind::Choice) | bit(UiWidgetKind::List)));
    {
        UiPropDesc d = head("items", kVal, UiPropType::StringList,
                            bit(UiWidgetKind::Choice) | bit(UiWidgetKind::List));
        d.get = [](const P& w, std::vector<std::string>& o) { for (const std::string& s : w.items) o.push_back(s); };
        d.set = [](P& w, const std::vector<std::string>& t) { w.items = t; return true; };
        v.push_back(std::move(d));
    }
    v.push_back(fprop("rowHeight", kVal, &P::rowHeight, 0, 200, bit(UiWidgetKind::List)));

    // ---- navigation ----
    v.push_back(bprop("focusable", kNav, &P::focusable, kInteractive | bit(UiWidgetKind::Panel)));
    v.push_back(iprop("tabIndex", kNav, &P::tabIndex, -1, 100));
    v.push_back(sprop("navUp", kNav, &P::navUp));
    v.push_back(sprop("navDown", kNav, &P::navDown));
    v.push_back(sprop("navLeft", kNav, &P::navLeft));
    v.push_back(sprop("navRight", kNav, &P::navRight));
    v.push_back(sprop("command", kNav, &P::command, kInteractive));

    // ---- root ----
    v.push_back(eprop("inputMode", kRoot, &P::inputMode, {"passive", "blocking", "menu"}, kAll, true));
    v.push_back(bprop("modal", kRoot, &P::modal, kAll, true));
    v.push_back(iprop("zOrder", kRoot, &P::zOrder, -100, 100, kAll, true));
    v.push_back(eprop("layer", kRoot, &P::layer, {"background", "content", "overlay", "tooltip", "debug"}, kAll, true));
    v.push_back(sprop("defaultFocus", kRoot, &P::defaultFocus, kAll, true));
    v.push_back(sprop("cancelCommand", kRoot, &P::cancelCommand, kAll, true));
    return v;
}

} // namespace

const std::vector<UiPropDesc>& uiProperties() {
    static const std::vector<UiPropDesc> table = build();
    return table;
}

const UiPropDesc* uiFindProperty(std::string_view key) {
    for (const UiPropDesc& d : uiProperties()) if (ieq(d.key, key)) return &d;
    return nullptr;
}

bool uiGetProp(const UiWidgetProps& w, std::string_view key, std::string& out) {
    const UiPropDesc* d = uiFindProperty(key);
    if (!d) return false;
    std::vector<std::string> tokens;
    d->get(w, tokens);
    out.clear();
    for (usize i = 0; i < tokens.size(); ++i) {
        if (i) out += ' ';
        out += uiQuote(tokens[i]);
    }
    return true;
}

bool uiSetProp(UiWidgetProps& w, std::string_view key, std::string_view text) {
    const UiPropDesc* d = uiFindProperty(key);
    if (!d) return false;
    std::vector<std::string> tokens;
    if (!uiTokenize(text, tokens)) return false;
    // Set into a copy so a rejected value leaves the widget as it was.
    UiWidgetProps copy = w;
    if (!d->set(copy, tokens)) return false;
    w = std::move(copy);
    return true;
}

// ---- text helpers ------------------------------------------------------------------------------

bool uiTokenize(std::string_view s, std::vector<std::string>& out) {
    out.clear();
    usize i = 0;
    const usize n = s.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= n) break;
        std::string tok;
        if (s[i] == '"') {
            ++i;
            bool closed = false;
            while (i < n) {
                const char c = s[i++];
                if (c == '\\' && i < n) {
                    const char e = s[i++];
                    tok.push_back(e == 'n' ? '\n' : (e == 't' ? '\t' : e));
                } else if (c == '"') {
                    closed = true;
                    break;
                } else {
                    tok.push_back(c);
                }
            }
            if (!closed) return false;
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(s[i]))) tok.push_back(s[i++]);
        }
        out.push_back(std::move(tok));
    }
    return true;
}

std::string uiQuote(std::string_view s) {
    bool need = s.empty();
    for (const char c : s)
        if (c == ' ' || c == '"' || c == '\\' || c == '\n' || c == '\t' || c == '\r') { need = true; break; }
    if (!need) return std::string(s);
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') continue;
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string uiFormatFloat(f32 v) {
    if (!std::isfinite(v)) return "0";
    char buf[40];
    for (int p = 6; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, static_cast<double>(v));
        if (std::strtof(buf, nullptr) == v) break;
    }
    return buf;
}

bool uiParseFloat(std::string_view s, f32& out) {
    if (s.empty()) return false;
    const std::string tmp(s);
    char* end = nullptr;
    const double d = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || !std::isfinite(d)) return false;
    out = static_cast<f32>(d);
    return true;
}

bool uiParseInt(std::string_view s, i32& out) {
    if (s.empty()) return false;
    const std::string tmp(s);
    char* end = nullptr;
    const long v = std::strtol(tmp.c_str(), &end, 10);
    if (end != tmp.c_str() + tmp.size()) return false;
    out = static_cast<i32>(v);
    return true;
}

} // namespace aver::ui
