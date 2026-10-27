// Cook: an authored f64 .ocworld level into packed region files and an index that ChunkStreamer streams.
// Reads only f64 OcWorldData, never a scene (f32 CLocal loses precision past 10 km). Pure: shared by AverAssetC and the editor.
// Output: <project>/Binaries/Chunks/<levelStem>/ with r.<x>.<y>.<z>.avrgn, world.ocindex, level.ocworld.
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

// Bump when the output for the same source changes; part of the source hash.
inline constexpr u32 kCookVersion = 1;

struct CookInput {
    // Content-relative path; names the level in the LEVELINST cycle check.
    std::string levelAsset;
    // Names the cook directory and the set's levelId (fnv1a64).
    std::string levelStem;
    std::string contentDir;
    std::string projectDir;
    // Null: built over contentDir. The editor passes its own so unsaved child levels are the ones cooked.
    LevelAssetLibrary* library = nullptr;
    // Local bounds in cm, false = unknown (an unknown mesh makes the placement level-global). Built-ins are the unit box [-1, 1]^3.
    std::function<bool(const std::string& asset, Vec3& lo, Vec3& hi)> meshBounds;
};

struct CookOptions {
    i32 chunkSizeCm = kDefaultChunkSizeCm;
    // Root is WIDE when its subtree AABB reaches more than wideCm / 2 from its position on X or Y.
    // Clamped to the streamer's pad (kAuthoredPadChunks, AuthoredChunkSource.hpp).
    f64 wideCm = 6400.0;
    std::string outDir;
};

struct CookReport {
    u32 placementsIn = 0;
    u32 flatPlacements = 0;
    u32 instances = 0;
    u32 chunkOwned = 0;               // flat placements in payloads (children included)
    u32 chunkOwnedRoots = 0;
    u32 levelGlobal = 0;
    // Level-global ROOTS by the first reason that applied.
    u32 globalClass = 0, globalVehicle = 0, globalSequence = 0, globalUnpackable = 0, globalUnbounded = 0;
    u32 wide = 0;
    u32 beyondSidecarLimit = 0;       // beyond +-1,048,576 cm: fine for chunks, a warning for the f32 sidecars
    std::vector<std::string> unknownBoundsAssets;   // distinct assets with no bounds; at most 32 named
    u32 chunks = 0;
    u32 regions = 0;
    u32 largestChunkEntities = 0;
    ChunkCoord largestChunk{};
    u64 bytes = 0;
    u64 sourceHash = 0;
    f64 elapsedMs = 0;
    std::string dir;                  // where the set was written (empty for a plan)
    std::vector<WideItem> wideItems;
    std::vector<std::string> warnings;
};

// <projectDir>/Binaries/Chunks/<levelStem>
std::string cookDirFor(const std::string& projectDir, const std::string& levelStem);

// The plan: everything decided, nothing written, payloads built on demand.
// Borrows the level given to planAuthoredCook (placements point into it): it must outlive the plan.
class CookPlan {
public:
    struct Region {
        RegionCoord coord;
        usize first = 0, end = 0;
    };
    // Occupied chunks sorted by (region, chunk): a region's chunks are one run.
    std::vector<ChunkCoord> chunks;
    std::vector<Region> regions;
    // Sorted by (owner, box).
    std::vector<WideItem> wide;
    // What stays in level.ocworld: level-global placements (sequence targets renumbered), other records, no LEVELINST records.
    fmt::OcWorldData slim;
    u64 sourceHash = 0;
    i32 chunkSizeCm = kDefaultChunkSizeCm;

    // Built from the flat level; deterministic.
    ChunkPayload payload(usize i) const;

private:
    struct State;
    std::shared_ptr<State> state_;
    friend bool planAuthoredCook(const fmt::OcWorldData&, const CookInput&, const CookOptions&, CookPlan&, CookReport&, std::string*);
};

// Decides the cook without writing it. False with `why` on a cycle, a bad chunk size, or an unflattenable level.
bool planAuthoredCook(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt, CookPlan& plan,
                      CookReport& report, std::string* why = nullptr);

// Writes through a temporary directory and a rename, so an interrupted cook keeps the previous set.
// False with `why` on failure; `report` is filled as far as it got.
bool cookAuthoredLevel(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt, CookReport& report,
                       std::string* why = nullptr);

// Source hash stamped in the index: cook and codec versions, options, placements, rest of level, LEVELINST records and
// the content of instanced levels. A set with a different hash is stale. Needs a library for levels that instance others.
u64 cookSourceHash(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt);

} // namespace aver::world
