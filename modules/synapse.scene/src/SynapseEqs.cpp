#include "aver/synapse/SynapseEqs.hpp"

#include "aver/core/Log.hpp"
#include "aver/synapse/AiPaths.hpp"
#include "aver/synapse/SynapseAi.hpp"
#include "aver/synapse/SynapsePerception.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif

namespace aver::synapse {
namespace {

constexpr double kResultLifetimeSec = 30.0;
constexpr usize kMaxQueuedRuns = 64;
constexpr usize kMaxRetainedResults = 64;
constexpr usize kMaxDebugRuns = 8;
constexpr usize kMaxDebugItems = 256;

scene::World& world() { return scene::World::instance(); }

Vec3 worldPos(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

bool live(scene::World& w, u32 e) { return e != 0 && w.valid(static_cast<scene::Entity>(e)) && !w.destroyPending(static_cast<scene::Entity>(e)); }

} // namespace

// ---- query cache ------------------------------------------------------------------------------------

u32 EqsSystem::loadQuery(const std::string& path) {
    if (const auto it = byPath_.find(path); it != byPath_.end()) return it->second;
    EqsQuery q;
    std::string why;
    if (!loadEqsQuery(resolveAiAsset(path), q, &why)) {
        lastMessage_ = why;
        if (failedPaths_.emplace(path, true).second) AVER_WARN("[Synapse] could not load query '{}': {}", path, why);
        return 0;
    }
    lastMessage_.clear();
    const u32 id = nextQuery_++;
    q.name = path;
    queries_[id] = QueryRec{std::move(q), path};
    byPath_[path] = id;
    return id;
}

u32 EqsSystem::registerQuery(const std::string& name, const EqsQuery& q) {
    if (const auto it = byPath_.find(name); it != byPath_.end()) {
        queries_[it->second].q = q;
        queries_[it->second].q.name = name;
        return it->second;
    }
    const u32 id = nextQuery_++;
    QueryRec rec{q, name};
    rec.q.name = name;
    queries_[id] = std::move(rec);
    byPath_[name] = id;
    return id;
}

bool EqsSystem::reloadQuery(const std::string& path) {
    EqsQuery q;
    std::string why;
    if (!loadEqsQuery(resolveAiAsset(path), q, &why)) {
        lastMessage_ = why;
        AVER_WARN("[Synapse] could not reload query '{}': {}", path, why);
        return false;
    }
    lastMessage_.clear();
    failedPaths_.erase(path);
    q.name = path;
    if (const auto it = byPath_.find(path); it != byPath_.end()) {
        queries_[it->second].q = std::move(q);
        return true;
    }
    const u32 id = nextQuery_++;
    queries_[id] = QueryRec{std::move(q), path};
    byPath_[path] = id;
    return true;
}

const EqsQuery* EqsSystem::query(u32 id) const {
    const auto it = queries_.find(id);
    return it == queries_.end() ? nullptr : &it->second.q;
}

// ---- environment ------------------------------------------------------------------------------------

EqsContext EqsSystem::envContext(void* user, std::string_view name, const EqsRunParams& p) {
    auto* e = static_cast<EnvCtx*>(user);
    scene::World& w = world();
    EqsContext c;
    const auto fromEntity = [&](u32 ent) {
        if (!live(w, ent)) return;
        c.entity = ent;
        c.pos = worldPos(w, static_cast<scene::Entity>(ent));
        c.ok = true;
    };
    if (name == "querier") {
        fromEntity(p.querier);
        return c;
    }
    if (name == "seen") {
        const u32 t = e->sys->ai_ ? e->sys->ai_->perceptionType(w) : 0;
        const auto* per = t && live(w, p.querier) ? w.component<CSynapsePerception>(static_cast<scene::Entity>(p.querier), t) : nullptr;
        if (per) fromEntity(static_cast<u32>(per->lastKnownTargetEntity));
        return c;
    }
    for (const auto& d : e->contexts) {
        if (d.first != name) continue;
        Blackboard* b = e->sys->ai_ ? e->sys->ai_->boardOf(static_cast<scene::Entity>(p.querier)) : nullptr;
        const BbValue* v = b ? b->get(d.second) : nullptr;
        if (v && v->type == BbType::Entity) fromEntity(static_cast<u32>(v->i));
        else if (v && v->type == BbType::Vec3) { c.pos = v->v; c.ok = true; }
        break;
    }
    if (!c.ok && name == "target") fromEntity(p.target);
    return c;
}

bool EqsSystem::envForward(void*, u32 entity, Vec3& out) {
    scene::World& w = world();
    if (!live(w, entity)) return false;
    const Mat4& m = w.worldMatrix(static_cast<scene::Entity>(entity));
    const Vec3 f = Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.getSafeNormal();
    if (f.sizeSquared() < 0.5f) return false;
    out = f;
    return true;
}

u32 EqsSystem::envActors(void* user, const EqsGenerator& g, const Vec3&, u32, u32* out, u32 cap) {
    auto* e = static_cast<EnvCtx*>(user);
    scene::World& w = world();
    u32 n = 0;
    if (!g.className.empty()) {
        if (!e->sys->actors_) return 0;
        std::vector<i32> ids(cap);
        const i32 got = e->sys->actors_(g.className.c_str(), g.derived ? 1 : 0, ids.data(), static_cast<i32>(cap), e->sys->actorsUser_);
        for (i32 i = 0; i < got && n < cap; ++i)
            if (live(w, static_cast<u32>(ids[static_cast<usize>(i)]))) out[n++] = static_cast<u32>(ids[static_cast<usize>(i)]);
        return n;
    }
    for (u32 i = 0; i < w.count() && n < cap; ++i) {
        const scene::Entity ent = w.at(i);
        if (g.tagBits != 0) {
            const auto* t = w.component<scene::CTags>(ent, scene::kComponentTags);
            if (!t || (t->bits & g.tagBits) == 0) continue;
        } else {
            const char* nm = w.name(ent);
            if (!nm || std::string_view(nm).substr(0, g.namePrefix.size()) != g.namePrefix) continue;
        }
        out[n++] = ent;
    }
    return n;
}

u32 EqsSystem::envSlots(void* user, const EqsGenerator& g, const Vec3& centre, const EqsRunParams& p, EqsItem* out, TagList* tags, u32 cap) {
    auto* e = static_cast<EnvCtx*>(user);
    if (!e->sys->ai_) return 0;
    return e->sys->ai_->smartObjects().findSlots(world(), g, centre, p, out, tags, cap);
}

bool EqsSystem::envEntityPos(void*, u32 entity, Vec3& out) {
    scene::World& w = world();
    if (!live(w, entity)) return false;
    out = worldPos(w, static_cast<scene::Entity>(entity));
    return true;
}

u32 EqsSystem::envTagBits(void*, u32 entity) {
    scene::World& w = world();
    if (!live(w, entity)) return 0;
    const auto* t = w.component<scene::CTags>(static_cast<scene::Entity>(entity), scene::kComponentTags);
    return t ? t->bits : 0;
}

bool EqsSystem::envTrace(void*, const Vec3& a, const Vec3& b, f32 radius, u32 ignoreA, u32 ignoreB) {
#if AVER_MODULE_PHYSICS
    const Vec3 d = b - a;
    const f32 len = d.size();
    if (len < 1e-3f) return false;
    const Vec3 dir = d / len;
    float pt[3], nrm[3];
    if (radius > 0.0f) return aver_phys_sphere_cast(a.x, a.y, a.z, dir.x, dir.y, dir.z, len, radius, pt, nrm) != 0;   // cannot tell whose body it hit
    int32_t hitEntity = 0;
    const int32_t hit = aver_phys_raycast(a.x, a.y, a.z, dir.x, dir.y, dir.z, len, pt, nrm, &hitEntity);
    if (hit == 0) return false;
    // An ownerless hit (landscape) reads entity 0 and blocks; the two ends themselves do not.
    return hitEntity == 0 || (static_cast<u32>(hitEntity) != ignoreA && static_cast<u32>(hitEntity) != ignoreB);
#else
    (void)a; (void)b; (void)radius; (void)ignoreA; (void)ignoreB;
    return false;
#endif
}

bool EqsSystem::envGround(void*, const Vec3& p, f32 up, f32 down, f32& z) {
#if AVER_MODULE_PHYSICS
    float pt[3], nrm[3];
    int32_t ent = 0;
    if (aver_phys_raycast(p.x, p.y, p.z + up, 0.0f, 0.0f, -1.0f, up + down, pt, nrm, &ent) == 0) return false;
    z = pt[2];
    return true;
#else
    (void)p; (void)up; (void)down; (void)z;
    return false;
#endif
}

bool EqsSystem::envCustom(void* user, const EqsTest& t, const EqsRunParams& p, const EqsItem& item, const EqsContext& ctx, f32& value) {
    auto* e = static_cast<EnvCtx*>(user);
    return e->sys->ai_ && e->sys->ai_->managed().callEqsTest(t, p, item, ctx, value);
}

EqsEnv EqsSystem::makeEnv(EnvCtx* e) const {
    EqsEnv env;
    env.user = e;
    env.nav = nav_;
    env.context = &EqsSystem::envContext;
    env.forward = &EqsSystem::envForward;
    env.actors = &EqsSystem::envActors;
    env.slots = &EqsSystem::envSlots;
    env.entityPos = &EqsSystem::envEntityPos;
    env.entityTagBits = &EqsSystem::envTagBits;
#if AVER_MODULE_PHYSICS
    env.trace = &EqsSystem::envTrace;
    env.ground = &EqsSystem::envGround;
#endif
    env.custom = &EqsSystem::envCustom;
    return env;
}

// ---- runs -------------------------------------------------------------------------------------------

EqsSystem::RunRec* EqsSystem::find(u32 handle) {
    const auto it = runs_.find(handle);
    return it == runs_.end() ? nullptr : &it->second;
}

const EqsSystem::RunRec* EqsSystem::find(u32 handle) const {
    const auto it = runs_.find(handle);
    return it == runs_.end() ? nullptr : &it->second;
}

u32 EqsSystem::create(u32 queryId, const EqsRunParams& params, i32 priority) {
    const EqsQuery* q = query(queryId);
    if (!q) return 0;
    usize waiting = 0;
    for (const auto& [h, r] : runs_) waiting += (r.status == Status::Queued || r.status == Status::Running) ? 1 : 0;
    if (waiting >= kMaxQueuedRuns) return 0;
    const u32 handle = nextHandle_++;
    RunRec& r = runs_[handle];
    r.handle = handle;
    r.queryId = queryId;
    r.priority = priority;
    r.queryName = q->name;
    r.env = std::make_unique<EnvCtx>();
    r.env->sys = this;
    r.env->contexts = q->contexts;
    EqsRunParams p = params;
    r.run = std::make_unique<EqsRun>(*q, std::move(p), makeEnv(r.env.get()));
    return handle;
}

u32 EqsSystem::start(u32 queryId, const EqsRunParams& params, i32 priority) {
    return create(queryId, params, priority);
}

u32 EqsSystem::runSync(u32 queryId, const EqsRunParams& params) {
    const u32 handle = create(queryId, params, 0);
    if (!handle) return 0;
    RunRec& r = *find(handle);
    r.run->setNav(nav_);
    r.status = Status::Running;
    r.run->step(0);
    finishRun(r);
    return handle;
}

void EqsSystem::finishRun(RunRec& r) {
    r.finishedAt = time_;
    if (r.run->cancelled()) {
        r.status = Status::Cancelled;
        return;
    }
    const EqsResult& res = r.run->result();
    r.status = res.status == EqsResult::Status::Finished ? Status::Finished : Status::Failed;

    EqsDebugRun d;
    d.query = r.queryName;
    d.querier = r.run->params().querier;
    scene::World& w = world();
    if (live(w, d.querier)) d.origin = worldPos(w, static_cast<scene::Entity>(d.querier));
    d.generated = res.generated;
    d.failed = r.status == Status::Failed;
    d.reason = res.reason;
    for (usize i = 0; i < res.items.size() && i < kMaxDebugItems; ++i) d.items.push_back(res.items[i]);
    if (!res.items.empty()) {
        const EqsQuery& q = r.run->query();
        const usize opt = std::min<usize>(res.items[0].option, q.options.size() - 1);
        for (const EqsTest& t : q.options[opt].tests) d.testNames.push_back(t.name.empty() ? eqsTestName(t.kind) : t.name);
        for (u32 t = 0; t < res.testCount && t < d.testNames.size(); ++t) d.bestValues.push_back(res.testValues[t]);
    }
    debug_.insert(debug_.begin(), std::move(d));
    if (debug_.size() > kMaxDebugRuns) debug_.resize(kMaxDebugRuns);

    if (ai_ && live(w, d.querier)) ai_->raise(static_cast<scene::Entity>(d.querier), r.status == Status::Finished ? "EqsFinished" : "EqsFailed");

    // Retain at most 64 finished results: drop the oldest.
    usize finished = 0;
    for (const auto& [h, x] : runs_) finished += (x.status == Status::Finished || x.status == Status::Failed || x.status == Status::Cancelled) ? 1 : 0;
    while (finished > kMaxRetainedResults) {
        u32 oldest = 0;
        for (const auto& [h, x] : runs_) {
            if (x.status != Status::Finished && x.status != Status::Failed && x.status != Status::Cancelled) continue;
            if (oldest == 0 || x.finishedAt < runs_[oldest].finishedAt) oldest = h;
        }
        if (oldest == 0) break;
        runs_.erase(oldest);
        --finished;
    }
}

EqsSystem::Status EqsSystem::status(u32 handle) const {
    const RunRec* r = find(handle);
    return r ? r->status : Status::Unknown;
}

const EqsResult* EqsSystem::result(u32 handle) const {
    const RunRec* r = find(handle);
    if (!r || (r->status != Status::Finished && r->status != Status::Failed)) return nullptr;
    return &r->run->result();
}

u32 EqsSystem::queryIdOf(u32 handle) const {
    const RunRec* r = find(handle);
    return r ? r->queryId : 0;
}

bool EqsSystem::cancel(u32 handle) {
    RunRec* r = find(handle);
    if (!r || (r->status != Status::Queued && r->status != Status::Running)) return false;
    r->run->cancel();
    r->status = Status::Cancelled;
    r->finishedAt = time_;
    return true;
}

void EqsSystem::release(u32 handle) { runs_.erase(handle); }

usize EqsSystem::pending() const {
    usize n = 0;
    for (const auto& [h, r] : runs_) n += (r.status == Status::Queued || r.status == Status::Running) ? 1 : 0;
    return n;
}

const EqsSystem::LastResult* EqsSystem::lastResult(i32 subject, u32 queryId) const {
    const auto it = last_.find({subject, queryId});
    return it == last_.end() ? nullptr : &it->second;
}

void EqsSystem::rebase(const Vec3& delta) {
    for (auto& [h, r] : runs_) {
        if (!r.run) continue;
        if (r.status == Status::Queued || r.status == Status::Running) {
            r.run->cancel();
            r.status = Status::Cancelled;
            r.finishedAt = time_;
        } else if (r.status == Status::Finished) {
            for (EqsItem& it : const_cast<EqsResult&>(r.run->result()).items) it.pos -= delta;
        }
    }
    for (EqsDebugRun& d : debug_) {
        d.origin -= delta;
        for (EqsItem& it : d.items) it.pos -= delta;
    }
}
void EqsSystem::reset() {
    runs_.clear();
    debug_.clear();
    last_.clear();
    leafRuns_.clear();
    nav_ = nullptr;
}

void EqsSystem::tick(scene::World& w, const fmt::OcNavData* nav, f32 dt, SynapseAi& ai) {
    ai_ = &ai;
    nav_ = nav;
    time_ += dt;
    for (EqsDebugRun& d : debug_) d.age += dt;

    // Results nobody released expire; a querier that is gone cancels its runs.
    for (auto it = runs_.begin(); it != runs_.end();) {
        RunRec& r = it->second;
        const bool done = r.status == Status::Finished || r.status == Status::Failed || r.status == Status::Cancelled;
        if (done && time_ - r.finishedAt > kResultLifetimeSec) { it = runs_.erase(it); continue; }
        if (!done) {
            const u32 q = r.run->params().querier;
            if (q != 0 && !live(w, q)) { r.run->cancel(); r.status = Status::Cancelled; r.finishedAt = time_; }
        }
        ++it;
    }

    std::vector<RunRec*> order;
    for (auto& [h, r] : runs_)
        if (r.status == Status::Queued || r.status == Status::Running) order.push_back(&r);
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [](const RunRec* a, const RunRec* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        return a->handle < b->handle;
    });

    const auto t0 = std::chrono::steady_clock::now();
    u64 spent = 0;
    bool stepped = false;
    const auto exhausted = [&]() {
        if (maxUnits_ > 0 && spent >= maxUnits_) return true;
        if (maxMs_ > 0.0f) {
            const std::chrono::duration<double, std::milli> el = std::chrono::steady_clock::now() - t0;
            if (el.count() >= static_cast<double>(maxMs_)) return true;
        }
        return false;
    };
    for (RunRec* r : order) {
        r->status = Status::Running;
        r->run->setNav(nav);
        while (!r->run->done()) {
            if (stepped && exhausted()) return;
            u32 slice = 256;
            if (maxUnits_ > 0) slice = static_cast<u32>(std::min<u64>(256, std::max<u64>(1, maxUnits_ > spent ? maxUnits_ - spent : 1)));
            const u64 before = r->run->unitsSpent();
            r->run->step(slice);
            spent += r->run->unitsSpent() - before;
            stepped = true;
        }
        finishRun(*r);
    }
}

} // namespace aver::synapse
