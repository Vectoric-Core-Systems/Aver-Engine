// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The wheeled-vehicle rows of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2).
// Every method is in VehicleGraph (Aver.Framework/VehicleGraph.cs), a thin wrapper over Vehicles and Car. A vehicle is its
// integer handle. Palette mirror: sandbox/src/GraphNodeDefsVehicle.hpp.

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
    };
}
