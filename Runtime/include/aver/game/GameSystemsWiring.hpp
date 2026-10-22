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
#if AVER_MODULE_SCENE && AVER_MODULE_HAIR_SCENE
#  include "aver/hair/HairSystem.hpp"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_hair_abi.h"
#  endif
#endif

#include <map>
#include <string>
#include <string_view>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/anim/AnimGraphSystem.hpp"
#  include "aver/anim/ControlRig.hpp"
#  include "aver/anim/MotionMatchSystem.hpp"
#  include "aver/game/ColliderShapes.hpp"   // registerCollisionShapeComponent (wave 11 SHAPES_MATERIALS)
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
#  include "aver/softbody/SoftBodySystem.hpp"

#  include "aver/softbody/SoftShapeSystem.hpp"
#  if AVER_MODULE_RAGDOLL
#    include "aver/physics/physics_ragdoll_abi.h"
#    include "aver/physics/physics_layers_abi.h"
#    include "aver/ragdoll/RagdollSystem.hpp"
#  endif
#endif
#if AVER_MODULE_AUDIO_SCENE
#  include "aver/audio/AudioScene.hpp"
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
#  include <algorithm>
#  include <functional>
#  include <aver/core/Hash.hpp>
#  include <aver/platform/FileSystem.hpp>
#  include <aver/vehicle/OcVehicle.hpp>
#  include <aver/vehicle/VehiclePhysicsSystem.hpp>
#  if AVER_MODULE_FRAMEWORK
#    include <aver/framework/framework_vehicle_abi.h>
#  endif
#  if AVER_MODULE_CAMERA
#    include <aver/camera/CameraRigSystem.hpp>
#  endif
#  if AVER_WITH_AUDIO_ABI
#    include <aver/audio/audio_abi.h>
#  endif
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_PARTICLES
#  include "aver/particles/ParticleSpawn.hpp"
#  include "aver/particles/ParticleSystem.hpp"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/particles/ParticleProvider.hpp"
#    include "aver/framework/framework_particles_abi.h"
#  endif
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_CAMERA
#  include "aver/camera/CameraRigSystem.hpp"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_camera_abi.h"
#    include "aver/framework/framework_access_abi.h"
#  endif
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
#  include "aver/framework/framework_softbody_abi.h"
#  include "aver/framework/framework_softfield_abi.h"

#  include "aver/framework/framework_softshape_abi.h"
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
// Wave 11: the group a cloth with "group": { "ragdollOf": "owner" } ignores is the physics handle of the owner's ragdoll (0 until it exists).
inline i32 softBodyRagdollGroupFn(scene::Entity skeleton, void*) { return ragdoll::ragdollSystem().physicsHandle(skeleton); }
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

    sys.setRagdollGroupFn(&detail::softBodyRagdollGroupFn, nullptr);
#  endif
#else
    (void)pathFn; (void)pathUser; (void)notifyFn; (void)notifyUser;
#endif
}

// ---- vehicles (docs/VEHICLES.md) -------------------------------------------------------------------------------
// The vehicle module links none of audio, particles, soft bodies, cameras or animation: every one of those leaves through a sink installed here.
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
namespace detail {
inline vehicle::SpecCache g_vehicleSpecs;                       // absolute path -> parsed, validated spec (a failure is cached too)
inline std::string (*g_vehiclePathFn)(u64, void*) = nullptr;   // content id -> absolute path: the resolver the soft bodies and animation use
inline void* g_vehiclePathUser = nullptr;
inline vehicle::NotifyFn g_vehicleNotify = nullptr;
inline void* g_vehicleNotifyUser = nullptr;

inline std::string vehicleAbsPath(const std::string& contentPath) {
    if (!g_vehiclePathFn) return {};
    std::string rel = contentPath;
    for (char& c : rel) if (c == static_cast<char>(92)) c = '/';
    return g_vehiclePathFn(fnv1a64(std::string_view(rel)), g_vehiclePathUser);
}
inline const vehicle::VehicleSpec* vehicleSpecAt(const std::string& absPath) {
    if (absPath.empty()) return nullptr;
    return g_vehicleSpecs.get(absPath, [](const std::string& p, std::string& text) { return readFileText(p, text); });
}
inline const vehicle::VehicleSpec* vehicleSpecById(u64 id) {
    return g_vehiclePathFn ? vehicleSpecAt(g_vehiclePathFn(id, g_vehiclePathUser)) : nullptr;
}

// Graph events: raised at the vehicle, and also at its driver, so a pawn graph hears VehicleEntered / VehicleCrash / VehicleSkid ... without being attached to the car.
inline void vehicleNotifyThunk(scene::Entity e, const char* name, void*) {
    if (!g_vehicleNotify) return;
    g_vehicleNotify(e, name, g_vehicleNotifyUser);
    const scene::Entity driver = vehicle::vehiclePhysicsSystem().driverOf(e);
    if (driver != scene::kInvalidEntity && driver != e) g_vehicleNotify(driver, name, g_vehicleNotifyUser);
}

#  if AVER_MODULE_FRAMEWORK
// A driver's input: the Drive.* actions of the project's input scheme, looked up by name once and cached. A name the project does not define reads as absent.
inline bool vehicleAction(const char* name, vehicle::ActionState& out) {
    static std::map<std::string, i32> ids;
    auto it = ids.find(name);
    if (it == ids.end()) it = ids.emplace(name, aver_fw_action_find(name)).first;
    if (it->second == 0) it->second = aver_fw_action_find(name);   // the scheme may load after the first look
    out = vehicle::ActionState{};
    if (it->second == 0) return false;
    f32 v[2] = {0.0f, 0.0f};
    aver_fw_action_value2(it->second, v);
    out.found = true;
    out.value = v[0];
    out.held = aver_fw_action_held(it->second) != 0;
    out.pressed = aver_fw_action_pressed(it->second) != 0;
    return true;
}
#  endif

#  if AVER_WITH_AUDIO_ABI
// Engine, skid and impact sounds through the C audio ABI. Content paths resolve through the content index.
class VehicleAudioPort final : public vehicle::AudioPort {
public:
    i32 loadSound(const std::string& contentPath) override {
        const std::string abs = vehicleAbsPath(contentPath);
        return abs.empty() ? 0 : aver_audio_load(abs.c_str());
    }
    i32 play(i32 sound, const Vec3& p, f32 volume, f32 pitch, bool looping) override {
        return sound == 0 ? 0 : aver_audio_play_at(sound, p.x, p.y, p.z, volume, pitch, looping ? 1 : 0, 0, 300.0f, 6000.0f);
    }
    void set(i32 voice, const Vec3& p, f32 volume, f32 pitch) override {
        if (voice == 0) return;
        aver_audio_set_voice_position(voice, p.x, p.y, p.z);
        aver_audio_set_voice_volume(voice, volume);
        aver_audio_set_voice_pitch(voice, pitch);
    }
    void stop(i32 voice) override { if (voice != 0) aver_audio_stop(voice); }
};
inline VehicleAudioPort g_vehicleAudio;
#  endif

#  if AVER_MODULE_SOFTBODY_SCENE
// A hard hit dents the soft bodies near it.
inline void vehicleCrashSink(const vehicle::CrashInfo& i, void*) {
    softbody::softBodySystem().impact(i.point, i.dir, i.radiusCm, i.energyJ, std::min(i.closingCmS * 0.25f, 600.0f));
}
#  endif

#  if AVER_MODULE_CAMERA
// Enter: attach the VehicleChase / FirstPerson / Seat presets to the rig entities the system made (once), point the view at rig 0 over 0.6 s.
// Next: at rig `current` over 0.4 s. Exit: back to the automatic choice. Impact: a shake at the point.
inline void vehicleCameraSink(scene::Entity vehicleEntity, scene::Entity, vehicle::CameraEvent ev, const vehicle::VehicleCameraInfo& info, void*) {
    scene::World& w = scene::World::instance();
    camera::CameraRigSystem& cams = camera::cameraRigSystem();
    switch (ev) {
        case vehicle::CameraEvent::Enter:
        case vehicle::CameraEvent::Next: {
            for (u32 i = 0; i < info.rigCount; ++i) {
                const vehicle::CameraRigInfo& r = info.rigs[i];
                if (!w.valid(r.rig) || w.component<camera::CCameraRig>(r.rig, camera::CameraRigSystem::typeId())) continue;
                const bool chase = r.kind == vehicle::CameraRigKind::Chase;
                const camera::CameraPreset p = chase ? camera::CameraPreset::VehicleChase
                                             : r.kind == vehicle::CameraRigKind::Hood ? camera::CameraPreset::FirstPerson : camera::CameraPreset::Seat;
                if (camera::CCameraRig* c = camera::CameraRigSystem::attach(w, r.rig, p, chase ? vehicleEntity : scene::kInvalidEntity)) {
                    c->ignoreBody = info.chassisBody;
                    if (chase && r.armCm > 0.0f) c->armLengthCm = r.armCm;
                }
            }
            const u32 which = ev == vehicle::CameraEvent::Enter ? 0u : info.current;
            if (which < info.rigCount && w.valid(info.rigs[which].rig))
                cams.setViewTarget(info.rigs[which].rig, camera::ViewBlend{ev == vehicle::CameraEvent::Enter ? 0.6f : 0.4f, camera::BlendCurve::EaseInOut, false});
            break;
        }
        case vehicle::CameraEvent::Exit:
            cams.clearViewTarget(camera::ViewBlend{0.6f, camera::BlendCurve::EaseInOut, false});
            break;
        case vehicle::CameraEvent::Impact:
            cams.shakeAt(info.impactPoint, 1500.0f, std::min(info.impactSpeedCmS / 2000.0f, 1.0f), 1.0f);
            break;
    }
}
#  endif

// The skeletal wheel route: the vehicle's wheel_<bone>_pos / _rot inputs go to the control rig.
inline void vehicleRigInput(scene::Entity e, u64 nameHash, const f32* values, u32 count, void*) {
    anim::controlRigSystem().setInput(e, nameHash, values, count);
}
} // namespace detail

// Content-relative `.ocvehicle` path -> spec, for the traffic system's `vehicle <path>` token. Null when the file is missing or invalid.
inline const vehicle::VehicleSpec* vehicleSpecByPath(const std::string& contentPath) {
    return detail::vehicleSpecAt(detail::vehicleAbsPath(contentPath));
}
#endif

// Host wiring for the vehicle system that is not world state: the content resolver (the same function the host hands AnimSystem), the spec cache over
// it, the action source, and the sinks for sound, soft-body damage, the camera and the skeletal wheel route. Call once at startup after registerGameSystems().
inline void installVehicleHost(std::string (*pathFn)(u64, void*), void* pathUser) {
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
    detail::g_vehiclePathFn = pathFn;
    detail::g_vehiclePathUser = pathUser;
    vehicle::VehiclePhysicsSystem& sys = vehicle::vehiclePhysicsSystem();
    sys.setSpecSource([](u64 id) { return detail::vehicleSpecById(id); });
#  if AVER_MODULE_FRAMEWORK
    sys.setActionSource(&detail::vehicleAction);
#  endif
#  if AVER_WITH_AUDIO_ABI
    sys.setAudioPort(&detail::g_vehicleAudio);
#  endif
#  if AVER_MODULE_SOFTBODY_SCENE
    sys.setCrashSink(&detail::vehicleCrashSink, nullptr);
#  endif
#  if AVER_MODULE_CAMERA
    sys.setCameraSink(&detail::vehicleCameraSink, nullptr);
#  endif
    sys.setRigInputSink(&detail::vehicleRigInput, nullptr);
    sys.setNotifySink(&detail::vehicleNotifyThunk, nullptr);
    // The effect sink stays null: the system's default is aver::spawnEffectAt, which Aver.Particles installs itself behind.
#else
    (void)pathFn; (void)pathUser;
#endif
}

// The graph-event sink (the same function the host gives the animation system). Install it where that one is, once scripting is up.
inline void setVehicleNotify(void (*fn)(u32, const char*, void*), void* user) {
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
    detail::g_vehicleNotify = fn;
    detail::g_vehicleNotifyUser = user;
#else
    (void)fn; (void)user;
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
    registerCollisionShapeComponent(world);                                    // CCollisionShape: an .ocshape collision beside a CRigidBody (wave 11 SHAPES_MATERIALS)
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::SoftBodySystem::registerComponents(scene::World::instance());   // CCloth, CDestructible: cloth and destruction
    softbody::softBodySystem().install(scene::World::instance());

    softbody::SoftShapeSystem::registerComponents(scene::World::instance());   // CSoftVolume, CSoftRope: jelly, ropes and cables (wave 11)
    softbody::softShapeSystem().install(scene::World::instance());
#  if AVER_MODULE_FRAMEWORK
    aver_fw_set_softbody_provider(&softbody::softBodyProvider, &softbody::softBodySystem());
    aver_fw_set_softfield_provider(&softbody::softFieldProvider, &softbody::softBodySystem());

    aver_fw_set_softshape_provider(&softbody::softShapeProvider, &softbody::softShapeSystem());
#  endif
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
    vehicle::VehiclePhysicsSystem::registerComponents(scene::World::instance());   // CVehicle: drivable vehicles
    vehicle::vehiclePhysicsSystem().install(scene::World::instance());
#  if AVER_MODULE_FRAMEWORK
    aver_fw_set_vehicle_provider(&vehicle::vehicleProvider, &vehicle::vehiclePhysicsSystem());
#  endif
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_CAMERA
    camera::CameraRigSystem::registerComponents(scene::World::instance());   // CCameraRig: gameplay cameras (docs/CAMERAS.md)
    camera::cameraRigSystem().install(scene::World::instance());
#  if AVER_MODULE_SOFTBODY_SCENE
    {   // the probe ignores the cloth and character-proxy layers unless a rig names them in its collisionMask
        const softbody::Options& so = softbody::softBodySystem().options();
        u32 skip = 0;
        if (so.clothLayer >= 0 && so.clothLayer < 32) skip |= 1u << so.clothLayer;
        if (so.proxyLayer >= 0 && so.proxyLayer < 32) skip |= 1u << so.proxyLayer;
        camera::cameraRigSystem().options().defaultProbeMask = ~skip;
    }
#  endif
#  if AVER_MODULE_FRAMEWORK
    aver_fw_set_camera_provider(&camera::cameraRigProvider, &camera::cameraRigSystem());
    camera::cameraRigSystem().setShakeSink(&aver_fw_camera_shake_add);   // shake_at and the shake modifier feed the existing CameraFx
    camera::cameraRigSystem().setBobSink(&aver_fw_camera_bob_set);
#  endif
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PARTICLES
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    particles::installEffectSpawner(&particles::particleSystem());   // aver::spawnEffectAt (soft-body dust, vehicle skid smoke)
#  if AVER_MODULE_FRAMEWORK
    aver_fw_set_particles_provider(&particles::particleProvider, &particles::particleSystem());
#  endif
#endif
#if AVER_MODULE_AUDIO_SCENE
    audio::audioSceneSystem().registerComponents(scene::World::instance());
#endif
#if AVER_MODULE_HAIR_SCENE
    hair::HairSystem::registerComponents(scene::World::instance());   // CHair, CHairDraw: strand hair (docs/HAIR.md)
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_HAIR_SCENE && AVER_MODULE_FRAMEWORK
    // The hair relay (framework_hair_abi.h): teleport, script colliders and the state read go to the HairSystem.
    aver_fw_set_hair_provider(&hair::hairProvider, &hair::hairSystem());
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
#if AVER_MODULE_HAIR_SCENE
    // Hair reads the skinned pose and the soft-body wind, and runs before HairScene::update copies the result.
    hair::hairSystem().setWind(softbody::softBodySystem().windVelocity(), 0.0f);
    hair::hairSystem().tick(scene::World::instance(), dt, viewPos);
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::SoftBodySystem& sys = softbody::softBodySystem();
    sys.setViewPos(viewPos);
    sys.tick(scene::World::instance(), dt);

    softbody::softShapeSystem().tick(scene::World::instance(), dt);   // after the cloth tick: it refreshes the tail of the same view list
#  if AVER_MODULE_RAGDOLL
    detail::applySoftProxyLayers();
#  endif
#else
    (void)dt; (void)viewPos;
#endif
}

// Vehicles, around the physics step (docs/VEHICLES.md): the drivers' pedals, builds and tunes BEFORE aver_phys_step; poses, telemetry, events,
// damage and sound AFTER it. Both are no-ops without the module. tickVehiclesPost declares vehicles built this frame movable to `mobility`
// (a game::PlayMobility), so the GI bake treats them like the traffic cars. `viewPos` is the camera, for the audio and the distance tiers.
inline void tickVehiclesPre(f32 dt, const Vec3& viewPos) {
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
    vehicle::VehiclePhysicsSystem& sys = vehicle::vehiclePhysicsSystem();
    sys.setViewPos(viewPos);
    sys.prePhysics(scene::World::instance(), dt);
#else
    (void)dt; (void)viewPos;
#endif
}
template <class Mobility>
inline void tickVehiclesPost(f32 dt, Mobility& mobility) {
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
    vehicle::VehiclePhysicsSystem& sys = vehicle::vehiclePhysicsSystem();
    sys.postPhysics(scene::World::instance(), dt);
    if (sys.stats().builtThisFrame > 0) mobility.seedMovable(sys.entities());
#else
    (void)dt; (void)mobility;
#endif
}

// Destroys the vehicles' physics bodies and constraints, voices and queued events, and forgets the parsed specs (so an edited .ocvehicle is read afresh).
// Before the level goes: the bodies must not outlive the physics world.
inline void clearVehicles() {
#if AVER_MODULE_SCENE && AVER_MODULE_VEHICLE_SCENE
    vehicle::vehiclePhysicsSystem().clear();
    detail::g_vehicleSpecs.clear();
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
    anim::animSystem().morphs().clearRuntime();   // [wave9/morph] script and sequencer weight overrides, lip-sync
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_PARTICLES
    particles::particleSystem().reset();   // live instances, queued spawn-at requests and GPU ranges; recreated on the next tick
#endif
    clearVehicles();                           // the CVehicle vehicles, voices and queued events
#if AVER_MODULE_HAIR_SCENE
    hair::hairSystem().clear();   // before the physics world is torn down: destroys the hair handles and proxy bodies
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE
    softbody::softShapeSystem().clear();       // jelly and rope bodies first: they publish into the cloth system's view list
    softbody::softBodySystem().clear();        // bodies, debris entities, queued inbox and events
#  if AVER_MODULE_RAGDOLL
    detail::g_softProxies.clear();
#  endif
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_CAMERA
    camera::cameraRigSystem().clear();         // rig runtime state, modifiers, the view target and the inbox
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
