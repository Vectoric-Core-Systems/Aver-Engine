// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_areas_abi.h: per-body contact monitoring and Godot-style Areas. A further part of Native, so the parity test in
// tests/abi checks every name and parameter name below against the header; they are copied from it character for character.
// Areas.cs is the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    [DllImport(Lib)] internal static extern int aver_phys_areas_abi_version();

    // ---- contact monitor ----
    [DllImport(Lib)] internal static extern int aver_phys_body_set_contact_monitor(int body, int on, int maxContacts);
    [DllImport(Lib)] internal static extern int aver_phys_body_contact_monitor(int body, out int outMaxContacts);
    [DllImport(Lib)] internal static extern int aver_phys_body_contact_count(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_contact_get(int body, int index, out int outOther, float[]? outPoint, float[]? outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_contact_event_count();
    [DllImport(Lib)] internal static extern int aver_phys_contact_event_get(int index, out int outBody, out int outOther, out int outBegan, float[]? outPoint, float[]? outNormal);

    // ---- areas ----
    [DllImport(Lib)] internal static extern int aver_phys_area_create(int body);
    [DllImport(Lib)] internal static extern int aver_phys_area_destroy(int area);
    [DllImport(Lib)] internal static extern int aver_phys_area_exists(int area);
    [DllImport(Lib)] internal static extern int aver_phys_area_count();
    [DllImport(Lib)] internal static extern int aver_phys_area_set_flags(int area, int monitoring, int monitorable);
    [DllImport(Lib)] internal static extern int aver_phys_area_flags(int area, out int outMonitoring, out int outMonitorable);
    [DllImport(Lib)] internal static extern int aver_phys_area_set_collision(int area, uint layerBits, uint maskBits);
    [DllImport(Lib)] internal static extern int aver_phys_area_collision(int area, out uint outLayerBits, out uint outMaskBits);
    [DllImport(Lib)] internal static extern int aver_phys_area_set_priority(int area, int priority);
    [DllImport(Lib)] internal static extern int aver_phys_area_set_gravity(int area, int mode, float magnitudeCmS2, float dx, float dy, float dz);
    [DllImport(Lib)] internal static extern int aver_phys_area_set_gravity_point(int area, int on, float cx, float cy, float cz, float unitDistanceCm);
    [DllImport(Lib)] internal static extern int aver_phys_area_set_damping(int area, int linearMode, float linear, int angularMode, float angular);
    [DllImport(Lib)] internal static extern int aver_phys_area_set_wind(int area, float forceMagnitude, float dx, float dy, float dz, float attenuation, float unitDistanceCm);
    [DllImport(Lib)] internal static extern int aver_phys_area_overlap_count(int area);
    [DllImport(Lib)] internal static extern int aver_phys_area_overlap_get(int area, int index, out int outOther, out int outIsArea);
    [DllImport(Lib)] internal static extern int aver_phys_area_event_count();
    [DllImport(Lib)] internal static extern int aver_phys_area_event_get(int index, out int outArea, out int outOther, out int outEntered, out int outIsArea);
    [DllImport(Lib)] internal static extern int aver_phys_body_area_gravity(int body, float[] outGravityCmS2);
    [DllImport(Lib)] internal static extern int aver_phys_body_area_damping(int body, out float outLinear, out float outAngular);
}
