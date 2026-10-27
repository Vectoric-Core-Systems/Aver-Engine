// GameTick: the ONE copy of physics start, audio start, the manifest's PHYSICS.*/AUDIO.* apply, the
// framework tick passes around the physics steps, and the Synapse AI tick -- Sandbox.exe (the
// editor) and AverEngineRuntime.exe (the shipped game) each carried an identical copy of all five
// before this file existed (docs/RUNTIME-DEDUP.md, Phase C).
//
// HEADER-ONLY ON PURPOSE. AVER_WITH_AUDIO_ABI is a compile definition on the two HOST
// targets (Sandbox PRIVATE, Aver.Runtime.Game PUBLIC) and is never defined on
// Aver.Runtime.Game.Core, which both hosts link. A .cpp living in Core would see that macro false
// regardless of what either host actually defines, and would compile the audio half of this file to
// nothing for both of them. A header re-evaluates every #if in each translation unit that includes
// it, so each host's own compile sees its own macros correctly.
//
// EACH HOST KEEPS ITS OWN GATES AND CALL POSITIONS -- nothing here decides WHEN to call any of this
// or what a caller does with a failure beyond logging it. Both hosts now apply PHYSICS.*/AUDIO.*
// unconditionally as their project opens: GameApp always did, and the editor was changed to match
// after the owner decision, because its old position -- inside applyProjectRenderSettings, behind
// that function's #if AVER_MODULE_VOXI and its hasRenderSettings()/voxiAttached_ early returns --
// silently discarded gravity and audio mix for any project that stated no RENDER.* key at all.
// Both pass their own host tag ("Sandbox" or "Game") through so the log lines below stay
// distinguishable in a shared log.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/game/PhysicsProjectApply.hpp"
#endif
#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
// framework_abi.h declares the AVER_FW_TICK_* pass numbers; aver_fw_tick itself is declared in
// framework_hooks.h -- both hosts already include this same pair together for the identical reason.
#  include "aver/framework/framework_abi.h"
#  include "aver/framework/framework_hooks.h"
#  include "aver/framework/framework_timers_abi.h"
#endif
#if AVER_MODULE_SCENE
#  include "aver/scene/decal_abi.h"
#endif
#if AVER_MODULE_AUDIO_SCENE
#  include "aver/audio/AudioScene.hpp"
#endif
#if AVER_WITH_SYNAPSE_AI
#  include "aver/synapse/synapse_ai_abi.h"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/formats/OcNav.hpp"
#  include "aver/synapse/SynapseAgent.hpp"
#  include "aver/synapse/SynapsePerception.hpp"
#  include "aver/synapse/SynapseBt.hpp"
#endif

namespace aver::game {

#if AVER_MODULE_PHYSICS
// Starts the physics world for `hostTag` ("Sandbox" or "Game"), logging success or failure under
// that tag. NO IMPLICIT GROUND: a level supplies its own collision (one static body per colliding
// PLACE), so nothing here creates a floor plane -- a level with a pit or a chasm below z = 0 falls
// exactly as far as it authored.
inline bool startPhysics(const char* hostTag) {
    if (aver_phys_init()) {
        AVER_INFO("[{}] physics started (fixed step {:.4f}s)", hostTag, aver_phys_fixed_step());
        return true;
    }
    AVER_WARN("[{}] physics failed to start - gameplay will not collide", hostTag);
    return false;
}

// Applies the manifest's PHYSICS.* keys that need a live world (gravity, fixed step) and the solver settings.
// The body is in PhysicsProjectApply.hpp, which also has configureProjectPhysics for the limits (MAXBODIES,
// MAXBODYPAIRS, MAXCONTACTS, TEMPALLOCMB, THREADS): those are fixed for a world's lifetime, so the hosts call it
// right after the manifest is parsed and before the first level loads. Call sites of this function are unchanged.
inline void applyProjectPhysics(const fmt::ProjectDesc& project) { applyProjectPhysicsKeys(project); }
#endif

#if AVER_WITH_AUDIO_ABI
// Starts the audio device for `hostTag`, logging on success only: 0 means "no output device" -- a
// machine fact, not an error -- so a failure to start stays quiet where physics warns.
inline bool startAudio(const char* hostTag) {
    if (aver_audio_init()) {
        AVER_INFO("[{}] audio started ({} Hz, {} channel(s))", hostTag,
                  aver_audio_sample_rate(), aver_audio_channels());
        return true;
    }
    return false;
}

// Applies AUDIO.MASTER / AUDIO.BUS from the manifest. Safe with no device: the ABI's setters are
// no-ops until aver_audio_init has succeeded.
inline void applyProjectAudioMix(const fmt::ProjectDesc& project) {
    if (project.hasAudioMix) {
        aver_audio_set_master_volume(project.masterVolume);
        for (int b = 0; b < 4; ++b) aver_audio_set_bus_volume(b, project.busVolume[b]);
    }
}
#endif

#if AVER_MODULE_FRAMEWORK
// A paused play frame: IGNORE_PAUSE timers, posted events and the per-frame pumps, so a pause
// menu's handlers still run. No tick pass, no physics step, no action update.
inline void tickPausedFrame(f32 dt) {
    aver_fw_timers_update(dt);
    aver_fw_events_flush();
    aver_fw_tick(AVER_FW_PUMP_FRAME, dt);
}

// The physics tick pass, run by aver_phys_step at the start of each fixed step.
inline void physicsTickHook(float fixedStep, void*) { aver_fw_tick(AVER_FW_TICK_PHYSICS, fixedStep); }

// Physics-pass time carried between frames when no physics world steps (a build or a level without one).
inline f32& physicsTickCarry() { static f32 carry = 0.0f; return carry; }

// One gameplay frame in Godot's order: input, timers and events, the frame pumps, then for each fixed
// physics step the PHYSICS pass (OnPhysicsTick, fixed dt) followed by the step, then the PROCESS pass
// (OnTick, frame dt). Returns how many fixed steps ran: 0-8, the catch-up clamp. The caller decides
// whether the world advances this frame at all.
inline i32 tickGameplayGroups(f32 dt) {
    // Input triggers and smoothing advance once a frame, after the host published this frame's input.
    aver_fw_action_update(dt);
    aver_fw_timers_update(dt);
    aver_fw_events_flush();
    aver_fw_tick(AVER_FW_PUMP_FRAME, dt);
    aver_fw_tick(AVER_FW_PUMP_PLAY, dt);
    i32 steps = 0;
#if AVER_MODULE_PHYSICS
    if (aver_phys_ready()) {
        aver_phys_set_step_hook(&physicsTickHook, nullptr);
        steps = aver_phys_step(dt);
        aver_phys_set_step_hook(nullptr, nullptr);
        physicsTickCarry() = 0.0f;
    } else
#endif
    {
        // The same fixed cadence and catch-up clamp as the physics world, with nothing to step.
        constexpr f32 kStep = 1.0f / 60.0f;
        f32& carry = physicsTickCarry();
        carry += dt > 0.0f ? (dt < kStep * 8.0f ? dt : kStep * 8.0f) : 0.0f;
        while (carry >= kStep) { carry -= kStep; aver_fw_tick(AVER_FW_TICK_PHYSICS, kStep); }
    }
    aver_fw_tick(AVER_FW_TICK_PROCESS, dt);
#if AVER_MODULE_SCENE
    aver_decal_tick(dt);   // lifetimes of pooled decals
#endif
#if AVER_MODULE_AUDIO_SCENE
    // Reverb zones and audio occlusion probes, after this frame's movement.
    aver::audio::audioSceneSystem().tick(scene::World::instance(), dt);
#endif
    return steps;
}
#endif

#if AVER_MODULE_SYNAPSE_SCENE
// Ticks the three Synapse systems for one frame, in the order each depends on the last. Must be
// called AFTER World::flush -- an agent must path from where physics/anim actually left it this
// frame, not from last frame's stale transform. The caller gates this on a live Play session.
inline void tickAi(f32 dt, fmt::OcNavData* nav) {
    // AFTER flush: see above. `nav` may be empty (no baked navigation for this level, or none
    // loaded yet) -- AgentSystem::tick treats that as "wait", not an error.
    synapse::agentSystem().tick(scene::World::instance(), nav);
    // Same "after flush" reasoning -- needs dt (unlike AgentSystem::tick) for its own
    // think-interval throttle.
    synapse::perceptionSystem().tick(scene::World::instance(), dt);
#if AVER_WITH_SYNAPSE_AI
    // Crowd avoidance, hearing memory and cover/squad tactics: after the agents have their waypoints,
    // before the behaviour tree reads what they produce.
    aver_syn_ai_tick(nav, dt);
#endif
    // AFTER perceptionSystem: a behaviour's own "CanSeeTarget"/"HasTarget" conditions read THIS
    // frame's sight state, not last frame's.
    synapse::btSystem().tick(scene::World::instance(), dt);
}
#endif

} // namespace aver::game
