// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_pose_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The pose seam C ABI (framework_pose_abi.h). Declared here, not in a bridge assembly, for the same reason as
/// <see cref="Fw"/>: only this assembly has the DllImport resolver. A transform crosses as ten floats.</summary>
internal static class FwPose
{
    private const string Lib = "Aver.Framework";

    internal const int Position = 0x1, Rotation = 0x2, Scale = 0x4, All = 0x7, Model = 0x8, Once = 0x10;
    internal const int ReadModel = 0x1, ReadBeforeSeam = 0x2;

    [DllImport(Lib)] internal static extern int aver_fw_pose_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_pose_bone_count(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_pose_find_bone(int entity, long nameHash);
    [DllImport(Lib)] internal static extern int aver_fw_pose_bone_info(int entity, int bone, byte[] nameBuf, int nameBufLen,
                                                                       out int outParent, float[] outRest10);
    [DllImport(Lib)] internal static extern int aver_fw_pose_read(int entity, int bone, int readFlags, float[] out10);
    [DllImport(Lib)] internal static extern int aver_fw_pose_write(int entity, int bone, int flags, float[] in10, float amount);
    [DllImport(Lib)] internal static extern int aver_fw_pose_write_range(int entity, int first, int count, int flags, float[] inFloats);
    [DllImport(Lib)] internal static extern int aver_fw_pose_clear(int entity, int bone);
    [DllImport(Lib)] internal static extern int aver_fw_pose_set_weight(int entity, float weight);
    [DllImport(Lib)] internal static extern int aver_fw_pose_set_enabled(int entity, int enabled);
    [DllImport(Lib)] internal static extern int aver_fw_pose_override_count(int entity);
}
