// .occaptions reader and record-preserving writer, the sound-pattern matcher, caption colours. See OcCaptions.hpp.
#include "aver/formats/OcCaptions.hpp"
#include "aver/core/CharConv.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
char lowerC(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

struct Tok { std::string text; bool quoted = false; };

// 1 = token read, 0 = end of line (or a comment), -1 = malformed (why set). '#' starts a comment at the start of a
// token outside quotes, unless it is a colour (#RRGGBB or #RRGGBBAA followed by whitespace or the end).
int nextTok(std::string_view line, usize& p, Tok& t, std::string& why) {
    while (p < line.size() && isWs(line[p])) ++p;
    if (p >= line.size()) return 0;
    if (line[p] == '#') {
        usize e = p + 1;
        while (e < line.size() && isHex(line[e])) ++e;
        const usize digits = e - p - 1;
        const bool colour = (digits == 6 || digits == 8) && (e >= line.size() || isWs(line[e]));
        if (!colour) { p = line.size(); return 0; }
    }
    t = Tok{};
    if (line[p] == '"') {
        ++p;
        t.quoted = true;
        while (p < line.size()) {
            const char c = line[p++];
            if (c == '"') {
                if (p < line.size() && !isWs(line[p])) { why = "text after a closing quote"; return -1; }
                return 1;
            }
            if (c == '\\') {
                if (p >= line.size()) { why = "dangling backslash"; return -1; }
                const char e = line[p++];
                switch (e) {
                    case '"': case '\\': t.text.push_back(e); break;
                    case 'n': t.text.push_back('\n'); break;
                    case 't': t.text.push_back('\t'); break;
                    case 'r': t.text.push_back('\r'); break;
                    default: why = std::string("unknown escape \\") + e; return -1;
                }
            } else {
                t.text.push_back(c);
            }
        }
        why = "unterminated quote";
        return -1;
    }
    while (p < line.size() && !isWs(line[p])) t.text.push_back(line[p++]);
    return 1;
}

bool strictF32(std::string_view s, f32& out) {
    s = trim(s);
    if (s.empty()) return false;
    f64 v{};
    const auto r = aver::fromChars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    out = static_cast<f32>(v);
    return true;
}
bool strictI32(std::string_view s, i32& out) {
    s = trim(s);
    if (s.empty()) return false;
    i32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

std::string num(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", static_cast<f64>(v));
    return buf;
}

std::string quoteText(std::string_view s) {
    std::string o = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default:   o.push_back(c);
        }
    }
    o.push_back('"');
    return o;
}

// A bare word when it is safe, else quoted.
std::string wordOrQuoted(std::string_view s) {
    bool bare = !s.empty() && s[0] != '"' && s[0] != '#';
    for (const char c : s) if (isWs(c) || c == '"' || c == '\\') { bare = false; break; }
    return bare ? std::string(s) : quoteText(s);
}

const char* kindWord(OcCaptionKind k) {
    return k == OcCaptionKind::Dialogue ? "dialogue" : (k == OcCaptionKind::Music ? "music" : "sound");
}

// ---- record parsers (the keyword is already consumed) ----

bool parseSpeakerRest(std::string_view line, usize p, OcCaptionSpeaker& s, std::string& why) {
    Tok t;
    if (nextTok(line, p, t, why) != 1) { if (why.empty()) why = "SPEAKER needs an id"; return false; }
    s = OcCaptionSpeaker{};
    s.id = t.text;
    for (;;) {
        Tok k;
        const int r = nextTok(line, p, k, why);
        if (r < 0) return false;
        if (r == 0) return true;
        Tok v;
        if (nextTok(line, p, v, why) != 1) { if (why.empty()) why = "'" + k.text + "' needs a value"; return false; }
        if (equalsCI(k.text, "name")) s.name = v.text;
        else if (equalsCI(k.text, "colour") || equalsCI(k.text, "color")) {
            if (!parseCaptionColour(v.text, s.rgba)) { why = "bad colour '" + v.text + "' (expected #RRGGBB)"; return false; }
        } else { why = "unknown SPEAKER keyword '" + k.text + "'"; return false; }
    }
}

bool parseCueRest(std::string_view line, usize p, OcCaptionCue& c, std::string& why) {
    Tok t;
    if (nextTok(line, p, t, why) != 1) { if (why.empty()) why = "CUE needs an id"; return false; }
    c = OcCaptionCue{};
    c.id = t.text;
    for (;;) {
        Tok k;
        const int r = nextTok(line, p, k, why);
        if (r < 0) return false;
        if (r == 0) return true;
        Tok v;
        if (nextTok(line, p, v, why) != 1) { if (why.empty()) why = "'" + k.text + "' needs a value"; return false; }
        const std::string& w = k.text;
        if (equalsCI(w, "sound")) c.sounds.push_back(v.text);
        else if (equalsCI(w, "kind")) {
            if (equalsCI(v.text, "dialogue")) c.kind = OcCaptionKind::Dialogue;
            else if (equalsCI(v.text, "sound")) c.kind = OcCaptionKind::Sound;
            else if (equalsCI(v.text, "music")) c.kind = OcCaptionKind::Music;
            else { why = "unknown kind '" + v.text + "' (dialogue, sound or music)"; return false; }
        }
        else if (equalsCI(w, "speaker")) c.speaker = v.text;
        else if (equalsCI(w, "priority")) { if (!strictI32(v.text, c.priority)) { why = "bad number '" + v.text + "'"; return false; } }
        else if (equalsCI(w, "radius") || equalsCI(w, "cooldown") || equalsCI(w, "hold")) {
            f32 f = 0;
            if (!strictF32(v.text, f) || f < 0) { why = "bad number '" + v.text + "'"; return false; }
            (equalsCI(w, "radius") ? c.radius : (equalsCI(w, "cooldown") ? c.cooldown : c.hold)) = f;
        }
        else if (equalsCI(w, "tag")) c.tag = v.text;
        else { why = "unknown CUE keyword '" + w + "'"; return false; }
    }
}

bool parseLineRest(std::string_view line, usize p, OcCaptionLine& l, std::string& why) {
    Tok a, b, t;
    if (nextTok(line, p, a, why) != 1 || nextTok(line, p, b, why) != 1 || nextTok(line, p, t, why) != 1) {
        if (why.empty()) why = "LINE needs: start dur text";
        return false;
    }
    l = OcCaptionLine{};
    if (!strictF32(a.text, l.start) || l.start < 0) { why = "bad start '" + a.text + "'"; return false; }
    if (!strictF32(b.text, l.dur) || l.dur < 0) { why = "bad duration '" + b.text + "'"; return false; }
    l.text = t.text;
    Tok k;
    const int r = nextTok(line, p, k, why);
    if (r < 0) return false;
    if (r == 0) return true;
    Tok v;
    if (!equalsCI(k.text, "speaker") || nextTok(line, p, v, why) != 1) { if (why.empty()) why = "unexpected '" + k.text + "' after the text"; return false; }
    l.speaker = v.text;
    return true;
}

// ---- canonical lines ----

std::string speakerLine(const OcCaptionSpeaker& s) {
    std::string o = "SPEAKER " + wordOrQuoted(s.id);
    if (!s.name.empty()) o += " name " + quoteText(s.name);
    if (s.rgba != 0xFFFFFFFFu) o += " colour " + formatCaptionColour(s.rgba);
    return o;
}

std::string cueLine(const OcCaptionCue& c) {
    std::string o = "CUE " + wordOrQuoted(c.id);
    for (const std::string& s : c.sounds) o += " sound " + quoteText(s);
    if (c.kind != OcCaptionKind::Sound) o += std::string(" kind ") + kindWord(c.kind);
    if (!c.speaker.empty()) o += " speaker " + wordOrQuoted(c.speaker);
    if (c.priority != 0) o += " priority " + std::to_string(c.priority);
    if (c.radius != 0) o += " radius " + num(c.radius);
    if (c.cooldown != 1.0f) o += " cooldown " + num(c.cooldown);
    if (c.hold >= 0) o += " hold " + num(c.hold);
    if (!c.tag.empty()) o += " tag " + quoteText(c.tag);
    return o;
}

std::string lineLine(const OcCaptionLine& l) {
    std::string o = "  LINE " + num(l.start) + " " + num(l.dur) + " " + quoteText(l.text);
    if (!l.speaker.empty()) o += " speaker " + wordOrQuoted(l.speaker);
    return o;
}

struct SrcLine { std::string_view body; std::string_view eol; };

std::vector<SrcLine> splitLines(std::string_view t) {
    std::vector<SrcLine> out;
    usize pos = 0;
    while (pos < t.size()) {
        const usize nl = t.find('\n', pos);
        SrcLine l;
        if (nl == std::string_view::npos) { l.body = t.substr(pos); pos = t.size(); }
        else {
            usize end = nl;
            if (end > pos && t[end - 1] == '\r') --end;
            l.body = t.substr(pos, end - pos);
            l.eol = t.substr(end, nl + 1 - end);
            pos = nl + 1;
        }
        out.push_back(l);
    }
    return out;
}

// ---- glob over one segment ----

bool globSeg(std::string_view pat, std::string_view s) {
    usize pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) { ++pi; ++si; }
        else if (pi < pat.size() && pat[pi] == '*') { star = pi++; mark = si; }
        else if (star != std::string_view::npos) { pi = star + 1; si = ++mark; }
        else return false;
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

std::vector<std::string> segments(std::string_view p) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : p) {
        if (c == '/' || c == '\\') { if (!cur.empty() && cur != ".") out.push_back(std::move(cur)); cur.clear(); }
        else cur.push_back(lowerC(c));
    }
    if (!cur.empty() && cur != ".") out.push_back(std::move(cur));
    return out;
}

} // namespace

// ---- vocabulary ----

bool parseCaptionColour(std::string_view s, u32& rgba) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (usize i = 1; i < s.size(); ++i) if (!isHex(s[i])) return false;
    u32 v = 0;
    for (usize i = 1; i < s.size(); ++i) {
        const char c = lowerC(s[i]);
        v = (v << 4) | static_cast<u32>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    rgba = s.size() == 7 ? ((v << 8) | 0xFFu) : v;
    return true;
}

std::string formatCaptionColour(u32 rgba) {
    char b[16];
    if ((rgba & 0xFFu) == 0xFFu) std::snprintf(b, sizeof b, "#%06X", rgba >> 8);
    else std::snprintf(b, sizeof b, "#%08X", rgba);
    return b;
}

std::vector<OcCaptionLine> captionCueLines(const OcCaptionCue& c) {
    std::vector<OcCaptionLine> out;
    if (!c.tag.empty()) { OcCaptionLine l; l.text = c.tag; out.push_back(std::move(l)); }
    out.insert(out.end(), c.lines.begin(), c.lines.end());
    return out;
}

f32 captionCueHold(const OcCaptionCue& c) {
    if (c.hold >= 0) return c.hold;
    return c.kind == OcCaptionKind::Dialogue ? 0.8f : 1.5f;
}

bool captionPatternMatches(std::string_view pattern, std::string_view path) {
    const std::vector<std::string> pat = segments(pattern), p = segments(path);
    if (pat.empty() || pat.size() > p.size()) return false;
    const usize off = p.size() - pat.size();
    for (usize i = 0; i < pat.size(); ++i) if (!globSeg(pat[i], p[off + i])) return false;
    return true;
}

bool captionPatternExact(std::string_view pattern, std::string_view path) {
    if (pattern.find_first_of("*?") != std::string_view::npos) return false;
    return captionPatternMatches(pattern, path);
}

// ---- reader ----

bool parseOcCaptions(std::string_view text, OcCaptionData& out, std::vector<std::string>* diag, std::string* err) {
    OcCaptionData d;
    bool sawHeader = false;
    usize lineNo = 0;
    const auto note = [&](usize at, const std::string& m) { if (diag) diag->push_back("line " + std::to_string(at) + ": " + m); };

    OcCaptionCue cur;
    usize curLine = 0;
    bool haveCue = false, badCue = false;
    const auto flush = [&]() {
        if (!haveCue) return;
        haveCue = false;
        if (cur.tag.empty() && cur.lines.empty()) { note(curLine, "CUE '" + cur.id + "' has no LINE and no tag; skipped"); return; }
        d.cues.push_back(std::move(cur));
    };

    for (const SrcLine& sl : splitLines(text)) {
        ++lineNo;
        std::string_view line = sl.body;
        if (lineNo == 1 && line.size() >= 3 && static_cast<u8>(line[0]) == 0xEF && static_cast<u8>(line[1]) == 0xBB &&
            static_cast<u8>(line[2]) == 0xBF) line.remove_prefix(3);
        usize p = 0;
        Tok kw;
        std::string why;
        const int r = nextTok(line, p, kw, why);
        if (r == 0) continue;
        if (r < 0) {
            if (!sawHeader) { if (err) *err = "not an .occaptions: line " + std::to_string(lineNo) + ": " + why; return false; }
            note(lineNo, why);
            continue;
        }
        if (!sawHeader) {
            Tok v;
            std::string w2;
            if (kw.quoted || !equalsCI(kw.text, "OCCAPTIONS") || nextTok(line, p, v, w2) != 1) {
                if (err) *err = "not an .occaptions: the first record must be OCCAPTIONS <version>";
                return false;
            }
            const i32 ver = parseI32(v.text, -1);
            if (ver < 1) { if (err) *err = "OCCAPTIONS needs a version number"; return false; }
            if (ver > kOcCaptionsVersion) { if (err) *err = "unsupported .occaptions version " + std::to_string(ver); return false; }
            d.version = ver;
            sawHeader = true;
            continue;
        }
        if (kw.quoted) continue;
        if (equalsCI(kw.text, "SPEAKER")) {
            OcCaptionSpeaker s;
            if (!parseSpeakerRest(line, p, s, why)) { note(lineNo, "malformed SPEAKER skipped: " + why); continue; }
            bool dup = false;
            for (OcCaptionSpeaker& e : d.speakers) if (e.id == s.id) { note(lineNo, "duplicate speaker '" + s.id + "' (the last one wins)"); e = s; dup = true; break; }
            if (!dup) d.speakers.push_back(std::move(s));
        } else if (equalsCI(kw.text, "CUE")) {
            flush();
            badCue = false;
            OcCaptionCue c;
            if (!parseCueRest(line, p, c, why)) { note(lineNo, "malformed CUE skipped: " + why); badCue = true; continue; }
            cur = std::move(c);
            curLine = lineNo;
            haveCue = true;
        } else if (equalsCI(kw.text, "LINE")) {
            if (badCue) continue;   // the owning CUE was already reported
            if (!haveCue) { note(lineNo, "LINE outside a CUE skipped"); continue; }
            OcCaptionLine l;
            if (!parseLineRest(line, p, l, why)) { note(lineNo, "malformed LINE skipped: " + why); continue; }
            cur.lines.push_back(std::move(l));
        }
        // anything else: a record from a newer build, preserved by the writer
    }
    flush();
    if (!sawHeader) { if (err) *err = "not an .occaptions: no OCCAPTIONS header"; return false; }
    out = std::move(d);
    return true;
}

bool loadOcCaptions(const std::string& path, OcCaptionData& out, std::vector<std::string>* diag, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) { if (err) *err = "cannot read " + path; return false; }
    return parseOcCaptions(text, out, diag, err);
}

// ---- writer ----

namespace {

enum class RecKind { Header, Speaker, Cue, Line, Other };
struct Rec {
    RecKind kind = RecKind::Other;
    SrcLine src;
    bool ok = false;               // parsed (Speaker, Cue, Line)
    OcCaptionSpeaker spk;
    OcCaptionCue cue;
    OcCaptionLine ln;
    usize owner = static_cast<usize>(-1);   // Line: index of the Cue record that owns it
    bool badOwner = false;                  // Line after a malformed CUE: kept raw
};

} // namespace

std::string writeOcCaptions(const OcCaptionData& d, std::string_view existing) {
    const std::vector<SrcLine> lines = splitLines(existing);
    std::vector<Rec> recs;
    recs.reserve(lines.size());
    bool hasHeader = false;
    usize curCue = static_cast<usize>(-1);
    bool curBad = false;
    for (const SrcLine& sl : lines) {
        Rec r;
        r.src = sl;
        usize p = 0;
        Tok kw;
        std::string why;
        if (nextTok(sl.body, p, kw, why) == 1 && !kw.quoted) {
            if (!hasHeader && equalsCI(kw.text, "OCCAPTIONS")) { r.kind = RecKind::Header; hasHeader = true; }
            else if (hasHeader && equalsCI(kw.text, "SPEAKER")) { r.kind = RecKind::Speaker; r.ok = parseSpeakerRest(sl.body, p, r.spk, why); }
            else if (hasHeader && equalsCI(kw.text, "CUE")) {
                r.kind = RecKind::Cue;
                r.ok = parseCueRest(sl.body, p, r.cue, why);
                curCue = recs.size();
                curBad = !r.ok;
            } else if (hasHeader && equalsCI(kw.text, "LINE")) {
                r.kind = RecKind::Line;
                r.ok = parseLineRest(sl.body, p, r.ln, why);
                r.owner = curCue;
                r.badOwner = curBad || curCue == static_cast<usize>(-1);
            } else if (hasHeader) {
                curCue = static_cast<usize>(-1);   // an unknown record ends the CUE block
            }
        }
        recs.push_back(std::move(r));
    }

    if (!hasHeader) {
        std::string s = "OCCAPTIONS " + std::to_string(kOcCaptionsVersion) + "\n";
        for (const OcCaptionSpeaker& sp : d.speakers) s += speakerLine(sp) + "\n";
        for (const OcCaptionCue& c : d.cues) {
            s += cueLine(c) + "\n";
            for (const OcCaptionLine& l : c.lines) s += lineLine(l) + "\n";
        }
        return s;
    }

    std::string_view eol = "\n";
    for (const SrcLine& sl : lines) if (!sl.eol.empty()) { eol = sl.eol; break; }
    const auto eolOf = [&](const SrcLine& sl) { return std::string(sl.eol.empty() ? eol : sl.eol); };

    // Data cues by id, in order; the k-th source occurrence of an id pairs with the k-th data occurrence.
    std::unordered_map<std::string, std::vector<usize>> cueIdx;
    for (usize i = 0; i < d.cues.size(); ++i) cueIdx[d.cues[i].id].push_back(i);
    std::unordered_map<std::string, usize> seenCue;
    std::vector<bool> cueWritten(d.cues.size(), false);
    std::unordered_map<std::string, usize> spkIdx;
    for (usize i = 0; i < d.speakers.size(); ++i) spkIdx[d.speakers[i].id] = i;
    std::vector<bool> spkWritten(d.speakers.size(), false);

    // Pre-pass: which data cue each source Cue record pairs with.
    std::vector<usize> pair(recs.size(), static_cast<usize>(-1));
    for (usize i = 0; i < recs.size(); ++i) {
        if (recs[i].kind != RecKind::Cue || !recs[i].ok) continue;
        const auto it = cueIdx.find(recs[i].cue.id);
        const usize k = seenCue[recs[i].cue.id]++;
        if (it != cueIdx.end() && k < it->second.size()) pair[i] = it->second[k];
    }
    // The last LINE of each Cue record, to know where leftover new lines go.
    std::vector<usize> lastLine(recs.size(), static_cast<usize>(-1));
    std::vector<usize> lineCount(recs.size(), 0);
    for (usize i = 0; i < recs.size(); ++i)
        if (recs[i].kind == RecKind::Line && recs[i].ok && !recs[i].badOwner) { lastLine[recs[i].owner] = i; ++lineCount[recs[i].owner]; }

    std::vector<usize> lineOrd(recs.size(), 0);
    {
        std::vector<usize> ord(recs.size(), 0);
        for (usize i = 0; i < recs.size(); ++i)
            if (recs[i].kind == RecKind::Line && recs[i].ok && !recs[i].badOwner) lineOrd[i] = ord[recs[i].owner]++;
    }

    std::vector<std::string> out;
    usize lastSpeakerOut = static_cast<usize>(-1), headerOut = 0;
    const auto emitLeftover = [&](usize cueRec, const SrcLine& at) {
        const OcCaptionCue& c = d.cues[pair[cueRec]];
        for (usize k = lineCount[cueRec]; k < c.lines.size(); ++k) out.push_back(lineLine(c.lines[k]) + eolOf(at));
    };

    for (usize i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[i];
        const std::string raw = std::string(r.src.body) + std::string(r.src.eol);
        switch (r.kind) {
            case RecKind::Header: out.push_back(raw); headerOut = out.size() - 1; break;
            case RecKind::Speaker: {
                if (!r.ok) { out.push_back(raw); break; }
                const auto it = spkIdx.find(r.spk.id);
                if (it == spkIdx.end() || spkWritten[it->second]) break;   // removed, or a duplicate
                spkWritten[it->second] = true;
                const OcCaptionSpeaker& n = d.speakers[it->second];
                out.push_back(speakerLine(n) == speakerLine(r.spk) ? raw : speakerLine(n) + eolOf(r.src));
                lastSpeakerOut = out.size() - 1;
                break;
            }
            case RecKind::Cue: {
                if (!r.ok) { out.push_back(raw); break; }
                if (pair[i] == static_cast<usize>(-1)) break;   // removed
                const OcCaptionCue& n = d.cues[pair[i]];
                cueWritten[pair[i]] = true;
                out.push_back(cueLine(n) == cueLine(r.cue) ? raw : cueLine(n) + eolOf(r.src));
                if (lineCount[i] == 0) emitLeftover(i, r.src);
                break;
            }
            case RecKind::Line: {
                if (!r.ok || r.badOwner) { out.push_back(raw); break; }
                const usize cueRec = r.owner;
                if (pair[cueRec] == static_cast<usize>(-1)) break;   // owner removed
                const OcCaptionCue& n = d.cues[pair[cueRec]];
                const usize k = lineOrd[i];
                if (k < n.lines.size()) out.push_back(lineLine(n.lines[k]) == lineLine(r.ln) ? raw : lineLine(n.lines[k]) + eolOf(r.src));
                if (lastLine[cueRec] == i) emitLeftover(cueRec, r.src);
                break;
            }
            case RecKind::Other: out.push_back(raw); break;
        }
    }

    const auto endLine = [&]() {
        if (!out.empty() && (out.back().empty() || out.back().back() != '\n')) out.back() += std::string(eol);
    };
    std::vector<std::string> addSpk;
    for (usize i = 0; i < d.speakers.size(); ++i) if (!spkWritten[i]) addSpk.push_back(speakerLine(d.speakers[i]) + std::string(eol));
    if (!addSpk.empty()) {
        const usize at = lastSpeakerOut != static_cast<usize>(-1) ? lastSpeakerOut + 1 : headerOut + 1;
        if (at > 0 && at <= out.size() && (out[at - 1].empty() || out[at - 1].back() != '\n')) out[at - 1] += std::string(eol);
        out.insert(out.begin() + static_cast<isize>(at), addSpk.begin(), addSpk.end());
    }
    for (usize i = 0; i < d.cues.size(); ++i) {
        if (cueWritten[i]) continue;
        endLine();
        out.push_back(cueLine(d.cues[i]) + std::string(eol));
        for (const OcCaptionLine& l : d.cues[i].lines) out.push_back(lineLine(l) + std::string(eol));
    }
    std::string s;
    for (const std::string& l : out) s += l;
    return s;
}

} // namespace aver::fmt
