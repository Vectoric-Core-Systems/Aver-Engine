// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_envfx_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The environment-effects C ABI (framework_envfx_abi.h). Declared here for the same reason as <see cref="Fw"/>: only this assembly has the DllImport
/// resolver. The host-side half (pack, revision, tick) is not declared: scripts do not call it.</summary>
internal static class FwEnvFx
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_envfx_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_fogvol_create([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_fogvol_destroy(int id);
    [DllImport(Lib)] internal static extern int aver_fw_fogvol_find([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_fogvol_set(int id, int shape, float[] world12, float[] halfExtentCm3, float densityPerM, float[]? albedo3,
                                                                   float[]? emission3, float heightFalloff, float edgeFade);
    [DllImport(Lib)] internal static extern int aver_fw_fogvol_set_enabled(int id, int on);
    [DllImport(Lib)] internal static extern int aver_fw_fogvol_count();
    [DllImport(Lib)] internal static extern float aver_fw_fogvol_density_at(float x, float y, float z);

    [DllImport(Lib)] internal static extern int aver_fw_probe_create([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_probe_destroy(int id);
    [DllImport(Lib)] internal static extern int aver_fw_probe_find([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_probe_set_box(int id, float[] world12, float[] halfExtentCm3, float[]? originOffsetCm3);
    [DllImport(Lib)] internal static extern int aver_fw_probe_set_look(int id, float intensity, float blendDistanceCm, float maxDistanceCm, int boxProjection,
                                                                       int interior, int updateMode, int resolution);
    [DllImport(Lib)] internal static extern int aver_fw_probe_set_ambient(int id, int mode, float[]? colour3, float energy);
    [DllImport(Lib)] internal static extern int aver_fw_probe_set_masks(int id, uint cullMask, uint reflectionMask);
    [DllImport(Lib)] internal static extern int aver_fw_probe_set_enabled(int id, int on);
    [DllImport(Lib)] internal static extern int aver_fw_probe_count();
    [DllImport(Lib)] internal static extern int aver_fw_probe_select(float x, float y, float z, float cx, float cy, float cz, int[] outIds, float[] outWeights, out float outRest);
    [DllImport(Lib)] internal static extern int aver_fw_probe_project(int id, float x, float y, float z, float dx, float dy, float dz, float[] out3);
}
