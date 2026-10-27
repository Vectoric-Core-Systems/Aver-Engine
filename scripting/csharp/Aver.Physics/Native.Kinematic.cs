// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_kinematic_abi.h: the kinematic-body character controller with Godot's move_and_slide semantics. A further part of Native, so
// the parity test in tests/abi checks every name and parameter name below against the header; they are copied from it character for character.
// KinematicCharacter.cs is the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    [DllImport(Lib)] internal static extern int aver_phys_kinematic_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_kc_create(int body);
    [DllImport(Lib)] internal static extern int aver_phys_kc_destroy(int kc);
    [DllImport(Lib)] internal static extern int aver_phys_kc_exists(int kc);
    [DllImport(Lib)] internal static extern int aver_phys_kc_count();
    [DllImport(Lib)] internal static extern int aver_phys_kc_body(int kc);
    [DllImport(Lib)] internal static extern int aver_phys_kc_set_param(int kc, int param, float value);
    [DllImport(Lib)] internal static extern int aver_phys_kc_param(int kc, int param, out float outValue);
    [DllImport(Lib)] internal static extern int aver_phys_kc_set_up(int kc, float[] up3);
    [DllImport(Lib)] internal static extern int aver_phys_kc_up(int kc, float[] outUp3);
    [DllImport(Lib)] internal static extern int aver_phys_kc_set_platform_layers(int kc, uint floorLayers, uint wallLayers);
    [DllImport(Lib)] internal static extern int aver_phys_kc_platform_layers(int kc, out uint outFloorLayers, out uint outWallLayers);
    [DllImport(Lib)] internal static extern int aver_phys_kc_set_velocity(int kc, float[] velocity3);
    [DllImport(Lib)] internal static extern int aver_phys_kc_velocity(int kc, float[] outVelocity3);
    [DllImport(Lib)] internal static extern int aver_phys_kc_move_and_slide(int kc, float dt, out int outCollided);
    [DllImport(Lib)] internal static extern int aver_phys_kc_clear(int kc);
    [DllImport(Lib)] internal static extern int aver_phys_kc_state(int kc, out int outFlags);
    [DllImport(Lib)] internal static extern int aver_phys_kc_info(int kc, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_kc_collision(int kc, int index, float[]? outF, int nF, int[]? outI, int nI);
}
