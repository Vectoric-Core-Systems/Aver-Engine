// The cook: an authored level (f64 .ocworld) turned into packed region files and an index that ChunkStreamer can stream
// (LARGE_WORLD 2.4.5). It reads f64 OcWorldData and NEVER a scene: instantiating into scene::World and partitioning would go
// through f32 CLocal, which is exactly the B8 loss past 10 km that the whole design exists to avoid.
//
// PURE: Core + Formats + this module's own pure half (LevelAssetLibrary, LevelFlatten, ChunkPayload/Codec/RegionFile/Index), so
// AverAssetC and the editor call the same function and a test needs no scene.
//
// WHAT HAPPENS, in order:
//   1  LEVELINST expanded in f64 (flattenLevelInstances)
//   2  every placement's WORLD AABB in f64, from the mesh's local bounds (CookInput::meshBounds, the built-in unit meshes, or the
//      .ocmesh under contentDir); a subtree is bounded by the union of its members
//   3  each ROOT (and its whole subtree) is LEVEL-GLOBAL or CHUNK-OWNED:
//        level-global: a className or vehiclePreset anywhere in the subtree, a level-sequence track target or bind name, anything the cook cannot
//        bound (an unknown mesh), a value the payload cannot carry. These stay in the slim level.ocworld and are instantiated by
//        the flat path, exactly as before.
//        chunk-owned:  everything else. A root whose reach from its own position exceeds half of CookOptions::wideCm is also WIDE
//        (terrain tile, skyline building, bridge): still owned by its position's chunk, but listed in the index with its AABB so
//        the streamer keeps that chunk resident while the box is near a viewer.
//   4  owner chunk = the ROOT's f64 position split once (never f32, never the scene)
//   5  one payload per chunk, grouped into regions, written PACKED (byte-deterministic: two cooks memcmp equal), then the index
//      and the slim level
//   6  a report
//
// The set lives in <project>/Binaries/Chunks/<levelStem>/ : r.<x>.<y>.<z>.avrgn, world.ocindex, level.ocworld.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/world/AuthoredChunkSource.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayload.hpp"
#include "aver/world/LevelAssetLibrary.hpp"
#include "aver/world/RegionIndex.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::world {

// Bumped when the cook's output for the same source changes (what is chunk-owned, how a payload is built). Part of the source hash.
inline constexpr u32 kCookVersion = 1;

struct CookInput {
    // The level's content-relative path ("Maps/NeonDistrict.ocworld"): names it in the cycle check of its LEVELINST closure.
    std::string levelAsset;
    // Its file stem ("NeonDistrict"): names the cook directory and (fnv1a64) the set's levelId.
    std::string levelStem;
    // Where meshes and child levels resolve (the project's Content directory).
    std::string contentDir;
    // The project root; the set goes to <projectDir>/Binaries/Chunks/<levelStem> unless CookOptions::outDir says otherwise.
    std::string projectDir;
    // The parse cache that finds child levels. Null: the cook makes its own over contentDir. An editor passes its own so an
    // unsaved child is the one cooked.
    LevelAssetLibrary* library = nullptr;
    // A mesh's LOCAL bounds (cm) by content-relative asset path; false = unknown. Absent, or false: the built-in meshes (cube, sphere, drone
    // and the primitives, all taken as the unit box [-1, 1]^3), then the .ocmesh file under contentDir. A placement whose mesh stays
    // unknown is level-global and reported.
    std::function<bool(const std::string& asset, Vec3& lo, Vec3& hi)> meshBounds;
};

struct CookOptions {
    i32 chunkSizeCm = kDefaultChunkSizeCm;
    // A root is WIDE when its subtree's AABB reaches more than wideCm / 2 from the root's position on X or Y. Never larger than
    // the streamer's fixed pad (kAuthoredPadChunks chunks each way, AuthoredChunkSource.hpp), so a larger value is clamped to it.
    f64 wideCm = 6400.0;
    // Empty: <projectDir>/Binaries/Chunks/<levelStem>.
    std::string outDir;
};

struct CookReport {
    u32 placementsIn = 0;         // the level's own placements
    u32 flatPlacements = 0;       // after LEVELINST expansion
    u32 instances = 0;            // LEVELINST records expanded
    u32 chunkOwned = 0;           // flat placements in payloads (children included)
    u32 chunkOwnedRoots = 0;
    u32 levelGlobal = 0;          // flat placements left in the slim level
    // Level-global ROOTS by the first reason that applied.
    u32 globalClass = 0, globalVehicle = 0, globalSequence = 0, globalUnpackable = 0, globalUnbounded = 0;
    u32 wide = 0;                 // chunk-owned roots listed in the index
    u32 beyondSidecarLimit = 0;   // chunk-owned roots farther than +-1,048,576 cm of world (0, 0): fine for chunks, a warning for the f32 sidecars
    std::vector<std::string> unknownBoundsAssets;   // distinct assets with no bounds (at most 32 named)
    u32 chunks = 0;
    u32 regions = 0;
    u32 largestChunkEntities = 0;
    ChunkCoord largestChunk{};
    u64 bytes = 0;                // region files on disk
    u64 sourceHash = 0;
    f64 elapsedMs = 0;
    std::string dir;              // where the set was written (empty for a plan)
    std::vector<WideItem> wideItems;
    std::vector<std::string> warnings;
};

// <projectDir>/Binaries/Chunks/<levelStem>
std::string cookDirFor(const std::string& projectDir, const std::string& levelStem);

// The plan: everything decided, nothing written, payloads built on demand. BORROWS the level handed to planAuthoredCook (the
// placements it flattened point into it), so that level must outlive the plan.
class CookPlan {
public:
    struct Region {
        RegionCoord coord;
        usize first = 0, end = 0;   // chunks [first, end)
    };
    // Occupied chunks sorted by (region, chunk): a region's chunks are one run.
    std::vector<ChunkCoord> chunks;
    std::vector<Region> regions;
    // Sorted by (owner, box).
    std::vector<WideItem> wide;
    // The level without what the chunks carry: level-global placements (sequence targets renumbered), the level's other records,
    // lights/decals/prefab instances composed out of its LEVELINST records, no LEVELINST records. STREAM chunks= is retained.
    fmt::OcWorldData slim;
    u64 sourceHash = 0;
    i32 chunkSizeCm = kDefaultChunkSizeCm;

    // The payload of chunks[i], built from the flat level. Deterministic.
    ChunkPayload payload(usize i) const;

private:
    struct State;
    std::shared_ptr<State> state_;
    friend bool planAuthoredCook(const fmt::OcWorldData&, const CookInput&, const CookOptions&, CookPlan&, CookReport&, std::string*);
};

// Decides the cook without writing it. False with `why` on a cycle, a bad chunk size, or a level the cook cannot flatten.
bool planAuthoredCook(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt, CookPlan& plan,
                      CookReport& report, std::string* why = nullptr);

// Plans, then writes the set through a temporary directory and a rename (an interrupted cook leaves the previous set intact).
// False with `why` on failure; `report` is filled as far as it got.
bool cookAuthoredLevel(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt, CookReport& report,
                       std::string* why = nullptr);

// What the cook of `level` would stamp in the index as its source hash: the cook version, the codec version, the options, every
// placement, the rest of the level, the LEVELINST records and the content of the levels they instance. A cooked set whose index
// carries a different hash is stale. Needs a library for a level that instances others (CookInput::library, else one over contentDir).
u64 cookSourceHash(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt);

} // namespace aver::world
