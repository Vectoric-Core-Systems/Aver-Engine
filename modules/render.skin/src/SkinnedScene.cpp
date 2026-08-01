// The scene join: walk the world, give every skinned entity its own posed mesh, dispatch, release.
#include "aver/render/SkinnedScene.hpp"
#include "aver/scene/Components.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/Log.hpp"

namespace aver::render {

namespace {

// A ceiling, so a runaway spawner degrades to unskinned draws instead of exhausting GPU memory in
// silence. Each resident costs its own posed vertex buffer plus a bone ring.
constexpr u32 kMaxResident = 256;

// Frames a retired record waits before it can be reused. writeBuffer is by contract immediate and
// unsynchronised, so a record handed on the frame its entity died is written while the GPU may
// still be reading it.
constexpr u32 kRetireCooldown = SkinnedMeshGpu::kRing + 1;

} // namespace

SkinnedScene::~SkinnedScene() { shutdown(); }

bool SkinnedScene::init(rhi::IDevice& dev) {
    shutdown();
    ready_ = pass_.init(dev);
    if (!ready_)
        AVER_ERROR("[Skin] the skinning pass would not initialise; skinned entities will draw at "
                   "rest for this run");
    return ready_;
}

void SkinnedScene::shutdown() {
    for (auto& kv : live_) pass_.destroyMesh(kv.second.gpu);
    for (auto& kv : free_) pass_.destroyMesh(kv.second.r.gpu);
    live_.clear();
    free_.clear();
    decoded_.clear();
    skinnable_.clear();
    pass_.shutdown();
    ready_ = false;
    posedLastFrame_ = 0;
    boundsLastFrame_ = 0;
}

void SkinnedScene::retire(scene::Entity e) {
    const auto it = live_.find(e);
    if (it == live_.end()) return;
    Retired r{std::move(it->second), kRetireCooldown};
    const u64 id = r.r.meshId;
    // Reused rather than freed: there is no IDevice::destroyMesh, so dropping a record leaks its
    // vertex buffer for the run. Reuse is what keeps a spawn/despawn loop bounded.
    r.r.atRest = false;   // its buffer still holds the dead entity's last pose; see update()
    free_.emplace(id, std::move(r));
    live_.erase(it);
}

// Finds or builds this entity's residency. Null means it cannot be skinned, which is a reason to
// draw it statically and never a reason to drop it.
SkinnedScene::Resident* SkinnedScene::acquire(scene::World& world, anim::AnimSystem& anim,
                                              rhi::IDevice& dev, scene::Entity e, u64 meshId) {
    if (const auto it = live_.find(e); it != live_.end()) return &it->second;
    if (live_.size() >= kMaxResident) return nullptr;

    // A record from a dead entity that drew the same mesh, once it has aged out.
    auto range = free_.equal_range(meshId);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.cooldown > 0) continue;
        Resident r = std::move(it->second.r);
        free_.erase(it);
        return &live_.emplace(e, std::move(r)).first->second;
    }

    if (!path_ || !mesh_) return nullptr;
    if (const auto it = skinnable_.find(meshId); it != skinnable_.end() && !it->second) return nullptr;

    const rhi::MeshHandle source = mesh_(meshId, user_);
    if (!source) return nullptr;

    // The decoded mesh, cached by mesh id. Re-read once per mesh rather than once per entity,
    // because the host discards its OcMeshData after upload and the joints and weights are not
    // recoverable from a MeshHandle.
    auto dit = decoded_.find(meshId);
    if (dit == decoded_.end()) {
        const std::string p = path_(meshId, user_);
        fmt::OcMeshData md;
        std::string why;
        if (p.empty() || !fmt::loadOcMesh(p, md, &why)) {
            AVER_WARN("[Skin] mesh {} could not be re-read for its skin streams ({}); it draws "
                      "unskinned", meshId, why.empty() ? "no path" : why);
            skinnable_[meshId] = false;
            return nullptr;
        }
        if (!md.hasSkin()) {
            AVER_WARN("[Skin] '{}' has no skin streams, so an entity naming it with a CSkeletalMesh "
                      "draws unskinned rather than not at all", p);
            skinnable_[meshId] = false;
            return nullptr;
        }
        dit = decoded_.emplace(meshId, std::move(md)).first;
        skinnable_[meshId] = true;
    }

    // The rig's size. Preferred from the component, which AnimSystem fills in once the skeleton
    // resolves; the skeleton itself is the fallback so an entity with no animator still gets a
    // correctly-sized ring.
    u32 bones = 0;
    if (const auto* sm = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh))
        bones = sm->boneCount;
    if (bones == 0)
        if (const auto* sm = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh))
            if (const fmt::OcSkeleton* sk = anim.skeleton(sm->skeleton))
                bones = static_cast<u32>(sk->bones.size());
    // Deferred rather than sized wrong: the asset may simply not have loaded yet, and a ring built
    // for the wrong rig would be silently truncating every frame after.
    if (bones == 0) return nullptr;

    Resident r;
    r.drawMesh = dev.createSkinTargetMesh(source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        AVER_WARN("[Skin] this device cannot make a skin-target mesh; entity {} draws unskinned", e);
        skinnable_[meshId] = false;
        return nullptr;
    }
    if (!pass_.createMesh(dit->second, bones, r.gpu, r.vertices)) return nullptr;

    // The per-bone rest boxes, once. minWeight ZERO on purpose: at zero this counts exactly the
    // influences the skinning counts, which is what makes the per-frame bound provably contain
    // every posed vertex rather than merely usually contain it.
    const fmt::OcMeshData& md = dit->second;
    anim::boneRestBounds(md.positions, md.joints, md.weights, bones, 0.0f,
                         r.boneMin, r.boneMax, r.boneUsed);
    r.restMin = md.boundsMin;
    r.restMax = md.boundsMax;

    r.meshId = meshId;
    r.boneCount = bones;
    // Its buffer was seeded with the source mesh's vertices, so it IS at rest already.
    r.atRest = true;
    return &live_.emplace(e, std::move(r)).first->second;
}

void SkinnedScene::update(scene::World& world, anim::AnimSystem& anim, rhi::IDevice& dev) {
    if (!ready_) return;

    for (auto& kv : free_) if (kv.second.cooldown) --kv.second.cooldown;

    // ---- retire residency whose entity is gone or no longer declares itself skinned ----
    std::vector<scene::Entity> dead;
    for (const auto& kv : live_) {
        const scene::Entity e = kv.first;
        if (!world.valid(e) || world.destroyPending(e) ||
            !world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh))
            dead.push_back(e);
    }
    for (scene::Entity e : dead) retire(e);

    // ---- reconcile the living ----
    posedLastFrame_ = 0;
    boundsLastFrame_ = 0;
    const u32 n = world.count();
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity e = world.at(i);
        if (world.destroyPending(e)) continue;

        const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        const auto* sm = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
        // The COMPONENT'S PRESENCE declares the intent, not its skeleton field: an entity whose
        // skeleton has not resolved yet is still a skinned entity, it is just at rest.
        if (!mr || !sm || mr->mesh == 0) continue;

        Resident* r = acquire(world, anim, dev, e, mr->mesh);
        if (!r) continue;

        // The POSED bounds, written back onto the component every frame. Culling and picking read
        // CMeshRenderer's box, and a rig's rest box is the wrong shape the moment a limb leaves it:
        // the character pops out at the screen edge, or the cursor misses it. O(bones), so this is
        // affordable on every skinned entity every frame.
        u32 boneN = 0;
        if (const Mat4* bones = anim.skinning(e, boneN); bones && boneN > 0) {
            auto* mw = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            Vec3 lo, hi;
            anim::posedBounds(r->boneMin, r->boneMax, r->boneUsed, bones, boneN,
                              r->restMin, r->restMax, lo, hi);
            if (mw) {
                mw->aabbMin[0] = lo.x; mw->aabbMin[1] = lo.y; mw->aabbMin[2] = lo.z;
                mw->aabbMax[0] = hi.x; mw->aabbMax[1] = hi.y; mw->aabbMax[2] = hi.z;
                ++boundsLastFrame_;
            }
        } else if (auto* mw = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
            // Unposed: the rest extent, which is the truth for a mesh drawn at rest and is still an
            // improvement on whatever placeholder the spawn path left there.
            mw->aabbMin[0] = r->restMin.x; mw->aabbMin[1] = r->restMin.y; mw->aabbMin[2] = r->restMin.z;
            mw->aabbMax[0] = r->restMax.x; mw->aabbMax[1] = r->restMax.y; mw->aabbMax[2] = r->restMax.z;
            ++boundsLastFrame_;
        }

        u32 count = 0;
        const Mat4* skin = anim.skinning(e, count);
        if (skin && count > 0) {
            const u32 take = count > r->gpu.boneCapacity ? r->gpu.boneCapacity : count;
            r->staged.assign(skin, skin + take);
            r->atRest = false;
            ++posedLastFrame_;
        } else if (!r->atRest) {
            // NO POSE MEANS DISPATCH IDENTITY, not skip the dispatch. Linear-blend skinning with
            // identity matrices reproduces the rest positions exactly, so this one mechanism covers
            // an entity with no animator, an entity whose rig failed to load, and -- the case that
            // makes it necessary rather than tidy -- a REUSED record whose buffer still holds the
            // dead entity's last pose. Skipping instead would show that stranger's pose forever.
            r->staged.assign(r->boneCount, Mat4::identity());
            r->atRest = true;
        } else {
            r->staged.clear();   // already at rest; an unposed entity costs one dispatch, ever
        }
    }
}

rhi::MeshHandle SkinnedScene::drawHandle(scene::Entity e) const {
    const auto it = live_.find(e);
    return it == live_.end() ? 0 : it->second.drawMesh;
}

void SkinnedScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (r.staged.empty() || !r.gpu.valid()) continue;
        pass_.dispatch(ctx, r.gpu, r.staged.data(), static_cast<u32>(r.staged.size()));
    }
}

void SkinnedScene::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    (void)width; (void)height;
    if (!ready_) return;
    // EVERY resident, not just the ones dispatched this frame, and retired ones too: a buffer left
    // in GeometryRead at submit makes next frame's barrier claim a state the hardware dropped, and
    // skinTransition is a no-op when the state already matches.
    for (auto& kv : live_) skinTransition(ctx, kv.second.gpu, rhi::ResourceState::Common);
    for (auto& kv : free_) skinTransition(ctx, kv.second.r.gpu, rhi::ResourceState::Common);
}

} // namespace aver::render
