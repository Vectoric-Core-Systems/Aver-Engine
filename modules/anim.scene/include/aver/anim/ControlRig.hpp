#pragma once
// CControlRig -- an .ocrig attached to an entity, applied every frame through AnimSystem's
// pose-modifier seam.
//
// This is the piece that makes a rig a THING IN A LEVEL rather than a function somebody can call.
// The component names a rig asset and a master weight; the system below loads the asset once,
// resolves its bone names against whatever skeleton each entity actually has, and runs its ops in
// order on the sampled pose.
//
// REGISTERED DYNAMICALLY, exactly as CSynapseAgent and friends are (docs/SYNAPSE.md section 6). That
// keeps Components.hpp, scene_abi.h and kComponentBuiltinMax untouched and moves no ABI version --
// a new component type is not a reason to renumber the built-ins every other language binds against.
//
// ATTACH THROUGH attachControlRig, NEVER world.addComponent DIRECTLY. ComponentPool::add resizes a
// byte buffer with a raw zero fill and runs no constructor, so this struct's in-class initialisers
// are dead code for a raw attach: `weight` would read 0 and the rig would silently do nothing, which
// is the exact failure mode SYNAPSE.md records for an agent whose moveSpeedCm came back zero.
//
// ONE POSE MODIFIER, THREE LAYERS (docs/plans/wave5/IK_RETARGETING.md section 2). For an entity the order is:
// its CControlRig's ops, then its CIkRig stack (IkRigSystem.hpp), then -- chained -- the ragdoll. Montages and
// state machines run before all of it.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/IkRigSystem.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcRig.hpp"

#include <string>
#include <unordered_map>

namespace aver::scene { class World; }

namespace aver::anim {

// One rig on one entity.
struct CControlRig {
    // Interned asset path of the .ocrig, resolved through AnimSystem's own resolver so a rig is
    // found the same way a skeleton and a clip are.
    u64 rig = 0;

    // Scales every op in the rig at once, on top of each op's own weight. 0 turns the whole rig off
    // without detaching it, which is what an animator wants while comparing with and without.
    f32 weight = 1.0f;
};

// Resolves the references an OCRIG 2 op carries -- `@input`, `bone:name+x,y,z` -- against the entity and the pose
// as it stands when the op runs. Empty (the default) resolves literals only, which is all a v1 rig has; the
// system fills it per entity. Cheap to build: nothing is computed until an op asks.
struct RigEval {
    const fmt::OcRigData* rig = nullptr;                 // the declarations (types, defaults, spaces)
    // Looks an input's value up by name hash into out[4]; false = the script never set it (the default applies).
    using InputFn = bool (*)(u64 nameHash, f32* out4, void* user);
    InputFn input = nullptr;
    void* inputUser = nullptr;
    Mat4 worldToModel = Mat4::identity();                // world-space inputs are brought into model space with this
    Quat worldRotInv = Quat::identity();                 // and quaternions with this (q_model = worldRotInv * q_world)
    // The CIkRig's limits, so `limits=1` on a fabrik / ccd op means the rig's numbers; null = the ragdoll defaults.
    const std::vector<IkRigLimitDef>* limitDefs = nullptr;

    // Filled by applyRigOp for the op in flight (the pose changes between ops, so the model cache is per op).
    mutable const fmt::OcSkeleton* skel = nullptr;
    mutable const Pose* pose = nullptr;
    mutable std::vector<Mat4> model;
    mutable bool modelValid = false;

    // `point`: the value is a position (world inputs convert, bone: refs apply); false = a direction or a local
    // value that is used as given. False when the reference cannot be resolved (unknown input, wrong type, missing bone).
    bool vec(const fmt::OcRigRef& ref, const Vec3& literal, bool point, Vec3& out) const;
    bool quat(const fmt::OcRigRef& ref, const Quat& literal, bool convert, Quat& out) const;
    bool weightOf(const fmt::OcRigOp& op, f32& out) const;
};

// Loads rigs, resolves their bone names, and applies them. One per world; the host owns it.
class ControlRigSystem {
public:
    // Registers CControlRig (and CIkRig) with the world. Idempotent, and safe to call before any entity exists.
    static u32 registerComponents(scene::World& world);

    // Attaches a rig to an entity and returns it, or nullptr if the component cannot be added.
    // Assigns a fresh CControlRig over the zero-filled bytes -- see the header note.
    static CControlRig* attach(scene::World& world, scene::Entity e, u64 rigAsset, f32 weight = 1.0f);
    // The same for an IK Rig (.ocikrig): CIkRig, weight defaulting to 1.
    static CIkRig* attachIk(scene::World& world, scene::Entity e, u64 ikRigAsset, f32 weight = 1.0f);

    // Installs this as `anim`'s pose modifier. Everything else follows from that one call: the
    // modifier is handed each animated entity's sampled pose and edits it in place.
    void install(AnimSystem& anim, scene::World& world);

    // Removes the modifier, restoring exactly the behaviour of a tick with no rig in it.
    static void uninstall(AnimSystem& anim);

    // Drops every loaded rig. Call when the level changes; the next tick reloads what it needs.
    void clear() { rigs_.clear(); loadFailed_.clear(); ik_.clear(); }

    // Drops runtime state: rig inputs, IK goals, foot smoothing. `game::resetGameSystems` calls it.
    void clearRuntime();

    // How many entities this system actually applied a rig to on the last tick. Exists so a test --
    // and a person wondering why nothing is moving -- can tell "no rig ran" apart from "a rig ran and
    // did nothing", which look identical from the outside.
    u32 appliedLastTick() const { return applied_; }

    // The rig loaded for `asset`, or nullptr. Loads on first use.
    const fmt::OcRigData* rigFor(u64 asset, AnimSystem& anim);

    // ---- OCRIG 2 inputs: named values a script sets (docs/plans/wave5/IK_RETARGETING.md 4.1) ----
    // `n` is 1 (float), 3 (vec3) or 4 (quat xyzw). Thirty-two distinct names per entity (a six-wheel truck's wheels need 12,
    // a tank's 12 plus its track scroll); a 33rd is refused with one warning.
    static constexpr u32 kMaxInputsPerEntity = 32;
    bool setInput(scene::Entity e, u64 nameHash, const f32* v, u32 n);
    // hash 0 clears every input of the entity.
    void clearInput(scene::Entity e, u64 nameHash);

    // ---- IK Rig ----
    IkRigRuntime& ik() { return ik_; }
    // Installs the host's ground probe (GameIkProbe.hpp); null clears it and feet see "no ground".
    void setGroundProbe(GroundProbeFn fn, void* user) { ik_.setGroundProbe(fn, user); }
    // The most foot rays a tick may cast across all entities; entities past it hold their last offsets that tick.
    u32 maxFootProbesPerTick = 128;

private:
    static void modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user);
    void applyTo(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose);
    static bool inputThunk(u64 nameHash, f32* out4, void* user);

    // Loaded rigs by asset id. A failed load is cached as an empty entry so a missing file is not
    // re-opened every frame for every entity.
    std::unordered_map<u64, fmt::OcRigData> rigs_;
    std::unordered_map<u64, bool> loadFailed_;

    struct Inputs {
        struct Slot { u64 hash = 0; f32 v[4] = {0, 0, 0, 0}; };
        Slot slots[kMaxInputsPerEntity];
        u32 count = 0;
    };
    std::unordered_map<scene::Entity, Inputs> inputs_;
    IkRigRuntime ik_;
    u64 pruneTick_ = ~0ull;

    scene::World* world_ = nullptr;
    AnimSystem* anim_ = nullptr;
    u32 applied_ = 0;
};

// Resolves a bone by name. Returns false when the skeleton has no such bone -- which is the ordinary
// case for a rig authored against a different skeleton, and must not be an error.
bool findBone(const fmt::OcSkeleton& skel, const std::string& name, u32& out);

// Applies one op to a pose, blending by `weight`. Exposed so it can be tested without a world, an
// entity or an AnimSystem -- the arithmetic is the part worth checking and none of it needs a scene.
// This overload resolves literals only (a v1 rig); a v2 rig's references need the RigEval below.
bool applyRigOp(const fmt::OcSkeleton& skel, Pose& pose, const fmt::OcRigOp& op, f32 weight);
bool applyRigOp(const fmt::OcSkeleton& skel, Pose& pose, const fmt::OcRigOp& op, f32 weight, const RigEval& ev);

// The process-wide control rig system, mirroring animSystem() next door. One per process because
// the World it reads is a singleton too -- two of these would race to be AnimSystem's single pose
// modifier, and the loser would silently do nothing.
ControlRigSystem& controlRigSystem();

} // namespace aver::anim
