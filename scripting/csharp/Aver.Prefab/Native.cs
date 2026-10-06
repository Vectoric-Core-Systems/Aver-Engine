// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for the Aver.Prefab.Abi C ABI (modules/prefab/include/aver/prefab/prefab_abi.h).
using System.Runtime.InteropServices;

namespace Aver.Prefab;

/// <summary>One extern per aver_prefab_* export, spelled exactly as the header spells it.</summary>
internal static class Native
{
    private const string Lib = "Aver.Prefab.Abi";

    [DllImport(Lib)] internal static extern int aver_prefab_abi_version();

    [DllImport(Lib)] internal static extern int aver_prefab_spawn(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int parent,
        float x, float y, float z, float yaw, float pitch, float roll, float scale);
    [DllImport(Lib)] internal static extern int aver_prefab_destroy(int root);
    [DllImport(Lib)] internal static extern int aver_prefab_root_of(int entity);
    [DllImport(Lib)] internal static extern int aver_prefab_is_instance(int entity);
    [DllImport(Lib)] internal static extern int aver_prefab_find(int root,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string nodePath);
    [DllImport(Lib)] internal static extern int aver_prefab_revert(int root);
    [DllImport(Lib)] internal static extern int aver_prefab_override_count(int root);
}
