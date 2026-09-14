// The tick that turns a CAnimator's clock into a posed skeleton.
#include "aver/anim/AnimSystem.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/ComponentPool.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

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

// Did this step cross `point`? EXACTLY notifiesCrossed's own per-index formula (AnimSampler.cpp),
// pulled out to one point instead of one clip's worth, because a notify STATE needs the identical
// interval test twice -- once for where it opens, once for where it closes -- and a second,
// slightly different reimplementation of "was this instant crossed" is exactly the kind of drift
// that would land a hit window's Begin on a different frame than an instant notify sitting at the
// same time would fire on. Covers forward, backward, both wrapped, and "the step swept the whole
// clip" (unconditionally true there, matching notifiesCrossed's "one step, one firing").
bool crossedPoint(f32 point, f32 dur, const ClipStep& s) {
    const f32 t = dur > 0.0f ? std::min(std::max(point, 0.0f), dur) : 0.0f;
    if (s.sweptWholeClip) return true;
    if (s.forward) {
        return (s.now >= s.prev)
                 ? (s.inclusiveStart ? (t >= s.prev && t <= s.now) : (t > s.prev && t <= s.now))
                 : ((t > s.prev && t <= dur) || (t >= 0.0f && t <= s.now));
    }
    return (s.now <= s.prev)
             ? (s.inclusiveStart ? (t <= s.prev && t >= s.now) : (t < s.prev && t >= s.now))
             : ((t < s.prev && t >= 0.0f) || (t >= s.now && t <= dur));
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
    // REENTRANCY GUARD. Firing "_End" below runs the host's notify sink, which is arbitrary code (a
    // graph node, in practice) -- if it reacts by calling clear() again, that reentrant call must do
    // nothing at all: proceeding would drop clips_ out from under THIS call's loop just below, which
    // is still reading clock.asset -- a pointer into clips_ -- for every entry it has not reached yet.
    if (clearing_) return;
    clearing_ = true;

    // LEAK (e): see the header comment on both this method and stepNotifyStates. A reload drops
    // every clock outright, and any notify state still open under one (a hit window a graph opened,
    // say) would vanish with no "_End" ever telling that graph it's over.
    //
    // MOVED OUT, not iterated in place. The guard above stops the notify sink from clearing clips_
    // or skeletons_ mid-loop, but it does not stop it touching clocks_ ITSELF through some other
    // call (a fresh animator ticked from inside the callback, say) -- and inserting into the map
    // this loop is walking would be exactly the iterator invalidation tick()'s own pruning loop
    // avoids by erasing as it goes. Emptying the member first means any such call lands on a clean,
    // empty map instead of one mid-iteration.
    auto closing = std::move(clocks_);
    clocks_.clear();
    for (auto& [e, clock] : closing) closeAllOpen(e, clock);

    skeletons_.clear();
    clips_.clear();
    posed_.clear();
    // Reset AFTER the fires above, matching notifiesFired()'s own contract ("since the last
    // clear()") -- the forced Ends just delivered belong to the epoch that ended, not the one
    // starting.
    fired_ = 0;
    clearing_ = false;
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
        if (world.valid(it->first) && !world.destroyPending(it->first)) { ++it; continue; }
        // LEAK (c): THE ENTITY IS GONE, mid-window or not. If a notify state was open under it,
        // this is the only place left to close it -- there is no later tick on a dead entity to ever
        // reach the point that would have closed it normally, so a hit window (say, a collider a
        // graph switched on at _Begin) would otherwise stay live for the rest of the session.
        closeAllOpen(it->first, it->second);
        it = clocks_.erase(it);
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
        // THE POSE MODIFIER SEAM. Everything above produced a local-space pose from a clip; this is
        // the one point at which anything else may edit it, and it was empty until a control rig
        // needed it -- sampleAnimation wrote p.pose and the next line consumed it.
        //
        // AFTER the sample and blend so a rig LAYERS on animation rather than fighting it, and
        // BEFORE poseToSkinning so what the rig did is what gets skinned. Put it the other side of
        // that call and the modifier would be editing a pose nothing reads again.
        if (poseMod_) poseMod_(e, *skel, p.pose, poseModUser_);

        poseToSkinning(*skel, p.pose, p.skin);
    }

    updateAttachments(world);
}

// A weapon in a hand. Reads the parent's posed rig and writes THIS entity's CLocal.
void AnimSystem::updateAttachments(scene::World& world) {
    attachmentsPlaced_ = 0;
    scene::ComponentPool* pool = world.pool(scene::kComponentAttachment);
    if (!pool) return;

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        const auto* a = static_cast<const scene::CAttachment*>(pool->dataAt(i));
        if (!a || a->socket == 0) continue;

        // WHO IT RIDES IS THE PARENT LINK, not a second field -- see CAttachment's own comment.
        const auto* h = world.component<scene::CHierarchy>(e, scene::kComponentHierarchy);
        if (!h || h->parent == scene::kInvalidEntity) continue;

        auto it = posed_.find(h->parent);
        if (it == posed_.end() || !it->second.skel) continue;
        const fmt::OcSocket* sock = it->second.skel->socketById(a->socket);
        // A NAME THAT MATCHES NOTHING LEAVES THE ENTITY WHERE IT IS, silently, rather than
        // snapping it to the parent's origin. A typo should look like "it did not attach", which
        // is diagnosable, not like "it attached to the wrong place", which is not.
        if (!sock) continue;

        // Recomputed per attachment rather than cached on Posed: see socketModel's own comment.
        // A character with three attachments walks its bones three times, which is nothing beside
        // the sampling that produced the pose in the first place.
        std::vector<Mat4> model;
        poseToModel(*it->second.skel, it->second.pose, model);
        Mat4 m;
        if (!socketModelMatrix(model, *sock, m)) continue;

        auto* loc = world.component<scene::CLocal>(e, scene::kComponentLocal);
        if (!loc) continue;
        loc->xf = transformFromMatrix(m);
        // THE REVISION MUST BE BUMPED OR NOTHING DOWNSTREAM NOTICES. CWorld is recomposed only
        // when composedLocalRev disagrees with this, so writing the transform and leaving the
        // revision alone gives an attachment that is correct in memory and never moves on screen.
        ++loc->rev;
        ++attachmentsPlaced_;
    }
}

void AnimSystem::stepNotifies(scene::Entity e, const scene::CAnimator& a, f32 step, bool paused) {
    const fmt::OcAnimation* c = a.clip != 0 ? clip(a.clip) : nullptr;
    if (!c || c->duration <= 0.0f) {
        // No clip, or one with no length to travel: forget any history so a later clip starts clean
        // rather than measuring its first step from a stranger's playhead.
        //
        // LEAK (b)'s twin. Losing the clip out from under an open window -- unset, failed to load,
        // or trimmed to zero length -- is the same hazard as pointing the animator at a DIFFERENT
        // clip below: whatever the OLD clip (still sitting in the clock we are about to erase) had
        // open must close first, or nothing ever will.
        auto it = clocks_.find(e);
        if (it != clocks_.end()) {
            closeAllOpen(e, it->second);
            clocks_.erase(it);
        }
        return;
    }

    NotifyClock& clock = clocks_[e];
    if (clock.clip != a.clip) {
        // LEAK (b): THE CLIP CHANGED WHILE A WINDOW WAS OPEN. clock.asset and clock.open still name
        // the OLD clip at this point -- the very last chance to read them before the reset below
        // throws them away with everything else a new clip needs a clean slate for.
        closeAllOpen(e, clock);
        clock = NotifyClock{};
        clock.clip = a.clip;
    }

    const f32 wrapped = wrapClipTime(*c, a.time, (a.flags & scene::kAnimatorOnce) != 0);
    // Published for curveValue BEFORE any of the early-outs below. A paused animator still has a
    // playhead and its curves still read there -- scrubbing a clip in the editor should move the
    // curve readout even though it fires no notifies.
    clock.asset = c;
    clock.wrapped = wrapped;
    // One open flag per notify THIS clip has. Resizing only on a mismatch means this runs (and
    // zeroes fresh) exactly once per clip switch, right after the reset above -- never on an
    // ordinary tick, which is what lets an open flag survive as long as `prev` does.
    if (clock.open.size() != c->notifies.size()) clock.open.assign(c->notifies.size(), 0);

    // A PAUSED ANIMATOR FIRES NOTHING, and this is the deliberate answer to scrubbing. A script (or
    // an editor) that writes CAnimator.time while paused is INSPECTING the clip, and delivering a
    // gunshot every time somebody drags a slider is the behaviour nobody wants. The playhead is
    // still recorded, so resuming continues from where the scrub left it rather than replaying the
    // span that was skipped.
    //
    // A NOTIFY STATE ALREADY OPEN WHEN A SCRUB LANDS INSIDE IT is not retroactively opened here --
    // only a CROSSING of its start opens one, exactly as only a crossing fires an instant notify, so
    // resuming from a scrub that landed mid-window closes it normally on its way out without ever
    // having announced it began. That is a missed Begin, not a leak: nothing is left open past when
    // playback actually leaves the window, which is the failure this system exists to prevent.
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
            stepNotifyStates(e, *c, s, clock);
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
    stepNotifyStates(e, *c, s, clock);
}

// See the header comment for the four leak shapes and where each is actually closed; this covers
// (a) and, for free, (d).
void AnimSystem::stepNotifyStates(scene::Entity e, const fmt::OcAnimation& c, const ClipStep& s,
                                   NotifyClock& clock) {
    const f32 dur = c.duration;
    for (u32 i = 0; i < static_cast<u32>(c.notifies.size()); ++i) {
        const f32 durI = c.notifyDuration(i);
        if (durI <= 0.0f) continue;   // an ordinary instant notify: deliver() already handled it

        const f32 beginT = dur > 0.0f ? std::min(std::max(c.notifies[i].time, 0.0f), dur) : 0.0f;
        // CLAMPED TO `dur`, NEVER PAST IT -- an author can still drag a marker near the end and give
        // it a window that would otherwise overrun the clip, but the window cannot survive the loop
        // seam (leak (d)). Clamping here is what makes crossedPoint's existing wrap-aware formula
        // close it for free below: a wrapped step's tail segment always runs up to `dur`, so an end
        // point sitting exactly AT `dur` is always inside that segment.
        const f32 endT = dur > 0.0f ? std::min(beginT + durI, dur) : 0.0f;

        // Forward playback ENTERS the window at its start and LEAVES at its end; played backward
        // (a negative CAnimator.speed) that is reversed -- the observer reaches the end point first.
        // "Begin"/"End" name what crossing means to whoever is watching, not which literal point was
        // crossed.
        const f32 entryPoint = s.forward ? beginT : endT;
        const f32 exitPoint  = s.forward ? endT   : beginT;
        const bool entryHit = crossedPoint(entryPoint, dur, s);
        const bool exitHit  = crossedPoint(exitPoint, dur, s);

        if (!clock.open[i]) {
            if (!entryHit) continue;
            clock.open[i] = 1;
            fireState(e, c, i, true);
            // BOTH IN ONE STEP: a short window (or a step that swept the whole clip, where
            // crossedPoint answers true unconditionally for every point -- see its own comment) can
            // cross both of its own ends inside a single tick. beginT <= endT always, so along
            // whichever direction this step travelled, entry is never later than exit -- firing End
            // right behind Begin here is what keeps a state from sitting open an extra tick with
            // nothing left to close it until crossedPoint happens to answer true again.
            if (exitHit) { clock.open[i] = 0; fireState(e, c, i, false); }
        } else if (exitHit) {
            clock.open[i] = 0;
            fireState(e, c, i, false);
        }
    }
}

// Fires one "<Name>_Begin" or "<Name>_End" and counts it, exactly like deliver() does for an instant
// notify -- including doing nothing at all when no sink is installed, so installing one later still
// delivers no backlog for a state either.
void AnimSystem::fireState(scene::Entity e, const fmt::OcAnimation& c, u32 index, bool begin) {
    if (!notify_) return;
    ++fired_;
    // A COPY, unlike deliver()'s pointer straight into OcNotify::name: "<Name>_Begin" and
    // "<Name>_End" exist nowhere in the loaded clip for a pointer to alias, so one has to be built.
    // It outlives the call below, which is all AnimNotifyFn's contract asks for.
    const std::string suffixed = c.notifies[index].name + (begin ? "_Begin" : "_End");
    notify_(e, suffixed.c_str(), notifyUser_);
}

// The shared tail of leaks (b) and (c): both are "this clock's bookkeeping is about to be thrown
// away", and the only difference between them is what throws it away (a new clip, or a dead entity).
void AnimSystem::closeAllOpen(scene::Entity e, NotifyClock& clock) {
    if (!clock.asset) return;   // never observed a clip: nothing could be open
    for (u32 i = 0; i < static_cast<u32>(clock.open.size()); ++i) {
        if (!clock.open[i]) continue;
        clock.open[i] = 0;
        fireState(e, *clock.asset, i, false);
    }
}

void AnimSystem::deliver(scene::Entity e, const fmt::OcAnimation& c, const ClipStep& s) {
    if (!notify_) return;   // still tracked above, so installing a sink later delivers no backlog
    crossed_.clear();
    notifiesCrossed(c, s, crossed_);
    for (const u32 i : crossed_) {
        // A NOTIFY STATE'S OWN CROSSING FIRES NOTHING HERE. notifiesCrossed has no idea a duration
        // exists -- it reports `time` being crossed exactly as it always has, for every notify --
        // so without this a hit window would announce itself under its bare name AND under
        // "<Name>_Begin" from stepNotifyStates below. Skipped by VALUE (notifyDuration), not by a
        // second index set, so this is the one and only place that decision is made.
        if (c.notifyDuration(i) > 0.0f) continue;
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

bool AnimSystem::curveValue(scene::Entity e, u64 nameHash, f32& out) const {
    auto it = clocks_.find(e);
    if (it == clocks_.end() || !it->second.asset) return false;
    const fmt::OcCurve* c = it->second.asset->curveById(nameHash);
    if (!c) return false;
    out = sampleCurve(*c, it->second.wrapped);
    return true;
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
