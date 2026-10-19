"""Procedural geometry and sound for the Vehicle template and the VehicleLab (stdlib only, no third-party assets).

Shapes are authored in ENGINE axes (centimetres, +X forward, +Y right, +Z up, origin at the bottom centre of the vehicle) and
written as Wavefront OBJ in the glTF convention the importer expects (metres, +Y up, -Z forward, right-handed), so
`AverAssetC convert x.obj --out-dir ... --base x` gives an .ocmesh that lines up with the physics:

    engine (x, y, z) cm  ->  obj (y, z, -x) / 100

Triangle winding is decided per face from the outward normal the shape knows, in OBJ space, so it survives the handedness flip.

This module is imported by make_vehicle_template.py (here) and by test-content/VehicleLab/make_vehicle_lab.py; it is not copied
into projects (a template copies only Content/).
"""
import math
import os
import wave
import struct


# ---------------------------------------------------------------------------------------------------------------- vectors

def to_obj(p):
    return (p[1] / 100.0, p[2] / 100.0, -p[0] / 100.0)


def dir_to_obj(n):
    return (n[1], n[2], -n[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def unit(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 0.0, 1.0)


def centroid(points):
    n = float(len(points))
    return (sum(p[0] for p in points) / n, sum(p[1] for p in points) / n, sum(p[2] for p in points) / n)


# ---------------------------------------------------------------------------------------------------------------- meshes

class Mesh:
    """Accumulates flat- and smooth-shaded faces in OBJ space. Every shape method takes ENGINE-space centimetres."""

    def __init__(self, name):
        self.name = name
        self.verts = []
        self.norms = []
        self.uvs = []
        self.tris = []

    def _vertex(self, p_eng, n_eng, uv):
        self.verts.append(to_obj(p_eng))
        self.norms.append(dir_to_obj(unit(n_eng)))
        self.uvs.append(uv)
        return len(self.verts) - 1

    def _tri(self, a, b, c, n_eng):
        pa, pb, pc = self.verts[a], self.verts[b], self.verts[c]
        if dot(cross(sub(pb, pa), sub(pc, pa)), dir_to_obj(n_eng)) < 0.0:
            b, c = c, b
        self.tris.append((a, b, c))

    @staticmethod
    def _uv(p, n):
        ax = max(range(3), key=lambda i: abs(n[i]))
        u, v = [i for i in range(3) if i != ax]
        return (0.5 + p[u] / 400.0, 0.5 + p[v] / 400.0)

    def polygon(self, pts, solid_center):
        """A convex planar polygon, flat shaded, facing away from `solid_center`."""
        n = unit(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])))
        if dot(n, sub(centroid(pts), solid_center)) < 0.0:
            n = (-n[0], -n[1], -n[2])
        idx = [self._vertex(p, n, self._uv(p, n)) for p in pts]
        for i in range(1, len(idx) - 1):
            self._tri(idx[0], idx[i], idx[i + 1], n)

    def box(self, cx, cy, cz, hx, hy, hz):
        c = (cx, cy, cz)
        p = [(cx + sx * hx, cy + sy * hy, cz + sz * hz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        # index = (sx > 0) * 4 + (sy > 0) * 2 + (sz > 0)
        faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        for f in faces:
            self.polygon([p[i] for i in f], c)

    def frustum(self, cx, cy, cz, bottom_hx, bottom_hy, top_hx, top_hy, hz, top_dx=0.0):
        """A box that tapers: `bottom_*` half extents at z - hz, `top_*` at z + hz (the top shifted by `top_dx` along X)."""
        c = (cx, cy, cz)
        b = [(cx - bottom_hx, cy - bottom_hy, cz - hz), (cx + bottom_hx, cy - bottom_hy, cz - hz),
             (cx + bottom_hx, cy + bottom_hy, cz - hz), (cx - bottom_hx, cy + bottom_hy, cz - hz)]
        t = [(cx + top_dx - top_hx, cy - top_hy, cz + hz), (cx + top_dx + top_hx, cy - top_hy, cz + hz),
             (cx + top_dx + top_hx, cy + top_hy, cz + hz), (cx + top_dx - top_hx, cy + top_hy, cz + hz)]
        self.polygon(b, c)
        self.polygon(t, c)
        for i in range(4):
            j = (i + 1) % 4
            self.polygon([b[i], b[j], t[j], t[i]], c)

    def cylinder(self, axis, cx, cy, cz, radius, half, segments=24, caps=True, radius_end=None):
        """A cylinder along `axis` ('x', 'y' or 'z') with smooth sides. `radius_end` makes a cone frustum (end at +axis)."""
        a = {"x": 0, "y": 1, "z": 2}[axis]
        u, v = [i for i in range(3) if i != a]
        center = (cx, cy, cz)
        r0 = radius
        r1 = radius if radius_end is None else radius_end

        def point(angle, along, r):
            p = [cx, cy, cz]
            p[a] += along
            p[u] += math.cos(angle) * r
            p[v] += math.sin(angle) * r
            return (p[0], p[1], p[2])

        def radial(angle):
            n = [0.0, 0.0, 0.0]
            n[u] = math.cos(angle)
            n[v] = math.sin(angle)
            return (n[0], n[1], n[2])

        ring0, ring1 = [], []
        for i in range(segments):
            ang = 2.0 * math.pi * i / segments
            n = radial(ang)
            ring0.append(self._vertex(point(ang, -half, r0), n, (i / segments, 0.0)))
            ring1.append(self._vertex(point(ang, half, r1), n, (i / segments, 1.0)))
        for i in range(segments):
            j = (i + 1) % segments
            mid = radial(2.0 * math.pi * (i + 0.5) / segments)
            self._tri(ring0[i], ring0[j], ring1[j], mid)
            self._tri(ring0[i], ring1[j], ring1[i], mid)
        if caps:
            for sign, r in ((-1.0, r0), (1.0, r1)):
                if r <= 0.0:
                    continue
                n = [0.0, 0.0, 0.0]
                n[a] = sign
                pts = [point(2.0 * math.pi * i / segments, sign * half, r) for i in range(segments)]
                idx = [self._vertex(p, tuple(n), self._uv(p, tuple(n))) for p in pts]
                for i in range(1, segments - 1):
                    self._tri(idx[0], idx[i], idx[i + 1], tuple(n))

    def write_obj(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# %s: procedural, no third-party assets (vehicle_geo.py)\no %s\n" % (self.name, self.name))
            for p in self.verts:
                fh.write("v %.5f %.5f %.5f\n" % p)
            for t in self.uvs:
                fh.write("vt %.5f %.5f\n" % t)
            for n in self.norms:
                fh.write("vn %.5f %.5f %.5f\n" % n)
            for a, b, c in self.tris:
                fh.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a + 1, a + 1, a + 1, b + 1, b + 1, b + 1, c + 1, c + 1, c + 1))
        print("wrote %s: %d vertices, %d triangles" % (path, len(self.verts), len(self.tris)))

    def bounds_eng(self):
        """The mesh's bounds back in engine centimetres (x, y, z) -> (min, max), for a sanity print."""
        xs = [-v[2] * 100.0 for v in self.verts]
        ys = [v[0] * 100.0 for v in self.verts]
        zs = [v[1] * 100.0 for v in self.verts]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


# ---------------------------------------------------------------------------------------------------------------- parts

def wheel_mesh(name, radius, width, segments=28):
    """A wheel with its axle on engine Y, forward on X and its origin at the hub, with spokes so that spin is visible."""
    m = Mesh(name)
    m.cylinder("y", 0, 0, 0, radius, width * 0.5, segments)                              # the tyre
    m.cylinder("y", 0, 0, 0, radius * 0.62, width * 0.5 + 0.5, segments)                 # the rim face, proud of the tyre
    for side in (-1.0, 1.0):
        y = side * (width * 0.5 + 1.0)
        m.box(0, y, 0, radius * 0.56, 0.5, 3.0)                                          # spokes
        m.box(0, y, 0, 3.0, 0.5, radius * 0.56)
        m.box(radius * 0.40, y, radius * 0.40, 1.0, 0.7, 5.0)
    return m


def sedan_body(name="Sedan"):
    """A boxy saloon: narrow hull so the wheels show at the sides, a tapered cabin. 440 x 180 x 100 cm."""
    m = Mesh(name)
    m.box(0, 0, 49, 214, 62, 21)                                    # hull, z 28..70
    m.box(0, 0, 36, 218, 80, 6)                                     # sill / bumper line
    m.frustum(-10, 0, 85, 90, 64, 62, 54, 17, top_dx=-6)            # cabin, z 68..102
    m.box(228, 0, 45, 6, 74, 14)                                    # front bumper
    m.box(-228, 0, 45, 6, 74, 14)                                   # rear bumper
    m.box(150, 0, 74, 62, 56, 4)                                    # bonnet plate
    return m


def hatchback_body(name="Hatchback"):
    m = Mesh(name)
    m.box(0, 0, 47, 182, 58, 19)
    m.box(0, 0, 35, 186, 74, 5)
    m.frustum(-20, 0, 80, 88, 60, 68, 52, 16, top_dx=-14)
    m.box(194, 0, 44, 5, 68, 12)
    m.box(-194, 0, 44, 5, 68, 12)
    return m


def truck_body(name="Truck"):
    """A 6x4 lorry: cab over the front axle, a long bed over the two rear axles. About 800 x 240 x 330 cm."""
    m = Mesh(name)
    m.box(0, 0, 70, 390, 90, 12)                                    # chassis rails, z 58..82
    m.box(250, 0, 150, 120, 116, 60)                                # cab, z 90..210
    m.frustum(255, 0, 235, 112, 108, 96, 92, 25)                    # roof
    m.box(-80, 0, 135, 270, 118, 50)                                # bed floor + sides, z 85..185
    m.box(-80, 0, 215, 268, 4, 30)                                  # bed side boards
    m.box(-80, 114, 215, 268, 4, 30)
    m.box(-80, -114, 215, 268, 4, 30)
    m.box(375, 0, 110, 8, 108, 24)                                  # front bumper
    return m


def tank_hull(name="TankHull"):
    m = Mesh(name)
    m.box(0, 0, 66, 330, 100, 36)                                   # lower hull, z 30..102
    m.box(0, 0, 118, 300, 142, 14)                                  # superstructure over the tracks, z 104..132
    m.frustum(318, 0, 96, 20, 100, 8, 100, 18, top_dx=-12)          # glacis plate in front of the hull
    for side in (-1.0, 1.0):
        m.box(0, side * 130, 82, 318, 25, 3)                        # track belt, top run, z 79..85
        m.box(0, side * 130, 3, 318, 25, 3)                         # track belt, bottom run, z 0..6
    return m


def tank_turret(name="TankTurret"):
    """Origin at the centre of the turret ring, on the hull deck."""
    m = Mesh(name)
    m.cylinder("z", 0, 0, 22, 95, 22, 28)                           # turret ring, z 0..44
    m.box(10, 0, 38, 110, 84, 18)                                   # turret body
    m.box(120, 0, 36, 14, 44, 26)                                   # mantlet
    m.cylinder("x", 250, 0, 38, 9, 130, 16)                         # barrel
    m.cylinder("x", 392, 0, 38, 14, 12, 16)                         # muzzle brake
    return m


# ---------------------------------------------------------------------------------------------------------------- sound

RATE = 44100


def write_wav(path, samples, rate=RATE):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = 0.8 / peak
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * gain)) * 32767)) for s in samples))
    print("wrote %s: %.2f s" % (path, len(samples) / float(rate)))


def engine_loop(fundamental, seconds=2.0, harmonics=7, wobble_cycles=1, rough=0.0):
    """A looping engine tone: a fundamental and its harmonics with a slow amplitude wobble. `fundamental * seconds` and
    `wobble_cycles` are whole numbers, so every partial completes an integer number of cycles and the loop is seamless."""
    n = int(RATE * seconds)
    out = []
    for i in range(n):
        t = i / float(RATE)
        s = 0.0
        for k in range(1, harmonics + 1):
            s += math.sin(2.0 * math.pi * k * fundamental * t + 0.7 * k) / (k ** 1.15)
        wobble = 1.0 + 0.12 * math.sin(2.0 * math.pi * wobble_cycles * t / seconds)
        # a deterministic "firing" ripple at the cylinder rate
        ripple = 1.0 + rough * math.sin(2.0 * math.pi * (fundamental * 0.5) * t)
        out.append(s * wobble * ripple)
    return out


def _lcg(seed):
    state = seed & 0xFFFFFFFF
    while True:
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        yield (state / 2147483648.0) - 1.0


def skid_loop(seconds=1.5):
    """Band-limited noise with a slow swell, with the ends crossfaded so it loops without a click."""
    rng = _lcg(7)
    n = int(RATE * seconds)
    raw = []
    lp1 = lp2 = 0.0
    for _ in range(n):
        x = next(rng)
        lp1 += 0.35 * (x - lp1)
        lp2 += 0.12 * (lp1 - lp2)
        raw.append(lp1 - lp2)
    fade = int(RATE * 0.12)
    out = raw[:]
    for i in range(fade):
        t = i / float(fade)
        out[i] = raw[i] * t + raw[n - fade + i] * (1.0 - t)
    out = out[:n - fade]
    m = len(out)
    return [s * (0.8 + 0.2 * math.sin(2.0 * math.pi * 2.0 * i / m)) for i, s in enumerate(out)]


def crash_hit(seconds=0.9):
    rng = _lcg(21)
    n = int(RATE * seconds)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / float(RATE)
        f = 40.0 + 80.0 * math.exp(-t * 14.0)
        phase += 2.0 * math.pi * f / RATE
        thud = math.sin(phase) * math.exp(-t * 5.5)
        crunch = next(rng) * math.exp(-t * 11.0) * 0.8
        out.append(thud + crunch)
    return out
