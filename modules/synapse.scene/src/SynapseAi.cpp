#include "aver/synapse/SynapseAi.hpp"

#include "aver/synapse/SynapseAgent.hpp"   // CSynapseAgent layout and AgentStatus only

#include <cmath>

namespace aver::synapse {
namespace {

SynapseAi& ai(void* user) { return *static_cast<SynapseAi*>(user); }
scene::Entity entityOf(i32 subject) { return static_cast<scene::Entity>(subject); }

Vec3 worldPos(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
}

// Points the path-following agent at a goal. Setting Requested is all AgentSystem::setGoal does
// that matters: its stale-path erase is redundant, since the Requested branch overwrites the path.
bool requestGoal(SynapseAi& a, scene::World& w, scene::Entity e, const Vec3& goal) {
    const u32 t = a.agentType(w);
    auto* ag = t ? w.component<CSynapseAgent>(e, t) : nullptr;
    if (!ag) return false;
    ag->goalXCm = goal.x;
    ag->goalYCm = goal.y;
    ag->goalZCm = goal.z;
    ag->status = static_cast<i32>(AgentStatus::Requested);
    return true;
}

// Running until the agent arrives. A failed path releases `releaseCover` first when asked.
BtStatus goalProgress(SynapseAi& a, scene::World& w, scene::Entity e, bool releaseCoverOnFail) {
    const u32 t = a.agentType(w);
    const auto* ag = t ? w.component<CSynapseAgent>(e, t) : nullptr;
    if (!ag) return BtStatus::Failure;
    switch (static_cast<AgentStatus>(ag->status)) {
        case AgentStatus::Arrived: return BtStatus::Success;
        case AgentStatus::Failed:
            if (releaseCoverOnFail) a.tactics().releaseCover(e);
            return BtStatus::Failure;
        default: return BtStatus::Running;
    }
}

// mode 0: the perceived target first, then heard noise. mode 1: the other way round.
bool resolveThreat(SynapseAi& a, scene::World& w, scene::Entity e, i32 mode, Vec3& out) {
    const auto seen = [&]() {
        const u32 t = a.perceptionType(w);
        const auto* p = t ? w.component<CSynapsePerception>(e, t) : nullptr;
        const auto target = p ? static_cast<scene::Entity>(p->lastKnownTargetEntity) : scene::kInvalidEntity;
        if (target == scene::kInvalidEntity || !w.valid(target)) return false;
        out = worldPos(w, target);
        return true;
    };
    const auto heard = [&]() {
        const HearingMemory* m = a.hearing().memoryOf(e);
        const HeardMemory* best = m ? m->best() : nullptr;
        if (!best) return false;
        out = best->pos;
        return true;
    };
    return mode == 1 ? (heard() || seen()) : (seen() || heard());
}

// ---- conditions -----------------------------------------------------------------------------------

bool heardNoiseCond(i32 subject, const fmt::OcBtNode& node, void* u) {
    const HearingMemory* m = ai(u).hearing().memoryOf(entityOf(subject));
    const HeardMemory* b = m ? m->best() : nullptr;
    return b && b->confidence > node.params[0];
}

bool heardNoiseTagCond(i32 subject, const fmt::OcBtNode& node, void* u) {
    const HearingMemory* m = ai(u).hearing().memoryOf(entityOf(subject));
    if (!m) return false;
    for (const HeardMemory& h : m->entries())
        if (h.tag == static_cast<u32>(node.params[0])) return true;
    return false;
}

bool inCoverCond(i32 subject, const fmt::OcBtNode& node, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    const u32 id = a.tactics().reservations().coverOf(e);
    const CoverPoint* c = id ? a.tactics().covers().find(id) : nullptr;
    if (!c) return false;
    const f32 reach = node.params[0] > 0.0f ? node.params[0] : 80.0f;
    const Vec3 p = worldPos(w, e);
    return dist2(V2{p.x, p.y}, c->pos) <= reach;
}

bool squadRoleIsCond(i32 subject, const fmt::OcBtNode& node, void* u) {
    SynapseAi& a = ai(u);
    const auto* s = scene::World::instance().component<CSynapseSquad>(entityOf(subject), a.tactics().squadType());
    return s && s->role == static_cast<i32>(node.params[0]);
}

// ---- actions --------------------------------------------------------------------------------------
// `elapsed` doubles as the "goal already issued" flag, as MoveTo does: tickBt erases it when the
// node resolves, so the next run starts fresh.

BtStatus investigateNoiseAction(i32 subject, const fmt::OcBtNode&, f32, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    if (elapsed == 0.0f) {
        const HearingMemory* m = a.hearing().memoryOf(e);
        const HeardMemory* best = m ? m->best() : nullptr;
        if (!best || !requestGoal(a, w, e, best->pos)) return BtStatus::Failure;
        elapsed = 1.0f;
    }
    return goalProgress(a, w, e, false);
}

BtStatus takeCoverAction(i32 subject, const fmt::OcBtNode& node, f32, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    if (elapsed == 0.0f) {
        Vec3 threat;
        if (!resolveThreat(a, w, e, static_cast<i32>(node.params[0]), threat)) return BtStatus::Failure;
        CoverSearch s;
        if (node.params[1] > 0.0f) s.maxSeekCm = node.params[1];
        if (node.params[2] > 0.0f) s.minThreatDistCm = node.params[2];
        s.requireHigh = node.params[3] != 0.0f;
        CoverResult r;
        if (!a.tactics().findCover(w, e, threat, s, r)) return BtStatus::Failure;
        if (!requestGoal(a, w, e, Vec3{r.pos.x, r.pos.y, worldPos(w, e).z})) {
            a.tactics().releaseCover(e);
            return BtStatus::Failure;
        }
        elapsed = 1.0f;
    }
    return goalProgress(a, w, e, true);
}

BtStatus leaveCoverAction(i32 subject, const fmt::OcBtNode&, f32, f32&, void* u) {
    ai(u).tactics().releaseCover(entityOf(subject));
    return BtStatus::Success;
}

BtStatus squadFlankAction(i32 subject, const fmt::OcBtNode&, f32, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    if (elapsed == 0.0f) {
        const auto* s = w.component<CSynapseSquad>(e, a.tactics().squadType());
        if (!s || !s->hasSlot) return BtStatus::Failure;
        const Vec3 slot{s->slotXCm, s->slotYCm, s->slotZCm};
        const Vec3 me = worldPos(w, e);
        if (dist2(V2{me.x, me.y}, V2{slot.x, slot.y}) < 50.0f) return BtStatus::Success;
        if (!requestGoal(a, w, e, slot)) return BtStatus::Failure;
        elapsed = 1.0f;
    }
    return goalProgress(a, w, e, false);
}

BtStatus squadKeepSpacingAction(i32 subject, const fmt::OcBtNode&, f32, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    if (elapsed == 0.0f) {
        const V2 push = clampLen2(a.tactics().spacingPush(w, e), 300.0f);
        if (len2(push) < 10.0f) return BtStatus::Success;
        const Vec3 me = worldPos(w, e);
        if (!requestGoal(a, w, e, Vec3{me.x + push.x, me.y + push.y, me.z})) return BtStatus::Failure;
        elapsed = 1.0f;
    }
    return goalProgress(a, w, e, false);
}

BtStatus squadSetTargetAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void* u) {
    SynapseAi& a = ai(u);
    const auto* s = scene::World::instance().component<CSynapseSquad>(entityOf(subject), a.tactics().squadType());
    if (!s) return BtStatus::Failure;
    a.tactics().setSquadTarget(s->squadId, Vec3{node.params[0], node.params[1], node.params[2]});
    return BtStatus::Success;
}

BtStatus crowdSetModeAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void* u) {
    SynapseAi& a = ai(u);
    auto* c = scene::World::instance().component<CSynapseCrowd>(entityOf(subject), a.crowd().componentType());
    if (!c) return BtStatus::Failure;
    c->mode = static_cast<i32>(node.params[0]);
    c->steerXCm = node.params[1];
    c->steerYCm = node.params[2];
    c->steerZCm = node.params[3];
    return BtStatus::Success;
}

} // namespace

SynapseAi::SynapseAi() = default;

void SynapseAi::registerComponents(scene::World& world) {
    crowd_.registerComponents(world);
    hearing_.registerComponents(world);
    tactics_.registerComponents(world);
}

void SynapseAi::tick(scene::World& world, const fmt::OcNavData* nav, f32 dt) {
    hearing_.tick(world, dt);
    tactics_.tick(world, nav, dt);
    crowd_.tick(world, nav, dt);
}

u32 SynapseAi::agentType(scene::World& w) {
    if (!agentType_) agentType_ = w.componentId("CSynapseAgent");
    return agentType_;
}

u32 SynapseAi::perceptionType(scene::World& w) {
    if (!perceptionType_) perceptionType_ = w.componentId("CSynapsePerception");
    return perceptionType_;
}

void SynapseAi::registerBehaviors(BtRegistry& reg) {
    reg.registerCondition("HeardNoise", &heardNoiseCond, this);
    reg.registerCondition("HeardNoiseTag", &heardNoiseTagCond, this);
    reg.registerCondition("InCover", &inCoverCond, this);
    reg.registerCondition("SquadRoleIs", &squadRoleIsCond, this);
    reg.registerAction("InvestigateNoise", &investigateNoiseAction, this);
    reg.registerAction("TakeCover", &takeCoverAction, this);
    reg.registerAction("LeaveCover", &leaveCoverAction, this);
    reg.registerAction("SquadFlank", &squadFlankAction, this);
    reg.registerAction("SquadKeepSpacing", &squadKeepSpacingAction, this);
    reg.registerAction("SquadSetTarget", &squadSetTargetAction, this);
    reg.registerAction("CrowdSetMode", &crowdSetModeAction, this);
}

} // namespace aver::synapse
