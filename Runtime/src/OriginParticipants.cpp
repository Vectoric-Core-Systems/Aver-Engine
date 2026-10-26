#include "aver/game/OriginParticipants.hpp"

#if AVER_MODULE_SCENE

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/game/AuthoredStreaming.hpp"
#include "aver/game/GameLevel.hpp"
#include "aver/game/GameStreaming.hpp"
#include "aver/game/LevelSequence.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/game/SequenceDirector.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/WorldShift.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/LevelInstance.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/MotionMatchSystem.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_origin_abi.h"
#endif
#if AVER_MODULE_CAMERA
#  include "aver/camera/CameraRigSystem.hpp"
#endif
#if AVER_MODULE_SOFTBODY_SCENE
#  include "aver/softbody/SoftBodySystem.hpp"
#  include "aver/softbody/SoftShapeSystem.hpp"
#endif
#if AVER_MODULE_HAIR_SCENE
#  include "aver/hair/HairSystem.hpp"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/Nav.hpp"
#  include "aver/synapse/SynapseAgent.hpp"
#  include "aver/synapse/SynapseAi.hpp"
#endif
#if AVER_MODULE_VOXI
#  include "aver/voxi/VoxiRenderer.hpp"
#endif
#if AVER_MODULE_PARTICLES
#  include "aver/particles/ParticleSystem.hpp"
#endif
#if AVER_MODULE_LANDSCAPE
#  include "aver/game/GameLandscape.hpp"
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
#  include "aver/game/GameFoliage.hpp"
#endif
#if AVER_MODULE_FLUIDS
#  include "aver/game/GameWater.hpp"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_origin_abi.h"
#  include "aver/framework/framework_timers_abi.h"
#endif

#include <algorithm>

namespace aver::game {

namespace {

#if AVER_MODULE_FRAMEWORK
// The object the origin ABI currently answers from: the last one installed. Only that one clears it on its way out.
OriginParticipants* g_abiProvider = nullptr;
#endif

world::PrepareResult refuse(const char* why) {
    world::PrepareResult r;
    r.ok = false;
    r.deferReason = why;
    return r;
}

rhi::ShiftWorldInfo renderInfo(const world::ShiftEvent& ev) {
    return rhi::ShiftWorldInfo{{ev.deltaCm.x, ev.deltaCm.y, ev.deltaCm.z}, {ev.newOrigin.x, ev.newOrigin.y}, ev.generation};
}

// World::shiftRoots and PlacementWriteBack's rebaseBaseline subtract with exactly this, so an untouched entity stays
// bit-equal to its baseline.
void shiftVec(Vec3& p, const Vec3& d) { p = Vec3{p.x - d.x, p.y - d.y, p.z - d.z}; }

// The two renderers that take the shift info as it is, in this order: the device, then Voxi. Shared by the renderer participant and a
// level reset.
void shiftDeviceAndVoxi(const OriginBindings& b, const rhi::ShiftWorldInfo& info) {
    if (b.device) b.device->shiftWorld(info);
#if AVER_MODULE_VOXI
    if (b.voxi) b.voxi->shiftWorld(info);
#endif
}

} // namespace

OriginParticipants::OriginParticipants() = default;

OriginParticipants::~OriginParticipants() {
#if AVER_MODULE_FRAMEWORK
    if (providerSet_ && g_abiProvider == this) { aver_fw_set_origin_provider(nullptr); g_abiProvider = nullptr; }
#endif
}

void OriginParticipants::configure(int floatingOrigin, int marginCm, bool unstatedDefault, int allowShiftRequest) {
    world::WorldOrigin::Settings s = origin_.settings();
    s.enabled = floatingOrigin >= 0 ? floatingOrigin != 0 : unstatedDefault;
    if (marginCm >= 0) s.marginCm = marginCm;
    origin_.configure(s);
    allowShiftRequest_ = allowShiftRequest > 0;
    if (b_.streaming) b_.streaming->setOriginGuard(s.enabled);
}

bool OriginParticipants::requestShift(i32 dxRegions, i32 dyRegions) {
    if (!allowShiftRequest_ || !origin_.enabled() || (dxRegions == 0 && dyRegions == 0)) return false;
    requestPending_ = true;
    requestDx_ = dxRegions;
    requestDy_ = dyRegions;
    return true;
}

bool OriginParticipants::update(const Vec3& camLocalCm) {
    bool shifted = false;
    if (requestPending_) {
        requestPending_ = false;
        const world::OriginCm d{static_cast<i64>(requestDx_) * world::kOriginStepCm, static_cast<i64>(requestDy_) * world::kOriginStepCm};
        shifted = origin_.shiftBy(d);
    }
    return origin_.update(camLocalCm) || shifted;
}

void OriginParticipants::reg(const char* name, i32 order, std::function<world::PrepareResult(const world::ShiftEvent&)> prepare,
                             std::function<void(const world::ShiftEvent&)> commit) {
    world::Participant p;
    p.name = name;
    p.order = order;
    p.prepare = std::move(prepare);
    p.commit = std::move(commit);
    origin_.addParticipant(std::move(p));
    // The service keeps (order, registration) order; mirror it for the readout.
    const auto it = std::upper_bound(order_.begin(), order_.end(), order,
                                     [](i32 o, const std::pair<i32, const char*>& q) { return o < q.first; });
    order_.insert(it, {order, name});
}

void OriginParticipants::install() {
    if (installed_) return;
    installed_ = true;
#if AVER_MODULE_FRAMEWORK
    // The origin ABI answers from this object: scripts read O, the generation and whether it is on, and may ask for a shift.
    aver_fw_origin_provider prov{};
    prov.size = static_cast<int32_t>(sizeof(prov));
    prov.user = this;
    prov.get = [](void* u, int64_t* xy) -> int32_t {
        const world::OriginCm o = static_cast<OriginParticipants*>(u)->origin();
        xy[0] = o.x; xy[1] = o.y;
        return 1;
    };
    prov.generation = [](void* u) -> uint32_t { return static_cast<OriginParticipants*>(u)->generation(); };
    prov.enabled = [](void* u) -> int32_t { return static_cast<OriginParticipants*>(u)->service().enabled() ? 1 : 0; };
    prov.request_shift = [](void* u, int32_t dx, int32_t dy) -> int32_t { return static_cast<OriginParticipants*>(u)->requestShift(dx, dy) ? 1 : 0; };
    providerSet_ = aver_fw_set_origin_provider(&prov) != 0;
    if (providerSet_) g_abiProvider = this;
#endif
    if (b_.streaming) b_.streaming->setOriginGuard(origin_.enabled());

    // 0  GATE: the prepare-side deferral rules (2.2.4). A level loading or unloading, a play transition, a gesture. The flat
    //    LevelStreaming holds absolute f64 bounds and converts its viewers each tick, so it follows a shift; LevelInstances
    //    (LEVELINST records realised at run time) still hold absolute f32 bounds and are not origin-aware, so a level that
    //    holds them keeps the old single frame instead of streaming itself out under a shifted camera.
    reg("gate", 0,
        [this](const world::ShiftEvent& ev) {
            if (busyReason_) return refuse(busyReason_);
            if (b_.busy) {
                if (const char* why = b_.busy(ev.urgent)) return refuse(why);
            }
            if (b_.level && b_.level->instances().count() > 0) return refuse("level instances are not origin-aware (cook the level)");
            return world::PrepareResult{};
        },
        nullptr);

    // 10 PHYSICS: the only expensive commit. Its prepare is aver_phys_origin_prepare, which refuses a bad delta or a call from inside a step.
    reg("physics", 10,
        [this](const world::ShiftEvent& ev) {
            physicsLive_ = false;
#if AVER_MODULE_PHYSICS
            if (aver_phys_origin_prepare(ev.deltaCm.x, ev.deltaCm.y, 0.0) >= 0) { physicsLive_ = true; return world::PrepareResult{}; }
            if (aver_phys_last_error() == static_cast<int32_t>(AbiError::NotInitialised)) return world::PrepareResult{};   // no world: nothing to move
            return refuse("physics refused the shift");
#else
            (void)ev;
            return world::PrepareResult{};
#endif
        },
        [this](const world::ShiftEvent& ev) {
#if AVER_MODULE_PHYSICS
            if (physicsLive_ && aver_phys_origin_shift(ev.deltaCm.x, ev.deltaCm.y, 0.0) != 1)
                AVER_ERROR("[Origin] physics shift failed after prepare accepted it (generation {})", ev.generation);
#else
            (void)ev;
#endif
            physicsLive_ = false;
        });

    // 20 SCENE: every root moves by -delta (children are parent-relative), then PlayMobility forgets the moves so the whole
    //    level does not turn Movable for the session, then the sequencer is told the frame its absolute keys convert into.
    reg("scene", 20, nullptr,
        [this](const world::ShiftEvent& ev) {
            scene::World::instance().shiftRoots(ev.deltaF);
            if (b_.mobility) b_.mobility->noteOriginShift(ev.generation);
            SequencePlayer::setSceneOrigin(ev.newOrigin.x, ev.newOrigin.y);
        });

    // 30 HOST MEMBERS: the camera and everything the host holds across frames.
    reg("host", 30, nullptr,
        [this](const world::ShiftEvent& ev) {
            for (Vec3* p : b_.positions) if (p) shiftVec(*p, ev.deltaF);
            if (b_.onHostCommit) b_.onHostCommit(ev);
        });

    // 40 SCENE SYSTEMS: world-space state cached across frames. All after the scene and physics have moved.
    reg("systems", 40, nullptr,
        [this](const world::ShiftEvent& ev) {
            scene::World& w = scene::World::instance();
            const Vec3& d = ev.deltaF;
            anim::animSystem().rebase(w, d);
            anim::motionMatchSystem().rebase(w, d);
            anim::controlRigSystem().ik().rebase(d);
            if (b_.sequences) b_.sequences->rebase(w, d);
#if AVER_MODULE_CAMERA
            camera::cameraRigSystem().rebase(d);
#endif
#if AVER_MODULE_SOFTBODY_SCENE
            softbody::softBodySystem().rebase(d);
            softbody::softShapeSystem().rebase(d);
#endif
#if AVER_MODULE_HAIR_SCENE
            hair::hairSystem().rebase(d);
#endif
            world::rebaseKinematicDrivers(d);
#if AVER_MODULE_SYNAPSE_SCENE
            synapse::agentSystem().rebase(w, d);
            if (b_.nav) synapse::rebaseNav(*b_.nav, d);
            if (b_.synapseAi) {
                b_.synapseAi->hearing().rebase(d);
                b_.synapseAi->tactics().rebase(d);
                b_.synapseAi->crowd().rebase(w, d);
                b_.synapseAi->smartObjects().rebase();
                b_.synapseAi->eqs().rebase(d);
            }
#endif
        });

    // 50 STREAMING: chunk keys are absolute, so nothing loads or evicts; each streamer takes the new origin and moves the one
    //    position it remembers (its velocity history).
    reg("streaming", 50, nullptr,
        [this](const world::ShiftEvent& ev) {
            if (b_.streaming) b_.streaming->onOriginShift(ev);
            if (b_.level) {
                b_.level->authored().onOriginShift(ev);
                b_.level->streaming().onOriginShift(ev);
            }
        });

    // 60 RENDER: after the scene, so the GI re-key sees the shifted draws. Device and Voxi before the next setCamera; foliage in
    //    the same commit as Voxi (a TLAS built between them would hold foliage in the old frame).
    reg("render", 60, nullptr,
        [this](const world::ShiftEvent& ev) {
            const rhi::ShiftWorldInfo info = renderInfo(ev);
            shiftDeviceAndVoxi(b_, info);
#if AVER_MODULE_LANDSCAPE
            if (b_.landscape) b_.landscape->onOriginShift(info);
#endif
#if AVER_MODULE_VOXI
            if (b_.foliage) b_.foliage->onOriginShift(info);
#endif
#if AVER_MODULE_FLUIDS
            if (b_.water) b_.water->onOriginShift(info);
#endif
            if (b_.onRenderCommit) b_.onRenderCommit(ev);
        });

    // 70 AUDIO, PARTICLES: independent of the rest.
    reg("audio+particles", 70, nullptr,
        [this](const world::ShiftEvent& ev) {
            if (b_.audioShift) b_.audioShift(static_cast<f32>(ev.deltaF.x), static_cast<f32>(ev.deltaF.y), static_cast<f32>(ev.deltaF.z));
#if AVER_MODULE_PARTICLES
            particles::particleSystem().shiftWorld(rhi::ShiftVec3d{ev.deltaCm.x, ev.deltaCm.y, ev.deltaCm.z});
#endif
        });

    // 80 EDITOR: its prepare is the gesture gate; its commit rebases the undo and redo stacks, the clipboard and the play snapshots.
    reg("editor", 80,
        [this](const world::ShiftEvent& ev) { return b_.editorPrepare ? b_.editorPrepare(ev) : world::PrepareResult{}; },
        [this](const world::ShiftEvent& ev) { if (b_.editorCommit) b_.editorCommit(ev); });

    // 90 SCRIPT EVENT: scripts see a consistent world.
    reg("event", 90, nullptr,
        [this](const world::ShiftEvent& ev) {
            if (b_.postEvent) { b_.postEvent(ev); return; }
#if AVER_MODULE_FRAMEWORK
            // The shift travels with the event: handlers learn how far every local position moved without asking the ABI.
            aver_fw_event_payload_clear();
            aver_fw_event_push_int(static_cast<int64_t>(ev.deltaCm.x));
            aver_fw_event_push_int(static_cast<int64_t>(ev.deltaCm.y));
            aver_fw_event_push_int(static_cast<int64_t>(ev.generation));
            aver_fw_event_push_int(ev.newOrigin.x);
            aver_fw_event_push_int(ev.newOrigin.y);
            aver_fw_event_post("world.origin_changed", 0, 0);
#endif
        });
}

void OriginParticipants::resetForLevel(world::OriginCm o) {
    const world::OriginCm old = origin_.origin();
    origin_.reset(o);
    const world::OriginCm now = origin_.origin();   // snapped when off the step
    if (b_.streaming) b_.streaming->setOrigin(now);
    if (b_.level) {
        b_.level->authored().setOrigin(now);
        b_.level->streaming().setOrigin(now);
    }
    SequencePlayer::setSceneOrigin(now.x, now.y);
    if (now == old) return;

    // The renderer remembers an origin (the constant-buffer rows, the cloud fold, landscape and foliage frames); a level
    // that starts somewhere else names it, before it loads. No history survives a level load, so nothing is composed.
    rhi::ShiftWorldInfo info;
    info.deltaCm = {static_cast<f64>(now.x - old.x), static_cast<f64>(now.y - old.y), 0.0};
    info.newOrigin = {now.x, now.y};
    info.generation = origin_.generation();
    shiftDeviceAndVoxi(b_, info);
#if AVER_MODULE_LANDSCAPE
    if (b_.landscape) b_.landscape->setFrameOrigin(info.newOrigin);
#endif
#if AVER_MODULE_VOXI
    if (b_.foliage) b_.foliage->setFrameOrigin(info.newOrigin);
#endif
#if AVER_MODULE_FLUIDS
    if (b_.water) b_.water->onOriginShift(info);
#endif
}

void OriginParticipants::levelLoaded() {
    // I3: the sub-millimetre cliff holds while the rebase margin plus the largest streaming radius fits under it; the service warns once.
    if (b_.level && b_.level->streamSettings().enabled)
        origin_.checkStreamRadius(static_cast<i32>(b_.level->streamSettings().evictCm));
}

bool OriginParticipants::ownsStreamed(scene::Entity e) const {
    if (b_.streaming && b_.streaming->owns(e)) return true;
    return b_.level && b_.level->authored().owns(e);
}

bool OriginParticipants::skipStreamed(scene::Entity e, void* self) {
    return self && static_cast<const OriginParticipants*>(self)->ownsStreamed(e);
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
