// aver_syn_last_error: the reason behind a 0 from Aver.Synapse.Abi. Every call returns 1/0, so a dead
// entity, an entity missing the component, a bad value and a "nothing yet" answer used to look alike.
// Falsify by making crowdOf return null without recording: the BadHandle checks fail.
#include "SynapseAiTestUtil.hpp"

#include "aver/scene/World.hpp"
#include "aver/synapse/synapse_ai_abi.h"

#include <vector>

using namespace aver;
using aitest::check;

namespace {

void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

} // namespace

int main() {
    AVER_INFO("SynapseAbiTest");
    scene::World& w = scene::World::instance();
    check(aver_syn_ai_register() == 1 && aver_syn_last_error() == 0, "register succeeds and records Ok");
    clearWorld(w);
    aver_syn_ai_reset();

    const scene::Entity live = w.create("Live");
    const scene::Entity dead = w.create("Dead");
    w.destroy(dead);
    w.flush();
    const i32 liveId = static_cast<i32>(live);
    const i32 deadId = static_cast<i32>(dead);

    AVER_INFO("dead and non-positive entities are BadHandle");
    check(aver_syn_crowd_configure(deadId, 30, 300, 600, 1) == 0 && aver_syn_last_error() == -1,
          "configure on a destroyed entity is 0 and -1");
    check(aver_syn_crowd_configure(0, 30, 300, 600, 1) == 0 && aver_syn_last_error() == -1,
          "configure on entity 0 is 0 and -1");
    check(aver_syn_crowd_attach(-5) == 0 && aver_syn_last_error() == -1, "attach on a negative entity is 0 and -1");
    check(aver_syn_hearing_forget(deadId) == 0 && aver_syn_last_error() == -1, "hearing_forget on a dead entity is 0 and -1");
    check(aver_syn_cover_find(deadId, 0, 0, 0, 0, 0, 0, nullptr, nullptr) == 0 && aver_syn_last_error() == -1,
          "cover_find for a dead seeker is 0 and -1");

    AVER_INFO("a live entity without the component is BadHandle");
    check(aver_syn_crowd_configure(liveId, 30, 300, 600, 1) == 0 && aver_syn_last_error() == -1,
          "configure without CSynapseCrowd is 0 and -1");
    check(aver_syn_hearing_configure(liveId, 1, 1000, 5) == 0 && aver_syn_last_error() == -1,
          "hearing_configure without CSynapseHearing is 0 and -1");
    check(aver_syn_squad_spacing_push(liveId, nullptr, nullptr) == 0 && aver_syn_last_error() == -1,
          "squad_spacing_push without CSynapseSquad is 0 and -1");

    AVER_INFO("a success clears the stale reason");
    check(aver_syn_crowd_attach(liveId) == 1 && aver_syn_last_error() == 0, "attach succeeds and records Ok");
    check(aver_syn_crowd_configure(deadId, 30, 300, 600, 1) == 0 && aver_syn_last_error() == -1, "a failure sets -1");
    check(aver_syn_crowd_configure(liveId, 30, 300, 600, 1) == 1 && aver_syn_last_error() == 0,
          "the next success resets it to 0");

    AVER_INFO("bad values are InvalidArgument");
    check(aver_syn_crowd_set_mode(liveId, 99, 0, 0, 0) == 0 && aver_syn_last_error() == -6, "mode 99 is 0 and -6");
    check(aver_syn_crowd_set_mode(liveId, AVER_SYN_MODE_HOLD, 0, 0, 0) == 1 && aver_syn_last_error() == 0,
          "a legal mode is 1 and 0");
    check(aver_syn_crowd_set_backend(7) == 0 && aver_syn_last_error() == -6, "an unknown backend is 0 and -6");
    check(aver_syn_crowd_set_drive(9) == 0 && aver_syn_last_error() == -6, "an unknown drive is 0 and -6");
    check(aver_syn_crowd_set_max_agents(-1) == 0 && aver_syn_last_error() == -6, "a negative cap is 0 and -6");
    check(aver_syn_crowd_set_backend(AVER_SYN_BACKEND_GPU) == 1 && aver_syn_last_error() == 0,
          "GPU with no backend installed falls back to CPU and is not an error");
    aver_syn_crowd_set_backend(AVER_SYN_BACKEND_CPU);

    AVER_INFO("a valid question with no answer yet is 0 with Ok");
    aver_syn_crowd_configure(deadId, 30, 300, 600, 1);   // leave a stale -1 behind
    float vx = 0, vy = 0, sp = 0;
    check(aver_syn_crowd_velocity(liveId, &vx, &vy, &sp) == 0 && aver_syn_last_error() == 0,
          "crowd_velocity on an unsimulated agent is 0 and Ok");
    check(aver_syn_hearing_attach(liveId) == 1, "hearing_attach succeeds");
    aver_syn_crowd_configure(deadId, 30, 300, 600, 1);
    float hx, hy, hz, lvl, conf, since;
    int32_t tag;
    check(aver_syn_hearing_get(liveId, &hx, &hy, &hz, &lvl, &tag, &conf, &since) == 0 && aver_syn_last_error() == 0,
          "hearing_get with nothing remembered is 0 and Ok");
    check(aver_syn_squad_attach(liveId, 1, 100) == 1, "squad_attach succeeds");
    aver_syn_crowd_configure(deadId, 30, 300, 600, 1);
    int32_t role;
    check(aver_syn_squad_slot(liveId, &role, &hx, &hy, &hz) == 0 && aver_syn_last_error() == 0,
          "squad_slot before the squad has a target is 0 and Ok");
    aver_syn_crowd_configure(deadId, 30, 300, 600, 1);
    check(aver_syn_cover_find(liveId, 0, 0, 0, 0, 0, 0, &hx, &hy) == 0 && aver_syn_last_error() == 0,
          "cover_find with no cover point is 0 and Ok");

    clearWorld(w);
    aver_syn_ai_reset();
    return aitest::g_failures == 0 ? 0 : 1;
}
