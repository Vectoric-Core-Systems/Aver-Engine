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
#include "aver/anim/AnimSystem.hpp"
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

// Loads rigs, resolves their bone names, and applies them. One per world; the host owns it.
class ControlRigSystem {
public:
    // Registers CControlRig with the world. Idempotent, and safe to call before any entity exists.
    static u32 registerComponents(scene::World& world);

    // Attaches a rig to an entity and returns it, or nullptr if the component cannot be added.
    // Assigns a fresh CControlRig over the zero-filled bytes -- see the header note.
    static CControlRig* attach(scene::World& world, scene::Entity e, u64 rigAsset, f32 weight = 1.0f);

    // Installs this as `anim`'s pose modifier. Everything else follows from that one call: the
    // modifier is handed each animated entity's sampled pose and edits it in place.
    void install(AnimSystem& anim, scene::World& world);

    // Removes the modifier, restoring exactly the behaviour of a tick with no rig in it.
    static void uninstall(AnimSystem& anim);

    // Drops every loaded rig. Call when the level changes; the next tick reloads what it needs.
    void clear() { rigs_.clear(); }

    // How many entities this system actually applied a rig to on the last tick. Exists so a test --
    // and a person wondering why nothing is moving -- can tell "no rig ran" apart from "a rig ran and
    // did nothing", which look identical from the outside.
    u32 appliedLastTick() const { return applied_; }

    // The rig loaded for `asset`, or nullptr. Loads on first use.
    const fmt::OcRigData* rigFor(u64 asset, AnimSystem& anim);

private:
    static void modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user);
    void applyTo(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose);

    // Loaded rigs by asset id. A failed load is cached as an empty entry so a missing file is not
    // re-opened every frame for every entity.
    std::unordered_map<u64, fmt::OcRigData> rigs_;
    std::unordered_map<u64, bool> loadFailed_;

    scene::World* world_ = nullptr;
    AnimSystem* anim_ = nullptr;
    u32 applied_ = 0;
};

// Resolves a bone by name. Returns false when the skeleton has no such bone -- which is the ordinary
// case for a rig authored against a different skeleton, and must not be an error.
bool findBone(const fmt::OcSkeleton& skel, const std::string& name, u32& out);

// Applies one op to a pose, blending by `weight`. Exposed so it can be tested without a world, an
// entity or an AnimSystem -- the arithmetic is the part worth checking and none of it needs a scene.
bool applyRigOp(const fmt::OcSkeleton& skel, Pose& pose, const fmt::OcRigOp& op, f32 weight);

// The process-wide control rig system, mirroring animSystem() next door. One per process because
// the World it reads is a singleton too -- two of these would race to be AnimSystem's single pose
// modifier, and the loser would silently do nothing.
ControlRigSystem& controlRigSystem();

} // namespace aver::anim
