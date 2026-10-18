// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The entry points the vehicle Aver Node nodes call (GraphVehicleNodeTable.cs; palette mirror
// sandbox/src/GraphNodeDefsVehicle.hpp), each a thin wrapper over Vehicles and Car (Vehicles.cs). A vehicle crosses the graph
// as its integer handle (0 = none), the way an entity does. A dead or unknown handle, or a missing physics library, answers
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
}
