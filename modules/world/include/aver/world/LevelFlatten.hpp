// A level with its LEVELINST records expanded into one flat placement list, composed in f64 (LARGE_WORLD 2.4.5 step 1).
//
// THE COMPOSITION THIS REUSES, and what it does not. LevelAssetLibrary::resolve already does the hard half for every consumer
// (the child level, its LIOVERRIDEs applied, removed placements and their subtrees dropped, nested overrides moved onto the
// nested record); LevelPack's baker composes f32 matrices in the level's own frame for instance rows, and the runtime
// (LevelInstances) composes through the entity hierarchy, so neither is an f64 world position. This is the f64 one, with the
// runtime's rule (instantiate's compose: component-wise scale, rotation product, rotated scaled offset), and it takes its
// children from resolve, so it is not a third copy of the override logic.
//
// PURE: Core + Formats + the library. No scene, no GPU, so the cook and AverAssetC can call it.
//
// WHAT COMES OUT. Every placement of the level, then every placement of every instance (depth first), parents before children,
// each with the pose instantiate() would have narrowed from: a ROOT carries its WORLD position in f64 (an instance's root is
// composed through its instance chain; the rotation of such a root is the composed quaternion as Euler degrees, which is the
// form every consumer takes), a CHILD carries its authored parent-relative pose. The rotation and scale are the same f32 casts
// instantiate() makes, so a flat placement and a chunk restored from it agree bit for bit on both.
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

// The narrowing rule of instantiate(): rotation from quatFromEulerDeg on the f32 casts, scale as f32 casts, position kept in f64.
Pose64 poseOfPlacement(const fmt::OcWorldPlacement& p);
Pose64 poseOfInstance(const fmt::OcLevelInstance& r);
// instantiate()'s composition of a child under a parent, with the position taken in f64.
Pose64 composePose(const Pose64& parent, const Pose64& child);
// A vector rotated by a quaternion, in f64.
void rotateD(const Quat& q, const f64 v[3], f64 out[3]);

struct FlatPlacement {
    // The authored record. Points into the level handed to flattenLevelInstances or into FlatLevel::held; neither may move.
    const fmt::OcWorldPlacement* src = nullptr;
    // Index into FlatLevel::placements, -1 for a root. Always lower than this placement's own index.
    i32 parent = -1;
    // A root: world centimetres. A child: the authored parent-relative position.
    f64 pos[3] = {0, 0, 0};
    // Degrees, the f32 casts instantiate() makes (or the composed rotation of an instance's root).
    f32 roll = 0, pitch = 0, yaw = 0;
    f32 scale[3] = {1, 1, 1};
    // Stable and non-zero: the authored placementId; for a placement inside an instance a hash of the instance path and the
    // child's own id; a derived one when the record has none. Unique within the flat list.
    u64 id = 0;
    // Index in the level's own placement list, or -1 for one that came out of an instance.
    i32 topIndex = -1;
    // 0 for the level's own placements, 1 inside an instance, 2 inside a nested one, ...
    u32 depth = 0;
    // True when the pose above was composed through an instance (the record's own x, y, z, yaw, pitch, roll are NOT the pose).
    bool composed = false;
};

struct FlatLevel {
    std::vector<FlatPlacement> placements;
    // Level-global records of the instances, composed into the world frame (their own level frame is gone once flattened).
    std::vector<fmt::OcLight>          lights;
    std::vector<fmt::OcDecal>          decals;
    std::vector<fmt::OcPrefabInstance> prefabInstances;
    // Named, one line each: an instance that did not load, a nested level's foliage table (it cannot be composed), a child that
    // names a parent that does not precede it.
    std::vector<std::string> warnings;
    u32 instances = 0;          // LEVELINST records expanded, nested ones included
    u32 deepest = 0;            // the deepest nesting reached (1 = a record of the level itself)
    u32 unloadedInstances = 0;
    u32 droppedFoliage = 0;     // FOLIAGE tables inside instances (not composable into a flat list)
    // Keeps every child level's resolved records alive: FlatPlacement::src points into them.
    std::vector<std::unique_ptr<ResolvedContents>> held;
};

// Expands the level's LEVELINST records through `lib` (whose loader finds the child files). `level` stays the owner of its own
// placements; `levelAsset` names it for the cycle check (an instance of the level inside itself is refused). False with `why`
// on a cycle or a nest deeper than LevelAssetLibrary::kMaxDepth; an instance that merely fails to load is a warning, as it is at runtime.
bool flattenLevelInstances(const fmt::OcWorldData& level, const std::string& levelAsset, LevelAssetLibrary& lib,
                           FlatLevel& out, std::string* why = nullptr);

} // namespace aver::world
