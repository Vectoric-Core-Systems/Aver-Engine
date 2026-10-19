// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The v2 vehicle P/Invokes (physics_vehicle_ext_abi.h). A further part of Native, as Native.Ragdoll.cs is, so
// the parity test in tests/abi checks every name and parameter name below against the header; they are copied
// from it character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Vehicles v2 (physics_vehicle_ext_abi.h) ----------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_vehicle_abi_version();
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_kind(int v, int kind);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_chassis_boxes(int v, float[] boxes6, int count);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_engine_ex(int v, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_torque_curve(int v, float[] rpmFraction, float[] torqueFraction, int n);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_gearbox(int v, int manual, float[] fwd, int nFwd, float[] rev, int nRev, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_axle_count(int v);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_wheel_axle(int v, int wheel);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_axle(int v, int axle, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_center_diff(int v, int mode, float slipRatio);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_wheel_ex(int v, int wheel, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_wheel_curves(int v, int wheel, float[] longSlip, float[] longMu, int nLong, float[] latDeg, float[] latMu, int nLat);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_track(int v, int side, int[] wheels, int n, int drivenWheel, float[]? p, int np);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_assists(int v, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_aero(int v, float dragNs2Pm2, float downforceNs2Pm2);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_collision_period(int v, int everyNSteps);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_tune(int v, int key, float value);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_gearbox_mode(int v, int manual);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_gear(int v, int gear);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_shift(int v, int delta);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_flip(int v, float liftCm);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_state(int v, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_wheel_state(int v, int wheel, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_event_count();
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_event_get(int index, int[] outI4, float[] outF8);
}
