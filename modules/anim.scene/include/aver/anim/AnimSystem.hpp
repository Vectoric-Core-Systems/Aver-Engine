// The join between a scene and the animation sampler: advances every CAnimator's clock and keeps
// the skinning matrices its CSkeletalMesh implies.
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

// Owns the loaded rigs and clips, and the pose of every animated entity.
class AnimSystem {
public:
    // Installs the resolver. Without one nothing loads and tick() is a no-op that still advances
    // clocks, so a scrubbing editor works before any asset path is known.
    void setResolver(AssetPathFn fn, void* user) { resolve_ = fn; user_ = user; }

    // Installs the notify sink. Without one, crossings are still TRACKED (so installing a sink
    // mid-session does not deliver a backlog) but nothing is delivered -- an editor previewing a
    // clip has no graphs to fire at, and should not pay for pretending otherwise.
    void setNotifySink(AnimNotifyFn fn, void* user) { notify_ = fn; notifyUser_ = user; }
    bool hasNotifySink() const { return notify_ != nullptr; }

    // How many notifies this system has delivered since the last clear(). Exists so a test can
    // assert on the COUNT without installing a sink that records, and so a host can see at a glance
    // whether an "it never fires" report is about the sink or about the clip.
    u64 notifiesFired() const { return fired_; }

    // Advances every playing CAnimator by `dt` and reposes the entity's skeleton.
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

    // The rig this entity is posed against, or nullptr. Exposed so a caller can enumerate sockets
    // (an editor listing them, a script validating a name) without resolving the asset itself.
    const fmt::OcSkeleton* posedSkeleton(scene::Entity e) const;

    // Assets, loaded on demand and cached by id. Null when the id is unknown or the file is bad.
    const fmt::OcSkeleton*  skeleton(u64 objectId);
    const fmt::OcAnimation* clip(u64 objectId);

    // Drops every cached asset and pose. Call when a project closes or content changes on disk.
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

    // Observes one animator's clock and fires whatever it passed. Split out of tick() because it
    // is the one part of that loop with nothing to do with posing, and because its own state
    // (clocks_) has a different lifetime rule than the pose cache beside it.
    void stepNotifies(scene::Entity e, const scene::CAnimator& a, f32 step, bool paused);
    // Runs one step's crossings out to the sink.
    void deliver(scene::Entity e, const fmt::OcAnimation& c, const ClipStep& s);

    std::unordered_map<scene::Entity, NotifyClock> clocks_;
    AssetPathFn resolve_ = nullptr;
    void* user_ = nullptr;
    AnimNotifyFn notify_ = nullptr;
    void* notifyUser_ = nullptr;
    u64 fired_ = 0;
    u32 attachmentsPlaced_ = 0;
    // Reused across entities and ticks so a frame of notifies costs no allocation after the first.
    std::vector<u32> crossed_;
};

// The process-global system, matching scene::World::instance().
AnimSystem& animSystem();

} // namespace aver::anim
