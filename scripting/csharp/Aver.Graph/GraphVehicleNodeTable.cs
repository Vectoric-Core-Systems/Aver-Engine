// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The vehicle rows of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2; family version 2 is
// docs/plans/wave10/VEHICLES.md section 2.11). Every method is in VehicleGraph (Aver.Framework/VehicleGraph.cs), a thin wrapper over
// Vehicles, Car and the Entity vehicle members. The six original rows hold a vehicle by its integer physics handle; the fourteen
// appended ones hold it by its entity (an_vehiclefromentity maps one to the other). Vehicle events reach graphs through the named-event
// seam (AN_OnEvent: VehicleCrash, VehicleSkid, VehicleLanded, VehicleFlipped, VehicleShift, VehicleEntered, VehicleExited), so they need
// no node. Palette mirror: sandbox/src/GraphNodeDefsVehicle.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] VehicleTable() => new[]
    {
        Reg("an_vehiclecreatecar", GenericKind.Exec, typeof(VehicleGraph), "CreateCarForGraph", "x:float,y:float,z:float,yaw:float,massKg:float=1500", "vehicle:int,entity:int", "p:x,p:y,p:z,p:yaw,p:massKg,>entity", "vehicle"),
        Reg("an_vehiclesetinput", GenericKind.Exec, typeof(VehicleGraph), "SetInputForGraph", "vehicle:int,throttle:float,steer:float,brake:float,handbrake:float", "success:bool", "p:vehicle,p:throttle,p:steer,p:brake,p:handbrake", "success"),
        Reg("an_vehiclegetstate", GenericKind.Pure, typeof(VehicleGraph), "GetStateForGraph", "vehicle:int", "found:bool,speedCmS:float,x:float,y:float,z:float,yaw:float", "p:vehicle,>speedCmS,>x,>y,>z,>yaw", "found"),
        Reg("an_vehiclesetpose", GenericKind.Exec, typeof(VehicleGraph), "SetPoseForGraph", "vehicle:int,x:float,y:float,z:float,yaw:float", "success:bool", "p:vehicle,p:x,p:y,p:z,p:yaw", "success"),
        Reg("an_vehicledestroy", GenericKind.Exec, typeof(VehicleGraph), "DestroyForGraph", "vehicle:int", "success:bool", "p:vehicle", "success"),
        Reg("an_vehiclewheelcontact", GenericKind.Pure, typeof(VehicleGraph), "WheelContactForGraph", "vehicle:int,index:int", "contact:bool", "p:vehicle,p:index", "contact"),
        // Version 2 (wave 10): fourteen rows appended; the six above and their pins do not change, so saved graphs keep loading.
        Reg("an_vehiclespawn", GenericKind.Exec, typeof(VehicleGraph), "SpawnForGraph", "x:float,y:float,z:float,yaw:float", "vehicle:int,entity:int,success:bool", "p:x,p:y,p:z,p:yaw,@asset,>entity,>success", "vehicle"),
        Reg("an_vehicledrive", GenericKind.Exec, typeof(VehicleGraph), "DriveForGraph", "entity:int,throttle:float,steer:float,brake:float,handbrake:float", "success:bool", "p:entity,p:throttle,p:steer,p:brake,p:handbrake", "success"),
        Reg("an_vehicleshift", GenericKind.Exec, typeof(VehicleGraph), "ShiftForGraph", "entity:int,delta:int", "success:bool", "p:entity,p:delta", "success"),
        Reg("an_vehiclesetgear", GenericKind.Exec, typeof(VehicleGraph), "SetGearForGraph", "entity:int,gear:int", "success:bool", "p:entity,p:gear", "success"),
        Reg("an_vehiclegearmode", GenericKind.Exec, typeof(VehicleGraph), "GearModeForGraph", "entity:int,manual:bool", "success:bool", "p:entity,p:manual", "success"),
        Reg("an_vehicleenter", GenericKind.Exec, typeof(VehicleGraph), "EnterForGraph", "entity:int,driver:int", "success:bool", "p:entity,p:driver", "success"),
        Reg("an_vehicleexit", GenericKind.Exec, typeof(VehicleGraph), "ExitForGraph", "entity:int", "driver:int,success:bool", "p:entity,>driver", "success"),
        Reg("an_vehicleflip", GenericKind.Exec, typeof(VehicleGraph), "FlipForGraph", "entity:int,liftCm:float=50", "success:bool", "p:entity,p:liftCm", "success"),
        Reg("an_vehicletune", GenericKind.Exec, typeof(VehicleGraph), "TuneForGraph", "entity:int,value:float", "success:bool", "p:entity,@key,p:value", "success"),
        Reg("an_vehiclerepair", GenericKind.Exec, typeof(VehicleGraph), "RepairForGraph", "entity:int,amount:float=1", "success:bool", "p:entity,p:amount", "success"),
        Reg("an_vehicletelemetry", GenericKind.Pure, typeof(VehicleGraph), "TelemetryForGraph", "entity:int", "found:bool,speedCmS:float,rpm:float,gear:int,throttle:float,steer:float,brake:float,wheelsOnGround:int,health:float,abs:bool,tcs:bool", "p:entity,>speedCmS,>rpm,>gear,>throttle,>steer,>brake,>wheelsOnGround,>health,>abs,>tcs", "found"),
        Reg("an_vehiclewheelstate", GenericKind.Pure, typeof(VehicleGraph), "WheelStateForGraph", "entity:int,index:int", "found:bool,contact:bool,slipLong:float,slipLatDeg:float,loadN:float,compressionCm:float,angVelRadS:float,surfaceFriction:float,skidding:bool", "p:entity,p:index,>contact,>slipLong,>slipLatDeg,>loadN,>compressionCm,>angVelRadS,>surfaceFriction,>skidding", "found"),
        Reg("an_vehicledriver", GenericKind.Pure, typeof(VehicleGraph), "DriverForGraph", "entity:int", "driver:int", "p:entity", "driver"),
        Reg("an_vehiclefromentity", GenericKind.Pure, typeof(VehicleGraph), "FromEntityForGraph", "entity:int", "vehicle:int,found:bool", "p:entity,>vehicle", "found"),
    };
}
