// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The CVehicle surface on Entity: make a vehicle, drive it, change gear, get in and out, read telemetry, tune it live. Design:
// docs/VEHICLES.md.
//
// CVehicle is registered at RUNTIME like CMotionMatcher, so adding it and the pedals are field writes over the generic scene ABI
// and a host that never registered it gets false, not a throw. What HAPPENS (gears, flip, repair, damage, getting in) goes through
// the framework relay. Centimetres, degrees, N*m throughout.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private const string VehicleType = "CVehicle";

    // False, not a throw, in a process with no native scene (the bare-process graph tests, a tool).
    private bool HasVehicleComponent
    {
        get
        {
            try { return HasComponent(VehicleType); }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
        }
    }

    /// <summary>Adds a Vehicle component: from an .ocvehicle <paramref name="asset"/> path, or, with no asset, from the built-in
    /// <paramref name="preset"/> sized by this entity's mesh (so it needs a mesh then). False for a dead entity, a preset vehicle
    /// with no mesh, or a build without the vehicle module.</summary>
    public bool AddVehicle(string asset = "", VehiclePreset preset = VehiclePreset.Car)
    {
        try
        {
            bool hasAsset = !string.IsNullOrEmpty(asset);
            if (!hasAsset && !HasComponent(Component.MeshRenderer)) return false;
            if (!HasVehicleComponent && !AddComponent(VehicleType)) return false;
            SetInt("CVehicle.preset", (int)preset);
            return SetInt64("CVehicle.asset", hasAsset ? Assets.ObjectIdOf(asset) : 0);
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>True when the entity carries a Vehicle component.</summary>
    public bool IsVehicle => HasVehicleComponent;

    /// <summary>The driver's pedals: throttle -1..1 (negative reverses or brakes, see the aids), steer -1..1 (+1 right), brake and
    /// handbrake 0..1. They hold until set again. A driver's Drive.* actions overwrite them each frame while somebody is in the seat.</summary>
    public bool SetVehicleInput(float throttle, float steer, float brake = 0f, float handbrake = 0f)
    {
        if (!HasVehicleComponent) return false;
        SetFloat("CVehicle.inThrottle", Math.Clamp(throttle, -1f, 1f));
        SetFloat("CVehicle.inSteer", Math.Clamp(steer, -1f, 1f));
        SetFloat("CVehicle.inBrake", Math.Clamp(brake, 0f, 1f));
        return SetFloat("CVehicle.inHandbrake", Math.Clamp(handbrake, 0f, 1f));
    }

    /// <summary>The vehicle's state as of the last fixed physics step. False when the entity is not a built vehicle.</summary>
    public bool TryGetVehicleTelemetry(out VehicleTelemetry telemetry)
    {
        telemetry = default;
        var f = new float[FwVehicle.TfCount];
        var i = new int[FwVehicle.TiCount];
        try
        {
            if (FwVehicle.aver_fw_vehicle_telemetry(Handle, f, f.Length, i, i.Length) == 0) return false;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
        int state = i[FwVehicle.TiState];
        telemetry = new VehicleTelemetry(
            f[FwVehicle.TfSpeedCmS], f[FwVehicle.TfForwardSpeedCmS], f[FwVehicle.TfRpm], i[FwVehicle.TiGear], i[FwVehicle.TiGearCount],
            i[FwVehicle.TiManual] != 0, i[FwVehicle.TiShifting] != 0, f[FwVehicle.TfThrottle], f[FwVehicle.TfBrake], f[FwVehicle.TfSteer],
            f[FwVehicle.TfHandbrake], i[FwVehicle.TiWheelsOnGround], i[FwVehicle.TiAbsActive] != 0, i[FwVehicle.TiTcsActive] != 0,
            (state & FwVehicle.StateFlipped) != 0, (state & FwVehicle.StateDestroyed) != 0, f[FwVehicle.TfHealth01], f[FwVehicle.TfAirborneSeconds],
            f[FwVehicle.TfLongAccelG], f[FwVehicle.TfLatAccelG], f[FwVehicle.TfTrackLeftCmS], f[FwVehicle.TfTrackRightCmS]);
        return true;
    }

    /// <summary>One wheel's state as of the last fixed physics step. False for a bad index or a vehicle that is not built.</summary>
    public bool TryGetWheelTelemetry(int wheel, out WheelTelemetry telemetry)
    {
        telemetry = default;
        var f = new float[FwVehicle.WfCount];
        var i = new int[FwVehicle.WiCount];
        try
        {
            if (FwVehicle.aver_fw_vehicle_wheel(Handle, wheel, f, f.Length, i, i.Length) == 0) return false;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
        telemetry = new WheelTelemetry(
            i[FwVehicle.WiContact] != 0, f[FwVehicle.WfSlipLong], f[FwVehicle.WfSlipLatDeg], f[FwVehicle.WfLoadN], f[FwVehicle.WfCompressionCm],
            f[FwVehicle.WfAngVelRadS], f[FwVehicle.WfSteerDeg], f[FwVehicle.WfSpinRad],
            new Vec3(f[FwVehicle.WfContactPointX], f[FwVehicle.WfContactPointY], f[FwVehicle.WfContactPointZ]),
            new Vec3(f[FwVehicle.WfContactNormalX], f[FwVehicle.WfContactNormalY], f[FwVehicle.WfContactNormalZ]),
            f[FwVehicle.WfForceLongN], f[FwVehicle.WfForceLatN], f[FwVehicle.WfSurfaceFriction], new Entity(i[FwVehicle.WiContactEntity]),
            i[FwVehicle.WiSkidding] != 0);
        return true;
    }

    /// <summary>How many wheels the vehicle has; 0 when it is not built.</summary>
    public int VehicleWheelCount
    {
        get
        {
            var i = new int[FwVehicle.TiCount];
            try { return FwVehicle.aver_fw_vehicle_telemetry(Handle, null, 0, i, i.Length) != 0 ? i[FwVehicle.TiWheelCount] : 0; }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
        }
    }

    /// <summary>Switches between an automatic and a manual gearbox.</summary>
    public bool SetVehicleManualGearbox(bool manual)
    {
        try { return FwVehicle.aver_fw_vehicle_set_manual(Handle, manual ? 1 : 0) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Manual gearbox: -1 reverse, 0 neutral, 1 and up. False when out of range or the box is automatic.</summary>
    public bool SetVehicleGear(int gear)
    {
        try { return FwVehicle.aver_fw_vehicle_set_gear(Handle, gear) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Manual gearbox: one gear up (+1) or down (-1), through a timed clutch.</summary>
    public bool ShiftVehicle(int delta)
    {
        try { return FwVehicle.aver_fw_vehicle_shift(Handle, delta) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Puts <paramref name="driver"/> in the seat: the Drive.* actions drive the vehicle, the driver is hidden and parked, and the
    /// camera rigs are made and the view moves to the first. False if the seat is taken, the driver is already driving, or this is not a built vehicle.</summary>
    public bool EnterVehicle(Entity driver)
    {
        try { return FwVehicle.aver_fw_vehicle_enter(Handle, driver.Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Gets the driver out beside the vehicle and gives the view back. False when nobody is driving.</summary>
    public bool ExitVehicle()
    {
        try { return FwVehicle.aver_fw_vehicle_exit(Handle, out _) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Who is in the seat; <c>default</c> when nobody is.</summary>
    public Entity VehicleDriver
    {
        get
        {
            var i = new int[FwVehicle.TiCount];
            try { return FwVehicle.aver_fw_vehicle_telemetry(Handle, null, 0, i, i.Length) != 0 ? new Entity(i[FwVehicle.TiDriver]) : default; }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return default; }
        }
    }

    /// <summary>Puts the vehicle upright about its heading, stops it dead and sets it down <paramref name="liftCm"/> higher.</summary>
    public bool FlipVehicle(float liftCm = 50f)
    {
        try { return FwVehicle.aver_fw_vehicle_flip(Handle, liftCm) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Restores full health and gives the engine back.</summary>
    public bool RepairVehicle()
    {
        try { return FwVehicle.aver_fw_vehicle_repair(Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Takes <paramref name="amount01"/> of the vehicle's health (1 destroys it, which cuts the engine); a negative amount gives
    /// health back. Needs a DAMAGE record in the vehicle's spec.</summary>
    public bool DamageVehicle(float amount01)
    {
        try { return FwVehicle.aver_fw_vehicle_damage(Handle, amount01) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Live tuning by key (case-insensitive): <c>power</c>, <c>brake</c>, <c>grip</c>, <c>suspension</c>, <c>steer</c>, <c>drag</c>,
    /// <c>downforce</c> (scales, 1 = as authored), <c>abs</c>, <c>tcs</c>, <c>manual</c> (0 or 1) and <c>collisionperiod</c> (1 to 8). An
    /// unknown key is false.</summary>
    public bool TuneVehicle(string key, float value)
    {
        if (string.IsNullOrEmpty(key)) return false;
        try { return FwVehicle.aver_fw_vehicle_tune(Handle, key, value) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }
}
