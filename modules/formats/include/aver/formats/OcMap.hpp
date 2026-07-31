#pragma once
// .ocmap — the world container: identity + environment + a list of placements (asset reference plus
// transform). See docs/recon/ocmap-scene.md. Engine space: cm, X fwd / Y right / Z up.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One SURFACE row: the friction and bounce of a named surface type.
struct OcSurface {
    i32 id = 0;
    std::string name;
    f64 grip = 1.0, roll = 0.0, restitution = 0.0;
};

// One PLACE or DEFORM row: an asset and where it sits.
struct OcPlacement {
    std::string asset;              // asset reference (name or path)
    ObjectId objectId = kInvalidObjectId; // resolved reference id
    f64 x = 0, y = 0, z = 0;        // cm (doubles for precision)
    f64 yaw = 0, pitch = 0, roll = 0; // degrees
    f64 scale = 1.0;                // uniform (PLACE only)
    i32 surface = -1;               // -1 = use the asset's own surface
    bool deform = false;            // true => server-simulated deformable cage
    std::string material;           // deform material (else empty)
};

// A whole map: identity, ground and spawn, surfaces, and every placement.
struct OcMapData {
    int version = 1;                // OCMAP <n>
    u64 contentId = 0;              // ID = FNV-1a-64(NAME)
    std::string name;
    u32 build = 0;
    u32 algo = 3;                   // hash-algo id (raw)
    std::array<u8, 32> root{};      // ROOT hash (up to 32 bytes)
    std::string clientUmap;         // "" = build-from-assets ("scene")

    bool hasGround = false;
    f64 groundZ = 0;
    i32 groundSurface = 0;
    f64 killZ = -5000.0;
    bool hasSpawn = false;
    f64 spawnX = 0, spawnY = 0, spawnZ = 0, spawnYaw = 0;

    std::vector<OcSurface> surfaces;
    std::vector<OcPlacement> placements; // PLACE + DEFORM

    u32 deformCount() const { u32 n = 0; for (const auto& p : placements) if (p.deform) ++n; return n; }
    u32 placeCount() const { return static_cast<u32>(placements.size()) - deformCount(); }
};

// Parses a map from memory. Returns true if the text parsed; invariant failures (missing NAME/ID/ROOT
// or no collision source) are reported through `err` without failing the parse.
bool parseOcmap(std::string_view text, OcMapData& out, std::string* err = nullptr);

// Loads a map from disk.
bool loadOcmap(const std::string& path, OcMapData& out, std::string* err = nullptr);

// True when the map satisfies the load invariants: NAME, ID != 0, ROOT != 0, and GROUND or at least
// one placement.
bool ocmapIsServerValid(const OcMapData& m, std::string* why = nullptr);

} // namespace aver::fmt
