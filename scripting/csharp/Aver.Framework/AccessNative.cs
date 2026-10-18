// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_access_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The accessibility C ABI (framework_access_abi.h): options, captions, speech and camera feel.
/// Declared here, not in <see cref="Fw"/>, because the header is separate and versioned on its own
/// (framework_loc_abi.h is the precedent). All state lives in Aver.Framework.dll.</summary>
internal static class FwAccess
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_access_abi_version();

    // ---- options ----
    [DllImport(Lib)] internal static extern int aver_fw_access_load_ini([MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Lib)] internal static extern int aver_fw_access_key_count();
    [DllImport(Lib)] internal static extern IntPtr aver_fw_access_key_at(int index);
    [DllImport(Lib)] internal static extern int aver_fw_access_key_info([MarshalAs(UnmanagedType.LPUTF8Str)] string key, float[] out6);
    [DllImport(Lib)] internal static extern IntPtr aver_fw_access_key_choices([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern float aver_fw_access_get([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_access_set([MarshalAs(UnmanagedType.LPUTF8Str)] string key, float value);
    [DllImport(Lib)] internal static extern void aver_fw_access_reset_defaults();
    [DllImport(Lib)] internal static extern int aver_fw_access_revision();
    [DllImport(Lib)] internal static extern int aver_fw_access_take_dirty(int index, byte[] keyOut, int cap, out float valueOut);
    [DllImport(Lib)] internal static extern void aver_fw_access_set_toggle_allowed([MarshalAs(UnmanagedType.LPUTF8Str)] string action, int allowed);
    [DllImport(Lib)] internal static extern int aver_fw_access_toggle_allowed([MarshalAs(UnmanagedType.LPUTF8Str)] string action);
    [DllImport(Lib)] internal static extern int aver_fw_access_effective(float[] out8);

    // ---- captions ----
    [DllImport(Lib)] internal static extern int aver_fw_caption_load_dir([MarshalAs(UnmanagedType.LPUTF8Str)] string dir);
    [DllImport(Lib)] internal static extern int aver_fw_caption_reload_path([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib)] internal static extern void aver_fw_caption_clear();
    [DllImport(Lib)] internal static extern int aver_fw_caption_cue_count();
    [DllImport(Lib)] internal static extern int aver_fw_caption_show([MarshalAs(UnmanagedType.LPUTF8Str)] string cue, int positional, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_fw_caption_show_text([MarshalAs(UnmanagedType.LPUTF8Str)] string speakerRef,
                                                                          [MarshalAs(UnmanagedType.LPUTF8Str)] string textRef,
                                                                          float seconds, uint rgba, int kind);
    [DllImport(Lib)] internal static extern void aver_fw_caption_stop([MarshalAs(UnmanagedType.LPUTF8Str)] string? cue);

    // ---- speech ----
    [DllImport(Lib)] internal static extern int aver_fw_speech_available();
    [DllImport(Lib)] internal static extern int aver_fw_speech_say([MarshalAs(UnmanagedType.LPUTF8Str)] string text, int interrupt);
    [DllImport(Lib)] internal static extern void aver_fw_speech_stop();
    [DllImport(Lib)] internal static extern int aver_fw_speech_speaking();
    [DllImport(Lib)] internal static extern void aver_fw_speech_tick();
    [DllImport(Lib)] internal static extern int aver_fw_speech_os_screen_reader();

    // ---- camera feel ----
    [DllImport(Lib)] internal static extern void aver_fw_camera_shake_add(float trauma);
    [DllImport(Lib)] internal static extern void aver_fw_camera_bob_set(float amount01, float speedCmPerSec);

    /// <summary>Decodes a native UTF-8 pointer; empty when null.</summary>
    internal static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? string.Empty;
}
