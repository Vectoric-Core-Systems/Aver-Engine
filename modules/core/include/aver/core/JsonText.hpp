#pragma once
// A string as JSON text, for the engine's JSON writers: '"' and '\' escaped, \n \r \t by name, other control
// characters as \u00XX, every other byte as it is (UTF-8 stays UTF-8).
#include <cstdio>
#include <string>
#include <string_view>

namespace aver {

inline void appendJsonEscaped(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += b;
                } else {
                    out += c;
                }
        }
    }
}

// The escaped text without quotes.
inline std::string jsonEscaped(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    appendJsonEscaped(o, s);
    return o;
}

// The escaped text in quotes: a complete JSON string value.
inline std::string jsonQuoted(std::string_view s) {
    std::string o = "\"";
    appendJsonEscaped(o, s);
    o += '"';
    return o;
}

} // namespace aver
