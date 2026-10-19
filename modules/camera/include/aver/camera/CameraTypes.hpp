// Aver.Camera types: modes, presets, flag bits, the CCameraRig component and the pose that travels between the rig system,
// the blend and the host. Core only (no scene, no physics), so the editor's Details model and the pure tests include it
// anywhere. Design: docs/CAMERAS.md.
//
// UNITS AND AXES ARE THE ENGINE'S: centimetres, +X forward, +Y right, +Z up, left-handed. Yaw/pitch follow
// game::cameraForward (positive pitch looks up); radians in C++ unless a field name says Deg, degrees at every authored
// or scripted boundary.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

namespace aver::camera {

enum class CameraMode : u32 { Chase = 0, Orbit = 1, FirstPerson = 2, Seat = 3, TopDown = 4, Fixed = 5 };
inline constexpr u32 kCameraModeCount = 6;
enum class CameraPreset : u32 { Chase = 0, VehicleChase = 1, Orbit = 2, FirstPerson = 3, Seat = 4, TopDown = 5, Fixed = 6 };
inline constexpr u32 kCameraPresetCount = 7;
// 0 is the zero-filled default. EaseInOut is smoothstep, the sequencer's smooth01.
enum class BlendCurve : u32 { EaseInOut = 0, Linear = 1, EaseIn = 2, EaseOut = 3 };
enum class ModKind : u32 { FovKick = 0, FovScale = 1, Punch = 2, RollLean = 3, ArmScale = 4, SocketOffset = 5 };
inline constexpr u32 kModKindCount = 6;

// Negative sense throughout, so zero-filled storage is a working Chase rig.
inline constexpr u32 kRigNoCollision = 1u << 0;
inline constexpr u32 kRigNoInheritYaw = 1u << 1;     // yaw comes from the rig's own yawDeg instead of the source rotation
inline constexpr u32 kRigNoInheritPitch = 1u << 2;
inline constexpr u32 kRigNoInheritRoll = 1u << 3;
inline constexpr u32 kRigNoFade = 1u << 4;
inline constexpr u32 kRigLookAtTarget = 1u << 5;     // Fixed: face the target, smoothed by rotLagSpeed
inline constexpr u32 kRigDisabled = 1u << 6;         // never selected
inline constexpr u32 kRigApplyPreset = 1u << 30;     // one-shot: the system applies `preset` over the tuning fields and clears it
inline constexpr u32 kRigCut = 1u << 31;             // one-shot: the system snaps the lag state next tick and clears it

// CCameraRig::outState
inline constexpr u32 kRigStateActive = 1u << 0, kRigStateBlending = 1u << 1, kRigStateColliding = 1u << 2,
                     kRigStateFaded = 1u << 3, kRigStateLagging = 1u << 4;

// ONE RIG, ALL THE TUNING (docs/CAMERAS.md). 160 bytes, 40 four-byte fields, registered by CameraRigSystem as "CCameraRig".
// A value of 0 in a numeric tuning field reads as the default shown; "off" is stated where 0 means off. Lengths in cm,
// angles in degrees, times in seconds. Zero-filled storage is a valid Chase rig.
struct CCameraRig {
    u32 flags = 0;               // kRig* bits
    u32 mode = 0;                // CameraMode; changing it at run time blends over blendTimeS
    u32 preset = 0;              // CameraPreset: the base the editor and the level record start from; kRigApplyPreset applies it
    u32 target = 0;              // entity the arm follows; 0 = the rig's parent, else the rig's own entity
    i32 ignoreBody = 0;          // extra physics body the probe skips (a vehicle chassis). Process-local, never saved
    f32 armLengthCm = 0;         // 0 reads 450
    f32 minArmCm = 0;            // 0 reads 20: the closest the camera may come to the pivot
    f32 probeRadiusCm = 0;       // 0 reads 12
    u32 collisionMask = 0;       // physics layer bitmask; 0 = every layer except cloth and proxy
    f32 recoverHalfLifeS = 0;    // 0 reads 0.25: how fast the arm returns outward after a hit (inward is instant)
    f32 socketX = 0, socketY = 0, socketZ = 0;   // offset at the arm's end in the camera frame (forward, right, up)
    f32 pivotX = 0, pivotY = 0, pivotZ = 0;      // offset of the arm origin in world axes (eye height lives here)
    f32 posLagSpeed = 0;         // 0 = no position lag; else 1/s
    f32 lagMaxDistCm = 0;        // 0 = unlimited
    f32 rotLagSpeed = 0;         // 0 = no rotation lag
    f32 rotLagMaxDeg = 0;        // 0 = unlimited
    f32 lagMaxStepS = 0;         // 0 reads 1/60: lag is integrated in steps no longer than this
    f32 lookAheadS = 0;          // 0 = off: the pivot leads by planar velocity times this
    f32 lookAheadMaxCm = 0;      // 0 reads 150
    f32 pitchMinDeg = 0, pitchMaxDeg = 0;        // both 0 reads -80 / +80
    f32 yawDeg = 0, pitchDeg = 0;                // the rig's own rotation (Orbit/TopDown/Fixed, a no-inherit axis, Seat head look)
    f32 recenterDelayS = 0;      // 0 = off: eases behind the target after this long without look input
    f32 blendTimeS = 0;          // seconds to blend INTO this rig/mode; 0 = cut
    u32 blendCurve = 0;          // BlendCurve
    f32 fovKickDeg = 0;          // 0 = off: added FOV at fovKickSpeedCmS
    f32 fovKickSpeedCmS = 0;     // 0 reads 1500
    f32 fadeStartCm = 0, fadeEndCm = 0;          // both 0 reads 140 / 70 unless kRigNoFade
    f32 bobAmount = 0;           // 0 = off: FirstPerson/Seat head bob through CameraFx
    f32 seatYawLimitDeg = 0, seatPitchLimitDeg = 0;   // 0 reads 110 / 50
    // OUT (written by the system; registered read-only)
    f32 outArmCm = 0;            // arm length after collision and recovery
    u32 outState = 0;            // kRigState*
    f32 outFovDeg = 0;           // final FOV of this rig's last evaluation
};
static_assert(sizeof(CCameraRig) == 160, "CCameraRig must be padding-free: 40 four-byte fields");

// Defaults a 0 field reads as.
inline constexpr f32 kDefaultArmCm = 450.0f, kDefaultMinArmCm = 20.0f, kDefaultProbeCm = 12.0f, kDefaultRecoverS = 0.25f,
                     kDefaultLagStepS = 1.0f / 60.0f, kDefaultLookAheadMaxCm = 150.0f, kDefaultPitchLimitDeg = 80.0f,
                     kDefaultFovKickSpeed = 1500.0f, kDefaultFadeStartCm = 140.0f, kDefaultFadeEndCm = 70.0f,
                     kDefaultSeatYawDeg = 110.0f, kDefaultSeatPitchDeg = 50.0f;

// A camera pose: position, rotation as yaw/pitch/roll, projection.
struct CameraPov {
    Vec3 pos;
    f32 yaw = 0, pitch = 0, roll = 0;
    f32 fovYRad = 1.0471976f;    // 60 degrees, what pushCamera used before FOV was an input
    f32 nearCm = 2, farCm = 200000;
};

struct ViewBlend {
    f32 seconds = 0.5f;
    BlendCurve curve = BlendCurve::EaseInOut;
    bool cut = false;
};

}  // namespace aver::camera
