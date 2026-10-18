// CMorphTargets and the weight layers. See MorphSystem.hpp and docs/plans/wave9/MORPH_TARGETS.md 2.5.
#include "aver/anim/MorphSystem.hpp"

#include "aver/anim/AnimSampler.hpp"
#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>

namespace aver::anim {

namespace {

u32 g_type = 0;

constexpr usize kMaxQueuedVisemes = 4096;

f32 clamp01f(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Adds into a list sorted by id.
void addWeight(std::vector<MorphWeight>& v, u64 id, f32 w) {
    const auto it = std::lower_bound(v.begin(), v.end(), id, [](const MorphWeight& m, u64 k) { return m.id < k; });
    if (it != v.end() && it->id == id) it->w += w;
    else v.insert(it, MorphWeight{id, w});
}

MorphWeight* findWeight(std::vector<MorphWeight>& v, u64 id) {
    const auto it = std::lower_bound(v.begin(), v.end(), id, [](const MorphWeight& m, u64 k) { return m.id < k; });
    return (it != v.end() && it->id == id) ? &*it : nullptr;
}

f32 moveToward(f32 cur, f32 target, f32 rate, f32 dt) {
    const f32 step = rate * dt;
    if (cur < target) return std::min(cur + step, target);
    return std::max(cur - step, target);
}

// Where a raw animator clock lands inside a clip (the rule AnimSystem's sampler uses).
f32 wrapTime(const fmt::OcAnimation& c, f32 time, bool once) {
    if (c.duration <= 0.0f) return 0.0f;
    if (once) return time < 0.0f ? 0.0f : (time > c.duration ? c.duration : time);
    f32 t = std::fmod(time, c.duration);
    if (t < 0.0f) t += c.duration;
    return t;
}

std::string lowerOf(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

// symbol hash -> viseme pose id, for every name a script may queue without a rig: ARPAbet (both cases), the
// silence symbols and the viseme names (full and short, both cases).
const std::unordered_map<u64, u64>& symbolTable() {
    static const std::unordered_map<u64, u64> t = [] {
        std::unordered_map<u64, u64> m;
        const auto put = [&](const std::string& sym, const std::string& viseme) {
            const u64 pose = fnv1a64(std::string_view(viseme));
            m[fnv1a64(std::string_view(sym))] = pose;
            m[fnv1a64(std::string_view(lowerOf(sym)))] = pose;
        };
        for (u32 i = 0; arpabetSymbol(i); ++i) put(arpabetSymbol(i), arpabetViseme(arpabetSymbol(i)));
        for (const char* s : {"sil", "sp", "pau"}) put(s, "viseme_sil");
        for (u32 i = 0; i < kVisemeCount; ++i) {
            const std::string full = kVisemeNames[i];
            put(full, full);
            put(full.substr(7), full);
        }
        return m;
    }();
    return t;
}

} // namespace

// ---- component ----

u32 MorphSystem::registerComponent(scene::World& world) {
    auto b = world.registerComponent<CMorphTargets>("CMorphTargets");
    b.field("faceRig", scene::FieldKind::I64, static_cast<u16>(offsetof(CMorphTargets, faceRig)))
        .field("flags", scene::FieldKind::I32, static_cast<u16>(offsetof(CMorphTargets, flags)))
        .field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CMorphTargets, weight)))
        .field("reserved0", scene::FieldKind::I32, static_cast<u16>(offsetof(CMorphTargets, reserved)), 0, true)
        .field("reserved1", scene::FieldKind::I32, static_cast<u16>(offsetof(CMorphTargets, reserved) + sizeof(u32)), 0, true);
    AVER_ASSERTM(b.verify(sizeof(CMorphTargets)), "CMorphTargets");
    g_type = b.typeId();
    return g_type;
}

u32 MorphSystem::typeId() { return g_type; }

CMorphTargets* MorphSystem::attach(scene::World& world, scene::Entity e, u64 rig, u32 flags) {
    if (g_type == 0) return nullptr;
    auto* c = static_cast<CMorphTargets*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    *c = CMorphTargets{};
    c->faceRig = rig;
    c->flags = flags;
    c->weight = 1.0f;
    return c;
}

// ---- assets ----

const FaceRigAsset* MorphSystem::rigAsset(u64 id) {
    if (id == 0) return nullptr;
    if (const auto it = rigs_.find(id); it != rigs_.end()) return it->second.get();
    const std::string path = owner_.assetPath(id);
    if (path.empty()) { rigs_.emplace(id, nullptr); return nullptr; }
    auto a = std::make_unique<FaceRigAsset>();
    std::string why;
    if (!loadFaceRig(path, *a, &why) || !a->valid(&why)) {
        AVER_WARN("[Morph] face rig 0x{:016X} did not load: {}", id, why);
        rigs_.emplace(id, nullptr);
        return nullptr;
    }
    const FaceRigAsset* raw = a.get();
    rigs_.emplace(id, std::move(a));
    return raw;
}

const VisemeTrack* MorphSystem::trackAsset(u64 id) {
    if (id == 0) return nullptr;
    if (const auto it = tracks_.find(id); it != tracks_.end()) return it->second.get();
    const std::string path = owner_.assetPath(id);
    if (path.empty()) { tracks_.emplace(id, nullptr); return nullptr; }
    auto a = std::make_unique<VisemeTrack>();
    std::string why;
    if (!loadVisemeTrack(path, *a, nullptr, &why) || !a->valid(&why)) {
        AVER_WARN("[Morph] lip-sync track 0x{:016X} did not load: {}", id, why);
        tracks_.emplace(id, nullptr);
        return nullptr;
    }
    const VisemeTrack* raw = a.get();
    tracks_.emplace(id, std::move(a));
    return raw;
}

void MorphSystem::bindRig(EntityState& st, u64 rigId) {
    if (st.rigId == rigId) return;
    st.rigId = rigId;
    st.rig = rigAsset(rigId);
    st.rt.bind(st.rig);
}

// ---- state ----

MorphSystem::EntityState* MorphSystem::stateFor(scene::World& world, scene::Entity e, bool create) {
    if (!world.valid(e) || world.destroyPending(e)) return nullptr;
    const auto it = states_.find(e);
    if (it != states_.end()) return &it->second;
    return create ? &states_[e] : nullptr;
}

bool MorphSystem::ensure(scene::World& world, scene::Entity e) {
    if (!world.valid(e) || world.destroyPending(e)) return false;
    if (g_type != 0 && !world.hasComponent(e, g_type)) {
        if (!world.hasComponent(e, scene::kComponentMeshRenderer)) return false;
        if (!attach(world, e)) return false;
    }
    return stateFor(world, e, true) != nullptr;
}

bool MorphSystem::needs(const scene::World& world, scene::Entity e, const Pose* pose) const {
    if (g_type != 0 && world.hasComponent(e, g_type)) return true;
    if (pose && !pose->curves.empty()) return true;
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const EntityState& st = it->second;
    return !st.frame.active.empty() || !st.overrides.empty() || st.lip.active();
}

const MorphFrame* MorphSystem::frame(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? nullptr : &it->second.frame;
}

void MorphSystem::declareTargets(scene::Entity e, std::span<const MorphDecl> targets) {
    if (targets.empty()) {
        const auto it = states_.find(e);
        if (it == states_.end()) return;
        it->second.declared.clear();
        it->second.declaredKnown = false;
        it->second.rt.setDeclared({}, false);
        return;
    }
    EntityState& st = states_[e];
    st.declared.assign(targets.begin(), targets.end());
    std::sort(st.declared.begin(), st.declared.end(), [](const MorphDecl& a, const MorphDecl& b) { return a.id < b.id; });
    st.declaredKnown = true;
    std::vector<u64> ids;
    ids.reserve(st.declared.size());
    for (const MorphDecl& d : st.declared) ids.push_back(d.id);
    st.rt.setDeclared(ids, true);
}

u32 MorphSystem::declaredCount(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? 0u : static_cast<u32>(it->second.declared.size());
}

u32 MorphSystem::rigPoseCount(scene::Entity e) const {
    const auto it = states_.find(e);
    return (it == states_.end() || !it->second.rig) ? 0u : static_cast<u32>(it->second.rig->poses.size());
}

bool MorphSystem::weightOf(scene::Entity e, u64 id, f32& out) const {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const auto& a = it->second.frame.active;
    const auto f = std::lower_bound(a.begin(), a.end(), id, [](const MorphWeight& m, u64 k) { return m.id < k; });
    out = (f != a.end() && f->id == id) ? f->w : 0.0f;
    return true;
}

// ---- overrides ----

bool MorphSystem::setOverride(scene::World& world, scene::Entity e, u32 kind, u64 id, f32 w, f32 blendSec,
                              Layer layer, u64 owner) {
    EntityState* st = stateFor(world, e, true);
    if (!st || id == 0 || !std::isfinite(w)) return false;
    w = clampMorphWeight(w);
    for (Override& o : st->overrides) {
        if (o.layer != layer || o.kind != kind || o.id != id) continue;
        o.target = w;
        o.alphaTarget = 1.0f;
        o.owner = owner;
        if (blendSec <= 0.0f) { o.value = w; o.alpha = 1.0f; o.valueRate = o.alphaRate = 0.0f; }
        else {
            o.valueRate = std::fabs(w - o.value) / blendSec;
            o.alphaRate = std::fabs(1.0f - o.alpha) / blendSec;
        }
        return true;
    }
    Override o;
    o.id = id; o.kind = kind; o.layer = layer; o.owner = owner;
    o.value = o.target = w;
    o.alphaTarget = 1.0f;
    if (blendSec <= 0.0f) o.alpha = 1.0f;
    else { o.alpha = 0.0f; o.alphaRate = 1.0f / blendSec; }
    st->overrides.push_back(o);
    return true;
}

bool MorphSystem::setTarget(scene::World& world, scene::Entity e, u64 id, f32 w, f32 blendSec, Layer layer, u64 owner) {
    return setOverride(world, e, kPoseCurveMorph, id, w, blendSec, layer, owner);
}

bool MorphSystem::setPose(scene::World& world, scene::Entity e, u64 id, f32 w, f32 blendSec, Layer layer, u64 owner) {
    return setOverride(world, e, kPoseCurveFace, id, w, blendSec, layer, owner);
}

bool MorphSystem::clear(scene::Entity e, u64 id, u32 kindMask, f32 blendSec, Layer layer, u64 owner) {
    (void)owner;
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    bool any = false;
    auto& ov = it->second.overrides;
    for (usize i = 0; i < ov.size();) {
        Override& o = ov[i];
        const u32 bit = o.kind == kPoseCurveMorph ? 1u : 2u;
        if (o.layer != layer || !(kindMask & bit) || (id != 0 && o.id != id)) { ++i; continue; }
        any = true;
        if (blendSec <= 0.0f) { ov.erase(ov.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        o.alphaTarget = 0.0f;
        o.alphaRate = o.alpha / blendSec;
        ++i;
    }
    return any || id == 0;
}

void MorphSystem::releaseOwner(u64 owner) {
    if (owner == 0) return;
    for (auto& kv : states_) {
        auto& ov = kv.second.overrides;
        ov.erase(std::remove_if(ov.begin(), ov.end(), [owner](const Override& o) { return o.owner == owner; }), ov.end());
    }
}

bool MorphSystem::setFaceRig(scene::World& world, scene::Entity e, u64 asset) {
    if (g_type == 0 || !world.valid(e) || world.destroyPending(e)) return false;
    auto* c = world.component<CMorphTargets>(e, g_type);
    if (!c) return false;
    c->faceRig = asset;
    return true;
}

void MorphSystem::stepOverrides(EntityState& st, f32 dt) {
    auto& ov = st.overrides;
    for (usize i = 0; i < ov.size();) {
        Override& o = ov[i];
        o.value = moveToward(o.value, o.target, o.valueRate, dt);
        o.alpha = moveToward(o.alpha, o.alphaTarget, o.alphaRate, dt);
        if (o.alphaTarget <= 0.0f && o.alpha <= 0.0f) ov.erase(ov.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// ---- lip-sync ----

bool MorphSystem::lipSyncPlay(scene::World& world, scene::Entity e, u64 trackId, f32 rate, f32 start, f32 strength) {
    if (!ensure(world, e)) return false;
    const VisemeTrack* track = trackAsset(trackId);
    if (!track) return false;
    EntityState& st = states_[e];
    u64 rigId = 0;
    if (g_type != 0) if (const auto* c = world.component<CMorphTargets>(e, g_type)) rigId = c->faceRig;
    bindRig(st, rigId);

    LipState& l = st.lip;
    l.events = track->events;
    std::vector<std::string> unknown;
    resolveVisemeEvents(l.events, st.rig, track->set == VisemeSet::Viseme, &unknown);
    static std::set<std::string> warned;
    for (const std::string& s : unknown)
        if (warned.insert(s).second) AVER_WARN("[Morph] lip-sync symbol '{}' is not a phoneme or viseme; ignored", s);
    l.playing = true;
    l.clockSet = false;
    l.time = start;
    l.rate = rate == 0.0f ? 1.0f : rate;
    l.strength = strength < 0.0f ? 1.0f : strength;
    l.length = track->length();
    l.fading = false;
    l.fade = 1.0f;
    return true;
}

void MorphSystem::lipSyncStop(scene::Entity e, f32 blendOut) {
    const auto it = states_.find(e);
    if (it == states_.end()) return;
    LipState& l = it->second.lip;
    if (!l.active()) return;
    if (blendOut <= 0.0f) { l = LipState{}; return; }
    l.fading = true;
    l.fadeRate = 1.0f / blendOut;
}

bool MorphSystem::lipSyncSetTime(scene::Entity e, f32 t) {
    const auto it = states_.find(e);
    if (it == states_.end() || !it->second.lip.playing) return false;
    it->second.lip.time = t;
    it->second.lip.clockSet = true;
    return true;
}

bool MorphSystem::lipSyncQueue(scene::Entity e, u64 symbol, f32 startFromNow, f32 duration, f32 intensity) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    EntityState& st = it->second;
    u64 pose = 0;
    if (st.rig)
        for (const auto& ph : st.rig->phonemes)
            if (fnv1a64(std::string_view(ph.first)) == symbol) { pose = fnv1a64(std::string_view(ph.second)); break; }
    if (pose == 0) {
        const auto& table = symbolTable();
        const auto f = table.find(symbol);
        if (f == table.end()) return false;
        pose = f->second;
    }
    LipState& l = st.lip;
    if (l.queued.size() >= kMaxQueuedVisemes || !std::isfinite(startFromNow) || !std::isfinite(duration)) return false;
    VisemeEvent ev;
    ev.start = l.qclock + std::max(0.0f, startFromNow);
    ev.end = ev.start + std::max(duration, 1e-3f);
    ev.intensity = clamp01f(intensity);
    ev.pose = pose;
    l.queued.push_back(ev);
    l.fading = false;
    l.fade = 1.0f;
    return true;
}

bool MorphSystem::lipSyncState(scene::Entity e, f32& time, f32& length, f32& strength, bool& playing) const {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const LipState& l = it->second.lip;
    playing = l.active();
    time = l.playing ? l.time : l.qclock;
    length = l.playing ? l.length : 0.0f;
    strength = l.strength * l.fade;
    return true;
}

void MorphSystem::stepLip(EntityState& st, f32 dt, std::vector<MorphWeight>& poseW) {
    LipState& l = st.lip;
    if (!l.active()) return;
    const LipSyncParams params = st.rig ? st.rig->lipsync : LipSyncParams{};

    if (l.playing) {
        if (!l.clockSet) l.time += dt * l.rate;
        if (l.time > l.length + params.release + params.lead) l.playing = false;
    }
    l.clockSet = false;
    l.qclock += dt;
    l.queued.erase(std::remove_if(l.queued.begin(), l.queued.end(), [&](const VisemeEvent& v) {
        return std::max(v.end, v.start + params.minHold) + params.release < l.qclock;
    }), l.queued.end());
    if (l.fading) {
        l.fade -= l.fadeRate * dt;
        if (l.fade <= 0.0f) { l = LipState{}; return; }
    }
    if (!l.active()) { l = LipState{}; return; }

    lipA_.clear();
    lipB_.clear();
    if (l.playing) evalVisemeWeights(l.events, params, l.time, lipA_);
    if (!l.queued.empty()) evalVisemeWeights(l.queued, params, l.qclock, lipB_);
    // The two sources merge by the larger weight, then share one maxTotal.
    for (const VisemeSample& b : lipB_) {
        bool merged = false;
        for (VisemeSample& a : lipA_) if (a.pose == b.pose) { a.weight = std::max(a.weight, b.weight); merged = true; break; }
        if (!merged) lipA_.push_back(b);
    }
    f32 sum = 0.0f;
    for (const VisemeSample& s : lipA_) sum += s.weight;
    const f32 k = (params.maxTotal > 0.0f && sum > params.maxTotal) ? params.maxTotal / sum : 1.0f;
    for (const VisemeSample& s : lipA_) addWeight(poseW, s.pose, s.weight * k * l.strength * l.fade);
}

// ---- the frame ----

void MorphSystem::finish(EntityState& st, std::vector<MorphWeight>& w) {
    std::vector<MorphWeight> next;
    next.reserve(w.size());
    if (!st.off) {
        for (const MorphWeight& m : w) {
            const f32 v = clampMorphWeight(m.w * st.master);
            if (!(std::fabs(v) >= kMorphActiveEps)) continue;
            if (st.declaredKnown) {
                const auto d = std::lower_bound(st.declared.begin(), st.declared.end(), m.id,
                                                [](const MorphDecl& x, u64 k) { return x.id < k; });
                if (d == st.declared.end() || d->id != m.id) continue;
            }
            next.push_back(MorphWeight{m.id, v});
        }
    }
    MorphFrame& f = st.frame;
    bool changed = next.size() != f.active.size(), moved = false;
    if (!changed) {
        for (usize i = 0; i < next.size(); ++i) {
            if (next[i].id != f.active[i].id) { changed = true; break; }
            if (std::fabs(next[i].w - f.active[i].w) > 1e-5f) moved = true;
        }
    }
    f.moving = changed || moved;
    // Unmoved: keep the published values, so a sub-threshold drift cannot accumulate unseen.
    if (!f.moving) return;
    ++f.revision;
    f.active = std::move(next);
}

void MorphSystem::evaluate(scene::World& world, scene::Entity e, const fmt::OcSkeleton* skel, Pose* pose, f32 dt) {
    if (!world.valid(e)) return;
    EntityState& st = states_[e];
    st.stamp = owner_.tickIndex();
    ++evalCount_;

    u32 flags = 0;
    f32 master = 1.0f;
    u64 rigId = 0;
    if (g_type != 0)
        if (const auto* c = world.component<CMorphTargets>(e, g_type)) {
            flags = c->flags;
            master = c->weight == 0.0f ? 1.0f : c->weight;
            rigId = c->faceRig;
        }
    bindRig(st, rigId);
    st.off = (flags & kMorphOff) != 0;
    st.master = master;
    const bool inherit = (flags & kMorphInheritParent) != 0;

    tw_.clear();
    pw_.clear();

    // 1. Animation: the pose's curves, or the entity's own clip when it has no rig.
    if (!st.off && !inherit && !(flags & kMorphIgnoreAnim)) {
        const Pose* src = pose;
        if (!src) {
            animScratch_.curves.clear();
            animScratch_.local.clear();
            if (const auto* a = world.component<scene::CAnimator>(e, scene::kComponentAnimator); a && a->clip != 0) {
                if (const fmt::OcAnimation* c = owner_.clip(a->clip)) {
                    sampleClipCurves(*c, wrapTime(*c, a->time, (a->flags & scene::kAnimatorOnce) != 0), animScratch_);
                    const f32 bw = a->blendWeight == 0.0f ? 1.0f : a->blendWeight;
                    if (bw < 0.999f) for (PoseCurve& pc : animScratch_.curves) pc.value *= bw;
                    src = &animScratch_;
                }
            }
        }
        if (src)
            for (const PoseCurve& pc : src->curves)
                addWeight(pc.kind == kPoseCurveMorph ? tw_ : pw_, pc.id, pc.value);
    }

    // 3. Lip-sync (viseme pose weights).
    stepOverrides(st, dt);
    if (!st.off) stepLip(st, dt, pw_);

    // 4/5 for poses: sequencer then script overrides lerp the pose weight.
    for (const Layer layer : {Layer::Sequencer, Layer::Script}) {
        for (const Override& o : st.overrides) {
            if (o.layer != layer || o.kind != kPoseCurveFace) continue;
            MorphWeight* cur = findWeight(pw_, o.id);
            const f32 base = cur ? cur->w : 0.0f;
            const f32 v = base + (o.value - base) * o.alpha;
            if (cur) cur->w = v; else addWeight(pw_, o.id, v);
        }
    }

    // Poses become target weights (and bone deltas).
    expanded_.clear();
    for (const MorphWeight& m : pw_) {
        const f32 w = clampMorphWeight(m.w);
        if (w == 0.0f) continue;
        st.rt.expand(m.id, w, expanded_);
        if (pose && skel && st.rt.hasJoints(m.id)) st.rt.applyJoints(m.id, clamp01f(w), *skel, *pose);
    }
    for (const FaceTargetOut& t : expanded_) addWeight(tw_, t.target, t.weight);

    // 2. Defaults: a declared default is the value of a target nothing drives.
    for (const MorphDecl& d : st.declared)
        if (d.defaultWeight != 0.0f && !findWeight(tw_, d.id)) addWeight(tw_, d.id, d.defaultWeight);

    // 4/5 for targets.
    for (const Layer layer : {Layer::Sequencer, Layer::Script}) {
        for (const Override& o : st.overrides) {
            if (o.layer != layer || o.kind != kPoseCurveMorph) continue;
            MorphWeight* cur = findWeight(tw_, o.id);
            const f32 base = cur ? cur->w : 0.0f;
            const f32 v = base + (o.value - base) * o.alpha;
            if (cur) cur->w = v; else addWeight(tw_, o.id, v);
        }
    }

    if (inherit) {
        // Finished by resolveInherit, once every entity (a child may precede its parent) has a frame.
        st.own = tw_;
        st.inheritPending = true;
        inherit_.push_back(e);
        return;
    }
    st.inheritPending = false;
    finish(st, tw_);
}

void MorphSystem::tickLoose(scene::World& world, f32 dt) {
    if (g_type == 0) return;
    scene::ComponentPool* pool = world.pool(g_type);
    if (!pool) return;
    const u64 tick = owner_.tickIndex();
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        if (world.destroyPending(e)) continue;
        const auto it = states_.find(e);
        if (it != states_.end() && it->second.stamp == tick) continue;
        evaluate(world, e, nullptr, nullptr, dt);
    }
}

void MorphSystem::resolveInherit(scene::World& world) {
    std::sort(inherit_.begin(), inherit_.end(),
              [&](scene::Entity a, scene::Entity b) { return world.depth(a) < world.depth(b); });
    for (const scene::Entity e : inherit_) {
        const auto it = states_.find(e);
        if (it == states_.end() || !it->second.inheritPending) continue;
        EntityState& st = it->second;
        tw_ = st.own;
        for (scene::Entity p = world.parent(e); p != scene::kInvalidEntity; p = world.parent(p)) {
            const auto pit = states_.find(p);
            if (pit == states_.end()) continue;
            for (const MorphWeight& m : pit->second.frame.active) addWeight(tw_, m.id, m.w);
            break;
        }
        st.inheritPending = false;
        finish(st, tw_);
    }
    inherit_.clear();

    // Forget what died, and what nothing drives any more.
    const u64 tick = owner_.tickIndex();
    for (auto it = states_.begin(); it != states_.end();) {
        const scene::Entity e = it->first;
        const EntityState& st = it->second;
        const bool dead = !world.valid(e) || world.destroyPending(e);
        const bool idle = st.stamp != tick && !(g_type != 0 && world.hasComponent(e, g_type)) && st.overrides.empty() &&
                          !st.lip.active() && !st.declaredKnown && st.frame.active.empty();
        if (dead || idle) it = states_.erase(it); else ++it;
    }
    lastEvaluated_ = evalCount_;
    evalCount_ = 0;
}

void MorphSystem::clearRuntime() {
    for (auto& kv : states_) {
        EntityState& st = kv.second;
        st.overrides.clear();
        st.lip = LipState{};
        if (!st.frame.active.empty()) {
            st.frame.active.clear();
            ++st.frame.revision;
            st.frame.moving = true;
        }
    }
    inherit_.clear();
}

void MorphSystem::clear() {
    rigs_.clear();
    tracks_.clear();
    for (auto& kv : states_) {
        EntityState& st = kv.second;
        st.rigId = ~0ull;
        st.rig = nullptr;
        st.rt.bind(nullptr);
        st.lip = LipState{};
    }
}

// ---- provider ----

namespace {
// framework_morph_abi.h
constexpr i32 kSetTarget = 1, kSetPose = 2, kClear = 3, kGetWeight = 4, kSetRig = 5, kLipPlay = 6, kLipStop = 7,
              kLipTime = 8, kLipQueue = 9, kLipState = 10, kInfo = 11;
}

i32 morphProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, const f32* in, i32 inCount, f32* outF, i64* outI, void* user) {
    (void)h2;
    if (!user) return 0;
    MorphSystem& m = *static_cast<MorphSystem*>(user);
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const auto need = [&](i32 n) { return in != nullptr && inCount >= n; };
    const auto blend = [&](i32 idx) { return inCount > idx && in[idx] > 0.0f ? in[idx] : 0.0f; };
    switch (op) {
        case kSetTarget: return need(1) && m.ensure(w, e) && m.setTarget(w, e, static_cast<u64>(h0), in[0], blend(1)) ? 1 : 0;
        case kSetPose:   return need(1) && m.ensure(w, e) && m.setPose(w, e, static_cast<u64>(h0), in[0], blend(1)) ? 1 : 0;
        case kClear:     return m.clear(e, static_cast<u64>(h0), h1 == 0 ? 3u : static_cast<u32>(h1), blend(0)) ? 1 : 0;
        case kGetWeight: {
            f32 v = 0.0f;
            if (!m.weightOf(e, static_cast<u64>(h0), v)) return 0;
            if (outF) outF[0] = v;
            return 1;
        }
        case kSetRig:    return m.ensure(w, e) && m.setFaceRig(w, e, static_cast<u64>(h0)) ? 1 : 0;
        case kLipPlay:   return m.ensure(w, e) && m.lipSyncPlay(w, e, static_cast<u64>(h0), need(1) ? in[0] : 1.0f,
                                                                 need(2) ? in[1] : 0.0f, need(3) ? in[2] : 1.0f) ? 1 : 0;
        case kLipStop:   m.lipSyncStop(e, need(1) && in[0] >= 0.0f ? in[0] : 0.1f); return 1;
        case kLipTime:   return need(1) && m.lipSyncSetTime(e, in[0]) ? 1 : 0;
        case kLipQueue:  return need(3) && m.ensure(w, e) && m.lipSyncQueue(e, static_cast<u64>(h0), in[0], in[1], in[2]) ? 1 : 0;
        case kLipState: {
            f32 t = 0, len = 0, str = 0;
            bool playing = false;
            if (!m.lipSyncState(e, t, len, str, playing)) return 0;
            if (outF) { outF[0] = t; outF[1] = len; outF[2] = str; }
            return playing ? 1 : 0;
        }
        case kInfo: {
            if (!w.valid(e) || MorphSystem::typeId() == 0 || !w.hasComponent(e, MorphSystem::typeId())) return 0;
            const auto* c = w.component<CMorphTargets>(e, MorphSystem::typeId());
            if (outI) {
                outI[0] = static_cast<i64>(m.declaredCount(e));
                outI[1] = static_cast<i64>(m.rigPoseCount(e));
                outI[2] = static_cast<i64>(c ? c->faceRig : 0);
            }
            if (outF) outF[0] = (c && c->weight != 0.0f) ? c->weight : 1.0f;
            return 1;
        }
        default: return 0;
    }
}

} // namespace aver::anim
