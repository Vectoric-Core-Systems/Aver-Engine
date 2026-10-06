#pragma once
// Percent-escaping and number spelling shared by the newer text records (.ocprefab and the prefab
// records inside .ocworld). OcWorld.cpp keeps its own private copies of the same escape; those are
// left alone so this header adds nothing to a merge there.
#include "aver/core/Types.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace aver::fmt::detail {

// One token out of any string: whitespace, '%', quotes, '#' and ';' (the two characters the line
// scanner eats before it splits tokens) and control bytes become %XX.
inline std::string pctEncode(std::string_view s) {
    static const char* const hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c <= 0x20 || c == 0x7f || c == '%' || c == '"' || c == '\'' || c == '#' || c == ';') {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// The inverse. A '%' not followed by two hex digits passes through literally.
inline std::string pctDecode(std::string_view s) {
    const auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// A string as exactly one non-empty token: '~' then the escaped text, so "" is "~" and never
// vanishes from a whitespace-split line.
inline std::string strToken(std::string_view s) { return "~" + pctEncode(s); }

// The inverse of strToken. A token without the '~' is taken as plain escaped text.
inline std::string strFromToken(std::string_view t) {
    if (!t.empty() && t.front() == '~') t.remove_prefix(1);
    return pctDecode(t);
}

// A float with enough digits that reading it back gives the identical f32 (%.9g).
inline std::string numF32(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    return buf;
}

// A double with enough digits to round-trip exactly (%.17g).
inline std::string numF64(f64 v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

} // namespace aver::fmt::detail
