"""Draws the editor's 3D-VIEWPORT icons -- today just the Player Start marker.

NOT THE CONTENT BROWSER'S ICONS. scripts/make-asset-icons.py draws those, and it matches a house
style measured off the project owner's own file-icons.png: a grey page with a folded corner, a
hexagon badge, a caption. None of that applies here. These are drawn on a camera-facing quad
floating in the 3D scene, over whatever the level happens to look like behind them, at whatever size
perspective makes them -- so the constraints are different ones:

  * TRANSPARENT BACKGROUND, and a shape that is legible as a silhouette. There is no tile to sit on.
  * A SINGLE DARK OUTLINE around a light body. The badge has to read against a bright sky AND against
    dark interior geometry; a white-on-anything-light shape would vanish, so the body stays light
    (Unreal's own actor-icon convention) and a dark rim carries the silhouette against a bright
    background. Built by dilating the body's own alpha mask (see OUTLINE_PX below), so it always
    traces the actual silhouette -- arms, flag and all -- rather than a shape drawn by hand.
  * NO SIGNAGE ABOUT FACING. The quad is a CAMERA-FACING billboard (ViewportIconRenderer.cpp), so
    anything on its face that implied a direction would point wherever the viewer stands, not where
    the spawn actually faces -- see figure()'s own comment. The flag is decorative, the way Unreal's
    APlayerStart reads as "a spawn point standing here", not a compass.

Machine-drawn, and branding/ASSETS.md lists it as such: that file is strict that anything shipped be
traceable to its author and that anything drawn by a script be obvious rather than discovered later.

Everything is drawn at 4x and downsampled, which is where the antialiasing comes from -- the same
way make-asset-icons.py gets its edges.

    python scripts/make-editor-icons.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PLAYER_START = os.path.join(_ROOT, "branding", "player-start-icon.png")

SIZE = 256                       # square; the renderer scales it to a world-space half-size
SS = 4                           # supersample factor

BODY = (245, 247, 250, 255)      # light grey/white body -- Unreal's own actor-icon convention
RIM_DARK = (10, 22, 26, 255)     # outline -- carries the shape against a bright sky
FLAG = (235, 90, 60, 255)        # a warm accent so the flag reads as "the" landmark on the figure

# Outline thickness, in the FINAL (post-downsample) SIZE-space pixels -- multiplied by SS below to
# get the supersampled dilation radius, the same convention every other measurement here uses.
OUTLINE_PX = 6.0


def figure(d, cx, cy, scale):
    """A standing person holding a small flag: head, shoulders, two legs, one raised arm and a
    pennant. Drawn as one silhouette -- every part the same fill colour -- so OUTLINE_PX's mask
    dilation traces the whole shape at once rather than per limb.

    A FIGURE RATHER THAN AN ARROW ON THE FACE, and the reason is worth writing down: this icon is a
    CAMERA-FACING billboard, so anything directional drawn on it would point wherever the viewer
    happens to stand and would be actively misleading about the spawn's yaw. The facing is shown
    separately, in world space, by the arrow SandboxApp.cpp draws through the marker; the flag here
    is not that arrow -- it is decoration, the same way a real flagpole reads as "a landmark", not
    a compass.
    """
    def s(v):
        return v * scale

    d.ellipse([(cx - s(13)) * SS, (cy - s(40)) * SS, (cx + s(13)) * SS, (cy - s(14)) * SS], fill=BODY)
    # Torso: a rounded trapezoid, wider at the shoulders.
    d.polygon([((cx - s(21)) * SS, (cy - s(6)) * SS), ((cx + s(21)) * SS, (cy - s(6)) * SS),
               ((cx + s(14)) * SS, (cy + s(20)) * SS), ((cx - s(14)) * SS, (cy + s(20)) * SS)],
              fill=BODY)
    d.rounded_rectangle([((cx - s(21)) * SS, (cy - s(10)) * SS), ((cx + s(21)) * SS, (cy + s(4)) * SS)],
                        radius=int(s(7) * SS), fill=BODY)
    # Legs.
    for sign in (-1, 1):
        d.polygon([((cx + sign * s(6)) * SS, (cy + s(14)) * SS),
                   ((cx + sign * s(16)) * SS, (cy + s(14)) * SS),
                   ((cx + sign * s(13)) * SS, (cy + s(44)) * SS),
                   ((cx + sign * s(5)) * SS, (cy + s(44)) * SS)], fill=BODY)
    # Raised arm: a short capsule from the shoulder up and outward to the hand that holds the pole.
    shoulder = (cx + s(19), cy - s(4))
    hand = (cx + s(34), cy - s(34))
    d.line([(shoulder[0] * SS, shoulder[1] * SS), (hand[0] * SS, hand[1] * SS)],
           fill=BODY, width=int(s(9) * SS))
    d.ellipse([(hand[0] - s(5)) * SS, (hand[1] - s(5)) * SS,
               (hand[0] + s(5)) * SS, (hand[1] + s(5)) * SS], fill=BODY)
    return hand


def make_player_start():
    n = SIZE * SS
    scale = 2.0
    cx, cy = 128.0, 122.0

    # The body silhouette, on its own layer -- the source for both the outline (its dilated alpha
    # mask, below) and the final light fill composited on top of it.
    body = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bd = ImageDraw.Draw(body)
    hand = figure(bd, cx, cy, scale)

    # The flag: a pole rising from the hand, and a pennant at its top. Its own fill colour, not
    # BODY's, but still part of the silhouette the outline traces -- drawn on the same layer.
    pole_top = (hand[0], hand[1] - 34.0 * scale * 0.5)
    bd.line([(hand[0] * SS, hand[1] * SS), (pole_top[0] * SS, pole_top[1] * SS)],
            fill=BODY, width=int(3.0 * scale * SS))
    bd.polygon([(pole_top[0] * SS, pole_top[1] * SS),
                (pole_top[0] * SS, (pole_top[1] + 16.0 * scale) * SS),
                ((pole_top[0] + 26.0 * scale) * SS, (pole_top[1] + 8.0 * scale) * SS)],
               fill=FLAG)

    # Outline: dilate the body layer's own alpha mask by OUTLINE_PX (in supersampled pixels), then
    # flood that dilated mask with RIM_DARK. MaxFilter needs an odd kernel size (2*radius + 1).
    radius = max(1, int(round(OUTLINE_PX * SS)))
    kernel = radius * 2 + 1
    dilated = body.split()[3].filter(ImageFilter.MaxFilter(kernel))
    outline = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    outline.paste(RIM_DARK, (0, 0), dilated)

    img = Image.alpha_composite(outline, body)
    out = img.resize((SIZE, SIZE), Image.LANCZOS)
    out.save(OUT_PLAYER_START)
    return out


if __name__ == "__main__":
    im = make_player_start()
    print("wrote {} ({}x{})".format(OUT_PLAYER_START, im.size[0], im.size[1]))
