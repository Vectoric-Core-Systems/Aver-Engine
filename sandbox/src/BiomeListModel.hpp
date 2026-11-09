#pragma once
// The Landscape mode's Biomes section, the parts that need no window (WAVE2_UE W4 step 6): the project's `.ocbiome` and `.ocfoliage` files, a starter
// biome for a foliage type, a free file name, and the readout lines for an estimate and a run. Header-only; Formats + Platform only.
#include "aver/core/Types.hpp"
#include "aver/formats/OcBiome.hpp"
#include "aver/formats/OcFoliage.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace aver::editor::biomes {

// Content-relative ('/'), sorted, capped. `ext` includes the dot.
inline std::vector<std::string> listFiles(const std::string& contentDir, const std::string& ext, usize cap = 300) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(contentDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (out.size() >= cap) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ext) continue;
        std::error_code rc;
        out.push_back(fs::relative(it->path(), contentDir, rc).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A one-entry biome for a foliage type: the density and no mask, slope or layer gate, so it grows everywhere until the author narrows it.
inline fmt::OcBiomeData starterBiome(const std::string& name, const std::string& foliageRel, f32 densityPer100m2 = 10.0f) {
    fmt::OcBiomeData b;
    b.name = name;
    fmt::OcBiomeEntry e;
    e.foliage = foliageRel;
    e.densityPer100m2 = densityPer100m2;
    b.entries.push_back(e);
    return b;
}

// <dir>/Biomes/<stem>.ocbiome, or <stem>2, <stem>3... when taken; content-relative.
inline std::string freeBiomePath(const std::string& contentDir, const std::string& stem) {
    namespace fs = std::filesystem;
    for (int i = 1;; ++i) {
        const std::string rel = "Biomes/" + stem + (i == 1 ? std::string() : std::to_string(i)) + ".ocbiome";
        std::error_code ec;
        if (!fs::exists(fs::path(contentDir) / rel, ec)) return rel;
    }
}

// "Fern" for "Foliage/Fern.ocfoliage".
inline std::string stemOf(const std::string& rel) { return std::filesystem::path(rel).stem().string(); }

// "up to 1200 instances and 115 KB per cell" plus what the guardrail says.
inline std::string estimateLine(f64 instancesPerCellMax, f64 bytesPerCellMax, bool overCap) {
    char b[160];
    const f64 kb = bytesPerCellMax / 1024.0;
    if (kb >= 1024.0) std::snprintf(b, sizeof b, "Up to %.0f instances and %.1f MB per cell%s", instancesPerCellMax, kb / 1024.0, overCap ? " (over the cap)" : "");
    else std::snprintf(b, sizeof b, "Up to %.0f instances and %.0f KB per cell%s", instancesPerCellMax, kb, overCap ? " (over the cap)" : "");
    return b;
}

// "Generated 4210 instances (kept 3 manual edits), 2 cells capped".
inline std::string outcomeLine(u32 instances, u32 kept, u32 cellsCapped) {
    std::string t = "Generated " + std::to_string(instances) + (instances == 1 ? " instance" : " instances");
    if (kept) t += " (kept " + std::to_string(kept) + " manual edit" + (kept == 1 ? ")" : "s)");
    if (cellsCapped) t += ", " + std::to_string(cellsCapped) + (cellsCapped == 1 ? " cell" : " cells") + " hit the instance cap";
    return t;
}

}  // namespace aver::editor::biomes
