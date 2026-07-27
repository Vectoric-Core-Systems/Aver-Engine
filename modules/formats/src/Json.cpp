#include "aver/formats/Json.hpp"

#include <cstdlib>
#include <cstring>

namespace aver::fmt {
namespace {

// A shared empty value, so operator[] can return a reference for a missing key without allocating
// and without the caller having to null-check. Reading it gives the fallbacks, which is what makes
// `doc["a"]["b"][3].asInt(-1)` safe on a document that has none of those.
const JsonValue& nullValue() { static const JsonValue v; return v; }

// glTF nests shallowly -- document, arrays of objects, an array of numbers. A limit costs nothing
// and turns a malicious or corrupt file from a stack overflow into an error message.
constexpr int kMaxDepth = 64;

} // namespace

usize JsonValue::size() const {
    if (type_ == Type::Array)  return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const JsonValue& JsonValue::operator[](usize i) const {
    if (type_ != Type::Array || i >= arr_.size()) return nullValue();
    return arr_[i];
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    if (type_ != Type::Object) return nullValue();
    // LAST wins on a duplicate key, which is what JSON implementations converge on and what a
    // reverse scan gives for free.
    for (usize i = obj_.size(); i-- > 0;) if (obj_[i].key == key) return obj_[i].value;
    return nullValue();
}

bool JsonValue::has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (const JsonMember& m : obj_) if (m.key == key) return true;
    return false;
}

const std::vector<JsonMember>& JsonValue::members() const { return obj_; }

class JsonParser {
public:
    JsonParser(std::string_view t, std::string* why) : t_(t), why_(why) {}

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

    bool err(const std::string& msg) {
        if (why_) *why_ = "JSON: " + msg + " at byte " + std::to_string(p_);
        return false;
    }
    bool eof() const { return p_ >= t_.size(); }
    char cur() const { return t_[p_]; }

    void skipWs() {
        // The four JSON whitespace characters and no others. A stray control byte is an error, not
        // something to skip past.
        while (p_ < t_.size()) {
            const char c = t_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p_;
            else break;
        }
    }

    bool literal(std::string_view lit) {
        if (t_.compare(p_, lit.size(), lit) != 0) return false;
        p_ += lit.size();
        return true;
    }

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

    bool parseObject(JsonValue& v, int depth) {
        v.type_ = JsonValue::Type::Object;
        ++p_;                                  // '{'
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

    bool parseArray(JsonValue& v, int depth) {
        v.type_ = JsonValue::Type::Array;
        ++p_;                                  // '['
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

    // Appends the UTF-8 encoding of a code point. glTF names are usually ASCII, but a Blender export
    // will happily put a non-ASCII bone name in, and truncating it would produce a name that no
    // animation could match to its skeleton.
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

    bool parseString(std::string& out) {
        out.clear();
        ++p_;                                  // opening quote
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
                        // A surrogate PAIR is one code point in two escapes. Decoding the high half
                        // alone would emit an invalid UTF-8 sequence that survives all the way to a
                        // filename.
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
            // Raw control characters are illegal in a JSON string. Rejecting them is what catches a
            // file that is actually binary being handed to the text parser.
            if (static_cast<unsigned char>(c) < 0x20) return err("raw control character in a string");
            out.push_back(c);
            ++p_;
        }
    }

    bool parseNumber(JsonValue& v) {
        const usize start = p_;
        if (!eof() && cur() == '-') ++p_;
        if (eof()) return err("truncated number");
        // JSON forbids a leading zero followed by more digits, and forbids a leading '+' or '.'.
        // Enforced rather than tolerated, because a number like 007 means the producer is not
        // emitting JSON and the rest of the file is suspect.
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

        // strtod on a NUL-terminated copy: the source is a string_view and may not be terminated,
        // and strtod would otherwise read past the end of the token.
        const std::string tok(t_.substr(start, p_ - start));
        v.type_ = JsonValue::Type::Number;
        v.num_ = std::strtod(tok.c_str(), nullptr);
        return true;
    }
};

bool parseJson(std::string_view text, JsonValue& out, std::string* why) {
    // A UTF-8 BOM is not valid JSON but exporters emit it, and refusing it would fail on files every
    // other tool reads. Skipped rather than accepted anywhere else in the document.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    if (text.empty()) { if (why) *why = "JSON: empty document"; return false; }
    JsonParser p(text, why);
    return p.run(out);
}

} // namespace aver::fmt
