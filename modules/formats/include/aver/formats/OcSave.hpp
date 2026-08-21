#pragma once
// .ocsave -- a saved game: the whole world, captured generically, as an AVR1 container.
//
// WHY A FULL SNAPSHOT RATHER THAN A DELTA against the level it came from. A delta needs a stable
// per-entity identity, and THIS ENGINE HAS NONE. `World::create(name)` sets
// `objectId = fnv1a64(name)` (World.cpp:386-396), so ten `PLACE Content/Meshes/crate.ocmesh` lines
// produce ten entities sharing one id (LevelInstance.cpp:62), and a graph-spawned actor passes a
// null name so every one of those shares the FNV offset basis. `OcWorldPlacement::objectId` is not
// an instance identity either -- it is recomputed at parse and consumed as `CMeshRenderer.mesh`
// (OcWorld.cpp:212, LevelInstance.cpp:68).
//
// A snapshot needs no identity at all: capture in a deterministic order, and the ARRAY INDEX is the
// identity. Parents and every cross-entity reference are stored as indices into this file's own
// entity array, and restore rebuilds in the same order so index -> handle is one array. That is
// exactly what `world::ChunkPayload` already does and proves (ChunkPayload.cpp:11-12: "a parent is
// always at a lower index than any of its descendants. restore() relies on exactly that and does no
// fix-up pass"); this file is that idea made generic over every component.
//
// GENERIC OVER COMPONENTS, BY NAME. Fields are written by NAME through the scene's reflection API,
// never by offset, so a component that gains a field still loads an older save, a component that
// loses one drops it with a note rather than corrupting the struct after it, and a component
// registered at RUNTIME (Synapse's own, for instance) saves without this file knowing it exists.
//
// The cost is size: a save carries the level's content as well as what the player changed. That is
// the trade the owner chose, and it buys a save that survives editing the level it was made in.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// One field's value. `kind` is a scene FieldKind (AVER_SCENE_KIND_* in scene_abi.h) and decides
// which member below carries it. This module deliberately does NOT include scene_abi.h -- Aver
// Formats sits below Aver.Scene and may not gain an edge to it, so the kind crosses as a plain u32
// exactly as an asset ObjectId crosses as a plain u64.
struct OcSaveField {
    std::string name;        // the BARE field name -- "position", not "CLocal.position"
    u32 kind = 0;
    // F32 uses one value, VEC3 three, QUAT four, MAT4 sixteen. Sized to the kind on write, and
    // checked against it on read -- a field whose count disagrees with its kind is refused rather
    // than read past.
    std::vector<f32> f;
    // I32, BOOL and I64 carry their value here. So does ENTITY -- but as an INDEX INTO
    // OcSaveData::entities, not a handle: a handle carries a generation and is meaningless in a
    // file. -1 is "referred to nothing, or to something outside the snapshot".
    i64 i = 0;
    std::string s;           // STRING only
};

// One component instance on one entity.
struct OcSaveComponent {
    std::string type;        // the registered name, e.g. "CLocal"
    std::vector<OcSaveField> fields;
};

// One entity.
struct OcSaveEntity {
    std::string name;
    u64 objectId = 0;
    // Index into OcSaveData::entities, ALWAYS LOWER than this entity's own index, or -1 for a root.
    // The capture order is what guarantees that; see the header comment.
    i32 parent = -1;
    // The framework class this entity is an instance of, or empty for a plain entity. BY NAME,
    // because a class handle is process-local -- OcWorldPlacement::className's own comment states
    // the same rule for the same reason: "only a NAME survives a save/load round trip".
    std::string className;
    std::vector<OcSaveComponent> components;
};

// A whole saved world.
struct OcSaveData {
    // Bumped only for a change a reader cannot handle additively. Chunks are the additive mechanism.
    u32 contentVersion = 1;
    // The level this save was taken in, so a loader can tell the player it is loading the wrong
    // one. NOT used to reconstruct anything -- a snapshot is self-contained by construction.
    std::string levelPath;
    // The engine version that wrote it, for a message rather than for a decision.
    std::string engineVersion;
    std::vector<OcSaveEntity> entities;

    // True when every parent index refers to a LOWER index (or -1) and every ENTITY field refers to
    // a valid index or -1. This is the invariant restore depends on to need no fix-up pass, so it is
    // checked on write AND on read rather than assumed.
    bool valid() const;
};

bool loadOcSave(const std::string& path, OcSaveData& out, std::string* why = nullptr);
// Writes ATOMICALLY: to `<path>.tmp`, then renames over `path`. A half-written save is the worst
// failure this format can have -- it destroys the thing the player asked to keep -- and
// platform::renameFile is MoveFileExW with MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH,
// commented at its own definition as "exactly the operation write-to-temp-then-swap needs".
// RegionFile::compact is the only other caller in the tree; this is the second.
bool saveOcSave(const std::string& path, const OcSaveData& in, std::string* why = nullptr);
bool parseOcSave(const u8* bytes, usize size, OcSaveData& out, std::string* why = nullptr);
bool writeOcSave(const OcSaveData& in, std::vector<u8>& out, std::string* why = nullptr);

// How many f32 values a field kind carries, or 0 when the kind is not float-shaped.
u32 ocSaveFloatCount(u32 kind);

} // namespace aver::fmt
