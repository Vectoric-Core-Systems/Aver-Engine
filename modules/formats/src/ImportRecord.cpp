// .ocimport: the import record. See ImportRecord.hpp.
#include "aver/formats/ImportRecord.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

std::string lowerOf(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Splits "KEY rest of the line" at the first blank; the rest keeps its inner spaces.
void splitKey(std::string_view line, std::string_view& key, std::string_view& rest) {
    const usize sp = line.find_first_of(" \t");
    if (sp == std::string_view::npos) { key = line; rest = {}; return; }
    key = line.substr(0, sp);
    rest = trim(line.substr(sp + 1));
}

bool parseU64(std::string_view s, u64& out) {
    if (s.empty()) return false;
    u64 v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + u64(c - '0');
    }
    out = v;
    return true;
}

bool parseI64(std::string_view s, i64& out) {
    if (s.empty()) return false;
    const bool neg = s.front() == '-';
    u64 mag = 0;
    if (!parseU64(neg ? s.substr(1) : s, mag)) return false;
    out = neg ? -static_cast<i64>(mag) : static_cast<i64>(mag);
    return true;
}

} // namespace

bool parseImportRecord(std::string_view text, ImportRecord& out, std::string* why) {
    ImportRecord r;
    r.version = 0;
    bool header = false, haveSource = false;
    usize lineNo = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);        // CRLF
        if (trim(line).empty()) continue;
        const std::string at = ".ocimport line " + std::to_string(lineNo) + ": ";

        if (!header) {
            std::string_view key, rest;
            splitKey(trim(line), key, rest);
            if (key != "OCIMPORT") return fail(why, ".ocimport: not an import record (no OCIMPORT header)");
            u64 v = 0;
            if (!parseU64(rest, v) || v == 0) return fail(why, at + "OCIMPORT needs a version number");
            if (v > kImportRecordVersion)
                return fail(why, at + "record version " + std::to_string(v) + " is newer than this editor reads");
            r.version = static_cast<u32>(v);
            header = true;
            continue;
        }

        std::string_view key, rest;
        splitKey(trim(line), key, rest);
        if (key == "FORMAT") {
            r.format = std::string(rest);
        } else if (key == "SOURCE") {
            r.source = std::string(rest);
            haveSource = !r.source.empty();
        } else if (key == "SOURCESIZE") {
            if (!parseU64(rest, r.sourceSize)) return fail(why, at + "SOURCESIZE is not a number");
        } else if (key == "SOURCEMTIME") {
            if (!parseI64(rest, r.sourceMTime)) return fail(why, at + "SOURCEMTIME is not a number");
        } else if (key == "OPTIONS") {
            r.options = std::string(rest);
        } else if (key == "OUTPUT") {
            std::string_view kind, file;
            splitKey(rest, kind, file);
            if (kind.empty() || file.empty()) return fail(why, at + "OUTPUT needs a kind and a file");
            r.outputs.push_back({std::string(kind), std::string(file)});
        } else {
            r.extraLines.push_back(std::string(line));                              // unknown: kept verbatim
        }
    }
    if (!header) return fail(why, ".ocimport: empty record");
    if (r.format.empty()) return fail(why, ".ocimport: no FORMAT line");
    if (!haveSource) return fail(why, ".ocimport: no SOURCE line");
    out = std::move(r);
    return true;
}

std::string writeImportRecordText(const ImportRecord& in) {
    std::string s;
    s += "OCIMPORT " + std::to_string(in.version ? in.version : kImportRecordVersion) + "\n";
    s += "FORMAT " + in.format + "\n";
    s += "SOURCE " + in.source + "\n";
    s += "SOURCESIZE " + std::to_string(in.sourceSize) + "\n";
    s += "SOURCEMTIME " + std::to_string(in.sourceMTime) + "\n";
    if (!in.options.empty()) s += "OPTIONS " + in.options + "\n";
    for (const ImportRecordOutput& o : in.outputs) s += "OUTPUT " + o.kind + " " + o.file + "\n";
    for (const std::string& e : in.extraLines) s += e + "\n";
    return s;
}

bool loadImportRecord(const std::string& path, ImportRecord& out, std::string* why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(why, ".ocimport: cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseImportRecord(ss.str(), out, why);
}

bool saveImportRecord(const std::string& path, const ImportRecord& in, std::string* why) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(why, ".ocimport: cannot write " + path);
    const std::string text = writeImportRecordText(in);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return fail(why, ".ocimport: short write on " + path);
    return true;
}

std::string importRecordSourcePath(const ImportRecord& r, const std::string& projectDir) {
    const std::string& s = r.source;
    const bool absolute = !s.empty() && (s[0] == '/' || s[0] == '\\' || (s.size() > 1 && s[1] == ':'));
    if (absolute || projectDir.empty()) return s;
    std::string dir = projectDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir + "/" + s;
}

std::string importRecordPathFor(const std::string& destDir, const std::string& stem) {
    std::string dir = destDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir + "/" + stem + ".ocimport";
}

bool importRecordHasOutput(const ImportRecord& r, std::string_view file) {
    std::string want = lowerOf(file);
    for (char& c : want) if (c == '\\') c = '/';
    for (const ImportRecordOutput& o : r.outputs) {
        std::string have = lowerOf(o.file);
        for (char& c : have) if (c == '\\') c = '/';
        if (have == want) return true;
    }
    return false;
}

} // namespace aver::fmt
