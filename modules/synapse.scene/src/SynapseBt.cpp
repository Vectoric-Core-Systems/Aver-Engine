#include "aver/synapse/SynapseBt.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/synapse/SynapseAgent.hpp"

#include <cmath>
#include <cstddef>

namespace aver::synapse {
namespace {

// Duplicated from SynapseAgent.cpp/SynapsePerception.cpp rather than shared -- see either of their
// own comments for why a three-line helper is not worth a fourth small translation unit.
Vec3 worldPositionOf(scene::World& world, scene::Entity e) {
    const Mat4& m = world.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

// ---- the seven built-ins ------------------------------------------------------------------------
// Every one reaches agentSystem()/perceptionSystem() directly rather than through a `user` pointer
// -- see registerBuiltinBehaviors' own comment for why that is deliberate, not a shortcut.

bool hasTargetCondition(i32 subject, const fmt::OcBtNode&, void*) {
    const auto* p = scene::World::instance().component<CSynapsePerception>(
        static_cast<scene::Entity>(subject), perceptionSystem().componentType());
    return p && p->lastKnownTargetEntity != 0;
}

bool canSeeTargetCondition(i32 subject, const fmt::OcBtNode&, void*) {
    const auto* p = scene::World::instance().component<CSynapsePerception>(
        static_cast<scene::Entity>(subject), perceptionSystem().componentType());
    return p && p->canSeeTarget != 0;
}

// params[0] = the threshold, world-space centimetres.
bool distanceToTargetLessCondition(i32 subject, const fmt::OcBtNode& node, void*) {
    scene::World& w = scene::World::instance();
    const auto* p = w.component<CSynapsePerception>(static_cast<scene::Entity>(subject),
                                                     perceptionSystem().componentType());
    if (!p || p->lastKnownTargetEntity == 0) return false;
    const Vec3 a = worldPositionOf(w, static_cast<scene::Entity>(subject));
    const Vec3 b = worldPositionOf(w, static_cast<scene::Entity>(p->lastKnownTargetEntity));
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) < node.params[0];
}

// params[0..2] = the world-space goal. `elapsed` is reused as a one-shot "have I already issued
// this goal" flag rather than an actual duration -- see BtActionFn's own comment (Bt.hpp) for why
// that reuse is exactly what the scratch slot is for: tickBt erases it the moment this node
// resolves (Success or Failure), so the NEXT time MoveTo runs from scratch it correctly reads as
// "not yet issued" again, with no separate bookkeeping needed.
BtStatus moveToAction(i32 subject, const fmt::OcBtNode& node, f32, f32& elapsed, void*) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(subject);
    auto* a = w.component<CSynapseAgent>(e, agentSystem().componentType());
    if (!a) return BtStatus::Failure;   // this subject has no CSynapseAgent to path with at all

    if (elapsed == 0.0f) {
        agentSystem().setGoal(w, e, Vec3{node.params[0], node.params[1], node.params[2]});
        elapsed = 1.0f;
    }
    switch (static_cast<AgentStatus>(a->status)) {
        case AgentStatus::Arrived: return BtStatus::Success;
        case AgentStatus::Failed:  return BtStatus::Failure;
        default:                   return BtStatus::Running;   // None/Requested/Pathing: still working
    }
}

// params[0] = the duration, seconds. The one built-in with no scene dependency at all -- kept here
// beside the other six anyway, so a project reads all seven registrations in one place.
BtStatus waitAction(i32, const fmt::OcBtNode& node, f32 dt, f32& elapsed, void*) {
    elapsed += dt;
    if (elapsed >= node.params[0]) { elapsed = 0.0f; return BtStatus::Success; }
    return BtStatus::Running;
}

// Turns the subject to face its last-known target's CURRENT position, directly (Rot-around-Z, this
// engine's own yaw convention -- see SynapseAgent.cpp's worldForwardOf for the identical math read
// the other way). NOT routed through AverCharacter/CharacterMove: unlike AgentSystem's own tick
// ("Synapse advises, it does not move" -- SynapseAgent.hpp), a BEHAVIOUR TREE ACTION is exactly the
// layer that IS meant to cause a real effect; that rule was about the PATHING tick specifically,
// not about every native system Synapse owns.
BtStatus lookAtAction(i32 subject, const fmt::OcBtNode&, f32, f32&, void*) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(subject);
    const auto* p = w.component<CSynapsePerception>(e, perceptionSystem().componentType());
    if (!p || p->lastKnownTargetEntity == 0) return BtStatus::Failure;

    const Vec3 self = worldPositionOf(w, e);
    const Vec3 target = worldPositionOf(w, static_cast<scene::Entity>(p->lastKnownTargetEntity));
    const f32 dx = target.x - self.x, dy = target.y - self.y;
    if (dx * dx + dy * dy < 1e-6f) return BtStatus::Success;   // standing on it; nothing to turn toward
    const f32 yawRad = std::atan2(dy, dx);
    w.setLocalRotation(e, Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, yawRad));
    return BtStatus::Success;
}

// stringParam = the event name to raise (NOT `name`, which is "FireEvent" itself -- see
// OcBtNode::stringParam's own comment for why the two are separate fields).
BtStatus fireEventAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void*) {
    if (node.stringParam.empty()) return BtStatus::Failure;
    if (!btSystem().fireNotify(static_cast<scene::Entity>(subject), node.stringParam.c_str()))
        return BtStatus::Failure;
    return BtStatus::Success;
}

} // namespace

u32 BtSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CSynapseBehavior>("CSynapseBehavior");
    b.field("treeAssetId", scene::FieldKind::I64,
            static_cast<u16>(offsetof(CSynapseBehavior, treeAssetId)))
        .field("lastStatus", scene::FieldKind::I32,
               static_cast<u16>(offsetof(CSynapseBehavior, lastStatus)), 0, /*readOnly*/ true);
    AVER_ASSERTM(b.verify(sizeof(CSynapseBehavior)), "CSynapseBehavior");
    type_ = b.typeId();
    return type_;
}

CSynapseBehavior* BtSystem::attach(scene::World& world, scene::Entity e) {
    if (type_ == 0) return nullptr;
    auto* b = static_cast<CSynapseBehavior*>(world.addComponent(e, type_));
    if (!b) return nullptr;
    *b = CSynapseBehavior{};
    return b;
}

u64 BtSystem::loadTree(const std::string& path) {
    const u64 id = fnv1a64(path);
    if (trees_.find(id) != trees_.end()) return id;

    fmt::OcBtData data;
    std::string why;
    if (!fmt::loadOcBt(path, data, &why)) {
        AVER_WARN("[Synapse] could not load behaviour tree '{}': {}", path, why);
        return 0;
    }
    trees_[id] = std::move(data);
    return id;
}

bool BtSystem::fireNotify(scene::Entity e, const char* name) const {
    if (!notify_ || !name) return false;
    notify_(e, name, notifyUser_);
    return true;
}

void BtSystem::tick(scene::World& world, f32 dt) {
    if (type_ == 0) return;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return;

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* b = static_cast<CSynapseBehavior*>(pool->dataAt(i));
        if (!b || b->treeAssetId == 0) continue;

        const auto treeIt = trees_.find(b->treeAssetId);
        if (treeIt == trees_.end()) continue;   // the id does not resolve to a loaded tree

        BtRunningState& state = running_[e];   // default-constructs on first use
        const BtStatus s = tickBt(treeIt->second, registry_, static_cast<i32>(e), dt, state);
        b->lastStatus = static_cast<i32>(s);
    }

    prune(world);
}

void BtSystem::prune(scene::World& world) {
    // Mirrors AgentSystem::prune's own fix, for the identical reason: running_ is keyed by the full
    // entity handle, so a destroyed behaviour's entry cannot alias a fresh spawn that reuses its
    // index -- but it also never goes away on its own, so it must be erased explicitly once the
    // entity that owns it no longer has a tree assigned.
    for (auto it = running_.begin(); it != running_.end();) {
        const scene::Entity e = it->first;
        const auto* b = world.component<CSynapseBehavior>(e, type_);
        if (!b || b->treeAssetId == 0)
            it = running_.erase(it);
        else
            ++it;
    }
}

BtSystem& btSystem() {
    static BtSystem system;
    return system;
}

void registerBuiltinBehaviors(BtSystem& system) {
    BtRegistry& reg = system.registry();
    reg.registerCondition("HasTarget", &hasTargetCondition);
    reg.registerCondition("CanSeeTarget", &canSeeTargetCondition);
    reg.registerCondition("DistanceToTargetLess", &distanceToTargetLessCondition);
    reg.registerAction("MoveTo", &moveToAction);
    reg.registerAction("Wait", &waitAction);
    reg.registerAction("LookAt", &lookAtAction);
    reg.registerAction("FireEvent", &fireEventAction);
}

} // namespace aver::synapse
