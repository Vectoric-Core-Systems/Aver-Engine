// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The strand hair P/Invokes (physics_hair_abi.h). A further part of Native, as Native.SoftBodyExt.cs is, so the parity test in
// tests/abi checks every name and parameter name below against the header; they are copied from it character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

/// <summary>The function table a host installs so Jolt's hair kernels run on its device (AverPhysComputeHostV1). The C++ composition root fills
/// and installs it; managed code only reads the host kind, so the fields are opaque pointers.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct AverPhysComputeHostV1
{
    public uint StructSize;
    public uint Version;
    public IntPtr User;
    public IntPtr CreateShader;
    public IntPtr CreateBuffer;
    public IntPtr Map;
    public IntPtr Unmap;
    public IntPtr Release;
    public IntPtr CreateQueue;
    public IntPtr SetShader;
    public IntPtr Bind;
    public IntPtr Dispatch;
    public IntPtr Readback;
    public IntPtr Execute;
    public IntPtr Wait;
}

internal static partial class Native
{
    // ---- Strand hair (physics_hair_abi.h) ---------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_hair_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_hair_set_compute_host(ref AverPhysComputeHostV1 host);
    [DllImport(Lib)] internal static extern int aver_phys_hair_host_kind();

    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_begin();
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_strands(int s, float[] pointsXyz, float[]? invMass, int vertexCount, int[] strandStart, int[]? strandMaterial, int strandCount);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_scalp(int s, float[] vertsXyz, int vertexCount, int[] tri3, int triCount, int[]? joints4, float[]? weights4, int jointCount);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_material(int s, int index, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_params(int s, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_finish(int s, int resampleVertices, out float outRootToScalpCm);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_cancel(int s);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_destroy(int settings);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_set_material(int settings, int index, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_info(int settings, int[]? outI, int nI, float[]? outF, int nF);
    [DllImport(Lib)] internal static extern int aver_phys_hair_settings_render_strands(int settings, int[]? outStartCount2, int maxStrands);

    [DllImport(Lib)] internal static extern int aver_phys_hair_create(int settings, float[] world16, int layer);
    [DllImport(Lib)] internal static extern int aver_phys_hair_destroy(int hair);
    [DllImport(Lib)] internal static extern int aver_phys_hair_set_transform(int hair, float[] world16);
    [DllImport(Lib)] internal static extern int aver_phys_hair_teleport(int hair);
    [DllImport(Lib)] internal static extern int aver_phys_hair_set_palette(int hair, float[] joints16, int jointCount);
    [DllImport(Lib)] internal static extern int aver_phys_hair_set_wind(int hair, float[] windCmS3, float dragPerS, float turbulenceCmS);
    [DllImport(Lib)] internal static extern int aver_phys_hair_update(int hair, float dt);
    [DllImport(Lib)] internal static extern int aver_phys_hair_flush();
    [DllImport(Lib)] internal static extern ulong aver_phys_hair_render_buffer(int hair);
    [DllImport(Lib)] internal static extern int aver_phys_hair_info(int hair, int[]? outI, int nI, float[]? outF, int nF);
    [DllImport(Lib)] internal static extern int aver_phys_hair_read_positions(int hair, float[]? outSimXyz, int maxSim, float[]? outRenderXyz, int maxRender);
}
