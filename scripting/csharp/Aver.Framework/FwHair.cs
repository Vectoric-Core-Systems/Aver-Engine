// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_hair_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The strand hair C ABI (framework_hair_abi.h). Declared here, not in a bridge assembly, for the same reason as
/// <see cref="FwSoftShape"/>: only this assembly has the DllImport resolver. Every call goes to the hair system's provider and applies at once,
/// in arrival order; with no provider each returns 0. Names and parameter names are copied from the header character for character (tests/abi
/// checks them).</summary>
internal static class FwHair
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_hair_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_hair_teleport(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_hair_add_collider(int entity, int shape, float sx, float sy, float sz, float ox, float oy, float oz,
                                                                         [MarshalAs(UnmanagedType.LPUTF8Str)] string bone, out int outCount);
    [DllImport(Lib)] internal static extern int aver_fw_hair_clear_colliders(int entity);
    [DllImport(Lib)] internal static extern int aver_fw_hair_state(int entity, int[] outI6);
}
