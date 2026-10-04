"""Draws the Content Browser's ASSET-TYPE icons: animation, skeleton and mesh.

SEPARATE FROM branding/file-icons.png ON PURPOSE. That sheet is the project owner's own artwork,
sliced; branding/ASSETS.md is strict that anything shipped be traceable to a human author and that
anything machine-made be obvious rather than discovered later. So this draws a NEW sheet rather than
adding a fifth tile to theirs, and ASSETS.md lists it as generated.

The house style is matched from measurements of the human sheet rather than guessed:

    tile            321 x 432
    page            #999999, full bleed
    background      #262626 (the banner colour), showing only through the folded corner
    fold            triangle (232,0) (320,0) (320,88), with a #4D4D4D crease
    badge           pointy-top hexagon, centre (159,215), 245 wide x 265 tall
    caption         Roboto Medium, white, baseline block y 381..414, centred

Everything is drawn at 4x and downsampled, which is where the antialiasing comes from.

    python scripts/make-asset-icons.py
"""
import os
import math
from PIL import Image, ImageDraw, ImageFont

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(_ROOT, "branding", "asset-icons.png")
FONT = os.path.join(_ROOT, "third_party", "fonts", "Roboto-Medium.ttf")

TILE_W, TILE_H = 321, 432
SS = 4                                   # supersample factor
BG = (38, 38, 38, 255)
PAGE = (153, 153, 153, 255)
CREASE = (77, 77, 77, 255)
WHITE = (255, 255, 255, 255)

HEX_CX, HEX_CY = 159, 215
HEX_W, HEX_H = 245, 265
FOLD = [(232, 0), (321, 0), (321, 88)]
CAPTION_MID_Y = 398


def hexagon(cx, cy, w, h):
    """A pointy-top hexagon: vertices at top and bottom, flats left and right."""
    return [
        (cx, cy - h / 2),
        (cx + w / 2, cy - h / 4),
        (cx + w / 2, cy + h / 4),
        (cx, cy + h / 2),
        (cx - w / 2, cy + h / 4),
        (cx - w / 2, cy - h / 4),
    ]


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(4))


def draw_badge(d, top, bottom):
    """The hexagon, filled with a vertical gradient between two colours."""
    poly = [(x * SS, y * SS) for x, y in hexagon(HEX_CX, HEX_CY, HEX_W, HEX_H)]
    # Scanline the gradient inside a mask of the hexagon, which keeps the edge antialiased by the
    # same downsample as everything else.
    y0 = int((HEX_CY - HEX_H / 2) * SS)
    y1 = int((HEX_CY + HEX_H / 2) * SS)
    mask = Image.new("L", (TILE_W * SS, TILE_H * SS), 0)
    ImageDraw.Draw(mask).polygon(poly, fill=255)
    grad = Image.new("RGBA", (TILE_W * SS, TILE_H * SS), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, y1 - y0)
        gd.line([(0, y), (TILE_W * SS, y)], fill=lerp(top, bottom, t))
    d._image.paste(grad, (0, 0), mask)


def thick_line(d, pts, width, colour=WHITE):
    """A polyline with round joins and caps, in supersampled space."""
    p = [(x * SS, y * SS) for x, y in pts]
    w = int(width * SS)
    d.line(p, fill=colour, width=w, joint="curve")
    r = w // 2
    for x, y in p:
        d.ellipse([x - r, y - r, x + r, y + r], fill=colour)


def dot(d, x, y, r, colour=WHITE):
    d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS], fill=colour)


def glyph_anim(d):
    """A running figure, mid-stride, with three motion streaks behind it.

    Drawn rather than posed: the shape has to read at 32 px in a file list, so it is a pictogram
    with heavy limbs and a clear diagonal, not an accurate skeleton.
    """
    cx, cy = HEX_CX + 8, HEX_CY
    W = 13
    dot(d, cx + 26, cy - 62, 17)                                   # head
    thick_line(d, [(cx + 20, cy - 34), (cx - 2, cy + 4)], W)       # torso, leaning forward
    thick_line(d, [(cx + 22, cy - 26), (cx + 48, cy - 8), (cx + 40, cy + 18)], W)   # lead arm
    thick_line(d, [(cx + 16, cy - 22), (cx - 12, cy - 30), (cx - 30, cy - 12)], W)  # trailing arm
    thick_line(d, [(cx - 2, cy + 4), (cx + 20, cy + 34), (cx + 16, cy + 66)], W)    # lead leg
    thick_line(d, [(cx - 2, cy + 4), (cx - 30, cy + 22), (cx - 46, cy + 8)], W)     # trailing leg
    for i, (dy, ln) in enumerate(((-30, 46), (-6, 62), (20, 40))):
        x1 = cx - 58
        thick_line(d, [(x1 - ln, cy + dy), (x1, cy + dy)], 8)


def glyph_skeleton(d):
    """A bone chain: three joints and the links between them, which is what an .ocskel is.

    Solid joints, not rings. A ring would have to be punched with a transparent ellipse, and on an
    RGBA tile that cuts a hole straight through the badge to the page rather than showing the badge
    colour -- which is exactly what the first version of this drew.
    """
    cx, cy = HEX_CX, HEX_CY
    joints = ((cx - 36, cy + 64, 21), (cx + 10, cy - 2, 24), (cx - 8, cy - 68, 19))
    thick_line(d, [(joints[0][0], joints[0][1]), (joints[1][0], joints[1][1])], 13)
    thick_line(d, [(joints[1][0], joints[1][1]), (joints[2][0], joints[2][1])], 13)
    for x, y, r in joints:
        dot(d, x, y, r)


def rounded_box(d, cx, cy, w, h, r, colour=WHITE):
    d.rounded_rectangle([((cx - w / 2) * SS, (cy - h / 2) * SS),
                         ((cx + w / 2) * SS, (cy + h / 2) * SS)],
                        radius=r * SS, fill=colour)


def curve(d, p0, p1, p2, width, colour=WHITE):
    """A quadratic bezier, sampled and drawn as a polyline so it gets thick_line's round joins."""
    pts = []
    for i in range(25):
        t = i / 24.0
        u = 1.0 - t
        pts.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))
    thick_line(d, pts, width, colour)


def glyph_graph(d):
    """Three nodes wired together: two feeding one, which is what a .ocgraph looks like.

    DELIBERATELY UNLIKE glyph_skeleton, which is also three things joined by two links. That one is
    circles with thick straight bones; this is rectangles with thin curved wires. At 32px in a file
    list the pair have to be told apart at a glance, and shape-plus-curvature does that where a
    different arrangement of the same primitives would not.

    Wires are drawn BEFORE the boxes so the boxes sit on top. They meet the boxes at their edges, so
    nothing actually overlaps -- but drawing them after would leave a wire crossing a node body if
    the layout is ever nudged, and that reads as a bug rather than a diagram.
    """
    cx, cy = HEX_CX, HEX_CY
    NW, NH, NR = 58, 36, 11
    a = (cx - 52, cy - 58)      # top-left input
    b = (cx - 52, cy + 58)      # bottom-left input
    o = (cx + 58, cy)           # the node they feed

    curve(d, (a[0] + NW / 2, a[1]), (cx + 10, a[1]), (o[0] - NW / 2, o[1]), 8)
    curve(d, (b[0] + NW / 2, b[1]), (cx + 10, b[1]), (o[0] - NW / 2, o[1]), 8)
    for x, y in (a, b, o):
        rounded_box(d, x, y, NW, NH, NR)


def glyph_mesh(d):
    """An isometric wireframe cube: the shape a static mesh reads as."""
    cx, cy = HEX_CX, HEX_CY
    w, h, d2 = 78, 44, 62
    top = [(cx, cy - h - d2 // 2), (cx + w, cy - d2 // 2), (cx, cy + h - d2 // 2), (cx - w, cy - d2 // 2)]
    thick_line(d, top + [top[0]], 11)
    for i in (0, 1, 3):
        thick_line(d, [top[i], (top[i][0], top[i][1] + d2)], 11)
    thick_line(d, [(top[1][0], top[1][1] + d2), (top[2][0], top[2][1] + d2),
                   (top[3][0], top[3][1] + d2)], 11)


# Order is LOAD-BEARING: SandboxApp.cpp's assetIconTile() returns these indices by number, so
# inserting a tile rather than appending one silently renames every icon after it.
TILES = [
    ("ANIM",     (232, 138, 38, 255),  (245, 180, 83, 255),  glyph_anim),
    ("SKELETON", (31, 169, 160, 255),  (72, 205, 196, 255),  glyph_skeleton),
    ("MESH",     (63, 163, 77, 255),   (108, 197, 120, 255), glyph_mesh),
    # Violet, because the other three already hold orange, teal and green and a fourth hue is what
    # makes a file list scannable without reading any of the captions.
    ("GRAPH",    (114, 88, 206, 255),  (156, 133, 232, 255), glyph_graph),
]


def make_tile(caption, top, bottom, glyph):
    img = Image.new("RGBA", (TILE_W * SS, TILE_H * SS), PAGE)
    d = ImageDraw.Draw(img)
    d._image = img

    # The folded corner: background showing through, with a lighter crease along the diagonal.
    d.polygon([(x * SS, y * SS) for x, y in FOLD], fill=BG)
    d.line([(FOLD[0][0] * SS, FOLD[0][1] * SS), (FOLD[2][0] * SS, FOLD[2][1] * SS)],
           fill=CREASE, width=3 * SS)

    draw_badge(d, top, bottom)
    glyph(d)

    font = ImageFont.truetype(FONT, 34 * SS)
    box = d.textbbox((0, 0), caption, font=font)
    d.text(((TILE_W * SS - (box[2] - box[0])) / 2 - box[0],
            CAPTION_MID_Y * SS - (box[3] - box[1]) / 2 - box[1]),
           caption, font=font, fill=WHITE)

    return img.resize((TILE_W, TILE_H), Image.LANCZOS)


sheet = Image.new("RGBA", (TILE_W * len(TILES), TILE_H), BG)
for i, (cap, top, bot, glyph) in enumerate(TILES):
    sheet.paste(make_tile(cap, top, bot, glyph), (i * TILE_W, 0))
sheet.save(OUT)
print(f"wrote {OUT}  {sheet.size[0]}x{sheet.size[1]}, {len(TILES)} tiles of {TILE_W}x{TILE_H}")
