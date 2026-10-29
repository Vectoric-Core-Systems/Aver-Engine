// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke into Aver.UI.Abi for the Control runtime (G5 U1): the style box seam (ui_style_abi.h) and the handful of HUD draw calls (ui_abi.h)
// the GUI draws through. The enums restate the headers' constants and are checked against them by AbiEnumTest; the declarations are checked
// against the headers' parameter names. Without the library beside the executable <see cref="UiNative.Available"/> is false: Controls still
// lay out, take input and run signals, and nothing is drawn.

using System.Runtime.InteropServices;

namespace Aver.Fabric;

/// <summary>AVER_UI_SB_*: the kinds of style box.</summary>
internal enum UiSbKind { Empty = 0, Flat = 1, Texture = 2, Line = 3 }

/// <summary>AVER_UI_SBF_*: UiStyleBox::flags.</summary>
[Flags]
internal enum UiSbFlags : uint { DrawCenter = 1, AntiAlias = 2, BorderBlend = 4, Vertical = 8 }

/// <summary>AVER_UI_AXIS_*: how a 9-patch axis fills its middle.</summary>
internal enum UiAxisStretch { Stretch = 0, Tile = 1, TileFit = 2 }

/// <summary>AVER_UI_STRETCH_*: display/window/stretch/mode.</summary>
internal enum UiStretchModeId { Disabled = 0, CanvasItems = 1, Viewport = 2 }

/// <summary>AVER_UI_ASPECT_*: display/window/stretch/aspect.</summary>
internal enum UiStretchAspectId { Ignore = 0, Keep = 1, KeepWidth = 2, KeepHeight = 3, Expand = 4 }

/// <summary>AVER_UI_SCALEMODE_*: display/window/stretch/scale_mode.</summary>
internal enum UiStretchScaleModeId { Fractional = 0, Integer = 1 }

/// <summary>aver::ui::UiStyleBox, field for field (200 bytes, checked against the library at start).</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct UiStyleBoxStruct
{
    public ulong Texture;
    public int Kind;
    public uint Flags;
    public float ContentL, ContentT, ContentR, ContentB;
    public float ExpandL, ExpandT, ExpandR, ExpandB;
    public uint BgColor;
    public uint BorderColorL, BorderColorT, BorderColorR, BorderColorB;
    public float BorderL, BorderT, BorderR, BorderB;
    public float RadiusTL, RadiusTR, RadiusBR, RadiusBL;
    public int CornerDetail;
    public float AaSize;
    public float SkewX, SkewY;
    public uint ShadowColor;
    public float ShadowSize;
    public float ShadowOffX, ShadowOffY;
    public float TexW, TexH;
    public float RegionX, RegionY, RegionW, RegionH;
    public float PatchL, PatchT, PatchR, PatchB;
    public int AxisH, AxisV;
    public uint Modulate;
    public uint LineColor;
    public float Thickness;
    public float GrowBegin, GrowEnd;
}

/// <summary>The geometry the engine tessellated for a style box: what <c>aver_ui_stylebox_mesh</c> returns.</summary>
public sealed class StyleBoxMesh
{
    /// <summary>x, y, u, v per vertex.</summary>
    public float[] Xyuv = System.Array.Empty<float>();
    /// <summary>Straight 0xAABBGGRR per vertex.</summary>
    public uint[] Colors = System.Array.Empty<uint>();
    public uint[] Indices = System.Array.Empty<uint>();
    public ulong Texture;
    public int VertexCount => Colors.Length;
}

internal static class UiNative
{
    private const string Lib = "Aver.UI.Abi";
    private static int s_state;   // 0 unknown, 1 loaded, 2 missing
    private static IntPtr s_scratch;

    internal static bool Available
    {
        get
        {
            if (s_state == 0)
            {
                try
                {
                    s_state = aver_ui_style_abi_version() >= 1 && aver_ui_stylebox_struct_size() == Marshal.SizeOf<UiStyleBoxStruct>() ? 1 : 2;
                    if (s_state == 2) Aver.Scripting.Log.Error("[Fabric] Aver.UI.Abi has a different UiStyleBox size than this build of Aver.Fabric; Control drawing is off");
                }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { s_state = 2; }
            }
            return s_state == 1;
        }
    }

    /// <summary>Forgets a failed probe (a test that has just copied the library beside itself).</summary>
    internal static void Reprobe() { if (s_state == 2) s_state = 0; }

    // The single game thread owns one native copy of the box being passed: no allocation per draw.
    private static IntPtr Pin(in UiStyleBoxStruct sb)
    {
        if (s_scratch == IntPtr.Zero) s_scratch = Marshal.AllocHGlobal(Marshal.SizeOf<UiStyleBoxStruct>());
        Marshal.StructureToPtr(sb, s_scratch, false);
        return s_scratch;
    }

    internal static UiStyleBoxStruct Defaults(UiSbKind kind)
    {
        IntPtr buf = Marshal.AllocHGlobal(Marshal.SizeOf<UiStyleBoxStruct>());
        try
        {
            aver_ui_stylebox_defaults((int)kind, buf);
            return Marshal.PtrToStructure<UiStyleBoxStruct>(buf);
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    internal static void DrawStyleBox(in UiStyleBoxStruct sb, float x, float y, float w, float h)
    {
        if (!Available) return;
        aver_ui_stylebox_draw(Pin(sb), x, y, w, h);
    }

    internal static StyleBoxMesh? Mesh(in UiStyleBoxStruct sb, float x, float y, float w, float h)
    {
        if (!Available) return null;
        IntPtr p = Pin(sb);
        var counts = new int[4];
        aver_ui_stylebox_mesh(p, x, y, w, h, null, null, 0, null, 0, counts);
        var m = new StyleBoxMesh { Xyuv = new float[counts[0] * 4], Colors = new uint[counts[0]], Indices = new uint[counts[1]] };
        if (counts[0] > 0 && aver_ui_stylebox_mesh(p, x, y, w, h, m.Xyuv, m.Colors, counts[0], m.Indices, counts[1], counts) != 1) return null;
        m.Texture = (uint)counts[2] | ((ulong)(uint)counts[3] << 32);
        return m;
    }

    /// <summary>The cells of a 9-patch: x, y, w, h, u0, v0, u1, v1 each.</summary>
    internal static float[] NinePatch(in UiStyleBoxStruct sb, float x, float y, float w, float h)
    {
        if (!Available) return System.Array.Empty<float>();
        IntPtr p = Pin(sb);
        int n = aver_ui_ninepatch_slices(p, x, y, w, h, null, 0);
        var cells = new float[n * 8];
        if (n > 0) aver_ui_ninepatch_slices(p, x, y, w, h, cells, n);
        return cells;
    }

    internal static (Vector2 MinSize, float L, float T, float R, float B) Metrics(in UiStyleBoxStruct sb)
    {
        if (!Available) return (Vector2.Zero, 0, 0, 0, 0);
        var m = new float[6];
        aver_ui_stylebox_metrics(Pin(sb), m);
        return (new Vector2(m[0], m[1]), m[2], m[3], m[4], m[5]);
    }

    internal static string DefaultThemeText()
    {
        if (!Available) return "";
        int n = aver_ui_theme_default_text(IntPtr.Zero, 0);
        var buf = new byte[n + 1];
        IntPtr p = Marshal.AllocHGlobal(n + 1);
        try
        {
            aver_ui_theme_default_text(p, n + 1);
            Marshal.Copy(p, buf, 0, n);
        }
        finally { Marshal.FreeHGlobal(p); }
        return System.Text.Encoding.UTF8.GetString(buf, 0, n);
    }

    internal static bool ThemeValid(string text, out string error)
    {
        error = "";
        if (!Available) return true;
        IntPtr p = Marshal.AllocHGlobal(256);
        try
        {
            int ok = aver_ui_theme_validate(text, p, 256);
            if (ok == 0) error = Marshal.PtrToStringUTF8(p) ?? "";
            return ok == 1;
        }
        finally { Marshal.FreeHGlobal(p); }
    }

    internal static (Rect2 Screen, Vector2 Content, Vector2 Render, Vector2 Scale, bool Letterboxed) Stretch(
        UiStretchModeId mode, UiStretchAspectId aspect, UiStretchScaleModeId scaleMode, float baseW, float baseH, float scale, float windowW, float windowH)
    {
        var o = new float[11];
        if (!Available)
            return (new Rect2(0, 0, windowW, windowH), new Vector2(windowW, windowH), new Vector2(windowW, windowH), Vector2.One, false);
        aver_ui_stretch_compute(new float[] { (float)mode, (float)aspect, (float)scaleMode, baseW, baseH, scale }, windowW, windowH, o);
        return (new Rect2(o[0], o[1], o[2], o[3]), new Vector2(o[4], o[5]), new Vector2(o[6], o[7]), new Vector2(o[8], o[9]), o[10] != 0f);
    }

    // ---- ui_style_abi.h ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_style_abi_version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_stylebox_struct_size();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_stylebox_defaults(int kind, IntPtr outStyleBox);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_stylebox_draw(IntPtr styleBox, float x, float y, float w, float h);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int aver_ui_stylebox_mesh(IntPtr styleBox, float x, float y, float w, float h, float[]? outXYUV, uint[]? outRgba, int maxVerts, uint[]? outIdx, int maxIdx, int[] outCounts);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_stylebox_metrics(IntPtr styleBox, float[] out6);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_ninepatch_slices(IntPtr styleBox, float x, float y, float w, float h, float[]? outCells, int maxCells);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_stretch_compute(float[] in6, float windowW, float windowH, float[] out11);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_theme_default_text(IntPtr outText, int cap);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_theme_validate([MarshalAs(UnmanagedType.LPUTF8Str)] string text, IntPtr errOut, int cap);

    // ---- ui_abi.h (the draw calls and the pointer) ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_begin_frame(float x, float y, float w, float h);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_vertex_count();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_command_count();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_viewport(float[] outXYWH);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_set_layer(int layer);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_layer();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_push_clip(int left, int top, int right, int bottom);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_pop_clip();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_rect(float x, float y, float w, float h, uint rgba);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern void aver_ui_pointer(float[] outXY);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_ui_pointer_down(int button);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int aver_ui_text_measure([MarshalAs(UnmanagedType.LPUTF8Str)] string utf8, [MarshalAs(UnmanagedType.LPUTF8Str)] string? family, float px, int flags, float maxWidth, float[] outWHL);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern float aver_ui_text_block(float x, float y, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8, uint rgba, [MarshalAs(UnmanagedType.LPUTF8Str)] string? family, float px, int flags, float maxWidth, int align, float[]? outWHL);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern float aver_ui_text_block_fx(float x, float y, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8, uint rgba, [MarshalAs(UnmanagedType.LPUTF8Str)] string? family, float px, int flags, float maxWidth, int align,
                                                       float outlineSize, uint outlineRgba, uint shadowRgba, float shadowOffX, float shadowOffY, float shadowOutlineSize, float[]? outWHL);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void aver_ui_textured_rect(float x, float y, float w, float h, ulong texture, float u0, float v0, float u1, float v1, uint rgba);
}
