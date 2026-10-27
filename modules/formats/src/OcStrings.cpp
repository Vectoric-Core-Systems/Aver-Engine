// .ocstrings / string-table .csv reader and writer, the text reference grammar and culture tags.
// See OcStrings.hpp and docs/LOCALIZATION.md.
#include "aver/formats/OcStrings.hpp"
#include "aver/formats/detail/Csv.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return isAlpha(c) || isDigit(c); }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

struct Tok { std::string text; bool quoted = false; };

// 1 = token read, 0 = end of line (or a comment), -1 = malformed (why set). A '#' starts a comment only at the
// start of a token outside quotes, so '#' inside text is safe.
int nextTok(std::string_view line, usize& p, Tok& t, std::string& why) {
    while (p < line.size() && isWs(line[p])) ++p;
    if (p >= line.size() || line[p] == '#') { p = line.size(); return 0; }
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

// `ENTRY <key> <text> [NOTE <note>]` after the ENTRY word has been consumed.
bool parseEntryRest(std::string_view line, usize p, OcStringEntry& e, std::string& why) {
    Tok k, t, n;
    if (nextTok(line, p, k, why) != 1) { if (why.empty()) why = "ENTRY needs a key"; return false; }
    if (!validLocKey(k.text)) { why = "invalid key '" + k.text + "'"; return false; }
    if (nextTok(line, p, t, why) != 1) { if (why.empty()) why = "ENTRY needs a text"; return false; }
    e = OcStringEntry{};
    e.key = k.text;
    e.text = t.text;
    Tok x;
    const int r = nextTok(line, p, x, why);
    if (r < 0) return false;
    if (r == 0) return true;
    if (x.quoted || !equalsCI(x.text, "NOTE")) { why = "unexpected '" + x.text + "' after the text"; return false; }
    if (nextTok(line, p, n, why) != 1) { if (why.empty()) why = "NOTE needs a value"; return false; }
    e.note = n.text;
    Tok y;
    const int r2 = nextTok(line, p, y, why);
    if (r2 < 0) return false;
    if (r2 > 0) { why = "unexpected '" + y.text + "' after the note"; return false; }
    return true;
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

std::string entryLine(const OcStringEntry& e) {
    std::string s = "ENTRY " + e.key + " " + quoteText(e.text);
    if (!e.note.empty()) s += " NOTE " + quoteText(e.note);
    return s;
}

struct SrcLine { std::string_view body; std::string_view eol; };

std::vector<SrcLine> splitLines(std::string_view t) {
    std::vector<SrcLine> out;
    usize pos = 0;
    while (pos < t.size()) {
        usize nl = t.find('\n', pos);
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

std::string lowerTrim(std::string_view s) {
    s = trim(s);
    std::string o(s);
    for (char& c : o) c = lower(c);
    return o;
}

} // namespace

// ---- vocabulary ---------------------------------------------------------------------------------

bool validLocKey(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) if (!(isAlnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

bool validLocTable(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) if (!(isAlnum(c) || c == '_')) return false;
    return true;
}

bool parseLocRef(std::string_view s, LocRef& out) {
    if (s.size() < 4 || s[0] != '@' || s[1] == '@') return false;
    usize i = 1;
    while (i < s.size() && (isAlnum(s[i]) || s[i] == '_')) ++i;
    if (i == 1 || i >= s.size() || s[i] != ':') return false;
    const usize tableEnd = i;
    ++i;
    const usize keyStart = i;
    while (i < s.size() && (isAlnum(s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-')) ++i;
    if (i == keyStart) return false;
    LocRef r;
    r.table = std::string(s.substr(1, tableEnd - 1));
    r.key = std::string(s.substr(keyStart, i - keyStart));
    if (i < s.size()) {
        if (s[i] != '=') return false;
        r.hasDefault = true;
        r.defaultText = std::string(s.substr(i + 1));
    }
    out = std::move(r);
    return true;
}

std::string formatLocRef(const LocRef& r) {
    std::string s = "@" + r.table + ":" + r.key;
    if (r.hasDefault) { s.push_back('='); s += r.defaultText; }
    return s;
}

std::string normalizeCulture(std::string_view tag) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : tag) {
        if (c == '-' || c == '_') { parts.push_back(std::move(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(std::move(cur));
    if (parts.empty() || parts.size() > 3) return {};
    const auto allAlpha = [](const std::string& p) { for (const char c : p) if (!isAlpha(c)) return false; return !p.empty(); };
    const auto allDigit = [](const std::string& p) { for (const char c : p) if (!isDigit(c)) return false; return !p.empty(); };
    if (!allAlpha(parts[0]) || parts[0].size() < 2 || parts[0].size() > 3) return {};
    std::string out;
    for (const char c : parts[0]) out.push_back(lower(c));
    usize i = 1;
    if (i < parts.size() && parts[i].size() == 4 && allAlpha(parts[i])) {
        out.push_back('-');
        out.push_back(upper(parts[i][0]));
        for (usize k = 1; k < 4; ++k) out.push_back(lower(parts[i][k]));
        ++i;
    }
    if (i < parts.size()) {
        const std::string& r = parts[i];
        if (r.size() == 2 && allAlpha(r)) {
            out.push_back('-');
            for (const char c : r) out.push_back(upper(c));
        } else if (r.size() == 3 && allDigit(r)) {
            out.push_back('-');
            out += r;
        } else {
            return {};
        }
        ++i;
    }
    if (i != parts.size()) return {};
    return out;
}

std::vector<std::string> cultureChain(std::string_view active, std::string_view dflt) {
    std::vector<std::string> out;
    const auto addWithParents = [&](std::string tag) {
        while (!tag.empty()) {
            if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
            const usize dash = tag.rfind('-');
            if (dash == std::string::npos) break;
            tag.resize(dash);
        }
    };
    addWithParents(normalizeCulture(active));
    std::string d = normalizeCulture(dflt);
    if (d.empty()) d = "en";
    addWithParents(d);
    return out;
}

// ---- .ocstrings -----------------------------------------------------------------------------------

bool parseOcstrings(std::string_view text, OcStringsData& out, std::vector<std::string>* diag, std::string* err) {
    OcStringsData d;
    std::unordered_map<std::string, usize> index;
    bool sawHeader = false;
    usize lineNo = 0;
    const auto note = [&](const std::string& m) { if (diag) diag->push_back("line " + std::to_string(lineNo) + ": " + m); };

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
            if (!sawHeader) { if (err) *err = "not an .ocstrings: line " + std::to_string(lineNo) + ": " + why; return false; }
            note(why);
            continue;
        }
        if (!sawHeader) {
            Tok v;
            std::string w2;
            if (kw.quoted || !equalsCI(kw.text, "OCSTRINGS") || nextTok(line, p, v, w2) != 1) {
                if (err) *err = "not an .ocstrings: the first record must be OCSTRINGS <version>";
                return false;
            }
            const i32 ver = parseI32(v.text, -1);
            if (ver < 1) { if (err) *err = "OCSTRINGS needs a version number"; return false; }
            if (ver > kOcStringsVersion) { if (err) *err = "unsupported .ocstrings version " + std::to_string(ver); return false; }
            d.version = ver;
            sawHeader = true;
            continue;
        }
        if (kw.quoted) continue;
        if (equalsCI(kw.text, "ENTRY")) {
            OcStringEntry e;
            if (!parseEntryRest(line, p, e, why)) { note("malformed ENTRY skipped: " + why); continue; }
            const auto it = index.find(e.key);
            if (it != index.end()) {
                note("duplicate key '" + e.key + "' (the last one wins)");
                d.entries[it->second] = std::move(e);
            } else {
                index[e.key] = d.entries.size();
                d.entries.push_back(std::move(e));
            }
        } else if (equalsCI(kw.text, "CULTURE")) {
            Tok c;
            if (nextTok(line, p, c, why) == 1) d.culture = c.text;
        }
        // anything else: a record from a newer build, preserved by the writer
    }
    if (!sawHeader) { if (err) *err = "not an .ocstrings: no OCSTRINGS header"; return false; }
    out = std::move(d);
    return true;
}

std::string writeOcstrings(const OcStringsData& d, std::string_view existing) {
    bool hasHeader = false;
    const std::vector<SrcLine> lines = splitLines(existing);
    for (const SrcLine& sl : lines) {
        usize p = 0;
        Tok kw;
        std::string why;
        if (nextTok(sl.body, p, kw, why) == 1) { hasHeader = !kw.quoted && equalsCI(kw.text, "OCSTRINGS"); break; }
    }

    if (!hasHeader) {
        std::string s = "OCSTRINGS " + std::to_string(kOcStringsVersion) + "\n";
        if (!d.culture.empty()) s += "CULTURE " + d.culture + "\n";
        for (const OcStringEntry& e : d.entries) s += entryLine(e) + "\n";
        return s;
    }

    std::unordered_map<std::string, const OcStringEntry*> byKey;
    for (const OcStringEntry& e : d.entries) byKey[e.key] = &e;
    std::unordered_set<std::string> written;

    std::string_view eol = "\n";
    for (const SrcLine& sl : lines) if (!sl.eol.empty()) { eol = sl.eol; break; }

    std::vector<std::string> out;
    usize lastEntry = static_cast<usize>(-1);
    for (const SrcLine& sl : lines) {
        const std::string raw = std::string(sl.body) + std::string(sl.eol);
        usize p = 0;
        Tok kw;
        std::string why;
        if (nextTok(sl.body, p, kw, why) != 1 || kw.quoted) { out.push_back(raw); continue; }
        if (equalsCI(kw.text, "ENTRY")) {
            OcStringEntry e;
            if (!parseEntryRest(sl.body, p, e, why)) { out.push_back(raw); continue; }   // malformed: not ours to drop
            const auto it = byKey.find(e.key);
            if (it == byKey.end() || written.count(e.key)) continue;                       // removed, or a duplicate
            written.insert(e.key);
            const OcStringEntry& n = *it->second;
            if (n.text == e.text && n.note == e.note) out.push_back(raw);
            else out.push_back(entryLine(n) + std::string(sl.eol.empty() ? eol : sl.eol));
            lastEntry = out.size() - 1;
        } else if (equalsCI(kw.text, "CULTURE") && !d.culture.empty()) {
            Tok c;
            if (nextTok(sl.body, p, c, why) == 1 && c.text == d.culture) out.push_back(raw);
            else out.push_back("CULTURE " + d.culture + std::string(sl.eol.empty() ? eol : sl.eol));
        } else {
            out.push_back(raw);
        }
    }

    std::vector<std::string> add;
    for (const OcStringEntry& e : d.entries) {
        if (written.count(e.key)) continue;
        written.insert(e.key);
        add.push_back(entryLine(byKey[e.key] ? *byKey[e.key] : e) + std::string(eol));
    }
    if (!add.empty()) {
        usize at = lastEntry == static_cast<usize>(-1) ? out.size() : lastEntry + 1;
        if (at > 0 && (out[at - 1].empty() || out[at - 1].back() != '\n')) out[at - 1] += std::string(eol);
        out.insert(out.begin() + static_cast<isize>(at), add.begin(), add.end());
    }
    std::string s;
    for (const std::string& l : out) s += l;
    return s;
}

// ---- string-table CSV -----------------------------------------------------------------------------

bool parseStringsCsv(std::string_view text, OcStringsData& out, std::vector<std::string>* diag, std::string* err) {
    text = csvStripBom(text);
    std::vector<CsvRecord> rows;
    if (!csvParse(text, csvDetectDelimiter(text), rows, err)) return false;
    if (rows.empty()) { if (err) *err = "empty string table: expected a Key,Text header row"; return false; }
    int kc = -1, tc = -1, nc = -1;
    for (usize i = 0; i < rows[0].cells.size(); ++i) {
        const std::string h = lowerTrim(rows[0].cells[i]);
        if (h == "key" && kc < 0) kc = static_cast<int>(i);
        else if (h == "text" && tc < 0) tc = static_cast<int>(i);
        else if (h == "note" && nc < 0) nc = static_cast<int>(i);
    }
    if (kc < 0 || tc < 0) { if (err) *err = "a string table needs 'Key' and 'Text' columns"; return false; }
    const usize need = static_cast<usize>(std::max(kc, std::max(tc, nc))) + 1;

    OcStringsData d;
    std::unordered_map<std::string, usize> index;
    for (usize r = 1; r < rows.size(); ++r) {
        const CsvRecord& rec = rows[r];
        const auto note = [&](const std::string& m) { if (diag) diag->push_back("line " + std::to_string(rec.line) + ": " + m); };
        bool allEmpty = true;
        for (const std::string& c : rec.cells) if (!trim(c).empty()) { allEmpty = false; break; }
        if (allEmpty) continue;
        const auto cell = [&](int c) -> std::string { return c >= 0 && static_cast<usize>(c) < rec.cells.size() ? rec.cells[static_cast<usize>(c)] : std::string(); };
        if (rec.cells.size() < need) note("expected " + std::to_string(need) + " cells, got " + std::to_string(rec.cells.size()));
        OcStringEntry e;
        e.key = std::string(trim(cell(kc)));
        if (!validLocKey(e.key)) { note("invalid key '" + e.key + "' skipped"); continue; }
        e.text = cell(tc);
        e.note = cell(nc);
        const auto it = index.find(e.key);
        if (it != index.end()) {
            note("duplicate key '" + e.key + "' (the last one wins)");
            d.entries[it->second] = std::move(e);
        } else {
            index[e.key] = d.entries.size();
            d.entries.push_back(std::move(e));
        }
    }
    out = std::move(d);
    return true;
}

std::string writeStringsCsv(const OcStringsData& d) {
    bool anyNote = false;
    for (const OcStringEntry& e : d.entries) if (!e.note.empty()) { anyNote = true; break; }
    std::string s = anyNote ? "Key,Text,Note\r\n" : "Key,Text\r\n";
    for (const OcStringEntry& e : d.entries) {
        csvWriteField(s, e.key, ',');
        s.push_back(',');
        csvWriteField(s, e.text, ',');
        if (anyNote) { s.push_back(','); csvWriteField(s, e.note, ','); }
        s += "\r\n";
    }
    return s;
}

// ---- gettext .po ----------------------------------------------------------------------------------

namespace {
// A PO string literal's contents, with C escapes undone. False when it is not one quoted string.
bool poUnquote(std::string_view s, std::string& out) {
    s = trim(s);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    out.clear();
    for (usize i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 2 < s.size()) {
            const char n = s[++i];
            c = n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
        }
        out.push_back(c);
    }
    return true;
}
}  // namespace

bool parsePo(std::string_view text, PoData& out, std::vector<std::string>* diag, std::string* err) {
    text = csvStripBom(text);
    PoData d;
    PoEntry cur;
    std::string* target = nullptr;   // the string the next continuation line appends to
    bool any = false;
    const auto flush = [&]() {
        if (any) {
            if (cur.id.empty() && cur.context.empty()) {   // the header: pick out Language
                const usize at = cur.text.find("Language:");
                if (at != std::string::npos) {
                    const usize end = cur.text.find('\n', at);
                    d.language = std::string(trim(std::string_view(cur.text).substr(at + 9, end == std::string::npos ? std::string::npos : end - at - 9)));
                }
            } else {
                d.entries.push_back(std::move(cur));
            }
        }
        cur = PoEntry{};
        target = nullptr;
        any = false;
    };
    usize lineNo = 0, start = 0;
    while (start <= text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim(text.substr(start, end - start));
        start = end + 1;
        ++lineNo;
        if (line.empty()) { if (start > text.size()) break; continue; }
        if (line.front() == '#') continue;
        std::string s;
        if (line.front() == '"') {
            if (!target || !poUnquote(line, s)) { if (diag) diag->push_back("line " + std::to_string(lineNo) + ": stray string"); continue; }
            *target += s;
            continue;
        }
        const usize sp = line.find_first_of(" \t");
        const std::string_view kw = line.substr(0, sp);
        const std::string_view rest = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
        if (!poUnquote(rest, s)) { if (diag) diag->push_back("line " + std::to_string(lineNo) + ": expected a quoted string"); continue; }
        if (kw == "msgctxt") { flush(); cur.context = s; target = &cur.context; any = true; }
        else if (kw == "msgid") {
            if (any && (!cur.id.empty() || !cur.text.empty() || !cur.plurals.empty())) flush();
            cur.id = s; target = &cur.id; any = true;
        }
        else if (kw == "msgid_plural") { cur.idPlural = s; target = &cur.idPlural; }
        else if (kw == "msgstr") { cur.text = s; target = &cur.text; }
        else if (kw.rfind("msgstr[", 0) == 0 && kw.back() == ']') {
            const usize n = static_cast<usize>(std::atoi(std::string(kw.substr(7, kw.size() - 8)).c_str()));
            if (cur.plurals.size() <= n) cur.plurals.resize(n + 1);
            cur.plurals[n] = s;
            target = &cur.plurals[n];
        }
        else if (diag) diag->push_back("line " + std::to_string(lineNo) + ": unknown keyword '" + std::string(kw) + "'");
        if (start > text.size()) break;
    }
    flush();
    if (d.entries.empty() && d.language.empty()) { if (err) *err = "no messages"; return false; }
    out = std::move(d);
    return true;
}

bool loadStrings(const std::string& path, OcStringsData& out, std::vector<std::string>* diag, std::string* err) {
    std::string ext;
    const usize dot = path.rfind('.');
    if (dot != std::string::npos) for (usize i = dot; i < path.size(); ++i) ext.push_back(lower(path[i]));
    std::string text;
    if (!readFileText(path, text)) { if (err) *err = "cannot read " + path; return false; }
    if (ext == ".ocstrings") return parseOcstrings(text, out, diag, err);
    if (ext == ".csv") return parseStringsCsv(text, out, diag, err);
    if (err) *err = "unsupported string table extension '" + ext + "'";
    return false;
}

} // namespace aver::fmt
