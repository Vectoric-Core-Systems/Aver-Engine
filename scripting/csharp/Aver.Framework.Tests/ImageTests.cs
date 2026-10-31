// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// ImageTests (E8): the managed Image type -- pixel access in every format, fills, blits, resize, conversion, flips, and the bytes a runtime texture uploads. No engine.

using Aver.Framework;

static class ImageTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("image: a new image is zero and sized by its format", Creation);
        T.Run("image: get/set pixel in every format", PixelAccess);
        T.Run("image: out-of-range access and bad sizes are refused", Bounds);
        T.Run("image: fill and fill-rect", Fills);
        T.Run("image: blit clips, converts, and handles itself", Blits);
        T.Run("image: resize, nearest and bilinear", Resizing);
        T.Run("image: convert between formats", Converting);
        T.Run("image: flips", Flipping);
        T.Run("image: the bytes a runtime texture uploads", Native);
        T.Run("image: data in and out", Data);
    });

    private static void Creation()
    {
        foreach (var (f, size) in new[] { (ImageFormat.L8, 1), (ImageFormat.LA8, 2), (ImageFormat.RGB8, 3), (ImageFormat.RGBA8, 4), (ImageFormat.RF, 4), (ImageFormat.RGBAF, 16) })
        {
            var img = new Image(3, 2, f);
            T.Check($"{f}: {size} bytes a pixel, 3x2 is {3 * 2 * size} bytes", Image.BytesPerPixel(f) == size && img.GetData().Length == 3 * 2 * size && img.Width == 3 && img.Height == 2);
            bool zero = true;
            foreach (byte b in img.GetData()) zero &= b == 0;
            T.Check($"{f}: starts zero", zero);
        }
    }

    private static void PixelAccess()
    {
        var rgba = new Image(2, 2, ImageFormat.RGBA8);
        rgba.SetPixel(1, 0, new ImageColor(1f, 0.5f, 0f, 0.25f));
        ImageColor p = rgba.GetPixel(1, 0);
        T.Check("RGBA8 round-trips to the nearest 1/255", T.Near(p.R, 1f) && T.Near(p.G, 0.5f, 0.003f) && T.Near(p.B, 0f) && T.Near(p.A, 0.25f, 0.003f));
        T.Check("RGBA8 stores bytes 255,128,0,64", rgba.GetData()[4] == 255 && rgba.GetData()[5] == 128 && rgba.GetData()[6] == 0 && rgba.GetData()[7] == 64);
        T.Check("a neighbour is untouched", rgba.GetPixel(0, 0).Equals(ImageColor.Clear));

        rgba.SetPixel(0, 0, new ImageColor(2f, -1f, float.NaN, 9f));
        ImageColor c = rgba.GetPixel(0, 0);
        T.Check("byte formats clamp to 0..1 and read NaN as 0", c.R == 1f && c.G == 0f && c.B == 0f && c.A == 1f);

        var rgb = new Image(1, 1, ImageFormat.RGB8);
        rgb.SetPixel(0, 0, new ImageColor(0.2f, 0.4f, 0.6f, 0.1f));
        T.Check("RGB8 reads alpha as 1", rgb.GetPixel(0, 0).A == 1f && T.Near(rgb.GetPixel(0, 0).G, 0.4f, 0.003f));

        var l8 = new Image(1, 1, ImageFormat.L8);
        l8.SetPixel(0, 0, new ImageColor(1f, 1f, 1f));
        T.Check("L8 stores luminance (white is 255)", l8.GetData()[0] == 255);
        l8.SetPixel(0, 0, new ImageColor(1f, 0f, 0f));
        T.Check("L8 of pure red is Rec.709 luminance 0.2126", l8.GetData()[0] == 54);
        T.Check("L8 reads back as grey with alpha 1", l8.GetPixel(0, 0).R == l8.GetPixel(0, 0).G && l8.GetPixel(0, 0).A == 1f);

        var la = new Image(1, 1, ImageFormat.LA8);
        la.SetPixel(0, 0, new ImageColor(0.5f, 0.5f, 0.5f, 0.5f));
        T.Check("LA8 keeps alpha", T.Near(la.GetPixel(0, 0).A, 0.5f, 0.003f));

        var rf = new Image(1, 1, ImageFormat.RF);
        rf.SetPixel(0, 0, new ImageColor(123.5f, 9f, 9f, 9f));
        ImageColor r = rf.GetPixel(0, 0);
        T.Check("RF keeps an unclamped float and has no other channel", r.R == 123.5f && r.G == 0f && r.B == 0f && r.A == 1f);

        var f4 = new Image(1, 1, ImageFormat.RGBAF);
        f4.SetPixel(0, 0, new ImageColor(-2f, 3.5f, 100f, 0.125f));
        T.Check("RGBAF keeps four unclamped floats", f4.GetPixel(0, 0).Equals(new ImageColor(-2f, 3.5f, 100f, 0.125f)));
    }

    private static void Bounds()
    {
        var img = new Image(2, 2, ImageFormat.RGBA8);
        T.Check("Contains is exact", img.Contains(1, 1) && !img.Contains(2, 0) && !img.Contains(0, -1));
        T.Check("GetPixel past the edge throws", Throws<ArgumentOutOfRangeException>(() => img.GetPixel(2, 0)));
        T.Check("SetPixel at a negative coordinate throws", Throws<ArgumentOutOfRangeException>(() => img.SetPixel(-1, 0, ImageColor.Clear)));
        T.Check("a zero side throws", Throws<ArgumentOutOfRangeException>(() => new Image(0, 4, ImageFormat.L8)));
        T.Check("a side past 8192 throws", Throws<ArgumentOutOfRangeException>(() => new Image(Image.MaxSide + 1, 1, ImageFormat.L8)));
        T.Check("a hostile size throws instead of allocating", Throws<ArgumentOutOfRangeException>(() => new Image(int.MaxValue, int.MaxValue, ImageFormat.RGBAF)));
        T.Check("an unknown format throws", Throws<ArgumentOutOfRangeException>(() => new Image(1, 1, (ImageFormat)99)));
        T.Check("FromData with the wrong byte count throws", Throws<ArgumentException>(() => Image.FromData(2, 2, ImageFormat.RGBA8, new byte[15])));
        T.Check("FromData with null throws", Throws<ArgumentException>(() => Image.FromData(2, 2, ImageFormat.RGBA8, null!)));
        T.Check("Resize to a bad size throws and leaves the image alone", Throws<ArgumentOutOfRangeException>(() => img.Resize(0, 3)) && img.Width == 2 && img.Height == 2);
    }

    private static bool Throws<E>(Action a) where E : Exception
    {
        try { a(); } catch (E) { return true; }
        return false;
    }

    private static void Fills()
    {
        var img = new Image(5, 4, ImageFormat.RGBA8);
        var red = new ImageColor(1f, 0f, 0f, 1f);
        img.Fill(red);
        bool all = true;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 5; ++x) all &= img.GetPixel(x, y).Equals(red);
        T.Check("Fill sets every pixel (an odd pixel count too)", all);

        var rf = new Image(7, 3, ImageFormat.RGBAF);
        rf.Fill(new ImageColor(0.5f, 1.5f, -1f, 2f));
        T.Check("Fill works for 16-byte pixels", rf.GetPixel(6, 2).Equals(new ImageColor(0.5f, 1.5f, -1f, 2f)) && rf.GetPixel(0, 0).Equals(rf.GetPixel(3, 1)));

        var g = new Image(6, 6, ImageFormat.RGBA8);
        var green = new ImageColor(0f, 1f, 0f, 1f);
        g.FillRect(1, 2, 3, 2, green);
        int inside = 0, outside = 0;
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x)
            {
                bool want = x >= 1 && x < 4 && y >= 2 && y < 4;
                if (g.GetPixel(x, y).Equals(green)) ++inside;
                else if (want) ++outside;
                else if (!g.GetPixel(x, y).Equals(ImageColor.Clear)) ++outside;
            }
        T.Check("FillRect fills exactly its rectangle (6 pixels)", inside == 6 && outside == 0);

        var h = new Image(4, 4, ImageFormat.RGBA8);
        h.FillRect(-2, -2, 4, 4, green);
        T.Check("FillRect clips at the corner (2x2 left)", h.GetPixel(0, 0).Equals(green) && h.GetPixel(1, 1).Equals(green) && h.GetPixel(2, 2).Equals(ImageColor.Clear));
        h.FillRect(10, 10, 3, 3, red);
        h.FillRect(0, 0, 0, 5, red);
        h.FillRect(int.MaxValue - 1, 0, 100, 4, red);
        T.Check("an empty, distant or overflowing rectangle changes nothing", h.GetPixel(3, 3).Equals(ImageColor.Clear) && h.GetPixel(0, 0).Equals(green));
    }

    private static void Blits()
    {
        var src = new Image(3, 3, ImageFormat.RGBA8);
        for (int i = 0; i < 9; ++i) src.SetPixel(i % 3, i / 3, new ImageColor(i / 8f, 0f, 0f, 1f));
        var dst = new Image(5, 5, ImageFormat.RGBA8);
        dst.Blit(src, 1, 1);
        T.Check("a whole-image blit lands at the destination", dst.GetPixel(1, 1).Equals(src.GetPixel(0, 0)) && dst.GetPixel(3, 3).Equals(src.GetPixel(2, 2)) && dst.GetPixel(0, 0).Equals(ImageColor.Clear));

        var edge = new Image(4, 4, ImageFormat.RGBA8);
        edge.Blit(src, -1, -1);
        T.Check("a blit hanging off the top-left keeps the visible part", edge.GetPixel(0, 0).Equals(src.GetPixel(1, 1)) && edge.GetPixel(1, 1).Equals(src.GetPixel(2, 2)) && edge.GetPixel(2, 2).Equals(ImageColor.Clear));
        var edge2 = new Image(4, 4, ImageFormat.RGBA8);
        edge2.Blit(src, 3, 3);
        T.Check("and off the bottom-right", edge2.GetPixel(3, 3).Equals(src.GetPixel(0, 0)));
        var none = new Image(4, 4, ImageFormat.RGBA8);
        none.Blit(src, 100, 100);
        none.Blit(src, -100, 0);
        none.Blit(src, 0, 0, -3, 3, 0, 0);
        bool clear = true;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) clear &= none.GetPixel(x, y).Equals(ImageColor.Clear);
        T.Check("a blit entirely outside, or of a negative size, copies nothing", clear);

        var part = new Image(4, 4, ImageFormat.RGBA8);
        part.Blit(src, 1, 1, 2, 2, 0, 0);
        T.Check("a sub-rectangle blit reads the source rectangle", part.GetPixel(0, 0).Equals(src.GetPixel(1, 1)) && part.GetPixel(1, 1).Equals(src.GetPixel(2, 2)) && part.GetPixel(2, 0).Equals(ImageColor.Clear));

        var l8 = new Image(2, 1, ImageFormat.L8);
        l8.SetPixel(0, 0, new ImageColor(1f, 1f, 1f));
        var into = new Image(2, 1, ImageFormat.RGBA8);
        into.Blit(l8, 0, 0);
        T.Check("a blit converts the format (L8 white to opaque white RGBA8)", into.GetPixel(0, 0).Equals(new ImageColor(1f, 1f, 1f, 1f)) && into.GetPixel(1, 0).A == 1f);

        var self = new Image(4, 1, ImageFormat.RGBA8);
        for (int i = 0; i < 4; ++i) self.SetPixel(i, 0, new ImageColor(i / 3f, 0f, 0f, 1f));
        self.Blit(self, 0, 0, 3, 1, 1, 0);
        T.Check("a blit onto itself reads the source as it was (overlap)", T.Near(self.GetPixel(1, 0).R, 0f) && T.Near(self.GetPixel(2, 0).R, 1f / 3f) && T.Near(self.GetPixel(3, 0).R, 2f / 3f));

        Image? region = src.GetRegion(1, 1, 10, 10);
        T.Check("GetRegion clips to the image", region != null && region.Width == 2 && region.Height == 2 && region.GetPixel(0, 0).Equals(src.GetPixel(1, 1)));
        T.Check("GetRegion of nothing is null", src.GetRegion(5, 5, 2, 2) == null);
        T.Check("Blit(null) throws", Throws<ArgumentNullException>(() => dst.Blit(null!, 0, 0)));
    }

    private static void Resizing()
    {
        var img = new Image(2, 2, ImageFormat.RGBA8);
        img.SetPixel(0, 0, new ImageColor(1f, 0f, 0f));
        img.SetPixel(1, 0, new ImageColor(0f, 1f, 0f));
        img.SetPixel(0, 1, new ImageColor(0f, 0f, 1f));
        img.SetPixel(1, 1, new ImageColor(1f, 1f, 1f));
        Image big = img.Clone();
        big.Resize(4, 4, ImageInterpolation.Nearest);
        T.Check("nearest doubles each pixel into a 2x2 block", big.Width == 4 && big.GetPixel(0, 0).Equals(img.GetPixel(0, 0)) && big.GetPixel(1, 1).Equals(img.GetPixel(0, 0)) &&
                big.GetPixel(2, 0).Equals(img.GetPixel(1, 0)) && big.GetPixel(3, 3).Equals(img.GetPixel(1, 1)) && big.GetPixel(0, 3).Equals(img.GetPixel(0, 1)));

        Image same = img.Clone();
        same.Resize(2, 2);
        T.Check("resizing to the same size is a no-op", same.GetData().AsSpan().SequenceEqual(img.GetData()));

        Image half = big.Clone();
        half.Resize(2, 2, ImageInterpolation.Nearest);
        T.Check("nearest back down recovers the original", half.GetData().AsSpan().SequenceEqual(img.GetData()));

        var grey = new Image(2, 1, ImageFormat.RGBA8);
        grey.SetPixel(0, 0, new ImageColor(0f, 0f, 0f));
        grey.SetPixel(1, 0, new ImageColor(1f, 1f, 1f));
        Image smooth = grey.Clone();
        smooth.Resize(4, 1, ImageInterpolation.Bilinear);
        float a = smooth.GetPixel(0, 0).R, b = smooth.GetPixel(1, 0).R, c = smooth.GetPixel(2, 0).R, d = smooth.GetPixel(3, 0).R;
        T.Check("bilinear ramps monotonically between the two pixels and keeps the ends", a == 0f && d == 1f && a < b && b < c && c < d);

        Image one = img.Clone();
        one.Resize(1, 1);
        T.Check("shrinking to one pixel gives a pixel inside the source's range", one.Width == 1 && one.GetPixel(0, 0).R >= 0f && one.GetPixel(0, 0).R <= 1f);

        var wide = new Image(3, 1, ImageFormat.L8);
        wide.Resize(7, 5);
        T.Check("an L8 image resizes and keeps its format", wide.Width == 7 && wide.Height == 5 && wide.Format == ImageFormat.L8 && wide.GetData().Length == 35);
    }

    private static void Converting()
    {
        var img = new Image(1, 1, ImageFormat.RGBA8);
        img.SetPixel(0, 0, new ImageColor(1f, 0f, 0f, 0.5f));
        Image rgb = img.ConvertedTo(ImageFormat.RGB8);
        T.Check("RGBA8 to RGB8 drops alpha", rgb.Format == ImageFormat.RGB8 && rgb.GetData().Length == 3 && rgb.GetData()[0] == 255 && rgb.GetPixel(0, 0).A == 1f);
        Image f = img.ConvertedTo(ImageFormat.RGBAF);
        T.Check("RGBA8 to RGBAF widens exactly", f.GetPixel(0, 0).R == 1f && T.Near(f.GetPixel(0, 0).A, 128f / 255f, 1e-4f));
        Image l = img.ConvertedTo(ImageFormat.L8);
        T.Check("RGBA8 to L8 keeps luminance", l.GetData().Length == 1 && l.GetData()[0] == 54);
        Image la = img.ConvertedTo(ImageFormat.LA8);
        T.Check("RGBA8 to LA8 keeps luminance and alpha", la.GetData()[0] == 54 && la.GetData()[1] == 128);
        img.Convert(ImageFormat.RF);
        T.Check("Convert changes the image in place", img.Format == ImageFormat.RF && img.PixelSize == 4 && img.GetPixel(0, 0).R == 1f);
        img.Convert(ImageFormat.RF);
        T.Check("converting to the same format changes nothing", img.GetPixel(0, 0).R == 1f);
        T.Check("an unknown format throws and leaves the image", Throws<ArgumentOutOfRangeException>(() => img.Convert((ImageFormat)77)) && img.Format == ImageFormat.RF);
    }

    private static void Flipping()
    {
        var img = new Image(3, 2, ImageFormat.RGBA8);
        for (int i = 0; i < 6; ++i) img.SetPixel(i % 3, i / 3, new ImageColor(i / 5f, 0f, 0f));
        Image x = img.Clone();
        x.FlipX();
        T.Check("FlipX mirrors each row (and the odd middle pixel stays)", x.GetPixel(0, 0).Equals(img.GetPixel(2, 0)) && x.GetPixel(1, 0).Equals(img.GetPixel(1, 0)) && x.GetPixel(2, 1).Equals(img.GetPixel(0, 1)));
        Image y = img.Clone();
        y.FlipY();
        T.Check("FlipY swaps the rows", y.GetPixel(0, 0).Equals(img.GetPixel(0, 1)) && y.GetPixel(2, 1).Equals(img.GetPixel(2, 0)));
        x.FlipX();
        y.FlipY();
        T.Check("flipping twice restores it", x.GetData().AsSpan().SequenceEqual(img.GetData()) && y.GetData().AsSpan().SequenceEqual(img.GetData()));
        var one = new Image(1, 1, ImageFormat.RGBAF);
        one.FlipX(); one.FlipY();
        T.Check("a 1x1 image flips to itself", one.Width == 1);
    }

    private static void Native()
    {
        var rgba = new Image(2, 1, ImageFormat.RGBA8);
        rgba.SetPixel(0, 0, new ImageColor(1f, 0.5f, 0f, 1f));
        byte[] b = rgba.ToNative(srgb: false, out int fmt);
        T.Check("RGBA8 uploads as RGBA8 (linear)", fmt == 0 && b.Length == 8 && b[0] == 255);
        rgba.ToNative(srgb: true, out fmt);
        T.Check("with srgb set it uploads as RGBA8-sRGB", fmt == 1);

        var l8 = new Image(3, 1, ImageFormat.L8);
        l8.Fill(new ImageColor(1f, 1f, 1f));
        b = l8.ToNative(false, out fmt);
        T.Check("L8 uploads as R8, one byte a pixel", fmt == 2 && b.Length == 3 && b[2] == 255);

        var rgb = new Image(1, 1, ImageFormat.RGB8);
        rgb.SetPixel(0, 0, new ImageColor(1f, 0f, 0f));
        b = rgb.ToNative(false, out fmt);
        T.Check("RGB8 uploads as RGBA8 with opaque alpha", fmt == 0 && b.Length == 4 && b[0] == 255 && b[3] == 255);

        var f = new Image(1, 1, ImageFormat.RGBAF);
        f.SetPixel(0, 0, new ImageColor(1f, 0.5f, 2f, 0.25f));
        b = f.ToNative(false, out fmt);
        T.Check("RGBAF uploads as RGBA16F, 8 bytes a pixel", fmt == 3 && b.Length == 8);
        Image back = Image.FromNative(1, 1, 3, b);
        T.Check("and the halves read back as the same floats (all four are exact halves)", back.GetPixel(0, 0).Equals(new ImageColor(1f, 0.5f, 2f, 0.25f)));
        Image r8 = Image.FromNative(2, 1, 2, new byte[] { 7, 9 });
        T.Check("FromNative R8 is an L8 image", r8.Format == ImageFormat.L8 && r8.GetData()[1] == 9);
        T.Check("FromNative with an unknown format throws", Throws<ArgumentOutOfRangeException>(() => Image.FromNative(1, 1, 42, new byte[4])));
    }

    private static void Data()
    {
        var data = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };
        Image img = Image.FromData(2, 1, ImageFormat.RGBA8, data);
        data[0] = 99;
        T.Check("FromData copies: changing the source afterwards does nothing", img.GetData()[0] == 1);
        byte[] copy = img.GetData();
        copy[1] = 99;
        T.Check("GetData copies too", img.GetData()[1] == 2);
        img.SetData(new byte[] { 9, 9, 9, 9, 8, 8, 8, 8 });
        T.Check("SetData replaces the bytes", img.GetPixel(1, 0).R == 8f / 255f);
        T.Check("SetData with the wrong length throws", Throws<ArgumentException>(() => img.SetData(new byte[3])));
        img.AsSpan()[0] = 0;
        T.Check("AsSpan is a live view", img.GetData()[0] == 0);
    }
}
