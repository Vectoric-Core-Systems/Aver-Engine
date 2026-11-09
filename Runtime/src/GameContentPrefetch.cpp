#include "GameContentInternal.hpp"

namespace aver::game {

#if AVER_MODULE_SCENE

// Two workers read .ocmesh files and collision caches; uploads stay on the main thread.
struct GameContent::MeshPrefetch {
    struct Job {
        std::string path, rel, projectDir, binariesDir, contentDir;
        bool collisionBuilt = false;   // made on the worker (not read from the disk cache)
        fmt::OcMeshData md;
        MeshCpu cpu;
        bool meshOk = false;
        std::string why;
        std::shared_ptr<rhi::IMeshStager> stager;   // null: the main thread creates the GPU mesh
        std::unique_ptr<CollisionMesh> collision;
        i32 shape = 0;   // released here unless taken
        std::atomic<bool> done{false};
        u64 order = 0;
        u64 touchedTick = 0;   // prefetchTick() count when last asked for
        f32 priority = 0.0f;   // guarded by MeshPrefetch::m while queued
        ~Job() {
#  if AVER_MODULE_PHYSICS
            if (shape) aver_phys_release_mesh_shape(shape);
#  endif
        }
    };
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    bool stop = false;
    std::unordered_map<u64, std::shared_ptr<Job>> jobs;   // main thread only
    // Dropped reads are freed off the main thread: their staged upload buffers were up to 240 ms of a tick.
    std::vector<std::shared_ptr<Job>> dead;
    std::future<void> reaper;
    void drop(u64 id) {
        if (const auto it = jobs.find(id); it != jobs.end()) { dead.push_back(std::move(it->second)); jobs.erase(it); }
    }
    void reap() {
        if (dead.empty() || (reaper.valid() && reaper.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
        reaper = std::async(std::launch::async, [d = std::move(dead)]() mutable { d.clear(); });
        dead.clear();
    }
    u64 nextOrder = 0;
    u64 tick = 0;
    std::mutex slotMutex;
    std::unordered_map<std::string, bool> slotCollidesMemo;   // material slot -> collides (workers)
    std::unordered_map<std::string, i32> slotSurfaceMemo;     // material slot -> physical surface id (workers)
#  if AVER_MODULE_PBR
    std::unordered_map<std::string, std::vector<std::pair<std::string, assets::TextureUsage>>> slotTexturesMemo;

    std::vector<std::pair<std::string, assets::TextureUsage>> slotTextures(const Job& j, const std::string& slot) {
        {
            std::lock_guard<std::mutex> l(slotMutex);
            if (const auto it = slotTexturesMemo.find(slot); it != slotTexturesMemo.end()) return it->second;
        }
        auto t = slotTexturesFromFiles(j.binariesDir, j.contentDir, slot);
        if (t.empty()) return t;   // not remembered: the material may not be written yet
        std::lock_guard<std::mutex> l(slotMutex);
        return slotTexturesMemo.emplace(slot, std::move(t)).first->second;
    }
#  endif

    bool slotCollides(const Job& j, const std::string& slot) {
#  if AVER_MODULE_PBR
        {
            std::lock_guard<std::mutex> l(slotMutex);
            if (const auto it = slotCollidesMemo.find(slot); it != slotCollidesMemo.end()) return it->second;
        }
        const bool c = slotCollidesFromFiles(j.binariesDir, j.contentDir, slot);
        std::lock_guard<std::mutex> l(slotMutex);
        slotCollidesMemo.emplace(slot, c);
        return c;
#  else
        (void)j; (void)slot;
        return true;
#  endif
    }

    i32 slotSurface(const Job& j, const std::string& slot) {
#  if AVER_MODULE_PBR
        {
            std::lock_guard<std::mutex> l(slotMutex);
            if (const auto it = slotSurfaceMemo.find(slot); it != slotSurfaceMemo.end()) return it->second;
        }
        const i32 surface = slotSurfaceFromFiles(j.binariesDir, j.contentDir, slot);
        std::lock_guard<std::mutex> l(slotMutex);
        slotSurfaceMemo.emplace(slot, surface);
        return surface;
#  else
        (void)j; (void)slot;
        return 0;
#  endif
    }

    ~MeshPrefetch() {
        { std::lock_guard<std::mutex> l(m); stop = true; }
        cv.notify_all();
        for (std::thread& t : workers) t.join();
    }
    void push(std::shared_ptr<Job> j) {
        if (workers.empty())
            for (int i = 0; i < 3; ++i) workers.emplace_back([this] { run(); });
        { std::lock_guard<std::mutex> l(m); queue.push_back(std::move(j)); }
        cv.notify_one();
    }
    void run() {
        for (;;) {
            std::shared_ptr<Job> j;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [this] { return stop || !queue.empty(); });
                if (stop) return;
                // Nearest first: the queue holds what the streamer asked for over many ticks.
                auto best = queue.begin();
                for (auto it = queue.begin(); it != queue.end(); ++it)
                    if ((*it)->priority < (*best)->priority) best = it;
                j = std::move(*best);
                queue.erase(best);
            }
            j->meshOk = fmt::loadOcMesh(j->path, j->md, &j->why);
            if (j->meshOk) prepareMeshCpu(j->md, j->rel, j->cpu);
            // The GPU buffers too: allocating and filling a large mesh's was 25-75 ms on the main thread.
            if (j->meshOk && j->stager) {
                j->cpu.staged = j->stager->stage(j->cpu.verts.data(), static_cast<u32>(j->cpu.verts.size()),
                                                 j->md.indices.data(), static_cast<u32>(j->md.indices.size()));
                for (PartCpu& p : j->cpu.parts)
                    p.staged = j->stager->stageIndices(p.indices.data(), static_cast<u32>(p.indices.size()));
                for (const fmt::OcMeshLod& lod : j->md.coarserLods)
                    j->cpu.lodStaged.push_back(j->stager->stageIndices(lod.indices.data(),
                                                                       static_cast<u32>(lod.indices.size())));
            }
            if (j->meshOk && !j->projectDir.empty()) {
                std::error_code ec;
                const u64 size = static_cast<u64>(std::filesystem::file_size(j->path, ec));
                const i64 mtime = ec ? 0 : static_cast<i64>(
                    std::filesystem::last_write_time(j->path, ec).time_since_epoch().count());
                if (!ec) {
                    const std::string cachePath = collisionCachePathFor(j->projectDir, j->path, size, mtime);
                    auto c = std::make_unique<CollisionMesh>();
                    // What each material slot says (collides, physical surface): the build's input, and the check a cached
                    // entry must still pass.
                    SlotQuery slots;
                    slots.collides = [this, &j](const std::string& slot) { return slotCollides(*j, slot); };
                    slots.surface = [this, &j](const std::string& slot) { return slotSurface(*j, slot); };
                    if (!cachePath.empty() && readCollisionCacheFile(cachePath, j->path, size, mtime, *c, slots)) {
                        j->collision = std::move(c);
                        j->collision->shapeSource = shapeSourceKey(j->path, size, mtime, *j->collision);
                    } else {
                        // First visit: build it here and fill the disk cache.
                        j->collision = buildCollisionMesh(j->md, j->path, slots);
                        if (j->collision && !cachePath.empty()) {
                            std::string why;
                            if (!writeCollisionCacheFile(cachePath, j->path, size, mtime, *j->collision, &why))
                                AVER_WARN("[Collision] {}: could not write the disk cache: {}", j->path, why);
                        }
                        j->collisionBuilt = true;
                        if (j->collision) j->collision->shapeSource = shapeSourceKey(j->path, size, mtime, *j->collision);
                    }
                }
            }
#  if AVER_MODULE_PHYSICS
            // The mesh's shared collision shape (its BVH build is the costly part of a placement's body).
            if (j->collision && j->collision->indices.size() >= 3 && aver_phys_ready()) {
                const CollisionMesh& cm = *j->collision;
                // A mesh with physical surfaces becomes a shape with a palette, so a ray on any placement reports the slot's.
                j->shape = !cm.palette.empty() && cm.triPalette.size() == cm.indices.size() / 3
                    ? aver_phys_shape_mesh(cm.positions.data(), static_cast<i32>(cm.positions.size() / 3),
                                           reinterpret_cast<const i32*>(cm.indices.data()), static_cast<i32>(cm.indices.size()),
                                           cm.triPalette.data(), cm.palette.data(), static_cast<i32>(cm.palette.size()),
                                           nullptr, nullptr, 0)
                    : aver::world::createMeshShapeFromSource(cm.positions.data(), static_cast<i32>(cm.positions.size() / 3),
                                                             reinterpret_cast<const i32*>(cm.indices.data()), static_cast<i32>(cm.indices.size()),
                                                             aver::world::physicsShapeCacheDir(j->projectDir), cm.shapeSource);
            }
#  endif
#  if AVER_MODULE_PBR
            // Its materials' textures, so binding them on the main thread is a GPU copy, not a disk read.
            if (j->meshOk)
                for (const std::string& slot : j->md.materialSlots)
                    for (const auto& [path, usage] : slotTextures(*j, slot)) assets::prefetchTexture(path, usage);
#  endif
            j->done.store(true, std::memory_order_release);
        }
    }
};

void GameContent::prefetchMesh(u64 id, f32 priority) {
    if (!lazyRel_.count(id) || sceneMeshes_.count(id)) return;
    const auto pit = contentIndex_.find(id);
    if (pit == contentIndex_.end()) return;
    if (!prefetch_) prefetch_ = std::make_shared<MeshPrefetch>();
    MeshPrefetch& pf = *prefetch_;
    if (const auto it = pf.jobs.find(id); it != pf.jobs.end()) {
        it->second->order = ++pf.nextOrder;
        it->second->touchedTick = pf.tick;
        std::lock_guard<std::mutex> l(pf.m);
        it->second->priority = priority;
        return;
    }
    // Finished reads the viewer turned away from are dropped, oldest request first -- never one asked
    // for in the last two streaming ticks (a streamer may be about to take it).
    constexpr usize kMaxJobs = 1024;
    if (pf.jobs.size() >= kMaxJobs) {
        std::vector<std::pair<u64, u64>> finished;
        for (const auto& [jid, j] : pf.jobs)
            if (j->done.load(std::memory_order_acquire) && j->touchedTick + 2 < pf.tick)
                finished.emplace_back(j->order, jid);
        std::sort(finished.begin(), finished.end());
        for (usize i = 0; i < finished.size() && pf.jobs.size() > kMaxJobs * 3 / 4; ++i) pf.drop(finished[i].second);
        if (pf.jobs.size() >= kMaxJobs) {
            // Still full: finished reads farther than this request give way, farthest first, down to 7/8 of the
            // pool. A streamer loads a root only once ALL its meshes are read, so a pool full of finished
            // reads for half-read sets (a jump into dense content asks for many at once) would otherwise refuse
            // every remaining mesh forever and nothing would load again.
            std::lock_guard<std::mutex> l(pf.m);
            std::vector<std::pair<f32, u64>> far;
            for (const auto& [jid, j] : pf.jobs)
                if (j->done.load(std::memory_order_acquire) && j->priority > priority) far.emplace_back(j->priority, jid);
            std::sort(far.begin(), far.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (usize i = 0; i < far.size() && pf.jobs.size() > kMaxJobs * 7 / 8; ++i) pf.drop(far[i].second);
            if (pf.jobs.size() >= kMaxJobs) {
                // Then queued reads: the farthest gives way to a nearer one.
                auto q = pf.queue.end();
                for (auto it = pf.queue.begin(); it != pf.queue.end(); ++it)
                    if (q == pf.queue.end() || (*it)->priority > (*q)->priority) q = it;
                if (q == pf.queue.end() || (*q)->priority <= priority) return;
                for (auto it = pf.jobs.begin(); it != pf.jobs.end(); ++it)
                    if (it->second == *q) { pf.drop(it->first); break; }
                pf.queue.erase(q);
            }
        }
    }
    auto j = std::make_shared<MeshPrefetch::Job>();
    j->path = pit->second;
    j->rel = lazyRel_[id];
    j->projectDir = project_.dir;
    j->binariesDir = project_.binariesDir();
    j->contentDir = project_.contentDir();
    j->stager = meshStager_;
    j->order = ++pf.nextOrder;
    j->touchedTick = pf.tick;
    j->priority = priority;
    pf.jobs.emplace(id, j);
    pf.push(std::move(j));
}

void GameContent::prefetchTick() {
    HitchMarks hm("content tick", 0.25);
    if (++streamTick_ % 60 == 0) {
#  if AVER_MODULE_PBR
        releaseIdleMaterials();
        hm.mark("materials");
        assets::trimPrefetchedTextures();
        hm.mark("textures");
        if (hitchVerbose() && streamTick_ % 120 == 0)
            AVER_INFO("[Stream] held: {} mesh read(s), textures {} MB; {} material(s), {} mesh(es) loaded",
                      prefetch_ ? prefetch_->jobs.size() : 0, assets::prefetchedTextureBytes() >> 20,
                      materialAssets_.size(), sceneMeshes_.size());
#  endif
    }
    if (!prefetch_) return;
    MeshPrefetch& pf = *prefetch_;
    ++pf.tick;
    pf.reap();
    // Distance-ranked requests (priority > 0) nobody asked for in ~2 s are dropped: a queued one would keep
    // its old, now wrong distance and be read before what the viewer needs after a jump, and a finished one
    // would hold a pool slot forever. (Foliage re-asks on a timer, so not after two ticks.) Unranked ones
    // (streamers re-ask every tick while they want them) after ~10 s, so a cell flown past frees its reads.
    constexpr u64 kStaleTicks = 120, kUnrankedStaleTicks = 600;
    std::unordered_set<const MeshPrefetch::Job*> stale;
    for (const auto& [id, j] : pf.jobs)
        if (j->touchedTick + (j->priority > 0.0f ? kStaleTicks : kUnrankedStaleTicks) < pf.tick) stale.insert(j.get());
    if (stale.empty()) return;
    std::lock_guard<std::mutex> l(pf.m);
    std::unordered_set<const MeshPrefetch::Job*> queued;
    for (const auto& j : pf.queue) queued.insert(j.get());
    for (auto it = pf.jobs.begin(); it != pf.jobs.end();) {
        const MeshPrefetch::Job* j = it->second.get();   // one being read stays until it finishes
        if (stale.count(j) && (queued.count(j) || j->done.load(std::memory_order_acquire))) {
            pf.dead.push_back(std::move(it->second));
            it = pf.jobs.erase(it);
        } else ++it;
    }
    pf.queue.erase(std::remove_if(pf.queue.begin(), pf.queue.end(),
                                  [&](const std::shared_ptr<MeshPrefetch::Job>& j) { return stale.count(j.get()) != 0; }),
                   pf.queue.end());
    pf.reap();
    hm.mark("reads");
}

bool GameContent::meshReady(u64 id) const {
    if (!lazyRel_.count(id) || sceneMeshes_.count(id)) return true;
    if (!prefetch_) return false;
    const auto it = prefetch_->jobs.find(id);
    return it != prefetch_->jobs.end() && it->second->done.load(std::memory_order_acquire);
}

int GameContent::takePrefetch(u64 id, fmt::OcMeshData& md, MeshCpu& cpu, std::unique_ptr<CollisionMesh>& collision,
                              i32& shape, std::string& why) {
    if (!prefetch_) return -1;
    const auto it = prefetch_->jobs.find(id);
    if (it == prefetch_->jobs.end()) return -1;
    const std::shared_ptr<MeshPrefetch::Job> j = it->second;
    prefetch_->jobs.erase(it);   // a read still running finishes into its own job and is dropped
    if (!j->done.load(std::memory_order_acquire)) return -2;   // still reading: the caller reads it itself
    if (!j->meshOk) { why = std::move(j->why); return 0; }
    md = std::move(j->md);
    cpu = std::move(j->cpu);
    collision = std::move(j->collision);
    shape = j->shape;
    j->shape = 0;
    return 1;
}

i32 GameContent::takeMeshShape(u64 id) {
    const auto it = readyShapes_.find(id);
    if (it == readyShapes_.end()) return 0;
    const i32 h = it->second;
    readyShapes_.erase(it);
    return h;
}

void GameContent::dropPrefetches() {
    if (prefetch_) prefetch_->jobs.clear();
#  if AVER_MODULE_PHYSICS
    for (const auto& [mesh, shape] : readyShapes_) aver_phys_release_mesh_shape(shape);
#  endif
    readyShapes_.clear();
}

#endif // AVER_MODULE_SCENE

} // namespace aver::game
