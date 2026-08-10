#include "aver/world/ChunkWorld.hpp"

#if AVER_MODULE_SCENE

#  include "aver/platform/FileSystem.hpp"

namespace aver::world {

bool ChunkWorld::open(const ChunkWorldSettings& settings, std::string* why) {
    settings_ = settings;

    if (!aver::createDirectories(settings_.worldDir)) {
        if (why) *why = "could not create world directory: " + settings_.worldDir;
        return false;
    }

    generator_.setSettings(settings_.generator);
    streamer_.setSettings(settings_.stream);
    streamer_.setSource(&source_);

    // Best-effort. A missing index means nothing has been saved yet -- not a failure -- and
    // RegionChunkSource::open leaves `index_` untouched (readIndex does not touch `out` on a file it
    // could not read: RegionIndex.cpp's readIndex returns before decodeIndex is ever called), so a
    // failed open here is indistinguishable from "opened an index with nothing in it" to anything
    // that queries `overrides_` afterwards. Both answer has()/load() false for every chunk.
    indexPath_ = settings_.worldDir + "/world.ocindex";
    std::string indexWhy;
    overrides_.open(indexPath_, settings_.worldDir, &indexWhy);

    owned_.clear();
    ownedSet_.clear();
    opened_ = true;
    return true;
}

void ChunkWorld::rebuildOwned() {
    owned_.clear();
    ownedSet_.clear();
    for (const ChunkCoord& c : streamer_.residentChunks()) {
        const std::vector<scene::Entity>* ents = streamer_.entitiesOf(c);
        if (!ents) continue;
        for (const scene::Entity e : *ents) {
            owned_.push_back(e);
            ownedSet_.insert(e);
        }
    }
}

StreamStats ChunkWorld::update(scene::World& w, const Vec3& viewerPosCm, const Vec3& viewerVelCmPerSec,
                               f32 dt, std::vector<i32>* freedBodies) {
    return update(w, std::vector<StreamSource>{StreamSource{viewerPosCm, viewerVelCmPerSec}}, dt, freedBodies);
}

StreamStats ChunkWorld::update(scene::World& w, const std::vector<StreamSource>& sources,
                               f32 dt, std::vector<i32>* freedBodies) {
    (void)dt;   // see the header comment: the streamer's own prediction runs on leadSeconds, not dt.
    streamer_.setSources(sources);
    const StreamStats stats = streamer_.update(w, bodies_, freedBodies);
    // ChunkStreamer::update only QUEUES destroys (scene::World::destroy defers to the next flush --
    // World.hpp:27-28); every test and caller of the streamer itself flushes immediately after update
    // for exactly that reason. ChunkWorld's whole point is that a host does not have to know that --
    // "say where the viewer is" has to include making this frame's evictions actually take effect,
    // or `owns()`/`streamedEntities()` below would answer questions about entities the scene has
    // already forgotten belong to it, while `w.valid()` still says they exist.
    w.flush();
    // ONLY WHEN THE RESIDENT SET ACTUALLY CHANGED. rebuildOwned() clears and refills a vector and a
    // hash set covering EVERY streamed entity, and it used to run unconditionally on every update --
    // so a designer standing perfectly still paid a full O(residentChunks + residentEntities) rebuild
    // every frame, for every field. On the ElectricDreams level that is three ChunkWorlds over 6,370
    // entities: about 19,000 vector pushes and hash inserts per frame to reproduce, exactly, the
    // answer it produced the frame before.
    //
    // loadedThisUpdate/evictedThisUpdate are the complete set of ways the resident set can change --
    // both counters are incremented in ChunkStreamer::update's own load and evict loops, and nothing
    // else adds or removes a chunk. A failed load still counts as loaded (it inserts an empty
    // Resident), so it is covered too.
    if (stats.loadedThisUpdate || stats.evictedThisUpdate) rebuildOwned();
    return stats;
}

void ChunkWorld::shutdown(scene::World& w, std::vector<i32>& freedBodies) {
    streamer_.unloadAll(w, bodies_, freedBodies);
    w.flush();   // same reason as update(): destroy() only queues, flush() is what retires it.
    owned_.clear();
    ownedSet_.clear();
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
