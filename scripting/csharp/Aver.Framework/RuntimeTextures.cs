// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Textures a script builds from an Image at run time (E8). The C ABI is framework_rtex_abi.h; the host uploads what changed each frame.

using System.Runtime.InteropServices;
using System.Text;

namespace Aver.Framework;

/// <summary>The runtime-texture C ABI (framework_rtex_abi.h) and the image decoder. Declared here for the same reason as <see cref="Fw"/>: only this assembly has the
/// DllImport resolver. The host-side half (taking the dirty list, publishing the GPU handle) is not declared: scripts do not call it.</summary>
internal static class FwRtex
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_rtex_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_rtex_create([MarshalAs(UnmanagedType.LPUTF8Str)] string key, int width, int height, int format, int mips);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_destroy(int id);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_find([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_set_pixels(int id, int width, int height, int format, byte[] data, int bytes);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_set_region(int id, int x, int y, int w, int h, byte[] data, int bytes);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_get_pixels(int id, byte[]? output, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_info(int id, int[] out6);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_key(int id, byte[] output, int cap);
    [DllImport(Lib)] internal static extern int aver_fw_rtex_material_path(int id, byte[] output, int cap);
    [DllImport(Lib)] internal static extern ulong aver_fw_rtex_gpu_handle(int id);
    [DllImport(Lib)] internal static extern int aver_fw_image_decode([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int[]? out2, byte[]? output, int cap);

    internal static string Text(Func<byte[], int, int> read)
    {
        var buf = new byte[256];
        int n = read(buf, buf.Length);
        if (n <= 0) return "";
        if (n >= buf.Length) { buf = new byte[n + 1]; read(buf, buf.Length); }
        return Encoding.UTF8.GetString(buf, 0, Math.Min(n, buf.Length - 1));
    }
}

/// <summary>A texture whose pixels come from an <see cref="Image"/> and can be replaced at any time. A material uses it by putting <see cref="MaterialPath"/> in a
/// texture slot (<c>Pbr.PbrMaterial.SetTexturePath</c>); the UI draws it through <see cref="GpuHandle"/>. The pixels reach the GPU on the next frame after
/// <see cref="Update(Image)"/>, as a new GPU texture (the old one is freed a few frames later), so updating every frame is allowed but not free: keep the image small
/// or update a region at a time. At most 256 live, 8192 pixels a side, 256 MB each, 1 GB together (the engine refuses past that).</summary>
public sealed class RuntimeTexture
{
    private readonly int _id;

    private RuntimeTexture(int id) { _id = id; }

    /// <summary>The engine's id for this texture.</summary>
    public int Id => _id;

    /// <summary>The key it was made with.</summary>
    public string Key => FwRtex.Text((b, c) => FwRtex.aver_fw_rtex_key(_id, b, c));

    /// <summary>The texture-slot path a material names it by ("@rt:rtex.&lt;key&gt;").</summary>
    public string MaterialPath => FwRtex.Text((b, c) => FwRtex.aver_fw_rtex_material_path(_id, b, c));

    /// <summary>The GPU texture handle the host uploaded, what a UI draw command carries. 0 until the first upload (the frame after the first pixels are set); it
    /// changes on every upload, so read it again after a frame that updated the texture.</summary>
    public ulong GpuHandle => FwRtex.aver_fw_rtex_gpu_handle(_id);

    /// <summary>Whether the texture still exists (it does not after <see cref="Destroy"/> or the end of a play session).</summary>
    public bool IsValid => Info(out _);

    private bool Info(out int[] info)
    {
        info = new int[6];
        return FwRtex.aver_fw_rtex_info(_id, info) != 0;
    }

    /// <summary>Width in pixels (0 when the texture is gone).</summary>
    public int Width => Info(out var i) ? i[0] : 0;
    /// <summary>Height in pixels (0 when the texture is gone).</summary>
    public int Height => Info(out var i) ? i[1] : 0;
    /// <summary>Whether the host builds a full mip chain at upload (8-bit textures only).</summary>
    public bool Mipmaps => Info(out var i) && i[3] != 0;
    /// <summary>Counts how many times the pixels changed.</summary>
    public int Generation => Info(out var i) ? i[4] : 0;

    /// <summary>Makes the texture <paramref name="key"/> from <paramref name="image"/>, or returns the one that already has the key (its pixels are then replaced by the
    /// image's). <paramref name="srgb"/> says the 8-bit colour bytes are sRGB-encoded (colour images; leave it false for data such as masks and normal maps).
    /// <paramref name="mipmaps"/> asks for a mip chain. The image formats map as: L8 to one channel, RF and RGBAF to half floats, the rest to RGBA8. Null when the
    /// engine refuses (a bad key, a budget).</summary>
    public static RuntimeTexture? Create(string key, Image image, bool srgb = false, bool mipmaps = false)
    {
        ArgumentNullException.ThrowIfNull(image);
        if (string.IsNullOrEmpty(key)) return null;
        byte[] bytes = image.ToNative(srgb, out int format);
        int id = FwRtex.aver_fw_rtex_create(key, image.Width, image.Height, format, mipmaps ? 1 : 0);
        if (id == 0) return null;
        var t = new RuntimeTexture(id);
        return FwRtex.aver_fw_rtex_set_pixels(id, image.Width, image.Height, format, bytes, bytes.Length) != 0 ? t : null;
    }

    /// <summary>The texture with this key, or null.</summary>
    public static RuntimeTexture? Find(string key)
    {
        int id = string.IsNullOrEmpty(key) ? 0 : FwRtex.aver_fw_rtex_find(key);
        return id == 0 ? null : new RuntimeTexture(id);
    }

    /// <summary>Replaces the pixels (and the size and format) with the image's. False when the engine refuses.</summary>
    public bool Update(Image image, bool srgb = false)
    {
        ArgumentNullException.ThrowIfNull(image);
        byte[] bytes = image.ToNative(srgb, out int format);
        return FwRtex.aver_fw_rtex_set_pixels(_id, image.Width, image.Height, format, bytes, bytes.Length) != 0;
    }

    /// <summary>Writes the rectangle (srcX, srcY, width, height) of <paramref name="image"/> to (x, y) of the texture. The image's format must be the one the texture
    /// was made from (L8, RGBA8, or a float format for a half-float texture). False when the rectangle leaves either side.</summary>
    public bool UpdateRegion(Image image, int srcX, int srcY, int width, int height, int x, int y, bool srgb = false)
    {
        ArgumentNullException.ThrowIfNull(image);
        Image? part = image.GetRegion(srcX, srcY, width, height);
        if (part == null || part.Width != width || part.Height != height) return false;
        byte[] bytes = part.ToNative(srgb, out int format);
        if (!Info(out var info) || info[2] != format) return false;
        return FwRtex.aver_fw_rtex_set_region(_id, x, y, width, height, bytes, bytes.Length) != 0;
    }

    /// <summary>A copy of the texture's current pixels as an image (null when the texture is gone). R8 reads as L8, the RGBA8 forms as RGBA8, half floats as RGBAF.</summary>
    public Image? GetImage()
    {
        if (!Info(out var info)) return null;
        int bytes = FwRtex.aver_fw_rtex_get_pixels(_id, null, 0);
        if (bytes <= 0) return null;
        var data = new byte[bytes];
        if (FwRtex.aver_fw_rtex_get_pixels(_id, data, data.Length) != bytes) return null;
        return Image.FromNative(info[0], info[1], info[2], data);
    }

    /// <summary>Removes the texture. Materials that name it fall back to their default texture; the GPU texture is freed a few frames later.</summary>
    public bool Destroy() => FwRtex.aver_fw_rtex_destroy(_id) != 0;
}
