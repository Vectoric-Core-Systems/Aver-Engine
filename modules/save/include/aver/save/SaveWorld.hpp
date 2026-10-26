#pragma once
// Capturing a scene::World into an OcSaveData and putting it back.
//
// A SECOND TARGET rather than an edge added to either side, for the reason Aver.Anim.Scene is one:
// Aver.Formats sits below Aver.Scene and must not reach it, and Aver.Scene must not gain a Formats
// edge (its components carry opaque ids so nothing unmarshallable reaches the P/Invoke boundary).
// Turning a live world into a file therefore cannot live in either, and lives here.
//
// IT DOES NOT LINK Aver.Framework. Spawning an actor by class name, asking what class an entity is,
// and destroying one through the path that runs OnEndPlay all arrive as HOST-INSTALLED FUNCTION
// POINTERS -- the same shape AnimSystem::setResolver, AnimSystem::setNotifySink and
// aver_fw_set_anim_curve_provider already use, and for the same reason. A world of plain meshes
// saves and restores completely with none of them installed.
#include "aver/formats/OcSave.hpp"
#include "aver/scene/World.hpp"

namespace aver::save {

// Spawns an instance of `className` WITHOUT dispatching BeginPlay, and returns it.
//
// WITHOUT, and this is not a preference. aver_fw_spawn is synchronous -- spawnActor runs
// bind -> build_models -> beginPlay inline before it returns (FrameworkAbi.cpp:535-580) -- so an
// actor spawned and THEN patched has already run OnBeginPlay against its CLASS DEFAULTS. A door
// that checks `isOpen` to decide whether to play its opening animation reads provably wrong data on
// every single load. The host installs aver_fw_spawn_preview here (which passes
// dispatchBeginPlay=false) and `beginPlay` below is called after the fields are in.
using SpawnClassFn = scene::Entity (*)(const char* className, void* user);
// The class this entity is an instance of, or nullptr/"" for a plain entity.
using ClassOfFn = const char* (*)(scene::Entity e, void* user);
// Dispatches BeginPlay on a restored actor, after its fields are back.
using BeginPlayFn = void (*)(scene::Entity e, void* user);
// Destroys an actor through the framework's own path, so OnEndPlay runs and its managed instance is
// released. Plain entities are destroyed through World::destroy and never reach this.
using DestroyActorFn = void (*)(scene::Entity e, void* user);

// GRAPH-LOCAL VARIABLES (Aver.Graph's own GraphVarStore) live entirely in MANAGED memory, with no
// representation in the native scene at all -- there is no component this module's ordinary,
// generic field capture (OcSave.hpp's own "generic over components, by name" design) could ever
// see. These two seams are the second, narrower door that lets a save reach them anyway, without
// this module gaining an edge to Aver.Framework OR Aver.Graph -- the identical reasoning
// spawnClass/classOf/beginPlay/destroyActor above already gives, applied to a kind of state those
// four cannot touch. COUNT THEN INDEX, matching aver_fw_graph_var_count/_at's own shape
// (framework_abi.h) exactly, since that ABI is what a host's implementation of these ultimately
// relays through.
using GraphVarCountFn = i32 (*)(scene::Entity e, void* user);
using GraphVarAtFn    = i32 (*)(scene::Entity e, i32 index, char* nameBuf, i32 nameBufLen,
                                u32* outKind, f32* outF, i32* outI, void* user);
using GraphVarSetFn   = i32 (*)(scene::Entity e, const char* name, u32 kind, f32 f, i32 i, void* user);

// MANAGED ENTITY STATE (framework_state_abi.h): named text channels per entity, for state that lives
// in C# and has no native representation (gameplay abilities is the first user). Same COUNT / AT / SET
// shape as the graph-var seam, relayed through aver_fw_state_*; this module still does not link the
// framework. stateAt returns the payload's FULL byte length (it may exceed dataCap; the caller retries
// with a larger buffer) or -1 for a bad index; stateSet returns 1 if the channel was accepted.
using StateCountFn = i32 (*)(scene::Entity e, void* user);
using StateAtFn    = i32 (*)(scene::Entity e, i32 index, char* nameBuf, i32 nameCap,
                             char* dataBuf, i32 dataCap, void* user);
using StateSetFn   = i32 (*)(scene::Entity e, const char* name, const char* data, i32 dataLen, void* user);

// The seams a host supplies. Every one may be null; the effect of leaving one null is stated.
struct Host {
    SpawnClassFn   spawnClass   = nullptr;   // null: actors restore as PLAIN entities, and say so
    ClassOfFn      classOf      = nullptr;   // null: nothing is recognised as an actor
    BeginPlayFn    beginPlay    = nullptr;   // null: a restored actor never begins play
    DestroyActorFn destroyActor = nullptr;   // null: actors are torn down as plain entities
    GraphVarCountFn graphVarCount = nullptr; // null: no entity is ever asked for graph VARs
    GraphVarAtFn    graphVarAt    = nullptr; // null: capture() never reads them (paired with above)
    GraphVarSetFn   graphVarSet   = nullptr; // null: restore() never writes them; graphs keep defaults
    StateCountFn    stateCount    = nullptr; // null: no managed state is captured
    StateAtFn       stateAt       = nullptr; // null: no managed state is captured (paired with above)
    StateSetFn      stateSet      = nullptr; // null: restore() counts every "$State" field as dropped
    void*          user         = nullptr;
};

// True for an entity this save should pretend does not exist.
using SkipFn = bool (*)(scene::Entity e, void* user);

struct CaptureOptions {
    // The floating origin (centimetres, a whole number of regions) the world's positions are measured from. It is stored in the
    // snapshot (the additive ORIG chunk) so a restore under another origin can re-express every root (LARGE_WORLD 2.6.4).
    // Zero, the default, writes no chunk and the file is byte-identical to before.
    i64 originX = 0;
    i64 originY = 0;
    Host host;
    // SKIPPED WITH ITS WHOLE SUBTREE. A host that streams chunks passes its own
    // "does any ChunkWorld own this" predicate here: streamed content already has its own
    // persistence (world::RegionFile / ChunkPayload) and capturing it as well would restore two
    // copies of every tree.
    SkipFn skip = nullptr;
};

struct RestoreOptions {
    // The origin the world measures from NOW. restore() adds (the snapshot's origin - this) to X and Y of every root entity's
    // CLocal.position, so each root lands at the same absolute place. Equal origins change nothing.
    i64 originX = 0;
    i64 originY = 0;
    Host host;
    // Entities this returns true for are LEFT ALONE by the teardown -- they are not destroyed and
    // not replaced. Pass the same predicate capture used, or restore will delete the streamed
    // content the snapshot deliberately never captured.
    SkipFn skip = nullptr;
};

// Captures every live entity (minus `skip`) into `out`, parents before children.
//
// The ORDER is the identity: OcSaveData stores a parent as an index into its own array, and this
// function is what guarantees that index is always lower. See OcSave.hpp for why an index rather
// than an id.
bool capture(const scene::World& w, fmt::OcSaveData& out, const CaptureOptions& opt = {},
             std::string* why = nullptr);

// Tears the world down and rebuilds it from `in`. Returns false and leaves the world EMPTY on a
// failure part-way -- a half-restored world is worse than an empty one, because it looks playable.
bool restore(const fmt::OcSaveData& in, scene::World& w, const RestoreOptions& opt = {},
             std::string* why = nullptr);

} // namespace aver::save
