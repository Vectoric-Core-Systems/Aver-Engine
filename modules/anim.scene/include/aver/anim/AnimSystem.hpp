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

// Owns the loaded rigs and clips, and the pose of every animated entity.
class AnimSystem {
public:
    // Installs the resolver. Without one nothing loads and tick() is a no-op that still advances
    // clocks, so a scrubbing editor works before any asset path is known.
    void setResolver(AssetPathFn fn, void* user) { resolve_ = fn; user_ = user; }

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

    // Assets, loaded on demand and cached by id. Null when the id is unknown or the file is bad.
    const fmt::OcSkeleton*  skeleton(u64 objectId);
    const fmt::OcAnimation* clip(u64 objectId);

    // Drops every cached asset and pose. Call when a project closes or content changes on disk.
    void clear();

    u32 loadedSkeletons() const { return static_cast<u32>(skeletons_.size()); }
    u32 loadedClips() const { return static_cast<u32>(clips_.size()); }
    u32 posedEntities() const { return static_cast<u32>(posed_.size()); }

private:
    struct Posed {
        Pose pose;
        std::vector<Mat4> skin;
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
    AssetPathFn resolve_ = nullptr;
    void* user_ = nullptr;
};

// The process-global system, matching scene::World::instance().
AnimSystem& animSystem();

} // namespace aver::anim
