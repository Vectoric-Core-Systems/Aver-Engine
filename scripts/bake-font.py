"""Bakes a TrueType font into an .ocfont glyph table plus a PNG atlas.

WHY OFFLINE. modules/ui is the retained GAME UI and could not draw a character: no font, no glyph,
no addText anywhere in ui/, ui.abi/ or render.ui/. Rasterising at runtime would mean vendoring a
TrueType library, and Dear ImGui's atlas is not reusable -- ImGui is deliberately split out of the
game build, so reaching for it would put an editor UI toolkit back into a shipped game. Baking keeps
the engine's dependency set unchanged and matches how every other asset here works.

    python scripts/bake-font.py third_party/fonts/Roboto-Regular.ttf --size 16 --out <dir>

Writes <out>/<stem>.ocfont and <out>/<stem>.png. Needs Pillow, which is already used by this repo's
other asset scripts (make-editor-icons.py, brand.py).
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont


def bake(ttf, size, out_dir, first, last, padding):
    font = ImageFont.truetype(ttf, size)
    stem = os.path.splitext(os.path.basename(ttf))[0]
    # PIL's default text anchor is 'la' -- LEFT, ASCENDER -- so both draw.text() and getbbox() work
    # in a space whose y = 0 is the ascender line, NOT the baseline. UiFont::offY is documented as
    # down from the BASELINE, so every glyph's vertical offset has to be rebased by the ascent or
    # every string renders a full ascent too low. Measured on Roboto 16: ascent 15.
    ascent_px, descent_px = font.getmetrics()

    # Measure every glyph first, so the atlas is sized rather than guessed at and then overflowed.
    glyphs = []
    for cp in range(first, last + 1):
        ch = chr(cp)
        # getbbox returns the INK box relative to the origin: left/top can be negative, and top is
        # negative for anything above the baseline, which is exactly the offY the draw list wants.
        box = font.getbbox(ch)
        adv = font.getlength(ch)
        if box is None:
            box = (0, 0, 0, 0)
        w = box[2] - box[0]
        h = box[3] - box[1]
        # inkTop is kept in ASCENDER space for the draw call below; offY is the rebased value the
        # engine consumes. Keeping both apart is what stops the two uses being confused.
        glyphs.append({"cp": cp, "w": w, "h": h,
                       "offX": box[0], "inkTop": box[1], "offY": box[1] - ascent_px, "adv": adv})

    # A simple shelf pack. Not optimal, and it does not need to be: a 96-glyph ASCII range at 16px
    # fits comfortably, and a tighter packer would be code to maintain for no measurable gain.
    atlas_w = 512
    x = padding
    y = padding
    row_h = 0
    for g in glyphs:
        if g["w"] <= 0 or g["h"] <= 0:
            g["x"] = g["y"] = 0          # a space: advance only, no bitmap
            continue
        if x + g["w"] + padding > atlas_w:
            x = padding
            y += row_h + padding
            row_h = 0
        g["x"] = x
        g["y"] = y
        x += g["w"] + padding
        row_h = max(row_h, g["h"])
    atlas_h = 1
    while atlas_h < y + row_h + padding:
        atlas_h *= 2

    # WHITE WITH AN ALPHA MASK, not black-on-transparent: the draw list multiplies by a vertex
    # colour, so the glyph has to carry coverage in alpha and full brightness in RGB or every
    # coloured string would come out dark.
    #
    # AND PREMULTIPLIED, below, which is not optional here. UiVertex colours are premultiplied
    # (uiPremultiply) and the UI pipeline blends accordingly, so an atlas left in straight alpha
    # makes every partially-covered edge texel contribute full RGB at partial coverage. Measured:
    # each glyph rendered legible but sitting on a pale box, which reads as a packing bug and is not
    # one.
    img = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for g in glyphs:
        if g["w"] <= 0 or g["h"] <= 0:
            continue
        # Draw at an offset that puts the ink box's top-left exactly at (x, y).
        draw.text((g["x"] - g["offX"], g["y"] - g["inkTop"]), chr(g["cp"]), font=font, fill=(255, 255, 255, 255))

    # Premultiply: for white text RGB simply becomes the coverage, so a zero-alpha texel is fully
    # black-transparent and contributes nothing when blended.
    px = img.load()
    for yy in range(atlas_h):
        for xx in range(atlas_w):
            a = px[xx, yy][3]
            px[xx, yy] = (a, a, a, a)

    png_path = os.path.join(out_dir, stem + ".png")
    img.save(png_path, format="PNG", optimize=True)

    ascent, descent = ascent_px, descent_px
    lines = [
        "OCFONT 1",
        "# Baked by scripts/bake-font.py. Regenerate rather than hand-editing the GLYPH rows.",
        "NAME %s" % stem,
        "SIZE %d" % size,
        "ATLAS Fonts/%s.png" % stem,
        # descent is reported positive by PIL and is BELOW the baseline, so it is negated here to
        # match UiFont's stated convention.
        "METRICS %g %g %g" % (ascent, -descent, ascent + descent),
        "",
    ]
    for g in glyphs:
        lines.append("GLYPH %d %.6f %.6f %.6f %.6f %g %g %g %g %g" % (
            g["cp"],
            g["x"] / atlas_w, g["y"] / atlas_h,
            (g["x"] + g["w"]) / atlas_w, (g["y"] + g["h"]) / atlas_h,
            g["w"], g["h"], g["offX"], g["offY"], g["adv"]))
    ocfont_path = os.path.join(out_dir, stem + ".ocfont")
    open(ocfont_path, "w", newline="\n").write("\n".join(lines) + "\n")

    print("[font] %s: %d glyphs, atlas %dx%d" % (stem, len(glyphs), atlas_w, atlas_h))
    print("[font] wrote %s" % ocfont_path)
    print("[font] wrote %s" % png_path)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("--size", type=int, default=16)
    ap.add_argument("--out", default=".")
    ap.add_argument("--first", type=int, default=32)   # space
    ap.add_argument("--last", type=int, default=126)   # tilde: printable ASCII
    ap.add_argument("--padding", type=int, default=1)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    return bake(a.ttf, a.size, a.out, a.first, a.last, a.padding)


if __name__ == "__main__":
    sys.exit(main())
