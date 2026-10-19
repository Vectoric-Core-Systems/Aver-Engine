// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_vehicle_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The vehicle C ABI (framework_vehicle_abi.h). Declared here, not in a bridge assembly, for the same reason as
/// <see cref="Fw"/>: only this assembly has the DllImport resolver. Every call lands at once in the vehicle system (the physics
/// world is idle between steps) and the telemetry reads are the last fixed step's. The block layouts below are that header's
/// AVER_FW_VEH_TF_ / _TI_ / _WF_ / _WI_ indices.</summary>
internal static class FwVehicle
{
    private const string Lib = "Aver.Framework";

    // Telemetry floats (the physics VS block, then health) and ints (the VSI block, then the state bits and the driver).
    internal const int TfRpm = 0, TfEngineTorqueNm = 1, TfSpeedCmS = 2, TfForwardSpeedCmS = 3, TfLateralSpeedCmS = 4, TfThrottle = 5, TfBrake = 6,
        TfSteer = 7, TfHandbrake = 8, TfClutch = 9, TfWheelRpmAtClutch = 10, TfYawRateDegS = 11, TfLongAccelG = 12, TfLatAccelG = 13, TfUpZ = 14,
        TfTrackLeftCmS = 15, TfTrackRightCmS = 16, TfAirborneSeconds = 17, TfHealth01 = 18, TfCount = 19;
    internal const int TiGear = 0, TiGearCount = 1, TiManual = 2, TiShifting = 3, TiWheelsOnGround = 4, TiAbsActive = 5, TiTcsActive = 6, TiKind = 7,
        TiAxleCount = 8, TiWheelCount = 9, TiRevLimited = 10, TiState = 11, TiDriver = 12, TiCount = 13;
    internal const int WfSlipLong = 0, WfSlipLatDeg = 1, WfLoadN = 2, WfCompressionCm = 3, WfSuspLengthCm = 4, WfAngVelRadS = 5, WfSteerDeg = 6,
        WfSpinRad = 7, WfContactPointX = 8, WfContactPointY = 9, WfContactPointZ = 10, WfContactNormalX = 11, WfContactNormalY = 12,
        WfContactNormalZ = 13, WfForceLongN = 14, WfForceLatN = 15, WfSurfaceFriction = 16, WfBrakeTorqueNm = 17, WfCount = 18;
    internal const int WiContact = 0, WiContactBody = 1, WiContactEntity = 2, WiSkidding = 3, WiCount = 4;
    internal const int EventInts = 6, EventFloats = 8;

    // CVehicle.outState bits.
    internal const int StateFlipped = 0x20, StateDestroyed = 0x80;

    [DllImport(Lib)] internal static extern int aver_fw_vehicle_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_vehicle_spawn([MarshalAs(UnmanagedType.LPUTF8Str)] string assetOrPresetOrInlineText, float x, float y, float z, float yawDeg, out int outEntity, out int outVehicle);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_enter(int vehicle, int driver);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_exit(int vehicle, out int outDriver);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_shift(int vehicle, int delta);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_set_gear(int vehicle, int gear);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_set_manual(int vehicle, int manual);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_flip(int vehicle, float liftCm);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_repair(int vehicle);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_damage(int vehicle, float amount01);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_tune(int vehicle, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, float value);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_telemetry(int vehicle, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_wheel(int vehicle, int wheel, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_fw_vehicle_drain_events(int[] outI, float[] outF, int max);
}
