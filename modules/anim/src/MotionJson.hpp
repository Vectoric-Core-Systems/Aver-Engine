// Small JSON text helpers for the .ocmotion writer, following AnimGraphAsset.cpp's conventions:
// lossless f32 (%.9g), a tag + version gate, whole-file read and write. Private to Aver.Anim.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace aver::anim::mjson {

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

// Object with the right "format" tag and a version this build understands.
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

} // namespace aver::anim::mjson
