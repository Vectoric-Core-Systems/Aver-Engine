// GameStreaming: opt-in PCG chunk streaming around a moving viewer (the player camera), built on
// aver::world::ChunkWorld.
//
// Mirrors SandboxApp::setChunkStreamingEnabled / warnIfCameraOutsideGeneratedBand /
// residentTriangleCount (sandbox/src/SandboxLevelLoad.cpp) and the per-frame tick inside
// SandboxApp::onUpdate (sandbox/src/SandboxApp.cpp) -- minus the editor-only parts: the Window-menu
// toggle, MCP, and the graph-driven drone as a second StreamSource (see tick()).
//
// Takes the project, the level's PCG records and the content cache as arguments; GameApp hands over
// what setChunkStreamingEnabled reads off SandboxApp.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"

#include <functional>
#include <string>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/world/ChunkWorld.hpp"

#  include <memory>

namespace aver::game {

class GameContent;

// One or more aver::world::ChunkWorld instances -- one per non-"Sky" PCGVOLUME a level declares --
// kept resident around a moving viewer. See enable() for construction and tick() for the per-frame
// step.
class GameStreaming {
public:
    // Height at a world (x, y), in centimetres, for scatter placement to follow terrain. Returns
    // false for "no surface known here" -- passed straight into
    // aver::world::GeneratorSettings::heightSource, whose contract this mirrors exactly. Left empty
    // (the default) when the level has no landscape, or the caller has none wired up: an empty
    // std::function is falsy, so every candidate's Z stays flat, the same as never installing one.
    //
    // A callback rather than a landscape reference, so this class does not depend on Aver.Landscape
    // (GeneratorSettings::heightSource asks the same of its installer).
    using HeightQueryFn = std::function<bool(f32 worldXCm, f32 worldYCm, f32& outWorldZCm)>;
    // Triangle count for a mesh id, for residentTriangleCount() below. Returns 0 for an id it does
    // not know.
    using TriangleLookupFn = std::function<u32(u64 meshId)>;

    // Builds one aver::world::ChunkWorld per non-"Sky" PCGVOLUME `pcgVolumes` declares (or one on
    // GeneratorSettings' shipped defaults when `pcgVolumes` is empty -- the same "a level that never
    // mentions a volume still streams one species" fallback the editor keeps). A no-op if already
    // enabled(); call disable() first to rebuild with different content.
    //
    //   project       - refused unless valid(), as in the editor. Chunks are written under
    //                   `<project dir>\Chunks`, or `\Chunks\<field name or index>` once more than one
    //                   field builds a world (region files are keyed by chunk coordinate, so two
    //                   worlds sharing a directory would overwrite each other).
    //   pcgVolumes    - the level's raw PCGVOLUME records (GameLevel::pcgVolumes()), not pcgFields(),
    //                   which dropped samplesPerAxis, radiusChunks and the names SCATTER refers to.
    //   scatterSpecies - the level's raw SCATTER records.
    //   content       - binds each restored entity's surface material, as GameLevel::load does. May
    //                   be null.
    //   heightSource  - see HeightQueryFn. Empty = flat.
    void enable(const fmt::ProjectDesc& project,
                const std::vector<fmt::OcPcgVolume>& pcgVolumes,
                const std::vector<fmt::OcScatterSpecies>& scatterSpecies,
                GameContent* content, HeightQueryFn heightSource = {});

    // Drops every resident field and returns the scene to what it looked like before this object
    // touched it. A no-op if not enabled(). Frees each field's physics bodies itself (guarded
    // AVER_MODULE_PHYSICS), the same way GameLevel::unload() frees its own -- this class has no
    // caller-facing "freed bodies" list to hand back, unlike aver::world::ChunkWorld's own lower-level
    // contract, because it already has everything aver_phys_remove_body needs.
    void disable();

    bool enabled() const { return primary_ != nullptr; }

    // One step, driving every resident field from a single moving source (the camera). A no-op,
    // returning a default StreamStats, when not enabled(). Call right before World::flush, which
    // retires this frame's evictions, as the editor's tick does.
    //
    // VELOCITY IS ZEROED ON THE FIRST TICK after enable() and after resetVelocityTracking() --
    // differencing against a stale/teleported-from position would ask the streamer to prefetch a
    // corridor toward nowhere real. Mirrors SandboxApp.cpp's identical guard on chunkStreamHaveLastPos_.
    //
    // SINGLE-SOURCE ONLY. The editor also streams around a second, independently-moving source (the
    // graph-driven drone, via aver::world::ChunkWorld's std::vector<StreamSource> update overload) --
    // that stays editor-only here along with the drone itself. A future caller with a second mover
    // to keep resident around (a possessed pawn distinct from the camera, say) has nowhere to plug
    // that in yet; extending this to take an optional second source is straightforward against
    // ChunkWorld's existing overload if that is ever needed.
    world::StreamStats tick(const Vec3& camPos, f32 dt);

    // Call after teleporting the tracked camera (a level change, a respawn, an editor-style camera
    // jump) so the next tick() computes zero velocity instead of one huge one-frame spike toward
    // wherever the camera used to be. Mirrors SandboxViewport.cpp's frameCameraOn setting
    // chunkStreamHaveLastPos_ = false on every teleport.
    void resetVelocityTracking() { haveLastPos_ = false; }

    // True when any resident field owns `e`. A shipped game has no World Outliner to filter with
    // this, but any future save/serialise path needs the same "streamed, not authored" distinction
    // the editor's own anyChunkWorldOwns answers.
    bool owns(scene::Entity e) const;

    // Warns once when `camPos` sits outside the band a resident field actually populates (the
    // generator fills a single Z layer band; a camera above or below it gets an empty wanted-set by
    // construction, with no error and no chunks). Call after enable() and after any teleport that
    // might have moved the camera out of the band. A no-op when not enabled().
    void warnIfCameraOutsideGeneratedBand(const Vec3& camPos) const;

    // Sum of triangle counts over every entity every resident field currently owns, via `lookup`.
    // O(residentEntities); walked fresh rather than kept running, matching the editor's own
    // residentTriangleCount (SandboxLevelLoad.cpp) -- a few hundred entities at most. Returns 0 when
    // not enabled() or `lookup` is empty.
    u64 residentTriangleCount(const TriangleLookupFn& lookup) const;

    const world::StreamStats& stats() const { return stats_; }

private:
    static void accumulateStreamStats(world::StreamStats& into, const world::StreamStats& add);

    std::unique_ptr<world::ChunkWorld> primary_;
    // The level's SECOND and further density fields, one ChunkWorld each -- see enable()'s .cpp
    // comment for why one radius cannot serve every field. Empty for a level declaring at most one.
    std::vector<std::unique_ptr<world::ChunkWorld>> extra_;

    world::StreamStats stats_;
    Vec3 lastPos_{};
    bool haveLastPos_ = false;
    // First few load/evict frames get an explicit log line, then it quiets down. Reset by enable().
    u32  logsLeft_ = 0;
    // Decreasing-frequency counter for the per-tick timing line, matching SandboxApp.cpp's own
    // chunkStreamReports_ (1, 2, 4, 8, ... frames apart) so a long run is not flooded.
    u32  reports_ = 0;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
