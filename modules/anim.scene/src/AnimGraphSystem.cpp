// CAnimGraph and the system that runs it. See AnimGraphSystem.hpp.
#include "aver/anim/AnimGraphSystem.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace aver::anim {

namespace {
u32 g_type = 0;
}

u32 AnimGraphSystem::registerComponents(scene::World& world) {
    // Names outlive the call in case the field table keeps the pointer.
    static std::array<std::string, kAnimGraphSlots * 2> names = [] {
        std::array<std::string, kAnimGraphSlots * 2> n;
        for (u32 i = 0; i < kAnimGraphSlots; ++i) {
            n[i] = "paramHash" + std::to_string(i);
            n[kAnimGraphSlots + i] = "paramValue" + std::to_string(i);
        }
        return n;
    }();

    auto b = world.registerComponent<CAnimGraph>("CAnimGraph");
    b.field("machine", scene::FieldKind::I64, static_cast<u16>(offsetof(CAnimGraph, machine)))
        .field("activeState", scene::FieldKind::I64, static_cast<u16>(offsetof(CAnimGraph, activeState)), 0, true)
        .field("playRate", scene::FieldKind::F32, static_cast<u16>(offsetof(CAnimGraph, playRate)))
        .field("stateTime", scene::FieldKind::F32, static_cast<u16>(offsetof(CAnimGraph, stateTime)), 0, true)
        .field("flags", scene::FieldKind::I32, static_cast<u16>(offsetof(CAnimGraph, flags)))
        .field("reserved", scene::FieldKind::I32, static_cast<u16>(offsetof(CAnimGraph, reserved)), 0, true);
    for (u32 i = 0; i < kAnimGraphSlots; ++i)
        b.field(names[i].c_str(), scene::FieldKind::I64,
                static_cast<u16>(offsetof(CAnimGraph, paramHash) + i * sizeof(u64)));
    for (u32 i = 0; i < kAnimGraphSlots; ++i)
        b.field(names[kAnimGraphSlots + i].c_str(), scene::FieldKind::F32,
                static_cast<u16>(offsetof(CAnimGraph, paramValue) + i * sizeof(f32)));
    AVER_ASSERTM(b.verify(sizeof(CAnimGraph)), "CAnimGraph");
    g_type = b.typeId();
    return g_type;
}

CAnimGraph* AnimGraphSystem::attach(scene::World& world, scene::Entity e, u64 machineAsset) {
    if (g_type == 0) return nullptr;
    auto* c = static_cast<CAnimGraph*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    *c = CAnimGraph{};   // fresh value over the zero-filled bytes, so playRate is 1
    c->machine = machineAsset;
    return c;
}

void AnimGraphSystem::install(AnimSystem& anim, scene::World&) {
    anim_ = &anim;
    anim.setPoseSource(&AnimGraphSystem::sourceThunk, this);
}

void AnimGraphSystem::uninstall(AnimSystem& anim) { anim.setPoseSource(nullptr, nullptr); }

void AnimGraphSystem::clear() {
    instances_.clear();
    machines_.clear();
    spaces_.clear();
}

const AnimStateMachine* AnimGraphSystem::instance(scene::Entity e) const {
    const auto it = instances_.find(e);
    return it == instances_.end() ? nullptr : it->second.sm.get();
}

const AnimGraphSystem::Loaded* AnimGraphSystem::machineFor(u64 id) {
    if (id == 0 || !anim_) return nullptr;
    auto it = machines_.find(id);
    if (it != machines_.end()) return it->second.ok ? &it->second : nullptr;

    // Cached even on failure, so a bad file is reported once and not per entity per frame.
    Loaded& l = machines_[id];
    const std::string path = anim_->assetPath(id);
    std::string why;
    if (path.empty() || !loadStateMachine(path, l.asset, &why) || !l.asset.valid(&why)) {
        AVER_WARN("[AnimGraph] could not load state machine {}: {}", id, why.empty() ? path : why);
        return nullptr;
    }
    l.ok = true;
    return &l;
}

const BlendSpaceAsset* AnimGraphSystem::spaceFor(const std::string& ref) {
    const u64 id = fnv1a64(std::string_view(ref));
    auto it = spaces_.find(id);
    if (it != spaces_.end()) return it->second.get();

    auto space = std::make_unique<BlendSpaceAsset>();
    std::string why;
    const std::string path = anim_ ? anim_->assetPath(id) : std::string{};
    if (path.empty() || !loadBlendSpace(path, *space, &why) || !space->valid(&why)) {
        AVER_WARN("[AnimGraph] could not load blend space {}: {}", ref, why.empty() ? path : why);
        spaces_[id] = nullptr;
        return nullptr;
    }
    return (spaces_[id] = std::move(space)).get();
}

const fmt::OcAnimation* AnimGraphSystem::clipFor(const std::string& ref) {
    return anim_ ? anim_->clip(fnv1a64(std::string_view(ref))) : nullptr;
}

void AnimGraphSystem::tick(scene::World& world, f32 dt) {
    if (g_type == 0 || !anim_) return;
    scene::ComponentPool* pool = world.pool(g_type);
    ++stamp_;
    if (pool) {
        for (usize i = 0; i < pool->size(); ++i) {
            const scene::Entity e = pool->entityAt(i);
            auto* c = static_cast<CAnimGraph*>(pool->dataAt(i));
            if (!c || c->machine == 0) continue;

            const Loaded* loaded = machineFor(c->machine);
            if (!loaded) continue;

            // AnimSystem poses entities it finds an animator on, so a graph entity gets one.
            if (!world.hasComponent(e, scene::kComponentAnimator)) world.addComponent(e, scene::kComponentAnimator);

            Instance& inst = instances_[e];
            if (!inst.sm || inst.machine != c->machine) {
                inst.machine = c->machine;
                inst.sm = std::make_unique<AnimStateMachine>();
                inst.sm->bind(&loaded->asset,
                              [this](const std::string& r) { return clipFor(r); },
                              [this](const std::string& r) { return spaceFor(r); });
            }
            inst.stamp = stamp_;
            AnimStateMachine& sm = *inst.sm;

            for (u32 s = 0; s < kAnimGraphSlots; ++s) {
                if (c->paramHash[s] == 0) continue;
                const bool trigger = sm.isTriggerHash(c->paramHash[s]);
                if (sm.setByHash(c->paramHash[s], c->paramValue[s]) && trigger) c->paramValue[s] = 0.0f;   // one-shot: zero tells the script it was taken
            }

            if (!(c->flags & kAnimGraphPaused)) sm.tick(dt * (c->playRate == 0.0f ? 1.0f : c->playRate));

            events_.clear();
            sm.drainEvents(events_);
            if (sink_)
                for (const AsmEvent& ev : events_) sink_(e, ev, sinkUser_);
            c->activeState = fnv1a64(std::string_view(sm.activeState()));
            c->stateTime = sm.normalizedTime();
        }
    }
    // An instance nothing drove this tick belongs to an entity that died or lost the component.
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (it->second.stamp == stamp_) ++it;
        else it = instances_.erase(it);
    }
}

bool AnimGraphSystem::sourceThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user) {
    auto* self = static_cast<AnimGraphSystem*>(user);
    const auto it = self->instances_.find(e);
    if (it == self->instances_.end() || !it->second.sm) return false;
    it->second.sm->evaluate(skel, pose);
    return true;
}

AnimGraphSystem& animGraphSystem() {
    static AnimGraphSystem s;
    return s;
}

} // namespace aver::anim
