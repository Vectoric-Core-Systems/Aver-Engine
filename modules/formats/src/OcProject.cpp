#include "aver/formats/OcProject.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Version.hpp"

#include <charconv>
#include <filesystem>
#include <string>
#include <vector>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// The rest of the line after the key, comment-stripped. AUTHOR and NAME are prose, so they must
// not be split on whitespace the way a numeric row is.
std::string_view restOfLine(std::string_view line, std::string_view key) {
    std::string_view r = line.substr(key.size());
    return trim(r);
}

} // namespace

bool parseOcproject(std::string_view text, ProjectDesc& out, std::string* err) {
    const std::string dir = out.dir, manifest = out.manifestPath; // preserved across the reset
    out = ProjectDesc{};
    out.dir = dir;
    out.manifestPath = manifest;

    // A manifest is the one .oc* file people hand-author in a text editor, and Notepad and
    // PowerShell's `Out-File -Encoding utf8` both prepend a UTF-8 BOM. Left in place it becomes
    // part of the first token, so OCPROJECT stops matching and the file reads as "not a project".
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF &&
        static_cast<u8>(text[1]) == 0xBB && static_cast<u8>(text[2]) == 0xBF)
        text = text.substr(3);

    bool sawHeader = false;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];

        if (equalsCI(key, "OCPROJECT")) {
            sawHeader = true;
            if (t.size() > 1) out.version = parseI32(t[1], 1);
        } else if (equalsCI(key, "NAME")) {
            out.name = std::string(restOfLine(line, key));
        } else if (equalsCI(key, "ENGINE")) {
            if (t.size() > 1) out.engineName = std::string(t[1]);
            if (t.size() > 2) out.engineMinVersion = std::string(t[2]);
        } else if (equalsCI(key, "CONTENT")) {
            if (t.size() > 1) out.contentRoot = std::string(t[1]);
        } else if (equalsCI(key, "STARTMAP")) {
            if (t.size() > 1) out.startMap = std::string(t[1]);
        } else if (equalsCI(key, "AUTHOR")) {
            out.author = std::string(restOfLine(line, key));
        } else if (equalsCI(key, "RENDER.GI")) {
            if (t.size() > 1) out.giQuality = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RAYTRACING")) {
            if (t.size() > 1) out.rayTracing = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.PATHTRACING")) {
            if (t.size() > 1) out.pathTracing = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.VOXELRES")) {
            if (t.size() > 1) out.voxelResolution = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.GIINTENSITY")) {
            if (t.size() > 1) out.giIntensity = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "RENDER.GIDISTANCE")) {
            if (t.size() > 1) out.giMaxDistance = static_cast<f32>(parseF64(t[1], -1.0));
        }
        // Everything else is ignored on purpose — the format is forward-compatible, so a key this
        // build has never heard of (dependencies, plugins, cook targets) must not fail the load.
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocproject: no OCPROJECT header line";
        return false;
    }
    if (out.name.empty()) {
        if (err) *err = "missing NAME";
        return false;
    }
    return true;
}

bool loadOcproject(const std::string& path, ProjectDesc& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "cannot read file: " + path;
        return false;
    }

    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    out = ProjectDesc{};
    out.manifestPath = ec ? path : abs.string();
    out.dir = std::filesystem::path(out.manifestPath).parent_path().string();

    if (!parseOcproject(text, out, err)) return false;

    // ENGINE is the compatibility gate. A project asking for a build we are not is a refusal, not
    // a warning: loading it anyway would silently give the author an engine missing whatever they
    // depended on, and the failure would surface later as broken content.
    if (!out.engineName.empty() && !equalsCI(out.engineName, kEngineName)) {
        if (err)
            *err = "project targets engine '" + out.engineName + "', this is " + std::string(kEngineName);
        return false;
    }
    if (!out.engineMinVersion.empty() && compareVersions(out.engineMinVersion, kEngineVersion) > 0) {
        if (err)
            *err = "project needs " + std::string(kEngineName) + " " + out.engineMinVersion +
                   " or newer; this build is " + std::string(kEngineVersion);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// writeOcproject
// ---------------------------------------------------------------------------------------------

namespace {

// One "KEY value" line, or nothing when the value is not stated.
void appendKey(std::string& out, const char* key, int v) {
    if (v < 0) return;
    out += key; out += ' '; out += std::to_string(v); out += '\n';
}
void appendKey(std::string& out, const char* key, f32 v) {
    if (v < 0.0f) return;
    // Shortest round-trip, and NOT %g: the OC dialect's own reader is from_chars, which does not
    // accept an exponent-free locale-formatted number written by printf under a comma locale.
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    if (r.ec != std::errc{}) return;
    out += key; out += ' '; out.append(buf, static_cast<usize>(r.ptr - buf)); out += '\n';
}

// Is this line one of the keys writeOcproject owns? Only those are replaced; everything else --
// comments, blank lines, and any key a newer build wrote -- passes through untouched.
bool isOwnedKey(std::string_view line) {
    const std::string_view l = trim(line);
    if (l.empty() || l[0] == '#') return false;
    static const char* kOwned[] = {
        "NAME", "ENGINE", "CONTENT", "STARTMAP", "AUTHOR",
        "RENDER.GI", "RENDER.RAYTRACING", "RENDER.PATHTRACING",
        "RENDER.VOXELRES", "RENDER.GIINTENSITY", "RENDER.GIDISTANCE",
    };
    const std::vector<std::string_view> t = splitWhitespace(l);
    if (t.empty()) return false;
    for (const char* k : kOwned) if (equalsCI(t[0], k)) return true;
    return false;
}

} // namespace

std::string writeOcproject(const ProjectDesc& d, std::string_view existing) {
    // What the owned keys become. Built first so the rewrite below is a lookup rather than a
    // second copy of the formatting rules.
    std::string owned;
    if (!d.name.empty())             { owned += "NAME "; owned += d.name; owned += '\n'; }
    if (!d.engineName.empty()) {
        owned += "ENGINE "; owned += d.engineName;
        if (!d.engineMinVersion.empty()) { owned += ' '; owned += d.engineMinVersion; }
        owned += '\n';
    }
    if (!d.contentRoot.empty())      { owned += "CONTENT ";  owned += d.contentRoot; owned += '\n'; }
    if (!d.startMap.empty())         { owned += "STARTMAP "; owned += d.startMap;    owned += '\n'; }
    if (!d.author.empty())           { owned += "AUTHOR ";   owned += d.author;      owned += '\n'; }
    appendKey(owned, "RENDER.GI",          d.giQuality);
    appendKey(owned, "RENDER.RAYTRACING",  d.rayTracing);
    appendKey(owned, "RENDER.PATHTRACING", d.pathTracing);
    appendKey(owned, "RENDER.VOXELRES",    d.voxelResolution);
    appendKey(owned, "RENDER.GIINTENSITY", d.giIntensity);
    appendKey(owned, "RENDER.GIDISTANCE",  d.giMaxDistance);

    // A FRESH file: header, then the keys. The header line is not optional -- parseOcproject
    // refuses a file without it, so a writer that omitted it would produce a manifest the engine
    // could not load.
    if (trim(existing).empty()) {
        std::string out = "OCPROJECT " + std::to_string(d.version > 0 ? d.version : 1) + "\n";
        out += "# Written by the Aver Engine editor.\n";
        out += owned;
        return out;
    }

    // AN EXISTING file: copy it through, dropping the owned keys, and put the owned block where the
    // FIRST of them was. Anchoring to the first rather than appending keeps a hand-arranged manifest
    // recognisable -- SkyForge's has a comment block above NAME that would otherwise end up
    // describing a file whose content had moved below it.
    std::string out;
    out.reserve(existing.size() + owned.size() + 64);
    bool placed = false;
    bool sawHeader = false;
    // `pos < size`, NOT `<=`. A file that ends with a newline -- every one of them -- leaves pos
    // exactly at size after its last real line, and a `<=` loop then processes one more, empty,
    // segment and emits a newline for it. That is one blank line appended per save: the file grows
    // every time somebody presses the button, which is the slow corruption this writer exists not to
    // cause. Caught by the idempotence assertion in FormatTest, which is the only kind of test that
    // finds it -- a single write looks perfect.
    usize pos = 0;
    while (pos < existing.size()) {
        usize nl = existing.find('\n', pos);
        const bool last = (nl == std::string_view::npos);
        if (last) nl = existing.size();
        const std::string_view raw = existing.substr(pos, nl - pos);
        pos = nl + 1;

        const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(raw)));
        if (!sawHeader && !t.empty() && equalsCI(t[0], "OCPROJECT")) {
            sawHeader = true;
            // Rewritten rather than copied, so a version bump is expressible.
            out += "OCPROJECT "; out += std::to_string(d.version > 0 ? d.version : 1); out += '\n';
            if (last) break;
            continue;
        }
        if (isOwnedKey(raw)) {
            if (!placed) { placed = true; out += owned; }
            if (last) break;
            continue;   // the old line is dropped; its value is already in `owned`
        }
        out += raw;
        out += '\n';
        if (last) break;
    }
    // No owned key was in the file at all -- possible for a manifest that is only a header and
    // comments. The block still has to land somewhere.
    if (!placed) out += owned;
    return out;
}

} // namespace aver::fmt
