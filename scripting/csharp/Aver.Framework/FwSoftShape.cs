// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_softshape_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The jelly, rope, grab, buoyancy and contact-policy C ABI (framework_softshape_abi.h). Declared here, not in a bridge
/// assembly, for the same reason as <see cref="FwSoftBody"/>: only this assembly has the DllImport resolver. Every call goes to the
/// soft shape system's provider and applies at once, in arrival order; with no provider each returns 0. Names and parameter names are
/// copied from the header character for character (tests/abi checks them).</summary>
internal static class FwSoftShape
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_softshape_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_softshape_set_pressure(int entity, float pressureKPa);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_volume(int entity, float[] outRestNowCm3_2);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_grab(int entity, float x, float y, float z, float reachCm, float targetX, float targetY, float targetZ);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_move_grab(int entity, int grab, float targetX, float targetY, float targetZ);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_release_grab(int entity, int grab);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_set_buoyancy(int entity, float fluidDensityKgM3, float drag);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_submerged(int entity, float[] outFractionVolume2);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_set_contact(int entity, float softInvMassScale, float otherInvMassScale);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_set_pair_mode(int entity, int otherEntity, int mode);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_cut_rope(int entity, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_create_rope(float x1, float y1, float z1, float x2, float y2, float z2, float lengthCm, float radiusCm);
    [DllImport(Lib)] internal static extern int aver_fw_softshape_drain_sensor_events(int[] outI, int max);
}
