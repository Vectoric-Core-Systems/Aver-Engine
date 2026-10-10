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

    /// <summary>Why the last prefab call on THIS thread failed. See <see cref="PrefabError"/>.</summary>
    [DllImport(Lib)] internal static extern int aver_prefab_last_error();
}

/// <summary>Why a prefab call failed, mirroring aver::AbiError in
/// modules/core/include/aver/core/ErrorCodes.hpp. Every value except <see cref="Ok"/> is negative,
/// so <c>(int)code &lt; 0</c> means "failed" even for a code added after this assembly was built.
/// </summary>
public enum PrefabError
{
    /// <summary>No error was recorded by the last call that records one.</summary>
    Ok = 0,
    /// <summary>The root is not a live instance (the host refused it).</summary>
    BadHandle = -1,
    /// <summary>A required out-parameter was null.</summary>
    NullPointer = -2,
    /// <summary>No host is installed: the runtime never installed a PrefabSystem.</summary>
    NotInitialised = -3,
    /// <summary>An index past the end of what exists.</summary>
    OutOfRange = -4,
    /// <summary>The installed host does not provide this function.</summary>
    Unsupported = -5,
    /// <summary>A null or empty spawn path, or a spawn the host refused.</summary>
    InvalidArgument = -6,
    /// <summary>The request was legal and the memory was not there.</summary>
    AllocationFailed = -7,
}

/// <summary>Reads the reason the last prefab call failed. The calls keep their 0 / non-zero
/// returns; the reason travels on its own thread-local entry point.</summary>
public static class PrefabAbi
{
    /// <summary>Why the last prefab call on this thread failed, or <see cref="PrefabError.Ok"/>.</summary>
    public static PrefabError LastError => (PrefabError)Native.aver_prefab_last_error();

    /// <summary>True when the last prefab call on this thread recorded a failure.</summary>
    public static bool LastCallFailed => Native.aver_prefab_last_error() < 0;
}
