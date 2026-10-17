// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_import_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The model import C ABI (framework_import_abi.h). Declared here, not in <see cref="Fw"/>, because the
/// header is separate and versioned on its own (framework_anim_abi.h is the precedent). Strings cross as UTF-8.</summary>
internal static class FwImport
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_import_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_import_available();
    [DllImport(Lib)] internal static extern int aver_fw_import_model(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string source,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string destFolder,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string options,
        int overwrite, byte[] outMessage, int messageCap, int[] outCounts5);
}
