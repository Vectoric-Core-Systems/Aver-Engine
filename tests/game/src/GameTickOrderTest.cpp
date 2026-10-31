// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GameTickOrderTest: one gameplay frame runs in its fixed order -- the frame pumps, then for each fixed
// physics step the PHYSICS pass before that step, then the PROCESS pass once -- with and without a
// physics world. A recording stand-in for the managed dispatch; a real physics world; no device.

#include "aver/game/GameTick.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#include "aver/framework/framework_interp_abi.h"
#include "aver/framework/framework_timers_abi.h"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) AVER_INFO("   ok    {}", what);
    else { ++g_failures; AVER_ERROR("   FAIL  {}", what); }
}

struct Call { int32_t group; float dt; float bodyZ; };
static std::vector<Call> g_calls;
static int32_t g_body = 0;
static scene::Entity g_mover = scene::kInvalidEntity;   // moved 10 cm along x in every PHYSICS pass, like a platform a script drives
static bool g_teleportInProcess = false;                // and sent to x = 1000 (with a reset) in the PROCESS pass

// Records each tick_all with the body's height at that moment.
static void AVER_FW_CALL recordTickAll(int32_t group, float dt) {
    float p[3] = {0, 0, 0};
    if (g_body) aver_phys_body_position(g_body, p);
    g_calls.push_back({group, dt, p[2]});
    if (g_mover != scene::kInvalidEntity && (group == AVER_FW_TICK_PHYSICS || (group == AVER_FW_TICK_PROCESS && g_teleportInProcess))) {
        scene::World& w = scene::World::instance();
        Transform t = w.localTransform(g_mover);
        if (group == AVER_FW_TICK_PHYSICS) t.position.x += 10.0f;
        else { t.position.x = 1000.0f; aver_fw_interp_reset(static_cast<int32_t>(g_mover)); }
        w.setLocalTransform(g_mover, t);
    }
}

static int count(int32_t group) {
    int n = 0;
    for (const Call& c : g_calls) n += c.group == group;
    return n;
}

int main() {
    AVER_INFO("GameTickOrder test");

    AvManagedDispatch table{};
    table.structBytes     = static_cast<int32_t>(sizeof(AvManagedDispatch));
    table.contractVersion = AVER_FW_DISPATCH_VERSION;
    table.tick_all        = &recordTickAll;
    check(aver_fw_install_managed_dispatch(&table) == 1, "the recording dispatch installs");

    AVER_INFO("=== with a physics world: PHYSICS before each step, PROCESS once after ===");
    check(aver_phys_init() == 1, "a physics world starts");
    const float step = aver_phys_fixed_step();
    g_body = aver_phys_add_dynamic_sphere(0.0f, 0.0f, 1000.0f, 10.0f, 1.0f);
    check(g_body != 0, "a falling sphere exists");

    g_calls.clear();
    const i32 steps = game::tickGameplayGroups(step * 3.5f);
    check(steps == 3, "3.5 fixed steps of time runs 3 steps");
    check(g_calls.size() == 6, "six tick_all calls: two pumps, three physics passes, one process pass");
    if (g_calls.size() == 6) {
        check(g_calls[0].group == AVER_FW_PUMP_FRAME && g_calls[1].group == AVER_FW_PUMP_PLAY,
              "the frame pump, then the play pump, come first");
        check(g_calls[2].group == AVER_FW_TICK_PHYSICS && g_calls[3].group == AVER_FW_TICK_PHYSICS &&
              g_calls[4].group == AVER_FW_TICK_PHYSICS, "then one PHYSICS pass per step");
        check(g_calls[5].group == AVER_FW_TICK_PROCESS, "then the PROCESS pass, last");
        check(g_calls[2].dt == step && g_calls[4].dt == step, "the PHYSICS pass gets the fixed step as dt");
        check(std::fabs(g_calls[5].dt - step * 3.5f) < 1e-6f, "the PROCESS pass gets the frame dt");
        check(g_calls[2].bodyZ == 1000.0f, "the first PHYSICS pass runs before any step (the sphere has not moved)");
        check(g_calls[3].bodyZ < g_calls[2].bodyZ && g_calls[4].bodyZ < g_calls[3].bodyZ,
              "each later PHYSICS pass sees the step before it");
        check(g_calls[5].bodyZ < g_calls[4].bodyZ, "the PROCESS pass sees the last step");
    }

    g_calls.clear();
    check(game::tickGameplayGroups(step * 0.4f) == 0 && count(AVER_FW_TICK_PHYSICS) == 0 &&
          count(AVER_FW_TICK_PROCESS) == 1, "a frame shorter than the leftover step runs no PHYSICS pass, one PROCESS pass");
    g_calls.clear();
    game::tickGameplayGroups(step * 100.0f);
    check(count(AVER_FW_TICK_PHYSICS) == 8, "a long stall is clamped to eight PHYSICS passes");

    AVER_INFO("=== time scale: the same real-time cadence, every pass and step shrunk ===");
    aver_fw_timers_set_dilation(0.5f);
    float before[3] = {0, 0, 0};
    aver_phys_body_position(g_body, before);
    g_calls.clear();
    check(game::tickGameplayGroups(step * 3.0f) == 3, "half speed still runs three steps for three steps of real time");
    check(count(AVER_FW_TICK_PHYSICS) == 3, "and three PHYSICS passes");
    if (g_calls.size() == 6) {
        check(std::fabs(g_calls[2].dt - step * 0.5f) < 1e-7f, "each PHYSICS pass gets half the fixed step");
        check(std::fabs(g_calls[5].dt - step * 1.5f) < 1e-6f, "the PROCESS pass gets half the frame");
    }
    aver_fw_timers_set_dilation(0.0f);
    float held[3] = {0, 0, 0};
    aver_phys_body_position(g_body, held);
    g_calls.clear();
    check(game::tickGameplayGroups(step * 2.0f) == 2 && count(AVER_FW_TICK_PHYSICS) == 2,
          "time scale 0 still runs the PHYSICS passes");
    float after[3] = {0, 0, 0};
    aver_phys_body_position(g_body, after);
    check(after[2] == held[2] && held[2] < before[2], "but the world holds still");
    aver_fw_timers_set_dilation(1.0f);

    AVER_INFO("=== a dynamic rigid body moves its entity; interpolation draws it between steps ===");
    {
        scene::World& w = scene::World::instance();
        const scene::Entity ent = w.create("falling");
        auto* rb = static_cast<scene::CRigidBody*>(w.addComponent(ent, scene::kComponentRigidBody));
        check(rb != nullptr, "the entity gets a CRigidBody");
        if (rb) { rb->body = g_body; rb->motionType = AVER_PHYS_MOTION_DYNAMIC; }
        game::bodyPoseSync().interpolate = false;
        game::bodyPoseSync().clear();
        game::tickGameplayGroups(step * 2.0f);
        float o[3] = {0, 0, 0};
        aver_phys_body_origin(g_body, o);
        check(w.localTransform(ent).position.z == o[2], "after the frame the entity stands where its body is");
        game::bodyPoseSync().interpolate = true;
        game::tickGameplayGroups(step * 1.5f);   // one step, half a step of time left over
        float now[3] = {0, 0, 0};
        aver_phys_body_origin(g_body, now);
        const float alpha = aver_phys_step_alpha();
        const float z = w.localTransform(ent).position.z;
        check(alpha > 0.0f && alpha < 1.0f, "some time is left over in the step");
        check(z > now[2] && z < o[2] + 1e-3f, "with interpolation the entity is drawn between the last two steps");
        game::bodyPoseSync().restore(w);   // what the next frame does before its gameplay
        check(w.localTransform(ent).position.z == now[2], "and stands on the real pose again before the next frame's gameplay");
        // A mode of OFF keeps a body out, a script's switch overrides the project's.
        aver_fw_interp_set_mode(static_cast<int32_t>(ent), AVER_FW_INTERP_OFF);
        game::tickGameplayGroups(step * 1.5f);
        aver_phys_body_origin(g_body, now);
        check(w.localTransform(ent).position.z == now[2], "mode OFF: the entity is drawn on its real pose although the project switch is on");
        aver_fw_interp_set_mode(static_cast<int32_t>(ent), AVER_FW_INTERP_INHERIT);
        game::bodyPoseSync().interpolate = false;
        aver_fw_interp_set_enabled(1);
        game::bodyPoseSync().restore(w);
        game::tickGameplayGroups(step * 1.5f);
        aver_phys_body_origin(g_body, now);
        check(w.localTransform(ent).position.z > now[2], "the script's switch on blends it although the project's is off");
        aver_fw_interp_set_enabled(0);
        game::bodyPoseSync().interpolate = true;
        game::bodyPoseSync().restore(w);
        game::tickGameplayGroups(step * 1.5f);
        aver_phys_body_origin(g_body, now);
        check(w.localTransform(ent).position.z == now[2], "and its switch off stops it although the project's is on");
        aver_fw_interp_set_enabled(-1);
        game::bodyPoseSync().restore(w);
        game::bodyPoseSync().interpolate = false;
        game::bodyPoseSync().clear();
        w.destroy(ent);
        w.flush();
    }

    AVER_INFO("=== any entity: a script puts a mover on, resets it, reads the pose drawn ===");
    {
        scene::World& w = scene::World::instance();
        g_mover = w.create("mover");
        const int32_t id = static_cast<int32_t>(g_mover);
        game::bodyPoseSync().interpolate = true;
        game::bodyPoseSync().clear();
        game::tickGameplayGroups(step * 2.0f);
        const float inherit = w.localTransform(g_mover).position.x;
        check(inherit >= 10.0f && inherit == std::floor(inherit / 10.0f) * 10.0f, "an entity that inherits is not blended: it stands where its ticks left it");

        aver_fw_interp_set_mode(id, AVER_FW_INTERP_ON);
        game::tickGameplayGroups(step * 2.0f);   // two samples for the blend
        game::bodyPoseSync().restore(w);
        game::tickGameplayGroups(step * 1.5f);
        const float shown = w.localTransform(g_mover).position.x;
        float pos[3] = {0, 0, 0}, rot[4] = {0, 0, 0, 1};
        check(aver_fw_interp_pose(id, pos, rot) == 1 && pos[0] == shown, "mode ON: the pose drawn is published");
        game::bodyPoseSync().restore(w);
        const float real = w.localTransform(g_mover).position.x;
        AVER_INFO("   real x {}, drawn x {}", real, shown);
        check(shown < real && shown >= real - 10.0f - 1e-3f, "and it is between the last two steps' poses");

        aver_fw_interp_set_mode(id, AVER_FW_INTERP_OFF);
        game::tickGameplayGroups(step * 1.5f);
        const float off = w.localTransform(g_mover).position.x;
        game::bodyPoseSync().restore(w);
        check(w.localTransform(g_mover).position.x == off && aver_fw_interp_pose(id, nullptr, nullptr) == 0, "mode OFF: drawn on the real pose and nothing published");

        aver_fw_interp_set_mode(id, AVER_FW_INTERP_ON);
        game::tickGameplayGroups(step * 2.0f);
        game::bodyPoseSync().restore(w);
        g_teleportInProcess = true;
        game::tickGameplayGroups(step * 1.5f);
        g_teleportInProcess = false;
        check(w.localTransform(g_mover).position.x == 1000.0f, "after a reset the teleport is drawn at once, with no blend from where it was");
        aver_fw_interp_pose(id, pos, rot);
        check(pos[0] == 1000.0f, "and that is the pose published");

        aver_fw_interp_set_mode(id, AVER_FW_INTERP_INHERIT);
        check(aver_fw_interp_mode(id) == AVER_FW_INTERP_INHERIT, "INHERIT forgets the entity");
        game::bodyPoseSync().restore(w);
        game::bodyPoseSync().interpolate = false;
        game::bodyPoseSync().clear();
        const scene::Entity moverEnt = g_mover;
        g_mover = scene::kInvalidEntity;
        w.destroy(moverEnt);
        w.flush();
    }

    AVER_INFO("=== a paused frame: the pumps and both passes on the fixed clock, no step ===");
    float pausedAt[3] = {0, 0, 0};
    aver_phys_body_position(g_body, pausedAt);
    g_calls.clear();
    game::tickPausedFrame(step * 2.5f);
    check(count(AVER_FW_PUMP_FRAME) == 1 && count(AVER_FW_PUMP_PLAY) == 0,
          "the frame pump runs while paused, the gameplay pump does not");
    check(count(AVER_FW_TICK_PHYSICS) == 2 && count(AVER_FW_TICK_PROCESS) == 1,
          "two PHYSICS passes for 2.5 steps of time and one PROCESS pass, for actors that tick while paused");
    float pausedAfter[3] = {0, 0, 0};
    aver_phys_body_position(g_body, pausedAfter);
    check(pausedAfter[2] == pausedAt[2], "physics does not step while paused");
    check(game::physicsTickCarry() > 0.0f, "the paused frame carries its leftover half step");
    game::tickGameplayGroups(step * 0.25f);
    check(game::physicsTickCarry() == 0.0f, "back in play the world's own accumulator takes over and the paused carry is dropped");

    AVER_INFO("=== without a physics world: the same cadence from the frame's own clock ===");
    aver_phys_shutdown();
    g_body = 0;
    g_calls.clear();
    check(game::tickGameplayGroups(step * 2.5f) == 0, "no world, no steps reported");
    check(count(AVER_FW_TICK_PHYSICS) == 2 && count(AVER_FW_TICK_PROCESS) == 1,
          "2.5 fixed steps of time still runs two PHYSICS passes and one PROCESS pass");
    g_calls.clear();
    game::tickGameplayGroups(step * 0.6f);
    check(count(AVER_FW_TICK_PHYSICS) == 1, "the leftover half step carries into the next frame");

    check(aver_fw_clear_managed_dispatch() == 1, "the recording dispatch clears");
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
