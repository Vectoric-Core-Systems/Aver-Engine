// VehiclePhysicsSystem: drivable vehicles on scene entities, over Jolt's vehicle constraint (docs/VEHICLES.md).
//
// The one runtime owner of every physics vehicle that belongs to an entity with a CVehicle. It builds each one lazily from
// its spec (Aver.Vehicle: an .ocvehicle, or a preset sized from the entity's mesh), feeds the physics world the driver's
// pedals, WRITES THE ENTITY'S POSE every frame in C++ (no script timer copies it), moves the wheel visuals, publishes
// telemetry and events, and lets a driver get in and out. Sound, smoke and sparks, crash damage and the camera leave
// through sinks the host installs, so this module links none of audio, particles, soft bodies or cameras.
//
// COMPONENTS ARE REGISTERED DYNAMICALLY, the CCloth / CMotionMatcher pattern: Components.hpp ids and the scene ABI do not
// change. Attach through VehiclePhysicsSystem::attach, never world.addComponent directly: the storage arrives zero-filled
// and runs no constructor, and zero-filled storage means the useful state (every scale of 0 reads as 1, every flag is
// negative-sense).
//
// WHERE IT RUNS. The host calls prePhysics() once per frame before aver_phys_step and postPhysics() right after it, inside
// the PLAYING gate (the world::VehicleSystem calls). Both are no-ops without a physics world.
//
// DETERMINISM. Inputs to the simulation are the pedals, the gear requests and the ordered tune / flip / repair calls.
// Every loop is ascending by entity id, then wheel index; events carry a per-system sequence number; nothing reads a
// clock or a random number. On one binary the same input stream reproduces the same poses, telemetry and events
// (tests/vehicle.scene replays it twice). The audio and effect sinks are presentation and are excluded.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/World.hpp"
#include "aver/vehicle/VehicleSpec.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aver::vehicle {

// ---- the component ------------------------------------------------------------------------------------

inline constexpr u32 kVehicleNoActions = 0x1;          // negative sense: zero-filled = reads Drive.* actions while it has a driver
inline constexpr u32 kVehicleNoAudio = 0x2, kVehicleNoEffects = 0x4, kVehicleNoWheelVisuals = 0x8, kVehicleIndestructible = 0x10;
inline constexpr u32 kVehicleFlipRequest = 0x20, kVehicleRepairRequest = 0x40;   // one-shot: a script sets, the system clears

// CVehicle::outState bits.
inline constexpr u32 kVehicleStateAwake = 0x1, kVehicleStateGrounded = 0x2, kVehicleStateReversing = 0x4, kVehicleStateAbs = 0x8,
                     kVehicleStateTcs = 0x10, kVehicleStateFlipped = 0x20, kVehicleStateShifting = 0x40, kVehicleStateDestroyed = 0x80,
                     kVehicleStateManual = 0x100, kVehicleStateSkidding = 0x200;

struct CVehicle {                                   // "CVehicle"
    u64 asset = 0;                                  //  0  .ocvehicle ObjectId; 0 = preset
    u32 preset = 0;                                 //  8  0 car 1 van 2 truck 3 bus 4 sports 5 tank (when asset == 0)
    u32 flags = 0;                                  // 12
    f32 inThrottle = 0, inSteer = 0, inBrake = 0, inHandbrake = 0;   // 16..31 hold until set again
    f32 powerScale = 0, gripScale = 0, brakeScale = 0, damageScale = 0;   // 32..47 0 reads 1
    u32 driver = 0;                                 // 48 OUT: the driving entity
    i32 vehicle = 0;                                // 52 OUT: the physics vehicle handle
    u32 outState = 0;                               // 56 OUT: kVehicleState* bits
    i32 outGear = 0;                                // 60 OUT: -1 reverse, 0 neutral, 1..
    f32 outSpeedCmS = 0, outForwardCmS = 0, outRpm = 0, outHealth01 = 0;   // 64..79 OUT (health reads 1 once built)
    f32 outThrottle = 0, outSteer = 0, outBrake = 0;   // 80..91 OUT: as applied after the aids
    u32 outWheelsOnGround = 0;                      // 92 OUT
};
static_assert(sizeof(CVehicle) == 96, "CVehicle must be padding-free");

// ---- options, telemetry, events ------------------------------------------------------------------------

struct Options {
    u32 maxVehicles = 64;               // built at once
    u32 buildsPerFrame = 2;
    u32 audibleVehicles = 8;
    f32 audioRangeCm = 5000.0f;
    f32 lodMidCm = 6000.0f;             // a driverless vehicle farther than this runs its wheel casts every 2nd step ...
    f32 lodFarCm = 15000.0f;            // ... and farther than this every 4th. A driven one always every step.
    f32 flippedSeconds = 3.0f;          // on its side this long = flipped
    u32 maxQueuedEvents = 1024;
};

struct Telemetry {
    f32 speedCmS = 0, forwardSpeedCmS = 0, rpm = 0;
    i32 gear = 0, gearCount = 0;
    bool manual = false, shifting = false;
    f32 throttle = 0, brake = 0, steer = 0, handbrake = 0;
    i32 wheelsOnGround = 0;
    bool absActive = false, tcsActive = false, flipped = false, destroyed = false;
    f32 health01 = 1.0f, airborneSeconds = 0, longAccelG = 0, latAccelG = 0, trackLeftCmS = 0, trackRightCmS = 0;
    f32 engineTorqueNm = 0, clutch = 0, yawRateDegS = 0, upZ = 1;
};

struct WheelTelemetry {
    bool contact = false, skidding = false;
    f32 slipLongitudinal = 0, slipLateralDeg = 0, loadN = 0, compressionCm = 0, angularVelocityRadS = 0, steerDeg = 0, spinRad = 0;
    Vec3 contactPoint, contactNormal;
    f32 forceLongitudinalN = 0, forceLateralN = 0, surfaceFriction = 0, brakeTorqueNm = 0;
    i32 surfaceBody = 0;
    scene::Entity surfaceEntity = scene::kInvalidEntity;
};

// The kinds 1..7 are the physics ABI's (physics_vehicle_ext_abi.h VEV); 8 and up are the system's own.
enum class VehicleEventKind : u8 {
    SkidBegin = 1, SkidEnd = 2, Land = 3, Impact = 4, Shift = 5, Flipped = 6, Assist = 7,
    Entered = 8, Exited = 9, Crash = 10, Destroyed = 11,
};

struct VehicleEvent {
    VehicleEventKind kind = VehicleEventKind::Impact;
    scene::Entity vehicle = scene::kInvalidEntity;
    scene::Entity other = scene::kInvalidEntity;   // Entered / Exited: the driver; Impact / Crash: the entity hit, when stamped
    i32 a = 0, b = 0;                              // as the physics event (wheel, gear from / to, ...)
    Vec3 point, normal;
    f32 value = 0, value2 = 0;
    u32 seq = 0;
};

struct Stats {
    u32 vehicles = 0;                   // built
    u32 pending = 0;                    // with a CVehicle, not built (waiting, refused or failed)
    u32 driven = 0;
    u32 audible = 0;
    u32 builtThisFrame = 0;
};

// ---- seams the host fills ------------------------------------------------------------------------------

// A sound source: loads a content path and plays voices. The default (null) is silence. A voice of 0 means none.
class AudioPort {
public:
    virtual ~AudioPort() = default;
    virtual i32 loadSound(const std::string& contentPath) = 0;                                          // 0 = unavailable
    virtual i32 play(i32 sound, const Vec3& position, f32 volume, f32 pitch, bool looping) = 0;         // a voice
    virtual void set(i32 voice, const Vec3& position, f32 volume, f32 pitch) = 0;
    virtual void stop(i32 voice) = 0;
};

enum class CameraEvent : u8 { Enter, Exit, Next, Impact };
enum class CameraRigKind : u8 { Chase, Hood, Cockpit };
struct CameraRigInfo {
    scene::Entity rig = scene::kInvalidEntity;     // the empty child entity the system made for this rig
    CameraRigKind kind = CameraRigKind::Chase;
    f32 armCm = 0;                                 // Chase: the spec's arm length
    Vec3 localPoint;                               // where it sits, vehicle-origin relative
};
struct VehicleCameraInfo {
    i32 chassisBody = 0;                           // the probe must ignore it
    CameraRigInfo rigs[3];
    u32 rigCount = 0;
    u32 current = 0;                               // index of the rig that is live (Enter / Next)
    f32 impactSpeedCmS = 0;                        // Impact
    Vec3 impactPoint;
};
// Called from enter / exit / the camera key / a hard impact. The sink attaches rigs to `info.rigs[i].rig`, points the view
// (Enter: rig 0; Next: rig `current`; Exit: back to the automatic choice) and may shake on Impact. No camera maths lives here.
using CameraFn = void (*)(scene::Entity vehicle, scene::Entity driver, CameraEvent ev, const VehicleCameraInfo& info, void* user);

// Skid smoke and sparks. Default: aver::spawnEffectAt (Aver.Core's seam, a no-op until a particle system installs itself).
using EffectFn = void (*)(u64 effectId, const Vec3& position, const Vec3& direction, u32 count, void* user);

struct CrashInfo {
    scene::Entity vehicle = scene::kInvalidEntity;
    scene::Entity shell = scene::kInvalidEntity;   // the spec's DAMAGE shell child, or invalid
    Vec3 point, dir;                               // dir: the contact normal, out of the other body into the vehicle
    f32 radiusCm = 0, energyJ = 0, closingCmS = 0;
};
using CrashFn = void (*)(const CrashInfo& info, void* user);

// (entity, "VehicleCrash" | "VehicleSkid" | "VehicleLanded" | "VehicleFlipped" | "VehicleShift" | "VehicleEntered" | "VehicleExited", user)
using NotifyFn = void (*)(scene::Entity e, const char* name, void* user);

// A named input action as the system reads it. `found` false = the project defines no such action.
struct ActionState {
    bool found = false;
    f32 value = 0;                                 // the X of the action's value
    bool held = false, pressed = false;
};
using ActionFn = std::function<bool(const char* name, ActionState& out)>;

// A skeletal wheel route: the system sets named rig inputs on the entity (Anim.Scene's ControlRigSystem::setInput).
using RigInputFn = void (*)(scene::Entity e, u64 nameHash, const f32* values, u32 count, void* user);

using SpecSourceFn = std::function<const VehicleSpec*(u64 assetId)>;
using BoundsFn = std::function<bool(scene::Entity e, Vec3& lo, Vec3& hi)>;

// What the editor overlay draws.
struct DebugView {
    bool valid = false;
    const VehicleSpec* spec = nullptr;
    Vec3 position;
    Quat rotation;
    Vec3 velocity;
    Vec3 comWorld;
    u32 wheelCount = 0;
    struct Wheel {
        Vec3 attach, hub, contactPoint, normal;
        Vec3 axle{0, 1, 0};                // the wheel's axle in world space (spin-invariant); forward = axle x up
        bool inContact = false, skidding = false;
        f32 compressionCm = 0, slipLong = 0, slipLatDeg = 0, forceLongN = 0, forceLatN = 0, radiusCm = 0, suspMaxCm = 0;
    } wheels[16];
    Telemetry telemetry;
};

class VehiclePhysicsSystem {
public:
    VehiclePhysicsSystem();
    ~VehiclePhysicsSystem();
    VehiclePhysicsSystem(const VehiclePhysicsSystem&) = delete;
    VehiclePhysicsSystem& operator=(const VehiclePhysicsSystem&) = delete;

    // Registers CVehicle and returns its type id (0 on failure).
    static u32 registerComponents(scene::World&);
    static u32 typeId();
    // Attaches CVehicle with its defaults. An entity needs a mesh only for a preset (its bounds size the vehicle).
    static CVehicle* attach(scene::World&, scene::Entity, u64 asset, u32 preset);

    void install(scene::World&);
    void uninstall();

    // ---- host seams --------------------------------------------------------------------------------
    void setSpecSource(SpecSourceFn fn);        // asset id -> spec (the host's content cache)
    void setBoundsSource(BoundsFn fn);          // entity -> mesh bounds, for a preset-only vehicle; default: CMeshRenderer's
    void setActionSource(ActionFn fn);          // name -> action state; without one a driver's actions read as zero
    void setAudioPort(AudioPort* port);
    void setEffectSink(EffectFn fn, void* user);
    void setCrashSink(CrashFn fn, void* user);
    void setCameraSink(CameraFn fn, void* user);
    void setNotifySink(NotifyFn fn, void* user);
    void setRigInputSink(RigInputFn fn, void* user);
    void setViewPos(const Vec3& position);      // the camera, for audio and the per-vehicle LOD
    Options& options();
    const Options& options() const;

    // ---- the frame -----------------------------------------------------------------------------------
    void prePhysics(scene::World&, f32 dt);     // before aver_phys_step
    void postPhysics(scene::World&, f32 dt);    // after it, before the world's flush
    void clear();                               // destroys the vehicles (not the entities), voices, queued events

    // ---- driving -------------------------------------------------------------------------------------
    bool enter(scene::Entity vehicle, scene::Entity driver);
    bool exit(scene::Entity vehicle);
    scene::Entity driverOf(scene::Entity vehicle) const;
    bool shift(scene::Entity, i32 delta);
    bool setGear(scene::Entity, i32 gear);
    bool setManual(scene::Entity, bool manual);
    bool flip(scene::Entity, f32 liftCm);
    bool repair(scene::Entity);
    bool damage(scene::Entity, f32 amount01);   // health lost, as a fraction of the spec's DAMAGE health; negative gives it back
    // Keys (case-insensitive): power, brake, grip, suspension, steer, drag, downforce, abs, tcs, manual, collisionperiod.
    bool tune(scene::Entity, std::string_view key, f32 value);

    // ---- reading ---------------------------------------------------------------------------------------
    bool isVehicle(scene::Entity) const;         // built and alive
    bool telemetry(scene::Entity, Telemetry&) const;
    bool wheel(scene::Entity, u32 index, WheelTelemetry&) const;
    u32  wheelCount(scene::Entity) const;
    i32  physicsHandle(scene::Entity) const;
    i32  collisionPeriodOf(scene::Entity) const;    // the wheel-cast period the distance LOD last asked for (1 = every step)
    const VehicleSpec* specOf(scene::Entity) const;
    u32  drainEvents(VehicleEvent* out, u32 max);
    Stats stats() const;
    // Vehicles and their wheel children, ascending, for PlayMobility::seedMovable.
    std::vector<scene::Entity> entities() const;
    DebugView debug(scene::Entity) const;

    // ---- spawning (the relay's door) ---------------------------------------------------------------------
    // Makes an entity with a CVehicle at the pose and builds it NOW. `assetOrPresetOrText`: an `.ocvehicle` content path,
    // a preset name, or inline `.ocvehicle` text (beginning OCVEHICLE). A preset gets a default mesh size (no mesh is needed).
    // Returns the entity, or kInvalidEntity with a log line.
    scene::Entity spawn(scene::World&, std::string_view assetOrPresetOrText, const Vec3& position, f32 yawDegrees, i32* outVehicle = nullptr);

    // The relay provider's body (framework_vehicle_abi.h); see VehicleProvider.cpp.
    i32 relay(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* outF, i32* outI, i32 cap);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

VehiclePhysicsSystem& vehiclePhysicsSystem();   // process singleton, like anim::animSystem()

// The framework relay's provider (aver_fw_set_vehicle_provider); `user` = &vehiclePhysicsSystem().
i32 vehicleProvider(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* outF, i32* outI, i32 cap, void* user);

// ---- internals shared by the system's translation units (not an API) -----------------------------------------

namespace detail {

constexpr u32 kMaxWheelSlots = 16;

struct Entry {
    scene::Entity entity = scene::kInvalidEntity;
    // Build state
    enum class State : u8 { Pending, Built, Refused } state = State::Pending;
    u64 keyAsset = ~0ull;                          // what the build was attempted for, so a change retries
    u32 keyPreset = ~0u;
    bool warnedNoSpec = false, warnedScale = false, warnedConflict = false;
    std::shared_ptr<VehicleSpec> spec;
    i32 vehicle = 0, body = 0;
    u32 wheelCount = 0;
    Vec3 scale{1, 1, 1};
    scene::Entity wheelNode[kMaxWheelSlots] = {};
    bool warnedNode[kMaxWheelSlots] = {};
    Vec3 wheelWrittenPos[kMaxWheelSlots];
    Quat wheelWrittenRot[kMaxWheelSlots];
    bool wheelWritten[kMaxWheelSlots] = {};
    // Pose
    Vec3 pos;
    Quat rot;
    Vec3 writtenPos;
    Quat writtenRot;
    bool wrote = false;
    // Telemetry
    Telemetry tel;
    std::vector<WheelTelemetry> wheels;
    f32 rawVs[18] = {};                            // the physics blocks as read (physics_vehicle_ext_abi.h VS / VSI / VWS / VWSI), for the relay
    i32 rawVsi[11] = {};
    std::vector<f32> rawWheelF;                    // 18 per wheel
    std::vector<i32> rawWheelI;                    // 4 per wheel
    u32 outState = 0;
    f32 flippedFor = 0;
    // Damage
    f32 health = -1.0f;                            // < 0 = not yet set from the spec
    bool destroyed = false;
    // Applied tunes (so a field that did not change costs nothing)
    f32 appliedPower = -1, appliedGrip = -1, appliedBrake = -1;
    i32 appliedPeriod = 1;
    i32 absOverride = -1, tcsOverride = -1;        // tune("abs" / "tcs"): -1 = the spec's
    f32 lastIn[4] = {-9, -9, -9, -9};
    // Driver
    scene::Entity driver = scene::kInvalidEntity;
    i32 driverCharacter = 0;
    f32 driverGravity = 1.0f, driverStrength = 0.0f;
    std::vector<std::pair<scene::Entity, u32>> hiddenMeshes;   // (entity, previous CMeshRenderer::flags)
    std::vector<CameraRigInfo> rigs;
    u32 rigCurrent = 0;
    // Audio and effects
    i32 voiceIdle = 0, voiceHigh = 0, voiceSkid = 0;
    f32 smokeAcc[kMaxWheelSlots] = {};
    u64 skidEffect = 0, impactEffect = 0;
    bool audible = false;
    // Replay-visible counters
    u32 shifts = 0;
};

} // namespace detail

struct VehiclePhysicsSystem::Impl {
    scene::World* world = nullptr;
    Options opt;
    SpecSourceFn specSource;
    BoundsFn boundsSource;
    ActionFn actions;
    AudioPort* audio = nullptr;
    EffectFn effectFn = nullptr;      void* effectUser = nullptr;
    CrashFn crashFn = nullptr;        void* crashUser = nullptr;
    CameraFn cameraFn = nullptr;      void* cameraUser = nullptr;
    NotifyFn notifyFn = nullptr;      void* notifyUser = nullptr;
    RigInputFn rigFn = nullptr;       void* rigUser = nullptr;
    Vec3 viewPos;
    u32 frame = 0;

    std::map<scene::Entity, detail::Entry> entries;            // ascending entity id: the determinism contract
    std::unordered_map<i32, scene::Entity> byHandle;           // physics vehicle handle -> entity
    std::map<u64, std::shared_ptr<VehicleSpec>> inlineSpecs;   // specs made from text or presets by spawn()
    std::unordered_map<std::string, i32> sounds;               // content path -> sound handle (0 = unavailable)
    std::vector<VehicleEvent> events;
    u32 seq = 0;
    Stats stats;
    u32 builtThisFrame = 0;

    // ---- VehiclePhysicsSystem.cpp
    void resolveAndBuild(scene::World& w);
    std::shared_ptr<VehicleSpec> specFor(scene::World& w, detail::Entry& e, const CVehicle& c);
    bool buildEntry(scene::World& w, detail::Entry& e, CVehicle& c);
    void destroyEntry(detail::Entry& e, bool destroyPhysics);
    void writePose(scene::World& w, detail::Entry& e);
    void readTelemetry(scene::World& w, detail::Entry& e, CVehicle& c);
    void processPhysicsEvents(scene::World& w, f32 dt);
    detail::Entry* find(scene::Entity e);
    const detail::Entry* find(scene::Entity e) const;
    void emit(VehicleEventKind kind, scene::Entity vehicle, scene::Entity other, i32 a, i32 b, const Vec3& point, const Vec3& normal,
              f32 v1, f32 v2, const char* notifyName);
    void notify(scene::Entity e, const char* name);

    // ---- VehicleDriver.cpp
    void applyInputs(scene::World& w, detail::Entry& e, CVehicle& c);
    void buildRigs(scene::World& w, detail::Entry& e);
    void cameraNext(detail::Entry& e);
    void hideDriver(scene::World& w, detail::Entry& e);
    void showDriver(scene::World& w, detail::Entry& e);
    bool applyAssistOverrides(detail::Entry& e);
    void readActions(scene::World& w, detail::Entry& e, CVehicle& c);
    void snapDriver(scene::World& w, detail::Entry& e);
    bool enterImpl(scene::World& w, detail::Entry& e, scene::Entity driver);
    bool exitImpl(scene::World& w, detail::Entry& e);
    void applyDamage(scene::World& w, detail::Entry& e, CVehicle& c, f32 amountHealth, bool announce);
    void onImpact(scene::World& w, detail::Entry& e, CVehicle& c, const VehicleEvent& ev);
    void updateLod(detail::Entry& e, bool driven);
    f32 specHealth(const detail::Entry& e) const;

    // ---- VehicleVisuals.cpp
    void writeWheelVisuals(scene::World& w, detail::Entry& e, CVehicle& c);
    void resolveWheelNodes(scene::World& w, detail::Entry& e);

    // ---- VehicleAudio.cpp
    void updateAudio(scene::World& w, f32 dt);
    void updateEffects(scene::World& w, detail::Entry& e, CVehicle& c, f32 dt);
    void stopVoices(detail::Entry& e);
    i32 soundFor(const std::string& path);
    void oneShot(const detail::Entry& e, const std::string& path, f32 volume, f32 pitch);
};

} // namespace aver::vehicle
