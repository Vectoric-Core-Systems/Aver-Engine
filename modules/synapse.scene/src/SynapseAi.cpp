#include "aver/synapse/SynapseAi.hpp"

#include "aver/core/Log.hpp"
#include "aver/synapse/SynapseAgent.hpp"   // CSynapseAgent layout and AgentStatus only

#include <algorithm>
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

// Drops the move request of an aborted path-following action (the agent stops chasing a goal nobody wants).
void cancelGoal(SynapseAi& a, scene::World& w, scene::Entity e) {
    const u32 t = a.agentType(w);
    auto* ag = t ? w.component<CSynapseAgent>(e, t) : nullptr;
    if (!ag) return;
    if (ag->status == static_cast<i32>(AgentStatus::Requested) || ag->status == static_cast<i32>(AgentStatus::Pathing))
        ag->status = static_cast<i32>(AgentStatus::None);
}

void goalAbort(i32 subject, const fmt::OcBtNode&, void* u) {
    cancelGoal(ai(u), scene::World::instance(), entityOf(subject));
}

void takeCoverAbort(i32 subject, const fmt::OcBtNode&, void* u) {
    SynapseAi& a = ai(u);
    cancelGoal(a, scene::World::instance(), entityOf(subject));
    a.tactics().releaseCover(entityOf(subject));   // the point was only claimed to run to
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

// ---- environment queries and smart objects (AI: wave 3 #21) ------------------------------------------

void putValue(Blackboard& b, const char* name, const BbValue& v) {
    if (!b.has(name)) {
        BbKeyDef d;
        d.name = name;
        d.type = v.type;
        d.defaultValue = bbDefault(v.type);
        b.defineKey(d);
    }
    b.set(name, v);
}

// The subject's tags for smart-object requirements: the String board key UserTags, when it has one.
TagList userTagsOf(SynapseAi& a, scene::Entity e) {
    Blackboard* b = a.boardOf(e);
    return b && b->has("UserTags") ? TagList::parse(b->getString("UserTags")) : TagList{};
}

BtStatus runEqsAction(i32 subject, const fmt::OcBtNode& node, f32 dt, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    EqsSystem& q = a.eqs();
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    u32& handle = q.leafRun(subject, &node);
    if (elapsed == 0.0f) {
        const u32 id = q.loadQuery(node.stringParam);
        if (!id) { q.dropLeafRun(subject, &node); return BtStatus::Failure; }
        EqsRunParams p;
        p.querier = static_cast<u32>(subject);
        Blackboard* b = a.boardOf(e);
        if (b && b->has("Target")) {
            p.target = b->getEntity("Target");
        } else {
            const u32 t = a.perceptionType(w);
            const auto* per = t ? w.component<CSynapsePerception>(e, t) : nullptr;
            if (per) p.target = static_cast<u32>(per->lastKnownTargetEntity);
        }
        p.userTags = userTagsOf(a, e);
        p.runId = static_cast<u32>(q.time() * 1000.0) ^ (static_cast<u32>(subject) * 2654435761u);
        handle = q.start(id, p, static_cast<i32>(node.params[1]));
        if (!handle) { q.dropLeafRun(subject, &node); return BtStatus::Failure; }
        elapsed = 1e-6f;
    } else {
        elapsed += dt;
    }
    if (node.params[0] > 0.0f && elapsed > node.params[0]) {
        q.cancel(handle);
        q.release(handle);
        q.dropLeafRun(subject, &node);
        return BtStatus::Failure;
    }
    switch (q.status(handle)) {
        case EqsSystem::Status::Queued:
        case EqsSystem::Status::Running: return BtStatus::Running;
        case EqsSystem::Status::Finished: {
            const EqsResult* res = q.result(handle);
            const u32 queryId = q.queryIdOf(handle);
            const EqsQuery* query = q.query(queryId);
            const u32 count = res ? static_cast<u32>(res->items.size()) : 0;
            q.noteResult(subject, queryId, count);
            if (count > 0 && query) {
                if (Blackboard* b = a.boardOf(e)) {
                    const EqsItem& best = res->items[0];
                    if (!query->out.pos.empty()) putValue(*b, query->out.pos.c_str(), BbValue::ofVec3(best.pos));
                    if (!query->out.entity.empty()) putValue(*b, query->out.entity.c_str(), BbValue::ofEntity(best.entity));
                    if (!query->out.score.empty()) putValue(*b, query->out.score.c_str(), BbValue::ofFloat(best.score));
                    if (!query->out.count.empty()) putValue(*b, query->out.count.c_str(), BbValue::ofInt(count));
                }
            }
            q.release(handle);
            q.dropLeafRun(subject, &node);
            return count > 0 ? BtStatus::Success : BtStatus::Failure;
        }
        default:
            q.release(handle);
            q.dropLeafRun(subject, &node);
            return BtStatus::Failure;
    }
}

void runEqsAbort(i32 subject, const fmt::OcBtNode& node, void* u) {
    EqsSystem& q = ai(u).eqs();
    const u32 handle = q.leafRun(subject, &node);
    if (handle) {
        q.cancel(handle);
        q.release(handle);
    }
    q.dropLeafRun(subject, &node);
}

bool eqsHasResultCond(i32 subject, const fmt::OcBtNode& node, void* u) {
    EqsSystem& q = ai(u).eqs();
    const u32 id = q.loadQuery(node.stringParam);
    const EqsSystem::LastResult* r = id ? q.lastResult(subject, id) : nullptr;
    return r && r->count >= 1 && (node.params[0] <= 0.0f || q.time() - r->time <= static_cast<double>(node.params[0]));
}

BtStatus claimSmartObjectAction(i32 subject, const fmt::OcBtNode& node, f32, f32&, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    SmartObjectSystem& so = a.smartObjects();
    SoQuery q;
    q.origin = worldPos(w, e);
    q.radius = node.params[0] > 0.0f ? node.params[0] : 1500.0f;
    q.slotTags = TagList::parse(node.stringParam);
    q.matchAll = node.params[2] != 0.0f;
    q.userTags = userTagsOf(a, e);
    q.max = 8;
    SoSlotRef refs[8];
    const u32 n = so.find(w, q, refs, nullptr, 8);
    const f32 ttl = node.params[1] > 0.0f ? node.params[1] : -1.0f;   // <= 0: the slot's own CLAIMTTL
    for (u32 i = 0; i < n; ++i)
        if (so.claim(w, e, refs[i], q.userTags, ttl, nullptr) != 0) return BtStatus::Success;
    return BtStatus::Failure;
}

BtStatus moveToSmartObjectAction(i32 subject, const fmt::OcBtNode& node, f32, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    SmartObjectSystem& so = a.smartObjects();
    u32 handle = 0;
    if (so.claimsOf(e, &handle, 1) == 0) return BtStatus::Failure;
    SoClaimInfo info;
    if (!so.claimInfo(handle, info)) return BtStatus::Failure;
    const f32 reach = node.params[0] > 0.0f ? node.params[0] : 60.0f;
    const Vec3 me = worldPos(w, e);
    if (dist2(V2{me.x, me.y}, V2{info.world.entry.x, info.world.entry.y}) <= reach) return BtStatus::Success;
    if (elapsed == 0.0f) {
        if (!requestGoal(a, w, e, info.world.entry)) return BtStatus::Failure;
        elapsed = 1.0f;
    }
    return goalProgress(a, w, e, false);
}

BtStatus useSmartObjectAction(i32 subject, const fmt::OcBtNode& node, f32, f32& elapsed, void* u) {
    SynapseAi& a = ai(u);
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOf(subject);
    SmartObjectSystem& so = a.smartObjects();
    u32& held = so.leafClaim(subject, &node);
    if (elapsed == 0.0f) {
        u32 hs[kSoMaxClaimsPerUser];
        const u32 n = so.claimsOf(e, hs, kSoMaxClaimsPerUser);
        if (n == 0) { so.dropLeafClaim(subject, &node); return BtStatus::Failure; }
        held = hs[0];
        SoClaimInfo first;
        if (so.claimInfo(held, first) && first.state == SoSlotState::Claimed && !so.use(w, held)) {
            so.dropLeafClaim(subject, &node);
            return BtStatus::Failure;
        }
        elapsed = 1.0f;
    }
    SoClaimInfo info;
    if (so.claimInfo(held, info)) return BtStatus::Running;
    SoReleaseReason why = SoReleaseReason::Released;
    const bool known = so.endedReason(held, why);
    so.dropLeafClaim(subject, &node);
    return known && why == SoReleaseReason::Completed ? BtStatus::Success : BtStatus::Failure;
}

void useSmartObjectAbort(i32 subject, const fmt::OcBtNode& node, void* u) {
    SmartObjectSystem& so = ai(u).smartObjects();
    const u32 held = so.leafClaim(subject, &node);
    if (held) so.release(scene::World::instance(), held, SoReleaseReason::Released);
    so.dropLeafClaim(subject, &node);
}

BtStatus releaseSmartObjectAction(i32 subject, const fmt::OcBtNode&, f32, f32&, void* u) {
    SmartObjectSystem& so = ai(u).smartObjects();
    u32 hs[kSoMaxClaimsPerUser];
    const u32 n = so.claimsOf(entityOf(subject), hs, kSoMaxClaimsPerUser);
    for (u32 i = 0; i < n; ++i) so.release(scene::World::instance(), hs[i], SoReleaseReason::Released);
    return BtStatus::Success;
}

bool hasSmartObjectClaimCond(i32 subject, const fmt::OcBtNode& node, void* u) {
    SmartObjectSystem& so = ai(u).smartObjects();
    u32 hs[kSoMaxClaimsPerUser];
    const u32 n = so.claimsOf(entityOf(subject), hs, kSoMaxClaimsPerUser);
    for (u32 i = 0; i < n; ++i) {
        SoClaimInfo info;
        if (so.claimInfo(hs[i], info) && (node.params[0] == 0.0f || info.state == SoSlotState::Occupied)) return true;
    }
    return false;
}

} // namespace

SynapseAi::SynapseAi() {
    eqs_.bind(this);
    smart_.bind(this);
}

void SynapseAi::registerComponents(scene::World& world) {
    crowd_.registerComponents(world);
    hearing_.registerComponents(world);
    tactics_.registerComponents(world);
    smart_.registerComponents(world);   // AI: wave 3 #21
}

void SynapseAi::tick(scene::World& world, const fmt::OcNavData* nav, f32 dt) {
    CallScope scope(*this);
    hearing_.tick(world, dt);
    tactics_.tick(world, nav, dt);
    crowd_.tick(world, nav, dt);
    smart_.tick(world, dt);             // AI: wave 3 #21
    eqs_.tick(world, nav, dt, *this);
}

void SynapseAi::reset() {
    crowd_.reset();
    hearing_.reset();
    tactics_.reset();
    eqs_.reset();       // AI: wave 3 #21
    smart_.reset();
    queued_.clear();
}

void SynapseAi::raise(scene::Entity e, const char* name) {
    if (!notify_ || !name || e == scene::kInvalidEntity) return;
    queued_.push_back(QueuedEvent{e, name});
    if (depth_ == 0) flushEvents();
}

void SynapseAi::flushEvents() {
    if (queued_.empty()) return;
    if (flushDepth_ >= 8) {
        AVER_WARN("[Synapse] event handlers keep calling back into the AI (depth 8): {} events dropped", queued_.size());
        queued_.clear();
        return;
    }
    ++flushDepth_;
    std::vector<QueuedEvent> batch;
    batch.swap(queued_);
    for (const QueuedEvent& ev : batch)
        if (notify_ && scene::World::instance().valid(ev.e)) notify_(ev.e, ev.name.c_str(), notifyUser_);
    --flushDepth_;
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
    reg_ = &reg;
    managed_.attachRegistry(&reg);
    reg.registerCondition("HeardNoise", &heardNoiseCond, this);
    reg.registerCondition("HeardNoiseTag", &heardNoiseTagCond, this);
    reg.registerCondition("InCover", &inCoverCond, this);
    reg.registerCondition("SquadRoleIs", &squadRoleIsCond, this);
    reg.registerAction("InvestigateNoise", &investigateNoiseAction, this, &goalAbort);
    reg.registerAction("TakeCover", &takeCoverAction, this, &takeCoverAbort);
    reg.registerAction("LeaveCover", &leaveCoverAction, this);
    reg.registerAction("SquadFlank", &squadFlankAction, this, &goalAbort);
    reg.registerAction("SquadKeepSpacing", &squadKeepSpacingAction, this, &goalAbort);
    reg.registerAction("SquadSetTarget", &squadSetTargetAction, this);
    reg.registerAction("CrowdSetMode", &crowdSetModeAction, this);
    // AI: wave 3 #21
    reg.registerAction("RunEqsQuery", &runEqsAction, this, &runEqsAbort);
    reg.registerCondition("EqsHasResult", &eqsHasResultCond, this);
    reg.registerAction("ClaimSmartObject", &claimSmartObjectAction, this);
    reg.registerAction("MoveToSmartObject", &moveToSmartObjectAction, this, &goalAbort);
    reg.registerAction("UseSmartObject", &useSmartObjectAction, this, &useSmartObjectAbort);
    reg.registerAction("ReleaseSmartObject", &releaseSmartObjectAction, this);
    reg.registerCondition("HasSmartObjectClaim", &hasSmartObjectClaimCond, this);
}

} // namespace aver::synapse
