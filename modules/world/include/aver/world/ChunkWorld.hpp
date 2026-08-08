// The reusable object that binds a ChunkStreamer to a source stack and a scene, so a host only has
// to say "here is where the viewer is" once a frame and never has to assemble a generator, a
// region-file override layer and a streamer by hand to get a streaming world.
//
// THE SOURCE STACK IS FIXED, ON PURPOSE. docs/CHUNKS.md describes exactly one shape -- generated
// content underneath, persisted overrides on top -- and LayeredChunkSource is already the type that
// implements it (ChunkGenerator.hpp:99-114). ChunkWorld does not invent a second way to combine a
// generator with a save; it owns one of each and binds them the one way the format supports.
//
// THE OVERRIDE SOURCE IS NEVER NULL, unlike the general LayeredChunkSource contract that allows it.
// RegionChunkSource with nothing open()'d answers has()/load() false for everything (ChunkSource.cpp:
// 49-52, `index_.find` on an empty index) -- the same behaviour a null pointer would produce, through
// LayeredChunkSource::isOverridden's `overrides_ && overrides_->has(c)` short-circuit. Owning the
// object outright rather than conditionally opening it removes a null-pointer case from this class
// for a behaviour that was already the same either way.
//
// GUARDS: THE WHOLE CLASS IS GATED ON AVER_MODULE_SCENE, not narrowed member-by-member. Every type
// it is built from -- BodyRegistry, ChunkStreamer, GeneratedChunkSource, RegionChunkSource,
// LayeredChunkSource -- is already gated the same way, entirely, for the same reason (each one's own
// header says so). A ChunkWorld that tried to exist without scene::World would have nothing left to
// hold: there is no scene-free subset of "stream chunks into a scene" that means anything. So a
// consumer guards ITS use of aver::world::ChunkWorld with `#if AVER_MODULE_SCENE`, exactly as it
// already must for aver::world::ChunkStreamer -- this class adds no new guarding shape to learn.
//
// PHYSICS: THIS CLASS TAKES NO DEPENDENCY ON Aver.Physics AND NEEDS NONE. BodyRegistry -- the type
// ChunkStreamer::update requires -- links only Aver.Scene by its own design (BodyRegistry.hpp:21-23),
// and ChunkWorld does the same: it owns a BodyRegistry and forwards ChunkStreamer::update's
// freedBodies out to its caller, exactly as ChunkStreamer itself does, without ever calling into a
// physics ABI. So a tree built with AVER_MODULE_PHYSICS=OFF needs no special case here at all -- the
// "handle PHYSICS=OFF explicitly" instruction is satisfied by NOT depending on the module, not by an
// #if that reacts to its absence. The caller who DOES have physics is the one who owns the
// createBody/destroy wiring, via restoreOptions() and the freedBodies this returns, same as always.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/ChunkPayload.hpp"
#include "aver/world/ChunkSource.hpp"
#include "aver/world/ChunkStreamer.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <string>
#  include <unordered_set>
#  include <vector>

namespace aver::world {

// Where a ChunkWorld lives on disk, and how it generates what is not saved yet.
struct ChunkWorldSettings {
    // The directory holding this world's `.ocindex` and `.avrgn` files. Created if it does not
    // exist -- refusing to run without one already there is not the contract; a brand-new project
    // has no world directory until its first world does.
    std::string worldDir;
    GeneratorSettings generator;
    StreamSettings stream;
};

// Binds a ChunkStreamer to a generated-baseline-plus-saved-overrides source stack and a scene. A
// host drives it with one call a frame: "the viewer is here, moving like this."
class ChunkWorld {
public:
    // Creates `settings.worldDir` if it is absent. If an `.ocindex` already lives there, its
    // regions become the override layer served on top of generation; if not, generation alone
    // answers everything, exactly the state a brand-new world is in before anything is saved.
    //
    // False only on a real failure -- the directory could not be created. A missing or absent index
    // is NOT a failure: see the header comment on why the override source tolerates that silently.
    bool open(const ChunkWorldSettings& settings, std::string* why = nullptr);

    // One step. Reports the viewer's position and velocity, drives the streamer, and returns its
    // stats. `dt` is accepted for symmetry with a host's own per-frame Timestep and is not otherwise
    // used here: the streamer's own look-ahead runs on `StreamSettings::leadSeconds * velocity`, not
    // on the caller's frame time, so nothing in this class multiplies by it. A future host that wants
    // to smooth a jittery velocity estimate across frames has somewhere to plug that in without
    // changing this signature again.
    //
    // Bodies freed by this step's evictions are appended to `freedBodies`, exactly as
    // ChunkStreamer::update documents -- this class links Aver.Scene and not Aver.Physics, and stays
    // that way, so the caller (who may or may not have physics) removes them.
    StreamStats update(scene::World& w, const Vec3& viewerPosCm, const Vec3& viewerVelCmPerSec,
                       f32 dt, std::vector<i32>* freedBodies = nullptr);

    // Same step, for more than one thing the world stays loaded around at once -- e.g. the editor
    // camera AND a graph-driven actor flying independently of it. `sources` replaces whatever was
    // passed last call entirely (this is not additive across calls); a caller that wants the camera
    // to keep counting must include it in `sources` itself. See StreamSource (ChunkStreamer.hpp) for
    // why this is a list rather than a single position in the first place.
    StreamStats update(scene::World& w, const std::vector<StreamSource>& sources,
                       f32 dt, std::vector<i32>* freedBodies = nullptr);

    // Every entity this world currently owns, chunk by chunk flattened into one list. This is the
    // answer an outliner or a save path needs to tell a streamed entity from an authored one --
    // without it, a streamed world gets written into a level file or clutters a panel meant for what
    // a designer actually placed. Rebuilt after each update() from ChunkStreamer::residentChunks(),
    // which is what makes this possible at all: there was no way to enumerate a streamer's resident
    // set before that method existed.
    const std::vector<scene::Entity>& streamedEntities() const { return owned_; }
    bool owns(scene::Entity e) const { return ownedSet_.find(e) != ownedSet_.end(); }

    // Drops every entity this world holds and returns the scene to what it looked like before this
    // world touched it. Bodies go out through `freedBodies`, same as update().
    void shutdown(scene::World& w, std::vector<i32>& freedBodies);

    ChunkStreamer& streamer() { return streamer_; }
    const ChunkStreamer& streamer() const { return streamer_; }
    BodyRegistry& bodies() { return bodies_; }
    const StreamStats& stats() const { return streamer_.stats(); }
    const ChunkWorldSettings& settings() const { return settings_; }

private:
    void rebuildOwned();

    ChunkWorldSettings settings_;
    bool opened_ = false;
    std::string indexPath_;

    // Declaration order matters: `source_`'s in-class initializer takes the addresses of the two
    // members before it, which are guaranteed constructed by the time `source_` is -- C++ runs
    // member initialization in declaration order regardless of an initializer list's order, and a
    // constructor never needs to name `source_` explicitly because of it.
    GeneratedChunkSource generator_;
    RegionChunkSource overrides_;
    LayeredChunkSource source_{&generator_, &overrides_};

    ChunkStreamer streamer_;
    BodyRegistry bodies_;

    std::vector<scene::Entity> owned_;
    std::unordered_set<scene::Entity> ownedSet_;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
