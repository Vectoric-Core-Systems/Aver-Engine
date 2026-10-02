// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// P/Invoke declarations for the Aver.UI.Abi C ABI.
using System.Runtime.InteropServices;

namespace Aver.UI;

/// <summary>The P/Invoke surface for the <c>Aver.UI.Abi</c> C ABI. See <c>modules/ui.abi/include/aver/ui/ui_abi.h</c>.</summary>
/// <remarks>The native library and this assembly are named apart, so no <c>NativeResolver</c> is needed.</remarks>
internal static class Native
{
    private const string Lib = "Aver.UI.Abi";

    // The frame. Called by the HOST, never by a game.
    [DllImport(Lib)] internal static extern void aver_ui_begin_frame(float x, float y, float w, float h);
    [DllImport(Lib)] internal static extern void aver_ui_viewport(float[] outXYWH);

    [DllImport(Lib)] internal static extern void aver_ui_set_layer(int layer);
    [DllImport(Lib)] internal static extern int  aver_ui_layer();

    [DllImport(Lib)] internal static extern void aver_ui_push_clip(int left, int top, int right, int bottom);
    [DllImport(Lib)] internal static extern void aver_ui_pop_clip();

    [DllImport(Lib)] internal static extern void aver_ui_rect(float x, float y, float w, float h, uint rgba);
    [DllImport(Lib)] internal static extern void aver_ui_textured_rect(float x, float y, float w, float h,
                                                                       ulong texture,
                                                                       float u0, float v0, float u1, float v1,
                                                                       uint rgba);

    // Text. The font is lent to the ABI by the HOST (aver_ui_set_font, not exposed here — a game
    // does not own the atlas and must not be able to swap it mid-frame).
    [DllImport(Lib)] internal static extern int   aver_ui_has_font();
    [DllImport(Lib)] internal static extern void  aver_ui_font_metrics(float[] outAscentDescentLine);
    [DllImport(Lib)] internal static extern float aver_ui_text(float x, float y,
                                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8,
                                                               uint rgba);
    [DllImport(Lib)] internal static extern float aver_ui_text_width(
                                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8);

    // Hit testing.
    [DllImport(Lib)] internal static extern void  aver_ui_hit_rect(ulong id, float x, float y, float w, float h);
    [DllImport(Lib)] internal static extern ulong aver_ui_hit_test(float x, float y);

    // The pointer. aver_ui_set_pointer is the HOST's; a game only reads.
    [DllImport(Lib)] internal static extern void aver_ui_pointer(float[] outXY);
    [DllImport(Lib)] internal static extern int  aver_ui_pointer_down(int button);

    [DllImport(Lib)] internal static extern int aver_ui_vertex_count();
    [DllImport(Lib)] internal static extern int aver_ui_command_count();
}
