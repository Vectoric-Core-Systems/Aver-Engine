// A chunk's contents, described WITHOUT a single process-local value.
//
// THIS IS THE PARTICIPATION PROTOCOL, and the reason it has to exist is the most important
// structural finding in docs/CHUNKS.md: a chunk cannot be serialised by dumping component pools.
// Three things an entity owns live outside scene::World -- its gameplay class, its physics body and
// its GPU mesh -- so each subsystem has to CONTRIBUTE to a chunk and RESTORE from one, rather than
// the chunk being a memcpy of anything.
//
// FIVE VALUES THAT MAY NEVER APPEAR HERE, all id-SHAPED and all process-local. Written to disk, each
// produces a file that loads correctly exactly once, in the process that wrote it:
//
//   CMeshRenderer::material   `table.size() + 1` at the moment the name was first seen in THIS
//                             process (SceneAbi.cpp:377-390). Stored here as the NAME.
//   a String field's 8 bytes  an index into a DLL-local pool (SceneAbi.cpp:70-73).
//   CName::offset/len         offsets into a nameBlob that is never compacted (World.cpp:386-396).
//                             Stored here as the text.
//   component ids, field ids  registration-order counters (World.cpp:453, :523). Builtins.cpp:94-96
//                             spells out the hazard. Nothing here is keyed by one.
//   World::at() indices       dense over live entities and SHIFTING on every flush.
//
// That is also why the round-trip test must eventually cross a PROCESS boundary: every one of these
// round-trips perfectly within the process that minted it, so a same-process test passes on a file
// that is unloadable anywhere else.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/world/ChunkCoord.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <functional>
#  include <string>
#  include <vector>

namespace aver::world {

// One entity, portable.
struct PayloadEntity {
    // Identity. The TEXT, not the CName slice -- see the header comment.
    std::string name;
    u64 objectId = 0;

    // Index into ChunkPayload::entities, or -1 for a root. Parents always precede their children,
    // so a restore pass can resolve this as it goes without a second fix-up walk.
    i32 parent = -1;

    // POSITION MEANS TWO DIFFERENT THINGS, deliberately, and this is where the coordinate hierarchy
    // earns its place:
    //   parent == -1  -> `local.position` is CHUNK-LOCAL, in [0, chunkSizeCm) per axis. At the
    //                    default 16 m that is a four-digit number, which is exact in f32 to about a
    //                    micron and exact in the text writer's %.6g -- at ANY distance from the
    //                    world origin. An absolute coordinate is neither past ~10 km.
    //   parent >= 0   -> `local.position` is parent-relative, exactly as CLocal already holds it,
    //                    and is already small by construction.
    Transform local;
    u32 tags = 0;

    bool hasMesh = false;
    u64  mesh = 0;              // an ObjectId: FNV-1a-64 of a Content-relative path. Portable.
    std::string material;       // the NAME. Never the token.
    u32  meshFlags = 0;
    f32  aabbMin[3] = {0, 0, 0};
    f32  aabbMax[3] = {0, 0, 0};

    // Whether this entity had a static collision box, and how big. The engine has NO entity -> body
    // link of its own (aver_phys_add_static_box returns an opaque int and takes no entity), so the
    // fact is recorded here and rebuilt on restore rather than looked up.
    bool hasBody = false;
    f32  bodyHalfExtentCm[3] = {0, 0, 0};
};

// One chunk's worth of entities.
struct ChunkPayload {
    ChunkCoord coord;
    i32 chunkSizeCm = kDefaultChunkSizeCm;
    // Topologically ordered: every parent precedes its children.
    std::vector<PayloadEntity> entities;
};

// What a host lends the restore pass. The same shape as InstantiateOptions, and for the same reason:
// resolving a surface name to a material and building a collision box are the HOST's business -- the
// game goes through GameContent, the editor through its own map -- so this says when, never how.
struct RestoreOptions {
    // Called for an entity carrying a non-empty material name, with the token that name interned to.
    std::function<void(i32 token, const std::string& surface)> bindMaterial;
    // Called to build the static box. Returns the body id, or -1. Absent means bodies are skipped,
    // which is what a headless partition/reassemble test wants.
    std::function<i32(scene::Entity e, const Vec3& worldPos, const Vec3& halfExtentCm)> createBody;
};

// Captures `roots` and their whole subtrees into a payload.
//
// HIERARCHIES ARE CAPTURED WHOLE, always, whatever their children's positions. CHierarchy is
// intrusive (parent/child/sibling links straight into the components), so a child that loads without
// its parent is a dangling link -- a crash, not a glitch. That is why ownership is decided per ROOT
// and never per entity.
ChunkPayload capture(const scene::World& w, const std::vector<scene::Entity>& roots,
                     const ChunkCoord& coord, i32 chunkSizeCm);

// Recreates a payload's entities in `w`, in order. Returns them parallel to payload.entities.
std::vector<scene::Entity> restore(const ChunkPayload& p, scene::World& w,
                                   const RestoreOptions& opt = {});

} // namespace aver::world

#endif // AVER_MODULE_SCENE
