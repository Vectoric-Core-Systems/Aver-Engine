// The .ocfont parser. See UiFont.hpp for why the game UI bakes its fonts rather than rasterising.
#include "aver/ui/UiFont.hpp"

#include <cstdlib>
#include <vector>

namespace aver::ui {
namespace {

// A minimal whitespace splitter. Aver.UI is Core-only by design, so this does NOT reach for
// formats/detail/TextScan.hpp -- pulling Aver.Formats in here to save twelve lines would cost the
// module the property its own CMakeLists exists to state.
std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= line.size()) break;
        const usize start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

f64 num(std::string_view s) {
    // strtod needs a NUL-terminated buffer, and a string_view into the document is not one.
    const std::string tmp(s);
    return std::strtod(tmp.c_str(), nullptr);
}

bool equalsCI(std::string_view a, const char* b) {
    usize i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return i == a.size() && b[i] == '\0';
}

} // namespace

bool parseOcfont(std::string_view text, UiFont& out, std::string* err) {
    out = UiFont{};
    bool sawHeader = false;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;

        // `#` starts a comment. Truncate rather than skip, so a trailing note on a real line is fine.
        const usize hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        const std::vector<std::string_view> t = split(line);
        if (t.empty()) continue;

        if (equalsCI(t[0], "OCFONT")) {
            sawHeader = true;
        } else if (equalsCI(t[0], "NAME") && t.size() > 1) {
            out.name = std::string(t[1]);
        } else if (equalsCI(t[0], "SIZE") && t.size() > 1) {
            out.pixelSize = static_cast<f32>(num(t[1]));
        } else if (equalsCI(t[0], "ATLAS") && t.size() > 1) {
            out.atlasPath = std::string(t[1]);
        } else if (equalsCI(t[0], "METRICS") && t.size() > 3) {
            out.ascent     = static_cast<f32>(num(t[1]));
            out.descent    = static_cast<f32>(num(t[2]));
            out.lineHeight = static_cast<f32>(num(t[3]));
        } else if (equalsCI(t[0], "GLYPH") && t.size() > 10) {
            UiGlyph g;
            const u32 cp = static_cast<u32>(std::strtoul(std::string(t[1]).c_str(), nullptr, 10));
            g.u0 = static_cast<f32>(num(t[2]));
            g.v0 = static_cast<f32>(num(t[3]));
            g.u1 = static_cast<f32>(num(t[4]));
            g.v1 = static_cast<f32>(num(t[5]));
            g.w  = static_cast<f32>(num(t[6]));
            g.h  = static_cast<f32>(num(t[7]));
            g.offX = static_cast<f32>(num(t[8]));
            g.offY = static_cast<f32>(num(t[9]));
            g.advance = static_cast<f32>(num(t[10]));
            out.glyphs[cp] = g;
        }
        // Unknown keys are ignored, matching every other OC format's forward-compatibility rule.
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocfont: no OCFONT header line";
        return false;
    }
    if (out.glyphs.empty()) {
        // A font with no glyphs would draw nothing and report success, which is the shape of failure
        // this engine has been bitten by often enough to refuse outright.
        if (err) *err = "the .ocfont declares no GLYPH rows";
        return false;
    }
    // A lineHeight of zero would stack every line of a multi-line string on one baseline.
    if (out.lineHeight <= 0.0f) out.lineHeight = out.pixelSize > 0.0f ? out.pixelSize * 1.2f : 16.0f;
    return true;
}

f32 uiTextWidth(const UiFont& f, std::string_view text) {
    f32 widest = 0.0f, line = 0.0f;
    for (const char c : text) {
        if (c == '\n') { widest = line > widest ? line : widest; line = 0.0f; continue; }
        if (const UiGlyph* g = f.glyph(static_cast<u32>(static_cast<unsigned char>(c)))) line += g->advance;
    }
    return line > widest ? line : widest;
}

u32 uiTextLines(std::string_view text) {
    u32 n = 1;
    for (const char c : text) if (c == '\n') ++n;
    return n;
}

} // namespace aver::ui
