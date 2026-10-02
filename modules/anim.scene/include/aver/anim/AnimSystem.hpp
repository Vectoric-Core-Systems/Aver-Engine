// The join between a scene and the animation sampler: advances every CAnimator's clock and keeps
// the skinning matrices its CSkeletalMesh implies. It also plays OBJECT animation: a transform clip
// (fmt::kOcAnimObject) on an entity with no skeleton moves that entity's own CLocal.
//
// A SECOND TARGET, deliberately, for the reason Aver.Assets.Gpu is one. Aver.Scene links Core and
// Assets and may not gain a Formats edge -- its components hold OPAQUE asset ids and nothing else,
// or the P/Invoke boundary stops being marshallable. Resolving one of those ids to a loaded .ocskel
// is therefore somebody else's job, and this is that somebody.
#pragma once

#include "aver/anim/AnimSampler.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::anim {

// Turns an asset ObjectId into a path this system can load. Supplied by the host, because asset
// discovery is a project's business and this module has no idea where content lives.
using AssetPathFn = std::string (*)(u64 objectId, void* user);

// Called when a playing clip crosses one of its notifies. Supplied by the host FOR THE SAME REASON
// AssetPathFn is: what a named event MEANS is a project's business. This module knows when the
// clock passed 1.25 s and that the marker there says "OnFootstep"; it has, and should have, no idea
// that somewhere there is a graph with an OnFootstep handler. The host owns that wire.
//
// `name` points into the loaded clip and is valid ONLY for the duration of the call. A sink that
// wants to keep it must copy it.
using AnimNotifyFn = void (*)(scene::Entity e, const char* name, void* user);

// A POSE MODIFIER: the seam a control rig lives in.
//
// Called once per animated entity per tick, AFTER the clip has been sampled and blended into `pose`
// and BEFORE that pose becomes skinning matrices. Until this existed there was nothing between those
// two statements -- sampleAnimation wrote the pose and poseToSkinning consumed it in the next line,
// with no way for anything to sit in between. Foot placement, a head that tracks a target, a hand
// held on a grip while the arm plays a canned clip: all of them are edits made in that gap.
//
// IT MUTATES `pose.local` IN PLACE, which is the whole reason this is cheap: anim::Pose is a flat
// vector of local transforms with no accessor wall, so a modifier assigns to the bones it cares
// about and leaves the rest of the sampled animation exactly as it was.
//
// EVERYTHING DOWNSTREAM ALREADY FOLLOWS. updateAttachments runs after every entity's pose exists, so
// a socket on a bone this moves tracks it for free, and the skinning matrices are computed from the
// modified pose rather than the sampled one.
using PoseModifierFn = void (*)(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user);

// Owns the loaded rigs and clips, and the pose of every animated entity.
class AnimSystem {
public:
    // Installs the resolver. Without one nothing loads and tick() is a no-op that still advances
    // clocks, so a scrubbing editor works before any asset path is known.
    void setResolver(AssetPathFn fn, void* user) { resolve_ = fn; user_ = user; }

    // The path an interned asset id resolves to, or empty when there is no resolver or it declines.
    //
    // Exposed because this class OWNS the resolver and other systems in this module need it: the
    // control rig loads a .ocrig by the same asset id, through the same host-supplied mapping, so
    // that a rig is found exactly the way a skeleton and a clip are. A second resolver installed
    // beside this one would be a second answer to the same question.
    std::string assetPath(u64 objectId) const {
        return (resolve_ && objectId) ? resolve_(objectId, user_) : std::string{};
    }

    // Installs the notify sink. Without one, crossings are still TRACKED (so installing a sink
    // mid-session does not deliver a backlog) but nothing is delivered -- an editor previewing a
    // clip has no graphs to fire at, and should not pay for pretending otherwise.
    void setNotifySink(AnimNotifyFn fn, void* user) { notify_ = fn; notifyUser_ = user; }
    bool hasNotifySink() const { return notify_ != nullptr; }

    // Installs the pose modifier. Without one the tick is exactly what it was before the seam
    // existed -- sample, blend, skin -- so the cost of having this hook and not using it is one null
    // check per animated entity per frame.
    void setPoseModifier(PoseModifierFn fn, void* user) { poseMod_ = fn; poseModUser_ = user; }
    bool hasPoseModifier() const { return poseMod_ != nullptr; }

    // How many notifies this system has delivered since the last clear(). Exists so a test can
    // assert on the COUNT without installing a sink that records, and so a host can see at a glance
    // whether an "it never fires" report is about the sink or about the clip.
    u64 notifiesFired() const { return fired_; }

    // OBJECT ANIMATION: an .ocanim flagged fmt::kOcAnimObject on a CAnimator entity with NO
    // CSkeletalMesh moves that entity's CLocal instead of posing bones. The clip's one track (bone 0)
    // is an object transform A(t) in some fixed frame; only RELATIVE motion is played, so the entity
    // follows F(t) = B * A(t0)^-1 * A(t) -- B its CLocal when playback began, t0 the animator's time
    // then. A car placed at the route pose for t0 rides the route; a fan placed anywhere spins about
    // its own pivot.
    //
    // LIVE is the host's call, and NOT LIVE is the default: the editor outside Play leaves objects at
    // their placements, as Unreal does for a level sequence. While not live an object clip's clock is
    // held too, so nothing advances behind the editor's back. Going not-live drops every captured B
    // and A(t0): the host restores the transforms itself, and a base kept across the gap would be the
    // previous session's placement.
    void setObjectAnimationLive(bool live);
    bool objectAnimationLive() const { return objectLive_; }
    // PAUSED, for a host whose Play can be paused (and frame-stepped by lifting the pause for one
    // tick). While live AND paused every object clock holds, every captured base is KEPT (resuming
    // continues the same session rather than re-basing on wherever the entity was left) and nothing
    // is written to CLocal. Skeletal animators are not affected: their previews keep their own
    // paused flag. Going not-live also clears it, like the bases: a session that ended paused must
    // not hold the next one.
    void setObjectAnimationPaused(bool paused) { objectPaused_ = paused; }
    bool objectAnimationPaused() const { return objectPaused_; }
    // Entities whose base is currently captured. A test can assert on it, and a host can see whether
    // object animation is actually driving anything.
    u32 objectAnimatedEntities() const { return static_cast<u32>(objects_.size()); }

    // Advances every playing CAnimator by `dt` and reposes the entity's skeleton. While object
    // animation is live it also writes CLocal for every object-clip entity (see above).
    //
    // CALLED UNCONDITIONALLY, not from the gameplay tick. The framework's tick groups are gated on
    // PLAYING, so hanging the clock off them would freeze every preview the moment the editor was
    // not in play -- which is exactly when somebody is looking at an animation.
    void tick(scene::World& world, f32 dt);

    // The skinning matrices for an entity, or nullptr. Valid until the next tick.
    const Mat4* skinning(scene::Entity e, u32& outCount) const;

    // The local-space pose, for a caller that wants joints rather than skinning matrices -- the
    // animation editor draws a box per bone from this.
    const Pose* pose(scene::Entity e) const;

    // Where a named socket on this entity's rig is THIS FRAME, in the skeleton's model space.
    // False when the entity is not posed, has no rig, or the rig declares no socket of that name --
    // three different reasons a caller cannot usefully tell apart, and none of them an error.
    //
    // MODEL SPACE, NOT WORLD, deliberately: this module has a pose and a skeleton and no opinion
    // about where the character is standing. Composing with the entity's own world matrix is the
    // caller's job, and the caller is the one holding it.
    //
    // RECOMPUTED PER CALL from the stored pose rather than cached. A pose is a handful of bones and
    // a socket query is a handful per frame; caching model matrices for every posed entity would
    // cost every character memory so that the few with attachments could save a walk.
    bool socketModel(scene::Entity e, const std::string& name, Mat4& out) const;

    // The value of a named float curve on whatever clip this entity is playing, at its current
    // playhead. False when the entity has no clip or the clip declares no such curve -- which a
    // caller SHOULD distinguish from a value of zero, because "the curve is not there" and "the
    // curve reads 0" mean opposite things to a script.
    //
    // BY HASH, because the only callers that matter cannot carry a string: a graph node and the C#
    // relay both name a curve by fnv1a64, exactly as a socket is named.
    bool curveValue(scene::Entity e, u64 nameHash, f32& out) const;

    // The rig this entity is posed against, or nullptr. Exposed so a caller can enumerate sockets
    // (an editor listing them, a script validating a name) without resolving the asset itself.
    const fmt::OcSkeleton* posedSkeleton(scene::Entity e) const;

    // Assets, loaded on demand and cached by id. Null when the id is unknown or the file is bad.
    const fmt::OcSkeleton*  skeleton(u64 objectId);
    const fmt::OcAnimation* clip(u64 objectId);

    // Drops every cached asset and pose, and every captured object base. Call when a project closes
    // or content changes on disk.
    //
    // ALSO CLOSES ANY NOTIFY STATE STILL OPEN, firing its "_End" first -- the fifth way an open
    // window leaks, and the one none of stepNotifyStates' four (see its own comment) can reach: a
    // reload drops every clock outright, with no clip switch, no destroy and no later tick for any
    // of those four to hang a close off of. Verified safe to fire rather than merely documented
    // around: the only real caller is game::GameContent::adopt, and BOTH of its call sites
    // (Runtime/src/GameApp.cpp and sandbox/src/SandboxProject.cpp -- the editor stopped keeping a
    // rebuildContentIndex of its own when the two hosts converged on one content index) run at
    // PROJECT-OPEN, before the old level's entities are torn down and before a new one is spawned --
    // never at engine or world teardown -- so the sink this reaches is exactly as alive as it is on
    // an ordinary tick, and staying silent here would BE the leak, not a way of avoiding one.
    void clear();

    u32 loadedSkeletons() const { return static_cast<u32>(skeletons_.size()); }
    u32 loadedClips() const { return static_cast<u32>(clips_.size()); }
    u32 posedEntities() const { return static_cast<u32>(posed_.size()); }

    // How many attachments the last tick actually placed. A test can assert on it, and a host can
    // tell "the socket name is wrong" (0) from "nothing is attached" (also 0, but with no
    // CAttachment anywhere) without guessing from a screenshot.
    u32 attachmentsPlaced() const { return attachmentsPlaced_; }

private:
    struct Posed {
        Pose pose;
        std::vector<Mat4> skin;
        // The rig this pose belongs to, borrowed from skeletons_ which owns it and outlives this
        // entry (clear() drops both together). Held so a socket query needs no World to find the
        // CSkeletalMesh and no second cache lookup.
        const fmt::OcSkeleton* skel = nullptr;
    };

    // WHERE THE CLOCK WAS LAST TICK, which is the whole of what firing a notify needs and the whole
    // of what a component cannot hold: CAnimator.time is a savable field a script may write at any
    // moment, so "the previous value of time" is a property of this SYSTEM'S last observation, not
    // of the entity. Kept in its own map rather than on Posed because a clip with no rig resolved
    // still has a clock, still crosses its notifies, and never gets a Posed entry.
    struct NotifyClock {
        f32 prev = 0.0f;
        u64 clip = 0;         // resets the history when the animator is pointed at a different clip
        bool started = false; // false until the first observation, which is what makes step 1 inclusive
        // WHERE THE PLAYHEAD IS AND WHAT IT IS PLAYING, refreshed every tick. Kept here rather than
        // on Posed because a clip needs NO RIG to have curves on it -- an "animation" that is only
        // curves is a real thing, and Posed exists only once a skeleton has resolved.
        const fmt::OcAnimation* asset = nullptr;
        f32 wrapped = 0.0f;   // `prev` is the last OBSERVATION; this is where the clock is NOW

        // WHICH NOTIFY STATES ARE CURRENTLY OPEN, one byte per notify in `asset`, index-for-index --
        // resized (and zeroed) only when it disagrees with asset->notifies.size(), which happens
        // exactly once per clip switch, right after the reset below throws the old array away with
        // everything else. This is the ONE piece of state a notify's Begin/End pairing depends on,
        // and it lives here rather than being recomputed, for the identical reason `prev` does: an
        // entity can be inspected (paused, scrubbed) for any number of ticks with nothing to
        // recompute it from.
        std::vector<u8> open;
    };
    // An asset that failed to load is cached as a null so a missing file is not re-opened every
    // frame for the life of the session.
    std::unordered_map<u64, std::unique_ptr<fmt::OcSkeleton>>  skeletons_;
    std::unordered_map<u64, std::unique_ptr<fmt::OcAnimation>> clips_;
    // KEYED BY THE FULL ENTITY HANDLE -- index AND generation -- and pruned every tick.
    //
    // It was keyed by entityIndex() alone and never erased, which meant a destroyed entity's pose
    // outlived it and the next entity to be handed that index inherited it. A fresh character with
    // no animator at all came up wearing a stranger's pose, and anything downstream keyed the same
    // way would have inherited its GPU buffers with it. The generation is in the handle precisely
    // so a recycled slot is a different key.
    std::unordered_map<scene::Entity, Posed> posed_;
    // Puts every CAttachment entity onto its parent's socket. Runs at the END of tick(), after
    // every pose in the world exists -- an attachment reads a pose it does not own, so it cannot
    // run inside the same loop that is still producing them.
    void updateAttachments(scene::World& world);

    // The object path, split around tick()'s clock advance: the base must be captured at the
    // animator's START time, before the first live advance moves it, and CLocal written after.
    // Capture only acts when the entity has no base for THIS clip, and marks the entry as driven
    // this tick so tick() can drop the ones that stopped qualifying.
    void captureObject(scene::World& world, scene::Entity e, const scene::CAnimator& a,
                       const fmt::OcAnimation& c);
    void writeObject(scene::World& world, scene::Entity e, const scene::CAnimator& a,
                     const fmt::OcAnimation& c);
    // A(t) from the clip's one track. Needs no skeleton: a one-bone pose is all the sampler wants.
    Transform sampleObject(const fmt::OcAnimation& c, f32 t);

    // What a live object animation composes against, captured on an entity's first live tick.
    struct ObjectBase {
        Transform base;      // B
        Mat4 startInverse;   // A(t0)^-1
        u64 clip = 0;        // the clip this was captured for; a different clip captures afresh
        u32 seen = 0;        // objectStamp_ of the last tick that drove this entity
    };
    // Keyed by the full Entity handle, like posed_, and pruned every tick.
    std::unordered_map<scene::Entity, ObjectBase> objects_;
    Pose objectPose_;        // sampleObject's scratch, so a tick allocates nothing per entity
    u32 objectStamp_ = 0;
    bool objectLive_ = false;
    bool objectPaused_ = false;

    // Observes one animator's clock and fires whatever it passed. Split out of tick() because it
    // is the one part of that loop with nothing to do with posing, and because its own state
    // (clocks_) has a different lifetime rule than the pose cache beside it.
    void stepNotifies(scene::Entity e, const scene::CAnimator& a, f32 step, bool paused);
    // Runs one step's crossings out to the sink.
    void deliver(scene::Entity e, const fmt::OcAnimation& c, const ClipStep& s);

    // NOTIFY STATES: a window that opens, stays open and closes, layered beside the instant notifies
    // `deliver` already handles (see OcAnimation::notifyDurations for the format half). Runs the SAME
    // step `deliver` just ran through every duration>0 notify in `c`, firing "<Name>_Begin" and
    // "<Name>_End" through the identical sink -- two suffixed instant events, not a second wire, which
    // is what keeps this reachable from a node with no C# involved.
    //
    // THE FOUR WAYS AN OPEN STATE LEAKS, and where each is actually closed:
    //   (a) normal playback out the far side of the window -- closed HERE, by the ordinary crossing
    //       test below reaching the window's end point on some later call.
    //   (b) the clip changing while a window is open -- closed in stepNotifies, BEFORE the clock
    //       resets for the new clip (see closeAllOpen there).
    //   (c) the entity being destroyed or pruned mid-window -- closed in tick()'s clock-pruning loop,
    //       for the identical reason (b) is: there is no later call on a dead entity to close it.
    //   (d) the loop seam, playback wrapping past the end while a window is open -- closed HERE, for
    //       free: the window's end point is clamped to `c.duration` (never past it, see the local
    //       `endT` below), and a wrapped step's tail segment always runs up to `c.duration`, so a
    //       clamped end point is always inside it. No separate "detect a wrap" branch exists because
    //       none is needed once the end point cannot outrun the clip it belongs to.
    //   (e) a full reload dropping every clock outright -- NOT closed here, because there is no
    //       ClipStep for a reload to reuse this crossing math with. Closed in clear() instead; see
    //       its own comment for why firing there is safe rather than merely documented as unsafe.
    void stepNotifyStates(scene::Entity e, const fmt::OcAnimation& c, const ClipStep& s, NotifyClock& clock);
    // Fires one "<Name>_Begin" or "<Name>_End" through the sink and counts it, exactly like `deliver`
    // does for an instant notify -- including doing NOTHING when no sink is installed, so installing
    // one later still delivers no backlog.
    void fireState(scene::Entity e, const fmt::OcAnimation& c, u32 index, bool begin);
    // Fires "_End" for every notify still open under `clock` and marks them closed. The shared tail
    // of leaks (b) and (c): both are "this clock's bookkeeping is about to be thrown away", and the
    // only difference between them is what throws it away.
    void closeAllOpen(scene::Entity e, NotifyClock& clock);

    std::unordered_map<scene::Entity, NotifyClock> clocks_;

    // WHAT tick() REMEMBERS ABOUT EACH ANIMATOR FROM THE LAST TIME, one per dense slot of the animator
    // pool. It exists for two questions that would otherwise be re-asked of every animated entity
    // every frame: has the set of entities changed (the prune scans), and is this held object clip
    // already observed (the clock work). See tick() for why each answer is exact.
    struct Slot {
        // Who held this slot at the last pre-scan. The sequence of these, compared slot for slot, is
        // the "did the entity set change" signal -- see tick().
        scene::Entity e = scene::kInvalidEntity;
        // The held-clip fast path may skip this entity: its clock is already as a held stepNotifies
        // leaves it, for exactly the clip, time bits and loop flag below, and `objClip` is that
        // clip. Voided by steadyEpoch_ moving on, and cleared whenever tick() takes the full path for
        // this entity (only a held step that ran no sink re-arms it); never trusted across a change of `e`.
        bool steady = false;
        u32  epoch = 0;
        u64  clip = 0;
        u32  timeBits = 0;
        u32  once = 0;
        const fmt::OcAnimation* objClip = nullptr;   // borrowed from clips_, which clear() drops with the epoch
    };
    std::vector<Slot> slots_;
    // Bumped whenever a slot's claim about its clock may have gone stale: a prune scan that erases a
    // clock (and so may run sinks) and clear() (which drops clocks and clips both).
    u32 steadyEpoch_ = 1;
    // A kept clock or pose belongs to an entity that is alive but not in the animator pool, so its
    // death would not show in the pool's sequence: scan every tick until none is left.
    bool orphans_ = false;

    AssetPathFn resolve_ = nullptr;
    void* user_ = nullptr;
    AnimNotifyFn notify_ = nullptr;
    void* notifyUser_ = nullptr;

    PoseModifierFn poseMod_ = nullptr;
    void* poseModUser_ = nullptr;
    u64 fired_ = 0;
    u32 attachmentsPlaced_ = 0;
    // Set for the duration of clear() itself, so a notify sink that reacts to a forced "_End" by
    // calling clear() again -- reentrantly, from inside the loop clear() is still running -- does
    // nothing rather than clearing clips_/skeletons_ out from under that still-in-flight loop.
    bool clearing_ = false;
    // Reused across entities and ticks so a frame of notifies costs no allocation after the first.
    std::vector<u32> crossed_;
};

// The process-global system, matching scene::World::instance().
AnimSystem& animSystem();

} // namespace aver::anim
