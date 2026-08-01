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
}

void AnimSystem::tick(scene::World& world, f32 dt) {
    // Retire poses whose entity is gone. Without this the map only ever grows, and a long-running
    // session pays for every character it has ever spawned -- the generational key stops a recycled
    // slot INHERITING a pose, but it cannot stop the dead entry sitting there forever.
    for (auto it = posed_.begin(); it != posed_.end(); ) {
        if (world.valid(it->first) && !world.destroyPending(it->first)) ++it;
        else it = posed_.erase(it);
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
        if (!(a->flags & scene::kAnimatorPaused)) a->time += dt * speed;

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
        restPose(*skel, p.pose);

        if (const fmt::OcAnimation* c = clip(a->clip)) {
            // The loop flag lives on the COMPONENT, not the clip, so one clip can be looped by one
            // actor and played once by another.
            f32 t = a->time;
            if (c->duration > 0.0f) {
                if (a->flags & scene::kAnimatorOnce) {
                    t = t < 0.0f ? 0.0f : (t > c->duration ? c->duration : t);
                } else {
                    t = std::fmod(t, c->duration);
                    if (t < 0.0f) t += c->duration;
                }
            }
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

const Mat4* AnimSystem::skinning(scene::Entity e, u32& outCount) const {
    outCount = 0;
    auto it = posed_.find(e);
    if (it == posed_.end() || it->second.skin.empty()) return nullptr;
    outCount = static_cast<u32>(it->second.skin.size());
    return it->second.skin.data();
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
