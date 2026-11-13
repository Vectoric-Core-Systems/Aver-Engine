#include "aver/formats/OcStream.hpp"

#include "aver/core/Hash.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aver::fmt {

namespace {
// Paths sit between other tokens on a line: a space (a level called "My Level") would split them. Written with ' ', '%',
// tab and '#' escaped as %XX and read back by undoing exactly those four, so files written before (no escapes) read the same.
std::string escapePath(const std::string& p) {
    std::string o;
    for (const char c : p) {
        if (c == ' ') o += "%20";
        else if (c == '%') o += "%25";
        else if (c == '\t') o += "%09";
        else if (c == '#') o += "%23";
        else o += c;
    }
    return o;
}
std::string unescapePath(const std::string& p) {
    std::string o;
    for (usize i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size()) {
            const char a = p[i + 1], b = p[i + 2];
            const char c = a == '2' && b == '0' ? ' ' : a == '2' && b == '5' ? '%' : a == '0' && b == '9' ? '\t' : a == '2' && b == '3' ? '#' : 0;
            if (c) { o += c; i += 2; continue; }
        }
        o += p[i];
    }
    return o;
}
}  // namespace

bool loadOcStream(const std::string& path, OcStreamData& out, std::string* why) {
    out = OcStreamData{};
    std::ifstream f(aver::nativePath(path));
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    std::string line;
    if (!std::getline(f, line) || line.rfind("OCSTREAM", 0) != 0) {
        if (why) *why = path + " is not an .ocstream file";
        return false;
    }
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        if (key == "SOURCE") {
            std::string hex;
            ls >> hex;
            out.sourceHash = std::strtoull(hex.c_str(), nullptr, 16);
        } else if (key == "FOLIAGE") {
            OcStreamFoliage fo;
            if (ls >> fo.source >> fo.cells) {
                fo.source = unescapePath(fo.source);
                fo.cells = unescapePath(fo.cells);
                out.foliage.push_back(std::move(fo));
            }
        } else if (key == "HLOD") {
            std::string cell, hex;
            OcStreamHlod h;
            u32 level = 0;
            if (!(ls >> cell >> level >> h.proxy >> hex) || level == 0) continue;
            if (std::sscanf(cell.c_str(), "%d,%d", &h.cx, &h.cy) != 2) continue;
            h.proxy = unescapePath(h.proxy);
            h.level = level;
            h.sourceHash = std::strtoull(hex.c_str(), nullptr, 16);
            out.hlod.push_back(std::move(h));
        } else if (key == "B") {
            std::string hex;
            OcStreamBounds b;
            if (!(ls >> hex >> b.min[0] >> b.min[1] >> b.min[2] >> b.max[0] >> b.max[1] >> b.max[2])) continue;
            b.id = std::strtoull(hex.c_str(), nullptr, 16);
            if (b.id) out.bounds.push_back(b);
        }
    }
    return true;
}

bool saveOcStream(const std::string& path, const OcStreamData& in, std::string* why) {
    std::string s = "OCSTREAM 1\n# Generated streaming data (docs/LEVEL_STREAMING.md). Regenerate; do not edit.\n";
    char buf[192];
    std::snprintf(buf, sizeof buf, "SOURCE %016" PRIx64 "\n", static_cast<uint64_t>(in.sourceHash));
    s += buf;
    for (const OcStreamHlod& h : in.hlod) {
        std::snprintf(buf, sizeof buf, "HLOD %d,%d %u ", h.cx, h.cy, h.level);
        s += buf + escapePath(h.proxy);
        std::snprintf(buf, sizeof buf, " %016" PRIx64 "\n", static_cast<uint64_t>(h.sourceHash));
        s += buf;
    }
    for (const OcStreamFoliage& fo : in.foliage) s += "FOLIAGE " + escapePath(fo.source) + " " + escapePath(fo.cells) + "\n";
    for (const OcStreamBounds& b : in.bounds) {
        std::snprintf(buf, sizeof buf, "B %016" PRIx64 " %.9g %.9g %.9g %.9g %.9g %.9g\n", static_cast<uint64_t>(b.id),
                      b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
        s += buf;
    }
    createParentDirectories(path);
    if (!writeFileBytesAtomic(path, s.data(), s.size())) {
        if (why) *why = "cannot write " + path;
        return false;
    }
    return true;
}

u64 hashPlacements(const OcWorldData& w) {
    std::string s;
    s.reserve(w.placements.size() * 96);
    char buf[160];
    for (const OcWorldPlacement& p : w.placements) {
        std::snprintf(buf, sizeof buf, "%016" PRIx64 "|%d|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.4f,%.4f,%.4f|",
                      static_cast<uint64_t>(p.placementId), p.parent, p.x, p.y, p.z, p.yaw, p.pitch, p.roll,
                      p.sx, p.sy, p.sz);
        s += p.asset;
        s += buf;
    }
    return fnv1a64(std::string_view(s));
}

} // namespace aver::fmt
