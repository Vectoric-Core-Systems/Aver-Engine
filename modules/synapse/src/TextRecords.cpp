#include "aver/synapse/TextRecords.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none

#include <algorithm>
#include <charconv>
#include <cmath>

namespace aver::synapse {
namespace {

struct Tok {
    std::string text;
    usize eq = std::string::npos;   // index into `text` of the first unquoted '='
    usize rawBegin = 0, rawEnd = 0;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// False on an unterminated quote. `restRaw` is the line after the first token, comments removed.
bool tokenize(std::string_view line, std::vector<Tok>& toks, std::string& restRaw) {
    usize i = 0;
    const usize n = line.size();
    while (i < n) {
        while (i < n && isSpace(line[i])) ++i;
        if (i >= n || line[i] == '#') break;
        Tok t;
        t.rawBegin = i;
        bool inQuote = false;
        while (i < n) {
            const char c = line[i];
            if (inQuote) {
                if (c == '\\' && i + 1 < n && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                    t.text += line[i + 1];
                    i += 2;
                } else if (c == '"') {
                    inQuote = false;
                    ++i;
                } else {
                    t.text += c;
                    ++i;
                }
                continue;
            }
            if (c == '"') { inQuote = true; ++i; continue; }
            if (isSpace(c) || c == '#') break;
            if (c == '=' && t.eq == std::string::npos) t.eq = t.text.size();
            t.text += c;
            ++i;
        }
        if (inQuote) return false;
        t.rawEnd = i;
        toks.push_back(std::move(t));
    }
    if (toks.size() > 1) {
        usize b = toks[0].rawEnd;
        while (b < toks.back().rawEnd && isSpace(line[b])) ++b;
        restRaw.assign(line.substr(b, toks.back().rawEnd - b));
    }
    return true;
}

} // namespace

std::string TextRecord::get(std::string_view key, std::string_view fallback) const {
    for (const auto& p : kv)
        if (p.first == key) return p.second;
    return std::string(fallback);
}

bool TextRecord::has(std::string_view key) const {
    for (const auto& p : kv)
        if (p.first == key) return true;
    return false;
}

bool readTextRecords(std::string_view text, std::string_view header, int version,
                     std::vector<TextRecord>& out, std::string* why) {
    out.clear();
    const auto fail = [&](u32 line, const std::string& msg) {
        out.clear();
        if (why) *why = "line " + std::to_string(line) + ": " + msg;
        return false;
    };
    std::string head(header);
    for (char& c : head) c = upper(c);

    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);

    bool sawHeader = false;
    u32 lineNo = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;

        std::vector<Tok> toks;
        std::string restRaw;
        if (!tokenize(line, toks, restRaw)) return fail(lineNo, "unterminated quote");
        if (toks.empty()) {
            if (end >= text.size()) break;
            continue;
        }

        TextRecord r;
        r.line = lineNo;
        r.keyword = toks[0].text;
        for (char& c : r.keyword) c = upper(c);
        r.rest = std::move(restRaw);
        for (usize k = 1; k < toks.size(); ++k) {
            Tok& t = toks[k];
            if (t.eq != std::string::npos && t.eq > 0) {
                std::string key = t.text.substr(0, t.eq);
                for (char& c : key) c = lower(c);
                r.kv.emplace_back(std::move(key), t.text.substr(t.eq + 1));
            } else {
                r.args.push_back(std::move(t.text));
            }
        }

        if (!sawHeader) {
            if (r.keyword != head) return fail(lineNo, "expected header '" + head + " " + std::to_string(version) + "'");
            i32 v = 0;
            if (r.args.size() != 1 || !parseI32(r.args[0], v) || !r.kv.empty()) return fail(lineNo, "header needs a version number");
            if (v != version) return fail(lineNo, "unsupported version " + r.args[0]);
            sawHeader = true;
        } else {
            if (r.keyword == head) return fail(lineNo, "repeated header");
            out.push_back(std::move(r));
        }
        if (end >= text.size()) break;
    }
    if (!sawHeader) return fail(1, "missing header '" + head + " " + std::to_string(version) + "'");
    return true;
}

std::string quoteToken(std::string_view s) {
    bool need = s.empty();
    for (const char c : s)
        if (isSpace(c) || c == '"' || c == '#' || c == '=' || c == '\n') need = true;
    if (!need) return std::string(s);
    std::string r = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += (c == '\n' || c == '\r') ? ' ' : c;
    }
    r += '"';
    return r;
}

std::string fmtF32(f32 v) {
    char buf[96];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    return std::string(buf, r.ptr);
}

bool parseF32(std::string_view s, f32& out) {
    if (s.empty()) return false;
    f32 v = 0;
    const char* const end = s.data() + s.size();
    const auto r = aver::fromChars(s.data(), end, v);
    if (r.ec != std::errc{} || r.ptr != end || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parseI32(std::string_view s, i32& out) {
    if (s.empty()) return false;
    i32 v = 0;
    const char* const end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (r.ec != std::errc{} || r.ptr != end) return false;
    out = v;
    return true;
}

} // namespace aver::synapse
