// A level with its LEVELINST records expanded into one flat placement list, composed in f64 (LARGE_WORLD 2.4.5 step 1).
// Child contents come from LevelAssetLibrary::resolve (overrides and removals applied). LevelPack composes f32 matrices and
// LevelInstances composes through the entity hierarchy, so neither gives an f64 world position.
// Pure: Core + Formats + the library. No scene, no GPU, so the cook and AverAssetC can call it.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/world/LevelAssetLibrary.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::world {

// A pose with an f64 position and the f32 rotation and scale every consumer uses.
struct Pose64 {
    f64  pos[3] = {0, 0, 0};
    Quat rot = Quat::identity();
    Vec3 scale{1, 1, 1};
};

// Narrowing as instantiate() does: f32 rotation and scale, f64 position.
Pose64 poseOfPlacement(const fmt::OcWorldPlacement& p);
Pose64 poseOfInstance(const fmt::OcLevelInstance& r);
Pose64 composePose(const Pose64& parent, const Pose64& child);
void rotateD(const Quat& q, const f64 v[3], f64 out[3]);

struct FlatPlacement {
    // Points into the level handed to flattenLevelInstances or into FlatLevel::held; neither may move.
    const fmt::OcWorldPlacement* src = nullptr;
    // Always lower than this placement's own index.
    i32 parent = -1;
    // World cm for a root; authored parent-relative for a child.
    f64 pos[3] = {0, 0, 0};
    f32 roll = 0, pitch = 0, yaw = 0;
    f32 scale[3] = {1, 1, 1};
    // Authored placementId; for instance children a hash of the instance path and the child id; derived when none.
    u64 id = 0;
    i32 topIndex = -1;
    u32 depth = 0;
    // True when the pose was composed through an instance (the record's own x, y, z, yaw, pitch, roll are NOT the pose).
    bool composed = false;
};

struct FlatLevel {
    std::vector<FlatPlacement> placements;
    // Level-global records of the instances, composed into the world frame.
    std::vector<fmt::OcLight>          lights;
    std::vector<fmt::OcDecal>          decals;
    std::vector<fmt::OcPrefabInstance> prefabInstances;
    // One line each: an instance that did not load, a foliage table inside an instance, a child naming a parent that does not precede it.
    std::vector<std::string> warnings;
    u32 instances = 0;
    u32 deepest = 0;
    u32 unloadedInstances = 0;
    u32 droppedFoliage = 0;
    // Keeps child level records alive: FlatPlacement::src points into them.
    std::vector<std::unique_ptr<ResolvedContents>> held;
};

// Expands LEVELINST records through lib. `level` owns its own placements; levelAsset names it for the cycle check.
// False with why on a cycle or nesting deeper than LevelAssetLibrary::kMaxDepth; a child that fails to load is a warning.
bool flattenLevelInstances(const fmt::OcWorldData& level, const std::string& levelAsset, LevelAssetLibrary& lib,
                           FlatLevel& out, std::string* why = nullptr);

} // namespace aver::world
