# Cut a supplied graphic (icons on a flat background) into the sprite sheets the editor samples:
# N tiles side by side, transparent background.
#
# The source may lay its icons out in COLUMNS (left to right) or ROWS (top to bottom) -- -Layout picks
# which. The OUTPUT is always a horizontal strip, because that is what the renderers slice: tile t is
# UV [t/N, (t+1)/N] in U, all of V.
#
# Done as inline C# rather than PowerShell loops purely for speed: the keying + box filter is a few
# million inner iterations, which is seconds in C# and minutes in PowerShell.
param(
  [Parameter(Mandatory=$true)][string]$In,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$Tiles = 3,
  [int]$TileW = 256,
  # 0 = derive the tile height from the artwork's own aspect, so nothing is stretched. Give it
  # explicitly only to match a sheet whose aspect is already baked into a renderer.
  [int]$TileH = 0,
  [double]$PadFrac = 0.06,
  [ValidateSet('Columns','Rows')][string]$Layout = 'Columns',
  # Shared: one crop window for every tile, so icons keep their true RELATIVE size and position --
  #   right for states of one motif (the compile badge), where a jump between states would read as
  #   the button twitching.
  # PerTile: a common window SIZE, but centred on each icon's own bounds -- right for unrelated icons
  #   the artist did not align in the source, which would otherwise inherit that misalignment.
  [ValidateSet('Shared','PerTile')][string]$Align = 'Shared'
)

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class SheetMaker
{
    // Straight-alpha RGBA buffer.
    static byte[] px; static int W, H;

    static void Load(string path)
    {
        using (Bitmap src = new Bitmap(path))
        {
            W = src.Width; H = src.Height;
            using (Bitmap b32 = new Bitmap(W, H, PixelFormat.Format32bppArgb))
            {
                using (Graphics g = Graphics.FromImage(b32)) g.DrawImageUnscaled(src, 0, 0);
                BitmapData bd = b32.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                px = new byte[Math.Abs(bd.Stride) * H];
                for (int y = 0; y < H; y++)
                    Marshal.Copy(IntPtr.Add(bd.Scan0, y * bd.Stride), px, y * W * 4, W * 4);
                b32.UnlockBits(bd);
            }
        }
    }

    // Key the flat background out.
    //
    // Only the background REACHABLE FROM THE BORDER is removed, found by a flood fill, rather than every
    // pixel that merely resembles it. The distinction matters: the stale badge's dark question mark sits
    // inside the yellow disc and is nearly the same colour as the background, so a plain colour key
    // deletes it and the badge ships hollow. The fill cannot cross the disc, so anything enclosed
    // survives -- including whatever alpha it was authored with, which is why this branch leaves the
    // pixel completely alone rather than forcing it opaque.
    //
    // Within the outside region a pixel's alpha ramps with its distance from the background, so
    // anti-aliased edges keep partial coverage instead of turning into a staircase, and the colour is
    // un-matted (the background's contribution divided back out) so edges are not left muddied by it.
    static void Key(int br, int bg, int bb, double floor, double full)
    {
        int n = W * H;
        double[] dist = new double[n];
        for (int i = 0; i < n; i++)
        {
            int b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
            dist[i] = Math.Max(Math.Abs(r - br), Math.Max(Math.Abs(g - bg), Math.Abs(b - bb)));
        }

        bool[] outside = new bool[n];
        int[] stack = new int[n];
        int sp = 0;
        for (int x = 0; x < W; x++)
        {
            int t = x, b2 = (H - 1) * W + x;
            if (dist[t]  <= full && !outside[t])  { outside[t]  = true; stack[sp++] = t;  }
            if (dist[b2] <= full && !outside[b2]) { outside[b2] = true; stack[sp++] = b2; }
        }
        for (int y = 0; y < H; y++)
        {
            int l = y * W, r2 = y * W + W - 1;
            if (dist[l]  <= full && !outside[l])  { outside[l]  = true; stack[sp++] = l;  }
            if (dist[r2] <= full && !outside[r2]) { outside[r2] = true; stack[sp++] = r2; }
        }
        while (sp > 0)
        {
            int i = stack[--sp];
            int x = i % W, y = i / W;
            if (x > 0)     { int j = i - 1; if (!outside[j] && dist[j] <= full) { outside[j] = true; stack[sp++] = j; } }
            if (x < W - 1) { int j = i + 1; if (!outside[j] && dist[j] <= full) { outside[j] = true; stack[sp++] = j; } }
            if (y > 0)     { int j = i - W; if (!outside[j] && dist[j] <= full) { outside[j] = true; stack[sp++] = j; } }
            if (y < H - 1) { int j = i + W; if (!outside[j] && dist[j] <= full) { outside[j] = true; stack[sp++] = j; } }
        }

        for (int i = 0; i < n; i++)
        {
            if (!outside[i]) continue;   // enclosed by the art: keep it exactly as authored, alpha included
            int b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
            double a = (dist[i] - floor) / (full - floor);
            if (a <= 0) { px[i*4+0] = px[i*4+1] = px[i*4+2] = px[i*4+3] = 0; continue; }
            if (a > 1) a = 1;
            double cr = br + (r - br) / a, cg = bg + (g - bg) / a, cb = bb + (b - bb) / a;
            px[i*4+2] = (byte)Math.Max(0, Math.Min(255, Math.Round(cr)));
            px[i*4+1] = (byte)Math.Max(0, Math.Min(255, Math.Round(cg)));
            px[i*4+0] = (byte)Math.Max(0, Math.Min(255, Math.Round(cb)));
            px[i*4+3] = (byte)Math.Round(a * 255);
        }
    }

    // Bounds of the artwork inside one source cell.
    static bool Bounds(int x0, int x1, int y0, int y1, out int minX, out int minY, out int maxX, out int maxY)
    {
        minX = int.MaxValue; minY = int.MaxValue; maxX = int.MinValue; maxY = int.MinValue;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (px[(y * W + x) * 4 + 3] > 38)
                {
                    if (x < minX) minX = x; if (x > maxX) maxX = x;
                    if (y < minY) minY = y; if (y > maxY) maxY = y;
                }
        return maxX >= minX;
    }

    // Area-average downscale, done in PREMULTIPLIED space so transparent pixels cannot bleed their
    // colour into the edges of what survives. The sample window is clamped to the cell's OWN rect:
    // a crop wide enough to cover tall artwork can otherwise reach into the neighbouring icon and
    // drag a sliver of the wrong state into the tile.
    static void Blit(byte[] dst, int dstW, int tile, int tw, int th,
                     double sx0, double sy0, double winW, double winH,
                     int cx0, int cy0, int cx1, int cy1)
    {
        double stepX = winW / tw, stepY = winH / th;
        for (int oy = 0; oy < th; oy++)
        {
            double fy0 = sy0 + oy * stepY, fy1 = fy0 + stepY;
            int iy0 = (int)Math.Floor(fy0), iy1 = (int)Math.Ceiling(fy1);
            for (int ox = 0; ox < tw; ox++)
            {
                double fx0 = sx0 + ox * stepX, fx1 = fx0 + stepX;
                int ix0 = (int)Math.Floor(fx0), ix1 = (int)Math.Ceiling(fx1);
                double ar = 0, ag = 0, ab = 0, aa = 0, wsum = 0;
                for (int y = iy0; y < iy1; y++)
                {
                    if (y < cy0 || y >= cy1) continue;
                    double covY = Math.Min(y + 1, fy1) - Math.Max(y, fy0); if (covY <= 0) continue;
                    for (int x = ix0; x < ix1; x++)
                    {
                        if (x < cx0 || x >= cx1) continue;
                        double covX = Math.Min(x + 1, fx1) - Math.Max(x, fx0); if (covX <= 0) continue;
                        double w = covX * covY;
                        int i = (y * W + x) * 4;
                        double a = px[i + 3] / 255.0;
                        ab += px[i + 0] * a * w; ag += px[i + 1] * a * w; ar += px[i + 2] * a * w;
                        aa += a * w; wsum += w;
                    }
                }
                if (wsum <= 0) continue;
                double outA = aa / wsum;
                int o = ((oy * dstW) + tile * tw + ox) * 4;
                if (outA <= 0.0001) { dst[o] = dst[o+1] = dst[o+2] = dst[o+3] = 0; continue; }
                dst[o + 0] = (byte)Math.Max(0, Math.Min(255, Math.Round(ab / wsum / outA)));
                dst[o + 1] = (byte)Math.Max(0, Math.Min(255, Math.Round(ag / wsum / outA)));
                dst[o + 2] = (byte)Math.Max(0, Math.Min(255, Math.Round(ar / wsum / outA)));
                dst[o + 3] = (byte)Math.Round(outA * 255);
            }
        }
    }

    public static string Run(string inPath, string outPath, int tiles, int tw, int th,
                             double padFrac, string layout, string align)
    {
        Load(inPath);
        int br = px[2], bg = px[1], bb = px[0];       // background = the top-left pixel
        Key(br, bg, bb, 6.0, 34.0);

        bool rows = layout == "Rows";
        string report = string.Format("bg=({0},{1},{2}) src={3}x{4} layout={5} tiles={6}\n",
                                      br, bg, bb, W, H, layout, tiles);

        // One SHARED crop window across every tile, in cell-local coordinates: each icon then keeps its
        // true relative size and position, so a button does not jump as its state changes.
        int cellW = rows ? W : W / tiles;
        int cellH = rows ? H / tiles : H;
        double[] bcx = new double[tiles], bcy = new double[tiles];   // each tile's own bounds centre
        int lx0 = int.MaxValue, ly0 = int.MaxValue, lx1 = int.MinValue, ly1 = int.MinValue;
        double maxW = 0, maxH = 0;
        for (int t = 0; t < tiles; t++)
        {
            int ox = rows ? 0 : t * cellW, oy = rows ? t * cellH : 0;
            int a, b, c, d;
            if (!Bounds(ox, ox + cellW, oy, oy + cellH, out a, out b, out c, out d))
                return string.Format("ERROR: tile {0} of {1} is empty -- wrong -Tiles or -Layout?", t, inPath);
            report += string.Format("  tile {0}: x {1}..{2} y {3}..{4}  (local {5}..{6} , {7}..{8})\n",
                                    t, a, c, b, d, a - ox, c - ox, b - oy, d - oy);
            lx0 = Math.Min(lx0, a - ox); lx1 = Math.Max(lx1, c - ox);
            ly0 = Math.Min(ly0, b - oy); ly1 = Math.Max(ly1, d - oy);
            maxW = Math.Max(maxW, c - a + 1); maxH = Math.Max(maxH, d - b + 1);
            bcx[t] = (a + c) / 2.0 - ox;     bcy[t] = (b + d) / 2.0 - oy;
        }

        bool perTile = align == "PerTile";
        double uw = perTile ? maxW : lx1 - lx0 + 1;
        double uh = perTile ? maxH : ly1 - ly0 + 1;
        if (!perTile)
            for (int t = 0; t < tiles; t++) { bcx[t] = lx0 + uw / 2.0; bcy[t] = ly0 + uh / 2.0; }
        // Derive the tile height from the artwork when the caller did not pin one, so the sheet is
        // never the reason an icon looks stretched.
        if (th <= 0) th = Math.Max(1, (int)Math.Round(tw * uh / uw));

        // Grow the union box to the OUTPUT tile's aspect before padding: sampling a window of a
        // different aspect into the tile is exactly what squashes an icon.
        double aspect = (double)tw / th;
        double winW = Math.Max(uw, uh * aspect), winH = winW / aspect;
        winW *= (1.0 + 2.0 * padFrac); winH *= (1.0 + 2.0 * padFrac);
        report += string.Format("  align={0} box {1}x{2} -> tile {3}x{4}, window {5:F1}x{6:F1}\n",
                                align, uw, uh, tw, th, winW, winH);

        int outW = tiles * tw, outH = th;
        byte[] dst = new byte[outW * outH * 4];
        for (int t = 0; t < tiles; t++)
        {
            int ox = rows ? 0 : t * cellW, oy = rows ? t * cellH : 0;
            Blit(dst, outW, t, tw, th,
                 ox + bcx[t] - winW / 2.0, oy + bcy[t] - winH / 2.0, winW, winH,
                 ox, oy, ox + cellW, oy + cellH);
        }

        using (Bitmap ob = new Bitmap(outW, outH, PixelFormat.Format32bppArgb))
        {
            BitmapData bd = ob.LockBits(new Rectangle(0, 0, outW, outH), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            for (int y = 0; y < outH; y++)
                Marshal.Copy(dst, y * outW * 4, IntPtr.Add(bd.Scan0, y * bd.Stride), outW * 4);
            ob.UnlockBits(bd);
            ob.Save(outPath, ImageFormat.Png);
        }
        report += string.Format("wrote {0} ({1}x{2})", outPath, outW, outH);
        return report;
    }
}
'@

[SheetMaker]::Run($In, $Out, $Tiles, $TileW, $TileH, $PadFrac, $Layout, $Align)
