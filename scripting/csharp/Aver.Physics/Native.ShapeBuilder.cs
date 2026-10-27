// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_shape_builder_abi.h. A further part of Native, as Native.Queries.cs is, so the parity test
// in tests/abi checks every name and parameter name below against the header; they are copied from it character
// for character. CollisionShape.cs and BodyShape.cs are the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Shape builders (physics_shape_builder_abi.h) -----------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_shape_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_shape_box(float hx, float hy, float hz, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_sphere(float radius, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_capsule(float radius, float height, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_cylinder(float radius, float height, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_tapered_capsule(float topRadius, float bottomRadius, float height, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_tapered_cylinder(float topRadius, float bottomRadius, float height, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_triangle(float[] abcXyz, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_plane(float[] normalXyz, float offsetCm, float halfExtentCm, int surface);
    [DllImport(Lib)] internal static extern int aver_phys_shape_empty(float[] comXyz);
    [DllImport(Lib)] internal static extern int aver_phys_shape_separation_ray(float lengthCm, float[] dirXyz, float radiusCm, int surface);
    [DllImport(Lib)] internal static extern int aver_phys_shape_convex_hull(float[] pointsXyz, int count, int surface, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_shape_mesh(float[] verticesXyz, int vertexCount, int[] indices, int indexCount, byte[]? triPaletteIndex, int[]? palette, int paletteCount, int[]? triUserData, float[]? p, int n);

    [DllImport(Lib)] internal static extern int aver_phys_shape_rotated_translated(int inner, float[]? posCm, float[]? quatXyzw);
    [DllImport(Lib)] internal static extern int aver_phys_shape_offset_com(int inner, float[] offsetCm);
    [DllImport(Lib)] internal static extern int aver_phys_shape_scaled(int inner, float[] scaleXyz);
    [DllImport(Lib)] internal static extern int aver_phys_shape_compound(int kind, int[] children, float[]? posCm, float[]? quatXyzw, int[]? childUserData, int count);

    [DllImport(Lib)] internal static extern int aver_phys_shape_release(int shape);
    [DllImport(Lib)] internal static extern int aver_phys_shape_count();
    [DllImport(Lib)] internal static extern int aver_phys_shape_info(int shape, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_shape_of_body(int body);

    [DllImport(Lib)] internal static extern int aver_phys_body_from_shape(int shape, float[]? originCm, float[]? quatXyzw, int motionType, float massKg, float[]? p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_shape(int body, int shape, int keepMass);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_scale(int body, float[] scaleXyz, int keepMass);
    [DllImport(Lib)] internal static extern int aver_phys_body_scale(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_origin(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_enhanced_edges(int body, int on);
    [DllImport(Lib)] internal static extern int aver_phys_body_compound_add(int body, int child, float[]? posCm, float[]? quatXyzw, int userData);
    [DllImport(Lib)] internal static extern int aver_phys_body_compound_remove(int body, int index);
    [DllImport(Lib)] internal static extern int aver_phys_body_compound_set_child(int body, int index, float[]? posCm, float[]? quatXyzw);
    [DllImport(Lib)] internal static extern int aver_phys_body_compound_count(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_shape_disabled(int body, int index, int disabled);
    [DllImport(Lib)] internal static extern int aver_phys_body_shape_disabled(int body, int index, out int outDisabled);
    [DllImport(Lib)] internal static extern int aver_phys_body_triangles(int body, float[]? regionMin, float[]? regionMax, float[] outXyz, int[]? outSurface, int maxTriangles);

    [DllImport(Lib)] internal static extern int aver_phys_vehicle_set_chassis_shape(int v, int shape);
}
