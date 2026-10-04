// JSON reader: the recursive-descent parser behind parseJson, plus JsonValue's accessors.
#include "aver/formats/Json.hpp"

#include <cstdlib>
#include <cstring>

namespace aver::fmt {
namespace {

// A shared empty value, returned for a missing key or index.
const JsonValue& nullValue() { static const JsonValue v; return v; }

constexpr int kMaxDepth = 64;

} // namespace

// Elements for an array, members for an object, 0 otherwise.
usize JsonValue::size() const {
    if (type_ == Type::Array)  return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

// Array element, or the null value when out of range.
const JsonValue& JsonValue::operator[](usize i) const {
    if (type_ != Type::Array || i >= arr_.size()) return nullValue();
    return arr_[i];
}

// Object member, or the null value when absent. Last duplicate key wins.
const JsonValue& JsonValue::operator[](std::string_view key) const {
    if (type_ != Type::Object) return nullValue();
    for (usize i = obj_.size(); i-- > 0;) if (obj_[i].key == key) return obj_[i].value;
    return nullValue();
}

// True when the object has this key.
bool JsonValue::has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (const JsonMember& m : obj_) if (m.key == key) return true;
    return false;
}

const std::vector<JsonMember>& JsonValue::members() const { return obj_; }

// Recursive-descent parser over one JSON document.
class JsonParser {
public:
    // Parses `t`, reporting failures through `why`.
    JsonParser(std::string_view t, std::string* why) : t_(t), why_(why) {}

    // Parses one top-level value and requires nothing after it.
    bool run(JsonValue& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (p_ != t_.size()) return err("trailing characters after the top-level value");
        return true;
    }

private:
    std::string_view t_;
    usize p_ = 0;
    std::string* why_;

    // Records a message with the current byte offset and returns false.
    bool err(const std::string& msg) {
        if (why_) *why_ = "JSON: " + msg + " at byte " + std::to_string(p_);
        return false;
    }
    bool eof() const { return p_ >= t_.size(); }
    char cur() const { return t_[p_]; }

    // Skips space, tab, newline and carriage return.
    void skipWs() {
        while (p_ < t_.size()) {
            const char c = t_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p_;
            else break;
        }
    }

    // Consumes `lit` when it is next. Returns false without moving otherwise.
    bool literal(std::string_view lit) {
        if (t_.compare(p_, lit.size(), lit) != 0) return false;
        p_ += lit.size();
        return true;
    }

    // Parses any value at the given nesting depth.
    bool parseValue(JsonValue& v, int depth) {
        if (depth > kMaxDepth) return err("nesting deeper than " + std::to_string(kMaxDepth));
        if (eof()) return err("unexpected end of input");
        switch (cur()) {
            case '{': return parseObject(v, depth);
            case '[': return parseArray(v, depth);
            case '"': {
                v.type_ = JsonValue::Type::String;
                return parseString(v.str_);
            }
            case 't': if (!literal("true"))  return err("expected 'true'");
                      v.type_ = JsonValue::Type::Bool; v.bool_ = true;  return true;
            case 'f': if (!literal("false")) return err("expected 'false'");
                      v.type_ = JsonValue::Type::Bool; v.bool_ = false; return true;
            case 'n': if (!literal("null"))  return err("expected 'null'");
                      v.type_ = JsonValue::Type::Null; return true;
            default:  return parseNumber(v);
        }
    }

    // Parses an object body, cursor on the opening brace.
    bool parseObject(JsonValue& v, int depth) {
        v.type_ = JsonValue::Type::Object;
        ++p_;
        skipWs();
        if (!eof() && cur() == '}') { ++p_; return true; }
        for (;;) {
            skipWs();
            if (eof() || cur() != '"') return err("expected a quoted member name");
            JsonMember m;
            if (!parseString(m.key)) return false;
            skipWs();
            if (eof() || cur() != ':') return err("expected ':' after a member name");
            ++p_;
            skipWs();
            if (!parseValue(m.value, depth + 1)) return false;
            v.obj_.push_back(std::move(m));
            skipWs();
            if (eof()) return err("unterminated object");
            if (cur() == ',') { ++p_; continue; }
            if (cur() == '}') { ++p_; return true; }
            return err("expected ',' or '}' in an object");
        }
    }

    // Parses an array body, cursor on the opening bracket.
    bool parseArray(JsonValue& v, int depth) {
        v.type_ = JsonValue::Type::Array;
        ++p_;
        skipWs();
        if (!eof() && cur() == ']') { ++p_; return true; }
        for (;;) {
            skipWs();
            JsonValue e;
            if (!parseValue(e, depth + 1)) return false;
            v.arr_.push_back(std::move(e));
            skipWs();
            if (eof()) return err("unterminated array");
            if (cur() == ',') { ++p_; continue; }
            if (cur() == ']') { ++p_; return true; }
            return err("expected ',' or ']' in an array");
        }
    }

    // Appends the UTF-8 encoding of a code point.
    static void appendUtf8(std::string& out, u32 cp) {
        if (cp < 0x80) out.push_back(char(cp));
        else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    // Reads four hex digits into `out`.
    bool hex4(u32& out) {
        if (p_ + 4 > t_.size()) return err("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[p_ + i];
            u32 d;
            if (c >= '0' && c <= '9') d = u32(c - '0');
            else if (c >= 'a' && c <= 'f') d = u32(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = u32(c - 'A' + 10);
            else return err("bad hex digit in a \\u escape");
            out = (out << 4) | d;
        }
        p_ += 4;
        return true;
    }

    // Parses a quoted string into `out`, decoding escapes and surrogate pairs.
    bool parseString(std::string& out) {
        out.clear();
        ++p_;
        for (;;) {
            if (eof()) return err("unterminated string");
            const char c = t_[p_];
            if (c == '"') { ++p_; return true; }
            if (c == '\\') {
                ++p_;
                if (eof()) return err("unterminated escape");
                const char e = t_[p_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        u32 cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (p_ + 1 < t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                                p_ += 2;
                                u32 lo;
                                if (!hex4(lo)) return false;
                                if (lo < 0xDC00 || lo > 0xDFFF) return err("unpaired high surrogate");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else return err("unpaired high surrogate");
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return err("unpaired low surrogate");
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return err("unknown escape sequence");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) return err("raw control character in a string");
            out.push_back(c);
            ++p_;
        }
    }

    // Parses a number in the strict JSON grammar into `v`.
    bool parseNumber(JsonValue& v) {
        const usize start = p_;
        if (!eof() && cur() == '-') ++p_;
        if (eof()) return err("truncated number");
        if (cur() == '0') { ++p_; }
        else if (cur() >= '1' && cur() <= '9') { while (!eof() && cur() >= '0' && cur() <= '9') ++p_; }
        else return err("expected a digit");

        if (!eof() && cur() == '.') {
            ++p_;
            if (eof() || cur() < '0' || cur() > '9') return err("expected a digit after '.'");
            while (!eof() && cur() >= '0' && cur() <= '9') ++p_;
        }
        if (!eof() && (cur() == 'e' || cur() == 'E')) {
            ++p_;
            if (!eof() && (cur() == '+' || cur() == '-')) ++p_;
            if (eof() || cur() < '0' || cur() > '9') return err("expected a digit in the exponent");
            while (!eof() && cur() >= '0' && cur() <= '9') ++p_;
        }

        const std::string tok(t_.substr(start, p_ - start));
        v.type_ = JsonValue::Type::Number;
        v.num_ = std::strtod(tok.c_str(), nullptr);
        return true;
    }
};

// Parses a whole document, skipping a leading UTF-8 BOM. Returns false with `why` set on failure.
bool parseJson(std::string_view text, JsonValue& out, std::string* why) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    if (text.empty()) { if (why) *why = "JSON: empty document"; return false; }
    JsonParser p(text, why);
    return p.run(out);
}

} // namespace aver::fmt
