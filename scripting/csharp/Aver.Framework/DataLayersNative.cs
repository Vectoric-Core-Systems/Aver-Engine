// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_datalayer_abi.h (data layer state).

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The data layers C ABI (framework_datalayer_abi.h). Only the queries are bound: the provider is native (the layers and the
/// streamer live in the game runtime), so no managed callback is ever installed.</summary>
internal static class FwDataLayer
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_datalayer_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_datalayer_count();

    [DllImport(Lib)] internal static extern int aver_fw_datalayer_name(int index, ref byte buf, int cap);

    [DllImport(Lib)] internal static extern int aver_fw_datalayer_get_state([MarshalAs(UnmanagedType.LPUTF8Str)] string name, out int outState);

    [DllImport(Lib)] internal static extern int aver_fw_datalayer_set_state([MarshalAs(UnmanagedType.LPUTF8Str)] string name, int state);

    [DllImport(Lib)] internal static extern int aver_fw_datalayer_poll_completed(ref byte nameBuf, int nameCap, out int outState);
}
