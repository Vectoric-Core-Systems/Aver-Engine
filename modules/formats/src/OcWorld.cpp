// .ocworld reader and writer: the text scanner and serialiser for the native world format.
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/core/Hash.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// Token `i` as a double, or `dflt` when the line is shorter than that.
f64 tokF(const std::vector<std::string_view>& t, usize i, f64 dflt = 0.0) {
    return i < t.size() ? parseF64(t[i]) : dflt;
}

// Formats a number for the text form: enough digits to round-trip, no trailing noise.
std::string num(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

} // namespace

// Parses an .ocworld or .ocmap from memory. Unknown records are skipped.
bool parseOcworld(std::string_view text, OcWorldData& out, std::string* err) {
    out = OcWorldData{};
    bool sawHeader = false;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        std::string_view key = t[0];

        if (equalsCI(key, "OCWORLD") || equalsCI(key, "OCMAP")) {
            out.version = t.size() > 1 ? parseI32(t[1], 1) : 1;
            sawHeader = true;
        } else if (equalsCI(key, "ID")) {
            out.contentId = t.size() > 1 ? parseU64(t[1]) : 0;
        } else if (equalsCI(key, "NAME")) {
            out.name = t.size() > 1 ? std::string(t[1]) : std::string();
        } else if (equalsCI(key, "BUILD")) {
            out.build = static_cast<u32>(t.size() > 1 ? parseU64(t[1]) : 0);
        } else if (equalsCI(key, "ALGO")) {
            out.algo = static_cast<u32>(t.size() > 1 ? parseU64(t[1]) : 3);
        } else if (equalsCI(key, "SPAWN")) {
            out.hasSpawn = true;
            out.spawnX = tokF(t, 1); out.spawnY = tokF(t, 2);
            out.spawnZ = tokF(t, 3); out.spawnYaw = tokF(t, 4);
        } else if (equalsCI(key, "SUN")) {
            out.hasSun = true;
            // `dir` and `elev`/`azim` are alternative spellings of the same field; the later token
            // wins. Degrees exist because the physical sky makes elevation the only input that
            // matters, and nobody authors a time of day as a unit vector.
            f64 elevDeg = 0.0, azimDeg = 0.0;
            bool sawElev = false, sawAzim = false;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "dir") && i + 3 < t.size()) {
                    out.sunDir[0] = parseF64(t[i+1]); out.sunDir[1] = parseF64(t[i+2]); out.sunDir[2] = parseF64(t[i+3]);
                    sawElev = sawAzim = false;
                } else if (equalsCI(t[i], "elev") && i + 1 < t.size()) {
                    elevDeg = parseF64(t[i+1]); sawElev = true;
                } else if (equalsCI(t[i], "azim") && i + 1 < t.size()) {
                    azimDeg = parseF64(t[i+1]); sawAzim = true;
                } else if (equalsCI(t[i], "color") && i + 3 < t.size()) {
                    out.sunColor[0] = parseF64(t[i+1]); out.sunColor[1] = parseF64(t[i+2]); out.sunColor[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "lux") && i + 1 < t.size()) {
                    out.sunLux = parseF64(t[i+1]);
                }
            }
            if (sawElev || sawAzim) {
                const f64 kDeg = 3.14159265358979 / 180.0;
                const f64 ce = std::cos(elevDeg * kDeg);
                out.sunDir[0] = ce * std::cos(azimDeg * kDeg);
                out.sunDir[1] = ce * std::sin(azimDeg * kDeg);
                out.sunDir[2] = std::sin(elevDeg * kDeg);
            }
        } else if (equalsCI(key, "SKY")) {
            out.hasSky = true;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "model") && i + 1 < t.size()) {
                    out.skyPhysical = !equalsCI(t[i+1], "authored");
                } else if (equalsCI(t[i], "mie") && i + 1 < t.size()) {
                    out.skyMieScatter = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "multiscatter") && i + 1 < t.size()) {
                    out.skyMultiScatter = parseF64(t[i+1]);
                } else if (equalsCI(t[i], "steps") && i + 1 < t.size()) {
                    out.skyViewSteps = parseI32(t[i+1], 0);
                } else if (equalsCI(t[i], "aerial") && i + 1 < t.size()) {
                    out.skyAerialSteps = parseI32(t[i+1], 0);
                }
            }
        } else if (equalsCI(key, "FOG")) {
            out.hasFog = true;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "density") && i + 1 < t.size()) out.fogDensity = parseF64(t[i+1]);
                else if (equalsCI(t[i], "color") && i + 3 < t.size()) {
                    out.fogColor[0] = parseF64(t[i+1]); out.fogColor[1] = parseF64(t[i+2]); out.fogColor[2] = parseF64(t[i+3]);
                }
            }
        } else if (equalsCI(key, "PCGVOLUME")) {
            OcPcgVolume v;
            for (usize i = 1; i < t.size(); ++i) {
                if      (equalsCI(t[i], "name")    && i + 1 < t.size()) v.name          = std::string(t[++i]);
                else if (equalsCI(t[i], "seed")    && i + 1 < t.size()) v.seed          = static_cast<i32>(parseF64(t[++i]));
                else if (equalsCI(t[i], "cell")    && i + 1 < t.size()) v.cellSizeCm    = parseF64(t[++i]);
                else if (equalsCI(t[i], "octaves") && i + 1 < t.size()) v.octaves       = static_cast<i32>(parseF64(t[++i]));
                else if (equalsCI(t[i], "floor")   && i + 1 < t.size()) v.coverageFloor = parseF64(t[++i]);
                else if (equalsCI(t[i], "bias")    && i + 1 < t.size()) v.coverageBias  = parseF64(t[++i]);
                // A BARE TOKEN, not `infinite 1`. It is a statement about what the field IS rather
                // than a value it carries, and it reads that way in the file.
                else if (equalsCI(t[i], "infinite")) v.infinite = true;
                else if (equalsCI(t[i], "bounds") && i + 6 < t.size()) {
                    v.infinite = false;
                    v.boundsMin[0] = parseF64(t[i+1]); v.boundsMin[1] = parseF64(t[i+2]); v.boundsMin[2] = parseF64(t[i+3]);
                    v.boundsMax[0] = parseF64(t[i+4]); v.boundsMax[1] = parseF64(t[i+5]); v.boundsMax[2] = parseF64(t[i+6]);
                    i += 6;
                }
            }
            // Guarded rather than trusted: a cell size of zero divides by zero in every sampler
            // that reads this, and the file is authored by hand.
            if (v.cellSizeCm <= 0.0) v.cellSizeCm = 1600.0;
            if (v.octaves < 1) v.octaves = 1;
            out.pcgVolumes.push_back(std::move(v));
        } else if (equalsCI(key, "PLACE") || equalsCI(key, "PLACEG")) {
            const bool g = equalsCI(key, "PLACEG");
            OcWorldPlacement p;
            p.asset = t.size() > 1 ? std::string(t[1]) : std::string();
            p.x = tokF(t, 2); p.y = tokF(t, 3); p.z = tokF(t, 4);
            p.yaw = tokF(t, 5); p.pitch = tokF(t, 6); p.roll = tokF(t, 7);
            usize next;
            if (g) {
                p.sx = tokF(t, 8, 1.0); p.sy = tokF(t, 9, 1.0); p.sz = tokF(t, 10, 1.0);
                next = 11;
            } else {
                const f64 s = tokF(t, 8, 1.0);
                p.sx = p.sy = p.sz = (s == 0.0 ? 1.0 : s);
                next = 9;
            }
            for (usize i = next; i < t.size(); ++i) {
                if (equalsCI(t[i], "nocollide")) p.collide = false;
                else if (p.material.empty()) p.material = std::string(t[i]);
            }
            p.objectId = fnv1a64(std::string_view(p.asset));
            out.placements.push_back(std::move(p));
        }
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocworld/.ocmap file (no OCWORLD or OCMAP header line)";
        return false;
    }
    if (out.contentId == 0 && !out.name.empty()) out.contentId = fnv1a64(std::string_view(out.name));
    return true;
}

// Loads an .ocworld from disk.
bool loadOcworld(const std::string& path, OcWorldData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcworld(text, out, err);
}

// Serialises a world to the text form. PLACE for a uniform scale, PLACEG otherwise.
std::string writeOcworld(const OcWorldData& w) {
    std::string s;
    s.reserve(256 + w.placements.size() * 96);
    s += "OCWORLD 1\n";
    s += "# Written by the Aver Engine editor. Centimetres, +X forward, +Y right, +Z up.\n";

    char idbuf[32];
    const u64 id = w.contentId ? w.contentId : fnv1a64(std::string_view(w.name));
    std::snprintf(idbuf, sizeof idbuf, "0x%016llX", static_cast<unsigned long long>(id));
    s += "ID "; s += idbuf; s += "\n";
    s += "NAME "; s += (w.name.empty() ? std::string("untitled") : w.name); s += "\n";
    s += "BUILD "; s += std::to_string(w.build); s += "\n";
    s += "ALGO "; s += std::to_string(w.algo); s += "\n";

    if (w.hasSpawn) {
        s += "SPAWN " + num(w.spawnX) + " " + num(w.spawnY) + " " + num(w.spawnZ) + " " + num(w.spawnYaw) + "\n";
    }
    if (w.hasSun) {
        // The VECTOR is written, because it round-trips exactly where degrees do not. The elevation
        // rides along as a trailing comment so the line is still readable by a person -- the parser
        // truncates at '#', so it cannot be read back and drift.
        const f64 len = std::sqrt(w.sunDir[0]*w.sunDir[0] + w.sunDir[1]*w.sunDir[1] + w.sunDir[2]*w.sunDir[2]);
        const f64 kRad = 180.0 / 3.14159265358979;
        const f64 elev = len > 1e-9 ? std::asin(w.sunDir[2] / len) * kRad : 0.0;
        const f64 azim = std::atan2(w.sunDir[1], w.sunDir[0]) * kRad;
        s += "SUN dir " + num(w.sunDir[0]) + " " + num(w.sunDir[1]) + " " + num(w.sunDir[2]) +
             " color " + num(w.sunColor[0]) + " " + num(w.sunColor[1]) + " " + num(w.sunColor[2]) +
             " lux " + num(w.sunLux) +
             "   # elev " + num(elev) + " azim " + num(azim) + "\n";
    }
    if (w.hasSky) {
        s += "SKY model ";
        s += w.skyPhysical ? "physical" : "authored";
        if (w.skyMieScatter   >= 0.0) s += " mie " + num(w.skyMieScatter);
        if (w.skyMultiScatter >= 0.0) s += " multiscatter " + num(w.skyMultiScatter);
        if (w.skyViewSteps    >  0)   s += " steps " + std::to_string(w.skyViewSteps);
        if (w.skyAerialSteps  >  0)   s += " aerial " + std::to_string(w.skyAerialSteps);
        s += "\n";
    }
    if (w.hasFog) {
        s += "FOG exp density " + num(w.fogDensity) +
             " color " + num(w.fogColor[0]) + " " + num(w.fogColor[1]) + " " + num(w.fogColor[2]) + "\n";
    }

    if (!w.pcgVolumes.empty()) {
        s += "\n";
        for (const OcPcgVolume& v : w.pcgVolumes) {
            s += "PCGVOLUME name " + (v.name.empty() ? std::string("unnamed") : v.name) +
                 " seed " + std::to_string(v.seed) +
                 " cell " + num(v.cellSizeCm) +
                 " octaves " + std::to_string(v.octaves) +
                 " floor " + num(v.coverageFloor) +
                 " bias " + num(v.coverageBias);
            // The bounds token LAST, because parsing `bounds` consumes the six numbers after it and
            // anything following them would have to be re-found. Writing it last means the reader
            // never has to.
            if (v.infinite) {
                s += " infinite";
            } else {
                s += " bounds " + num(v.boundsMin[0]) + " " + num(v.boundsMin[1]) + " " + num(v.boundsMin[2]) +
                     " " + num(v.boundsMax[0]) + " " + num(v.boundsMax[1]) + " " + num(v.boundsMax[2]);
            }
            s += "\n";
        }
    }

    s += "\n";
    for (const OcWorldPlacement& p : w.placements) {
        if (p.uniform()) {
            s += "PLACE  " + p.asset + " " +
                 num(p.x) + " " + num(p.y) + " " + num(p.z) + " " +
                 num(p.yaw) + " " + num(p.pitch) + " " + num(p.roll) + " " + num(p.sx);
        } else {
            s += "PLACEG " + p.asset + " " +
                 num(p.x) + " " + num(p.y) + " " + num(p.z) + " " +
                 num(p.yaw) + " " + num(p.pitch) + " " + num(p.roll) + " " +
                 num(p.sx) + " " + num(p.sy) + " " + num(p.sz);
        }
        if (!p.material.empty()) { s += " "; s += p.material; }
        if (!p.collide) s += " nocollide";
        s += "\n";
    }
    return s;
}

// Writes a world to disk, creating parent directories.
bool saveOcworld(const std::string& path, const OcWorldData& w, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    const std::string text = writeOcworld(w);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}

} // namespace aver::fmt
