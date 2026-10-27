// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_surface_abi.h. A further part of Native, as Native.ShapeBuilder.cs is, so the parity test in
// tests/abi checks every name and parameter name below against the header; they are copied from it character for
// character. Surfaces.cs and SurfaceHits.cs are the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Physical surfaces (physics_surface_abi.h) --------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_surface_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_surface_id([MarshalAs(UnmanagedType.LPUTF8Str)] string? utf8Name);
    [DllImport(Lib)] internal static extern int aver_phys_surface_register([MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Name, float friction, float restitution, int tag);
    [DllImport(Lib)] internal static extern int aver_phys_surface_tune(int surface, float friction, float restitution, int tag);
    [DllImport(Lib)] internal static extern int aver_phys_surface_info(int surface, out float outFriction, out float outRestitution, out int outTag, byte[]? outName, int nameCap);
    [DllImport(Lib)] internal static extern int aver_phys_surface_count();
    [DllImport(Lib)] internal static extern int aver_phys_surface_at(int index);
    [DllImport(Lib)] internal static extern void aver_phys_surface_reset();
    [DllImport(Lib)] internal static extern int aver_phys_body_set_surface(int body, int surface, int copyPhysics);
    [DllImport(Lib)] internal static extern int aver_phys_body_surface(int body);

    [DllImport(Lib)] internal static extern int aver_phys_set_combine_mode(int mode);
    [DllImport(Lib)] internal static extern int aver_phys_combine_mode();
    [DllImport(Lib)] internal static extern int aver_phys_body_set_combine_flags(int body, int flags);
    [DllImport(Lib)] internal static extern int aver_phys_body_combine_flags(int body);

    [DllImport(Lib)] internal static extern int aver_phys_raycast_detail(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm, uint layerMask, int ignoreBody, int flags, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_subshape_info(int body, int subShape, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_contact_detail(int index, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_character_ground_detail(int ch, float[]? outF, int nF, int[]? outI, int nI);
    [DllImport(Lib)] internal static extern int aver_phys_vehicle_wheel_surface(int v, int wheel, float[]? outF, int nF, int[]? outI, int nI);
}
