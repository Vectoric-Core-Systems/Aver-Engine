// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_raycast_vehicle_abi.h: the raycast vehicle (Godot's VehicleBody3D). A further part of Native, so the parity test in
// tests/abi checks every name and parameter name below against the header; they are copied from it character for character.
// RaycastVehicle.cs is the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_rayveh_create(int chassisBody);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_destroy(int veh);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_exists(int veh);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_count();
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_chassis(int veh);

    [DllImport(Lib)] internal static extern int aver_phys_rayveh_add_wheel(int veh, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_wheel_count(int veh);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_set_wheel_param(int veh, int wheel, int key, float value);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_wheel_param(int veh, int wheel, int key, out float outValue);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_set_wheel_mask(int veh, int wheel, uint mask);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_wheel_mask(int veh, int wheel, out uint outMask);

    [DllImport(Lib)] internal static extern int aver_phys_rayveh_set_engine_force(int veh, int wheel, float engineForce);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_set_brake(int veh, int wheel, float brake);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_set_steering(int veh, int wheel, float steeringRad);

    [DllImport(Lib)] internal static extern int aver_phys_rayveh_wheel_state(int veh, int wheel, float[]? outState, int n);
    [DllImport(Lib)] internal static extern int aver_phys_rayveh_wheel_pose(int veh, int wheel, float[]? outPositionCm, float[]? outRotation);
}
