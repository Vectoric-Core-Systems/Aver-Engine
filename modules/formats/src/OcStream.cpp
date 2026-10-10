#include "aver/formats/OcStream.hpp"

#include "aver/core/Hash.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aver::fmt {

bool loadOcStream(const std::string& path, OcStreamData& out, std::string* why) {
    out = OcStreamData{};
    std::ifstream f(path);
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
            if (ls >> fo.source >> fo.cells) out.foliage.push_back(std::move(fo));
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
    for (const OcStreamFoliage& fo : in.foliage) s += "FOLIAGE " + fo.source + " " + fo.cells + "\n";
    for (const OcStreamBounds& b : in.bounds) {
        std::snprintf(buf, sizeof buf, "B %016" PRIx64 " %.9g %.9g %.9g %.9g %.9g %.9g\n", static_cast<uint64_t>(b.id),
                      b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
        s += buf;
    }
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
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
