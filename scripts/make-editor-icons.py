"""Draws the editor's 3D-VIEWPORT icons -- today just the Player Start marker.

NOT THE CONTENT BROWSER'S ICONS. scripts/make-asset-icons.py draws those, and it matches a house
style measured off the project owner's own file-icons.png: a grey page with a folded corner, a
hexagon badge, a caption. None of that applies here. These are drawn on a camera-facing quad
floating in the 3D scene, over whatever the level happens to look like behind them, at whatever size
perspective makes them -- so the constraints are different ones:

  * TRANSPARENT BACKGROUND, and a shape that is legible as a silhouette. There is no tile to sit on.
  * A DARK RIM OUTSIDE A LIGHT ONE. The badge has to read against a bright sky AND against dark
    interior geometry, and a single-colour outline can only do one of those. Two nested strokes,
    light inside dark, is the standard trick and it is why the outer ring here is near-black.
  * AN ANCHOR THE SHAPE POINTS AT. The quad is centred on the entity's own origin, so a map-pin
    silhouette -- wide head, tapering to a tip at the bottom -- says "the thing is HERE, at the tip"
    rather than "somewhere in this circle".

Machine-drawn, and branding/ASSETS.md lists it as such: that file is strict that anything shipped be
traceable to its author and that anything drawn by a script be obvious rather than discovered later.

Everything is drawn at 4x and downsampled, which is where the antialiasing comes from -- the same
way make-asset-icons.py gets its edges.

    python scripts/make-editor-icons.py
"""
import os
import math
from PIL import Image, ImageDraw

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PLAYER_START = os.path.join(_ROOT, "branding", "player-start-icon.png")

SIZE = 256                       # square; the renderer scales it to a world-space half-size
SS = 4                           # supersample factor

# Teal, deliberately: it collides with nothing else already on screen in a viewport. The gizmo owns
# red/green/blue, the selection outline owns orange, the grid is grey. A spawn marker that shared a
# hue with any of those would be one more thing to disambiguate mid-drag.
TEAL_TOP = (45, 212, 191, 255)
TEAL_BOT = (13, 116, 138, 255)
RIM_DARK = (10, 22, 26, 255)     # outer rim -- carries the shape against a bright sky
RIM_LIGHT = (233, 254, 252, 255) # inner rim -- carries it against dark geometry
WHITE = (255, 255, 255, 255)


def pin_outline(cx, head_cy, head_r, tip_y, inflate=0.0):
    """A map pin: a circle head with two tangent lines running down to a point.

    The tangent point is what makes the join smooth instead of a circle with a triangle stuck on it.
    For a tip at distance d from the centre, the tangent from the tip touches the circle at angle
    acos(r/d) off the centre-to-tip direction -- so the straight edges leave the circle exactly where
    its own tangent already points at the tip, and there is no corner.
    """
    r = head_r + inflate
    tip = (cx, tip_y + inflate)
    d = tip[1] - head_cy
    if d <= r:
        raise ValueError("the tip must lie outside the head circle")
    a = math.acos(r / d)                     # half-angle of the tangent pair, from straight down
    pts = [tip]
    # Sweep the circle the long way round, from the right tangent point up over the top to the left
    # one, so the polygon closes through the head rather than across it.
    start = math.pi / 2 - a                  # right tangent, measured from +x, y down
    end = math.pi / 2 + a - 2 * math.pi      # left tangent, going the long way (over the top)
    steps = 96
    for i in range(steps + 1):
        t = start + (end - start) * i / steps
        pts.append((cx + r * math.cos(t), head_cy + r * math.sin(t)))
    return pts


def vertical_gradient(size, top, bottom, mask):
    """Fills `mask` with a top-to-bottom lerp of two colours."""
    grad = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(size):
        t = y / max(size - 1, 1)
        gd.line([(0, y), (size, y)],
                fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def figure(d, cx, cy, scale):
    """A standing person: head, shoulders, two legs. Drawn in the negative space of the pin head.

    A FIGURE RATHER THAN AN ARROW, and the reason is worth writing down: this icon is a CAMERA-FACING
    billboard, so anything directional on its face would point wherever the viewer happens to stand
    and would be actively misleading about the spawn's yaw. The facing is shown separately, in world
    space, by the direction line the editor draws through the marker.
    """
    def s(v):
        return v * scale
    d.ellipse([(cx - s(13)) * SS, (cy - s(40)) * SS, (cx + s(13)) * SS, (cy - s(14)) * SS], fill=WHITE)
    # Torso: a rounded trapezoid, wider at the shoulders.
    d.polygon([((cx - s(21)) * SS, (cy - s(6)) * SS), ((cx + s(21)) * SS, (cy - s(6)) * SS),
               ((cx + s(14)) * SS, (cy + s(20)) * SS), ((cx - s(14)) * SS, (cy + s(20)) * SS)],
              fill=WHITE)
    d.rounded_rectangle([((cx - s(21)) * SS, (cy - s(10)) * SS), ((cx + s(21)) * SS, (cy + s(4)) * SS)],
                        radius=int(s(7) * SS), fill=WHITE)
    # Legs.
    for sign in (-1, 1):
        d.polygon([((cx + sign * s(3)) * SS, (cy + s(14)) * SS),
                   ((cx + sign * s(16)) * SS, (cy + s(14)) * SS),
                   ((cx + sign * s(13)) * SS, (cy + s(44)) * SS),
                   ((cx + sign * s(2)) * SS, (cy + s(44)) * SS)], fill=WHITE)


def make_player_start():
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    cx, head_cy, head_r, tip_y = 128.0, 104.0, 74.0, 240.0

    def poly(inflate):
        return [(x * SS, y * SS) for x, y in pin_outline(cx, head_cy, head_r, tip_y, inflate)]

    # Three nested silhouettes, outermost first: dark rim, light rim, then the gradient body.
    d.polygon(poly(9.0), fill=RIM_DARK)
    d.polygon(poly(4.0), fill=RIM_LIGHT)

    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).polygon(poly(0.0), fill=255)
    img = Image.alpha_composite(img, vertical_gradient(n, TEAL_TOP, TEAL_BOT, mask))

    figure(ImageDraw.Draw(img), cx, head_cy + 2.0, 1.0)

    out = img.resize((SIZE, SIZE), Image.LANCZOS)
    out.save(OUT_PLAYER_START)
    return out


if __name__ == "__main__":
    im = make_player_start()
    print("wrote {} ({}x{})".format(OUT_PLAYER_START, im.size[0], im.size[1]))
