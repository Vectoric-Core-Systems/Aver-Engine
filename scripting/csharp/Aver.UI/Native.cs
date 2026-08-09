// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
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

    [DllImport(Lib)] internal static extern int aver_ui_vertex_count();
    [DllImport(Lib)] internal static extern int aver_ui_command_count();
}
