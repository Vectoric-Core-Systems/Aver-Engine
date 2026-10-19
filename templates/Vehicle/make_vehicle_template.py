#!/usr/bin/env python3
"""Writes the Vehicle template (docs/VEHICLES.md "The Vehicle template"): procedural source meshes and engine loops, the
.ocvehicle specs, the map, the input scheme, the HUD layout, the particle effects and the five Aver Node graphs. Stdlib only,
no third-party assets. Run it from anywhere:

    python make_vehicle_template.py            # writes Source/*.obj, Content/**, then imports the meshes if AverAssetC is found
    python make_vehicle_template.py --no-import  # skip the .ocmesh import (the OBJ files are still written)

The committed Content/Meshes/*.ocmesh are the importer's output for Source/*.obj (this script runs `AverAssetC convert` on each;
pass --assetc <path> when it is not at ../../build/bin/AverAssetC.exe). A template copies only Content/, so Source/, this script and
its helper modules (vehicle_geo.py, graphkit.py) stay in the engine repo.

The two .ocvehicle files are the same vehicles the engine embeds as vehicle::builtinSpec("sedan") and ("tank")
(modules/vehicle/src/VehiclePresets.cpp); tests/vehicle/src/OcVehicleTest.cpp parses them and compares.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import vehicle_geo as geo          # noqa: E402
from graphkit import Graph         # noqa: E402

CONTENT = os.path.join(HERE, "Content")
SOURCE = os.path.join(HERE, "Source")


def write(rel, text, base=CONTENT):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote", os.path.relpath(path, HERE))


# ----------------------------------------------------------------------------------------------------------- .ocvehicle

SEDAN = """OCVEHICLE 1
# The template's saloon: rear-wheel drive, a five-speed automatic, open differentials, ABS, a speed-sensitive steering curve.
# It is the same vehicle the engine embeds as vehicle::builtinSpec("sedan"); the tests compare the two.
#
# Units: centimetres, kilograms, N*m, degrees. +X is forward, +Y right, +Z up, and the origin is the BOTTOM CENTRE of the car,
# where Meshes/Sedan.ocmesh has its own origin. A WHEEL's hub is the SETTLED wheel centre: z equal to the radius puts the tyre
# bottom on the ground. Axle 0 is the front (largest x).
NAME Sedan
KIND wheeled
BODY half 220 90 35 clearance 25 com 0 0 40 mass 1300 pitchroll 60
ENGINE torque 500 minrpm 1000 maxrpm 6000 inertia 0.5 damping 0.2
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
ASSIST abs 1 absslip 0.25 absrelease 8 tcs 0 tcsslip 0.35 steerin 3 steerout 5 throttlein 6 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.3 holdspeed 30 skidslip 0.45 skidangle 12
STEERCURVE 0 1 1500 0.6 3500 0.35
AERO drag 0.35 downforce 0.8
SEAT driver 20 -35 55 exit -20 -140 0
AUDIO engine Sounds/Engine_Idle.wav idle 1000 Sounds/Engine_High.wav high 6000 pitchlow 0.8 pitchhigh 2.2 skid Sounds/Skid.wav impact Sounds/Crash.wav
EFFECT skid Particles/TyreSmoke.ocparticle rate 40 impact Particles/Sparks.ocparticle count 12
DAMAGE health 1000 crash 600 energy 0.5 radius 80
CAMERA modes chase hood cockpit arm 600 eye 25 -35 110 hood 150 0 95
HUD UI/VehicleHud.ocui
"""

TANK = """OCVEHICLE 1
# The template's tank: 38 tonnes on five road wheels a side, steered by driving the two tracks at different speeds (it pivots on the
# spot when standing). A four-speed automatic with a manual-capable box. The same vehicle as vehicle::builtinSpec("tank").
NAME Tank
KIND tracked
BODY half 350 170 45 clearance 35 com 0 0 70 mass 38000 pitchroll 70
ENGINE torque 6000 minrpm 800 maxrpm 3500 inertia 0 damping 0.4
GEARBOX auto forward 4 3 2 1 reverse 4 3 switch 0.6 clutchrelease 0.4 latency 0.5 up 0.6 down 0.2 clutch 0
WHEELCLASS road radius 40 width 50 susp 5 35 hz 1.8 damping 0.6 brake 0 handbrake 0 inertia 5 spindamping 0.2
WHEEL L0 class road hub -280 -130 40 steer 0 driven 0 handbrake 0 node WheelL0
WHEEL L1 class road hub -140 -130 40 steer 0 driven 0 handbrake 0 node WheelL1
WHEEL L2 class road hub 0 -130 40 steer 0 driven 0 handbrake 0 node WheelL2
WHEEL L3 class road hub 140 -130 40 steer 0 driven 0 handbrake 0 node WheelL3
WHEEL L4 class road hub 280 -130 40 steer 0 driven 1 handbrake 0 node WheelL4
WHEEL R0 class road hub -280 130 40 steer 0 driven 0 handbrake 0 node WheelR0
WHEEL R1 class road hub -140 130 40 steer 0 driven 0 handbrake 0 node WheelR1
WHEEL R2 class road hub 0 130 40 steer 0 driven 0 handbrake 0 node WheelR2
WHEEL R3 class road hub 140 130 40 steer 0 driven 0 handbrake 0 node WheelR3
WHEEL R4 class road hub 280 130 40 steer 0 driven 1 handbrake 0 node WheelR4
TRACK left wheels 0 1 2 3 4 driven 4 inertia 10 damping 0.5 brake 60000 final 6 steergain 1.6 pivot 0.6
TRACK right wheels 5 6 7 8 9 driven 9 inertia 10 damping 0.5 brake 60000 final 6 steergain 1.6 pivot 0.6
ASSIST abs 0 absslip 0.25 absrelease 8 tcs 0 tcsslip 0.35 steerin 2 steerout 4 throttlein 4 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.5 holdspeed 30 skidslip 0.45 skidangle 12
SEAT driver 60 0 120 exit -100 -260 0
AUDIO engine Sounds/Engine_Idle.wav idle 800 Sounds/Engine_High.wav high 3500 pitchlow 0.6 pitchhigh 1.6 impact Sounds/Crash.wav
EFFECT impact Particles/Sparks.ocparticle count 16 rate 0
DAMAGE health 3000 crash 900 energy 0.3 radius 120
CAMERA modes chase arm 900 eye 60 0 170 hood 250 0 130
HUD UI/VehicleHud.ocui
"""

# ----------------------------------------------------------------------------------------------------------- input scheme

INPUT = """OCINPUT 1
NAME Vehicle
# The project's ONE input scheme. Move, Look and Interact are the on-foot actions (the template's pawn graph reads the devices
# directly, like the FirstPerson template, so they are here for rebinding and C# projects). The Drive.* actions are what
# Aver.Vehicle.Scene reads while somebody is driving: nothing else needs to know about them, and they are ignored while nobody is.
# E is both Interact (on foot) and Drive.ShiftUp (in a manual gearbox); F gets out.
ACTION Move axis2
ACTION Look axis2
ACTION Interact digital
ACTION Drive.Throttle axis1
ACTION Drive.Brake axis1
ACTION Drive.Steer axis1
ACTION Drive.Handbrake digital
ACTION Drive.ShiftUp digital
ACTION Drive.ShiftDown digital
ACTION Drive.Reset digital
ACTION Drive.Exit digital
ACTION Drive.CameraNext digital

BIND Move key W component y
BIND Move key S scale -1 component y
BIND Move key D component x
BIND Move key A scale -1 component x
BIND Move gamepadaxis LeftX component x
BIND Move gamepadaxis LeftY component y
BIND Look mousex component x
BIND Look mousey component y
BIND Look gamepadaxis RightX component x
BIND Look gamepadaxis RightY component y
BIND Interact key E
BIND Interact gamepadbutton X

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
BIND Drive.Reset key R
BIND Drive.Reset gamepadbutton Y
BIND Drive.Exit key F
BIND Drive.Exit gamepadbutton B
BIND Drive.CameraNext key C
BIND Drive.CameraNext gamepadbutton RightThumb

CONTEXT Vehicle priority 0
"""

# ----------------------------------------------------------------------------------------------------------- HUD layout

HUD = """OCUI 1
NAME VehicleHud
# Bars, not numbers: a graph has no number-to-text node, so the template shows speed, revs, gear and health as filled bars and the
# ABS / TCS lamps as text that appears while the aid is working. (A C# project formats real numbers: VehicleHud.FormatSpeed, FormatGear.)
# AN_VehicleHud.ocgraph opens this layout, shows Root while somebody drives the sedan or the tank and fills the bars from telemetry.
WIDGET Root panel -
SET Root anchors 0 0 1 1
SET Root style clear
SET Root inputMode passive
SET Root hit never
SET Root visible false

WIDGET Hud panel Root
SET Hud anchors 1 1
SET Hud pivot 1 1
SET Hud offsets -40 -40 0 0
SET Hud width 380
SET Hud layout vstack
SET Hud spacing 6
SET Hud padding 14
SET Hud hit never

WIDGET SpeedLabel text Hud
SET SpeedLabel text "SPEED   (full bar = 250 km/h)"
WIDGET Speed progress Hud
SET Speed minValue 0
SET Speed maxValue 250
SET Speed height 18

WIDGET RpmLabel text Hud
SET RpmLabel text "ENGINE   (full bar = 7000 rpm)"
WIDGET Rpm progress Hud
SET Rpm minValue 0
SET Rpm maxValue 7000
SET Rpm height 18

WIDGET GearLabel text Hud
SET GearLabel text "GEAR   R  N  1  2  3  4  5  6  7"
WIDGET Gear progress Hud
SET Gear minValue 0
SET Gear maxValue 9
SET Gear height 18

WIDGET HealthLabel text Hud
SET HealthLabel text "HEALTH"
WIDGET Health progress Hud
SET Health minValue 0
SET Health maxValue 1
SET Health height 18

WIDGET Lamps panel Hud
SET Lamps layout hstack
SET Lamps spacing 16
SET Lamps hit never
WIDGET AbsLamp text Lamps
SET AbsLamp text "ABS"
SET AbsLamp visible false
SET AbsLamp textColor #FFD040
WIDGET TcsLamp text Lamps
SET TcsLamp text "TCS"
SET TcsLamp visible false
SET TcsLamp textColor #FFD040
"""

# ----------------------------------------------------------------------------------------------------------- particles

TYRE_SMOKE = """OCPARTICLE 2
NAME Tyre Smoke
# Smoke at a skidding tyre. SPAWNAT on lets the vehicle system place bursts of it (aver::spawnEffectAt) at each skidding wheel's
# contact point; the Count in the spec's EFFECT record scales how many are emitted per second.
EMITTER Smoke
SIM auto
SHAPE sphere 6
BLEND alphablend
EMISSION 0 6 1024
LIFETIME 0.8 1.6
DIRECTION 0 0 1 40
SPEED 20 60
GRAVITY 0 0 40
DAMPING 0.6
SIZE 14 60
COLORCURVE 0 0.62 0.62 0.62 0 0.15 0.62 0.62 0.62 0.45 0.6 0.55 0.55 0.55 0.3 1 0.5 0.5 0.5 0
SPRITE camera
SORT depth
LIGHTING ambient
FOG on
SPAWNAT on
BOUNDS 600
MODULE spawn.sizescale min=0.8 max=1.3
MODULE spawn.rotation minDeg=0 maxDeg=360 spinMin=-40 spinMax=40
"""

SPARKS = """OCPARTICLE 2
NAME Sparks
# Metal sparks for a hard impact. Self-lit and additive, with their own gravity; SPAWNAT on, like TyreSmoke.
EMITTER Sparks
SIM auto
SHAPE sphere 6
BLEND additive
EMISSION 0 12 512
LIFETIME 0.4 0.9
DIRECTION 0 0 1 60
SPEED 180 420
GRAVITY 0 0 -700
DAMPING 0.05
SIZE 2 0.5
COLOR start 1 0.8 0.35 1
COLOR end 1 0.25 0.05 0
SPRITE velocity stretch 0.5
SORT none
LIGHTING unlit
FOG on
SPAWNAT on
BOUNDS 500
"""

# ----------------------------------------------------------------------------------------------------------- the map

def map_text():
    o = []
    add = o.append
    add("OCWORLD 1")
    add("NAME Vehicle Test Arena")
    add("SPAWN -250 0 100 0")
    add("SUN elev 48.0 azim 130.0 color 1 0.97 0.92 lux 90000")
    add("SKY model physical")
    add("# Nothing here is a third-party asset: the arena is the built-in unit cube (Meshes/cube.ocmesh, half-extent 1 cm, so the last three")
    add("# numbers of a PLACEG are half extents in centimetres) and the vehicles are procedural (make_vehicle_template.py).")
    add("# +X is forward from the spawn. Walk to a vehicle (within 4 m) and press E; F gets out; C changes the camera; see README.md.")
    add("PLACEG Meshes/cube.ocmesh 0 0 -50 0 0 0 6000 6000 50 name Floor")
    add("")
    add("# The sedan and the tank. nocollide: the vehicle's own physics body is the chassis, not a static collider. Each wheel is a CHILD")
    add("# the system moves every frame (the spec's `node` names); the tank's Turret is a child AN_TankTurret turns.")
    add("PLACE Meshes/Sedan.ocmesh 0 -320 5 0 0 0 1 nocollide name Sedan")
    add("  COMPONENT Vehicle asset=Vehicles/Sedan.ocvehicle")
    add("  BEGIN")
    for name, x, y in (("WheelFL", 180, -77), ("WheelFR", 180, 77), ("WheelRL", -180, -77), ("WheelRR", -180, 77)):
        add("    CHILD Meshes/Wheel.ocmesh %d %d 33 0 0 0 1 nocollide name %s" % (x, y, name))
    add("  END")
    add("PLACE Meshes/TankHull.ocmesh 0 520 5 0 0 0 1 nocollide name Tank")
    add("  COMPONENT Vehicle asset=Vehicles/Tank.ocvehicle")
    add("  BEGIN")
    for side, y in (("L", -130), ("R", 130)):
        for i, x in enumerate((-280, -140, 0, 140, 280)):
            add("    CHILD Meshes/TankWheel.ocmesh %d %d 40 0 0 0 1 nocollide name Wheel%s%d" % (x, y, side, i))
    add("    CHILD Meshes/TankTurret.ocmesh 30 0 130 0 0 0 1 nocollide name Turret")
    add("  END")
    add("")
    add("# Graph classes placed in the level: the HUD (opens UI/VehicleHud.ocui, shows it while a vehicle has a driver) and the turret.")
    add("PLACE none 0 0 0 0 0 0 1 class AN_VehicleHud")
    add("PLACE none 0 0 0 0 0 0 1 class AN_TankTurret")
    add("")
    add("# A ramp 10 m ahead of the sedan (12 degrees up along +X, then the ground falls away: the landing event), a wall to brake against,")
    add("# and a slalom of posts.")
    add("PLACEG Meshes/cube.ocmesh 3600 -320 90 0 -12 0 500 250 20 name Ramp")
    add("PLACEG Meshes/cube.ocmesh 2400 1000 100 0 0 0 20 500 100 name BrakeWall")
    for i in range(8):
        x = 900 + i * 260
        y = -320 + (140 if i % 2 == 0 else -140)
        add("PLACEG Meshes/cube.ocmesh %d %d 60 0 0 0 25 25 60 name Post%d" % (x, y, i + 1))
    add("")
    return "\n".join(o) + "\n"


# ----------------------------------------------------------------------------------------------------------- graphs

def graph_rules():
    g = Graph("AN_VehicleRules", "GameMode", "The game mode, as a graph: the play session's pawn, controller and a round clock.",
              class_attrs="pawn=AN_VehicleDriver controller=AN_VehicleController",
              header_comment="""THE GAME MODE. It names the pawn and the controller, which is what makes the play session spawn and possess them (the
FirstPerson template's AN_FPRules explains why both are required). Two copies of this tick in a headless log because the map-less game
mode is spawned once by begin_play; they do not interfere.""")
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
    return Graph("AN_VehicleController", "PlayerController",
                 "The player controller. Empty on purpose: it exists so the game mode has something concrete to possess with.",
                 header_comment="""THE CONCRETE PLAYER CONTROLLER. The engine's built-in PlayerController is abstract and cannot be spawned, and begin_play
possesses a pawn only when it has both. Input is polled by the pawn itself, so nothing else belongs here (AN_FPController.ocgraph in the
FirstPerson template has the full story).""")


def graph_driver():
    g = Graph("AN_VehicleDriver", "Character", "On foot: walk, look, and press E within 4 m of the sedan or the tank to get in.",
              class_attrs="view=firstperson",
              header_comment="""THE ON-FOOT PAWN. Walking and looking are the FirstPerson template's (MoveAxis, MouseDelta, CharacterMove); there is no mesh,
so the first-person view is the capsule's eye. While either vehicle has a driver this pawn does nothing: the vehicle system has hidden
it, parked it in the seat and taken the camera (its camera sink points the view at the vehicle's chase rig). F (Drive.Exit) puts it
back at the vehicle's exit point and the walking resumes the next tick, because `busy` goes false.

IT TESTS "SOMEBODY IS DRIVING" WITH `driver != 0`, never with `driver == me`: an entity handle is an int that does not survive a float
(handles start at 2^24), so two handles cannot be compared through the float pins of Equal. Only the player ever drives here.""")
    g.param("entity", "int")
    g.param("deltaTime", "float")
    g.node("ent", "Param", param="entity")
    g.node("dt", "Param", param="deltaTime")

    # Who is driving what.
    g.node("sedan", "FindEntity", name="Sedan")
    g.node("tank", "FindEntity", name="Tank")
    g.node("sedanDrv", "AN_VehicleDriver")
    g.node("tankDrv", "AN_VehicleDriver")
    g.link("sedan", "entity", "sedanDrv", "entity")
    g.link("tank", "entity", "tankDrv", "entity")
    g.node("sedanDrvF", "IntToFloat")
    g.node("tankDrvF", "IntToFloat")
    g.link("sedanDrv", "driver", "sedanDrvF", "a")
    g.link("tankDrv", "driver", "tankDrvF", "a")
    g.node("half", "ConstFloat", value="0.5")
    g.node("sedanBusy", "Greater")
    g.node("tankBusy", "Greater")
    g.link("sedanDrvF", "result", "sedanBusy", "a")
    g.link("half", "value", "sedanBusy", "b")
    g.link("tankDrvF", "result", "tankBusy", "a")
    g.link("half", "value", "tankBusy", "b")
    g.node("busy", "Or")
    g.link("sedanBusy", "result", "busy", "a")
    g.link("tankBusy", "result", "busy", "b")

    # The tick: nothing while driving, walk and look otherwise.
    g.entry("tick", "OnTick")
    g.node("tick", "OnTick")
    g.node("gate", "Branch")
    g.link("tick", "exec", "gate", "exec")
    g.link("busy", "result", "gate", "cond")

    g.node("move", "MoveAxis")
    g.node("mouse", "MouseDelta")
    g.link("gate", "false", "move", "exec")
    g.link("move", "then", "mouse", "exec")
    g.node("lookScale", "ConstFloat", value="0.15")
    g.node("lookScalePitch", "ConstFloat", value="-0.15")
    g.node("yaw", "Multiply")
    g.node("pitch", "Multiply")
    g.link("mouse", "deltaX", "yaw", "a")
    g.link("lookScale", "value", "yaw", "b")
    g.link("mouse", "deltaY", "pitch", "a")
    g.link("lookScalePitch", "value", "pitch", "b")
    g.node("drive", "CharacterMove")
    g.link("mouse", "then", "drive", "exec")
    g.link("ent", "value", "drive", "entity")
    g.link("dt", "value", "drive", "dt")
    g.link("move", "forward", "drive", "forward")
    g.link("move", "right", "drive", "right")
    g.link("yaw", "result", "drive", "yawDelta")
    g.link("pitch", "result", "drive", "pitchDelta")

    # E (Key.E = 4) within reach of a vehicle: get in.
    g.node("eKey", "ConstInt", value="4")
    g.node("ePressed", "InputKeyPressed")
    g.link("eKey", "value", "ePressed", "key")
    g.node("useGate", "Branch")
    g.link("drive", "then", "useGate", "exec")
    g.link("ePressed", "triggered", "useGate", "cond")
    g.node("reach", "ConstFloat", value="400.0")
    for who in ("sedan", "tank"):
        g.node(who + "Dist", "AN_GetDistance")
        g.link("ent", "value", who + "Dist", "a")
        g.link(who, "entity", who + "Dist", "b")
        g.node(who + "Near", "Less")
        g.link(who + "Dist", "distance", who + "Near", "a")
        g.link("reach", "value", who + "Near", "b")
        g.node(who + "Close", "And")
        g.link(who + "Near", "result", who + "Close", "a")
        g.link(who + "Dist", "success", who + "Close", "b")
    g.node("pickSedan", "Branch")
    g.link("useGate", "true", "pickSedan", "exec")
    g.link("sedanClose", "result", "pickSedan", "cond")
    g.node("enterSedan", "AN_VehicleEnter")
    g.link("pickSedan", "true", "enterSedan", "exec")
    g.link("sedan", "entity", "enterSedan", "entity")
    g.link("ent", "value", "enterSedan", "driver")
    g.node("pickTank", "Branch")
    g.link("pickSedan", "false", "pickTank", "exec")
    g.link("tankClose", "result", "pickTank", "cond")
    g.node("enterTank", "AN_VehicleEnter")
    g.link("pickTank", "true", "enterTank", "exec")
    g.link("tank", "entity", "enterTank", "entity")
    g.link("ent", "value", "enterTank", "driver")
    return g


def graph_hud():
    g = Graph("AN_VehicleHud", "Actor", "Opens UI/VehicleHud.ocui and fills its bars from the telemetry of whichever vehicle has a driver.",
              header_comment="""THE HUD. Placed in the level (PLACE none ... class AN_VehicleHud). On start it opens the layout; every tick it looks at the sedan and the
tank, and for the first one that has a driver it shows the layout's Root and sets the bars from AN_VehicleTelemetry. With nobody
driving it hides Root. Widgets are found by name with no layout handle (a layout of 0 searches everywhere), so nothing has to be kept
between ticks. Speed is km/h (cm/s * 0.036) against a 250 km/h bar; the gear bar is gear + 1 of 9 (R is 0, N is 1).""")
    g.entry("start", "OnStart")
    g.node("start", "OnStart")
    g.node("open", "AN_OpenUiLayout", path="UI/VehicleHud.ocui")
    g.link("start", "exec", "open", "exec")

    g.node("half", "ConstFloat", value="0.5")
    g.node("one", "ConstFloat", value="1.0")
    g.node("kmh", "ConstFloat", value="0.036")
    g.node("no", "ConstBool", value="false")
    for who, name in (("sedan", "Sedan"), ("tank", "Tank")):
        g.node(who, "FindEntity", name=name)
        g.node(who + "Drv", "AN_VehicleDriver")
        g.link(who, "entity", who + "Drv", "entity")
        g.node(who + "DrvF", "IntToFloat")
        g.link(who + "Drv", "driver", who + "DrvF", "a")
        g.node(who + "Busy", "Greater")
        g.link(who + "DrvF", "result", who + "Busy", "a")
        g.link("half", "value", who + "Busy", "b")

    g.entry("tick", "OnTick")
    g.node("tick", "OnTick")
    g.node("ifSedan", "Branch")
    g.link("tick", "exec", "ifSedan", "exec")
    g.link("sedanBusy", "result", "ifSedan", "cond")
    g.node("ifTank", "Branch")
    g.link("ifSedan", "false", "ifTank", "exec")
    g.link("tankBusy", "result", "ifTank", "cond")

    def show_chain(who, exec_node, exec_pin):
        tel = who + "Tel"
        g.node(tel, "AN_VehicleTelemetry")
        g.link(who, "entity", tel, "entity")
        # km/h and gear
        g.node(who + "SpeedAbs", "Abs")
        g.link(tel, "speedCmS", who + "SpeedAbs", "a")
        g.node(who + "SpeedKmh", "Multiply")
        g.link(who + "SpeedAbs", "result", who + "SpeedKmh", "a")
        g.link("kmh", "value", who + "SpeedKmh", "b")
        g.node(who + "GearF", "IntToFloat")
        g.link(tel, "gear", who + "GearF", "a")
        g.node(who + "GearBar", "Add")
        g.link(who + "GearF", "result", who + "GearBar", "a")
        g.link("one", "value", who + "GearBar", "b")
        prev, prev_pin = exec_node, exec_pin

        def step(ident, typ, **attrs):
            nonlocal prev, prev_pin
            g.node(ident, typ, **attrs)
            g.link(prev, prev_pin, ident, "exec")
            prev, prev_pin = ident, "then"

        step(who + "FindRoot", "AN_FindUiWidget", name="Root")
        step(who + "ShowRoot", "AN_SetUiVisible")
        g.link(who + "FindRoot", "widget", who + "ShowRoot", "widget")
        for widget, source in (("Speed", (who + "SpeedKmh", "result")), ("Rpm", (tel, "rpm")), ("Gear", (who + "GearBar", "result")),
                               ("Health", (tel, "health"))):
            step(who + "Find" + widget, "AN_FindUiWidget", name=widget)
            step(who + "Set" + widget, "AN_SetUiValue")
            g.link(who + "Find" + widget, "widget", who + "Set" + widget, "widget")
            g.link(source[0], source[1], who + "Set" + widget, "value")
        for lamp, pin in (("AbsLamp", "abs"), ("TcsLamp", "tcs")):
            step(who + "Find" + lamp, "AN_FindUiWidget", name=lamp)
            step(who + "Set" + lamp, "AN_SetUiVisible")
            g.link(who + "Find" + lamp, "widget", who + "Set" + lamp, "widget")
            g.link(tel, pin, who + "Set" + lamp, "visible")

    show_chain("sedan", "ifSedan", "true")
    show_chain("tank", "ifTank", "true")
    g.node("hideRoot", "AN_FindUiWidget", name="Root")
    g.link("ifTank", "false", "hideRoot", "exec")
    g.node("hide", "AN_SetUiVisible")
    g.link("hideRoot", "then", "hide", "exec")
    g.link("hideRoot", "widget", "hide", "widget")
    g.link("no", "value", "hide", "visible")
    return g


def graph_turret():
    g = Graph("AN_TankTurret", "Actor", "While somebody drives the tank, the mouse turns the turret child about its ring.",
              header_comment="""THE TURRET. The tank in Maps/Default.ocworld has a child named Turret (Meshes/TankTurret.ocmesh). This actor, placed in the level, adds
the mouse's horizontal movement to a yaw it remembers and writes that as the turret's local rotation, only while the tank has a driver.
The barrel's elevation is left out on purpose (the template shows the vehicle, not a weapon).""")
    g.param("deltaTime", "float")
    g.var("yaw", "float", "0")
    g.node("dt", "Param", param="deltaTime")
    g.node("tank", "FindEntity", name="Tank")
    g.node("tankDrv", "AN_VehicleDriver")
    g.link("tank", "entity", "tankDrv", "entity")
    g.node("tankDrvF", "IntToFloat")
    g.link("tankDrv", "driver", "tankDrvF", "a")
    g.node("half", "ConstFloat", value="0.5")
    g.node("busy", "Greater")
    g.link("tankDrvF", "result", "busy", "a")
    g.link("half", "value", "busy", "b")
    g.entry("tick", "OnTick")
    g.node("tick", "OnTick")
    g.node("gate", "Branch")
    g.link("tick", "exec", "gate", "exec")
    g.link("busy", "result", "gate", "cond")
    g.node("mouse", "MouseDelta")
    g.link("gate", "true", "mouse", "exec")
    g.node("scale", "ConstFloat", value="0.15")
    g.node("turn", "Multiply")
    g.link("mouse", "deltaX", "turn", "a")
    g.link("scale", "value", "turn", "b")
    g.node("prev", "GetVar", var="yaw")
    g.node("next", "Add")
    g.link("prev", "value", "next", "a")
    g.link("turn", "result", "next", "b")
    g.node("store", "SetVar", var="yaw")
    g.link("mouse", "then", "store", "exec")
    g.link("next", "result", "store", "value")
    g.node("turret", "FindEntity", name="Turret")
    g.node("zero", "ConstFloat", value="0.0")
    g.node("rotate", "SetLocalRotation")
    g.link("store", "then", "rotate", "exec")
    g.link("turret", "entity", "rotate", "entity")
    g.link("next", "result", "rotate", "yaw")
    g.link("zero", "value", "rotate", "pitch")
    g.link("zero", "value", "rotate", "roll")
    return g


# ----------------------------------------------------------------------------------------------------------- meshes

MESHES = {
    "Sedan": geo.sedan_body("Sedan"),
    "Wheel": geo.wheel_mesh("Wheel", 33.0, 22.0),
    "TankHull": geo.tank_hull("TankHull"),
    "TankWheel": geo.wheel_mesh("TankWheel", 40.0, 50.0),
    "TankTurret": geo.tank_turret("TankTurret"),
}


def import_meshes(assetc):
    if not assetc or not os.path.exists(assetc):
        print("AverAssetC not found (%s): the OBJ files are written; import them with the command in README.md" % assetc)
        return
    out = os.path.join(CONTENT, "Meshes")
    os.makedirs(out, exist_ok=True)
    for name in MESHES:
        cmd = [assetc, "convert", os.path.join(SOURCE, name + ".obj"), "--out-dir", out, "--base", name]
        res = subprocess.run(cmd, capture_output=True, text=True)
        last = [l for l in res.stdout.splitlines() if l.startswith("{")]
        print("import", name, "->", last[0] if last else res.stdout[-200:] + res.stderr[-200:])
        if res.returncode != 0:
            raise SystemExit("AverAssetC failed on %s" % name)


def main():
    assetc = os.path.normpath(os.path.join(HERE, "..", "..", "build", "bin", "AverAssetC.exe"))
    do_import = "--no-import" not in sys.argv
    if "--assetc" in sys.argv:
        assetc = sys.argv[sys.argv.index("--assetc") + 1]

    for name, mesh in MESHES.items():
        mesh.write_obj(os.path.join(SOURCE, name + ".obj"))
        lo, hi = mesh.bounds_eng()
        print("  bounds (cm): x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f" % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    if do_import:
        import_meshes(assetc)

    geo.write_wav(os.path.join(CONTENT, "Sounds", "Engine_Idle.wav"), geo.engine_loop(55.0, 2.0, 7, 1, rough=0.08))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Engine_High.wav"), geo.engine_loop(165.0, 2.0, 6, 1, rough=0.03))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Skid.wav"), geo.skid_loop(1.5))
    geo.write_wav(os.path.join(CONTENT, "Sounds", "Crash.wav"), geo.crash_hit(0.9))

    write("Vehicles/Sedan.ocvehicle", SEDAN)
    write("Vehicles/Tank.ocvehicle", TANK)
    write("Input/Vehicle.ocinput", INPUT)
    write("UI/VehicleHud.ocui", HUD)
    write("Particles/TyreSmoke.ocparticle", TYRE_SMOKE)
    write("Particles/Sparks.ocparticle", SPARKS)
    write("Maps/Default.ocworld", map_text())
    for build in (graph_rules, graph_controller, graph_driver, graph_hud, graph_turret):
        g = build()
        write("Scripts/%s.ocgraph" % g.cls, g.text())


if __name__ == "__main__":
    main()
