// CSynapsePerception + PerceptionSystem::tick -- a sight cone, one occlusion raycast, and
// OnSeeTarget firing exactly once per acquisition.
//
// A REAL Jolt physics world, unlike AgentTest.cpp (which never needed one): the occlusion check is
// the whole point of this test, and Aver.Synapse (the pure pathfinder) has no raycast of its own to
// fake it with -- see SynapsePerception.hpp's own header for why this module links Aver.Physics
// directly (a plain DEPS edge, not a relay) while it must never link Aver.Framework.
#include "aver/synapse/SynapsePerception.hpp"

#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

// The test's own target resolver: a fixed entity handle, set before each tick() call rather than
// discovered through aver_fw_controlled_pawn/aver_fw_player_controller -- this test has no
// scripting host and no Framework link, exactly the constraint the resolver seam exists to work
// around (see SynapsePerception.hpp's own comment on TargetResolverFn).
static scene::Entity g_testTarget = scene::kInvalidEntity;
static scene::Entity resolveTestTarget(void*) { return g_testTarget; }

static int g_notifyCount = 0;
static void countNotify(scene::Entity, const char* name, void*) {
    if (name && std::string(name) == "OnSeeTarget") ++g_notifyCount;
}

int main() {
    AVER_INFO("PerceptionTest");
    scene::World& w = scene::World::instance();

    const u32 type = synapse::perceptionSystem().registerComponents(w);
    check(type != 0, "CSynapsePerception registers");

    synapse::perceptionSystem().setTargetResolver(&resolveTestTarget, nullptr);
    synapse::perceptionSystem().setNotifySink(&countNotify, nullptr);

    check(aver_phys_init() == 1, "physics world starts");

    // Observer at the origin facing +X (Transform's default rotation is identity, which this
    // engine's own convention maps to yaw=0 -- see SynapsePerception.cpp's worldForwardOf comment).
    // Target 500cm straight ahead, close enough to be well inside both the default sightRangeCm
    // (3000) and the default 45-degree half-angle cone (the eye-to-target ray is ~18 degrees off
    // the observer's horizontal forward, from the eyeHeightCm offset alone).
    const Vec3 observerPos{0, 0, 0};
    const Vec3 targetPos{500, 0, 0};
    const f32 dt = 0.05f;   // 4 ticks covers the default 0.2s thinkIntervalSec exactly

    AVER_INFO("an agent sees a target in the open, and OnSeeTarget fires exactly once per acquisition");
    {
        clearWorld(w);
        g_notifyCount = 0;

        Transform txf; txf.position = targetPos;
        const scene::Entity target = w.create("Target", scene::kInvalidEntity, txf);
        g_testTarget = target;

        Transform oxf; oxf.position = observerPos;
        const scene::Entity observer = w.create("Observer", scene::kInvalidEntity, oxf);
        auto* p = synapse::perceptionSystem().attach(w, observer);
        check(p != nullptr, "the observer takes CSynapsePerception");
        if (p) check(p->canSeeTarget == 0, "canSeeTarget starts false, before any tick has run");

        // Tick past the think interval -- the first think-tick should acquire the target.
        for (int i = 0; i < 5; ++i) synapse::perceptionSystem().tick(w, dt);

        p = w.component<synapse::CSynapsePerception>(observer, type);
        check(p && p->canSeeTarget != 0, "canSeeTarget is true once acquired");
        check(p && p->lastKnownTargetEntity == static_cast<i32>(target),
              "lastKnownTargetEntity records which entity was seen");
        check(p && p->timeSinceSeenSec >= 0.0f && p->timeSinceSeenSec < dt * 5.0f + 1e-3f,
              "timeSinceSeenSec resets to (near) 0 on the acquiring tick");
        check(g_notifyCount == 1, "OnSeeTarget fired exactly once so far");

        // Many MORE think-ticks while still visible must NOT refire -- the whole point of "exactly
        // once per acquisition", not "once per think-tick the target happens to still be visible".
        for (int i = 0; i < 40; ++i) synapse::perceptionSystem().tick(w, dt);
        check(g_notifyCount == 1,
              "AND STILL EXACTLY ONE FIRING after 40 more think-ticks of continued visibility");
    }

    AVER_INFO("an agent does not see a target behind a wall, and OnSeeTarget never fires");
    {
        clearWorld(w);
        g_notifyCount = 0;

        Transform txf; txf.position = targetPos;
        const scene::Entity target = w.create("Target", scene::kInvalidEntity, txf);
        g_testTarget = target;

        Transform oxf; oxf.position = observerPos;
        const scene::Entity observer = w.create("Observer", scene::kInvalidEntity, oxf);
        synapse::perceptionSystem().attach(w, observer);

        // A wall spanning the midpoint of the eye-to-target ray -- thin along X (the ray's own
        // axis, so it cannot be stepped around by the ray's own geometry), wide enough on Y/Z that
        // the ray cannot pass beside or above it.
        const i32 wall = aver_phys_add_static_box(250.0f, 0.0f, 80.0f, 10.0f, 200.0f, 200.0f);
        check(wall != 0, "the wall body is created");

        for (int i = 0; i < 20; ++i) synapse::perceptionSystem().tick(w, dt);

        auto* p = w.component<synapse::CSynapsePerception>(observer, type);
        check(p && p->canSeeTarget == 0, "canSeeTarget stayed false -- the cone passed, the ray did not");
        check(p && p->lastKnownTargetEntity == 0, "lastKnownTargetEntity was never set");
        check(p && p->timeSinceSeenSec < 0.0f, "timeSinceSeenSec stayed at its 'never seen' sentinel");
        check(g_notifyCount == 0, "OnSeeTarget never fired");

        aver_phys_remove_body(wall);

        // Remove the wall and tick again -- the SAME observer, now with a clear line of sight,
        // must still be able to acquire. Proves the wall's absence is what changed, not some
        // one-shot latch left over from the blocked ticks above.
        for (int i = 0; i < 5; ++i) synapse::perceptionSystem().tick(w, dt);
        p = w.component<synapse::CSynapsePerception>(observer, type);
        check(p && p->canSeeTarget != 0, "and sees it the moment the wall is gone");
        check(g_notifyCount == 1, "OnSeeTarget fires now that it actually can");
    }

    aver_phys_shutdown();
    AVER_INFO(g_failures ? "PerceptionTest: {} FAILURES" : "PerceptionTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
