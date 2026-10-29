// .ocbus reader and writer; see OcBusLayout.hpp.
#include "aver/formats/OcBusLayout.hpp"
#include "aver/core/CharConv.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>

namespace aver::fmt {
namespace {

// Splits a line into tokens; "quoted text" is one token (quotes removed). Returns false on an unterminated quote.
bool tokenize(std::string_view line, std::vector<std::string>& out) {
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= line.size()) break;
        std::string tok;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
            if (line[i] == '"') {
                const usize close = line.find('"', i + 1);
                if (close == std::string_view::npos) return false;
                tok.append(line.substr(i + 1, close - i - 1));
                i = close + 1;
            } else {
                tok.push_back(line[i++]);
            }
        }
        out.push_back(std::move(tok));
    }
    return true;
}

bool toFloat(const std::string& s, f32& v) {
    f32 x = 0.0f;
    const auto r = fromChars(s.data(), s.data() + s.size(), x);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    v = x;
    return true;
}

std::string clean(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c == '"' || c == '\n' || c == '\r') c = '_';
    return o;
}

std::string num(f32 v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}

} // namespace

bool parseOcBusLayout(std::string_view text, audio::BusLayout& out, std::string* why) {
    out.buses.clear();
    int lineNo = 0;
    bool sawHeader = false;
    auto fail = [&](const char* m) {
        if (why) *why = "line " + std::to_string(lineNo) + ": " + m;
        return false;
    };
    usize pos = 0;
    while (pos <= text.size()) {
        const usize nl = text.find('\n', pos);
        const std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++lineNo;
        std::vector<std::string> t;
        if (!tokenize(line, t)) return fail("unterminated quote");
        if (t.empty() || t[0][0] == '#') continue;
        if (t[0] == "OCBUS") {
            if (t.size() < 2 || t[1] != "1") return fail("unsupported OCBUS version");
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail("missing OCBUS header");
        if (t[0] == "BUS") {
            if (t.size() < 2) return fail("BUS needs a name");
            audio::BusLayoutBus b;
            b.name = t[1];
            for (usize i = 2; i < t.size(); ++i) {
                const usize eq = t[i].find('=');
                if (eq == std::string::npos) return fail("expected key=value");
                const std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
                f32 f = 0.0f;
                if (k == "send") b.send = v;
                else if (!toFloat(v, f)) return fail("a number is expected");
                else if (k == "volume_db") b.volumeDb = f;
                else if (k == "mute") b.mute = f != 0.0f;
                else if (k == "solo") b.solo = f != 0.0f;
                else if (k == "bypass") b.bypass = f != 0.0f;
            }
            out.buses.push_back(std::move(b));
        } else if (t[0] == "EFFECT") {
            if (out.buses.empty()) return fail("EFFECT before any BUS");
            if (t.size() < 2) return fail("EFFECT needs a class name");
            audio::BusLayoutEffect e;
            e.className = t[1];
            for (usize i = 2; i < t.size(); ++i) {
                const usize eq = t[i].find('=');
                if (eq == std::string::npos) return fail("expected key=value");
                const std::string k = t[i].substr(0, eq), v = t[i].substr(eq + 1);
                if (k == "sidechain") { e.sidechain = v; continue; }
                f32 f = 0.0f;
                if (!toFloat(v, f)) return fail("a number is expected");
                if (k == "enabled") e.enabled = f != 0.0f;
                else e.params.emplace_back(k, f);
            }
            out.buses.back().effects.push_back(std::move(e));
        } else {
            return fail("unknown record");
        }
    }
    if (!sawHeader) { if (why) *why = "empty file"; return false; }
    return true;
}

std::string writeOcBusLayout(const audio::BusLayout& layout) {
    std::string s = "OCBUS 1\n";
    for (const audio::BusLayoutBus& b : layout.buses) {
        s += "BUS \"" + clean(b.name) + "\" send=\"" + clean(b.send) + "\" volume_db=" + num(b.volumeDb) + " mute=" + (b.mute ? "1" : "0") +
             " solo=" + (b.solo ? "1" : "0") + " bypass=" + (b.bypass ? "1" : "0") + "\n";
        for (const audio::BusLayoutEffect& e : b.effects) {
            s += "EFFECT \"" + clean(e.className) + "\" enabled=" + (e.enabled ? "1" : "0") + " sidechain=\"" + clean(e.sidechain) + "\"";
            for (const auto& p : e.params) s += " " + clean(p.first) + "=" + num(p.second);
            s += "\n";
        }
    }
    return s;
}

bool loadOcBusLayout(const std::string& path, audio::BusLayout& out, std::string* why) {
    std::string text;
    if (!readFileText(path, text)) { if (why) *why = "cannot read '" + path + "'"; return false; }
    return parseOcBusLayout(text, out, why);
}

} // namespace aver::fmt
