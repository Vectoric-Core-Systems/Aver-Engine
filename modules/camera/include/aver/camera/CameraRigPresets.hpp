// The seven rig presets (docs/CAMERAS.md) and the "0 reads the default" resolution of a CCameraRig's tuning fields.
// Header-only and Core only: the editor, the level record, C# AddCameraRig and the system all go through this one table.
// Every unspecified field is 0, which reads as the default.
#pragma once

#include "aver/camera/CameraTypes.hpp"

namespace aver::camera {

inline CCameraRig presetRig(CameraPreset p) {
    CCameraRig r;
    r.preset = static_cast<u32>(p);
    switch (p) {
        case CameraPreset::Chase:
            r.mode = static_cast<u32>(CameraMode::Chase);
            r.armLengthCm = 450; r.probeRadiusCm = 12;
            r.pitchMinDeg = -80; r.pitchMaxDeg = 80;
            r.blendTimeS = 0.4f;
            r.fadeStartCm = 140; r.fadeEndCm = 70;
            break;
        case CameraPreset::VehicleChase:
            r.mode = static_cast<u32>(CameraMode::Chase);
            r.armLengthCm = 600; r.probeRadiusCm = 14;
            r.pivotZ = 120;
            r.socketZ = 60;
            r.posLagSpeed = 8; r.lagMaxDistCm = 150;
            r.rotLagSpeed = 5;
            r.lookAheadS = 0.25f; r.lookAheadMaxCm = 250;
            r.flags = kRigNoInheritPitch | kRigNoFade;
            r.pitchDeg = -12;
            r.blendTimeS = 0.8f;
            r.fovKickDeg = 10; r.fovKickSpeedCmS = 3000;
            break;
        case CameraPreset::Orbit:
            r.mode = static_cast<u32>(CameraMode::Orbit);
            r.armLengthCm = 500; r.probeRadiusCm = 12;
            r.posLagSpeed = 12;
            r.pitchMinDeg = -75; r.pitchMaxDeg = 75;
            r.pitchDeg = -20;
            r.blendTimeS = 0.5f;
            r.fadeStartCm = 140; r.fadeEndCm = 70;
            break;
        case CameraPreset::FirstPerson:
            r.mode = static_cast<u32>(CameraMode::FirstPerson);
            r.pitchMinDeg = -85; r.pitchMaxDeg = 85;
            r.blendTimeS = 0.25f;
            r.bobAmount = 0.5f;
            break;
        case CameraPreset::Seat:
            r.mode = static_cast<u32>(CameraMode::Seat);
            r.posLagSpeed = 25; r.lagMaxDistCm = 20;
            r.blendTimeS = 0.6f;
            r.seatYawLimitDeg = 110; r.seatPitchLimitDeg = 50;
            r.fovKickDeg = 6; r.fovKickSpeedCmS = 3000;
            break;
        case CameraPreset::TopDown:
            r.mode = static_cast<u32>(CameraMode::TopDown);
            r.armLengthCm = 1400;
            r.posLagSpeed = 6;
            r.lookAheadS = 0.4f; r.lookAheadMaxCm = 250;
            r.pitchDeg = -62;
            r.blendTimeS = 0.7f;
            r.flags = kRigNoInheritYaw | kRigNoInheritPitch | kRigNoInheritRoll | kRigNoCollision;
            break;
        case CameraPreset::Fixed:
            r.mode = static_cast<u32>(CameraMode::Fixed);
            r.blendTimeS = 0.5f;
            break;
    }
    return r;
}

inline CameraMode modeOf(const CCameraRig& r) {
    return r.mode < kCameraModeCount ? static_cast<CameraMode>(r.mode) : CameraMode::Chase;
}
inline BlendCurve curveOf(u32 v) { return v <= static_cast<u32>(BlendCurve::EaseOut) ? static_cast<BlendCurve>(v) : BlendCurve::EaseInOut; }

// A rig's tuning with every "0 reads the default" resolved. Angles stay in degrees as authored.
struct RigTuning {
    f32 armCm, minArmCm, probeCm, recoverS, lagStepS, lookAheadMaxCm, pitchMinDeg, pitchMaxDeg, fovKickSpeed;
    f32 fadeStartCm, fadeEndCm, seatYawDeg, seatPitchDeg;
};

inline RigTuning resolveTuning(const CCameraRig& r) {
    RigTuning t;
    t.armCm = r.armLengthCm != 0 ? r.armLengthCm : kDefaultArmCm;
    t.minArmCm = r.minArmCm != 0 ? r.minArmCm : kDefaultMinArmCm;
    t.probeCm = r.probeRadiusCm != 0 ? r.probeRadiusCm : kDefaultProbeCm;
    t.recoverS = r.recoverHalfLifeS != 0 ? r.recoverHalfLifeS : kDefaultRecoverS;
    t.lagStepS = r.lagMaxStepS != 0 ? r.lagMaxStepS : kDefaultLagStepS;
    t.lookAheadMaxCm = r.lookAheadMaxCm != 0 ? r.lookAheadMaxCm : kDefaultLookAheadMaxCm;
    const bool noLimits = r.pitchMinDeg == 0 && r.pitchMaxDeg == 0;
    t.pitchMinDeg = noLimits ? -kDefaultPitchLimitDeg : r.pitchMinDeg;
    t.pitchMaxDeg = noLimits ? kDefaultPitchLimitDeg : r.pitchMaxDeg;
    t.fovKickSpeed = r.fovKickSpeedCmS != 0 ? r.fovKickSpeedCmS : kDefaultFovKickSpeed;
    const bool noFade = r.fadeStartCm == 0 && r.fadeEndCm == 0;
    t.fadeStartCm = noFade ? kDefaultFadeStartCm : r.fadeStartCm;
    t.fadeEndCm = noFade ? kDefaultFadeEndCm : r.fadeEndCm;
    t.seatYawDeg = r.seatYawLimitDeg != 0 ? r.seatYawLimitDeg : kDefaultSeatYawDeg;
    t.seatPitchDeg = r.seatPitchLimitDeg != 0 ? r.seatPitchLimitDeg : kDefaultSeatPitchDeg;
    return t;
}

// The near plane's corner distance from the view axis at the given FOV and aspect: a probe radius below it lets the
// camera see through the surface it stopped on. near * tan(fov/2) * sqrt(aspect^2 + 1).
inline f32 nearCornerRadiusCm(f32 nearCm, f32 fovYRad, f32 aspect) {
    return nearCm * std::tan(fovYRad * 0.5f) * std::sqrt(aspect * aspect + 1.0f);
}

}  // namespace aver::camera
