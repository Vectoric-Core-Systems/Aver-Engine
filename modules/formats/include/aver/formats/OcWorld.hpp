#pragma once
// .ocworld — the native world format, and a strict superset of .ocmap (FORMAT_SPECS.md §11).
// Identity, SUN/FOG environment, and PLACE/PLACEG placements. Unknown records parse and are skipped.
// Engine space: centimetres, +X forward, +Y right, +Z up, left-handed. Positions are f64.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One placed thing. Covers both record forms: PLACE (uniform scale) and PLACEG (non-uniform).
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

// A whole world: identity, optional environment, and the placements.
struct OcWorldData {
    int version = 1;
    u64 contentId = 0;                     // ID = FNV-1a-64(NAME)
    std::string name;
    u32 build = 0;
    u32 algo = 3;

    bool hasSun = false;
    // Points TOWARD the light, matching rhi::SkyAtmosphere::sunDirection.
    f64 sunDir[3] = {-0.5481, 0.3838, 0.7431};
    f64 sunColor[3] = {1.0, 0.98, 0.92};
    f64 sunLux = 100000.0;

    bool hasFog = false;
    f64 fogDensity = 4e-6;                 // per cm
    f64 fogColor[3] = {1.0, 1.0, 1.0};     // a tint on the in-scattered sky; white is clear air

    // SKY: which sky model, and the few air parameters worth authoring per level. Kept as plain
    // numbers rather than an rhi::AtmosphereProfile because this module depends on Core alone.
    // A negative or zero override means "leave the engine's default alone".
    bool hasSky = false;
    bool skyPhysical = true;               // false selects the authored two-colour dome
    f64 skyMieScatter = -1.0;              // per km; raise for haze, dust or a coastal day
    f64 skyMultiScatter = -1.0;            // isotropic multiple-scattering gain
    i32 skyViewSteps = 0;                  // samples along a sky ray
    i32 skyAerialSteps = 0;                // samples along the air between camera and surface

    bool hasSpawn = false;
    f64 spawnX = 0, spawnY = 0, spawnZ = 0, spawnYaw = 0;

    std::vector<OcWorldPlacement> placements;
};

// Parses a world from memory. Unknown records are skipped, not failed.
bool parseOcworld(std::string_view text, OcWorldData& out, std::string* err = nullptr);

// Loads a world from disk.
bool loadOcworld(const std::string& path, OcWorldData& out, std::string* err = nullptr);

// Serialises a world to the text form. Round-trips through parseOcworld.
std::string writeOcworld(const OcWorldData& w);

// Writes a world to disk, creating parent directories.
bool saveOcworld(const std::string& path, const OcWorldData& w, std::string* err = nullptr);

} // namespace aver::fmt
