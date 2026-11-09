#pragma once
// `.ocveg` -- the record a Generate Vegetation run leaves beside its `.ocinst`, so the run can be repeated: which
// biomes, seed, area and cell size, which table it wrote, a hash of everything that went in, and the MANUAL OVERRIDES
// made to the generated result since (brush additions and erasures). A regenerate re-runs the biomes and re-applies the
// overrides, so hand edits survive; the baseline-plus-overrides split is the one LayeredChunkSource uses for chunks.
//
//     OCVEG 1
//     BIOME Biomes/Forest.ocbiome
//     SEED 7
//     AREA -12800 -12800 12800 12800 -100000 100000
//     CELL 6400
//     OUTPUT Foliage/Terrain.ocinst
//     HASH 1f2e3d4c5b6a7988
//     COUNT 18342
//     ADD Meshes/Rock.ocmesh  px py pz  qx qy qz qw  sx sy sz
//     ERASE x y z radiusCm
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct OcVegAdded {
    std::string asset;
    f32 pos[3] = {0, 0, 0};
    f32 rot[4] = {0, 0, 0, 1};
    f32 scale[3] = {1, 1, 1};
};
struct OcVegErased {
    f32 center[3] = {0, 0, 0};
    f32 radiusCm = 0.0f;       // generated rows within this XY distance of the centre are dropped
};

struct OcVegetationRecord {
    std::vector<std::string> biomes;     // content-relative .ocbiome paths
    i32 seed = 0;
    f32 areaMin[2] = {0, 0}, areaMax[2] = {0, 0};
    f32 zMin = -100000.0f, zMax = 100000.0f;
    i32 cellCm = 6400;
    std::string output;                  // content-relative .ocinst the run wrote
    u64 inputHash = 0;                   // what went in (graph, seed, area, cell); a different hash = the table is stale
    u32 count = 0;                       // rows the run wrote
    std::vector<OcVegAdded> added;
    std::vector<OcVegErased> erased;

    bool valid(std::string* why = nullptr) const;
};

bool parseOcVegetation(const std::string& text, OcVegetationRecord& out, std::string* why = nullptr);
bool writeOcVegetation(const OcVegetationRecord& in, std::string& out, std::string* why = nullptr);
bool loadOcVegetation(const std::string& path, OcVegetationRecord& out, std::string* why = nullptr);
bool saveOcVegetation(const std::string& path, const OcVegetationRecord& in, std::string* why = nullptr);

} // namespace aver::fmt
