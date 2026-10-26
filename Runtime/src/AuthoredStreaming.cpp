#include "aver/game/AuthoredStreaming.hpp"

#if AVER_MODULE_SCENE

#  include "aver/core/HitchMarks.hpp"
#  include "aver/core/Log.hpp"
#  include "aver/game/GameContent.hpp"
#  include "aver/world/ChunkCook.hpp"
#  include "aver/world/ChunkPayloadExtras.hpp"
#  include "aver/world/LevelTransform.hpp"

#  include <algorithm>
#  include <cmath>
#  include <filesystem>

namespace aver::game {
namespace {

// A chunk whose meshes are not read after this many prepare() calls loads anyway (and reads them itself): the progress guarantee
// LevelStreaming has, since the prefetch pool can saturate with partial sets after a teleport.
constexpr u32 kMaxPrepareWaits = 90;
// A move this far in one tick is a teleport, not a fast run: the velocity history is dropped.
constexpr f32 kTeleportCm = 100000.0f;
constexpr f32 kMaxTickSeconds = 0.25f;

// A payload as the placement records the flat path would have instantiated. Roots are f64 ABSOLUTE (chunk origin in i64 plus the
// chunk-local f32, which is exact), children parent-relative; instantiate() then subtracts the floating origin in f64 and narrows
// once, so a streamed chunk lands where the flat path would have put it.
bool toRecords(const world::ChunkPayload& p, std::vector<fmt::OcWorldPlacement>& recs, std::vector<u64>& plids,
               std::vector<u64>& meshes, std::string* why) {
    recs.clear();
    plids.clear();
    meshes.clear();
    recs.reserve(p.entities.size());
    plids.reserve(p.entities.size());
    for (usize i = 0; i < p.entities.size(); ++i) {
        const world::PayloadEntity& e = p.entities[i];
        world::PlacementExtras x;
        std::string err;
        if (!world::getPlacementExtras(e, x, &err)) {
            if (why) *why = err;
            return false;
        }
        fmt::OcWorldPlacement r;
        r.asset = e.name;
        r.objectId = e.hasMesh ? e.mesh : e.objectId;
        r.material = e.material;
        if (e.parent < 0) {
            r.x = static_cast<f64>(world::chunkOriginCmAxis(p.coord.x, p.chunkSizeCm)) + static_cast<f64>(e.local.position.x);
            r.y = static_cast<f64>(world::chunkOriginCmAxis(p.coord.y, p.chunkSizeCm)) + static_cast<f64>(e.local.position.y);
            r.z = static_cast<f64>(world::chunkOriginCmAxis(p.coord.z, p.chunkSizeCm)) + static_cast<f64>(e.local.position.z);
        } else {
            r.x = e.local.position.x; r.y = e.local.position.y; r.z = e.local.position.z;
        }
        if (x.hasEuler) {
            r.roll = x.euler[0]; r.pitch = x.euler[1]; r.yaw = x.euler[2];
        } else if (e.local.rotation.x != 0.0f || e.local.rotation.y != 0.0f || e.local.rotation.z != 0.0f) {
            // A payload not written by the cook (no EULR): the quaternion is all there is.
            const Vec3 d = world::eulerDegFromQuat(e.local.rotation);
            r.roll = d.x; r.pitch = d.y; r.yaw = d.z;
        }
        r.sx = e.local.scale.x; r.sy = e.local.scale.y; r.sz = e.local.scale.z;
        r.visible = !e.hasMesh || (e.meshFlags & 1u) != 0;
        r.collide = !x.hasCollide || x.collide;
        r.snapToGround = x.snapToGround;
        if (x.hasAnim) {
            r.animClip = x.animClip;
            r.animSpeed = x.animSpeed;
            r.animTime = x.animTime;
            r.animOnce = x.animOnce;
        }
        r.components = std::move(x.components);
        if (x.hasLabel) r.name = x.label;
        r.parent = e.parent;
        r.placementId = x.placementId;
        if (e.hasMesh && std::find(meshes.begin(), meshes.end(), r.objectId) == meshes.end()) meshes.push_back(r.objectId);
        plids.push_back(x.placementId);
        recs.push_back(std::move(r));
    }
    return true;
}

} // namespace

// The authored host: ChunkStreamer's seam to the scene for a cooked level.
class AuthoredStreaming::Host final : public world::IChunkHost {
public:
    explicit Host(AuthoredStreaming& o) : o_(o) {}

    world::ChunkPrepare prepare(const world::ChunkPayload& p) override {
        if (p.entities.empty()) return world::ChunkPrepare::Ready;
        Prepared& pr = o_.prepared_[p.coord];
        pr.stamp = o_.tickStamp_;
        if (pr.recs.empty()) {
            std::string err;
            if (!toRecords(p, pr.recs, pr.plids, pr.meshes, &err)) {
                AVER_WARN("[Authored] chunk ({},{},{}) could not be decoded into placements: {}", p.coord.x, p.coord.y, p.coord.z, err);
                o_.prepared_.erase(p.coord);
                return world::ChunkPrepare::Failed;
            }
        }
        // The meshes first: a chunk is made only once every mesh it shows is read, so a building never appears without its walls.
        bool ready = true;
        const f32 priority = o_.priorityOf(p.coord);
        for (const u64 id : pr.meshes) {
            if (o_.loader_.ready(id)) continue;
            ready = false;
            o_.loader_.prefetch(id, priority);
        }
        if (ready) return world::ChunkPrepare::Ready;
        if (++pr.waits > kMaxPrepareWaits) {
            if (pr.waits == kMaxPrepareWaits + 1)
                AVER_WARN("[Authored] chunk ({},{},{}) waited {} updates for its meshes; loading it with synchronous reads",
                          p.coord.x, p.coord.y, p.coord.z, kMaxPrepareWaits);
            return world::ChunkPrepare::Ready;
        }
        return world::ChunkPrepare::Pending;
    }

    std::vector<scene::Entity> restore(const world::ChunkPayload& p, scene::World&, const world::RestoreOptions& opt) override {
        std::vector<scene::Entity> made;
        if (p.entities.empty()) return made;
        auto it = o_.prepared_.find(p.coord);
        if (it == o_.prepared_.end() || it->second.recs.empty()) {
            // restore without a prepare (a host driven by hand): build the records now.
            if (prepare(p) == world::ChunkPrepare::Failed) return made;
            it = o_.prepared_.find(p.coord);
            if (it == o_.prepared_.end()) return made;
        }
        Prepared& pr = it->second;
        HitchMarks hm("chunk load", 0.25);
        const u32 item = o_.nextItem_++;
        std::vector<ItemLoader::Entry> entries;
        entries.reserve(pr.recs.size());
        for (usize i = 0; i < pr.recs.size(); ++i) {
            ItemLoader::Entry en;
            en.rec = &pr.recs[i];
            en.tag = static_cast<u32>(i);
            en.item = item;
            en.parent = pr.recs[i].parent;   // the payload's own indices: every entity of the chunk is an entry
            entries.push_back(en);
        }
        const std::vector<ItemLoader::Loaded> loaded = o_.loader_.load(entries, opt.origin, &hm);
        o_.loader_.applyComponents(loaded);
        hm.mark("components");
        made.reserve(loaded.size());
        std::vector<u64> ids;
        std::vector<i32> bodies;
        for (const ItemLoader::Loaded& l : loaded) {
            made.push_back(l.entity);
            ids.push_back(l.tag < pr.plids.size() ? pr.plids[l.tag] : 0);
            bodies.push_back(l.body);
        }
        if (!loaded.empty()) o_.itemOf_[p.coord] = item;
        if (o_.hooks_.afterLoad && !made.empty()) o_.hooks_.afterLoad(made, ids, bodies);
        o_.prepared_.erase(it);
        hm.mark("hook");
        return made;
    }

    u32 evict(const world::ChunkCoord& c, const std::vector<scene::Entity>& entities, scene::World& w, world::BodyRegistry&,
              std::vector<i32>&) override {
        const auto it = o_.itemOf_.find(c);
        o_.prepared_.erase(c);
        if (it == o_.itemOf_.end()) {
            // A chunk the loader never saw (nothing made): any stray entity still goes.
            u32 n = 0;
            for (const scene::Entity e : entities) if (w.valid(e)) { w.destroy(e); ++n; }
            return n;
        }
        const u32 n = static_cast<u32>(entities.size());
        HitchMarks hm("chunk evict", 0.25);
        o_.loader_.evict(w, it->second, &hm);   // bodies, entities (children first), meshes, kinematic bodies
        o_.itemOf_.erase(it);
        return n;
    }

private:
    AuthoredStreaming& o_;
};

AuthoredStreaming::AuthoredStreaming() = default;
AuthoredStreaming::~AuthoredStreaming() = default;

AuthoredStreaming::Choice AuthoredStreaming::resolve(const std::string& projectDir, const std::string& contentDir,
                                                      const std::string& chunksPath, const std::string& levelAsset,
                                                      const fmt::OcWorldData& source) {
    Choice c;
    if (chunksPath.empty()) return c;
    namespace fs = std::filesystem;
    fs::path dir(chunksPath);
    if (dir.is_relative()) dir = fs::path(projectDir) / dir;
    c.dir = dir.string();

    world::AuthoredChunkSource probe;
    std::string err;
    if (!probe.open(c.dir, &err)) {
        c.why = "no usable cooked set (" + err + ")";
        return c;
    }
    world::CookInput in;
    in.levelAsset = levelAsset;
    in.levelStem = dir.filename().string();
    in.contentDir = contentDir;
    in.projectDir = projectDir;
    world::CookOptions opt;
    opt.chunkSizeCm = probe.chunkSizeCm();
    if (world::cookSourceHash(source, in, opt) != probe.sourceHash()) {
        c.stale = true;
        c.why = "the cooked set is stale: the level changed since it was cooked (World Settings > Level Streaming > Cook Chunks)";
        return c;
    }
    if (!fmt::loadOcworld(probe.levelPath(), c.slim, &err)) {
        c.why = "the cooked set's level.ocworld is unreadable (" + err + ")";
        return c;
    }
    // What is left in the slim level is level-global: loaded whole by the flat path, never streamed by records.
    c.slim.stream.enabled = false;
    c.use = true;
    return c;
}

bool AuthoredStreaming::begin(const std::string& cookDir, GameContent& content, rhi::IDevice& device,
                              const world::InstantiateOptions& opt, const fmt::OcStreamSettings& stream, Hooks hooks,
                              std::string* why) {
    if (active_) end(scene::World::instance());
    world::AuthoredChunkSource probe;
    if (!probe.open(cookDir, why)) return false;

    hooks_ = std::move(hooks);
    loader_.begin(content, device, opt);
    host_ = std::make_unique<Host>(*this);
    const world::StreamSettings ss = world::authoredStreamSettings(probe.chunkSizeCm(), stream.loadCm, stream.evictCm);
    if (!world_.openAuthored(cookDir, ss, host_.get(), why)) {
        loader_.end();
        host_.reset();
        return false;
    }
    prepared_.clear();
    itemOf_.clear();
    nextItem_ = 1;
    tickStamp_ = 0;
    haveLastPos_ = false;
    stats_ = world::StreamStats{};
    active_ = true;
    AVER_INFO("[Authored] streaming the cooked set '{}': {} region(s), {} wide item(s), chunk {} cm, load {} / evict {} chunks (+{} pad)",
              cookDir, world_.authoredSource().index().regions.size(), world_.authoredSource().wide().size(),
              ss.chunkSizeCm, ss.loadRadius, ss.evictRadius, ss.padChunks);
    return true;
}

void AuthoredStreaming::end(scene::World& world) {
    if (!active_) return;
    std::vector<i32> freed;
    world_.shutdown(world, freed);   // the host evicts every resident chunk through the loader
    loader_.end();
    prepared_.clear();
    itemOf_.clear();
    host_.reset();
    active_ = false;
    haveLastPos_ = false;
}

f32 AuthoredStreaming::priorityOf(const world::ChunkCoord& c) const {
    world::WorldPos wp;
    wp.chunk = c;
    wp.local = Vec3{800.0f, 800.0f, 800.0f};
    const Vec3 centre = world::localOf(wp, origin_);
    f32 best = 3.0e38f;
    for (const Vec3& v : viewers_) {
        const f32 dx = centre.x - v.x, dy = centre.y - v.y;
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
    }
    return best < 3.0e38f ? best : 0.0f;
}

void AuthoredStreaming::tick(scene::World& world, const std::vector<Vec3>& viewers) {
    if (!active_ || viewers.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    f32 dt = 0.0f;
    if (haveLastPos_) dt = std::min(std::chrono::duration<f32>(now - lastTick_).count(), kMaxTickSeconds);
    lastTick_ = now;

    Vec3 vel{0.0f, 0.0f, 0.0f};
    if (haveLastPos_ && dt > 1e-4f) {
        const Vec3 d = viewers[0] - lastPos_;
        if (d.size() <= kTeleportCm) vel = d * (1.0f / dt);
    }
    lastPos_ = viewers[0];
    haveLastPos_ = true;

    viewers_ = viewers;
    std::vector<world::StreamSource> sources;
    sources.reserve(viewers.size());
    sources.push_back(world::StreamSource{viewers[0], vel});
    for (usize i = 1; i < viewers.size(); ++i) sources.push_back(world::StreamSource{viewers[i], Vec3{0.0f, 0.0f, 0.0f}});

    ++tickStamp_;
    loader_.prefetchTick();   // ages the mesh-read queue once a tick
    std::vector<i32> freed;
    stats_ = world_.update(world, sources, dt, &freed, origin_);
    if (stats_.evictedThisUpdate) loader_.releaseUnusedShapes();

    // A decoded chunk nobody asked about this tick is for a chunk that is no longer wanted.
    for (auto it = prepared_.begin(); it != prepared_.end();)
        it = (it->second.stamp != tickStamp_ && itemOf_.find(it->first) == itemOf_.end()) ? prepared_.erase(it) : std::next(it);
}

void AuthoredStreaming::pin(const world::ChunkCoord& c, bool on) {
    if (active_) world_.streamer().pin(c, on);
}

void AuthoredStreaming::onOriginShift(const world::ShiftEvent& ev) {
    origin_ = ev.newOrigin;
    // Plain f32 subtraction per axis, the same as every other holder of a scene-frame position.
    lastPos_ = Vec3{lastPos_.x - ev.deltaF.x, lastPos_.y - ev.deltaF.y, lastPos_.z - ev.deltaF.z};
}

bool AuthoredStreaming::owns(scene::Entity e) const {
    return active_ && world_.owns(e);
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
