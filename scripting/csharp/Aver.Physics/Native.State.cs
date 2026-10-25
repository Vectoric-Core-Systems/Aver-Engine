// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The state-recording P/Invokes (physics_state_abi.h): snapshots, exact stepping, the state hash. A further part of Native, as
// Native.World.cs is, so the parity test in tests/abi checks every name and parameter name below against the header; they are
// copied from it character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- State recording (physics_state_abi.h) -------------------------------------------------------------

    [DllImport(Lib)] internal static extern int   aver_phys_state_abi_version();

    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_capture(int reuse, uint flags);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_restore(int snapshot, uint options);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_last_result();
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_release(int snapshot);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_release_all();
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_count();
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_bytes(int snapshot);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_read(int snapshot, byte[]? outData, int cap);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_import(byte[] data, int size);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_info(int snapshot, int[] outValues, int n);
    [DllImport(Lib)] internal static extern ulong aver_phys_snapshot_hash(int snapshot);
    [DllImport(Lib)] internal static extern int   aver_phys_snapshot_diff(int a, int b);

    [DllImport(Lib)] internal static extern ulong aver_phys_state_hash(uint flags);
    [DllImport(Lib)] internal static extern int   aver_phys_hash_log([MarshalAs(UnmanagedType.LPUTF8Str)] string? pathOrNull);

    [DllImport(Lib)] internal static extern int   aver_phys_step_exact(int steps);
    [DllImport(Lib)] internal static extern long  aver_phys_step_total();
    [DllImport(Lib)] internal static extern int   aver_phys_accumulator_set(float seconds);
    [DllImport(Lib)] internal static extern float aver_phys_accumulator_get();

    // ---- Shape binary cache (physics_state_abi.h) ------------------------------------------------------------

    [DllImport(Lib)] internal static extern uint  aver_phys_shape_cache_version();
    [DllImport(Lib)] internal static extern int   aver_phys_shape_cache_enable(int on);
    [DllImport(Lib)] internal static extern int   aver_phys_shape_cache_enabled();
    [DllImport(Lib)] internal static extern int   aver_phys_create_heightfield_shape(float[] samples, int sampleCount, float spacingCm);
    [DllImport(Lib)] internal static extern int   aver_phys_shape_save(int shape, ulong key, byte[]? outData, int cap);
    [DllImport(Lib)] internal static extern int   aver_phys_shape_restore(byte[] data, int size, ulong key);
}
