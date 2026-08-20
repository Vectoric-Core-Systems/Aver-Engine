// The tick that turns a CAnimator's clock into a posed skeleton.
#include "aver/anim/AnimSystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/ComponentPool.hpp"

#include <cmath>

namespace aver::anim {

namespace {

// Loads an asset once and caches the outcome, INCLUDING a failure. Without caching the failure a
// missing file is reopened every frame for the rest of the session.
template <class T, class LoadFn>
const T* cachedLoad(std::unordered_map<u64, std::unique_ptr<T>>& cache, u64 id,
                    AssetPathFn resolve, void* user, LoadFn load, const char* what) {
    if (id == 0) return nullptr;
    if (auto it = cache.find(id); it != cache.end()) return it->second.get();
    if (!resolve) return nullptr;

    const std::string path = resolve(id, user);
    if (path.empty()) { cache.emplace(id, nullptr); return nullptr; }

    auto asset = std::make_unique<T>();
    std::string why;
    if (!load(path, *asset, &why)) {
        AVER_WARN("[Anim] {} 0x{:016X} did not load: {}", what, id, why);
        cache.emplace(id, nullptr);
        return nullptr;
    }
    const T* raw = asset.get();
    cache.emplace(id, std::move(asset));
    return raw;
}

// Where a raw animator clock lands inside a clip. ONE COPY, called by both the sampling path and
// the notify path, because the two disagreeing about where the playhead is would show up as a
// footstep landing on the wrong frame -- a symptom nobody would trace back to a duplicated fmod.
f32 wrapClipTime(const fmt::OcAnimation& c, f32 time, bool once) {
    if (c.duration <= 0.0f) return 0.0f;
    if (once) return time < 0.0f ? 0.0f : (time > c.duration ? c.duration : time);
    f32 t = std::fmod(time, c.duration);
    if (t < 0.0f) t += c.duration;
    return t;
}

} // namespace

const fmt::OcSkeleton* AnimSystem::skeleton(u64 objectId) {
    return cachedLoad(skeletons_, objectId, resolve_, user_,
                      [](const std::string& p, fmt::OcSkeleton& s, std::string* w) {
                          return fmt::loadOcSkel(p, s, w);
                      }, "skeleton");
}

const fmt::OcAnimation* AnimSystem::clip(u64 objectId) {
    return cachedLoad(clips_, objectId, resolve_, user_,
                      [](const std::string& p, fmt::OcAnimation& a, std::string* w) {
                          return fmt::loadOcAnim(p, a, w);
                      }, "clip");
}

void AnimSystem::clear() {
    skeletons_.clear();
    clips_.clear();
    posed_.clear();
    // The clock history goes with the clips it refers to. Keeping it would have the first tick after
    // a project reload compare against times measured in a world that no longer exists.
    clocks_.clear();
    fired_ = 0;
}

void AnimSystem::tick(scene::World& world, f32 dt) {
    // Retire poses whose entity is gone. Without this the map only ever grows, and a long-running
    // session pays for every character it has ever spawned -- the generational key stops a recycled
    // slot INHERITING a pose, but it cannot stop the dead entry sitting there forever.
    for (auto it = posed_.begin(); it != posed_.end(); ) {
        if (world.valid(it->first) && !world.destroyPending(it->first)) ++it;
        else it = posed_.erase(it);
    }
    // The clock history is pruned on the SAME rule and for the same reason -- an entry that outlived
    // its entity would hand a recycled handle somebody else's playhead, and the first step measured
    // from it could fire a burst of notifies that nothing in the new entity's clip ever passed.
    for (auto it = clocks_.begin(); it != clocks_.end(); ) {
        if (world.valid(it->first) && !world.destroyPending(it->first)) ++it;
        else it = clocks_.erase(it);
    }

    scene::ComponentPool* animators = world.pool(scene::kComponentAnimator);
    if (!animators) return;

    for (usize i = 0; i < animators->size(); ++i) {
        const scene::Entity e = animators->entityAt(i);
        auto* a = static_cast<scene::CAnimator*>(animators->dataAt(i));
        if (!a) continue;

        // A zero-filled component is a PLAYING one at normal speed and full weight -- see the note
        // on kAnimatorPaused. addComponent zero-fills, so anything that reads 0 as "off" gives you
        // an animator that attaches perfectly and does nothing.
        const f32 speed = a->speed == 0.0f ? 1.0f : a->speed;
        const f32 weight = a->blendWeight == 0.0f ? 1.0f : a->blendWeight;

        // THE CLOCK ADVANCES EVEN WITHOUT A RIG. Time is data on the component, so a scrubbing
        // editor and a save file both work before any asset has resolved.
        const bool paused = (a->flags & scene::kAnimatorPaused) != 0;
        if (!paused) a->time += dt * speed;

        // NOTIFIES, here rather than beside the sampling below, because a clip crosses its markers
        // whether or not a rig ever resolved -- the two `continue`s that follow are about having
        // something to POSE, and an audio cue on an unrigged prop is not less real for having no
        // bones. This is also before any weight or blend consideration for the same reason: a notify
        // is an event on a clock, not a contribution to a pose. A clip faded to zero weight still
        // reaches the moment its footstep is on.
        stepNotifies(e, *a, dt * speed, paused);

        const auto* sm = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
        if (!sm || sm->skeleton == 0) continue;
        const fmt::OcSkeleton* skel = skeleton(sm->skeleton);
        if (!skel || skel->bones.empty()) continue;

        // boneCount is published back so a script can see the rig resolved without loading it.
        if (auto* w = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh)) {
            w->boneCount = static_cast<u32>(skel->bones.size());
            w->dirty = 0;
        }

        Posed& p = posed_[e];
        p.skel = skel;
        restPose(*skel, p.pose);

        if (const fmt::OcAnimation* c = clip(a->clip)) {
            // The loop flag lives on the COMPONENT, not the clip, so one clip can be looped by one
            // actor and played once by another.
            const f32 t = wrapClipTime(*c, a->time, (a->flags & scene::kAnimatorOnce) != 0);
            if (weight >= 0.999f) {
                sampleAnimation(*c, t, p.pose);
            } else if (weight > 0.0f) {
                Pose sampled;
                restPose(*skel, sampled);
                sampleAnimation(*c, t, sampled);
                Pose blended;
                blendPose(p.pose, sampled, weight, blended);
                p.pose = blended;
            }
        }
        poseToSkinning(*skel, p.pose, p.skin);
    }
}

void AnimSystem::stepNotifies(scene::Entity e, const scene::CAnimator& a, f32 step, bool paused) {
    const fmt::OcAnimation* c = a.clip != 0 ? clip(a.clip) : nullptr;
    if (!c || c->duration <= 0.0f) {
        // No clip, or one with no length to travel: forget any history so a later clip starts clean
        // rather than measuring its first step from a stranger's playhead.
        clocks_.erase(e);
        return;
    }

    NotifyClock& clock = clocks_[e];
    if (clock.clip != a.clip) { clock = NotifyClock{}; clock.clip = a.clip; }

    const f32 wrapped = wrapClipTime(*c, a.time, (a.flags & scene::kAnimatorOnce) != 0);

    // A PAUSED ANIMATOR FIRES NOTHING, and this is the deliberate answer to scrubbing. A script (or
    // an editor) that writes CAnimator.time while paused is INSPECTING the clip, and delivering a
    // gunshot every time somebody drags a slider is the behaviour nobody wants. The playhead is
    // still recorded, so resuming continues from where the scrub left it rather than replaying the
    // span that was skipped.
    if (paused || step == 0.0f || !clock.started) {
        const bool first = !clock.started;
        clock.started = true;
        clock.prev = wrapped;
        // The FIRST observation of a running clip is not silent: it fires whatever sits exactly at
        // the start. See ClipStep::inclusiveStart. It is skipped for a paused first observation,
        // which is a clip sitting still rather than one starting.
        if (first && !paused && step != 0.0f) {
            ClipStep s;
            s.prev = 0.0f;
            s.now = wrapped;
            s.forward = step > 0.0f;
            s.inclusiveStart = true;
            deliver(e, *c, s);
        }
        return;
    }

    ClipStep s;
    s.prev = clock.prev;
    s.now = wrapped;
    s.forward = step > 0.0f;
    s.sweptWholeClip = std::fabs(step) >= c->duration;
    clock.prev = wrapped;
    deliver(e, *c, s);
}

void AnimSystem::deliver(scene::Entity e, const fmt::OcAnimation& c, const ClipStep& s) {
    if (!notify_) return;   // still tracked above, so installing a sink later delivers no backlog
    crossed_.clear();
    notifiesCrossed(c, s, crossed_);
    for (const u32 i : crossed_) {
        ++fired_;
        // BY INDEX INTO THE LIVE CLIP, and the sink is called immediately rather than queued. A
        // queue would need the names copied (the clip is cached and could in principle be evicted
        // between filling the queue and draining it) and would put the event a frame after the pose
        // it belongs to. Calling straight through costs the sink the right to unload this clip from
        // inside itself, which nothing does and which a comment is cheaper than defending against.
        notify_(e, c.notifies[i].name.c_str(), notifyUser_);
    }
}

const Mat4* AnimSystem::skinning(scene::Entity e, u32& outCount) const {
    outCount = 0;
    auto it = posed_.find(e);
    if (it == posed_.end() || it->second.skin.empty()) return nullptr;
    outCount = static_cast<u32>(it->second.skin.size());
    return it->second.skin.data();
}

const fmt::OcSkeleton* AnimSystem::posedSkeleton(scene::Entity e) const {
    auto it = posed_.find(e);
    return it == posed_.end() ? nullptr : it->second.skel;
}

bool AnimSystem::socketModel(scene::Entity e, const std::string& name, Mat4& out) const {
    auto it = posed_.find(e);
    if (it == posed_.end() || !it->second.skel) return false;
    const fmt::OcSocket* s = it->second.skel->socket(name);
    if (!s) return false;
    std::vector<Mat4> model;
    poseToModel(*it->second.skel, it->second.pose, model);
    return socketModelMatrix(model, *s, out);
}

const Pose* AnimSystem::pose(scene::Entity e) const {
    auto it = posed_.find(e);
    return it == posed_.end() ? nullptr : &it->second.pose;
}

AnimSystem& animSystem() {
    static AnimSystem s;
    return s;
}

} // namespace aver::anim
