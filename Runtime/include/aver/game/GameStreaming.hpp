// GameStreaming: opt-in PCG chunk streaming around a moving viewer (the player camera), built on
// aver::world::ChunkWorld.
//
// Shared by every host that streams chunks around a camera: GameApp drives tick() with the camera
// alone, while the editor's SandboxApp drives the same tick() with a second source (its graph-driven
// drone) folded in, a triangle count folded into the load/evict log line, and its own hint appended
// to warnIfCameraOutsideGeneratedBand's message -- the Window-menu toggle and MCP wiring stay
// editor-only, layered on top of this.
//
// Takes the project, the level's PCG records and the content cache as arguments -- each host hands
// over its own project and level state; see enable() for what it does with them.
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
// kept resident around a moving camera, and optionally a second independently-moving source. See
// enable() for construction and tick() for the per-frame step.
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

    // One step, driving every resident field from the camera and, when given, a second
    // independently-moving source. A no-op, returning a default StreamStats, when not enabled().
    // Call right before World::flush, which retires this frame's evictions.
    //
    // VELOCITY IS ZEROED ON THE FIRST TICK after enable() and after resetVelocityTracking() --
    // differencing against a stale/teleported-from position would ask the streamer to prefetch a
    // corridor toward nowhere real.
    //
    // `second`, left null, means exactly today's single-source path. Non-null, it is combined with
    // the camera into a two-entry StreamSource list -- {camPos, vel} then *second, `second`'s own
    // velocityCmPerSec used as given -- and driven through aver::world::ChunkWorld's
    // std::vector<StreamSource> update overload on every resident field instead of the single-source
    // one (a possessed pawn distinct from the camera, or the editor's graph-driven drone, plug in
    // here).
    //
    // `trisLookup`, when non-empty, folds residentTriangleCount(trisLookup) into the load/evict log
    // line as a "/{}tris" segment; left empty, that line omits it.
    world::StreamStats tick(const Vec3& camPos, f32 dt, const world::StreamSource* second = nullptr,
                            const TriangleLookupFn& trisLookup = {});

    // Call after teleporting the tracked camera (a level change, a respawn, an editor-style camera
    // jump) so the next tick() computes zero velocity instead of one huge one-frame spike toward
    // wherever the camera used to be.
    void resetVelocityTracking() { haveLastPos_ = false; }

    // True when any resident field owns `e`. A shipped game has no World Outliner to filter with
    // this, but any future save/serialise path needs the same "streamed, not authored" distinction
    // the editor's own anyChunkWorldOwns answers.
    bool owns(scene::Entity e) const;

    // Warns once when `camPos` sits outside the band a resident field actually populates (the
    // generator fills a single Z layer band; a camera above or below it gets an empty wanted-set by
    // construction, with no error and no chunks). Call after enable() and after any teleport that
    // might have moved the camera out of the band. A no-op when not enabled().
    //
    // `actionHint`, non-null and non-empty, is appended after "...inside that band -- " and supplies
    // its own final punctuation (an editor might pass "press F to focus something near ground level,
    // or fly down."); left null, the message just ends "...inside that band."
    void warnIfCameraOutsideGeneratedBand(const Vec3& camPos, const char* actionHint = nullptr) const;

    // Sum of triangle counts over every entity every resident field currently owns, via `lookup`.
    // O(residentEntities); walked fresh rather than kept running -- a few hundred entities at most.
    // Returns 0 when not enabled() or `lookup` is empty.
    u64 residentTriangleCount(const TriangleLookupFn& lookup) const;

    const world::StreamStats& stats() const { return stats_; }
    // The primary field's settings. Only meaningful while enabled(); returns a static
    // default-constructed ChunkWorldSettings when not.
    const world::ChunkWorldSettings& settings() const;

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
    // Decreasing-frequency counter for the per-tick timing line (1, 2, 4, 8, ... frames apart) so a
    // long run is not flooded.
    u32  reports_ = 0;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
