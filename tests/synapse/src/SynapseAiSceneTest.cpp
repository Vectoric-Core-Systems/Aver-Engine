// The AI systems against a real scene::World: hearing listeners and their memory, crowd agents in
// both drive modes and under a max-agents cap, cover markers and reservations that follow their
// entities, squad slots, and the behaviour-tree actions that tie them to path following.
//
// Hearing's occlusion is replaced with a fake in every block, so this needs no physics world.
#include "SynapseAiTestUtil.hpp"

#include "aver/scene/World.hpp"
#include "aver/synapse/HearingBlackboardSink.hpp"
#include "aver/synapse/SynapseAgent.hpp"
#include "aver/synapse/SynapseAi.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::synapse;
using aitest::check;

namespace {

bool approx(f32 a, f32 b, f32 eps = 1e-2f) { return std::fabs(a - b) <= eps; }

void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

scene::Entity spawn(scene::World& w, const char* name, Vec3 p) {
    Transform xf;
    xf.position = p;
    return w.create(name, scene::kInvalidEntity, xf);
}

Vec3 posOf(scene::World& w, scene::Entity e) { return w.localTransform(e).position; }

u32 noOcclusion(void*, const Vec3&, const Vec3&) { return 0; }

struct CountingSink final : IHearingMemorySink {
    int heard = 0, forgotten = 0;
    u32 lastListener = 0;
    void onHeard(u32 l, const HeardMemory&) override { ++heard; lastListener = l; }
    void onForgotten(u32 l, const HeardMemory&) override { ++forgotten; lastListener = l; }
};

int g_notifies = 0;
void notifyFn(scene::Entity, const char* name, void*) {
    if (std::string(name) == "OnHearNoise") ++g_notifies;
}

fmt::OcNavData splitMap() {
    std::vector<std::string> rows;
    for (int y = 0; y < 7; ++y) {
        std::string row(15, '.');
        row[7] = '#';
        rows.push_back(row);
    }
    return aitest::gridFrom(rows);
}

// Runs a registered action once, the way tickBt would.
BtStatus runAction(const BtRegistry& reg, const char* name, i32 subject, const fmt::OcBtNode& node, f32& elapsed) {
    const auto* a = reg.findAction(name);
    return a && a->fn ? a->fn(subject, node, 0.033f, elapsed, a->user) : BtStatus::Failure;
}

fmt::OcBtNode nodeWith(const char* name, f32 p0 = 0, f32 p1 = 0, f32 p2 = 0, f32 p3 = 0) {
    fmt::OcBtNode n;
    n.kind = fmt::OcBtNodeKind::Action;
    n.name = name;
    n.params[0] = p0; n.params[1] = p1; n.params[2] = p2; n.params[3] = p3;
    return n;
}

} // namespace

int main() {
    AVER_INFO("SynapseAiSceneTest");
    scene::World& w = scene::World::instance();
    agentSystem().registerComponents(w);
    perceptionSystem().registerComponents(w);

    SynapseAi ai;
    ai.registerComponents(w);
    check(ai.crowd().componentType() != 0 && ai.hearing().componentType() != 0 &&
              ai.tactics().markerType() != 0 && ai.tactics().squadType() != 0,
          "every AI component registers");
    ai.hearing().setOcclusion(&noOcclusion, nullptr);

    AVER_INFO("hearing: range, memory, decay, sink and notify");
    {
        clearWorld(w);
        ai.reset();
        CountingSink sink;
        ai.hearing().setMemorySink(&sink);
        ai.hearing().setNotifySink(&notifyFn, nullptr);
        g_notifies = 0;

        const scene::Entity near1 = spawn(w, "Near", {500, 0, 0});
        const scene::Entity far1 = spawn(w, "Far", {4000, 0, 0});
        const scene::Entity emitter = spawn(w, "Emitter", {0, 0, 0});
        ai.hearing().attach(w, near1);
        ai.hearing().attach(w, far1);
        auto* self = ai.hearing().attach(w, emitter);
        check(self != nullptr, "an entity takes CSynapseHearing");
        ai.hearing().attach(w, emitter);   // idempotent: must not reset the configuration
        self->sensitivity = 3.0f;
        check(w.component<CSynapseHearing>(emitter, ai.hearing().componentType())->sensitivity == 3.0f,
              "attaching twice leaves the first configuration alone");
        self->sensitivity = 1.0f;

        NoiseEvent ev;
        ev.pos = Vec3{0, 0, 0};
        ev.loudnessCm = 1000.0f;
        ev.tag = 1;
        ev.source = emitter;
        check(ai.hearing().emit(ev), "a noise is queued");
        check(ai.hearing().pendingCount() == 1, "and waits for the tick");
        ai.hearing().tick(w, 0.1f);

        const auto* hn = w.component<CSynapseHearing>(near1, ai.hearing().componentType());
        const auto* hf = w.component<CSynapseHearing>(far1, ai.hearing().componentType());
        const auto* he = w.component<CSynapseHearing>(emitter, ai.hearing().componentType());
        check(hn && hn->hasMemory == 1 && approx(hn->heardLevel, 0.5f, 0.05f), "the near listener heard it at about half level");
        check(hn && approx(hn->heardXCm, 0.0f) && hn->heardTag == 1 && hn->heardSource == static_cast<i32>(emitter),
              "and remembers where, what and who");
        check(hf && hf->hasMemory == 0 && hf->timeSinceHeardSec < 0.0f, "the far listener heard nothing");
        check(he && he->hasMemory == 0, "a source does not hear its own noise");
        check(sink.heard == 1 && sink.lastListener == near1 && g_notifies == 1, "the sink and notify fired once, for the near listener");
        check(ai.hearing().pendingCount() == 0, "the queue drained");

        ai.hearing().tick(w, 4.0f);
        check(hn->hasMemory == 1 && approx(hn->confidence, 0.5f, 0.02f) && approx(hn->timeSinceHeardSec, 4.0f, 0.02f),
              "four seconds later the memory is half as confident");
        ai.hearing().tick(w, 5.0f);
        check(hn->hasMemory == 0 && hn->confidence == 0.0f && sink.forgotten == 1, "after memorySec it is forgotten, and the sink says so");

        ev.source = 0;
        ev.tag = 7;
        ai.hearing().emit(ev);
        auto* mutableNear = w.component<CSynapseHearing>(near1, ai.hearing().componentType());
        mutableNear->tagMask = 1 << 2;
        ai.hearing().tick(w, 0.1f);
        check(mutableNear->hasMemory == 0, "a tag outside the listener's mask is ignored");

        for (u32 i = 0; i < 300; ++i) ai.hearing().emit(ev);
        check(ai.hearing().pendingCount() == 256 && ai.hearing().droppedCount() == 44,
              "the queue is bounded at 256 and counts the 44 it dropped");
        ai.hearing().tick(w, 0.1f);

        ai.hearing().setMemorySink(nullptr);
        ai.hearing().setNotifySink(nullptr, nullptr);
    }

    AVER_INFO("hearing: the blackboard sink mirrors the strongest memory");
    {
        clearWorld(w);
        ai.reset();
        const scene::Entity listener = spawn(w, "Listener", {300, 0, 0});
        ai.hearing().attach(w, listener);
        Blackboard board;
        HearingBlackboardSink::defineKeys(board);
        struct Bound { u32 entity; Blackboard* board; } bound{listener, &board};
        HearingBlackboardSink sink(
            [](u32 l, void* u) -> Blackboard* { auto* b = static_cast<Bound*>(u); return l == b->entity ? b->board : nullptr; },
            &bound);
        ai.hearing().setMemorySink(&sink);
        check(!board.getBool("Heard.Valid"), "nothing is heard yet");

        NoiseEvent ev;
        ev.pos = Vec3{0, 0, 0};
        ev.loudnessCm = 1000.0f;
        ev.tag = 4;
        ev.source = 77;
        ai.hearing().emit(ev);
        ai.hearing().tick(w, 0.1f);
        check(board.getBool("Heard.Valid") && board.getInt("Heard.Tag") == 4 && board.getEntity("Heard.Source") == 77u,
              "a noise sets Valid, Tag and Source on the board");
        check(approx(board.getVec3("Heard.Position").x, 0.0f) && board.getFloat("Heard.Level") > 0.3f &&
                  approx(board.getFloat("Heard.Confidence"), 1.0f),
              "and its position, level and confidence");

        int changes = 0;
        board.observe("Heard.Valid", [&](std::string_view, const BbValue&, const BbValue&) { ++changes; });
        ai.hearing().tick(w, 9.0f);
        check(!board.getBool("Heard.Valid") && changes == 1, "forgetting it clears Valid and fires the key's observer once");
        ai.hearing().setMemorySink(nullptr);
    }

    AVER_INFO("crowd: Move drive, head-on swap");
    {
        clearWorld(w);
        ai.reset();
        ai.crowd().setDrive(CrowdDrive::Move);
        ai.crowd().setMaxAgents(1024);
        const scene::Entity a = spawn(w, "A", {100, 100, 0});
        const scene::Entity b = spawn(w, "B", {700, 100, 0});
        check(ai.crowd().attach(w, a) && ai.crowd().attach(w, b), "entities take CSynapseCrowd");
        // Fetched after both attaches: adding to a pool can move its storage.
        CSynapseCrowd* ca = w.component<CSynapseCrowd>(a, ai.crowd().componentType());
        CSynapseCrowd* cb = w.component<CSynapseCrowd>(b, ai.crowd().componentType());
        ca->radiusCm = cb->radiusCm = 30.0f;
        ca->maxSpeedCm = cb->maxSpeedCm = 200.0f;
        ca->mode = cb->mode = static_cast<i32>(CrowdMode::Arrive);
        ca->steerXCm = 700.0f; ca->steerYCm = 100.0f;
        cb->steerXCm = 100.0f; cb->steerYCm = 100.0f;

        f32 closest = 1e9f;
        for (int i = 0; i < 450; ++i) {
            ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
            const Vec3 pa = posOf(w, a), pb = posOf(w, b);
            closest = std::min(closest, std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y)));
        }
        check(posOf(w, a).x > 640.0f && posOf(w, b).x < 160.0f, "they swapped places");
        check(closest >= 2.0f * 30.0f * 0.9f, "without ever overlapping by more than 10%");
        check(ca->active == 1 && cb->active == 1 && ai.crowd().simulatedCount() == 2, "both are simulated");

        w.destroy(b);
        w.flush();
        ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        check(ai.crowd().simulatedCount() == 1, "a destroyed entity leaves the simulation");

        ai.crowd().setMaxAgents(1);
        const scene::Entity c = spawn(w, "C", {900, 900, 0});
        CSynapseCrowd* cc = ai.crowd().attach(w, c);
        ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        check(ai.crowd().simulatedCount() == 1 && ai.crowd().overflowCount() == 1, "past the cap, the extra agent is left out and counted");
        const auto* lowId = w.component<CSynapseCrowd>(a < c ? a : c, ai.crowd().componentType());
        const auto* highId = w.component<CSynapseCrowd>(a < c ? c : a, ai.crowd().componentType());
        check(lowId->active == 1 && highId->active == 0 && cc != nullptr, "the lowest entity id is the one kept");
        ai.crowd().setMaxAgents(1024);
    }

    AVER_INFO("crowd: Advise drive moves nothing and publishes a velocity");
    {
        clearWorld(w);
        ai.reset();
        ai.crowd().setDrive(CrowdDrive::Advise);
        const scene::Entity a = spawn(w, "A", {100, 100, 0});
        CSynapseCrowd* c = ai.crowd().attach(w, a);
        c->mode = static_cast<i32>(CrowdMode::Seek);
        c->steerXCm = 900.0f; c->steerYCm = 100.0f;
        for (int i = 0; i < 10; ++i) ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        const Vec3 p = posOf(w, a);
        check(approx(p.x, 100.0f) && approx(p.y, 100.0f), "the entity did not move");
        check(c->velXCm > 50.0f && c->speedCm > 50.0f && c->desiredXCm > 100.0f, "but its velocity and desired velocity were published");
        c->mode = static_cast<i32>(CrowdMode::Hold);
        for (int i = 0; i < 30; ++i) ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        check(c->speedCm < 1.0f, "Hold brings it to a stop");
        c->enabled = 0;
        ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        check(c->active == 0 && ai.crowd().simulatedCount() == 0, "a disabled agent is not simulated");
    }

    AVER_INFO("crowd: FollowAgent steers at the path target");
    {
        clearWorld(w);
        ai.reset();
        ai.crowd().setDrive(CrowdDrive::Advise);
        const scene::Entity a = spawn(w, "A", {0, 0, 0});
        agentSystem().attach(w, a);
        CSynapseCrowd* c = ai.crowd().attach(w, a);
        auto* ag = w.component<CSynapseAgent>(a, agentSystem().componentType());
        check(c && ag && approx(c->radiusCm, ag->radiusCm), "attach took its radius from the path agent");
        ag->status = static_cast<i32>(AgentStatus::Pathing);
        ag->targetXCm = 400.0f; ag->targetYCm = 0.0f;
        ag->goalXCm = 2000.0f; ag->goalYCm = 0.0f;
        ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        check(c->desiredXCm > 300.0f && approx(c->desiredYCm, 0.0f), "it heads for the current waypoint at speed");
        ag->status = static_cast<i32>(AgentStatus::Arrived);
        ai.crowd().tick(w, nullptr, 1.0f / 30.0f);
        check(approx(c->desiredXCm, 0.0f) && approx(c->desiredYCm, 0.0f), "with no path it asks for nothing");
    }

    AVER_INFO("tactics: markers, reservations that follow their owners, squads");
    {
        clearWorld(w);
        ai.reset();
        const fmt::OcNavData nav = splitMap();

        const scene::Entity m1 = spawn(w, "M1", {325, 175, 0});
        const scene::Entity m2 = spawn(w, "M2", {325, 75, 0});
        check(ai.tactics().attachMarker(w, m1) != nullptr && ai.tactics().attachMarker(w, m2) != nullptr, "markers attach");
        ai.tactics().tick(w, &nav, 0.033f);
        check(ai.tactics().covers().points().size() == 2, "each marker became a cover point");

        w.setLocalPosition(m2, Vec3{325, 275, 0});
        ai.tactics().tick(w, &nav, 0.033f);
        bool moved = false;
        for (const CoverPoint& p : ai.tactics().covers().points()) moved = moved || approx(p.pos.y, 275.0f);
        check(moved && ai.tactics().covers().points().size() == 2, "moving a marker moved its point, with no duplicate");

        const scene::Entity s1 = spawn(w, "S1", {300, 175, 0});
        const scene::Entity s2 = spawn(w, "S2", {300, 170, 0});
        CoverResult r1, r2;
        CoverSearch search;
        search.minThreatDistCm = 100.0f;
        search.reserveSec = 0.0f;
        check(ai.tactics().findCover(w, s1, Vec3{600, 175, 0}, search, r1), "the first seeker finds cover");
        check(ai.tactics().findCover(w, s2, Vec3{600, 175, 0}, search, r2) && r2.id != r1.id,
              "the second is given a different point");
        check(ai.tactics().isCovered(w, s1, Vec3{600, 175, 0}), "a seeker standing near its point counts as covered");
        check(!ai.tactics().isCovered(w, s1, Vec3{100, 175, 0}), "but not against a threat on its own side");

        w.destroy(s2);
        w.flush();
        ai.tactics().tick(w, &nav, 0.033f);
        CoverResult r3;
        const scene::Entity s3 = spawn(w, "S3", {300, 170, 0});
        check(ai.tactics().findCover(w, s3, Vec3{600, 175, 0}, search, r3) && r3.id == r2.id,
              "a destroyed seeker's reservation was freed for the next one");

        w.destroy(m1);
        w.flush();
        ai.tactics().tick(w, &nav, 0.033f);
        check(ai.tactics().covers().points().size() == 1 && ai.tactics().reservations().ownerOf(r1.id) == 0,
              "destroying a marker removes its point and the claim on it");

        // Squads.
        clearWorld(w);
        ai.reset();
        const scene::Entity q1 = spawn(w, "Q1", {1000, 0, 0});
        const scene::Entity q2 = spawn(w, "Q2", {1000, 100, 0});
        const scene::Entity q3 = spawn(w, "Q3", {1100, 0, 0});
        const scene::Entity q4 = spawn(w, "Q4", {1300, 0, 0});
        for (const scene::Entity e : {q1, q2, q3, q4}) ai.tactics().attachSquad(w, e)->squadId = 5;
        ai.tactics().tick(w, nullptr, 0.033f);
        check(w.component<CSynapseSquad>(q1, ai.tactics().squadType())->hasSlot == 0, "no target, no slots");
        ai.tactics().setSquadTarget(5, Vec3{0, 0, 0});
        ai.tactics().tick(w, nullptr, 0.033f);
        const auto* c1 = w.component<CSynapseSquad>(q1, ai.tactics().squadType());
        const auto* c2 = w.component<CSynapseSquad>(q2, ai.tactics().squadType());
        const auto* c3 = w.component<CSynapseSquad>(q3, ai.tactics().squadType());
        const auto* c4 = w.component<CSynapseSquad>(q4, ai.tactics().squadType());
        check(c1->role == static_cast<i32>(SquadRole::Anchor), "the nearest member anchors");
        check(c2->role == static_cast<i32>(SquadRole::FlankLeft) && c3->role == static_cast<i32>(SquadRole::FlankRight) &&
                  c4->role == static_cast<i32>(SquadRole::Support),
              "then the flankers, then support");
        check(c1->hasSlot && approx(c1->slotXCm, 1000.0f), "the anchor holds where it stands");
        check(c2->slotYCm < -100.0f && c3->slotYCm > 100.0f, "the flankers' slots are on opposite sides");
        ai.tactics().clearSquadTarget(5);
        ai.tactics().tick(w, nullptr, 0.033f);
        check(c1->hasSlot == 0 && c1->role == 0, "clearing the target clears the slots");
    }

    AVER_INFO("behaviours: registered, and they drive path following");
    {
        clearWorld(w);
        ai.reset();
        const fmt::OcNavData nav = splitMap();
        BtRegistry reg;
        ai.registerBehaviors(reg);
        for (const char* n : {"InvestigateNoise", "TakeCover", "LeaveCover", "SquadFlank", "SquadKeepSpacing", "SquadSetTarget", "CrowdSetMode"})
            check(reg.findAction(n) != nullptr, std::string("action registered: ") + n);
        for (const char* n : {"HeardNoise", "HeardNoiseTag", "InCover", "SquadRoleIs"})
            check(reg.findCondition(n) != nullptr, std::string("condition registered: ") + n);

        const scene::Entity guard = spawn(w, "Guard", {300, 175, 0});
        agentSystem().attach(w, guard);
        ai.hearing().attach(w, guard);
        auto* ag = w.component<CSynapseAgent>(guard, agentSystem().componentType());

        const auto* heardCond = reg.findCondition("HeardNoise");
        check(heardCond && !heardCond->fn(static_cast<i32>(guard), nodeWith("HeardNoise"), heardCond->user),
              "HeardNoise is false before anything is heard");

        f32 elapsed = 0.0f;
        check(runAction(reg, "InvestigateNoise", static_cast<i32>(guard), nodeWith("InvestigateNoise"), elapsed) == BtStatus::Failure,
              "InvestigateNoise fails with nothing remembered");

        NoiseEvent ev;
        ev.pos = Vec3{600, 175, 0};
        ev.loudnessCm = 1000.0f;
        ev.tag = 2;
        ai.hearing().emit(ev);
        ai.tick(w, &nav, 0.033f);
        check(heardCond->fn(static_cast<i32>(guard), nodeWith("HeardNoise"), heardCond->user), "HeardNoise is true after a noise");
        const auto* tagCond = reg.findCondition("HeardNoiseTag");
        check(tagCond->fn(static_cast<i32>(guard), nodeWith("HeardNoiseTag", 2), tagCond->user) &&
                  !tagCond->fn(static_cast<i32>(guard), nodeWith("HeardNoiseTag", 3), tagCond->user),
              "HeardNoiseTag tells tags apart");

        elapsed = 0.0f;
        check(runAction(reg, "InvestigateNoise", static_cast<i32>(guard), nodeWith("InvestigateNoise"), elapsed) == BtStatus::Running,
              "InvestigateNoise runs");
        check(ag->status == static_cast<i32>(AgentStatus::Requested) && approx(ag->goalXCm, 600.0f) && approx(ag->goalYCm, 175.0f),
              "by asking the path agent for the noise's position");
        ag->status = static_cast<i32>(AgentStatus::Pathing);
        check(runAction(reg, "InvestigateNoise", static_cast<i32>(guard), nodeWith("InvestigateNoise"), elapsed) == BtStatus::Running,
              "still running while it walks");
        ag->status = static_cast<i32>(AgentStatus::Arrived);
        check(runAction(reg, "InvestigateNoise", static_cast<i32>(guard), nodeWith("InvestigateNoise"), elapsed) == BtStatus::Success,
              "and succeeds on arrival");

        // Cover: the noise at (600, 175) is the threat; the wall is between.
        ai.tactics().covers().addAuthored({325, 175}, {1, 0});
        ai.tick(w, &nav, 0.033f);
        elapsed = 0.0f;
        ag->status = static_cast<i32>(AgentStatus::None);
        check(runAction(reg, "TakeCover", static_cast<i32>(guard), nodeWith("TakeCover", 1, 0, 100), elapsed) == BtStatus::Running,
              "TakeCover claims a point and starts walking to it");
        check(approx(ag->goalXCm, 325.0f) && approx(ag->goalYCm, 175.0f), "toward the cover point");
        check(ai.tactics().reservations().coverOf(guard) != 0, "which is reserved for the guard");
        ag->status = static_cast<i32>(AgentStatus::Arrived);
        check(runAction(reg, "TakeCover", static_cast<i32>(guard), nodeWith("TakeCover", 1, 0, 100), elapsed) == BtStatus::Success,
              "it succeeds on arrival");
        w.setLocalPosition(guard, Vec3{325, 175, 0});
        const auto* inCover = reg.findCondition("InCover");
        check(inCover->fn(static_cast<i32>(guard), nodeWith("InCover"), inCover->user), "InCover is true standing on the point");
        f32 unused = 0.0f;
        check(runAction(reg, "LeaveCover", static_cast<i32>(guard), nodeWith("LeaveCover"), unused) == BtStatus::Success &&
                  ai.tactics().reservations().coverOf(guard) == 0,
              "LeaveCover releases it");
        check(!inCover->fn(static_cast<i32>(guard), nodeWith("InCover"), inCover->user), "so InCover is false again");

        // A failed path to the cover gives the reservation back.
        elapsed = 0.0f;
        ag->status = static_cast<i32>(AgentStatus::None);
        runAction(reg, "TakeCover", static_cast<i32>(guard), nodeWith("TakeCover", 1, 0, 100), elapsed);
        ag->status = static_cast<i32>(AgentStatus::Failed);
        check(runAction(reg, "TakeCover", static_cast<i32>(guard), nodeWith("TakeCover", 1, 0, 100), elapsed) == BtStatus::Failure &&
                  ai.tactics().reservations().coverOf(guard) == 0,
              "a failed path fails the action and frees the point");

        // CrowdSetMode.
        CSynapseCrowd* cc = ai.crowd().attach(w, guard);
        f32 e2 = 0.0f;
        check(runAction(reg, "CrowdSetMode", static_cast<i32>(guard), nodeWith("CrowdSetMode", static_cast<f32>(CrowdMode::Flee), 10, 20, 30), e2) == BtStatus::Success &&
                  cc->mode == static_cast<i32>(CrowdMode::Flee) && approx(cc->steerYCm, 20.0f),
              "CrowdSetMode sets the mode and its target");
    }

    return aitest::g_failures == 0 ? 0 : 1;
}
