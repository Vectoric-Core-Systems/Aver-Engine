// GameSystemsWiring: the ONE copy of the host-side wiring for the game systems (timers/events,
// blackboard, crowds/hearing/cover, animation state machines, streamed-audio zones). Sandbox.exe and
// AverEngineRuntime.exe both call it, for the reason GameTick.hpp is shared: two identical copies
// drift.
//
// HEADER-ONLY, like GameTick.hpp, so each host's own AVER_* macros decide what compiles.
//
// WHEN EACH PIECE RUNS is the host's decision, as for GameTick: registerGameSystems() once beside the
// other component registration, tickAnimGraphs() immediately BEFORE animSystem().tick every frame, and
// resetGameSystems() when a level goes away or Play stops.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Log.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/anim/AnimGraphSystem.hpp"
#endif
#if AVER_MODULE_AUDIO_SCENE
#  include "aver/audio/AudioScene.hpp"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_blackboard_abi.h"
#  include "aver/framework/framework_timers_abi.h"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseBt.hpp"
#endif
#if AVER_WITH_SYNAPSE_AI
#  include "aver/synapse/synapse_ai_abi.h"
#  if AVER_MODULE_SYNAPSE_GPU
#    include "aver/rhi/RHI.hpp"
#    include "aver/synapse/CrowdGpu.hpp"
#  endif
#endif

namespace aver::game {

#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_SCENE
// Aver.Synapse.Abi owns the ONE hearing system; the behaviour-tree blackboard lives in this
// process's BtSystem. This is the seam between them: every memory change the DLL reports is handed
// to BtSystem's own sink, which mirrors it into the listener's board.
inline void hearingMemoryToBlackboard(int32_t listener, float x, float y, float z, float level, int32_t tag,
                                      int32_t source, float confidence, int32_t forgotten, void*) {
    synapse::HeardMemory m;
    m.pos = Vec3{x, y, z};
    m.level = level;
    m.tag = static_cast<u32>(tag);
    m.source = static_cast<u32>(source);
    m.confidence = confidence;
    auto& sink = synapse::btSystem().hearingSink();
    if (forgotten) sink.onForgotten(static_cast<u32>(listener), m);
    else           sink.onHeard(static_cast<u32>(listener), m);
}
#endif

// Registers the components and installs the hooks the game systems need. Call once at startup, after
// the Synapse and control-rig registration (it adds a pose source to the animation system and the
// crowd/hearing behaviours to the behaviour-tree registry).
inline void registerGameSystems() {
#if AVER_MODULE_SCENE
    scene::World& world = scene::World::instance();
    anim::AnimGraphSystem::registerComponents(world);
    anim::animGraphSystem().install(anim::animSystem(), world);
#endif
#if AVER_MODULE_AUDIO_SCENE
    audio::audioSceneSystem().registerComponents(scene::World::instance());
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SYNAPSE_SCENE
    aver_fw_set_blackboard_provider(&synapse::blackboardRelay, nullptr);
#endif
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_SCENE
    aver_syn_ai_register();
    aver_syn_ai_register_behaviors(&synapse::btSystem().registry());
    aver_syn_hearing_set_memory_callback(&hearingMemoryToBlackboard, nullptr);
#endif
}

#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
// The GPU crowd backend: compiles its compute shader, joins the device's render features (the RHI only
// hands out a command context during a frame) and installs itself behind the ONE SynapseAi instance, so
// "GPU" becomes selectable. False leaves the CPU solver in charge; nothing else changes.
inline bool installCrowdGpu(rhi::IDevice& dev, synapse::GpuCrowdBackend& backend) {
    if (!backend.init(dev)) return false;
    dev.addRenderFeature(&backend);
    aver_syn_crowd_set_gpu_backend(static_cast<synapse::ICrowdBackend*>(&backend));
    return true;
}
// Reverse of installCrowdGpu, before the device goes: the device holds a bare pointer to the feature.
inline void removeCrowdGpu(rhi::IDevice* dev, synapse::GpuCrowdBackend& backend) {
    aver_syn_crowd_set_gpu_backend(nullptr);
    if (dev) dev->removeRenderFeature(&backend);
    backend.shutdown();
}
#endif

// Advances every animation state machine. BEFORE animSystem().tick in the same frame, so the pose it
// samples is this frame's.
inline void tickAnimGraphs(f32 dt) {
#if AVER_MODULE_SCENE
    anim::animGraphSystem().tick(scene::World::instance(), dt);
#else
    (void)dt;
#endif
}

// Drops per-level state: state-machine instances, boards, crowd/hearing/cover/squad state, pending
// timers and events. Call when a level is unloaded or Play stops.
inline void resetGameSystems() {
#if AVER_MODULE_SCENE
    anim::animGraphSystem().clear();
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    synapse::btSystem().clearBlackboards();
#endif
#if AVER_WITH_SYNAPSE_AI
    aver_syn_ai_reset();
#endif
}

} // namespace aver::game
