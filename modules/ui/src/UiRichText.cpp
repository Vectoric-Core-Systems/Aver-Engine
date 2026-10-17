// Rich-text tag parser. See UiRichText.hpp for the (small) tag set.
#include "aver/ui/UiRichText.hpp"

#include <algorithm>
#include <cstdlib>

namespace aver::ui {
namespace {

enum class Kind : u8 { Bold, Italic, Color, Size, Font };

struct Open {
    Kind kind;
    u32 color = 0;
    f32 size = 0;
    std::string font;
};

char lowerAscii(u32 c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : (c < 128 ? static_cast<char>(c) : '\0'); }

// The tag text between '<' and '>' as ASCII lower case (non-ASCII becomes '\0', which matches nothing).
std::string lowered(const std::vector<u32>& s, usize from, usize to) {
    std::string o;
    for (usize i = from; i < to; ++i) o.push_back(lowerAscii(s[i]));
    return o;
}

bool hexDigit(char c, u32& v) {
    if (c >= '0' && c <= '9') { v = static_cast<u32>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { v = static_cast<u32>(c - 'a' + 10); return true; }
    return false;
}

bool parseColor(const std::string& v, u32& out) {
    if (v.size() != 7 && v.size() != 9) return false;
    if (v[0] != '#') return false;
    u32 comp[4] = {0, 0, 0, 255};
    for (usize i = 0; i < (v.size() - 1) / 2; ++i) {
        u32 hi = 0, lo = 0;
        if (!hexDigit(v[1 + i * 2], hi) || !hexDigit(v[2 + i * 2], lo)) return false;
        comp[i] = hi * 16 + lo;
    }
    out = comp[0] | (comp[1] << 8) | (comp[2] << 16) | (comp[3] << 24);
    return true;
}

bool parseSize(const std::string& v, f32& out) {
    if (v.empty() || v.size() > 8) return false;
    for (const char c : v) if (!((c >= '0' && c <= '9') || c == '.')) return false;
    const double d = std::strtod(v.c_str(), nullptr);
    if (!(d > 0.0) || d > 512.0) return false;
    out = static_cast<f32>(d);
    return true;
}

struct Style {
    bool bold = false, italic = false, hasColor = false;
    u32 color = 0;
    f32 size = 0;
    std::string font;
    bool operator==(const Style& o) const {
        return bold == o.bold && italic == o.italic && hasColor == o.hasColor && color == o.color && size == o.size &&
               font == o.font;
    }
};

Style styleOf(const std::vector<Open>& opens) {
    Style s;
    for (const Open& o : opens) {
        switch (o.kind) {
            case Kind::Bold: s.bold = true; break;
            case Kind::Italic: s.italic = true; break;
            case Kind::Color: s.hasColor = true; s.color = o.color; break;
            case Kind::Size: s.size = o.size; break;
            case Kind::Font: s.font = o.font; break;
        }
    }
    return s;
}

} // namespace

usize uiParseRich(std::string_view in, std::vector<u32>& plain, std::vector<UiRichSpan>& spans) {
    std::vector<u32> src;
    uiDecodeUtf8(in, src);
    plain.clear();
    spans.clear();
    plain.reserve(src.size());

    std::vector<Open> opens;
    Style cur;
    u32 runStart = 0;
    usize tags = 0;

    const auto flush = [&]() {
        const u32 end = static_cast<u32>(plain.size());
        if (end > runStart) {
            if (!spans.empty()) {
                // Merge with the previous run when the style is identical.
                UiRichSpan& p = spans.back();
                Style ps;
                ps.bold = p.bold; ps.italic = p.italic; ps.hasColor = p.hasColor; ps.color = p.color; ps.size = p.size; ps.font = p.font;
                if (ps == cur && p.end == runStart) { p.end = end; runStart = end; return; }
            }
            UiRichSpan s;
            s.begin = runStart;
            s.end = end;
            s.bold = cur.bold; s.italic = cur.italic; s.hasColor = cur.hasColor; s.color = cur.color; s.size = cur.size;
            s.font = cur.font;
            spans.push_back(std::move(s));
        }
        runStart = end;
    };
    const auto restyle = [&]() { flush(); cur = styleOf(opens); };

    for (usize i = 0; i < src.size();) {
        if (src[i] != '<') { plain.push_back(src[i++]); continue; }
        if (i + 1 < src.size() && src[i + 1] == '<') { plain.push_back('<'); i += 2; continue; }

        // find the '>' of a candidate tag; a '<' first means this one is plain text
        usize close = i + 1;
        while (close < src.size() && src[close] != '>' && src[close] != '<' && close - i < 80) ++close;
        if (close >= src.size() || src[close] != '>') { plain.push_back('<'); ++i; continue; }

        const std::string body = lowered(src, i + 1, close);
        bool recognised = true;
        if (body == "b") { opens.push_back({Kind::Bold}); restyle(); }
        else if (body == "i") { opens.push_back({Kind::Italic}); restyle(); }
        else if (body == "br" || body == "br/") { plain.push_back('\n'); }
        else if (body == "/b" || body == "/i" || body == "/color" || body == "/size" || body == "/font") {
            const Kind k = body == "/b" ? Kind::Bold : body == "/i" ? Kind::Italic : body == "/color" ? Kind::Color
                         : body == "/size" ? Kind::Size : Kind::Font;
            for (usize j = opens.size(); j-- > 0;)
                if (opens[j].kind == k) { opens.erase(opens.begin() + static_cast<std::ptrdiff_t>(j)); break; }
            restyle();
        } else if (body.compare(0, 6, "color=") == 0) {
            Open o{Kind::Color};
            if (parseColor(body.substr(6), o.color)) { opens.push_back(o); restyle(); }
            else recognised = false;
        } else if (body.compare(0, 5, "size=") == 0) {
            Open o{Kind::Size};
            if (parseSize(body.substr(5), o.size)) { opens.push_back(o); restyle(); }
            else recognised = false;
        } else if (body.compare(0, 5, "font=") == 0) {
            // The family keeps its case: read it from the original text.
            std::string name;
            for (usize j = i + 6; j < close; ++j) uiEncodeUtf8Append(name, src[j]);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            if (!name.empty()) { Open o{Kind::Font}; o.font = name; opens.push_back(o); restyle(); }
            else recognised = false;
        } else {
            recognised = false;
        }

        if (recognised) { ++tags; i = close + 1; }
        else { plain.push_back('<'); ++i; }
    }
    flush();
    return tags;
}

std::string uiStripRichText(std::string_view in) {
    std::vector<u32> plain;
    std::vector<UiRichSpan> spans;
    uiParseRich(in, plain, spans);
    return uiEncodeUtf8(plain);
}

} // namespace aver::ui
