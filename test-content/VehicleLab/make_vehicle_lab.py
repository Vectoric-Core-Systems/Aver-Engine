#!/usr/bin/env python3
"""Writes the VehicleLab's generated content (docs/VEHICLES.md "VehicleLab"): the procedural source meshes (Wavefront OBJ) and the
skinned glTF for the rigged wheel car, the .ocvehicle specs, the control rig, the input scheme and the map. Stdlib only, no
third-party assets. Run it from anywhere:

    python make_vehicle_lab.py              # writes Content/Source/*, Content/**, then cooks the meshes if AverAssetC is found
    python make_vehicle_lab.py --no-import  # write only (the cook command is in README.md)
    python make_vehicle_lab.py --assetc <path-to-AverAssetC>

The shapes come from templates/Vehicle/vehicle_geo.py, which this script imports from the repository.
"""
import base64
import json
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "templates", "Vehicle")))

import vehicle_geo as geo          # noqa: E402

CONTENT = os.path.join(HERE, "Content")
SOURCE = os.path.join(CONTENT, "Source")


def write(rel, text):
    path = os.path.join(CONTENT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote", os.path.relpath(path, HERE))


# ----------------------------------------------------------------------------------------------------------- specs

TRUCK = """OCVEHICLE 1
# A 6x4 lorry: three axles (the front one steered and undriven, the two rear ones driven through limited-slip differentials), a centre
# differential between the front and the driven pair, BOX records for the cab, the bed and the chassis rails, and a six-speed automatic.
# Check: it settles on six wheels, the two rear axles share the torque (Show > Vehicles > Forces), and the cab tilts less than a car's on a brake.
NAME Truck6x4
KIND wheeled
BODY half 400 120 60 clearance 40 com 0 0 95 mass 9000 pitchroll 55
BOX 250 0 135 110 110 55
BOX -90 0 125 280 115 45
BOX 0 0 70 400 90 12
ENGINE torque 1500 minrpm 800 maxrpm 3200 inertia 1.5 damping 0.3
CURVE 0 0.7 0.35 1 0.75 0.95 1 0.6
GEARBOX auto forward 6.4 4.2 2.8 1.9 1.3 1 reverse 6 switch 0.7 clutchrelease 0.45 latency 0.6 up 0.65 down 0.25 clutch 40
CENTER lsd 1.6
AXLE 0 final 4.1 diff open split 0.5 lsd 1.4 share 0 antiroll 4000
AXLE 1 final 4.1 diff lsd split 0.5 lsd 1.8 share 1 antiroll 3000
AXLE 2 final 4.1 diff lsd split 0.5 lsd 1.8 share 1 antiroll 3000
WHEELCLASS truck radius 50 width 30 susp 8 40 hz 1.4 damping 0.55 brake 4000 handbrake 6000 inertia 3 spindamping 0.3
WHEEL FL class truck hub 300 -110 50 steer 32 driven 0 handbrake 0 node WheelFL
WHEEL FR class truck hub 300 110 50 steer 32 driven 0 handbrake 0 node WheelFR
WHEEL M1L class truck hub -100 -110 50 steer 0 driven 1 handbrake 1 node WheelM1L
WHEEL M1R class truck hub -100 110 50 steer 0 driven 1 handbrake 1 node WheelM1R
WHEEL R1L class truck hub -300 -110 50 steer 0 driven 1 handbrake 1 node WheelR1L
WHEEL R1R class truck hub -300 110 50 steer 0 driven 1 handbrake 1 node WheelR1R
ASSIST abs 1 absslip 0.22 absrelease 6 tcs 1 tcsslip 0.3 steerin 2.5 steerout 4 throttlein 4 throttleout 6 brakein 6 reversebrake 1 reversespeed 50 holdbrake 0.4 holdspeed 30 skidslip 0.45 skidangle 12
STEERCURVE 0 1 1200 0.55 3000 0.3
AERO drag 0.9 downforce 0
SEAT driver 250 -40 140 exit 250 -190 0
DAMAGE health 2500 crash 700 energy 0.5 radius 100
CAMERA modes chase hood cockpit arm 1000 eye 270 -40 190 hood 400 0 150
"""

HATCHBACK = """OCVEHICLE 1
# An all-wheel-drive hatchback. The centre differential is limited-slip; the REAR axle's differential is the one to switch: set
# `diff open`, `diff lsd` or `diff locked` on AXLE 1 in the Vehicle editor, save, press Play again and drive the ice-split lane
# (left wheels on ice, right on tarmac): open spins the iced wheel, limited slip sends torque to the gripping one, locked pulls straight.
NAME Hatchback
KIND wheeled
BODY half 190 85 32 clearance 22 com 0 0 38 mass 1150 pitchroll 60
ENGINE torque 320 minrpm 900 maxrpm 6800 inertia 0.35 damping 0.22
CURVE 0 0.75 0.5 1 0.9 0.95 1 0.8
GEARBOX auto forward 3.55 2.05 1.38 1 0.82 reverse 3.4 switch 0.45 clutchrelease 0.3 latency 0.5 up 0.62 down 0.22 clutch 8
CENTER lsd 1.5
AXLE 0 final 3.8 diff open split 0.5 lsd 1.4 share 0 antiroll 800
AXLE 1 final 3.8 diff lsd split 0.5 lsd 2.2 share 0 antiroll 600
TYRE street long 0 0 0.06 1.2 0.2 1 lat 0 0 3 1.2 20 1
WHEELCLASS street radius 30 width 20 susp 5 28 hz 2.1 damping 0.5 brake 1400 handbrake 2200 inertia 0.8 spindamping 0.2 tyre street
WHEEL FL class street hub 125 -72 30 steer 36 driven 1 handbrake 0 node WheelFL
WHEEL FR class street hub 125 72 30 steer 36 driven 1 handbrake 0 node WheelFR
WHEEL RL class street hub -115 -72 30 steer 0 driven 1 handbrake 1 node WheelRL
WHEEL RR class street hub -115 72 30 steer 0 driven 1 handbrake 1 node WheelRR
ASSIST abs 1 absslip 0.25 absrelease 8 tcs 1 tcsslip 0.3 steerin 3 steerout 5 throttlein 6 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.3 holdspeed 30 skidslip 0.45 skidangle 12
STEERCURVE 0 1 1500 0.6 3500 0.35
AERO drag 0.3 downforce 0.4
SEAT driver 15 -32 52 exit -10 -130 0
DAMAGE health 900 crash 550 energy 0.5 radius 70
CAMERA modes chase hood cockpit arm 520 eye 20 -32 100 hood 130 0 88
"""

TANK = """OCVEHICLE 1
# The template's tank with a MANUAL gearbox (Q and E shift): the manual-capable path of the tracked controller.
NAME LabTank
KIND tracked
BODY half 350 170 45 clearance 35 com 0 0 70 mass 38000 pitchroll 70
ENGINE torque 6000 minrpm 800 maxrpm 3500 inertia 0 damping 0.4
GEARBOX manual forward 4 3 2 1 reverse 4 3 switch 0.6 clutchrelease 0.4 latency 0.5 up 0.6 down 0.2 clutch 0
WHEELCLASS road radius 40 width 50 susp 5 35 hz 1.8 damping 0.6 brake 0 handbrake 0 inertia 5 spindamping 0.2
""" + "".join(
    "WHEEL %s%d class road hub %d %d 40 steer 0 driven %d handbrake 0 node Wheel%s%d\n" % (side, i, x, y, 1 if i == 4 else 0, side, i)
    for side, y in (("L", -130), ("R", 130)) for i, x in enumerate((-280, -140, 0, 140, 280))) + """TRACK left wheels 0 1 2 3 4 driven 4 inertia 10 damping 0.5 brake 60000 final 6 steergain 1.6 pivot 0.6
TRACK right wheels 5 6 7 8 9 driven 9 inertia 10 damping 0.5 brake 60000 final 6 steergain 1.6 pivot 0.6
ASSIST abs 0 absslip 0.25 absrelease 8 tcs 0 tcsslip 0.35 steerin 2 steerout 4 throttlein 4 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.5 holdspeed 30 skidslip 0.45 skidangle 12
SEAT driver 60 0 120 exit -100 -260 0
DAMAGE health 3000 crash 900 energy 0.3 radius 120
CAMERA modes chase arm 900 eye 60 0 170 hood 250 0 130
"""

RIGGED = """OCVEHICLE 1
# A small car whose wheels are BONES of a skinned mesh instead of child entities: each WHEEL names a `bone`, and the vehicle system sets
# the control-rig inputs wheel_<bone>_pos and wheel_<bone>_rot every frame (Rigs/RiggedCar.ocrig reads them with setbone ops).
# Source/RiggedCar.gltf is written by make_vehicle_lab.py: a box body and four wheels, five joints (Root, FL, FR, RL, RR).
NAME RiggedCar
KIND wheeled
BODY half 170 70 28 clearance 22 com 0 0 34 mass 1000 pitchroll 60
ENGINE torque 400 minrpm 1000 maxrpm 6000 inertia 0.4 damping 0.2
WHEELCLASS small radius 30 width 20 susp 5 26 hz 2 damping 0.5 brake 1300 handbrake 2200 inertia 0.8 spindamping 0.2
WHEEL FL class small hub 120 -72 30 steer 34 driven 0 handbrake 0 bone FL
WHEEL FR class small hub 120 72 30 steer 34 driven 0 handbrake 0 bone FR
WHEEL RL class small hub -120 -72 30 steer 0 driven 1 handbrake 1 bone RL
WHEEL RR class small hub -120 72 30 steer 0 driven 1 handbrake 1 bone RR
ASSIST abs 1 absslip 0.25 absrelease 8 tcs 0 tcsslip 0.35 steerin 3 steerout 5 throttlein 6 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.3 holdspeed 30 skidslip 0.45 skidangle 12
SEAT driver 10 -30 50 exit -10 -120 0
CAMERA modes chase hood arm 520 eye 20 -30 95 hood 120 0 80
"""

RIG = """OCRIG 2
# The wheel bones of RiggedCar.gltf. The vehicle system sets, for every WHEEL with a `bone`, the inputs wheel_<bone>_pos (the hub, vehicle
# origin relative, with the suspension travel in it) and wheel_<bone>_rot (steer then spin); these setbone ops put the bones there.
NAME RiggedCar
""" + "".join(
    "INPUT wheel_%s_pos vec3 space=model default=%s\nINPUT wheel_%s_rot quat space=model default=0,0,0,1\n" % (b, hub, b)
    for b, hub in (("FL", "120,-72,30"), ("FR", "120,72,30"), ("RL", "-120,-72,30"), ("RR", "-120,72,30"))) + "".join(
    "OP setbone bone=%s pos=@wheel_%s_pos rot=@wheel_%s_rot space=model weight=1\n" % (b, b, b) for b in ("FL", "FR", "RL", "RR"))

# The same scheme as the Vehicle template's (copied by make_vehicle_template.py's text; kept in step by tests/vehicle/OcVehicleTest).
def template_input():
    with open(os.path.normpath(os.path.join(HERE, "..", "..", "templates", "Vehicle", "Content", "Input", "Vehicle.ocinput")), encoding="utf-8") as fh:
        return fh.read()


# ----------------------------------------------------------------------------------------------------------- meshes

def subdivided_box(name, hx, hy, hz, n, cx=0.0, cy=0.0, cz=0.0):
    """A box whose faces are n x n quads, shared vertices, for a Destructible (a cube's 12 triangles are too few to tear)."""
    m = geo.Mesh(name)
    seen = {}
    pts = []

    def vert(p):
        key = tuple(round(c, 3) for c in p)
        if key not in seen:
            seen[key] = len(pts)
            pts.append(p)
        return seen[key]

    faces = []
    def face(origin, du, dv):
        for j in range(n):
            for i in range(n):
                def at(a, b):
                    return vert((cx + origin[0] + du[0] * a / n + dv[0] * b / n, cy + origin[1] + du[1] * a / n + dv[1] * b / n,
                                 cz + origin[2] + du[2] * a / n + dv[2] * b / n))
                faces.append((at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)))

    sx, sy, sz = 2 * hx, 2 * hy, 2 * hz
    face((-hx, -hy, hz), (sx, 0, 0), (0, sy, 0))
    face((-hx, -hy, -hz), (sx, 0, 0), (0, sy, 0))
    face((hx, -hy, -hz), (0, sy, 0), (0, 0, sz))
    face((-hx, -hy, -hz), (0, sy, 0), (0, 0, sz))
    face((-hx, hy, -hz), (sx, 0, 0), (0, 0, sz))
    face((-hx, -hy, -hz), (sx, 0, 0), (0, 0, sz))
    centre = (cx, cy, cz)
    for q in faces:
        # smooth-ish shared vertices, one normal per vertex from the box centre direction
        idx = []
        for k in q:
            p = pts[k]
            d = geo.unit(geo.sub(p, centre))
            idx.append(m._vertex(p, d, m._uv(p, d)))
        fc = geo.centroid([pts[k] for k in q])
        n_out = geo.unit(geo.sub(fc, centre))
        # face normal from the quad itself, pointing away from the centre
        nrm = geo.unit(geo.cross(geo.sub(pts[q[1]], pts[q[0]]), geo.sub(pts[q[2]], pts[q[0]])))
        if geo.dot(nrm, n_out) < 0:
            nrm = (-nrm[0], -nrm[1], -nrm[2])
        m._tri(idx[0], idx[1], idx[2], nrm)
        m._tri(idx[0], idx[2], idx[3], nrm)
    return m


def truck_wheel():
    return geo.wheel_mesh("TruckWheel", 50.0, 30.0)


def hatch_wheel():
    return geo.wheel_mesh("HatchWheel", 30.0, 20.0)


MESHES = {
    "Truck": geo.truck_body("Truck"),
    "TruckWheel": truck_wheel(),
    "Hatchback": geo.hatchback_body("Hatchback"),
    "HatchWheel": hatch_wheel(),
    "TankHull": geo.tank_hull("TankHull"),
    "TankWheel": geo.wheel_mesh("TankWheel", 40.0, 50.0),
    "TankTurret": geo.tank_turret("TankTurret"),
    "CrashPanel": subdivided_box("CrashPanel", 12.0, 300.0, 100.0, 10),
}


# ----------------------------------------------------------------------------------------------------------- the rigged car glTF

def rigged_car_gltf():
    """A box body and four wheels as ONE skinned mesh: joints Root, FL, FR, RL, RR, every vertex bound fully to one joint."""
    hubs = {"FL": (120.0, -72.0, 30.0), "FR": (120.0, 72.0, 30.0), "RL": (-120.0, -72.0, 30.0), "RR": (-120.0, 72.0, 30.0)}
    parts = []
    body = geo.Mesh("body")
    body.box(0, 0, 52, 170, 66, 22)
    body.frustum(-10, 0, 90, 80, 60, 56, 50, 16, top_dx=-6)
    parts.append((body, 0))
    for i, name in enumerate(("FL", "FR", "RL", "RR")):
        w = geo.Mesh(name)
        h = hubs[name]
        w.cylinder("y", h[0], h[1], h[2], 30.0, 10.0, 20)
        w.cylinder("y", h[0], h[1], h[2], 19.0, 10.6, 20)
        w.box(h[0], h[1] + (11.0 if h[1] > 0 else -11.0), h[2], 17.0, 0.6, 3.0)
        w.box(h[0], h[1] + (11.0 if h[1] > 0 else -11.0), h[2], 3.0, 0.6, 17.0)
        parts.append((w, i + 1))

    pos, nrm, uv, jnt, idx = [], [], [], [], []
    for mesh, joint in parts:
        base = len(pos)
        pos += mesh.verts
        nrm += mesh.norms
        uv += mesh.uvs
        jnt += [joint] * len(mesh.verts)
        idx += [(a + base, b + base, c + base) for a, b, c in mesh.tris]

    blob = bytearray()

    def add(data, target):
        while len(blob) % 4:
            blob.append(0)
        start = len(blob)
        blob.extend(data)
        return {"buffer": 0, "byteOffset": start, "byteLength": len(data), "target": target} if target else {"buffer": 0, "byteOffset": start, "byteLength": len(data)}

    views, accessors = [], []

    def accessor(view, ctype, count, atype, mn=None, mx=None):
        a = {"bufferView": view, "componentType": ctype, "count": count, "type": atype}
        if mn is not None:
            a["min"], a["max"] = mn, mx
        accessors.append(a)
        return len(accessors) - 1

    flat = [c for p in pos for c in p]
    views.append(add(struct.pack("<%df" % len(flat), *flat), 34962))
    a_pos = accessor(0, 5126, len(pos), "VEC3", [min(p[i] for p in pos) for i in range(3)], [max(p[i] for p in pos) for i in range(3)])
    flat = [c for p in nrm for c in p]
    views.append(add(struct.pack("<%df" % len(flat), *flat), 34962))
    a_nrm = accessor(1, 5126, len(nrm), "VEC3")
    flat = [c for p in uv for c in p]
    views.append(add(struct.pack("<%df" % len(flat), *flat), 34962))
    a_uv = accessor(2, 5126, len(uv), "VEC2")
    views.append(add(b"".join(struct.pack("<4B", j, 0, 0, 0) for j in jnt), 34962))
    a_jnt = accessor(3, 5121, len(jnt), "VEC4")
    views.append(add(b"".join(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) for _ in jnt), 34962))
    a_wgt = accessor(4, 5126, len(jnt), "VEC4")
    flat = [i for t in idx for i in t]
    views.append(add(struct.pack("<%dH" % len(flat), *flat), 34963))
    a_idx = accessor(5, 5123, len(flat), "SCALAR")

    # Inverse bind matrices (column-major): the inverse of each joint node's world bind transform = translate(-jointPosition).
    joint_pos = [(0.0, 0.0, 0.0)] + [geo.to_obj(hubs[n]) for n in ("FL", "FR", "RL", "RR")]
    ibm = b"".join(struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p[0], -p[1], -p[2], 1) for p in joint_pos)
    views.append(add(ibm, None))
    a_ibm = accessor(6, 5126, 5, "MAT4")

    nodes = [
        {"name": "RiggedCar", "children": [1, 6]},
        {"name": "Root", "children": [2, 3, 4, 5]},
    ]
    for n in ("FL", "FR", "RL", "RR"):
        nodes.append({"name": n, "translation": [round(c, 6) for c in geo.to_obj(hubs[n])]})
    nodes.append({"name": "Body", "mesh": 0, "skin": 0})
    gltf = {
        "asset": {"version": "2.0", "generator": "make_vehicle_lab.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": nodes,
        "skins": [{"joints": [1, 2, 3, 4, 5], "skeleton": 1, "inverseBindMatrices": a_ibm}],
        "meshes": [{"name": "RiggedCar", "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv, "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
                                                         "indices": a_idx}]}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii")}],
    }
    return json.dumps(gltf, indent=1)


# ----------------------------------------------------------------------------------------------------------- the map

def vehicle_block(add, mesh, name, x, y, asset, wheels, extra=()):
    add("PLACE Meshes/%s.ocmesh %d %d 5 0 0 0 1 nocollide name %s" % (mesh, x, y, name))
    add("  COMPONENT Vehicle asset=%s" % asset)
    add("  BEGIN")
    for wname, wmesh, wx, wy, wz in wheels:
        add("    CHILD Meshes/%s.ocmesh %d %d %d 0 0 0 1 nocollide name %s" % (wmesh, wx, wy, wz, wname))
    for line in extra:
        add("    " + line)
    add("  END")


def map_text():
    o = []
    add = o.append
    add("OCWORLD 1")
    add("NAME VehicleLab")
    add("SPAWN -600 0 150 0")
    add("SUN elev 48.0 azim 130.0 color 1 0.97 0.92 lux 90000")
    add("SKY model physical")
    add("# Engine test content for the VEHICLES wave (docs/VEHICLES.md). Not a demo game. Every static shape is the built-in unit cube")
    add("# (Meshes/cube.ocmesh, half-extent 1 cm: the last three numbers of a PLACEG are half extents in centimetres); the vehicles and the crash")
    add("# panel are procedural meshes from make_vehicle_lab.py. +X is forward from the spawn. README.md has the checks.")
    add("# The default pawn flies. To drive: Play, select a vehicle, Details > Vehicle > Drive, then the Drive.* keys (W A S D, Space, E / Q, F, R, C).")
    add("PLACEG Meshes/cube.ocmesh 3000 0 -50 0 0 0 9000 4500 50 name Floor")
    add("")
    add("# The vehicles, in a row along Y at x = 0 (the row the spawn looks along).")

    def wheels(names, mesh, pts):
        return [(n, mesh, x, y, z) for n, (x, y, z) in zip(names, pts)]

    hatch = wheels(("WheelFL", "WheelFR", "WheelRL", "WheelRR"), "HatchWheel", ((125, -72, 30), (125, 72, 30), (-115, -72, 30), (-115, 72, 30)))
    vehicle_block(add, "Hatchback", "Hatchback", 0, -450, "Vehicles/Hatchback.ocvehicle", hatch)
    vehicle_block(add, "Truck", "Truck", 0, -1100, "Vehicles/Truck6x4.ocvehicle",
                  wheels(("WheelFL", "WheelFR", "WheelM1L", "WheelM1R", "WheelR1L", "WheelR1R"), "TruckWheel",
                         ((300, -110, 50), (300, 110, 50), (-100, -110, 50), (-100, 110, 50), (-300, -110, 50), (-300, 110, 50))))
    tank_wheels = [("Wheel%s%d" % (s, i), "TankWheel", x, y, 40) for s, y in (("L", -130), ("R", 130)) for i, x in enumerate((-280, -140, 0, 140, 280))]
    vehicle_block(add, "TankHull", "Tank", 0, 450, "Vehicles/Tank.ocvehicle", tank_wheels,
                  extra=("CHILD Meshes/TankTurret.ocmesh 30 0 130 0 0 0 1 nocollide name Turret",))
    # The rigged car: a skinned mesh whose wheels are bones. Components in the canonical order of the COMPONENT table (SkeletalMesh ... ControlRig ... Vehicle).
    add("PLACE Meshes/RiggedCar.ocmesh 0 1100 5 0 0 0 1 nocollide name RiggedCar")
    add("  COMPONENT SkeletalMesh skeleton=Meshes/RiggedCar.ocskel")
    add("  COMPONENT ControlRig rig=Rigs/RiggedCar.ocrig")
    add("  COMPONENT Vehicle asset=Vehicles/RiggedCar.ocvehicle")
    add("")
    add("# The ice-split lane, 40 m long from x = 800, along y = 2000: two strips side by side, IceLeft under a car's left wheels (y 1840..2000)")
    add("# and TarmacRight under its right ones (2000..2160). A second Hatchback waits at its start. The level format has no friction key, so make")
    add("# the ice by hand: select IceLeft, Details > Add Component > Rigid Body, Static, friction 0.05; leave TarmacRight as it is. Then drive")
    add("# Hatchback2 along +X and compare AXLE 1's `diff open | lsd | locked` (README.md).")
    vehicle_block(add, "Hatchback", "Hatchback2", 400, 2000, "Vehicles/Hatchback.ocvehicle", hatch)
    add("PLACEG Meshes/cube.ocmesh 2800 1920 1 0 0 0 2000 80 1 name IceLeft")
    add("PLACEG Meshes/cube.ocmesh 2800 2080 1 0 0 0 2000 80 1 name TarmacRight")
    add("")
    add("# A brake test strip along y = -1800: a post every 10 m from x = 800 (the post's name is the distance from the first one, in metres).")
    for i in range(0, 51, 10):
        add("PLACEG Meshes/cube.ocmesh %d -1800 40 0 0 0 10 60 40 name Brake%dm" % (800 + i * 100, i))
    add("")
    add("# A drop for the landing event: a 3 m platform (top at z = 300, edge at x = 6200) with a 20 m ramp up to it. Drive off the edge.")
    add("PLACEG Meshes/cube.ocmesh 3000 450 140 0 -8.53 0 1011 600 10 name DropRamp")
    add("PLACEG Meshes/cube.ocmesh 5100 450 150 0 0 0 1100 600 150 name DropPlatform")
    add("")
    add("# A slalom: twelve posts 6 m apart along y = -2600, alternately 1.3 m either side of it.")
    for i in range(12):
        add("PLACEG Meshes/cube.ocmesh %d %d 70 0 0 0 20 20 70 name Slalom%d" % (1000 + i * 600, -2600 + (130 if i % 2 == 0 else -130), i + 1))
    add("")
    add("# A crash wall at x = 7200 with a Destructible panel filling the gap in it (Meshes/CrashPanel.ocmesh: a subdivided box, so there are triangles")
    add("# to tear). Drive into the panel at speed: the vehicle takes damage (health, Crash event) and the panel dents and tears (docs/SOFTBODY.md).")
    add("PLACEG Meshes/cube.ocmesh 7200 -1000 100 0 0 0 20 700 100 name CrashWallLeft")
    add("PLACEG Meshes/cube.ocmesh 7200 800 100 0 0 0 20 500 100 name CrashWallRight")
    add("PLACE Meshes/CrashPanel.ocmesh 7200 0 100 0 0 0 1 nocollide name CrashPanel")
    add("  COMPONENT Destructible preset=wood")
    add("")
    add("# A 15 degree, 8 m ramp for the tank (it should climb and come down without flipping).")
    add("PLACEG Meshes/cube.ocmesh 1400 2800 90 0 -15 0 400 200 20 name TankRamp")
    return "\n".join(o) + "\n"


# ----------------------------------------------------------------------------------------------------------- cook

def cook(assetc):
    out = os.path.join(CONTENT, "Meshes")
    os.makedirs(out, exist_ok=True)
    jobs = [(os.path.join(SOURCE, n + ".obj"), n) for n in MESHES] + [(os.path.join(SOURCE, "RiggedCar.gltf"), "RiggedCar")]
    for src, base in jobs:
        cmd = [assetc, "convert", src, "--out-dir", out, "--base", base]
        res = subprocess.run(cmd, capture_output=True, text=True)
        lines = [l for l in res.stdout.splitlines() if l.startswith("{")]
        print("cook", base, "->", lines[0][:160] if lines else (res.stdout + res.stderr)[-300:])
        if res.returncode != 0:
            print("  (AverAssetC failed on %s: see README.md, the skeletal route is then covered by the headless test only)" % base)


def main():
    assetc = os.path.normpath(os.path.join(HERE, "..", "..", "build", "bin", "AverAssetC.exe"))
    if "--assetc" in sys.argv:
        assetc = sys.argv[sys.argv.index("--assetc") + 1]

    for name, mesh in MESHES.items():
        mesh.write_obj(os.path.join(SOURCE, name + ".obj"))
    with open(os.path.join(SOURCE, "RiggedCar.gltf"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(rigged_car_gltf())
    print("wrote Content/Source/RiggedCar.gltf")

    write("Vehicles/Truck6x4.ocvehicle", TRUCK)
    write("Vehicles/Hatchback.ocvehicle", HATCHBACK)
    write("Vehicles/Tank.ocvehicle", TANK)
    write("Vehicles/RiggedCar.ocvehicle", RIGGED)
    write("Rigs/RiggedCar.ocrig", RIG)
    write("Input/Vehicle.ocinput", template_input())
    write("Maps/VehicleLab.ocworld", map_text())

    if "--no-import" not in sys.argv:
        if os.path.exists(assetc):
            cook(assetc)
        else:
            print("AverAssetC not found (%s): cook the meshes with the command in README.md" % assetc)


if __name__ == "__main__":
    main()
