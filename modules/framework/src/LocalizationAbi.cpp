// Localization store behind framework_loc_abi.h: culture folders, lazy per-culture table loading, the fallback
// chain, text-reference resolution, hot reload. One file-static store; single game thread. docs/LOCALIZATION.md.

#include "aver/framework/framework_loc_abi.h"
#include "aver/framework/framework_timers_abi.h"

#include "aver/core/Log.hpp"
#include "aver/formats/OcStrings.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace fmt = aver::fmt;

namespace {

struct Table { std::unordered_map<std::string, std::string> keys; };

struct Culture {
    std::string dir;                       // the folder, forward slashes
    bool loaded = false;
    std::map<std::string, Table> tables;   // table name -> keys
    // .po messages: context + '\x04' + source text (gettext's own separator) -> msgstr, then msgstr[1...].
    std::unordered_map<std::string, std::vector<std::string>> messages;
};

std::string messageKey(const std::string& context, const std::string& id) {
    return context.empty() ? id : context + '\x04' + id;
}

struct Store {
    std::string root;                      // the Localization directory, forward slashes, no trailing '/'
    std::string dflt = "en";
    std::string active = "en";
    std::map<std::string, Culture> cultures;   // normalised tag -> folder (sorted)
    std::vector<std::string> names;            // the keys of `cultures`, for culture_at
    std::vector<std::string> chain;
    std::vector<std::string> missing;
    std::unordered_set<std::string> missingSet;
    int32_t epoch = 0;
};

Store g;

// ---- paths ----

fs::path toPath(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string fromPath(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string normPath(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// ---- loading ----

bool parseFile(const std::string& path, Table& out) {
    fmt::OcStringsData d;
    std::vector<std::string> diag;
    std::string err;
    const bool ok = fmt::loadStrings(path, d, &diag, &err);
    for (const std::string& m : diag) AVER_WARN("[Loc] {}: {}", path, m);
    if (!ok) { AVER_WARN("[Loc] {}: {}", path, err); return false; }
    out.keys.clear();
    for (const fmt::OcStringEntry& e : d.entries) out.keys[e.key] = e.text;
    return true;
}

// The file a table lives in: .ocstrings wins over .csv. Empty when neither exists.
std::string tableFile(const Culture& c, const std::string& stem) {
    const std::string ocs = c.dir + "/" + stem + ".ocstrings";
    const std::string csv = c.dir + "/" + stem + ".csv";
    const bool hasOcs = aver::fileExists(ocs), hasCsv = aver::fileExists(csv);
    if (hasOcs && hasCsv) AVER_WARN("[Loc] {} and {}.csv both exist; the .ocstrings wins", ocs, c.dir + "/" + stem);
    return hasOcs ? ocs : (hasCsv ? csv : std::string());
}

void ensureLoaded(Culture& c) {
    if (c.loaded) return;
    c.loaded = true;
    std::error_code ec;
    std::unordered_set<std::string> stems;
    for (fs::directory_iterator it(toPath(c.dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lowerAscii(fromPath(it->path().extension()));
        if (ext == ".po") {   // gettext messages, merged across the culture's .po files
            fmt::PoData po;
            std::vector<std::string> diag;
            std::string err, text;
            if (std::ifstream f(it->path(), std::ios::binary); f) text.assign(std::istreambuf_iterator<char>(f), {});
            if (!fmt::parsePo(text, po, &diag, &err)) { AVER_WARN("[Loc] {}: {}", fromPath(it->path()), err); continue; }
            for (const std::string& m : diag) AVER_WARN("[Loc] {}: {}", fromPath(it->path()), m);
            for (fmt::PoEntry& e : po.entries) {
                std::vector<std::string> forms;
                if (!e.plurals.empty()) forms = std::move(e.plurals); else forms.push_back(std::move(e.text));
                if (!forms.empty() && !forms[0].empty()) c.messages[messageKey(e.context, e.id)] = std::move(forms);
            }
            continue;
        }
        if (ext != ".ocstrings" && ext != ".csv") continue;
        const std::string stem = fromPath(it->path().stem());
        if (!fmt::validLocTable(stem)) {
            AVER_WARN("[Loc] {}: '{}' is not a valid table name (letters, digits, underscore); skipped", c.dir, stem);
            continue;
        }
        stems.insert(stem);
    }
    for (const std::string& stem : stems) {
        const std::string file = tableFile(c, stem);
        Table t;
        if (!file.empty() && parseFile(file, t)) c.tables[stem] = std::move(t);
    }
}

void rebuildChain() {
    g.chain = fmt::cultureChain(g.active, g.dflt);
    for (const std::string& tag : g.chain) {
        const auto it = g.cultures.find(tag);
        if (it != g.cultures.end()) ensureLoaded(it->second);
    }
}

void refreshNames() {
    g.names.clear();
    for (const auto& kv : g.cultures) g.names.push_back(kv.first);
}

bool lookup(const std::string& table, const std::string& key, const std::string*& out) {
    for (const std::string& tag : g.chain) {
        const auto c = g.cultures.find(tag);
        if (c == g.cultures.end()) continue;
        const auto t = c->second.tables.find(table);
        if (t == c->second.tables.end()) continue;
        const auto k = t->second.keys.find(key);
        if (k == t->second.keys.end()) continue;
        out = &k->second;
        return true;
    }
    return false;
}

// The translation of a message through the chain: its forms (msgstr, or msgstr[0..n]), or null.
const std::vector<std::string>* lookupMessage(const std::string& key) {
    for (const std::string& tag : g.chain) {
        const auto c = g.cultures.find(tag);
        if (c == g.cultures.end()) continue;
        const auto m = c->second.messages.find(key);
        if (m != c->second.messages.end()) return &m->second;
    }
    return nullptr;
}

int32_t copyOut(const std::string& s, char* out, int32_t cap) {
    const int32_t len = static_cast<int32_t>(std::min<size_t>(s.size(), 0x7fffffff));
    if (out && cap > 0) {
        const size_t n = std::min<size_t>(s.size(), static_cast<size_t>(cap) - 1);
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return len;
}

void noteMissing(const std::string& table, const std::string& key, bool masked) {
    const std::string id = table + ":" + key;
    if (!g.missingSet.insert(id).second) return;
    g.missing.push_back(id);
    if (masked) AVER_TRACE("[Loc] no translation for {} (inline default used)", id);
    else AVER_WARN("[Loc] missing text {}", id);
}

std::string safe(const char* s) { return s ? std::string(s) : std::string(); }

} // namespace

extern "C" {

int32_t aver_fw_loc_abi_version(void) { return AVER_FW_LOC_ABI_VERSION; }

int32_t aver_fw_loc_init(const char* utf8Dir, const char* utf8DefaultCulture) {
    const int32_t epoch = g.epoch + 1;
    g = Store{};
    g.epoch = epoch;
    std::string d = fmt::normalizeCulture(safe(utf8DefaultCulture));
    g.dflt = d.empty() ? std::string("en") : d;
    g.active = g.dflt;
    g.root = normPath(safe(utf8Dir));

    std::error_code ec;
    if (g.root.empty() || !fs::is_directory(toPath(g.root), ec)) {
        rebuildChain();
        return -1;
    }
    for (fs::directory_iterator it(toPath(g.root), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string folder = fromPath(it->path().filename());
        const std::string tag = fmt::normalizeCulture(folder);
        if (tag.empty()) { AVER_WARN("[Loc] {}: folder '{}' is not a culture tag; ignored", g.root, folder); continue; }
        if (g.cultures.count(tag)) { AVER_WARN("[Loc] folders '{}' and '{}' are the same culture; ignored", g.cultures[tag].dir, folder); continue; }
        g.cultures[tag].dir = g.root + "/" + folder;
    }
    refreshNames();
    rebuildChain();
    return static_cast<int32_t>(g.names.size());
}

void aver_fw_loc_clear(void) {
    const int32_t epoch = g.epoch + 1;
    g = Store{};
    g.epoch = epoch;
    rebuildChain();
}

int32_t aver_fw_loc_culture_count(void) { return static_cast<int32_t>(g.names.size()); }

const char* aver_fw_loc_culture_at(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= g.names.size()) return "";
    return g.names[static_cast<size_t>(index)].c_str();
}

const char* aver_fw_loc_default_culture(void) { return g.dflt.c_str(); }
const char* aver_fw_loc_culture(void) { return g.active.c_str(); }

int32_t aver_fw_loc_set_culture(const char* utf8Tag) {
    const std::string tag = fmt::normalizeCulture(safe(utf8Tag));
    if (tag.empty()) return -1;

    bool any = false;
    for (std::string t = tag;;) {
        if (g.cultures.count(t)) any = true;
        const size_t dash = t.rfind('-');
        if (dash == std::string::npos) break;
        t.resize(dash);
    }
    if (tag == g.active) return any ? 1 : 0;

    g.active = tag;
    rebuildChain();
    ++g.epoch;
    aver_fw_event_payload_clear();
    aver_fw_event_push_string(tag.c_str());
    aver_fw_event_post("loc.culture_changed", 0, 0);
    return any ? 1 : 0;
}

int32_t aver_fw_loc_epoch(void) { return g.epoch; }

int32_t aver_fw_loc_resolve(const char* utf8Ref, char* out, int32_t cap) {
    if (!utf8Ref) return -1;
    fmt::LocRef ref;
    if (!fmt::parseLocRef(utf8Ref, ref)) return -1;
    const std::string* hit = nullptr;
    if (lookup(ref.table, ref.key, hit)) return copyOut(*hit, out, cap);
    noteMissing(ref.table, ref.key, ref.hasDefault);
    return copyOut(ref.hasDefault ? ref.defaultText : "[" + ref.table + ":" + ref.key + "]", out, cap);
}

int32_t aver_fw_loc_get(const char* utf8Table, const char* utf8Key, char* out, int32_t cap) {
    if (!utf8Table || !utf8Key) return -1;
    const std::string* hit = nullptr;
    if (!lookup(utf8Table, utf8Key, hit)) return -1;
    return copyOut(*hit, out, cap);
}

int32_t aver_fw_loc_tr(const char* utf8Message, const char* utf8Context, char* out, int32_t cap) {
    const std::string id = safe(utf8Message);
    const std::vector<std::string>* forms = lookupMessage(messageKey(safe(utf8Context), id));
    return copyOut(forms && !forms->empty() && !(*forms)[0].empty() ? (*forms)[0] : id, out, cap);
}

int32_t aver_fw_loc_tr_n(const char* utf8Message, const char* utf8Plural, int32_t n, const char* utf8Context,
                         char* out, int32_t cap) {
    const std::string id = safe(utf8Message);
    const size_t form = n == 1 ? 0 : 1;
    const std::vector<std::string>* forms = lookupMessage(messageKey(safe(utf8Context), id));
    if (forms && form < forms->size() && !(*forms)[form].empty()) return copyOut((*forms)[form], out, cap);
    return copyOut(n == 1 ? id : safe(utf8Plural), out, cap);
}

int32_t aver_fw_loc_has(const char* utf8Table, const char* utf8Key) {
    if (!utf8Table || !utf8Key) return 0;
    const std::string* hit = nullptr;
    return lookup(utf8Table, utf8Key, hit) ? 1 : 0;
}

int32_t aver_fw_loc_reload_path(const char* utf8AbsPath) {
    if (!utf8AbsPath || g.root.empty()) return 0;
    const std::string path = normPath(utf8AbsPath);
    if (path.size() <= g.root.size() + 1 || lowerAscii(path.substr(0, g.root.size())) != lowerAscii(g.root) ||
        path[g.root.size()] != '/') return 0;
    const std::string rel = path.substr(g.root.size() + 1);
    const size_t slash = rel.find('/');
    if (slash == std::string::npos || rel.find('/', slash + 1) != std::string::npos) return 0;
    const std::string folder = rel.substr(0, slash), file = rel.substr(slash + 1);
    const size_t dot = file.rfind('.');
    if (dot == std::string::npos || dot == 0) return 0;
    const std::string ext = lowerAscii(file.substr(dot)), stem = file.substr(0, dot);
    if ((ext != ".ocstrings" && ext != ".csv") || !fmt::validLocTable(stem)) return 0;
    const std::string tag = fmt::normalizeCulture(folder);
    if (tag.empty()) return 0;

    auto it = g.cultures.find(tag);
    if (it == g.cultures.end()) {                       // a culture folder created while running
        it = g.cultures.emplace(tag, Culture{}).first;
        it->second.dir = g.root + "/" + folder;
        refreshNames();
    }
    Culture& c = it->second;
    if (c.loaded) {
        const std::string f = tableFile(c, stem);
        if (f.empty()) {
            c.tables.erase(stem);
        } else {
            Table t;
            if (parseFile(f, t)) c.tables[stem] = std::move(t);
            else AVER_WARN("[Loc] {}: reload failed; keeping the previous text", f);
        }
    } else if (std::find(g.chain.begin(), g.chain.end(), tag) != g.chain.end()) {
        rebuildChain();
    }
    ++g.epoch;
    return 1;
}

int32_t aver_fw_loc_missing_count(void) { return static_cast<int32_t>(g.missing.size()); }

const char* aver_fw_loc_missing_at(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= g.missing.size()) return "";
    return g.missing[static_cast<size_t>(index)].c_str();
}

void aver_fw_loc_missing_clear(void) {
    g.missing.clear();
    g.missingSet.clear();
}

} // extern "C"
