// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_terrain_abi.h. A further part of Native, as Native.Surface.cs is, so the parity test in tests/abi
// checks every name and parameter name below against the header; they are copied from it character for character.
// Terrain.cs is the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Heightfields v2 (physics_terrain_abi.h) ----------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_terrain_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_heightfield_shape(float[] samples, int cols, int rows, float spacingCm, byte[]? cellPaletteIndex, int[]? palette, int paletteCount, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_heightfield_set_heights(int body, int x0, int y0, int w, int h, float[] heights, int flags);
    [DllImport(Lib)] internal static extern int aver_phys_heightfield_get_heights(int body, int x0, int y0, int w, int h, float[] outHeights);
    [DllImport(Lib)] internal static extern int aver_phys_heightfield_set_cell_surfaces(int body, int x0, int y0, int w, int h, byte[]? cellPaletteIndex, int[] palette, int paletteCount);
    [DllImport(Lib)] internal static extern int aver_phys_heightfield_info(int body, float[]? outF, int nF, int[]? outI, int nI);
}
