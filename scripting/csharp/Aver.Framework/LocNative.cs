// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_loc_abi.h.

using System.Runtime.InteropServices;
using System.Text;

namespace Aver.Framework;

/// <summary>The localization C ABI (framework_loc_abi.h). Declared here, not in <see cref="Fw"/>, because the
/// header is separate and versioned on its own (framework_timers_abi.h is the precedent).</summary>
internal static class FwLoc
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_loc_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_loc_culture_count();
    [DllImport(Lib)] internal static extern IntPtr aver_fw_loc_culture_at(int index);
    [DllImport(Lib)] internal static extern IntPtr aver_fw_loc_default_culture();
    [DllImport(Lib)] internal static extern IntPtr aver_fw_loc_culture();
    [DllImport(Lib)] internal static extern int aver_fw_loc_set_culture([MarshalAs(UnmanagedType.LPUTF8Str)] string tag);
    [DllImport(Lib)] internal static extern int aver_fw_loc_epoch();
    [DllImport(Lib)] internal static extern int aver_fw_loc_resolve([MarshalAs(UnmanagedType.LPUTF8Str)] string reference, byte[]? output, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_loc_get([MarshalAs(UnmanagedType.LPUTF8Str)] string table, [MarshalAs(UnmanagedType.LPUTF8Str)] string key, byte[]? output, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_loc_tr([MarshalAs(UnmanagedType.LPUTF8Str)] string message, [MarshalAs(UnmanagedType.LPUTF8Str)] string context, byte[]? output, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_loc_tr_n([MarshalAs(UnmanagedType.LPUTF8Str)] string message, [MarshalAs(UnmanagedType.LPUTF8Str)] string plural, int n, [MarshalAs(UnmanagedType.LPUTF8Str)] string context, byte[]? output, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_loc_has([MarshalAs(UnmanagedType.LPUTF8Str)] string table, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_loc_missing_count();
    [DllImport(Lib)] internal static extern IntPtr aver_fw_loc_missing_at(int index);
    [DllImport(Lib)] internal static extern void aver_fw_loc_missing_clear();

    /// <summary>Decodes a native UTF-8 pointer; empty when null.</summary>
    internal static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? string.Empty;

    /// <summary>Length-then-copy over a native call that fills a byte buffer. Null when the call returns -1.</summary>
    internal static string? Fetch(Func<byte[]?, int, int> call)
    {
        int n = call(null, 0);
        if (n < 0) return null;
        if (n == 0) return string.Empty;
        var buf = new byte[n + 1];
        int m = call(buf, buf.Length);
        if (m < 0) return null;
        return Encoding.UTF8.GetString(buf, 0, Math.Min(m, n));
    }
}
