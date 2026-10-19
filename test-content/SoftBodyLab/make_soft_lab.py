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
