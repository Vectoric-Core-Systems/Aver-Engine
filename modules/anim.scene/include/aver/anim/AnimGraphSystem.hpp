// CAnimGraph: an animation state machine (.ocasm) running on an entity.
//
// REGISTERED DYNAMICALLY, exactly as CControlRig is, so Components.hpp, scene_abi.h and the built-in
// component ids stay put. Attach through AnimGraphSystem::attach, never world.addComponent directly:
// the storage arrives zero-filled and runs no constructor (see ControlRig.hpp for the same trap).
//
// PARAMETERS TRAVEL AS (name hash, value) SLOTS ON THE COMPONENT, so C# and graph nodes drive a
// machine through the generic scene field ABI with no new P/Invoke. A script finds the slot holding
// the hash (or the first empty one), writes the hash then the value. Triggers are a non-zero value;
// the system consumes it by writing zero back. The active state and its normalised time are written
// back each tick for the other direction.
#pragma once

#include "aver/anim/AnimStateMachine.hpp"
#include "aver/anim/AnimSystem.hpp"

#include <memory>
#include <unordered_map>

namespace aver::anim {

inline constexpr u32 kAnimGraphSlots = 16;
inline constexpr u32 kAnimGraphPaused = 0x1;

struct CAnimGraph {
    u64 machine = 0;                  // .ocasm ObjectId (fnv1a64 of the asset path)
    u64 activeState = 0;              // OUT: fnv1a64 of the active leaf state's name
    f32 playRate = 1.0f;              // 0 is read as 1
    f32 stateTime = 0.0f;             // OUT: cumulative normalised time of the active leaf
    u32 flags = 0;                    // kAnimGraphPaused
    u32 reserved = 0;
    u64 paramHash[kAnimGraphSlots] = {};
    f32 paramValue[kAnimGraphSlots] = {};
};
static_assert(sizeof(CAnimGraph) == 32 + 8 * kAnimGraphSlots + 4 * kAnimGraphSlots, "CAnimGraph must be padding-free");

// Raised from tick() for each state enter/exit. `ev.name` is the state's authored event name and may
// be empty; the host forwards it to the event bus or the script event path.
using AnimGraphEventFn = void (*)(scene::Entity e, const AsmEvent& ev, void* user);

class AnimGraphSystem {
public:
    static u32 registerComponents(scene::World& world);
    static CAnimGraph* attach(scene::World& world, scene::Entity e, u64 machineAsset);

    // Installs this as `anim`'s pose source.
    void install(AnimSystem& anim, scene::World& world);
    static void uninstall(AnimSystem& anim);

    // Applies parameters, advances every machine and writes the outputs back. Call BEFORE
    // AnimSystem::tick in the same frame, so the pose it samples is this frame's.
    void tick(scene::World& world, f32 dt);

    void setEventSink(AnimGraphEventFn fn, void* user) { sink_ = fn; sinkUser_ = user; }

    // Drops loaded assets and instances (level change, asset hot reload).
    void clear();

    const AnimStateMachine* instance(scene::Entity e) const;
    u32 activeInstances() const { return static_cast<u32>(instances_.size()); }

private:
    struct Instance {
        u64 machine = 0;
        std::unique_ptr<AnimStateMachine> sm;
        u32 stamp = 0;
    };
    struct Loaded {
        AnimStateMachineAsset asset;
        bool ok = false;
    };

    static bool sourceThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user);
    const Loaded* machineFor(u64 id);
    const BlendSpaceAsset* spaceFor(const std::string& ref);
    const fmt::OcAnimation* clipFor(const std::string& ref);

    std::unordered_map<scene::Entity, Instance> instances_;
    std::unordered_map<u64, Loaded> machines_;
    std::unordered_map<u64, std::unique_ptr<BlendSpaceAsset>> spaces_;   // null = failed once
    AnimSystem* anim_ = nullptr;
    AnimGraphEventFn sink_ = nullptr;
    void* sinkUser_ = nullptr;
    u32 stamp_ = 0;
    std::vector<AsmEvent> events_;
};

AnimGraphSystem& animGraphSystem();

} // namespace aver::anim
