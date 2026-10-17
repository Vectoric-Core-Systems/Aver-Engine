// Strict UTF-8 and the small character predicates built on the range tables in UiUnicodeData.cpp.
#include "aver/ui/UiUnicode.hpp"

namespace aver::ui {

u32 uiDecodeOne(std::string_view s, usize& i) {
    const usize n = s.size();
    const u32 b0 = static_cast<u8>(s[i]);
    if (b0 < 0x80) { ++i; return b0; }

    usize need = 0;
    u32 cp = 0, minv = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF)       { need = 1; cp = b0 & 0x1Fu; minv = 0x80; }
    else if ((b0 & 0xF0u) == 0xE0u)     { need = 2; cp = b0 & 0x0Fu; minv = 0x800; }
    else if (b0 >= 0xF0 && b0 <= 0xF4)  { need = 3; cp = b0 & 0x07u; minv = 0x10000; }
    else { ++i; return kUiReplacementChar; }   // stray continuation, C0, C1, F5..FF

    if (n - i <= need) { ++i; return kUiReplacementChar; }   // truncated
    for (usize k = 1; k <= need; ++k) {
        const u32 b = static_cast<u8>(s[i + k]);
        if ((b & 0xC0u) != 0x80u) { ++i; return kUiReplacementChar; }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return kUiReplacementChar; }
    i += need + 1;
    return cp;
}

usize uiDecodeUtf8(std::string_view in, std::vector<u32>& out, std::vector<u32>* cpToByte, usize maxCps) {
    out.clear();
    if (cpToByte) cpToByte->clear();
    out.reserve(in.size());
    usize i = 0;
    while (i < in.size()) {
        if (maxCps && out.size() >= maxCps) break;
        if (cpToByte) cpToByte->push_back(static_cast<u32>(i));
        out.push_back(uiDecodeOne(in, i));
    }
    if (cpToByte) cpToByte->push_back(static_cast<u32>(i));
    return out.size();
}

void uiEncodeUtf8Append(std::string& out, u32 cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kUiReplacementChar;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string uiEncodeUtf8(const u32* cps, usize n) {
    std::string s;
    s.reserve(n);
    for (usize i = 0; i < n; ++i) uiEncodeUtf8Append(s, cps[i]);
    return s;
}

bool uiIsControlChar(u32 cp) { return (cp < 0x20 && cp != 0x09) || (cp >= 0x7F && cp <= 0x9F); }

bool uiIsParagraphBreak(u32 cp) { return cp == 0x0A || cp == 0x0D || cp == 0x85 || cp == 0x2029; }

bool uiIsHangingSpace(u32 cp) {
    return cp == 0x20 || cp == 0x09 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A && cp != 0x2007) ||
           cp == 0x205F || cp == 0x3000;
}

const char* uiScriptName(UiScript s) {
    switch (s) {
        case UiScript::Common: return "Common";
        case UiScript::Latin: return "Latin";
        case UiScript::Greek: return "Greek";
        case UiScript::Cyrillic: return "Cyrillic";
        case UiScript::Arabic: return "Arabic";
        case UiScript::Hebrew: return "Hebrew";
        case UiScript::Han: return "Han";
        case UiScript::Hiragana: return "Hiragana";
        case UiScript::Katakana: return "Katakana";
        case UiScript::Hangul: return "Hangul";
        case UiScript::Thai: return "Thai";
        case UiScript::Devanagari: return "Devanagari";
        case UiScript::Other: return "Other";
    }
    return "Other";
}

} // namespace aver::ui
