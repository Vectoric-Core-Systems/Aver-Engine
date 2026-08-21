#include "aver/synapse/SynapsePerception.hpp"

#include "aver/core/Assert.hpp"

#include <cmath>
#include <cstddef>

#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif

namespace aver::synapse {
namespace {

// The entity's CURRENT world-space position -- same helper as SynapseAgent.cpp's own
// worldPositionOf, duplicated rather than shared across two small translation units for one
// three-line function.
Vec3 worldPositionOf(scene::World& world, scene::Entity e) {
    const Mat4& m = world.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

// The entity's CURRENT world-space forward axis (local +X, this engine's own convention --
// AverCharacter.WalkForward is (cos yaw, sin yaw, 0) at local +X, Character.cs:104). Read from row
// 0 of the world matrix (v' = v * M, the same row-major "translation in row 3" convention
// world.worldMatrix's own callers already rely on) -- v=(1,0,0) transforms to row 0. Assumes unit
// scale: a non-uniformly scaled observer's cone would need de-scaling this does not do.
Vec3 worldForwardOf(scene::World& world, scene::Entity e) {
    const Mat4& m = world.worldMatrix(e);
    const Vec3 f{m.m[0][0], m.m[0][1], m.m[0][2]};
    const f32 len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    return len > 1e-6f ? Vec3{f.x / len, f.y / len, f.z / len} : Vec3{1, 0, 0};
}

} // namespace

u32 PerceptionSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CSynapsePerception>("CSynapsePerception");
    b.field("sightRangeCm", scene::FieldKind::F32,
            static_cast<u16>(offsetof(CSynapsePerception, sightRangeCm)))
        .field("sightHalfAngleDeg", scene::FieldKind::F32,
               static_cast<u16>(offsetof(CSynapsePerception, sightHalfAngleDeg)))
        .field("thinkIntervalSec", scene::FieldKind::F32,
               static_cast<u16>(offsetof(CSynapsePerception, thinkIntervalSec)))
        .field("eyeHeightCm", scene::FieldKind::F32,
               static_cast<u16>(offsetof(CSynapsePerception, eyeHeightCm)))
        .field("thinkAccumulatorSec", scene::FieldKind::F32,
               static_cast<u16>(offsetof(CSynapsePerception, thinkAccumulatorSec)), 0, /*readOnly*/ true)
        .field("canSeeTarget", scene::FieldKind::I32,
               static_cast<u16>(offsetof(CSynapsePerception, canSeeTarget)), 0, /*readOnly*/ true)
        .field("lastKnownTargetEntity", scene::FieldKind::I32,
               static_cast<u16>(offsetof(CSynapsePerception, lastKnownTargetEntity)), 0, /*readOnly*/ true)
        .field("timeSinceSeenSec", scene::FieldKind::F32,
               static_cast<u16>(offsetof(CSynapsePerception, timeSinceSeenSec)), 0, /*readOnly*/ true);
    AVER_ASSERTM(b.verify(sizeof(CSynapsePerception)), "CSynapsePerception");
    type_ = b.typeId();
    return type_;
}

CSynapsePerception* PerceptionSystem::attach(scene::World& world, scene::Entity e) {
    if (type_ == 0) return nullptr;
    auto* p = static_cast<CSynapsePerception*>(world.addComponent(e, type_));
    if (!p) return nullptr;
    *p = CSynapsePerception{};
    return p;
}

bool PerceptionSystem::canSee(scene::World& world, scene::Entity observer, scene::Entity target,
                              const CSynapsePerception& p) const {
    const Vec3 eye = worldPositionOf(world, observer) + Vec3{0.0f, 0.0f, p.eyeHeightCm};
    const Vec3 targetPos = worldPositionOf(world, target);

    Vec3 toTarget = targetPos - eye;
    const f32 dist = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y + toTarget.z * toTarget.z);
    if (dist > p.sightRangeCm) return false;
    if (dist < 1e-4f) return true;   // standing inside the target -- nothing to occlude

    const Vec3 dir{toTarget.x / dist, toTarget.y / dist, toTarget.z / dist};
    const Vec3 forward = worldForwardOf(world, observer);
    const f32 cosHalfAngle = std::cos(p.sightHalfAngleDeg * (3.14159265358979323846f / 180.0f));
    if (dot(forward, dir) < cosHalfAngle) return false;   // outside the cone

#if AVER_MODULE_PHYSICS
    // Stopped just short of the target's own body, or the ray would report a hit AGAINST the
    // target itself and this function would conclude "blocked" for the one case that is actually
    // success. Point-blank range (nothing left to stop short of) skips the ray entirely -- the cone
    // check above already confirmed direction, and nothing plausibly occludes at this distance.
    constexpr f32 kOcclusionMarginCm = 10.0f;
    const f32 rayDist = dist - kOcclusionMarginCm;
    if (rayDist > 1.0f) {
        float hitPoint[3], hitNormal[3];
        int32_t hitEntity = 0;
        const int32_t hit = aver_phys_raycast(eye.x, eye.y, eye.z, dir.x, dir.y, dir.z, rayDist,
                                              hitPoint, hitNormal, &hitEntity);
        // The RETURN VALUE, not hitEntity -- an occluding landscape body is ownerless (hitEntity
        // reads 0 exactly as a miss would), and physics_abi.h's own comment on aver_phys_raycast is
        // explicit that only the return value tells the two apart.
        if (hit != 0) return false;
    }
#endif

    return true;
}

void PerceptionSystem::tick(scene::World& world, f32 dt) {
    if (type_ == 0) return;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return;

    // Resolved ONCE per tick, not once per perceiver -- see TargetResolverFn's own comment.
    const scene::Entity target = resolve_ ? resolve_(resolveUser_) : scene::kInvalidEntity;

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* p = static_cast<CSynapsePerception*>(pool->dataAt(i));
        if (!p) continue;

        if (p->timeSinceSeenSec >= 0.0f) p->timeSinceSeenSec += dt;

        p->thinkAccumulatorSec += dt;
        if (p->thinkAccumulatorSec < p->thinkIntervalSec) continue;
        p->thinkAccumulatorSec -= p->thinkIntervalSec;   // NOT reset to 0 -- keeps the average
                                                           // interval correct rather than drifting
                                                           // long every time a frame runs late.

        const bool sawIt = (target != scene::kInvalidEntity && target != e)
                          ? canSee(world, e, target, *p)
                          : false;

        const bool wasVisible = p->canSeeTarget != 0;
        p->canSeeTarget = sawIt ? 1 : 0;
        if (sawIt) {
            p->lastKnownTargetEntity = static_cast<i32>(target);
            p->timeSinceSeenSec = 0.0f;
            // EXACTLY ONCE PER ACQUISITION: only the tick that flips false-to-true fires. A
            // perceiver that keeps seeing the same target on every subsequent think-tick does not
            // refire -- see this method's own header comment.
            if (!wasVisible && notify_) notify_(e, "OnSeeTarget", notifyUser_);
        }
    }
}

PerceptionSystem& perceptionSystem() {
    static PerceptionSystem system;
    return system;
}

} // namespace aver::synapse
