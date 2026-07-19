#pragma once
// Small, locale-independent text-scanning helpers shared by the .oc* text parsers.
// All views alias the caller's buffer; no allocation except the split vectors.
#include "aver/core/Types.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::detail {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

inline std::string_view trim(std::string_view s) {
    usize b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// One trailing ';' stripped, then re-trimmed (matches OcMap.cs / ParseOcbeam).
inline std::string_view stripTrailingSemicolon(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.back() == ';') s = trim(s.substr(0, s.size() - 1));
    return s;
}

// Truncate at first '#' (OcMap/.scene comment rule).
inline std::string_view truncateHash(std::string_view s) {
    const usize h = s.find('#');
    return h == std::string_view::npos ? s : s.substr(0, h);
}

inline std::vector<std::string_view> splitWhitespace(std::string_view s) {
    std::vector<std::string_view> out;
    usize i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpace(s[i])) ++i;
        const usize start = i;
        while (i < s.size() && !isSpace(s[i])) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

inline std::vector<std::string_view> splitChar(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    usize start = 0;
    for (usize i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

inline bool startsWithCI(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

inline bool equalsCI(std::string_view a, std::string_view b) {
    return a.size() == b.size() && startsWithCI(a, b);
}

// Parse helpers: return the fallback on any failure (mirrors the tolerant readers).
inline f64 parseF64(std::string_view s, f64 fallback = 0.0) {
    s = trim(s);
    f64 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} ? v : fallback;
}

inline i32 parseI32(std::string_view s, i32 fallback = 0) {
    s = trim(s);
    i32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} ? v : fallback;
}

// Accepts 0x-prefixed hex or decimal (matches OcMap.cs ParseU64).
inline u64 parseU64(std::string_view s, u64 fallback = 0) {
    s = trim(s);
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
    u64 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    return r.ec == std::errc{} ? v : fallback;
}

} // namespace aver::fmt::detail
