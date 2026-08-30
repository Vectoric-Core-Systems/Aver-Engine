#!/usr/bin/env python3
"""Generates tileable procedural PBR texture sets (diff / arm / nor_gl) as PNGs.

A GENERAL CONTENT TOOL, not a fix for any one project. It takes an output directory and writes there;
it knows nothing about which project, level or material will use the result. Point it at any
project's Content/Textures folder.

WHAT IT IS FOR. A surface with no albedo, normal or roughness VARIATION cannot look real however good
the renderer is -- a flat colour is a flat colour under any lighting model. These sets exist so that
a scene under development has something better than a single constant per surface while real art is
still being made.

These are PROCEDURAL, not photographs, and they are not a substitute for scanned material sets. A
shipping project should replace them.

Output layout follows this engine's .ocmat convention:
    <name>_diff.png    sRGB albedo
    <name>_arm.png     linear: R = ambient occlusion, G = roughness, B = metallic  (glTF ORM order)
    <name>_nor_gl.png  linear tangent-space normal, OpenGL convention (+Y up)

Bind them with, and note reflectance is F0 DIRECTLY in this engine -- 0.04 for any dielectric:
    TEX baseColor  {path:Textures/<dir>/<name>_diff.png}   uv0 sRGB
    TEX metalRough {path:Textures/<dir>/<name>_arm.png}    uv0 linear
    TEX normal     {path:Textures/<dir>/<name>_nor_gl.png} uv0 normal
    TEX occlusion  {path:Textures/<dir>/<name>_arm.png}    uv0 linear

Everything is tileable: the noise lattice wraps, and every filter used is periodic.

    python tools/make_pbr_textures.py <output-dir>
"""

import os
import sys

import numpy as np
from PIL import Image

SIZE = 1024


# ------------------------------------------------------------------ noise, wrapped so it tiles

def _value_noise(period, rng):
    """One octave of value noise on a `period`x`period` lattice, bilinearly upsampled to SIZE.

    The lattice is generated at `period+1` and its last row/column COPIED from the first, which is
    what makes the result seamless: the interpolation at the right edge lands exactly on the value
    the left edge starts from.
    """
    lat = rng.random((period + 1, period + 1), dtype=np.float32)
    lat[-1, :] = lat[0, :]
    lat[:, -1] = lat[:, 0]

    # Bilinear upsample with smoothstep weights -- smoothstep rather than linear so octaves do not
    # show the lattice as visible diamond creases.
    t = np.linspace(0.0, period, SIZE, endpoint=False, dtype=np.float32)
    i0 = np.floor(t).astype(np.int32)
    f = t - i0
    f = f * f * (3.0 - 2.0 * f)
    i1 = i0 + 1

    a = lat[np.ix_(i0, i0)]
    b = lat[np.ix_(i0, i1)]
    c = lat[np.ix_(i1, i0)]
    d = lat[np.ix_(i1, i1)]
    fx = f[None, :]
    fy = f[:, None]
    return (a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy)


def fbm(octaves, base_period, rng, gain=0.5):
    """Fractional Brownian motion: octaves of value noise at doubling frequency, halving amplitude."""
    out = np.zeros((SIZE, SIZE), dtype=np.float32)
    amp, period, norm = 1.0, base_period, 0.0
    for _ in range(octaves):
        out += _value_noise(period, rng) * amp
        norm += amp
        amp *= gain
        period *= 2
    return out / norm


def normalize01(a):
    lo, hi = float(a.min()), float(a.max())
    return (a - lo) / (hi - lo) if hi > lo else np.zeros_like(a)


# ------------------------------------------------------------------ height -> normal / occlusion

def height_to_normal(height, strength):
    """Tangent-space normal from a height field, OpenGL convention (+Y up), wrapped at the edges.

    np.roll is what keeps this tileable: the gradient at column 0 is taken against the LAST column,
    not against a clamped copy of itself, so the normals agree across the seam.
    """
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * strength
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * strength
    nx, ny, nz = -dx, dy, np.ones_like(height)     # +Y up: do NOT negate dy
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.stack([nx * inv, ny * inv, nz * inv], axis=-1) * 0.5 + 0.5


def cavity_ao(height, radius=6):
    """Cheap ambient occlusion: how far below its local neighbourhood a texel sits.

    Not a ray-traced AO -- it is a blurred-height comparison, which captures the crevices (grout
    lines, plank gaps) that are the only occlusion a flat tiling surface actually has.
    """
    blur = height.copy()
    for _ in range(3):
        blur = (blur
                + np.roll(blur, radius, 0) + np.roll(blur, -radius, 0)
                + np.roll(blur, radius, 1) + np.roll(blur, -radius, 1)) / 5.0
    return np.clip(1.0 - (blur - height) * 6.0, 0.35, 1.0)


def save_set(out_dir, name, albedo, height, rough, metal, normal_strength):
    os.makedirs(out_dir, exist_ok=True)
    Image.fromarray((np.clip(albedo, 0, 1) * 255).astype(np.uint8), "RGB").save(
        os.path.join(out_dir, f"{name}_diff.png"))

    ao = cavity_ao(height)
    arm = np.stack([ao, np.clip(rough, 0.03, 1.0), np.clip(metal, 0.0, 1.0)], axis=-1)
    Image.fromarray((arm * 255).astype(np.uint8), "RGB").save(
        os.path.join(out_dir, f"{name}_arm.png"))

    nrm = height_to_normal(height, normal_strength)
    Image.fromarray((nrm * 255).astype(np.uint8), "RGB").save(
        os.path.join(out_dir, f"{name}_nor_gl.png"))
    print(f"  {name}: diff / arm / nor_gl written ({SIZE}x{SIZE})")


# ------------------------------------------------------------------ the three surfaces

def make_floor_tiles(out_dir, rng):
    """Large pale floor tiles with grout lines -- the deck."""
    tiles = 4                                   # tiles across the texture
    u = (np.arange(SIZE, dtype=np.float32) / SIZE * tiles) % 1.0
    gx, gy = np.meshgrid(u, u)
    # Grout: a narrow band at each tile edge, smooth so it survives mipping.
    edge = np.minimum(np.minimum(gx, 1 - gx), np.minimum(gy, 1 - gy))
    grout = 1.0 - np.clip(edge / 0.02, 0, 1)

    # Per-tile tone variation, so the tiles are not clones of each other.
    tile_id = (np.floor(np.meshgrid(np.arange(SIZE) / SIZE * tiles,
                                    np.arange(SIZE) / SIZE * tiles)[0])
               + np.floor(np.meshgrid(np.arange(SIZE) / SIZE * tiles,
                                      np.arange(SIZE) / SIZE * tiles)[1]) * tiles)
    tone = (np.sin(tile_id * 12.9898) * 43758.5453) % 1.0
    tone = 0.92 + tone * 0.16

    grain = fbm(5, 8, rng)
    speck = normalize01(fbm(3, 128, rng))

    base = np.array([0.40, 0.385, 0.365], dtype=np.float32)   # light stone tile, ~0.40 albedo
    shade = (0.88 + grain * 0.24) * tone
    albedo = base[None, None, :] * shade[..., None]
    albedo += (speck[..., None] - 0.5) * 0.05
    albedo = albedo * (1.0 - grout[..., None] * 0.45)          # grout is darker

    height = grain * 0.35 - grout * 1.0                        # grout sits BELOW the tile face
    rough = 0.42 + grain * 0.22 + grout * 0.30                 # grout is rougher than the tile
    save_set(out_dir, "floor_tile", albedo, height, rough, np.zeros_like(rough), 2.2)


def make_concrete(out_dir, rng):
    """Poured concrete -- the pit floor and walls."""
    coarse = fbm(6, 4, rng)
    fine = fbm(4, 64, rng)
    pit = normalize01(fbm(3, 200, rng))
    pits = np.clip((pit - 0.72) / 0.28, 0, 1)                  # sparse little air holes

    base = np.array([0.30, 0.295, 0.285], dtype=np.float32)   # poured concrete, ~0.30 albedo
    shade = 0.82 + coarse * 0.30 + fine * 0.10
    albedo = base[None, None, :] * shade[..., None]
    albedo -= pits[..., None] * 0.10

    height = coarse * 0.5 + fine * 0.25 - pits * 1.2
    rough = 0.68 + fine * 0.20 - pits * 0.10                   # concrete is rough; pits a touch less
    save_set(out_dir, "concrete", albedo, height, rough, np.zeros_like(rough), 1.6)


def make_crate(out_dir, rng):
    """Planked wooden crate."""
    planks = 5
    v = (np.arange(SIZE, dtype=np.float32) / SIZE * planks) % 1.0
    _, py = np.meshgrid(v, v)
    gap = 1.0 - np.clip(np.minimum(py, 1 - py) / 0.015, 0, 1)

    # Grain runs ALONG the plank: sample noise on a lattice stretched 12:1 so the features are long
    # and thin rather than blobby, which is what reads as wood.
    grain_fine = _value_noise(256, rng)
    grain_long = _value_noise(16, rng)
    grain = (grain_fine * 0.35 + grain_long * 0.65)
    grain = np.roll(grain, 0, axis=0)
    grain = 0.5 + (grain - 0.5) * 1.6

    plank_id = np.floor(np.meshgrid(v, v)[1] * 0 + np.floor(np.arange(SIZE)[:, None] / SIZE * planks))
    tone = ((np.sin(plank_id * 7.233) * 21713.0) % 1.0) * 0.18 + 0.90

    base = np.array([0.42, 0.29, 0.17], dtype=np.float32)     # bare pine, ~0.35 luminance
    shade = (0.80 + grain * 0.34) * tone
    albedo = base[None, None, :] * shade[..., None]
    albedo *= (1.0 - gap[..., None] * 0.55)

    height = grain * 0.4 - gap * 1.0
    rough = 0.55 + grain * 0.25 + gap * 0.20
    save_set(out_dir, "crate_wood", albedo, height, rough, np.zeros_like(rough), 2.0)



def make_glass(out_dir, rng):
    """Float glass -- the smudges and waviness that are the only things a clear surface HAS.

    A TRANSPARENT SURFACE HAS ALMOST NO ALBEDO TO TEXTURE, which is why this set looks so unlike the
    three above it. What makes rendered glass read as real is ROUGHNESS VARIATION -- fingerprints,
    wipe streaks, settled dust -- because that is what breaks a mirror-perfect reflection into
    something an eye believes. The diffuse map here is therefore near-white on purpose: glass gets
    its colour from VOLUME ABSORPTION, which depends on how far the light travelled through the
    pane and so cannot be painted into a texture at all. Put the tint in attenuationColor, not here.

    THE ROUGHNESS IS ABSOLUTE, not a multiplier. Bind this with `PARAM roughnessFactor 1.0` and let
    the green channel carry the real value: about 0.03 where the pane is clean, rising toward 0.33
    in a smudge. Authoring a low roughnessFactor as well would multiply the two and flatten every
    smudge back out, which is the mistake this note exists to prevent.
    """
    # THE ROLLER WAVE. Float glass is drawn over rollers while still soft and keeps a very long,
    # very shallow ripple along one axis -- it is why a reflection in a shopfront swims slightly as
    # you walk past. Three full cycles across the tile, so it wraps.
    y = np.linspace(0.0, 2.0 * np.pi, SIZE, endpoint=False, dtype=np.float32)
    roller = np.repeat(np.sin(y * 3.0)[:, None], SIZE, axis=1)

    smear = fbm(4, 6, rng) - 0.5            # broad wipe marks, centred so they do not bias height
    dust = fbm(5, 90, rng) - 0.5            # fine settled speckle
    prints = normalize01(fbm(3, 14, rng))
    smudge = np.clip((prints - 0.62) / 0.38, 0, 1)      # sparse fingerprint patches

    albedo = np.full((SIZE, SIZE, 3), 0.98, dtype=np.float32)
    albedo -= (smudge * 0.04)[..., None]

    # TINY. This is waviness, not relief -- the amplitudes here are a hundredth of the crate's, and
    # normal_strength below is a sixth of concrete's, because a glass pane that shows surface relief
    # stops looking like glass immediately.
    height = roller * 0.030 + smear * 0.010 + dust * 0.004

    rough = 0.03 + smudge * 0.30 + np.clip(dust + 0.5, 0, 1) * 0.03
    save_set(out_dir, "glass", albedo, height, rough, np.zeros_like(rough), 0.25)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out_dir = sys.argv[1]
    rng = np.random.default_rng(20260829)       # fixed: regenerating must not change the look
    print(f"generating tileable PBR sets into {out_dir}")
    make_floor_tiles(out_dir, rng)
    make_concrete(out_dir, rng)
    make_crate(out_dir, rng)
    make_glass(out_dir, rng)
    return 0


if __name__ == "__main__":
    sys.exit(main())
