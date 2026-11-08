// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The ragdoll P/Invokes (physics_ragdoll_abi.h). A second part of Native, as Native.cs is, so the
// parity test in tests/abi checks these names against the header the same way; every name below is
// copied from the header character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Ragdolls (physics_ragdoll_abi.h) ---------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_create(int boneCount, int[] parents, float[] worldPos, float[] worldRot, float[] capsules, float[] limitsDeg, float totalMassKg, uint userLayer);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_destroy(int ragdoll);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_count();
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_bone_count(int ragdoll);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_mode(int ragdoll, int mode);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_mode(int ragdoll);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_strength(int ragdoll, float strength);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_target(int ragdoll, float[] worldPos, float[] worldRot);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_pose(int ragdoll, float[] worldPos, float[] worldRot);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_get_pose(int ragdoll, float[] outWorldPos, float[] outWorldRot);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_add_impulse(int ragdoll, int bone, float ix, float iy, float iz, float px, float py, float pz);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_part_strength(int ragdoll, int bone, float strength);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_part_damping(int ragdoll, int bone, float damping);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_part_torque_scale(int ragdoll, int bone, float scale);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_set_part_pinned(int ragdoll, int bone, int pinned);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_part_pinned(int ragdoll, int bone);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_body_of_bone(int ragdoll, int bone);
    [DllImport(Lib)] internal static extern int aver_phys_ragdoll_of_body(int body, out int outBone);
}
