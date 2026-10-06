#include "aver/synapse/SynapseHearing.hpp"

#include "aver/core/Assert.hpp"

#include <cmath>
#include <cstddef>

#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif

namespace aver::synapse {
namespace {

#if AVER_MODULE_PHYSICS
// Counts walls between a noise and an ear by re-casting past each hit. The ends are trimmed so
// the source's own body and the listener's capsule do not count as occluders.
u32 physicsOcclusion(void*, const Vec3& from, const Vec3& to) {
    constexpr f32 kTrimCm = 10.0f;
    constexpr u32 kMaxHits = 4;
    const Vec3 delta = to - from;
    const f32 total = delta.size();
    if (total <= 2.0f * kTrimCm) return 0;
    const Vec3 dir = delta * (1.0f / total);

    Vec3 start = from + dir * kTrimCm;
    f32 remaining = total - 2.0f * kTrimCm;
    u32 hits = 0;
    while (hits < kMaxHits && remaining > 1.0f) {
        float point[3], normal[3];
        int32_t entity = 0;
        // The return value, not `entity`: an ownerless landscape body reads 0 like a miss.
        if (aver_phys_raycast(start.x, start.y, start.z, dir.x, dir.y, dir.z, remaining, point, normal,
                              &entity) == 0)
            break;
        ++hits;
        const Vec3 hit{point[0], point[1], point[2]};
        const f32 used = (hit - start).size() + 5.0f;
        start = hit + dir * 5.0f;
        remaining -= used;
    }
    return hits;
}
#endif

Vec3 worldPositionOf(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

} // namespace

HearingSystem::HearingSystem() {
#if AVER_MODULE_PHYSICS
    occlusion_ = &physicsOcclusion;
#endif
}

u32 HearingSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CSynapseHearing>("CSynapseHearing");
    const auto f = [&](const char* name, scene::FieldKind k, usize off, bool ro) {
        b.field(name, k, static_cast<u16>(off), 0, ro);
    };
    f("sensitivity", scene::FieldKind::F32, offsetof(CSynapseHearing, sensitivity), false);
    f("maxRangeCm", scene::FieldKind::F32, offsetof(CSynapseHearing, maxRangeCm), false);
    f("tagMask", scene::FieldKind::I32, offsetof(CSynapseHearing, tagMask), false);
    f("memorySec", scene::FieldKind::F32, offsetof(CSynapseHearing, memorySec), false);
    f("earHeightCm", scene::FieldKind::F32, offsetof(CSynapseHearing, earHeightCm), false);
    f("hasMemory", scene::FieldKind::I32, offsetof(CSynapseHearing, hasMemory), true);
    f("heardXCm", scene::FieldKind::F32, offsetof(CSynapseHearing, heardXCm), true);
    f("heardYCm", scene::FieldKind::F32, offsetof(CSynapseHearing, heardYCm), true);
    f("heardZCm", scene::FieldKind::F32, offsetof(CSynapseHearing, heardZCm), true);
    f("heardLevel", scene::FieldKind::F32, offsetof(CSynapseHearing, heardLevel), true);
    f("heardTag", scene::FieldKind::I32, offsetof(CSynapseHearing, heardTag), true);
    f("heardSource", scene::FieldKind::I32, offsetof(CSynapseHearing, heardSource), true);
    f("confidence", scene::FieldKind::F32, offsetof(CSynapseHearing, confidence), true);
    f("timeSinceHeardSec", scene::FieldKind::F32, offsetof(CSynapseHearing, timeSinceHeardSec), true);
    f("heardCount", scene::FieldKind::I32, offsetof(CSynapseHearing, heardCount), true);
    AVER_ASSERTM(b.verify(sizeof(CSynapseHearing)), "CSynapseHearing");
    type_ = b.typeId();
    return type_;
}

CSynapseHearing* HearingSystem::attach(scene::World& world, scene::Entity e) {
    if (type_ == 0) return nullptr;
    if (world.hasComponent(e, type_)) return world.component<CSynapseHearing>(e, type_);   // idempotent
    auto* h = static_cast<CSynapseHearing*>(world.addComponent(e, type_));
    if (!h) return nullptr;
    *h = CSynapseHearing{};
    return h;
}

bool HearingSystem::emit(const NoiseEvent& ev) {
    if (queue_.size() >= maxQueue_) { ++dropped_; return false; }
    queue_.push_back(ev);
    return true;
}

const HearingMemory* HearingSystem::memoryOf(scene::Entity e) const {
    const auto it = memories_.find(e);
    return it == memories_.end() ? nullptr : &it->second;
}

void HearingSystem::forget(scene::Entity e) {
    const auto it = memories_.find(e);
    if (it == memories_.end()) return;
    if (sink_)
        for (const HeardMemory& m : it->second.entries()) sink_->onForgotten(e, m);
    it->second.clear();
}

void HearingSystem::tick(scene::World& world, f32 dt) {
    if (type_ == 0) { queue_.clear(); return; }
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) { queue_.clear(); return; }

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* h = static_cast<CSynapseHearing*>(pool->dataAt(i));
        if (!h || world.destroyPending(e)) continue;

        auto mi = memories_.find(e);
        if (mi == memories_.end()) mi = memories_.emplace(e, HearingMemory(capacity_)).first;
        HearingMemory& mem = mi->second;

        // Decay first, so a noise heard this tick carries age 0 into the publish below.
        forgotten_.clear();
        mem.tick(dt, h->memorySec, &forgotten_);
        if (sink_)
            for (const HeardMemory& m : forgotten_) sink_->onForgotten(e, m);

        if (!queue_.empty()) {
            HearingProfile prof;
            prof.sensitivity = h->sensitivity;
            prof.maxRangeCm = h->maxRangeCm;
            prof.occlusionFactor = occlusionFactor_;
            prof.tagMask = static_cast<u32>(h->tagMask);
            const Vec3 ear = worldPositionOf(world, e) + Vec3{0.0f, 0.0f, h->earHeightCm};

            for (const NoiseEvent& ev : queue_) {
                if (ev.source != 0 && ev.source == e) continue;   // not its own noise
                const f32 level = hearingLevel(ev, ear, prof, occlusion_, occlusionUser_);
                if (level <= 0.0f) continue;
                const HeardMemory& m = mem.remember(ev.pos, level, ev.tag, ev.source);
                ++h->heardCount;
                if (sink_) sink_->onHeard(e, m);
                if (notify_) notify_(e, "OnHearNoise", notifyUser_);
            }
        }

        if (const HeardMemory* best = mem.best()) {
            h->hasMemory = 1;
            h->heardXCm = best->pos.x;
            h->heardYCm = best->pos.y;
            h->heardZCm = best->pos.z;
            h->heardLevel = best->level;
            h->heardTag = static_cast<i32>(best->tag);
            h->heardSource = static_cast<i32>(best->source);
            h->confidence = best->confidence;
            h->timeSinceHeardSec = best->ageSec;
        } else {
            h->hasMemory = 0;
            h->confidence = 0.0f;
            h->heardLevel = 0.0f;
            h->timeSinceHeardSec = -1.0f;
        }
    }
    queue_.clear();

    for (auto it = memories_.begin(); it != memories_.end();) {
        if (!world.valid(it->first) || !world.component<CSynapseHearing>(it->first, type_))
            it = memories_.erase(it);
        else
            ++it;
    }
}

} // namespace aver::synapse
