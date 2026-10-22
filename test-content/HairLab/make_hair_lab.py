#!/usr/bin/env python3
"""Writes the HairLab source meshes (Wavefront OBJ, procedural, no third-party assets), the three groom descriptions
(the HEAD records of an .ochair as plain text), the level and the project file. Run it from anywhere:  python make_hair_lab.py

OBJ is the glTF convention (right-handed, +Y up, -Z forward, metres); the importer turns that into the engine's
cm / +Z up / +X forward, so OBJ y becomes engine z, OBJ x becomes engine y and OBJ -z becomes engine +x.

What it writes (all under Content/):
  Source/HairHead.obj     a 9 cm sphere with a nose, the visible head
  Source/HairScalp.obj    the cap of the same sphere above the hairline: the surface the hair roots grow from
  Hair/ShortStraight.hair.txt, LongCurly.hair.txt, Ponytail.hair.txt
                          the HEAD records of the three grooms. `AverAssetC hair-text <file> --out <groom.ochair>` writes the binary groom
                          (see README.md); a groom with a GENERATE record and no strands is regenerated from its scalp at load.
  Maps/HairLab.ocworld    three heads in a row, one groom each, on pedestals
and Game.ocproject beside this script.
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = os.path.join(HERE, "Content")


def uv_sphere(radius, lat, lon, centre=(0.0, 0.0, 0.0), keep=None):
    """Vertices, uvs and outward-wound triangles of a UV sphere. keep(v) -> bool selects the triangles whose three vertices all pass."""
    v, vt = [], []
    for j in range(lat + 1):
        phi = math.pi * j / lat                         # 0 at +Y (the crown), pi at -Y
        for i in range(lon + 1):
            theta = 2.0 * math.pi * i / lon
            v.append((centre[0] + radius * math.sin(phi) * math.cos(theta),
                      centre[1] + radius * math.cos(phi),
                      centre[2] + radius * math.sin(phi) * math.sin(theta)))
            vt.append((i / lon, 1.0 - j / lat))
    idx = lambda i, j: j * (lon + 1) + i
    f = []
    for j in range(lat):
        for i in range(lon):
            a, b, c, d = idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)
            # Counter-clockwise seen from outside (right-handed, glTF's front face): increasing theta runs to the viewer's left, so a is top right,
            # b top left, c bottom left, d bottom right, and the two triangles are a, c, d and a, b, c.
            for tri in ((a, c, d), (a, b, c)):
                if keep is None or all(keep(v[k]) for k in tri):
                    f.append(tri)
    return v, vt, f


def write_obj(name, parts):
    """parts: [(vertices, uvs, triangles)] merged into one object."""
    os.makedirs(os.path.join(CONTENT, "Source"), exist_ok=True)
    v_all, vt_all, f_all = [], [], []
    for v, vt, f in parts:
        base = len(v_all)
        v_all += v
        vt_all += vt
        f_all += [(a + base, b + base, c + base) for a, b, c in f]
    # Drop the vertices no triangle uses (the cap keeps a slice of the sphere), renumbering.
    used = sorted({k for tri in f_all for k in tri})
    remap = {old: n + 1 for n, old in enumerate(used)}
    path = os.path.join(CONTENT, "Source", name + ".obj")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# HairLab test mesh (procedural; see make_hair_lab.py)\no %s\n" % name)
        for k in used:
            fh.write("v %.5f %.5f %.5f\n" % v_all[k])
        for k in used:
            fh.write("vt %.5f %.5f\n" % vt_all[k])
        for a, b, c in f_all:
            fh.write("f %d/%d %d/%d %d/%d\n" % (remap[a], remap[a], remap[b], remap[b], remap[c], remap[c]))
    print("wrote", name, len(used), "vertices", len(f_all), "triangles")


# The head: radius 9 cm, a 1.8 cm nose pointing forward (OBJ -Z). The scalp is the part of a slightly larger sphere above the hairline
# (OBJ y >= 1.5 cm) so the roots sit just outside the visible head and nothing grows on the face.
HEAD_R = 0.09
write_obj("HairHead", [uv_sphere(HEAD_R, 24, 32), uv_sphere(0.018, 8, 12, centre=(0.0, -0.005, -HEAD_R))])
write_obj("HairScalp", [uv_sphere(HEAD_R * 1.02, 24, 32, keep=lambda p: p[1] >= 0.015)])

# ---- the grooms ----------------------------------------------------------------------------------------------------------------------------------

LOD = "LOD distances 300 700 1500 strandFractions 1 0.5 0.25 0.12"

GROOMS = {
    "ShortStraight": """# HairLab: short straight hair, brown. Edit and re-run AverAssetC hair-text.
OCHAIR 1
SCALP Meshes/HairScalp.ocmesh
SIM iterationsPerSecond 360 maxDeltaTime 0.0333 simFraction 0.1
MATERIAL 0 linearDamping 3 angularDamping 3 bendCompliance 5e-8 globalPose 0.02 0 0 0.4
LOOK 0 baseColor 0.30 0.20 0.11 tipColor 0.42 0.29 0.16 colorVariation 0.15 widthRootCm 0.10 widthTipCm 0.04 roughness 0.35 shift 0.07
       tt 1 trt 1 azimuthRough 0.4 tint 0.9 0.6 0.4 scatter 0.5
GENERATE seed 1001 strands 6000 lengthCm 6 9 segments 8 curlCm 0 curlFrequency 3 clumps 0 partDirection 0 1 0 partWidthCm 0.6
         style 0 0 -1 styleWeight 0.45
%s
""" % LOD,
    "LongCurly": """# HairLab: long curly hair, dark red. Clumps of followers around guide strands.
OCHAIR 1
SCALP Meshes/HairScalp.ocmesh
SIM iterationsPerSecond 360 maxDeltaTime 0.0333 simFraction 0.1
MATERIAL 0 linearDamping 2 angularDamping 2 bendCompliance 1e-7 globalPose 0.01 0 0 0.3
LOOK 0 baseColor 0.28 0.10 0.06 tipColor 0.45 0.18 0.09 colorVariation 0.2 widthRootCm 0.11 widthTipCm 0.04 roughness 0.30 shift 0.08
       tt 1 trt 1.2 azimuthRough 0.45 tint 0.95 0.55 0.35 scatter 0.55
GENERATE seed 2002 strands 9000 lengthCm 24 34 segments 12 curlCm 1.2 curlFrequency 3 clumps 150 clumpRadiusCm 1.5 clumpTightness 0.6
         partDirection 0 1 0 partWidthCm 0.8 style 0 0 -1 styleWeight 0.8
%s
""" % LOD,
    "Ponytail": """# HairLab: a ponytail, blonde. Pulled back (engine -X) and low; the neck and shoulder proxies catch it.
OCHAIR 1
SCALP Meshes/HairScalp.ocmesh
SIM iterationsPerSecond 360 maxDeltaTime 0.0333 simFraction 0.1
MATERIAL 0 linearDamping 1.5 angularDamping 1.5 bendCompliance 2e-7 globalPose 0.005 0 0 0.2
LOOK 0 baseColor 0.62 0.46 0.22 tipColor 0.78 0.62 0.34 colorVariation 0.12 widthRootCm 0.10 widthTipCm 0.04 roughness 0.28 shift 0.06
       tt 1 trt 1 azimuthRough 0.35 tint 0.95 0.75 0.45 scatter 0.5
GENERATE seed 3003 strands 8000 lengthCm 20 28 segments 12 curlCm 0.2 curlFrequency 2 clumps 12 clumpRadiusCm 8 clumpTightness 0.9
         partDirection 0 1 0 partWidthCm 0.4 style -0.7 0 -0.7 styleWeight 0.9
%s
""" % LOD,
}

os.makedirs(os.path.join(CONTENT, "Hair"), exist_ok=True)
for name, text in GROOMS.items():
    path = os.path.join(CONTENT, "Hair", name + ".hair.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote", os.path.relpath(path, HERE))

# ---- the level and the project -----------------------------------------------------------------------------------------------------------------

os.makedirs(os.path.join(CONTENT, "Maps"), exist_ok=True)
with open(os.path.join(CONTENT, "Maps", "HairLab.ocworld"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write("""OCWORLD 1
NAME HairLab
GROUND 0.0 0
KILLZ -5000.0
SPAWN -200 0 120 0
# Nothing here is a third-party asset. The meshes and grooms are generated by make_hair_lab.py; the .ocmesh and .ochair files appear after the
# import steps in README.md, and until then the placements draw nothing.
# Three heads in a row along Y, 300 cm ahead of the spawn, each on a pedestal (the built-in cube): short straight, long curly, ponytail.
PLACE Meshes/cube.ocmesh 300 -60 40 0 0 0 0.3 name PedestalA
PLACE Meshes/cube.ocmesh 300 0 40 0 0 0 0.3 name PedestalB
PLACE Meshes/cube.ocmesh 300 60 40 0 0 0 0.3 name PedestalC
PLACE Meshes/HairHead.ocmesh 300 -60 115 0 0 0 1 nocollide name HeadShortStraight
  COMPONENT Hair asset=Hair/ShortStraight.ochair
PLACE Meshes/HairHead.ocmesh 300 0 115 0 0 0 1 nocollide name HeadLongCurly
  COMPONENT Hair asset=Hair/LongCurly.ochair
PLACE Meshes/HairHead.ocmesh 300 60 115 0 0 0 1 nocollide name HeadPonytail
  COMPONENT Hair asset=Hair/Ponytail.ochair
""")
print("wrote Content/Maps/HairLab.ocworld")

with open(os.path.join(HERE, "Game.ocproject"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write("""OCPROJECT 1
# Engine test content for STRAND HAIR (docs/HAIR.md). Not a demo game.
# Open the project, import Content/Source/*.obj and write the grooms (see README.md), open Maps/HairLab.ocworld and press Play.
NAME HairLab
ENGINE Aver 0.7.0-1
CREATEDWITH 0.7.0-1
CONTENT Content
STARTMAP Maps/HairLab.ocworld
""")
print("wrote Game.ocproject")
