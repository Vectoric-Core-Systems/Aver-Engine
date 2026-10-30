#include "FabricReport.hpp"

#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace aver::fabric {

const char* ratingName(Rating r) {
    switch (r) {
        case Rating::Converted: return "converted";
        case Rating::Approximated: return "approximated";
        case Rating::Unsupported: return "unsupported";
    }
    return "converted";
}

std::string jsonString(const std::string& in) {
    std::string out = "\"";
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

void Report::add(ReportEntry e) {
    const std::string key = e.kind + '\x1f' + e.file + '\x1f' + e.node + '\x1f' + e.type + '\x1f' + e.property + '\x1f' +
                            std::to_string(static_cast<int>(e.rating)) + '\x1f' + e.reason;
    if (!seen_.insert(key).second) return;
    entries_.push_back(std::move(e));
}

void Report::add(const char* kind, const std::string& file, const std::string& node, const std::string& type,
                 const std::string& property, Rating rating, const std::string& reason, int line) {
    ReportEntry e;
    e.kind = kind; e.file = file; e.node = node; e.type = type; e.property = property;
    e.rating = rating; e.reason = reason; e.line = line;
    add(std::move(e));
}

size_t Report::count(Rating r) const {
    size_t n = 0;
    for (const ReportEntry& e : entries_) n += e.rating == r;
    return n;
}

size_t Report::count(Rating r, const std::string& kind) const {
    size_t n = 0;
    for (const ReportEntry& e : entries_) n += (e.rating == r && e.kind == kind);
    return n;
}

bool Report::writeJsonl(const std::string& path) const {
    std::string text;
    for (const ReportEntry& e : entries_) {
        text += "{\"schema\":1,\"kind\":" + jsonString(e.kind) + ",\"file\":" + jsonString(e.file) +
                ",\"node\":" + jsonString(e.node) + ",\"type\":" + jsonString(e.type) +
                ",\"property\":" + jsonString(e.property) + ",\"rating\":" + jsonString(ratingName(e.rating)) +
                ",\"reason\":" + jsonString(e.reason) + ",\"line\":" + std::to_string(e.line) + "}\n";
    }
    return writeFileText(path, text);
}

namespace {

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// One flat JSON object whose values are strings or numbers (a report line). False for anything else.
bool parseFlatObject(const std::string& s, std::map<std::string, std::string>& out) {
    size_t i = 0;
    const auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i; };
    const auto str = [&](std::string& v) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        v.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] != '\\') { v.push_back(s[i++]); continue; }
            if (++i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case 'n': v.push_back('\n'); break;
                case 'r': v.push_back('\r'); break;
                case 't': v.push_back('\t'); break;
                case 'b': v.push_back('\b'); break;
                case 'f': v.push_back('\f'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        const unsigned lo = static_cast<unsigned>(std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16));
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    appendUtf8(v, cp);
                    break;
                }
                default: v.push_back(e);
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    for (;;) {
        ws();
        if (i < s.size() && s[i] == '}') return true;
        std::string key, value;
        if (!str(key)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        ws();
        if (i < s.size() && s[i] == '"') {
            if (!str(value)) return false;
        } else {
            const size_t b = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}') ++i;
            value = s.substr(b, i - b);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
        }
        out[key] = value;
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
    }
}

}  // namespace

bool Report::readJsonl(const std::string& path) {
    std::string text;
    if (!readFileText(path, text)) return false;
    clear();
    size_t from = 0;
    while (from < text.size()) {
        size_t nl = text.find('\n', from);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(from, nl - from);
        from = nl + 1;
        std::map<std::string, std::string> kv;
        if (line.find('{') == std::string::npos || !parseFlatObject(line, kv) || !kv.count("kind") || !kv.count("rating")) continue;
        ReportEntry e;
        e.kind = kv["kind"]; e.file = kv["file"]; e.node = kv["node"]; e.type = kv["type"]; e.property = kv["property"]; e.reason = kv["reason"];
        e.rating = kv["rating"] == "unsupported" ? Rating::Unsupported : kv["rating"] == "approximated" ? Rating::Approximated : Rating::Converted;
        e.line = kv.count("line") ? std::atoi(kv["line"].c_str()) : 0;
        add(std::move(e));
    }
    return true;
}

std::vector<Report::Feature> Report::topUnsupported(size_t limit) const {
    std::vector<Feature> list;
    std::map<std::string, size_t> index;
    for (const ReportEntry& e : entries_) {
        if (e.rating != Rating::Unsupported) continue;
        std::string name;
        if (e.kind == "script") name = "script " + (e.type.empty() ? std::string("body") : e.type) + (e.property.empty() ? "" : " " + e.property);
        else if (!e.property.empty()) name = (e.type.empty() ? e.kind : e.type) + "." + e.property;
        else name = e.type.empty() ? e.kind : e.type;
        const auto it = index.find(name);
        if (it == index.end()) {
            index[name] = list.size();
            list.push_back(Feature{name, 1, e.reason});
        } else {
            ++list[it->second].count;
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const Feature& a, const Feature& b) { return a.count > b.count; });
    if (list.size() > limit) list.resize(limit);
    return list;
}

bool Report::writeMarkdown(const std::string& path, const std::string& projectName) const {
    std::string md = "# Fabric conversion report: " + projectName + "\n\n";
    md += "Godot to Aver. Each item is rated **converted** (it behaves as in Godot), **approximated** (it is there but differs, "
          "reason given) or **unsupported** (it is not functional in Aver yet, reason given). The same data, one object per line, is in "
          "`FabricReport.jsonl`.\n\n";
    md += "**Result: " + std::to_string(count(Rating::Converted)) + " converted, " + std::to_string(count(Rating::Approximated)) +
          " approximated, " + std::to_string(count(Rating::Unsupported)) + " unsupported.**\n\n";
    md += "| Kind | Converted | Approximated | Unsupported |\n|---|---:|---:|---:|\n";
    std::set<std::string> kinds;
    for (const ReportEntry& e : entries_) kinds.insert(e.kind);
    for (const std::string& k : kinds)
        md += "| " + k + " | " + std::to_string(count(Rating::Converted, k)) + " | " + std::to_string(count(Rating::Approximated, k)) +
              " | " + std::to_string(count(Rating::Unsupported, k)) + " |\n";
    md += "| **all** | " + std::to_string(count(Rating::Converted)) + " | " + std::to_string(count(Rating::Approximated)) + " | " +
          std::to_string(count(Rating::Unsupported)) + " |\n\n";

    const std::vector<Feature> top = topUnsupported(10);
    if (!top.empty()) {
        md += "### Top unsupported features\n\n| Feature | Items | Why |\n|---|---:|---|\n";
        for (const Feature& f : top) {
            std::string why = f.reason;
            for (char& c : why) if (c == '|' || c == '\n') c = ' ';
            md += "| " + f.name + " | " + std::to_string(f.count) + " | " + why + " |\n";
        }
        md += "\n";
    }

    const auto section = [&](Rating r, const char* title) {
        std::map<std::string, std::vector<const ReportEntry*>> byFile;
        for (const ReportEntry& e : entries_)
            if (e.rating == r) byFile[e.file].push_back(&e);
        md += std::string("## ") + title + "\n\n";
        if (byFile.empty()) { md += "None.\n\n"; return; }
        for (const auto& [file, list] : byFile) {
            md += "### " + (file.empty() ? std::string("(project)") : file) + "\n\n";
            for (const ReportEntry* e : list) {
                md += "- " + e->kind;
                if (!e->node.empty()) md += " `" + e->node + "`";
                if (!e->type.empty()) md += " (" + e->type + ")";
                if (!e->property.empty()) md += e->node.empty() && e->type.empty() ? " `" + e->property + "`" : " ." + e->property;
                if (!e->reason.empty()) md += ": " + e->reason;
                if (e->line > 0) md += " [line " + std::to_string(e->line) + "]";
                md += "\n";
            }
            md += "\n";
        }
    };
    section(Rating::Unsupported, "Unsupported");
    section(Rating::Approximated, "Approximated");

    // Converted items are counted above and listed in the jsonl; the md lists them only per file as a count.
    md += "## Converted\n\n";
    std::map<std::string, size_t> conv;
    for (const ReportEntry& e : entries_)
        if (e.rating == Rating::Converted) ++conv[e.file];
    if (conv.empty()) md += "None.\n";
    for (const auto& [file, n] : conv) md += "- " + (file.empty() ? std::string("(project)") : file) + ": " + std::to_string(n) + " item(s)\n";
    return writeFileText(path, md);
}

}  // namespace aver::fabric
