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
#include "aver/platform/FileSystem.hpp"   // userDataDir, for user://

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/game/PhysicsProjectApply.hpp"
#endif
#if AVER_MODULE_PHYSICS && AVER_MODULE_SCENE
#  include "aver/game/BodyPoseSync.hpp"
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
inline void applyProjectPhysics(const fmt::ProjectDesc& project) {
    applyProjectPhysicsKeys(project);
#if AVER_MODULE_SCENE
    bodyPoseSync().interpolate = project.physInterpolation == 1;
    bodyPoseSync().clear();
#endif
}
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
    // AUDIO.SPEAKERS / AUDIO.BINAURAL: both are remembered by the ABI whether audio has started or not; the defaults (stereo, off) are left alone.
    if (project.audioSpeakers == 6 || project.audioSpeakers == 8) aver_audio_set_speakers(project.audioSpeakers);
    if (project.audioBinaural) aver_audio_set_binaural(1);
}
#endif

#if AVER_MODULE_FRAMEWORK
// A project name as a directory name: reserved characters become '_', edge spaces and dots go.
inline std::string filesystemSafeProjectName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool reserved = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                               c == '\\' || c == '|' || c == '?' || c == '*' ||
                               static_cast<unsigned char>(c) < 0x20;
        out += reserved ? '_' : c;
    }
    const std::size_t b = out.find_first_not_of(" \t.");
    const std::size_t e = out.find_last_not_of(" \t.");
    out = (b == std::string::npos) ? std::string() : out.substr(b, e - b + 1);
    return out.empty() ? "Project" : out;
}

// The player's writable directory for a project (user://): the same for the editor and the shipped game.
inline std::string projectUserDir(const fmt::ProjectDesc& project) {
    return userDataDir() + AVER_PATH_SEP + filesystemSafeProjectName(project.name);
}

// Tells the framework where the open project lives (res://, user://), and which host this is.
inline void publishProjectPaths(const fmt::ProjectDesc& project, bool editor) {
    aver_fw_set_project(project.dir.c_str(), project.contentDir().c_str(), projectUserDir(project).c_str(), editor ? 1 : 0);
}

// Game time per real time (Godot's Engine.time_scale): the timer service's dilation, the one setting.
inline f32 gameTimeScale() { return aver_fw_timers_dilation(); }

// Steps the hook has seen this frame: from the second on, the step before has just finished.
inline int& physicsStepsThisFrame() { static int n = 0; return n; }

// The physics tick pass, run by aver_phys_step at the start of each fixed step, after the previous step's
// body poses reach their entities.
inline void physicsTickHook(float fixedStep, void*) {
#if AVER_MODULE_PHYSICS && AVER_MODULE_SCENE
    if (physicsStepsThisFrame()++ > 0) bodyPoseSync().capture(scene::World::instance());
#endif
    aver_fw_tick(AVER_FW_TICK_PHYSICS, fixedStep);
}

// Physics-pass time carried between frames when no physics world steps (no world, or a paused game).
inline f32& physicsTickCarry() { static f32 carry = 0.0f; return carry; }

// Runs the PHYSICS pass on the fixed cadence from the frame's own clock, stepping nothing: the same
// step and catch-up clamp as the physics world.
inline void physicsTicksOnClock(f32 dt, f32 scale) {
    f32 step = 1.0f / 60.0f;
#if AVER_MODULE_PHYSICS
    step = aver_phys_fixed_step();
#endif
    f32& carry = physicsTickCarry();
    carry += dt > 0.0f ? (dt < step * 8.0f ? dt : step * 8.0f) : 0.0f;
    while (carry >= step) { carry -= step; aver_fw_tick(AVER_FW_TICK_PHYSICS, step * scale); }
}

// A paused play frame: IGNORE_PAUSE timers, posted events, the per-frame pumps, and both tick passes
// for the actors whose PauseMode lets them run while paused (the bridge filters). Physics does not
// step and input actions do not update.
inline void tickPausedFrame(f32 dt) {
    const f32 scale = gameTimeScale();
    aver_fw_timers_update(dt);
    aver_fw_events_flush();
    aver_fw_tick(AVER_FW_PUMP_FRAME, dt);
    physicsTicksOnClock(dt, scale);
    aver_fw_tick(AVER_FW_TICK_PROCESS, dt * scale);
}

// One gameplay frame in Godot's order: input, timers and events, the frame pumps, then for each fixed
// physics step the PHYSICS pass (OnPhysicsTick) followed by the step, then the PROCESS pass (OnTick).
// `dt` is REAL time: steps keep a real-time cadence, and the time scale shrinks what each pass and step
// is handed (fixed step * scale, frame dt * scale). Returns how many fixed steps ran: 0-8, the catch-up
// clamp. The caller decides whether the world advances this frame at all.
inline i32 tickGameplayGroups(f32 dt) {
    const f32 scale = gameTimeScale();
    // Input triggers and smoothing advance once a frame, after the host published this frame's input.
    aver_fw_action_update(dt);
    aver_fw_timers_update(dt);
    aver_fw_events_flush();
    aver_fw_tick(AVER_FW_PUMP_FRAME, dt);
    aver_fw_tick(AVER_FW_PUMP_PLAY, dt);
    i32 steps = 0;
#if AVER_MODULE_PHYSICS
    if (aver_phys_ready()) {
#if AVER_MODULE_SCENE
        bodyPoseSync().restore(scene::World::instance());   // gameplay reads real poses, not last frame's blend
#endif
        aver_phys_set_time_scale(scale);
        physicsStepsThisFrame() = 0;
        aver_phys_set_step_hook(&physicsTickHook, nullptr);
        steps = aver_phys_step(dt);
        aver_phys_set_step_hook(nullptr, nullptr);
#if AVER_MODULE_SCENE
        if (steps > 0) bodyPoseSync().capture(scene::World::instance());   // the last step's poses
#endif
        physicsTickCarry() = 0.0f;
    } else
#endif
    {
        physicsTicksOnClock(dt, scale);
    }
    aver_fw_tick(AVER_FW_TICK_PROCESS, dt * scale);
#if AVER_MODULE_PHYSICS && AVER_MODULE_SCENE
    // For drawing: simulated bodies between their last two steps, when the project asks for interpolation.
    if (aver_phys_ready()) bodyPoseSync().present(scene::World::instance(), aver_phys_step_alpha());
#endif
#if AVER_MODULE_SCENE
    aver_decal_tick(dt * scale);   // lifetimes of pooled decals, in game time
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
