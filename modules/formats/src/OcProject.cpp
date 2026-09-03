// .ocproject reader and writer: parses a project manifest and rewrites it in place.
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

// The trimmed remainder of the line after the key, for prose values.
std::string_view restOfLine(std::string_view line, std::string_view key) {
    std::string_view r = line.substr(key.size());
    return trim(r);
}

} // namespace

// Parses a manifest from memory, preserving `dir` and `manifestPath`. Unknown keys are ignored.
bool parseOcproject(std::string_view text, ProjectDesc& out, std::string* err) {
    const std::string dir = out.dir, manifest = out.manifestPath;
    out = ProjectDesc{};
    out.dir = dir;
    out.manifestPath = manifest;

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
        } else if (equalsCI(key, "CREATEDWITH")) {
            if (t.size() > 1) out.createdWith = std::string(t[1]);
        } else if (equalsCI(key, "CONTENT")) {
            if (t.size() > 1) out.contentRoot = std::string(t[1]);
        } else if (equalsCI(key, "STARTMAP")) {
            if (t.size() > 1) out.startMap = std::string(t[1]);
        } else if (equalsCI(key, "DRONE.GRAPH")) {
            if (t.size() > 1) out.droneGraph = std::string(t[1]);
        } else if (equalsCI(key, "INPUT.SCHEME")) {
            if (t.size() > 1) out.inputScheme = std::string(t[1]);
        } else if (equalsCI(key, "AUTHOR")) {
            out.author = std::string(restOfLine(line, key));
        } else if (equalsCI(key, "RENDER.GI")) {
            if (t.size() > 1) out.giQuality = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RAYTRACING")) {
            if (t.size() > 1) out.rayTracing = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.PATHTRACING")) {
            if (t.size() > 1) out.pathTracing = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.LAYEREDBSDF")) {
            if (t.size() > 1) out.layeredBsdf = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.VOXELRES")) {
            if (t.size() > 1) out.voxelResolution = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.GIINTENSITY")) {
            if (t.size() > 1) out.giIntensity = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "RENDER.GIDISTANCE")) {
            if (t.size() > 1) out.giMaxDistance = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "RENDER.RTSHADOWRAYS")) {
            if (t.size() > 1) out.rtShadowRays = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RTPIXELSPERRAY")) {
            if (t.size() > 1) out.rtPixelsPerRayTile = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RTSHADOWDENOISE")) {
            if (t.size() > 1) out.rtShadowDenoise = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RTRENDERMODE")) {
            if (t.size() > 1) out.rtRenderMode = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.PTBOUNCES")) {
            if (t.size() > 1) out.ptBounces = parseI32(t[1], -1);
        }
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

// Loads a manifest from disk and rejects one whose ENGINE line this build cannot honour.
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

namespace {

// Appends one "KEY value" line, or nothing when the value is negative.
void appendKey(std::string& out, const char* key, int v) {
    if (v < 0) return;
    out += key; out += ' '; out += std::to_string(v); out += '\n';
}
void appendKey(std::string& out, const char* key, f32 v) {
    if (v < 0.0f) return;
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    if (r.ec != std::errc{}) return;
    out += key; out += ' '; out.append(buf, static_cast<usize>(r.ptr - buf)); out += '\n';
}

// True when the line's key is one writeOcproject owns and therefore replaces.
bool isOwnedKey(std::string_view line) {
    const std::string_view l = trim(line);
    if (l.empty() || l[0] == '#') return false;
    static const char* kOwned[] = {
        "NAME", "ENGINE", "CREATEDWITH", "CONTENT", "STARTMAP", "AUTHOR", "DRONE.GRAPH", "INPUT.SCHEME",
        "RENDER.GI", "RENDER.RAYTRACING", "RENDER.PATHTRACING",
        "RENDER.VOXELRES", "RENDER.GIINTENSITY", "RENDER.GIDISTANCE",
        "RENDER.RTSHADOWRAYS", "RENDER.RTPIXELSPERRAY", "RENDER.RTSHADOWDENOISE",
        "RENDER.RTRENDERMODE", "RENDER.PTBOUNCES", "RENDER.LAYEREDBSDF",
    };
    const std::vector<std::string_view> t = splitWhitespace(l);
    if (t.empty()) return false;
    for (const char* k : kOwned) if (equalsCI(t[0], k)) return true;
    return false;
}

} // namespace

// Serialises a manifest, copying every unowned line of `existing` through untouched.
std::string writeOcproject(const ProjectDesc& d, std::string_view existing) {
    std::string owned;
    if (!d.name.empty())             { owned += "NAME "; owned += d.name; owned += '\n'; }
    if (!d.engineName.empty()) {
        owned += "ENGINE "; owned += d.engineName;
        if (!d.engineMinVersion.empty()) { owned += ' '; owned += d.engineMinVersion; }
        owned += '\n';
    }
    // What last opened it, as against ENGINE's "what it needs at least". Written whenever it is
    // known, so a project stamped once carries its provenance forward through every later save.
    if (!d.createdWith.empty())      { owned += "CREATEDWITH "; owned += d.createdWith; owned += '\n'; }
    if (!d.contentRoot.empty())      { owned += "CONTENT ";  owned += d.contentRoot; owned += '\n'; }
    if (!d.startMap.empty())         { owned += "STARTMAP "; owned += d.startMap;    owned += '\n'; }
    if (!d.author.empty())           { owned += "AUTHOR ";   owned += d.author;      owned += '\n'; }
    // EMITTED BECAUSE IT IS AN OWNED KEY. isOwnedKey lists DRONE.GRAPH, so the writer strips whatever
    // line the file had; without this it would strip and never replace, and saving a project would
    // quietly delete its drone. Empty writes nothing -- a project with no drone graph has no line.
    if (!d.droneGraph.empty())       { owned += "DRONE.GRAPH "; owned += d.droneGraph; owned += '\n'; }
    // EMITTED BECAUSE IT IS AN OWNED KEY, for DRONE.GRAPH's exact reason immediately above: isOwnedKey
    // lists INPUT.SCHEME, so leaving this out would strip whatever line the file had without ever
    // replacing it -- silently deleting a project's input scheme reference on every save. Empty
    // writes nothing -- a project with no default scheme has no line.
    if (!d.inputScheme.empty())      { owned += "INPUT.SCHEME "; owned += d.inputScheme; owned += '\n'; }
    appendKey(owned, "RENDER.GI",          d.giQuality);
    appendKey(owned, "RENDER.RAYTRACING",  d.rayTracing);
    appendKey(owned, "RENDER.PATHTRACING", d.pathTracing);
    appendKey(owned, "RENDER.VOXELRES",    d.voxelResolution);
    appendKey(owned, "RENDER.GIINTENSITY", d.giIntensity);
    appendKey(owned, "RENDER.GIDISTANCE",  d.giMaxDistance);
    appendKey(owned, "RENDER.RTSHADOWRAYS",   d.rtShadowRays);
    appendKey(owned, "RENDER.RTPIXELSPERRAY", d.rtPixelsPerRayTile);
    appendKey(owned, "RENDER.RTSHADOWDENOISE", d.rtShadowDenoise);
    appendKey(owned, "RENDER.RTRENDERMODE", d.rtRenderMode);
    appendKey(owned, "RENDER.PTBOUNCES", d.ptBounces);
    appendKey(owned, "RENDER.LAYEREDBSDF", d.layeredBsdf);

    if (trim(existing).empty()) {
        std::string out = "OCPROJECT " + std::to_string(d.version > 0 ? d.version : 1) + "\n";
        out += "# Written by the Aver Engine editor.\n";
        out += owned;
        return out;
    }

    std::string out;
    out.reserve(existing.size() + owned.size() + 64);
    bool placed = false;
    bool sawHeader = false;
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
            out += "OCPROJECT "; out += std::to_string(d.version > 0 ? d.version : 1); out += '\n';
            if (last) break;
            continue;
        }
        if (isOwnedKey(raw)) {
            if (!placed) { placed = true; out += owned; }
            if (last) break;
            continue;
        }
        out += raw;
        out += '\n';
        if (last) break;
    }
    if (!placed) out += owned;

    // THE HEADER IS NOT OPTIONAL, and this branch used to omit it whenever `existing` carried no
    // OCPROJECT line of its own. Only the empty-existing branch above wrote one, so any caller
    // passing a non-empty preamble got back a manifest with every key and no header -- which
    // loadOcproject then refuses with "not an .ocproject: no OCPROJECT header line".
    //
    // That is not hypothetical. It broke NEW PROJECT ENTIRELY: ProjectScaffold::manifestText passes
    // three comment lines as `existing`, so every project the editor scaffolded was written with no
    // header and failed to load a moment later, from the very function that had just written it.
    // The guard then deleted the half-made folder, so the user saw a creation that simply refused.
    //
    // Prepended rather than fixed at the call site because the contract belongs here: this function
    // returns a manifest, and a manifest has a header. Any other caller passing a preamble -- a
    // template, an importer, a migration -- had the same bug waiting.
    if (!sawHeader)
        out.insert(0, "OCPROJECT " + std::to_string(d.version > 0 ? d.version : 1) + "\n");
    return out;
}

} // namespace aver::fmt
