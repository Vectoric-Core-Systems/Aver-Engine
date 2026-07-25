#include "aver/formats/OcWorld.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/core/Hash.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

f64 tokF(const std::vector<std::string_view>& t, usize i, f64 dflt = 0.0) {
    return i < t.size() ? parseF64(t[i]) : dflt;
}

// Formatting that round-trips: enough digits to reproduce the double, no trailing noise.
std::string num(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

} // namespace

bool parseOcworld(std::string_view text, OcWorldData& out, std::string* err) {
    out = OcWorldData{};
    bool sawHeader = false;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        // Same comment/terminator rule as .ocmap, because §11 says every .ocmap record is a legal
        // .ocworld record -- which has to include how a line is lexed, not just which keys exist.
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
            // SUN dir X Y Z color R G B lux L  -- read positionally after each keyword.
            out.hasSun = true;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "dir") && i + 3 < t.size()) {
                    out.sunDir[0] = parseF64(t[i+1]); out.sunDir[1] = parseF64(t[i+2]); out.sunDir[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "color") && i + 3 < t.size()) {
                    out.sunColor[0] = parseF64(t[i+1]); out.sunColor[1] = parseF64(t[i+2]); out.sunColor[2] = parseF64(t[i+3]);
                } else if (equalsCI(t[i], "lux") && i + 1 < t.size()) {
                    out.sunLux = parseF64(t[i+1]);
                }
            }
        } else if (equalsCI(key, "FOG")) {
            // FOG exp density D color R G B
            out.hasFog = true;
            for (usize i = 1; i < t.size(); ++i) {
                if (equalsCI(t[i], "density") && i + 1 < t.size()) out.fogDensity = parseF64(t[i+1]);
                else if (equalsCI(t[i], "color") && i + 3 < t.size()) {
                    out.fogColor[0] = parseF64(t[i+1]); out.fogColor[1] = parseF64(t[i+2]); out.fogColor[2] = parseF64(t[i+3]);
                }
            }
        } else if (equalsCI(key, "PLACE") || equalsCI(key, "PLACEG")) {
            // PLACE  <asset> x y z yaw pitch roll [scale] [material] [nocollide]
            // PLACEG <asset> x y z yaw pitch roll sx sy sz [material] [nocollide]
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
            // Trailing optional tokens, order-independent so a hand-edited file is forgiving.
            for (usize i = next; i < t.size(); ++i) {
                if (equalsCI(t[i], "nocollide")) p.collide = false;
                else if (p.material.empty()) p.material = std::string(t[i]);
            }
            p.objectId = fnv1a64(std::string_view(p.asset));
            out.placements.push_back(std::move(p));
        }
        // Anything else -- LAYER, NODE, CELL, STREAM, GEOREF, TERRAIN, SURFACE, GROUND, KILLZ,
        // DEFORM, CLIENT, ROOT -- is skipped rather than rejected, so a file written by a fuller
        // implementation still loads here with the parts this engine understands.
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocworld/.ocmap file (no OCWORLD or OCMAP header line)";
        return false;
    }
    if (out.contentId == 0 && !out.name.empty()) out.contentId = fnv1a64(std::string_view(out.name));
    return true;
}

bool loadOcworld(const std::string& path, OcWorldData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcworld(text, out, err);
}

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
        s += "SUN dir " + num(w.sunDir[0]) + " " + num(w.sunDir[1]) + " " + num(w.sunDir[2]) +
             " color " + num(w.sunColor[0]) + " " + num(w.sunColor[1]) + " " + num(w.sunColor[2]) +
             " lux " + num(w.sunLux) + "\n";
    }
    if (w.hasFog) {
        s += "FOG exp density " + num(w.fogDensity) +
             " color " + num(w.fogColor[0]) + " " + num(w.fogColor[1]) + " " + num(w.fogColor[2]) + "\n";
    }

    s += "\n";
    for (const OcWorldPlacement& p : w.placements) {
        // PLACE when the scale is uniform, PLACEG when it is not: writing the simpler record where it
        // suffices keeps a level readable, and keeps a uniform-only world loadable by an .ocmap reader.
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
