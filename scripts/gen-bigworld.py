#!/usr/bin/env python3
"""Generates test-content/BigWorldLab: the content the 100 km flight and the chunk cook need (LARGE_WORLD 2.4.9).

NeonDistrict is 16 km square and sits inside region 0, so no authored level exercises a floating-origin rebase. BigWorldLab is a road along
+-100 km of X (2,000 placements a kilometre by default: lamp posts and block buildings of the built-in cube, no art), with the cases that
break a chunked, origin-shifted world laid on top of it:

  * items straddling every region seam in range and chunk boundaries at NEGATIVE coordinates (exactly on the seam, a quarter centimetre to
    either side), because floor() on a negative coordinate is where the first version of every one of these systems was wrong
  * a crate pile and an animated, hinged door every 5 km
  * a spinning animated prop beside the spawn
  * one placement carrying a COMPONENT (a static CollisionShape)
  * a 500 m bridge: its reach from its own position is far past the wide threshold, so the cook lists it in the index and the streamer keeps
    its owner chunk resident while the bridge is near
  * a level-global graph-class placement (AN_BigBeacon): class placements are never chunk-owned
  * an infinite PCG field for scatter, so the chunk generator is exercised at 100 km and beyond
  * a few skyline buildings that are wide on their own

Only built-in meshes and surfaces are used. The level is generated, not committed: it is about 30 MB of text. The small assets the level
refers to (two object clips, a collision shape, the beacon graph) are written too. Everything is deterministic (a fixed LCG): two runs
give identical files.

    python scripts/gen-bigworld.py                         # test-content/BigWorldLab, 200 km, 2000 per km
    python scripts/gen-bigworld.py --km 10 --per-km 500    # a smaller slice (+-10 km)
    python scripts/gen-bigworld.py --assets-only           # only the small files (clips, shape, graph, manifest)

Then cook it (the level gains `chunks=` and the set lands in Binaries/Chunks/BigWorldLab):

    AverAssetC cook-chunks test-content/BigWorldLab/Content/Maps/BigWorldLab.ocworld --content-dir test-content/BigWorldLab/Content

Units are centimetres, +X forward at yaw 0, +Y right, +Z up. A PLACEG's last three numbers are HALF extents of the unit cube.
"""
import argparse
import math
import os
import struct
import sys

REGION_STEP = 1024 * 1600          # one region: 1,638,400 cm = 16.384 km
CHUNK = 1600
KM = 100000                        # 1 km in cm

# ---------------------------------------------------------------------------------------------------------------- number formatting


def num(v):
    """Shortest text that reads back as the same double (the writer's rule), never an exponent for the values used here."""
    if v == int(v):
        return str(int(v))
    s = repr(float(v))
    return s


def q(v):
    """Quantised to a quarter of a centimetre, so every position is exact in f32 and f64 and the chunked == flat comparison is bit for bit."""
    return round(v * 4.0) / 4.0


class Rng:
    """A tiny deterministic generator (the same LCG the other lab scripts use)."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state / 4294967296.0

    def range(self, lo, hi):
        return lo + (hi - lo) * self.next()

    def pick(self, seq):
        return seq[int(self.next() * len(seq)) % len(seq)]


# ---------------------------------------------------------------------------------------------------------------- AVR1 (for the two clips)


def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


_M64 = (1 << 64) - 1
_P1, _P2, _P3, _P4, _P5 = 11400714785074694791, 14029467366897019727, 1609587929392839161, 9650029242287828579, 2870177450012600261


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & _M64


def _round(acc, v):
    acc = (acc + v * _P2) & _M64
    acc = _rotl(acc, 31)
    return (acc * _P1) & _M64


def _merge(acc, v):
    acc ^= _round(0, v)
    return (acc * _P1 + _P4) & _M64


def xxh64(data):
    """xxHash64, seed 0 (avrHash64 in modules/formats/src/Avr1.cpp)."""
    n = len(data)
    p = 0
    if n >= 32:
        v1 = (_P1 + _P2) & _M64
        v2 = _P2
        v3 = 0
        v4 = (-_P1) & _M64
        while p <= n - 32:
            v1 = _round(v1, struct.unpack_from('<Q', data, p)[0]); p += 8
            v2 = _round(v2, struct.unpack_from('<Q', data, p)[0]); p += 8
            v3 = _round(v3, struct.unpack_from('<Q', data, p)[0]); p += 8
            v4 = _round(v4, struct.unpack_from('<Q', data, p)[0]); p += 8
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _M64
        h = _merge(h, v1)
        h = _merge(h, v2)
        h = _merge(h, v3)
        h = _merge(h, v4)
    else:
        h = _P5
    h = (h + n) & _M64
    while p + 8 <= n:
        h ^= _round(0, struct.unpack_from('<Q', data, p)[0])
        h = (_rotl(h, 27) * _P1 + _P4) & _M64
        p += 8
    if p + 4 <= n:
        h ^= (struct.unpack_from('<I', data, p)[0] * _P1) & _M64
        h = (_rotl(h, 23) * _P2 + _P3) & _M64
        p += 4
    while p < n:
        h ^= (data[p] * _P5) & _M64
        h = (_rotl(h, 11) * _P1) & _M64
        p += 1
    h ^= h >> 33
    h = (h * _P2) & _M64
    h ^= h >> 29
    h = (h * _P3) & _M64
    h ^= h >> 32
    return h


def fourcc(s):
    return s.encode('ascii')


def avr1(subtype, chunks, flags):
    """chunks: [(fourcc str, version, chunk flags, bytes)]. Layout and checks follow writeAvr1 (modules/formats/src/Avr1.cpp)."""
    out = bytearray(64 + 40 * len(chunks))
    offsets = []
    for _cid, _ver, _cf, data in chunks:
        while len(out) % 16:
            out.append(0)
        offsets.append(len(out))
        out.extend(data)
    directory = bytearray()
    for i, (cid, ver, cf, data) in enumerate(chunks):
        directory += struct.pack('<4sHBBQQQQ', fourcc(cid), ver, 0, cf, offsets[i], len(data), len(data), xxh64(bytes(data)))
    out[64:64 + len(directory)] = directory
    header = struct.pack('<4s4sHHHBBIIQQ16sI', b'AVR1', fourcc(subtype), 1, 1, 1, 0, 4, 64, len(chunks), 64, len(out), bytes(16), flags)
    out[0:60] = header
    out[60:64] = struct.pack('<I', crc32c(bytes(out[0:60])))
    return bytes(out)


def object_clip(duration, keys, loop=True):
    """An OBJECT clip (kOcAnimObject): one transform track on bone 0, translation + rotation, linear. keys: [(t, (x, y, z), (qx, qy, qz, qw))]."""
    times = b''.join(struct.pack('<f', k[0]) for k in keys)
    values = b''.join(struct.pack('<3f4f', *k[1], *k[2]) for k in keys)
    trks = times + values
    flags = (1 if loop else 0) | (1 << 3)                         # kOcAnimLoop | kOcAnimObject
    ahdr = struct.pack('<fIBBHI', duration, 1, 0, flags, 0, 0)    # duration, tracks, Keyframed, flags, sampleRate, skeletonRef (the empty string)
    ahdr += struct.pack('<HBBQII', 0, 3, 0, 0, len(keys), 0)      # bone 0, translation|rotation, Linear, key offset 0, key count, reserved
    strt = b'\x00\x00'                                            # the string table: just the empty string
    return avr1('ANIM', [('AHDR', 1, 1, ahdr), ('TRKS', 1, 0, trks), ('STRT', 1, 0, strt)], 1)


def quat_z(deg):
    h = math.radians(deg) * 0.5
    return (0.0, 0.0, math.sin(h), math.cos(h))


def spin_clip():
    keys = [(0.5 * i, (0.0, 0.0, 0.0), quat_z(45.0 * i)) for i in range(9)]   # a full turn in 4 s
    return object_clip(4.0, keys)


def door_clip(half_width):
    """The leaf swings open to 100 degrees and back about its hinge edge. The object's origin is its centre, half_width from the hinge, so the
    track moves the centre along the hinge circle: A(t) = T(w cos a, w sin a) R(a), and the engine plays the motion relative to A(t0)."""
    angles = [0.0, 50.0, 100.0, 100.0, 100.0, 50.0, 0.0, 0.0, 0.0]
    keys = []
    for i, a in enumerate(angles):
        r = math.radians(a)
        keys.append((0.5 * i, (half_width * math.cos(r), half_width * math.sin(r), 0.0), quat_z(a)))
    return object_clip(4.0, keys)


# ---------------------------------------------------------------------------------------------------------------- level


class Level:
    def __init__(self):
        self.lines = []
        self.count = 0

    def add(self, text):
        self.lines.append(text)

    def place(self, x, y, z, half, mat, yaw=0.0, name=None, collide=True, extra=''):
        """A cube with the same half extent on all three axes (PLACE: its last number is a uniform scale)."""
        s = 'PLACE Meshes/cube.ocmesh %s %s %s %s 0 0 %s %s' % (num(q(x)), num(q(y)), num(q(z)), num(yaw), num(half), mat)
        self.add(self._tail(s, collide, name, extra))
        self.count += 1

    def placeg(self, x, y, z, hx, hy, hz, mat, yaw=0.0, name=None, collide=True, extra=''):
        s = 'PLACEG Meshes/cube.ocmesh %s %s %s %s 0 0 %s %s %s %s' % (
            num(q(x)), num(q(y)), num(q(z)), num(yaw), num(round(hx, 2)), num(round(hy, 2)), num(round(hz, 2)), mat)
        self.add(self._tail(s, collide, name, extra))
        self.count += 1

    def child(self, parent, off, half, mat, collide=True):
        """A CHILDG under a PLACEG whose scale is `parent` (its half extents). A child's numbers are in the parent's SCALED space: the engine
        multiplies the offset and the scale by the parent's scale, so a world offset `off` and half extent `half` are divided by it here."""
        px, py, pz = parent
        lx, ly, lz = off[0] / px, off[1] / py, off[2] / pz
        sx, sy, sz = half[0] / px, half[1] / py, half[2] / pz
        self.add('    CHILDG Meshes/cube.ocmesh %s %s %s 0 0 0 %s %s %s %s%s' % (
            num(round(lx, 6)), num(round(ly, 6)), num(round(lz, 6)), num(round(sx, 6)), num(round(sy, 6)), num(round(sz, 6)), mat,
            '' if collide else ' nocollide'))
        self.count += 1

    @staticmethod
    def _tail(s, collide, name, extra):
        if not collide:
            s += ' nocollide'
        if name:
            s += ' name ' + name
        if extra:
            s += ' ' + extra
        return s


def header(spawn_x):
    return [
        'OCWORLD 1',
        'NAME BigWorldLab',
        'SPAWN %s 0 200 0' % num(spawn_x),
        'SUN elev 48.0 azim 130.0 color 1 0.97 0.92 lux 90000',
        'SKY model physical',
        '# Generated by scripts/gen-bigworld.py (edit the script, not this file). See README.md.',
        '# A road along +-100 km of X; lamp posts and block buildings of the built-in cube; the cases a chunked, origin-shifted world breaks on.',
        '# Centimetres, +X forward at yaw 0, +Y right, +Z up. A PLACEG\'s last three numbers are half extents.',
        '# Positions are on a quarter-centimetre grid, so a chunked restore equals a flat load bit for bit.',
        'STREAM cell=6400 load=50000 evict=60000',
        '',
        '# The infinite scatter field: the chunk generator at 100 km and beyond.',
        'PCGVOLUME name Field seed 424242 cell 1600 octaves 3 floor 0.55 bias 1 samples 2 radius 6 infinite',
        'SCATTER mesh Meshes/cube.ocmesh material M_Foliage volume Field weight 1 scale 25 70 collide 0',
        'SCATTER mesh Meshes/cube.ocmesh material M_Bark volume Field weight 0.4 scale 40 120 collide 0',
        '',
    ]


def build_level(km, per_km, spawn_x, seed):
    lv = Level()
    rng = Rng(seed)
    x_min, x_max = -km * KM, km * KM
    lamp_gap = 2000                       # a post every 20 m on each side
    posts_per_km = 2 * (KM // lamp_gap)   # 100
    road_per_km = KM // 10000             # a 100 m slab each
    fixed = road_per_km + 2 * posts_per_km
    buildings_per_km = max(0, (per_km - fixed) // 2)   # each building is two placements (body + roof)
    slots = [(side, row) for side in (-1, 1) for row in range(4)]

    # ---- the road ----
    lv.add('# ---- the road, lamp posts and buildings, kilometre by kilometre ----')
    first_km = -km
    for k in range(first_km, km):
        x0 = k * KM
        lv.add('# km %d' % k)
        for i in range(road_per_km):
            lv.placeg(x0 + 5000 + 10000 * i, 0, -50, 5000, 800, 50, 'M_Concrete', name=None)
        # lamp posts, each with a head as a CHILD: parent-relative numbers, travelling with the post
        for i in range(KM // lamp_gap):
            for side in (-1, 1):
                x = x0 + lamp_gap * i + lamp_gap // 2
                y = side * 900
                lv.placeg(x, y, 300, 12, 12, 300, 'M_Metal', collide=False)
                lv.add('  BEGIN')
                lv.child((12, 12, 300), (0, -side * 40, 312), (30, 60, 12), 'M_Accent', collide=False)   # the lamp head, 12 cm above the post top
                lv.add('  END')
        # buildings: four rows each side of the road
        for b in range(buildings_per_km):
            side, row = slots[b % len(slots)]
            x = x0 + rng.range(0, KM)
            y = side * (1800 + row * 2500 + rng.range(0, 800))
            skyline = (b % 40) == 7
            if skyline:
                hx, hy, hz = rng.range(3500, 4200), rng.range(1200, 1600), rng.range(3500, 6000)   # wide on its own: reach past 3,200 cm
            else:
                hx, hy, hz = rng.range(200, 750), rng.range(200, 600), rng.range(400, 3000)
            yaw = rng.pick((0, 0, 0, 90, 15, -15))
            collide = (b % 5) == 0
            lv.placeg(x, y, hz, hx, hy, hz, rng.pick(('M_Wall', 'M_Concrete', 'M_Trim')), yaw=yaw, collide=collide)
            lv.add('  BEGIN')
            lv.child((hx, hy, hz), (0, 0, hz + 60), (hx * 0.6, hy * 0.6, 60), 'M_Floor', collide=False)   # a roof box on top
            lv.add('  END')

    # ---- every 5 km: a crate pile and a door ----
    lv.add('')
    lv.add('# ---- every 5 km: a crate pile (statics) and an animated door that swings on its hinge edge ----')
    for m in range(x_min // (5 * KM), x_max // (5 * KM) + 1):
        x = m * 5 * KM + 2500
        if x < x_min or x > x_max:
            continue
        for ix in range(3):
            for iz in range(2):
                lv.place(x + ix * 90, 400, 40 + iz * 80, 40, 'M_Crate', yaw=(ix * 7 + iz * 11) % 20, name='Crate_%d_%d_%d' % (m, ix, iz))
        dx, dy = x, -400
        lv.placeg(dx - 68, dy, 115, 8, 12, 115, 'M_Trim', name='DoorPostL_%d' % m)
        lv.placeg(dx + 68, dy, 115, 8, 12, 115, 'M_Trim', name='DoorPostR_%d' % m)
        lv.placeg(dx, dy, 238, 76, 12, 8, 'M_Trim', name='DoorLintel_%d' % m)
        # the leaf: centre half a leaf-width (60 cm) from its hinge edge, which the clip swings about
        lv.placeg(dx, dy, 110, 60, 5, 110, 'M_Accent', name='Door_%d' % m, extra='anim Anims/Door.ocanim')

    # ---- the seams, and chunk boundaries at negative coordinates ----
    lv.add('')
    lv.add('# ---- items straddling every region seam in range (a seam is a chunk boundary 512 chunks either side of a region centre), and chunk')
    lv.add('# boundaries at negative coordinates: exactly on the line, a quarter centimetre either side ----')
    r = math.floor(x_min / REGION_STEP) - 1
    while True:
        seam = r * REGION_STEP - REGION_STEP // 2
        r += 1
        if seam < x_min:
            continue
        if seam > x_max:
            break
        tag = 'Seam%+d' % (r - 1)
        lv.placeg(seam, 2600, 150, 600, 300, 150, 'M_Accent', name=tag + '_Span')     # centred on the seam, 12 m long
        lv.place(seam - 0.25, 3400, 100, 40, 'M_Target', name=tag + '_West')
        lv.place(seam, 3400, 100, 40, 'M_Target', name=tag + '_On')
        lv.place(seam + 0.25, 3400, 100, 40, 'M_Target', name=tag + '_East')
    for n in (1, 2, 7, 64, 513, 1000, 6000):
        x = -CHUNK * n
        for dy, label in ((-3400, 'S'), (-CHUNK * 3, 'B')):
            for ddx, side in ((-0.25, 'Lt'), (0.0, 'On'), (0.25, 'Rt')):
                if x + ddx >= x_min:
                    lv.place(x + ddx, dy - (0.25 if dy == -CHUNK * 3 and ddx < 0 else 0.0), 90, 40, 'M_Target',
                             name='NegChunk%d_%s_%s' % (n, label, side))

    # ---- the bridge (wide), the prop, the COMPONENT placement, the class, the lights ----
    lv.add('')
    lv.add('# ---- a 500 m bridge: wide on its own, so the cook lists it and the streamer keeps its owner chunk while the deck is near ----')
    bx = 25 * KM if x_max >= 25 * KM + 25000 else (x_min + x_max) // 2
    lv.placeg(bx, 0, 1500, 25000, 900, 120, 'M_Metal', name='Bridge500m')
    lv.add('  BEGIN')
    for i in range(-5, 6):
        # pillars from the ground (z = 0) up to the deck's underside (1,380): half height 690, centre 810 below the deck's centre
        lv.child((25000, 900, 120), (i * 4400, 0, -810), (200, 200, 690), 'M_Concrete')
    lv.add('  END')

    lv.add('')
    lv.add('# ---- beside the spawn: a spinning prop (an animated OBJECT clip), a placement carrying a COMPONENT, a level-global graph class ----')
    lv.place(spawn_x + 3000, -600, 200, 80, 'M_Accent', name='SpinProp', extra='anim Anims/Spin.ocanim')
    lv.add('PLACE Meshes/cube.ocmesh %s 600 20 0 0 0 20 M_Crate name ShapedCrate' % num(q(spawn_x + 3000)))
    lv.add('  COMPONENT CollisionShape asset=Physics/Crate.ocshape noscale=1')
    lv.count += 1
    lv.add('PLACE none %s 1500 0 0 0 0 1 class AN_BigBeacon' % num(q(spawn_x + 2000)))
    lv.count += 1
    lv.add('LIGHT name BeaconLight kind point pos %s 1500 900 colour 1 0.7 0.3 intensity 30000 range 4000' % num(q(spawn_x + 2000)))
    return lv


OCSHAPE_CRATE = """OCSHAPE 1
# A 40 cm crate, 20 kg (the PhysicsShapesLab crate): the one COMPONENT-carrying placement of BigWorldLab.
NAME Crate
MASS 20
NODE box box 20 20 20 radius 0.5
ROOT box
"""

BEACON_GRAPH = """OCGRAPH 1
DOMAIN gameplay
NAME BigBeacon
DESCRIPTION A level-global class placement for BigWorldLab: a tall pillar beside the spawn. Class placements are never chunk-owned (CHUNKS.md 8.3), so the cook leaves this one in the slim level.

CLASS AN_BigBeacon Actor
COMP body Mesh mesh=Meshes/cube.ocmesh scale=60,60,400 pos=0,0,400
"""

MANIFEST = """OCPROJECT 1
# Engine test content for the LARGE-WORLD work (docs/plans/wave11/LARGE_WORLD.md): a level 200 km long, cooked into chunks.
# Not a demo game: the built-in cube only, no third-party assets. Generate the level first: python scripts/gen-bigworld.py (see README.md).
NAME BigWorldLab
ENGINE Aver 0.7.0-1
CREATEDWITH 0.7.0-1
CONTENT Content
STARTMAP Maps/BigWorldLab.ocworld
"""

GITIGNORE = """# Generated by scripts/gen-bigworld.py, about 30 MB of text; the script is the source.
Content/Maps/BigWorldLab.ocworld
# The cook's output and the editor's per-project data.
Binaries/
Saved/
"""


def write(path, data, mode='w'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if mode == 'wb':
        with open(path, 'wb') as f:
            f.write(data)
    else:
        with open(path, 'w', newline='\n', encoding='utf-8') as f:
            f.write(data)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(here, '..', 'test-content', 'BigWorldLab'))
    ap.add_argument('--km', type=int, default=100, help='half-length of the road in km (the level spans +-km)')
    ap.add_argument('--per-km', type=int, default=2000, help='placements per kilometre of road')
    ap.add_argument('--seed', type=int, default=20261026)
    ap.add_argument('--assets-only', action='store_true', help='write only the small files, not the level')
    args = ap.parse_args()
    out = os.path.abspath(args.out)

    write(os.path.join(out, 'Game.ocproject'), MANIFEST)
    write(os.path.join(out, '.gitignore'), GITIGNORE)
    write(os.path.join(out, 'Content', 'Anims', 'Spin.ocanim'), spin_clip(), 'wb')
    write(os.path.join(out, 'Content', 'Anims', 'Door.ocanim'), door_clip(60.0), 'wb')
    write(os.path.join(out, 'Content', 'Physics', 'Crate.ocshape'), OCSHAPE_CRATE)
    write(os.path.join(out, 'Content', 'Scripts', 'BigBeacon.ocgraph'), BEACON_GRAPH)
    if args.assets_only:
        print('wrote the small assets under', out)
        return 0

    if args.km < 1 or args.per_km < 300:
        print('--km must be at least 1 and --per-km at least 300', file=sys.stderr)
        return 2
    # Start 50 km before the middle so the 100 km flight crosses six rebases; a short slice starts near its west end.
    spawn_x = -50 * KM if args.km >= 100 else -args.km * KM + 5000
    lv = build_level(args.km, args.per_km, spawn_x, args.seed)
    text = '\n'.join(header(spawn_x) + lv.lines) + '\n'
    write(os.path.join(out, 'Content', 'Maps', 'BigWorldLab.ocworld'), text)
    print('wrote %s: %d placements over +-%d km (%.1f MB), spawn x = %d cm' % (
        os.path.join(out, 'Content', 'Maps', 'BigWorldLab.ocworld'), lv.count, args.km, len(text) / 1048576.0, spawn_x))
    return 0


if __name__ == '__main__':
    sys.exit(main())
