// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_subview_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The sub-viewport C ABI (framework_subview_abi.h). Declared here for the same reason as <see cref="Fw"/>: only this assembly has the
/// DllImport resolver. The host-side half (planning, the per-view host info, publishing the texture) is not declared: scripts do not call it.</summary>
internal static class FwSubView
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_subview_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_subview_create([MarshalAs(UnmanagedType.LPUTF8Str)] string key, int width, int height);
    [DllImport(Lib)] internal static extern int aver_fw_subview_destroy(int id);
    [DllImport(Lib)] internal static extern int aver_fw_subview_find([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_size(int id, int width, int height);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_update(int id, int mode);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_clear(int id, int mode);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_transparent(int id, int on);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_own_world(int id, int on);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_camera(int id, int entity);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_scale(int id, float scale, int msaa);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_parent_visible(int id, int on);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_clear_color(int id, float r, float g, float b, float a);
    [DllImport(Lib)] internal static extern int aver_fw_subview_consumer(int id, int kind, int delta);
    [DllImport(Lib)] internal static extern int aver_fw_subview_request_render(int id);
    [DllImport(Lib)] internal static extern int aver_fw_subview_set_screen_rect(int id, float x, float y, float w, float h, int visible);
    [DllImport(Lib)] internal static extern int aver_fw_subview_info(int id, int[] out8);
    [DllImport(Lib)] internal static extern ulong aver_fw_subview_texture(int id);
}
