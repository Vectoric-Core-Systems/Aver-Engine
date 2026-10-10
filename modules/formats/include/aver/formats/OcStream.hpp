// .ocstream -- a level's GENERATED streaming data (docs/LEVEL_STREAMING.md), kept under the project's
// Binaries/Streaming and named by the level's `STREAM data=`. Never authored: the editor rewrites it on
// save and World Settings > Regenerate Streaming Data rebuilds it from the mesh files.
//
//   OCSTREAM 1
//   SOURCE <16 hex>                                   hashPlacements() of the level it was made from
//   FOLIAGE <content-relative .ocinst> <project-relative cell-sorted copy>
//   B <placement id, 16 hex> minx miny minz maxx maxy maxz     world bounds, cm
#pragma once
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"

#include "aver/core/Math.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::fmt {

struct OcStreamBounds {
    u64 id = 0;
    f32 min[3] = {0, 0, 0};
    f32 max[3] = {0, 0, 0};
};

struct OcStreamFoliage {
    std::string source;   // the FOLIAGE record's path, content-relative
    std::string cells;    // its cell-sorted copy, project-relative
};

struct OcStreamData {
    u64 sourceHash = 0;
    std::vector<OcStreamFoliage> foliage;
    std::vector<OcStreamBounds> bounds;
};

bool loadOcStream(const std::string& path, OcStreamData& out, std::string* why = nullptr);
bool saveOcStream(const std::string& path, const OcStreamData& in, std::string* why = nullptr);

// What the streaming data was generated from: every placement's id, asset, parent and transform. A
// mismatch means placements moved, came or went since; bounds are still matched by id.
u64 hashPlacements(const OcWorldData& w);

// ---- generating it (OcStreamBake.cpp) ----

struct OcStreamBakeOptions {
    f32 foliageCellCm = 6400.0f;
    // Local bounds of a content-relative mesh when the host has it loaded; otherwise the file is read.
    std::function<bool(const std::string& asset, Vec3& lo, Vec3& hi)> meshBounds;
};
struct OcStreamBakeReport {
    usize placements = 0, withBounds = 0, newIds = 0;
    usize foliageTables = 0, foliageInstances = 0, foliageCells = 0;
};

// Ids for placements without one; returns how many were made.
usize assignPlacementIds(OcWorldData& w);
// A loaded .ocstream's bounds onto the placements, by id; returns how many matched.
usize applyOcStreamBounds(OcWorldData& w, const OcStreamData& d);
// Full regeneration: ids, every placement's world bounds from its mesh, a cell-sorted copy of each
// FOLIAGE table under <projectDir>/Binaries/Streaming/<levelStem>/, w.stream.lazyDirs/dataPath, and
// the .ocstream. The caller saves `w` afterwards (it now carries ids and data=).
bool bakeOcStream(OcWorldData& w, const std::string& levelStem, const std::string& contentDir,
                  const std::string& projectDir, const OcStreamBakeOptions& opt, OcStreamBakeReport& report,
                  std::string* why = nullptr);
// The .ocstream from the bounds already on the placements (an editor save), keeping `foliage`.
bool writeOcStreamFor(OcWorldData& w, const std::string& levelStem, const std::string& projectDir,
                      const std::vector<OcStreamFoliage>& foliage, std::string* why = nullptr);

} // namespace aver::fmt
