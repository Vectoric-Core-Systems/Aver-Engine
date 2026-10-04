#include "aver/synapse/SynapseAgent.hpp"

#include "aver/core/Assert.hpp"
#include "aver/synapse/Nav.hpp"

#include <cstddef>

namespace aver::synapse {
namespace {

// The entity's CURRENT world-space position, decomposed from its world matrix rather than assumed
// equal to CLocal -- an agent parented to a moving platform (or anything else) must path from
// where it actually is, not from a local offset that means nothing outside its parent's frame.
Vec3 worldPositionOf(aver::scene::World& world, aver::scene::Entity e) {
    const Mat4& m = world.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

} // namespace

u32 AgentSystem::registerComponents(aver::scene::World& world) {
    auto b = world.registerComponent<CSynapseAgent>("CSynapseAgent");
    b.field("radiusCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, radiusCm)))
        .field("heightCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, heightCm)))
        .field("moveSpeedCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, moveSpeedCm)))
        .field("turnRateDegPerSec", scene::FieldKind::F32,
               static_cast<u16>(offsetof(CSynapseAgent, turnRateDegPerSec)))
        .field("arriveRadiusCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, arriveRadiusCm)))
        .field("goalXCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, goalXCm)))
        .field("goalYCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, goalYCm)))
        .field("goalZCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, goalZCm)))
        .field("status", scene::FieldKind::I32, static_cast<u16>(offsetof(CSynapseAgent, status)))
        .field("targetXCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, targetXCm)),
               0, /*readOnly*/ true)
        .field("targetYCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, targetYCm)),
               0, /*readOnly*/ true)
        .field("targetZCm", scene::FieldKind::F32, static_cast<u16>(offsetof(CSynapseAgent, targetZCm)),
               0, /*readOnly*/ true);
    AVER_ASSERTM(b.verify(sizeof(CSynapseAgent)), "CSynapseAgent");
    type_ = b.typeId();
    return type_;
}

CSynapseAgent* AgentSystem::attach(aver::scene::World& world, aver::scene::Entity e) {
    if (type_ == 0) return nullptr;
    auto* a = static_cast<CSynapseAgent*>(world.addComponent(e, type_));
    if (!a) return nullptr;
    *a = CSynapseAgent{};
    return a;
}

bool AgentSystem::setGoal(aver::scene::World& world, aver::scene::Entity e, const Vec3& goalCm) {
    if (type_ == 0) return false;
    auto* a = world.component<CSynapseAgent>(e, type_);
    if (!a) return false;
    a->goalXCm = goalCm.x;
    a->goalYCm = goalCm.y;
    a->goalZCm = goalCm.z;
    a->status = static_cast<i32>(AgentStatus::Requested);
    paths_.erase(e);
    return true;
}

void AgentSystem::tick(aver::scene::World& world, const aver::fmt::OcNavData* nav) {
    if (type_ == 0) return;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return;

    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* a = static_cast<CSynapseAgent*>(pool->dataAt(i));
        if (!a || a->status == static_cast<i32>(AgentStatus::None)) continue;

        const Vec3 pos = worldPositionOf(world, e);

        if (a->status == static_cast<i32>(AgentStatus::Requested)) {
            // No grid yet is not a failure -- SandboxApp's own .ocnav loader is one frame async
            // (navLoadPending_, serviced from onRender), so a goal set the same tick a level opens
            // must be able to wait rather than give up on a grid that simply has not arrived.
            if (!nav || !nav->valid()) continue;

            PathRequest req;
            req.startXCm = pos.x;
            req.startYCm = pos.y;
            req.goalXCm  = a->goalXCm;
            req.goalYCm  = a->goalYCm;
            const PathResult r = findPath(*nav, req);

            if (r.status == PathStatus::Found || r.status == PathStatus::Partial) {
                AgentPath& p = paths_[e];
                p.points = r.points;
                p.index = 0;
                a->status = static_cast<i32>(AgentStatus::Pathing);
            } else {
                // Unreachable, OffMesh, or Invalid: GIVES UP HERE. status is now Failed, so this
                // branch is never entered again for this request -- only a fresh setGoal() (which
                // resets status to Requested) earns another attempt. This is the whole difference
                // between "an impossible goal costs one query" and "an impossible goal costs one
                // query every frame forever".
                a->status = static_cast<i32>(AgentStatus::Failed);
                paths_.erase(e);
                continue;
            }
        }

        if (a->status == static_cast<i32>(AgentStatus::Pathing)) {
            auto it = paths_.find(e);
            if (it == paths_.end() || it->second.points.empty()) {
                // The path this agent was following disappeared from under it (e.g. a caller erased
                // paths_ some other way). Treat it the same as never having found one.
                a->status = static_cast<i32>(AgentStatus::Failed);
                continue;
            }
            AgentPath& p = it->second;
            const f32 arriveSq = a->arriveRadiusCm * a->arriveRadiusCm;

            // Advance past every waypoint already within range -- a fast agent or a fine path can
            // legitimately clear more than one in a single tick.
            while (p.index < p.points.size()) {
                const f32 dx = p.points[p.index].x - pos.x;
                const f32 dy = p.points[p.index].y - pos.y;
                if (dx * dx + dy * dy > arriveSq) break;
                ++p.index;
            }

            if (p.index >= p.points.size()) {
                a->status = static_cast<i32>(AgentStatus::Arrived);
                paths_.erase(e);
            } else {
                const Vec3& target = p.points[p.index];
                a->targetXCm = target.x;
                a->targetYCm = target.y;
                a->targetZCm = target.z;
            }
        }
    }

    prune(world);
}

void AgentSystem::prune(aver::scene::World& world) {
    // Mirrors AnimSystem::posed_'s own fix, for the identical reason: paths_ is keyed by the full
    // entity handle (index + generation), so a destroyed agent's entry cannot alias a fresh spawn
    // that reuses its index -- but it also never goes away on its own, so it must be erased
    // explicitly once the entity that owns it is no longer Pathing.
    for (auto it = paths_.begin(); it != paths_.end();) {
        const scene::Entity e = it->first;
        const auto* a = world.component<CSynapseAgent>(e, type_);
        if (!a || a->status != static_cast<i32>(AgentStatus::Pathing))
            it = paths_.erase(it);
        else
            ++it;
    }
}

AgentSystem& agentSystem() {
    static AgentSystem system;
    return system;
}

} // namespace aver::synapse
