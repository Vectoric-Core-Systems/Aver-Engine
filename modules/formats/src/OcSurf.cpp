// .ocsurf text reader and writer. See OcSurf.hpp for what the format is.
#include "aver/formats/OcSurf.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none
#include "aver/core/Log.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <unordered_set>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

// WHOLE TOKEN OR NOTHING: TextScan's parsers accept a numeric prefix ("0.6abc" reads as 0.6), which would turn a typo into a
// number nobody wrote.
template <class T>
bool parseWhole(std::string_view s, T& out) {
    const char* const end = s.data() + s.size();
    const auto r = aver::fromChars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

bool parseWholeInt(std::string_view s, i32& out) {
    const char* const end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

// The shortest text that reads back as the SAME f32, in fixed notation.
void appendNum(std::string& s, f32 v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    s.append(buf, r.ptr);
}

// 0..10, a negative value is "unset" (-1).
f32 normNumber(f32 v) {
    if (v < 0.0f) return -1.0f;
    return v > 10.0f ? 10.0f : v;
}

} // namespace

bool ocSurfNameValid(std::string_view name) {
    if (name.empty() || name.size() > 63) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool parseOcSurf(std::string_view text, OcSurfData& out, std::string* err) {
    out = OcSurfData{};

    OcSurfData d;
    bool sawHeader = false, warned = false;
    std::unordered_set<std::string> names;

    int lineNo = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;

        // trim drops the CR a CRLF file leaves at the end of every line.
        const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(rawLine)));
        if (t.empty()) continue;
        const std::string at = " (line " + std::to_string(lineNo) + ")";

        if (equalsCI(t[0], "OCSURF")) {
            if (sawHeader) return fail(err, "a second OCSURF header" + at);
            if (t.size() < 2) return fail(err, "OCSURF needs a version" + at);
            i32 version = 0;
            if (!parseWholeInt(t[1], version)) return fail(err, "bad .ocsurf version '" + std::string(t[1]) + "'" + at);
            if (version > 1) return fail(err, "this .ocsurf is version " + std::to_string(version) + ", newer than this engine reads (1)" + at);
            if (version < 1) return fail(err, "unsupported .ocsurf version '" + std::string(t[1]) + "'" + at);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail(err, "the first record must be OCSURF 1" + at);

        if (!equalsCI(t[0], "SURFACE")) {
            if (!warned) { AVER_WARN("[ocsurf] unknown record '{}' skipped (a newer table?){}", std::string(t[0]), at); warned = true; }
            continue;
        }

        if (t.size() < 2) return fail(err, "SURFACE needs a name" + at);
        OcSurfRecord rec;
        rec.name = std::string(t[1]);
        if (!ocSurfNameValid(rec.name))
            return fail(err, "bad surface name '" + rec.name + "' (letters, digits and underscore, 1 to 63)" + at);
        if (!names.insert(rec.name).second) return fail(err, "surface '" + rec.name + "' is declared twice" + at);
        if (d.surfaces.size() >= kOcSurfMaxSurfaces)
            return fail(err, "more than " + std::to_string(kOcSurfMaxSurfaces) + " surfaces" + at);

        for (usize i = 2; i < t.size(); i += 2) {
            const std::string_view key = t[i];
            const bool known = equalsCI(key, "friction") || equalsCI(key, "restitution") || equalsCI(key, "tag");
            if (!known) {
                // A key this engine does not know: skipped with its value, once per file.
                if (!warned) { AVER_WARN("[ocsurf] surface '{}': unknown key '{}' skipped (a newer table?){}", rec.name, std::string(key), at); warned = true; }
                continue;
            }
            if (i + 1 >= t.size()) return fail(err, "'" + std::string(key) + "' has no value for surface '" + rec.name + "'" + at);
            const std::string_view value = t[i + 1];
            if (equalsCI(key, "tag")) {
                if (!parseWholeInt(value, rec.tag)) return fail(err, "bad tag '" + std::string(value) + "' for surface '" + rec.name + "'" + at);
            } else {
                f32 v = 0.0f;
                if (!parseWhole(value, v) || !std::isfinite(v))
                    return fail(err, "bad " + std::string(key) + " '" + std::string(value) + "' for surface '" + rec.name + "'" + at);
                (equalsCI(key, "friction") ? rec.friction : rec.restitution) = normNumber(v);
            }
        }
        d.surfaces.push_back(std::move(rec));
    }

    if (!sawHeader) return fail(err, "empty or missing OCSURF header");
    out = std::move(d);
    return true;
}

std::string writeOcSurf(const OcSurfData& d) {
    std::string s = "OCSURF 1\n";
    for (const OcSurfRecord& r : d.surfaces) {
        s += "SURFACE " + r.name;
        if (r.friction >= 0.0f)    { s += " friction ";    appendNum(s, normNumber(r.friction)); }
        if (r.restitution >= 0.0f) { s += " restitution "; appendNum(s, normNumber(r.restitution)); }
        if (r.tag != 0)            { s += " tag " + std::to_string(r.tag); }
        s += '\n';
    }
    return s;
}

bool loadOcSurf(const std::string& path, OcSurfData& out, std::string* err) {
    // readFileText, not an ifstream of our own: it reports the open to the file trace, which is how a packaged game proves it
    // read this file from inside its package (platform/FileSystem.hpp).
    std::string text;
    if (!readFileText(path, text)) return fail(err, "cannot open " + path);
    return parseOcSurf(text, out, err);
}

bool saveOcSurf(const std::string& path, const OcSurfData& in, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (!writeFileTextAtomic(path, writeOcSurf(in))) return fail(err, "could not write " + path);
    return true;
}

} // namespace aver::fmt
