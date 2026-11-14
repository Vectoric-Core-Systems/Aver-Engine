#pragma once
// Struct and array literals as the editor writes them into a pin's value: the C++ side of Aver.Graph's
// GraphStructLiterals / GraphLiterals.TryParseArray. A struct is `{field=value|field=value}` (fields by name, one
// left out reads as zero, a nested struct in its own braces); an array is its elements joined by commas (float3
// elements by semicolons). Header-only and headless so GraphEditorLoadSaveTest drives it.
#include "aver/formats/OcGraph.hpp"

#include <string>
#include <utility>
#include <vector>

namespace aver::editor {

using StructLiteralFields = std::vector<std::pair<std::string, std::string>>;   // field -> its literal text, as written

// A string inside a struct literal: the .ocgraph escapes plus | { } , (they delimit literals and array elements).
inline std::string encodeLiteralString(const std::string& s) {
    std::string out;
    for (const char c : fmt::encodeGraphString(s)) {
        switch (c) {
            case '|': out += "%7C"; break;
            case '{': out += "%7B"; break;
            case '}': out += "%7D"; break;
            case ',': out += "%2C"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Splits on `sep` outside nested braces.
inline std::vector<std::string> splitLiteralTop(const std::string& s, char sep) {
    std::vector<std::string> parts;
    int depth = 0;
    usize start = 0;
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}') --depth;
        else if (s[i] == sep && depth == 0) { parts.push_back(s.substr(start, i - start)); start = i + 1; }
    }
    parts.push_back(s.substr(start));
    return parts;
}

// The fields a literal sets, in the order written; empty for an empty or malformed literal.
inline StructLiteralFields parseStructLiteral(const std::string& text) {
    StructLiteralFields out;
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') return out;
    for (const std::string& part : splitLiteralTop(text.substr(1, text.size() - 2), '|')) {
        const usize eq = part.find('=');
        if (part.empty() || eq == std::string::npos || eq == 0) continue;
        out.emplace_back(part.substr(0, eq), part.substr(eq + 1));
    }
    return out;
}

inline std::string structLiteralField(const std::string& literal, const std::string& field) {
    for (const auto& [k, v] : parseStructLiteral(literal)) if (k == field) return v;
    return {};
}

// `literal` with `field` set to `value` (removed when `value` is empty), fields in `def`'s order. Empty when no
// field is set, which is the zero struct.
inline std::string withStructLiteralField(const std::string& literal, const fmt::OcGraphStruct& def, const std::string& field,
                                          const std::string& value) {
    StructLiteralFields cur = parseStructLiteral(literal);
    std::string out;
    for (const auto& f : def.fields) {
        std::string v = f.name == field ? value : std::string();
        if (f.name != field)
            for (const auto& [k, raw] : cur) if (k == f.name) { v = raw; break; }
        if (v.empty()) continue;
        out += (out.empty() ? "" : "|") + f.name + "=" + v;
    }
    return out.empty() ? out : "{" + out + "}";
}

// What a person types for an array becomes its literal: spaces dropped around elements, a string element escaped.
inline std::string arrayLiteralFromText(const std::string& elemType, const std::string& text) {
    if (elemType != "string") {
        std::string out;
        for (const char c : text) if (c != ' ' && c != '\t') out += c;
        return out;
    }
    std::string out;
    const std::vector<std::string> parts = splitLiteralTop(text, ',');
    if (parts.size() == 1 && parts[0].empty()) return out;
    for (usize i = 0; i < parts.size(); ++i) {
        std::string e = parts[i];
        while (!e.empty() && e.front() == ' ') e.erase(e.begin());
        while (!e.empty() && e.back() == ' ') e.pop_back();
        out += (i ? "," : "") + encodeLiteralString(e);
    }
    return out;
}

// The inverse, for showing an array literal in a text field.
inline std::string arrayTextFromLiteral(const std::string& elemType, const std::string& literal) {
    if (elemType != "string") return literal;
    std::string out;
    const std::vector<std::string> parts = splitLiteralTop(literal, ',');
    for (usize i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + fmt::decodeGraphString(parts[i]);
    return literal.empty() ? std::string() : out;
}

// A one-line summary for the box on the node: `k 3, xs 1,2,3, inner {..}`.
inline std::string structLiteralSummary(const std::string& literal) {
    std::string out;
    for (const auto& [k, v] : parseStructLiteral(literal)) {
        const std::string shown = !v.empty() && v.front() == '{' ? std::string("{..}") : fmt::decodeGraphString(v);
        out += (out.empty() ? "" : ", ") + k + " " + shown;
    }
    return out;
}

} // namespace aver::editor
