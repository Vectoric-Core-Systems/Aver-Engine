// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
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

    // ---- the retained widget system (ui_widget_abi.h) -----------------------------------------------
    // The frame and the input feeds are the HOST's. A game reads state and handles events.
    [DllImport(Lib)] internal static extern void aver_ui_widgets_frame(float dt);
    [DllImport(Lib)] internal static extern uint aver_ui_wants_input();
    [DllImport(Lib)] internal static extern void aver_ui_set_theme([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern void aver_ui_set_dpi(int mode, float refWidth, float refHeight, float userScale);
    [DllImport(Lib)] internal static extern float aver_ui_scale();

    [DllImport(Lib)] internal static extern uint aver_ui_widget_create(int kind, uint parent,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_destroy(uint id);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_reparent(uint id, uint newParent, int index);
    [DllImport(Lib)] internal static extern uint aver_ui_widget_find([MarshalAs(UnmanagedType.LPUTF8Str)] string name, uint under);
    [DllImport(Lib)] internal static extern uint aver_ui_widget_parent(uint id);
    [DllImport(Lib)] internal static extern uint aver_ui_widget_root(uint id);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_child_count(uint id);
    [DllImport(Lib)] internal static extern uint aver_ui_widget_child_at(uint id, int index);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_kind(uint id);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_exists(uint id);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_set_prop(uint id, [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Lib)] internal static extern IntPtr aver_ui_widget_get_prop(uint id, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_text(uint id, [MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Lib)] internal static extern IntPtr aver_ui_widget_get_text(uint id);
    [DllImport(Lib)] internal static extern void  aver_ui_widget_set_value(uint id, float value);
    [DllImport(Lib)] internal static extern float aver_ui_widget_get_value(uint id);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_checked(uint id, int value);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_get_checked(uint id);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_selected(uint id, int index);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_get_selected(uint id);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_items(uint id, [MarshalAs(UnmanagedType.LPUTF8Str)] string items);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_visible(uint id, int visible);
    [DllImport(Lib)] internal static extern int  aver_ui_widget_get_visible(uint id);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_enabled(uint id, int enabled);
    [DllImport(Lib)] internal static extern void aver_ui_widget_set_texture(uint id, ulong texture);
    [DllImport(Lib)] internal static extern void aver_ui_widget_rect(uint id, float[] outXYWH);
    [DllImport(Lib)] internal static extern int  aver_ui_set_focus(uint id);
    [DllImport(Lib)] internal static extern uint aver_ui_focused();
    [DllImport(Lib)] internal static extern uint aver_ui_hit_widget(float x, float y);

    [DllImport(Lib)] internal static extern uint aver_ui_layout_open([MarshalAs(UnmanagedType.LPUTF8Str)] string text, int applyEnvironment);
    [DllImport(Lib)] internal static extern IntPtr aver_ui_layout_error();
    [DllImport(Lib)] internal static extern IntPtr aver_ui_layout_capture(uint root);

    [DllImport(Lib)] internal static extern int aver_ui_event_poll(out int type, out uint widget, out float value,
                                                  out int index, byte[] textBuf, int textCap);

    [DllImport(Lib)] internal static extern uint aver_ui_menu_create([MarshalAs(UnmanagedType.LPUTF8Str)] string rootName,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string title,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string entries,
                                                  int modal, [MarshalAs(UnmanagedType.LPUTF8Str)] string cancelCommand);
    [DllImport(Lib)] internal static extern void aver_ui_settings_reset_default();
    [DllImport(Lib)] internal static extern void aver_ui_settings_clear();
    [DllImport(Lib)] internal static extern void aver_ui_settings_add(int type, [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string label,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string tab,
                                                  float value, float min, float max, float step,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string choices);
    [DllImport(Lib)] internal static extern void  aver_ui_settings_set_value([MarshalAs(UnmanagedType.LPUTF8Str)] string key, float value);
    [DllImport(Lib)] internal static extern float aver_ui_settings_value([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern void aver_ui_rebind_clear();
    [DllImport(Lib)] internal static extern void aver_ui_rebind_add([MarshalAs(UnmanagedType.LPUTF8Str)] string action, int slot,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string label,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string binding, int rebindable);
    [DllImport(Lib)] internal static extern void aver_ui_rebind_set_binding(int row, [MarshalAs(UnmanagedType.LPUTF8Str)] string binding);
    [DllImport(Lib)] internal static extern uint aver_ui_settings_build([MarshalAs(UnmanagedType.LPUTF8Str)] string rootName);

    /// <summary>Decodes a native UTF-8 string the ABI lent (valid only until its next string call).</summary>
    internal static string Utf8(IntPtr p) => p == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(p) ?? string.Empty;
}
