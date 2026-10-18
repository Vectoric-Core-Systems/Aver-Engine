#include "LocGatherRun.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcStrings.hpp"
#include "aver/formats/OcTable.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace aver::editor {
namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string pathStr(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
fs::path toPath(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string extOf(const fs::path& p) { return lowerAscii(pathStr(p.extension())); }

bool skippedDir(const std::string& name) {
    const std::string n = lowerAscii(name);
    return n == "bin" || n == "obj" || n == "source";
}

} // namespace

LocFiles loadLocFiles(const std::string& localizationDir) {
    LocFiles out;
    std::error_code ec;
    if (localizationDir.empty() || !fs::is_directory(toPath(localizationDir), ec)) return out;
    for (fs::directory_iterator cul(toPath(localizationDir), ec), end; !ec && cul != end; cul.increment(ec)) {
        if (!cul->is_directory(ec)) continue;
        const std::string folder = pathStr(cul->path().filename());
        const std::string tag = fmt::normalizeCulture(folder);
        if (tag.empty()) { out.diagnostics.push_back(folder + ": not a culture tag; ignored"); continue; }
        std::map<std::string, std::string> byStem;   // stem -> chosen file
        for (fs::directory_iterator f(cul->path(), ec), fend; !ec && f != fend; f.increment(ec)) {
            if (!f->is_regular_file(ec)) continue;
            const std::string ext = extOf(f->path());
            if (ext != ".ocstrings" && ext != ".csv") continue;
            const std::string stem = pathStr(f->path().stem());
            if (!fmt::validLocTable(stem)) { out.diagnostics.push_back(pathStr(f->path()) + ": not a valid table name; ignored"); continue; }
            const auto it = byStem.find(stem);
            if (it == byStem.end() || ext == ".ocstrings") byStem[stem] = pathStr(f->path());
        }
        for (const auto& [stem, file] : byStem) {
            fmt::OcStringsData d;
            std::vector<std::string> diag;
            std::string err;
            if (!fmt::loadStrings(file, d, &diag, &err)) { out.diagnostics.push_back(file + ": " + err); continue; }
            for (const std::string& m : diag) out.diagnostics.push_back(file + ": " + m);
            out.corpus[tag][stem] = std::move(d);
            out.paths[tag + "/" + stem] = file;
        }
    }
    return out;
}

std::vector<std::string> listLocSources(const std::string& contentRoot) {
    std::vector<std::string> out;
    std::error_code ec;
    if (contentRoot.empty() || !fs::is_directory(toPath(contentRoot), ec)) return out;
    for (fs::recursive_directory_iterator it(toPath(contentRoot), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (skippedDir(pathStr(it->path().filename()))) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = extOf(it->path());
        if (ext == ".ocui" || ext == ".ocgraph" || ext == ".occaptions" || ext == ".cs") { out.push_back(pathStr(it->path())); continue; }
        if (ext != ".csv" && ext != ".json") continue;
        // Under Localization/ a .csv is a string table, not a data table.
        const fs::path rel = fs::relative(it->path(), toPath(contentRoot), ec);
        bool inLoc = false;
        for (const fs::path& part : rel) if (lowerAscii(pathStr(part)) == "localization") { inLoc = true; break; }
        if (!inLoc) out.push_back(pathStr(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<fmt::LocUse> scanLocUses(const std::vector<std::string>& files, std::vector<std::string>* diagnostics) {
    std::vector<fmt::LocUse> uses;
    for (const std::string& file : files) {
        const std::string ext = extOf(toPath(file));
        if (ext == ".csv" || ext == ".json") {
            fmt::OcTableData t;
            std::string err;
            if (fmt::loadTable(file, t, &err)) fmt::gatherTable(t, file, uses);
            else if (diagnostics && ext == ".csv") diagnostics->push_back(file + ": " + err);   // a JSON that is not a table is normal
            continue;
        }
        std::string text;
        if (!readFileText(file, text)) {
            if (diagnostics) diagnostics->push_back(file + ": cannot read");
            continue;
        }
        if (ext == ".ocui") fmt::gatherOcui(text, file, uses);
        else if (ext == ".ocgraph") fmt::gatherOcgraph(text, file, uses);
        else if (ext == ".occaptions") fmt::gatherOccaptions(text, file, uses);
        else if (ext == ".cs") fmt::gatherCsharp(text, file, uses);
    }
    return uses;
}

std::string formatLocReport(const fmt::LocReport& r, usize usedKeys) {
    std::string s = "Localization report: " + std::to_string(usedKeys) + " key(s) used\n";
    const auto section = [&](const char* title, usize n) { s += std::string(title) + " (" + std::to_string(n) + ")\n"; };
    section("Missing", r.missing.size());
    for (const auto& m : r.missing) s += "  [" + m.culture + "] " + m.table + ":" + m.key + (m.source.empty() ? "" : "  <- " + m.source) + "\n";
    section("Unused", r.unused.size());
    for (const auto& u : r.unused) s += "  [" + u.culture + "] " + u.table + ":" + u.key + "\n";
    section("Conflicting inline defaults", r.conflicts.size());
    for (const auto& c : r.conflicts) {
        s += "  " + c.table + ":" + c.key + ":";
        for (const std::string& d : c.defaults) s += " \"" + d + "\"";
        s += "\n";
    }
    section("Untranslated (same text as the default culture)", r.untranslated.size());
    for (const auto& u : r.untranslated) s += "  [" + u.culture + "] " + u.table + ":" + u.key + "\n";
    return s;
}

namespace {

LocRunResult buildResult(const std::string& contentRoot, const std::string& defaultCulture, LocFiles& files,
                         std::vector<fmt::LocUse>& uses) {
    LocRunResult res;
    const std::string loc = contentRoot + "/Localization";
    files = loadLocFiles(loc);
    uses = scanLocUses(listLocSources(contentRoot), &res.diagnostics);
    for (const std::string& d : files.diagnostics) res.diagnostics.push_back(d);
    std::set<std::pair<std::string, std::string>> distinct;
    for (const fmt::LocUse& u : uses) distinct.insert({u.table, u.key});
    res.usedKeys = distinct.size();
    res.report = fmt::buildLocReport(uses, files.corpus, defaultCulture);
    res.text = formatLocReport(res.report, res.usedKeys);
    return res;
}

} // namespace

LocRunResult runLocReport(const std::string& contentRoot, const std::string& defaultCulture) {
    LocFiles files;
    std::vector<fmt::LocUse> uses;
    return buildResult(contentRoot, defaultCulture, files, uses);
}

LocRunResult runLocGather(const std::string& contentRoot, const std::string& defaultCulture) {
    LocFiles files;
    std::vector<fmt::LocUse> uses;
    LocRunResult res = buildResult(contentRoot, defaultCulture, files, uses);

    const std::string dc = fmt::normalizeCulture(defaultCulture).empty() ? std::string("en") : fmt::normalizeCulture(defaultCulture);
    std::map<std::string, usize> before;                       // table -> entry count in the default culture
    if (const auto it = files.corpus.find(dc); it != files.corpus.end())
        for (const auto& [tab, d] : it->second) before[tab] = d.entries.size();

    res.added = fmt::applyGather(uses, files.corpus, dc);
    if (res.added == 0) return res;

    for (const auto& [tab, d] : files.corpus[dc]) {
        const auto b = before.find(tab);
        if (b != before.end() && b->second == d.entries.size()) continue;
        const auto known = files.paths.find(dc + "/" + tab);
        const std::string file = known != files.paths.end() ? known->second : contentRoot + "/Localization/" + dc + "/" + tab + ".ocstrings";
        std::error_code ec;
        fs::create_directories(toPath(file).parent_path(), ec);
        std::string text;
        if (extOf(toPath(file)) == ".csv") {
            text = fmt::writeStringsCsv(d);
        } else {
            std::string existing;
            readFileText(file, existing);                        // absent for a new table: a fresh file
            text = fmt::writeOcstrings(d, existing);
        }
        if (writeFileTextAtomic(file, text)) res.filesWritten.push_back(file);
        else res.diagnostics.push_back(file + ": could not write");
    }
    return res;
}

} // namespace aver::editor
