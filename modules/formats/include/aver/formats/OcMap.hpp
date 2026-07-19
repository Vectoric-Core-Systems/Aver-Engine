#pragma once
// .ocmap — the world container: identity + environment + a list of placements
// (asset reference + transform). Faithful to the authoritative reader OcMap.cs, with
// two deliberate Aver improvements per project direction:
//   * positions/rotations stored as f64 (doubles) for large-world precision;
//   * every placement resolves an ObjectId (explicit, else fnv1a64 of the asset name).
// See docs/recon/ocmap-scene.md. Engine space: cm, X fwd / Y right / Z up.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

struct OcSurface {
    i32 id = 0;
    std::string name;
    f64 grip = 1.0, roll = 0.0, restitution = 0.0;
};

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

// Parse from memory. Returns true if the text parsed. Identity-invariant problems
// (missing NAME/ID/ROOT, or no collision source) that OcMap.cs treats as fatal are
// reported through `err` but do not fail the parse — callers gate on them as needed.
bool parseOcmap(std::string_view text, OcMapData& out, std::string* err = nullptr);

// Load from disk (positions as doubles).
bool loadOcmap(const std::string& path, OcMapData& out, std::string* err = nullptr);

// True if the map satisfies OcMap.cs's load invariants (NAME, ID!=0, ROOT!=0,
// and GROUND or >=1 placement) — i.e. the existing server would accept it.
bool ocmapIsServerValid(const OcMapData& m, std::string* why = nullptr);

} // namespace aver::fmt
