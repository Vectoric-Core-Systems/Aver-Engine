// .ocproject reader and writer: parses a project manifest and rewrites it in place.
#include "aver/formats/OcProject.hpp"

#include <cstdio>
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Version.hpp"

#include <cctype>
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
            // REFUSED, RATHER THAN READ AS VERSION 1. This number was parsed, round-tripped and
            // never once compared to anything, so a future OCPROJECT 2 -- whatever it came to mean --
            // would be read by THIS build as if every key still meant what it means today, and then
            // written back out having quietly dropped whatever it did not understand. `.ocmat` and
            // the AVR1 container both refuse an unsupported version; these text formats were the
            // odd ones out, and a version field nobody checks is a field that cannot be used.
            //
            // A CEILING, NOT AN EQUALITY: a project written by an OLDER engine is the migration
            // system's business (modules/upgrade), and it opens those on purpose. Only the future is
            // unreadable.
            if (out.version > kOcProjectVersion) {
                if (err) *err = "this project is OCPROJECT version " + std::to_string(out.version) +
                                ", and this engine understands up to " +
                                std::to_string(kOcProjectVersion) + " -- it was written by a newer build";
                return false;
            }
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
        } else if (equalsCI(key, "RENDER.RDSTAGES")) {
            if (t.size() > 1) out.rdStages = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.PTBOUNCES")) {
            if (t.size() > 1) out.ptBounces = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.GICONES")) {
            if (t.size() > 1) out.giCones = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.GIMODE")) {
            if (t.size() > 1) out.giMode = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.DENOISER")) {
            if (t.size() > 1) out.denoiser = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RESTIRVISIBILITY")) {
            if (t.size() > 1) out.restirVisibility = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.RESTIRHISTORY")) {
            if (t.size() > 1) out.restirHistory = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.REFRACTIONMODE")) {
            if (t.size() > 1) out.refractionMode = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.REFRACTIONSTRENGTH")) {
            if (t.size() > 1) out.refractionStrength = static_cast<f32>(parseF64(t[1]));
        } else if (equalsCI(key, "RENDER.REFRACTIONEDGEFADE")) {
            if (t.size() > 1) out.refractionEdgeFade = static_cast<f32>(parseF64(t[1]));
        } else if (equalsCI(key, "RENDER.LODSELECT")) {
            if (t.size() > 1) out.lodSelect = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.LODTHRESHOLD")) {
            if (t.size() > 1) out.lodThresholdPx = static_cast<f32>(parseF64(t[1]));
        } else if (equalsCI(key, "RENDER.OCCLUSIONCULL")) {
            if (t.size() > 1) out.occlusionCull = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.DEPTHPREPASS")) {
            if (t.size() > 1) out.depthPrepass = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.FRAMEBUDGETMS")) {
            if (t.size() > 1) out.frameBudgetMs = static_cast<f32>(parseF64(t[1]));
        } else if (equalsCI(key, "RENDER.AVERSR")) {
            if (t.size() > 1) out.averSr = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.BACKEND")) {
            // Stored verbatim and lowercased; validated where it is USED, not here. A manifest naming
            // a backend this build has no support for is not a broken manifest -- the same file is
            // meant to open on a machine that does.
            if (t.size() > 1) { out.backend = t[1]; for (char& ch : out.backend) ch = static_cast<char>(::tolower(ch)); }
        } else if (equalsCI(key, "RENDER.MSAA")) {
            if (t.size() > 1) out.msaa = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.MESHSHADERS")) {
            if (t.size() > 1) out.meshShaders = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.GIUPDATEINTERVAL")) {
            if (t.size() > 1) out.giUpdateInterval = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.GIVOLUME")) {
            // ALL FOUR OR NONE, for PHYSICS.GRAVITY's reason immediately below: a half-stated volume
            // would keep defaults the author believed they had replaced.
            if (t.size() > 4) {
                for (int i = 0; i < 3; ++i) out.giCenter[i] = static_cast<f32>(parseF64(t[1 + i]));
                out.giExtent = static_cast<f32>(parseF64(t[4]));
                out.hasGiVolume = true;
            }
        } else if (equalsCI(key, "RENDER.EXPOSURE")) {
            // -1.0 ON A MALFORMED VALUE, not parseF64's own 0.0 default, and the difference is not
            // cosmetic for this pair: zero is a legal authored exposure and a legal authored bloom,
            // so falling back to 0 would turn "RENDER.BLOOM banana" into a black-and-bloomless frame
            // the author never asked for. Unreadable and absent mean the same thing here -- leave
            // rhi::PostSettings' compiled default alone -- which is what -1 says.
            if (t.size() > 1) out.postExposure = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "RENDER.BLOOM")) {
            if (t.size() > 1) out.postBloom = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "RENDER.AUTOEXPOSURE")) {
            if (t.size() > 1) out.postAutoExposure = parseI32(t[1], -1);
        } else if (equalsCI(key, "RENDER.TONEMAP")) {
            if (t.size() > 1) out.postTonemap = parseI32(t[1], -1);
        } else if (equalsCI(key, "WINDOW.TITLE")) {
            out.windowTitle = std::string(restOfLine(line, key));
        } else if (equalsCI(key, "WINDOW.SIZE")) {
            // BOTH OR NEITHER: half a resolution is not a resolution.
            if (t.size() > 2) {
                out.windowWidth  = parseI32(t[1], -1);
                out.windowHeight = parseI32(t[2], -1);
            }
        } else if (equalsCI(key, "WINDOW.RESIZABLE")) {
            if (t.size() > 1) out.windowResizable = parseI32(t[1], -1);
        } else if (equalsCI(key, "WINDOW.FULLSCREEN")) {
            if (t.size() > 1) out.windowFullscreen = parseI32(t[1], -1);
        } else if (equalsCI(key, "IMPORT.SCALE")) {
            if (t.size() > 1) out.importScale = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "IMPORT.CONVERTAXES")) {
            if (t.size() > 1) out.importConvertAxes = parseI32(t[1], -1);
        } else if (equalsCI(key, "IMPORT.GENNORMALS")) {
            if (t.size() > 1) out.importGenNormals = parseI32(t[1], -1);
        } else if (equalsCI(key, "IMPORT.GENMIPS")) {
            if (t.size() > 1) out.importGenMips = parseI32(t[1], -1);
        } else if (equalsCI(key, "IMPORT.MAXTEXTURE")) {
            if (t.size() > 1) out.importMaxTexture = parseI32(t[1], -1);
        } else if (equalsCI(key, "STREAM.LOADRADIUS")) {
            if (t.size() > 1) out.streamLoadRadius = parseI32(t[1], -1);
        } else if (equalsCI(key, "STREAM.EVICTRADIUS")) {
            if (t.size() > 1) out.streamEvictRadius = parseI32(t[1], -1);
        } else if (equalsCI(key, "STREAM.LOADBUDGET")) {
            if (t.size() > 1) out.streamLoadBudget = parseI32(t[1], -1);
        } else if (equalsCI(key, "STREAM.EVICTBUDGET")) {
            if (t.size() > 1) out.streamEvictBudget = parseI32(t[1], -1);
        } else if (equalsCI(key, "STREAM.VERTICALRADIUS")) {
            if (t.size() > 1) out.streamVerticalRadius = parseI32(t[1], -1);
        } else if (equalsCI(key, "STREAM.LEADSECONDS")) {
            if (t.size() > 1) out.streamLeadSeconds = static_cast<f32>(parseF64(t[1], -1.0));
        } else if (equalsCI(key, "PHYSICS.MAXBODIES")) {
            if (t.size() > 1) out.physMaxBodies = parseI32(t[1], -1);
        } else if (equalsCI(key, "PHYSICS.MAXBODYPAIRS")) {
            if (t.size() > 1) out.physMaxBodyPairs = parseI32(t[1], -1);
        } else if (equalsCI(key, "PHYSICS.MAXCONTACTS")) {
            if (t.size() > 1) out.physMaxContacts = parseI32(t[1], -1);
        } else if (equalsCI(key, "PHYSICS.TEMPALLOCMB")) {
            if (t.size() > 1) out.physTempAllocatorMb = parseI32(t[1], -1);
        } else if (equalsCI(key, "PHYSICS.GRAVITY")) {
            // All three or none: a partial vector is worse than no vector, because two of the axes
            // would silently keep a default the author thought they had replaced.
            if (t.size() > 3) {
                out.gravity[0] = static_cast<f32>(parseF64(t[1]));
                out.gravity[1] = static_cast<f32>(parseF64(t[2]));
                out.gravity[2] = static_cast<f32>(parseF64(t[3]));
                out.hasGravity = true;
            }
        } else if (equalsCI(key, "PHYSICS.FIXEDSTEP")) {
            if (t.size() > 1) out.fixedStep = static_cast<f32>(parseF64(t[1]));
        } else if (equalsCI(key, "AUDIO.MASTER")) {
            if (t.size() > 1) { out.masterVolume = static_cast<f32>(parseF64(t[1])); out.hasAudioMix = true; }
        } else if (equalsCI(key, "AUDIO.BUS")) {
            // Four on one line, in audio_abi.h's own bus order. One key rather than four keeps the
            // mix readable as a mix, and makes a partial write impossible.
            if (t.size() > 4) {
                for (int i = 0; i < 4; ++i) out.busVolume[i] = static_cast<f32>(parseF64(t[static_cast<usize>(i) + 1]));
                out.hasAudioMix = true;
            }
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
void appendKey(std::string& out, const char* key, const std::string& v) {
    if (v.empty()) return;
    out += key; out += ' '; out += v; out += '\n';
}
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
        "RENDER.RTRENDERMODE", "RENDER.RDSTAGES", "RENDER.PTBOUNCES", "RENDER.LAYEREDBSDF",
        "RENDER.GICONES", "RENDER.GIMODE", "RENDER.DENOISER", "RENDER.RESTIRVISIBILITY",
        "RENDER.RESTIRHISTORY",
        "RENDER.REFRACTIONMODE", "RENDER.REFRACTIONSTRENGTH",
        "RENDER.REFRACTIONEDGEFADE", "RENDER.LODSELECT", "RENDER.LODTHRESHOLD",
        "RENDER.OCCLUSIONCULL", "RENDER.DEPTHPREPASS",
        // IN EMIT ORDER, and these two were missing. A key appended to `owned` above but absent
        // here is copied through as the author's "unowned text" AND re-emitted, so the manifest
        // grows a duplicate on every save -- and because the owned block splices in at the first
        // owned key while the author's line stays below it, last-write-wins parsing makes the STALE
        // line win. Changing the renderer appeared to work and reverted on reload.
        "RENDER.BACKEND", "RENDER.FRAMEBUDGETMS", "RENDER.AVERSR",
        "RENDER.MSAA", "RENDER.MESHSHADERS", "RENDER.GIUPDATEINTERVAL", "RENDER.GIVOLUME",
        "RENDER.EXPOSURE", "RENDER.BLOOM", "RENDER.AUTOEXPOSURE", "RENDER.TONEMAP",
        "WINDOW.TITLE", "WINDOW.SIZE", "WINDOW.RESIZABLE", "WINDOW.FULLSCREEN",
        "IMPORT.SCALE", "IMPORT.CONVERTAXES", "IMPORT.GENNORMALS", "IMPORT.GENMIPS",
        "IMPORT.MAXTEXTURE",
        "STREAM.LOADRADIUS", "STREAM.EVICTRADIUS", "STREAM.LOADBUDGET", "STREAM.EVICTBUDGET",
        "STREAM.VERTICALRADIUS", "STREAM.LEADSECONDS",
        "PHYSICS.MAXBODIES", "PHYSICS.MAXBODYPAIRS", "PHYSICS.MAXCONTACTS", "PHYSICS.TEMPALLOCMB",
        "PHYSICS.GRAVITY", "PHYSICS.FIXEDSTEP", "AUDIO.MASTER", "AUDIO.BUS",
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
    appendKey(owned, "RENDER.RDSTAGES", d.rdStages);
    appendKey(owned, "RENDER.PTBOUNCES", d.ptBounces);
    appendKey(owned, "RENDER.LAYEREDBSDF", d.layeredBsdf);
    appendKey(owned, "RENDER.GICONES", d.giCones);
    appendKey(owned, "RENDER.GIMODE", d.giMode);
    appendKey(owned, "RENDER.DENOISER", d.denoiser);
    appendKey(owned, "RENDER.RESTIRVISIBILITY", d.restirVisibility);
    appendKey(owned, "RENDER.RESTIRHISTORY", d.restirHistory);
    appendKey(owned, "RENDER.REFRACTIONMODE", d.refractionMode);
    appendKey(owned, "RENDER.REFRACTIONSTRENGTH", d.refractionStrength);
    appendKey(owned, "RENDER.REFRACTIONEDGEFADE", d.refractionEdgeFade);
    appendKey(owned, "RENDER.LODSELECT", d.lodSelect);
    appendKey(owned, "RENDER.LODTHRESHOLD", d.lodThresholdPx);
    appendKey(owned, "RENDER.OCCLUSIONCULL", d.occlusionCull);
    appendKey(owned, "RENDER.DEPTHPREPASS", d.depthPrepass);
    if (!d.backend.empty()) appendKey(owned, "RENDER.BACKEND", d.backend);
    appendKey(owned, "RENDER.FRAMEBUDGETMS", d.frameBudgetMs);
    appendKey(owned, "RENDER.AVERSR", d.averSr);
    appendKey(owned, "RENDER.MSAA", d.msaa);
    appendKey(owned, "RENDER.MESHSHADERS", d.meshShaders);
    appendKey(owned, "RENDER.GIUPDATEINTERVAL", d.giUpdateInterval);
    // Same presence-flag shape as PHYSICS.GRAVITY below, and written with snprintf for the same
    // reason: %g gives the shortest round-tripping spelling, so a value the author typed comes back
    // looking like what they typed.
    if (d.hasGiVolume) {
        char b[200];
        std::snprintf(b, sizeof b, "RENDER.GIVOLUME %g %g %g %g\n",
                      static_cast<double>(d.giCenter[0]), static_cast<double>(d.giCenter[1]),
                      static_cast<double>(d.giCenter[2]), static_cast<double>(d.giExtent));
        owned += b;
    }

    // THE POST CHAIN. appendKey's f32 overload is exactly the right rule for these two: it skips a
    // NEGATIVE value, which is the sentinel, and emits a zero, which is an author saying "no bloom".
    // Had the sentinel been 0 instead, this line could not have told the two apart and a project
    // would have been unable to state the one bloom value anybody deliberately sets.
    appendKey(owned, "RENDER.EXPOSURE", d.postExposure);
    appendKey(owned, "RENDER.BLOOM", d.postBloom);
    appendKey(owned, "RENDER.AUTOEXPOSURE", d.postAutoExposure);
    appendKey(owned, "RENDER.TONEMAP", d.postTonemap);

    // WINDOW.* -- how a shipped game presents itself. TITLE is prose, so it is written directly
    // rather than through appendKey, which is numeric.
    if (!d.windowTitle.empty()) owned += "WINDOW.TITLE " + d.windowTitle + "\n";
    if (d.windowWidth > 0 && d.windowHeight > 0) {
        char b[96];
        std::snprintf(b, sizeof b, "WINDOW.SIZE %d %d\n", d.windowWidth, d.windowHeight);
        owned += b;
    }
    appendKey(owned, "WINDOW.RESIZABLE", d.windowResizable);
    appendKey(owned, "WINDOW.FULLSCREEN", d.windowFullscreen);

    appendKey(owned, "IMPORT.SCALE", d.importScale);
    appendKey(owned, "IMPORT.CONVERTAXES", d.importConvertAxes);
    appendKey(owned, "IMPORT.GENNORMALS", d.importGenNormals);
    appendKey(owned, "IMPORT.GENMIPS", d.importGenMips);
    appendKey(owned, "IMPORT.MAXTEXTURE", d.importMaxTexture);

    appendKey(owned, "STREAM.LOADRADIUS", d.streamLoadRadius);
    appendKey(owned, "STREAM.EVICTRADIUS", d.streamEvictRadius);
    appendKey(owned, "STREAM.LOADBUDGET", d.streamLoadBudget);
    appendKey(owned, "STREAM.EVICTBUDGET", d.streamEvictBudget);
    appendKey(owned, "STREAM.VERTICALRADIUS", d.streamVerticalRadius);
    appendKey(owned, "STREAM.LEADSECONDS", d.streamLeadSeconds);

    appendKey(owned, "PHYSICS.MAXBODIES", d.physMaxBodies);
    appendKey(owned, "PHYSICS.MAXBODYPAIRS", d.physMaxBodyPairs);
    appendKey(owned, "PHYSICS.MAXCONTACTS", d.physMaxContacts);
    appendKey(owned, "PHYSICS.TEMPALLOCMB", d.physTempAllocatorMb);

    // WRITTEN ON THEIR PRESENCE FLAG, not on a sentinel -- appendKey's "negative means unstated"
    // rule cannot express a downward gravity or a muted bus. See OcProjectDesc for both reasons.
    if (d.hasGravity) {
        char b[160];
        std::snprintf(b, sizeof b, "PHYSICS.GRAVITY %g %g %g\n",
                      static_cast<double>(d.gravity[0]), static_cast<double>(d.gravity[1]),
                      static_cast<double>(d.gravity[2]));
        owned += b;
    }
    appendKey(owned, "PHYSICS.FIXEDSTEP", d.fixedStep);
    if (d.hasAudioMix) {
        char b[200];
        std::snprintf(b, sizeof b, "AUDIO.MASTER %g\nAUDIO.BUS %g %g %g %g\n",
                      static_cast<double>(d.masterVolume),
                      static_cast<double>(d.busVolume[0]), static_cast<double>(d.busVolume[1]),
                      static_cast<double>(d.busVolume[2]), static_cast<double>(d.busVolume[3]));
        owned += b;
    }

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
