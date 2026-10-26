// CMotionMatcher and the system that runs it. See MotionMatchSystem.hpp.
#include "aver/anim/MotionMatchSystem.hpp"

#include "aver/anim/AnimGraphSystem.hpp"
#include "aver/anim/MotionAsset.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>

namespace aver::anim {

namespace {

u32 g_type = 0;

// Everything a worker needs, by value: the main thread's caches (clips_, skeletons_) are not thread-safe and
// AnimSystem::clear() may free them under a running build, so the worker owns copies.
struct LoadTask {
    MotionAsset asset;
    fmt::OcSkeleton skel;
    std::string skelPath, sidecar;
    std::vector<std::string> clipRefs, clipPaths;   // distinct clip references in entry order, and their files
    std::map<std::string, fmt::OcAnimation> clips;
    bool bake = true;
};

std::vector<u8> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

u32 MotionMatchSystem::registerComponents(scene::World& world) {
    static std::array<std::string, kMotionTagSlots * 2> names = [] {
        std::array<std::string, kMotionTagSlots * 2> n;
        for (u32 i = 0; i < kMotionTagSlots; ++i) {
            n[i] = "requireTag" + std::to_string(i);
            n[kMotionTagSlots + i] = "excludeTag" + std::to_string(i);
        }
        return n;
    }();

    auto b = world.registerComponent<CMotionMatcher>("CMotionMatcher");
    b.field("database", scene::FieldKind::I64, static_cast<u16>(offsetof(CMotionMatcher, database)))
        .field("flags", scene::FieldKind::I32, static_cast<u16>(offsetof(CMotionMatcher, flags)))
        .field("playRate", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, playRate)))
        .field("searchInterval", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, searchInterval)))
        .field("desiredVelX", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, desiredVelX)))
        .field("desiredVelY", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, desiredVelY)))
        .field("desiredYaw", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, desiredYaw)))
        .field("velHalfLife", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, velHalfLife)))
        .field("turnHalfLife", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, turnHalfLife)));
    for (u32 i = 0; i < kMotionTagSlots; ++i)
        b.field(names[i].c_str(), scene::FieldKind::I64,
                static_cast<u16>(offsetof(CMotionMatcher, requireTag) + i * sizeof(u64)));
    for (u32 i = 0; i < kMotionTagSlots; ++i)
        b.field(names[kMotionTagSlots + i].c_str(), scene::FieldKind::I64,
                static_cast<u16>(offsetof(CMotionMatcher, excludeTag) + i * sizeof(u64)));
    b.field("clip", scene::FieldKind::I64, static_cast<u16>(offsetof(CMotionMatcher, clip)), 0, true)
        .field("clipTime", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, clipTime)), 0, true)
        .field("cost", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, cost)), 0, true)
        .field("switches", scene::FieldKind::I32, static_cast<u16>(offsetof(CMotionMatcher, switches)), 0, true)
        .field("row", scene::FieldKind::I32, static_cast<u16>(offsetof(CMotionMatcher, row)), 0, true)
        .field("sinceSwitch", scene::FieldKind::F32, static_cast<u16>(offsetof(CMotionMatcher, sinceSwitch)), 0, true)
        .field("inputSource", scene::FieldKind::I32, static_cast<u16>(offsetof(CMotionMatcher, inputSource)));
    AVER_ASSERTM(b.verify(sizeof(CMotionMatcher)), "CMotionMatcher");
    g_type = b.typeId();
    return g_type;
}

CMotionMatcher* MotionMatchSystem::attach(scene::World& world, scene::Entity e, u64 databaseAsset) {
    if (g_type == 0) return nullptr;
    auto* c = static_cast<CMotionMatcher*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    *c = CMotionMatcher{};
    c->database = databaseAsset;
    return c;
}

CMotionMatcher* MotionMatchSystem::ensure(scene::World& world, scene::Entity e) {
    if (g_type == 0) return nullptr;
    if (auto* have = world.component<CMotionMatcher>(e, g_type)) return have;
    return attach(world, e, 0);
}

void MotionMatchSystem::install(AnimSystem& anim, scene::World&) {
    anim_ = &anim;
    if (anim.poseSource() == &MotionMatchSystem::sourceThunk && anim.poseSourceUser() == this) return;   // already in
    prev_ = anim.poseSource();
    prevUser_ = anim.poseSourceUser();
    anim.setPoseSource(&MotionMatchSystem::sourceThunk, this);
}

void MotionMatchSystem::uninstall(AnimSystem& anim) {
    MotionMatchSystem& self = motionMatchSystem();
    if (anim.poseSource() == &MotionMatchSystem::sourceThunk && anim.poseSourceUser() == &self)
        anim.setPoseSource(self.prev_, self.prevUser_);
    self.prev_ = nullptr;
    self.prevUser_ = nullptr;
}

bool MotionMatchSystem::sourceThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user) {
    auto* self = static_cast<MotionMatchSystem*>(user);
    if (self->prev_ && self->prev_(e, skel, pose, self->prevUser_)) return true;   // a state machine owns the entity
    const auto it = self->runtimes_.find(e);
    if (it == self->runtimes_.end() || !it->second.standalone || !it->second.mm.bound()) return false;
    it->second.mm.evaluate(skel, pose);
    return true;
}

// ---- databases -------------------------------------------------------------------------------------

namespace {

// The worker body: the sidecar when its key matches the bytes of everything it was built from, else a full
// build (and a best-effort bake).
struct LoadOutcome {
    bool ok = false, fromSidecar = false, wrote = false;
    std::unique_ptr<PoseDatabase> db;
    MotionBuildReport report;
    std::string why;
};

LoadOutcome runLoad(LoadTask& t) {
    LoadOutcome out;
    std::vector<std::vector<u8>> clipBytes;
    for (const std::string& p : t.clipPaths) clipBytes.push_back(p.empty() ? std::vector<u8>{} : slurp(p));
    const std::vector<u8> skelBytes = t.skelPath.empty() ? std::vector<u8>{} : slurp(t.skelPath);
    const u64 key = motionSourceKey(t.asset, skelBytes, clipBytes);

    out.db = std::make_unique<PoseDatabase>();
    u64 stored = 0;
    std::string why;
    if (loadPoseDatabase(t.sidecar, *out.db, &stored, &why) && stored == key) {
        out.ok = out.fromSidecar = true;
        return out;
    }
    auto resolve = [&t](const std::string& ref) -> const fmt::OcAnimation* {
        const auto it = t.clips.find(ref);
        return it == t.clips.end() ? nullptr : &it->second;
    };
    out.db = std::make_unique<PoseDatabase>();
    if (!buildPoseDatabase(t.asset, t.skel, resolve, *out.db, &out.report, &out.why)) {
        out.db.reset();
        return out;
    }
    out.ok = true;
    if (t.bake) out.wrote = savePoseDatabase(t.sidecar, *out.db, key);
    return out;
}

}  // namespace

void MotionMatchSystem::startLoad(u64 id, Slot& slot) {
    slot.state = Slot::State::Failed;   // until a worker is running
    const std::string path = anim_ ? anim_->assetPath(id) : std::string{};
    LoadTask t;
    std::string why;
    if (path.empty() || !loadMotionAsset(path, t.asset, &why) || !t.asset.valid(&why)) {
        AVER_WARN("[MotionMatch] could not load database {}: {}", id, why.empty() ? path : why);
        return;
    }
    slot.name = t.asset.name.empty() ? path : t.asset.name;
    const u64 skelId = fnv1a64(std::string_view(t.asset.skeleton));
    const fmt::OcSkeleton* sk = anim_->skeleton(skelId);
    if (!sk) {
        AVER_WARN("[MotionMatch] database {}: skeleton '{}' did not load", slot.name, t.asset.skeleton);
        return;
    }
    t.skel = *sk;
    t.skelPath = anim_->assetPath(skelId);
    for (const MotionClipEntry& e : t.asset.clips) {
        if (std::find(t.clipRefs.begin(), t.clipRefs.end(), e.clip) != t.clipRefs.end()) continue;
        const u64 cid = fnv1a64(std::string_view(e.clip));
        const fmt::OcAnimation* c = anim_->clip(cid);   // the caches are main-thread only: copy what the worker needs
        t.clipRefs.push_back(e.clip);
        t.clipPaths.push_back(anim_->assetPath(cid));
        if (c) t.clips[e.clip] = *c;
    }
    t.sidecar = std::filesystem::path(path).replace_extension(".ocmbake").string();
    t.bake = bake_;
    slot.state = Slot::State::Loading;
    slot.fut = std::async(std::launch::async, [task = std::move(t)]() mutable {
        LoadOutcome o = runLoad(task);
        LoadResult r;
        r.ok = o.ok;
        r.fromSidecar = o.fromSidecar;
        r.wrote = o.wrote;
        r.db = std::move(o.db);
        r.report = std::move(o.report);
        r.why = std::move(o.why);
        return r;
    });
}

void MotionMatchSystem::finish(u64 id, Slot& slot) {
    LoadResult r = slot.fut.get();
    if (!r.ok || !r.db) {
        slot.state = Slot::State::Failed;
        AVER_WARN("[MotionMatch] database {} failed to build: {}", slot.name, r.why);
        return;
    }
    slot.db = std::move(r.db);
    slot.state = Slot::State::Ready;
    if (r.fromSidecar) {
        AVER_INFO("[MotionMatch] {}: {} rows from the baked sidecar", slot.name, slot.db->rows());
        return;
    }
    ++builds_;
    AVER_INFO("[MotionMatch] {}: built {} rows in {:.0f} ms{}", slot.name, r.report.rows, r.report.seconds * 1000.0,
              r.wrote ? "" : " (sidecar not written)");
    if (!r.wrote && bake_ && !bakeWarned_) {
        bakeWarned_ = true;
        AVER_INFO("[MotionMatch] could not write the .ocmbake next to {}; the database is rebuilt on every load", slot.name);
    }
    for (const std::string& w : r.report.warnings) AVER_WARN("[MotionMatch] {}: {}", slot.name, w);
    (void)id;
}

void MotionMatchSystem::poll() {
    for (auto& [id, slot] : dbs_)
        if (slot.state == Slot::State::Loading && slot.fut.valid() &&
            slot.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            finish(id, slot);
}

const PoseDatabase* MotionMatchSystem::database(u64 id) {
    if (!anim_ || id == 0) return nullptr;
    auto it = dbs_.find(id);
    if (it == dbs_.end()) {
        it = dbs_.emplace(id, Slot{}).first;
        startLoad(id, it->second);
    }
    Slot& slot = it->second;
    if (slot.state == Slot::State::Loading && slot.fut.valid() &&
        slot.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        finish(id, slot);
    return slot.state == Slot::State::Ready ? slot.db.get() : nullptr;
}

const PoseDatabase* MotionMatchSystem::databaseFor(scene::Entity e, u64 id) {
    const PoseDatabase* db = database(id);
    if (db) {
        Runtime& rt = runtimes_[e];
        if (!rt.refDb) rt.refDb = db;
    }
    return db;
}

u32 MotionMatchSystem::pendingBuilds() const {
    u32 n = 0;
    for (const auto& kv : dbs_) n += kv.second.state == Slot::State::Loading;
    return n;
}

u32 MotionMatchSystem::loadedCount() const {
    u32 n = 0;
    for (const auto& kv : dbs_) n += kv.second.state == Slot::State::Ready;
    return n;
}

void MotionMatchSystem::rebase(const scene::World& world, const Vec3& delta) {
    for (auto& [e, rt] : runtimes_)
        if (world.valid(e) && world.parent(e) == scene::kInvalidEntity) rt.traj.rebase(delta);
}
void MotionMatchSystem::clear() {
    for (auto& kv : dbs_)
        if (kv.second.fut.valid()) kv.second.fut.wait();   // a worker owns copies, but never leave one running
    runtimes_.clear();
    dbs_.clear();
    builds_ = 0;
    searchesLast_ = 0;
    epoch_ = anim_ ? anim_->contentEpoch() : 0;
}

const MotionQuery* MotionMatchSystem::query(scene::Entity e) const {
    const auto it = runtimes_.find(e);
    return (it == runtimes_.end() || !it->second.q.valid) ? nullptr : &it->second.q;
}

// ---- the tick --------------------------------------------------------------------------------------

void MotionMatchSystem::tick(scene::World& world, f32 dt) {
    if (g_type == 0 || !anim_) return;
    if (anim_->contentEpoch() != epoch_) clear();   // clip pointers died with the caches
    poll();
    ++stamp_;
    searchesLast_ = 0;

    struct Item { scene::Entity e; Runtime* rt; CMotionMatcher* c; const PoseDatabase* db; };
    std::vector<Item> ready;
    if (scene::ComponentPool* pool = world.pool(g_type)) {
        for (usize i = 0; i < pool->size(); ++i) {
            const scene::Entity e = pool->entityAt(i);
            auto* c = static_cast<CMotionMatcher*>(pool->dataAt(i));
            if (!c) continue;
            Runtime& rt = runtimes_[e];
            rt.stamp = stamp_;

            // An entity running a state machine (or about to: the instance appears on the machine's first tick) is
            // never matched standalone.
            bool graph = animGraphSystem().instance(e) != nullptr;
            if (!graph) {
                const u32 gt = world.componentId("CAnimGraph");
                const auto* g = gt ? world.component<CAnimGraph>(e, gt) : nullptr;
                graph = g && g->machine != 0;
            }
            // AnimSystem poses entities it finds an animator on, so a matched entity gets one at once: while its
            // database loads it shows the rest pose rather than nothing.
            if (!graph && c->database && !world.hasComponent(e, scene::kComponentAnimator))
                world.addComponent(e, scene::kComponentAnimator);
            const PoseDatabase* db = c->database ? database(c->database) : nullptr;
            const PoseDatabase* ref = graph ? (rt.refDb ? rt.refDb : db) : db;
            if (!graph && !db && rt.standalone) { rt.mm.unbind(); rt.standalone = false; rt.boundDb = nullptr; }

            // Observe the entity's own motion whether or not a database is ready, so the history is warm
            // by the time one is.
            const Transform& xf = world.localTransform(e);
            const MotionAsset* defs = ref ? &ref->defaults() : nullptr;
            rt.traj.observe(xf.position, yawOf(xf.rotation), dt, defs ? defs->spring.historySeconds : 1.0f);
            if (!ref) { rt.q.valid = false; continue; }

            SpringParams sp{c->velHalfLife > 0.0f ? c->velHalfLife : defs->spring.velocityHalfLife,
                            c->turnHalfLife > 0.0f ? c->turnHalfLife : defs->spring.turnHalfLife};
            f32 vx = c->desiredVelX, vy = c->desiredVelY, yaw = c->desiredYaw;
            if (c->inputSource == kMotionInputEntityVelocity) {
                const Vec3 v = rt.traj.velocity();   // measured and already smoothed
                vx = v.x; vy = v.y; yaw = rt.traj.yaw();
                c->desiredVelX = vx; c->desiredVelY = vy; c->desiredYaw = yaw;   // the inspector and scripts read what is used
            }
            rt.traj.setIntent(vx, vy, (c->flags & kMotionFaceYaw) != 0, yaw, defs->spring.faceSpeedMin);
            rt.traj.fill(sp, ref->futureTimes(), ref->pastTimes(), rt.q);
            rt.q.require = ref->tagMaskOf(c->requireTag, kMotionTagSlots);
            rt.q.exclude = ref->tagMaskOf(c->excludeTag, kMotionTagSlots);
            rt.refDb = ref;

            if (graph || !db) continue;   // a state machine owns the entity, or the database is still loading

            // Standalone: bind to the entity's skeleton.
            const auto* skm = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
            const fmt::OcSkeleton* skel = (skm && skm->skeleton) ? anim_->skeleton(skm->skeleton) : nullptr;
            if (!skel) continue;
            if (rt.boundDb != db || rt.boundSkel != skel) {
                rt.boundDb = db;
                rt.boundSkel = skel;
                // clipFor: a clip retargeted to this skeleton plays its bake (the raw clip when they match).
                const u64 skelId = skm->skeleton;
                rt.standalone = rt.mm.bind(db, skel, [this, skelId](const std::string& ref2) -> const fmt::OcAnimation* {
                    return anim_->clipFor(fnv1a64(std::string_view(ref2)), skelId);
                });
                if (!rt.standalone) AVER_WARN("[MotionMatch] database {} does not fit the skeleton of this entity; not matched", ref->defaults().name);
            }
            if (rt.standalone) ready.push_back({e, &rt, c, db});
        }
    }

    // Spread the searches: the entities that waited longest go first, and only maxSearches_ may start an
    // interval search per tick. Forced searches and a matcher with nothing playing are never held back.
    std::sort(ready.begin(), ready.end(), [](const Item& a, const Item& b) {
        return a.rt->mm.sinceSearch() > b.rt->mm.sinceSearch();
    });
    u32 searches = 0;
    for (Item& it : ready) {
        CMotionMatcher& c = *it.c;
        Runtime& rt = *it.rt;
        MotionParams p;
        p.playRate = c.playRate > 0.0f ? c.playRate : 1.0f;
        p.searchInterval = c.searchInterval;
        p.paused = (c.flags & kMotionPaused) != 0;
        p.forceSearch = (c.flags & kMotionForceSearch) != 0;

        RootSpec spec;
        const bool rootOn = anim_->rootSpec(world, it.e, kRootSrcGraph, spec);
        rt.mm.setRootMotion(rootOn, rootOn ? spec.axes : 0);

        const f32 interval = p.searchInterval > 0.0f ? p.searchInterval : it.db->defaults().search.interval;
        const bool idle = rt.mm.info().row == kNoRow;
        if (!p.forceSearch && !idle && rt.mm.sinceSearch() + dt >= interval && searches >= maxSearches_)
            p.searchInterval = 1e9f;   // over budget: this one waits for a later tick
        if (!p.paused) rt.mm.tick(dt, rt.q, p);
        searches += rt.mm.info().searched;
        anim_->addRootMotion(it.e, p.paused ? RootDelta{} : rt.mm.consumeRootDelta());

        const MotionInfo& info = rt.mm.info();
        c.flags &= ~kMotionForceSearch;
        c.clip = rt.mm.clipHash();
        c.clipTime = info.clipTime;
        c.cost = info.playingCost;
        c.switches = info.switches;
        c.row = info.row == kNoRow ? 0xFFFFFFFFu : info.row;
        c.sinceSwitch = info.sinceSwitch;
    }
    searchesLast_ = searches;

    // A runtime nothing drove this tick belongs to an entity that died or lost the component.
    for (auto it = runtimes_.begin(); it != runtimes_.end();) {
        if (it->second.stamp == stamp_) ++it;
        else it = runtimes_.erase(it);
    }
}

// ---- debug -----------------------------------------------------------------------------------------

bool MotionMatchSystem::debug(scene::Entity e, Debug& out) const {
    const auto it = runtimes_.find(e);
    if (it == runtimes_.end() || !it->second.traj.observed()) return false;
    const Runtime& rt = it->second;
    out = Debug{};
    out.origin = rt.traj.position();
    out.yaw = rt.traj.yaw();
    out.desiredVel = rt.traj.desiredVelocity();
    rt.traj.historyWorld(out.history);
    if (!rt.refDb) return true;

    const MotionAsset& d = rt.refDb->defaults();
    rt.traj.predictedWorld({d.spring.velocityHalfLife, d.spring.turnHalfLife}, rt.refDb->futureTimes(), out.predicted, out.predictedYaw);
    if (!rt.mm.bound()) return true;

    const MotionInfo& info = rt.mm.info();
    out.clip = info.range == kNoRow ? std::string{} : rt.refDb->ranges()[info.range].clip;
    out.clipTime = info.clipTime;
    out.cost = info.cost;
    out.currentCost = info.currentCost;
    out.switches = info.switches;
    for (const FeatureGroup& g : rt.refDb->groups()) {
        static const char* kinds[] = {"pos", "vel", "root velocity", "future pos", "future dir", "past pos"};
        std::string n = kinds[static_cast<int>(g.kind)];
        if (g.bone < rt.refDb->boneNames().size()) n = rt.refDb->boneNames()[g.bone] + " " + n;
        out.groupNames.push_back(n);
    }
    rt.mm.lastGroupCosts(out.groupCurrent, out.groupBest);

    // The playing row's own future trajectory, re-expressed from the entity's pose: where the database
    // thinks this motion goes, to compare with where the character wants to go.
    if (info.row != kNoRow) {
        std::vector<f32> raw(rt.refDb->dimPadded());
        rt.refDb->denormalise(rt.refDb->row(info.row), raw.data());
        for (const FeatureGroup& g : rt.refDb->groups()) {
            if (g.kind != FeatureKind::FuturePos) continue;
            const f32 c = std::cos(out.yaw), s = std::sin(out.yaw);
            for (u32 i = 0; i + 1 < g.count; i += 2) {
                const f32 x = raw[g.firstDim + i], y = raw[g.firstDim + i + 1];
                out.matched.push_back({out.origin.x + c * x - s * y, out.origin.y + s * x + c * y, out.origin.z});
            }
        }
    }
    return true;
}

MotionMatchSystem& motionMatchSystem() {
    static MotionMatchSystem s;
    return s;
}

}  // namespace aver::anim
