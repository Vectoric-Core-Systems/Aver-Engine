#include "aver/game/GameStreaming.hpp"

#if AVER_MODULE_SCENE

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/Components.hpp"
#include "aver/world/ScatterPalette.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif

namespace aver::game {

namespace {
// The residency radius ceiling, shared by the manifest's STREAM.LOADRADIUS / EVICTRADIUS /
// VERTICALRADIUS and by a PCGVOLUME's own radiusChunks. Both are hand-authored text and chunk
// residency cost grows with the cube of this, so a stray digit must not be able to ask for a world
// that never finishes loading. One constant rather than one per site: two ceilings that disagree
// would make the same number legal from the level and clamped from the project.
constexpr i32 kMaxRadiusChunks = 24;

// Names the key it clamped, because the value the streamer ends up running is otherwise nowhere a
// reader of the manifest can see it.
i32 clampRadiusKey(const char* key, i32 asked) {
    if (asked <= kMaxRadiusChunks) return asked;
    AVER_WARN("[ChunkWorld] {} asks for radius {}; clamped to {}", key, asked, kMaxRadiusChunks);
    return kMaxRadiusChunks;
}
} // namespace

void GameStreaming::accumulateStreamStats(world::StreamStats& into, const world::StreamStats& add) {
    into.residentChunks    += add.residentChunks;
    into.residentEntities  += add.residentEntities;
    into.loadedThisUpdate  += add.loadedThisUpdate;
    into.evictedThisUpdate += add.evictedThisUpdate;
    into.entitiesIn        += add.entitiesIn;
    into.entitiesOut       += add.entitiesOut;
    into.pendingLoads      += add.pendingLoads;
    into.failedLoads       += add.failedLoads;
    into.totalLoads        += add.totalLoads;
}

void GameStreaming::disable() {
    if (!primary_) return;

    std::vector<i32> freed;
    world::StreamStats last = primary_->stats();
    primary_->shutdown(scene::World::instance(), freed);
    // Every extra field is torn down in the same pass and into the SAME `freed` list -- those
    // bodies are as real as the primary's, and leaving them would leak a physics body per streamed
    // collider each time streaming is toggled.
    for (auto& extra : extra_) {
        if (!extra) continue;
        accumulateStreamStats(last, extra->stats());
        extra->shutdown(scene::World::instance(), freed);
    }
    extra_.clear();
    scene::World::instance().flush();
#if AVER_MODULE_PHYSICS
    for (const i32 b : freed) if (b >= 0) aver_phys_remove_body(b);
#endif
    primary_.reset();
    haveLastPos_ = false;
    stats_ = world::StreamStats{};
    AVER_INFO("[ChunkWorld] streaming disabled -- {} chunk(s) / {} entities released",
              last.residentChunks, last.residentEntities);
}

void GameStreaming::enable(const fmt::ProjectDesc& project,
                            const std::vector<fmt::OcPcgVolume>& pcgVolumes,
                            const std::vector<fmt::OcScatterSpecies>& scatterSpecies,
                            GameContent* content, HeightQueryFn heightSource) {
    if (primary_) return;

    if (!project.valid()) {
        AVER_WARN("[ChunkWorld] cannot enable streaming: no project is open");
        return;
    }
    const std::string& projectDir = project.dir;
    const std::string contentDir = project.contentDir();

    // ---- ONE ChunkWorld PER DECLARED DENSITY FIELD ----
    // A ChunkWorld's loadRadius decides how far the world populates, and one radius cannot serve a
    // dense floor and a sparse canopy -- see aver::world::ChunkWorldSettings.
    std::vector<const fmt::OcPcgVolume*> fields;
    for (const fmt::OcPcgVolume& pv : pcgVolumes)
        if (pv.name != "Sky") fields.push_back(&pv);

    // No non-Sky volume still builds exactly ONE world on GeneratorSettings' shipped defaults --
    // the single-species fallback every level had before PCGVOLUME existed.
    const usize fieldCount = fields.empty() ? usize{1} : fields.size();
    const bool multi = fieldCount > 1;

    // A species naming a volume the level does not declare would otherwise scatter nowhere, in
    // silence. Reported once per bad name and folded into the primary rather than dropped.
    for (const fmt::OcScatterSpecies& sp : scatterSpecies) {
        if (sp.volume.empty()) continue;
        bool found = false;
        for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { found = true; break; }
        if (!found)
            AVER_WARN("[ChunkWorld] SCATTER '{}' names volume '{}', which this level does not "
                      "declare; it will scatter in the first field instead",
                      sp.meshPath, sp.volume);
    }

    // ---- STREAM.* FROM THE MANIFEST, WHICH USED TO REACH NOTHING --------------------------------
    //
    // All six keys parse, serialise and carry a Project Settings widget, and not one of them was
    // read: ChunkWorldSettings reached open() on world::StreamSettings' compiled-in defaults, so a
    // title that lowered its load budget for a slower platform shipped the built-in 2 anyway. A key
    // that survives a save and changes nothing is worse than a missing key -- it reads as a dial
    // somebody already tried, which is why this one is applied rather than deleted.
    //
    // UNSTATED IS NEGATIVE, this block's own rule in OcProject.hpp, so a key the project never
    // writes leaves the StreamSettings default standing instead of writing a zero over it. A STATED
    // zero budget IS honoured and means unbounded -- StreamSettings' own documented contract for it,
    // and the reason the test here is `>= 0` rather than `> 0`.
    world::StreamSettings manifestStream;   // defaults until a key overrides one
    if (project.streamLoadRadius >= 0)
        manifestStream.loadRadius = clampRadiusKey("STREAM.LOADRADIUS", project.streamLoadRadius);
    if (project.streamEvictRadius >= 0)
        manifestStream.evictRadius = clampRadiusKey("STREAM.EVICTRADIUS", project.streamEvictRadius);
    if (project.streamVerticalRadius >= 0)
        manifestStream.verticalRadius =
            clampRadiusKey("STREAM.VERTICALRADIUS", project.streamVerticalRadius);
    if (project.streamLoadBudget >= 0)
        manifestStream.loadBudget = static_cast<u32>(project.streamLoadBudget);
    if (project.streamEvictBudget >= 0)
        manifestStream.evictBudget = static_cast<u32>(project.streamEvictBudget);
    if (project.streamLeadSeconds >= 0.0f)
        manifestStream.leadSeconds = project.streamLeadSeconds;
    // Logged only when the project actually stated something, so a manifest with no STREAM block
    // does not grow a line claiming it configured streaming. evictRadius may still be raised from
    // here by ChunkStreamer::setSettings, which enforces evictRadius > loadRadius rather than
    // trusting either source of them.
    if (project.hasStreamSettings())
        AVER_INFO("[ChunkWorld] STREAM.* applied -- loadRadius={} evictRadius={} loadBudget={} "
                  "evictBudget={} verticalRadius={} leadSeconds={:.2f}",
                  manifestStream.loadRadius, manifestStream.evictRadius, manifestStream.loadBudget,
                  manifestStream.evictBudget, manifestStream.verticalRadius,
                  manifestStream.leadSeconds);

    std::vector<std::unique_ptr<world::ChunkWorld>> built;
    for (usize fi = 0; fi < fieldCount; ++fi) {
        const fmt::OcPcgVolume* v = fields.empty() ? nullptr : fields[fi];

        // Which species this field places. An UNNAMED species goes to the first field, so a level
        // never mentioning volumes still produces one world with the whole palette.
        std::vector<fmt::OcScatterSpecies> mine;
        for (const fmt::OcScatterSpecies& sp : scatterSpecies) {
            bool named = false;
            if (!sp.volume.empty())
                for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { named = true; break; }
            if (named) { if (v && sp.volume == v->name) mine.push_back(sp); }
            else if (fi == 0)                            mine.push_back(sp);
        }
        // A field with no species would generate the fallback cube everywhere. Skip it: a level may
        // declare a field for something other than scatter (a cave mask, a moisture map).
        if (mine.empty()) {
            AVER_INFO("[ChunkWorld] field '{}' has no SCATTER species; not streamed",
                      v ? v->name : std::string("<none>"));
            continue;
        }

        auto cw = std::make_unique<world::ChunkWorld>();
        world::ChunkWorldSettings cwSettings;
        // The project's budgets first; anything this field states for itself overrides them below.
        cwSettings.stream = manifestStream;

        // PER FIELD ONLY WHEN THERE IS MORE THAN ONE: region files are keyed by chunk coordinate, so
        // two worlds sharing a directory would write each other's chunks.
        cwSettings.worldDir = projectDir + "\\Chunks";
        if (multi) cwSettings.worldDir += "\\" + (v && !v->name.empty() ? v->name : std::to_string(fi));

        {
            std::vector<std::string> scatterErrors;
            if (!world::buildScatterPalette(mine, contentDir, cwSettings.generator.palette, scatterErrors)) {
                for (const std::string& e : scatterErrors)
                    AVER_WARN("[ChunkWorld] {}", e);
            }
        }

        if (v) {
            cwSettings.generator.worldSeed = static_cast<u64>(static_cast<u32>(v->seed));
            if (v->cellSizeCm > 0.0) cwSettings.generator.featureSizeCm = static_cast<f32>(v->cellSizeCm);
            if (v->octaves > 0) cwSettings.generator.octaves = static_cast<u32>(v->octaves);
            {
                const f64 t = v->coverageFloor < 0.0 ? 0.0
                                                      : (v->coverageFloor > 1.0 ? 1.0 : v->coverageFloor);
                cwSettings.generator.threshold = static_cast<f32>(t);
            }
            // Clamped rather than trusted: cost is quadratic in this and the file is authored by
            // hand, so a stray digit would generate millions of entities per chunk.
            if (v->samplesPerAxis > 0) {
                constexpr i32 kMaxSamplesPerAxis = 64;   // 4096 candidates in one chunk
                const i32 n = v->samplesPerAxis > kMaxSamplesPerAxis ? kMaxSamplesPerAxis
                                                                      : v->samplesPerAxis;
                if (n != v->samplesPerAxis)
                    AVER_WARN("[ChunkWorld] PCGVOLUME '{}' asks for {} samples per axis; clamped to {}",
                              v->name, v->samplesPerAxis, n);
                cwSettings.generator.samplesPerAxis = static_cast<u32>(n);
            }
            // evictRadius is raised with loadRadius: ChunkStreamer.hpp requires evictRadius >
            // loadRadius or the boundary thrashes, and it enforces that rather than trusting the caller.
            //
            // AND THIS BEATS THE MANIFEST'S STREAM.LOADRADIUS, which was applied to cwSettings.stream
            // above. That is the narrower statement winning, for the same reason this function builds
            // one ChunkWorld per field at all: a single project-wide radius cannot serve a dense floor
            // and a sparse canopy, so a field that states its own has said something the project-wide
            // key cannot say. The remaining four STREAM.* keys are untouched here and still apply.
            if (v->radiusChunks > 0) {
                const i32 r = v->radiusChunks > kMaxRadiusChunks ? kMaxRadiusChunks : v->radiusChunks;
                if (r != v->radiusChunks)
                    AVER_WARN("[ChunkWorld] PCGVOLUME '{}' asks for radius {}; clamped to {}",
                              v->name, v->radiusChunks, r);
                cwSettings.stream.loadRadius  = r;
                cwSettings.stream.evictRadius = r + 2;
            }
        }

        // An empty heightSource is falsy (GeneratorSettings::heightSource's own contract), so this
        // costs a caller with no landscape nothing -- applied to every field, terrain or not, same as
        // the editor's own `#if AVER_MODULE_LANDSCAPE` block.
        cwSettings.generator.heightSource = heightSource;

        world::RestoreOptions& restore = cw->streamer().restoreOptions();
#if AVER_MODULE_PBR
        restore.bindMaterial = [content](i32 token, const std::string& surface) {
            if (!content) return;
            const pbr::MaterialHandle h = content->materialForSurface(surface);
            if (h) content->bindSurfaceMaterial(token, h);
        };
#endif
#if AVER_MODULE_PHYSICS
        restore.createBody = [](scene::Entity e, const Vec3& worldPos, const Vec3& halfExtentCm) -> i32 {
            if (!aver_phys_ready()) return -1;
            const i32 body = aver_phys_add_static_box(worldPos.x, worldPos.y, worldPos.z,
                                                      halfExtentCm.x, halfExtentCm.y, halfExtentCm.z);
            if (body) aver_phys_set_entity(body, static_cast<i32>(e));
            return body;
        };
#endif

        std::string why;
        if (!cw->open(cwSettings, &why)) {
            // One field failing must not take the others down with it -- a level with a good floor
            // and a broken canopy should still show its floor.
            AVER_WARN("[ChunkWorld] field '{}' failed to open: {}",
                      v ? v->name : std::string("<none>"), why);
            continue;
        }

        AVER_INFO("[ChunkWorld] field '{}' -- worldDir='{}' loadRadius={} evictRadius={} "
                  "palette={} species threshold={:.2f} samples={}/axis (populated to {:.0f}m)",
                  v ? v->name : std::string("<none>"), cwSettings.worldDir,
                  cwSettings.stream.loadRadius, cwSettings.stream.evictRadius,
                  cwSettings.generator.palette.size(), cwSettings.generator.threshold,
                  cwSettings.generator.samplesPerAxis,
                  static_cast<f32>(cwSettings.stream.loadRadius * cwSettings.stream.chunkSizeCm) / 100.0f);
        built.push_back(std::move(cw));
    }

#if !AVER_MODULE_PBR
    (void)content;   // only ever read from restore.bindMaterial's closure, above, under AVER_MODULE_PBR
#endif

    if (built.empty()) {
        AVER_WARN("[ChunkWorld] cannot enable streaming: no density field produced a world");
        return;
    }

    primary_ = std::move(built[0]);
    extra_.clear();
    for (usize k = 1; k < built.size(); ++k) extra_.push_back(std::move(built[k]));

    haveLastPos_ = false;
    logsLeft_ = 8;
    reports_ = 0;
    stats_ = world::StreamStats{};
    AVER_INFO("[ChunkWorld] streaming enabled -- {} density field(s), chunkSize={}cm verticalRadius={}",
              built.size(), primary_->settings().stream.chunkSizeCm,
              primary_->settings().stream.verticalRadius);
}

world::StreamStats GameStreaming::tick(const Vec3& camPos, f32 dt, const world::StreamSource* second,
                                        const TriangleLookupFn& trisLookup) {
    if (!primary_) return world::StreamStats{};

    const f32 invDt = dt > 1e-6f ? 1.0f / dt : 0.0f;
    // First tick after enable() (or after resetVelocityTracking()) reports zero velocity:
    // differencing against a stale/teleported-from position would ask the streamer to prefetch a
    // corridor toward nowhere real.
    const Vec3 vel = haveLastPos_ ? (camPos - lastPos_) * invDt : Vec3{0.0f, 0.0f, 0.0f};
    lastPos_ = camPos;
    haveLastPos_ = true;

    const auto t0 = std::chrono::steady_clock::now();
    scene::World& world = scene::World::instance();
    std::vector<i32> freed;
    if (second) {
        // Both entries are as real as each other from here on -- either one moving keeps its own
        // corridor resident, via ChunkWorld's std::vector<StreamSource> overload.
        const std::vector<world::StreamSource> sources = {
            world::StreamSource{camPos, vel},
            *second,
        };
        stats_ = primary_->update(world, sources, dt, &freed);
        for (auto& extra : extra_) {
            if (!extra) continue;
            accumulateStreamStats(stats_, extra->update(world, sources, dt, &freed));
        }
    } else {
        stats_ = primary_->update(world, camPos, vel, dt, &freed);
        for (auto& extra : extra_) {
            if (!extra) continue;
            accumulateStreamStats(stats_, extra->update(world, camPos, vel, dt, &freed));
        }
    }
#if AVER_MODULE_PHYSICS
    for (const i32 b : freed) if (b >= 0) aver_phys_remove_body(b);
#endif

    // The streamer reports a pending backlog in the hundreds for the first few seconds -- it is not
    // generating that much every frame forever, the backlog drains. Reported at a decreasing
    // frequency so a long run is not flooded.
    const f64 streamMs = std::chrono::duration<f64, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    if ((reports_ & (reports_ + 1)) == 0)
        AVER_INFO("[ChunkWorld] stream update {:.1f}ms on the main thread (resident {}, pending {})",
                  streamMs, stats_.residentChunks, stats_.pendingLoads);
    ++reports_;

    // Greppable proof for a headless run: "[ChunkWorld]" lines for the first few frames that
    // actually loaded or evicted something, then it quiets down.
    if (logsLeft_ > 0 && (stats_.loadedThisUpdate > 0 || stats_.evictedThisUpdate > 0)) {
        --logsLeft_;
        if (trisLookup) {
            AVER_INFO("[ChunkWorld] loaded={} evicted={} resident={}chunks/{}entities/{}tris "
                      "pending={} failed={} totalLoads={} vel=({:.0f},{:.0f},{:.0f})cm/s",
                      stats_.loadedThisUpdate, stats_.evictedThisUpdate, stats_.residentChunks,
                      stats_.residentEntities, residentTriangleCount(trisLookup),
                      stats_.pendingLoads, stats_.failedLoads, stats_.totalLoads,
                      vel.x, vel.y, vel.z);
        } else {
            AVER_INFO("[ChunkWorld] loaded={} evicted={} resident={}chunks/{}entities pending={} "
                      "failed={} totalLoads={} vel=({:.0f},{:.0f},{:.0f})cm/s",
                      stats_.loadedThisUpdate, stats_.evictedThisUpdate, stats_.residentChunks,
                      stats_.residentEntities, stats_.pendingLoads, stats_.failedLoads,
                      stats_.totalLoads, vel.x, vel.y, vel.z);
        }
    }
    return stats_;
}

bool GameStreaming::owns(scene::Entity e) const {
    if (primary_ && primary_->owns(e)) return true;
    for (const auto& extra : extra_) if (extra && extra->owns(e)) return true;
    return false;
}

// Says so when streaming is switched on somewhere nothing will ever load. The generator fills a
// SINGLE BAND of chunk layers, and a camera above or below it gets an empty wanted-set by
// construction: no error, no chunks. A WARNING, NOT A CORRECTION -- moving the camera would be a
// harder bug to understand than being told the range is wrong.
void GameStreaming::warnIfCameraOutsideGeneratedBand(const Vec3& camPos, const char* actionHint) const {
    if (!primary_) return;
    const world::StreamSettings& st = primary_->settings().stream;
    if (st.chunkSizeCm <= 0) return;

    const i32 camChunkZ = world::floorDiv(static_cast<i32>(camPos.z), st.chunkSizeCm);
    const i32 surfaceZ  = primary_->settings().generator.surfaceChunkZ;
    if (std::abs(camChunkZ - surfaceZ) <= st.verticalRadius) return;

    // Inclusive of the top layer's full height, so the number quoted is the last Z that can
    // actually contain something rather than the coordinate its floor sits at.
    const i64 lo = i64(surfaceZ - st.verticalRadius) * st.chunkSizeCm;
    const i64 hi = i64(surfaceZ + st.verticalRadius + 1) * st.chunkSizeCm;
    if (actionHint && *actionHint) {
        AVER_WARN("[ChunkWorld] the camera is at Z={:.0f}cm (chunk layer {}), outside the generated "
                  "band {}..{}cm (layers {}..{}). Nothing will load until it is inside that band -- {}",
                  camPos.z, camChunkZ, lo, hi, surfaceZ - st.verticalRadius, surfaceZ + st.verticalRadius,
                  actionHint);
    } else {
        AVER_WARN("[ChunkWorld] the camera is at Z={:.0f}cm (chunk layer {}), outside the generated "
                  "band {}..{}cm (layers {}..{}). Nothing will load until it is inside that band.",
                  camPos.z, camChunkZ, lo, hi, surfaceZ - st.verticalRadius, surfaceZ + st.verticalRadius);
    }
}

// Sum of triangle counts over every entity every resident field currently owns. O(residentEntities),
// walked fresh each call rather than kept running -- a few hundred at most, and this only runs when a
// caller's own diagnostic asks for it.
u64 GameStreaming::residentTriangleCount(const TriangleLookupFn& lookup) const {
    u64 total = 0;
    if (!primary_ || !lookup) return total;
    const scene::World& world = scene::World::instance();
    const auto add = [&](const world::ChunkWorld& cw) {
        for (const scene::Entity e : cw.streamedEntities()) {
            const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            if (!mr) continue;
            total += lookup(mr->mesh);
        }
    };
    add(*primary_);
    for (const auto& extra : extra_) if (extra) add(*extra);
    return total;
}

const world::ChunkWorldSettings& GameStreaming::settings() const {
    if (primary_) return primary_->settings();
    static const world::ChunkWorldSettings kDefault{};
    return kDefault;
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
