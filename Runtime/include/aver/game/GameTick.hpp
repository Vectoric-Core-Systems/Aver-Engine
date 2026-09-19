// GameTick: the ONE copy of physics start, audio start, the manifest's PHYSICS.*/AUDIO.* apply, the
// framework tick groups around the physics step, and the Synapse AI tick -- Sandbox.exe (the
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
#endif
#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
// framework_abi.h declares the AVER_FW_TICK_* group numbers; aver_fw_tick itself is declared in
// framework_hooks.h -- both hosts already include this same pair together for the identical reason.
#  include "aver/framework/framework_abi.h"
#  include "aver/framework/framework_hooks.h"
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

// Applies PHYSICS.GRAVITY / PHYSICS.FIXEDSTEP from the manifest.
//
// READINESS, CHECKED: both setters are no-ops before aver_phys_init() has succeeded, so this asks
// aver_phys_ready() itself rather than trusting the caller to have gated on it already -- a project
// that states physics settings before the world exists is told they will apply once it does, rather
// than having them silently discarded.
inline void applyProjectPhysics(const fmt::ProjectDesc& project) {
    if (aver_phys_ready()) {
        if (project.hasGravity)
            aver_phys_set_gravity(project.gravity[0], project.gravity[1], project.gravity[2]);
        // CHECKED, because the setter refuses a step outside (0, 0.5] and says so by returning 0. A
        // manifest with a nonsense step must not read as applied.
        if (project.fixedStep > 0.0f && !aver_phys_set_fixed_step(project.fixedStep))
            AVER_WARN("[Project] PHYSICS.FIXEDSTEP {} refused -- must be within (0, 0.5] seconds",
                      project.fixedStep);
    } else if (project.hasPhysicsSettings()) {
        AVER_INFO("[Project] physics settings will apply once the world exists");
    }
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
}
#endif

#if AVER_MODULE_FRAMEWORK
// Runs the three gameplay tick groups for one frame, with the physics step between the first two.
// Returns true when physics actually stepped, so the caller can count it.
//
// THE CALLER DECIDES whether the world advances THIS FRAME AT ALL (a live Play session, or the
// editor's own --spawn-test harness) -- once called, this runs unconditionally.
//
// ORDER COPIED FROM THE CODE, NOT FROM A COMMENT ABOUT IT: the tick groups are meant to bracket the
// physics step, PrePhysics -> Physics -> PostPhysics, but the step itself sits BETWEEN PRE_PHYSICS
// and PHYSICS below, so PHYSICS-group ticks observe the results of this frame's simulation rather
// than last frame's. Reordering to match "bracket" literally would make every PHYSICS-group actor
// read stale transforms.
inline bool tickGameplayGroups(f32 dt) {
    aver_fw_tick(AVER_FW_TICK_PRE_PHYSICS, dt);
    bool stepped = false;
#if AVER_MODULE_PHYSICS
    aver_phys_step(dt);
    stepped = true;
#endif
    aver_fw_tick(AVER_FW_TICK_PHYSICS, dt);
    aver_fw_tick(AVER_FW_TICK_POST_PHYSICS, dt);
    return stepped;
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
    // AFTER perceptionSystem: a behaviour's own "CanSeeTarget"/"HasTarget" conditions read THIS
    // frame's sight state, not last frame's.
    synapse::btSystem().tick(scene::World::instance(), dt);
}
#endif

} // namespace aver::game
