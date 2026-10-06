#include "aver/synapse/SynapseCrowd.hpp"

#include "aver/core/Assert.hpp"
#include "aver/synapse/Nav.hpp"
#include "aver/synapse/SynapseAgent.hpp"   // CSynapseAgent layout and AgentStatus only; no singleton

#include <algorithm>
#include <cstddef>

namespace aver::synapse {
namespace {

#define CROWD_FIELD(builder, kind, name, ro) \
    builder.field(#name, scene::FieldKind::kind, static_cast<u16>(offsetof(CSynapseCrowd, name)), 0, ro)

V2 worldXY(scene::World& w, scene::Entity e, f32* z = nullptr) {
    const Mat4& m = w.worldMatrix(e);
    if (z) *z = m.m[3][2];
    return {m.m[3][0], m.m[3][1]};
}

} // namespace

CrowdSystem::CrowdSystem() = default;

u32 CrowdSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CSynapseCrowd>("CSynapseCrowd");
    CROWD_FIELD(b, F32, radiusCm, false);
    CROWD_FIELD(b, F32, maxSpeedCm, false);
    CROWD_FIELD(b, F32, maxAccelCm, false);
    CROWD_FIELD(b, F32, priority, false);
    CROWD_FIELD(b, I32, mode, false);
    CROWD_FIELD(b, F32, steerXCm, false);
    CROWD_FIELD(b, F32, steerYCm, false);
    CROWD_FIELD(b, F32, steerZCm, false);
    CROWD_FIELD(b, F32, arriveSlowRadiusCm, false);
    CROWD_FIELD(b, F32, arriveStopRadiusCm, false);
    CROWD_FIELD(b, F32, fleePanicRadiusCm, false);
    CROWD_FIELD(b, I32, enabled, false);
    CROWD_FIELD(b, I32, active, true);
    CROWD_FIELD(b, F32, velXCm, true);
    CROWD_FIELD(b, F32, velYCm, true);
    CROWD_FIELD(b, F32, speedCm, true);
    CROWD_FIELD(b, F32, desiredXCm, true);
    CROWD_FIELD(b, F32, desiredYCm, true);
    CROWD_FIELD(b, I32, stuck, true);
    AVER_ASSERTM(b.verify(sizeof(CSynapseCrowd)), "CSynapseCrowd");
    type_ = b.typeId();
    return type_;
}

CSynapseCrowd* CrowdSystem::attach(scene::World& world, scene::Entity e) {
    if (type_ == 0) return nullptr;
    if (world.hasComponent(e, type_)) return world.component<CSynapseCrowd>(e, type_);   // idempotent
    auto* c = static_cast<CSynapseCrowd*>(world.addComponent(e, type_));
    if (!c) return nullptr;
    *c = CSynapseCrowd{};
    if (const u32 agentType = world.componentId("CSynapseAgent")) {
        if (const auto* a = world.component<CSynapseAgent>(e, agentType)) {
            c->radiusCm = a->radiusCm;
            c->maxSpeedCm = a->moveSpeedCm;
        }
    }
    return c;
}

void CrowdSystem::setBackend(CrowdBackendKind kind, ICrowdBackend* gpu) {
    sim_.setBackendKind(kind);
    sim_.setGpuBackend(gpu);
}

void CrowdSystem::setMaxAgents(u32 n) {
    CrowdParams p = sim_.params();
    p.maxAgents = n;
    sim_.setParams(p);
}

void CrowdSystem::tick(scene::World& world, const fmt::OcNavData* nav, f32 dt) {
    if (type_ == 0) return;
    scene::ComponentPool* pool = world.pool(type_);
    if (!pool) return;
    sim_.setNav(nav && nav->valid() ? nav : nullptr);
    const u32 agentType = world.componentId("CSynapseAgent");

    // Deterministic order: entity id, not pool order (which swap-removes).
    kept_.clear();
    for (usize i = 0; i < pool->size(); ++i) {
        const scene::Entity e = pool->entityAt(i);
        auto* c = static_cast<CSynapseCrowd*>(pool->dataAt(i));
        if (!c) continue;
        if (!c->enabled || world.destroyPending(e)) { c->active = 0; continue; }
        kept_.push_back(e);
    }
    std::sort(kept_.begin(), kept_.end());

    const usize cap = sim_.params().maxAgents;
    overflow_ = 0;
    if (kept_.size() > cap) {
        overflow_ = static_cast<u32>(kept_.size() - cap);
        for (usize i = cap; i < kept_.size(); ++i)
            if (auto* c = world.component<CSynapseCrowd>(kept_[i], type_)) c->active = 0;
        kept_.resize(cap);
    }

    // Agents whose entity left (disabled, destroyed, over the cap) leave the sim.
    drop_.clear();
    for (const CrowdAgent& a : sim_.agents())
        if (!std::binary_search(kept_.begin(), kept_.end(), a.id)) drop_.push_back(a.id);
    for (const u32 id : drop_) { sim_.removeAgent(id); wander_.erase(id); }

    for (const scene::Entity e : kept_) {
        auto* c = world.component<CSynapseCrowd>(e, type_);
        if (!c) continue;
        const V2 pos = worldXY(world, e);

        CrowdAgent* a = sim_.find(e);
        if (!a) {
            CrowdAgent n;
            n.id = e;
            n.pos = pos;
            if (!sim_.addAgent(n)) { c->active = 0; continue; }
            a = sim_.find(e);
        } else if (drive_ == CrowdDrive::Advise) {
            a->pos = pos;   // the character controller owns the position
        } else if (dist2(a->pos, pos) > std::max(200.0f, c->maxSpeedCm)) {
            a->pos = pos;   // something teleported the entity: believe it
        }

        a->radius = std::max(c->radiusCm, 1.0f);
        a->maxSpeed = std::max(c->maxSpeedCm, 0.0f);
        a->maxAccel = std::max(c->maxAccelCm, 1.0f);
        a->priority = c->priority;
        const CrowdMode mode = static_cast<CrowdMode>(c->mode);
        a->flags = mode == CrowdMode::Hold ? kCrowdStatic : 0u;

        const V2 steer{c->steerXCm, c->steerYCm};
        V2 want{};
        switch (mode) {
            case CrowdMode::FollowAgent: {
                const auto* ag = agentType ? world.component<CSynapseAgent>(e, agentType) : nullptr;
                if (ag && ag->status == static_cast<i32>(AgentStatus::Pathing)) {
                    const V2 target{ag->targetXCm, ag->targetYCm};
                    const V2 goal{ag->goalXCm, ag->goalYCm};
                    // The last leg slows into the goal; earlier legs keep their speed through.
                    if (dist2(target, goal) <= ag->arriveRadiusCm + 25.0f)
                        want = steerArrive(pos, target, a->maxSpeed, c->arriveSlowRadiusCm,
                                           std::min(c->arriveStopRadiusCm, ag->arriveRadiusCm));
                    else
                        want = steerSeek(pos, target, a->maxSpeed);
                }
                break;
            }
            case CrowdMode::Seek:   want = steerSeek(pos, steer, a->maxSpeed); break;
            case CrowdMode::Arrive: want = steerArrive(pos, steer, a->maxSpeed, c->arriveSlowRadiusCm, c->arriveStopRadiusCm); break;
            case CrowdMode::Flee:   want = steerFlee(pos, steer, a->maxSpeed, c->fleePanicRadiusCm); break;
            case CrowdMode::Wander: {
                WanderState& ws = wander_[e];
                if (ws.rng == 1) ws.rng = hashU32(e) | 1u;
                want = steerWander(len2(a->vel) > 1.0f ? a->vel : V2{1.0f, 0.0f}, ws, a->maxSpeed * 0.5f,
                                   200.0f, 100.0f, 3.0f, dt);
                break;
            }
            case CrowdMode::Hold: break;
        }
        a->prefVel = want;
        c->active = 1;
        c->desiredXCm = want.x;
        c->desiredYCm = want.y;
    }

    sim_.advance(dt);

    for (const scene::Entity e : kept_) {
        auto* c = world.component<CSynapseCrowd>(e, type_);
        const CrowdAgent* a = sim_.find(e);
        if (!c || !a) continue;
        c->velXCm = a->vel.x;
        c->velYCm = a->vel.y;
        c->speedCm = len2(a->vel);
        c->stuck = a->stuckSec >= sim_.params().stuckSeconds ? 1 : 0;

        if (drive_ == CrowdDrive::Move && !(a->flags & kCrowdStatic)) {
            f32 z = 0.0f;
            worldXY(world, e, &z);
            if (nav && nav->valid()) {
                u32 gx, gy;
                if (worldToCell(*nav, a->pos.x, a->pos.y, gx, gy))
                    if (const fmt::OcNavCell* cell = nav->at(gx, gy))
                        if (cell->flags & fmt::kOcNavWalkable) z = cell->floorZCm;
            }
            world.setLocalPosition(e, Vec3{a->pos.x, a->pos.y, z});
        }
    }
}

} // namespace aver::synapse
