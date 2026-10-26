// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the vehicle Aver Node nodes call (GraphVehicleNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsVehicle.hpp), each a thin wrapper over Vehicles, Car and the Entity vehicle members. The six original
// rows hold a vehicle by its integer physics handle (0 = none); the rest hold it by its ENTITY, the way every other
// family does, and an_vehiclefromentity maps one to the other. A dead or unknown handle, or a missing physics library, answers
// false/0, logged once per node.

using Aver.Scene;

namespace Aver.Framework;

internal static class VehicleGraph
{
    private const string NoCar = "no car with that vehicle handle (it was never made by AN_VehicleCreateCar, or was destroyed)";

    /// <summary>AN_VehicleCreateCar: a default car (CarSpec.Default) of massKg at a world point facing yaw degrees. Returns the vehicle
    /// handle, 0 on failure; entity is the scene entity that follows the car.</summary>
    internal static int CreateCarForGraph(float x, float y, float z, float yaw, float massKg, out int entity)
    {
        entity = 0;
        try
        {
            Car car = Vehicles.CreateCar(new Vec3(x, y, z), yaw, CarSpec.Default with { MassKg = massKg });
            if (!FamiliesBLog.Ok("AN_VehicleCreateCar", car.IsValid, "the physics layer refused the car (no physics world, or the mass or position is not valid)"))
                return 0;
            entity = car.Entity.Handle;
            return car.Handle;
        }
        catch (Exception e)
        {
            FamiliesBLog.Failed("AN_VehicleCreateCar", e);
            return 0;
        }
    }

    /// <summary>AN_VehicleSetInput: throttle -1..1 (negative reverses), steer -1..1 (+1 toward the car's right), brake and handbrake 0..1.
    /// The input holds until set again.</summary>
    internal static bool SetInputForGraph(int vehicle, float throttle, float steer, float brake, float handbrake)
    {
        try
        {
            return FamiliesBLog.Ok("AN_VehicleSetInput", Vehicles.TryGet(vehicle, out Car car) && car.SetInput(throttle, steer, brake, handbrake), NoCar);
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleSetInput", e); }
    }

    /// <summary>AN_VehicleGetState: forward speed in cm/s (negative when reversing), the origin position and the heading in degrees.
    /// found is false for a dead car.</summary>
    internal static bool GetStateForGraph(int vehicle, out float speedCmS, out float x, out float y, out float z, out float yaw)
    {
        speedCmS = 0f; x = 0f; y = 0f; z = 0f; yaw = 0f;
        try
        {
            if (!Vehicles.TryGet(vehicle, out Car car) || !car.TryGetState(out CarState s)) return false;
            speedCmS = s.ForwardSpeedCmS;
            x = s.Position.X; y = s.Position.Y; z = s.Position.Z;
            yaw = s.YawDegrees;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleGetState", e); }
    }

    /// <summary>AN_VehicleSetPose: puts the car's origin at a world point facing yaw degrees, stops it dead and wakes it.</summary>
    internal static bool SetPoseForGraph(int vehicle, float x, float y, float z, float yaw)
    {
        try
        {
            return FamiliesBLog.Ok("AN_VehicleSetPose", Vehicles.TryGet(vehicle, out Car car) && car.SetPose(new Vec3(x, y, z), yaw), NoCar);
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleSetPose", e); }
    }

    /// <summary>AN_VehicleDestroy: removes the vehicle, its chassis and its entity.</summary>
    internal static bool DestroyForGraph(int vehicle)
    {
        try
        {
            return FamiliesBLog.Ok("AN_VehicleDestroy", Vehicles.TryGet(vehicle, out Car car) && car.Destroy(), NoCar);
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleDestroy", e); }
    }

    /// <summary>AN_VehicleWheelContact: whether wheel index touched ground on the last physics step. False for a dead car or a bad index.</summary>
    internal static bool WheelContactForGraph(int vehicle, int index)
    {
        try { return Vehicles.TryGet(vehicle, out Car car) && car.WheelContact(index); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleWheelContact", e); }
    }

    // ---- version 2: rows by ENTITY ---------------------------------------------------------------------------

    private const string NoVehicle = "the entity is not a built vehicle (no Vehicle component, or it was not built, or the host has no vehicle system)";

    /// <summary>AN_VehicleSpawn: an .ocvehicle path or preset name (the node's asset attribute) at a world point facing yaw degrees. Returns the
    /// physics handle the older rows take; entity is the new entity.</summary>
    internal static int SpawnForGraph(float x, float y, float z, float yaw, string asset, out int entity, out bool success)
    {
        entity = 0; success = false;
        try
        {
            Entity e = Vehicles.SpawnFromText(asset ?? string.Empty, new Vec3(x, y, z), yaw, out int vehicle);
            success = FamiliesBLog.Ok("AN_VehicleSpawn", e.IsValid, "the vehicle was refused (no vehicle system in this host, an unknown asset or preset, or an invalid spec)");
            if (!success) return 0;
            entity = e.Handle;
            return vehicle;
        }
        catch (Exception ex) { FamiliesBLog.Failed("AN_VehicleSpawn", ex); return 0; }
    }

    /// <summary>AN_VehicleDrive: the pedals of the vehicle on an entity (throttle -1..1, steer -1..1, brake and handbrake 0..1); they hold until set again.</summary>
    internal static bool DriveForGraph(int entity, float throttle, float steer, float brake, float handbrake)
    {
        try { return FamiliesBLog.Ok("AN_VehicleDrive", new Entity(entity).SetVehicleInput(throttle, steer, brake, handbrake), NoVehicle); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleDrive", e); }
    }

    /// <summary>AN_VehicleShift: one gear up (+1) or down (-1) in a manual gearbox.</summary>
    internal static bool ShiftForGraph(int entity, int delta)
    {
        try { return FamiliesBLog.Ok("AN_VehicleShift", new Entity(entity).ShiftVehicle(delta), "the entity is not a vehicle with a manual gearbox"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleShift", e); }
    }

    /// <summary>AN_VehicleSetGear: -1 reverse, 0 neutral, 1 and up, in a manual gearbox.</summary>
    internal static bool SetGearForGraph(int entity, int gear)
    {
        try { return FamiliesBLog.Ok("AN_VehicleSetGear", new Entity(entity).SetVehicleGear(gear), "the entity is not a vehicle with a manual gearbox, or the gear does not exist"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleSetGear", e); }
    }

    /// <summary>AN_VehicleGearMode: switches the gearbox between automatic (false) and manual (true).</summary>
    internal static bool GearModeForGraph(int entity, bool manual)
    {
        try { return FamiliesBLog.Ok("AN_VehicleGearMode", new Entity(entity).SetVehicleManualGearbox(manual), NoVehicle); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleGearMode", e); }
    }

    /// <summary>AN_VehicleEnter: puts the driver entity in the vehicle's seat.</summary>
    internal static bool EnterForGraph(int entity, int driver)
    {
        try { return FamiliesBLog.Ok("AN_VehicleEnter", new Entity(entity).EnterVehicle(new Entity(driver)), "the seat is taken, the driver is already driving, or the entity is not a built vehicle"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleEnter", e); }
    }

    /// <summary>AN_VehicleExit: gets the driver out; driver is the entity that left (0 when nobody was in).</summary>
    internal static bool ExitForGraph(int entity, out int driver)
    {
        driver = 0;
        try
        {
            var v = new Entity(entity);
            Entity d = v.VehicleDriver;
            bool ok = FamiliesBLog.Ok("AN_VehicleExit", v.ExitVehicle(), "nobody is driving that vehicle");
            driver = ok ? d.Handle : 0;
            return ok;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleExit", e); }
    }

    /// <summary>AN_VehicleFlip: puts the vehicle upright about its heading, stopped, liftCm higher.</summary>
    internal static bool FlipForGraph(int entity, float liftCm)
    {
        try { return FamiliesBLog.Ok("AN_VehicleFlip", new Entity(entity).FlipVehicle(liftCm), NoVehicle); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleFlip", e); }
    }

    /// <summary>AN_VehicleTune: a live tuning value by the node's key attribute (power, brake, grip, suspension, steer, drag, downforce, abs, tcs, manual, collisionperiod).</summary>
    internal static bool TuneForGraph(int entity, string key, float value)
    {
        try { return FamiliesBLog.Ok("AN_VehicleTune", new Entity(entity).TuneVehicle(key ?? string.Empty, value), "unknown key, or the entity is not a built vehicle"); }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleTune", e); }
    }

    /// <summary>AN_VehicleRepair: gives health back, amount being the fraction of full health restored (1 or more is a full repair, which also brings a destroyed vehicle's engine back).</summary>
    internal static bool RepairForGraph(int entity, float amount)
    {
        try
        {
            var v = new Entity(entity);
            bool ok = amount >= 1f ? v.RepairVehicle() : (amount > 0f && v.DamageVehicle(-amount));
            return FamiliesBLog.Ok("AN_VehicleRepair", ok, "the entity is not a built vehicle with a DAMAGE record, or the amount is not positive");
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleRepair", e); }
    }

    /// <summary>AN_VehicleTelemetry: the vehicle's state as of the last physics step. found is false when the entity is not a built vehicle.</summary>
    internal static bool TelemetryForGraph(int entity, out float speedCmS, out float rpm, out int gear, out float throttle, out float steer,
                                           out float brake, out int wheelsOnGround, out float health, out bool abs, out bool tcs)
    {
        speedCmS = 0f; rpm = 0f; gear = 0; throttle = 0f; steer = 0f; brake = 0f; wheelsOnGround = 0; health = 0f; abs = false; tcs = false;
        try
        {
            if (!new Entity(entity).TryGetVehicleTelemetry(out VehicleTelemetry t)) return false;
            speedCmS = t.SpeedCmS; rpm = t.Rpm; gear = t.Gear; throttle = t.Throttle; steer = t.Steer; brake = t.Brake;
            wheelsOnGround = t.WheelsOnGround; health = t.Health01; abs = t.AbsActive; tcs = t.TcsActive;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleTelemetry", e); }
    }

    /// <summary>AN_VehicleWheelState: one wheel's contact, slip, load, suspension compression, speed, surface grip and whether it is skidding.</summary>
    internal static bool WheelStateForGraph(int entity, int index, out bool contact, out float slipLong, out float slipLatDeg, out float loadN,
                                            out float compressionCm, out float angVelRadS, out float surfaceFriction, out bool skidding)
    {
        contact = false; slipLong = 0f; slipLatDeg = 0f; loadN = 0f; compressionCm = 0f; angVelRadS = 0f; surfaceFriction = 0f; skidding = false;
        try
        {
            if (!new Entity(entity).TryGetWheelTelemetry(index, out WheelTelemetry w)) return false;
            contact = w.Contact; slipLong = w.SlipLongitudinal; slipLatDeg = w.SlipLateralDeg; loadN = w.LoadN; compressionCm = w.CompressionCm;
            angVelRadS = w.AngularVelocityRadS; surfaceFriction = w.SurfaceFriction; skidding = w.Skidding;
            return true;
        }
        catch (Exception e) { return FamiliesBLog.Failed("AN_VehicleWheelState", e); }
    }

    /// <summary>AN_VehicleDriver: the entity in the vehicle's seat, 0 when nobody is.</summary>
    internal static int DriverForGraph(int entity)
    {
        try { return new Entity(entity).VehicleDriver.Handle; }
        catch (Exception e) { FamiliesBLog.Failed("AN_VehicleDriver", e); return 0; }
    }

    /// <summary>AN_VehicleFromEntity: the physics handle the six original rows take, 0 when the entity is not a built vehicle.</summary>
    internal static bool FromEntityForGraph(int entity, out int vehicle)
    {
        vehicle = 0;
        try
        {
            var e = new Entity(entity);
            if (!e.IsVehicle) return false;
            int handle = e.GetInt("CVehicle.vehicle");
            if (handle == 0) return false;
            Vehicles.Adopt(handle, e);
            vehicle = handle;
            return true;
        }
        catch (Exception ex) { return FamiliesBLog.Failed("AN_VehicleFromEntity", ex); }
    }
}
