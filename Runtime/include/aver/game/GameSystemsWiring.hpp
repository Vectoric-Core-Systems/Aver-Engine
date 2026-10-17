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

#include <map>
#include <string>
#include <string_view>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/anim/AnimGraphSystem.hpp"
#  include "aver/anim/ControlRig.hpp"
#  include "aver/anim/MotionMatchSystem.hpp"
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
#  include "aver/softbody/SoftBodySystem.hpp"
#  if AVER_MODULE_RAGDOLL
#    include "aver/physics/physics_ragdoll_abi.h"
#    include "aver/physics/physics_layers_abi.h"
#    include "aver/ragdoll/RagdollSystem.hpp"
#  endif
#endif
#if AVER_MODULE_AUDIO_SCENE
#  include "aver/audio/AudioScene.hpp"
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
#  include "aver/framework/framework_softbody_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"            // AI: wave 3 #21 (aver_fw_class_of for the actors generator)
#  include "aver/framework/framework_blackboard_abi.h"
#  include "aver/framework/framework_timers_abi.h"
#endif
#if AVER_MODULE_SYNAPSE
#  include "aver/synapse/AiPaths.hpp"                    // AI: wave 3 #21
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseBt.hpp"
#endif
#if AVER_WITH_SYNAPSE_AI
#  include "aver/synapse/synapse_ai_abi.h"
#  include "aver/synapse/synapse_query_abi.h"            // AI: wave 3 #21
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

// AI: wave 3 #21 -- the seams the environment-query / smart-object / state-tree features need from the host.
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
// Answers the actors generator's `class=` filter: the entities whose actor class is `className` (or derives from it).
inline int32_t actorsOfClass(const char* className, int32_t includeDerived, int32_t* out, int32_t cap, void*) {
    if (!className || !out || cap <= 0) return 0;
    scene::World& w = scene::World::instance();
    int32_t n = 0;
    for (u32 i = 0; i < w.count() && n < cap; ++i) {
        const scene::Entity e = w.at(i);
        int32_t c = aver_fw_class_of(static_cast<int32_t>(e));
        for (int depth = 0; c != 0 && depth < 32; ++depth) {
            if (std::string_view(aver_fw_class_name(c)) == className) { out[n++] = static_cast<int32_t>(e); break; }
            if (!includeDerived) break;
            c = aver_fw_class_parent(c);
        }
    }
    return n;
}
#endif

// Where content-relative AI asset paths (.oceqs, .ocso, .ocst, .ocbt named from them) resolve. Sets BOTH
// copies: this image's and Aver.Synapse.Abi's.
inline void setAiContentRoot(const std::string& dir) {
#if AVER_MODULE_SYNAPSE
    synapse::setAiContentRoot(dir);
#endif
#if AVER_WITH_SYNAPSE_AI
    aver_syn_set_content_root(dir.c_str());
#else
    (void)dir;
#endif
}

#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_SCENE
namespace detail {
inline synapse::NotifyFn g_aiNotify = nullptr;
inline void* g_aiNotifyUser = nullptr;
inline void aiNotifyThunk(int32_t entity, const char* name, void*) {
    if (g_aiNotify) g_aiNotify(static_cast<scene::Entity>(entity), name, g_aiNotifyUser);
}
} // namespace detail
// Routes the AI's named events (EqsFinished, SmartObjectClaimed, ...) to the graph notify seam, the same
// function the host installs for animation notifies.
inline void installAiNotify(synapse::NotifyFn fn, void* user) {
    detail::g_aiNotify = fn;
    detail::g_aiNotifyUser = user;
    aver_syn_set_notify(fn ? &detail::aiNotifyThunk : nullptr, nullptr);
}
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS && AVER_MODULE_SOFTBODY_SCENE && AVER_MODULE_RAGDOLL
// A cloth on a character collides with it through the character's KINEMATIC ragdoll, on the proxy layer (the
// character capsule never reaches a soft body's collision query). The ragdoll is started here and its part
// bodies are moved to the proxy layer once they exist; a character that already has a ragdoll is left alone.
namespace detail {
inline std::map<scene::Entity, bool> g_softProxies;   // started by us -> layer applied
inline void softBodyProxyFn(scene::Entity e, bool on, void*) {
    ragdoll::RagdollSystem& rs = ragdoll::ragdollSystem();
    if (on) {
        if (rs.mode(e) >= 0 || g_softProxies.count(e)) return;
        if (rs.start(e, ragdoll::RagdollMode::Kinematic, 0.0f)) g_softProxies[e] = false;
    } else {
        const auto it = g_softProxies.find(e);
        if (it == g_softProxies.end()) return;
        rs.stop(e, 0.0f);
        g_softProxies.erase(it);
    }
}
inline void applySoftProxyLayers() {
    ragdoll::RagdollSystem& rs = ragdoll::ragdollSystem();
    const i32 layer = softbody::softBodySystem().options().proxyLayer;
    for (auto& [e, applied] : g_softProxies) {
        if (applied) continue;
        const i32 h = rs.physicsHandle(e);
        if (h == 0) continue;
        const i32 bones = aver_phys_ragdoll_bone_count(h);
        for (i32 b = 0; b < bones; ++b)
            if (const i32 body = aver_phys_ragdoll_body_of_bone(h, b)) aver_phys_body_set_layer(body, layer);
        applied = true;
    }
}
} // namespace detail
#endif

// Host wiring for the soft-body system that is not world state: the path resolver (the same function the host
// hands AnimSystem) for the mesh/.ocsoft decode caches, the notify sink for graph events, and the character
// proxy. Call once at startup after registerGameSystems(); any argument may be null.
inline void installSoftBodyHost(std::string (*pathFn)(u64, void*), void* pathUser,
                                void (*notifyFn)(u32, const char*, void*), void* notifyUser) {
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::SoftBodySystem& sys = softbody::softBodySystem();
    if (pathFn) sys.setPathResolver(pathFn, pathUser);
    if (notifyFn) sys.setNotifySink(notifyFn, notifyUser);
#  if AVER_MODULE_RAGDOLL
    sys.setCharacterProxyFn(&detail::softBodyProxyFn, nullptr);
#  endif
#else
    (void)pathFn; (void)pathUser; (void)notifyFn; (void)notifyUser;
#endif
}

// Registers the components and installs the hooks the game systems need. Call once at startup, after
// the Synapse and control-rig registration (it adds a pose source to the animation system and the
// crowd/hearing behaviours to the behaviour-tree registry).
inline void registerGameSystems() {
#if AVER_MODULE_SCENE
    scene::World& world = scene::World::instance();
    anim::AnimGraphSystem::registerComponents(world);
    anim::AnimSystem::registerComponents(world);   // CRootMotion
    anim::animGraphSystem().install(anim::animSystem(), world);
    anim::MotionMatchSystem::registerComponents(world);                        // motion matching: wave 5 #25
    anim::motionMatchSystem().install(anim::animSystem(), world);              // after the graph: it chains behind it
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::SoftBodySystem::registerComponents(scene::World::instance());   // CCloth, CDestructible: cloth and destruction
    softbody::softBodySystem().install(scene::World::instance());
#  if AVER_MODULE_FRAMEWORK
    aver_fw_set_softbody_provider(&softbody::softBodyProvider, &softbody::softBodySystem());
#  endif
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
#  if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    aver_syn_set_actor_provider(&actorsOfClass, nullptr);   // AI: wave 3 #21
#  endif
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
    anim::motionMatchSystem().tick(scene::World::instance(), dt);   // queries first: the machines' matcher layers read them
    anim::animGraphSystem().tick(scene::World::instance(), dt);
#else
    (void)dt;
#endif
}

// Advances cloth and destruction: reads the Jolt soft bodies back, runs dents, tears and debris, pushes the skin
// palettes. AFTER animSystem().tick (the palette it reads is this frame's) and before the soft-body draw scene
// updates, in both hosts. `viewPos` is the camera, for the distance tiers.
inline void tickSoftBodies(f32 dt, const Vec3& viewPos) {
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::SoftBodySystem& sys = softbody::softBodySystem();
    sys.setViewPos(viewPos);
    sys.tick(scene::World::instance(), dt);
#  if AVER_MODULE_RAGDOLL
    detail::applySoftProxyLayers();
#  endif
#else
    (void)dt; (void)viewPos;
#endif
}

// Drops per-level state: state-machine instances, boards, crowd/hearing/cover/squad state, pending
// timers and events. Call when a level is unloaded or Play stops.
inline void resetGameSystems() {
#if AVER_MODULE_SCENE
    anim::animGraphSystem().clear();
    anim::motionMatchSystem().clear();   // after the graph: its matcher layers point into these databases
    anim::animSystem().montages().stopAll();
    anim::controlRigSystem().clearRuntime();   // [wave5/ik-retarget] rig inputs, IK goals, foot smoothing
    anim::controlRigSystem().clear();          // and the loaded rigs, so an edited .ocrig / .ocikrig is read afresh
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::softBodySystem().clear();        // bodies, debris entities, queued inbox and events
#  if AVER_MODULE_RAGDOLL
    detail::g_softProxies.clear();
#  endif
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    synapse::btSystem().clearStateTrees();   // AI: wave 3 #21 -- instances only; before the boards their tasks may read
    synapse::btSystem().clearBlackboards();
#endif
#if AVER_WITH_SYNAPSE_AI
    aver_syn_ai_reset();
#endif
}

} // namespace aver::game
