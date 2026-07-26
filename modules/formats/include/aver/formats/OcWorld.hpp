#pragma once
// .ocworld — the NATIVE world format, and a strict superset of .ocmap (FORMAT_SPECS.md §11).
//
// Why this and not .ocmap: an .ocmap PLACE carries ONE uniform scale, and a blockout level is made of
// boxes with three different half-extents each. .ocworld's PLACEG exists for exactly that (`scale3`),
// and it is the format the spec designates as native, so a level authored here is not a legacy file
// the engine happens to tolerate.
//
// Implemented here: the identity block, PLACE (read, for .ocmap compatibility), PLACEG, and the SUN /
// FOG environment records. NOT implemented: LAYER / NODE / CELL / STREAM / GEOREF / TERRAIN, which
// §11 marks engine-optional and nothing yet needs. A file using them still PARSES -- unknown records
// are skipped rather than rejected, which is what keeps a partial implementation forwards-compatible
// instead of a trap.
//
// Engine space throughout: centimetres, +X forward, +Y right, +Z up, left-handed. Positions are f64
// for large-world precision, matching .ocmap.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One placed thing. Covers both record forms: PLACE (asset name, uniform scale) and PLACEG
// (non-uniform scale), because the difference is what was WRITTEN, not what a level means.
struct OcWorldPlacement {
    std::string asset;                     // asset reference (mesh path or name)
    ObjectId objectId = kInvalidObjectId;  // fnv1a64(asset) unless given explicitly
    std::string material;                  // material name; empty = the asset's own
    f64 x = 0, y = 0, z = 0;               // cm
    f64 yaw = 0, pitch = 0, roll = 0;      // degrees
    f64 sx = 1, sy = 1, sz = 1;            // non-uniform scale (a PLACE sets all three equal)
    bool collide = true;                   // whether the world builds a static body for it

    bool uniform() const { return sx == sy && sy == sz; }
};

struct OcWorldData {
    int version = 1;
    u64 contentId = 0;                     // ID = FNV-1a-64(NAME)
    std::string name;
    u32 build = 0;
    u32 algo = 3;

    // Environment. Optional: a world that sets none inherits whatever the editor has.
    bool hasSun = false;
    f64 sunDir[3] = {-0.3, -0.4, -0.85};
    f64 sunColor[3] = {1.0, 0.98, 0.92};
    f64 sunLux = 100000.0;

    bool hasFog = false;
    // Per CENTIMETRE, so a level owns its own depth cue. 4e-6 is light haze (~10 km visibility by
    // Koschmieder); it was 2e-4, which is 196 m and therefore fog by the WMO's definition.
    f64 fogDensity = 4e-6;
    // A TINT on the in-scattered sky rather than a replacement for it, so white is clear air.
    f64 fogColor[3] = {1.0, 1.0, 1.0};

    bool hasSpawn = false;
    f64 spawnX = 0, spawnY = 0, spawnZ = 0, spawnYaw = 0;

    std::vector<OcWorldPlacement> placements;
};

// Parse from memory. Unknown records are skipped, not failed -- see the header note.
bool parseOcworld(std::string_view text, OcWorldData& out, std::string* err = nullptr);

// Load from disk.
bool loadOcworld(const std::string& path, OcWorldData& out, std::string* err = nullptr);

// Serialise to the text form. Round-trips through parseOcworld.
std::string writeOcworld(const OcWorldData& w);

// Write to disk, creating parent directories. Returns false and fills `err` on failure.
bool saveOcworld(const std::string& path, const OcWorldData& w, std::string* err = nullptr);

} // namespace aver::fmt
