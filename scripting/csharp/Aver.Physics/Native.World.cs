// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The world-configuration P/Invokes (physics_world_abi.h): build facts, the live solver settings, the init-time
// limits, rebuild, stats, and the profiler table. A further part of Native, as Native.VehicleExt.cs is, so the parity test in
// tests/abi checks every name and parameter name below against the header; they are copied from it character for
// character.
//
// NOT BOUND HERE: aver_phys_setting_name / _unit / aver_phys_config_name return a pointer to a static string, and
// a DllImport that returns `string` frees the pointer it is given. The managed names come from the enums beside
// this file instead, and PhysicsStateApiTests reads the native table through its own IntPtr import and compares.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- World configuration (physics_world_abi.h) ---------------------------------------------------------

    [DllImport(Lib)] internal static extern int  aver_phys_world_abi_version();

    [DllImport(Lib)] internal static extern int  aver_phys_precision();
    [DllImport(Lib)] internal static extern uint aver_phys_build_flags();
    [DllImport(Lib)] internal static extern int  aver_phys_build_string(byte[]? outUtf8, int cap);

    [DllImport(Lib)] internal static extern int  aver_phys_setting_count();
    [DllImport(Lib)] internal static extern int  aver_phys_setting_find([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int  aver_phys_setting_range(int key, out float outMin, out float outMax, out float outDefault);
    [DllImport(Lib)] internal static extern int  aver_phys_setting_set(int key, float value);
    [DllImport(Lib)] internal static extern int  aver_phys_setting_get(int key, out float outValue);
    [DllImport(Lib)] internal static extern int  aver_phys_setting_reset(int key);
    [DllImport(Lib)] internal static extern int  aver_phys_setting_get_all(float[] outValues, int n);

    [DllImport(Lib)] internal static extern int  aver_phys_config_range(int key, out float outMin, out float outMax, out float outDefault);
    [DllImport(Lib)] internal static extern int  aver_phys_config_set(int key, float value);
    [DllImport(Lib)] internal static extern int  aver_phys_config_get(int key, out float outValue);
    [DllImport(Lib)] internal static extern int  aver_phys_config_active(int key, out float outValue);
    [DllImport(Lib)] internal static extern int  aver_phys_config_reset(int key);
    [DllImport(Lib)] internal static extern int  aver_phys_config_differs();
    [DllImport(Lib)] internal static extern int  aver_phys_rebuild();
    [DllImport(Lib)] internal static extern int  aver_phys_world_empty();

    [DllImport(Lib)] internal static extern int  aver_phys_stats(double[] outValues, int n);
    [DllImport(Lib)] internal static extern int  aver_phys_stats_reset_window();
    [DllImport(Lib)] internal static extern int  aver_phys_diag_report(int kind);
    [DllImport(Lib)] internal static extern int  aver_phys_diag_log_dir([MarshalAs(UnmanagedType.LPUTF8Str)] string? dirUtf8);

    [DllImport(Lib)] internal static extern int  aver_phys_profile_enable(int on);
    [DllImport(Lib)] internal static extern int  aver_phys_profile_is_enabled();
    [DllImport(Lib)] internal static extern int  aver_phys_profile_count();
    [DllImport(Lib)] internal static extern int  aver_phys_profile_get(int index, byte[]? outNameUtf8, int nameCap, double[]? outValues, int[]? outParentDepth);
    [DllImport(Lib)] internal static extern int  aver_phys_profile_reset();
    [DllImport(Lib)] internal static extern int  aver_phys_profile_summary(byte[]? outUtf8, int cap);
}
