// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_origin_abi.h (the floating origin: docs/plans/wave11/LARGE_WORLD.md 2.5).

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The floating-origin C ABI. Names and parameter names are copied from the header character for character (tests/abi checks
/// them). <c>aver_fw_set_origin_provider</c> is the host's call and is not imported.</summary>
internal static class FwOrigin
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int  aver_fw_origin_abi_version();

    [DllImport(Lib)] internal static extern int  aver_fw_origin_get(long[] outXyCm);
    [DllImport(Lib)] internal static extern uint aver_fw_origin_generation();
    [DllImport(Lib)] internal static extern int  aver_fw_origin_enabled();

    [DllImport(Lib)] internal static extern void aver_fw_origin_to_local(double ax, double ay, double az, float[] outXyz);
    [DllImport(Lib)] internal static extern void aver_fw_origin_to_abs(float x, float y, float z, double[] outXyz);

    [DllImport(Lib)] internal static extern void aver_fw_worldpos_from_abs(double ax, double ay, double az, int[] outChunk3, float[] outLocal3);
    [DllImport(Lib)] internal static extern void aver_fw_worldpos_to_abs(int[] chunk3, float[] local3, double[] outXyz);

    [DllImport(Lib)] internal static extern int  aver_fw_origin_request_shift(int dxRegions, int dyRegions);
}
