// Localization key gathering (text scanners) and the used-versus-present report. See LocGather.hpp.
#include "aver/formats/LocGather.hpp"
#include "aver/formats/detail/TextScan.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool isIdStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdChar(char c) { return isIdStart(c) || (c >= '0' && c <= '9'); }

std::string where(std::string_view path, usize line) { return std::string(path) + ":" + std::to_string(line); }

void addUse(const LocRef& r, const std::string& source, std::vector<LocUse>& out) {
    LocUse u;
    u.table = r.table;
    u.key = r.key;
    u.defaultText = r.defaultText;
    u.hasDefault = r.hasDefault;
    u.source = source;
    out.push_back(std::move(u));
}

// Same quoting as the .ocui reader (UiProps.cpp uiTokenize): "..." with \n \t and \x = x, else whitespace-split.
bool tokenizeQuoted(std::string_view s, std::vector<std::string>& out) {
    out.clear();
    usize i = 0;
    const usize n = s.size();
    while (i < n) {
        while (i < n && isSpace(s[i])) ++i;
        if (i >= n) break;
        std::string tok;
        if (s[i] == '"') {
            ++i;
            bool closed = false;
            while (i < n) {
                const char c = s[i++];
                if (c == '\\' && i < n) {
                    const char e = s[i++];
                    tok.push_back(e == 'n' ? '\n' : (e == 't' ? '\t' : e));
                } else if (c == '"') { closed = true; break; }
                else tok.push_back(c);
            }
            if (!closed) return false;
        } else {
            while (i < n && !isSpace(s[i])) tok.push_back(s[i++]);
        }
        out.push_back(std::move(tok));
    }
    return true;
}

template <class Fn>
void forEachLine(std::string_view text, Fn&& fn) {
    usize pos = 0, no = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fn(line, ++no);
        pos = nl + 1;
    }
}

// ---- C# lexer ---------------------------------------------------------------------------------

struct Cs {
    std::string_view t;
    usize p = 0, line = 1;

    bool eof() const { return p >= t.size(); }
    char at(usize o = 0) const { return p + o < t.size() ? t[p + o] : '\0'; }

    // Whitespace and comments.
    void skipTrivia() {
        for (;;) {
            if (eof()) return;
            const char c = t[p];
            if (c == '\n') { ++line; ++p; }
            else if (c == ' ' || c == '\t' || c == '\r') ++p;
            else if (c == '/' && at(1) == '/') { while (!eof() && t[p] != '\n') ++p; }
            else if (c == '/' && at(1) == '*') {
                p += 2;
                while (!eof() && !(t[p] == '*' && at(1) == '/')) { if (t[p] == '\n') ++line; ++p; }
                p = std::min(p + 2, t.size());
            } else return;
        }
    }

    // A string literal starting at p ("..." or @"..."); false for anything else (interpolated, raw, not a string).
    bool readString(std::string& out) {
        out.clear();
        const bool verbatim = at() == '@' && at(1) == '"';
        if (verbatim) ++p;
        if (at() != '"') return false;
        if (!verbatim && at(1) == '"' && at(2) == '"') return false;   // raw string literal
        ++p;
        while (!eof()) {
            const char c = t[p++];
            if (c == '\n') ++line;
            if (verbatim) {
                if (c == '"') { if (at() == '"') { out.push_back('"'); ++p; } else return true; }
                else out.push_back(c);
            } else if (c == '\\' && !eof()) {
                const char e = t[p++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case '0': out.push_back('\0'); break;
                    default:  out.push_back(e);
                }
            } else if (c == '"') return true;
            else out.push_back(c);
        }
        return false;
    }

    // Skips a literal the main scan is not interested in.
    void skipLiteral() {
        std::string ignored;
        const usize save = p, saveLine = line;
        if (at() == '$') ++p;
        if (!readString(ignored)) { p = save + 1; line = saveLine; }
    }

    void skipChar() {
        ++p;
        if (at() == '\\') ++p;
        if (!eof()) ++p;
        while (!eof() && t[p] != '\'' && t[p] != '\n') ++p;
        if (at() == '\'') ++p;
    }

    bool word(std::string& w) {
        if (!isIdStart(at())) return false;
        w.clear();
        while (isIdChar(at())) w.push_back(t[p++]);
        return true;
    }
    bool eat(char c) { skipTrivia(); if (at() == c) { ++p; return true; } return false; }
};

} // namespace

// ---- scanners -------------------------------------------------------------------------------------

void gatherOcui(std::string_view text, std::string_view path, std::vector<LocUse>& out) {
    forEachLine(text, [&](std::string_view line, usize no) {
        line = trim(line);
        if (line.empty() || line[0] == '#') return;
        std::vector<std::string> t;
        if (!tokenizeQuoted(line, t) || t.size() < 4 || !equalsCI(t[0], "SET")) return;
        const bool items = equalsCI(t[2], "items");
        if (!items && !equalsCI(t[2], "text") && !equalsCI(t[2], "placeholder")) return;
        const usize last = items ? t.size() : 4;
        for (usize i = 3; i < last; ++i) {
            if (items) {
                for (const std::string_view part : splitChar(t[i], '\n')) {
                    LocRef r;
                    if (parseLocRef(part, r)) addUse(r, where(path, no), out);
                }
            } else {
                LocRef r;
                if (parseLocRef(t[i], r)) addUse(r, where(path, no), out);
            }
        }
    });
}

void gatherOcgraph(std::string_view text, std::string_view path, std::vector<LocUse>& out) {
    forEachLine(text, [&](std::string_view line, usize no) {
        line = trim(line);
        if (line.size() < 5 || line[0] == '#' || !startsWithCI(line, "NODE") || !isSpace(line[4])) return;
        usize i = 0;
        const usize n = line.size();
        while (i < n) {
            while (i < n && isSpace(line[i])) ++i;
            if (i >= n) break;
            std::string tok;   // unquoted
            bool inQuote = false;
            while (i < n && (inQuote || !isSpace(line[i]))) {
                const char c = line[i++];
                if (c == '"') { inQuote = !inQuote; continue; }
                if (c == '\\' && inQuote && i < n) {
                    const char e = line[i++];
                    tok.push_back(e == 'n' ? '\n' : (e == 't' ? '\t' : e));
                    continue;
                }
                tok.push_back(c);
            }
            const usize eq = tok.find('=');
            if (eq == 0 || eq == std::string::npos) continue;
            bool ident = true;
            for (usize k = 0; k < eq; ++k) if (!isIdChar(tok[k])) { ident = false; break; }
            LocRef r;
            if (ident && parseLocRef(std::string_view(tok).substr(eq + 1), r)) addUse(r, where(path, no), out);
        }
    });
}

void gatherCsharp(std::string_view text, std::string_view path, std::vector<LocUse>& out) {
    Cs s;
    s.t = text;
    std::string w;
    while (!s.eof()) {
        const char c = s.t[s.p];
        if (c == '\n') { ++s.line; ++s.p; continue; }
        if (c == '/' && (s.at(1) == '/' || s.at(1) == '*')) { s.skipTrivia(); continue; }
        if (c == '"' || ((c == '@' || c == '$') && (s.at(1) == '"' || s.at(1) == '@' || s.at(1) == '$'))) { s.skipLiteral(); continue; }
        if (c == '\'') { s.skipChar(); continue; }
        if (!isIdStart(c)) { ++s.p; continue; }

        const bool prevId = s.p > 0 && isIdChar(s.t[s.p - 1]);
        const usize startLine = s.line;
        s.word(w);
        if (prevId) continue;

        enum class Form { None, Text, Format, Plural, LocTextCtor } form = Form::None;
        if (w == "Loc") {
            std::string m;
            if (s.eat('.')) {
                s.skipTrivia();
                if (s.word(m)) form = m == "Text" ? Form::Text : m == "Format" ? Form::Format : m == "Plural" ? Form::Plural : Form::None;
            }
        } else if (w == "new") {
            s.skipTrivia();
            std::string m;
            if (s.word(m) && m == "LocText") form = Form::LocTextCtor;
        }
        if (form == Form::None || !s.eat('(')) continue;

        s.skipTrivia();
        std::string table, key, dflt;
        if (!s.readString(table)) continue;
        LocRef ref;
        s.skipTrivia();
        if (form == Form::LocTextCtor && s.at() == ')') {
            if (parseLocRef(table, ref)) addUse(ref, where(path, startLine), out);
            continue;
        }
        if (!s.eat(',')) continue;
        s.skipTrivia();
        if (!s.readString(key)) continue;
        if (!validLocTable(table) || !validLocKey(key)) continue;
        ref.table = table;
        ref.key = key;
        if (form != Form::Plural && s.eat(',')) {
            s.skipTrivia();
            ref.hasDefault = s.readString(dflt);
            if (ref.hasDefault) ref.defaultText = dflt;
        }
        addUse(ref, where(path, startLine), out);
    }
}

void gatherOccaptions(std::string_view text, std::string_view path, std::vector<LocUse>& out) {
    forEachLine(text, [&](std::string_view line, usize no) {
        line = trim(line);
        if (line.empty() || line[0] == '#') return;
        std::vector<std::string> t;
        if (!tokenizeQuoted(line, t) || t.size() < 3) return;
        for (usize i = 0; i < t.size(); ++i) if (t[i] == "#") { t.resize(i); break; }   // trailing comment
        const auto add = [&](const std::string& v) {
            LocRef r;
            if (parseLocRef(v, r)) addUse(r, where(path, no), out);
        };
        if (equalsCI(t[0], "LINE")) { if (t.size() >= 4) add(t[3]); }
        else if (equalsCI(t[0], "SPEAKER") || equalsCI(t[0], "CUE")) {
            const char* key = equalsCI(t[0], "SPEAKER") ? "name" : "tag";
            for (usize i = 2; i + 1 < t.size(); ++i) if (equalsCI(t[i], key)) { add(t[i + 1]); break; }
        }
    });
}

void gatherTable(const OcTableData& d, std::string_view path, std::vector<LocUse>& out) {
    for (usize r = 0; r < d.rowCount(); ++r) {
        for (usize c = 0; c < d.columns.size(); ++c) {
            LocRef ref;
            if (!parseLocRef(d.cell(r, c), ref)) continue;
            const std::string src = d.source == OcTableData::Source::Csv ? where(path, r + 2) : std::string(path) + ":" + d.rowNames[r];
            addUse(ref, src, out);
        }
    }
}

// ---- report ---------------------------------------------------------------------------------------

namespace {

const char* const kPluralSuffix[] = {".zero", ".one", ".two", ".few", ".many", ".other"};

// "Items.count.one" -> "Items.count" when the suffix is a plural category; else empty.
std::string pluralBase(const std::string& key) {
    for (const char* sfx : kPluralSuffix) {
        const usize n = std::char_traits<char>::length(sfx);
        if (key.size() > n && key.compare(key.size() - n, n, sfx) == 0) return key.substr(0, key.size() - n);
    }
    return {};
}

using KeySet = std::unordered_map<std::string, const OcStringEntry*>;
using TableIndex = std::map<std::string /*culture*/, std::map<std::string /*table*/, KeySet>>;

TableIndex indexCorpus(const LocCorpus& c) {
    TableIndex idx;
    for (const auto& [cul, tables] : c)
        for (const auto& [tab, data] : tables) {
            KeySet& ks = idx[cul][tab];
            for (const OcStringEntry& e : data.entries) ks[e.key] = &e;
        }
    return idx;
}

// A key is present when the table has it, or has its plural form (a Loc.Plural use names the base key).
bool presentIn(const TableIndex& idx, const std::string& cul, const std::string& tab, const std::string& key) {
    const auto c = idx.find(cul);
    if (c == idx.end()) return false;
    const auto t = c->second.find(tab);
    if (t == c->second.end()) return false;
    if (t->second.count(key)) return true;
    for (const char* sfx : kPluralSuffix) if (t->second.count(key + sfx)) return true;
    return false;
}

struct UseAgg { std::string source; std::vector<std::string> defaults; bool hasDefault = false; };

std::map<std::pair<std::string, std::string>, UseAgg> aggregate(const std::vector<LocUse>& used) {
    std::map<std::pair<std::string, std::string>, UseAgg> m;
    for (const LocUse& u : used) {
        UseAgg& a = m[{u.table, u.key}];
        if (a.source.empty()) a.source = u.source;
        if (u.hasDefault) {
            a.hasDefault = true;
            if (std::find(a.defaults.begin(), a.defaults.end(), u.defaultText) == a.defaults.end()) a.defaults.push_back(u.defaultText);
        }
    }
    return m;
}

std::string defaultCultureOf(std::string_view d) {
    std::string n = normalizeCulture(d);
    return n.empty() ? std::string("en") : n;
}

} // namespace

LocReport buildLocReport(const std::vector<LocUse>& used, const LocCorpus& corpus, std::string_view defaultCulture) {
    LocReport rep;
    const std::string dc = defaultCultureOf(defaultCulture);
    const TableIndex idx = indexCorpus(corpus);
    const auto uses = aggregate(used);

    std::set<std::string> cultures;
    for (const auto& [cul, tables] : corpus) { (void)tables; cultures.insert(cul); }
    cultures.insert(dc);

    for (const auto& [tk, agg] : uses) {
        if (agg.defaults.size() > 1) rep.conflicts.push_back({tk.first, tk.second, agg.defaults});
    }
    for (const std::string& cul : cultures) {
        for (const auto& [tk, agg] : uses) {
            if (presentIn(idx, cul, tk.first, tk.second)) continue;
            if (cul == dc && agg.hasDefault) continue;
            rep.missing.push_back({cul, tk.first, tk.second, agg.source});
        }
    }
    for (const auto& [cul, tables] : corpus) {
        for (const auto& [tab, data] : tables) {
            for (const OcStringEntry& e : data.entries) {
                if (uses.count({tab, e.key})) continue;
                const std::string base = pluralBase(e.key);
                if (!base.empty() && uses.count({tab, base})) continue;
                rep.unused.push_back({cul, tab, e.key});
            }
        }
    }
    const auto dcIt = corpus.find(dc);
    if (dcIt != corpus.end()) {
        for (const auto& [cul, tables] : corpus) {
            if (cul == dc) continue;
            for (const auto& [tab, data] : tables) {
                const auto dt = dcIt->second.find(tab);
                if (dt == dcIt->second.end()) continue;
                for (const OcStringEntry& e : data.entries) {
                    if (e.text.empty()) continue;
                    for (const OcStringEntry& de : dt->second.entries)
                        if (de.key == e.key) { if (de.text == e.text) rep.untranslated.push_back({cul, tab, e.key, {}}); break; }
                }
            }
        }
    }
    return rep;
}

usize applyGather(const std::vector<LocUse>& used, LocCorpus& corpus, std::string_view defaultCulture) {
    const std::string dc = defaultCultureOf(defaultCulture);
    usize added = 0;
    std::set<std::pair<std::string, std::string>> done;
    for (const LocUse& u : used) {
        if (!done.insert({u.table, u.key}).second) continue;
        OcStringsData& d = corpus[dc][u.table];
        if (d.culture.empty()) d.culture = dc;
        bool have = false;
        for (const OcStringEntry& e : d.entries) {
            if (e.key == u.key || pluralBase(e.key) == u.key) { have = true; break; }
        }
        if (have) continue;
        OcStringEntry e;
        e.key = u.key;
        e.text = u.hasDefault ? u.defaultText : std::string();
        d.entries.push_back(std::move(e));
        ++added;
    }
    return added;
}

} // namespace aver::fmt
