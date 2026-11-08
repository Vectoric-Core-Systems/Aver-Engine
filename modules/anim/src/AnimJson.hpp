// JSON writing helpers shared by the wave-5 assets (.ocikrig, .ocretarget). Copies of the ones private to
// AnimGraphAsset.cpp: that file belongs to another feature, so the helpers are duplicated, not hoisted.
#pragma once

#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace aver::anim::ajson {

inline std::string esc(std::string_view s) {
    std::string o = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}

inline std::string num(f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}

inline std::string str(const fmt::JsonValue& v, std::string_view key) { return std::string(v[key].asString()); }

inline bool readFile(const std::string& path, std::string& out, std::string* why) {
    std::ifstream f(aver::nativePath(path), std::ios::binary);
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline bool writeFile(const std::string& path, const std::string& text, std::string* why) {
    std::ofstream f(aver::nativePath(path), std::ios::binary | std::ios::trunc);
    if (!f) { if (why) *why = "cannot write " + path; return false; }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

// The format tag and version gate every asset shares: a different tag is not this asset, a newer version
// is refused rather than misread.
inline bool header(const fmt::JsonValue& root, const char* tag, int version, std::string* why) {
    if (!root.isObject() || root["format"].asString() != tag) {
        if (why) *why = std::string("not a ") + tag + " file";
        return false;
    }
    if (root["version"].asInt() > version) {
        if (why) *why = "file version is newer than this build";
        return false;
    }
    return true;
}

} // namespace aver::anim::ajson
