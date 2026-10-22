#!/usr/bin/env python3
"""Writes the PhysicsShapesLab's generated content (docs/PHYSICS_SHAPES.md "PhysicsShapesLab"): the procedural source meshes (Wavefront OBJ
with one usemtl per material slot), the physical-surface table, the materials that name a surface, the .ocshape trees, the project file and
the map. Stdlib only, no third-party assets. Run it from anywhere:

    python make_shapes_lab.py              # writes Content/Source/*, Content/**, then cooks the meshes if AverAssetC is found
    python make_shapes_lab.py --no-import  # write only (the cook command is in README.md)
    python make_shapes_lab.py --assetc <path-to-AverAssetC>

Shapes are authored in ENGINE axes (centimetres, +X forward, +Y right, +Z up) and written as OBJ in the glTF convention the importer
expects (metres, +Y up, -Z forward): engine (x, y, z) cm -> obj (y, z, -x) / 100. Winding is decided per face from the outward normal the
shape knows, in OBJ space, so it survives the handedness flip (the same method templates/Vehicle/vehicle_geo.py uses).
"""
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = os.path.join(HERE, "Content")
SOURCE = os.path.join(CONTENT, "Source")


def write(rel, text, root=CONTENT):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote", os.path.relpath(path, HERE))


# ----------------------------------------------------------------------------------------------------------------- geometry

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


def centroid(points):
    n = float(len(points))
    return (sum(p[0] for p in points) / n, sum(p[1] for p in points) / n, sum(p[2] for p in points) / n)


class Mesh:
    """Polygons in ENGINE centimetres, each with a material slot name; written as OBJ grouped by slot."""

    def __init__(self, name, solid_center):
        self.name = name
        self.center = solid_center
        self.faces = []   # (material, [engine points], outward normal in engine space)

    def polygon(self, material, pts, center=None):
        a, b, c = pts[0], pts[1], pts[2]
        n = cross(sub(b, a), sub(c, a))
        if dot(n, sub(centroid(pts), center or self.center)) < 0.0:
            n = (-n[0], -n[1], -n[2])
        length = math.sqrt(dot(n, n)) or 1.0
        self.faces.append((material, list(pts), (n[0] / length, n[1] / length, n[2] / length)))

    def box(self, material, lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        p = {(sx, sy, sz): (x1 if sx else x0, y1 if sy else y0, z1 if sz else z0) for sx in (0, 1) for sy in (0, 1) for sz in (0, 1)}
        mid = ((x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5)   # this box's own centre: outward is relative to IT, not to the mesh
        for corners in (((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)), ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)),
                        ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)), ((0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1)),
                        ((0, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 1)), ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1))):
            self.polygon(material, [p[c] for c in corners], mid)

    def write_obj(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        order = []
        for mat, _, _ in self.faces:
            if mat not in order:
                order.append(mat)
        verts, uvs, norms, groups = [], [], [], {m: [] for m in order}
        for mat, pts, n_eng in self.faces:
            n_obj = dir_to_obj(n_eng)
            axis = max(range(3), key=lambda i: abs(n_eng[i]))
            u, v = [i for i in range(3) if i != axis]
            idx = []
            for p in pts:
                verts.append(to_obj(p))
                uvs.append((0.5 + p[u] / 400.0, 0.5 + p[v] / 400.0))
                norms.append(n_obj)
                idx.append(len(verts))
            for i in range(1, len(idx) - 1):
                a, b, c = idx[0], idx[i], idx[i + 1]
                if dot(cross(sub(verts[b - 1], verts[a - 1]), sub(verts[c - 1], verts[a - 1])), n_obj) < 0.0:
                    b, c = c, b
                groups[mat].append((a, b, c))
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# %s: procedural, no third-party assets (make_shapes_lab.py)\no %s\n" % (self.name, self.name))
            for p in verts:
                fh.write("v %.5f %.5f %.5f\n" % p)
            for t in uvs:
                fh.write("vt %.5f %.5f\n" % t)
            for n in norms:
                fh.write("vn %.5f %.5f %.5f\n" % n)
            for mat in order:
                fh.write("usemtl %s\n" % mat)
                for a, b, c in groups[mat]:
                    fh.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a, a, a, b, b, b, c, c, c))
        tris = sum(len(g) for g in groups.values())
        print("wrote %s: %d vertices, %d triangles, slots %s" % (os.path.relpath(path, HERE), len(verts), tris, ", ".join(order)))


def two_slot_floor():
    """An 800 x 400 cm slab, top at z = 0: the left half (y < 0) is slot M_LabIce, the right half (y > 0) M_LabRubber."""
    m = Mesh("LabFloorTwoSlot", (0.0, 0.0, -10.0))
    m.box("M_LabIce", (-400.0, -200.0, -20.0), (400.0, 0.0, 0.0))
    m.box("M_LabRubber", (-400.0, 0.0, -20.0), (400.0, 200.0, 0.0))
    return m


def ramp():
    """A 900 cm long wedge falling 330 cm (about 20 degrees) towards +X and 300 cm wide, with three 100 cm lanes along the slope:
    ice, rubber and metal. Its back, sides and underside are M_LabBase, which names no surface."""
    m = Mesh("LabRamp", (300.0, 0.0, 110.0))
    h, length = 330.0, 900.0
    for lo, hi, mat in ((-150.0, -50.0, "M_LabIce"), (-50.0, 50.0, "M_LabRubber"), (50.0, 150.0, "M_LabMetal")):
        m.polygon(mat, [(0.0, lo, h), (length, lo, 0.0), (length, hi, 0.0), (0.0, hi, h)])
    m.polygon("M_LabBase", [(0.0, -150.0, 0.0), (length, -150.0, 0.0), (length, 150.0, 0.0), (0.0, 150.0, 0.0)])
    m.polygon("M_LabBase", [(0.0, -150.0, 0.0), (0.0, 150.0, 0.0), (0.0, 150.0, h), (0.0, -150.0, h)])
    m.polygon("M_LabBase", [(0.0, -150.0, 0.0), (length, -150.0, 0.0), (0.0, -150.0, h)])
    m.polygon("M_LabBase", [(0.0, 150.0, 0.0), (length, 150.0, 0.0), (0.0, 150.0, h)])
    return m


def rock():
    """A small irregular convex rock (an octahedron with uneven points), about 80 cm across: the `hull mesh` prop."""
    m = Mesh("LabRock", (0.0, 0.0, 0.0))
    xs = [(45.0, 0.0, 2.0), (-38.0, 5.0, 0.0)]
    ys = [(3.0, 42.0, -2.0), (-4.0, -36.0, 3.0)]
    zs = [(2.0, 6.0, 30.0), (-3.0, -2.0, -28.0)]
    for a in xs:
        for b in ys:
            for c in zs:
                m.polygon("M_LabBase", [a, b, c])
    return m


def closed_barrel():
    """A closed 16-sided drum, radius 30, 80 cm tall (z -40..40): the dynamic mesh prop (a concave-capable mesh body with a mass)."""
    m = Mesh("LabBarrelMesh", (0.0, 0.0, 0.0))
    seg, r, hz = 16, 30.0, 40.0
    ring = [(math.cos(2 * math.pi * i / seg) * r, math.sin(2 * math.pi * i / seg) * r) for i in range(seg)]
    for i in range(seg):
        j = (i + 1) % seg
        m.polygon("M_LabMetal", [(ring[i][0], ring[i][1], -hz), (ring[j][0], ring[j][1], -hz), (ring[j][0], ring[j][1], hz), (ring[i][0], ring[i][1], hz)])
    m.polygon("M_LabMetal", [(x, y, hz) for x, y in ring])
    m.polygon("M_LabMetal", [(x, y, -hz) for x, y in ring])
    return m


MESHES = {"LabFloorTwoSlot": two_slot_floor, "LabRamp": ramp, "LabRock": rock, "LabBarrelMesh": closed_barrel}


# ----------------------------------------------------------------------------------------------------------------- text assets

SURFACES = """OCSURF 1
# The lab's physical surfaces. Friction and restitution left out defer to the body; a stated number replaces the body's on that surface.
# Ice slides, Rubber grips and bounces, Metal sits between, Gravel and Grass are for the landscape layers, Wood labels the chair.
SURFACE Grass  friction 0.6  restitution 0.05  tag 1
SURFACE Ice    friction 0.03  tag 2
SURFACE Rubber friction 1.2   restitution 0.8   tag 3
SURFACE Metal  friction 0.25  restitution 0.2   tag 4
SURFACE Gravel friction 0.8   tag 5
SURFACE Wood   friction 0.5   restitution 0.1   tag 6
"""


def material(name, colour, surface=None):
    lines = ["OCMAT 1", "# %s: a plain lab material%s." % (name, (" naming the physical surface " + surface) if surface else " that names no physical surface"),
             "SHADER standard", "BLEND opaque", "CULL back", "FLAGS twosided=0 castshadow=1", "",
             "PARAM baseColorFactor %.3f %.3f %.3f 1.0" % colour, "PARAM metallicFactor 0.0", "PARAM roughnessFactor 0.7",
             "PARAM emissiveFactor 0.0 0.0 0.0", "PARAM normalScale 1.0", "PARAM occlusionStrength 1.0"]
    if surface:
        lines += ["", "PHYSSURFACE " + surface]
    return "\n".join(lines) + "\n"


MATERIALS = {
    "M_LabIce": ((0.62, 0.82, 0.95), "Ice"),
    "M_LabRubber": ((0.12, 0.12, 0.14), "Rubber"),
    "M_LabMetal": ((0.62, 0.64, 0.68), "Metal"),
    "M_LabBase": ((0.45, 0.42, 0.38), None),
}

CHAIR = """OCSHAPE 1
# A chair as a static compound of boxes and cylinders: the design's example. Play and watch Show > Collision Shapes: the seat, the back and
# the four legs are drawn, the centre of mass (yellow) sits away from the origin (cyan), and every leaf is labelled Wood.
NAME Chair
MASS 12
SURFACE Wood
NODE seat  box 40 40 4
NODE back  box 40 3 25
NODE leg   cylinder 3 45 radius 0.5
NODE top   compound static
CHILD top seat at 0 0 45
CHILD top back at -37 0 75
CHILD top leg  at 32 32 22
CHILD top leg  at 32 -32 22
CHILD top leg  at -32 32 22
CHILD top leg  at -32 -32 22
ROOT top
"""

BARREL = """OCSHAPE 1
# A barrel as a TAPERED CYLINDER: 30 cm radius at the top (+Z), 25 cm at the bottom, 90 cm tall. It stands on its small end when it settles.
NAME Barrel
MASS 40
SURFACE Metal
NODE barrel tcylinder 30 25 90
ROOT barrel
"""

GROW_CRATE = """OCSHAPE 1
# A MUTABLE compound: two boxes the body can gain and lose children from at run time (Body.AddChild / RemoveChild). One body owns it.
NAME GrowCrate
MASS 20
SURFACE Wood
NODE core box 25 25 25
NODE arm  box 12 12 12
NODE crate compound mutable
CHILD crate core
CHILD crate arm at 45 0 0
ROOT crate
"""

ROCK_HULL = """OCSHAPE 1
# `hull mesh`: the convex hull of the rock mesh's vertices, one piece. Needs Meshes/LabRock.ocmesh (cooked by make_shapes_lab.py).
NAME RockHull
MASS 30
SURFACE Gravel
NODE hull hull mesh Meshes/LabRock.ocmesh
ROOT hull
"""

MESH_PROP = """OCSHAPE 1
# A DYNAMIC TRIANGLE MESH: the closed drum as a mesh leaf with a mass. It collides with convex shapes (the floor box, other props) and falls
# THROUGH mesh floors and heightfields, because Jolt has no mesh-mesh collision. Drop one onto Meshes/LabFloorTwoSlot.ocmesh to see it.
NAME MeshProp
MASS 15
NODE drum mesh Meshes/LabBarrelMesh.ocmesh
ROOT drum
"""

PLANE_RAMP = """OCSHAPE 1
# A solid half-space tilted 30 degrees: the points with n.x = 0 form the plane, the side opposite n is solid. Static bodies only.
NAME PlaneRamp
NODE ramp plane -0.5 0 0.866 0 800
ROOT ramp
"""

CRATE = """OCSHAPE 1
# A 40 cm crate, 20 kg: the sliding test on the ramp's three lanes.
NAME Crate
MASS 20
NODE box box 20 20 20 radius 0.5
ROOT box
"""

SHAPES = {"Chair": CHAIR, "Barrel": BARREL, "GrowCrate": GROW_CRATE, "RockHull": ROCK_HULL, "MeshProp": MESH_PROP, "PlaneRamp": PLANE_RAMP, "Crate": CRATE}

PROJECT = """OCPROJECT 1
# Engine test content for SHAPES_MATERIALS (docs/PHYSICS_SHAPES.md). Not a demo game: cubes standing in for shapes, four small meshes, no third-party assets.
# Open the project, open Maps/Lab.ocworld and press Play (README.md says what to check).
NAME PhysicsShapesLab
ENGINE Aver 0.7.0-1
CREATEDWITH 0.7.0-1
CONTENT Content
STARTMAP Maps/Lab.ocworld
"""


def map_text():
    o = []
    add = o.append
    add("OCWORLD 1")
    add("NAME PhysicsShapesLab")
    add("SPAWN -600 0 150 0")
    add("SUN elev 48.0 azim 130.0 color 1 0.97 0.92 lux 90000")
    add("SKY model physical")
    add("# Engine test content for SHAPES_MATERIALS (docs/PHYSICS_SHAPES.md). Not a demo game. The stand-ins are the built-in unit cube")
    add("# (Meshes/cube.ocmesh, half-extent 1 cm: PLACE's scale is the half extent in centimetres); the four meshes are procedural, from make_shapes_lab.py.")
    add("# A COMPONENT CollisionShape names the .ocshape that gives the placement its collision (the stand-in cube is only what you SEE; turn on")
    add("# Show > Collision Shapes to see what collides). `motion=dynamic` makes it fall: no level file carries a Rigid Body, so the record does. +X is forward.")
    add("# A stand-in cube is scaled to its size, and a body is built at the entity's world scale unless `noscale=1`: the cubes say noscale, the meshes (scale 1) do not.")
    add("# CRigidBody has no pose write-back (docs/PHYSICS_SHAPES.md), so a dynamic body falls INVISIBLY: its cube stays where it was placed. Watch the wire.")
    add("PLACEG Meshes/cube.ocmesh 3000 0 -50 0 0 0 9000 4500 50 name Floor")
    add("")
    add("# The shape row along y = -1000..1000 at x = 0 (the spawn looks along +X), dropped from 1.2 m so the fall is visible.")
    add("PLACE Meshes/cube.ocmesh 0 -1000 120 0 0 0 40 nocollide name Chair")
    add("  COMPONENT CollisionShape asset=Physics/Chair.ocshape surface=Wood motion=dynamic noscale=1")
    add("PLACE Meshes/cube.ocmesh 0 -600 150 0 0 0 28 nocollide name Barrel")
    add("  COMPONENT CollisionShape asset=Physics/Barrel.ocshape surface=Metal motion=dynamic noscale=1")
    add("PLACE Meshes/cube.ocmesh 0 -200 100 0 0 0 25 nocollide name GrowCrate")
    add("  COMPONENT CollisionShape asset=Physics/GrowCrate.ocshape surface=Wood motion=dynamic noscale=1")
    add("PLACE Meshes/LabRock.ocmesh 0 200 120 0 0 0 1 nocollide name RockHull")
    add("  COMPONENT CollisionShape asset=Physics/RockHull.ocshape surface=Gravel motion=dynamic")
    add("PLACE Meshes/LabBarrelMesh.ocmesh 0 600 200 0 0 0 1 nocollide name MeshPropOnFloor")
    add("  COMPONENT CollisionShape asset=Physics/MeshProp.ocshape motion=dynamic")
    add("PLACE Meshes/cube.ocmesh 0 1000 0 0 0 0 1 nocollide name PlaneRamp")
    add("  COMPONENT CollisionShape asset=Physics/PlaneRamp.ocshape")
    add("PLACE Meshes/cube.ocmesh 150 1000 120 0 -30 0 20 nocollide name PlaneCrate")
    add("  COMPONENT CollisionShape asset=Physics/Crate.ocshape motion=dynamic noscale=1")
    add("")
    add("# The two-slot floor (800 x 400 cm, ice on the left half, rubber on the right): its collision mesh carries each slot's surface, so a probe")
    add("# (Details > Collision Shape > Probe below the entity, or an `an_surfacebeneath` node) reports Ice on the left and Rubber on the right.")
    add("# A mesh prop dropped on it falls THROUGH (mesh vs mesh does not collide: the documented limit).")
    add("PLACE Meshes/LabFloorTwoSlot.ocmesh 2000 0 1 0 0 0 1 name TwoSlotFloor")
    add("PLACE Meshes/LabBarrelMesh.ocmesh 2000 100 300 0 0 0 1 nocollide name MeshPropOnMesh")
    add("  COMPONENT CollisionShape asset=Physics/MeshProp.ocshape motion=dynamic")
    add("")
    add("# The three-lane ramp, 900 cm long and falling about 20 degrees towards +X: ice (y -150..-50), rubber (-50..50), metal (50..150). A crate sits")
    add("# at the top of each lane: the ice crate slides away, the metal crate creeps, the rubber crate holds. Lower Rubber's friction in")
    add("# Project Settings > Physics > Surfaces while it plays and the next slide changes.")
    add("PLACE Meshes/LabRamp.ocmesh 3200 0 0.5 0 0 0 1 name Ramp")
    for i, (lane, y) in enumerate((("Ice", -100), ("Rubber", 0), ("Metal", 100))):
        add("PLACE Meshes/cube.ocmesh 3250 %d 335 0 20 0 20 nocollide name Crate%s" % (y, lane))
        add("  COMPONENT CollisionShape asset=Physics/Crate.ocshape motion=dynamic noscale=1")
    add("")
    return "\n".join(o) + "\n"


# ----------------------------------------------------------------------------------------------------------------- cook

def cook(assetc):
    out = os.path.join(CONTENT, "Meshes")
    os.makedirs(out, exist_ok=True)
    for name in MESHES:
        src = os.path.join(SOURCE, name + ".obj")
        res = subprocess.run([assetc, "convert", src, "--out-dir", out, "--base", name], capture_output=True, text=True)
        lines = [l for l in res.stdout.splitlines() if l.startswith("{")]
        print("cook", name, "->", lines[0][:160] if lines else (res.stdout + res.stderr)[-300:])
        if res.returncode != 0:
            print("  (AverAssetC failed on %s: see README.md)" % name)


def main():
    assetc = os.path.normpath(os.path.join(HERE, "..", "..", "build", "bin", "AverAssetC.exe"))
    if "--assetc" in sys.argv:
        assetc = sys.argv[sys.argv.index("--assetc") + 1]

    for name, make in MESHES.items():
        make().write_obj(os.path.join(SOURCE, name + ".obj"))
    write("Physics/Surfaces.ocsurf", SURFACES)
    for name, (colour, surface) in MATERIALS.items():
        write("Materials/%s.ocmat" % name, material(name, colour, surface))
    for name, text in SHAPES.items():
        write("Physics/%s.ocshape" % name, text)
    write("Maps/Lab.ocworld", map_text())
    write("Game.ocproject", PROJECT, root=HERE)

    if "--no-import" not in sys.argv:
        if os.path.exists(assetc):
            cook(assetc)
        else:
            print("AverAssetC not found (%s): cook the meshes with the command in README.md" % assetc)


if __name__ == "__main__":
    main()
