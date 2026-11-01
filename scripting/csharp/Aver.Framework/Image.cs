// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A CPU image a script builds, edits and turns into a texture (E8). Pure managed code: it needs no engine to run. See RuntimeTextures.cs for the texture side.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>How an <see cref="Image"/> stores a pixel. Rows are tightly packed, top row first.</summary>
public enum ImageFormat
{
    /// <summary>One byte of luminance.</summary>
    L8 = 0,
    /// <summary>Luminance and alpha, one byte each.</summary>
    LA8 = 1,
    /// <summary>Red, green, blue, one byte each.</summary>
    RGB8 = 2,
    /// <summary>Red, green, blue, alpha, one byte each.</summary>
    RGBA8 = 3,
    /// <summary>One 32-bit float (red).</summary>
    RF = 4,
    /// <summary>Four 32-bit floats.</summary>
    RGBAF = 5,
}

/// <summary>How <see cref="Image.Resize"/> reads the source.</summary>
public enum ImageInterpolation
{
    /// <summary>The nearest source pixel.</summary>
    Nearest = 0,
    /// <summary>A bilinear blend of the four nearest pixels.</summary>
    Bilinear = 1,
}

/// <summary>A colour with float channels, 0..1 for the byte formats and unclamped for the float ones. Not colour-managed: the image's bytes are what they are.</summary>
public readonly struct ImageColor : IEquatable<ImageColor>
{
    /// <summary>Red.</summary>
    public float R { get; }
    /// <summary>Green.</summary>
    public float G { get; }
    /// <summary>Blue.</summary>
    public float B { get; }
    /// <summary>Alpha (1 is opaque).</summary>
    public float A { get; }

    /// <summary>Makes a colour; alpha defaults to opaque.</summary>
    public ImageColor(float r, float g, float b, float a = 1f) { R = r; G = g; B = b; A = a; }

    /// <summary>Transparent black.</summary>
    public static ImageColor Clear => new(0f, 0f, 0f, 0f);

    /// <inheritdoc/>
    public bool Equals(ImageColor o) => R == o.R && G == o.G && B == o.B && A == o.A;
    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is ImageColor c && Equals(c);
    /// <inheritdoc/>
    public override int GetHashCode() => HashCode.Combine(R, G, B, A);
    /// <inheritdoc/>
    public override string ToString() => $"({R}, {G}, {B}, {A})";
}

/// <summary>A width x height grid of pixels in one <see cref="ImageFormat"/>, held in managed memory. Make one, change it with
/// <see cref="SetPixel"/> / <see cref="Fill"/> / <see cref="Blit"/> / <see cref="Resize"/> / <see cref="Convert"/>, then hand it to a
/// <see cref="RuntimeTexture"/> to draw with it. An image costs width x height x <see cref="BytesPerPixel"/> bytes; the engine refuses a texture past 8192 pixels a side.</summary>
public sealed class Image
{
    /// <summary>The largest side an image may have (the runtime texture limit, so an image always fits one).</summary>
    public const int MaxSide = 8192;

    private byte[] _data;

    /// <summary>Width in pixels.</summary>
    public int Width { get; private set; }
    /// <summary>Height in pixels.</summary>
    public int Height { get; private set; }
    /// <summary>The pixel format.</summary>
    public ImageFormat Format { get; private set; }

    /// <summary>Bytes in one pixel of <paramref name="format"/>.</summary>
    public static int BytesPerPixel(ImageFormat format) => format switch
    {
        ImageFormat.L8 => 1,
        ImageFormat.LA8 => 2,
        ImageFormat.RGB8 => 3,
        ImageFormat.RGBA8 => 4,
        ImageFormat.RF => 4,
        ImageFormat.RGBAF => 16,
        _ => throw new ArgumentOutOfRangeException(nameof(format), format, "unknown image format"),
    };

    /// <summary>The bytes in one pixel of this image's format.</summary>
    public int PixelSize => BytesPerPixel(Format);

    /// <summary>A zero-filled image (every channel 0: transparent black for the formats that have alpha).</summary>
    /// <exception cref="ArgumentOutOfRangeException">A side is below 1 or past <see cref="MaxSide"/>, or the format is unknown.</exception>
    public Image(int width, int height, ImageFormat format)
    {
        CheckSize(width, height);
        Width = width;
        Height = height;
        Format = format;
        _data = new byte[checked(width * height * BytesPerPixel(format))];
    }

    private Image(int width, int height, ImageFormat format, byte[] data)
    {
        Width = width;
        Height = height;
        Format = format;
        _data = data;
    }

    private static void CheckSize(int width, int height)
    {
        if (width < 1 || width > MaxSide) throw new ArgumentOutOfRangeException(nameof(width), width, $"an image side is 1..{MaxSide}");
        if (height < 1 || height > MaxSide) throw new ArgumentOutOfRangeException(nameof(height), height, $"an image side is 1..{MaxSide}");
    }

    /// <summary>An image over a copy of <paramref name="data"/> (exactly width x height x <see cref="BytesPerPixel"/> bytes).</summary>
    /// <exception cref="ArgumentException">The byte count is wrong.</exception>
    public static Image FromData(int width, int height, ImageFormat format, byte[] data)
    {
        CheckSize(width, height);
        int want = checked(width * height * BytesPerPixel(format));
        if (data == null || data.Length != want) throw new ArgumentException($"a {width}x{height} {format} image is {want} bytes, got {data?.Length ?? 0}", nameof(data));
        return new Image(width, height, format, (byte[])data.Clone());
    }

    /// <summary>The engine's decoder reads <paramref name="path"/> (PNG, JPEG, TGA, BMP, GIF, PSD, WebP, SVG, OpenEXR clamped to 0..1) into an RGBA8 image; null
    /// when the file is missing, unreadable or past the size limit. The path is used as given.</summary>
    public static Image? Load(string path)
    {
        var size = new int[2];
        if (FwRtex.aver_fw_image_decode(path, size, null, 0) == 0) return null;
        if (size[0] < 1 || size[1] < 1 || size[0] > MaxSide || size[1] > MaxSide) return null;
        var data = new byte[checked(size[0] * size[1] * 4)];
        if (FwRtex.aver_fw_image_decode(path, size, data, data.Length) == 0) return null;
        return new Image(size[0], size[1], ImageFormat.RGBA8, data);
    }

    /// <summary>A copy of this image.</summary>
    public Image Clone() => new(Width, Height, Format, (byte[])_data.Clone());

    /// <summary>A copy of the pixel bytes, rows top first, tightly packed.</summary>
    public byte[] GetData() => (byte[])_data.Clone();

    /// <summary>Replaces the pixel bytes (same size and format as the image).</summary>
    /// <exception cref="ArgumentException">The byte count is wrong.</exception>
    public void SetData(byte[] data)
    {
        if (data == null || data.Length != _data.Length) throw new ArgumentException($"this image is {_data.Length} bytes, got {data?.Length ?? 0}", nameof(data));
        Buffer.BlockCopy(data, 0, _data, 0, data.Length);
    }

    /// <summary>The pixel bytes themselves (no copy): a view the caller may read and write. Valid until the image is resized or converted.</summary>
    public Span<byte> AsSpan() => _data;

    /// <summary>Whether (x, y) is inside the image.</summary>
    public bool Contains(int x, int y) => x >= 0 && y >= 0 && x < Width && y < Height;

    private int Offset(int x, int y)
    {
        if (!Contains(x, y)) throw new ArgumentOutOfRangeException(nameof(x), $"({x}, {y}) is outside the {Width}x{Height} image");
        return (y * Width + x) * PixelSize;
    }

    private static float Byte(byte b) => b / 255f;
    private static byte ToByte(float f)
    {
        if (!(f > 0f)) return 0;   // also NaN
        if (f >= 1f) return 255;
        return (byte)(f * 255f + 0.5f);
    }

    // Rec. 709 luminance of the colour: what an L8 image stores.
    private static float Luma(ImageColor c) => 0.2126f * c.R + 0.7152f * c.G + 0.0722f * c.B;

    private ImageColor Read(int offset)
    {
        switch (Format)
        {
            case ImageFormat.L8: { float l = Byte(_data[offset]); return new ImageColor(l, l, l, 1f); }
            case ImageFormat.LA8: { float l = Byte(_data[offset]); return new ImageColor(l, l, l, Byte(_data[offset + 1])); }
            case ImageFormat.RGB8: return new ImageColor(Byte(_data[offset]), Byte(_data[offset + 1]), Byte(_data[offset + 2]), 1f);
            case ImageFormat.RGBA8: return new ImageColor(Byte(_data[offset]), Byte(_data[offset + 1]), Byte(_data[offset + 2]), Byte(_data[offset + 3]));
            case ImageFormat.RF: return new ImageColor(BitConverter.ToSingle(_data, offset), 0f, 0f, 1f);
            default:
                return new ImageColor(BitConverter.ToSingle(_data, offset), BitConverter.ToSingle(_data, offset + 4),
                                      BitConverter.ToSingle(_data, offset + 8), BitConverter.ToSingle(_data, offset + 12));
        }
    }

    private void Write(int offset, ImageColor c)
    {
        switch (Format)
        {
            case ImageFormat.L8: _data[offset] = ToByte(Luma(c)); break;
            case ImageFormat.LA8: _data[offset] = ToByte(Luma(c)); _data[offset + 1] = ToByte(c.A); break;
            case ImageFormat.RGB8: _data[offset] = ToByte(c.R); _data[offset + 1] = ToByte(c.G); _data[offset + 2] = ToByte(c.B); break;
            case ImageFormat.RGBA8: _data[offset] = ToByte(c.R); _data[offset + 1] = ToByte(c.G); _data[offset + 2] = ToByte(c.B); _data[offset + 3] = ToByte(c.A); break;
            case ImageFormat.RF: BitConverter.TryWriteBytes(new Span<byte>(_data, offset, 4), c.R); break;
            default:
                BitConverter.TryWriteBytes(new Span<byte>(_data, offset, 4), c.R);
                BitConverter.TryWriteBytes(new Span<byte>(_data, offset + 4, 4), c.G);
                BitConverter.TryWriteBytes(new Span<byte>(_data, offset + 8, 4), c.B);
                BitConverter.TryWriteBytes(new Span<byte>(_data, offset + 12, 4), c.A);
                break;
        }
    }

    /// <summary>The pixel at (x, y). A format without a channel reads it as 0 (green and blue of <see cref="ImageFormat.RF"/>) or 1 (alpha without alpha).</summary>
    /// <exception cref="ArgumentOutOfRangeException">(x, y) is outside the image.</exception>
    public ImageColor GetPixel(int x, int y) => Read(Offset(x, y));

    /// <summary>Writes the pixel at (x, y), converting the colour to the image's format (byte formats clamp to 0..1; L8 stores luminance).</summary>
    /// <exception cref="ArgumentOutOfRangeException">(x, y) is outside the image.</exception>
    public void SetPixel(int x, int y, ImageColor color) => Write(Offset(x, y), color);

    /// <summary>Sets every pixel.</summary>
    public void Fill(ImageColor color)
    {
        Write(0, color);
        int size = PixelSize;
        for (int filled = size; filled < _data.Length; filled *= 2)
            Buffer.BlockCopy(_data, 0, _data, filled, Math.Min(filled, _data.Length - filled));
    }

    /// <summary>Sets the pixels of a rectangle; the part outside the image is ignored.</summary>
    public void FillRect(int x, int y, int width, int height, ImageColor color)
    {
        int x0 = Math.Max(x, 0), y0 = Math.Max(y, 0);
        int x1 = (int)Math.Min((long)x + width, Width), y1 = (int)Math.Min((long)y + height, Height);
        if (x0 >= x1 || y0 >= y1) return;
        int size = PixelSize;
        Write((y0 * Width + x0) * size, color);
        int first = (y0 * Width + x0) * size;
        for (int xx = x0 + 1; xx < x1; ++xx) Buffer.BlockCopy(_data, first, _data, (y0 * Width + xx) * size, size);
        int rowBytes = (x1 - x0) * size;
        for (int yy = y0 + 1; yy < y1; ++yy) Buffer.BlockCopy(_data, first, _data, (yy * Width + x0) * size, rowBytes);
    }

    /// <summary>Copies the rectangle (srcX, srcY, srcWidth, srcHeight) of <paramref name="source"/> to (dstX, dstY) of this image, converting each pixel to this
    /// format. Whatever falls outside either image is skipped. Source and destination may be the same image (the copy reads the source as it was).</summary>
    public void Blit(Image source, int srcX, int srcY, int srcWidth, int srcHeight, int dstX, int dstY)
    {
        ArgumentNullException.ThrowIfNull(source);
        // Clip the source rectangle to the source, then to the destination, moving the destination origin with it.
        if (srcX < 0) { dstX -= srcX; srcWidth += srcX; srcX = 0; }
        if (srcY < 0) { dstY -= srcY; srcHeight += srcY; srcY = 0; }
        if (dstX < 0) { srcX -= dstX; srcWidth += dstX; dstX = 0; }
        if (dstY < 0) { srcY -= dstY; srcHeight += dstY; dstY = 0; }
        srcWidth = Math.Min(srcWidth, Math.Min(source.Width - srcX, Width - dstX));
        srcHeight = Math.Min(srcHeight, Math.Min(source.Height - srcY, Height - dstY));
        if (srcWidth <= 0 || srcHeight <= 0) return;
        Image from = ReferenceEquals(source, this) ? source.Clone() : source;
        if (from.Format == Format)
        {
            int size = PixelSize;
            for (int y = 0; y < srcHeight; ++y)
                Buffer.BlockCopy(from._data, ((srcY + y) * from.Width + srcX) * size, _data, ((dstY + y) * Width + dstX) * size, srcWidth * size);
            return;
        }
        for (int y = 0; y < srcHeight; ++y)
            for (int x = 0; x < srcWidth; ++x)
                Write(((dstY + y) * Width + dstX + x) * PixelSize, from.Read(((srcY + y) * from.Width + srcX + x) * from.PixelSize));
    }

    /// <summary>Copies the whole of <paramref name="source"/> to (dstX, dstY).</summary>
    public void Blit(Image source, int dstX, int dstY)
    {
        ArgumentNullException.ThrowIfNull(source);
        Blit(source, 0, 0, source.Width, source.Height, dstX, dstY);
    }

    /// <summary>A new image of the rectangle (clipped to the image) in the same format; null when the clipped rectangle is empty.</summary>
    public Image? GetRegion(int x, int y, int width, int height)
    {
        int x0 = Math.Max(x, 0), y0 = Math.Max(y, 0);
        int x1 = (int)Math.Min((long)x + width, Width), y1 = (int)Math.Min((long)y + height, Height);
        if (x0 >= x1 || y0 >= y1) return null;
        var r = new Image(x1 - x0, y1 - y0, Format);
        r.Blit(this, x0, y0, x1 - x0, y1 - y0, 0, 0);
        return r;
    }

    /// <summary>Turns this image into <paramref name="format"/> in place. Going to a format with fewer channels drops them (to L8: luminance); one with more
    /// fills them (alpha 1, a missing colour 0).</summary>
    public void Convert(ImageFormat format)
    {
        if (format == Format) return;
        _ = BytesPerPixel(format);   // rejects an unknown value
        var to = new Image(Width, Height, format);
        int fromSize = PixelSize, toSize = to.PixelSize, n = Width * Height;
        for (int i = 0; i < n; ++i) to.Write(i * toSize, Read(i * fromSize));
        _data = to._data;
        Format = format;
    }

    /// <summary>A copy of this image in <paramref name="format"/>.</summary>
    public Image ConvertedTo(ImageFormat format)
    {
        Image c = Clone();
        c.Convert(format);
        return c;
    }

    /// <summary>Resamples this image to <paramref name="width"/> x <paramref name="height"/> in place.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A side is below 1 or past <see cref="MaxSide"/>.</exception>
    public void Resize(int width, int height, ImageInterpolation interpolation = ImageInterpolation.Bilinear)
    {
        CheckSize(width, height);
        if (width == Width && height == Height) return;
        var to = new Image(width, height, Format);
        int size = PixelSize;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                // Pixel centres: destination pixel x covers source [x*sw/w, (x+1)*sw/w).
                float sx = (x + 0.5f) * Width / width - 0.5f, sy = (y + 0.5f) * Height / height - 0.5f;
                if (interpolation == ImageInterpolation.Nearest)
                {
                    int nx = Math.Clamp((int)MathF.Floor((x + 0.5f) * Width / width), 0, Width - 1);
                    int ny = Math.Clamp((int)MathF.Floor((y + 0.5f) * Height / height), 0, Height - 1);
                    Buffer.BlockCopy(_data, (ny * Width + nx) * size, to._data, (y * width + x) * size, size);
                    continue;
                }
                int x0 = (int)MathF.Floor(sx), y0 = (int)MathF.Floor(sy);
                float fx = sx - x0, fy = sy - y0;
                int xa = Math.Clamp(x0, 0, Width - 1), xb = Math.Clamp(x0 + 1, 0, Width - 1);
                int ya = Math.Clamp(y0, 0, Height - 1), yb = Math.Clamp(y0 + 1, 0, Height - 1);
                ImageColor c00 = Read((ya * Width + xa) * size), c10 = Read((ya * Width + xb) * size);
                ImageColor c01 = Read((yb * Width + xa) * size), c11 = Read((yb * Width + xb) * size);
                to.Write((y * width + x) * size, new ImageColor(
                    Bilerp(c00.R, c10.R, c01.R, c11.R, fx, fy), Bilerp(c00.G, c10.G, c01.G, c11.G, fx, fy),
                    Bilerp(c00.B, c10.B, c01.B, c11.B, fx, fy), Bilerp(c00.A, c10.A, c01.A, c11.A, fx, fy)));
            }
        }
        _data = to._data;
        Width = width;
        Height = height;
    }

    private static float Bilerp(float a, float b, float c, float d, float fx, float fy)
    {
        float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
        return top + (bottom - top) * fy;
    }

    /// <summary>Mirrors the image left to right.</summary>
    public void FlipX()
    {
        int size = PixelSize;
        var tmp = new byte[size];
        for (int y = 0; y < Height; ++y)
            for (int x = 0; x < Width / 2; ++x)
            {
                int a = (y * Width + x) * size, b = (y * Width + (Width - 1 - x)) * size;
                Buffer.BlockCopy(_data, a, tmp, 0, size);
                Buffer.BlockCopy(_data, b, _data, a, size);
                Buffer.BlockCopy(tmp, 0, _data, b, size);
            }
    }

    /// <summary>Mirrors the image top to bottom.</summary>
    public void FlipY()
    {
        int row = Width * PixelSize;
        var tmp = new byte[row];
        for (int y = 0; y < Height / 2; ++y)
        {
            int a = y * row, b = (Height - 1 - y) * row;
            Buffer.BlockCopy(_data, a, tmp, 0, row);
            Buffer.BlockCopy(_data, b, _data, a, row);
            Buffer.BlockCopy(tmp, 0, _data, b, row);
        }
    }

    // ---- what RuntimeTexture uploads: the image in one of the four formats the engine's runtime textures hold ----

    internal const int NativeRgba8 = 0, NativeRgba8Srgb = 1, NativeR8 = 2, NativeRgba16F = 3;

    /// <summary>The pixels as the engine's runtime texture wants them: L8 becomes R8, RF and RGBAF become RGBA16F (halves), every other format RGBA8 (srgb picks the
    /// colour decode).</summary>
    internal byte[] ToNative(bool srgb, out int nativeFormat)
    {
        switch (Format)
        {
            case ImageFormat.L8: nativeFormat = NativeR8; return (byte[])_data.Clone();
            case ImageFormat.RGBA8: nativeFormat = srgb ? NativeRgba8Srgb : NativeRgba8; return (byte[])_data.Clone();
            case ImageFormat.RF:
            case ImageFormat.RGBAF:
            {
                nativeFormat = NativeRgba16F;
                var halves = new byte[Width * Height * 8];
                int size = PixelSize;
                for (int i = 0; i < Width * Height; ++i)
                {
                    ImageColor c = Read(i * size);
                    Span<byte> p = new(halves, i * 8, 8);
                    BitConverter.TryWriteBytes(p.Slice(0, 2), BitConverter.HalfToUInt16Bits((Half)c.R));
                    BitConverter.TryWriteBytes(p.Slice(2, 2), BitConverter.HalfToUInt16Bits((Half)c.G));
                    BitConverter.TryWriteBytes(p.Slice(4, 2), BitConverter.HalfToUInt16Bits((Half)c.B));
                    BitConverter.TryWriteBytes(p.Slice(6, 2), BitConverter.HalfToUInt16Bits((Half)c.A));
                }
                return halves;
            }
            default:
                nativeFormat = srgb ? NativeRgba8Srgb : NativeRgba8;
                return ConvertedTo(ImageFormat.RGBA8)._data;
        }
    }

    /// <summary>An image of a native runtime-texture format's bytes: R8 as L8, RGBA8 and RGBA8-sRGB as RGBA8, RGBA16F as RGBAF (halves widened to floats).</summary>
    internal static Image FromNative(int width, int height, int nativeFormat, byte[] bytes)
    {
        switch (nativeFormat)
        {
            case NativeR8: return FromData(width, height, ImageFormat.L8, bytes);
            case NativeRgba8: case NativeRgba8Srgb: return FromData(width, height, ImageFormat.RGBA8, bytes);
            case NativeRgba16F:
            {
                var img = new Image(width, height, ImageFormat.RGBAF);
                for (int i = 0; i < width * height; ++i)
                {
                    var src = MemoryMarshal.Cast<byte, ushort>(new ReadOnlySpan<byte>(bytes, i * 8, 8));
                    img.Write(i * 16, new ImageColor((float)BitConverter.UInt16BitsToHalf(src[0]), (float)BitConverter.UInt16BitsToHalf(src[1]),
                                                     (float)BitConverter.UInt16BitsToHalf(src[2]), (float)BitConverter.UInt16BitsToHalf(src[3])));
                }
                return img;
            }
            default: throw new ArgumentOutOfRangeException(nameof(nativeFormat), nativeFormat, "unknown runtime texture format");
        }
    }
}
