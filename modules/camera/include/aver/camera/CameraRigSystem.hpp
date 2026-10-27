// CameraRigSystem: the CCameraRig component and the per-frame driver (picks the live rig, evaluates it, blends view targets, applies modifiers). Docs: docs/CAMERAS.md.
// Registered dynamically, like CMotionMatcher; attach with CameraRigSystem::attach, never world.addComponent (zero-filled storage, no constructor, a valid Chase rig).
// No C ABI of its own; framework_camera_abi.h carries what is not a scene field write.
// With no rig, modifier, blend or hand-back pending, drive() returns {active = false} and the legacy boom pose stands bit for bit.
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
    f32 externalBlendS = 0.5f;
    u32 defaultProbeMask = 0xFFFFFFFFu;       // used when a rig's collisionMask is 0 (the host drops the cloth and proxy layers)
    f32 reduceMotionLagFloor = 40;            // position lag speed while Reduce motion is on: stiff, not swaying
    f32 holdDestroyedS = 0.5f;                // a rig whose target died holds its pose this long, then yields to the fallback
};

struct RigFrame {
    f32 dt = 0;                               // the host's frame dt (what applyCameraFx gets), not the fixed physics step
    scene::Entity possessed = scene::kInvalidEntity;
    scene::Entity viewNode = scene::kInvalidEntity;
    bool motionAllowed = true;                // false while access.motion.reduce is on
    CameraPov fallback;                       // the legacy boom pose, computed by the host first
    bool externalActive = false;
    CameraPov external;
};

struct RigResult {
    bool active = false;                      // false: use the fallback untouched
    CameraPov pov;
    scene::Entity ownerHideRoot = scene::kInvalidEntity;
    bool hideAll = false;                     // hide every mesh under ownerHideRoot (the collision fade), not only the flagged ones
    bool cut = false;
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

    // Once per frame; order in docs/CAMERAS.md.
    RigResult drive(scene::World&, const RigFrame&);
    // Stateless: no lag, blend, modifiers, or write to the component.
    RigResult preview(scene::World&, scene::Entity rig, const RigFrame&);

    // Applied in arrival order at the next drive; modifier calls act on the stack at once and return their id.
    bool setViewTarget(scene::Entity, ViewBlend);     // kInvalidEntity = the legacy / pawn camera
    bool clearViewTarget(ViewBlend);
    bool cut(scene::Entity);                          // kInvalidEntity = the active rig
    u32  pushModifier(const CameraModifier&);         // 0 when the stack is full
    bool removeModifier(u32 id, f32 outS);
    u32  clearModifiers(u32 tag);
    void shakeAt(Vec3 pos, f32 radiusCm, f32 intensity, f32 falloffPower);

    const CameraPov& pose() const { return lastFinite_; }
    bool hasPose() const { return haveFinite_; }
    scene::Entity activeRig() const { return current_; }
    bool blending() const { return blending_; }
    f32 blendAlpha() const { return blendAlphaNow_; }
    bool rigState(scene::Entity, f32& armCm, u32& state) const;
    u32 modifierCount() const { return mods_.count(); }
    bool explicitViewTarget() const { return explicit_ != Selection::Auto; }

    // Play stop / level unload: state, modifiers, inbox.
    void clear();

    // Floating-origin shift: subtracts deltaCm from every held world position. Call after World::shiftRoots.
    void rebase(const Vec3& deltaCm);

    // Editor overlay data for the last drive. False when no rig is live.
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
        Vec3 lagPivot, prevTarget;
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
        f32 deadFor = 0;
        CameraPov lastPov;
        bool havePov = false;
        bool warnedNonFinite = false;
        bool bobOn = false;
    };
    struct EvalOut {
        CameraPov pov;
        bool cut = false;
        bool held = false;                // target dead, holding the last pose
        bool first = false;
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

    Selection explicit_ = Selection::Auto;
    scene::Entity explicitRig_ = scene::kInvalidEntity;
    ViewBlend pendingBlend_;
    bool hasPendingBlend_ = false;
    scene::Entity current_ = scene::kInvalidEntity;   // the live rig, none = the fallback
    bool haveSource_ = false;

    CameraPov lastOut_;                               // previous frame's pose before modifiers (the blend 'from')
    bool haveLast_ = false;
    CameraPov lastFinite_;
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

CameraRigSystem& cameraRigSystem();

// The framework relay's provider (framework_camera_abi.h, aver_fw_set_camera_provider), with user = &cameraRigSystem().
i32 cameraRigProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user);

}  // namespace aver::camera
