#!/usr/bin/env python3
"""Writes the SoftBodyLab source meshes (Wavefront OBJ, procedural, no third-party assets) into
Content/Source/. Run it from anywhere:  python make_soft_lab.py

OBJ is the glTF convention (right-handed, +Y up, -Z forward, metres); the importer turns that into the
engine's cm / +Z up / +X forward, so OBJ y becomes engine z and OBJ x becomes engine y. Every sheet is a
grid with SHARED vertices (a UV seam would weld in the sim anyway, but there is none to weld here).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "Content", "Source")


def sheet(name, w, h, nx, ny, x0=0.0, y0=0.0):
    """A w x h sheet in the OBJ XY plane (normal +Z), nx x ny quads, lower-left corner at (x0, y0)."""
    v, vt, f = [], [], []
    for j in range(ny + 1):
        for i in range(nx + 1):
            v.append((x0 + w * i / nx, y0 + h * j / ny, 0.0))
            vt.append((i / nx, j / ny))
    idx = lambda i, j: j * (nx + 1) + i + 1
    for j in range(ny):
        for i in range(nx):
            a, b, c, d = idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)
            f.append((a, b, c))
            f.append((a, c, d))
    write(name, v, vt, f)


def box(name, sx, sy, sz, n, cx=0.0, cy=0.0, cz=0.0):
    """An axis-aligned box, n x n quads per face, vertices shared across edges, outward winding."""
    v, vt, f = [], [], []
    seen = {}

    def vert(p):
        key = tuple(round(c, 5) for c in p)
        if key not in seen:
            seen[key] = len(v) + 1
            v.append(p)
            vt.append((0.5 + p[0] / max(sx, 1e-6), 0.5 + p[1] / max(sy, 1e-6)))
        return seen[key]

    hx, hy, hz = sx / 2, sy / 2, sz / 2

    def face(origin, du, dv):
        # du x dv must point outward
        for j in range(n):
            for i in range(n):
                def at(a, b):
                    return vert((cx + origin[0] + du[0] * a / n + dv[0] * b / n,
                                 cy + origin[1] + du[1] * a / n + dv[1] * b / n,
                                 cz + origin[2] + du[2] * a / n + dv[2] * b / n))
                p00, p10, p11, p01 = at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)
                f.append((p00, p10, p11))
                f.append((p00, p11, p01))

    face((-hx, -hy, hz), (sx, 0, 0), (0, sy, 0))      # +Z
    face((hx, -hy, -hz), (-sx, 0, 0), (0, sy, 0))     # -Z
    face((hx, -hy, -hz), (0, sy, 0), (0, 0, sz))      # +X
    face((-hx, -hy, -hz), (0, 0, sz), (0, sy, 0))     # -X
    face((-hx, hy, -hz), (0, 0, sz), (sx, 0, 0))      # +Y
    face((-hx, -hy, -hz), (sx, 0, 0), (0, 0, sz))     # -Y
    write(name, v, vt, f)


def write(name, v, vt, f):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + ".obj"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# SoftBodyLab test mesh (procedural; see make_soft_lab.py)\no %s\n" % name)
        for p in v:
            fh.write("v %.5f %.5f %.5f\n" % p)
        for t in vt:
            fh.write("vt %.5f %.5f\n" % t)
        for a, b, c in f:
            fh.write("f %d/%d %d/%d %d/%d\n" % (a, a, b, b, c, c))
    print("wrote", name, len(v), "vertices", len(f), "triangles")


# Cloth. Pole on the right (max OBJ x), flag hangs to the left.
sheet("Flag", 1.5, 1.0, 24, 16, x0=-1.5, y0=0.0)
sheet("Curtain", 2.0, 2.4, 20, 24, x0=-1.0, y0=0.0)
sheet("Cape", 0.7, 1.0, 12, 16, x0=-0.35, y0=0.0)
# Destructibles. Glass and door are sheets; the crate is a closed box so it breaks into planks.
sheet("GlassPane", 1.2, 1.8, 12, 18, x0=-0.6, y0=0.0)
sheet("SteelDoor", 1.0, 2.1, 10, 21, x0=-1.0, y0=0.0)
# The same door at 12000 triangles (51 x 121 = 6171 welded vertices), over the 4096-particle cap: it is simulated through a cooked
# proxy cage ("Create proxy cage" in Details; docs/SOFTBODY.md "Proxy cage").
sheet("HighPolyDoor", 1.0, 2.1, 50, 120, x0=-1.0, y0=0.0)
box("PlankCrate", 0.8, 0.8, 0.8, 6, cy=0.4)
# Static dressing (no soft body): a pole, a lintel and a mannequin torso.
box("Pole", 0.06, 3.2, 0.06, 1, cy=1.6)
box("Lintel", 2.4, 0.08, 0.08, 1, cy=2.45)
box("Mannequin", 0.5, 0.9, 0.25, 1, cy=1.35)


# ---- wave 11 (docs/plans/wave11/SOFTBODY_HAIR.md): inflatables, jelly, ropes and cables ---------------------------------------------
import json
import math

SOFT = os.path.join(HERE, "Content", "Soft")
MAPS = os.path.join(HERE, "Content", "Maps")


def uv_sphere(name, r, rings, segs, cy=0.0):
    """A CLOSED UV sphere (a single vertex at each pole), shared vertices, outward winding, centre at OBJ (0, cy, 0)."""
    v, vt, f = [(0.0, cy + r, 0.0)], [(0.5, 1.0)], []
    for ri in range(1, rings):
        phi = math.pi * ri / rings
        for si in range(segs):
            th = 2.0 * math.pi * si / segs
            v.append((r * math.sin(phi) * math.cos(th), cy + r * math.cos(phi), r * math.sin(phi) * math.sin(th)))
            vt.append((si / segs, 1.0 - ri / rings))
    v.append((0.0, cy - r, 0.0))
    vt.append((0.5, 0.0))
    south = len(v)
    ring = lambda ri, si: 1 + (ri - 1) * segs + (si % segs) + 1
    for si in range(segs):
        f.append((1, ring(1, si + 1), ring(1, si)))
    for ri in range(1, rings - 1):
        for si in range(segs):
            f.append((ring(ri, si), ring(ri, si + 1), ring(ri + 1, si)))
            f.append((ring(ri, si + 1), ring(ri + 1, si + 1), ring(ri + 1, si)))
    for si in range(segs):
        f.append((south, ring(rings - 1, si), ring(rings - 1, si + 1)))
    write(name, v, vt, f)


def torus(name, major, minor, around, tube, cy=0.0):
    """A closed torus around the OBJ Y axis, shared vertices."""
    v, vt, f = [], [], []
    for i in range(around):
        for j in range(tube):
            u = 2.0 * math.pi * i / around
            w = 2.0 * math.pi * j / tube
            v.append(((major + minor * math.cos(w)) * math.cos(u), cy + minor * math.sin(w), (major + minor * math.cos(w)) * math.sin(u)))
            vt.append((i / around, j / tube))
    idx = lambda i, j: (i % around) * tube + (j % tube) + 1
    for i in range(around):
        for j in range(tube):
            f.append((idx(i, j), idx(i, j + 1), idx(i + 1, j)))
            f.append((idx(i + 1, j), idx(i, j + 1), idx(i + 1, j + 1)))
    write(name, v, vt, f)


def ocsoft(name, doc):
    os.makedirs(SOFT, exist_ok=True)
    with open(os.path.join(SOFT, name + ".ocsoft"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(doc, separators=(",", ":")) + "\n")
    print("wrote", name + ".ocsoft")


# Meshes: a closed ball (a balloon), a closed cube and a closed ring (the jellies).
uv_sphere("Balloon", 0.35, 12, 24, cy=0.35)
box("JellyCube", 0.6, 0.6, 0.6, 4, cy=0.3)
torus("JellyRing", 0.35, 0.12, 24, 12, cy=0.12)

HEAD = {"format": "ocsoft", "version": 1}
# A balloon: a closed cloth with a pressure, hanging from its top vertices so it cannot fall (kPa at rest volume; 3 is a beach ball).
ocsoft("Balloon", dict(HEAD, kind="cloth", preset="cloth_light", cloth={"dragCoeff": 1.0, "liftCoeff": 0.2},
                       inflate={"pressureKPa": 3.0}, paint={"rules": [{"type": "above", "axis": "z", "valueCm": 66.0}]}))
# The same ball on the fast solver with a slow leak: it deflates over about two seconds once the pressure is on.
ocsoft("LeakyBalloon", dict(HEAD, kind="cloth", preset="cloth_light", inflate={"pressureKPa": 4.0, "leakKPaPerS": 2.0},
                            paint={"rules": [{"type": "above", "axis": "z", "valueCm": 66.0}]}))
# A cape-like flag on the fast solver: no dents, no tears, long range attachments hold it to its length.
ocsoft("FastFlag", dict(HEAD, kind="cloth", preset="cloth_light", cloth={"solver": "fast", "bend": "distance", "lra": "geodesic", "lraSlack": 1.02},
                        paint={"rules": [{"type": "above", "axis": "y", "valueCm": -1.0}]}))
ocsoft("JellyCube", dict(HEAD, kind="jelly", preset="jelly", jelly={"cellCm": 10, "snapCm": 3, "volumeCompliance": 0.0002, "edgeCompliance": 0.00005}))
ocsoft("JellyRing", dict(HEAD, kind="jelly", preset="jelly", jelly={"cellCm": 6, "snapCm": 2, "volumeCompliance": 0.0005, "edgeCompliance": 0.0001}))
ocsoft("Rope", dict(HEAD, kind="rope", preset="rope", rope={"radiusCm": 1.2, "segmentCm": 12, "massPerMeterKg": 0.4, "slack": 1.02}))
ocsoft("Cable", dict(HEAD, kind="rope", preset="rope", rope={"radiusCm": 0.8, "segmentCm": 10, "massPerMeterKg": 0.6, "slack": 1.0, "sides": 6, "uvCmPerTile": 12}))

# The level: a row ahead of the spawn along Y, like SoftBodyLab.ocworld. A rope has no mesh of its own: its placement is the engine
# cube, which the published tube replaces; the two ends are world points (a free end: endpin=0).
WORLD = """OCWORLD 1
NAME SoftShapesLab
GROUND 0.0 0
KILLZ -5000.0
SPAWN -100 0 100 0
# Generated by make_soft_lab.py. Import the OBJ files first (README.md step 2); until Meshes/*.ocmesh exist the placements draw nothing.
# Static dressing: a pole and a crane post for the ropes.
PLACE Meshes/Pole.ocmesh 300 -700 -1 0 0 0 1 name RopePole
PLACE Meshes/Pole.ocmesh 300 -200 -1 0 0 0 1 name CranePost
# Inflatable: a ball hanging by its top, 3 kPa. Select it in Play and drag the Pressure slider (or Set Soft Pressure) to deflate it.
PLACE Meshes/Balloon.ocmesh 300 -1000 150 0 0 0 1 nocollide name Balloon
  COMPONENT Cloth asset=Soft/Balloon.ocsoft preset=cloth_light
PLACE Meshes/Balloon.ocmesh 300 -1150 150 0 0 0 1 nocollide name LeakyBalloon
  COMPONENT Cloth asset=Soft/LeakyBalloon.ocsoft preset=cloth_light
# Fast cloth: the flag mesh on Jolt's own constraints.
PLACE Meshes/Flag.ocmesh 300 -850 200 0 0 0 1 nocollide name FastFlag
  COMPONENT Cloth asset=Soft/FastFlag.ocsoft preset=cloth_light
# Jelly: a cube and a ring dropped from 2 m.
PLACE Meshes/JellyCube.ocmesh 300 -500 200 0 0 0 1 nocollide name JellyCube
  COMPONENT Jelly asset=Soft/JellyCube.ocsoft
PLACE Meshes/JellyRing.ocmesh 300 -350 200 0 0 0 1 nocollide name JellyRing
  COMPONENT Jelly asset=Soft/JellyRing.ocsoft
# Ropes: one hanging from a pole with its end free, one between two points that sags, a cable hanging from the crane post.
PLACE Meshes/cube.ocmesh 300 -700 330 0 0 0 1 nocollide name HangingRope
  COMPONENT Rope asset=Soft/Rope.ocsoft start=300,-700,330 end=300,-700,130 length=200 endpin=0
PLACE Meshes/cube.ocmesh 300 0 250 0 0 0 1 nocollide name SaggingRope
  COMPONENT Rope asset=Soft/Rope.ocsoft start=300,100,250 end=300,500,250 length=440
PLACE Meshes/cube.ocmesh 300 -200 320 0 0 0 1 nocollide name CraneCable
  COMPONENT Rope asset=Soft/Cable.ocsoft start=300,-200,320 end=300,-200,60 length=260 endpin=0
"""
os.makedirs(MAPS, exist_ok=True)
with open(os.path.join(MAPS, "SoftShapesLab.ocworld"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write(WORLD)
print("wrote SoftShapesLab.ocworld")
