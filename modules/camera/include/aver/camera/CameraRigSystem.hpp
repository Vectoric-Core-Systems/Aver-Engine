// CameraRigSystem: the CCameraRig component (spring arm, camera mode, blend source in one) and the per-frame driver that
// picks the live rig, evaluates it against the physics world, blends view targets and applies the modifier stack.
// Design, modes and tuning: docs/CAMERAS.md.
//
// REGISTERED DYNAMICALLY, exactly as CMotionMatcher is, so Components.hpp and scene_abi.h stay put. Attach through
// CameraRigSystem::attach, never world.addComponent directly: the storage arrives zero-filled and runs no
// constructor. Zero-filled storage is a valid Chase rig (a 0 tuning field reads as its default).
//
// NO C ABI of its own: a script drives a rig by writing fields through the generic scene ABI; the framework relay
// (framework_camera_abi.h, provider cameraRigProvider below) carries what is not a field write.
//
// A PROJECT WITH NO RIG IS UNTOUCHED: with no CCameraRig, no modifier, no blend and no external hand-back pending,
// drive() returns {active = false} and the host uses the legacy boom pose it computed first, bit for bit.
//
// DETERMINISM. Inputs are component fields, world matrices after World::flush, Jolt query results, the host's frame dt and
// the ordered inbox. No wall clock and no rand; ids come from counters advanced in call order.
#pragma once

#include "aver/camera/CameraModifiers.hpp"
#include "aver/camera/CameraRigMath.hpp"
#include "aver/camera/CameraRigPresets.hpp"
#include "aver/camera/CameraTypes.hpp"
#include "aver/scene/World.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aver::camera {

struct Options {
    f32 teleportCm = 1000;                    // a pivot moving farther than this in one frame snaps the lag state
    f32 externalBlendS = 0.5f;                // hand-back blend after a sequencer camera lets go
    u32 defaultProbeMask = 0xFFFFFFFFu;       // used when a rig's collisionMask is 0 (the host drops the cloth and proxy layers)
    f32 reduceMotionLagFloor = 40;            // position lag speed while Reduce motion is on: stiff, not swaying
    f32 holdDestroyedS = 0.5f;                // a rig whose target died holds its pose this long, then yields to the fallback
};

struct RigFrame {
    f32 dt = 0;                               // the host's frame dt (what applyCameraFx gets), not the fixed physics step
    scene::Entity possessed = scene::kInvalidEntity;   // player 0's pawn
    scene::Entity viewNode = scene::kInvalidEntity;    // aver_fw_view_entity() if valid
    bool motionAllowed = true;                // false while access.motion.reduce is on
    CameraPov fallback;                       // the legacy boom pose, computed by the host first
    bool externalActive = false;              // the sequencer camera holds the view this frame
    CameraPov external;
};

struct RigResult {
    bool active = false;                      // false: use the fallback untouched
    CameraPov pov;
    scene::Entity ownerHideRoot = scene::kInvalidEntity;
    bool hideAll = false;                     // hide every mesh under ownerHideRoot (the collision fade), not only the flagged ones
    bool cut = false;                         // the camera teleported this frame
};

// The graph notify seam: (entity, "CameraViewTargetChanged" | "CameraBlendFinished" | "CameraCut", user).
using NotifyFn = void (*)(scene::Entity e, const char* name, void* user);

class CameraRigSystem {
public:
    // Registers CCameraRig and returns its type id (0 on failure).
    static u32 registerComponents(scene::World&);
    // Attaches CCameraRig seeded from `preset` (and CCamera when absent: near 2, far 200000, 60 degrees).
    static CCameraRig* attach(scene::World&, scene::Entity rig, CameraPreset, scene::Entity target = scene::kInvalidEntity);
    static CCameraRig presetValues(CameraPreset p) { return presetRig(p); }
    static u32 typeId();

    void install(scene::World&);
    void uninstall();
    void setNotifySink(NotifyFn fn, void* user) { notifyFn_ = fn; notifyUser_ = user; }
    // The system does not link the framework: the host installs the global CameraFx entry points here.
    void setShakeSink(void (*fn)(float trauma)) { shakeSink_ = fn; }
    void setBobSink(void (*fn)(float amount, float speed)) { bobSink_ = fn; }
    Options& options() { return options_; }
    const Options& options() const { return options_; }

    // Once per frame. See docs/CAMERAS.md for the order.
    RigResult drive(scene::World&, const RigFrame&);
    // Stateless: the pose one rig would give right now (editor "look through"). No lag, no blend, no modifiers, and no
    // write to the component.
    RigResult preview(scene::World&, scene::Entity rig, const RigFrame&);

    // Inbox: applied in arrival order at the top of the next drive. Modifier calls take effect on the stack at once (the
    // stack only advances inside drive, so the order is the same) and return their id.
    bool setViewTarget(scene::Entity, ViewBlend);     // kInvalidEntity = the legacy / pawn camera
    bool clearViewTarget(ViewBlend);                  // back to automatic selection
    bool cut(scene::Entity);                          // kInvalidEntity = the active rig
    u32  pushModifier(const CameraModifier&);         // 0 when the stack is full
    bool removeModifier(u32 id, f32 outS);
    u32  clearModifiers(u32 tag);
    void shakeAt(Vec3 pos, f32 radiusCm, f32 intensity, f32 falloffPower);

    // Reads (last drive)
    const CameraPov& pose() const { return lastFinite_; }
    bool hasPose() const { return haveFinite_; }
    scene::Entity activeRig() const { return current_; }
    bool blending() const { return blending_; }
    f32 blendAlpha() const { return blendAlphaNow_; }
    bool rigState(scene::Entity, f32& armCm, u32& state) const;
    u32 modifierCount() const { return mods_.count(); }
    // True when the view target was set explicitly (SetViewTarget) and not cleared.
    bool explicitViewTarget() const { return explicit_ != Selection::Auto; }

    // Play stop / level unload: state, modifiers, inbox.
    void clear();

    // Debug view for the editor overlay: what the live rig computed last frame. False when none is live.
    struct DebugView {
        scene::Entity rig = scene::kInvalidEntity;
        Vec3 pivot, laggedPivot, desiredEnd, camera, hit, lead;
        f32 probeRadius = 0, fadeEndCm = 0, armWanted = 0, armResolved = 0;
        bool hasProbe = false, didHit = false;
        u32 mode = 0;
        Vec3 fwd, right, up;
        f32 pitchMinRad = 0, pitchMaxRad = 0;
    };
    bool debug(scene::Entity rig, DebugView& out) const;

private:
    enum class Selection : u8 { Auto, Rig, Legacy };
    struct Inbox {
        enum class Kind : u8 { SetView, ClearView, Cut, Shake } kind = Kind::SetView;
        scene::Entity entity = scene::kInvalidEntity;
        ViewBlend blend;
        Vec3 pos;
        f32 radius = 0, intensity = 0, falloff = 0;
    };
    // Per-rig runtime, keyed by entity. Nothing here is saved.
    struct RigState {
        u64 lastFrame = 0;                // drive counter of the last evaluation; a gap resets the follow state
        bool lagValid = false;
        Vec3 lagPivot, prevTarget;        // the lagged pivot and last frame's unlagged one
        LeadState lead;
        bool rotValid = false;
        f32 yaw = 0, pitch = 0, roll = 0; // smoothed rotation (radians)
        f32 arm = -1;                     // last resolved segment length, < 0 before the first probe
        bool faded = false;
        u32 lastMode = 0;
        bool modeValid = false;
        bool snapNext = false;
        f32 sinceLook = 0, lastYawDeg = 0, lastPitchDeg = 0;
        bool lookValid = false;
        f32 kick = 0;
        Vec3 lastTargetPos;
        bool targetPosValid = false;
        f32 speed = 0;
        f32 deadFor = 0;                  // seconds the rig's target has been dead
        CameraPov lastPov;
        bool havePov = false;
        bool warnedNonFinite = false;
        bool bobOn = false;
    };
    struct EvalOut {
        CameraPov pov;
        bool cut = false;
        bool held = false;                // target dead, holding the last pose
        bool first = false;               // this rig's follow state was fresh (first evaluation)
        scene::Entity hideRoot = scene::kInvalidEntity;
        bool hideAll = false;
        u32 state = 0;
        f32 arm = 0;
        DebugView dbg;
    };

    void applyInbox(scene::World&);
    bool rigUsable(scene::World&, scene::Entity e);
    scene::Entity selectAuto(scene::World&, const RigFrame&);
    void startBlend(f32 seconds, BlendCurve curve, bool cut, const CameraPov* from);
    bool evaluate(scene::World&, scene::Entity rigEntity, CCameraRig& rig, RigState& st, const RigFrame& fr,
                  const ModResult& mods, bool write, EvalOut& out);
    void raise(scene::Entity e, const char* name);
    void flushEvents();

    static scene::Entity sourceEntity(scene::World&, const CCameraRig&, scene::Entity rigEntity);

    scene::World* world_ = nullptr;
    Options options_;
    NotifyFn notifyFn_ = nullptr;
    void* notifyUser_ = nullptr;
    void (*shakeSink_)(float) = nullptr;
    void (*bobSink_)(float, float) = nullptr;

    std::vector<Inbox> inbox_;
    ModifierStack mods_;
    std::unordered_map<scene::Entity, RigState> states_;
    std::vector<scene::Entity> scratch_;

    // The view target
    Selection explicit_ = Selection::Auto;
    scene::Entity explicitRig_ = scene::kInvalidEntity;
    ViewBlend pendingBlend_;
    bool hasPendingBlend_ = false;
    scene::Entity current_ = scene::kInvalidEntity;   // the live rig, none = the fallback
    bool haveSource_ = false;                         // current_ was decided at least once

    CameraPov lastOut_;                               // previous frame's pose before modifiers (the blend 'from')
    bool haveLast_ = false;
    CameraPov lastFinite_;                            // previous frame's final finite pose
    bool haveFinite_ = false;
    bool blending_ = false;
    CameraPov from_;
    f32 blendT_ = 0, blendDur_ = 0, blendAlphaNow_ = 0;
    BlendCurve blendCurve_ = BlendCurve::EaseInOut;

    bool externalWas_ = false;
    CameraPov lastExternal_;

    struct Event { scene::Entity e; const char* name; };
    std::vector<Event> events_;
    u64 frame_ = 0;
    scene::Entity possessed_ = scene::kInvalidEntity;   // last frame's pawn: the entity events are raised at for the fallback
    bool bobActive_ = false;
    bool warnedAttach_ = false;
    DebugView debug_;
    bool haveDebug_ = false;
};

CameraRigSystem& cameraRigSystem();   // process singleton, like anim::animSystem()

// The framework relay's provider (framework_camera_abi.h, aver_fw_set_camera_provider), with user = &cameraRigSystem().
i32 cameraRigProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

}  // namespace aver::camera
