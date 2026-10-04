#pragma once
// Sight: a cone check, then one occlusion raycast, throttled per agent by its own think-interval
// accumulator. "OnSeeTarget" fires through the SAME seam animation notifies use --
// ScriptHost::graphFire via a notify sink installed at both composition roots -- so the two cannot
// drift apart (see NotifyFn below, whose signature is byte-for-byte AnimNotifyFn's own).
//
// Plain aver::synapse namespace, NOT aver::synapse::scene -- see SynapseAgent.hpp's own header
// comment for why a namespace segment literally named "scene" would shadow the sibling namespace
// aver::scene for every bare `scene::X` reference in this file.
//
// WHO IS THE TARGET arrives through a host-installed function pointer, exactly like AssetPathFn on
// AnimSystem (modules/anim.scene/include/aver/anim/AnimSystem.hpp) -- a function-pointer type and a
// setter declared HERE, in the consuming module's own header, not a new relay added to
// framework_abi.h. Aver.Synapse.Scene must not link Aver.Framework at all (see
// aver_fw_set_synapse_target_provider's own comment in framework_abi.h for the reason), so only a
// composition root -- which already links both -- can supply the answer. The installed one-liner is
// aver_fw_controlled_pawn(aver_fw_player_controller(0)).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/World.hpp"

namespace aver::synapse {

// Answers "who should agents perceive right now" -- 0 (scene::kInvalidEntity) for "nobody". Called
// ONCE per PerceptionSystem::tick, not once per agent: v1 has exactly one target for the whole
// system (see the design doc's own "who is the target" note), so resolving it per-tick rather than
// per-agent avoids asking the same question through the seam N times for N perceiving agents.
using TargetResolverFn = scene::Entity (*)(void* user);

// Fires a named event on an entity's graph, and reports nothing back -- byte-for-byte
// aver::anim::AnimNotifyFn's own signature (AnimSystem.hpp), reused rather than duplicated: the
// SAME function a composition root already installs onto anim::animSystem() (its body is just
// scripts_.graphFire(entity, name), nothing anim-specific) is the correct sink here too.
using NotifyFn = void (*)(scene::Entity e, const char* name, void* user);

// Registered via World::registerComponent, all scalars -- same reasoning as CSynapseAgent
// (SynapseAgent.hpp's own comment): the reflection API's "verify covers the struct byte for byte"
// contract has no room for anything variable-length, and there is nothing variable-length here.
struct CSynapsePerception {
    // Tuning. eyeHeightCm matches AverCharacter.EyeHeight's own default (Character.cs:56).
    // sightRangeCm/sightHalfAngleDeg/thinkIntervalSec have no existing engine analogue to match --
    // this module's own choice: a generous room-to-two sight range, a forward-biased cone, and five
    // checks a second, cheap enough for "a handful of agents" (the design doc's own phrase for the
    // whole feature's expected scale).
    f32 sightRangeCm      = 3000.0f;
    f32 sightHalfAngleDeg = 45.0f;
    f32 thinkIntervalSec  = 0.2f;
    f32 eyeHeightCm       = 160.0f;

    // Runtime state, owned by the tick.
    f32 thinkAccumulatorSec    = 0.0f;
    i32 canSeeTarget           = 0;    // true iff the LAST think-tick's check succeeded
    i32 lastKnownTargetEntity  = 0;    // the target last CONFIRMED visible -- persists after losing sight
    f32 timeSinceSeenSec       = -1.0f; // -1 = never seen at all; else seconds since canSeeTarget was last true
};

class PerceptionSystem {
public:
    // Registers CSynapsePerception, idempotently. Call before spawning anything that will carry it.
    u32 registerComponents(scene::World& world);
    u32 componentType() const { return type_; }

    // Adds CSynapsePerception to `e` with its REAL defaults applied -- see
    // AgentSystem::attach's own comment (SynapseAgent.hpp) for why a bare world.addComponent alone
    // is not enough (ComponentPool::add zero-fills; it never runs a constructor).
    CSynapsePerception* attach(scene::World& world, scene::Entity e);

    void setTargetResolver(TargetResolverFn fn, void* user) { resolve_ = fn; resolveUser_ = user; }
    void setNotifySink(NotifyFn fn, void* user) { notify_ = fn; notifyUser_ = user; }
    bool hasNotifySink() const { return notify_ != nullptr; }

    // Advances every CSynapsePerception's think clock by `dt`; on the tick a perceiver's
    // accumulator crosses thinkIntervalSec, resolves the target once (not once per agent) and
    // checks: within sightRangeCm, within the forward cone (sightHalfAngleDeg), then one occlusion
    // raycast if AVER_MODULE_PHYSICS is built in. Fires "OnSeeTarget" through the installed notify
    // sink EXACTLY ONCE per acquisition -- the tick a perceiver's canSeeTarget flips false-to-true,
    // never on a tick where it was already true. No sink installed is not an error: acquisitions
    // are still tracked (canSeeTarget/lastKnownTargetEntity/timeSinceSeenSec all still update), the
    // same "tracked but not delivered" contract AnimSystem's own notify sink documents.
    void tick(scene::World& world, f32 dt);

private:
    u32 type_ = 0;
    TargetResolverFn resolve_ = nullptr;
    void* resolveUser_ = nullptr;
    NotifyFn notify_ = nullptr;
    void* notifyUser_ = nullptr;

    // The cone-then-occlusion check for one (observer, target) pair. Pure query; no side effects.
    bool canSee(scene::World& world, scene::Entity observer, scene::Entity target,
               const CSynapsePerception& p) const;
};

// The process-global system, matching anim::animSystem() / synapse::agentSystem().
PerceptionSystem& perceptionSystem();

} // namespace aver::synapse
