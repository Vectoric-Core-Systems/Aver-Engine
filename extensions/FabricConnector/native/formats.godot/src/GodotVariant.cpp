// Godot Variant text literals. See GodotVariant.hpp for the payload layout of each type.
//
// The grammar follows Godot 4's own text serialisation (what its resource and project writers emit),
// plus the Godot 3 spellings a `format=2` file uses (PoolIntArray, Quat, Transform, integer
// ExtResource ids). Written from the documented behaviour, not from Godot's source.
#include "aver/formats/godot/GodotVariant.hpp"

#include "aver/core/CharConv.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace aver::fmt::godot {

namespace {

constexpr int kMaxDepth = 200;

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<u8>& d) {
    std::string out;
    out.reserve((d.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 2 < d.size(); i += 3) {
        const u32 v = (u32(d[i]) << 16) | (u32(d[i + 1]) << 8) | d[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i + 1 == d.size()) {
        const u32 v = u32(d[i]) << 16;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == d.size()) {
        const u32 v = (u32(d[i]) << 16) | (u32(d[i + 1]) << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// False on a character outside the alphabet (whitespace is skipped).
bool base64Decode(std::string_view s, std::vector<u8>& out) {
    u32 acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else return false;
        acc = (acc << 6) | u32(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

struct MathCtor {
    const char* name;
    GodotVType type;
};
constexpr MathCtor kMath[] = {
    {"Vector2", GodotVType::Vector2},     {"Vector2i", GodotVType::Vector2i},
    {"Vector3", GodotVType::Vector3},     {"Vector3i", GodotVType::Vector3i},
    {"Vector4", GodotVType::Vector4},     {"Vector4i", GodotVType::Vector4i},
    {"Rect2", GodotVType::Rect2},         {"Rect2i", GodotVType::Rect2i},
    {"Quaternion", GodotVType::Quaternion}, {"Quat", GodotVType::Quaternion},
    {"Basis", GodotVType::Basis},         {"Transform2D", GodotVType::Transform2D},
    {"Transform3D", GodotVType::Transform3D}, {"Transform", GodotVType::Transform3D},
    {"Plane", GodotVType::Plane},         {"AABB", GodotVType::AABB},
    {"Projection", GodotVType::Projection}, {"Color", GodotVType::Color},
};

struct PackedCtor {
    const char* name;
    GodotVType type;
    int stride;  // numbers per element; 0 = bytes, -1 = strings
};
constexpr PackedCtor kPacked[] = {
    {"PackedByteArray", GodotVType::PackedByteArray, 0},       {"PoolByteArray", GodotVType::PackedByteArray, 0},
    {"PackedInt32Array", GodotVType::PackedInt32Array, 1},     {"PoolIntArray", GodotVType::PackedInt32Array, 1},
    {"PackedInt64Array", GodotVType::PackedInt64Array, 1},
    {"PackedFloat32Array", GodotVType::PackedFloat32Array, 1}, {"PoolRealArray", GodotVType::PackedFloat32Array, 1},
    {"PackedFloat64Array", GodotVType::PackedFloat64Array, 1},
    {"PackedStringArray", GodotVType::PackedStringArray, -1},  {"PoolStringArray", GodotVType::PackedStringArray, -1},
    {"PackedVector2Array", GodotVType::PackedVector2Array, 2}, {"PoolVector2Array", GodotVType::PackedVector2Array, 2},
    {"PackedVector3Array", GodotVType::PackedVector3Array, 3}, {"PoolVector3Array", GodotVType::PackedVector3Array, 3},
    {"PackedVector4Array", GodotVType::PackedVector4Array, 4},
    {"PackedColorArray", GodotVType::PackedColorArray, 4},     {"PoolColorArray", GodotVType::PackedColorArray, 4},
};

bool isIntegerNums(GodotVType t) {
    return t == GodotVType::Vector2i || t == GodotVType::Vector3i || t == GodotVType::Vector4i ||
           t == GodotVType::Rect2i || t == GodotVType::PackedInt32Array || t == GodotVType::PackedInt64Array;
}

// Shortest decimal text that parses back to exactly v (no forced ".0").
std::string fmtNum(f64 v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "inf_neg";
    char buf[40];
    for (int prec : {15, 16, 17}) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

std::string fmtFloat(f64 v) {
    std::string s = fmtNum(v);
    if (s.find_first_of(".eni") == std::string::npos) s += ".0";
    return s;
}

std::string fmtInt(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    return buf;
}

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void writeValue(std::string& out, const GodotVariant& v);

void writeNums(std::string& out, const GodotVariant& v) {
    const bool ints = isIntegerNums(v.type);
    for (usize k = 0; k < v.nums.size(); ++k) {
        if (k) out += ", ";
        out += ints ? fmtInt(v.nums[k]) : fmtNum(v.nums[k]);
    }
}

void writeValue(std::string& out, const GodotVariant& v) {
    switch (v.type) {
        case GodotVType::Nil: out += "null"; return;
        case GodotVType::Bool: out += v.b ? "true" : "false"; return;
        case GodotVType::Int: out += std::to_string(v.i); return;
        case GodotVType::Float: out += fmtFloat(v.f); return;
        case GodotVType::String: writeString(out, v.s); return;
        case GodotVType::StringName: out += '&'; writeString(out, v.s); return;
        case GodotVType::NodePath: out += "NodePath("; writeString(out, v.s); out += ')'; return;
        case GodotVType::Array: {
            if (v.typed) {
                out += "Array[" + v.s + "](";
            }
            out += '[';
            for (usize k = 0; k < v.items.size(); ++k) {
                if (k) out += ", ";
                writeValue(out, v.items[k]);
            }
            out += ']';
            if (v.typed) out += ')';
            return;
        }
        case GodotVType::Dictionary: {
            if (v.typed) out += "Dictionary[" + v.s + ", " + v.s2 + "](";
            out += '{';
            for (usize k = 0; k < v.items.size(); ++k) {
                if (k) out += ", ";
                writeValue(out, v.keys[k]);
                out += ": ";
                writeValue(out, v.items[k]);
            }
            out += '}';
            if (v.typed) out += ')';
            return;
        }
        case GodotVType::Object: {
            out += "Object(" + v.s;
            for (usize k = 0; k < v.items.size(); ++k) {
                out += ", ";
                writeValue(out, v.keys[k]);
                out += ": ";
                writeValue(out, v.items[k]);
            }
            out += ')';
            return;
        }
        case GodotVType::PackedByteArray:
            out += "PackedByteArray(\"" + base64Encode(v.bytes) + "\")";
            return;
        case GodotVType::PackedStringArray: {
            out += "PackedStringArray(";
            for (usize k = 0; k < v.strs.size(); ++k) {
                if (k) out += ", ";
                writeString(out, v.strs[k]);
            }
            out += ')';
            return;
        }
        case GodotVType::ExtResource:
        case GodotVType::SubResource:
            out += godotVTypeName(v.type);
            out += '(';
            writeString(out, v.s);
            out += ')';
            return;
        case GodotVType::Resource: out += "Resource("; writeString(out, v.s); out += ')'; return;
        case GodotVType::Rid: out += "RID(" + std::to_string(v.i) + ")"; return;
        case GodotVType::Callable:
        case GodotVType::Signal: {
            out += godotVTypeName(v.type);
            out += '(';
            for (usize k = 0; k < v.items.size(); ++k) {
                if (k) out += ", ";
                writeValue(out, v.items[k]);
            }
            out += ')';
            return;
        }
        default:
            out += godotVTypeName(v.type);
            out += '(';
            writeNums(out, v);
            out += ')';
            return;
    }
}

bool sameF64(f64 a, f64 b) { return a == b || (std::isnan(a) && std::isnan(b)); }

}  // namespace

const char* godotVTypeName(GodotVType t) {
    switch (t) {
        case GodotVType::Nil: return "Nil";
        case GodotVType::Bool: return "bool";
        case GodotVType::Int: return "int";
        case GodotVType::Float: return "float";
        case GodotVType::String: return "String";
        case GodotVType::StringName: return "StringName";
        case GodotVType::NodePath: return "NodePath";
        case GodotVType::Vector2: return "Vector2";
        case GodotVType::Vector2i: return "Vector2i";
        case GodotVType::Vector3: return "Vector3";
        case GodotVType::Vector3i: return "Vector3i";
        case GodotVType::Vector4: return "Vector4";
        case GodotVType::Vector4i: return "Vector4i";
        case GodotVType::Rect2: return "Rect2";
        case GodotVType::Rect2i: return "Rect2i";
        case GodotVType::Quaternion: return "Quaternion";
        case GodotVType::Basis: return "Basis";
        case GodotVType::Transform2D: return "Transform2D";
        case GodotVType::Transform3D: return "Transform3D";
        case GodotVType::Plane: return "Plane";
        case GodotVType::AABB: return "AABB";
        case GodotVType::Projection: return "Projection";
        case GodotVType::Color: return "Color";
        case GodotVType::Array: return "Array";
        case GodotVType::Dictionary: return "Dictionary";
        case GodotVType::Object: return "Object";
        case GodotVType::PackedByteArray: return "PackedByteArray";
        case GodotVType::PackedInt32Array: return "PackedInt32Array";
        case GodotVType::PackedInt64Array: return "PackedInt64Array";
        case GodotVType::PackedFloat32Array: return "PackedFloat32Array";
        case GodotVType::PackedFloat64Array: return "PackedFloat64Array";
        case GodotVType::PackedStringArray: return "PackedStringArray";
        case GodotVType::PackedVector2Array: return "PackedVector2Array";
        case GodotVType::PackedVector3Array: return "PackedVector3Array";
        case GodotVType::PackedVector4Array: return "PackedVector4Array";
        case GodotVType::PackedColorArray: return "PackedColorArray";
        case GodotVType::ExtResource: return "ExtResource";
        case GodotVType::SubResource: return "SubResource";
        case GodotVType::Resource: return "Resource";
        case GodotVType::Rid: return "RID";
        case GodotVType::Callable: return "Callable";
        case GodotVType::Signal: return "Signal";
    }
    return "?";
}

int godotVTypeNums(GodotVType t) {
    switch (t) {
        case GodotVType::Vector2: case GodotVType::Vector2i: return 2;
        case GodotVType::Vector3: case GodotVType::Vector3i: return 3;
        case GodotVType::Vector4: case GodotVType::Vector4i: case GodotVType::Rect2: case GodotVType::Rect2i:
        case GodotVType::Quaternion: case GodotVType::Plane: case GodotVType::Color: return 4;
        case GodotVType::Basis: return 9;
        case GodotVType::Transform2D: return 6;
        case GodotVType::Transform3D: return 12;
        case GodotVType::AABB: return 6;
        case GodotVType::Projection: return 16;
        default: return 0;
    }
}

// ---- GodotVariant ---------------------------------------------------------------------------------

const std::string& GodotVariant::emptyString() {
    static const std::string e;
    return e;
}

f64 GodotVariant::asNumber(f64 def) const {
    if (type == GodotVType::Int) return static_cast<f64>(i);
    if (type == GodotVType::Float) return f;
    return def;
}

i64 GodotVariant::asInt(i64 def) const {
    if (type == GodotVType::Int) return i;
    if (type == GodotVType::Float && std::isfinite(f)) return static_cast<i64>(f);
    return def;
}

bool GodotVariant::asBool(bool def) const {
    if (type == GodotVType::Bool) return b;
    if (type == GodotVType::Int) return i != 0;
    return def;
}

const std::string& GodotVariant::asString(const std::string& def) const {
    switch (type) {
        case GodotVType::String: case GodotVType::StringName: case GodotVType::NodePath:
        case GodotVType::ExtResource: case GodotVType::SubResource: case GodotVType::Resource:
            return s;
        default: return def;
    }
}

usize GodotVariant::count() const {
    switch (type) {
        case GodotVType::Array: case GodotVType::Dictionary: case GodotVType::Object: return items.size();
        case GodotVType::PackedByteArray: return bytes.size();
        case GodotVType::PackedStringArray: return strs.size();
        case GodotVType::PackedVector2Array: return nums.size() / 2;
        case GodotVType::PackedVector3Array: return nums.size() / 3;
        case GodotVType::PackedVector4Array: case GodotVType::PackedColorArray: return nums.size() / 4;
        case GodotVType::PackedInt32Array: case GodotVType::PackedInt64Array:
        case GodotVType::PackedFloat32Array: case GodotVType::PackedFloat64Array: return nums.size();
        default: return 0;
    }
}

const GodotVariant* GodotVariant::find(std::string_view key) const {
    if (type != GodotVType::Dictionary && type != GodotVType::Object) return nullptr;
    for (usize k = 0; k < keys.size() && k < items.size(); ++k)
        if (keys[k].isStringLike() && keys[k].s == key) return &items[k];
    return nullptr;
}

bool GodotVariant::operator==(const GodotVariant& o) const {
    if (type != o.type || b != o.b || i != o.i || !sameF64(f, o.f) || s != o.s || s2 != o.s2 ||
        typed != o.typed || strs != o.strs || bytes != o.bytes || nums.size() != o.nums.size() ||
        items.size() != o.items.size() || keys.size() != o.keys.size())
        return false;
    for (usize k = 0; k < nums.size(); ++k)
        if (!sameF64(nums[k], o.nums[k])) return false;
    for (usize k = 0; k < items.size(); ++k)
        if (!(items[k] == o.items[k])) return false;
    for (usize k = 0; k < keys.size(); ++k)
        if (!(keys[k] == o.keys[k])) return false;
    return true;
}

std::string godotVariantToText(const GodotVariant& v) {
    std::string out;
    writeValue(out, v);
    return out;
}

bool parseGodotVariant(std::string_view text, GodotVariant& out, GodotDiagnostics* diag, std::string_view file,
                       int firstLine) {
    GodotVariantReader r(text, file, diag, firstLine);
    out = GodotVariant{};
    if (!r.parseValue(out)) return false;
    r.skipSpace(true);
    if (!r.atEnd()) {
        r.error("unexpected text after the value");
        return false;
    }
    return true;
}

// ---- reader ----------------------------------------------------------------------------------------

GodotVariantReader::GodotVariantReader(std::string_view text, std::string_view file, GodotDiagnostics* diag,
                                       int firstLine)
    : text_(text), file_(file), diag_(diag), line_(firstLine) {
    // A UTF-8 byte-order mark opens some files written on Windows.
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF && static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF)
        pos_ = 3;
}

char GodotVariantReader::advance() {
    if (pos_ >= text_.size()) return '\0';
    const char c = text_[pos_++];
    if (c == '\n') ++line_;
    return c;
}

void GodotVariantReader::skipSpace(bool newlines) {
    while (pos_ < text_.size()) {
        const char c = text_[pos_];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++pos_;
        } else if (c == '\n') {
            if (!newlines) return;
            advance();
        } else if (c == ';') {
            while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
        } else {
            return;
        }
    }
}

void GodotVariantReader::skipLine() {
    while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
    if (pos_ < text_.size()) advance();
}

void GodotVariantReader::skipToNextTag() {
    skipLine();
    while (pos_ < text_.size()) {
        usize p = pos_;
        while (p < text_.size() && (text_[p] == ' ' || text_[p] == '\t' || text_[p] == '\r')) ++p;
        if (p < text_.size() && text_[p] == '[') {
            pos_ = p;
            return;
        }
        skipLine();
    }
}

bool GodotVariantReader::consume(char c) {
    if (peek() != c) return false;
    advance();
    return true;
}

bool GodotVariantReader::expect(char c, const char* what) {
    if (consume(c)) return true;
    error(std::string("expected '") + c + "' " + what);
    return false;
}

std::string GodotVariantReader::readIdentifier() {
    const usize start = pos_;
    while (pos_ < text_.size() && isIdentChar(text_[pos_])) ++pos_;
    return std::string(text_.substr(start, pos_ - start));
}

void GodotVariantReader::error(const std::string& message, std::string_view code) { errorAt(line_, message, code); }

void GodotVariantReader::errorAt(int line, const std::string& message, std::string_view code) {
    failed_ = true;
    if (diag_) diag_->error(file_, line, message, code);
}

void GodotVariantReader::warn(const std::string& message) {
    if (diag_) diag_->warn(file_, line_, message);
}

bool GodotVariantReader::parseString(std::string& out) {
    const int startLine = line_;
    if (!consume('"')) {
        error("expected a string");
        return false;
    }
    out.clear();
    while (true) {
        if (atEnd()) {
            errorAt(startLine, "unterminated string");
            return false;
        }
        const char c = advance();
        if (c == '"') return true;
        // Godot writes a CR as "\r"; a raw CR before LF is a CRLF checkout, and the string means LF.
        if (c == '\r' && peek() == '\n') continue;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (atEnd()) {
            errorAt(startLine, "unterminated string");
            return false;
        }
        const char e = advance();
        switch (e) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'v': out += '\v'; break;
            case 'u':
            case 'U': {
                const int digits = e == 'u' ? 4 : 6;
                u32 cp = 0;
                for (int k = 0; k < digits; ++k) {
                    const int h = hexVal(peek());
                    if (h < 0) {
                        error("bad \\u escape");
                        return false;
                    }
                    cp = cp * 16 + static_cast<u32>(h);
                    advance();
                }
                // A UTF-16 surrogate pair spelled as two \u escapes is one code point.
                if (e == 'u' && cp >= 0xD800 && cp < 0xDC00 && peek() == '\\' && peek(1) == 'u') {
                    u32 lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        const int h = hexVal(peek(2 + static_cast<usize>(k)));
                        if (h < 0) { ok = false; break; }
                        lo = lo * 16 + static_cast<u32>(h);
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        for (int k = 0; k < 6; ++k) advance();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += e; break;  // \" \\ \' \/ and anything else stands for itself
        }
    }
}

bool GodotVariantReader::parseNumber(GodotVariant& out) {
    const usize start = pos_;
    bool neg = false;
    if (peek() == '-') { neg = true; advance(); }
    else if (peek() == '+') advance();
    const usize digitsStart = pos_;

    if (isIdentStart(peek())) {  // -inf, +inf, -nan
        const std::string id = readIdentifier();
        if (id == "inf") { out.type = GodotVType::Float; out.f = neg ? -std::numeric_limits<f64>::infinity() : std::numeric_limits<f64>::infinity(); return true; }
        if (id == "inf_neg") { out.type = GodotVType::Float; out.f = -std::numeric_limits<f64>::infinity(); return true; }
        if (id == "nan") { out.type = GodotVType::Float; out.f = std::numeric_limits<f64>::quiet_NaN(); return true; }
        error("unexpected '" + id + "' after a sign");
        return false;
    }

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance(); advance();
        u64 v = 0;
        int n = 0;
        while (hexVal(peek()) >= 0) {
            v = v * 16 + static_cast<u64>(hexVal(peek()));
            advance();
            ++n;
        }
        if (n == 0) {
            error("bad hexadecimal number");
            return false;
        }
        out.type = GodotVType::Int;
        out.i = neg ? -static_cast<i64>(v) : static_cast<i64>(v);
        return true;
    }

    bool isFloat = false;
    while (isDigit(peek())) advance();
    if (peek() == '.' && (isDigit(peek(1)) || pos_ > digitsStart)) {
        isFloat = true;
        advance();
        while (isDigit(peek())) advance();
    }
    if ((peek() == 'e' || peek() == 'E') &&
        (isDigit(peek(1)) || ((peek(1) == '-' || peek(1) == '+') && isDigit(peek(2))))) {
        isFloat = true;
        advance();
        if (peek() == '-' || peek() == '+') advance();
        while (isDigit(peek())) advance();
    }
    if (pos_ == digitsStart || (pos_ == digitsStart + 1 && text_[digitsStart] == '.')) {
        error("bad number");
        return false;
    }
    // from_chars takes no '+'.
    const char* first = text_.data() + start + (text_[start] == '+' ? 1 : 0);
    const char* last = text_.data() + pos_;
    if (!isFloat) {
        i64 v = 0;
        const auto r = aver::fromChars(first, last, v);
        if (r.ec == std::errc() && r.ptr == last) {
            out.type = GodotVType::Int;
            out.i = v;
            return true;
        }
        // Past 64 bits: Godot keeps the magnitude as a float.
    }
    f64 d = 0;
    const auto r = aver::fromChars(first, last, d);
    if (r.ptr != last) {
        error("bad number");
        return false;
    }
    out.type = GodotVType::Float;
    out.f = d;
    return true;
}

bool GodotVariantReader::parseArgs(std::vector<GodotVariant>& args) {
    // The cursor is just past '('.
    while (true) {
        skipSpace(true);
        if (consume(')')) return true;
        GodotVariant v;
        if (!parseValue(v)) return false;
        args.push_back(std::move(v));
        skipSpace(true);
        if (consume(',')) continue;
        if (consume(')')) return true;
        error("expected ',' or ')'");
        return false;
    }
}

bool GodotVariantReader::parseTypeArg(std::string& out) {
    skipSpace(true);
    if (!isIdentStart(peek())) {
        error("expected a type name");
        return false;
    }
    const int line = line_;
    const std::string id = readIdentifier();
    skipSpace(false);
    if (peek() == '(') {  // a script class: ExtResource("id")
        GodotVariant tmp;
        if (!parseConstructor(id, tmp, line)) return false;
        out = godotVariantToText(tmp);
    } else {
        out = id;
    }
    return true;
}

bool GodotVariantReader::parseArray(GodotVariant& out) {
    advance();  // '['
    out.type = GodotVType::Array;
    while (true) {
        skipSpace(true);
        if (consume(']')) return true;
        if (atEnd()) {
            error("unterminated array");
            return false;
        }
        GodotVariant v;
        if (!parseValue(v)) return false;
        out.items.push_back(std::move(v));
        skipSpace(true);
        if (consume(',')) continue;
        if (consume(']')) return true;
        error("expected ',' or ']' in an array");
        return false;
    }
}

bool GodotVariantReader::parseDictionary(GodotVariant& out) {
    advance();  // '{'
    out.type = GodotVType::Dictionary;
    while (true) {
        skipSpace(true);
        if (consume('}')) return true;
        if (atEnd()) {
            error("unterminated dictionary");
            return false;
        }
        GodotVariant k, v;
        if (!parseValue(k)) return false;
        skipSpace(true);
        if (!expect(':', "after a dictionary key")) return false;
        if (!parseValue(v)) return false;
        out.keys.push_back(std::move(k));
        out.items.push_back(std::move(v));
        skipSpace(true);
        if (consume(',')) continue;
        if (consume('}')) return true;
        error("expected ',' or '}' in a dictionary");
        return false;
    }
}

bool GodotVariantReader::parseConstructor(const std::string& name, GodotVariant& out, int line) {
    // The cursor is on '('.
    advance();
    out.line = line;

    // Fixed-size math types and packed numeric arrays: read the numbers straight into nums.
    const MathCtor* math = nullptr;
    for (const MathCtor& m : kMath)
        if (name == m.name) { math = &m; break; }
    const PackedCtor* packed = nullptr;
    for (const PackedCtor& p : kPacked)
        if (name == p.name) { packed = &p; break; }

    auto readNums = [&](std::vector<f64>& dst) -> bool {
        GodotVariant tmp;
        while (true) {
            skipSpace(true);
            if (consume(')')) return true;
            tmp = GodotVariant{};
            if (!parseValue(tmp)) return false;
            if (!tmp.isNumber()) {
                error(name + " takes numbers");
                return false;
            }
            dst.push_back(tmp.asNumber());
            skipSpace(true);
            if (consume(',')) continue;
            if (consume(')')) return true;
            error("expected ',' or ')'");
            return false;
        }
    };

    if (math) {
        out.type = math->type;
        if (!readNums(out.nums)) return false;
        const int want = godotVTypeNums(math->type);
        if (static_cast<int>(out.nums.size()) != want) {
            errorAt(line, name + " takes " + std::to_string(want) + " numbers, got " + std::to_string(out.nums.size()));
            return false;
        }
        return true;
    }

    if (packed) {
        out.type = packed->type;
        if (packed->stride > 0) {
            if (!readNums(out.nums)) return false;
            if (out.nums.size() % static_cast<usize>(packed->stride) != 0) {
                errorAt(line, name + " needs a multiple of " + std::to_string(packed->stride) + " numbers, got " +
                                  std::to_string(out.nums.size()));
                return false;
            }
            return true;
        }
        std::vector<GodotVariant> args;
        if (!parseArgs(args)) return false;
        if (packed->stride == -1) {
            for (const GodotVariant& a : args) {
                if (a.type != GodotVType::String && a.type != GodotVType::StringName) {
                    errorAt(a.line, "PackedStringArray takes strings");
                    return false;
                }
                out.strs.push_back(a.s);
            }
            return true;
        }
        // PackedByteArray: one base64 string (Godot 4.3+), or a list of integers.
        if (args.size() == 1 && args[0].type == GodotVType::String) {
            if (!base64Decode(args[0].s, out.bytes)) {
                errorAt(args[0].line, "PackedByteArray: bad base64");
                return false;
            }
            return true;
        }
        for (const GodotVariant& a : args) {
            if (a.type != GodotVType::Int || a.i < 0 || a.i > 255) {
                errorAt(a.line, "PackedByteArray takes bytes (0..255) or one base64 string");
                return false;
            }
            out.bytes.push_back(static_cast<u8>(a.i));
        }
        return true;
    }

    std::vector<GodotVariant> args;
    if (name == "ExtResource" || name == "SubResource") {
        out.type = name == "ExtResource" ? GodotVType::ExtResource : GodotVType::SubResource;
        if (!parseArgs(args)) return false;
        if (args.size() != 1 || (args[0].type != GodotVType::String && args[0].type != GodotVType::Int)) {
            errorAt(line, name + " takes one id");
            return false;
        }
        out.s = args[0].type == GodotVType::Int ? std::to_string(args[0].i) : args[0].s;
        return true;
    }
    if (name == "NodePath" || name == "StringName" || name == "Resource") {
        out.type = name == "NodePath" ? GodotVType::NodePath : name == "StringName" ? GodotVType::StringName : GodotVType::Resource;
        if (!parseArgs(args)) return false;
        if (args.size() != 1 || !args[0].isStringLike()) {
            errorAt(line, name + " takes one string");
            return false;
        }
        out.s = args[0].s;
        return true;
    }
    if (name == "RID") {
        out.type = GodotVType::Rid;
        if (!parseArgs(args)) return false;
        if (!args.empty()) out.i = args[0].asInt();
        return true;
    }
    if (name == "Callable" || name == "Signal") {
        out.type = name == "Callable" ? GodotVType::Callable : GodotVType::Signal;
        if (!parseArgs(args)) return false;
        out.items = std::move(args);
        return true;
    }
    if (name == "Object") {
        out.type = GodotVType::Object;
        skipSpace(true);
        if (peek() == '"') {
            if (!parseString(out.s)) return false;
        } else {
            out.s = readIdentifier();
            if (out.s.empty()) {
                error("Object( needs a class name");
                return false;
            }
        }
        while (true) {
            skipSpace(true);
            if (consume(')')) return true;
            if (!expect(',', "in Object(...)")) return false;
            skipSpace(true);
            if (consume(')')) return true;  // trailing comma
            GodotVariant k, v;
            if (!parseValue(k)) return false;
            if (!k.isStringLike()) {
                errorAt(k.line, "an Object property name must be a string");
                return false;
            }
            skipSpace(true);
            if (!expect(':', "after an Object property name")) return false;
            if (!parseValue(v)) return false;
            out.keys.push_back(std::move(k));
            out.items.push_back(std::move(v));
        }
    }
    errorAt(line, "unknown type '" + name + "'");
    return false;
}

bool GodotVariantReader::parseIdentifierValue(GodotVariant& out) {
    const int line = line_;
    const std::string id = readIdentifier();
    if (id == "true") { out.type = GodotVType::Bool; out.b = true; return true; }
    if (id == "false") { out.type = GodotVType::Bool; out.b = false; return true; }
    if (id == "null" || id == "nil") { out.type = GodotVType::Nil; return true; }
    if (id == "inf") { out.type = GodotVType::Float; out.f = std::numeric_limits<f64>::infinity(); return true; }
    if (id == "inf_neg") { out.type = GodotVType::Float; out.f = -std::numeric_limits<f64>::infinity(); return true; }
    if (id == "nan") { out.type = GodotVType::Float; out.f = std::numeric_limits<f64>::quiet_NaN(); return true; }

    if (id == "Array" || id == "Dictionary") {
        skipSpace(false);
        const bool isArray = id == "Array";
        if (peek() == '[') {  // typed
            advance();
            std::string t1, t2;
            if (!parseTypeArg(t1)) return false;
            if (!isArray) {
                skipSpace(true);
                if (!expect(',', "between the dictionary key and value types")) return false;
                if (!parseTypeArg(t2)) return false;
            }
            skipSpace(true);
            if (!expect(']', "closing the type list")) return false;
            skipSpace(false);
            if (!expect('(', "after the type list")) return false;
            skipSpace(true);
            if (!parseValue(out)) return false;
            skipSpace(true);
            if (!expect(')', "closing the typed container")) return false;
            if (out.type != (isArray ? GodotVType::Array : GodotVType::Dictionary)) {
                errorAt(line, id + "[...] must wrap " + (isArray ? "an array" : "a dictionary") + " literal");
                return false;
            }
            out.typed = true;
            out.s = t1;
            out.s2 = t2;
            out.line = line;
            return true;
        }
        if (peek() == '(') {  // Array([...]) / Dictionary({...}) with no types
            advance();
            skipSpace(true);
            if (consume(')')) {
                out.type = isArray ? GodotVType::Array : GodotVType::Dictionary;
                return true;
            }
            if (!parseValue(out)) return false;
            skipSpace(true);
            if (!expect(')', "closing the container")) return false;
            if (out.type != (isArray ? GodotVType::Array : GodotVType::Dictionary)) {
                errorAt(line, id + "(...) must wrap " + (isArray ? "an array" : "a dictionary") + " literal");
                return false;
            }
            return true;
        }
        errorAt(line, "'" + id + "' needs [Type](...) or (...)");
        return false;
    }

    skipSpace(false);
    if (peek() != '(') {
        errorAt(line, "unexpected '" + id + "'");
        return false;
    }
    return parseConstructor(id, out, line);
}

bool GodotVariantReader::parseValue(GodotVariant& out) {
    skipSpace(true);
    out = GodotVariant{};
    out.line = line_;
    if (atEnd()) {
        error("expected a value");
        return false;
    }
    if (++depth_ > kMaxDepth) {
        error("value nested too deeply");
        return false;
    }
    struct DepthGuard {
        int& d;
        ~DepthGuard() { --d; }
    } guard{depth_};

    const char c = peek();
    if (c == '"') {
        out.type = GodotVType::String;
        return parseString(out.s);
    }
    if (c == '&' && peek(1) == '"') {
        advance();
        out.type = GodotVType::StringName;
        return parseString(out.s);
    }
    if (c == '^' && peek(1) == '"') {
        advance();
        out.type = GodotVType::NodePath;
        return parseString(out.s);
    }
    if (c == '[') return parseArray(out);
    if (c == '{') return parseDictionary(out);
    if (isDigit(c) || c == '-' || c == '+' || (c == '.' && isDigit(peek(1)))) return parseNumber(out);
    if (isIdentStart(c)) return parseIdentifierValue(out);
    error(std::string("unexpected '") + c + "'");
    return false;
}

}  // namespace aver::fmt::godot
