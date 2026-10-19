#!/usr/bin/env python3
"""Writes RallyDash, a small playable driving game made ONLY from content (docs/VEHICLES.md, docs/CAMERAS.md): the level (.ocworld) and
its traffic lanes (.oclanes), four Aver Node graphs (.ocgraph), the input scheme (.ocinput), the HUD (.ocui), the vehicle (.ocvehicle),
particles (.ocparticle), materials (.ocmat), a number table (.csv) for the HUD's digits, sounds, and the meshes it needs as .ocmesh.
Stdlib only, no third-party assets, no engine source touched. Deterministic and idempotent: run it as often as you like.

    python make_rally_dash.py              # writes everything under this folder, then cooks the three new meshes if AverAssetC is found
    python make_rally_dash.py --no-import  # skip the .ocmesh cook (Source/*.obj are still written)
    python make_rally_dash.py --assetc <path-to-AverAssetC>

It reuses templates/Vehicle (vehicle_geo.py for shapes and tones, graphkit.py to check every LINK against docs/nodes/*.md) by importing
them, and copies (never moves) the template's Sedan.ocmesh, Wheel.ocmesh, engine sounds and particle effects into Content/.
"""
import bisect
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
TEMPLATE = os.path.join(REPO, "templates", "Vehicle")
sys.path.insert(0, TEMPLATE)

import vehicle_geo as geo          # noqa: E402
from graphkit import Graph         # noqa: E402

CONTENT = os.path.join(HERE, "Content")
SOURCE = os.path.join(HERE, "Source")


def write(rel, text, base=CONTENT):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote", os.path.relpath(path, HERE).replace("\\", "/"))


def copy_from_template(rel):
    src = os.path.join(TEMPLATE, "Content", rel)
    dst = os.path.join(CONTENT, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    print("copied", rel)


def n(v):
    """A number as the level format likes it: short, no exponent."""
    s = "%.2f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


# ================================================================================================================ the track
# A closed loop around the origin, centimetres, +X forward at yaw 0, +Y right, +Z up, a yaw turns +X toward +Y. The centre line is a
# polar curve with a few harmonics, which gives long gentle rights, two lefts and one tight kink; it is resampled by arc length so the
# straight segments the level is built from are all about the same length.

ROAD_W = 1600.0           # 16 m of asphalt between the barriers
HALF = ROAD_W / 2.0
WALL_T = 80.0             # barrier thickness
WALL_H = 90.0             # barrier height
KERB_W = 120.0            # painted kerb strip on each edge
SEGMENTS = 140            # coarse segments of the loop (about 4.7 m each)
SUB = 14                  # fine samples per coarse segment, for placing things at an arc length
MIN_RADIUS_CM = 3000.0    # the generator refuses a loop with a tighter bend than this (the road would fold over itself)


def _centre_curve(theta):
    r = 9500.0 * (1.0 + 0.27 * math.cos(2 * theta) + 0.10 * math.cos(3 * theta + 1.1) + 0.04 * math.cos(5 * theta + 0.3))
    return (r * math.cos(theta), r * math.sin(theta))


class Track:
    def __init__(self):
        dense = 20000
        pts = [_centre_curve(2 * math.pi * j / dense) for j in range(dense)]
        cum = [0.0]
        for j in range(1, dense + 1):
            a, b = pts[j - 1], pts[j % dense]
            cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
        self.length = cum[-1]
        fine_n = SEGMENTS * SUB
        self.fine = []
        for i in range(fine_n):
            s = i * self.length / fine_n
            k = bisect.bisect_right(cum, s) - 1
            f = (s - cum[k]) / (cum[k + 1] - cum[k])
            a, b = pts[k], pts[(k + 1) % dense]
            self.fine.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
        self.fine_n = fine_n
        self.ds = self.length / fine_n
        self.coarse = [self.fine[i * SUB] for i in range(SEGMENTS)]
        # turn angle at each coarse point (radians, + = turning toward +Y) and the radius of curvature there
        self.turn = []
        self.radius = []
        for i in range(SEGMENTS):
            a, b, c = self.coarse[i - 1], self.coarse[i], self.coarse[(i + 1) % SEGMENTS]
            h1 = math.atan2(b[1] - a[1], b[0] - a[0])
            h2 = math.atan2(c[1] - b[1], c[0] - b[0])
            d = (h2 - h1 + math.pi) % (2 * math.pi) - math.pi
            ds = 0.5 * (math.hypot(b[0] - a[0], b[1] - a[1]) + math.hypot(c[0] - b[0], c[1] - b[1]))
            self.turn.append(d)
            self.radius.append(ds / max(abs(d), 1e-9))
        if min(self.radius) < MIN_RADIUS_CM:
            raise SystemExit("the loop has a bend of %.0f cm radius (limit %.0f): retune _centre_curve" % (min(self.radius), MIN_RADIUS_CM))
        # the straightest stretch (smoothed over +-5 segments) is where the start line goes
        best, best_i = 1e9, 0
        for i in range(SEGMENTS):
            sm = sum(abs(self.turn[(i + k) % SEGMENTS]) for k in range(-5, 6))
            if sm < best:
                best, best_i = sm, i
        self.s_start = best_i * self.length / SEGMENTS

    # ---- positions along the loop -----------------------------------------------------------------------------------------
    def _fine_index(self, s):
        s %= self.length
        f = s / self.ds
        i = int(math.floor(f)) % self.fine_n
        return i, f - math.floor(f)

    def pos(self, s, lateral=0.0):
        """World (x, y) at arc length s, `lateral` cm to the RIGHT of the direction of travel (negative = left)."""
        i, f = self._fine_index(s)
        a, b = self.fine[i], self.fine[(i + 1) % self.fine_n]
        x, y = a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f
        h = math.radians(self.heading(s))
        return (x - math.sin(h) * lateral, y + math.cos(h) * lateral)

    def heading(self, s):
        """Direction of travel at s, degrees, from the fine samples either side of it."""
        i, f = self._fine_index(s)
        a, b = self.fine[(i - 1) % self.fine_n], self.fine[(i + 2) % self.fine_n]
        return math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))

    def straight_near(self, s_target, window=4000.0):
        """The arc length within `window` of s_target where the loop is straightest (smoothed over about 14 m)."""
        best, best_s = 1e9, s_target
        steps = int(window * 2 / (self.length / SEGMENTS))
        for k in range(steps + 1):
            s = s_target - window + k * (self.length / SEGMENTS)
            idx = int(round((s % self.length) / (self.length / SEGMENTS))) % SEGMENTS
            sm = sum(abs(self.turn[(idx + j) % SEGMENTS]) for j in range(-2, 3))
            if sm < best:
                best, best_s = sm, s
        return best_s

    def clearance(self, x, y):
        """Distance from a point to the nearest point of the centre line."""
        return min(math.hypot(x - p[0], y - p[1]) for p in self.fine[::2])


TRACK = Track()
PERIM = TRACK.length

# Checkpoints (gates). Gate 0 is the start/finish line; the others follow in driving order. A gate is passed when the car's origin comes
# within GATE_RADIUS of the gate's centre (the graph tests this each tick: there is no overlap/trigger node to rely on).
GATES = 8
GATE_RADIUS = 1250.0
LAPS = 3
GATE_S = [TRACK.s_start + k * PERIM / GATES for k in range(GATES)]
GRID_BACK = 2600.0        # the car waits this far before the line


def gate_pose(k):
    s = GATE_S[k]
    x, y = TRACK.pos(s)
    return (x, y, TRACK.heading(s))


def respawn_pose(k):
    """Where R puts the car when the last checkpoint passed was k: on that gate's centre line, facing on. 0 is the grid slot."""
    if k == 0:
        s = GATE_S[0] - GRID_BACK
        x, y = TRACK.pos(s, -300.0)
        return (x, y, TRACK.heading(s))
    x, y, h = gate_pose(k)
    return (x, y, h)


class Rng:
    """A tiny deterministic generator, so the scenery is the same on every run."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state / 4294967296.0

    def range(self, lo, hi):
        return lo + (hi - lo) * self.next()


# ================================================================================================================ materials

# (base colour, metallic, roughness, emissive, alpha / blend). Built-in surface names (M_Concrete, M_Wall, M_Trim, M_Crate, M_Foliage,
# M_Bark, M_Rock, M_Metal, M_Accent) are used as they are; everything named M_Rally* is authored here.
MATERIALS = {
    "M_RallyAsphalt":   ((0.10, 0.10, 0.11), 0.0, 0.92, None, None),
    "M_RallyGrass":     ((0.13, 0.32, 0.09), 0.0, 0.95, None, None),
    "M_RallyKerbRed":   ((0.78, 0.07, 0.05), 0.0, 0.70, None, None),
    "M_RallyKerbWhite": ((0.90, 0.90, 0.88), 0.0, 0.70, None, None),
    "M_RallyBlack":     ((0.02, 0.02, 0.02), 0.0, 0.80, None, None),
    "M_RallyCarRed":    ((0.70, 0.04, 0.03), 0.55, 0.28, None, None),
    "M_RallyCarBlue":   ((0.05, 0.17, 0.72), 0.50, 0.30, None, None),
    "M_RallyCarYellow": ((0.92, 0.72, 0.06), 0.40, 0.35, None, None),
    "M_RallyTyre":      ((0.025, 0.025, 0.03), 0.0, 0.95, None, None),
    "M_RallyCone":      ((0.95, 0.32, 0.04), 0.0, 0.60, None, None),
    "M_RallyBlue":      ((0.07, 0.20, 0.65), 0.1, 0.55, None, None),
    "M_RallyOrange":    ((0.95, 0.45, 0.08), 0.1, 0.55, None, None),
    "M_RallyGate":      ((0.02, 0.25, 0.30), 0.0, 0.40, (0.0, 1.1, 1.4), None),
    "M_RallyGold":      ((0.90, 0.62, 0.05), 0.3, 0.35, (0.9, 0.55, 0.0), None),
    "M_RallyLamp":      ((1.00, 1.00, 0.90), 0.0, 0.40, (1.6, 1.5, 1.2), None),
    "M_RallyGlass":     ((0.62, 0.86, 0.95), 0.0, 0.05, None, 0.30),
}


def material_text(name, spec):
    base, metal, rough, emissive, alpha = spec
    o = ["OCMAT 1", "# %s: authored by make_rally_dash.py (flat colour, no textures)." % name, "SHADER standard"]
    o.append("BLEND translucent" if alpha is not None else "BLEND opaque")
    o += ["CULL back", "FLAGS twosided=0 castshadow=%d" % (0 if alpha is not None else 1), ""]
    o.append("PARAM baseColorFactor %s %s %s %s" % (n(base[0]), n(base[1]), n(base[2]), n(alpha if alpha is not None else 1.0)))
    o.append("PARAM metallicFactor %s" % n(metal))
    o.append("PARAM roughnessFactor %s" % n(rough))
    e = emissive or (0.0, 0.0, 0.0)
    o.append("PARAM emissiveFactor %s %s %s" % (n(e[0]), n(e[1]), n(e[2])))
    o.append("PARAM normalScale 1")
    o.append("PARAM occlusionStrength 1")
    return "\n".join(o) + "\n"


# ================================================================================================================ the vehicle

# The template's sedan, retuned for a lap game: a little less torque and a lot more drag so it tops out near 160 km/h on a straight,
# traction control on, no HUD record (the HUD is a graph here), 3000 health. Same wheels and mesh, so the Sedan.ocmesh / Wheel.ocmesh fit.
SEDAN = """OCVEHICLE 1
# RallyDash's car: the Vehicle template's rear-wheel-drive saloon, retuned for laps (less torque, more drag so it tops out near 160 km/h,
# traction control on, a stiffer hold at standstill). Units: centimetres, kilograms, N*m, degrees. +X forward, +Y right, +Z up, origin at
# the BOTTOM CENTRE of the car, where Meshes/Sedan.ocmesh has its own origin. A WHEEL's hub is the SETTLED wheel centre.
NAME RallySedan
KIND wheeled
BODY half 220 90 35 clearance 25 com 0 0 40 mass 1300 pitchroll 60
ENGINE torque 400 minrpm 1000 maxrpm 6000 inertia 0.5 damping 0.2
CURVE 0 0.8 0.66 1 1 0.8
GEARBOX auto forward 2.66 1.78 1.3 1 0.74 reverse 2.9 switch 0.5 clutchrelease 0.3 latency 0.5 up 0.6 down 0.2 clutch 10
AXLE 0 final 3.42 diff open split 0.5 lsd 1.4 share 0 antiroll 1000
AXLE 1 final 3.42 diff open split 0.5 lsd 1.4 share 0 antiroll 800
TYRE street long 0 0 0.06 1.2 0.2 1 lat 0 0 3 1.2 20 1
WHEELCLASS street radius 33 width 22 susp 5 30 hz 2 damping 0.5 brake 1500 handbrake 2500 inertia 0.9 spindamping 0.2 tyre street
WHEEL FL class street hub 180 -77 33 steer 35 driven 0 handbrake 0 node WheelFL
WHEEL FR class street hub 180 77 33 steer 35 driven 0 handbrake 0 node WheelFR
WHEEL RL class street hub -180 -77 33 steer 0 driven 1 handbrake 1 node WheelRL
WHEEL RR class street hub -180 77 33 steer 0 driven 1 handbrake 1 node WheelRR
ASSIST abs 1 absslip 0.25 absrelease 8 tcs 1 tcsslip 0.35 steerin 3 steerout 5 throttlein 6 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.5 holdspeed 30 skidslip 0.45 skidangle 12
STEERCURVE 0 1 1500 0.6 3500 0.35
AERO drag 1.4 downforce 1.2
SEAT driver 20 -35 55 exit -20 -140 0
AUDIO engine Sounds/Engine_Idle.wav idle 1000 Sounds/Engine_High.wav high 6000 pitchlow 0.8 pitchhigh 2.2 skid Sounds/Skid.wav impact Sounds/Crash.wav
EFFECT skid Particles/TyreSmoke.ocparticle rate 40 impact Particles/Sparks.ocparticle count 12
DAMAGE health 3000 crash 700 energy 0.5 radius 80
CAMERA modes chase hood cockpit arm 650 eye 25 -35 110 hood 150 0 95
"""

# ================================================================================================================ input

INPUT = """OCINPUT 1
NAME Rally
# RallyDash's ONE input scheme. The Drive.* actions are what Aver.Vehicle.Scene reads while somebody is driving (the Vehicle template's
# set, same names and bindings). Two differences on purpose: Drive.Exit and Drive.Reset are declared but UNBOUND -- the pawn stays in the
# car for the whole race (F would let it out), and R is the graph's own `Respawn` action (the car goes back to the last checkpoint),
# not the vehicle system's in-place flip. W / Up throttle, S / Down brake and reverse, A D / Left Right steer, Space handbrake.
ACTION Drive.Throttle axis1
ACTION Drive.Brake axis1
ACTION Drive.Steer axis1
ACTION Drive.Handbrake digital
ACTION Drive.ShiftUp digital
ACTION Drive.ShiftDown digital
ACTION Drive.Reset digital
ACTION Drive.Exit digital
ACTION Drive.CameraNext digital
ACTION Respawn digital

BIND Drive.Throttle key W
BIND Drive.Throttle key Up
BIND Drive.Throttle gamepadaxis RightTrigger
BIND Drive.Brake key S
BIND Drive.Brake key Down
BIND Drive.Brake gamepadaxis LeftTrigger
BIND Drive.Steer key D
BIND Drive.Steer key A scale -1
BIND Drive.Steer key Right
BIND Drive.Steer key Left scale -1
BIND Drive.Steer gamepadaxis LeftX
BIND Drive.Handbrake key Space
BIND Drive.Handbrake gamepadbutton A
BIND Drive.ShiftUp key E
BIND Drive.ShiftUp gamepadbutton RightShoulder
BIND Drive.ShiftDown key Q
BIND Drive.ShiftDown gamepadbutton LeftShoulder
BIND Drive.CameraNext key C
BIND Drive.CameraNext gamepadbutton RightThumb
BIND Respawn key R
BIND Respawn gamepadbutton Y

CONTEXT Rally priority 0
"""

# ================================================================================================================ particles

CELEBRATE = """OCPARTICLE 2
NAME Celebrate
# A burst of golden sparkles for a finished lap and the finish line (AN_SpawnParticlesAt). Self-lit, additive, SPAWNAT on.
EMITTER Sparkle
SIM auto
SHAPE sphere 40
BLEND additive
EMISSION 0 40 1024
LIFETIME 0.9 1.8
DIRECTION 0 0 1 70
SPEED 300 900
GRAVITY 0 0 -520
DAMPING 0.1
SIZE 7 1
COLOR start 1 0.85 0.3 1
COLOR end 1 0.3 0.1 0
SPRITE camera
SORT none
LIGHTING unlit
FOG on
SPAWNAT on
BOUNDS 1500
"""

# ================================================================================================================ the HUD

NUMBERS = "Data/Numbers.csv"      # content-relative; the table every digit on the HUD comes from
NUMBER_ROWS = 1000


def numbers_csv():
    """Aver Node has no number-to-text node, so the HUD's digits are looked up: row N of this table is the number N. Columns: Plain
    (N), Pad2 (N with two digits at least), Gear (row 0 = R, 1 = N, 2.. = 1..), Lap (row 0 = READY, 1..3 = LAP n/3) and Count (row 0 =
    GO!, 1..3)."""
    gears = ["R", "N", "1", "2", "3", "4", "5", "6", "7"]
    laps = ["READY"] + ["LAP %d/%d" % (i, LAPS) for i in range(1, LAPS + 1)]
    counts = ["GO!", "1", "2", "3"]
    rows = ["Name,Plain,Pad2,Gear,Lap,Count"]
    for i in range(NUMBER_ROWS):
        rows.append("%d,%d,%02d,%s,%s,%s" % (i, i, i, gears[i] if i < len(gears) else "",
                                              laps[i] if i < len(laps) else "", counts[i] if i < len(counts) else ""))
    return "\n".join(rows) + "\n"


class Ui:
    def __init__(self):
        self.lines = ["OCUI 1", "NAME RallyHud",
                      "# RallyDash's HUD. AN_RallyRace.ocgraph finds these widgets by name each tick and fills them: the digits come from",
                      "# Data/Numbers.csv (AN_SetUiTextFromTable), the bars from AN_SetUiValue, the panels' visibility from AN_SetUiVisible.",
                      "# Nothing here takes the pointer or the keyboard (passive root, hit never)."]

    def widget(self, name, kind, parent, **props):
        self.lines.append("WIDGET %s %s %s" % (name, kind, parent or "-"))
        for k, v in props.items():
            self.lines.append("SET %s %s %s" % (name, k, v))

    def text(self):
        return "\n".join(self.lines) + "\n"


def hud_text():
    u = Ui()
    # Colours: white, gold, cyan, red; panels are a translucent black.
    u.widget("Root", "panel", None, anchors="0 0 1 1", style="clear", inputMode="passive", hit="never", visible="false")

    # top left: lap and times
    u.widget("LapPanel", "panel", "Root", anchors="0 0", pivot="0 0", offsets="36 36 0 0", width=470, layout="vstack", spacing=4,
             padding=14, hit="never", bgColor="#000000A0")
    u.widget("LapText", "text", "LapPanel", text='"LAP 1/3"', fontSize=60, bold="true", textColor="#FFFFFF")
    u.widget("TimeRow", "panel", "LapPanel", layout="hstack", spacing=2, style="clear", hit="never")
    u.widget("TimeLabel", "text", "TimeRow", text='"TIME  "', fontSize=30, textColor="#9FB0C8")
    u.widget("TimeMin", "text", "TimeRow", text='"0"', fontSize=52, bold="true", textColor="#FFFFFF")
    u.widget("TimeC1", "text", "TimeRow", text='":"', fontSize=52, textColor="#FFFFFF")
    u.widget("TimeSec", "text", "TimeRow", text='"00"', fontSize=52, bold="true", textColor="#FFFFFF")
    u.widget("TimeDot", "text", "TimeRow", text='"."', fontSize=52, textColor="#FFFFFF")
    u.widget("TimeCs", "text", "TimeRow", text='"00"', fontSize=52, bold="true", textColor="#FFFFFF")
    u.widget("BestRow", "panel", "LapPanel", layout="hstack", spacing=2, style="clear", hit="never", visible="false")
    u.widget("BestLabel", "text", "BestRow", text='"BEST  "', fontSize=30, textColor="#FFD84A")
    u.widget("BestMin", "text", "BestRow", text='"0"', fontSize=40, bold="true", textColor="#FFD84A")
    u.widget("BestC1", "text", "BestRow", text='":"', fontSize=40, textColor="#FFD84A")
    u.widget("BestSec", "text", "BestRow", text='"00"', fontSize=40, bold="true", textColor="#FFD84A")
    u.widget("BestDot", "text", "BestRow", text='"."', fontSize=40, textColor="#FFD84A")
    u.widget("BestCs", "text", "BestRow", text='"00"', fontSize=40, bold="true", textColor="#FFD84A")

    # bottom right: speed, gear, health
    u.widget("SpeedPanel", "panel", "Root", anchors="1 1", pivot="1 1", offsets="-36 -36 0 0", width=520, layout="vstack", spacing=6,
             padding=14, hit="never", bgColor="#000000A0")
    u.widget("SpeedRow", "panel", "SpeedPanel", layout="hstack", spacing=10, style="clear", hit="never", justify="end")
    u.widget("SpeedValue", "text", "SpeedRow", text='"0"', fontSize=120, bold="true", textAlign="right", minWidth=250, textColor="#FFFFFF")
    u.widget("SpeedUnit", "text", "SpeedRow", text='"km/h"', fontSize=36, textColor="#9FB0C8", alignV="end")
    u.widget("Speed", "progress", "SpeedPanel", minValue=0, maxValue=200, height=16, accentColor="#3AA0FF")
    u.widget("GearRow", "panel", "SpeedPanel", layout="hstack", spacing=12, style="clear", hit="never", justify="end")
    u.widget("GearLabel", "text", "GearRow", text='"GEAR"', fontSize=32, textColor="#9FB0C8", alignV="end")
    u.widget("GearValue", "text", "GearRow", text='"N"', fontSize=64, bold="true", textAlign="center", minWidth=60, textColor="#FFD84A")
    u.widget("HealthLabel", "text", "SpeedPanel", text='"CAR HEALTH"', fontSize=24, textColor="#9FB0C8")
    u.widget("Health", "progress", "SpeedPanel", minValue=0, maxValue=1, height=14, accentColor="#5CE05C")

    # centre: countdown, checkpoint flash, wrecked warning
    u.widget("CountdownText", "text", "Root", anchors="0.5 0.30", pivot="0.5 0.5", text='"3"', fontSize=200, bold="true",
             textAlign="center", textColor="#FFD84A", hit="never")
    u.widget("CheckpointText", "text", "Root", anchors="0.5 0.13", pivot="0.5 0.5", text='"CHECKPOINT"', fontSize=56, bold="true",
             textAlign="center", textColor="#4DE8FF", hit="never", visible="false")
    u.widget("WreckedText", "text", "Root", anchors="0.5 0.80", pivot="0.5 0.5", text='"WRECKED  -  PRESS R"', fontSize=64, bold="true",
             textAlign="center", textColor="#FF4B3A", hit="never", visible="false")

    # finish
    u.widget("FinishPanel", "panel", "Root", anchors="0.5 0.45", pivot="0.5 0.5", width=900, layout="vstack", spacing=10, padding=30,
             hit="never", bgColor="#000000C0", visible="false")
    u.widget("FinTitle", "text", "FinishPanel", text='"FINISH!"', fontSize=140, bold="true", textAlign="center", textColor="#FFD84A")
    u.widget("FinTotalRow", "panel", "FinishPanel", layout="hstack", spacing=2, style="clear", hit="never", justify="center")
    u.widget("FinTotalLabel", "text", "FinTotalRow", text='"TOTAL TIME   "', fontSize=48, textColor="#9FB0C8")
    u.widget("FinMin", "text", "FinTotalRow", text='"0"', fontSize=72, bold="true", textColor="#FFFFFF")
    u.widget("FinC1", "text", "FinTotalRow", text='":"', fontSize=72, textColor="#FFFFFF")
    u.widget("FinSec", "text", "FinTotalRow", text='"00"', fontSize=72, bold="true", textColor="#FFFFFF")
    u.widget("FinDot", "text", "FinTotalRow", text='"."', fontSize=72, textColor="#FFFFFF")
    u.widget("FinCs", "text", "FinTotalRow", text='"00"', fontSize=72, bold="true", textColor="#FFFFFF")
    u.widget("FinBestRow", "panel", "FinishPanel", layout="hstack", spacing=2, style="clear", hit="never", justify="center")
    u.widget("FinBestLabel", "text", "FinBestRow", text='"BEST LAP   "', fontSize=40, textColor="#FFD84A")
    u.widget("FinBestMin", "text", "FinBestRow", text='"0"', fontSize=56, bold="true", textColor="#FFD84A")
    u.widget("FinBestC1", "text", "FinBestRow", text='":"', fontSize=56, textColor="#FFD84A")
    u.widget("FinBestSec", "text", "FinBestRow", text='"00"', fontSize=56, bold="true", textColor="#FFD84A")
    u.widget("FinBestDot", "text", "FinBestRow", text='"."', fontSize=56, textColor="#FFD84A")
    u.widget("FinBestCs", "text", "FinBestRow", text='"00"', fontSize=56, bold="true", textColor="#FFD84A")
    u.widget("FinHint", "text", "FinishPanel", text='"Stop and press Play to race again"', fontSize=30, textAlign="center", textColor="#9FB0C8")

    # bottom left: the keys
    u.widget("Keys", "text", "Root", anchors="0 1", pivot="0 1", offsets="36 -30 0 0",
             text='"W / S  gas, brake      A / D  steer      SPACE  handbrake      R  respawn at the last checkpoint      C  camera"',
             fontSize=24, textColor="#C8D2E4", hit="never")
    return u.text()


# ================================================================================================================ graphs

class Builder:
    """Wraps graphkit.Graph with an exec-chain cursor (`tails`: the open exec outputs the next exec node hangs from) and expression
    helpers. Every pure expression is built from NEW nodes (only constants and Param are shared): a pure node is recomputed per reader
    in a PUSH graph, so a GetVar built after a SetVar reads the new value, and nothing here depends on how that is cached."""

    def __init__(self, g):
        self.g = g
        self.count = 0
        self.tails = []
        self.consts = {}

    def uid(self, prefix):
        self.count += 1
        return "%s%d" % (prefix, self.count)

    # ---- constants ---------------------------------------------------------------------------------------------------
    def cf(self, v):
        key = ("f", float(v))
        if key not in self.consts:
            text = "%.1f" % v if float(v) == int(v) else "%.6g" % v
            ident = "kf_%s" % text.replace("-", "m").replace(".", "_")
            self.g.node(ident, "ConstFloat", value=text)
            self.consts[key] = (ident, "value")
        return self.consts[key]

    def ci(self, v):
        key = ("i", int(v))
        if key not in self.consts:
            ident = "ki_%s" % str(int(v)).replace("-", "m")
            self.g.node(ident, "ConstInt", value=str(int(v)))
            self.consts[key] = (ident, "value")
        return self.consts[key]

    def cb(self, v):
        key = ("b", bool(v))
        if key not in self.consts:
            ident = "kb_%s" % ("true" if v else "false")
            self.g.node(ident, "ConstBool", value="true" if v else "false")
            self.consts[key] = (ident, "value")
        return self.consts[key]

    def coerce(self, x):
        if isinstance(x, tuple):
            return x
        if isinstance(x, bool):
            return self.cb(x)
        return self.cf(float(x))

    # ---- pure nodes --------------------------------------------------------------------------------------------------
    def pure(self, typ, pins=None, out="result", **attrs):
        ident = self.uid(typ.lower().replace("an_", "")[:8] + "_")
        self.g.node(ident, typ, **attrs)
        for pin, ref in (pins or {}).items():
            r = self.coerce(ref)
            self.g.link(r[0], r[1], ident, pin)
        return (ident, out)

    def node(self, typ, pins=None, **attrs):
        """A pure node whose outputs the caller picks by name: returns the node id."""
        ident = self.uid(typ.lower().replace("an_", "")[:8] + "_")
        self.g.node(ident, typ, **attrs)
        for pin, ref in (pins or {}).items():
            r = self.coerce(ref)
            self.g.link(r[0], r[1], ident, pin)
        return ident

    def getv(self, name):
        return self.pure("GetVar", out="value", var=name)

    def add(self, a, b): return self.pure("Add", {"a": a, "b": b})
    def sub(self, a, b): return self.pure("Subtract", {"a": a, "b": b})
    def mul(self, a, b): return self.pure("Multiply", {"a": a, "b": b})
    def div(self, a, b): return self.pure("Divide", {"a": a, "b": b})
    def mod(self, a, b): return self.pure("Mod", {"a": a, "b": b})
    def mn(self, a, b): return self.pure("Min", {"a": a, "b": b})
    def mx(self, a, b): return self.pure("Max", {"a": a, "b": b})
    def clamp(self, a, lo, hi): return self.pure("Clamp", {"a": a, "min": lo, "max": hi})
    def floor(self, a): return self.pure("Floor", {"a": a})
    def ceil(self, a): return self.pure("Ceil", {"a": a})
    def absf(self, a): return self.pure("Abs", {"a": a})
    def sel(self, c, a, b): return self.pure("Select", {"cond": c, "ifTrue": a, "ifFalse": b})
    def lt(self, a, b): return self.pure("Less", {"a": a, "b": b})
    def gt(self, a, b): return self.pure("Greater", {"a": a, "b": b})
    def ge(self, a, b): return self.pure("GreaterEqual", {"a": a, "b": b})
    def eq(self, a, b): return self.pure("Equal", {"a": a, "b": b})
    def ne(self, a, b): return self.pure("NotEqual", {"a": a, "b": b})
    def and_(self, a, b): return self.pure("And", {"a": a, "b": b})
    def or_(self, a, b): return self.pure("Or", {"a": a, "b": b})
    def not_(self, a): return self.pure("Not", {"a": a})
    def i2f(self, a): return self.pure("IntToFloat", {"a": a})
    def f2i(self, a): return self.pure("FloatToInt", {"a": a})

    def row(self, value, hi):
        """A float as a table row (int) clamped to 0..hi."""
        return self.f2i(self.floor(self.clamp(value, 0.0, float(hi))))

    # ---- exec nodes --------------------------------------------------------------------------------------------------
    def link_tails(self, ident):
        if not self.tails:
            raise SystemExit("graph builder: an exec node was added after a branch or sequence ended the chain (no merging: see Builder.br)")
        for node, pin in self.tails:
            self.g.link(node, pin, ident, "exec")

    def x(self, typ, pins=None, **attrs):
        ident = self.uid(typ.lower().replace("an_", "")[:8] + "_")
        self.g.node(ident, typ, **attrs)
        for pin, ref in (pins or {}).items():
            r = self.coerce(ref)
            self.g.link(r[0], r[1], ident, pin)
        self.link_tails(ident)
        self.tails = [(ident, "then")]
        return ident

    def setv(self, name, ref):
        return self.x("SetVar", {"value": ref}, var=name)

    def br(self, cond, yes=None, no=None):
        """A Branch that ENDS the chain it is in. GraphCompiler.EmitExecNode inlines the rest of a chain into EVERY arm that reaches it (a
        diamond is emitted twice, "rather than joined with a jump"), so two arms may never flow into one node: a run of N merging branches
        would be emitted 2^N times. Whatever must run after a branch goes into a sibling arm of a Sequence (see seq)."""
        ident = self.uid("br_")
        self.g.node(ident, "Branch")
        self.link_tails(ident)
        r = self.coerce(cond)
        self.g.link(r[0], r[1], ident, "cond")
        if yes:
            self.tails = [(ident, "true")]
            yes()
        if no:
            self.tails = [(ident, "false")]
            no()
        self.tails = []
        return ident

    def seq(self, *parts):
        """Runs each part (a function that adds exec nodes) one after the other, each from its own Sequence output, so nothing merges.
        Built as nested two-output Sequence nodes (the default pins then0 and then1)."""
        parts = list(parts)
        if len(parts) == 1:
            parts[0]()
            self.tails = []
            return
        ident = self.uid("seq_")
        self.g.node(ident, "Sequence")
        self.link_tails(ident)
        self.tails = [(ident, "then0")]
        parts[0]()
        self.tails = [(ident, "then1")]
        self.seq(*parts[1:])
        self.tails = []

    # ---- game vocabulary -----------------------------------------------------------------------------------------------
    def sound(self, path, pitch=1.0, volume=1.0):
        self.x("PlaySound", {"volume": volume, "pitch": pitch, "looping": self.cb(False), "bus": self.ci(0)}, sound=path)

    def ui_find(self, name):
        ident = self.x("AN_FindUiWidget", name=name)
        return (ident, "widget")

    def ui_text(self, widget, column, row_ref):
        w = self.ui_find(widget)
        self.x("AN_SetUiTextFromTable", {"widget": w, "row": row_ref}, table=NUMBERS, column=column)

    def ui_visible(self, widget, cond):
        w = self.ui_find(widget)
        self.x("AN_SetUiVisible", {"widget": w, "visible": cond})

    def ui_value(self, widget, value):
        w = self.ui_find(widget)
        self.x("AN_SetUiValue", {"widget": w, "value": value})


SOUND_BEEP = "Sounds/Beep.wav"
SOUND_GO = "Sounds/Go.wav"
SOUND_CHECKPOINT = "Sounds/Checkpoint.wav"
SOUND_LAP = "Sounds/Lap.wav"
SOUND_FINISH = "Sounds/Finish.wav"
FX_CELEBRATE = "Particles/Celebrate.ocparticle"


def graph_rules():
    g = Graph("AN_RallyRules", "GameMode", "The game mode, as a graph: the play session's pawn, controller and a round clock.",
              class_attrs="pawn=AN_RallyDriver controller=AN_RallyController",
              header_comment="""THE GAME MODE. It names the pawn and the controller, which is what makes the play session spawn and possess them (the
FirstPerson template's AN_FPRules explains why both are required). The race itself is AN_RallyRace, an actor placed in the level.""")
    g.param("deltaTime", "float")
    g.var("elapsed", "float", "0")
    g.node("dt", "Param", param="deltaTime")
    g.entry("tick", "OnTick")
    g.node("tick", "OnTick")
    g.node("prev", "GetVar", var="elapsed")
    g.node("sum", "Add")
    g.link("prev", "value", "sum", "a")
    g.link("dt", "value", "sum", "b")
    g.node("store", "SetVar", var="elapsed")
    g.link("tick", "exec", "store", "exec")
    g.link("sum", "result", "store", "value")
    g.node("report", "GetVar", var="elapsed")
    g.out("report", "value")
    return g


def graph_controller():
    return Graph("AN_RallyController", "PlayerController",
                 "The player controller. Empty on purpose: it exists so the game mode has something concrete to possess with.",
                 header_comment="""THE CONCRETE PLAYER CONTROLLER. The engine's built-in PlayerController is abstract and cannot be spawned, and begin_play
possesses a pawn only when it has both. Input is read by the vehicle system and by AN_RallyRace, so nothing else belongs here.""")


def graph_driver():
    g = Graph("AN_RallyDriver", "Character", "The player's pawn: it sits in the car; it hears the car's crash and landing events and shakes the camera.",
              header_comment="""THE PAWN. It has no mesh and no logic of its own: AN_RallyRace puts it in the car (AN_VehicleEnter) when the countdown ends, and from
then on the vehicle system hides it and parks it in the seat. It exists as a graph so that it HEARS the car: the vehicle system raises
VehicleCrash, VehicleLanded ... at the vehicle AND at its driver (GameSystemsWiring.hpp, vehicleNotifyThunk), so these two entries run on
this pawn while it drives. A hard hit shakes the camera; a landing after a jump punches it down a little.""")
    b = Builder(g)
    g.entry("crash", "VehicleCrash")
    g.node("crash", "AN_OnEvent")
    b.tails = [("crash", "exec")]
    sedan = b.node("FindEntity", name="Sedan")
    pos = b.node("GetWorldPosition", {"entity": (sedan, "entity")})
    b.x("AN_CameraShakeAt", {"x": (pos, "x"), "y": (pos, "y"), "z": (pos, "z"), "radiusCm": 2500.0, "intensity": 0.7})
    g.entry("land", "VehicleLanded")
    g.node("land", "AN_OnEvent")
    b.tails = [("land", "exec")]
    b.x("AN_CameraPunch", {"pitchDeg": 2.5, "yawDeg": 0.0, "rollDeg": 0.0, "recoverSeconds": 0.45})
    return g


def graph_race(gate_poses, respawns):
    g = Graph("AN_RallyRace", "Actor", "The race: countdown, gates, laps, lap times, respawn, and the HUD (a placed actor).",
              header_comment="""THE RACE. One actor, placed in the level (PLACE none ... class AN_RallyRace); everything the game does lives here as a small state machine
in float VARs, so nothing needs to be shared between graphs (a graph cannot pass a number to another: FireEvent carries no payload).

    phase 0  waiting     until the play session exists, the pawn is possessed and the car is built; then everything below starts
    phase 1  countdown   3 - 2 - 1, the car is held (handbrake) and has NO driver, so the Drive.* actions cannot move it
    phase 2  racing      GO!: the pawn gets in (AN_VehicleEnter), the clock runs, gates are tested every tick by distance
    phase 3  finished    after LAPS laps; the car stays driveable, the HUD shows the total time

Checkpoints are tested by POSITION: each tick the car's distance from the next gate's centre (a constant) is compared with the gate radius.
Gate 0 is the start/finish line; gates 1.. come in driving order; passing gate 0 with every other gate behind completes a lap.
Numbers on the HUD are looked up in Data/Numbers.csv (row N is the number N), because a graph has no number-to-text node.
The graph is ticked in the editor before Play too, so phase 0 waits for IsPlaying and a possessed pawn.""")
    g.param("entity", "int")
    g.param("deltaTime", "float")
    for name, init in (("phase", 0), ("t", 0), ("lap", 1), ("lapTime", 0), ("totalTime", 0), ("bestLap", 0), ("nextGate", 1),
                       ("lastGate", 0), ("goTimer", 0), ("cpFlash", 99), ("lastCount", 4), ("camDone", 0)):
        g.var(name, "float", str(init))
    g.var("sedanE", "int", "0")
    b = Builder(g)
    g.node("dt", "Param", param="deltaTime")
    DT = ("dt", "value")

    def sedan():
        return b.getv("sedanE")

    def pawn():
        return b.pure("GetPlayerPawn", {"index": b.ci(0)}, out="entity")

    # ---------------------------------------------------------------------------------------------------- OnStart: the HUD
    g.entry("start", "OnStart")
    g.node("start", "OnStart")
    b.tails = [("start", "exec")]
    root = b.x("AN_FindUiWidget", name="Root")
    b.br(b.not_((root, "success")), lambda: b.x("AN_OpenUiLayout", path="UI/RallyHud.ocui"))

    # ---------------------------------------------------------------------------------------------------- OnTick
    # One Sequence of independent statements (see Builder.br for why nothing may merge): each is "if this phase, do this".
    g.entry("tick", "OnTick")
    g.node("tick", "OnTick")
    b.tails = [("tick", "exec")]

    # ---- phase 0: wait for the session, then arm the race ----
    def stmt_wait():
        def phase0():
            playing = b.pure("IsPlaying", out="playing")
            pawn_ok = b.gt(b.i2f(pawn()), 0.5)
            found = b.node("FindEntity", name="Sedan")
            built = b.node("AN_VehicleFromEntity", {"entity": (found, "entity")})
            ok = b.and_(b.and_(playing, pawn_ok), b.and_((found, "found"), (built, "found")))

            def arm():
                b.setv("sedanE", (found, "entity"))
                for name, v in (("phase", 1), ("t", 0), ("lap", 1), ("lapTime", 0), ("totalTime", 0), ("bestLap", 0), ("nextGate", 1),
                                ("lastGate", 0), ("goTimer", 0), ("cpFlash", 99), ("lastCount", 4), ("camDone", 0)):
                    b.setv(name, float(v))
                b.x("AN_CameraSetViewTarget", {"entity": sedan(), "blendSeconds": 0.6})
                b.x("AN_VehicleDrive", {"entity": sedan(), "throttle": 0.0, "steer": 0.0, "brake": 0.0, "handbrake": 1.0})
                r = b.ui_find("Root")
                b.x("AN_SetUiVisible", {"widget": r, "visible": b.cb(True)})
            b.br(ok, arm)
        b.br(b.eq(b.getv("phase"), 0.0), phase0)

    # ---- phase 1: 3 - 2 - 1 - GO ----
    def stmt_countdown():
        def tick_clock():
            b.setv("t", b.add(b.getv("t"), DT))
            b.x("AN_VehicleDrive", {"entity": sedan(), "throttle": 0.0, "steer": 0.0, "brake": 0.0, "handbrake": 1.0})

        def beeps():
            count = b.ceil(b.sub(3.0, b.getv("t")))

            def beep():
                b.sound(SOUND_BEEP)
                b.setv("lastCount", b.ceil(b.sub(3.0, b.getv("t"))))
            b.br(b.and_(b.ne(count, b.getv("lastCount")), b.gt(count, 0.5)), beep)

        def goes():
            def go():
                b.x("AN_VehicleDrive", {"entity": sedan(), "throttle": 0.0, "steer": 0.0, "brake": 0.0, "handbrake": 0.0})
                b.x("AN_VehicleEnter", {"entity": sedan(), "driver": pawn()})
                b.sound(SOUND_GO)
                b.x("AN_CameraPushFovKick")
                for name, v in (("phase", 2), ("goTimer", 0), ("lapTime", 0), ("totalTime", 0), ("camDone", 0)):
                    b.setv(name, float(v))
            b.br(b.ge(b.getv("t"), 3.0), go)
        b.br(b.eq(b.getv("phase"), 1.0), lambda: b.seq(tick_clock, beeps, goes))

    # ---- after GO: make sure somebody is driving (a failed enter retries) ----
    def stmt_driver():
        def ensure():
            driver = b.node("AN_VehicleDriver", {"entity": sedan()})
            b.br(b.eq(b.i2f((driver, "driver")), 0.0), lambda: b.x("AN_VehicleEnter", {"entity": sedan(), "driver": pawn()}))
        b.br(b.ge(b.getv("phase"), 2.0), ensure)

    # ---- phase 2: the race ----
    def stmt_race():
        def clocks():
            b.setv("lapTime", b.add(b.getv("lapTime"), DT))
            b.setv("totalTime", b.add(b.getv("totalTime"), DT))
            b.setv("goTimer", b.add(b.getv("goTimer"), DT))
            b.setv("cpFlash", b.add(b.getv("cpFlash"), DT))

        # the explicit chase rig on the car becomes the view once, shortly after the vehicle system has pointed the view at its own
        def reassert_cam():
            def reassert():
                b.x("AN_CameraSetViewTarget", {"entity": sedan(), "blendSeconds": 0.5})
                b.setv("camDone", 1.0)
            b.br(b.and_(b.lt(b.getv("camDone"), 0.5), b.gt(b.getv("goTimer"), 0.3)), reassert)

        pos = b.node("GetWorldPosition", {"entity": sedan()})

        def celebrate():
            b.x("AN_SpawnParticlesAt", {"x": (pos, "x"), "y": (pos, "y"), "z": b.add((pos, "z"), 120.0), "dirX": 0.0, "dirY": 0.0,
                                         "dirZ": 1.0, "count": b.ci(90)}, effect=FX_CELEBRATE)

        def lap_done():
            def best():
                b.br(b.or_(b.eq(b.getv("bestLap"), 0.0), b.lt(b.getv("lapTime"), b.getv("bestLap"))),
                     lambda: b.setv("bestLap", b.getv("lapTime")))

            def finish():
                b.setv("phase", 3.0)
                b.sound(SOUND_FINISH)
                celebrate()
                b.x("AN_CameraPushFovKick", {"deltaDeg": 12.0})

            def next_lap():
                b.setv("lap", b.add(b.getv("lap"), 1.0))
                b.setv("lapTime", 0.0)
                b.setv("nextGate", 1.0)
                b.setv("lastGate", 0.0)
                b.setv("cpFlash", 0.0)
                b.sound(SOUND_LAP)
                celebrate()
            b.seq(best, lambda: b.br(b.ge(b.getv("lap"), float(LAPS)), finish, next_lap))

        def gate_passed(i):
            def body():
                b.setv("lastGate", float(i))
                b.setv("nextGate", float((i + 1) % GATES))
                b.setv("cpFlash", 0.0)
                b.sound(SOUND_CHECKPOINT)
            return body

        def gate_test(i):
            def test():
                gx, gy, _ = gate_poses[i]
                dist = b.pure("AN_VecDistance2D", {"ax": (pos, "x"), "ay": (pos, "y"), "bx": gx, "by": gy})
                cond = b.and_(b.eq(b.getv("nextGate"), float(i)), b.lt(dist, GATE_RADIUS))
                b.br(cond, lap_done if i == 0 else gate_passed(i))
            return test
        b.br(b.eq(b.getv("phase"), 2.0), lambda: b.seq(clocks, reassert_cam, *[gate_test(i) for i in range(GATES)]))

    # ---- R: back to the last checkpoint, upright, repaired ----
    def stmt_respawn():
        def at_gate(i):
            def part():
                x, y, yaw = respawns[i]

                def place():
                    veh = b.node("AN_VehicleFromEntity", {"entity": sedan()})
                    b.x("AN_VehicleSetPose", {"vehicle": (veh, "vehicle"), "x": x, "y": y, "z": 40.0, "yaw": yaw})
                b.br(b.eq(b.getv("lastGate"), float(i)), place)
            return part

        def repair():
            b.x("AN_VehicleRepair", {"entity": sedan(), "amount": 1.0})
            b.x("AN_CameraCut", {"entity": b.ci(0)})
        pressed = b.pure("InputActionPressed", out="triggered", action="Respawn")
        b.br(b.and_(b.ge(b.getv("phase"), 2.0), pressed), lambda: b.seq(*[at_gate(i) for i in range(GATES)], repair))

    # ---- the HUD ----
    def stmt_hud():
        def hud():
            tel = b.node("AN_VehicleTelemetry", {"entity": sedan()})
            kmh = b.mn(b.mul(b.absf((tel, "speedCmS")), 0.036), 999.0)
            b.ui_text("SpeedValue", "Plain", b.row(kmh, NUMBER_ROWS - 1))
            b.ui_value("Speed", kmh)
            b.ui_text("GearValue", "Gear", b.row(b.add(b.i2f((tel, "gear")), 1.0), 8))
            b.ui_value("Health", b.sel((tel, "found"), (tel, "health"), 0.0))
            b.ui_text("LapText", "Lap", b.row(b.mn(b.getv("lap"), float(LAPS)), LAPS))

            def digits(prefix_min, prefix_sec, prefix_cs, seconds_ref_fn):
                minutes = b.floor(b.div(seconds_ref_fn(), 60.0))
                secs = b.floor(b.mod(seconds_ref_fn(), 60.0))
                cs = b.floor(b.mul(b.mod(seconds_ref_fn(), 1.0), 100.0))
                b.ui_text(prefix_min, "Plain", b.row(minutes, 99))
                b.ui_text(prefix_sec, "Pad2", b.row(secs, 99))
                b.ui_text(prefix_cs, "Pad2", b.row(cs, 99))
            digits("TimeMin", "TimeSec", "TimeCs", lambda: b.getv("lapTime"))
            digits("BestMin", "BestSec", "BestCs", lambda: b.getv("bestLap"))
            b.ui_visible("BestRow", b.gt(b.getv("bestLap"), 0.0))

            phase1_ = b.eq(b.getv("phase"), 1.0)
            show_count = b.or_(phase1_, b.and_(b.eq(b.getv("phase"), 2.0), b.lt(b.getv("goTimer"), 1.0)))
            count_row = b.sel(b.eq(b.getv("phase"), 1.0), b.ceil(b.sub(3.0, b.getv("t"))), 0.0)
            b.ui_text("CountdownText", "Count", b.row(count_row, 3))
            b.ui_visible("CountdownText", show_count)
            b.ui_visible("CheckpointText", b.and_(b.eq(b.getv("phase"), 2.0), b.lt(b.getv("cpFlash"), 1.5)))
            b.ui_visible("WreckedText", b.and_(b.and_((tel, "found"), b.ge(b.getv("phase"), 2.0)), b.lt((tel, "health"), 0.02)))
            b.ui_visible("FinishPanel", b.eq(b.getv("phase"), 3.0))

            def finish_digits():
                digits("FinMin", "FinSec", "FinCs", lambda: b.getv("totalTime"))
                digits("FinBestMin", "FinBestSec", "FinBestCs", lambda: b.getv("bestLap"))
            b.br(b.eq(b.getv("phase"), 3.0), finish_digits)
        b.br(b.ge(b.getv("phase"), 1.0), hud)

    b.seq(stmt_wait, stmt_countdown, stmt_driver, stmt_race, stmt_respawn, stmt_hud)
    return g


# ================================================================================================================ meshes

def subdivided_box(name, hx, hy, hz, nsub):
    """A box centred on the origin whose faces are nsub x nsub quads with shared vertices, for a Destructible (a cube's 12 triangles are
    too few to dent or tear). Same construction as test-content/VehicleLab's CrashPanel."""
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
        for j in range(nsub):
            for i in range(nsub):
                def at(a, b):
                    return vert((origin[0] + du[0] * a / nsub + dv[0] * b / nsub, origin[1] + du[1] * a / nsub + dv[1] * b / nsub,
                                 origin[2] + du[2] * a / nsub + dv[2] * b / nsub))
                faces.append((at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)))

    sx, sy, sz = 2 * hx, 2 * hy, 2 * hz
    face((-hx, -hy, hz), (sx, 0, 0), (0, sy, 0))
    face((-hx, -hy, -hz), (sx, 0, 0), (0, sy, 0))
    face((hx, -hy, -hz), (0, sy, 0), (0, 0, sz))
    face((-hx, -hy, -hz), (0, sy, 0), (0, 0, sz))
    face((-hx, hy, -hz), (sx, 0, 0), (0, 0, sz))
    face((-hx, -hy, -hz), (sx, 0, 0), (0, 0, sz))
    centre = (0.0, 0.0, 0.0)
    for q in faces:
        idx = []
        for k in q:
            p = pts[k]
            d = geo.unit(geo.sub(p, centre))
            idx.append(m._vertex(p, d, m._uv(p, d)))
        fc = geo.centroid([pts[k] for k in q])
        n_out = geo.unit(geo.sub(fc, centre))
        nrm = geo.unit(geo.cross(geo.sub(pts[q[1]], pts[q[0]]), geo.sub(pts[q[2]], pts[q[0]])))
        if geo.dot(nrm, n_out) < 0:
            nrm = (-nrm[0], -nrm[1], -nrm[2])
        m._tri(idx[0], idx[1], idx[2], nrm)
        m._tri(idx[0], idx[2], idx[3], nrm)
    return m


def cone_mesh():
    m = geo.Mesh("Cone")
    m.box(0, 0, 2, 26, 26, 2)
    m.cylinder("z", 0, 0, 29, 19, 25, 16, True, 4.0)
    return m


MESHES = {
    "Crate": subdivided_box("Crate", 50.0, 50.0, 50.0, 5),
    "GlassPane": subdivided_box("GlassPane", 6.0, 250.0, 130.0, 10),
    "Cone": cone_mesh(),
}


def cook_meshes(assetc):
    out = os.path.join(CONTENT, "Meshes")
    os.makedirs(out, exist_ok=True)
    for name in MESHES:
        cmd = [assetc, "convert", os.path.join(SOURCE, name + ".obj"), "--out-dir", out, "--base", name]
        res = subprocess.run(cmd, capture_output=True, text=True)
        lines = [l for l in res.stdout.splitlines() if l.startswith("{")]
        print("cook", name, "->", lines[0][:140] if lines else (res.stdout + res.stderr)[-300:])
        if res.returncode != 0:
            raise SystemExit("AverAssetC failed on %s" % name)


# ================================================================================================================ the level

CUBE = "Meshes/cube.ocmesh"
SPHERE = "Meshes/sphere.ocmesh"


class Level:
    def __init__(self):
        self.lines = []
        self.placements = 0

    def add(self, line):
        self.lines.append(line)

    def comment(self, text):
        self.lines.append("# " + text)

    def box(self, x, y, z, yaw, hx, hy, hz, material, name=None, collide=True, pitch=0.0):
        """A scaled unit cube (the cube mesh has half-extent 1 cm, so the last three numbers are half extents in centimetres)."""
        s = "PLACEG %s %s %s %s %s %s 0 %s %s %s %s" % (CUBE, n(x), n(y), n(z), n(yaw), n(pitch), n(hx), n(hy), n(hz), material)
        if not collide:
            s += " nocollide"
        if name:
            s += " name " + name
        self.lines.append(s)
        self.placements += 1

    def sphere(self, x, y, z, r, material):
        self.lines.append("PLACEG %s %s %s %s 0 0 0 %s %s %s %s" % (SPHERE, n(x), n(y), n(z), n(r), n(r), n(r), material))
        self.placements += 1

    def text(self):
        return "\n".join(self.lines) + "\n"


def strip_segments(level, lateral, half_width, z, half_height, material_for, collide, extend):
    """A continuous strip along the loop at `lateral` (centre of the strip), `half_width` cm wide each side, built from one box per coarse
    segment. Each box runs between the offset points of two neighbouring samples; it is lengthened by `extend` * tan(half the turn) so
    that the outside of a bend has no wedge gap. `material_for(i)` picks the box's material."""
    pts = []
    for i in range(SEGMENTS):
        x, y = TRACK.coarse[i]
        # direction of travel at the sample: from its two neighbours
        a, b = TRACK.coarse[i - 1], TRACK.coarse[(i + 1) % SEGMENTS]
        h = math.atan2(b[1] - a[1], b[0] - a[0])
        pts.append((x - math.sin(h) * lateral, y + math.cos(h) * lateral))
    for i in range(SEGMENTS):
        a, b = pts[i], pts[(i + 1) % SEGMENTS]
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dy)
        yaw = math.degrees(math.atan2(dy, dx))
        turn = max(abs(TRACK.turn[i]), abs(TRACK.turn[(i + 1) % SEGMENTS]))
        ext = extend * math.tan(turn * 0.5) + 3.0
        level.box((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0, z, yaw, length / 2.0 + ext, half_width, half_height, material_for(i), collide=collide)


def build_level():
    lv = Level()
    gx0, gy0, gyaw0 = gate_pose(0)
    sx, sy, syaw = respawn_pose(0)
    lv.add("OCWORLD 1")
    lv.add("NAME RallyDash")
    # No KILLZ: it is one of the six legacy .ocmap records, and one of them makes the loader read the whole file as
    # .ocmap, which drops every PLACEG placement (OcWorld.cpp levelFileIsLegacyOcmap).
    lv.add("GAMEMODE AN_RallyRules")
    lv.add("DEFAULTPAWN AN_RallyDriver")
    # the pawn waits beside the grid slot; it is moved into the car at GO
    px, py = TRACK.pos(GATE_S[0] - GRID_BACK - 900.0, 500.0)
    lv.add("SPAWN %s %s 150 %s" % (n(px), n(py), n(syaw)))
    lv.add("SUN elev 33.0 azim 215.0 color 1 0.96 0.88 lux 95000")
    lv.add("SKY model physical")
    lv.add("FOG exp density 0.000012 color 0.72 0.80 0.90")
    lv.add("POSTVOLUME infinite saturation 1.1 vignette 0.25")
    lv.comment("RallyDash: a lap game made only of content (make_rally_dash.py writes this file; edit the script, not the file). Centimetres, +X forward at yaw 0,")
    lv.comment("+Y right, +Z up. Every static shape is the built-in unit cube or sphere (half-extent 1 cm, so the last three numbers of a PLACEG are half extents).")
    lv.comment("The loop is %.0f m round, %d laps; 8 checkpoints (gate 0 is the start/finish line); traffic follows RallyDash.oclanes on the right." % (PERIM / 100.0, LAPS))

    # ---- ground and the horizon ring ----
    lv.comment("Ground: top at z = 0. A ring of rocks hides where it ends.")
    lv.box(0, 0, -50, 0, 20000, 20000, 50, "M_RallyGrass", name="Ground")
    rng = Rng(7)
    for k in range(40):
        a = 2 * math.pi * k / 40 + rng.range(-0.05, 0.05)
        rr = rng.range(18600, 19600)
        lv.box(rr * math.cos(a), rr * math.sin(a), 0, math.degrees(a) + rng.range(-25, 25), rng.range(900, 2200), rng.range(1100, 2200),
               rng.range(900, 3200), "M_Rock", pitch=rng.range(-6, 6))

    # ---- the road ----
    lv.comment("Road surface: one slab per segment of the loop (top at z = 2), painted kerbs on both edges (top at z = 3), then concrete barriers.")
    strip_segments(lv, 0.0, HALF + 10.0, -13.0, 15.0, lambda i: "M_RallyAsphalt", True, HALF)
    kerb_lat = HALF - KERB_W / 2.0
    for side in (-1.0, 1.0):
        strip_segments(lv, side * kerb_lat, KERB_W / 2.0, -2.0, 5.0,
                       lambda i, side=side: "M_RallyKerbRed" if (i + (0 if side < 0 else 1)) % 2 == 0 else "M_RallyKerbWhite", False, KERB_W)
    wall_lat = HALF + WALL_T / 2.0
    for side in (-1.0, 1.0):
        strip_segments(lv, side * wall_lat, WALL_T / 2.0, WALL_H / 2.0, WALL_H / 2.0,
                       lambda i, side=side: "M_RallyKerbWhite" if (i // 2 + (0 if side < 0 else 1)) % 2 == 0 else "M_RallyKerbRed", True, WALL_T)

    # ---- cones along the kerbs ----
    lv.comment("Cone rows on both kerbs, every fifth segment.")
    for i in range(0, SEGMENTS, 5):
        x, y = TRACK.coarse[i]
        a, b = TRACK.coarse[i - 1], TRACK.coarse[(i + 1) % SEGMENTS]
        h = math.atan2(b[1] - a[1], b[0] - a[0])
        for side in (-1.0, 1.0):
            lat = side * (HALF - 150.0)
            lv.add("PLACE Meshes/Cone.ocmesh %s %s 3 0 0 0 1 M_RallyCone" % (n(x - math.sin(h) * lat), n(y + math.cos(h) * lat)))
            lv.placements += 1

    # ---- start / finish line and checkpoint arches ----
    lv.comment("Start/finish line: a chequered strip across the road and a gold arch. Checkpoints 1.. are cyan arches with a glowing line.")
    h0 = math.radians(gyaw0)
    for row in (-1, 0):
        for col in range(16):
            lat = -HALF + 50.0 + col * 100.0
            fx, fy = TRACK.pos(GATE_S[0] + row * 100.0 + 50.0, lat)
            lv.box(fx, fy, 2.5, gyaw0, 50.0, 50.0, 0.5, "M_RallyBlack" if (col + row) % 2 == 0 else "M_RallyKerbWhite", collide=False)
    for k in range(GATES):
        s = GATE_S[k]
        x, y = TRACK.pos(s)
        yaw = TRACK.heading(s)
        hh = math.radians(yaw)
        mat_post = "M_RallyGold" if k == 0 else "M_RallyGate"
        for side in (-1.0, 1.0):
            lat = side * (HALF - 45.0)
            lv.box(x - math.sin(hh) * lat, y + math.cos(hh) * lat, 190.0, yaw, 32.0, 32.0, 190.0, mat_post)
        if k == 0:
            for col in range(16):
                lat = -HALF + 50.0 + col * 100.0
                bx, by = x - math.sin(hh) * lat, y + math.cos(hh) * lat
                lv.box(bx, by, 405.0, yaw, 30.0, 50.0, 45.0, "M_RallyBlack" if col % 2 == 0 else "M_RallyKerbWhite", collide=False)
        else:
            lv.box(x, y, 395.0, yaw, 28.0, HALF - 30.0, 26.0, "M_RallyGate", collide=False)
            lv.box(x, y, 3.0, yaw, 22.0, HALF - 20.0, 0.6, "M_RallyGate", collide=False)

    # ---- the player's car ----
    lv.comment("The player's car: COMPONENT Vehicle (Vehicles/Sedan.ocvehicle) plus a chase COMPONENT CameraRig; its four wheels are CHILD placements the")
    lv.comment("vehicle system moves every frame (the spec's `node` names). nocollide: the physics vehicle is the chassis.")
    lv.add("PLACE Meshes/Sedan.ocmesh %s %s 12 %s 0 0 1 M_RallyCarRed nocollide name Sedan" % (n(sx), n(sy), n(syaw)))
    lv.add("  COMPONENT Vehicle asset=Vehicles/Sedan.ocvehicle")
    lv.add("  COMPONENT CameraRig preset=vehiclechase arm=700 collision=0")
    lv.add("  BEGIN")
    for name, wx, wy in (("WheelFL", 180, -77), ("WheelFR", 180, 77), ("WheelRL", -180, -77), ("WheelRR", -180, 77)):
        lv.add("    CHILD Meshes/Wheel.ocmesh %d %d 33 0 0 0 1 M_RallyTyre nocollide name %s" % (wx, wy, name))
    lv.add("  END")
    lv.placements += 1

    # ---- traffic ----
    lv.comment("Two slow traffic cars: `vehicle <asset>` makes the placement a lane-following car on RallyDash.oclanes (the right-hand lane).")
    for s, mat in ((GATE_S[0] + 0.07 * PERIM, "M_RallyCarBlue"), (GATE_S[0] + 0.55 * PERIM, "M_RallyCarYellow")):
        x, y = TRACK.pos(s, TRAFFIC_LATERAL)
        yaw = TRACK.heading(s)
        lv.add("PLACE Meshes/Sedan.ocmesh %s %s 12 %s 0 0 1 %s nocollide vehicle Vehicles/Sedan.ocvehicle" % (n(x), n(y), n(yaw), mat))
        lv.add("  BEGIN")
        # Named like the player's: the spec's `node` names are how the vehicle system finds the wheels to spin.
        for wx, wy, wn in ((180, -77, "WheelFL"), (180, 77, "WheelFR"), (-180, -77, "WheelRL"), (-180, 77, "WheelRR")):
            lv.add("    CHILD Meshes/Wheel.ocmesh %d %d 33 0 0 0 1 M_RallyTyre nocollide name %s" % (wx, wy, wn))
        lv.add("  END")
        lv.placements += 1

    # ---- kickers ----
    lv.comment("Two kickers on the left half of the road (traffic keeps right): 10 degrees up over 8.9 m, then the ground falls away.")
    a_half, thick, pitch = 450.0, 25.0, 10.0
    zc = a_half * math.sin(math.radians(pitch)) - thick * math.cos(math.radians(pitch))
    for frac in (0.17, 0.62):
        s = TRACK.straight_near(GATE_S[0] + frac * PERIM, 3500.0)
        cx, cy = TRACK.pos(s + a_half * math.cos(math.radians(pitch)), -380.0)
        lv.box(cx, cy, zc, TRACK.heading(s), a_half, 250.0, thick, "M_RallyOrange", pitch=-pitch)

    # ---- things to smash ----
    lv.comment("Destructibles (COMPONENT Destructible): two crate pyramids and a glass pane, all on the left half of the road.")
    s_crates = TRACK.straight_near(GATE_S[0] + 0.36 * PERIM, 3500.0)
    for row_s in (s_crates, s_crates + 2600.0):
        yaw = TRACK.heading(row_s)
        tiers = ((50.0, (-700.0, -590.0, -480.0, -370.0)), (150.0, (-645.0, -535.0, -425.0)), (250.0, (-590.0, -480.0)))
        for z, lats in tiers:
            for lat in lats:
                cx, cy = TRACK.pos(row_s, lat)
                lv.add("PLACE Meshes/Crate.ocmesh %s %s %s %s 0 0 1 M_Crate nocollide" % (n(cx), n(cy), n(z), n(yaw)))
                lv.add("  COMPONENT Destructible preset=wood")
                lv.placements += 1
    s_glass = TRACK.straight_near(GATE_S[0] + 0.80 * PERIM, 3500.0)
    gx, gy = TRACK.pos(s_glass, -450.0)
    lv.add("PLACE Meshes/GlassPane.ocmesh %s %s 130 %s 0 0 1 M_RallyGlass nocollide" % (n(gx), n(gy), n(TRACK.heading(s_glass))))
    lv.add("  COMPONENT Destructible preset=glass")
    lv.placements += 1

    # ---- scenery ----
    lv.comment("Scenery: lamp posts, a pit building, buildings, billboards and trees, placed with a fixed seed and kept clear of the road.")
    for i in range(3, SEGMENTS, 7):
        x, y = TRACK.coarse[i]
        a, b = TRACK.coarse[i - 1], TRACK.coarse[(i + 1) % SEGMENTS]
        h = math.atan2(b[1] - a[1], b[0] - a[0])
        side = 1.0 if (i // 7) % 2 == 0 else -1.0
        lat = side * (HALF + WALL_T + 220.0)
        px_, py_ = x - math.sin(h) * lat, y + math.cos(h) * lat
        lv.box(px_, py_, 220.0, math.degrees(h), 14.0, 14.0, 220.0, "M_Trim")
        lv.box(px_, py_, 450.0, math.degrees(h), 40.0, 40.0, 14.0, "M_RallyLamp", collide=False)

    # a pit building opposite the start line
    pbx, pby = TRACK.pos(GATE_S[0] + 600.0, HALF + WALL_T + 900.0)
    lv.box(pbx, pby, 220.0, gyaw0, 1100.0, 380.0, 220.0, "M_Wall")
    lv.box(pbx, pby, 470.0, gyaw0, 1130.0, 400.0, 30.0, "M_RallyBlue")
    sgx, sgy = TRACK.pos(GATE_S[0] + 600.0, HALF + WALL_T + 505.0)
    lv.box(sgx, sgy, 330.0, gyaw0, 1000.0, 12.0, 80.0, "M_RallyGold", collide=False)

    rng = Rng(21)
    mats_b = ["M_Concrete", "M_Wall", "M_Trim", "M_RallyBlue", "M_RallyOrange", "M_Metal"]
    placed = 0
    tries = 0
    while placed < 26 and tries < 4000:
        tries += 1
        px_, py_ = rng.range(-16000, 16000), rng.range(-16000, 16000)
        hx, hy, hz = rng.range(400, 1300), rng.range(400, 1300), rng.range(500, 2800)
        if TRACK.clearance(px_, py_) < HALF + WALL_T + max(hx, hy) * 1.5 + 700.0:
            continue
        if math.hypot(px_ - TRACK.pos(GATE_S[0])[0], py_ - TRACK.pos(GATE_S[0])[1]) < 3500.0:
            continue
        lv.box(px_, py_, hz, rng.range(0, 90), hx, hy, hz, mats_b[placed % len(mats_b)])
        placed += 1
    for k in range(7):
        s = GATE_S[0] + (k + 0.5) * PERIM / 7.0
        bx, by = TRACK.pos(s, (1.0 if k % 2 == 0 else -1.0) * (HALF + WALL_T + 1500.0))
        yaw = TRACK.heading(s) + 90.0
        lv.box(bx, by, 150.0, yaw, 30.0, 30.0, 150.0, "M_Trim")
        lv.box(bx, by, 450.0, yaw + 90.0, 20.0, 380.0, 170.0, ("M_RallyOrange", "M_RallyBlue", "M_RallyCarRed")[k % 3], collide=False)
    placed = 0
    tries = 0
    while placed < 70 and tries < 8000:
        tries += 1
        px_, py_ = rng.range(-17000, 17000), rng.range(-17000, 17000)
        r_ = rng.range(180, 320)
        if TRACK.clearance(px_, py_) < HALF + WALL_T + 500.0 + r_:
            continue
        if math.hypot(px_ - TRACK.pos(GATE_S[0])[0], py_ - TRACK.pos(GATE_S[0])[1]) < 2500.0:
            continue
        lv.box(px_, py_, 110.0, rng.range(0, 90), 28.0, 28.0, 110.0, "M_Bark")
        lv.sphere(px_, py_, 220.0 + r_, r_, "M_Foliage")
        placed += 1

    # ---- the race itself ----
    lv.comment("The race: a graph class (Content/Scripts/AN_RallyRace.ocgraph). It counts down, tests the gates, keeps the laps and drives the HUD.")
    lv.add("PLACE none 0 0 0 0 0 0 1 class AN_RallyRace")
    return lv


TRAFFIC_LATERAL = 360.0
TRAFFIC_LANES = 14


def lanes_text():
    o = ["OCLANES 1",
         "# RallyDash's traffic lanes: the right-hand side of the loop (%d cm right of the centre line), in driving order, cut into %d lanes that" % (TRAFFIC_LATERAL, TRAFFIC_LANES),
         "# each flow into the next, the last into the first. Written by make_rally_dash.py. Speed limit 14 m/s (50 km/h), width 4 m."]
    for i in range(TRAFFIC_LANES):
        s0 = i * PERIM / TRAFFIC_LANES
        s1 = (i + 1) * PERIM / TRAFFIC_LANES
        count = max(2, int((s1 - s0) / 420.0) + 1)
        coords = []
        for k in range(count):
            s = s0 + (s1 - s0) * k / (count - 1)
            x, y = TRACK.pos(s, TRAFFIC_LATERAL)
            coords += [n(x), n(y), "2"]
        o.append("LANE %d 1400 400 0 %d  %s" % (i + 1, count, "  ".join("%s %s %s" % tuple(coords[j:j + 3]) for j in range(0, len(coords), 3))))
    for i in range(TRAFFIC_LANES):
        o.append("NEXT %d %d" % (i + 1, (i + 1) % TRAFFIC_LANES + 1))
    return "\n".join(o) + "\n"


# ================================================================================================================ project, sounds

PROJECT = """OCPROJECT 1
# RallyDash: a small lap game made only from content -- a level, four Aver Node graphs, an input scheme, a HUD layout, a vehicle, particle
# effects and procedural meshes and tones. No C#, no engine change. Open this file (File > Open Project), then press Play. README.md has the
# controls. Everything is written by make_rally_dash.py.
NAME RallyDash
ENGINE Aver 0.7.0-1
CREATEDWITH 0.7.0-1
CONTENT Content
STARTMAP Maps/RallyDash.ocworld
INPUT.SCHEME Input/Rally.ocinput
GAME.MODE AN_RallyRules
WINDOW.TITLE Rally Dash
"""


def tone(freq_hz, seconds, attack=0.005, release=0.08, harmonics=((1, 1.0),), rate=geo.RATE):
    count = int(rate * seconds)
    out = []
    for i in range(count):
        t = i / float(rate)
        env = min(1.0, t / attack) * min(1.0, (seconds - t) / release)
        out.append(env * sum(a * math.sin(2 * math.pi * freq_hz * k * t) for k, a in harmonics))
    return out


def sequence(notes, rate=geo.RATE):
    out = []
    for freq, seconds in notes:
        out += tone(freq, seconds, release=min(0.08, seconds * 0.5), harmonics=((1, 1.0), (2, 0.3), (3, 0.12)), rate=rate)
    return out


def write_sounds():
    for name in ("Engine_Idle", "Engine_High", "Skid", "Crash"):
        copy_from_template("Sounds/%s.wav" % name)
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Beep.wav"), tone(660.0, 0.16, harmonics=((1, 1.0), (2, 0.25))))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Go.wav"), tone(990.0, 0.55, release=0.2, harmonics=((1, 1.0), (2, 0.35), (3, 0.15))))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Checkpoint.wav"), sequence([(880.0, 0.07), (1320.0, 0.12)]))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Lap.wav"), sequence([(660.0, 0.09), (880.0, 0.09), (1100.0, 0.09), (1320.0, 0.22)]))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Finish.wav"),
                  sequence([(523.0, 0.12), (659.0, 0.12), (784.0, 0.12), (1046.0, 0.18), (784.0, 0.1), (1046.0, 0.5)]))


# ================================================================================================================ validation

def check_graph_file(path):
    """Reads a written .ocgraph back and checks what a typo would break: unique node ids, every LINK/ENTRY/OUT naming a declared node, every
    var= naming a declared VAR, exec links going from an exec pin to an exec pin, no node with two exec links out of one pin."""
    with open(path, encoding="utf-8") as fh:
        lines = [l.rstrip("\n") for l in fh]
    if not lines or lines[0] != "OCGRAPH 1":
        raise SystemExit("%s: bad header" % path)
    nodes, vars_, links, entries, outs = {}, set(), [], [], []
    for l in lines:
        t = l.split()
        if not t or t[0].startswith("#"):
            continue
        if t[0] == "NODE":
            if t[1] in nodes:
                raise SystemExit("%s: node %s twice" % (path, t[1]))
            nodes[t[1]] = (t[2], dict(x.split("=", 1) for x in t[3:] if "=" in x))
        elif t[0] == "VAR":
            vars_.add(t[1])
        elif t[0] == "LINK":
            links.append((t[1], t[2]))
        elif t[0] == "ENTRY":
            entries.append(t[1])
        elif t[0] == "OUT":
            outs.append(t[1])
    for a, b in links:
        for end in (a, b):
            if end.split(".")[0] not in nodes:
                raise SystemExit("%s: LINK names unknown node in %s -> %s" % (path, a, b))
    for e in entries + outs:
        if e not in nodes:
            raise SystemExit("%s: ENTRY/OUT names unknown node %s" % (path, e))
    for nid, (typ, attrs) in nodes.items():
        if "var" in attrs and attrs["var"] not in vars_:
            raise SystemExit("%s: node %s reads undeclared VAR %s" % (path, nid, attrs["var"]))
    exec_out = {}
    for a, b in links:
        if a.split(".")[1] in ("then", "true", "false", "exec", "then0", "then1"):
            if not (b.endswith(".exec")):
                raise SystemExit("%s: exec pin %s linked to non-exec %s" % (path, a, b))
            if a in exec_out:
                raise SystemExit("%s: exec pin %s has two outgoing links (%s, %s)" % (path, a, exec_out[a], b))
            exec_out[a] = b
    # GraphCompiler.EmitExecNode inlines a node's whole downstream chain into every arm that reaches it, so a node with two incoming exec
    # links doubles its chain's IL: refuse any such merge (the graphs use Sequence instead; see Builder.br).
    incoming = {}
    for a, b in exec_out.items():
        incoming.setdefault(b, []).append(a)
    for target, sources in incoming.items():
        if len(sources) > 1:
            raise SystemExit("%s: node %s has %d incoming exec links (%s): the compiler would emit its chain once per link" %
                             (path, target.split(".")[0], len(sources), ", ".join(sources)))
    reached = set()
    stack = [nodes_id for nodes_id in entries]
    follow = {}
    for a, b in exec_out.items():
        follow.setdefault(a.split(".")[0], []).append(b.split(".")[0])
    while stack:
        cur = stack.pop()
        if cur in reached:
            continue
        reached.add(cur)
        stack.extend(follow.get(cur, []))
    for nid, (typ, attrs) in nodes.items():
        if typ in ("SetVar", "Branch", "Sequence", "PlaySound") or typ.startswith("AN_Vehicle") or typ.startswith("AN_Camera") or typ.startswith("AN_SetUi"):
            if typ.startswith("AN_VehicleTelemetry") or typ == "AN_VehicleFromEntity" or typ == "AN_VehicleDriver":
                continue
            if nid not in reached:
                raise SystemExit("%s: exec node %s (%s) is not reachable from any ENTRY" % (path, nid, typ))
    return len(nodes), len(links)


def check_world(text):
    counts = {}
    for l in text.splitlines():
        t = l.split()
        if not t or t[0].startswith("#"):
            continue
        counts[t[0]] = counts.get(t[0], 0) + 1
        if t[0] in ("PLACE", "PLACEG", "CHILD"):
            for v in t[2:9]:
                try:
                    f = float(v)
                except ValueError:
                    raise SystemExit("level: bad number %r in %s" % (v, l[:80]))
                if math.isnan(f) or math.isinf(f):
                    raise SystemExit("level: non-finite number in %s" % l[:80])
    return counts


# ================================================================================================================ main

def main():
    assetc = os.path.normpath(os.path.join(REPO, "build", "bin", "AverAssetC.exe"))
    if "--assetc" in sys.argv:
        assetc = sys.argv[sys.argv.index("--assetc") + 1]

    print("loop: %.0f m, minimum bend radius %.0f m, start line at s = %.0f m" % (PERIM / 100.0, min(TRACK.radius) / 100.0, TRACK.s_start / 100.0))

    # meshes: the template's car and wheel are copied; the crate, the pane and the cone are generated
    copy_from_template("Meshes/Sedan.ocmesh")
    copy_from_template("Meshes/Wheel.ocmesh")
    for name, mesh in MESHES.items():
        mesh.write_obj(os.path.join(SOURCE, name + ".obj"))
        lo, hi = mesh.bounds_eng()
        print("  %s bounds (cm): x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f" % ((name,) + tuple(v for pair in zip(lo, hi) for v in pair)))
    if "--no-import" not in sys.argv:
        if os.path.exists(assetc):
            cook_meshes(assetc)
        else:
            print("AverAssetC not found (%s): cook the meshes with the command in README.md" % assetc)

    write_sounds()
    copy_from_template("Particles/TyreSmoke.ocparticle")
    copy_from_template("Particles/Sparks.ocparticle")
    write("Particles/Celebrate.ocparticle", CELEBRATE)
    for name, spec in MATERIALS.items():
        write("Materials/%s.ocmat" % name, material_text(name, spec))

    write("Vehicles/Sedan.ocvehicle", SEDAN)
    write("Input/Rally.ocinput", INPUT)
    write("UI/RallyHud.ocui", hud_text())
    write(NUMBERS, numbers_csv())

    poses = [gate_pose(k) for k in range(GATES)]
    respawns = [respawn_pose(k) for k in range(GATES)]
    total_nodes = 0
    for build in (graph_rules, graph_controller, graph_driver, lambda: graph_race(poses, respawns)):
        g = build()
        path_rel = "Scripts/%s.ocgraph" % g.cls
        write(path_rel, g.text())
        nodes, links = check_graph_file(os.path.join(CONTENT, path_rel))
        print("  %s: %d nodes, %d links, every pin and attribute checked against docs/nodes" % (g.cls, nodes, links))
        total_nodes += nodes

    level = build_level()
    text = level.text()
    counts = check_world(text)
    write("Maps/RallyDash.ocworld", text)
    write("Maps/RallyDash.oclanes", lanes_text())
    print("  level: %d placements (%s)" % (level.placements, ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    write("../Game.ocproject", PROJECT)
    print("done: %d graph nodes in total" % total_nodes)


if __name__ == "__main__":
    main()
