// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The debug-renderer P/Invokes (physics_debug_abi.h): Jolt's debug drawing as a flat line feed. A further part of Native, as
// Native.World.cs is, so the parity test in tests/abi checks every name and parameter name below against the header; they are
// copied from it character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Debug renderer (physics_debug_abi.h) --------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_debug_abi_version();
    [DllImport(Lib)] internal static extern int aver_phys_debug_available();
    [DllImport(Lib)] internal static extern int aver_phys_debug_set_color_scheme(int scheme);
    [DllImport(Lib)] internal static extern int aver_phys_debug_collect(uint flags, float[]? cameraXyz, float radiusCm, int maxLines);
    [DllImport(Lib)] internal static extern int aver_phys_debug_truncated();
    [DllImport(Lib)] internal static extern int aver_phys_debug_lines(float[] outLines, int maxLines);
    [DllImport(Lib)] internal static extern int aver_phys_debug_label_count();
    [DllImport(Lib)] internal static extern int aver_phys_debug_label_get(int index, float[]? outXyz, float[]? outRgb, byte[]? outTextUtf8, int textCap);
    [DllImport(Lib)] internal static extern int aver_phys_debug_arm(uint flags);
}
