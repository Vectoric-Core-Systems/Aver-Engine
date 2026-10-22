// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Heightfields v2 (physics_terrain_abi.h): a heightfield that can be non-square, have holes, carry a physical surface per
// cell, and be edited while the body runs. Samples are row-major in the v1 physics order of Physics.AddHeightfield:
// sample (column x, row y) lands at (origin.X - y * spacing, origin.Y + x * spacing, origin.Z + height).

using System;

namespace Aver.Physics;

/// <summary>What a heightfield body holds (aver_phys_heightfield_info). <see cref="Valid"/> is false when the body's shape
/// is not a heightfield made by this module. Heights are in cm; <see cref="MinHeightCm"/> and <see cref="MaxHeightCm"/> are
/// the range an edit can encode and <see cref="ClampedTotal"/> counts the samples edits had to clamp into it.</summary>
public readonly record struct TerrainInfo(bool Valid, float SpacingCm, float MinHeightCm, float MaxHeightCm, long ClampedTotal,
                                          int Cols, int Rows, int Padded, int BlockSize, int Bits, int Materials);

public static class Terrain
{
    private const int MaxSide = 16384;
    private const int MaxPalette = 256;

    /// <summary>A heightfield shape (static bodies only: <c>shape.CreateBody(origin, rotation, MotionType.Static)</c>).
    /// <paramref name="samples"/> is <paramref name="cols"/> * <paramref name="rows"/> heights in cm; a NaN or
    /// <c>float.MaxValue</c> is a hole. The shape is square inside (the larger side, the rest holes), so an 8:1 strip costs
    /// eight times a square of its short side. <paramref name="cellPalette"/> is one byte per cell,
    /// (cols - 1) * (rows - 1), indexing <paramref name="palette"/> (up to 256 surface ids).
    /// <paramref name="editHeadroomCm"/> widens the range later edits can encode, so a crater below the lowest sample is
    /// possible. Returns <see cref="CollisionShape.None"/> for a bad argument.</summary>
    public static CollisionShape Heightfield(float[] samples, int cols, int rows, float spacingCm,
                                             byte[]? cellPalette = null, int[]? palette = null,
                                             int blockSize = 2, int bitsPerSample = 8, float edgeAngleDeg = 5f, float editHeadroomCm = 0f)
    {
        if (samples is null || cols < 2 || rows < 2 || cols > MaxSide || rows > MaxSide) return CollisionShape.None;
        if ((long)cols * rows > samples.Length) return CollisionShape.None;
        if (palette is { Length: > MaxPalette }) return CollisionShape.None;
        if (cellPalette is not null && (palette is null || (long)(cols - 1) * (rows - 1) > cellPalette.Length)) return CollisionShape.None;

        bool custom = blockSize != 2 || bitsPerSample != 8 || edgeAngleDeg != 5f || editHeadroomCm != 0f;
        float[]? p = null;
        if (custom)
        {
            p = new float[(int)HpIndex.Count];
            p[(int)HpIndex.BlockSize] = blockSize;
            p[(int)HpIndex.BitsPerSample] = bitsPerSample;
            p[(int)HpIndex.EdgeAngleDeg] = edgeAngleDeg;
            p[(int)HpIndex.EditHeadroomCm] = editHeadroomCm;
        }
        return new CollisionShape(Native.aver_phys_heightfield_shape(samples, cols, rows, spacingCm, cellPalette, palette,
                                                                     palette?.Length ?? 0, p, p?.Length ?? 0));
    }

    /// <summary>Sets a <paramref name="w"/> x <paramref name="h"/> rectangle of samples at column <paramref name="x0"/>, row
    /// <paramref name="y0"/> (row-major, cm, NaN = hole). Values outside the encoded range are clamped and counted in
    /// <see cref="TerrainInfo.ClampedTotal"/>. <paramref name="wakeBodies"/> wakes the dynamic bodies over the area so a
    /// ball resting on a flattened hill falls. The body's shape must BE the heightfield. The shape is cloned per call, so
    /// batch an edit per frame; call between steps.</summary>
    public static bool SetHeights(Body body, int x0, int y0, int w, int h, float[] heights, bool wakeBodies = true)
    {
        if (heights is null || w < 1 || h < 1 || (long)w * h > heights.Length) return false;
        return Native.aver_phys_heightfield_set_heights(body.Handle, x0, y0, w, h, heights,
                                                        wakeBodies ? (int)HeightfieldFlags.Wake : 0) != 0;
    }

    /// <summary>Reads a rectangle of samples back into <paramref name="output"/> (row-major, cm). The values are the
    /// decompressed ones, so slightly different from what the field was made with; holes read <c>float.MaxValue</c>.</summary>
    public static bool GetHeights(Body body, int x0, int y0, int w, int h, float[] output)
    {
        if (output is null || w < 1 || h < 1 || (long)w * h > output.Length) return false;
        return Native.aver_phys_heightfield_get_heights(body.Handle, x0, y0, w, h, output) != 0;
    }

    /// <summary>Rewrites the surface of a rectangle of cells: one palette index per cell, the palette merged into the field's
    /// own. False when the field would hold more than 256 surfaces in all, or for an index outside the palette.</summary>
    public static bool SetCellSurfaces(Body body, int x0, int y0, int w, int h, byte[] cellPalette, int[] palette)
    {
        if (cellPalette is null || palette is null || palette.Length < 1 || palette.Length > MaxPalette) return false;
        if (w < 1 || h < 1 || (long)w * h > cellPalette.Length) return false;
        return Native.aver_phys_heightfield_set_cell_surfaces(body.Handle, x0, y0, w, h, cellPalette, palette, palette.Length) != 0;
    }

    /// <summary>The field's spacing, encodable height range, clamp counter and sizes; <c>Valid</c> is false for any other body.</summary>
    public static TerrainInfo Info(Body body)
    {
        var f = new float[(int)TerrainFloat.Count];
        var i = new int[(int)TerrainInt.Count];
        if (Native.aver_phys_heightfield_info(body.Handle, f, f.Length, i, i.Length) == 0) return default;
        return new TerrainInfo(true, f[(int)TerrainFloat.SpacingCm], f[(int)TerrainFloat.MinHeightCm], f[(int)TerrainFloat.MaxHeightCm],
                               (long)f[(int)TerrainFloat.ClampedTotal],
                               i[(int)TerrainInt.Cols], i[(int)TerrainInt.Rows], i[(int)TerrainInt.Padded],
                               i[(int)TerrainInt.BlockSize], i[(int)TerrainInt.Bits], i[(int)TerrainInt.Materials]);
    }
}
