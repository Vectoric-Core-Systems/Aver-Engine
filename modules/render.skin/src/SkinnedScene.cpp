// The scene join: walk the skinned entities, give each its own posed mesh, dispatch, release.
// Blend shapes ride the same residency: see SkinnedScene.hpp and docs/plans/wave9/MORPH_TARGETS.md 2.7.
#include "aver/render/SkinnedScene.hpp"
#include "aver/scene/Components.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/Hash.hpp"
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
    undecodable_.clear();
    morphable_.clear();
    morphInfo_.clear();
    pass_.shutdown();
    ready_ = false;
    posedLastFrame_ = 0;
    boundsLastFrame_ = 0;
    morphDispatched_ = 0;
}

u32 SkinnedScene::morphOnlyCount() const {
    u32 n = 0;
    for (const auto& kv : live_) if (kv.second.morphOnly) ++n;
    return n;
}

bool SkinnedScene::morphTargetNames(scene::Entity e, std::vector<std::string>& out) const {
    const auto it = live_.find(e);
    if (it == live_.end() || !it->second.mm) return false;
    out = it->second.mm->names;
    return true;
}

void SkinnedScene::retire(scene::Entity e, anim::AnimSystem& anim) {
    const auto it = live_.find(e);
    if (it == live_.end()) return;
    if (it->second.mm) anim.morphs().declareTargets(e, {});
    Retired r{std::move(it->second), kRetireCooldown};
    const u64 id = r.r.meshId;
    // Reused rather than freed: there is no IDevice::destroyMesh, so dropping a record leaks its
    // vertex buffer for the run. Reuse is what keeps a spawn/despawn loop bounded.
    r.r.atRest = false;   // its buffer still holds the dead entity's last pose; see update()
    free_.emplace(id, std::move(r));
    live_.erase(it);
}

// The mesh read back from disk once per mesh id (the host discards its OcMeshData after upload, and the
// joints, weights and blend shapes are not recoverable from a MeshHandle). Null when it cannot be read.
const fmt::OcMeshData* SkinnedScene::decode(u64 meshId) {
    if (const auto it = decoded_.find(meshId); it != decoded_.end()) return &it->second;
    if (undecodable_.count(meshId) || !path_) return nullptr;
    const std::string p = path_(meshId, user_);
    fmt::OcMeshData md;
    std::string why;
    if (p.empty() || !fmt::loadOcMesh(p, md, &why)) {
        AVER_WARN("[Skin] mesh {} could not be re-read for its skin streams ({}); it draws "
                  "unskinned", meshId, why.empty() ? "no path" : why);
        undecodable_.insert(meshId);
        return nullptr;
    }
    return &decoded_.emplace(meshId, std::move(md)).first->second;
}

const SkinnedScene::MeshMorph* SkinnedScene::morphInfo(u64 meshId, const fmt::OcMeshData& md) {
    if (!md.hasMorph()) return nullptr;
    if (const auto it = morphInfo_.find(meshId); it != morphInfo_.end()) return &it->second;
    MeshMorph mm;
    for (u32 i = 0; i < md.morphTargets.size(); ++i) {
        const fmt::OcMorphTarget& t = md.morphTargets[i];
        const u64 id = fnv1a64(std::string_view(t.name));
        mm.idToTarget[id] = i;
        mm.decls.push_back(anim::MorphDecl{id, t.defaultWeight});
        mm.names.push_back(t.name);
    }
    return &morphInfo_.emplace(meshId, std::move(mm)).first->second;
}

void SkinnedScene::adoptMorph(Resident& r, scene::Entity e, anim::AnimSystem& anim, bool fresh) {
    r.morphStaged.clear();
    r.morphPad = Vec3{0, 0, 0};
    // A fresh buffer was seeded with the rest vertices and frames start empty at revision 0, so nothing is
    // owed. A reused record's buffer holds the dead entity's last result: force a dispatch.
    r.morphRev = fresh ? 0u : ~0u;
    r.morphDirty = !fresh;
    r.morphIdle = anim::kMorphIdleFrames;
    r.drawsPosed = false;
    if (r.mm) anim.morphs().declareTargets(e, r.mm->decls);
}

void SkinnedScene::stageMorph(Resident& r, scene::Entity e, anim::AnimSystem& anim) {
    if (!r.mm || !r.mesh) return;
    const anim::MorphFrame* f = anim.morphs().frame(e);
    const u32 rev = f ? f->revision : 0u;
    if (rev == r.morphRev) return;
    r.morphRev = rev;
    r.morphStaged.clear();
    if (f) {
        for (const anim::MorphWeight& m : f->active) {
            const auto it = r.mm->idToTarget.find(m.id);
            if (it != r.mm->idToTarget.end()) r.morphStaged.push_back(MorphTargetWeight{it->second, m.w});
        }
    }
    r.morphPad = anim::morphBoundsPad(r.mesh->morphTargets, r.morphStaged.data(), static_cast<u32>(r.morphStaged.size()));
    r.morphDirty = true;
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
        if (it->second.cooldown > 0 || it->second.r.morphOnly) continue;
        Resident r = std::move(it->second.r);
        free_.erase(it);
        Resident* live = &live_.emplace(e, std::move(r)).first->second;
        adoptMorph(*live, e, anim, false);
        return live;
    }

    if (!path_ || !mesh_) return nullptr;
    if (const auto it = skinnable_.find(meshId); it != skinnable_.end() && !it->second) return nullptr;

    const rhi::MeshHandle source = mesh_(meshId, user_);
    if (!source) return nullptr;

    const fmt::OcMeshData* dmd = decode(meshId);
    if (!dmd) { skinnable_[meshId] = false; return nullptr; }
    if (!dmd->hasSkin()) {
        AVER_WARN("[Skin] mesh {} has no skin streams, so an entity naming it with a CSkeletalMesh "
                  "draws unskinned rather than not at all", meshId);
        skinnable_[meshId] = false;
        return nullptr;
    }
    skinnable_[meshId] = true;

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
    if (!pass_.createMesh(*dmd, bones, r.gpu, r.vertices, dmd->hasMorph() ? meshId : 0)) return nullptr;

    // The per-bone rest boxes, once. minWeight ZERO on purpose: at zero this counts exactly the
    // influences the skinning counts, which is what makes the per-frame bound provably contain
    // every posed vertex rather than merely usually contain it.
    anim::boneRestBounds(dmd->positions, dmd->joints, dmd->weights, bones, 0.0f,
                         r.boneMin, r.boneMax, r.boneUsed);
    r.restMin = dmd->boundsMin;
    r.restMax = dmd->boundsMax;

    r.meshId = meshId;
    r.boneCount = bones;
    r.mesh = dmd;
    // Blend shapes only when the pass kept their data (a layout over the limit leaves the mesh unmorphed).
    r.mm = r.gpu.morphKey ? morphInfo(meshId, *dmd) : nullptr;
    // Its buffer was seeded with the source mesh's vertices, so it IS at rest already.
    r.atRest = true;
    Resident* live = &live_.emplace(e, std::move(r)).first->second;
    adoptMorph(*live, e, anim, true);
    return live;
}

// An unskinned mesh on an entity with CMorphTargets: the same posed-buffer machinery with no bones.
SkinnedScene::Resident* SkinnedScene::acquireMorphOnly(scene::World& world, anim::AnimSystem& anim,
                                                       rhi::IDevice& dev, scene::Entity e, u64 meshId) {
    (void)world;
    if (const auto it = live_.find(e); it != live_.end()) return &it->second;
    if (live_.size() >= kMaxResident) return nullptr;
    if (const auto it = morphable_.find(meshId); it != morphable_.end() && !it->second) return nullptr;

    auto range = free_.equal_range(meshId);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.cooldown > 0 || !it->second.r.morphOnly) continue;
        Resident r = std::move(it->second.r);
        free_.erase(it);
        Resident* live = &live_.emplace(e, std::move(r)).first->second;
        adoptMorph(*live, e, anim, false);
        return live;
    }

    if (!path_ || !mesh_) return nullptr;
    const rhi::MeshHandle source = mesh_(meshId, user_);
    if (!source) return nullptr;
    const fmt::OcMeshData* dmd = decode(meshId);
    if (!dmd) { morphable_[meshId] = false; return nullptr; }
    if (!dmd->hasMorph()) { morphable_[meshId] = false; return nullptr; }

    Resident r;
    r.drawMesh = dev.createSkinTargetMesh(source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        AVER_WARN("[Skin] this device cannot make a skin-target mesh; blend shapes on entity {} are off", e);
        morphable_[meshId] = false;
        return nullptr;
    }
    if (!pass_.createMesh(*dmd, 0, r.gpu, r.vertices, meshId)) { morphable_[meshId] = false; return nullptr; }
    morphable_[meshId] = true;

    r.meshId = meshId;
    r.morphOnly = true;
    r.mesh = dmd;
    r.mm = morphInfo(meshId, *dmd);
    r.restMin = dmd->boundsMin;
    r.restMax = dmd->boundsMax;
    r.atRest = true;
    Resident* live = &live_.emplace(e, std::move(r)).first->second;
    adoptMorph(*live, e, anim, true);
    return live;
}

void SkinnedScene::update(scene::World& world, anim::AnimSystem& anim, rhi::IDevice& dev) {
    if (!ready_) return;

    for (auto& kv : free_) if (kv.second.cooldown) --kv.second.cooldown;

    // ---- retire residency whose entity is gone or no longer declares itself skinned (or morphing) ----
    const u32 morphType = anim::MorphSystem::typeId();
    std::vector<scene::Entity> dead;
    for (const auto& kv : live_) {
        const scene::Entity e = kv.first;
        bool gone = !world.valid(e) || world.destroyPending(e);
        if (!gone) {
            if (kv.second.morphOnly)
                gone = morphType == 0 || !world.hasComponent(e, morphType) ||
                       world.hasComponent(e, scene::kComponentSkeletalMesh);
            else
                gone = !world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
        }
        if (gone) dead.push_back(e);
    }
    for (scene::Entity e : dead) retire(e, anim);

    // ---- reconcile the living ----
    posedLastFrame_ = 0;
    boundsLastFrame_ = 0;
    morphDispatched_ = 0;
    // THE CSkeletalMesh POOL, NOT EVERY ENTITY. Walking world.count() paid a destroyPending and two
    // sparse-set probes for each of a level's tens of thousands of entities every frame, to find the
    // handful that carry this component; the pool is dense over exactly those. entityAt() is the full
    // generational handle, the same value world.at() gives, and a retired entity has already left the
    // pool (World::flush drops it from every pool). Nothing below adds or removes a component, and the
    // size is re-read each pass regardless.
    scene::ComponentPool* rigs = world.pool(scene::kComponentSkeletalMesh);
    if (rigs) {
        for (usize i = 0; i < rigs->size(); ++i) {
            const scene::Entity e = rigs->entityAt(i);
            if (world.destroyPending(e)) continue;

            const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            // The COMPONENT'S PRESENCE declares the intent, not its skeleton field: an entity whose
            // skeleton has not resolved yet is still a skinned entity, it is just at rest. Presence is
            // what membership of the pool above already says.
            if (!mr || mr->mesh == 0) continue;

            Resident* r = acquire(world, anim, dev, e, mr->mesh);
            if (!r) continue;
            stageMorph(*r, e, anim);

            // The POSED bounds, written back onto the component every frame. Culling and picking read
            // CMeshRenderer's box, and a rig's rest box is the wrong shape the moment a limb leaves it:
            // the character pops out at the screen edge, or the cursor misses it. O(bones), so this is
            // affordable on every skinned entity every frame. The blend-shape pad widens it.
            u32 boneN = 0;
            if (const Mat4* bones = anim.skinning(e, boneN); bones && boneN > 0) {
                auto* mw = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
                Vec3 lo, hi;
                anim::posedBounds(r->boneMin, r->boneMax, r->boneUsed, bones, boneN,
                                  r->restMin, r->restMax, lo, hi, r->morphPad);
                if (mw) {
                    mw->aabbMin[0] = lo.x; mw->aabbMin[1] = lo.y; mw->aabbMin[2] = lo.z;
                    mw->aabbMax[0] = hi.x; mw->aabbMax[1] = hi.y; mw->aabbMax[2] = hi.z;
                    ++boundsLastFrame_;
                }
            } else if (auto* mw = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
                // Unposed: the rest extent, which is the truth for a mesh drawn at rest and is still an
                // improvement on whatever placeholder the spawn path left there.
                mw->aabbMin[0] = r->restMin.x - r->morphPad.x; mw->aabbMin[1] = r->restMin.y - r->morphPad.y;
                mw->aabbMin[2] = r->restMin.z - r->morphPad.z;
                mw->aabbMax[0] = r->restMax.x + r->morphPad.x; mw->aabbMax[1] = r->restMax.y + r->morphPad.y;
                mw->aabbMax[2] = r->restMax.z + r->morphPad.z;
                ++boundsLastFrame_;
            }

            u32 count = 0;
            const Mat4* skin = anim.skinning(e, count);
            if (skin && count > 0) {
                const u32 take = count > r->gpu.boneCapacity ? r->gpu.boneCapacity : count;
                r->staged.assign(skin, skin + take);
                r->atRest = false;
                r->morphDirty = false;      // the dispatch below carries the weights
                ++posedLastFrame_;
            } else if (!r->atRest || r->morphDirty) {
                // NO POSE MEANS DISPATCH IDENTITY, not skip the dispatch. Linear-blend skinning with
                // identity matrices reproduces the rest positions exactly, so this one mechanism covers
                // an entity with no animator, an entity whose rig failed to load, and -- the case that
                // makes it necessary rather than tidy -- a REUSED record whose buffer still holds the
                // dead entity's last pose. Skipping instead would show that stranger's pose forever.
                // A weight change with no pose lands here too: one dispatch with the new weights.
                r->staged.assign(r->boneCount, Mat4::identity());
                r->atRest = true;
                r->morphDirty = false;
            } else {
                r->staged.clear();   // already at rest; an unposed entity costs one dispatch, ever
            }
        }
    }

    // ---- unskinned meshes that morph ----
    if (morphType != 0) {
        if (scene::ComponentPool* faces = world.pool(morphType)) {
            for (usize i = 0; i < faces->size(); ++i) {
                const scene::Entity e = faces->entityAt(i);
                if (world.destroyPending(e) || world.hasComponent(e, scene::kComponentSkeletalMesh)) continue;
                const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
                if (!mr || mr->mesh == 0) continue;

                Resident* r = acquireMorphOnly(world, anim, dev, e, mr->mesh);
                if (!r) continue;
                stageMorph(*r, e, anim);

                // Idle for kMorphIdleFrames frames and it goes back to the static mesh (and its static BLAS);
                // any weight brings the posed one back the same frame, its buffer already written by prePass.
                if (!r->morphStaged.empty()) {
                    r->morphIdle = 0;
                    r->drawsPosed = true;
                } else if (r->morphIdle < anim::kMorphIdleFrames) {
                    if (++r->morphIdle >= anim::kMorphIdleFrames) r->drawsPosed = false;
                }

                if (auto* mw = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
                    mw->aabbMin[0] = r->restMin.x - r->morphPad.x; mw->aabbMin[1] = r->restMin.y - r->morphPad.y;
                    mw->aabbMin[2] = r->restMin.z - r->morphPad.z;
                    mw->aabbMax[0] = r->restMax.x + r->morphPad.x; mw->aabbMax[1] = r->restMax.y + r->morphPad.y;
                    mw->aabbMax[2] = r->restMax.z + r->morphPad.z;
                    ++boundsLastFrame_;
                }
            }
        }
    }
}

rhi::MeshHandle SkinnedScene::drawHandle(scene::Entity e) const {
    // Asked once per drawn entity by both hosts, and a level with no rigs has nothing resident:
    // answer before hashing the handle.
    if (live_.empty()) return 0;
    const auto it = live_.find(e);
    if (it == live_.end()) return 0;
    // A morph-only entity with nothing active draws its static mesh (and keeps its static BLAS).
    if (it->second.morphOnly && !it->second.drawsPosed) return 0;
    return it->second.drawMesh;
}

void SkinnedScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (!r.gpu.valid()) continue;
        if (r.morphOnly) {
            if (!r.morphDirty) continue;
            pass_.dispatchMorph(ctx, r.gpu, r.morphStaged.data(), static_cast<u32>(r.morphStaged.size()));
            r.morphDirty = false;
            ++morphDispatched_;
            continue;
        }
        if (r.staged.empty()) continue;
        pass_.dispatch(ctx, r.gpu, r.staged.data(), static_cast<u32>(r.staged.size()),
                       r.morphStaged.data(), static_cast<u32>(r.morphStaged.size()));
        if (!r.morphStaged.empty()) ++morphDispatched_;
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
