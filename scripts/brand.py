"""Derive the Aver Engine brand slots from the human-authored master artwork.

The master is a raster lockup on a dark banner with white space to the right and below. Every slot
below is a crop or a rescale of it -- nothing is redrawn, so the shipped marks are the author's.
"""
from PIL import Image, ImageDraw
import os

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(_ROOT, "branding", "master-lockup.png")
OUT = os.path.join(_ROOT, "branding")
BG = (38, 38, 38)            # the banner's own colour, sampled from the master
CUBE = (210, 184, 626, 615)  # measured content bounds of the isocube
LOCKUP_TOP, LOCKUP_BOT = 120, 680

im = Image.open(SRC).convert("RGB")

# ---- the lockup: cube + wordmark, trimmed off the master's white margin ----
# Right edge found by scanning for the last non-background column inside the banner, so the crop
# follows the artwork rather than a hardcoded guess.
px = im.load()
def is_bg(c, t=18):
    return abs(c[0]-BG[0]) <= t and abs(c[1]-BG[1]) <= t and abs(c[2]-BG[2]) <= t

right = CUBE[2]
for x in range(1740, CUBE[2], -1):
    if any(not is_bg(px[x, y]) for y in range(LOCKUP_TOP, LOCKUP_BOT, 3)):
        right = x
        break
print("lockup right edge:", right)

pad = 70
lock = im.crop((max(0, CUBE[0]-pad), max(0, LOCKUP_TOP-pad),
                min(1746, right+pad), min(695, LOCKUP_BOT+pad)))
print("lockup crop:", lock.size)

# splash.png keeps its existing 1200x520 slot: the runtime shows it as a layered window at native
# size, so changing the dimensions would change the splash's on-screen size.
splash = Image.new("RGB", (1200, 520), BG)
s = lock.copy()
s.thumbnail((1200 - 80, 520 - 80), Image.LANCZOS)
splash.paste(s, ((1200 - s.width)//2, (520 - s.height)//2))
splash.save(os.path.join(OUT, "splash.png"))
print("splash.png", splash.size)

# ---- the mark on its own, background keyed out ----
# Flood-filled from the corners rather than colour-replaced globally: the cube's own dark outline is
# close to the bander colour, and a global replace would punch holes through it.
cube = im.crop(CUBE).convert("RGB")
KEY = (255, 0, 255)
work = cube.copy()
for corner in [(0, 0), (work.width-1, 0), (0, work.height-1), (work.width-1, work.height-1)]:
    ImageDraw.floodfill(work, corner, KEY, thresh=30)

rgba = cube.convert("RGBA")
wp, rp = work.load(), rgba.load()
for y in range(rgba.height):
    for x in range(rgba.width):
        if wp[x, y] == KEY:
            rp[x, y] = (0, 0, 0, 0)

# Square canvas with a little breathing room, so the mark is not flush to the icon edge.
side = max(rgba.size) + 24
mark = Image.new("RGBA", (side, side), (0, 0, 0, 0))
mark.paste(rgba, ((side - rgba.width)//2, (side - rgba.height)//2), rgba)

for name, px_size in (("logo.png", 512), ("icon512.png", 512), ("logo256.png", 256)):
    mark.resize((px_size, px_size), Image.LANCZOS).save(os.path.join(OUT, name))
    print(name, px_size)

# Windows picks the nearest size from the .ico; supplying the full ladder avoids the shell rescaling
# 256 down to 16 and turning the mark to mush in the taskbar.
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
mark.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "icon.ico"), sizes=sizes)
print("icon.ico", sizes)
