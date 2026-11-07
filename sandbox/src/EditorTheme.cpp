#include "EditorTheme.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/Json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace aver::editor {

namespace {

EditorTheme g_theme;

f64 linear(f32 c) {
    const f64 v = c;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void note(std::string* why, const std::string& s) {
    if (!why) return;
    if (!why->empty()) *why += "; ";
    *why += s;
}

// A number entry in [lo, hi]; anything else keeps the current value.
void readNum(const fmt::JsonValue& obj, const char* key, f32& dst, f32 lo, f32 hi, std::string* why) {
    if (!obj.has(key)) return;
    const fmt::JsonValue& v = obj[key];
    if (!v.isNumber() || !(v.asDouble() >= lo && v.asDouble() <= hi)) {
        note(why, std::string(key) + " ignored (need a number in " + std::to_string(static_cast<int>(lo)) + ".." +
                      std::to_string(static_cast<int>(hi)) + ")");
        return;
    }
    dst = v.asFloat();
}

void readColour(const fmt::JsonValue& obj, const char* key, Rgba& dst, std::string* why) {
    if (!obj.has(key)) return;
    Rgba c;
    if (!obj[key].isString() || !parseHexColour(obj[key].asString(), c)) {
        note(why, std::string(key) + " ignored (need \"#rrggbb\" or \"#rrggbbaa\")");
        return;
    }
    dst = c;
}

} // namespace

Rgba mixToWhite(const Rgba& c, f32 t) {
    return {c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t, c.b + (1.0f - c.b) * t, c.a};
}

f64 relativeLuminance(const Rgba& c) {
    return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

f64 contrastRatio(const Rgba& a, const Rgba& b) {
    const f64 la = relativeLuminance(a), lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

bool parseHexColour(std::string_view s, Rgba& out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    int v[4] = {0, 0, 0, 255};
    for (usize i = 0; i < (s.size() - 1) / 2; ++i) {
        const int hi = hexDigit(s[1 + i * 2]), lo = hexDigit(s[2 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        v[i] = hi * 16 + lo;
    }
    out = {v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f};
    return true;
}

bool applyThemeJson(std::string_view json, EditorTheme& t, std::string* why) {
    fmt::JsonValue doc;
    std::string err;
    if (!fmt::parseJson(json, doc, &err) || !doc.isObject()) {
        note(why, err.empty() ? "not a JSON object" : err);
        return false;
    }
    EditorTheme n = t;
    if (doc.has("colors") && doc["colors"].isObject()) {
        const fmt::JsonValue& c = doc["colors"];
        readColour(c, "sunken", n.sunken, why);   readColour(c, "panel", n.panel, why);
        readColour(c, "raised", n.raised, why);   readColour(c, "item", n.item, why);
        readColour(c, "popup", n.popup, why);     readColour(c, "line", n.line, why);
        readColour(c, "text", n.text, why);       readColour(c, "textDim", n.textDim, why);
        readColour(c, "accent", n.accent, why);   readColour(c, "overlayBorder", n.overlayBorder, why);
    }
    if (doc.has("metrics") && doc["metrics"].isObject()) {
        const fmt::JsonValue& m = doc["metrics"];
        readNum(m, "hoverLift", n.hoverLift, 0.0f, 0.5f, why);
        readNum(m, "activeLift", n.activeLift, 0.0f, 0.8f, why);
        readNum(m, "selectionAlpha", n.selectionAlpha, 0.0f, 1.0f, why);
        readNum(m, "overlayAlpha", n.overlayAlpha, 0.0f, 1.0f, why);
        readNum(m, "rounding", n.rounding, 0.0f, 16.0f, why);
        readNum(m, "grabRounding", n.grabRounding, 0.0f, 16.0f, why);
        readNum(m, "windowPaddingX", n.windowPaddingX, 0.0f, 40.0f, why);
        readNum(m, "windowPaddingY", n.windowPaddingY, 0.0f, 40.0f, why);
        readNum(m, "framePaddingX", n.framePaddingX, 0.0f, 40.0f, why);
        readNum(m, "itemSpacingX", n.itemSpacingX, 0.0f, 40.0f, why);
        readNum(m, "itemSpacingY", n.itemSpacingY, 0.0f, 40.0f, why);
        readNum(m, "scrollbarSize", n.scrollbarSize, 4.0f, 40.0f, why);
        readNum(m, "indentSpacing", n.indentSpacing, 0.0f, 60.0f, why);
    }
    if (doc.has("fonts") && doc["fonts"].isObject()) {
        const fmt::JsonValue& f = doc["fonts"];
        readNum(f, "header", n.fontHeader, 8.0f, 48.0f, why);
        readNum(f, "body", n.fontBody, 8.0f, 48.0f, why);
        readNum(f, "small", n.fontSmall, 8.0f, 48.0f, why);
    }
    t = n;
    return true;
}

bool loadThemeFile(const std::string& path, EditorTheme& out, std::string* why) {
    out = EditorTheme{};
    std::ifstream in(path, std::ios::binary);
    if (!in) { note(why, "no file"); return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    EditorTheme t;
    if (!applyThemeJson(ss.str(), t, why)) return false;
    out = t;
    return true;
}

const EditorTheme& editorTheme() { return g_theme; }

void reloadEditorTheme(const std::string& dir) {
    const std::string path = dir + "/editor-theme.json";
    g_theme = EditorTheme{};
    if (!std::ifstream(path, std::ios::binary)) return;   // the ordinary case: the defaults are the look
    EditorTheme t;
    std::string why;
    if (!loadThemeFile(path, t, &why)) {
        AVER_WARN("[Editor] '{}' rejected ({}) -- using the built-in theme", path, why);
        return;
    }
    g_theme = t;
    if (!why.empty()) AVER_WARN("[Editor] '{}': {}", path, why);
}

} // namespace aver::editor
